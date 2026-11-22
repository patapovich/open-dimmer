/* controls.c with dimmer.c, input_core.c and gesture_core.c on the PC, driven
 * through the pins (board.h host GPIO) every 1 ms like SysTick and the main
 * loop. Each test runs in its own process (a zeroed .bss, as after a reset). */
#include "host_stubs.h"
#include "bootif.h"
#include "controls.h"
#include "dimmer.h"
#include "gesture_core.h"
#include "host_boot.h"
#include "phase.h"
#include "tuya.h"
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CH1_ON     1
#define CH1_LEVEL  2
#define CH1_TYPE   4
#define CH2_ON     7
#define HOLD_MS_1  153

#define TOGGLE     0
#define STATE      1
#define MOMENTARY  2

static uint32_t T;

/* bootif.c's other side (tuya.c, phase.c, the reset): no frame from the module (a dead link), and
 * the restart path recorded. */
uint32_t g_tuya_frames;
uint8_t  g_tuya_confirm;
static unsigned g_shutdowns, g_resets;
void phase_shutdown(void) {
    g_shutdowns++;
}
void NVIC_SystemReset(void) {
    g_resets++;
}

static void pin(GPIO_TypeDef *port, uint32_t mask, uint8_t closed) {
    if (closed) {
        port->IDR &= ~mask;           /* active low */
    } else {
        port->IDR |= mask;
    }
}

static void s1(uint8_t closed) {
    pin(GPIOB, LL_GPIO_PIN_1, closed);
}

static void button(uint8_t closed) {
    pin(GPIOA, LL_GPIO_PIN_6, closed);
}

static void boot(uint32_t csr) {
    host_phase_init();
    T = 0;
    controls_init(dimmer_init(csr));            /* as main.c: the cold-start flag of dimmer_init */
}

/* Power-up with all inputs open (pull-ups) unless a test closed one before. */
static void power_up(uint8_t erase_flash) {
    memset((void *)&g_host_mailbox, 0xA5, sizeof(g_host_mailbox));
    if (erase_flash) {
        g_host_flash_valid = 0;
        g_host_gpioa.IDR   = 0xFFFFu;
        g_host_gpiob.IDR   = 0xFFFFu;
    }
    boot(DIMMER_RESET_POWER);
}

static void run_ms(uint32_t ms) {
    while (ms--) {
        T++;
        controls_tick_1ms();
        if (T % 10 == 0) {
            host_halfcycle();
        }
        controls_poll(T);
        dimmer_poll(T, 1000, 1);
    }
}

static const dimmer_channel_t *c1(void) {
    return dimmer_channel(0);
}

static void momentary_lit_at(uint16_t level) {
    power_up(1);
    host_write(CH1_TYPE, TUYA_DP_TYPE_ENUM, MOMENTARY);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, level);
    run_ms(3000);
}

/* ---- item 35 ---------------------------------------------------------------- */

static void test_type_change_to_state_syncs(void) {
    power_up(1);                                   /* toggle type */
    s1(1);
    run_ms(100);
    CHECK(c1()->on == 1);                          /* the switch moved: on */
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 0);      /* off from the network, switch closed */
    run_ms(100);
    host_log_clear();
    host_write(CH1_TYPE, TUYA_DP_TYPE_ENUM, STATE);
    CHECK(strcmp(g_host_log, "P[4=1] ") == 0);     /* the write's ack alone, as an answer */
    run_ms(1);
    CHECK(c1()->on == 1);                          /* a state switch says what the light is */
    CHECK(strcmp(g_host_log, "P[4=1] A[1=1] ") == 0);
    s1(0);
    run_ms(100);
    CHECK(c1()->on == 0);
}

static void test_type_change_mid_press(void) {
    momentary_lit_at(500);
    s1(1);
    run_ms(100);                                   /* a press seen as momentary ... */
    host_write(CH1_TYPE, TUYA_DP_TYPE_ENUM, TOGGLE);
    run_ms(10);
    s1(0);
    run_ms(100);                                   /* ... released as toggle (off) */
    CHECK(c1()->on == 0);
    host_write(CH1_TYPE, TUYA_DP_TYPE_ENUM, MOMENTARY);
    run_ms(100);
    host_log_clear();
    s1(1);                                         /* one tap */
    run_ms(120);
    s1(0);
    run_ms(1000);
    CHECK(host_log_find("105=1]") >= 0);           /* single, not double */
    CHECK(host_log_find("105=2]") < 0);
    CHECK(c1()->on == 1);
}

/* A press begun under another type: no 'on by this press' of the old one. */
static void test_type_change_clears_on_by_press(void) {
    power_up(1);
    host_write(CH1_TYPE, TUYA_DP_TYPE_ENUM, MOMENTARY);
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 600);
    run_ms(3000);
    s1(1);                                         /* a tap from dark: on by this press */
    run_ms(150);
    s1(0);
    run_ms(2000);
    CHECK(c1()->on == 1 && c1()->level == 600);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 0);
    host_write(CH1_TYPE, TUYA_DP_TYPE_ENUM, TOGGLE);
    run_ms(2000);
    s1(1);                                         /* toggle: on */
    run_ms(100);
    CHECK(c1()->on == 1);
    host_write(CH1_TYPE, TUYA_DP_TYPE_ENUM, MOMENTARY);  /* still closed, before the hold time */
    run_ms(500);
    CHECK(g_host_fader[0].hold_dir != 0);          /* the hold dims the lit light */
    run_ms(1000);
    s1(0);
    run_ms(100);
    CHECK(c1()->on == 1 && c1()->level != 600);    /* and its level is kept */
}

