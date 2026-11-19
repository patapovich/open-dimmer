/* Settings ring in the last flash sector (4 KB = 32 pages of 128 bytes).
 * Each save erases a page after the newest record and programs one record
 * into it, so the newest valid record survives a power cut in the middle of
 * a save, and also a page that does not verify (worn out): such a page is
 * skipped by the next save, and the newest record and the one before it are
 * never erased.
 *
 * Record: magic, sequence number, STORE_DATA_SIZE bytes of data, CRC-32.
 * From release M (2.1.14, RING_HIGH) the ring is the sector's upper half (16 pages); store_load
 * scans the whole sector above the app's own image and copies a newer record from below up into
 * the ring (docs/BOOTLOADER.md section 1).
 * A page erase takes ~3.5 ms and a program ~1 ms, during which the CPU
 * (running from flash) and its interrupts stall: one page attempt (erase +
 * program + verify, ~5 ms) per call, unless the caller asks for more. */
#ifndef STORE_H
#define STORE_H

#include <stdint.h>

#define STORE_DATA_SIZE    40   /* records of 20 (version 1) are read too, rest 0 */

/** Load the newest valid record. Returns 1 if one was found. */
uint8_t store_load(uint8_t *data);
/** Write data as the newest record, trying at most `attempts` pages (each
 * one a CPU stall of ~5 ms or more). A page that fails is skipped by the
 * next attempt, in this call or a later one. Returns 1 on success. */
uint8_t store_save_tries(const uint8_t *data, uint8_t attempts);
/** One page attempt: what fits a save window between zero crosses. */
static inline uint8_t store_save(const uint8_t *data) {
    return store_save_tries(data, 1);
}
/** Saves done since boot (diagnostics). */
uint32_t store_save_count(void);
/** 1 when the newest record is in this app's own ring (after the ring moved up: once its copy is
 * saved; health DP 137 bit 25). */
uint8_t store_in_own_ring(void);

#endif
