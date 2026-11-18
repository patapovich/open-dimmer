#include "uart.h"
#include "board.h"
#include "py32f0xx_ll_bus.h"
#include "py32f0xx_ll_usart.h"

#define RX_SIZE    128   /* power of two */

static volatile uint8_t  g_rx[RX_SIZE];
static volatile uint8_t  g_head;   /* written by the ISR */
static volatile uint8_t  g_tail;   /* read by the main loop */
static volatile uint32_t g_dropped;

/* One read of SR, then one of DR: that pair clears both RXNE and ORE, so ORE
 * must be taken from the same SR read (a check after the data read never
 * sees it). An overrun (a byte came while DR was still full, as during a
 * flash stall) lost at least one byte and counts once. */
void USART1_IRQHandler(void) {
    uint32_t sr = LL_USART_ReadReg(USART1, SR);
    uint8_t  b  = LL_USART_ReceiveData8(USART1);

    if (sr & USART_SR_ORE) {
        g_dropped++;
    }
    if (sr & USART_SR_RXNE) {
        uint8_t next = (uint8_t)((g_head + 1) & (RX_SIZE - 1));
        if (next != g_tail) {
            g_rx[g_head] = b;
            g_head       = next;
        } else {
            g_dropped++;
        }
    }
}

void uart_init(uint32_t baud) {
    LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA);
    LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_USART1);

    LL_GPIO_SetPinMode(UART_TX_PORT, UART_TX_PIN, LL_GPIO_MODE_ALTERNATE);
    LL_GPIO_SetPinSpeed(UART_TX_PORT, UART_TX_PIN, LL_GPIO_SPEED_FREQ_HIGH);
    LL_GPIO_SetPinPull(UART_TX_PORT, UART_TX_PIN, LL_GPIO_PULL_UP);
    LL_GPIO_SetAFPin_0_7(UART_TX_PORT, UART_TX_PIN, LL_GPIO_AF_1);

    LL_GPIO_SetPinMode(UART_RX_PORT, UART_RX_PIN, LL_GPIO_MODE_ALTERNATE);
    LL_GPIO_SetPinSpeed(UART_RX_PORT, UART_RX_PIN, LL_GPIO_SPEED_FREQ_HIGH);
    LL_GPIO_SetPinPull(UART_RX_PORT, UART_RX_PIN, LL_GPIO_PULL_UP);
    LL_GPIO_SetAFPin_0_7(UART_RX_PORT, UART_RX_PIN, LL_GPIO_AF_1);

    LL_USART_SetBaudRate(USART1, SystemCoreClock, LL_USART_OVERSAMPLING_16, baud);
    LL_USART_SetDataWidth(USART1, LL_USART_DATAWIDTH_8B);
    LL_USART_SetStopBitsLength(USART1, LL_USART_STOPBITS_1);
    LL_USART_SetParity(USART1, LL_USART_PARITY_NONE);
    LL_USART_SetHWFlowCtrl(USART1, LL_USART_HWCONTROL_NONE);
    LL_USART_SetTransferDirection(USART1, LL_USART_DIRECTION_TX_RX);
    LL_USART_Enable(USART1);
    LL_USART_ClearFlag_TC(USART1);

    LL_USART_EnableIT_RXNE(USART1);
    NVIC_SetPriority(USART1_IRQn, 1);
    NVIC_EnableIRQ(USART1_IRQn);
}

int uart_getc(void) {
    if (g_tail == g_head) {
        return -1;
    }
    uint8_t b = g_rx[g_tail];
    g_tail    = (uint8_t)((g_tail + 1) & (RX_SIZE - 1));
    return b;
}

void uart_putc(uint8_t b) {
    while (!LL_USART_IsActiveFlag_TXE(USART1)) {
    }
    LL_USART_TransmitData8(USART1, b);
}

void uart_flush(void) {
    while (!LL_USART_IsActiveFlag_TC(USART1)) {
    }
}

uint32_t uart_dropped(void) {
    return g_dropped;
}
