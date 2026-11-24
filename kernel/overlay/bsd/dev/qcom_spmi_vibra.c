/*
 * Qualcomm PM7250B SPMI LDO Vibrator / Haptics driver (qcom,qpnp-vibrator-ldo)
 * for XNU on SM6225 (sapphire).
 *
 * Verified from sapphire.dtb:
 *   /soc/qcom,spmi@1c40000/qcom,pm7250b@3/qcom,vibrator@5300
 *   SID = 3, base = 0x5300, qcom,vib-ldo-volt-uv = 3000000 (3.0V)
 *
 * Enabled with boot-arg qcom_vibra=1. Exposes sysctl machdep.qcom_vibra_ms
 * to trigger a timed haptic buzz (1..2000 ms).
 */
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/qcom_spmi.h>
#include <sys/hakios_input.h>

#define PM7250B_VIB_SID         3
#define PM7250B_VIB_BASE        0x5300u
#define PM7250B_VIB_VSET_LB     (PM7250B_VIB_BASE + 0x40u)
#define PM7250B_VIB_EN_CTL      (PM7250B_VIB_BASE + 0x46u)
#define PM7250B_VIB_EN_BIT      0x80u
#define PM7250B_VIB_DEFAULT_MV  3000u

static uint32_t qcom_vibra_ready;

void
qcom_spmi_vibra_set(int enable, uint32_t mv)
{
	uint8_t vset[2];
	uint8_t en_val;

	if (!qcom_vibra_ready) {
		return;
	}
	if (enable) {
		if (mv < 1500u) {
			mv = 1500u;
		}
		if (mv > 3500u) {
			mv = 3500u;
		}
		vset[0] = (uint8_t)(mv & 0xffu);
		vset[1] = (uint8_t)((mv >> 8) & 0xffu);
		en_val = PM7250B_VIB_EN_BIT;
		(void)qcom_spmi_write(PM7250B_VIB_SID, PM7250B_VIB_VSET_LB, vset, 2);
		(void)qcom_spmi_write(PM7250B_VIB_SID, PM7250B_VIB_EN_CTL, &en_val, 1);
	} else {
		en_val = 0x00u;
		(void)qcom_spmi_write(PM7250B_VIB_SID, PM7250B_VIB_EN_CTL, &en_val, 1);
	}
}

void
qcom_spmi_vibra_buzz(uint32_t ms)
{
	if (!qcom_vibra_ready || ms == 0) {
		return;
	}
	if (ms > 2000u) {
		ms = 2000u;
	}
	qcom_spmi_vibra_set(1, PM7250B_VIB_DEFAULT_MV);
	delay_for_interval(ms, NSEC_PER_MSEC);
	qcom_spmi_vibra_set(0, 0);
}

static int
sysctl_qcom_vibra(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	uint32_t ms = 0;
	int err = sysctl_handle_int(oidp, &ms, 0, req);
	if (err == 0 && req->newptr != USER_ADDR_NULL && ms > 0) {
		qcom_spmi_vibra_buzz(ms);
	}
	return err;
}

SYSCTL_PROC(_machdep, OID_AUTO, qcom_vibra_ms, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_qcom_vibra, "IU", "Trigger PM7250B haptic vibrator for N ms");

void
qcom_spmi_vibra_init(void)
{
	uint32_t en = 0;
	uint8_t ctl = 0;

	if (!PE_parse_boot_argn("qcom_vibra", &en, sizeof(en)) || !en) {
		return;
	}
	if (qcom_spmi_read(PM7250B_VIB_SID, PM7250B_VIB_EN_CTL, &ctl, 1) != 0) {
		printf("qcom_vibra: SPMI read failed on PM7250B SID %u\n", PM7250B_VIB_SID);
		return;
	}
	qcom_vibra_ready = 1;
	printf("qcom_vibra: PM7250B LDO vibrator ready (SID %u @ 0x%04x, ctl=0x%02x)\n",
	    PM7250B_VIB_SID, PM7250B_VIB_BASE, ctl);
}
