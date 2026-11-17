/* Tuya Zigbee MCU serial protocol, MCU side.
 *   55 AA 02 <seq:2> <cmd> <len:2> <data:len> <checksum>
 * checksum = sum of all preceding bytes mod 256, numbers big-endian. */
#ifndef TUYA_H
#define TUYA_H

#include <stdint.h>

#define TUYA_CMD_REPORT        0x06   /* MCU -> module: DP report */

#define TUYA_DP_TYPE_BOOL      0x01
#define TUYA_DP_TYPE_VALUE     0x02
#define TUYA_DP_TYPE_ENUM      0x04

void tuya_init(void);
void tuya_send_frame(uint8_t cmd, uint16_t seq, const uint8_t *data, uint16_t len);
void tuya_report_bool(uint8_t dpid, uint8_t value);

#endif
