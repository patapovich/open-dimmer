/* USART1 to the Zigbee module: interrupt-driven receive into a ring buffer,
 * blocking transmit. */
#ifndef UART_H
#define UART_H

#include <stdint.h>

/** USART1 at UART_BAUD 8N1 from SYSCLK_HZ (board.h), receive interrupt on. */
void uart_init(void);
/** Next received byte, or -1 if none. */
int  uart_getc(void);
void uart_putc(uint8_t b);
/** Wait until the last byte has left the shift register. */
void uart_flush(void);
/** Receive losses since start (diagnostics): overruns (at least one byte
 * each, the CPU was stalled) and bytes that found the ring buffer full. */
uint32_t uart_dropped(void);

#endif
