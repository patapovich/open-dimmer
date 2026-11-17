#include "tuya.h"
#include "dimmer.h"
#include "ota_proto.h"
#include "phase.h"
#include "py32f0xx.h"
#include "uart.h"
#include <string.h>

#define FRAME_HEADER       8      /* 55 AA ver seq:2 cmd len:2 */
#define RX_MAX_DATA        96
#define RX_STALE_MS        50     /* bytes of one frame arrive back to back */
#define REPORT_MAX_DATA    64

static uint8_t  g_rx[FRAME_HEADER + RX_MAX_DATA + 1];
static uint16_t g_rx_len;
static uint32_t g_rx_last_ms;
static uint16_t g_seq;
static uint8_t  g_network = 0xFF;   /* unknown until the module tells us */
static uint8_t  g_told_state;

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
    uint8_t head[FRAME_HEADER] = { 0x55, 0xAA, 0x02, (uint8_t)(seq >> 8), (uint8_t)seq, cmd,
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
        uint8_t sibling = dimmer_sibling_dp(dpid);
        if (sibling) {
            dimmer_report_dp(sibling);   /* min may have moved max and back */
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
        uint8_t id[7] = { 'Z', 'D', 'M', 'S', OTA_IDENT_KIND_APP, FW_VERSION, OTA_PROTOCOL };
        tuya_send(OTA_CMD_IDENT, seq, id, sizeof(id));
        break;
    }

    case OTA_CMD_NOTIFY: {
        /* The module has a newer firmware for us: say yes, then restart into
         * the bootloader, which fetches it. Anything else is declined, so a
         * bad offer cannot keep the light restarting. */
        uint32_t size = len >= 9 ? ((uint32_t)data[1] << 24) | ((uint32_t)data[2] << 16) |
                                   ((uint32_t)data[3] << 8) | data[4] : 0;
        uint8_t  ok   = (len >= 9 && data[0] > FW_VERSION && size >= APP_HEADER_OFFSET &&
                         size <= APP_MAX_SIZE) ? 0 : 1;
        tuya_send(OTA_CMD_NOTIFY, seq, &ok, 1);
        if (ok != 0) {
            break;
        }
        phase_shutdown();
        dimmer_prepare_restart();
        OTA_MAILBOX->magic     = OTA_MAILBOX_MAGIC;
        OTA_MAILBOX->magic_inv = ~OTA_MAILBOX_MAGIC;
        NVIC_SystemReset();
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
        if (dlen > RX_MAX_DATA) {
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
        g_rx_len = 0;            /* half a frame that will never complete */
    }
}
