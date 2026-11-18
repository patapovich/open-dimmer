/* controls.c with dimmer.c, input_core.c and gesture_core.c on the PC, driven
 * through the pins (board.h host GPIO) every 1 ms like SysTick and the main
 * loop. Each test runs in its own process (a zeroed .bss, as after a reset). */
#include "host_stubs.h"
#include "controls.h"
#include "dimmer.h"
#include "gesture_core.h"
#include "tuya.h"
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

static void test_controls_idle(void) {
    power_up(1);
    run_ms(100);
    CHECK(controls_idle());
    s1(1);
    run_ms(5);                                     /* bouncing, not yet a press */
    CHECK(controls_idle());
    run_ms(30);                                    /* pressed */
    CHECK(!controls_idle());
    s1(0);
    run_ms(100);
    CHECK(controls_idle());
    button(1);
    run_ms(50);
    CHECK(!controls_idle());
    button(0);
    run_ms(100);
    CHECK(controls_idle());
    pin(GPIOB, LL_GPIO_PIN_2, 1);                  /* S2 */
    run_ms(50);
    CHECK(!controls_idle());
    pin(GPIOB, LL_GPIO_PIN_2, 0);
    /* A release the main loop has not handled yet: an event waits. */
    for (int i = 0; i < 40; i++) {
        T++;
        controls_tick_1ms();
    }
    CHECK(!controls_idle());
    controls_poll(T);
    CHECK(controls_idle());
}

/* A toggle or state switch resting closed keeps it busy: the state switch would light its
 * channel at a restart (controls_init: it says what the light is). */
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
    power_up(1);                                   /* a toggle switch resting closed */
    s1(1);
    boot(DIMMER_RESET_POWER);
    run_ms(100);
    CHECK(c1()->on == 0 && !controls_idle());
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
    RUN(test_boot_held_momentary);
    RUN(test_boot_held_toggle);
    RUN(test_state_switch_over_restore);
    RUN(test_state_switch_at_power_up);
    RUN(test_dp_on_during_hold_from_dark);
    RUN(test_hold_cap);
    RUN(test_tap_from_dark_fastest_hold);
    if (g_fail) {
        printf("test_controls: FAILED\n");
        return 1;
    }
    printf("test_controls: all passed\n");
    return 0;
}
