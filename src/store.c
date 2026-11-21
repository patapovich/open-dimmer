#include "store.h"
#include "flash_drv.h"
#include <stddef.h>
#include <string.h>

#define PAGE_SIZE       128u
#define STORE_MAGIC     0x5A445332u   /* "ZDS2": STORE_DATA_SIZE bytes of data */
#define STORE_MAGIC_V1  0x5A445331u   /* "ZDS1": the first 20 bytes only (still read) */
#define STORE_V1_SIZE   20u

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint8_t  data[STORE_DATA_SIZE];
    uint32_t crc;                    /* magic .. data: what every firmware reads */
    uint8_t  ext[STORE_EXT_SIZE];    /* 2.2.8 */
    uint32_t ext_crc;                /* magic .. ext */
} record_t;

_Static_assert(sizeof(record_t) <= PAGE_SIZE, "record must fit a page");
_Static_assert(STORE_DATA_SIZE >= STORE_V1_SIZE, "a version 1 record must fit");

extern const uint8_t _settings_start[];
extern const uint8_t _settings_end[];
/* The lowest page a record may lie in: the sector start, or above this app's own image when that
 * reaches into the sector (linker script). From release M (RING_HIGH) the ring is the upper half
 * and the records of the ring before it still lie below it: they are found and the newest is
 * copied up (store_load). Only the own ring is ever written. */
extern const uint8_t _store_scan_start[];

#define STORE_PAGES     ((uint32_t)(_settings_end - _settings_start) / PAGE_SIZE)
#define SCAN_PAGES      ((uint32_t)(_settings_end - _store_scan_start) / PAGE_SIZE)

/* A save tries page g_newest + 1 + g_skip. g_skip counts the pages after the
 * newest record that did not verify and stays below STORE_PAGES - 2, so a save
 * never erases the newest record or the one before it. */
#define STORE_SKIP_MAX  (STORE_PAGES - 2u)

static int16_t  g_newest = -1;   /* page index of the newest valid record */
static uint8_t  g_found;         /* store_load found a valid record anywhere in its scan */
static uint8_t  g_copy_due;      /* the newest record lies below the ring, not copied up yet */
static uint32_t g_seq;           /* sequence number of the newest record or attempt */
static uint8_t  g_skip;
static uint32_t g_saves;

static uint32_t crc32(const uint8_t *p, uint32_t n) {
    return crc32_update(0, p, n);
}

/* Data size of a valid record (version 2 or the older version 1), else 0. */
static uint32_t record_size(const record_t *r) {
    if (r->magic == STORE_MAGIC &&
        r->crc == crc32((const uint8_t *)r, offsetof(record_t, crc))) {
        return STORE_DATA_SIZE;
    }
    if (r->magic == STORE_MAGIC_V1) {
        uint32_t crc;
        memcpy(&crc, r->data + STORE_V1_SIZE, sizeof(crc));
        if (crc == crc32((const uint8_t *)r, offsetof(record_t, data) + STORE_V1_SIZE)) {
            return STORE_V1_SIZE;
        }
    }
    return 0;
}

uint8_t store_load(uint8_t *data) {
    const record_t *best = NULL;

    g_newest = -1;
    g_skip   = 0;
    for (uint32_t i = 0; i < SCAN_PAGES; i++) {
        const record_t *r = (const record_t *)(_store_scan_start + i * PAGE_SIZE);
        if (!record_size(r)) {
            continue;
        }
        if (best == NULL || (int32_t)(r->seq - g_seq) > 0) {
            best  = r;
            g_seq = r->seq;
        }
    }
    g_found = best != NULL;
    if (best == NULL) {
        return 0;
    }
    memset(data, 0, STORE_SAVE_SIZE);              /* fields newer than the record: 0 */
    memcpy(data, best->data, record_size(best));
    if (best->magic == STORE_MAGIC &&
        best->ext_crc == crc32((const uint8_t *)best, offsetof(record_t, ext_crc))) {
        memcpy(data + STORE_DATA_SIZE, best->ext, STORE_EXT_SIZE);
    }
    if ((const uint8_t *)best >= _settings_start) {
        g_newest = (int16_t)((uint32_t)((const uint8_t *)best - _settings_start) / PAGE_SIZE);
    } else {
        /* The newest record lies below this app's ring (the ring moved up): copied into the
         * ring at once, seq + 1 (newer than every old record), the old one left as it is. One
         * page attempt here, before the main loop refreshes the watchdog; if it fails the
         * caller saves again by its usual path (store_copy_due), and a power cut first leaves
         * the old record, so the next start copies again. */
        g_copy_due = !store_save_tries(data, 1);
    }
    return 1;
}

uint8_t store_copy_due(void) {
    return g_copy_due;
}

uint8_t store_in_own_ring(void) {
    /* The newest record in this app's ring, or no record at all (an erased flash, a fresh
     * install): either way an image up to the ring's start loses no settings (DP 137 bit 25). */
    return g_newest >= 0 || !g_found;
}

uint8_t store_save_tries(const uint8_t *data, uint8_t attempts) {
    static uint32_t page_buf[PAGE_SIZE / 4u];
    record_t       *r = (record_t *)page_buf;

    while (attempts--) {
        /* A new sequence number for every attempt: a page that failed its
         * verify may still hold a valid record, and that one must never look
         * newer than a later save. */
        memset(page_buf, 0xFF, sizeof(page_buf));
        r->magic = STORE_MAGIC;
        r->seq   = ++g_seq;
        memcpy(r->data, data, STORE_DATA_SIZE);
        r->crc = crc32((const uint8_t *)r, offsetof(record_t, crc));
        memcpy(r->ext, data + STORE_DATA_SIZE, STORE_EXT_SIZE);
        r->ext_crc = crc32((const uint8_t *)r, offsetof(record_t, ext_crc));

        /* g_newest -1 (nothing saved yet): page g_skip */
        uint32_t page = ((uint32_t)g_newest + 1u + g_skip) % STORE_PAGES;
        if (flash_write_page((uint32_t)(uintptr_t)(_settings_start + page * PAGE_SIZE), page_buf)) {
            g_newest   = (int16_t)page;
            g_skip     = 0;
            g_copy_due = 0;
            g_saves++;
            return 1;
        }
        if (++g_skip >= STORE_SKIP_MAX) {   /* worn out (or all of them): round again */
            g_skip = 0;
        }
    }
    return 0;
}

uint32_t store_save_count(void) {
    return g_saves;
}
