/* dimmer.c on the PC (tests/host_stubs.c stands in for phase.c, store.c and
 * the Tuya report builder). Each test runs in its own process, so it starts
 * from a zeroed .bss like the MCU after a reset; dimmer_init() again in a
 * test is a reset that keeps the RAM (mailbox and dimmer.c's statics), and
 * across_real_reset() one that zeroes .bss as the MCU does. */
#include "host_stubs.h"
#include "bootif.h"
#include "controls.h"
#include "dimmer.h"
#include "host_boot.h"
#include "phase.h"
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
#define DP_POWER_ON 14
#define A_MINUTE_MS 60000      /* dimmer.c RESTORE_CLEAR_MS */

/* RCC_CSR at start (PY32F002A: bit 25 option bytes, 26 pin, 27 power/BOR,
 * 28 software, 29 watchdog). The pin flag comes with every reset. */
#define CSR_PIN        (1u << 26)
#define CSR_POWER_UP   (DIMMER_RESET_POWER | CSR_PIN)
#define CSR_WATCHDOG   (DIMMER_RESET_WDG | CSR_PIN)
#define CSR_SOFTWARE   (DIMMER_RESET_SOFT | CSR_PIN)

static uint32_t T;    /* ms since this boot (main.c g_ms) */

/* bootif.c's other side (tuya.c, controls.c, phase.c, the reset): the module's frame count and
 * the input state are the test's; the restart path is recorded. */
uint32_t g_tuya_frames;
uint8_t  g_tuya_confirm;
static uint8_t  g_inputs_idle = 1;
static unsigned g_shutdowns, g_resets;
static uint32_t g_word_at_shutdown, g_word_at_reset, g_check_at_reset, g_restored_at_reset;
uint8_t controls_idle(void) {
    return g_inputs_idle;
}
void phase_shutdown(void) {
    g_shutdowns++;
    g_word_at_shutdown = g_host_mailbox.boot;
}
void NVIC_SystemReset(void) {
    g_resets++;
    g_word_at_reset     = g_host_mailbox.boot;
    g_check_at_reset    = g_host_mailbox.boot_check;
    g_restored_at_reset = g_host_mailbox.restored;
}

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
    CHECK(g_host_saves == saves + 1);              /* the pending level is saved first, */
    CHECK(g_host_save_attempts == 2);              /* on a second page if the first fails */
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

/* A reset as on the MCU (.bss zeroed; flash, mailbox and pins kept): boots with csr into
 * `after` once `before` has run. */
static void across_real_reset(void (*before)(void), uint32_t csr, void (*after)(void)) {
    host_across_real_reset(before, boot, csr, after);
}

/* A restore is saved where flash differs (power-on 'last': an off that came
 * within the save delay before the reset), and costs no write where it agrees. */
static void off_not_saved_yet(void) {
    power_up(1);
    host_write(POWER_ON_1, TUYA_DP_TYPE_ENUM, 2);  /* last state */
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);
    run_ms(3000);
    host_saved_t s;
    memcpy(&s, g_host_flash, sizeof(s));
    CHECK(s.on_mask == 1);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 0);      /* off; its save is not due yet */
    run_ms(1000);
}

static void restored_off_is_saved(void) {
    CHECK(c1()->on == 0);                          /* restored: off */
    run_ms(3000);
    host_saved_t s;
    memcpy(&s, g_host_flash, sizeof(s));
    CHECK(s.on_mask == 0);
    CHECK(g_host_saves == 1 && g_host_save_attempts == 1);   /* a main-loop save: one page */
    power_up(0);                                   /* a power cut: still off */
    CHECK(c1()->on == 0);
}

static void lit_power_on_off(void) {
    power_up(1);                                   /* power-on 'off': nothing to save */
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);
    run_ms(3000);
}

static void restored_lit_no_write(void) {
    CHECK(c1()->on == 1);
    run_ms(3000);
    CHECK(g_host_saves == 0);
}

/* Each in its own test: across_real_reset needs a process nothing ran in yet. */
static void test_restore_is_saved(void) {
    across_real_reset(off_not_saved_yet, CSR_WATCHDOG, restored_off_is_saved);
}

