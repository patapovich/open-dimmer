/* USART1 to the Zigbee module: interrupt-driven receive into a ring buffer,
 * blocking transmit. */
#ifndef UART_H
#define UART_H

#include <stdint.h>

void uart_init(uint32_t baud);
/** Next received byte, or -1 if none. */
int  uart_getc(void);
void uart_putc(uint8_t b);
/** Wait until the last byte has left the shift register. */
void uart_flush(void);
/** Bytes lost because the ring buffer was full (diagnostics). */
uint16_t uart_dropped(void);

#endif
