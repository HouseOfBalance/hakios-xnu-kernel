/*
 * Qualcomm QUPv3 GENI UART debug serial driver interface for SM6225.
 */
#ifndef _SYS_QCOM_GENI_UART_H_
#define _SYS_QCOM_GENI_UART_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef KERNEL
void qcom_geni_uart_init(void);
bool qcom_geni_uart_ready(void);
int qcom_geni_uart_putc(char c);
int qcom_geni_uart_puts(const char *s);
int qcom_geni_uart_getc(void);
#endif

#endif /* _SYS_QCOM_GENI_UART_H_ */
