/* Tuya Zigbee MCU serial protocol, MCU side.
 *   55 AA 02 <seq:2> <cmd> <len:2> <data:len> <checksum>
 * checksum = sum of all preceding bytes mod 256, numbers big-endian.
 * DP unit: <dpid> <type> <len:2> <value:len>. */
#ifndef TUYA_H
#define TUYA_H

#include <stdint.h>

#define TUYA_CMD_PRODUCT_QUERY   0x01   /* stock Tuya ZTU -> MCU: product info? Never answered: our
                                           own ZTU never asks it (docs/BOOTLOADER.md section 7) */
#define TUYA_CMD_NETWORK_STATUS  0x02   /* module -> MCU: Zigbee network state */
#define TUYA_CMD_RESET_NETWORK   0x03   /* MCU -> module: leave and pair again */
#define TUYA_CMD_WRITE           0x04   /* module -> MCU: DP write */
#define TUYA_CMD_REPORT_PASSIVE  0x05   /* MCU -> module: DP report answering the module */
#define TUYA_CMD_REPORT          0x06   /* MCU -> module: DP report of a local change */
#define TUYA_CMD_VERSION         0x0B   /* module -> MCU: firmware version? */
#define TUYA_CMD_QUERY_NETWORK   0x20   /* MCU -> module: network state? */
#define TUYA_CMD_QUERY_ALL       0x28   /* module -> MCU: report every DP */

#define TUYA_DP_TYPE_BOOL        0x01
#define TUYA_DP_TYPE_VALUE       0x02
#define TUYA_DP_TYPE_ENUM        0x04

#define TUYA_NETWORK_CONNECTED   0x01
#define TUYA_RESET_NETWORK_REJOIN 0x01  /* data of 0x03: leave and pair again */

/* Our firmware version, Tuya packing: 2 bits major, 2 bits minor, 4 bits patch.
 * 0x81 = 2.0.1 (major 2 tells it apart from the stock 1.0.x). */
#if SIM_ZC
/* 2.3.15: bench build with a virtual zero cross. Its output is not in step
 * with the mains: NEVER run it on a module connected to the mains. */
#define FW_VERSION               0xBF
#else
#ifndef FW_VERSION
#define FW_VERSION               0xA2   /* 2.2.2, build 700 (make VERSION_BYTE=0x.. overrides) */
#endif
#endif

/* Build number (docs/BOOTLOADER.md section 2; header 0xCE, identity bytes 7-8). FW_BUILD is the
 * release build, a literal here and nowhere else (tools/check_app.py reads it for CI and
 * tools/release/release.sh): a release sets it to the previous release + 100 (the first, release
 * R, is 300). Test builds move it only by 1..99, so no typo can jump the build space:
 *  - make FW_TEST=n (1..99): FW_BUILD + n, a spare test build below the next release;
 *  - make FW_PREV=1 FW_TEST=n: FW_BUILD - 100 + n, below this release (bench tests B3 and B11,
 *    whose image must lie below the release the module carries). LINK_WDG_TEST needs it, so a
 *    B11 image, carried by mistake, is never offered to a unit on the release.
 * The version byte stays for display and for byte-only peers (2.1.x apps, phase-1 ZTUs):
 * releases bump it, test builds keep it. SIM builds carry 0xFFFE (OTA_BUILD_SIM): no image is
 * newer, so nothing is ever offered to them. */
#ifdef FW_BUILD
#error "FW_BUILD is the release build in src/tuya.h; test builds: make FW_TEST=n [FW_PREV=1]"
#endif
#define FW_BUILD                 700
#ifndef FW_TEST
#define FW_TEST                  0
#elif FW_TEST < 1 || FW_TEST > 99
#error "make FW_TEST=n: n is 1..99"
#endif
#ifndef FW_PREV
#define FW_PREV                  0
#elif FW_PREV != 1 || FW_TEST == 0
#error "make FW_PREV=1 FW_TEST=n: test build n above the previous release (FW_BUILD - 100 + n)"
#endif
#define FW_BUILD_NUMBER          (FW_BUILD - 100 * FW_PREV + FW_TEST)   /* in normal builds */
#if SIM_ZC
#define FW_IMAGE_BUILD           0xFFFEu
#else
#define FW_IMAGE_BUILD           ((unsigned)FW_BUILD_NUMBER)
#endif
#if FW_BUILD % 100 != 0 || FW_BUILD_NUMBER < 256 || FW_BUILD_NUMBER > 0xFFFD
#error "FW_BUILD: a multiple of 100; the build (FW_PREV, FW_TEST) must be 256..0xFFFD"
#endif
/* Bench test B11's switch (link_core.h): a normal build below the release only. */
#if LINK_WDG_TEST && (SIM_ZC || MEASURE || DIAG_REPORT)
#error "LINK_WDG_TEST is a normal build's bench switch"
#endif
#if LINK_WDG_TEST && FW_BUILD_NUMBER >= FW_BUILD
#error "LINK_WDG_TEST builds lie below the release: make LINK_WDG_TEST=1 FW_PREV=1 FW_TEST=n"
#endif

void    tuya_init(void);
/** Parse what the UART received and handle complete frames. */
void    tuya_poll(uint32_t now_ms);
void    tuya_send(uint8_t cmd, uint16_t seq, const uint8_t *data, uint16_t len);
/** Ask the module for its network state (0x20). Sequence 0 until the module has
 * answered one (it takes seq 0 for an MCU restart and asks who we are), then 1, 2, ... */
void    tuya_query_network(uint32_t now_ms);
uint8_t tuya_network_status(void);
/** ms since the last received byte or our last network query (its answer is
 * due), 0 while a frame is half received. */
uint32_t tuya_rx_quiet_ms(uint32_t now_ms);

/* Read by the bootloader interface (bootif.c) in the main loop. */
extern uint32_t g_tuya_frames;    /* well-formed frames received since start (header, length,
                                     checksum; any command): the link watchdog's sign of life */
extern uint8_t  g_tuya_confirm;   /* BOOT_CONFIRM_QUERIES (2) or more: this run may confirm
                                     itself (docs/BOOTLOADER.md section 7), it answered an
                                     identity question 0xA3 (an own ZTU), or that many
                                     well-formed product queries 0x01 came without one (a stock
                                     Tuya ZTU) */

/* Report builder: begin, add DP units, send as one frame. passive = 1 for answers
 * to the module (write ack, query-all): its firmware does not treat those as a
 * touch, so they never fire switch actions or count towards a multi-press reset. */
void tuya_report_begin(uint8_t passive);
void tuya_report_bool(uint8_t dpid, uint8_t value);
void tuya_report_enum(uint8_t dpid, uint8_t value);
void tuya_report_value(uint8_t dpid, uint32_t value);
void tuya_report_send(void);

#endif