static void test_release_reported_after_level(void) {
    momentary_lit_at(500);
    host_log_clear();
    s1(1);
    run_ms(1500);
    s1(0);
    run_ms(100);
    long level = host_log_find("A[1=1,2=");
    long rel   = host_log_find("A[105=5]");
    CHECK(host_log_find("A[105=4]") >= 0);
    CHECK(level >= 0 && rel >= 0 && level < rel);
}

static void test_first_hold_goes_down(void) {
    momentary_lit_at(800);                         /* above the middle of 200..1000 */
    s1(1);
    run_ms(1200);
    s1(0);
    run_ms(100);
    CHECK(c1()->on == 1 && c1()->level < 800);
}

static void test_boot_held_button(void) {
    power_up(1);
    button(1);
    boot(DIMMER_RESET_POWER);                      /* held at power-up */
    run_ms(3500);
    host_log_clear();
    button(0);
    run_ms(100);
    CHECK(host_log_find("S03") < 0);               /* no rejoin ... */
    CHECK(host_log_find("A[1=") < 0);              /* ... and no toggle */
    CHECK(c1()->on == 0);
    run_ms(10000 - T);                             /* past the first 10 s (the late-press rule) */
    button(1);                                     /* a real 3.5 s press still rejoins */
    run_ms(3500);
    button(0);
    run_ms(100);
    CHECK(host_log_find("S03") >= 0);
}

/* A press of 3 s or more on the module button rejoins, unless it began in the first 10 s after
 * a cold start: a bootloader rescue press that came after the bootloader looked must not take
 * the module off its network (docs/BOOTLOADER.md sections 4, 9 item 8). */
static uint8_t long_press_at(uint32_t csr, uint32_t at, uint32_t hold) {
    power_up(1);
    boot(csr);
    run_ms(at);
    host_log_clear();
    button(1);
    run_ms(hold);
    button(0);
    run_ms(100);
    CHECK(host_log_find("A[") < 0);                /* a long press never toggles */
    return host_log_find("S03") >= 0;
}

static void test_late_rescue_press_is_no_rejoin(void) {
    CHECK(!long_press_at(DIMMER_RESET_POWER, 100, 3500));      /* cold start, at once */
    CHECK(!long_press_at(DIMMER_RESET_POWER, 2000, 8000));     /* held long */
    CHECK(!long_press_at(DIMMER_RESET_POWER, 9900, 3500));     /* began in the window, ends after */
    CHECK(long_press_at(DIMMER_RESET_POWER, 10000, 3500));     /* began after it */
    CHECK(long_press_at(DIMMER_RESET_POWER, 60000, 3100));
    CHECK(!long_press_at(DIMMER_RESET_POWER | DIMMER_RESET_SOFT, 500, 3500));   /* the bootloader
                                                                  restarted after an install */
    CHECK(!long_press_at(1u << 26, 500, 3500));                 /* pin reset: cold too */
    CHECK(long_press_at(DIMMER_RESET_WDG, 500, 3500));         /* a warm start: no rescue press */
    CHECK(long_press_at(DIMMER_RESET_SOFT, 500, 3500));
}

/* Short presses in the window still switch, only the rejoin is held back. */
static void test_early_short_press_toggles(void) {
    power_up(1);
    run_ms(500);
    button(1);
    run_ms(120);
    button(0);
    run_ms(100);
    CHECK(c1()->on == 1 && dimmer_channel(1)->on == 1);
    button(1);                                     /* a 1 s hold: still a toggle */
    run_ms(1000);
    button(0);
    run_ms(100);
    CHECK(c1()->on == 0 && dimmer_channel(1)->on == 0);
}

/* The window closes for good: the ms counter's wrap after 49.7 days does not reopen it. */
static void test_late_press_window_never_reopens(void) {
    power_up(1);
    run_ms(11000);
    T = 0xFFFFFFFFu - 1000u;                       /* 49.7 days later */
    run_ms(3000);                                  /* the counter wraps: T = 1999 */
    CHECK(T < 10000u);
    host_log_clear();
    button(1);
    run_ms(3500);
    button(0);
    run_ms(100);
    CHECK(host_log_find("S03") >= 0);
}

/* ---- the link watchdog's input condition ---------------------------------------- */

static void s2(uint8_t closed) {
    pin(GPIOB, LL_GPIO_PIN_2, closed);
}

/* Ticks without the main loop: events wait in the ring. */
static void ticks(int ms) {
    while (ms--) {
        T++;
        controls_tick_1ms();
    }
}

/* Busy while a press or a change is under way: the button and momentary inputs from the first
 * closed sample to their release, every input while a change is being debounced, and while an
 * event waits for the main loop. */
