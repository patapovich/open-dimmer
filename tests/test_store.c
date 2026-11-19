/* Host test of src/store.c (settings ring) on a fake 4 KB settings sector:
 * ring order, the version 1 records, and what a worn page costs: at most one
 * page erase per store_save() call, never an erase of the newest record (or
 * the one before it), and saves go on past bad pages. Run: make test */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "flash_drv.h"
#include "store.h"

#define PAGES       32
#define PAGE        128u

/* The linker symbols of the settings sector, on the fake flash. RING_HIGH_TEST: the layout from
 * release M (the ring in the upper half, the scan from SCAN_OFF, the sector start unless the app's
 * image reaches into the sector). */
uint8_t g_flash[PAGES * PAGE] __attribute__((aligned(4096)));
#ifndef RING_HIGH_TEST
__asm__(".globl _settings_start\n.set _settings_start, g_flash\n"
        ".globl _settings_end\n.set _settings_end, g_flash + 4096\n"
        ".globl _store_scan_start\n.set _store_scan_start, g_flash\n");
#define RING_OFF 0u
#else
#ifndef SCAN_OFF
#define SCAN_OFF 0
#endif
#define STR2(x) #x
#define STR(x) STR2(x)
__asm__(".globl _settings_start\n.set _settings_start, g_flash + 2048\n"
        ".globl _settings_end\n.set _settings_end, g_flash + 4096\n"
        ".globl _store_scan_start\n.set _store_scan_start, g_flash + " STR(SCAN_OFF) "\n");
#define RING_OFF 2048u
#endif

static int      g_failures;
static unsigned g_erases[PAGES];
static unsigned g_calls_erases;      /* erases since the last reset of the count */
static uint8_t  g_worn[PAGES];       /* 1: the page never verifies */
static uint8_t  g_stuck_pad[PAGES];  /* 1: verify fails, but the record bytes take */

#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            g_failures++;                                                \
        }                                                                \
    } while (0)

uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t n) {
    uint32_t c = ~crc;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

/* As src/flash_drv.c: an identical page is left alone, else erase + program
 * + read back. store.c hands over the address as 32 bits. */
static int below_ring(uint32_t off) {
    uint32_t ring = RING_OFF;               /* a variable: no "always false" for the 0 of RING_OFF */
    return off < ring;
}

uint8_t flash_write_page(uint32_t addr, const uint32_t *src) {
    uint32_t off = addr - (uint32_t)(uintptr_t)g_flash;
    if (below_ring(off) || off >= sizeof(g_flash) || off % PAGE) {
        printf("FAIL: write outside the settings sector (offset %u)\n", off);
        g_failures++;
        return 0;
    }
    uint8_t *p = g_flash + off;
    unsigned pg = off / PAGE;
    if (memcmp(p, src, PAGE) == 0) {
        return 1;
    }
    g_erases[pg]++;
    g_calls_erases++;
    memset(p, 0xFF, PAGE);
    if (g_worn[pg]) {
        memset(p, 0x00, 16);          /* half programmed: no valid record */
        return 0;
    }
    memcpy(p, src, PAGE);
    if (g_stuck_pad[pg]) {
        p[PAGE - 1] = 0x7F;           /* a padding bit stays: the record itself is valid */
        return 0;
    }
    return 1;
}

/* A fresh sector, loaded as at boot. */
static void flash_reset(void) {
    uint8_t d[STORE_DATA_SIZE];
    memset(g_flash, 0xFF, sizeof(g_flash));
    memset(g_erases, 0, sizeof(g_erases));
    memset(g_worn, 0, sizeof(g_worn));
    memset(g_stuck_pad, 0, sizeof(g_stuck_pad));
    store_load(d);
}

static uint32_t page_seq(unsigned pg) {
    uint32_t s;
    memcpy(&s, g_flash + pg * PAGE + 4, 4);
    return s;
}

/* The newest valid record as the reference reads it (without store_load(),
 * which would restart the store's own state): its page, -1 if none; data
 * copied to d (version 2 records only). */
static int ref_newest(uint8_t *d) {
    int best = -1;
    for (unsigned pg = 0; pg < PAGES; pg++) {
        const uint8_t *p = g_flash + pg * PAGE;
        uint32_t magic, crc;
        memcpy(&magic, p, 4);
        memcpy(&crc, p + 8 + STORE_DATA_SIZE, 4);
        if (magic != 0x5A445332u || crc != crc32_update(0, p, 8 + STORE_DATA_SIZE)) {
            continue;
        }
        if (best < 0 || (int32_t)(page_seq(pg) - page_seq((unsigned)best)) > 0) {
            best = (int)pg;
        }
    }
    if (best >= 0 && d) {
        memcpy(d, g_flash + (unsigned)best * PAGE + 8, STORE_DATA_SIZE);
    }
    return best;
}

static int newest_page(void) {
    return ref_newest(NULL);
}

/* The newest record holds d. */
static int newest_is(const uint8_t *d) {
    uint8_t o[STORE_DATA_SIZE];
    return ref_newest(o) >= 0 && memcmp(o, d, STORE_DATA_SIZE) == 0;
}

static void data_of(uint8_t *d, unsigned n) {
    for (unsigned i = 0; i < STORE_DATA_SIZE; i++) {
        d[i] = (uint8_t)(n * 7u + i);
    }
}

static void test_empty_and_ring_order(void) {
    uint8_t d[STORE_DATA_SIZE], o[STORE_DATA_SIZE];
    flash_reset();
    CHECK(store_load(o) == 0);
    for (unsigned n = 0; n < 40; n++) {          /* round the ring once and more */
        data_of(d, n);
        g_calls_erases = 0;
        CHECK(store_save(d) == 1);
        CHECK(g_calls_erases == 1);
        CHECK(g_erases[n % PAGES] >= 1);        /* page by page from page 0 */
    }
    CHECK(store_save_count() >= 40);
    CHECK(store_load(o) == 1);
    data_of(d, 39);
    CHECK(memcmp(o, d, STORE_DATA_SIZE) == 0);
    CHECK(newest_page() == 39 % PAGES);
}

/* The sequence number decides, also across its 32-bit wrap. */
static void test_newest_by_sequence_across_the_wrap(void) {
    uint8_t d[STORE_DATA_SIZE], o[STORE_DATA_SIZE];
    flash_reset();
    struct {
        uint32_t magic, seq;
        uint8_t  data[STORE_DATA_SIZE];
        uint32_t crc;
    } r;
    const uint32_t seqs[3] = { 0xFFFFFFFEu, 0xFFFFFFFFu, 0x00000001u };
    const unsigned pages[3] = { 20, 3, 9 };
    for (int i = 0; i < 3; i++) {
        memset(&r, 0, sizeof(r));
        r.magic = 0x5A445332u;
        r.seq   = seqs[i];
        data_of(r.data, (unsigned)i + 100);
        r.crc = crc32_update(0, (const uint8_t *)&r, 8 + STORE_DATA_SIZE);
        memcpy(g_flash + pages[i] * PAGE, &r, sizeof(r));
    }
    CHECK(store_load(o) == 1);
    data_of(d, 102);
    CHECK(memcmp(o, d, STORE_DATA_SIZE) == 0);
    data_of(d, 7);
    CHECK(store_save(d) == 1);
    CHECK(g_erases[10] == 1);                   /* after the newest (page 9) */
    CHECK(page_seq(10) == 2);
}

/* A version 1 record (20 bytes of data) is read, the rest is 0; the next
 * save writes version 2 after it. */
static void test_version_1_record(void) {
    uint8_t o[STORE_DATA_SIZE], d[STORE_DATA_SIZE];
    flash_reset();
    struct {
        uint32_t magic, seq;
        uint8_t  data[20];
        uint32_t crc;
    } v1 = { 0x5A445331u, 77, { 0 }, 0 };
    for (int i = 0; i < 20; i++) {
        v1.data[i] = (uint8_t)(i + 1);
    }
    v1.crc = crc32_update(0, (const uint8_t *)&v1, 28);
    memcpy(g_flash + 5 * PAGE, &v1, sizeof(v1));
    memset(o, 0xEE, sizeof(o));
    CHECK(store_load(o) == 1);
    CHECK(o[0] == 1 && o[19] == 20 && o[20] == 0 && o[STORE_DATA_SIZE - 1] == 0);
    data_of(d, 3);
    CHECK(store_save(d) == 1);
    CHECK(page_seq(6) == 78);
    CHECK(store_load(o) == 1 && memcmp(o, d, STORE_DATA_SIZE) == 0);
}

/* One worn page after the newest record: one erase in the failing call, the
 * next call goes past it, and the newest record stays readable meanwhile. */
static void test_worn_page_one_erase_per_call(void) {
    uint8_t d[STORE_DATA_SIZE];
    flash_reset();
    for (unsigned n = 0; n < 8; n++) {
        data_of(d, n);
        store_save(d);
    }
    CHECK(newest_page() == 7);
    g_worn[8] = 1;
    data_of(d, 50);
    g_calls_erases = 0;
    CHECK(store_save(d) == 0);
    CHECK(g_calls_erases == 1);                 /* not two pages back to back */
    data_of(d, 7);
    CHECK(newest_is(d));                        /* the newest record survived */
    data_of(d, 50);
    g_calls_erases = 0;
    CHECK(store_save(d) == 1);
    CHECK(g_calls_erases == 1);
    CHECK(g_erases[8] == 1 && g_erases[9] == 1);
    CHECK(newest_page() == 9);
    CHECK(newest_is(d));
    /* The skip is over: the next save goes right after the new newest. */
    data_of(d, 51);
    CHECK(store_save(d) == 1);
    CHECK(newest_page() == 10);
}

/* Two worn pages in a row no longer block saves for good. */
static void test_two_worn_pages_are_passed(void) {
    uint8_t d[STORE_DATA_SIZE], o[STORE_DATA_SIZE];
    flash_reset();
    data_of(d, 1);
    CHECK(store_save(d) == 1);                  /* page 0 */
    g_worn[1] = g_worn[2] = 1;
    data_of(d, 2);
    CHECK(store_save(d) == 0);
    CHECK(store_save(d) == 0);
    CHECK(store_save(d) == 1);
    CHECK(g_erases[1] == 1 && g_erases[2] == 1 && g_erases[3] == 1);
    CHECK(newest_is(d));
    CHECK(store_load(o) == 1 && memcmp(o, d, STORE_DATA_SIZE) == 0);
}

/* Every page fails: calls keep failing with one erase each, and none ever
 * erases the newest record or the one before it. */
static void test_all_pages_worn_never_erase_the_newest(void) {
    uint8_t d[STORE_DATA_SIZE], o[STORE_DATA_SIZE];
    flash_reset();
    for (unsigned n = 0; n < 12; n++) {
        data_of(d, n);
        store_save(d);
    }
    CHECK(newest_page() == 11);
    unsigned keep_erases[2] = { g_erases[10], g_erases[11] };
    for (unsigned pg = 0; pg < PAGES; pg++) {
        if (pg != 10 && pg != 11) {
            g_worn[pg] = 1;
        }
    }
    data_of(d, 99);
    for (unsigned n = 0; n < 3 * PAGES; n++) {
        g_calls_erases = 0;
        CHECK(store_save(d) == 0);
        CHECK(g_calls_erases == 1);
    }
    CHECK(g_erases[10] == keep_erases[0] && g_erases[11] == keep_erases[1]);
    data_of(d, 11);
    CHECK(newest_is(d));
    CHECK(store_load(o) == 1 && memcmp(o, d, STORE_DATA_SIZE) == 0);
    /* Each of the other 30 pages was tried in turn. */
    for (unsigned pg = 0; pg < PAGES; pg++) {
        if (pg != 10 && pg != 11) {
            CHECK(g_erases[pg] >= 3);
        }
    }
}

/* store_save_tries(d, 2) (before a restart, gates off): past one worn page
 * in a single call. */
static void test_two_attempts_pass_one_worn_page(void) {
    uint8_t d[STORE_DATA_SIZE];
    flash_reset();
    data_of(d, 1);
    CHECK(store_save(d) == 1);                  /* page 0 */
    g_worn[1] = 1;
    data_of(d, 2);
    g_calls_erases = 0;
    CHECK(store_save_tries(d, 2) == 1);
    CHECK(g_calls_erases == 2);
    CHECK(newest_page() == 2);
    CHECK(newest_is(d));
    g_worn[2] = g_worn[3] = g_worn[4] = 1;
    data_of(d, 3);
    g_calls_erases = 0;
    CHECK(store_save_tries(d, 2) == 0);         /* never more than asked for */
    CHECK(g_calls_erases == 2);
}

/* A page that failed its verify may still hold a valid record: a later save
 * must be newer than it, also when that save is the same call's retry. */
static void test_failed_page_with_a_valid_record_is_older(void) {
    uint8_t d[STORE_DATA_SIZE], o[STORE_DATA_SIZE];
    flash_reset();
    data_of(d, 1);
    CHECK(store_save(d) == 1);                  /* page 0 */
    g_stuck_pad[1] = 1;
    data_of(d, 2);
    CHECK(store_save(d) == 0);                  /* page 1: valid record, failed verify */
    data_of(d, 3);
    CHECK(store_save(d) == 1);                  /* page 2: other data */
    CHECK(newest_is(d));
    CHECK(store_load(o) == 1 && memcmp(o, d, STORE_DATA_SIZE) == 0);   /* also after a restart */
    CHECK((int32_t)(page_seq(2) - page_seq(1)) > 0);
}

#ifdef RING_HIGH_TEST
/* ---- the ring moved up (release M): records of the old ring below it ----------------- */

/* A record as an older app wrote it, at a page of the whole sector (version 2, or 1). */
static void put_record(unsigned pg, uint32_t seq, unsigned n, int version1) {
    uint8_t *p = g_flash + pg * PAGE;
    uint8_t  d[STORE_DATA_SIZE];
    data_of(d, n);
    memset(p, 0xFF, PAGE);
    uint32_t magic = version1 ? 0x5A445331u : 0x5A445332u;
    memcpy(p, &magic, 4);
    memcpy(p + 4, &seq, 4);
    memcpy(p + 8, d, version1 ? 20u : STORE_DATA_SIZE);
    uint32_t crc = crc32_update(0, p, 8 + (version1 ? 20u : STORE_DATA_SIZE));
    memcpy(p + 8 + (version1 ? 20u : STORE_DATA_SIZE), &crc, 4);
}

static void old_ring(void) {
    memset(g_flash, 0xFF, sizeof(g_flash));
    memset(g_erases, 0, sizeof(g_erases));
    memset(g_worn, 0, sizeof(g_worn));
    put_record(3, 41, 1, 0);
    put_record(4, 43, 2, 0);              /* the newest */
    put_record(5, 42, 3, 0);
    put_record(20, 12, 4, 0);             /* an old one up there too (an old ring's 20th page) */
}

static void test_records_below_are_found_and_copied_up_once(void) {
    uint8_t d[STORE_DATA_SIZE], want[STORE_DATA_SIZE], below[2048];
    old_ring();
    memcpy(below, g_flash, sizeof(below));
    CHECK(store_load(d));
    data_of(want, 2);
    CHECK(memcmp(d, want, sizeof(d)) == 0);          /* the newest of the whole sector */
    int pg = newest_page();
    CHECK(pg >= 16 && page_seq((unsigned)pg) == 44); /* its copy, up in the ring, seq + 1 */
    CHECK(store_in_own_ring());
    CHECK(memcmp(below, g_flash, sizeof(below)) == 0);   /* the lower half as it was */
    unsigned before = g_calls_erases;
    CHECK(store_load(d) && memcmp(d, want, sizeof(d)) == 0);
    CHECK(g_calls_erases == before);                 /* the next start copies nothing */
    for (unsigned i = 0; i < 40; i++) {              /* saves round the 16 pages, never below */
        data_of(d, 100 + i);
        CHECK(store_save(d));
    }
    CHECK(memcmp(below, g_flash, sizeof(below)) == 0);
    CHECK(store_load(want) && newest_is(d) && memcmp(want, d, sizeof(d)) == 0);
}

static void test_a_failed_copy_is_made_at_the_next_start(void) {
    uint8_t d[STORE_DATA_SIZE], want[STORE_DATA_SIZE];
    old_ring();
    for (unsigned pg = 16; pg < 32; pg++) {
        g_worn[pg] = 1;                              /* the copy cannot take (a power cut) */
    }
    CHECK(store_load(d));
    data_of(want, 2);
    CHECK(memcmp(d, want, sizeof(d)) == 0);          /* the settings are there all the same */
    CHECK(!store_in_own_ring());                     /* "ring moved" not reported */
    memset(g_worn, 0, sizeof(g_worn));
    CHECK(store_load(d) && memcmp(d, want, sizeof(d)) == 0);
    CHECK(store_in_own_ring() && newest_page() >= 16);
}

static void test_a_newer_record_up_there_is_taken_without_a_copy(void) {
    uint8_t d[STORE_DATA_SIZE], want[STORE_DATA_SIZE];
    old_ring();
    put_record(17, 50, 5, 0);                        /* this app saved already */
    unsigned before = g_calls_erases;
    CHECK(store_load(d));
    data_of(want, 5);
    CHECK(memcmp(d, want, sizeof(d)) == 0 && g_calls_erases == before && store_in_own_ring());
}

static void test_a_version_1_record_below_is_copied_as_version_2(void) {
    uint8_t d[STORE_DATA_SIZE];
    memset(g_flash, 0xFF, sizeof(g_flash));
    put_record(0, 7, 6, 1);
    CHECK(store_load(d));
    uint8_t want[STORE_DATA_SIZE];
    data_of(want, 6);
    memset(want + 20, 0, STORE_DATA_SIZE - 20);      /* the newer fields: 0 */
    CHECK(memcmp(d, want, sizeof(d)) == 0 && newest_is(want) && newest_page() >= 16);
}

#if SCAN_OFF
static void test_code_below_the_scan_start_is_never_a_record(void) {
    uint8_t d[STORE_DATA_SIZE], want[STORE_DATA_SIZE];
    memset(g_flash, 0xFF, sizeof(g_flash));
    put_record(0, 900, 7, 0);                        /* under the app's code: not settings */
    put_record(SCAN_OFF / PAGE, 30, 8, 0);
    CHECK(store_load(d));
    data_of(want, 8);
    CHECK(memcmp(d, want, sizeof(d)) == 0);
}
#endif

int main(void) {
#if SCAN_OFF
    test_code_below_the_scan_start_is_never_a_record();   /* the old ring's pages are code here */
#else
    test_records_below_are_found_and_copied_up_once();
    test_a_failed_copy_is_made_at_the_next_start();
    test_a_newer_record_up_there_is_taken_without_a_copy();
    test_a_version_1_record_below_is_copied_as_version_2();
#endif
    if (g_failures) {
        printf("test_store (ring high, scan +%d): %d FAILED\n", SCAN_OFF, g_failures);
        return 1;
    }
    printf("test_store (ring high, scan +%d): all passed\n", SCAN_OFF);
    return 0;
}
#else
int main(void) {
    test_empty_and_ring_order();
    test_newest_by_sequence_across_the_wrap();
    test_version_1_record();
    test_worn_page_one_erase_per_call();
    test_two_worn_pages_are_passed();
    test_all_pages_worn_never_erase_the_newest();
    test_two_attempts_pass_one_worn_page();
    test_failed_page_with_a_valid_record_is_older();
    if (g_failures) {
        printf("test_store: %d FAILED\n", g_failures);
        return 1;
    }
    printf("test_store: all passed\n");
    return 0;
}
#endif
