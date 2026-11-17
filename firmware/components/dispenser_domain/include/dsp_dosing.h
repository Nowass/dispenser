#ifndef DSP_DOSING_H
#define DSP_DOSING_H

/*
 * Dosing Service (milestone M1.9a): the SOLE owner of the pump. Pure domain code over the
 * M1.2 coordinator (at-most-once automatic execution), M1.3 storage, M1.5 reservoir
 * accounting (single-use permits) and notification engine, and the M1.9a "dosing" record.
 * No ESP-IDF, no GPIO: the pump is an injected port (a real GPIO driver is a later,
 * hardware-dependent milestone).
 *
 * Approved invariants (M1.9 design):
 *  - The pump port is held only here; it runs only after a single-use permit was redeemed,
 *    and a permit exists only after the reduced reservoir estimate is durable.
 *  - Every write that RESTRICTS (claim / manual marker, daily ledger, last start) is
 *    durable BEFORE the write that ENABLES (the permit). A power cut can only cost a dose or
 *    make the accounting stricter, never permit an extra one.
 *  - No actuation unless: dosing record loaded, readiness READY, BLOCKED latch clear, no
 *    refill pending, no manual marker, a trusted clock, configuration format 2 with valid
 *    calibration and limits, a configured reservoir, and every limit satisfied (per-dose
 *    volume, runtime <= configured and hardware maximum, daily total per local date,
 *    minimum interval since the last start). Missing configuration never raises BLOCKED.
 *  - After any storage failure in this service the rest of the wake is DEGRADED: no
 *    further actuation (state is reloaded at the next boot).
 *  - Recovery (boot) completes a pending refill, then reports every interrupted automatic
 *    and manual dose with its DURABLE claimed volume BEFORE clearing it; reports are keyed
 *    per dose (dsp_notify_report_dosing_error_once), so repeating recovery after a power
 *    loss never loses a report, never queues a second one, never runs the pump and never
 *    changes the reservoir estimate.
 *  - A dose-bound report that cannot be persisted never removes its durable source: the
 *    STARTED record / manual marker is kept for recovery. A report whose dose left no durable
 *    source (claim or marker write failed, no pump run) is RAM-only and lost at a restart.
 *  - Single owner, single task, no allocation. Not thread-safe by design.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dsp_config.h"
#include "dsp_dosing_state.h"
#include "dsp_exec.h"
#include "dsp_notify.h"
#include "dsp_recurrence.h"
#include "dsp_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Pump port (platform; only the Dosing Service holds it) ------------------------- */

typedef enum {
    DSP_PUMP_OK = 0,           /* the commanded runtime finished (no flow sensing) */
    DSP_PUMP_START_FAILED = 1, /* could not be started */
    DSP_PUMP_FAILED = 2,       /* fault during the run (cutoff, driver error) */
} dsp_pump_result_t;

typedef struct {
    void *ctx;
    /* Blocking run of runtime_ms (already validated against every limit). The driver must
     * enforce its own hardware cutoff and leave the pump OFF on every return. */
    dsp_pump_result_t (*run)(void *ctx, uint32_t runtime_ms);
    /* Idempotent: drives the pump OFF. Called at init and after every run. */
    void (*force_off)(void *ctx);
    /* M1.9b: confirms the output is OFF (driver state / gate readback). NULL = cannot be
     * confirmed: safe-off is then reported unconfirmed (fail-safe, see dsp_power.h). */
    bool (*is_off)(void *ctx);
} dsp_pump_port_t;

/* Reads the current clock (for doses executed one after another in one wake). */
typedef struct {
    void *ctx;
    dsp_clock_t (*read)(void *ctx);
} dsp_dosing_clock_t;

/* ---- Results -------------------------------------------------------------------------- */

typedef enum {
    DSP_DOSING_OK = 0,        /* dosed (pump finished) */
    DSP_DOSING_ERR_ARG,
    DSP_DOSING_ERR_STORAGE,   /* durable state could not be read/written: no actuation */
    DSP_DOSING_REFUSED,       /* a gate is closed (result.gates) */
    DSP_DOSING_NOT_ELIGIBLE,  /* coordinator: not due, missed, already claimed, ... (result.exec) */
    DSP_DOSING_LIMIT,         /* a safety limit (result.limits) */
    DSP_DOSING_NOT_COVERED,   /* the estimate does not cover the dose (automatic: latch set) */
    DSP_DOSING_PUMP_FAILED,   /* the pump reported a fault (volume counted, uncertainty set) */
} dsp_dosing_status_t;