static void test_controls_idle(void) {
    power_up(1);
    host_write(CH1_TYPE, TUYA_DP_TYPE_ENUM, MOMENTARY);
    host_write(10, TUYA_DP_TYPE_ENUM, MOMENTARY);  /* channel 2's switch type */
    run_ms(3000);
    CHECK(controls_idle());
    s1(1);
    run_ms(5);                                     /* bouncing, not yet a press: under way */
    CHECK(!controls_idle());
    run_ms(30);                                    /* pressed */
    CHECK(!controls_idle());
    s1(0);
    run_ms(5);                                     /* the release being debounced */
    CHECK(!controls_idle());
    run_ms(100);
    CHECK(controls_idle());
    button(1);
    run_ms(1);
    CHECK(!controls_idle());
    run_ms(50);
    CHECK(!controls_idle());
    button(0);
    run_ms(100);
    CHECK(controls_idle());
    s2(1);
    run_ms(50);
    CHECK(!controls_idle());
    s2(0);
    ticks(40);                                     /* a release the main loop has not handled */
    CHECK(!controls_idle());
    controls_poll(T);
    CHECK(controls_idle());
    s1(1);                                         /* one noise sample: under way for a moment */
    ticks(1);
    s1(0);
    CHECK(!controls_idle());
    ticks(1);
    CHECK(controls_idle());
}

/* Resting closed: a state switch keeps it busy (it would light its channel at a restart:
 * controls_init, it says what the light is); a toggle switch does not (the restart takes it as
 * where it rests, test_link_restart_toggle_closed). */
static void test_controls_idle_resting_closed(void) {
    power_up(1);
    host_write(CH1_TYPE, TUYA_DP_TYPE_ENUM, STATE);
    run_ms(3000);
    s1(1);
    run_ms(100);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 0);      /* off from the network, the switch closed */
    run_ms(3000);
    CHECK(c1()->on == 0 && !controls_idle());
    boot(DIMMER_RESET_SOFT);                       /* what a restart now would do */
    CHECK(c1()->on == 1);

    power_up(1);                                   /* toggle switches (the default) */
    s1(1);
    s2(1);
    boot(DIMMER_RESET_POWER);                      /* resting closed at power-up */
    run_ms(100);
    CHECK(c1()->on == 0 && dimmer_channel(1)->on == 0 && controls_idle());
    s1(0);                                         /* on, and resting open */
    run_ms(100);
    CHECK(c1()->on == 1 && controls_idle());
    s1(1);                                         /* off again, resting closed */
    run_ms(100);
    CHECK(c1()->on == 0 && controls_idle());
    s1(0);
    run_ms(10);                                    /* a flip being debounced: busy */
    CHECK(!controls_idle());
    run_ms(100);
    CHECK(c1()->on == 1 && controls_idle());
}

/* ---- the link watchdog with the wall switches (docs/BOOTLOADER.md section 9, item 11) ---- */

/* A v2 bootloader's word after a power-on probe: tries 1. */
#define LINK_WORD (BOOT_WORD_FRESH | (40u << BOOT_PROBE_MS_SHIFT) | 1u)

/* The main loop over a long run with steady inputs, in 10 ms passes, until the restart. */
static void loop_until(uint32_t until) {
    while (T < until && !g_resets) {
        T += 10;
        controls_tick_1ms();
        host_halfcycle();
        controls_poll(T);
        dimmer_poll(T, 1000, 1);
        bootif_poll(T);
    }
}

/* Toggle switch S1 (the default type): on from the switch, off from the network, so the switch
 * rests closed while the light is off; then the link dies. */
static void toggle_closed_link_dead(void) {
    power_up(1);
    host_boot_v2(BOOT_FEATURES_V2, BOOT_APP_MAX);
    host_boot_word(LINK_WORD);
    bootif_init();
    s1(1);
    run_ms(100);
    CHECK(c1()->on == 1 && c1()->switch_type == TOGGLE);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 0);
    loop_until(LINK_WDG_UPTIME_MS + 3600000u);
    CHECK(g_resets == 1 && g_shutdowns == 1 && T == LINK_WDG_UPTIME_MS);   /* it fired */
    CHECK(host_boot_word_now() == ((LINK_WORD & ~BOOT_TRIES_MASK) | BOOT_PROBE_REQ));
}

/* After the restart (SFTRSTF only): still off, the switch where it rests, and its next change
 * toggles the light on, as it would have without the restart. */
static void toggle_closed_after_restart(void) {
    CHECK(c1()->on == 0 && dimmer_channel(1)->on == 0);
    run_ms(1000);
    CHECK(c1()->on == 0);
    host_log_clear();
    s1(0);
    run_ms(100);
    CHECK(c1()->on == 1 && host_log_find("A[1=1") >= 0);
    s1(1);
    run_ms(100);
    CHECK(c1()->on == 0);
}

static void test_link_restart_toggle_closed(void) {
    host_across_real_reset(toggle_closed_link_dead, boot, DIMMER_RESET_SOFT,
                           toggle_closed_after_restart);
}

/* A state switch resting closed while its channel is off (off from the network) would light it
 * at the restart: the watchdog waits, also over a day. */
static void test_link_restart_waits_for_state_switch(void) {
    power_up(1);
    host_boot_v2(BOOT_FEATURES_V2, BOOT_APP_MAX);
    host_boot_word(LINK_WORD);
    bootif_init();
    host_write(CH1_TYPE, TUYA_DP_TYPE_ENUM, STATE);
    s1(1);
    run_ms(100);
    CHECK(c1()->on == 1);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 0);
    loop_until(24u * 3600000u);
    CHECK(g_resets == 0 && c1()->on == 0);
    s1(0);                                         /* opened: the restart a minute later */
    uint32_t opened = T;
    loop_until(opened + LINK_WDG_QUIET_MS);
    CHECK(g_resets == 0);
    loop_until(opened + LINK_WDG_QUIET_MS + 1000u);
    CHECK(g_resets == 1);
}

