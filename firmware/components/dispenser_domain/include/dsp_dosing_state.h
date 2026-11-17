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
 *  - the daily volume ledger and the time of the last pump start (FR-016);
 *  - (format 2, M1.10b.2) manual-dose anti-replay: per-phone-slot request high-water marks,
 *    the in-flight marker's slot and request ID, and a ring of the last 8 request results.
 *
 * Anti-replay invariant (docs/BLE_PROTOCOL_DESIGN.md 7.4): a request (slot, seq) with
 * seq <= manual_hw[slot] is already decided and never executes. manual_hw only grows (until a
 * factory reset re-provisions the record); it is raised in the SAME write that records the
 * decision (the manual marker for an accepted request, the ring entry for a refusal), before any
 * reservoir deduction or pump activation. The ring only serves result retrieval; eviction never
 * weakens the high-water rule.
 */

#include <stdbool.h>
#include <stdint.h>

#include "dsp_time.h"

#define DSP_MANUAL_SLOTS 4                          /* phone slots (approved D5) */
#define DSP_MANUAL_RING 8                           /* retained request results */
#define DSP_MANUAL_SEQ_MAX ((uint64_t)INT64_MAX)    /* largest request ID (2^63 - 1) */

/* Result states (values = BLE protocol v1 GET_RESULT `state`). */
enum {
    DSP_MRES_NOT_DECIDED = 0,
    DSP_MRES_PENDING = 1,
    DSP_MRES_DONE = 2,
    DSP_MRES_FAILED = 3,
    DSP_MRES_INTERRUPTED = 4,
    DSP_MRES_REFUSED = 5,
    DSP_MRES_UNAVAILABLE = 6, /* decided earlier, result no longer retained */
};

typedef struct {
    uint8_t slot;    /* 0..DSP_MANUAL_SLOTS - 1 */
    uint8_t state;   /* DSP_MRES_PENDING..DSP_MRES_REFUSED; entry unused when seq == 0 */
    uint64_t seq;    /* request ID, 0 = unused entry */
    uint32_t detail; /* volume (PENDING/DONE/FAILED/INTERRUPTED) or refusal code (REFUSED) */
} dsp_manual_result_t;

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
    /* Format 2 (M1.10b.2). A format-1 record decodes with all of these zero. */
    uint64_t manual_hw[DSP_MANUAL_SLOTS]; /* per-slot request high-water mark (0 = none) */
    uint8_t marker_slot;  /* slot of the in-flight manual request (valid while marker_req != 0) */
    uint64_t marker_req;  /* request ID of the in-flight manual dose; 0 = none (or a format-1
                             marker without request binding) */
    uint8_t ring_next;    /* index of the next ring entry to (over)write, 0..DSP_MANUAL_RING - 1 */
    dsp_manual_result_t ring[DSP_MANUAL_RING];
    /* Format 3 (M1.10b.3): durable manual-dose admission counter (docs/OWNERSHIP_DESIGN.md
     * 11.2). Updated only in the write that makes a fresh decision durable; formats 1 and 2
     * decode with both zero. adm_date: the local date the count belongs to (all zero = none
     * yet; restarted only by a trusted later date); adm_count: fresh decisions counted. */
    dsp_date_t adm_date;
    uint16_t adm_count;
} dsp_dosing_state_t;

/* Provisioning value of a new record: SERVICE_REQUIRED, everything else zero. */
void dsp_dosing_state_defaults(dsp_dosing_state_t *s);

/* Structural validity (used by the codec). */
bool dsp_dosing_state_is_valid(const dsp_dosing_state_t *s);

/* A provisioning value (defaults, optionally with the BLOCKED latch derived during a
 * migration): used by storage to recognize an interrupted provisioning or migration. */
bool dsp_dosing_state_is_provisioning(const dsp_dosing_state_t *s);

bool dsp_dosing_state_equal(const dsp_dosing_state_t *a, const dsp_dosing_state_t *b);

/* Ring entry of (slot, seq), or NULL. */
const dsp_manual_result_t *dsp_dosing_state_find_result(const dsp_dosing_state_t *s, uint8_t slot,
                                                        uint64_t seq);

#ifdef __cplusplus
}
#endif

#endif /* DSP_DOSING_STATE_H */
