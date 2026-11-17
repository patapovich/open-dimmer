#include "store.h"
#include "flash_drv.h"
#include "py32f0xx.h"
#include <stddef.h>
#include <string.h>

#define PAGE_SIZE       128u
#define STORE_MAGIC     0x5A445331u   /* "ZDS1" */

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint8_t  data[STORE_DATA_SIZE];
    uint32_t crc;
} record_t;

_Static_assert(sizeof(record_t) <= PAGE_SIZE, "record must fit a page");

extern const uint8_t _settings_start[];
extern const uint8_t _settings_end[];

#define STORE_PAGES     ((uint32_t)(_settings_end - _settings_start) / PAGE_SIZE)

static int16_t  g_newest = -1;   /* page index of the newest valid record */
static uint32_t g_seq;
static uint16_t g_saves;

static uint32_t crc32(const uint8_t *p, uint32_t n) {
    return crc32_update(0, p, n);
}

static const record_t *page_record(uint32_t page) {
    return (const record_t *)(_settings_start + page * PAGE_SIZE);
}

static uint8_t record_valid(const record_t *r) {
    return r->magic == STORE_MAGIC &&
           r->crc == crc32((const uint8_t *)r, offsetof(record_t, crc));
}

uint8_t store_load(uint8_t *data) {
    g_newest = -1;
    for (uint32_t i = 0; i < STORE_PAGES; i++) {
        const record_t *r = page_record(i);
        if (!record_valid(r)) {
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
    memcpy(data, page_record((uint32_t)g_newest)->data, STORE_DATA_SIZE);
    return 1;
}

uint8_t store_save(const uint8_t *data) {
    static uint32_t page_buf[PAGE_SIZE / 4u];
    record_t       *r = (record_t *)page_buf;

    memset(page_buf, 0xFF, sizeof(page_buf));
    r->magic = STORE_MAGIC;
    r->seq   = g_seq + 1u;
    memcpy(r->data, data, STORE_DATA_SIZE);
    r->crc = crc32((const uint8_t *)r, offsetof(record_t, crc));

    /* Try the next page; if it does not verify (worn out), the one after. */
    uint32_t page = g_newest < 0 ? 0u : ((uint32_t)g_newest + 1u) % STORE_PAGES;
    for (uint8_t attempt = 0; attempt < 2; attempt++) {
        if (flash_write_page((uint32_t)(_settings_start + page * PAGE_SIZE), page_buf)) {
            g_newest = (int16_t)page;
            g_seq    = r->seq;
            g_saves++;
            return 1;
        }
        page = (page + 1u) % STORE_PAGES;
    }
    return 0;
}

uint16_t store_save_count(void) {
    return g_saves;
}
