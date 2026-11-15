#ifndef DSP_TIME_SERVICE_H
#define DSP_TIME_SERVICE_H

/*
 * One time-synchronization session (milestone M1.4), orchestrated over injected
 * interfaces: decide whether a sync is due, bring Wi-Fi up only then, sync once (bounded),
 * apply the timekeeper's decision, persist changed metadata, and shut Wi-Fi down unless
 * the caller keeps the session for further work (future HTTPS notifications).
 * Pure domain code; the single owner is the future dosing/connectivity service task.
 */

#include <stdbool.h>
#include <stdint.h>

#include "dsp_net.h"
#include "dsp_storage.h"
#include "dsp_timekeeping.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_SNTP_TIMEOUT_DEFAULT_MS 10000u

typedef struct {
    dsp_timekeeper_t *tk;
    dsp_wifi_manager_t *wifi;
    const dsp_time_source_t *source;
    const dsp_clock_port_t *clock;
    dsp_storage_t *storage; /* open storage; NULL = do not persist (diagnostics) */
    uint32_t sntp_timeout_ms;
} dsp_time_service_t;

typedef struct {
    bool attempted;              /* a sync was due (or forced) and was tried */
    dsp_net_status_t wifi;       /* result of opening the Wi-Fi session */
    dsp_sync_decision_t decision;
    bool clock_set;              /* the system clock was stepped */
    dsp_storage_status_t persist; /* DSP_STORAGE_OK when nothing needed persisting */
} dsp_time_sync_report_t;

/*
 * Runs one session. An accepted time is applied only after its metadata was confirmed in
 * storage (durable before effective); if that fails nothing is applied and the result is
 * DSP_SYNC_ERR_PERSIST (retry scheduled, trust unchanged).
 * force: try even if not due. keep_wifi: leave a session that this call
 * or an earlier one opened connected for the caller (who must call dsp_wifi_shutdown);
 * otherwise Wi-Fi is always shut down before returning. A failure never changes clock
 * trust and never blocks dosing (see dsp_timekeeping.h). Unpersisted metadata stays dirty
 * and is retried at the next session.
 */
dsp_time_sync_report_t dsp_time_service_run(dsp_time_service_t *svc, bool force, bool keep_wifi);

#ifdef __cplusplus
}
#endif

#endif /* DSP_TIME_SERVICE_H */
