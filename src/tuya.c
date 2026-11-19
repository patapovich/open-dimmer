#include "tuya.h"
#include "bootif.h"
#include "dimmer.h"
#include "ota_proto.h"
#include "uart.h"
#include <string.h>

#define FRAME_HEADER       8      /* 55 AA ver seq:2 cmd len:2 */
#define FRAME_VERSION      0x02   /* Tuya Zigbee framing, both ways */
#define RX_MAX_DATA        96
#define RX_STALE_MS        50     /* bytes of one frame arrive back to back */
#define REPORT_MAX_DATA    64

static uint8_t  g_rx[FRAME_HEADER + RX_MAX_DATA + 1];
static uint16_t g_rx_len;
static uint32_t g_rx_last_ms;     /* last byte received, or our network query sent */
static uint16_t g_seq;
static uint16_t g_query_seq;      /* 0 until the module answered a query, then 1, 2, ... */
static uint8_t  g_network = 0xFF;   /* unknown until the module tells us */
static uint8_t  g_told_state;
uint32_t        g_tuya_frames;
uint8_t         g_tuya_confirm;

/* Debug: the last DP writes received, read over SWD (symbol g_write_log). */
#define WRITE_LOG_SIZE     16
typedef struct {
    uint32_t ms;
    uint8_t  dpid;
    uint8_t  type;
    uint16_t len;
    uint32_t value;
} write_log_t;
write_log_t     g_write_log[WRITE_LOG_SIZE];
uint8_t         g_write_log_next;
static uint32_t g_now_ms;

static void write_log_add(uint8_t dpid, uint8_t type, const uint8_t *v, uint16_t len) {
    write_log_t *e = &g_write_log[g_write_log_next];
    uint32_t     x = 0;
    for (uint16_t i = 0; i < len && i < 4; i++) {
        x = (x << 8) | v[i];
    }
    e->ms    = g_now_ms;
    e->dpid  = dpid;
    e->type  = type;
    e->len   = len;
    e->value = x;
    g_write_log_next = (uint8_t)((g_write_log_next + 1) % WRITE_LOG_SIZE);
}

static uint8_t  g_report[REPORT_MAX_DATA];
static uint16_t g_report_len;
static uint8_t  g_report_cmd = TUYA_CMD_REPORT;

void tuya_init(void) {
    g_rx_len = 0;
}

uint8_t tuya_network_status(void) {
    return g_network;
}

uint32_t tuya_rx_quiet_ms(uint32_t now_ms) {
    return g_rx_len ? 0 : now_ms - g_rx_last_ms;
}

void tuya_send(uint8_t cmd, uint16_t seq, const uint8_t *data, uint16_t len) {
    uint8_t head[FRAME_HEADER] = { 0x55, 0xAA, FRAME_VERSION, (uint8_t)(seq >> 8), (uint8_t)seq, cmd,
                                   (uint8_t)(len >> 8), (uint8_t)len };
    uint8_t sum = 0;

    for (uint8_t i = 0; i < FRAME_HEADER; i++) {
        sum += head[i];
        uart_putc(head[i]);
    }
    for (uint16_t i = 0; i < len; i++) {
        sum += data[i];
        uart_putc(data[i]);
    }
    uart_putc(sum);
    uart_flush();
}

void tuya_query_network(uint32_t now_ms) {
    tuya_send(TUYA_CMD_QUERY_NETWORK, g_query_seq, NULL, 0);
    /* The module takes a query with seq 0 for our restart (a new contact, and it asks who we
     * are): seq 0 until it has answered one, so a lost first query does not hide the restart.
     * Then 1, 2, ..., and after 65535 the count goes on at 1. */
    if (g_query_seq && !++g_query_seq) {
        g_query_seq = 1;
    }
    g_rx_last_ms = now_ms;   /* its answer is due: no save stall right now */
}

/* ---- report builder ------------------------------------------------------ */

void tuya_report_begin(uint8_t passive) {
    g_report_len = 0;
    g_report_cmd = passive ? TUYA_CMD_REPORT_PASSIVE : TUYA_CMD_REPORT;
}

static void report_unit(uint8_t dpid, uint8_t type, const uint8_t *value, uint8_t len) {
    if (g_report_len + 4 + len > REPORT_MAX_DATA) {
        tuya_report_send();   /* keeps the frame type */
    }
    g_report[g_report_len++] = dpid;
    g_report[g_report_len++] = type;
    g_report[g_report_len++] = 0;
    g_report[g_report_len++] = len;
    memcpy(&g_report[g_report_len], value, len);
    g_report_len = (uint16_t)(g_report_len + len);
}