static void test_restore_agreeing_with_flash_is_not_saved(void) {
    across_real_reset(lit_power_on_off, CSR_WATCHDOG, restored_lit_no_write);
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

/* ---- the hold's way is learnt per level band ------------------------------------------- */

/* Lit at a level of band 0 (min 200, max 1000: 200..399), not in a correction sequence. */
static void lit_again(uint16_t level) {
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, level);
    run_ms(3000);
}

static int8_t hold_for(uint32_t ms) {
    dimmer_hold_begin(0, 0, 0);
    int8_t dir = g_host_fader[0].hold_dir;
    run_ms(ms);
    dimmer_hold_end(0);
    return dir;
}

static void test_hold_learns_the_way(void) {
    power_up(1);
    lit_again(300);
    CHECK(hold_for(500) == 1);                     /* band 0: the far end, up */
    run_ms(300);
    CHECK(hold_for(500) == -1);                    /* turned round at once: the other way */
    lit_again(300);
    CHECK(hold_for(500) == -1);                    /* learnt: band 0 goes down first now */
    lit_again(800);
    CHECK(hold_for(500) == -1);                    /* band 2 untouched: the far end, down */
}

static void test_a_long_hold_corrected_teaches_nothing(void) {
    power_up(1);
    lit_again(300);
    CHECK(hold_for(1500) == 1);                    /* long: it went the right way, too far */
    run_ms(300);
    CHECK(hold_for(300) == -1);
    lit_again(300);
    CHECK(hold_for(500) == 1);                     /* still up */
}

static void test_a_confirmed_band_needs_two_misses(void) {
    power_up(1);
    lit_again(300);
    CHECK(hold_for(500) == 1);
    lit_again(300);                                   /* kept: confirmed */
    CHECK(hold_for(500) == 1);
    run_ms(300);
    CHECK(hold_for(300) == -1);                    /* one miss */
    lit_again(300);
    CHECK(hold_for(500) == 1);                     /* not enough */
    run_ms(300);
    CHECK(hold_for(300) == -1);                    /* a second miss */
    lit_again(300);
    CHECK(hold_for(500) == -1);                    /* turned */
}

static void test_no_correction_across_an_off(void) {
    power_up(1);
    lit_again(300);
    CHECK(hold_for(500) == 1);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 0);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 300);
    run_ms(300);
    CHECK(hold_for(500) == 1);                     /* a new start: learnt way, not the other one */
    lit_again(300);
    CHECK(hold_for(500) == 1);                     /* and nothing learnt */
}

static void test_a_hold_to_the_end_was_the_right_way(void) {
    power_up(1);
    lit_again(250);
    CHECK(hold_for(500) == 1);
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 1000);   /* as if the hold had reached the max */
    run_ms(3000);
    dimmer_hold_begin(0, -1, 0);                       /* any hold settles the verdict */
    dimmer_hold_end(0);
    lit_again(250);
    CHECK(hold_for(500) == 1);
}

static void test_the_learnt_way_is_saved(void) {
    power_up(1);
    host_write(POWER_ON_1, TUYA_DP_TYPE_ENUM, 2);       /* power-on: last state */
    lit_again(300);
    CHECK(hold_for(500) == 1);
    run_ms(300);
    CHECK(hold_for(500) == -1);
    run_ms(10000);                                     /* saved */
    host_saved_t s;
    memcpy(&s, g_host_flash, sizeof(s));
    CHECK(s.format == 3);
    CHECK((s.on_mask & 1u) == 1u);                     /* channel 1 on, kept as before */
    CHECK(((s.on_mask >> 2) & 1u) == 1u);              /* band 0 of channel 1 learnt */
    CHECK((s.reserved[0] & 0xC0u) == 0);
    power_up(0);
    CHECK(c1()->on == 1);
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 300);
    run_ms(3000);
    CHECK(hold_for(500) == -1);                        /* still learnt after the power cut */
}

