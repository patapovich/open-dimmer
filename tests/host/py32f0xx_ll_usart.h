/* Host build stand-in for the USART LL driver: SR and DR reads go to the
 * test's model of the receiver (tests/test_uart.c), transmit to its log. */
#ifndef HOST_PY32F0XX_LL_USART_H
#define HOST_PY32F0XX_LL_USART_H

#include <stdint.h>

#define USART1             ((void *)0)
#define USART_SR_ORE       (1u << 3)
#define USART_SR_RXNE      (1u << 5)

uint32_t host_usart_SR(void);   /* reading SR, then DR, clears ORE (as on the chip) */
uint32_t host_usart_DR(void);   /* clears RXNE */
void     host_usart_tx(uint8_t b);

#define LL_USART_ReadReg(inst, reg)          host_usart_##reg()
#define LL_USART_ReceiveData8(inst)          ((uint8_t)host_usart_DR())
#define LL_USART_TransmitData8(inst, b)      host_usart_tx(b)
#define LL_USART_IsActiveFlag_TXE(inst)      1
#define LL_USART_IsActiveFlag_TC(inst)       1
#define LL_USART_SetBaudRate(...)            ((void)0)
#define LL_USART_SetDataWidth(...)           ((void)0)
#define LL_USART_SetStopBitsLength(...)      ((void)0)
#define LL_USART_SetParity(...)              ((void)0)
#define LL_USART_SetHWFlowCtrl(...)          ((void)0)
#define LL_USART_SetTransferDirection(...)   ((void)0)
#define LL_USART_Enable(...)                 ((void)0)
#define LL_USART_ClearFlag_TC(...)           ((void)0)
#define LL_USART_EnableIT_RXNE(...)          ((void)0)
#define NVIC_SetPriority(...)                ((void)0)
#define NVIC_EnableIRQ(...)                  ((void)0)
#define SystemCoreClock                      24000000u

#endif
