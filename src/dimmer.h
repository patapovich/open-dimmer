/* Dimmer channels and their Tuya datapoints (standard Tuya dimmer layout, the
 * one the Zigbee module's firmware expects):
 *   channel 1: DP 1 on, 2 level, 3 min, 4 switch type, 5 max, 6 countdown
 *   channel 2: DP 7 .. 12, same order
 *   DP 14 power-on behaviour (0 off, 1 on, 2 last state), both channels
 *     (written: both; reported: channel 1's, for the stock protocol). After a
 *     power cut only: a watchdog, fault or update restart brings the light
 *     back as it was (once in a row, see dimmer_init)
 *   DP 103 / 104 power-on behaviour of channel 1 / 2 (ours)
 *   DP 150..154 / 155..159 fade and hold of channel 1 / 2 (values, ours, saved):
 *     fade on ms (dark -> full), fade off ms (full -> dark), soft end % (going
 *     dark, the lowest % above the min slow down), hold ms (whole range), hold
 *     end % (the hold slows down there, to pick a very dim level)
 *   DP 160 / 161 transition time of channel 1 / 2 (value, ms, ours, not saved,
 *     never in the DP dump): the network on / off / level changes written in
 *     the next second each take that long, at one even rate (0: the fade
 *     settings). The module writes it right before them for a Zigbee transition
 *     time; a wall switch or the button always takes the fade settings
 *   DP 162 / 163 wall switch mode of channel 1 / 2 (value, ours, saved): 0 the
 *     switch drives the light, 1 detached (the light is the network's; the
 *     gestures are still reported, a toggle or state switch's change as a
 *     single), 2 off (ignored)
 *   DP 164 / 165 level after a power cut (value, 0 = the last level; kept in
 *     steps of 4; ours, saved): the light takes it at every cold start, on or
 *     off by the power-on behaviour
 *   DP 166 / 167 identify (value, s, at most 255; ours, not saved): the
 *     channel blinks (dark / full, 0.5 s) that long, then is as it was; 0 stops
 *     (a network transition still running when it starts is not resumed: the
 *     channel goes to its state by its fade)
 *   DP 168 / 169 start boost (value, ms in steps of 50 up to 750, 0 = none;
 *     ours, saved): coming on from dark the output is at full that long, then
 *     goes to the level (an LED driver that needs a kick); also before a hold
 *     from dark; a transition time runs on during it; none after a reset that
 *     kept the light (update, watchdog)
 *   DP 170 / 171 fast press (value 0 / 1, ours, saved): in the detached mode a
 *     momentary switch's press is reported at once as gesture 6, for direct
 *     control without the multi-tap wait; hold and release as before, no
 *     single / double / triple on that channel
 *   DP 6 / 12 countdown (s, up to 86400): auto-off, written while on: the
 *     channel goes off after that long (reported then with 0); an off cancels
 *     it (a local off reports the 0), written while off it stays 0. Not saved,
 *     and RAM only: a reset (update, watchdog) cancels it (the ZTU, which sees
 *     the MCU restart, takes it as 0)
 *   DP 105 / 106 wall switch gesture of channel 1 / 2, reported only, never
 *     in the DP dump (ours, gesture_core.h): 1 single, 2 double, 3 triple,
 *     4 hold, 5 release, 6 press (fast press, DP 170 / 171, detached only)
 *   DP 101 / 102 dimming curve of channel 1 / 2 (enum, curves.h; ours, the
 *   stock MCU has no such DP): 0 linear, 1 led, 2 led_soft, 3 incandescent,
 *   4 led_deep, 5 led_deeper
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
    uint32_t countdown;  /* seconds left at the write; 0: none (dimmer.c g_cd_end runs it) */
    /* How the lamp fades and holds (DPs 150.., dim_profile_set). */
    uint16_t fade_on_ms;
    uint16_t fade_off_ms;
    uint8_t  soft_end_pct;
    uint16_t hold_ms;
    uint8_t  hold_end_pct;
    /* Ours, saved in the record's spare bytes (0 = as before). */
    uint8_t  input_mode;     /* DP 162 / 163: 0 the wall switch drives the light, 1 detached (only
                                its gestures are reported), 2 off (ignored) */
    uint8_t  power_level4;   /* DP 164 / 165: the level after a power cut, / 4 (0: the last one) */
    uint8_t  boost50;        /* DP 168 / 169: start boost, 50 ms units (0..15, 0: none) */
    uint8_t  fast_press;     /* DP 170 / 171: detached momentary: a press reported at once (6) */
} dimmer_channel_t;

