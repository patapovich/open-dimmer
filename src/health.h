/* Health counters for long runs (normal and SIM builds; the measurement
 * builds report their own statistics every 10 s): 3 s after the module asked
 * who we are (its start or ours; a minute after start if it never asks) and
 * then every 2 min (the module's log keeps ~7 min) as reports of DPs
 * 130..137, logged by the module's MCU link diagnostics like the measurement
 * DPs (no converter entity):
 *   130 uptime in minutes
 *   131 reset cause (RCC_CSR flags at start >> 24: bit 7 LPWR, 6 WWDG, 5 IWDG,
 *       4 software (update, fault), 3 power/BOR, 2 pin, 1 option bytes)
 *   132 zero-cross lock losses   133 edges ignored as noise
 *   134 settings saves           135 UART receive losses (overruns, ring full)
 *   136 CRC-32 of the 4 KB boot sector, which tells the bootloader build (the
 *       released v1: 0xEAC45031)
 *   137 the boot word at start, bits 23..0 (ota_proto.h BOOT_*: probe answer
 *       time, rescue info, LINK, tries; 0 when it was not valid, v1); bit 24 the
 *       settings ring is the sector's upper half (RING_HIGH, from release M),
 *       bit 25 the newest settings record is in this app's own ring
 *   138 the MCU die temperature in degC (signed; -32768 none yet), from 2.2.0,
 *       in a frame of its own after the eight above (src/temp.h)
 *   139 with it: the chip's factory calibration of that sensor, the raw value at
 *       85 degC in bits 31..16 and at 30 degC in bits 15..0 (how far chips differ)
 * All since start; none wraps in a trial (uptime counts minutes, not the
 * millisecond clock that wraps after 49.7 days). */
#ifndef HEALTH_H
#define HEALTH_H

#include <stdint.h>

void health_init(uint32_t reset_cause);
/** Main loop: counts the minutes and sends the report when it is due. */
void health_poll(uint32_t now_ms);
/** The module asked who we are (a new contact: it restarted, or we did): the next report
 * goes out in 3 s instead of at its time, so a hub sees a restart and its cause at once;
 * the reports after it follow every 2 min. */
void health_soon(uint32_t now_ms);

#endif
