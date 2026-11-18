/* Host harness for dimmer.c and controls.c (tests/test_dimmer.c,
 * tests/test_controls.c), built with -DHOST_TEST (board.h, ota_proto.h).
 *
 * The real fader (dim_core.c) stands in for phase.c, one tick per 10 ms half
 * cycle; the settings store is one record in RAM; the Tuya report builder
 * writes what would go to the module into a text log:
 *   "A[1=1,2=500] "  an active report (0x06, a local change) of DP 1 and 2
 *   "P[3=200] "      a passive report (0x05, an answer such as a write ack)
 *   "S03 "           another frame sent (here 0x03, leave and rejoin) */
#ifndef HOST_STUBS_H
#define HOST_STUBS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "board.h"
#include "dim_core.h"
#include "ota_proto.h"
#include "store.h"

extern int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

/* Log of what was sent to the module. */
extern char     g_host_log[16384];
void            host_log_clear(void);
/** Offset of `what` in the log, -1 if not there. */
long            host_log_find(const char *what);

/* phase.c: the faders, ticked by host_halfcycle(). */
extern dim_fader_t g_host_fader[2];
void            host_phase_init(void);
void            host_halfcycle(void);

/* store.c: one record. */
extern uint8_t  g_host_flash[STORE_DATA_SIZE];
extern uint8_t  g_host_flash_valid;
extern unsigned g_host_saves;

/* What tuya.c does with a DP write from the module: apply it, then answer
 * with one passive report of the DP and its siblings. */
void            host_write(uint8_t dpid, uint8_t type, uint32_t value);

/* The saved record (dimmer.c saved_t, format 3), for tests that load one. */
typedef struct {
    uint8_t format;
    uint8_t power_on;
    uint8_t on_mask;
    uint8_t power_on_2;
    struct {
        uint16_t level;
        uint16_t min;
        uint16_t max;
        uint8_t  switch_type;
        uint8_t  curve;
    } ch[2];
    struct {
        uint16_t fade_on_ms;
        uint16_t fade_off_ms;
        uint16_t hold_ms;
        uint8_t  soft_end_pct;
        uint8_t  hold_end_pct;
    } pf[2];
    uint8_t reserved[4];
} host_saved_t;

_Static_assert(sizeof(host_saved_t) == STORE_DATA_SIZE, "host_saved_t must match dimmer.c saved_t");

#endif
