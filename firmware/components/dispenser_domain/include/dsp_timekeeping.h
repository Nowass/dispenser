#ifndef DSP_TIMEKEEPING_H
#define DSP_TIMEKEEPING_H

/*
 * Clock trust and synchronization policy (milestone M1.4). Pure domain logic: the caller
 * supplies every clock reading; nothing here reads a clock or touches the network.
 *
 * Trust model (approved 2026-11-15, "evidence-based, no expiry"):
 *   UNKNOWN   no automatic dosing. Entered when the platform reports that the time base
 *             was not continuous (power-on/brownout reset, RTC continuity marker missing),
 *             when time is outside the plausible range, when no sync ever happened, or when
 *             the clock is earlier than the last accepted synchronization.
 *   SYNCED    continuous time base since an accepted sync less than one interval ago.
 *   HOLDOVER  continuous time base since the last accepted sync, sync now overdue. STILL
 *             TRUSTED, with no time limit: failed Wi-Fi/NTP never stops dosing. "Overdue"
 *             (beyond 2 x interval) is a diagnostic flag only.
 *
 * Correction policy at a sync (approved, "hold only beyond 7 days"); the new time is
 * applied as an immediate step:
 *   forward, any size            accepted; occurrences jumped over are missed: a forward
 *                                step > DSP_TIME_SIGNIFICANT_S raises the persisted,
 *                                monotonic floor not_before_utc to the step instant + 1 s;
 *                                nothing scheduled before the floor is executed (dsp_clock_t
 *                                not_before), even inside the due window, after restarts,
 *                                power loss and later corrections
 *   backward <= 7 days           accepted; the M1.2 watermark blocks claimed dates
 *   backward > 7 days            accepted, but automatic dosing is HELD until the clock
 *                                reaches (highest previously trusted time - 7 days); the
 *                                hold is persisted and also ends when a later sync jumps
 *                                past it
 *   initial sync (previous clock  accepted; the floor not_before_utc is raised to
 *   not trusted)                 max(floor, synchronized time + 1 s): nothing scheduled at
 *                                or before re-establishing time runs (approved: a missed
 *                                dose is preferable to a possible extra one)
 *   before 2025-01-01 / after 2099  rejected as implausible (treated as a failed sync)
 */

#include <stdbool.h>
#include <stdint.h>

#include "dsp_exec.h"
#include "dsp_time_meta.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_TIME_PLAUSIBLE_MIN ((dsp_utc_t)1735689600) /* 2025-01-01T00:00:00Z */
#define DSP_TIME_ROLLBACK_LIMIT_S ((int64_t)DSP_EXEC_RETENTION_DAYS_DEFAULT * 86400)
#define DSP_TIME_SIGNIFICANT_S 600         /* corrections above this are "large" (diagnostic) */
#define DSP_TIME_CONTINUITY_TOLERANCE_S 60 /* clock may not be earlier than last sync - this */
#define DSP_TIME_RETRY_UNKNOWN_MIN_S (15 * 60)
#define DSP_TIME_RETRY_UNKNOWN_MAX_S (6 * 3600)
#define DSP_TIME_RETRY_TRUSTED_MIN_S 3600 /* doubling, capped at the sync interval */

typedef enum {
    DSP_TRUST_UNKNOWN = 0,
    DSP_TRUST_SYNCED,
    DSP_TRUST_HOLDOVER,
} dsp_trust_t;

typedef enum {
    DSP_UNTRUSTED_NONE = 0,
    DSP_UNTRUSTED_NEVER_SYNCED,
    DSP_UNTRUSTED_TIME_BASE_LOST,
    DSP_UNTRUSTED_OUT_OF_RANGE,
    DSP_UNTRUSTED_BEHIND_LAST_SYNC,
} dsp_untrusted_reason_t;

/* Platform evidence at a wake. `now` is the raw system time (seconds); after a power-on
 * reset it may be near 1970 and is then simply implausible. */
typedef struct {
    dsp_utc_t now;
    bool time_base_continuous; /* clock ran continuously since it was set by the last sync
                                  (reset reason + RTC-retained marker; platform decides) */
} dsp_time_reading_t;

/* Small state the platform keeps across deep sleep (RTC memory). Lost on power-on. */
typedef struct {
    uint16_t consecutive_failures;
    dsp_utc_t next_attempt; /* raw system time of the next sync attempt; 0 = as soon as possible */
} dsp_time_retained_t;

typedef enum {
    DSP_SYNC_OK = 0,
    DSP_SYNC_ERR_NO_CREDENTIALS, /* Wi-Fi not provisioned */
    DSP_SYNC_ERR_WIFI,           /* connection failed / timed out */
    DSP_SYNC_ERR_TIMEOUT,        /* no NTP response in time */
    DSP_SYNC_ERR_IMPLAUSIBLE,    /* response outside 2025..2099 */
    DSP_SYNC_ERR_SOURCE,         /* other time-source failure */
    DSP_SYNC_ERR_PERSIST,        /* accepted, but the metadata could not be made durable:
                                    NOT applied (durable before effective) */
} dsp_sync_status_t;

/* A synchronization sample: source time and the local clock at the same moment. */
typedef struct {
    dsp_utc_t source_utc;
    dsp_utc_t clock_utc;
} dsp_time_sample_t;

typedef enum {
    DSP_CORRECTION_NONE = 0,      /* no accepted sync */
    DSP_CORRECTION_INITIAL,       /* previous clock untrusted: time established */
    DSP_CORRECTION_SMALL,         /* |offset| <= DSP_TIME_SIGNIFICANT_S */
    DSP_CORRECTION_FORWARD_LARGE,
    DSP_CORRECTION_BACKWARD_LARGE, /* within the 7-day limit: dosing continues */
    DSP_CORRECTION_ROLLBACK_HOLD,  /* beyond 7 days: automatic dosing held */
} dsp_correction_t;

