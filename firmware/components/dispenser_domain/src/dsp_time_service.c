#include "dsp_time_service.h"

#include <string.h>

static dsp_sync_status_t from_net(dsp_net_status_t st)
{
    return st == DSP_NET_ERR_NO_CREDENTIALS ? DSP_SYNC_ERR_NO_CREDENTIALS : DSP_SYNC_ERR_WIFI;
}

static void persist(dsp_time_service_t *svc, dsp_time_sync_report_t *r)
{
    if (!svc->tk->meta_dirty || svc->storage == NULL) {
        return;
    }
    r->persist = dsp_storage_save_time(svc->storage, &svc->tk->meta);
    if (r->persist == DSP_STORAGE_OK) {
        svc->tk->meta_dirty = false;
    }
}

dsp_time_sync_report_t dsp_time_service_run(dsp_time_service_t *svc, bool force, bool keep_wifi)
{
    dsp_time_sync_report_t r;
    memset(&r, 0, sizeof r);
    r.decision.status = DSP_SYNC_ERR_SOURCE;
    r.wifi = DSP_NET_OK;
    r.persist = DSP_STORAGE_OK;
    if (svc == NULL || svc->tk == NULL || svc->wifi == NULL || svc->source == NULL ||
        svc->source->sync == NULL || svc->clock == NULL || svc->clock->now == NULL ||
        svc->clock->set == NULL || svc->clock->mark_synced == NULL) {
        r.wifi = DSP_NET_ERR_ARG;
        return r;
    }
    dsp_utc_t now = svc->clock->now(svc->clock->ctx);
    persist(svc, &r); /* e.g. an expired hold noticed by evaluate(), or an earlier failure */
    if (!force && !dsp_timekeeper_sync_due(svc->tk, now)) {
        if (!keep_wifi) {
            dsp_wifi_shutdown(svc->wifi);
        }
        return r;
    }
    r.attempted = true;
    r.wifi = dsp_wifi_connect(svc->wifi);
    if (r.wifi != DSP_NET_OK) {
        r.decision = dsp_timekeeper_on_sync(svc->tk, from_net(r.wifi), NULL,
                                            svc->clock->now(svc->clock->ctx));
        dsp_wifi_shutdown(svc->wifi);
        return r;
    }
    dsp_time_sample_t sample;
    memset(&sample, 0, sizeof sample);
    uint32_t timeout = svc->sntp_timeout_ms != 0 ? svc->sntp_timeout_ms : DSP_SNTP_TIMEOUT_DEFAULT_MS;
    dsp_sync_status_t ss = svc->source->sync(svc->source->ctx, timeout, &sample);
    dsp_timekeeper_t before = *svc->tk; /* to roll back if the new state cannot be made durable */
    r.decision = dsp_timekeeper_on_sync(svc->tk, ss, ss == DSP_SYNC_OK ? &sample : NULL,
                                        svc->clock->now(svc->clock->ctx));
    if (r.decision.apply) {
        /* Durable before effective: the new metadata (last sync, high water, hold, dosing
         * floor) must be confirmed in storage BEFORE the clock is stepped and the
         * continuity marker refreshed. Otherwise a restart could re-synchronize from stale
         * metadata and lose the floor or the hold. */
        if (svc->storage != NULL) {
            r.persist = dsp_storage_save_time(svc->storage, &svc->tk->meta);
            if (r.persist != DSP_STORAGE_OK) {
                *svc->tk = before; /* nothing applied: clock, trust and metadata unchanged */
                r.decision = dsp_timekeeper_on_sync(svc->tk, DSP_SYNC_ERR_PERSIST, NULL,
                                                    svc->clock->now(svc->clock->ctx));
                if (!keep_wifi) {
                    dsp_wifi_shutdown(svc->wifi);
                }
                return r;
            }
            svc->tk->meta_dirty = false;
        }
        r.clock_set = svc->clock->set(svc->clock->ctx, r.decision.set_to);
        if (r.clock_set) {
            svc->clock->mark_synced(svc->clock->ctx, svc->tk->meta.last_sync_utc);
        } else {
            /* The system clock still holds the old time: do not trust it for dosing. The
             * continuity marker is not refreshed either, so the next wake starts UNKNOWN and
             * synchronizes again; the already persisted metadata (incl. the floor) applies. */
            svc->tk->trust = DSP_TRUST_UNKNOWN;
            svc->tk->reason = DSP_UNTRUSTED_TIME_BASE_LOST;
        }
    }
    persist(svc, &r);
    if (!keep_wifi) {
        dsp_wifi_shutdown(svc->wifi);
    }
    return r;
}
