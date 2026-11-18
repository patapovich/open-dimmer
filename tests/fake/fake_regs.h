/* PC stand-in for the PY32 registers and LL calls that src/phase.c uses, so the
 * real phase.c runs in tests/test_phase.c. Plain memory, no hardware model,
 * except that every write to a timer's EGR is seen at the moment it happens:
 * TIMx->EGR expands to a fresh slot index from fake_egr_slot(), which runs
 * before the store and takes a snapshot of TIM1 and TIM16 (what an update
 * event copies from the preload registers). */
#ifndef FAKE_REGS_H
#define FAKE_REGS_H

#include <stdint.h>

#define FAKE_EGR_SLOTS 16u

typedef struct {
    volatile uint32_t CR1, DIER, SR, CCMR2, CCER, CNT, PSC, ARR, CCR1, CCR3, CCR4, BDTR;
    volatile uint32_t egr[FAKE_EGR_SLOTS];    /* EGR writes, one slot each */
} TIM_TypeDef;

typedef struct {
    volatile uint32_t IMR, RTSR, FTSR, PR;
} EXTI_TypeDef;

typedef struct {
    volatile uint32_t IDR, ODR;
    uint8_t           mode[16], pull[16], af[16];
} GPIO_TypeDef;

extern TIM_TypeDef  fake_tim1, fake_tim16;
extern EXTI_TypeDef fake_exti;
extern GPIO_TypeDef fake_gpioa, fake_gpiob;

/* Snapshot of a timer at an EGR write (before the store). */
typedef struct {
    uint32_t arr, ccr3, ccr4, cnt;
} fake_tim_snap_t;

typedef struct {
    fake_tim_snap_t tim1, tim16;
} fake_egr_t;

extern fake_egr_t fake_egr_log[FAKE_EGR_SLOTS];
extern unsigned   fake_egr_writes;          /* since start; slot = count % FAKE_EGR_SLOTS */
unsigned          fake_egr_slot(void);

#define TIM1  (&fake_tim1)
#define TIM16 (&fake_tim16)
#define EXTI  (&fake_exti)
#define GPIOA (&fake_gpioa)
#define GPIOB (&fake_gpiob)
#define EGR   egr[fake_egr_slot()]

#define TIM_EGR_UG          (1u << 0)
#define TIM_SR_CC1IF        (1u << 1)
#define TIM_DIER_CC1IE      (1u << 1)
#define TIM_CR1_CEN         (1u << 0)
#define TIM_CR1_ARPE        (1u << 7)
#define TIM_CCMR2_OC3M_Pos  4
#define TIM_CCMR2_OC4M_Pos  12
#define TIM_CCMR2_OC3PE     (1u << 3)
#define TIM_CCMR2_OC4PE     (1u << 11)
#define TIM_CCER_CC3E       (1u << 8)
#define TIM_CCER_CC4E       (1u << 12)
#define TIM_BDTR_MOE        (1u << 15)

/* NVIC and PRIMASK */
typedef enum { EXTI4_15_IRQn = 7, TIM16_IRQn = 21, FAKE_IRQ_COUNT = 32 } IRQn_Type;
extern uint8_t  fake_nvic_prio[FAKE_IRQ_COUNT];
extern uint8_t  fake_nvic_enabled[FAKE_IRQ_COUNT];
extern uint8_t  fake_irq_off;               /* PRIMASK */
static inline void NVIC_SetPriority(IRQn_Type n, uint32_t p) { fake_nvic_prio[n] = (uint8_t)p; }
static inline void NVIC_EnableIRQ(IRQn_Type n) { fake_nvic_enabled[n] = 1; }
static inline void NVIC_DisableIRQ(IRQn_Type n) { fake_nvic_enabled[n] = 0; }
static inline void __disable_irq(void) { fake_irq_off = 1; }
static inline void __enable_irq(void) { fake_irq_off = 0; }

/* GPIO (LL_GPIO_PIN_n is the pin's bit, as in the SDK) */
#define LL_GPIO_PIN_0          (1u << 0)
#define LL_GPIO_PIN_1          (1u << 1)
#define LL_GPIO_PIN_2          (1u << 2)
#define LL_GPIO_PIN_3          (1u << 3)
#define LL_GPIO_PIN_4          (1u << 4)
#define LL_GPIO_PIN_5          (1u << 5)
#define LL_GPIO_PIN_6          (1u << 6)
#define LL_GPIO_MODE_INPUT     0u
#define LL_GPIO_MODE_OUTPUT    1u
#define LL_GPIO_MODE_ALTERNATE 2u
#define LL_GPIO_PULL_NO        0u
#define LL_GPIO_PULL_UP        1u
#define LL_GPIO_AF_13          13u

static inline unsigned fake_pin(uint32_t mask) {
    unsigned i = 0;
    while (mask > 1u) {
        mask >>= 1;
        i++;
    }
    return i;
}
static inline uint32_t LL_GPIO_IsInputPinSet(GPIO_TypeDef *g, uint32_t m) { return (g->IDR & m) == m; }
static inline void LL_GPIO_SetOutputPin(GPIO_TypeDef *g, uint32_t m) { g->ODR |= m; }
static inline void LL_GPIO_ResetOutputPin(GPIO_TypeDef *g, uint32_t m) { g->ODR &= ~m; }
static inline void LL_GPIO_SetPinMode(GPIO_TypeDef *g, uint32_t m, uint32_t mode) { g->mode[fake_pin(m)] = (uint8_t)mode; }
static inline void LL_GPIO_SetPinPull(GPIO_TypeDef *g, uint32_t m, uint32_t p) { g->pull[fake_pin(m)] = (uint8_t)p; }
static inline void LL_GPIO_SetAFPin_0_7(GPIO_TypeDef *g, uint32_t m, uint32_t af) { g->af[fake_pin(m)] = (uint8_t)af; }

/* Clocks and the EXTI line source */
#define LL_APB1_GRP2_PERIPH_TIM1   (1u << 11)
#define LL_APB1_GRP2_PERIPH_TIM16  (1u << 17)
#define LL_EXTI_CONFIG_PORTA       0u
#define LL_EXTI_CONFIG_LINE4       4u
extern uint32_t fake_apb1_grp2_clocks;
extern int      fake_exti4_port;            /* -1 = not set */
static inline void LL_APB1_GRP2_EnableClock(uint32_t m) { fake_apb1_grp2_clocks |= m; }
static inline void LL_EXTI_SetEXTISource(uint32_t port, uint32_t line) {
    if (line == LL_EXTI_CONFIG_LINE4) {
        fake_exti4_port = (int)port;
    }
}

#endif
