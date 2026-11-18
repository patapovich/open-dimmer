/* dimmer.c on the PC (tests/host_stubs.c stands in for phase.c, store.c and
 * the Tuya report builder). Each test runs in its own process, so it starts
 * from a zeroed .bss like the MCU after a reset; dimmer_init() again in a
 * test is a reset that keeps the RAM (mailbox and dimmer.c's statics). */
#include "host_stubs.h"
#include "dimmer.h"
#include "tuya.h"
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CH1_ON     1
#define CH1_LEVEL  2
#define CH1_MIN    3
#define CH1_TYPE   4
#define CH1_MAX    5
#define POWER_ON_1 103
#define A_MINUTE_MS 60000      /* dimmer.c RESTORE_CLEAR_MS */

/* RCC_CSR at start (PY32F002A: bit 25 option bytes, 26 pin, 27 power/BOR,
 * 28 software, 29 watchdog). The pin flag comes with every reset. */
#define CSR_PIN        (1u << 26)
#define CSR_POWER_UP   (DIMMER_RESET_POWER | CSR_PIN)
#define CSR_WATCHDOG   (DIMMER_RESET_WDG | CSR_PIN)
#define CSR_SOFTWARE   (DIMMER_RESET_SOFT | CSR_PIN)

static uint32_t T;    /* ms since this boot (main.c g_ms) */

static void boot(uint32_t csr) {
    host_phase_init();
    T = 0;
    dimmer_init(csr);
}

static void run_ms(uint32_t ms) {
    while (ms--) {
        T++;
        if (T % 10 == 0) {
            host_halfcycle();
        }
        dimmer_poll(T, 1000, 1);
    }
}

/* Power-up: random RAM, nothing saved (or keep the flash: power_up(0)). */
static void power_up(uint8_t erase_flash) {
    memset((void *)&g_host_mailbox, 0xA5, sizeof(g_host_mailbox));
    if (erase_flash) {
        g_host_flash_valid = 0;
    }
    boot(CSR_POWER_UP);
}

static uint8_t mb_on(uint8_t ch) {
    return (uint8_t)((g_host_mailbox.restore >> ch) & 1u);
}

static uint16_t mb_level(uint8_t ch) {
    return (uint16_t)((g_host_mailbox.restore >> (2 + 10 * ch)) & 0x3FFu);
}

static uint8_t mb_valid(void) {
    return (g_host_mailbox.restore ^ OTA_RESTORE_MAGIC) == g_host_mailbox.restore_check;
}

static const dimmer_channel_t *c1(void) {
    return dimmer_channel(0);
}

/* ---- item 13: back as it was after a watchdog, fault or update reset ------- */

static void test_power_up_takes_power_on_rule(void) {
    power_up(1);
    CHECK(c1()->on == 0 && c1()->level == DIMMER_LEVEL_MAX);   /* defaults: off */
    CHECK(g_host_mailbox.restored == 0);
    CHECK(mb_valid() && mb_on(0) == 0 && mb_level(0) == 1000);  /* kept current from the start */

    /* A brown-out that kept the RAM is a power-on all the same. */
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);
    run_ms(3000);
    CHECK(mb_valid() && mb_on(0) == 1);
    boot(CSR_POWER_UP);
    CHECK(c1()->on == 0);
    CHECK(g_host_fader[0].target == 0);
}

static void test_watchdog_reset_restores_live_state(void) {
    power_up(1);                                   /* power-on: off (default) */
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 900);
    host_write(7, TUYA_DP_TYPE_BOOL, 1);           /* channel 2 on at 1000 */
    host_write(8, TUYA_DP_TYPE_VALUE, 450);
    run_ms(3000);
    CHECK(mb_valid() && mb_on(0) && mb_level(0) == 900 && mb_on(1) && mb_level(1) == 450);

    boot(CSR_WATCHDOG);                            /* watchdog: stays lit, not 'off' */
    CHECK(c1()->on == 1 && c1()->level == 900);
    CHECK(dimmer_channel(1)->on == 1 && dimmer_channel(1)->level == 450);
    CHECK(g_host_fader[0].target == 900 && g_host_fader[1].target == 450);
    CHECK(g_host_mailbox.restored == OTA_RESTORED_MAGIC);

    /* Dark stays dark with power-on 'on'. */
    host_write(POWER_ON_1, TUYA_DP_TYPE_ENUM, 1);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 0);
    run_ms(61000);                                 /* a minute: the next reset may restore */
    CHECK(g_host_mailbox.restored == 0);
    boot(CSR_WATCHDOG);
    CHECK(c1()->on == 0 && g_host_fader[0].target == 0);
    /* ... and a power cut takes the rule: on. */
    run_ms(61000);
    boot(CSR_POWER_UP);
    CHECK(c1()->on == 1);
}

