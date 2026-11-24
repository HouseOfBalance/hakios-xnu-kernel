/*
 * Qualcomm SPMI PMIC Arbiter v5 driver interface for SM6225 (Snapdragon 685).
 */
#ifndef _SYS_QCOM_SPMI_H_
#define _SYS_QCOM_SPMI_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef KERNEL
void qcom_spmi_init(void);
bool qcom_spmi_ready(void);

/*
 * Read or write 1..8 bytes at SPMI slave `sid` (0..15) and 16-bit register `addr`
 * (high byte = peripheral ID, low byte = register offset within peripheral).
 * Returns 0 on success, negative errno on failure.
 */
int qcom_spmi_read(uint8_t sid, uint16_t addr, uint8_t *buf, size_t len);
int qcom_spmi_write(uint8_t sid, uint16_t addr, const uint8_t *buf, size_t len);
#endif

#endif /* _SYS_QCOM_SPMI_H_ */
