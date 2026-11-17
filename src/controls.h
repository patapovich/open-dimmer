/* Wall switch inputs S1 (PB1) -> channel 1, S2 (PB2) -> channel 2, and the
 * module button (PA6). Sampled every 1 ms from SysTick; actions run in the
 * main loop.
 *
 * Switch type (DP 4 / 10, as in the stock firmware):
 *   0 toggle     every change of the switch toggles the light
 *   1 state      closed = on, open = off
 *   2 momentary  push button: press while off turns on at once; short press
 *                while on turns off on release; hold dims (light follows the
 *                finger, stops the moment it is released)
 * Module button: short press toggles channel 1; hold >= 3 s asks the Zigbee
 * module to leave and pair again (it decides, as with the stock MCU). */
#ifndef CONTROLS_H
#define CONTROLS_H

#include <stdint.h>

/* Debug over SWD: bit 7 set = ignore the pins and use bits 0..2 instead
 * (1 = closed): bit 0 S1, bit 1 S2, bit 2 button. */
extern volatile uint8_t g_input_override;

void controls_init(void);
/** From the 1 ms tick interrupt. */
void controls_tick_1ms(void);
/** From the main loop. */
void controls_poll(uint32_t now);

#endif