static void test_boot_held_momentary(void) {
    power_up(1);
    host_write(CH1_TYPE, TUYA_DP_TYPE_ENUM, MOMENTARY);
    run_ms(3000);                                  /* saved */
    s1(1);
    boot(DIMMER_RESET_POWER);
    CHECK(c1()->switch_type == MOMENTARY);
    run_ms(1000);
    host_log_clear();
    s1(0);
    run_ms(1000);
    CHECK(g_host_log[0] == 0);                     /* no 'release', no level report */
    CHECK(c1()->on == 0);
    s1(1);                                         /* the next tap works */
    run_ms(100);
    s1(0);
    run_ms(1000);
    CHECK(c1()->on == 1 && host_log_find("105=1]") >= 0);
}

static void test_boot_held_toggle(void) {
    power_up(1);
    s1(1);
    boot(DIMMER_RESET_POWER);                      /* toggle: closed at boot is where it rests */
    run_ms(1000);
    CHECK(c1()->on == 0);
    s1(0);                                         /* moving it is a real change */
    run_ms(100);
    CHECK(c1()->on == 1);
}

/* A state type switch says what the light is at boot, also over a restore
 * after a watchdog reset and over the power-on setting. */
static void test_state_switch_over_restore(void) {
    power_up(1);
    host_write(CH1_TYPE, TUYA_DP_TYPE_ENUM, STATE);/* S1 open */
    run_ms(10);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);      /* on from the network */
    host_write(CH2_ON, TUYA_DP_TYPE_BOOL, 1);
    run_ms(3000);
    CHECK(c1()->on == 1);
    boot(DIMMER_RESET_WDG);
    CHECK(dimmer_channel(1)->on == 1);             /* restored ... */
    CHECK(c1()->on == 0);                          /* ... but the open switch wins */
}

static void test_state_switch_at_power_up(void) {
    power_up(1);
    host_write(CH1_TYPE, TUYA_DP_TYPE_ENUM, STATE);
    run_ms(3000);                                  /* saved; power-on 'off' */
    s1(1);
    power_up(0);
    CHECK(c1()->switch_type == STATE);
    CHECK(c1()->on == 1);                          /* the closed switch wins */
    CHECK(dimmer_channel(1)->on == 0);             /* channel 2: power-on 'off' */
}

/* ---- item 14 through the switch ---------------------------------------------- */

static void test_dp_on_during_hold_from_dark(void) {
    power_up(1);
    host_write(CH1_TYPE, TUYA_DP_TYPE_ENUM, MOMENTARY);
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 800);
    run_ms(3000);
    s1(1);
    run_ms(300);
    uint16_t at300 = g_host_fader[0].current;
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);      /* HA turn_on with a brightness: */
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 600);/* DP 1 = 1, then DP 2 */
    run_ms(1700);
    uint16_t at2000 = g_host_fader[0].current;
    CHECK(at2000 > at300);                         /* the hold went on */
    host_log_clear();
    s1(0);
    run_ms(100);
    CHECK(c1()->on == 1);
    /* where the finger let go: the release is seen 25 ms later (<= 3 steps per 10 ms) */
    CHECK(c1()->level >= at2000 && c1()->level <= at2000 + 9);
    CHECK(host_log_find("A[1=1,2=") >= 0);
}

static void test_hold_cap(void) {
    power_up(1);
    host_write(CH1_TYPE, TUYA_DP_TYPE_ENUM, MOMENTARY);
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 800);
    run_ms(3000);
    host_log_clear();
    s1(1);                                         /* stuck closed, from dark */
    run_ms(8000);
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 300);
    run_ms(100);
    CHECK(g_host_fader[0].hold_dir == 1);          /* still the finger's: 300 ignored */
    CHECK(host_log_find("A[1=1,2=") < 0);
    run_ms(600);                                   /* 2 x 3300 + 2000 ms of press */
    CHECK(g_host_fader[0].hold_dir == 0);
    CHECK(host_log_find("A[1=1,2=1000]") >= 0);    /* ended at the top, reported */
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 300);/* the network has the light back */
    run_ms(2000);
    CHECK(g_host_fader[0].current == 300);
    host_log_clear();
    s1(0);
    run_ms(100);
    CHECK(strcmp(g_host_log, "A[105=5] ") == 0);
    CHECK(c1()->level == 300);
}

static void test_tap_from_dark_fastest_hold(void) {
    power_up(1);
    host_write(CH1_TYPE, TUYA_DP_TYPE_ENUM, MOMENTARY);
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 600);
    host_write(HOLD_MS_1, TUYA_DP_TYPE_VALUE, 0);  /* hold over the whole range at once */
    run_ms(3000);
    s1(1);
    run_ms(150);
    s1(0);
    run_ms(2000);
    CHECK(c1()->on == 1 && c1()->level == 600 && g_host_fader[0].current == 600);
}

/* DP 180 bit 0 set: a long press from off does not rise from the min: the light comes on at its
 * level as for a tap, and from the hold time on the hold goes on from there (the lit rule: from
 * the top half it goes down). With 1 (the default) the hold from dark rises from the min. */