static void test_restore_once_in_a_row(void) {
    power_up(1);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 700);
    run_ms(3000);
    boot(CSR_WATCHDOG);                            /* first reset: restored */
    CHECK(c1()->on == 1 && c1()->level == 700);
    run_ms(10000);
    boot(CSR_WATCHDOG);                            /* again within a minute: the rule (off) */
    CHECK(c1()->on == 0);
    CHECK(g_host_mailbox.restored == OTA_RESTORED_MAGIC);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);
    run_ms(5000);
    boot(CSR_WATCHDOG);                            /* still a loop: the rule again */
    CHECK(c1()->on == 0);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);
    run_ms(A_MINUTE_MS);
    boot(CSR_WATCHDOG);                            /* a run of a minute ended the loop */
    CHECK(c1()->on == 1 && c1()->level == 700);
}

static void test_update_restart_restores(void) {
    power_up(1);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 640);
    run_ms(3000);
    boot(CSR_WATCHDOG);                            /* a watchdog reset: restored */
    CHECK(c1()->on == 1 && c1()->level == 640);
    run_ms(5000);                                  /* an update offered within the minute */
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 610);
    unsigned saves = g_host_saves;
    dimmer_prepare_restart();
    CHECK(g_host_saves == saves + 1);              /* the pending level is saved first */
    boot(CSR_SOFTWARE);                            /* back from the bootloader */
    CHECK(c1()->on == 1 && c1()->level == 610);

    /* An update from a version that does not know the 'restored' word: the
     * word is whatever the RAM held. */
    run_ms(3000);
    dimmer_prepare_restart();
    g_host_mailbox.restored = 0xA5A5A5A5u;
    boot(CSR_SOFTWARE);
    CHECK(c1()->on == 1 && c1()->level == 610);
}

/* Which resets restore: watchdog and software (update restart, fault), not with
 * the power flag (a power-up or brown-out, also when the bootloader restarted
 * after it), not a pin or option byte reset alone. */
static void test_reset_flags(void) {
    static const struct {
        uint32_t csr;
        uint8_t  restores;
    } k[] = {
        { CSR_WATCHDOG, 1 },
        { CSR_SOFTWARE, 1 },
        { DIMMER_RESET_SOFT, 1 },
        { CSR_WATCHDOG | DIMMER_RESET_SOFT, 1 },
        { CSR_POWER_UP, 0 },
        { CSR_POWER_UP | DIMMER_RESET_SOFT, 0 },
        { CSR_POWER_UP | DIMMER_RESET_WDG, 0 },
        { CSR_PIN, 0 },
        { 1u << 25, 0 },
        { 0, 0 },
    };
    for (unsigned i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        power_up(1);
        host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);
        run_ms(A_MINUTE_MS);
        boot(k[i].csr);
        CHECK(c1()->on == k[i].restores);
    }
}

static void test_restore_needs_valid_words(void) {
    power_up(1);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);
    run_ms(3000);
    g_host_mailbox.restore_check ^= 1u;           /* torn or random */
    boot(CSR_WATCHDOG);
    CHECK(c1()->on == 0);
}

