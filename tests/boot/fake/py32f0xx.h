/* Host stand-in for the PY32 device header, for the bootloader harness (tests/boot/harness.c):
 * boot/boot.c is compiled unmodified against it.
 *
 * Every peripheral macro is a call into the harness (host_*()) that returns a plain struct. The
 * call lets the harness's models catch up with what the code did since the previous access (a
 * key written to IWDG->KR, a BRR or BSRR write, a write to RCC->CSR, ...) and charge the model
 * time of the access. Reads cannot be seen that way, and one read has an effect: reading
 * USART1->DR takes the received byte and clears RXNE and ORE. So DR and SR are array members
 * indexed through a hook (host_usart_dr(), host_sr()): the harness sees every such access, and
 * tells a DR read from a DR write by a marker bit (bit 8, outside the 8 data bits) that it puts
 * into DR before the access. USART1 itself is a plain struct, so a DR access holds exactly one
 * hook call: two calls in one expression would run in an unspecified order. host_sr() serves
 * IWDG->SR too (it sets both status registers; their loops never run at the same time).
 *
 * Bit values are the SDK's (py32f002ax5.h, core_cm0plus.h), so the harness checks the values
 * the chip would get. */
#ifndef HOST_BOOT_PY32F0XX_H
#define HOST_BOOT_PY32F0XX_H

#include <stdint.h>

typedef struct { volatile uint32_t CR, ICSCR, APBRSTR2, IOPENR, APBENR2, CSR; } RCC_TypeDef;
typedef struct { volatile uint32_t MODER, PUPDR, IDR, AFR[2], BSRR, BRR; } GPIO_TypeDef;
typedef struct { volatile uint32_t SR_[1], DR_[1], BRR, CR1; } USART_TypeDef;
typedef struct { volatile uint32_t CTRL, LOAD, VAL; } SysTick_Type;
typedef struct { volatile uint32_t KR, PR, RLR, SR_[1]; } IWDG_TypeDef;
typedef struct { volatile uint32_t VTOR; } SCB_Type;

extern USART_TypeDef host_usart;
RCC_TypeDef   *host_rcc(void);
GPIO_TypeDef  *host_gpioa(void);
uint32_t       host_sr(void);
uint32_t       host_usart_dr(void);
SysTick_Type  *host_systick(void);
IWDG_TypeDef  *host_iwdg(void);
SCB_Type      *host_scb(void);
void           host_irq(int enabled);
void           host_set_msp(uint32_t sp) __attribute__((noreturn));   /* the jump into the app */
void           host_system_reset(void) __attribute__((noreturn));

#define RCC     (host_rcc())
#define GPIOA   (host_gpioa())
#define USART1  (&host_usart)
#define SR      SR_[host_sr()]
#define DR      DR_[host_usart_dr()]
#define SysTick (host_systick())
#define IWDG    (host_iwdg())
#define SCB     (host_scb())

#define __disable_irq()     host_irq(0)
#define __enable_irq()      host_irq(1)
#define __set_MSP(sp)       host_set_msp(sp)
#define NVIC_SystemReset()  host_system_reset()

#define RCC_IOPENR_GPIOAEN          (1u << 0)
#define RCC_ICSCR_HSI_TRIM          0x1FFFu
#define RCC_ICSCR_HSI_FS_Pos        13u
#define RCC_ICSCR_HSI_FS            (7u << RCC_ICSCR_HSI_FS_Pos)
#define RCC_CR_HSIRDY               (1u << 10)
#define RCC_APBENR2_USART1EN        (1u << 14)
#define RCC_APBRSTR2_USART1RST      (1u << 14)
#define RCC_CSR_LSION               (1u << 0)
#define RCC_CSR_LSIRDY              (1u << 1)
#define RCC_CSR_RMVF                (1u << 23)
#define RCC_CSR_OBLRSTF             (1u << 25)
#define RCC_CSR_PINRSTF             (1u << 26)
#define RCC_CSR_PWRRSTF             (1u << 27)
#define RCC_CSR_SFTRSTF             (1u << 28)
#define RCC_CSR_IWDGRSTF            (1u << 29)

#define SysTick_CTRL_ENABLE_Msk     (1u << 0)
#define SysTick_CTRL_CLKSOURCE_Msk  (1u << 2)
#define SysTick_CTRL_COUNTFLAG_Msk  (1u << 16)

#define USART_SR_FE                 (1u << 1)
#define USART_SR_NE                 (1u << 2)
#define USART_SR_ORE                (1u << 3)
#define USART_SR_RXNE               (1u << 5)
#define USART_SR_TC                 (1u << 6)
#define USART_SR_TXE                (1u << 7)
#define USART_CR1_RE                (1u << 2)
#define USART_CR1_TE                (1u << 3)
#define USART_CR1_UE                (1u << 13)

#endif