static void test_an_older_record_has_the_far_end_ways(void) {
    host_saved_t s;
    memset(&s, 0, sizeof(s));
    s.format = 3;
    for (int i = 0; i < 2; i++) {
        s.ch[i].level = 300;
        s.ch[i].min   = 200;
        s.ch[i].max   = 1000;
    }
    s.reserved[0] = 0x3Fu;                             /* the spare bits clear */
    memcpy(g_host_flash, &s, sizeof(s));
    g_host_flash_valid = 1;
    power_up(0);
    lit_again(300);
    CHECK(hold_for(500) == 1);
    lit_again(800);
    CHECK(hold_for(500) == -1);
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

/* ---- the link watchdog (docs/BOOTLOADER.md section 9, item 11) ----------------- */

/* Dark: both channels off and the output at 0, also once a fade out has ended. */
static void test_dimmer_dark(void) {
    power_up(1);
    CHECK(dimmer_dark());
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);
    CHECK(!dimmer_dark());
    run_ms(3000);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 0);      /* off: the fade out runs */
    CHECK(!dimmer_dark());
    run_ms(20);
    CHECK(!dimmer_dark() && g_host_fader[0].current > 0);
    run_ms(5000);
    CHECK(dimmer_dark() && g_host_fader[0].current == 0);
    host_write(8, TUYA_DP_TYPE_VALUE, 300);        /* a level while off: still dark */
    CHECK(dimmer_dark());
    host_write(7, TUYA_DP_TYPE_BOOL, 1);           /* channel 2 */
    CHECK(!dimmer_dark());
}

/* A v2 bootloader's word after a power-on probe the ZTU answered after 40 ms: tries 1. */
#define LINK_WORD (BOOT_WORD_FRESH | (40u << BOOT_PROBE_MS_SHIFT) | 1u)

static void v2_power_up(void) {
    power_up(1);
    host_boot_v2(BOOT_FEATURES_V2, BOOT_APP_MAX);
    host_boot_word(LINK_WORD);
    bootif_init();
}

/* The main loop over a long run, in 10 ms passes (one half cycle each). */
static void loop_until(uint32_t until) {
    while (T < until && !g_resets) {
        T += 10;
        host_halfcycle();
        dimmer_poll(T, 1000, 1);
        bootif_poll(T);
    }
}

/* Lit at 700 / 400 with power-on 'on' (the lamp must not switch on by itself), then off from
 * the network, and no frame from the module for 4 h: the restart, once dark. */
static void link_dead_while_off(void) {
    v2_power_up();
    host_write(DP_POWER_ON, TUYA_DP_TYPE_ENUM, 1);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 700);
    host_write(7, TUYA_DP_TYPE_BOOL, 1);
    host_write(8, TUYA_DP_TYPE_VALUE, 400);
    run_ms(3000);
    loop_until(3u * 3600000u);
    CHECK(g_resets == 0);                          /* lit: never */
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 0);
    host_write(7, TUYA_DP_TYPE_BOOL, 0);
    loop_until(LINK_WDG_UPTIME_MS - 10u);
    CHECK(g_resets == 0);                          /* dark, but the run is not 4 h old */
    loop_until(LINK_WDG_UPTIME_MS + 1000u);
    CHECK(g_resets == 1 && T == LINK_WDG_UPTIME_MS);
    CHECK(g_shutdowns == 1 && g_word_at_shutdown == LINK_WORD);   /* gates first, word after */
    CHECK(g_word_at_reset == ((LINK_WORD & ~BOOT_TRIES_MASK) | BOOT_PROBE_REQ) &&
          g_check_at_reset == ~g_word_at_reset);
    CHECK(g_word_at_reset == (BOOT_WORD_FRESH | (40u << BOOT_PROBE_MS_SHIFT) | 0x80u));
    CHECK(g_restored_at_reset == 0);               /* a planned restart: restored after it */
    CHECK(g_host_mailbox.magic == 0xA5A5A5A5u);    /* the update magic is not touched */
    CHECK(mb_valid() && !mb_on(0) && !mb_on(1) && mb_level(0) == 700 && mb_level(1) == 400);
    /* The v2 bootloader: bit 7 read and cleared, the link probe (nothing newer), tries + 1. */
    uint32_t w = (g_word_at_reset & ~(BOOT_PROBE_REQ | BOOT_PROBE_MS_MASK)) | BOOT_INFO_LINK |
                 (9u << BOOT_PROBE_MS_SHIFT);
    host_boot_word(w + 1u);
}

