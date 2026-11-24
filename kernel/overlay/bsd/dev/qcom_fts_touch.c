/*
 * FocalTech FTS (fts_ts @ 0x38) touch controller driver over Qualcomm QUPv3 GENI I2C SE2
 * (`blocks.i2c2` = 0x04a88000 on SM6225 / sapphire).
 *
 * Verified from sapphire.dtb & dmesg:
 *   - Bus:   /soc/qcom,qupv3_0_geni_se@4ac0000/i2c@4a88000 (1-0038)
 *   - Chip:  FocalTech FTS (up to 10 touch points)
 *   - IRQ:   TLMM GPIO 80 (0x50), active-low
 *
 * Off until asked: boot-arg qcom_touch=1.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/hakios_input.h>
#include <sys/qcom_tlmm.h>
#include <sys/systm.h>
#include "hakios_board.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define GENI_I2C_MAP_SIZE           0x4000u
#define SE_GENI_STATUS              0x0040u
#define SE_I2C_TX_TRANS_LEN         0x026cu
#define SE_I2C_RX_TRANS_LEN         0x0270u
#define SE_GENI_M_CMD0              0x0600u
#define SE_GENI_M_IRQ_STATUS        0x0610u
#define SE_GENI_M_IRQ_CLEAR         0x0618u
#define SE_GENI_TX_FIFOn            0x0700u
#define SE_GENI_RX_FIFOn            0x0780u
#define SE_GENI_RX_FIFO_STATUS      0x0804u

#define M_GENI_CMD_ACTIVE           (1u << 0)
#define M_CMD_DONE_EN               (1u << 0)
#define M_GP_IRQ_1_EN               (1u << 6)   /* NACK */
#define M_GP_IRQ_3_EN               (1u << 8)   /* Bus proto / arb lost */
#define M_I2C_ERR_MASK              (M_GP_IRQ_1_EN | M_GP_IRQ_3_EN)

#define I2C_CMD_WRITE               (1u << 27)
#define I2C_CMD_READ                (2u << 27)
#define I2C_SLV_ADDR_SHIFT          9u
#define I2C_STOP_STRETCH            (1u << 2)

#define FTS_I2C_ADDR                0x38u
#define FTS_IRQ_GPIO                80u
#define FTS_REG_CHIP_ID_H           0xA3u
#define FTS_REG_CHIP_ID_L           0x9Fu
#define FTS_REG_TOUCH_DATA          0x00u
#define FTS_MAX_POINTS              10u
#define FTS_REPORT_LEN              (3u + 6u * FTS_MAX_POINTS)

#define FTS_EVT_DOWN                0u
#define FTS_EVT_UP                  1u
#define FTS_EVT_CONTACT             2u

#define GENI_POLL_LIMIT             100000u
#define TOUCH_POLL_MS               8u

static vm_offset_t i2c_base;
static bool touch_initialized;

#ifdef HAKIOS_MMIO_TEST
extern uint32_t hakios_test_rd(vm_offset_t address);
extern void hakios_test_wr(vm_offset_t address, uint32_t value);
#define i2c_rd(at)        hakios_test_rd(i2c_base + (at))
#define i2c_wr(at, value) hakios_test_wr(i2c_base + (at), (value))
#else
static inline uint32_t
i2c_rd(uint32_t at)
{
	return *(volatile uint32_t *)(i2c_base + at);
}

static inline void
i2c_wr(uint32_t at, uint32_t value)
{
	*(volatile uint32_t *)(i2c_base + at) = value;
}
#endif

static bool
geni_i2c_wait_idle(void)
{
	for (uint32_t spin = 0; spin < GENI_POLL_LIMIT; spin++) {
		if ((i2c_rd(SE_GENI_STATUS) & M_GENI_CMD_ACTIVE) == 0) {
			return true;
		}
	}
	return false;
}

static int
geni_i2c_wait_done(void)
{
	for (uint32_t spin = 0; spin < GENI_POLL_LIMIT; spin++) {
		uint32_t st = i2c_rd(SE_GENI_M_IRQ_STATUS);
		if (st & (M_CMD_DONE_EN | M_I2C_ERR_MASK)) {
			i2c_wr(SE_GENI_M_IRQ_CLEAR, st);
			if (st & M_I2C_ERR_MASK) {
				return -EIO;
			}
			return 0;
		}
	}
	return -ETIMEDOUT;
}

static int
fts_i2c_read_reg(uint8_t reg, uint8_t *buf, size_t len)
{
	if (!geni_i2c_wait_idle()) {
		return -ETIMEDOUT;
	}

	/* 1. Write register address (with repeated-start stretch) */
	i2c_wr(SE_I2C_TX_TRANS_LEN, 1u);
	i2c_wr(SE_GENI_M_CMD0, I2C_CMD_WRITE | ((uint32_t)FTS_I2C_ADDR << I2C_SLV_ADDR_SHIFT) | I2C_STOP_STRETCH);
	i2c_wr(SE_GENI_TX_FIFOn, reg);

	int rc = geni_i2c_wait_done();
	if (rc != 0) {
		return rc;
	}

	/* 2. Read payload bytes */
	i2c_wr(SE_I2C_RX_TRANS_LEN, (uint32_t)len);
	i2c_wr(SE_GENI_M_CMD0, I2C_CMD_READ | ((uint32_t)FTS_I2C_ADDR << I2C_SLV_ADDR_SHIFT));

	rc = geni_i2c_wait_done();
	if (rc != 0) {
		return rc;
	}

	size_t idx = 0;
	while (idx < len) {
		uint32_t word = i2c_rd(SE_GENI_RX_FIFOn);
		for (size_t b = 0; b < 4 && idx < len; b++, idx++) {
			buf[idx] = (uint8_t)((word >> (b * 8)) & 0xffu);
		}
	}
	return 0;
}

