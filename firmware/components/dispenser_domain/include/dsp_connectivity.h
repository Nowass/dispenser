#ifndef DSP_CONNECTIVITY_H
#define DSP_CONNECTIVITY_H

/*
 * Connectivity session (milestone M1.7): one bounded Wi-Fi session on an EXISTING wake:
 * time synchronization, then notification delivery over HTTPS, then Wi-Fi off. Pure
 * domain code over the M1.4 time service, the M1.5 engine and the M1.7 HTTPS transport.
 * The caller (future dosing/connectivity service) decides when the device is awake and
 * passes the dosing/BLE state; this module never schedules a wake-up.
 *
 * Approved policy (D1-B, 2026-11-16):
 *  - A session starts only if (a) the regular time sync is due (M1.4 schedule, including
 *    its backoff), or (b) a CRITICAL notification is pending, the last notification-
 *    initiated attempt is at least DSP_CONN_NOTIFY_MIN_INTERVAL_S ago and the M1.4 Wi-Fi
 *    backoff is not active (no failed sync pending a retry). WARNING notifications only
 *    travel with regular sessions. No delivery latency is promised.
 *  - Never while a dose runs or a dose permit is outstanding, never while BLE is active,
 *    and never if the next dose could fall inside the worst-case session duration plus
 *    DSP_CONN_DOSE_MARGIN_S.
 *  - D4: HTTPS only with a trusted clock (timekeeper not UNKNOWN) and a plausible time;
 *    otherwise nothing is sent and the queue is untouched (the CLOCK condition is reported
 *    by the notification engine; it cannot itself be e-mailed without trusted time).
 *  - Wi-Fi is always shut down before returning.
 *
 * Time-only mode (bench MVP, CONFIG_DSP_APP_WIFI_TIME): https == NULL. The session runs only
 * for a due regular time sync (a), never for notifications (b): no HTTP client exists, the
 * notification queue is never delivered from and stays untouched; the dose guard's worst case
 * contains no HTTPS budget. Time synchronization, trust and backoff are unchanged.
 */

#include <stdbool.h>
#include <stdint.h>

#include "dsp_https_transport.h"
#include "dsp_notify.h"
#include "dsp_time_service.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_CONN_NOTIFY_MIN_INTERVAL_S 3600       /* approved: at most one attempt per hour */
#define DSP_CONN_HTTPS_BUDGET_MS_DEFAULT 60000u   /* approved session budget */
#define DSP_CONN_DOSE_MARGIN_S 120                /* extra distance to the next dose */
#define DSP_CONN_MAX_SENDS DSP_NOTIFY_QUEUE_CAPACITY

/* Kept by the platform in RTC memory (lost on power-on: then one attempt is allowed). */
typedef struct {
    dsp_utc_t last_notify_attempt; /* raw time of the last notification-initiated attempt */
} dsp_connectivity_retained_t;

/* Device state supplied by the caller at the moment of the decision. */
typedef struct {
    bool dosing_active;      /* a dose is being executed or handled */
    bool permit_outstanding; /* a dose permit exists (M1.5 dsp_dose_permit_t) */
    bool ble_active;         /* a BLE session is running */
    bool next_dose_known;    /* next_dose_at is valid (dsp_wake_next_dose) */
    dsp_utc_t next_dose_at;
} dsp_connectivity_guard_t;

typedef enum {
    DSP_CONN_ERR_ARG = 0,
    DSP_CONN_SKIPPED_GUARD,     /* dosing, permit or BLE active */
    DSP_CONN_SKIPPED_DOSE_SOON, /* the next dose is too close */
    DSP_CONN_SKIPPED_NOT_DUE,   /* no sync due and no eligible CRITICAL notification */
    DSP_CONN_WIFI_FAILED,       /* Wi-Fi could not be connected (backoff recorded) */
    DSP_CONN_TIME_UNTRUSTED,    /* connected, but no trusted time: nothing sent */
    DSP_CONN_NO_TOKEN,          /* API token not provisioned: nothing sent */
    DSP_CONN_PORT_FAILED,       /* HTTP port could not be opened: nothing sent */
    DSP_CONN_DONE,              /* session completed (delivery may have stopped early) */
} dsp_conn_outcome_t;

typedef enum {
    DSP_CONN_REASON_NONE = 0,
    DSP_CONN_REASON_SYNC,     /* regular sync due */
    DSP_CONN_REASON_CRITICAL, /* CRITICAL notification on an existing wake */
} dsp_conn_reason_t;

typedef struct {
    dsp_conn_outcome_t outcome;
    dsp_conn_reason_t reason;
    dsp_time_sync_report_t sync;
    bool delivery_ran;
    dsp_notify_delivery_report_t delivery;
} dsp_connectivity_report_t;

typedef struct {
    dsp_time_service_t *time;               /* M1.4 (timekeeper, Wi-Fi, SNTP, clock, storage) */
    dsp_notify_t *notify;                   /* M1.5 engine (loaded) */
    dsp_https_transport_t *https;           /* M1.7 transport (initialized); NULL = time only */
    dsp_connectivity_retained_t *retained;  /* RTC-retained */
    uint32_t https_budget_ms;               /* 0 = DSP_CONN_HTTPS_BUDGET_MS_DEFAULT */
} dsp_connectivity_t;

/* Worst-case duration of one session in seconds (Wi-Fi attempts x timeout + SNTP timeout
 * + HTTPS budget unless time-only, rounded up), used for the dose guard. -1 on invalid input. */
int64_t dsp_connectivity_worst_case_s(const dsp_connectivity_t *c);

/* Runs at most one session (see policy above). */
dsp_connectivity_report_t dsp_connectivity_run(dsp_connectivity_t *c,
                                               const dsp_connectivity_guard_t *guard);

#ifdef __cplusplus
}
#endif

#endif /* DSP_CONNECTIVITY_H */
