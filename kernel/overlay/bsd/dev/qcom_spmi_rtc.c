/*
 * Qualcomm PM6125 SPMI Real-Time Clock (qcom,pm8941-rtc) driver for XNU (SM6225 / sapphire).
 *
 * Verified from sapphire.dtb:
 *   /soc/qcom,spmi@1c40000/qcom,pm6125@0/qcom,pm6125_rtc (SID 0, base 0x6000)
 *
 * Reads the 32-bit hardware RTC seconds counter via qcom_spmi_read() and exposes
 * the value via sysctl (machdep.qcom_rtc). Enabled with boot-arg qcom_rtc=1.
 */
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <pexpert/pexpert.h>
#include <sys/qcom_spmi.h>
#include <sys/hakios_input.h>

#define PM6125_RTC_SID          0
#define PM6125_RTC_BASE         0x6000u
#define PM6125_RTC_CTRL         (PM6125_RTC_BASE + 0x46u)
#define PM6125_RTC_READ_DATA0   (PM6125_RTC_BASE + 0x48u)

static uint32_t qcom_rtc_ready;

uint32_t
qcom_spmi_rtc_read_secs(void)
{
	uint8_t b[4] = {0, 0, 0, 0};
	uint8_t b0_again = 0;

	if (!qcom_rtc_ready) {
		return 0;
	}
	for (int tries = 0; tries < 2; tries++) {
		if (qcom_spmi_read(PM6125_RTC_SID, PM6125_RTC_READ_DATA0, b, 4) != 0) {
			return 0;
		}
		if (qcom_spmi_read(PM6125_RTC_SID, PM6125_RTC_READ_DATA0, &b0_again, 1) == 0 &&
		    b0_again == b[0]) {
			break;
		}
	}
	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static int
sysctl_qcom_rtc(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	uint32_t secs = qcom_spmi_rtc_read_secs();
	return SYSCTL_OUT(req, &secs, sizeof(secs));
}

SYSCTL_PROC(_machdep, OID_AUTO, qcom_rtc, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, sysctl_qcom_rtc, "IU", "Qualcomm PM6125 SPMI RTC seconds");

void
qcom_spmi_rtc_init(void)
{
	uint32_t en = 0;
	uint8_t ctrl = 0;

	if (!PE_parse_boot_argn("qcom_rtc", &en, sizeof(en)) || !en) {
		return;
	}
	if (qcom_spmi_read(PM6125_RTC_SID, PM6125_RTC_CTRL, &ctrl, 1) != 0) {
		printf("qcom_rtc: SPMI read failed on PM6125 SID %u\n", PM6125_RTC_SID);
		return;
	}
	qcom_rtc_ready = 1;
	uint32_t secs = qcom_spmi_rtc_read_secs();
	printf("qcom_rtc: PM6125 RTC ready (ctrl=0x%02x, secs=%u)\n", ctrl, secs);
}