/* After it (only SFTRSTF set, as after the link probe, also after an image it installed): the
 * state comes back, all off with the levels kept, not the power-on 'on'. */
static void back_off_after_link_restart(void) {
    CHECK(c1()->on == 0 && dimmer_channel(1)->on == 0);
    CHECK(c1()->level == 700 && dimmer_channel(1)->level == 400);
    CHECK(g_host_fader[0].target == 0 && g_host_fader[1].target == 0);
    CHECK(g_host_mailbox.restored == OTA_RESTORED_MAGIC);
    CHECK((host_boot_word_now() & BOOT_TRIES_MASK) == 1u);   /* the counter did not move */
    run_ms(A_MINUTE_MS);
    power_up(0);                                   /* only a power cut takes power-on 'on' */
    CHECK(c1()->on == 1 && dimmer_channel(1)->on == 1);
}

static void test_link_restart_restores_off(void) {
    across_real_reset(link_dead_while_off, CSR_SOFTWARE, back_off_after_link_restart);
}

/* Not while a fade out runs nor in the minute after it, nor while an input is pressed, nor after
 * frames came. */
static void test_link_restart_waits(void) {
    v2_power_up();
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);
    run_ms(3000);
    loop_until(LINK_WDG_UPTIME_MS + 600000u);
    CHECK(g_resets == 0);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 0);      /* the fade out takes a while */
    uint32_t off_at = T;
    loop_until(T + 10u);
    CHECK(g_resets == 0 && g_host_fader[0].current > 0);
    loop_until(off_at + LINK_WDG_QUIET_MS);        /* dark, but not a minute yet */
    CHECK(g_resets == 0 && g_host_fader[0].current == 0);
    loop_until(T + 60000u);
    CHECK(g_resets == 1 && g_host_fader[0].current == 0 && T > off_at + LINK_WDG_QUIET_MS);

    power_up(1);                                   /* a new run */
    g_resets = g_shutdowns = 0;
    host_boot_word(LINK_WORD);
    g_inputs_idle = 0;                             /* a switch held */
    T             = LINK_WDG_UPTIME_MS;
    loop_until(T + 3600000u);
    CHECK(g_resets == 0);
    g_inputs_idle = 1;
    g_tuya_frames++;                               /* a frame: 30 min of silence again */
    uint32_t heard = T;
    loop_until(T + LINK_WDG_SILENCE_MS + 1000u);
    CHECK(g_resets == 1 && T == heard + LINK_WDG_SILENCE_MS + 10u);
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


/* ---- transition time: DP 160 / 161 (a Zigbee transition time from the module) ------ */

#define CH1_TRANSITION 160
#define CH1_INPUT_MODE_DP 162

/* ms until channel 1's light is at v (10 ms half cycles), 0 if not within max ms. */
static uint32_t ms_to(uint16_t v, uint32_t max) {
    for (uint32_t ms = 10; ms <= max; ms += 10) {
        run_ms(10);
        if (g_host_fader[0].current == v) {
            return ms;
        }
    }
    return 0;
}

static void lit_at(uint16_t level) {
    power_up(1);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, level);
    run_ms(3000);
    CHECK(g_host_fader[0].current == level);
}

static void test_transition_level_takes_the_time(void) {
    lit_at(200);
    host_log_clear();
    host_write(CH1_TRANSITION, TUYA_DP_TYPE_VALUE, 2000);
    CHECK(host_log_find("P[160=2000] ") == 0);           /* the ack: the value back */
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);            /* as the module sends it: on, level */
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 800);
    uint32_t t = ms_to(800, 5000);
    CHECK(t >= 1980 && t <= 2020);
    CHECK(c1()->level == 800);
}

