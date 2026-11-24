/*
 * Qualcomm SM6225 PMIC Fuel Gauge (QG / PMI632) battery driver over SPMI.
 *
 * Reads state-of-charge, voltage, current, and charging status from the Qualcomm
 * Gauge (QG) peripheral on SPMI SID 2 (base 0x4800) and reports them to
 * hakios_battery_report() (sysctls kern.hakios.battery_* + HakiosBattery.cpp).
 *
 * Off until asked: boot-arg qcom_batt=1.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios_input.h>
#include <sys/qcom_spmi.h>
#include <sys/systm.h>
#include "hakios_board.h"

#define QG_SPMI_SID                 2u
#define QG_STATUS1_REG              0x4808u
#define QG_SOC_MONOTONIC_REG        0x4809u
#define QG_VBAT_DATA0_REG           0x4840u /* 16-bit little-endian */
#define QG_IBAT_DATA0_REG           0x4842u /* signed 16-bit little-endian */

#define QG_STATUS1_BATT_PRESENT     (1u << 0)
#define QG_STATUS1_CHARGING         (1u << 1)
#define QG_STATUS1_FULL             (1u << 2)

#define BATT_POLL_MS                5000u

static bool
qcom_batt_read_once(struct hakios_battery_state *out)
{
	uint8_t status1 = 0;
	uint8_t soc_raw = 0;
	uint8_t vbat_raw[2] = { 0, 0 };
	uint8_t ibat_raw[2] = { 0, 0 };

	if (qcom_spmi_read(QG_SPMI_SID, QG_STATUS1_REG, &status1, 1) != 0) {
		return false;
	}
	if (qcom_spmi_read(QG_SPMI_SID, QG_SOC_MONOTONIC_REG, &soc_raw, 1) != 0) {
		return false;
	}
	if (qcom_spmi_read(QG_SPMI_SID, QG_VBAT_DATA0_REG, vbat_raw, 2) != 0) {
		return false;
	}
	if (qcom_spmi_read(QG_SPMI_SID, QG_IBAT_DATA0_REG, ibat_raw, 2) != 0) {
		return false;
	}

	uint16_t v_code = (uint16_t)vbat_raw[0] | ((uint16_t)vbat_raw[1] << 8);
	int16_t i_code = (int16_t)((uint16_t)ibat_raw[0] | ((uint16_t)ibat_raw[1] << 8));

	bzero(out, sizeof(*out));
	out->size = sizeof(*out);
	out->percent_hundredths = ((int32_t)soc_raw * 10000) / 255;
	/* 194.637 uV per LSB for Qualcomm QG VBAT ADC */
	out->voltage_uv = (int32_t)(((int64_t)v_code * 194637LL) / 1000LL);
	/* 152.588 uA per LSB for Qualcomm QG IBAT ADC */
	out->current_ua = (int32_t)(((int64_t)i_code * 152588LL) / 1000LL);
	out->temperature_hundredths = 2500; /* 25.00 C default until ADC_TM is wired */
	out->present = (status1 & QG_STATUS1_BATT_PRESENT) ? 1 : 0;
	out->external = (status1 & (QG_STATUS1_CHARGING | QG_STATUS1_FULL)) ? 1 : 0;
	if (status1 & QG_STATUS1_FULL) {
		out->status = 4; /* POWER_SUPPLY_STATUS_FULL */
	} else if (status1 & QG_STATUS1_CHARGING) {
		out->status = 1; /* POWER_SUPPLY_STATUS_CHARGING */
	} else {
		out->status = 2; /* POWER_SUPPLY_STATUS_DISCHARGING */
	}
	out->design_uah = (int32_t)hakios_board()->battery.design_uah;
	return true;
}

static void
qcom_batt_thread(void *arg __unused, wait_result_t wr __unused)
{
	struct hakios_battery_state st;

	for (;;) {
		if (qcom_batt_read_once(&st)) {
			hakios_battery_report(&st);
		}
		delay_for_interval(BATT_POLL_MS, NSEC_PER_MSEC);
	}
}

void
qcom_spmi_battery_init(void)
{
	uint32_t arg = 0;

	if (!PE_parse_boot_argn("qcom_batt", &arg, sizeof(arg)) || arg == 0) { /* qcom_batt=1: enable Qualcomm PMIC fuel gauge over SPMI */
		return;
	}
	if (!hakios_board_wants(HAKIOS_HAS_BATTERY_QCOM_SPMI, "qcom_batt=1")) {
		return;
	}
	if (!qcom_spmi_ready()) {
		printf("qcom_batt: SPMI arbiter not ready (pass qcom_spmi=1)\n");
		return;
	}

	struct hakios_battery_state st;
	if (qcom_batt_read_once(&st)) {
		hakios_battery_report(&st);
		printf("qcom_batt: initial SOC=%d.%02d%% V=%dmV I=%dmA status=%d\n",
		    st.percent_hundredths / 100, st.percent_hundredths % 100,
		    st.voltage_uv / 1000, st.current_ua / 1000, st.status);
	} else {
		printf("qcom_batt: initial SPMI read failed; will keep polling\n");
	}

	thread_t th = NULL;
	if (kernel_thread_start(qcom_batt_thread, NULL, &th) == KERN_SUCCESS) {
		thread_deallocate(th);
	} else {
		printf("qcom_batt: failed to start polling thread\n");
	}
}
