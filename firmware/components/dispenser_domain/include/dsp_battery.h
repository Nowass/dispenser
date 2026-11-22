#ifndef DSP_BATTERY_H
#define DSP_BATTERY_H

/*
 * Battery telemetry (runtime only): the measured single-cell Li-ion voltage, read through a
 * resistive divider by the platform's ADC adapter. Nothing here is ever stored in NVS: a new
 * measurement after every boot is sufficient. A missing or failed measurement is a state of its
 * own and NEVER 0 mV / 0 %. The domain does not interpret the voltage (no thresholds, no dosing
 * restriction in this milestone; the percentage is an app-side estimate).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Measurement state (also the BLE wire value of GET_BATTERY, docs/BLE_PROTOCOL.md 7.7). */
typedef enum {
    DSP_BATT_VALID = 0,          /* battery_mv is a calibrated measurement */
    DSP_BATT_NOT_MEASURED = 1,   /* no battery port in this build, or not measured yet */
    DSP_BATT_ADC_FAILED = 2,     /* ADC unit/channel setup or a conversion failed */
    DSP_BATT_NO_CALIBRATION = 3, /* no ADC calibration (eFuse) for a calibrated voltage */
    DSP_BATT_OUT_OF_RANGE = 4,   /* not a credible 1S Li-ion voltage (divider open, saturated) */
    DSP_BATT_UNSTABLE = 5,       /* the samples disagree too much (see DSP_BATT_MAX_SPREAD_MV) */
} dsp_batt_state_t;
#define DSP_BATT_STATE_MAX 5u

/* Credible battery voltage window. Below 2.5 V a protected cell has already cut off (and the
 * board would not run); above 4.6 V the ADC input is beyond the 12 dB range or the divider is
 * wrong. Readings outside are OUT_OF_RANGE, never clamped into the window. */
#define DSP_BATT_MIN_MV 2500u
#define DSP_BATT_MAX_MV 4600u

/* One measurement. battery_mv is 0 unless state == DSP_BATT_VALID. */
typedef struct {
    uint8_t state;       /* dsp_batt_state_t */
    uint16_t battery_mv;
} dsp_batt_sample_t;

/* Platform port: one bounded, blocking measurement (a few milliseconds, no delays). Called by the
 * application task only, never while the pump runs. NULL port = no battery telemetry. */
typedef struct {
    void *ctx;
    dsp_batt_sample_t (*measure)(void *ctx);
} dsp_battery_port_t;

/* Normalizes a port result: an unknown state becomes ADC_FAILED, a VALID voltage outside the
 * credible window becomes OUT_OF_RANGE, and every non-VALID sample carries 0 mV. */
dsp_batt_sample_t dsp_batt_sanitize(dsp_batt_sample_t s);

#ifdef __cplusplus
}
#endif

#endif /* DSP_BATTERY_H */