static void test_transition_on_from_dark_and_off(void) {
    lit_at(700);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 0);
    run_ms(3000);
    CHECK(g_host_fader[0].current == 0);
    host_write(CH1_TRANSITION, TUYA_DP_TYPE_VALUE, 3000);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);            /* back to 700 in 3 s */
    uint32_t t = ms_to(700, 6000);
    CHECK(t >= 2950 && t <= 3030);
    run_ms(1000);
    host_write(CH1_TRANSITION, TUYA_DP_TYPE_VALUE, 1500);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 0);            /* dark in 1.5 s, the moment at the min in it */
    t = ms_to(0, 4000);
    CHECK(t >= 1490 && t <= 1520);
}

static void test_transition_is_for_the_next_second_only(void) {
    lit_at(200);
    host_write(CH1_TRANSITION, TUYA_DP_TYPE_VALUE, 4000);
    run_ms(1100);                                        /* too late for it */
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 800);
    CHECK(g_host_fader[0].timed_left == 0);
    uint32_t t = ms_to(800, 5000);
    CHECK(t > 0 && t <= 1300);                           /* the fade settings: 600 steps at 5 per 10 ms */
    host_write(CH1_TRANSITION, TUYA_DP_TYPE_VALUE, 0);   /* 0: the fade settings */
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 300);
    CHECK(g_host_fader[0].timed_left == 0);
}

static void test_transition_is_not_for_the_wall_switch(void) {
    lit_at(700);
    host_write(CH1_TRANSITION, TUYA_DP_TYPE_VALUE, 45000);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);            /* the network: slowly to 600 */
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 600);
    CHECK(g_host_fader[0].timed_left != 0);
    run_ms(300);
    dimmer_local_set(0, 0);                              /* the wall switch in that second: off */
    CHECK(g_host_fader[0].timed_left == 0);
    uint32_t t = ms_to(0, 5000);
    CHECK(t > 0 && t <= 3000);                           /* at the fade settings, not in 45 s */

    host_write(CH1_TRANSITION, TUYA_DP_TYPE_VALUE, 45000);
    run_ms(200);
    dimmer_local_set(0, 1);                              /* a tap on from dark in that second ... */
    dimmer_hold_begin(0, +1, 1);
    run_ms(50);
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 500);      /* ... the module's on level on top of it */
    run_ms(50);
    dimmer_hold_cancel(0);
    CHECK(g_host_fader[0].timed_left == 0);
    t = ms_to(500, 5000);
    CHECK(t > 0 && t <= 3000);

    host_write(CH1_TRANSITION, TUYA_DP_TYPE_VALUE, 45000);
    run_ms(200);
    dimmer_local_set(0, 0);                              /* off at the wall and on again, both in */
    dimmer_local_set(0, 1);                              /* that second ...                       */
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 300);      /* ... the module's on level: no time either */
    CHECK(g_host_fader[0].timed_left == 0);
    t = ms_to(300, 5000);
    CHECK(t > 0 && t <= 3000);
}

static void test_transition_is_neither_saved_nor_in_the_dump(void) {
    lit_at(500);
    run_ms(3000);
    host_saved_t before;
    memcpy(&before, g_host_flash, sizeof(before));
    host_write(CH1_TRANSITION, TUYA_DP_TYPE_VALUE, 2500);
    host_write(CH1_TRANSITION + 1, TUYA_DP_TYPE_VALUE, 70000);   /* channel 2, capped */
    CHECK(host_log_find("P[161=65535] ") >= 0);
    run_ms(5000);
    CHECK(memcmp(&before, g_host_flash, sizeof(before)) == 0);
    host_log_clear();
    dimmer_report_all();
    CHECK(host_log_find("160=") < 0 && host_log_find("161=") < 0);
    CHECK(!dimmer_write_dp(CH1_TRANSITION, TUYA_DP_TYPE_BOOL, (const uint8_t *)"\1", 1));   /* a value DP */
}


/* ---- 2.2.0: countdown, power-cut level, identify, start boost, the spare bytes ---------- */

