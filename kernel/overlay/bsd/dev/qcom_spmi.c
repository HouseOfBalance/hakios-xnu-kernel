/*
 * Qualcomm SPMI PMIC Arbiter v5 driver for SM6225 (Snapdragon 685 / sapphire).
 *
 * The SPMI PMIC Arbiter bridges the SoC to Qualcomm PMICs (PM6125 / PMI632 on SM6225).
 * ABL leaves the arbiter and PPID-to-APID channel table configured; this driver maps
 * the arbiter window, locates the APID for a given (sid, periph_id) pair, and executes
 * EXT_REG_READ_LONG / EXT_REG_WRITE_LONG transactions with bounded polling.
 *
 * Off until asked: boot-arg qcom_spmi=1 (or automatically used when qcom_keys=1 / qcom_batt=1).
 */
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/qcom_spmi.h>
#include <sys/systm.h>
#include "hakios_board.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

/*
 * PMIC Arbiter v5 register layout on SM6225 (verified from sapphire.dtb):
 *   core:   0x01c40000 (size 0x1100)    -> APID map at 0x0900 + 4 * apid
 *   chnls:  0x01e00000 (size 0x2000000) -> RW channel at 0x10000 * apid (mapped lazily per APID)
 *   obsrvr: 0x03e00000 (size 0x100000)  -> EE0 observer channel at 0x80 * apid
 */
#define SPMI_CORE_MAP_SIZE          0x00002000u
#define SPMI_CHNLS_PHYS_OFFSET      0x001c0000ULL /* 0x01e00000 - 0x01c40000 */
#define SPMI_CHNL_STRIDE            0x00010000ULL
#define SPMI_OBSRVR_PHYS_OFFSET     0x021c0000ULL /* 0x03e00000 - 0x01c40000 */
#define SPMI_OBSRVR_MAP_SIZE        0x00010000u   /* 512 APIDs * 0x80 = 64 KB for EE 0 */
#define SPMI_OBSRVR_STRIDE          0x00000080u

#define PMIC_ARB_VERSION            0x00000000u
#define PMIC_ARB_CORE_REG(apid)     (0x00000900u + 4u * (apid))
#define PMIC_ARB_MAX_APIDS          512u
#define PMIC_ARB_CHAN_IS_IRQ_OWNER  (1u << 24)

#define PMIC_ARB_REG_CMD            0x00u
#define PMIC_ARB_REG_CONFIG         0x04u
#define PMIC_ARB_REG_STATUS         0x08u
#define PMIC_ARB_REG_WDATA0         0x10u
#define PMIC_ARB_REG_WDATA1         0x14u
#define PMIC_ARB_REG_RDATA0         0x18u
#define PMIC_ARB_REG_RDATA1         0x1cu

/* SPMI command opcodes */
#define SPMI_CMD_EXT_WRITE_LONG     0x00u
#define SPMI_CMD_EXT_READ_LONG      0x01u

/* PMIC_ARB_STATUS bits */
#define PMIC_ARB_STATUS_DONE        (1u << 0)
#define PMIC_ARB_STATUS_FAILURE     (1u << 1)
#define PMIC_ARB_STATUS_DENIED      (1u << 2)
#define PMIC_ARB_STATUS_DROPPED     (1u << 3)

#define SPMI_POLL_LIMIT             100000u
#define SPMI_TX_CACHE_MAX           16u

struct spmi_tx_slot {
	uint32_t apid;
	vm_offset_t va;
};

static vm_offset_t spmi_phys_base;
static vm_offset_t spmi_core_base;
static vm_offset_t spmi_obsrvr_base;
static struct spmi_tx_slot spmi_tx_cache[SPMI_TX_CACHE_MAX];
static uint32_t spmi_tx_cache_count;
static bool spmi_initialized;

static LCK_GRP_DECLARE(spmi_lck_grp, "qcom-spmi");
static LCK_MTX_DECLARE(spmi_mtx, &spmi_lck_grp);