static void test_long_press_from_off_without_the_rise(void) {
    power_up(1);
    host_write(CH1_TYPE, TUYA_DP_TYPE_ENUM, MOMENTARY);
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 800);
    host_write(180, TUYA_DP_TYPE_VALUE, 0x0101);
    CHECK(c1()->dark_hold_off == 1);
    run_ms(3000);
    s1(1);
    run_ms(300);
    CHECK(c1()->on == 1 && !dimmer_holding(0) && g_host_fader[0].target == 800);   /* on to 800 */
    run_ms(100 + 300);                             /* the hold time, then 0.3 s of a hold */
    CHECK(dimmer_holding(0) && g_host_fader[0].hold_dir == -1);   /* 800 is in the top half: down */
    s1(0);
    run_ms(500);
    CHECK(c1()->on == 1 && c1()->level < 800 && c1()->level == g_host_fader[0].current);
    /* a tap from off: on to its level as before */
    s1(1);
    run_ms(100);
    s1(0);
    run_ms(1500);
    CHECK(c1()->on == 0);
    uint16_t lv = c1()->level;
    s1(1);
    run_ms(100);
    s1(0);
    run_ms(2000);
    CHECK(c1()->on == 1 && g_host_fader[0].current == lv);
    /* back on (bit 0 clear): the rise from the min */
    host_write(180, TUYA_DP_TYPE_VALUE, 0x0100);
    s1(1);
    run_ms(100);
    s1(0);
    run_ms(1500);
    CHECK(c1()->on == 0);
    s1(1);
    run_ms(200);
    CHECK(dimmer_holding(0) && g_host_fader[0].hold_dir == 1 && g_host_fader[0].current == c1()->min);
    s1(0);
    run_ms(200);
}

/* A double tap (two taps, the second begun within GESTURE_GAP_MS of the first's release) takes
 * the light to its max, from off and from on; a tap and a hold still dim; a third tap is a plain
 * one; DP 180 bit 2 turns it off (the old toggles). */
static void tap(void) {
    s1(1);
    run_ms(100);
    s1(0);
}

static void test_double_tap_takes_the_light_to_full(void) {
    power_up(1);
    host_write(CH1_TYPE, TUYA_DP_TYPE_ENUM, MOMENTARY);
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 500);
    run_ms(3000);
    tap();                                         /* from off */
    run_ms(150);
    tap();
    run_ms(2000);
    CHECK(c1()->on == 1 && c1()->level == c1()->max && g_host_fader[0].current == c1()->max);
    tap();                                         /* a single: off */
    run_ms(2000);
    CHECK(c1()->on == 0);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);      /* from on, at a level */
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 400);
    run_ms(3000);
    tap();
    run_ms(150);
    tap();
    run_ms(2000);
    CHECK(c1()->on == 1 && g_host_fader[0].current == c1()->max);
    /* a triple from off: full, then the third tap off */
    tap();
    run_ms(2000);
    CHECK(c1()->on == 0);
    tap();
    run_ms(150);
    tap();
    run_ms(150);
    tap();
    run_ms(2000);
    CHECK(c1()->on == 0);
}

static void test_a_tap_and_a_hold_still_dim(void) {
    momentary_lit_at(600);
    tap();                                         /* off */
    run_ms(150);
    s1(1);                                         /* within the gap, held: a hold from dark */
    run_ms(400 + 700 + 200);
    /* a hold from dark (from where the fade-off had got to), rising: no jump to full */
    CHECK(dimmer_holding(0) && g_host_fader[0].hold_dir == 1 && g_host_fader[0].current < c1()->max - 200);
    s1(0);
    run_ms(1000);
    CHECK(c1()->on == 1 && c1()->level < c1()->max - 200);
}

static void test_double_tap_off_keeps_the_toggles(void) {
    power_up(1);
    host_write(CH1_TYPE, TUYA_DP_TYPE_ENUM, MOMENTARY);
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 500);
    host_write(180, TUYA_DP_TYPE_VALUE, 0x0404);
    run_ms(3000);
    tap();
    run_ms(150);
    tap();                                         /* on, then off */
    run_ms(2000);
    CHECK(c1()->on == 0 && c1()->level == 500);
}

/* ---- a hold's level on the way ------------------------------------------------ */

/* The level-only reports (A[2=v]) in the log from that point on, at most max. */
static unsigned hold_reports(uint16_t *v, unsigned max) {
    unsigned n  = 0;
    long     at = 0;
    while (n < max && (at = host_log_find_from(at, "A[2=")) >= 0) {
        v[n++] = (uint16_t)atoi(&g_host_log[at + 4]);
        at    += 4;
    }
    return n;
}

/* Every 0.5 s from the hold time on (as the factory MCU), for the Zigbee side to follow it
 * (Home Assistant, the lights bound to the wall switch); the end reported as before. */
static void test_hold_reports_its_level_on_the_way(void) {
    uint16_t v[8];

    momentary_lit_at(800);
    host_log_clear();
    s1(1);
    run_ms(400 + 1200);                            /* the hold time, then 1.2 s */
    unsigned n = hold_reports(v, 8);
    CHECK(n == 2);
    CHECK(n == 2 && v[0] < 800 && v[1] < v[0]);    /* the first hold goes down from 800 */
    host_log_clear();
    s1(0);
    run_ms(100);
    CHECK(host_log_find("A[1=1,2=") >= 0);
    CHECK(hold_reports(v, 8) == 0);
}

