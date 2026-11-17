/* Settings ring in the last flash sector (4 KB = 32 pages of 128 bytes).
 * Each save erases the next page and programs one record into it, so the
 * newest valid record survives a power cut in the middle of a save.
 *
 * Record: magic, sequence number, STORE_DATA_SIZE bytes of data, CRC-32.
 * A page erase takes ~3.5 ms and a program ~1 ms, during which the CPU
 * (running from flash) and its interrupts stall. */
#ifndef STORE_H
#define STORE_H

#include <stdint.h>

#define STORE_DATA_SIZE    20

/** Load the newest valid record. Returns 1 if one was found. */
uint8_t store_load(uint8_t *data);
/** Write data as the newest record. Returns 1 on success. */
uint8_t store_save(const uint8_t *data);
/** Saves done since boot (diagnostics). */
uint16_t store_save_count(void);

#endif
