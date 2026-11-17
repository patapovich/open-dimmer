#include "tuya.h"
#include "board.h"
#include "py32f0xx_ll_bus.h"
#include "py32f0xx_ll_usart.h"

static uint16_t g_seq;

void tuya_init(void) {
    LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA);
    LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_USART1);

    LL_USART_SetBaudRate(USART1, SystemCoreClock, LL_USART_OVERSAMPLING_16, UART_BAUD);
    LL_USART_SetDataWidth(USART1, LL_USART_DATAWIDTH_8B);
    LL_USART_SetStopBitsLength(USART1, LL_USART_STOPBITS_1);
    LL_USART_SetParity(USART1, LL_USART_PARITY_NONE);
    LL_USART_SetHWFlowCtrl(USART1, LL_USART_HWCONTROL_NONE);
    LL_USART_SetTransferDirection(USART1, LL_USART_DIRECTION_TX_RX);
    LL_USART_Enable(USART1);
    LL_USART_ClearFlag_TC(USART1);

    LL_GPIO_SetPinMode(UART_TX_PORT, UART_TX_PIN, LL_GPIO_MODE_ALTERNATE);
    LL_GPIO_SetPinSpeed(UART_TX_PORT, UART_TX_PIN, LL_GPIO_SPEED_FREQ_HIGH);
    LL_GPIO_SetPinPull(UART_TX_PORT, UART_TX_PIN, LL_GPIO_PULL_UP);
    LL_GPIO_SetAFPin_0_7(UART_TX_PORT, UART_TX_PIN, LL_GPIO_AF_1);

    LL_GPIO_SetPinMode(UART_RX_PORT, UART_RX_PIN, LL_GPIO_MODE_ALTERNATE);
    LL_GPIO_SetPinSpeed(UART_RX_PORT, UART_RX_PIN, LL_GPIO_SPEED_FREQ_HIGH);
    LL_GPIO_SetPinPull(UART_RX_PORT, UART_RX_PIN, LL_GPIO_PULL_UP);
    LL_GPIO_SetAFPin_0_7(UART_RX_PORT, UART_RX_PIN, LL_GPIO_AF_1);
}

static void uart_put(uint8_t b) {
    while (!LL_USART_IsActiveFlag_TXE(USART1)) {
    }
    LL_USART_TransmitData8(USART1, b);
}

void tuya_send_frame(uint8_t cmd, uint16_t seq, const uint8_t *data, uint16_t len) {
    uint8_t head[8] = { 0x55, 0xAA, 0x02, (uint8_t)(seq >> 8), (uint8_t)seq, cmd,
                        (uint8_t)(len >> 8), (uint8_t)len };
    uint8_t sum = 0;

    for (uint8_t i = 0; i < sizeof(head); i++) {
        sum += head[i];
        uart_put(head[i]);
    }
    for (uint16_t i = 0; i < len; i++) {
        sum += data[i];
        uart_put(data[i]);
    }
    uart_put(sum);
    while (!LL_USART_IsActiveFlag_TC(USART1)) {
    }
}

void tuya_report_bool(uint8_t dpid, uint8_t value) {
    uint8_t dp[5] = { dpid, TUYA_DP_TYPE_BOOL, 0x00, 0x01, value ? 1 : 0 };

    tuya_send_frame(TUYA_CMD_REPORT, g_seq++, dp, sizeof(dp));
}