static void test_hold_from_dark_reports_from_its_hold_time(void) {
    uint16_t v[8];

    power_up(1);
    host_write(CH1_TYPE, TUYA_DP_TYPE_ENUM, MOMENTARY);
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 600);
    run_ms(3000);
    host_log_clear();
    s1(1);                                         /* a tap from dark: none */
    run_ms(300);
    s1(0);
    run_ms(1500);
    CHECK(c1()->on == 1 && hold_reports(v, 8) == 0);
    s1(1);                                         /* off again */
    run_ms(100);
    s1(0);
    run_ms(1500);
    CHECK(c1()->on == 0);
    host_log_clear();
    s1(1);                                         /* a hold from dark: rising */
    run_ms(400 + 1200);
    unsigned n = hold_reports(v, 8);
    CHECK(n == 2 && v[1] > v[0]);
    s1(0);
    run_ms(100);
}

static void run(void (*test)(void), const char *name) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        g_fail = 0;
        test();
        fflush(stdout);
        _exit(g_fail ? 1 : 0);
    }
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status)) {
        printf("FAIL %s\n", name);
        g_fail = 1;
    }
}

#define RUN(t) run(t, #t)


/* ---- 2.2.0: wall switch mode (DP 162 / 163) ----------------------------------------- */

#define CH1_INPUT_MODE 162
#define DETACHED 1
#define INPUT_OFF 2

static void test_detached_momentary_reports_gestures_and_leaves_the_light(void) {
    power_up(1);
    host_write(CH1_TYPE, TUYA_DP_TYPE_ENUM, MOMENTARY);
    host_write(CH1_INPUT_MODE, TUYA_DP_TYPE_VALUE, DETACHED);
    run_ms(3000);
    host_log_clear();
    s1(1);                                         /* a tap */
    run_ms(120);
    s1(0);
    run_ms(1000);
    CHECK(c1()->on == 0 && g_host_fader[0].target == 0);   /* the light is the network's */
    CHECK(host_log_find("105=1]") >= 0 && host_log_find("A[1=") < 0);
    host_log_clear();
    s1(1);                                         /* a hold: reported, no dimming */
    run_ms(1500);
    s1(0);
    run_ms(500);
    CHECK(host_log_find("105=4]") >= 0 && host_log_find("105=5]") >= 0);
    CHECK(c1()->on == 0 && g_host_fader[0].hold_dir == 0);
}

static void test_detached_toggle_reports_each_change_as_a_single(void) {
    power_up(1);                                   /* toggle type */
    host_write(CH1_INPUT_MODE, TUYA_DP_TYPE_VALUE, DETACHED);
    run_ms(3000);
    host_log_clear();
    s1(1);
    run_ms(100);
    s1(0);
    run_ms(100);
    CHECK(c1()->on == 0 && host_log_find("A[1=") < 0);
    long first = host_log_find("105=1]");
    CHECK(first >= 0 && host_log_find_from(first + 1, "105=1]") >= 0);   /* both flips */
}

static void test_input_off_ignores_the_switch_and_the_mode_is_saved(void) {
    power_up(1);
    host_write(CH1_TYPE, TUYA_DP_TYPE_ENUM, STATE);
    host_write(CH1_INPUT_MODE, TUYA_DP_TYPE_VALUE, INPUT_OFF);
    run_ms(3000);                                  /* saved */
    host_log_clear();
    s1(1);
    run_ms(200);
    CHECK(c1()->on == 0 && host_log_find("A[") < 0);
    boot(DIMMER_RESET_POWER);                      /* a power cut with the state switch closed */
    run_ms(200);
    CHECK(c1()->input_mode == INPUT_OFF && c1()->on == 0);   /* it does not set the light */
    host_write(CH1_INPUT_MODE, TUYA_DP_TYPE_VALUE, 0);       /* back to normal */
    s1(0);
    run_ms(100);
    s1(1);
    run_ms(100);
    CHECK(c1()->on == 1);
}


#define CH1_FAST_PRESS 170

static void test_fast_press_reports_the_press_at_once_and_no_multi_taps(void) {
    power_up(1);
    host_write(CH1_TYPE, TUYA_DP_TYPE_ENUM, MOMENTARY);
    host_write(CH1_INPUT_MODE, TUYA_DP_TYPE_VALUE, DETACHED);
    host_write(CH1_FAST_PRESS, TUYA_DP_TYPE_VALUE, 1);
    run_ms(3000);
    host_log_clear();
    s1(1);
    run_ms(40);                                    /* debounce + a pass: reported already */
    CHECK(host_log_find("105=6]") >= 0);
    s1(0);
    run_ms(1000);
    CHECK(host_log_find("105=1]") < 0 && host_log_find("105=2]") < 0);   /* no single after it */
    host_log_clear();
    s1(1);                                         /* a hold: press, hold, release */
    run_ms(1500);
    s1(0);
    run_ms(200);
    CHECK(host_log_find("105=6]") >= 0 && host_log_find("105=4]") >= 0 && host_log_find("105=5]") >= 0);
    CHECK(c1()->on == 0);                          /* detached: the light is the network's */
    host_write(CH1_FAST_PRESS, TUYA_DP_TYPE_VALUE, 0);     /* off: the gestures as before */
    host_log_clear();
    s1(1);
    run_ms(120);
    s1(0);
    run_ms(1000);
    CHECK(host_log_find("105=1]") >= 0 && host_log_find("105=6]") < 0);
    host_write(CH1_INPUT_MODE, TUYA_DP_TYPE_VALUE, 0);     /* normal mode: never 6 */
    host_write(CH1_FAST_PRESS, TUYA_DP_TYPE_VALUE, 1);
    host_log_clear();
    s1(1);
    run_ms(120);
    s1(0);
    run_ms(1000);
    CHECK(host_log_find("105=6]") < 0 && c1()->on == 1);
}