/* Gates (why nothing may be dosed). */
enum {
    DSP_DOSING_G_NOT_LOADED = 1u << 0,
    DSP_DOSING_G_DEGRADED = 1u << 1,      /* a storage failure earlier in this wake */
    DSP_DOSING_G_NOT_READY = 1u << 2,     /* SERVICE_REQUIRED */
    DSP_DOSING_G_BLOCKED = 1u << 3,       /* reservoir BLOCKED latch */
    DSP_DOSING_G_REFILL_PENDING = 1u << 4,
    DSP_DOSING_G_MANUAL_ACTIVE = 1u << 5, /* recovery has not run yet */
    DSP_DOSING_G_CLOCK = 1u << 6,         /* no trusted clock */
    DSP_DOSING_G_PUMP_CONFIG = 1u << 7,   /* calibration or limits missing/invalid */
    DSP_DOSING_G_RESERVOIR_UNCONFIGURED = 1u << 8,
    DSP_DOSING_G_CREDENTIALS = 1u << 9, /* M1.10b.3: credentials damaged (CRED_DEGRADED) or an
                                           identity key fault in this wake: no dosing until
                                           recovery or maintenance */
};

/* Safety limits violated. */
enum {
    DSP_DOSING_L_DOSE = 1u << 0,     /* volume > max_dose_ul */
    DSP_DOSING_L_RUNTIME = 1u << 1,  /* runtime > max_runtime_ms or the hardware maximum */
    DSP_DOSING_L_DAILY = 1u << 2,    /* daily total would exceed max_daily_ul */
    DSP_DOSING_L_INTERVAL = 1u << 3, /* min_interval_s since the last start not elapsed */
};

typedef struct {
    dsp_dosing_status_t status;
    uint32_t gates;         /* DSP_DOSING_G_* (REFUSED) */
    uint32_t pump_problems; /* DSP_PUMPCFG_E_* behind G_PUMP_CONFIG */
    uint32_t limits;        /* DSP_DOSING_L_* (LIMIT) */
    dsp_exec_status_t exec; /* coordinator result (NOT_ELIGIBLE, storage errors) */
    uint32_t volume_ul;
    uint32_t runtime_ms;
    bool pumped;            /* the pump port was called */
    dsp_pump_result_t pump;
} dsp_dosing_result_t;

/* Caller-owned service state. Treat as opaque. */
typedef struct {
    dsp_storage_t *st;
    dsp_exec_t *ex;
    dsp_notify_t *n;
    const dsp_pump_port_t *pump;
    const dsp_tz_t *tz;
    uint32_t hw_max_runtime_ms;
    dsp_dosing_state_t s; /* RAM copy, always equal to the last durable write */
    bool loaded;
    bool degraded;
    bool cred_inhibit;     /* M1.10b.3: DSP_DOSING_G_CREDENTIALS (set by the Application Service) */
    uint16_t adm_limit;    /* M1.10b.3: fresh manual-dose decisions per day (>= 1) */
} dsp_dosing_t;

#define DSP_MANUAL_DECISIONS_PER_DAY_DEFAULT 32 /* approved starting policy (O10) */
#define DSP_MANUAL_DECISIONS_PER_DAY_MAX 1000

/* hw_max_runtime_ms: the platform's hardware cutoff (0 = unknown: never actuates). Loads
 * the "dosing" record; drives the pump OFF. */
dsp_dosing_status_t dsp_dosing_init(dsp_dosing_t *d, dsp_storage_t *st, dsp_exec_t *ex,
                                    dsp_notify_t *n, const dsp_pump_port_t *pump,
                                    const dsp_tz_t *tz, uint32_t hw_max_runtime_ms);

typedef struct {
    bool refill_completed;  /* a pending refill was rolled forward */
    size_t automatic;       /* interrupted automatic doses handled */
    size_t manual;          /* interrupted manual doses handled (0/1) */
    size_t duplicates;      /* of those, already reported before (power loss in recovery) */
} dsp_dosing_recovery_t;

/* Boot recovery (see header comment). Never actuates. ERR_STORAGE stops early; it is safe
 * to call again (and it is called again at the next boot). */
dsp_dosing_status_t dsp_dosing_recover(dsp_dosing_t *d, dsp_notify_clock_t clock,
                                       dsp_dosing_recovery_t *out);

/* Gates for any dose right now (loads the configuration). */
uint32_t dsp_dosing_gates(dsp_dosing_t *d, bool clock_trusted, uint32_t *pump_problems);

/* One automatic occurrence (from the recurrence engine). */
dsp_dosing_result_t dsp_dosing_run_automatic(dsp_dosing_t *d, const dsp_schedule_t *schedule,
                                             const dsp_occurrence_t *occ, dsp_clock_t clock);

