/* Dimmer channels and their Tuya datapoints (standard Tuya dimmer layout, the
 * one the Zigbee module's firmware expects):
 *   channel 1: DP 1 on, 2 level, 3 min, 4 switch type, 5 max, 6 countdown
 *   channel 2: DP 7 .. 12, same order
 *   DP 14 power-on behaviour (0 off, 1 on, 2 last state), both channels
 *   DP 101 / 102 dimming curve of channel 1 / 2 (enum, curves.h; ours, the
 *   stock MCU has no such DP): 0 linear, 1 led, 2 led_soft, 3 incandescent
 * Levels are 10..1000 like the stock firmware. */
#ifndef DIMMER_H
#define DIMMER_H

#include <stdint.h>

#define DIMMER_CHANNELS     2
#define DIMMER_LEVEL_MIN    10
#define DIMMER_LEVEL_MAX    1000

typedef struct {
    uint8_t  on;
    uint16_t level;      /* target level while on */
    uint16_t min;
    uint16_t max;
    uint8_t  switch_type;
    uint8_t  curve;
    uint32_t countdown;  /* seconds, stored and reported only for now */
} dimmer_channel_t;

/** Defaults, then saved settings and the power-on behaviour. */
void dimmer_init(void);
/** Save changed settings once writes have settled, the UART is quiet and a
 * ~5 ms CPU stall would not miss a zero cross (stall_ok). */
void dimmer_poll(uint32_t now, uint32_t rx_quiet_ms, uint8_t stall_ok);
const dimmer_channel_t *dimmer_channel(uint8_t ch);

/** Apply a DP write. Returns 1 if the DP is ours (then it is reported back). */
uint8_t dimmer_write_dp(uint8_t dpid, uint8_t type, const uint8_t *value, uint16_t len);
/** Add one DP's current value to the report being built. */
void dimmer_report_dp(uint8_t dpid);
/* Local actions (wall switch, button): change the channel and tell the module
 * with an active 0x06 report, which it treats as a touch. */
void dimmer_local_set(uint8_t ch, uint8_t on);
/** At boot, before the module is told our state: no report. */
void dimmer_boot_set(uint8_t ch, uint8_t on);
void dimmer_local_toggle(uint8_t ch);
/** Start hold dimming; dir 0 = pick (up from dark or at min, down at max,
 * else the opposite of the last hold). */
void dimmer_hold_begin(uint8_t ch, int8_t dir, uint8_t from_dark);
/** End hold dimming: keep the level the light has and report it. */
void dimmer_hold_end(uint8_t ch);

/** Before a restart into the bootloader: save pending settings and leave the
 *  light state in the mailbox, to be restored after the update. */
void dimmer_prepare_restart(void);
/** min <-> max of the same channel (a write of one may move the other), or 0. */
uint8_t dimmer_sibling_dp(uint8_t dpid);

/** Report every DP (answer to query-all). */
void dimmer_report_all(void);

#endif
