/*
 * Qualcomm SM6225 TLMM (Top-Level Mode Multiplexer) GPIO controller driver.
 *
 * SM6225 has 130 GPIOs (0..129) mapped at blocks.gpio (0x0f100000) with a 0x1000-byte
 * stride per pin. Off until asked: boot-arg qcom_tlmm=1.
 */
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/qcom_tlmm.h>
#include <sys/systm.h>
#include "hakios_board.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define TLMM_MAX_PINS           130u
#define TLMM_PIN_STRIDE         0x1000u
#define TLMM_MAP_SIZE           (TLMM_MAX_PINS * TLMM_PIN_STRIDE)

#define TLMM_GPIO_CFG(n)        ((n) * TLMM_PIN_STRIDE + 0x00u)
#define TLMM_GPIO_IN_OUT(n)     ((n) * TLMM_PIN_STRIDE + 0x04u)
#define TLMM_GPIO_INTR_CFG(n)   ((n) * TLMM_PIN_STRIDE + 0x08u)
#define TLMM_GPIO_INTR_STAT(n)  ((n) * TLMM_PIN_STRIDE + 0x0cu)

#define TLMM_CFG_PULL_SHIFT     0u
#define TLMM_CFG_PULL_MASK      (0x3u << TLMM_CFG_PULL_SHIFT)
#define TLMM_CFG_FUNC_SHIFT     2u
#define TLMM_CFG_FUNC_MASK      (0xfu << TLMM_CFG_FUNC_SHIFT)
#define TLMM_CFG_DRV_SHIFT      6u
#define TLMM_CFG_DRV_MASK       (0x7u << TLMM_CFG_DRV_SHIFT)
#define TLMM_CFG_OE             (1u << 9)

#define TLMM_IN_BIT             (1u << 0)
#define TLMM_OUT_BIT            (1u << 1)

static vm_offset_t tlmm_base;
static bool tlmm_initialized;

#ifdef HAKIOS_MMIO_TEST
extern uint32_t hakios_test_rd(vm_offset_t address);
extern void hakios_test_wr(vm_offset_t address, uint32_t value);
#define tlmm_rd(at)        hakios_test_rd(tlmm_base + (at))
#define tlmm_wr(at, value) hakios_test_wr(tlmm_base + (at), (value))
#else
static inline uint32_t
tlmm_rd(uint32_t at)
{
	return *(volatile uint32_t *)(tlmm_base + at);
}

static inline void
tlmm_wr(uint32_t at, uint32_t value)
{
	*(volatile uint32_t *)(tlmm_base + at) = value;
}
#endif

static uint32_t
tlmm_drv_encode(uint32_t drv_ma)
{
	if (drv_ma <= 2u) {
		return 0u;
	}
	uint32_t val = (drv_ma - 2u) / 2u;
	return val > 7u ? 7u : val;
}

bool
qcom_tlmm_ready(void)
{
	return tlmm_initialized;
}

int
qcom_tlmm_configure(uint32_t pin, uint32_t func, uint32_t pull, uint32_t drv_ma, bool output)
{
	if (!tlmm_initialized || pin >= TLMM_MAX_PINS) {
		return -EINVAL;
	}

	uint32_t cfg = tlmm_rd(TLMM_GPIO_CFG(pin));
	cfg &= ~(TLMM_CFG_PULL_MASK | TLMM_CFG_FUNC_MASK | TLMM_CFG_DRV_MASK | TLMM_CFG_OE);
	cfg |= (pull & 0x3u) << TLMM_CFG_PULL_SHIFT;
	cfg |= (func & 0xfu) << TLMM_CFG_FUNC_SHIFT;
	cfg |= tlmm_drv_encode(drv_ma) << TLMM_CFG_DRV_SHIFT;
	if (output) {
		cfg |= TLMM_CFG_OE;
	}
	tlmm_wr(TLMM_GPIO_CFG(pin), cfg);
	return 0;
}

int
qcom_tlmm_set(uint32_t pin, bool high)
{
	if (!tlmm_initialized || pin >= TLMM_MAX_PINS) {
		return -EINVAL;
	}

	uint32_t val = tlmm_rd(TLMM_GPIO_IN_OUT(pin));
	if (high) {
		val |= TLMM_OUT_BIT;
	} else {
		val &= ~TLMM_OUT_BIT;
	}
	tlmm_wr(TLMM_GPIO_IN_OUT(pin), val);
	return 0;
}

int
qcom_tlmm_get(uint32_t pin)
{
	if (!tlmm_initialized || pin >= TLMM_MAX_PINS) {
		return -EINVAL;
	}
	return (tlmm_rd(TLMM_GPIO_IN_OUT(pin)) & TLMM_IN_BIT) ? 1 : 0;
}

void
qcom_tlmm_init(void)
{
	uint32_t arg = 0;

	if (tlmm_initialized) {
		return;
	}
	if (!PE_parse_boot_argn("qcom_tlmm", &arg, sizeof(arg)) || arg == 0) { /* qcom_tlmm=1: enable Qualcomm SM6225 TLMM GPIO controller */
		return;
	}
	if (!hakios_board_wants(HAKIOS_HAS_TLMM_QCOM, "qcom_tlmm=1")) {
		return;
	}

	vm_offset_t phys = (vm_offset_t)hakios_board()->blocks.gpio;
	if (phys == 0) {
		printf("qcom_tlmm: board has no gpio block address\n");
		return;
	}

	tlmm_base = ml_io_map(phys, TLMM_MAP_SIZE);
	if (tlmm_base == 0) {
		printf("qcom_tlmm: ml_io_map(0x%llx) failed\n", (unsigned long long)phys);
		return;
	}

	tlmm_initialized = true;
	printf("qcom_tlmm: SM6225 TLMM (%u pins) at 0x%llx ready\n", TLMM_MAX_PINS, (unsigned long long)phys);
}