static void test_restore_keeps_min_max(void) {
    power_up(1);
    host_write(CH1_MIN, TUYA_DP_TYPE_VALUE, 300);
    host_write(CH1_MAX, TUYA_DP_TYPE_VALUE, 800);
    run_ms(3000);
    uint32_t r = 1u | (100u << 2);                 /* on at 100: below the min */
    g_host_mailbox.restore       = r;
    g_host_mailbox.restore_check = r ^ OTA_RESTORE_MAGIC;
    boot(CSR_WATCHDOG);
    CHECK(c1()->on == 1 && c1()->level == 300);
}

/* ---- item 14: holds and network on/off -------------------------------------- */

static void ticks(uint32_t n) {
    run_ms(10 * n);
}

static void test_dp_on_during_hold_from_dark(void) {
    power_up(1);
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 800);
    run_ms(3000);
    dimmer_local_set(0, 1);                        /* momentary press from dark */
    dimmer_hold_begin(0, +1, 1);
    ticks(30);
    uint16_t at300 = g_host_fader[0].current;
    host_log_clear();
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);      /* the ZTU before a level change */
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 600);
    CHECK(g_host_fader[0].hold_dir == 1);          /* the hold goes on */
    ticks(100);
    uint16_t at1300 = g_host_fader[0].current;
    CHECK(at1300 > at300);
    dimmer_hold_end(0);                            /* release: the level the light has */
    CHECK(c1()->on == 1 && c1()->level == at1300);
    CHECK(host_log_find("A[1=1,2=") >= 0);
}

static void test_dp_off_ends_hold(void) {
    power_up(1);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 500);
    run_ms(3000);
    dimmer_hold_begin(0, 0, 0);
    ticks(20);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 0);
    CHECK(g_host_fader[0].hold_dir == 0 && g_host_fader[0].target == 0);
    host_log_clear();
    dimmer_hold_end(0);                            /* the release after it: nothing */
    CHECK(g_host_log[0] == 0);
    CHECK(c1()->on == 0);
}

static void test_local_off_ends_hold(void) {
    power_up(1);
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 800);
    run_ms(3000);
    dimmer_local_set(0, 1);
    dimmer_hold_begin(0, +1, 1);
    ticks(50);
    dimmer_local_toggle_all();                     /* module button: all off */
    CHECK(c1()->on == 0 && g_host_fader[0].hold_dir == 0 && g_host_fader[0].target == 0);
    ticks(30);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);      /* on again from the network */
    ticks(30);
    host_log_clear();
    dimmer_hold_end(0);                            /* the release: no frozen mid-fade level */
    CHECK(g_host_log[0] == 0);
    CHECK(c1()->level == 800 && g_host_fader[0].target == 800);
}

static void test_button_off_and_on_during_hold(void) {
    power_up(1);
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 800);
    run_ms(3000);
    dimmer_local_set(0, 1);
    dimmer_hold_begin(0, +1, 1);
    ticks(50);
    dimmer_local_toggle_all();                     /* module button: all off ... */
    ticks(20);
    dimmer_local_toggle_all();                     /* ... and on again, finger still down */
    ticks(20);
    CHECK(g_host_fader[0].current < 800);          /* fading on */
    host_log_clear();
    dimmer_hold_end(0);                            /* the release: the fade goes on */
    CHECK(g_host_log[0] == 0);
    CHECK(c1()->level == 800 && g_host_fader[0].target == 800);
}

/* ---- item 34: the hold level is saved; item 50: also over an update --------- */

static void test_hold_level_saved(void) {
    power_up(1);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 500);
    run_ms(3000);
    unsigned saves = g_host_saves;
    dimmer_hold_begin(0, +1, 0);
    ticks(60);
    dimmer_hold_end(0);
    uint16_t level = c1()->level;
    CHECK(level > 500);
    run_ms(3000);
    CHECK(g_host_saves == saves + 1);
    host_saved_t s;
    memcpy(&s, g_host_flash, sizeof(s));
    CHECK(s.ch[0].level == level);
}

static void test_update_during_hold(void) {
    power_up(1);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 500);
    run_ms(3000);
    dimmer_hold_begin(0, +1, 0);
    ticks(60);
    uint16_t now = g_host_fader[0].current;
    CHECK(now > 500);
    dimmer_prepare_restart();
    CHECK(mb_valid() && mb_on(0) && mb_level(0) == now);
    host_saved_t s;
    memcpy(&s, g_host_flash, sizeof(s));
    CHECK(s.ch[0].level == now);
    boot(CSR_SOFTWARE);
    CHECK(c1()->on == 1 && c1()->level == now);
}

