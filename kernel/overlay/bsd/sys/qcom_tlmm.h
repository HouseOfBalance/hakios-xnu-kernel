/*
 * Qualcomm SM6225 TLMM (Top-Level Mode Multiplexer) GPIO controller interface.
 */
#ifndef _SYS_QCOM_TLMM_H_
#define _SYS_QCOM_TLMM_H_

#include <stdbool.h>
#include <stdint.h>

#define QCOM_TLMM_PULL_NONE     0u
#define QCOM_TLMM_PULL_DOWN     1u
#define QCOM_TLMM_PULL_KEEPER   2u
#define QCOM_TLMM_PULL_UP       3u

#ifdef KERNEL
void qcom_tlmm_init(void);
bool qcom_tlmm_ready(void);
int qcom_tlmm_configure(uint32_t pin, uint32_t func, uint32_t pull, uint32_t drv_ma, bool output);
int qcom_tlmm_set(uint32_t pin, bool high);
int qcom_tlmm_get(uint32_t pin);
#endif

#endif /* _SYS_QCOM_TLMM_H_ */