void tuya_report_bool(uint8_t dpid, uint8_t value) {
    uint8_t v = value ? 1 : 0;
    report_unit(dpid, TUYA_DP_TYPE_BOOL, &v, 1);
}

void tuya_report_enum(uint8_t dpid, uint8_t value) {
    report_unit(dpid, TUYA_DP_TYPE_ENUM, &value, 1);
}

void tuya_report_value(uint8_t dpid, uint32_t value) {
    uint8_t v[4] = { (uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8),
                     (uint8_t)value };
    report_unit(dpid, TUYA_DP_TYPE_VALUE, v, 4);
}

void tuya_report_send(void) {
    if (g_report_len) {
        tuya_send(g_report_cmd, g_seq++, g_report, g_report_len);
        g_report_len = 0;
    }
}

/* ---- incoming frames ----------------------------------------------------- */

#if !(SIM_ZC || MEASURE || DIAG_REPORT)
__attribute__((noinline)) static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
#endif

static void handle_write(const uint8_t *data, uint16_t len) {
    tuya_report_begin(1);
    uint16_t i = 0;
    while (i + 4 <= len) {
        uint8_t  dpid = data[i];
        uint8_t  type = data[i + 1];
        uint16_t vlen = (uint16_t)((data[i + 2] << 8) | data[i + 3]);
        if (i + 4 + vlen > len) {
            break;
        }
        write_log_add(dpid, type, &data[i + 4], vlen);
        /* The module waits for a report of the DP it wrote: that is the ack.
         * A rejected value is answered with the value kept, so the module
         * neither waits for nothing nor keeps a value we did not take. */
        (void)dimmer_write_dp(dpid, type, &data[i + 4], vlen);
        dimmer_report_dp(dpid);
        uint8_t sibling;
        for (uint8_t n = 0; (sibling = dimmer_sibling_dp(dpid, n)) != 0; n++) {
            dimmer_report_dp(sibling);   /* min may have moved max and back; DP 14 both channels */
        }
        i = (uint16_t)(i + 4 + vlen);
    }
    tuya_report_send();
}

static void handle_frame(uint8_t cmd, uint16_t seq, const uint8_t *data, uint16_t len) {
    switch (cmd) {
    case TUYA_CMD_WRITE:
        handle_write(data, len);
        break;

    case TUYA_CMD_QUERY_ALL:
        dimmer_report_all();
        break;

    case OTA_CMD_IDENT: {
        /* v2: the build (2.1.x peers read bytes 0-6 only) and the bootloader's version. */
        uint8_t id[OTA_IDENT_LEN] = { 'Z', 'D', 'M', 'S', OTA_IDENT_KIND_APP, FW_VERSION,
                                      OTA_PROTOCOL, (uint8_t)(FW_IMAGE_BUILD >> 8),
                                      (uint8_t)FW_IMAGE_BUILD, g_boot_info.version };
        tuya_send(OTA_CMD_IDENT, seq, id, sizeof(id));
        g_tuya_confirm = BOOT_CONFIRM_QUERIES;   /* an own ZTU: this run may confirm */
        break;
    }

    case TUYA_CMD_PRODUCT_QUERY:
        /* Only a stock Tuya ZTU asks this (every 1 s while unanswered): counted for the
         * confirmation, never answered (Open Dimmer is not a stock product). */
        if (len == 0 && g_tuya_confirm < BOOT_CONFIRM_QUERIES) {
            g_tuya_confirm++;
        }
        break;

    case OTA_CMD_NOTIFY: {
        uint8_t ok = 1;
#if !(SIM_ZC || MEASURE || DIAG_REPORT)
        /* The module has a newer firmware for us: say yes, then restart into the bootloader,
         * which fetches it. Anything else is declined, so a bad offer cannot keep the light
         * restarting. A build-aware module (11 bytes) offers by build number, an older one by
         * the version byte (0xBF is the SIM bench build's and 0xFF is never made,
         * tools/mkimage.py). The image must fit both the bootloader and our settings layout.
         * Bench builds take no update: the module would replace a measurement or diagnostics
         * build with its carried release seconds after mains-on, before any statistics, and a
         * SIM bench stays what it is under any module. The way back to a normal build is SWD. */
        if (len >= OTA_NOTIFY_LEN_V1) {
            uint32_t size = be32(&data[OTA_NOTIFY_AT_SIZE]);
            uint8_t  newer;
            if (len >= OTA_NOTIFY_LEN) {
                const uint8_t *b     = &data[OTA_NOTIFY_AT_BUILD];
                uint16_t       build = (uint16_t)((b[0] << 8) | b[1]);
                newer = build >= OTA_BUILD_MIN && build <= OTA_BUILD_MAX && build > FW_IMAGE_BUILD;
            } else {
                newer = data[OTA_NOTIFY_AT_VERSION] > FW_VERSION &&
                        data[OTA_NOTIFY_AT_VERSION] != OTA_VERSION_SIM &&
                        data[OTA_NOTIFY_AT_VERSION] != OTA_VERSION_NEVER;
            }
            if (newer && size >= APP_SIZE_MIN && size <= g_boot_info.app_limit) {
                ok = 0;
            }
        }
#endif
#if defined(OFFER_DECLINE_TEST) && OFFER_DECLINE_TEST
        ok = 1;                       /* bench test B3: a broken update path, every offer declined */
#endif
        tuya_send(OTA_CMD_NOTIFY, seq, &ok, 1);
#if !(SIM_ZC || MEASURE || DIAG_REPORT)
        if (ok == 0) {
            OTA_MAILBOX->expect_crc = be32(&data[OTA_NOTIFY_AT_CRC]);   /* the image to take */
            bootif_restart(&OTA_MAILBOX->magic, OTA_MAILBOX_MAGIC);    /* magic, ~magic */
        }
#endif
        break;
    }

    case TUYA_CMD_VERSION: {
        uint8_t v = FW_VERSION;
        tuya_send(TUYA_CMD_VERSION, seq, &v, 1);
        break;
    }

    case TUYA_CMD_NETWORK_STATUS:
        if (len >= 1) {
            g_network = data[0];
        }
        tuya_send(TUYA_CMD_NETWORK_STATUS, seq, NULL, 0);   /* ack */
        break;

    case TUYA_CMD_QUERY_NETWORK:     /* the module's answer to our query */
        if (len >= 1) {
            if (!g_query_seq) {
                g_query_seq = 1;         /* it saw our start: the next query goes on at 1 */
            }
            /* First answer after our boot: the module is listening. If only
             * we restarted (watchdog, reflash), it still holds the state from
             * before, so tell it ours, as answers (no switch actions). */
            if (!g_told_state) {
                g_told_state = 1;
                dimmer_report_all();
            }
            g_network = data[0];
        }
        break;

    default:
        break;
    }
}