typedef struct {
    size_t candidates;       /* due-window occurrences found */
    size_t dosed;            /* pump runs that finished */
    size_t pump_failures;
    dsp_dosing_status_t stop; /* DSP_DOSING_OK, or why processing stopped early */
    dsp_dosing_result_t last; /* result of the last candidate */
} dsp_dosing_due_report_t;

/* All occurrences inside the due window of now (reading the clock before each), in time
 * order (then schedule ID), one after another. Stops at a storage failure, an uncovered
 * dose (BLOCKED) or a pump fault. */
dsp_dosing_due_report_t dsp_dosing_process_due(dsp_dosing_t *d, const dsp_schedule_t *schedules,
                                               size_t n, const dsp_dosing_clock_t *clock);

/* ---- Manual dose with durable anti-replay (M1.10b.2) ------------------------------------
 *
 * The ONLY manual-dose entry. `slot` must come from the authenticated session (never from a
 * request); `seq` is the phone's request ID for that slot. Same gates and limits as automatic
 * doses; refused while BLOCKED (Q7); never touches automatic history.
 *  - INVALID (slot >= 4, seq 0, seq > DSP_MANUAL_SEQ_MAX, volume 0): nothing changes, the request
 *    ID is not consumed.
 *  - DECIDED (seq <= manual_hw[slot]): never executes; the retained result (ring) or
 *    DSP_MRES_UNAVAILABLE. No side effect. A slot at DSP_MANUAL_SEQ_MAX accepts nothing more.
 *  - REFUSED (gates, limits, coverage, or app_refusal != 0 for refusals decided by the
 *    Application Service): manual_hw[slot] = seq and the ring entry are made durable in ONE
 *    dosing write before the answer, so the request can never execute later.
 *  - EXECUTED: write 1 = marker + ledger + last start + manual_hw[slot] = seq + ring PENDING
 *    (restricting), write 2 = consumption -> permit, pump, write 3 = marker cleared + ring
 *    DONE/FAILED; boot recovery turns PENDING into INTERRUPTED in the write that clears the
 *    marker. A failure after write 1 leaves the marker for recovery (never a second run).
 *  - NOT_DURABLE: the decision could not be made durable (state not loaded, DEGRADED earlier in
 *    this wake, or the decision write failed): nothing executed; a retry is evaluated again (the
 *    durable state decides: if the write landed, the retry is DECIDED).
 */
typedef enum {
    DSP_MANUAL_INVALID = 1,
    DSP_MANUAL_DECIDED,
    DSP_MANUAL_REFUSED,
    DSP_MANUAL_EXECUTED,
    DSP_MANUAL_NOT_DURABLE,
    DSP_MANUAL_RATE_LIMITED, /* M1.10b.3: admission refused; nothing written, still undecided */
} dsp_manual_kind_t;

/* Refusal codes stored as the REFUSED result detail (values = BLE protocol error codes). */
enum {
    DSP_MREFUSE_GATES = 7,       /* REFUSED: a gate is closed */
    DSP_MREFUSE_LIMIT = 8,       /* LIMIT */
    DSP_MREFUSE_NOT_COVERED = 9, /* NOT_COVERED */
    DSP_MREFUSE_DEGRADED = 14,   /* DEGRADED: refused by the Application Service in this wake */
};

typedef struct {
    dsp_manual_kind_t kind;
    uint8_t state;       /* DSP_MRES_*: the request's result after this call */
    uint32_t detail;     /* result detail (volume or refusal code) */
    dsp_dosing_result_t dose; /* gates / limits / pump detail (REFUSED, EXECUTED, NOT_DURABLE) */
} dsp_manual_outcome_t;

dsp_manual_outcome_t dsp_dosing_manual_request(dsp_dosing_t *d, uint8_t slot, uint64_t seq,
                                               uint32_t volume_ul, dsp_clock_t clock,
                                               uint32_t app_refusal);

/* ---- Manual-dose admission (M1.10b.3, docs/OWNERSHIP_DESIGN.md 11.2) ----------------------
 *
 * A durable count of FRESH decisions (accepted or refused) per local day lives in the dosing
 * record and is updated only inside the write that makes the decision durable (no write of its
 * own). Order inside dsp_dosing_manual_request: invalid -> already decided (always answered,
 * never counted) -> admission -> decision. A request over the limit is RATE_LIMITED: nothing is
 * written and it stays undecided, so the anti-replay rule is unchanged.
 * The count restarts only when a TRUSTED local date later than adm_date is seen; without a
 * trusted clock, and after a clock rollback, the stored count keeps applying. */
