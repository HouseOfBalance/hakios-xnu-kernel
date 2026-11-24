/*
 * Qualcomm Adreno 610 (KGSL-3D0) early bring-up & diagnostic driver for SM6225 (`sapphire`).
 *
 * Verified from sapphire.dtb:
 *   - /soc/qcom,kgsl-3d0@5900000 (reg = 0x05900000, size = 0x90000)
 *   - /soc/kgsl-smmu@0x59a0000   (reg = 0x059a0000, size = 0x10000)
 *
 * Maps the Adreno 610 RBBM register window, probes chip ID and RBBM status, and exposes
 * read-only diagnostics under kern.hakios.adreno_*.
 *
 * Off until asked: boot-arg qcom_adreno=1.
 */
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include "hakios_board.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define ADRENO_MAP_SIZE             0x90000u

/* Adreno A6xx RBBM register offsets (dword index * 4) */
#define A6XX_RBBM_INT_0_STATUS      (0x0201u * 4u)
#define A6XX_RBBM_STATUS            (0x0210u * 4u)
#define A6XX_RBBM_STATUS3           (0x0213u * 4u)
#define A6XX_CP_STATUS_1            (0x0826u * 4u)

static vm_offset_t adreno_base;
static int adreno_ready;
static int adreno_rbbm_status;
static int adreno_int0_status;

SYSCTL_DECL(_kern_hakios);
SYSCTL_INT(_kern_hakios, OID_AUTO, adreno_ready, CTLFLAG_RD | CTLFLAG_LOCKED,
    &adreno_ready, 0, "1 once Adreno 610 MMIO window is mapped and probed");
SYSCTL_INT(_kern_hakios, OID_AUTO, adreno_rbbm_status, CTLFLAG_RD | CTLFLAG_LOCKED,
    &adreno_rbbm_status, 0, "last read A6XX_RBBM_STATUS register");
SYSCTL_INT(_kern_hakios, OID_AUTO, adreno_int0_status, CTLFLAG_RD | CTLFLAG_LOCKED,
    &adreno_int0_status, 0, "last read A6XX_RBBM_INT_0_STATUS register");

#ifdef HAKIOS_MMIO_TEST
extern uint32_t hakios_test_rd(vm_offset_t address);
extern void hakios_test_wr(vm_offset_t address, uint32_t value);
#define adreno_rd(at)        hakios_test_rd(adreno_base + (at))
#define adreno_wr(at, value) hakios_test_wr(adreno_base + (at), (value))
#else
static inline uint32_t
adreno_rd(uint32_t at)
{
	return *(volatile uint32_t *)(adreno_base + at);
}

static inline void
adreno_wr(uint32_t at, uint32_t value)
{
	*(volatile uint32_t *)(adreno_base + at) = value;
}
#endif

void
qcom_adreno_init(void)
{
	uint32_t arg = 0;

	if (adreno_ready) {
		return;
	}
	if (!PE_parse_boot_argn("qcom_adreno", &arg, sizeof(arg)) || arg == 0) { /* qcom_adreno=1: probe Qualcomm Adreno 610 GPU (KGSL-3D0) */
		return;
	}
	if (!hakios_board_wants(HAKIOS_HAS_GPU_ADRENO_610, "qcom_adreno=1")) {
		return;
	}

	vm_offset_t phys = (vm_offset_t)hakios_board()->blocks.adreno;
	if (phys == 0) {
		printf("qcom_adreno: board has no adreno block address\n");
		return;
	}

	/*
	 * Guard against NOC bus hang on SM6225 (Khaje): ABL leaves Adreno 610 power-gated
	 * and clock-gated. Check GCC_GPU_CFG_AHB_CBCR (0x01436004, bit 31 CLK_OFF == 0) and
	 * GPU_CC_CX_GDSC (0x0599106c, bit 31 PWR_ON == 1) before touching 0x05900000.
	 */
	vm_offset_t gcc_gpu = ml_io_map(0x01436000ULL, 0x1000u);
	if (gcc_gpu == 0 || (*(volatile uint32_t *)(gcc_gpu + 0x004u) & (1u << 31)) != 0) {
		printf("qcom_adreno: GCC_GPU_CFG_AHB_CBCR is gated; skipping RBBM probe to avoid NOC hang\n");
		return;
	}
	vm_offset_t gpucc = ml_io_map(0x05990000ULL, 0x2000u);
	if (gpucc == 0 || (*(volatile uint32_t *)(gpucc + 0x106cu) & (1u << 31)) == 0) {
		printf("qcom_adreno: GPU_CC_CX_GDSC is powered off; skipping RBBM probe to avoid NOC hang\n");
		return;
	}

	adreno_base = ml_io_map(phys, ADRENO_MAP_SIZE);
	if (adreno_base == 0) {
		printf("qcom_adreno: ml_io_map(0x%llx) failed\n", (unsigned long long)phys);
		return;
	}

	uint32_t rbbm = adreno_rd(A6XX_RBBM_STATUS);
	uint32_t int0 = adreno_rd(A6XX_RBBM_INT_0_STATUS);
	adreno_rbbm_status = (int)rbbm;
	adreno_int0_status = (int)int0;
	adreno_ready = 1;

	printf("qcom_adreno: Adreno 610 at 0x%llx RBBM_STATUS=0x%08x INT0=0x%08x\n",
	    (unsigned long long)phys, rbbm, int0);
}
