#include "dsp_connectivity.h"

#include <string.h>

static uint32_t budget_ms(const dsp_connectivity_t *c)
{
    return c->https_budget_ms != 0 ? c->https_budget_ms : DSP_CONN_HTTPS_BUDGET_MS_DEFAULT;
}

static bool valid(const dsp_connectivity_t *c)
{
    return c != NULL && c->time != NULL && c->time->tk != NULL && c->time->tk->initialized &&
           c->time->wifi != NULL && c->time->wifi->initialized && c->time->clock != NULL &&
           c->time->clock->now != NULL && c->notify != NULL && c->notify->loaded &&
           (c->https == NULL || c->https->initialized) && c->retained != NULL; /* NULL: time only */
}

int64_t dsp_connectivity_worst_case_s(const dsp_connectivity_t *c)
{
    if (c == NULL || c->time == NULL || c->time->wifi == NULL) {
        return -1;
    }
    uint64_t ms = (uint64_t)c->time->wifi->cfg.max_attempts * c->time->wifi->cfg.connect_timeout_ms;
    ms += c->time->sntp_timeout_ms != 0 ? c->time->sntp_timeout_ms : DSP_SNTP_TIMEOUT_DEFAULT_MS;
    if (c->https != NULL) {
        ms += budget_ms(c); /* time-only sessions never open HTTPS */
    }
    return (int64_t)((ms + 999) / 1000);
}

static bool time_trusted(const dsp_timekeeper_t *tk, dsp_utc_t now)
{
    return tk->trust != DSP_TRUST_UNKNOWN && now >= DSP_TIME_PLAUSIBLE_MIN && now <= DSP_UTC_MAX;
}

dsp_connectivity_report_t dsp_connectivity_run(dsp_connectivity_t *c,
                                               const dsp_connectivity_guard_t *guard)
{
    dsp_connectivity_report_t r;
    memset(&r, 0, sizeof r);
    r.outcome = DSP_CONN_ERR_ARG;
    if (!valid(c) || guard == NULL) {
        return r;
    }
    dsp_timekeeper_t *tk = c->time->tk;
    dsp_utc_t now = c->time->clock->now(c->time->clock->ctx);

    /* 1. Never interfere with dosing or BLE. */
    if (guard->dosing_active || guard->permit_outstanding || guard->ble_active) {
        r.outcome = DSP_CONN_SKIPPED_GUARD;
        return r;
    }
    int64_t worst = dsp_connectivity_worst_case_s(c);
    if (guard->next_dose_known &&
        (guard->next_dose_at <= now || guard->next_dose_at - now <= worst + DSP_CONN_DOSE_MARGIN_S)) {
        r.outcome = DSP_CONN_SKIPPED_DOSE_SOON;
        return r;
    }

    /* 2. Reason: regular sync due, or an eligible CRITICAL notification (D1-B). */
    bool sync_due = dsp_timekeeper_sync_due(tk, now);
    if (sync_due) {
        r.reason = DSP_CONN_REASON_SYNC;
    } else {
        bool critical = dsp_notify_highest_pending_severity(c->notify) >= DSP_SEV_CRITICAL;
        dsp_utc_t last = c->retained->last_notify_attempt;
        /* A backward clock step (now < last) allows one attempt and restarts the interval. */
        bool interval_ok = last == 0 || now < last || now - last >= DSP_CONN_NOTIFY_MIN_INTERVAL_S;
        bool backoff = tk->retained.consecutive_failures > 0; /* retry not yet due */
        /* time-only (no HTTPS): notifications never start a session */
        if (critical && interval_ok && !backoff && c->https != NULL) {
            r.reason = DSP_CONN_REASON_CRITICAL;
        }
    }
    if (r.reason == DSP_CONN_REASON_NONE) {
        r.outcome = DSP_CONN_SKIPPED_NOT_DUE;
        return r;
    }
    if (r.reason == DSP_CONN_REASON_CRITICAL) {
        c->retained->last_notify_attempt = now; /* counts even if Wi-Fi then fails */
    }

    /* 3. Wi-Fi + time sync (a CRITICAL session also synchronizes: D4 needs trusted time,
     *    and a failure feeds the M1.4 backoff like any other attempt). */
    r.sync = dsp_time_service_run(c->time, r.reason == DSP_CONN_REASON_CRITICAL, true);
    if (r.sync.wifi != DSP_NET_OK || dsp_wifi_state(c->time->wifi) != DSP_WIFI_CONNECTED) {
        dsp_wifi_shutdown(c->time->wifi);
        r.outcome = DSP_CONN_WIFI_FAILED;
        return r;
    }
    now = c->time->clock->now(c->time->clock->ctx);
    dsp_notify_clock_t nclock = {now, tk->trust != DSP_TRUST_UNKNOWN};
    /* Conditions follow the new time state before delivery (e.g. TIME_SYNC resolved). */
    (void)dsp_notify_observe_time(c->notify, tk, now);

    /* 4. D4: trusted time is required for HTTPS (certificate dates are always checked). */
    if (!time_trusted(tk, now)) {
        dsp_wifi_shutdown(c->time->wifi);
        r.outcome = DSP_CONN_TIME_UNTRUSTED;
        return r;
    }

    /* 5. Notifications (all pending ones travel, CRITICAL first, within the budget).
     *    Time-only: none (no HTTP client); the queue stays untouched. */
    r.outcome = DSP_CONN_DONE;
    if (c->https != NULL && dsp_notify_pending_count(c->notify) > 0) {
        if (!dsp_https_ready(c->https)) {
            r.outcome = DSP_CONN_NO_TOKEN;
        } else if (!dsp_https_begin(c->https, budget_ms(c))) {
            r.outcome = DSP_CONN_PORT_FAILED;
        } else {
            dsp_notify_transport_t tx = dsp_https_transport(c->https);
            r.delivery = dsp_notify_deliver(c->notify, &tx, DSP_CONN_MAX_SENDS, nclock);
            r.delivery_ran = true;
            dsp_https_end(c->https);
        }
    }
    dsp_wifi_shutdown(c->time->wifi);
    return r;
}
