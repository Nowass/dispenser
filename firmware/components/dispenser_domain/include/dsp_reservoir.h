#ifndef DSP_RESERVOIR_H
#define DSP_RESERVOIR_H

/*
 * Reservoir accounting (milestone M1.5). The remaining volume is an ESTIMATE derived from
 * dosing consumption (no level sensor, FR-006/FR-020); it lives in the configuration
 * record (M1.3) and every change bumps the configuration revision.
 *
 * Conservative accounting (approved 2026-11-15): every dose whose pump is authorized,
 * automatic (after a successful M1.2 claim) or manual, subtracts its FULL requested volume,
 * whatever its outcome (COMPLETED, FAILED, INTERRUPTED). The estimate saturates at 0.
 *
 * Integration contract (M1.5 review), enforced by the API shape:
 *  - A dose permit (dsp_dose_permit_t) is issued ONLY by dsp_reservoir_consume(), and only
 *    after the reduced estimate is durable. The future actuator interface accepts a permit,
 *    nothing else, and redeems it with dsp_dose_permit_take() (single use). So the pump
 *    cannot run for a volume that has not been subtracted (durable before effective: a
 *    power cut can only make the estimate lower, never higher).
 *  - A failed start, failed pump run or interruption is reported with
 *    dsp_notify_report_dosing_error(), which also sets the persistent uncertainty flag in
 *    the same write. A refill (dsp_reservoir_refill, then dsp_notify_on_refill) clears it.
 * Not coupled to the recurrence engine.
 */

#include <stdbool.h>
#include <stdint.h>

#include "dsp_config.h"
#include "dsp_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DSP_DOSE_AUTOMATIC = 0,
    DSP_DOSE_MANUAL = 1,
} dsp_dose_kind_t;

/* FR-022: an automatic dose may run only if the reservoir is configured and the estimate
 * covers the full requested volume. */
bool dsp_reservoir_covers(const dsp_config_t *cfg, uint32_t requested_ul);

/* Authorization to run the pump for one dose (only from dsp_reservoir_consume). */
typedef struct {
    bool granted;
    uint8_t kind;             /* dsp_dose_kind_t */
    uint32_t volume_ul;       /* requested volume, already subtracted */
    uint32_t config_revision; /* configuration revision that recorded the subtraction */
} dsp_dose_permit_t;

/* Subtracts requested_ul (saturating at 0) and saves the configuration (revision + 1).
 * permit (required) is granted only when the save succeeded; on any error it is cleared.
 * *after (optional) receives the saved configuration. */
dsp_storage_status_t dsp_reservoir_consume(dsp_storage_t *st, uint32_t requested_ul,
                                           dsp_dose_kind_t kind, dsp_config_t *after,
                                           dsp_dose_permit_t *permit);

/* Redeems a permit exactly once (for the actuator): true and the volume if it was granted;
 * the permit is cleared either way. */
bool dsp_dose_permit_take(dsp_dose_permit_t *permit, uint32_t *volume_ul);

/* User-confirmed refill (FR-023): sets capacity and remaining (remaining <= capacity). The
 * caller then clears the uncertainty flag and lets the notification engine re-observe. */
dsp_storage_status_t dsp_reservoir_refill(dsp_storage_t *st, uint32_t capacity_ul,
                                          uint32_t remaining_ul, dsp_config_t *after);

#ifdef __cplusplus
}
#endif

#endif /* DSP_RESERVOIR_H */
