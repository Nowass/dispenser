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
} dsp_dosing_t;

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

/* Manual dose (future BLE command): same gates and limits; refused while BLOCKED (Q7);
 * never touches automatic history; durable in-flight marker. */
dsp_dosing_result_t dsp_dosing_manual(dsp_dosing_t *d, uint32_t volume_ul, dsp_clock_t clock);

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
