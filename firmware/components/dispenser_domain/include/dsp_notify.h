#ifndef DSP_NOTIFY_H
#define DSP_NOTIFY_H

/*
 * Notification engine (milestone M1.5). Pure domain code: detects conditions, creates
 * deduplicated events, schedules reminders, keeps the bounded persistent queue and hands
 * pending notifications to a transport. It never activates Wi-Fi: delivery happens during
 * the next regular Wi-Fi session (approved: no extra wake-ups for notifications).
 *
 * Approved policy (2026-11-15):
 *  - Conditions (reservoir LOW, reservoir BLOCKED, battery LOW, device errors) create ONE
 *    event when an episode opens, then one reminder every 7 days (trusted clock only) while
 *    unresolved. Dosing errors are one-shot events. Clearing a condition ends the episode;
 *    its not yet delivered notifications are withdrawn (stale).
 *  - Queue: 16 entries in one atomic record. A new event replaces an older pending entry of
 *    the same condition, else uses a free slot, else evicts the oldest entry of LOWER
 *    severity; CRITICAL entries are never evicted. Otherwise the event is not stored: a
 *    condition stays latched with "raise pending", one-shot events are counted and later
 *    reported as DEVICE_ERROR/QUEUE_OVERFLOW. Nothing is lost silently.
 *  - Delivery: at-least-once. An entry keeps its identity (device, storage epoch, sequence)
 *    across retries; it is removed only after an explicit acknowledgement. Failed or
 *    uncertain attempts keep it queued for the next session.
 *  - Payload freshness (M1.5 review): an entry's payload (value/value2) follows the latest
 *    observation of its still active condition UNTIL its first transmission attempt; from
 *    then on it is frozen, so every retry with the same ID carries identical content. The
 *    refresh is RAM-only (flash wear). Before the FIRST transmission of an entry, its final
 *    payload and the freeze marker (attempts > 0, counting started attempts) are persisted
 *    in one write; if that write fails nothing is sent. So the freeze survives power loss
 *    at any point after the transport may have delivered the event. A restart before the
 *    first attempt falls back to the last persisted payload.
 *  - Durable before sent: an entry is transmitted only after it has been persisted, so a
 *    sequence number can never be reused for a different notification after a restart.
 *    Every persistence failure is returned as DSP_NOTIFY_ERR_STORAGE and stays visible via
 *    dsp_notify_storage_degraded() (to be exposed through BLE diagnostics later).
 *
 * Caller-owned state, single owner (the future dosing/connectivity service), no allocation.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dsp_config.h"
#include "dsp_notify_state.h"
#include "dsp_storage.h"
#include "dsp_timekeeping.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_NOTIFY_REMINDER_INTERVAL_S ((int64_t)7 * 86400) /* approved */
#define DSP_NOTIFY_LOW_RECOVERY_MARGIN_PCT 5 /* episode ends at threshold + 5 points (proposal) */
#define DSP_NOTIFY_CLOCK_FAILURES 4         /* failed sync attempts while UNKNOWN (proposal) */
/* "dsp-" + 12 hex device + "-" + 8 hex epoch + "-" + up to 10 digits + NUL */
#define DSP_NOTIFY_ID_BUF_LEN 40

typedef enum {
    DSP_NOTIFY_OK = 0,
    DSP_NOTIFY_ERR_ARG,
    DSP_NOTIFY_ERR_STORAGE,   /* state could not be loaded/persisted (kept in RAM, retried) */
    DSP_NOTIFY_QUEUE_FULL,    /* event could not be queued (latched or counted, see above) */
} dsp_notify_status_t;

/* Time as seen by the notification engine: trusted == clock not UNKNOWN (a rollback hold
 * does not make the time untrustworthy for timestamps). */
typedef struct {
    dsp_utc_t now;
    bool trusted;
} dsp_notify_clock_t;

typedef enum {
    DSP_BATTERY_UNKNOWN = 0, /* no valid measurement: nothing changes */
    DSP_BATTERY_OK,          /* valid measurement, not low (classified by the battery monitor) */
    DSP_BATTERY_LOW,         /* valid measurement, low */
} dsp_battery_status_t;

typedef struct {
    dsp_battery_status_t status;
    uint32_t millivolts; /* 0 = not provided; never invented */
} dsp_battery_obs_t;

typedef struct {
    uint16_t code;         /* DSP_DOSEERR_* */
    bool manual;           /* manual dose (else automatic) */
    uint32_t requested_ul;
} dsp_dosing_error_t;

typedef enum {
    DSP_TX_ACK = 0,   /* the receiver confirmed acceptance */
    DSP_TX_FAILED,    /* not accepted (or not sent): retry later */
    DSP_TX_UNCERTAIN, /* may have been accepted (e.g. timeout after sending): retry later
                         with the SAME id; the receiver deduplicates */
} dsp_tx_result_t;