static void test_a_mode_change_drops_a_pending_gesture_and_a_press_under_way(void) {
    power_up(1);
    host_write(CH1_TYPE, TUYA_DP_TYPE_ENUM, MOMENTARY);
    host_write(CH1_INPUT_MODE, TUYA_DP_TYPE_VALUE, DETACHED);
    run_ms(3000);
    host_log_clear();
    s1(1);                                         /* a tap, its single still pending ... */
    run_ms(120);
    s1(0);
    run_ms(100);
    host_write(CH1_INPUT_MODE, TUYA_DP_TYPE_VALUE, INPUT_OFF);   /* ... when the mode goes off */
    run_ms(1000);
    CHECK(host_log_find("105=") < 0);
    host_write(CH1_INPUT_MODE, TUYA_DP_TYPE_VALUE, 0);           /* normal, the light on */
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);
    run_ms(100);
    host_write(CH1_INPUT_MODE, TUYA_DP_TYPE_VALUE, DETACHED);
    run_ms(50);
    s1(1);                                         /* pressed in detached ... */
    run_ms(100);
    host_write(CH1_INPUT_MODE, TUYA_DP_TYPE_VALUE, 0);           /* ... let go in normal */
    run_ms(50);
    s1(0);
    run_ms(1000);
    CHECK(c1()->on == 1);                          /* that release is not normal's: still on */
    s1(1);                                         /* the next press is */
    run_ms(120);
    s1(0);
    run_ms(200);
    CHECK(c1()->on == 0);
}

static void test_a_state_switch_says_the_light_when_back_in_normal(void) {
    power_up(1);
    host_write(CH1_TYPE, TUYA_DP_TYPE_ENUM, STATE);
    run_ms(100);
    s1(1);
    run_ms(100);
    CHECK(c1()->on == 1);
    host_write(CH1_INPUT_MODE, TUYA_DP_TYPE_VALUE, DETACHED);
    run_ms(50);
    s1(0);                                         /* flipped while detached: light stays */
    run_ms(100);
    CHECK(c1()->on == 1);
    host_write(CH1_INPUT_MODE, TUYA_DP_TYPE_VALUE, 0);
    run_ms(50);
    CHECK(c1()->on == 0);                          /* normal again: as the switch is */
}

/* ---- network reset by presses (DP 181) ------------------------------------------- */

#define RESET_PRESSES 181

/* n closures of S1 (or S2), every `every` ms; a toggle switch is closed and opened again. */
static void closures(uint8_t input, int n, uint32_t every) {
    for (int i = 0; i < n; i++) {
        if (input == 0) s1(1); else s2(1);
        run_ms(80);
        if (input == 0) s1(0); else s2(0);
        run_ms(every - 80);
    }
}

static int resets(void) {
    int n = 0;
    for (long at = 0; (at = host_log_find_from(at, "S03")) >= 0; at++) {
        n++;
    }
    return n;
}

static void reset_ready(uint8_t type) {
    power_up(1);
    host_write(CH1_TYPE, TUYA_DP_TYPE_ENUM, type);
    host_write(CH1_TYPE + 6, TUYA_DP_TYPE_ENUM, type);
    run_ms(3000);
    host_log_clear();
}

/* As the stock MCU: 8 closures of one switch within 10 s of the first ask for a leave and
 * pairing, once; the switch's channel blinks then. */
static void test_eight_closures_ask_for_a_network_reset(void) {
    reset_ready(MOMENTARY);
    closures(0, 7, 400);
    CHECK(resets() == 0);
    closures(0, 1, 400);
    CHECK(resets() == 1);
    unsigned dark = 0, full = 0;                    /* the blink: dark and full in turn */
    for (int i = 0; i < 250; i++) {
        run_ms(10);
        dark += g_host_fader[0].current == 0;
        full += g_host_fader[0].current == DIMMER_LEVEL_MAX;
    }
    CHECK(dark > 80 && full > 80);
    closures(0, 7, 400);                            /* a new count from 0 */
    CHECK(resets() == 1);
}

static void test_toggle_and_state_switches_count_their_closures(void) {
    reset_ready(TOGGLE);
    closures(0, 8, 1000);                           /* 16 changes, 8 closures in 7.1 s */
    CHECK(resets() == 1);
    reset_ready(STATE);
    closures(1, 8, 1000);                           /* S2 */
    CHECK(resets() == 1);
}