static void rx_drop(uint16_t n) {
    if (n >= g_rx_len) {
        g_rx_len = 0;
        return;
    }
    memmove(g_rx, &g_rx[n], g_rx_len - n);
    g_rx_len = (uint16_t)(g_rx_len - n);
}

static void rx_process(void) {
    while (g_rx_len >= 2) {
        if (g_rx[0] != 0x55 || g_rx[1] != 0xAA) {
            rx_drop(1);
            continue;
        }
        if (g_rx_len < FRAME_HEADER) {
            return;
        }
        uint16_t dlen = (uint16_t)((g_rx[6] << 8) | g_rx[7]);
        if (g_rx[2] != FRAME_VERSION || dlen > RX_MAX_DATA) {
            rx_drop(2);          /* not a frame we can take, resync */
            continue;
        }
        uint16_t flen = (uint16_t)(FRAME_HEADER + dlen + 1);
        if (g_rx_len < flen) {
            return;
        }
        uint8_t sum = 0;
        for (uint16_t i = 0; i < flen - 1; i++) {
            sum += g_rx[i];
        }
        if (sum != g_rx[flen - 1]) {
            rx_drop(2);          /* damaged: a real frame may start inside */
            continue;
        }
        g_tuya_frames++;
        handle_frame(g_rx[5], (uint16_t)((g_rx[3] << 8) | g_rx[4]), &g_rx[FRAME_HEADER], dlen);
        rx_drop(flen);
    }
}

void tuya_poll(uint32_t now_ms) {
    int b;

    g_now_ms = now_ms;
    uint8_t got = 0;

    while ((b = uart_getc()) >= 0) {
        if (g_rx_len == sizeof(g_rx)) {
            rx_process();                /* complete frames first */
        }
        if (g_rx_len == sizeof(g_rx)) {
            rx_drop(1);
        }
        g_rx[g_rx_len++] = (uint8_t)b;
        got = 1;
    }
    if (got) {
        g_rx_last_ms = now_ms;
        rx_process();
    } else if (g_rx_len && (now_ms - g_rx_last_ms) > RX_STALE_MS) {
        /* A header whose frame never completes (a damaged length): a good
         * frame may sit behind it, so drop only its sync bytes and look again
         * (once per poll until the buffer is clean). */
        rx_drop(2);
        rx_process();
    }
}