typedef struct {
    void *ctx;
    dsp_tx_result_t (*send)(void *ctx, const dsp_note_t *note, uint32_t epoch);
} dsp_notify_transport_t;

typedef struct {
    size_t attempted;
    size_t acked;
    bool stopped_early; /* first non-acknowledged attempt ended the session */
    dsp_notify_status_t status;
} dsp_notify_delivery_report_t;

/* Caller-owned engine state. Treat as opaque. */
typedef struct {
    dsp_storage_t *st;
    dsp_notify_state_t s;
    bool loaded;
    bool dirty;      /* RAM state not yet persisted (a write failed) */
    uint32_t random; /* non-zero, used once to assign the storage epoch */
} dsp_notify_t;

/* Loads the "notify" record. random: a non-zero random value (platform RNG), used only if
 * no epoch has been assigned yet. */
dsp_notify_status_t dsp_notify_init(dsp_notify_t *n, dsp_storage_t *st, uint32_t random);

/* ---- Observations (each persists only when something changed) ---------------------- */

/* Reservoir estimate from the configuration (capacity 0 = unknown: no LOW condition) and
 * the dosing service's FR-022 blocked state. */
dsp_notify_status_t dsp_notify_observe_reservoir(dsp_notify_t *n, const dsp_config_t *cfg,
                                                 bool blocked, dsp_notify_clock_t clock);

dsp_notify_status_t dsp_notify_observe_battery(dsp_notify_t *n, dsp_battery_obs_t obs,
                                               dsp_notify_clock_t clock);

/* One-shot event (no deduplication across distinct failures, no reminders). The codes
 * START_FAILED, PUMP_FAILED and INTERRUPTED (the requested volume was already consumed)
 * also mark the reservoir estimate uncertain, in the same atomic write. */
dsp_notify_status_t dsp_notify_report_dosing_error(dsp_notify_t *n, const dsp_dosing_error_t *e,
                                                   dsp_notify_clock_t clock);

/* Device conditions DSP_COND_STORAGE / DSP_COND_TIME_SYNC / DSP_COND_CLOCK. */
dsp_notify_status_t dsp_notify_set_device_condition(dsp_notify_t *n, dsp_condition_t cond,
                                                    bool active, uint32_t detail,
                                                    dsp_notify_clock_t clock);

/* Maps the M1.4 timekeeper: TIME_SYNC while trusted but sync overdue (> 2 x interval);
 * CLOCK while UNKNOWN with >= DSP_NOTIFY_CLOCK_FAILURES failed sync attempts. */
dsp_notify_status_t dsp_notify_observe_time(dsp_notify_t *n, const dsp_timekeeper_t *tk,
                                            dsp_utc_t now);

/* Reservoir estimate uncertainty (set by dosing errors, cleared at refill). */
dsp_notify_status_t dsp_notify_set_estimate_uncertain(dsp_notify_t *n, bool uncertain);

/* User-confirmed refill, after dsp_reservoir_refill() succeeded with *cfg: clears the
 * uncertainty and the BLOCKED state (FR-023) and re-evaluates LOW, in one write. */
dsp_notify_status_t dsp_notify_on_refill(dsp_notify_t *n, const dsp_config_t *cfg,
                                         dsp_notify_clock_t clock);

/* True while the persistent state is not durable (a write failed or the record could not be
 * loaded). Future BLE diagnostics must expose this. */
bool dsp_notify_storage_degraded(const dsp_notify_t *n);

/* Reminders and deferred events (raise pending, overflow report). */
dsp_notify_status_t dsp_notify_tick(dsp_notify_t *n, dsp_notify_clock_t clock);

/* ---- Delivery ---------------------------------------------------------------------- */

size_t dsp_notify_pending_count(const dsp_notify_t *n);
int dsp_notify_highest_pending_severity(const dsp_notify_t *n); /* 0 = none */

/* True when notifications are pending: deliver them in the next regular Wi-Fi session
 * (approved: this never requests an extra wake-up). */
bool dsp_notify_delivery_requested(const dsp_notify_t *n);

/* Sends up to max_sends pending entries (severity first, then oldest), over an already
 * open session. Stops at the first non-acknowledged attempt. Writes once before each
 * entry's first transmission (freeze) and once at the end (ACK removals, attempts).
 * Sends nothing that is not durable: a failed write stops the session (ERR_STORAGE). */
dsp_notify_delivery_report_t dsp_notify_deliver(dsp_notify_t *n, const dsp_notify_transport_t *tx,
                                                size_t max_sends, dsp_notify_clock_t clock);

/* Stable event identity for the receiver: "dsp-<device 12 hex>-<epoch 8 hex>-<seq>". */
bool dsp_notify_format_id(uint64_t device_id, uint32_t epoch, uint32_t seq, char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* DSP_NOTIFY_H */