#define CH1_COUNTDOWN    6
#define CH1_POWER_LEVEL  164
#define CH1_IDENTIFY     166
#define CH1_BOOST        168
#define CH2_BOOST        169

static void test_countdown_turns_off_after_its_time(void) {
    lit_at(500);
    host_log_clear();
    host_write(CH1_COUNTDOWN, TUYA_DP_TYPE_VALUE, 5);
    CHECK(host_log_find("P[6=5] ") >= 0);                 /* the ack */
    run_ms(4900);
    CHECK(c1()->on == 1);
    run_ms(200);
    CHECK(c1()->on == 0 && c1()->countdown == 0);          /* off, as by the wall switch */
    CHECK(host_log_find("A[1=0,6=0]") >= 0);              /* with the countdown's end */
    host_log_clear();
    host_write(CH1_COUNTDOWN, TUYA_DP_TYPE_VALUE, 60);    /* while off: nothing to count */
    CHECK(host_log_find("P[6=0] ") >= 0 && c1()->countdown == 0);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);
    host_write(CH1_COUNTDOWN, TUYA_DP_TYPE_VALUE, 100000);   /* capped at a day */
    CHECK(c1()->countdown == 86400);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 0);              /* an off cancels it */
    CHECK(c1()->countdown == 0);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);
    run_ms(100000);
    CHECK(c1()->on == 1);                                  /* not taken up again */
    host_write(CH1_COUNTDOWN, TUYA_DP_TYPE_VALUE, 600);
    host_log_clear();
    dimmer_local_set(0, 0);                                /* a wall switch off ends it */
    CHECK(host_log_find("A[1=0,6=0]") >= 0 && c1()->countdown == 0);
    dimmer_local_set(0, 1);
    host_log_clear();
    dimmer_local_set(0, 0);                                /* none running: the off alone */
    CHECK(host_log_find("A[1=0]") >= 0);
}

static void test_no_start_boost_after_a_warm_reset(void) {
    lit_at(400);
    host_write(CH1_BOOST, TUYA_DP_TYPE_VALUE, 300);
    host_write(POWER_ON_1, TUYA_DP_TYPE_ENUM, 1);          /* on after a power cut */
    run_ms(3000);
    boot(CSR_WATCHDOG);                                    /* the light comes back ... */
    unsigned full = 0;
    for (int i = 0; i < 400; i++) {
        run_ms(1);
        full += g_host_fader[0].current == 1000;
    }
    CHECK(c1()->on == 1 && full == 0);                     /* ... with no flash at full */
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 0);
    run_ms(3000);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);              /* later from dark: the boost */
    run_ms(10);
    CHECK(g_host_fader[0].current == 1000);
    run_ms(3000);
    power_up(0);                                           /* a power cut: from dark, boosted */
    run_ms(10);
    CHECK(c1()->on == 1 && g_host_fader[0].current == 1000);
}

static void test_power_cut_level_and_the_spare_bytes(void) {
    lit_at(500);
    host_write(POWER_ON_1, TUYA_DP_TYPE_ENUM, 1);          /* on after a power cut */
    host_write(CH1_POWER_LEVEL, TUYA_DP_TYPE_VALUE, 301);  /* kept in steps of 4 */
    host_write(CH1_INPUT_MODE_DP, TUYA_DP_TYPE_VALUE, 1);
    host_write(CH2_BOOST, TUYA_DP_TYPE_VALUE, 230);        /* 50 ms steps: 200 */
    host_log_clear();
    dimmer_report_dp(CH1_POWER_LEVEL);
    dimmer_report_dp(CH2_BOOST);
    run_ms(3000);                                          /* saved */
    host_saved_t s;
    memcpy(&s, g_host_flash, sizeof(s));
    CHECK(s.reserved[0] == 0x01 && s.reserved[1] == 0x40 && s.reserved[2] == 75 &&
          s.reserved[3] == 0);
    power_up(0);                                           /* a power cut */
    CHECK(c1()->on == 1 && c1()->level == 300);            /* at the power-cut level */
    CHECK(c1()->input_mode == 1 && dimmer_channel(1)->boost50 == 4);
    host_write(CH1_LEVEL, TUYA_DP_TYPE_VALUE, 700);
    run_ms(3000);
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);
    boot(CSR_WATCHDOG);                                    /* a warm reset restores, as before */
    CHECK(c1()->level == 700);
    host_write(CH1_POWER_LEVEL, TUYA_DP_TYPE_VALUE, 0);    /* 0: the last level again */
    run_ms(3000);
    power_up(0);
    CHECK(c1()->level == 700);
    /* An older record (spare bytes 0): as before. */
    memcpy(&s, g_host_flash, sizeof(s));
    memset(s.reserved, 0, sizeof(s.reserved));
    memcpy(g_host_flash, &s, sizeof(s));
    power_up(0);
    CHECK(c1()->input_mode == 0 && c1()->power_level4 == 0 && dimmer_channel(1)->boost50 == 0);
}

