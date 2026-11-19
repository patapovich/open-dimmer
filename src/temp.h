/* Die temperature of the PY32F002A from its internal sensor (ADC channel 11), with the factory
 * calibration of each chip (raw values at 30 and 85 degC with Vref+ = 3.3 V, 0x1FFF0F14 /
 * 0x1FFF0F18). Health DP 138, every report. One conversion per report, started at the report
 * before: nothing waits. It is the MCU's own temperature, near the gate MOSFETs on the same
 * board but not theirs: a diagnostic, no protection is built on it. */
#ifndef TEMP_H
#define TEMP_H

#include <stdint.h>

#define TEMP_NONE  (-32768)    /* no result yet (or no factory calibration) */

/** degC from a 12-bit raw value and the two calibration points (unsigned maths: no signed
 * division code). TEMP_NONE when the calibration is not plausible. */
static inline int16_t temp_from_raw(uint32_t raw, uint32_t cal30, uint32_t cal85) {
    if (cal85 <= cal30 || cal85 > 4095u) {
        return TEMP_NONE;
    }
    uint32_t span = cal85 - cal30;
    if (raw >= cal30) {
        return (int16_t)(30 + (int32_t)(((raw - cal30) * 55u + span / 2u) / span));
    }
    return (int16_t)(30 - (int32_t)(((cal30 - raw) * 55u + span / 2u) / span));
}

/** The chip's factory calibration as reported (health DP 139): the raw value at 85 degC in
 * bits 31..16, at 30 degC in bits 15..0, so the spread between chips can be seen. */
uint32_t temp_calibration(void);
/** ADC on, calibrated, the sensor's channel selected (once, at start). */
void    temp_init(void);
/** The result of the conversion started at the last call (TEMP_NONE if none), and the next one
 * started. */
int16_t temp_take(void);

#endif
