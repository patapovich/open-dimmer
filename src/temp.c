#include "temp.h"
#include "py32f0xx.h"

#define TS_CAL30   (*(const volatile uint16_t *)0x1FFF0F14u)
#define TS_CAL85   (*(const volatile uint16_t *)0x1FFF0F18u)
#define TS_CHANNEL 11u
/* Control bits that read back as set while their action runs: written as 0 they do nothing,
 * so CR is written whole (py32f0xx_ll_adc.h ADC_CR_BITS_PROPERTY_RS). */

static uint8_t g_started;

void temp_init(void) {
    RCC->APBENR2 |= RCC_APBENR2_ADCEN;
    ADC1->CR = ADC_CR_ADCAL;                          /* the calibration, the ADC still off */
    for (uint32_t n = 0; (ADC1->CR & ADC_CR_ADCAL) && n < 50000u; n++) {
    }
    ADC1->CFGR2  = ADC_CFGR2_CKMODE_1 | ADC_CFGR2_CKMODE_2;   /* PCLK / 64: 375 kHz */
    ADC1->SMPR   = ADC_SMPR_SMP;                      /* 239.5 cycles: the sensor wants long */
    ADC1->CHSELR = 1u << TS_CHANNEL;
    ADC->CCR    |= ADC_CCR_TSEN;
    ADC1->CR     = ADC_CR_ADEN;
}

int16_t temp_take(void) {
    int16_t t = TEMP_NONE;
    if (g_started && (ADC1->ISR & ADC_ISR_EOC)) {
        t = temp_from_raw(ADC1->DR & 0xFFFu, TS_CAL30, TS_CAL85);   /* reading DR clears EOC */
    }
    ADC1->CR  = ADC_CR_ADSTART;                       /* ADEN stays (a 0 there does nothing) */
    g_started = 1;
    return t;
}

uint32_t temp_calibration(void) {
    return (uint32_t)TS_CAL85 << 16 | TS_CAL30;
}