static void test_identify_blinks_then_is_as_it_was(void) {
    lit_at(400);
    host_write(CH1_IDENTIFY, TUYA_DP_TYPE_VALUE, 3);
    unsigned dark = 0, full = 0;
    for (int i = 0; i < 280; i++) {                        /* 2.8 s */
        run_ms(10);
        dark += g_host_fader[0].current == 0;
        full += g_host_fader[0].current == 1000;
    }
    CHECK(dark > 100 && full > 100);                       /* at once both ways, 0.5 s each */
    run_ms(500);
    CHECK(g_host_fader[0].target == 400 && c1()->on == 1); /* as it was */
    host_saved_t s;
    memcpy(&s, g_host_flash, sizeof(s));
    CHECK(s.ch[0].level == 400);                           /* nothing of it saved */
    host_write(CH1_IDENTIFY, TUYA_DP_TYPE_VALUE, 60);
    run_ms(600);
    host_write(CH1_IDENTIFY, TUYA_DP_TYPE_VALUE, 0);       /* 0 stops it */
    run_ms(20);
    CHECK(g_host_fader[0].target == 400);
}

static void test_boost_dp_drives_the_fader_and_the_dump_has_the_new_dps(void) {
    power_up(1);
    host_write(CH1_BOOST, TUYA_DP_TYPE_VALUE, 150);
    CHECK(!dimmer_write_dp(CH1_BOOST, TUYA_DP_TYPE_VALUE, (const uint8_t *)"\0\0\x03\x00", 4));  /* 768 */
    host_write(CH1_ON, TUYA_DP_TYPE_BOOL, 1);
    run_ms(10);
    CHECK(g_host_fader[0].current == 1000);                /* at full first */
    run_ms(200);
    CHECK(g_host_fader[0].current == 1000 || g_host_fader[0].current == DIMMER_LEVEL_MAX);
    host_log_clear();
    dimmer_report_all();
    CHECK(host_log_find("162=0") >= 0 && host_log_find("164=0") >= 0 && host_log_find("168=150") >= 0);
    CHECK(host_log_find("163=") >= 0 && host_log_find("169=") >= 0);
    CHECK(host_log_find("166=") < 0);                      /* identify: not in the dump */
    CHECK(host_log_find("170=0") >= 0 && host_log_find("171=0") >= 0);   /* fast press */
    host_write(170, TUYA_DP_TYPE_VALUE, 1);
    host_write(171, TUYA_DP_TYPE_VALUE, 1);
    CHECK(!dimmer_write_dp(170, TUYA_DP_TYPE_VALUE, (const uint8_t *)"\0\0\0\x02", 4));
    run_ms(3000);
    host_saved_t s;
    memcpy(&s, g_host_flash, sizeof(s));
    CHECK((s.reserved[0] & 0x30) == 0x30);                 /* bits 4 / 5 of the spare byte */
    power_up(0);
    CHECK(c1()->fast_press == 1 && dimmer_channel(1)->fast_press == 1);
}

