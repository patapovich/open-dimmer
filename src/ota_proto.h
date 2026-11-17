/* PY32 firmware update over the Tuya UART, from the Zigbee module (ZTU), which
 * carries the app image inside its own OTA image. Shared by app and bootloader.
 *
 * Flash: bootloader 0x08000000 (2 KB), app 0x08000800 (14 KB), settings
 * 0x08004000 (4 KB). The app image starts with its vector table; the header
 * below follows it at APP_HEADER_OFFSET.
 *
 * Commands (ours, not Tuya's own MCU OTA 0x0C..0x0E):
 *   0xA0 module -> MCU  "update available": version(1) size(4) crc(4)
 *        MCU -> module  answer: status(1), 0 = going to the bootloader
 *   0xA1 MCU -> module  block request: offset(4) max_len(2)
 *        module -> MCU  block: offset(4) size(4) crc(4) version(1) data(n);
 *                       n = 0 past the end. Stateless on the module side.
 *   0xA2 MCU -> module  result: status(1) version(1), 0 = written and verified
 * Numbers big-endian as everywhere in the Tuya protocol. The bootloader answers
 * the version query 0x0B with OTA_BOOT_VERSION. */
#ifndef OTA_PROTO_H
#define OTA_PROTO_H

#include <stdint.h>

#define OTA_CMD_NOTIFY        0xA0
#define OTA_CMD_BLOCK         0xA1
#define OTA_CMD_RESULT        0xA2

#define OTA_BOOT_VERSION      0x00     /* "bootloader, no app running" */
#define OTA_RESULT_OK         0
#define OTA_RESULT_BAD_IMAGE  1        /* CRC / header check failed after writing */
#define OTA_RESULT_FLASH      2        /* a page did not verify */

#define APP_BASE              0x08000800u
#define APP_MAX_SIZE          (14u * 1024u)
#define APP_HEADER_OFFSET     0xC0u    /* after the 48-entry vector table */
#define APP_MAGIC             0x5A445041u   /* "APDZ" */

typedef struct {
    uint32_t magic;
    uint32_t size;      /* image bytes from APP_BASE, filled in by tools/mkimage.py */
    uint32_t crc;       /* CRC-32 of the image with this field read as 0 */
    uint8_t  version;   /* Tuya packed: 2 bits major, 2 minor, 4 patch */
    uint8_t  reserved[3];
} app_header_t;

/* Mailbox in the last 16 bytes of RAM (outside .data/.bss/stack in both
 * linker scripts): survives a software reset, random after power-up. */
#define OTA_MAILBOX_ADDR      0x20000BF0u
#define OTA_MAILBOX_MAGIC     0x55504454u   /* "UPDT" */

typedef struct {
    uint32_t magic;
    uint32_t magic_inv;
    uint32_t reserved[2];
} ota_mailbox_t;

#define OTA_MAILBOX           ((volatile ota_mailbox_t *)OTA_MAILBOX_ADDR)

#endif