typedef struct {
    bool apply;           /* step the system clock to set_to */
    dsp_utc_t set_to;
    dsp_correction_t kind;
    int64_t offset_s;     /* source - previous clock (0 if the previous clock was untrusted) */
    dsp_sync_status_t status;
} dsp_sync_decision_t;

/* Caller-owned timekeeper state. Treat as opaque. */
typedef struct {
    bool initialized;
    int64_t interval_s;
    dsp_time_meta_t meta;
    bool meta_dirty; /* meta changed: persist with dsp_storage_save_time() */
    dsp_trust_t trust;
    dsp_untrusted_reason_t reason;
    dsp_time_retained_t retained;
} dsp_timekeeper_t;

/* meta: loaded "time" record (or provisioning zeros); interval_h: 24..48 (configuration);
 * retained: state kept in RTC memory, or NULL when it was lost. Trust starts UNKNOWN. */
bool dsp_timekeeper_init(dsp_timekeeper_t *tk, const dsp_time_meta_t *meta, uint16_t interval_h,
                         const dsp_time_retained_t *retained);

/* Re-evaluates trust from platform evidence (call at every wake, after a restart). Ends an
 * expired rollback hold (meta becomes dirty). */
void dsp_timekeeper_evaluate(dsp_timekeeper_t *tk, dsp_time_reading_t reading);

/* Clock snapshot for the M1.2 coordinator: valid only when trusted, plausible and not under
 * a rollback hold. */
dsp_clock_t dsp_timekeeper_clock(const dsp_timekeeper_t *tk, dsp_utc_t now);

bool dsp_timekeeper_hold_active(const dsp_timekeeper_t *tk, dsp_utc_t now);
bool dsp_timekeeper_sync_due(const dsp_timekeeper_t *tk, dsp_utc_t now);
dsp_utc_t dsp_timekeeper_next_sync(const dsp_timekeeper_t *tk); /* raw system time */
bool dsp_timekeeper_sync_overdue(const dsp_timekeeper_t *tk, dsp_utc_t now);

/*
 * Processes a synchronization attempt. status DSP_SYNC_OK requires sample. `now` is the
 * clock after the attempt (used for retry scheduling on failure). A failure never changes
 * trust. Returns what the platform must do (apply == true: step the clock to set_to,
 * then refresh the continuity marker with the new last_sync).
 */
dsp_sync_decision_t dsp_timekeeper_on_sync(dsp_timekeeper_t *tk, dsp_sync_status_t status,
                                           const dsp_time_sample_t *sample, dsp_utc_t now);

/* Changes the sync interval (configuration, 24..48 h) of a running timekeeper. A trusted
 * clock's next attempt is brought forward if the shorter interval makes it due earlier.
 * Trust classification follows at the next dsp_timekeeper_evaluate(). */
bool dsp_timekeeper_set_interval(dsp_timekeeper_t *tk, uint16_t interval_h);

/* ---- Phone-provided time (M1.10b.1, decision D10) -------------------------------------
 *
 * The owner's phone may supply the time over an authenticated BLE session (FR-032
 * fallback). It is applied through the same M1.4 correction rules as an NTP sample (the
 * initial-sync floor, the forward-step floor, high water), with these restrictions:
 *   - uncertainty above DSP_PHONE_TIME_MAX_UNCERTAINTY_MS        -> REJECTED_UNCERTAIN
 *   - outside 2025-01-01 .. DSP_UTC_MAX                           -> REJECTED_IMPLAUSIBLE
 *   - a rollback hold is recorded (active, or not yet provably over because the clock is
 *     untrusted)                                                  -> HOLD, nothing applied:
 *     only NTP or the NTP-established clock reaching the hold ends it; phone time could
 *     otherwise move the clock past the hold and end it indirectly
 *   - the clock is SYNCED (an accepted sync within the interval)  -> IGNORED_NTP_PREFERRED
 *   - more than 7 days behind the highest trusted time            -> HOLD, nothing applied:
 *     phone time never creates a hold either (an NTP sample would be applied with a hold)
 * Accepted only while the clock is UNKNOWN or in HOLDOVER. The NTP retry schedule is left
 * unchanged (NTP stays preferred and keeps its attempts). On ACCEPTED *out is the sync
 * decision to make durable and apply exactly like an NTP one (dsp_time_apply); for every
 * other result out->apply is false and the timekeeper is unchanged. clock_utc is the
 * device clock when the request is processed.
 */
#define DSP_PHONE_TIME_MAX_UNCERTAINTY_MS 2000u

typedef enum { /* values = BLE protocol v1 SET_TIME `decision` */
    DSP_PHONE_TIME_ACCEPTED = 0,
    DSP_PHONE_TIME_REJECTED_IMPLAUSIBLE = 1,
    DSP_PHONE_TIME_REJECTED_UNCERTAIN = 2,
    DSP_PHONE_TIME_IGNORED_NTP_PREFERRED = 3,
    DSP_PHONE_TIME_HOLD = 4,
} dsp_phone_time_decision_t;

dsp_phone_time_decision_t dsp_timekeeper_on_phone_time(dsp_timekeeper_t *tk, int64_t phone_utc_ms,
                                                       uint32_t uncertainty_ms, dsp_utc_t clock_utc,
                                                       dsp_sync_decision_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DSP_TIMEKEEPING_H */
