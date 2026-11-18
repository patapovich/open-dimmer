/* Wall switch inputs S1 (PB1) -> channel 1, S2 (PB2) -> channel 2, and the
 * module button (PA6). Sampled every 1 ms from SysTick; actions run in the
 * main loop.
 *
 * Switch type (DP 4 / 10, as in the stock firmware):
 *   0 toggle     every change of the switch toggles the light
 *   1 state      closed = on, open = off
 *   2 momentary  push button: press while off turns on at once; short press
 *                while on turns off on release; hold dims (light follows the
 *                finger, stops the moment it is released, or by itself 2 x
 *                hold ms + 2 s after the press began: a stuck input)
 * Module button: short press switches both channels (any on -> all off, else
 * all on); hold >= 3 s asks the Zigbee module to leave and pair again (it
 * decides, as with the stock MCU).
 * An input closed at power-up rests there: its release is a change of a
 * toggle or state switch, else nothing (no gesture, button action or rejoin).
 * A module-button hold of 3 s or more that began in the first 10 s after a cold
 * start is no rejoin either: it may be a bootloader rescue press that came after
 * the bootloader looked (docs/BOOTLOADER.md section 4), and the module must not
 * leave its network for it. */
#ifndef CONTROLS_H
#define CONTROLS_H

#include <stdint.h>

/* Debug over SWD: bit 7 set = ignore the pins and use bits 0..2 instead
 * (1 = closed): bit 0 S1, bit 1 S2, bit 2 button. */
extern volatile uint8_t g_input_override;

/** cold_start: this start takes the power-on behaviour (DIMMER_COLD_START). */
void controls_init(uint8_t cold_start);
/** From the 1 ms tick interrupt. */
void controls_tick_1ms(void);
/** From the main loop. */
void controls_poll(uint32_t now);
/** 1 when no input is pressed (closed, debounced) and no input event waits: a restart now
 * would lose no press or switch change (the link watchdog's condition). A state switch that
 * rests closed, or a toggle switch that does, keeps it 0: the first would light its channel
 * at the restart. */
uint8_t controls_idle(void);

#endif