static void
fts_process_report(const uint8_t *rep)
{
	uint32_t disp_w = hakios_board()->display.width;
	uint32_t disp_h = hakios_board()->display.height;
	uint8_t count = rep[2] & 0x0fu;

	if (disp_w == 0 || disp_h == 0) {
		return;
	}
	if (count > FTS_MAX_POINTS) {
		count = FTS_MAX_POINTS;
	}

	for (uint8_t i = 0; i < count; i++) {
		const uint8_t *p = &rep[3u + i * 6u];
		uint32_t evt = (p[0] >> 6) & 0x03u;
		uint32_t finger = (p[2] >> 4) & 0x0fu;
		uint32_t raw_x = (((uint32_t)(p[0] & 0x0fu)) << 8) | p[1];
		uint32_t raw_y = (((uint32_t)(p[2] & 0x0fu)) << 8) | p[3];

		uint32_t x = (raw_x * 65535u) / disp_w;
		uint32_t y = (raw_y * 65535u) / disp_h;
		if (x > 65535u) {
			x = 65535u;
		}
		if (y > 65535u) {
			y = 65535u;
		}

		uint32_t type = HAKIOS_TOUCH_MOVE;
		if (evt == FTS_EVT_DOWN) {
			type = HAKIOS_TOUCH_DOWN;
		} else if (evt == FTS_EVT_UP) {
			type = HAKIOS_TOUCH_UP;
		} else if (evt != FTS_EVT_CONTACT) {
			continue;
		}
		hakios_touch_post(type, finger, x, y);
	}
}

static void
fts_touch_thread(void *arg __unused, wait_result_t wr __unused)
{
	uint8_t report[FTS_REPORT_LEN];

	for (;;) {
		bool should_read = true;
		if (qcom_tlmm_ready()) {
			/* IRQ GPIO 80 is active-low when a touch report is pending */
			should_read = (qcom_tlmm_get(FTS_IRQ_GPIO) == 0);
		}
		if (should_read && fts_i2c_read_reg(FTS_REG_TOUCH_DATA, report, sizeof(report)) == 0) {
			fts_process_report(report);
		}
		delay_for_interval(TOUCH_POLL_MS, NSEC_PER_MSEC);
	}
}

void
qcom_fts_touch_init(void)
{
	uint32_t arg = 0;

	if (touch_initialized) {
		return;
	}
	if (!PE_parse_boot_argn("qcom_touch", &arg, sizeof(arg)) || arg == 0) { /* qcom_touch=1: enable FocalTech FTS touch over QUPv3 GENI I2C2 */
		return;
	}
	if (!hakios_board_wants(HAKIOS_HAS_TOUCH_FTS_I2C, "qcom_touch=1")) {
		return;
	}

	vm_offset_t phys = (vm_offset_t)hakios_board()->blocks.i2c2;
	if (phys == 0) {
		printf("qcom_touch: board has no i2c2 block address\n");
		return;
	}

	/* Guard against NOC hang if QUPv3 SE2 clock (GCC_QUPV3_WRAP0_S2_CBCR @ 0x01417270) is gated by ABL */
	vm_offset_t gcc_se2 = ml_io_map(0x01417000ULL, 0x1000u);
	if (gcc_se2 == 0 || (*(volatile uint32_t *)(gcc_se2 + 0x270u) & (1u << 31)) != 0) {
		printf("qcom_touch: QUPv3 SE2 I2C clock is gated by ABL; skipping probe\n");
		return;
	}

	i2c_base = ml_io_map(phys, GENI_I2C_MAP_SIZE);
	if (i2c_base == 0) {
		printf("qcom_touch: ml_io_map(0x%llx) failed\n", (unsigned long long)phys);
		return;
	}

	uint8_t id_h = 0, id_l = 0;
	if (fts_i2c_read_reg(FTS_REG_CHIP_ID_H, &id_h, 1) != 0 ||
	    fts_i2c_read_reg(FTS_REG_CHIP_ID_L, &id_l, 1) != 0) {
		printf("qcom_touch: FocalTech FTS probe failed on I2C2 (0x%llx @ 0x%02x); polling disabled\n",
		    (unsigned long long)phys, FTS_I2C_ADDR);
		return;
	}
	printf("qcom_touch: FocalTech FTS chip ID 0x%02x%02x on I2C2 (0x%llx @ 0x%02x)\n",
	    id_h, id_l, (unsigned long long)phys, FTS_I2C_ADDR);

	touch_initialized = true;

	thread_t th = NULL;
	if (kernel_thread_start(fts_touch_thread, NULL, &th) == KERN_SUCCESS) {
		thread_deallocate(th);
	} else {
		printf("qcom_touch: failed to start polling thread\n");
	}
}