#ifdef HAKIOS_MMIO_TEST
extern uint32_t hakios_test_rd(vm_offset_t address);
extern void hakios_test_wr(vm_offset_t address, uint32_t value);
#define spmi_mmio_rd(va, at)        hakios_test_rd((va) + (at))
#define spmi_mmio_wr(va, at, value) hakios_test_wr((va) + (at), (value))
#else
static inline uint32_t
spmi_mmio_rd(vm_offset_t va, uint32_t at)
{
	return *(volatile uint32_t *)(va + at);
}

static inline void
spmi_mmio_wr(vm_offset_t va, uint32_t at, uint32_t value)
{
	*(volatile uint32_t *)(va + at) = value;
}
#endif

static int
spmi_find_apid(uint8_t sid, uint8_t periph_id)
{
	uint32_t want = ((uint32_t)(sid & 0x0fu) << 8) | periph_id;
	int fallback = -ENOENT;

	for (uint32_t apid = 0; apid < PMIC_ARB_MAX_APIDS; apid++) {
		uint32_t reg = spmi_mmio_rd(spmi_core_base, PMIC_ARB_CORE_REG(apid));
		uint32_t ppid = (reg >> 8) & 0x0fffu;
		if (reg != 0 && ppid == want) {
			if (reg & PMIC_ARB_CHAN_IS_IRQ_OWNER) {
				return (int)apid;
			}
			fallback = (int)apid;
		}
	}
	return fallback;
}

static vm_offset_t
spmi_get_tx_va(uint32_t apid)
{
	for (uint32_t i = 0; i < spmi_tx_cache_count; i++) {
		if (spmi_tx_cache[i].apid == apid) {
			return spmi_tx_cache[i].va;
		}
	}
	if (spmi_tx_cache_count >= SPMI_TX_CACHE_MAX) {
		return 0;
	}
	vm_offset_t chnl_phys = spmi_phys_base + SPMI_CHNLS_PHYS_OFFSET + (vm_offset_t)apid * SPMI_CHNL_STRIDE;
	vm_offset_t va = ml_io_map(chnl_phys, 0x1000u);
	if (va != 0) {
		spmi_tx_cache[spmi_tx_cache_count].apid = apid;
		spmi_tx_cache[spmi_tx_cache_count].va = va;
		spmi_tx_cache_count++;
	}
	return va;
}

static int
spmi_wait_done(vm_offset_t chan_va)
{
	for (uint32_t spin = 0; spin < SPMI_POLL_LIMIT; spin++) {
		uint32_t st = spmi_mmio_rd(chan_va, PMIC_ARB_REG_STATUS);
		if (st & PMIC_ARB_STATUS_DONE) {
			if (st & (PMIC_ARB_STATUS_FAILURE | PMIC_ARB_STATUS_DENIED | PMIC_ARB_STATUS_DROPPED)) {
				return -EIO;
			}
			return 0;
		}
	}
	return -ETIMEDOUT;
}

bool
qcom_spmi_ready(void)
{
	return spmi_initialized;
}

int
qcom_spmi_read(uint8_t sid, uint16_t addr, uint8_t *buf, size_t len)
{
	if (!spmi_initialized || buf == NULL || len == 0 || len > 8) {
		return -EINVAL;
	}

	uint8_t periph = (uint8_t)((addr >> 8) & 0xffu);
	uint8_t offset = (uint8_t)(addr & 0xffu);

	lck_mtx_lock(&spmi_mtx);
	int apid = spmi_find_apid(sid, periph);
	if (apid < 0) {
		lck_mtx_unlock(&spmi_mtx);
		return apid;
	}

	vm_offset_t obs_va = spmi_obsrvr_base + (uint32_t)apid * SPMI_OBSRVR_STRIDE;
	uint32_t cmd = (SPMI_CMD_EXT_READ_LONG << 27) |
	    ((uint32_t)offset << 4) |
	    ((uint32_t)(len - 1u) & 0x07u);
	spmi_mmio_wr(obs_va, PMIC_ARB_REG_CMD, cmd);

	int rc = spmi_wait_done(obs_va);
	if (rc != 0) {
		lck_mtx_unlock(&spmi_mtx);
		return rc;
	}

	uint32_t d0 = spmi_mmio_rd(obs_va, PMIC_ARB_REG_RDATA0);
	uint32_t d1 = (len > 4) ? spmi_mmio_rd(obs_va, PMIC_ARB_REG_RDATA1) : 0;
	lck_mtx_unlock(&spmi_mtx);

	for (size_t i = 0; i < len; i++) {
		if (i < 4) {
			buf[i] = (uint8_t)((d0 >> (i * 8)) & 0xffu);
		} else {
			buf[i] = (uint8_t)((d1 >> ((i - 4) * 8)) & 0xffu);
		}
	}
	return 0;
}

