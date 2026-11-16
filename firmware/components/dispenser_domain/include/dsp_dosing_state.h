#ifndef DSP_DOSING_STATE_H
#define DSP_DOSING_STATE_H

/*
 * Persistent dosing state (milestone M1.9a): the "dosing" record of storage layout v4.
 * One atomic record holding everything the Dosing Service must know across restarts that
 * is neither configuration nor execution history:
 *  - readiness (READY / SERVICE_REQUIRED, FR-025); a new record starts SERVICE_REQUIRED and
 *    only an explicit, fully checked transition makes it READY (approved M1.9a);
 *  - the reservoir BLOCKED latch (FR-007/FR-022), cleared only by a completed refill;
 *  - the refill intent (restart-safe roll-forward of a refill across three records);
 *  - the manual in-flight marker (a manual dose's durable claimed volume);
 *  - the daily volume ledger and the time of the last pump start (FR-016).
 */

#include <stdbool.h>
#include <stdint.h>

#include "dsp_time.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DSP_READINESS_READY = 1,
    DSP_READINESS_SERVICE_REQUIRED = 2,
} dsp_readiness_t;

enum {
    DSP_DOSING_F_BLOCKED = 1u << 0,        /* reservoir BLOCKED latch */
    DSP_DOSING_F_REFILL_PENDING = 1u << 1, /* refill intent not yet completed */
    DSP_DOSING_F_MANUAL_ACTIVE = 1u << 2,  /* a manual dose may be in progress */
};
#define DSP_DOSING_F_ALL 0x07u

typedef struct {
    uint8_t readiness; /* dsp_readiness_t */
    uint8_t flags;     /* DSP_DOSING_F_* */
    /* Refill intent (valid while REFILL_PENDING; all zero otherwise). */
    uint32_t refill_capacity_ul;
    uint32_t refill_remaining_ul;
    uint32_t refill_seq;
    /* Manual in-flight marker (valid while MANUAL_ACTIVE; all zero otherwise). */
    uint32_t manual_volume_ul;
    dsp_utc_t manual_started_at;
    uint32_t manual_seq;
    /* Daily ledger: the highest local date with an authorized dose and its total. */
    dsp_date_t ledger_date; /* all zero = no dose recorded yet */
    uint32_t ledger_sum_ul;
    dsp_utc_t last_start_utc; /* trusted UTC of the last authorized pump start, 0 = none */
    uint32_t next_seq;        /* sequence for manual doses and refills (starts at 1) */
} dsp_dosing_state_t;

/* Provisioning value of a new record: SERVICE_REQUIRED, everything else zero. */
void dsp_dosing_state_defaults(dsp_dosing_state_t *s);

/* Structural validity (used by the codec). */
bool dsp_dosing_state_is_valid(const dsp_dosing_state_t *s);

/* A provisioning value (defaults, optionally with the BLOCKED latch derived during a
 * migration): used by storage to recognize an interrupted provisioning or migration. */
bool dsp_dosing_state_is_provisioning(const dsp_dosing_state_t *s);

bool dsp_dosing_state_equal(const dsp_dosing_state_t *a, const dsp_dosing_state_t *b);

#ifdef __cplusplus
}
#endif

#endif /* DSP_DOSING_STATE_H */