static void test_the_window_is_ten_seconds_from_the_first(void) {
    reset_ready(MOMENTARY);
    closures(0, 7, 1400);                           /* the 8th 9.8 s after the first: in */
    closures(0, 1, 400);
    CHECK(resets() == 1);
    host_log_clear();
    closures(0, 7, 1500);                           /* the 8th 10.5 s after the first: out ... */
    closures(0, 1, 400);
    CHECK(resets() == 0);
    closures(0, 7, 400);                            /* ... it began a new count */
    CHECK(resets() == 1);
}

static void test_the_other_switch_starts_a_new_count(void) {
    reset_ready(MOMENTARY);
    closures(0, 5, 300);
    closures(1, 1, 300);
    closures(0, 5, 300);
    CHECK(resets() == 0);
    closures(0, 3, 300);
    CHECK(resets() == 1);
}

static void test_the_count_is_dp_181(void) {
    reset_ready(MOMENTARY);
    host_write(RESET_PRESSES, TUYA_DP_TYPE_VALUE, 0);          /* never */
    closures(0, 20, 300);
    CHECK(resets() == 0);
    host_write(RESET_PRESSES, TUYA_DP_TYPE_VALUE, 3);          /* acts as 5: no triple tap */
    run_ms(11000);
    closures(0, 4, 300);
    CHECK(resets() == 0);
    closures(0, 1, 300);
    CHECK(resets() == 1);
    host_write(RESET_PRESSES, TUYA_DP_TYPE_VALUE, 12);
    run_ms(11000);
    host_log_clear();
    closures(0, 11, 300);
    CHECK(resets() == 0);
    closures(0, 1, 300);
    CHECK(resets() == 1);
    CHECK(!dimmer_write_dp(RESET_PRESSES, TUYA_DP_TYPE_VALUE, (const uint8_t *)"\0\0\x01\0", 4));
    boot(DIMMER_RESET_POWER);                                  /* not saved: 8 again */
    run_ms(3000);
    host_log_clear();
    closures(0, 8, 300);
    CHECK(resets() == 1);
}

static void test_detached_counts_and_off_does_not(void) {
    reset_ready(MOMENTARY);
    host_write(CH1_INPUT_MODE, TUYA_DP_TYPE_VALUE, DETACHED);
    closures(0, 8, 300);
    CHECK(resets() == 1 && c1()->on == 0);          /* the light stays the network's */
    host_write(CH1_INPUT_MODE, TUYA_DP_TYPE_VALUE, INPUT_OFF);
    run_ms(11000);
    host_log_clear();
    closures(0, 10, 300);
    CHECK(resets() == 0);
}

/* A switch closed at power-up rests there: its release is no press, its next closure is. */
static void test_a_switch_closed_at_power_up_is_no_press(void) {
    power_up(1);
    s1(1);
    boot(DIMMER_RESET_POWER);
    run_ms(3000);
    host_log_clear();
    s1(0);
    run_ms(300);
    closures(0, 7, 300);
    CHECK(resets() == 0);
    closures(0, 1, 300);
    CHECK(resets() == 1);
}

int main(void) {
    RUN(test_type_change_to_state_syncs);
    RUN(test_type_change_mid_press);
    RUN(test_type_change_clears_on_by_press);
    RUN(test_release_reported_after_level);
    RUN(test_first_hold_goes_down);
    RUN(test_boot_held_button);
    RUN(test_late_rescue_press_is_no_rejoin);
    RUN(test_early_short_press_toggles);
    RUN(test_late_press_window_never_reopens);
    RUN(test_controls_idle);
    RUN(test_controls_idle_resting_closed);
    RUN(test_link_restart_toggle_closed);
    RUN(test_link_restart_waits_for_state_switch);
    RUN(test_boot_held_momentary);
    RUN(test_boot_held_toggle);
    RUN(test_state_switch_over_restore);
    RUN(test_state_switch_at_power_up);
    RUN(test_dp_on_during_hold_from_dark);
    RUN(test_hold_cap);
    RUN(test_tap_from_dark_fastest_hold);
    RUN(test_hold_reports_its_level_on_the_way);
    RUN(test_hold_from_dark_reports_from_its_hold_time);
    RUN(test_long_press_from_off_without_the_rise);
    RUN(test_double_tap_takes_the_light_to_full);
    RUN(test_a_tap_and_a_hold_still_dim);
    RUN(test_double_tap_off_keeps_the_toggles);
    RUN(test_detached_momentary_reports_gestures_and_leaves_the_light);
    RUN(test_detached_toggle_reports_each_change_as_a_single);
    RUN(test_input_off_ignores_the_switch_and_the_mode_is_saved);
    RUN(test_fast_press_reports_the_press_at_once_and_no_multi_taps);
    RUN(test_a_mode_change_drops_a_pending_gesture_and_a_press_under_way);
    RUN(test_a_state_switch_says_the_light_when_back_in_normal);
    RUN(test_eight_closures_ask_for_a_network_reset);
    RUN(test_toggle_and_state_switches_count_their_closures);
    RUN(test_the_window_is_ten_seconds_from_the_first);
    RUN(test_the_other_switch_starts_a_new_count);
    RUN(test_the_count_is_dp_181);
    RUN(test_detached_counts_and_off_does_not);
    RUN(test_a_switch_closed_at_power_up_is_no_press);
    if (g_fail) {
        printf("test_controls: FAILED\n");
        return 1;
    }
    printf("test_controls: all passed\n");
    return 0;
}
