/* Host test of the receive interrupt in src/uart.c against a model of the
 * USART receiver: a byte that comes while DR is still full sets ORE and is
 * lost; reading SR and then DR clears RXNE and ORE (so does a DR read after
 * any SR read, as on the chip). Run: make test */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "py32f0xx_ll_usart.h"
#include "uart.h"

void USART1_IRQHandler(void);

static int g_failures;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            g_failures++;                                                \
        }                                                                \
    } while (0)

/* ---- receiver model ----------------------------------------------------------- */
static uint32_t g_sr;
static uint8_t  g_dr;
static uint8_t  g_sr_read;      /* SR read since the last DR read */
static unsigned g_sr_reads, g_dr_reads;

uint32_t host_usart_SR(void) {
    g_sr_reads++;
    g_sr_read = 1;
    return g_sr;
}
uint32_t host_usart_DR(void) {
    g_dr_reads++;
    g_sr &= ~USART_SR_RXNE;
    if (g_sr_read) {
        g_sr &= ~USART_SR_ORE;
    }
    g_sr_read = 0;
    return g_dr;
}
void host_usart_tx(uint8_t b) {
    (void)b;
}

/* A byte arrives on the line. */
static void line_byte(uint8_t b) {
    if (g_sr & USART_SR_RXNE) {
        g_sr |= USART_SR_ORE;   /* DR full: this byte is lost */
        return;
    }
    g_dr = b;
    g_sr |= USART_SR_RXNE;
}

static void isr(void) {
    g_sr_reads = g_dr_reads = 0;
    USART1_IRQHandler();
}

int main(void) {
    uart_init();
    CHECK(uart_getc() == -1);

    /* Normal bytes: one SR and one DR read each, nothing lost. */
    for (int i = 0; i < 10; i++) {
        line_byte((uint8_t)(0x30 + i));
        isr();
        CHECK(g_sr_reads == 1 && g_dr_reads == 1);
        CHECK(g_sr == 0);
    }
    for (int i = 0; i < 10; i++) {
        CHECK(uart_getc() == 0x30 + i);
    }
    CHECK(uart_getc() == -1);
    CHECK(uart_dropped() == 0);

    /* A CPU stall (flash save): three bytes come before the interrupt runs.
     * The first is kept, the others are lost: counted, and ORE is cleared. */
    line_byte(0x41);
    line_byte(0x42);
    line_byte(0x43);
    CHECK(g_sr == (USART_SR_RXNE | USART_SR_ORE));
    isr();
    CHECK(g_sr_reads == 1 && g_dr_reads == 1);
    CHECK(g_sr == 0);
    CHECK(uart_dropped() == 1);
    CHECK(uart_getc() == 0x41);
    CHECK(uart_getc() == -1);

    /* The next byte is taken normally. */
    line_byte(0x44);
    isr();
    CHECK(uart_getc() == 0x44);
    CHECK(uart_dropped() == 1);

    /* A full ring: the bytes that do not fit count as lost too. */
    for (int i = 0; i < 130; i++) {
        line_byte((uint8_t)i);
        isr();
    }
    CHECK(uart_dropped() == 1 + 3);    /* ring of 128 holds 127 */
    for (int i = 0; i < 127; i++) {
        CHECK(uart_getc() == i);
    }
    CHECK(uart_getc() == -1);

    if (g_failures) {
        printf("test_uart: %d FAILED\n", g_failures);
        return 1;
    }
    printf("test_uart: all passed\n");
    return 0;
}