void dsp_dosing_set_admission_limit(dsp_dosing_t *d, uint16_t per_day); /* 0 = default */

/* Read-only: true if a fresh decision would be admitted now (the Application Service checks
 * before suspending BLE; the request path re-checks authoritatively). *retry_s (optional):
 * seconds until the next local midnight with a trusted clock, 0 if unknown. */
bool dsp_dosing_manual_admissible(const dsp_dosing_t *d, dsp_clock_t clock, uint32_t *retry_s);

/* Credentials gate (CRED_DEGRADED or a key fault): closes DSP_DOSING_G_CREDENTIALS. */
void dsp_dosing_set_credentials_inhibit(dsp_dosing_t *d, bool inhibit);

/* Read-only result query (GET_RESULT): INVALID, DECIDED (state/detail as above) or
 * NOT_DURABLE with state NOT_DECIDED (seq above the high-water mark, or state not loaded). */
dsp_manual_outcome_t dsp_dosing_manual_lookup(const dsp_dosing_t *d, uint8_t slot, uint64_t seq);

/* The slot's next acceptable request ID (manual_hw[slot] + 1; 2^63 when exhausted); 0 when the
 * state is not loaded or the slot is invalid. */
uint64_t dsp_dosing_manual_floor(const dsp_dosing_t *d, uint8_t slot);

/* User-confirmed refill (FR-023): restart-safe roll-forward over dosing -> notify ->
 * config -> dosing; dosing stays disabled until the last write. */
dsp_dosing_status_t dsp_dosing_refill(dsp_dosing_t *d, uint32_t capacity_ul,
                                      uint32_t remaining_ul, dsp_notify_clock_t clock);

/* Readiness. READY only if every gate except the clock is open (configuration,
 * calibration, limits, reservoir, no latch/refill/marker); *problems receives the gates
 * and pump problems that refused it. SERVICE_REQUIRED is always accepted. */
dsp_dosing_status_t dsp_dosing_set_readiness(dsp_dosing_t *d, dsp_readiness_t readiness,
                                             uint32_t *gates, uint32_t *pump_problems);

/* Re-observes the reservoir for the notification engine with the BLOCKED latch. */
dsp_notify_status_t dsp_dosing_observe_reservoir(dsp_dosing_t *d, dsp_notify_clock_t clock);

const dsp_dosing_state_t *dsp_dosing_state(const dsp_dosing_t *d);

/* ---- M1.9b: planning and safe-off --------------------------------------------------------- */

/* False while a persistent gate makes every automatic dose impossible until a user action:
 * not loaded, SERVICE_REQUIRED, BLOCKED, refill pending, manual marker, calibration/limits
 * or reservoir not configured (Q13: no dose wake-ups then). */
bool dsp_dosing_dose_wakes_allowed(dsp_dosing_t *d);

/* Earliest instant at which dsp_dosing_process_due() could start an automatic dose, as of
 * clock (valid only): an unclaimed occurrence still inside its due window that only the
 * minimum interval holds back (retried when the interval allows, if still inside the
 * window), or the next future occurrence. *at may be <= clock.now (due now). false when
 * dose wakes are not allowed (see above), the clock is not valid or nothing is scheduled.
 * Never actuates and never writes. */
bool dsp_dosing_next_due(dsp_dosing_t *d, const dsp_schedule_t *schedules, size_t n,
                         dsp_clock_t clock, dsp_utc_t *at);

/* force_off() then the is_off() confirmation. The port variant serves SAFE_OFF before the
 * service is initialized (the port is only passed through, never driven by the caller). */
bool dsp_dosing_port_safe_off(const dsp_pump_port_t *pump);
bool dsp_dosing_safe_off(dsp_dosing_t *d);

/* True after a storage failure in this wake (no further actuation until the next boot). */
bool dsp_dosing_degraded(const dsp_dosing_t *d);

/* Runtime for volume_ul at flow_ul_per_s, rounded up, overflow-safe. false if flow is 0,
 * volume is 0 or the result does not fit 32 bits. */
bool dsp_dosing_runtime_ms(uint32_t volume_ul, uint32_t flow_ul_per_s, uint32_t *runtime_ms);

/* Dose-bound report keys (non-zero). */
uint64_t dsp_dosing_key_automatic(const dsp_history_record_t *rec);
uint64_t dsp_dosing_key_manual(uint32_t seq, dsp_utc_t started_at, uint32_t volume_ul);

#ifdef __cplusplus
}
#endif

#endif /* DSP_DOSING_H */