#define DIMMER_INPUT_NORMAL    0
#define DIMMER_INPUT_DETACHED  1
#define DIMMER_INPUT_OFF       2

/* Reset flags of RCC_CSR (main.c checks them against the device header). */
#define DIMMER_RESET_POWER  (1u << 27)   /* PWRRSTF: power-up or brown-out */
#define DIMMER_RESET_SOFT   (1u << 28)   /* SFTRSTF: software (update restart, fault) */
#define DIMMER_RESET_WDG    (1u << 29)   /* IWDGRSTF: watchdog */
/** A start that takes the power-on behaviour: a power-up or brown-out (the RAM is random), also
 * when the bootloader restarted after it, or a reset that was neither a software nor a watchdog
 * reset (pin, option bytes). Every other start restores the light (dimmer_init). */
#define DIMMER_COLD_START(csr) \
    (((csr) & DIMMER_RESET_POWER) || !((csr) & (DIMMER_RESET_SOFT | DIMMER_RESET_WDG)))

/** Defaults, then saved settings and the power-on behaviour. reset_csr: RCC_CSR
 * at start. After a watchdog or software reset that was no power-up or
 * brown-out (the RAM is kept) the light comes back as it was, from the mailbox
 * (ota_proto.h), once in a row. */
uint8_t dimmer_init(uint32_t reset_csr);   /* 1: a cold start (DIMMER_COLD_START) */
/** Save changed settings once writes have settled, the UART is quiet and a
 * ~5 ms CPU stall would not miss a zero cross (stall_ok). After a minute of
 * uptime the next warm reset may restore again. */
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
/** Module button: any channel on -> all off, else all on. */
void dimmer_local_toggle_all(void);
/** Report a wall switch gesture (GESTURE_*) of a channel to the module. */
void dimmer_report_action(uint8_t ch, uint8_t gesture);
/** Start hold dimming; dir 0 = pick (up from dark or at min, down at max,
 * else the opposite of the last hold; the first hold goes towards the far end). */
void dimmer_hold_begin(uint8_t ch, int8_t dir, uint8_t from_dark);
/** The hold time of a press from dark is over (its rise runs since the press): report its
 *  level on the way from now on, as a hold's (dimmer_hold_begin starts them by itself). */
void dimmer_hold_reports(uint8_t ch);
/** End hold dimming: keep the level the light has (saved), and report it.
 * Nothing if no hold is running. */
void dimmer_hold_end(uint8_t ch);
/** A press from dark started rising from the minimum but was a tap: on to the
 * level the channel has (not stored, nothing new to report). */
void dimmer_hold_cancel(uint8_t ch);

/** Before a restart into the bootloader: save pending settings and leave the
 *  light state in the mailbox, to be restored after the update (a hold in
 *  progress: at the level the light has). */
void dimmer_prepare_restart(void);
/** DPs to report with a written one (n = 0, 1, ...; 0 = no more): min <-> max of
 * the same channel and its level (a write of one may move the others); DP 14 ->
 * 103, 104. */
uint8_t dimmer_sibling_dp(uint8_t dpid, uint8_t n);

/** 1 when every channel is off and dark: no fade still running, no hold (the link watchdog's
 * condition, docs/BOOTLOADER.md section 9, item 11). */
uint8_t dimmer_dark(void);

/** Report every DP (answer to query-all). */
void dimmer_report_all(void);

#endif
