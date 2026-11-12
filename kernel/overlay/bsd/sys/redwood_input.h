/*
 * /dev/redwood-touch: touch samples on their way from whoever produces them to the
 * program that turns them into HID events for the window server.
 *
 * iOS expects touches from Apple's multitouch driver through IOHIDFamily; neither exists
 * here. Instead a producer -- a kernel driver for a real touch controller or for an emulated
 * pointer, or a user program replaying a script -- posts samples, and code of ours running
 * inside backboardd reads them and synthesizes the events. Both sides use the same record.
 *
 *   write(): one or more records (time is ignored; the kernel stamps them)
 *   read():  blocks until there is at least one record, returns as many whole ones as fit
 */
#ifndef _SYS_REDWOOD_INPUT_H_
#define _SYS_REDWOOD_INPUT_H_

#include <stdint.h>

#define REDWOOD_TOUCH_DEVICE    "/dev/redwood-touch"

#define REDWOOD_TOUCH_DOWN      1       /* a finger has landed */
#define REDWOOD_TOUCH_MOVE      2
#define REDWOOD_TOUCH_UP        3       /* it has lifted (x, y: where it was last) */
#define REDWOOD_BUTTON_DOWN     4       /* a hardware button; `finger` says which (REDWOOD_BUTTON_*), x and y are 0 */
#define REDWOOD_BUTTON_UP       5

#define REDWOOD_BUTTON_LOCK     1       /* the side button */
#define REDWOOD_BUTTON_VOLUME_UP 2
#define REDWOOD_BUTTON_VOLUME_DOWN 3
#define REDWOOD_BUTTON_HOME     4

struct redwood_touch {
	uint64_t time;                  /* mach_absolute_time() when the sample was posted */
	uint32_t type;                  /* REDWOOD_TOUCH_* or REDWOOD_BUTTON_DOWN/UP */
	uint32_t finger;                /* which finger, from 0 (or which button) */
	uint32_t x, y;                  /* position on the display, 0 = left/top edge, 65535 = right/bottom edge */
};

#ifdef KERNEL
/* For drivers; not from interrupt context. */
void redwood_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y);
void redwood_input_init(void);
void redwood_virtio_input_init(void);   /* boot-arg virtio_input=1: QEMU's virtio pointer and keyboard */
void redwood_tcm_init(void);            /* boot-arg touchscreen=2: the Infinix's OmniVision TD4160 */
void redwood_mali_init(void);            /* boot-arg mali=1: the Infinix's Mali-G52 brought up */
void redwood_mtkkeys_init(void);         /* boot-arg buttons=2: the Infinix's power and volume keys */
/*
 * How the battery is, as a board's gauge driver reads it (bsd/dev/redwood_mtkbatt.c, with mtkbatt=1):
 * handed to bsd/dev/redwood_battery.c, which keeps it for the sysctls kern.redwood.battery_* and
 * passes it on to iokit/Kernel/RedwoodBattery.cpp, the power source that iOS reads. The structure
 * is repeated there (this header is not seen from iokit/), and `size` tells whether the two agree.
 */
struct redwood_battery_state {
	uint32_t size;                  /* sizeof(struct redwood_battery_state) */
	int32_t percent_hundredths;     /* the charge, 0..10000 */
	int32_t voltage_uv;
	int32_t current_ua;             /* more than 0 while the battery takes charge */
	int32_t temperature_hundredths; /* of a degree Celsius */
	int32_t present;                /* 1 if there is a battery */
	int32_t external;               /* 1 if a charger or a computer is on the USB port */
	int32_t status;                 /* Linux's POWER_SUPPLY_STATUS_*: 1 charging, 2 discharging, 3 not charging, 4 full */
	int32_t charge_uah, full_uah, design_uah, cycles;      /* 0 when not known (yet) */
	int32_t usb_type;               /* Linux's POWER_SUPPLY_USB_TYPE_*: 1 a computer's port, 2 a charger, ... */
	int32_t usb_voltage_uv, usb_current_ua;                 /* what the port gives */
};
void redwood_battery_report(const struct redwood_battery_state *state);
void redwood_net_init(void);            /* en0, if there is something to carry its frames: sys/redwood_net.h */
void redwood_scp_init(void);            /* boot-arg scp_step=N: the MT6768's SCP (the sensor hub), steps 1 to N */
void redwood_connsys_init(void);        /* boot-arg connsys_step=N: the Infinix's CONNSYS (Wi-Fi/BT), steps 1 to N */
void redwood_pwrap_lock(void);          /* the MT6358 PMIC wrapper (0x1000d000) takes one transaction at a time: the camera, the keys/GPU-voltage and CONNSYS drivers each hold this for a whole one */
void redwood_pwrap_unlock(void);
void redwood_mtkbatt_init(void);        /* boot-arg mtkbatt=1: the Infinix's battery from the PMIC's BATADC, to iOS's power source */
void redwood_mtkcam_init(void);         /* boot-arg mtkcam=1: the Infinix's main camera (S5K JN1), one capture into memory, then all off */
uint8_t *redwood_connsys_file(const char *name, uint32_t *size);       /* /fw/connsys/NAME, kfree_data's */
void redwood_mtkwifi_start(uint32_t last);     /* connsys_step 12 and on: the Wi-Fi itself (redwood_mtkwifi.c) */
void redwood_mtkwifi_net_early(void);        /* connsys_step 16 and on: en0, before the radio is up */
int redwood_wifi_tell(const char *format, ...) __printflike(1, 2);     /* a line of the station's for whoever runs the phone (redwood_wifi_given.c) */
/* the network to join (fw/network of the small image): name and passphrase, buffer sizes in and lengths out */
bool redwood_wifi_network(uint8_t *name, unsigned *name_length, uint8_t *passphrase, unsigned *passphrase_length);
#endif

#endif /* _SYS_REDWOOD_INPUT_H_ */
