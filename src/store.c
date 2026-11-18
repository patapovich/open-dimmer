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
    uint32_t crc;
} record_t;

_Static_assert(sizeof(record_t) <= PAGE_SIZE, "record must fit a page");
_Static_assert(STORE_DATA_SIZE >= STORE_V1_SIZE, "a version 1 record must fit");

extern const uint8_t _settings_start[];
extern const uint8_t _settings_end[];

#define STORE_PAGES     ((uint32_t)(_settings_end - _settings_start) / PAGE_SIZE)

/* A save tries page g_newest + 1 + g_skip. g_skip counts the pages after the
 * newest record that did not verify and stays below STORE_PAGES - 2, so a save
 * never erases the newest record or the one before it. */
#define STORE_SKIP_MAX  (STORE_PAGES - 2u)

static int16_t  g_newest = -1;   /* page index of the newest valid record */
static uint32_t g_seq;           /* sequence number of the newest record or attempt */
static uint8_t  g_skip;
static uint32_t g_saves;

static uint32_t crc32(const uint8_t *p, uint32_t n) {
    return crc32_update(0, p, n);
}

static const record_t *page_record(uint32_t page) {
    return (const record_t *)(_settings_start + page * PAGE_SIZE);
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
    g_newest = -1;
    g_skip   = 0;
    for (uint32_t i = 0; i < STORE_PAGES; i++) {
        const record_t *r = page_record(i);
        if (!record_size(r)) {
            continue;
        }
        if (g_newest < 0 || (int32_t)(r->seq - g_seq) > 0) {
            g_newest = (int16_t)i;
            g_seq    = r->seq;
        }
    }
    if (g_newest < 0) {
        return 0;
    }
    const record_t *r = page_record((uint32_t)g_newest);
    memset(data, 0, STORE_DATA_SIZE);              /* fields newer than the record: 0 */
    memcpy(data, r->data, record_size(r));
    return 1;
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

        /* g_newest -1 (nothing saved yet): page g_skip */
        uint32_t page = ((uint32_t)g_newest + 1u + g_skip) % STORE_PAGES;
        if (flash_write_page((uint32_t)(uintptr_t)(_settings_start + page * PAGE_SIZE), page_buf)) {
            g_newest = (int16_t)page;
            g_skip   = 0;
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