/* ---- item 35 (dimmer part): the first hold goes towards the far end -------- */

static void test_first_hold_direction(void) {
    power_up(1);                                   /* min 200, max 1000: mid 600 */
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 800);
    run_ms(3000);
    dimmer_hold_begin(0, 0, 0);
    CHECK(g_host_fader[0].hold_dir == -1);         /* above mid: down */
    ticks(20);
    dimmer_hold_end(0);
    dimmer_hold_begin(0, 0, 0);
    CHECK(g_host_fader[0].hold_dir == 1);          /* then the other way, as before */
    dimmer_hold_end(0);
}

static void test_first_hold_direction_low(void) {
    power_up(1);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 400);
    run_ms(3000);
    dimmer_hold_begin(0, 0, 0);
    CHECK(g_host_fader[0].hold_dir == 1);          /* below mid: up */
    dimmer_hold_end(0);
}

/* ---- item 36: min/max writes keep the level inside -------------------------- */

static void test_max_below_level(void) {
    power_up(1);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);      /* at 1000 */
    run_ms(3000);
    host_log_clear();
    host_write(CH1_MAX, TUYA_DP_TYPE_VALUE, 394);
    CHECK(c1()->max == 394 && c1()->level == 394);
    CHECK(g_host_fader[0].target == 394);          /* the light follows at once */
    CHECK(host_log_find("P[5=394,3=200,2=394] ") == 0);
    host_log_clear();
    host_write(CH1_MIN, TUYA_DP_TYPE_VALUE, 600);  /* min above max: max follows, and the level */
    CHECK(c1()->min == 600 && c1()->max == 600 && c1()->level == 600);
    CHECK(host_log_find("P[3=600,5=600,2=600] ") == 0);
    run_ms(3000);
    host_saved_t s;
    memcpy(&s, g_host_flash, sizeof(s));
    CHECK(s.ch[0].level == 600 && s.ch[0].max == 600);
    host_log_clear();
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 900); /* the level write: no siblings */
    CHECK(host_log_find("P[2=600] ") == 0);
}

/* ---- items 36 and 40: what is loaded is kept in range ------------------------ */

static void test_load_clamps(void) {
    host_saved_t s;
    memset(&s, 0, sizeof(s));
    s.format = 3;
    for (int i = 0; i < 2; i++) {
        s.ch[i].level       = 1000;
        s.ch[i].min         = 200;
        s.ch[i].max         = 400;
        s.ch[i].switch_type = (uint8_t)(i ? 2 : 7);   /* 7: not a switch type */
    }
    memcpy(g_host_flash, &s, sizeof(s));
    g_host_flash_valid = 1;
    power_up(0);
    CHECK(c1()->level == 400 && dimmer_channel(1)->level == 400);
    CHECK(c1()->switch_type == 0 && dimmer_channel(1)->switch_type == 2);
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
    RUN(test_power_up_takes_power_on_rule);
    RUN(test_watchdog_reset_restores_live_state);
    RUN(test_restore_once_in_a_row);
    RUN(test_update_restart_restores);
    RUN(test_reset_flags);
    RUN(test_restore_needs_valid_words);
    RUN(test_restore_keeps_min_max);
    RUN(test_dp_on_during_hold_from_dark);
    RUN(test_dp_off_ends_hold);
    RUN(test_local_off_ends_hold);
    RUN(test_button_off_and_on_during_hold);
    RUN(test_hold_level_saved);
    RUN(test_update_during_hold);
    RUN(test_first_hold_direction);
    RUN(test_first_hold_direction_low);
    RUN(test_max_below_level);
    RUN(test_load_clamps);
    if (g_fail) {
        printf("test_dimmer: FAILED\n");
        return 1;
    }
    printf("test_dimmer: all passed\n");
    return 0;
}
