/*
 * Qualcomm SM6225 PMIC PON hardware keys driver (Power & Volume Down via SPMI,
 * optional Volume Up via TLMM GPIO).
 *
 * Reads PMIC PON_INT_RT_STS (SID 0, 0x0810) over qcom_spmi_read():
 *   bit 0 (KPDPWR_N_SET): Power / Lock button -> HAKIOS_BUTTON_LOCK
 *   bit 1 (RESIN_N_SET):  Volume Down button  -> HAKIOS_BUTTON_VOLUME_DOWN
 * Optional boot-arg qcom_volup_gpio=N reads active-low Volume Up via qcom_tlmm_get(N).
 *
 * Polls every 20 ms with 2-sample debounce and posts events via hakios_touch_post().
 * Off until asked: boot-arg qcom_keys=1.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios_input.h>
#include <sys/qcom_spmi.h>
#include <sys/qcom_tlmm.h>
#include <sys/systm.h>
#include "hakios_board.h"

#define PMIC_PON_SID                0u
#define PMIC_PON_INT_RT_STS         0x0810u
#define PON_KPDPWR_N_SET            (1u << 0)
#define PON_RESIN_N_SET             (1u << 1)

/* PM6125 GPIO 5 (pinctrl@c000 + (5-1)*0x100 + 0x08 STATUS1): active-low Volume Up on sapphire */
#define PMIC_VOLUP_GPIO_STATUS1     0xc408u
#define PMIC_GPIO_STATUS1_VAL       (1u << 0)

#define KEY_POLL_MS                 20u

static int volup_gpio = -1;
static bool keys_log;

static uint32_t
qcom_keys_sample_mask(void)
{
	uint8_t rt_sts = 0;
	uint8_t gpio_sts = 0;
	uint32_t mask = 0;

	if (qcom_spmi_read(PMIC_PON_SID, PMIC_PON_INT_RT_STS, &rt_sts, 1) == 0) {
		if (rt_sts & PON_KPDPWR_N_SET) {
			mask |= (1u << HAKIOS_BUTTON_LOCK);
		}
		if (rt_sts & PON_RESIN_N_SET) {
			mask |= (1u << HAKIOS_BUTTON_VOLUME_DOWN);
		}
	}
	if (volup_gpio >= 0 && qcom_tlmm_ready()) {
		int v = qcom_tlmm_get((uint32_t)volup_gpio);
		if (v == 0) { /* active-low SoC TLMM override */
			mask |= (1u << HAKIOS_BUTTON_VOLUME_UP);
		}
	} else if (qcom_spmi_read(PMIC_PON_SID, PMIC_VOLUP_GPIO_STATUS1, &gpio_sts, 1) == 0) {
		if ((gpio_sts & PMIC_GPIO_STATUS1_VAL) == 0) { /* active-low PM6125 GPIO 5 */
			mask |= (1u << HAKIOS_BUTTON_VOLUME_UP);
		}
	}
	return mask;
}

static void
qcom_keys_report_diff(uint32_t prev, uint32_t now)
{
	static const uint32_t buttons[] = {
		HAKIOS_BUTTON_LOCK,
		HAKIOS_BUTTON_VOLUME_UP,
		HAKIOS_BUTTON_VOLUME_DOWN,
	};

	for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
		uint32_t b = buttons[i];
		uint32_t bit = 1u << b;
		if ((prev & bit) != (now & bit)) {
			uint32_t type = (now & bit) ? HAKIOS_BUTTON_DOWN : HAKIOS_BUTTON_UP;
			if (keys_log) {
				printf("qcom_keys: button %u %s\n", b,
				    type == HAKIOS_BUTTON_DOWN ? "down" : "up");
			}
			hakios_touch_post(type, b, 0, 0);
		}
	}
}

static void
qcom_keys_thread(void *arg __unused, wait_result_t wr __unused)
{
	uint32_t stable = 0;
	uint32_t last_raw = 0;

	for (;;) {
		uint32_t raw = qcom_keys_sample_mask();
		if (raw == last_raw && raw != stable) {
			qcom_keys_report_diff(stable, raw);
			stable = raw;
		}
		last_raw = raw;
		delay_for_interval(KEY_POLL_MS, NSEC_PER_MSEC);
	}
}

void
qcom_spmi_keys_init(void)
{
	uint32_t arg = 0;
	uint32_t gpio_arg = 0;
	uint32_t log_arg = 0;

	if (!PE_parse_boot_argn("qcom_keys", &arg, sizeof(arg)) || arg == 0) { /* qcom_keys=1: enable Qualcomm PMIC PON keys over SPMI */
		return;
	}
	if (!hakios_board_wants(HAKIOS_HAS_KEYS_QCOM_SPMI, "qcom_keys=1")) {
		return;
	}
	if (!qcom_spmi_ready()) {
		printf("qcom_keys: SPMI arbiter not ready (pass qcom_spmi=1)\n");
		return;
	}

	if (PE_parse_boot_argn("qcom_volup_gpio", &gpio_arg, sizeof(gpio_arg))) { /* qcom_volup_gpio=N: TLMM GPIO pin for active-low Volume Up */
		volup_gpio = (int)gpio_arg;
	}
	if (PE_parse_boot_argn("keylog", &log_arg, sizeof(log_arg)) && log_arg != 0) {
		keys_log = true;
	}

	thread_t th = NULL;
	if (kernel_thread_start(qcom_keys_thread, NULL, &th) == KERN_SUCCESS) {
		thread_deallocate(th);
		printf("qcom_keys: polling PMIC PON (SID %u, 0x%04x), volup_gpio=%d\n",
		    PMIC_PON_SID, PMIC_PON_INT_RT_STS, volup_gpio);
	} else {
		printf("qcom_keys: failed to start polling thread\n");
	}
}
