/*
 * Qualcomm QUPv3 GENI Serial Engine UART driver for SM6225 (Snapdragon 685).
 *
 * Maps platform.uart0 (0x04a90000 on SM6225), which ABL leaves clocked and configured
 * for 115200 8N1, and provides bounded TX/RX polling helpers.
 *
 * Off until asked: boot-arg qcom_uart=1.
 */
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/qcom_geni_uart.h>
#include <sys/systm.h>
#include "hakios_board.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define GENI_MAP_SIZE               0x4000u
#define SE_GENI_STATUS              0x0040u
#define SE_UART_TX_TRANS_LEN        0x0270u
#define SE_GENI_M_CMD0              0x0600u
#define SE_GENI_M_IRQ_STATUS        0x0610u
#define SE_GENI_M_IRQ_CLEAR         0x0618u
#define SE_GENI_TX_FIFOn            0x0700u
#define SE_GENI_RX_FIFOn            0x0780u
#define SE_GENI_TX_FIFO_STATUS      0x0800u
#define SE_GENI_RX_FIFO_STATUS      0x0804u

#define M_GENI_CMD_ACTIVE           (1u << 0)
#define M_CMD_DONE_EN               (1u << 0)
#define UART_START_TX               (1u << 27)
#define RX_FIFO_WC_MSK              0x01ffffffu

#define GENI_POLL_LIMIT             100000u

static vm_offset_t geni_base;
static bool geni_initialized;

#ifdef HAKIOS_MMIO_TEST
extern uint32_t hakios_test_rd(vm_offset_t address);
extern void hakios_test_wr(vm_offset_t address, uint32_t value);
#define geni_rd(at)        hakios_test_rd(geni_base + (at))
#define geni_wr(at, value) hakios_test_wr(geni_base + (at), (value))
#else
static inline uint32_t
geni_rd(uint32_t at)
{
	return *(volatile uint32_t *)(geni_base + at);
}

static inline void
geni_wr(uint32_t at, uint32_t value)
{
	*(volatile uint32_t *)(geni_base + at) = value;
}
#endif

static bool
geni_wait_idle(void)
{
	for (uint32_t spin = 0; spin < GENI_POLL_LIMIT; spin++) {
		if ((geni_rd(SE_GENI_STATUS) & M_GENI_CMD_ACTIVE) == 0) {
			return true;
		}
	}
	return false;
}

static bool
geni_wait_tx_done(void)
{
	for (uint32_t spin = 0; spin < GENI_POLL_LIMIT; spin++) {
		if (geni_rd(SE_GENI_M_IRQ_STATUS) & M_CMD_DONE_EN) {
			geni_wr(SE_GENI_M_IRQ_CLEAR, M_CMD_DONE_EN);
			return true;
		}
	}
	return false;
}

bool
qcom_geni_uart_ready(void)
{
	return geni_initialized;
}

int
qcom_geni_uart_putc(char c)
{
	if (!geni_initialized) {
		return -ENODEV;
	}
	if (!geni_wait_idle()) {
		return -ETIMEDOUT;
	}

	geni_wr(SE_UART_TX_TRANS_LEN, 1u);
	geni_wr(SE_GENI_M_CMD0, UART_START_TX);
	geni_wr(SE_GENI_TX_FIFOn, (uint32_t)(uint8_t)c);

	if (!geni_wait_tx_done()) {
		return -ETIMEDOUT;
	}
	return 0;
}

int
qcom_geni_uart_puts(const char *s)
{
	if (!geni_initialized || s == NULL) {
		return -EINVAL;
	}
	while (*s != '\0') {
		if (*s == '\n') {
			int rc = qcom_geni_uart_putc('\r');
			if (rc != 0) {
				return rc;
			}
		}
		int rc = qcom_geni_uart_putc(*s++);
		if (rc != 0) {
			return rc;
		}
	}
	return 0;
}

int
qcom_geni_uart_getc(void)
{
	if (!geni_initialized) {
		return -ENODEV;
	}
	uint32_t wc = geni_rd(SE_GENI_RX_FIFO_STATUS) & RX_FIFO_WC_MSK;
	if (wc == 0) {
		return -EAGAIN;
	}
	return (int)(geni_rd(SE_GENI_RX_FIFOn) & 0xffu);
}

void
qcom_geni_uart_init(void)
{
	uint32_t arg = 0;

	if (geni_initialized) {
		return;
	}
	if (!PE_parse_boot_argn("qcom_uart", &arg, sizeof(arg)) || arg == 0) { /* qcom_uart=1: enable Qualcomm QUPv3 GENI debug UART */
		return;
	}
	if (!hakios_board_wants(HAKIOS_HAS_GENI_UART, "qcom_uart=1")) {
		return;
	}

	vm_offset_t phys = (vm_offset_t)hakios_board()->platform.uart0;
	if (phys == 0) {
		printf("qcom_uart: board has no uart0 address\n");
		return;
	}

	/* Guard against NOC hang if QUPv3 SE4 UART clock (GCC_QUPV3_WRAP0_S4_CBCR @ 0x014174b0) is gated by ABL */
	vm_offset_t gcc_se4 = ml_io_map(0x01417000ULL, 0x1000u);
	if (gcc_se4 == 0 || (*(volatile uint32_t *)(gcc_se4 + 0x4b0u) & (1u << 31)) != 0) {
		printf("qcom_uart: QUPv3 SE4 UART clock is gated by ABL; skipping UART init\n");
		return;
	}

	geni_base = ml_io_map(phys, GENI_MAP_SIZE);
	if (geni_base == 0) {
		printf("qcom_uart: ml_io_map(0x%llx) failed\n", (unsigned long long)phys);
		return;
	}

	geni_initialized = true;
	printf("qcom_uart: QUPv3 GENI UART at 0x%llx ready\n", (unsigned long long)phys);
}