static void test_lamp_compensation_dp_is_saved_in_the_top_bits_of_the_level(void) {
    power_up(1);
    host_write(150, TUYA_DP_TYPE_VALUE, 1500);                       /* fade on ms */
    host_write(153, TUYA_DP_TYPE_VALUE, 6000);                       /* hold ms */
    host_write(172, TUYA_DP_TYPE_VALUE, 32);
    host_write(173, TUYA_DP_TYPE_VALUE, 250);
    CHECK(c1()->comp4 == 8 && dimmer_channel(1)->comp4 == 63);
    CHECK(g_host_comp4[0] == 8 && g_host_comp4[1] == 63);           /* to the phase */
    CHECK(!dimmer_write_dp(172, TUYA_DP_TYPE_VALUE, (const uint8_t *)"\0\0\0\xFD", 4));   /* 253 */
    CHECK(c1()->comp4 == 8);
    host_log_clear();
    dimmer_report_all();
    CHECK(host_log_find("172=32") >= 0 && host_log_find("173=252") >= 0);
    run_ms(3000);
    host_saved_t s;
    memcpy(&s, g_host_flash, sizeof(s));
    CHECK(s.pf[0].fade_on_ms == 1501 && s.pf[0].hold_ms == 6001);    /* the times untouched */
    CHECK((s.ch[0].level >> 10) == 8 && (s.ch[1].level >> 10) == 63);
    CHECK((s.ch[0].level & 0x3FF) == c1()->level);
    power_up(0);
    CHECK(c1()->comp4 == 8 && dimmer_channel(1)->comp4 == 63);
    CHECK(c1()->fade_on_ms == 1500 && c1()->hold_ms == 6000);       /* the times as they were */
    CHECK(c1()->level == (s.ch[0].level & 0x3FF));                  /* the level without the bits */
    CHECK(g_host_comp4[0] == 8);
    /* a record of an older firmware (no top bits): none, times kept */
    memcpy(&s, g_host_flash, sizeof(s));
    host_write(172, TUYA_DP_TYPE_VALUE, 0);
    host_write(173, TUYA_DP_TYPE_VALUE, 0);
    run_ms(3000);
    power_up(0);
    CHECK(c1()->comp4 == 0 && dimmer_channel(1)->comp4 == 0 && c1()->fade_on_ms == 1500);
}

int main(void) {
    RUN(test_lamp_compensation_dp_is_saved_in_the_top_bits_of_the_level);
    RUN(test_power_up_takes_power_on_rule);
    RUN(test_watchdog_reset_restores_live_state);
    RUN(test_restore_once_in_a_row);
    RUN(test_update_restart_restores);
    RUN(test_reset_flags);
    RUN(test_restore_is_saved);
    RUN(test_restore_agreeing_with_flash_is_not_saved);
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
    RUN(test_hold_learns_the_way);
    RUN(test_a_long_hold_corrected_teaches_nothing);
    RUN(test_a_confirmed_band_needs_two_misses);
    RUN(test_no_correction_across_an_off);
    RUN(test_a_hold_to_the_end_was_the_right_way);
    RUN(test_the_learnt_way_is_saved);
    RUN(test_an_older_record_has_the_far_end_ways);
    RUN(test_max_below_level);
    RUN(test_load_clamps);
    RUN(test_transition_level_takes_the_time);
    RUN(test_transition_on_from_dark_and_off);
    RUN(test_transition_is_for_the_next_second_only);
    RUN(test_transition_is_not_for_the_wall_switch);
    RUN(test_transition_is_neither_saved_nor_in_the_dump);
    RUN(test_countdown_turns_off_after_its_time);
    RUN(test_no_start_boost_after_a_warm_reset);
    RUN(test_power_cut_level_and_the_spare_bytes);
    RUN(test_identify_blinks_then_is_as_it_was);
    RUN(test_boost_dp_drives_the_fader_and_the_dump_has_the_new_dps);
    RUN(test_dimmer_dark);
    RUN(test_link_restart_restores_off);
    RUN(test_link_restart_waits);
    if (g_fail) {
        printf("test_dimmer: FAILED\n");
        return 1;
    }
    printf("test_dimmer: all passed\n");
    return 0;
}