int
qcom_spmi_write(uint8_t sid, uint16_t addr, const uint8_t *buf, size_t len)
{
	if (!spmi_initialized || buf == NULL || len == 0 || len > 8) {
		return -EINVAL;
	}

	uint8_t periph = (uint8_t)((addr >> 8) & 0xffu);
	uint8_t offset = (uint8_t)(addr & 0xffu);

	uint32_t d0 = 0, d1 = 0;
	for (size_t i = 0; i < len; i++) {
		if (i < 4) {
			d0 |= ((uint32_t)buf[i] << (i * 8));
		} else {
			d1 |= ((uint32_t)buf[i] << ((i - 4) * 8));
		}
	}

	lck_mtx_lock(&spmi_mtx);
	int apid = spmi_find_apid(sid, periph);
	if (apid < 0) {
		lck_mtx_unlock(&spmi_mtx);
		return apid;
	}

	vm_offset_t tx_va = spmi_get_tx_va((uint32_t)apid);
	if (tx_va == 0) {
		lck_mtx_unlock(&spmi_mtx);
		return -ENOMEM;
	}

	spmi_mmio_wr(tx_va, PMIC_ARB_REG_WDATA0, d0);
	if (len > 4) {
		spmi_mmio_wr(tx_va, PMIC_ARB_REG_WDATA1, d1);
	}

	uint32_t cmd = (SPMI_CMD_EXT_WRITE_LONG << 27) |
	    ((uint32_t)offset << 4) |
	    ((uint32_t)(len - 1u) & 0x07u);
	spmi_mmio_wr(tx_va, PMIC_ARB_REG_CMD, cmd);

	int rc = spmi_wait_done(tx_va);
	lck_mtx_unlock(&spmi_mtx);
	return rc;
}

void
qcom_spmi_init(void)
{
	uint32_t arg = 0;

	if (spmi_initialized) {
		return;
	}
	if (!PE_parse_boot_argn("qcom_spmi", &arg, sizeof(arg)) || arg == 0) { /* qcom_spmi=1: enable Qualcomm SPMI PMIC arbiter */
		return;
	}
	if (!hakios_board_wants(HAKIOS_HAS_SPMI_QCOM, "qcom_spmi=1")) {
		return;
	}

	vm_offset_t phys = (vm_offset_t)hakios_board()->blocks.spmi;
	if (phys == 0) {
		printf("qcom_spmi: board has no spmi block address\n");
		return;
	}

	spmi_core_base = ml_io_map(phys, SPMI_CORE_MAP_SIZE);
	spmi_obsrvr_base = ml_io_map(phys + SPMI_OBSRVR_PHYS_OFFSET, SPMI_OBSRVR_MAP_SIZE);
	if (spmi_core_base == 0 || spmi_obsrvr_base == 0) {
		printf("qcom_spmi: ml_io_map(0x%llx) failed\n", (unsigned long long)phys);
		return;
	}

	spmi_phys_base = phys;
	uint32_t ver = spmi_mmio_rd(spmi_core_base, PMIC_ARB_VERSION);
	spmi_initialized = true;
	printf("qcom_spmi: PMIC arbiter v0x%08x at 0x%llx (obsrvr mapped) ready\n",
	    ver, (unsigned long long)phys);
}
