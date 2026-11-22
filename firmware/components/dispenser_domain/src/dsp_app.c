#include "dsp_app.h"
#include "dsp_app_sec.h"

#include <string.h>

#include "dsp_bleproto.h"
#include "dsp_time_service.h"

_Static_assert(DSP_APP_SCHEDULE_PAGE == DSP_BLE_LIST_MAX, "schedule page = protocol list size");
_Static_assert(DSP_CONFIG_CALIBRATED_AT_MIN == DSP_TIME_PLAUSIBLE_MIN,
               "calibration floor = timekeeper plausibility floor");
_Static_assert(DSP_SCHEDULE_NAME_MAX_BYTES <= DSP_BLE_STR_MAX, "names fit a protocol string");

/* ---- Helpers --------------------------------------------------------------------------- */

/* M1.10b.3 ownership (defined in the ownership section below). */
static void owner_load(dsp_app_t *app);
static void owner_wake_start(dsp_app_t *app);
static void owner_check_clock(dsp_app_t *app);
static void owner_session_start(dsp_app_t *app);
static void owner_session_end(dsp_app_t *app);
static void owner_button(dsp_app_t *app, const dsp_button_event_t *ev);
static void owner_abort_confirm(dsp_app_t *app);
static bool owner_confirm_pending(const dsp_app_t *app);
static void owner_confirm_tick(dsp_app_t *app);
static void conn_end(dsp_app_t *app);
static bool issued_ok(const dsp_app_t *app, const dsp_app_auth_t *auth);

static void trace(dsp_app_t *app, dsp_app_stage_t stage)
{
    if (app->rep.trace_len < DSP_APP_TRACE_MAX) {
        app->rep.trace[app->rep.trace_len++] = (uint8_t)stage;
    }
}

static dsp_utc_t raw_now(const dsp_app_t *app)
{
    return app->cfg->clock->now(app->cfg->clock->ctx);
}

static dsp_clock_t dose_clock(const dsp_app_t *app)
{
    return dsp_timekeeper_clock(&app->tk, raw_now(app));
}

static dsp_clock_t dose_clock_read(void *ctx)
{
    return dose_clock((const dsp_app_t *)ctx);
}

static dsp_notify_clock_t notify_clock(const dsp_app_t *app)
{
    dsp_notify_clock_t c = {raw_now(app), app->tk.trust != DSP_TRUST_UNKNOWN};
    return c;
}

static const dsp_schedule_t *schedules(const dsp_app_t *app, size_t *n)
{
    const dsp_schedule_set_t *set = dsp_schedule_repo_set(&app->repo);
    *n = set != NULL ? set->count : 0;
    return set != NULL ? set->schedules : NULL;
}

/* Dosing may still act in this wake (persistent gates are checked by the Dosing Service). */
static bool dosing_usable(const dsp_app_t *app)
{
    return app->loaded && !app->inhibited && !app->pump_fault && !app->storage_fault &&
           !dsp_dosing_degraded(&app->dosing);
}

static bool next_due(dsp_app_t *app, dsp_utc_t *at)
{
    if (!app->loaded) {
        return false;
    }
    size_t n = 0;
    const dsp_schedule_t *s = schedules(app, &n);
    return dsp_dosing_next_due(&app->dosing, s, n, dose_clock(app), at);
}

static bool dose_due_now(dsp_app_t *app)
{
    dsp_utc_t at = 0;
    return dosing_usable(app) && next_due(app, &at) && at <= raw_now(app);
}

/* ---- Stages ---------------------------------------------------------------------------- */

static bool load(dsp_app_t *app)
{
    const dsp_app_config_t *cfg = app->cfg;
    /* Phase C: the event history is independent of the layout (never fails the load). */
    (void)dsp_evlog_open(&app->evlog, cfg->kv);
    app->rep.open_status = dsp_storage_open(&app->st, cfg->kv, cfg->tz, true);
    if (app->rep.open_status != DSP_STORAGE_OK) {
        return false;
    }
    dsp_config_t config;
    dsp_time_meta_t meta;
    if (dsp_schedule_repo_load(&app->repo, &app->st) != DSP_STORAGE_OK ||
        dsp_storage_load_config(&app->st, &config) != DSP_STORAGE_OK ||
        dsp_storage_load_time(&app->st, &meta) != DSP_STORAGE_OK) {
        return false;
    }
    const dsp_time_retained_t *ret = app->rep.retained_valid ? &app->retained.time : NULL;
    if (!dsp_timekeeper_init(&app->tk, &meta, config.time_sync_interval_h, ret) &&
        !dsp_timekeeper_init(&app->tk, &meta, config.time_sync_interval_h, NULL)) {
        return false;
    }
    dsp_storage_history_store(&app->st, &app->hist);
    dsp_exec_config_t ecfg = {&app->hist, cfg->tz, DSP_EXEC_DUE_WINDOW_DEFAULT_S,
                              DSP_EXEC_RETENTION_DAYS_DEFAULT};
    if (dsp_exec_init(&app->ex, &ecfg) != DSP_EXEC_OK ||
        dsp_notify_init(&app->notify, &app->st, cfg->random != 0 ? cfg->random : 1u) != DSP_NOTIFY_OK ||
        dsp_dosing_init(&app->dosing, &app->st, &app->ex, &app->notify, cfg->pump, cfg->tz,
                        cfg->hw_max_runtime_ms) != DSP_DOSING_OK) {
        return false;
    }
    dsp_notify_set_mask(&app->notify, config.notify_mask); /* before recovery may report */
    dsp_dosing_set_admission_limit(&app->dosing, cfg->manual_decisions_per_day);
    dsp_dosing_set_event_log(&app->dosing, app->evlog.open ? &app->evlog : NULL,
                             cfg->event_pump_flags);
    dsp_time_service_t svc = {&app->tk, cfg->wifi, cfg->ntp, cfg->clock, &app->st, cfg->sntp_timeout_ms};
    app->time_svc = svc;
    dsp_connectivity_t conn = {&app->time_svc, &app->notify, cfg->https, &app->retained.conn,
                               cfg->https_budget_ms};
    app->conn = conn;
    return true;
}

static void stage_recover(dsp_app_t *app)
{
    trace(app, DSP_STAGE_RECOVER);
    /* Timestamps for the reports: a side-effect-free evaluation of a copy (CLOCK applies it). */
    dsp_timekeeper_t tk = app->tk;
    dsp_timekeeper_evaluate(&tk, app->cfg->time->reading(app->cfg->time->ctx, tk.meta.last_sync_utc));
    dsp_notify_clock_t c = {raw_now(app), tk.trust != DSP_TRUST_UNKNOWN};
    app->rep.recover_status = dsp_dosing_recover(&app->dosing, c, &app->rep.recovery);
}

static void stage_clock_eval(dsp_app_t *app)
{
    dsp_timekeeper_evaluate(&app->tk,
                            app->cfg->time->reading(app->cfg->time->ctx, app->tk.meta.last_sync_utc));
}

static void stage_clock(dsp_app_t *app)
{
    trace(app, DSP_STAGE_CLOCK);
    stage_clock_eval(app);
}

static void battery_observe(dsp_app_t *app);

static void stage_observe(dsp_app_t *app)
{
    trace(app, DSP_STAGE_OBSERVE);
    (void)dsp_notify_observe_time(&app->notify, &app->tk, raw_now(app));
    (void)dsp_notify_tick(&app->notify, notify_clock(app));
    (void)dsp_dosing_observe_reservoir(&app->dosing, notify_clock(app));
    battery_observe(app);
}

/* ---- Battery telemetry (runtime only) ---------------------------------------------------- */

static uint64_t mono_or_0(const dsp_app_t *app)
{
    return app->cfg->mono != NULL && app->cfg->mono->now_ms != NULL
               ? app->cfg->mono->now_ms(app->cfg->mono->ctx)
               : 0;
}

/* One measurement through the platform port (never while the pump runs: every caller runs in
 * the application task outside a pump run). */
static void battery_measure(dsp_app_t *app)
{
    const dsp_battery_port_t *b = app->cfg->battery;
    if (b == NULL || b->measure == NULL) {
        return;
    }
    app->batt = dsp_batt_sanitize(b->measure(b->ctx));
    app->batt_measured = true;
    app->batt_ms = mono_or_0(app);
    app->rep.battery_reads++;
    app->rep.battery = app->batt;
}

/* Effective BATTERY_LOW thresholds: the configured pair when low < recover inside the credible
 * window, else the defaults. */
static void battery_thresholds(const dsp_app_t *app, uint32_t *low, uint32_t *recover)
{
    uint32_t l = app->cfg->battery_low_mv;
    uint32_t r = app->cfg->battery_recover_mv;
    if (l == 0 && r == 0) {
        l = DSP_APP_BATTERY_LOW_MV_DEFAULT;
        r = DSP_APP_BATTERY_RECOVER_MV_DEFAULT;
    }
    if (l < DSP_BATT_MIN_MV || r > DSP_BATT_MAX_MV || l >= r) {
        l = DSP_APP_BATTERY_LOW_MV_DEFAULT;
        r = DSP_APP_BATTERY_RECOVER_MV_DEFAULT;
    }
    *low = l;
    *recover = r;
}

/* Hysteresis on the measured voltage (never the app's percentage estimate): with no open episode
 * LOW below the low threshold, else OK; with an open episode OK at or above the recovery
 * threshold, else still LOW (refreshes the not yet sent payload in RAM only). Anything but a
 * VALID measurement is UNKNOWN: nothing changes, and no voltage is ever invented. */
static dsp_battery_obs_t battery_classify(const dsp_app_t *app, bool episode)
{
    dsp_battery_obs_t o = {DSP_BATTERY_UNKNOWN, 0};
    if (!app->batt_measured || app->batt.state != DSP_BATT_VALID || app->batt.battery_mv == 0) {
        return o;
    }
    uint32_t low = 0, recover = 0;
    battery_thresholds(app, &low, &recover);
    uint32_t mv = app->batt.battery_mv;
    o.millivolts = mv;
    o.status = episode ? (mv >= recover ? DSP_BATTERY_OK : DSP_BATTERY_LOW)
                       : (mv < low ? DSP_BATTERY_LOW : DSP_BATTERY_OK);
    return o;
}

/* BATTERY_LOW through the existing notification engine (episode, 7-day reminder, queue, IDs,
 * delivery with the next regular sync session). Runs in OBSERVE, before any pump run and before
 * Wi-Fi. A change of the episode state (opening or closing) needs a second, independent
 * measurement that agrees; otherwise the wake reports UNKNOWN and nothing changes, so one
 * disturbed measurement can neither open nor close an episode. The engine writes only when the
 * episode state changes. */
static void battery_observe(dsp_app_t *app)
{
    bool episode = (app->notify.s.cond[DSP_COND_BATTERY_LOW].flags & DSP_COND_F_ACTIVE) != 0;
    dsp_battery_obs_t o = battery_classify(app, episode);
    bool change = (o.status == DSP_BATTERY_LOW && !episode) || (o.status == DSP_BATTERY_OK && episode);
    if (change) {
        battery_measure(app); /* confirmation (no pump has run in this wake yet) */
        dsp_battery_obs_t c = battery_classify(app, episode);
        if (c.status == o.status) {
            o = c; /* the confirming voltage is reported */
        } else {
            o.status = DSP_BATTERY_UNKNOWN;
            o.millivolts = 0;
        }
    }
    app->rep.battery_obs = (uint8_t)o.status;
    if (o.status != DSP_BATTERY_UNKNOWN) {
        (void)dsp_notify_observe_battery(&app->notify, o, notify_clock(app));
    }
}

/* The end of a pump run attempt: no battery measurement within DSP_APP_BATTERY_PUMP_QUIET_MS. */
static void pump_mark(dsp_app_t *app)
{
    app->pump_ran = true;
    app->pump_ms = mono_or_0(app);
}

static void stage_dose(dsp_app_t *app)
{
    trace(app, DSP_STAGE_DOSE);
    size_t n = 0;
    const dsp_schedule_t *s = schedules(app, &n);
    dsp_dosing_clock_t clock = {app, dose_clock_read};
    for (size_t pass = 0; pass < DSP_APP_DOSE_PASSES && dose_due_now(app); pass++) {
        dsp_dosing_due_report_t r = dsp_dosing_process_due(&app->dosing, s, n, &clock);
        app->rep.dose_passes++;
        app->rep.dosed += r.dosed;
        app->rep.pump_failures += r.pump_failures;
        if (r.dosed > 0 || r.pump_failures > 0) {
            pump_mark(app);
        }
        if (r.pump_failures > 0) {
            app->pump_fault = true; /* no further activation in this wake */
        }
        if (r.stop != DSP_DOSING_OK || (r.dosed == 0 && r.pump_failures == 0)) {
            break; /* stopped, or no progress: never spin */
        }
    }
    app->rep.dosing_degraded = dsp_dosing_degraded(&app->dosing);
    app->rep.pump_fault = app->pump_fault;
}

static void stage_connect(dsp_app_t *app)
{
    trace(app, DSP_STAGE_CONNECT);
    /* https may be NULL: time-only sessions (CONFIG_DSP_APP_WIFI_TIME, dsp_connectivity.h) */
    if (app->cfg->wifi == NULL || app->cfg->ntp == NULL || app->rep.conn_runs >= 2) {
        return;
    }
    dsp_connectivity_guard_t guard;
    memset(&guard, 0, sizeof guard);
    guard.dosing_active = false;      /* sequential: no dose runs during this stage */
    guard.permit_outstanding = false; /* permits never outlive a dsp_dosing_* call */
    guard.ble_active = app->ble_active;
    guard.next_dose_known = next_due(app, &guard.next_dose_at);
    dsp_connectivity_report_t r = dsp_connectivity_run(&app->conn, &guard);
    app->rep.conn[app->rep.conn_runs++] = r.outcome;
    owner_check_clock(app); /* an NTP step clears the ownership overlays */
}

/* ---- Service mode ------------------------------------------------------------------------ */

/* Physical presence for the service window and the security layer: a button wake, or (T7,
 * build-only bring-up) a power-on wake when the build enables it. */
static bool presence_wake(const dsp_app_t *app)
{
    return app->rep.cause == DSP_CAUSE_BUTTON ||
           (app->cfg->service_on_power_on && app->rep.cause == DSP_CAUSE_POWER_ON);
}

/* M1.10c.2a: a registered security session takes over every frame of the connection. */
static bool sec_session(const dsp_app_t *app)
{
    return app->cfg->security != NULL && app->cfg->security->on_frame != NULL;
}

static void ble_suspend(dsp_app_t *app)
{
    if (app->ble_active) {
        const dsp_app_security_t *sec = app->cfg->security;
        if (app->ss.conn_active && sec_session(app) && sec->on_suspend != NULL) {
            sec->on_suspend(sec->ctx, app); /* best effort, before the connection is closed */
        }
        app->cfg->ble->suspend(app->cfg->ble->ctx);
        conn_end(app); /* the connection is closed: its security session ends */
        app->ble_active = false;
        app->rep.ble_suspends++;
    }
}

/* Service-session lifetime on the monotonic clock (milliseconds): app->ss (dsp_app_session_t),
 * shared by the session loop and the operations that suspend BLE for the pump. */

static uint64_t mono_ms(const dsp_app_t *app)
{
    return app->cfg->mono->now_ms(app->cfg->mono->ctx);
}

static bool ble_connected(const dsp_app_t *app)
{
    return app->ble_active && app->cfg->ble->connected != NULL &&
           app->cfg->ble->connected(app->cfg->ble->ctx);
}

/* Resumes after a pump run only while the deadline permits. A connection that the suspension
 * closed restarts the idle timeout (the client has to reconnect). */
static bool ble_resume(dsp_app_t *app, dsp_app_session_t *ss, bool was_connected)
{
    uint64_t now = mono_ms(app);
    if (now < ss->start_ms) {
        ss->end = DSP_BLE_END_CLOCK;
        return false;
    }
    if (now >= ss->max_ms) {
        ss->end = DSP_BLE_END_MAX;
        return false;
    }
    if (!was_connected && now >= ss->idle_deadline_ms) {
        ss->end = DSP_BLE_END_IDLE;
        return false;
    }
    if (!app->cfg->ble->resume(app->cfg->ble->ctx)) {
        ss->end = DSP_BLE_END_RESUME;
        return false;
    }
    app->ble_active = true;
    if (was_connected) {
        ss->idle_deadline_ms = now + ss->idle_ms;
        ss->was_connected = false;
    }
    return true;
}

static void q_lock(const dsp_app_t *app)
{
    if (app->cfg->sync != NULL) {
        app->cfg->sync->lock(app->cfg->sync->ctx);
    }
}

static void q_unlock(const dsp_app_t *app)
{
    if (app->cfg->sync != NULL) {
        app->cfg->sync->unlock(app->cfg->sync->ctx);
    }
}

/* Pops one item and copies the connection snapshot in the same critical section: a frame of
 * generation g is posted only after the transport reported g, so the copy is never older than
 * the popped frame (the application reconciles with it before handling the frame). */
static bool q_pop(dsp_app_t *app, dsp_app_link_t *snap)
{
    q_lock(app);
    *snap = app->lnk;
    bool got = app->q_count > 0;
    if (got) {
        app->rx = app->q[app->q_head];
        app->q_head = (app->q_head + 1) % DSP_APP_CMDQ_CAPACITY;
        app->q_count--;
    }
    q_unlock(app);
    return got;
}

/* Opens (session start) or closes and empties (session end) the queue for producers. */
static void q_open(dsp_app_t *app, bool open)
{
    q_lock(app);
    app->accepting = open;
    app->q_head = 0;
    app->q_count = 0;
    app->lnk.up = false; /* gen is kept: generations only increase within a wake */
    app->lnk.faults = 0;
    q_unlock(app);
}

static void link_snapshot(dsp_app_t *app, dsp_app_link_t *snap)
{
    q_lock(app);
    *snap = app->lnk;
    q_unlock(app);
}

/* ---- Frames and connections (M1.10c.1) ------------------------------------------------------ */

#define DSP_APP_CAPS 0x06u /* PHONE_TIME, HISTORY (Phase C); Wi-Fi provisioning, priming, viewer: not yet */

static void conn_end(dsp_app_t *app)
{
    if (app->ss.conn_active) {
        const dsp_app_security_t *sec = app->cfg->security;
        if (sec_session(app) && sec->on_connection != NULL) {
            sec->on_connection(sec->ctx, app, false); /* zeroize the security session first */
        }
        app->ss.conn_active = false;
        dsp_link_close(&app->link);
        dsp_app_sec_connection_end(app);
    }
}

/* Public state for HELLO, Info and advertising. Never "unclaimed" unless the record is a valid
 * unclaimed one (damaged or unknown credentials report CLAIMED + CREDENTIALS_DAMAGED). */
static void fill_hello(dsp_app_t *app, dsp_link_hello_t *h)
{
    memset(h, 0, sizeof *h);
    dsp_app_sec_view_t v;
    dsp_app_sec_view(app, &v);
    h->claim_state = v.cred_state == DSP_CRED_UNCLAIMED ? 0 : 1;
    h->flags = (uint8_t)((v.recovery_armed ? DSP_BLE_HELLO_F_RECOVERY_ARMED : 0u) |
                         (v.cred_state == DSP_CRED_DEGRADED || v.cred_state == DSP_CRED_UNKNOWN
                              ? DSP_BLE_HELLO_F_CREDENTIALS_DAMAGED
                              : 0u));
    h->caps = DSP_APP_CAPS;
    h->session_id = app->ss.session_id;
    const dsp_app_security_t *sec = app->cfg->security;
    if (sec != NULL && sec->device_id_hash != NULL &&
        !sec->device_id_hash(sec->ctx, v.cred_state == DSP_CRED_CLAIMED ? v.id_pub : NULL,
                             h->device_id_hash)) {
        memset(h->device_id_hash, 0, sizeof h->device_id_hash);
    }
}

static void publish_info(dsp_app_t *app)
{
    const dsp_ble_port_t *ble = app->cfg->ble;
    if (ble == NULL || ble->publish_info == NULL) {
        return;
    }
    dsp_link_hello_t h;
    fill_hello(app, &h);
    const dsp_app_identity_t *id = app->cfg->identity;
    dsp_ble_info_t info;
    memset(&info, 0, sizeof info);
    info.proto_min = DSP_LINK_PROTO_MIN;
    info.proto_max = DSP_LINK_PROTO_MAX;
    info.claim_state = h.claim_state;
    info.flags = h.flags;
    info.caps = h.caps;
    if (id != NULL) {
        info.fw_major = id->fw_major;
        info.fw_minor = id->fw_minor;
        info.fw_patch = id->fw_patch;
        info.device_tag = (uint16_t)(id->device_id[5] | (id->device_id[4] << 8));
    }
    dsp_ble_advert_t adv;
    adv.proto_major = DSP_LINK_PROTO_MAX;
    adv.flags = (uint8_t)((h.claim_state ? DSP_BLE_ADV_F_CLAIMED : 0u) |
                          ((h.flags & DSP_BLE_HELLO_F_RECOVERY_ARMED) ? DSP_BLE_ADV_F_RECOVERY_ARMED : 0u) |
                          ((h.flags & DSP_BLE_HELLO_F_CREDENTIALS_DAMAGED) ? DSP_BLE_ADV_F_CREDENTIALS_DAMAGED : 0u));
    adv.device_tag = info.device_tag;
    uint8_t ib[DSP_BLE_INFO_LEN], ab[DSP_BLE_ADVERT_LEN];
    if (dsp_ble_info_encode(&info, ib) == DSP_BLE_OK && dsp_ble_advert_encode(&adv, ab) == DSP_BLE_OK) {
        ble->publish_info(ble->ctx, ib, ab);
    }
}

static void send_tx(dsp_app_t *app, size_t len)
{
    if (len > 0 && app->ble_active && app->ss.conn_active &&
        app->cfg->ble->send_frame(app->cfg->ble->ctx, app->ss.conn_gen, app->tx, len)) {
        app->rep.frames_out++;
    }
}

static void link_drop(dsp_app_t *app)
{
    app->rep.link_closes++;
    app->cfg->ble->disconnect(app->cfg->ble->ctx, app->ss.conn_gen);
    conn_end(app);
}

/* Opens generation gen: every state of an older connection was closed before (conn_end). */
static void conn_open(dsp_app_t *app, dsp_app_session_t *ss, uint32_t gen)
{
    ss->conn_gen = gen;
    ss->faults_seen = 0;
    ss->conn_active = true;
    app->rep.links_opened++;
    dsp_link_open(&app->link, gen, mono_ms(app));
    dsp_app_sec_connection_begin(app);
    publish_info(app);
    if (sec_session(app) && app->cfg->security->on_connection != NULL) {
        app->cfg->security->on_connection(app->cfg->security->ctx, app, true);
    }
}

/* One fragment error of the open connection (decision T3). */
static void rx_fault(dsp_app_t *app)
{
    app->rep.rx_faults++;
    if (sec_session(app)) {
        const dsp_app_security_t *sec = app->cfg->security;
        if (sec->on_rx_fault != NULL) {
            sec->on_rx_fault(sec->ctx, app);
        } else {
            link_drop(app); /* fail closed: no plaintext from an unknown security state */
        }
        return;
    }
    dsp_link_result_t r = dsp_link_on_fragment_error(&app->link, app->tx, sizeof app->tx);
    if (r.actions & DSP_LINK_ACT_REPLY) {
        send_tx(app, r.out_len);
    }
    if (r.actions & DSP_LINK_ACT_CLOSE) {
        link_drop(app);
    }
}

/* Reconciles the application's connection with the transport's snapshot (decision T2):
 *  - the open connection closes when the snapshot names another generation or is down (every
 *    authorization and security state of it ends in conn_end);
 *  - the snapshot's generation opens when it is up and newer than the last one opened (a
 *    generation is opened at most once, so a dropped connection is never revived);
 *  - new fragment errors of the open connection are handled, at most DSP_LINK_ERRORS_MAX per
 *    call (each can close it). */
static void link_sync(dsp_app_t *app, dsp_app_session_t *ss, const dsp_app_link_t *snap)
{
    if (ss->conn_active && (!snap->up || snap->gen != ss->conn_gen)) {
        conn_end(app);
        publish_info(app);
    }
    if (snap->up && snap->gen > ss->conn_gen && app->ble_active) {
        conn_open(app, ss, snap->gen);
    }
    for (int i = 0; i < DSP_LINK_ERRORS_MAX && ss->conn_active && snap->gen == ss->conn_gen &&
                    snap->faults > ss->faults_seen;
         i++) {
        ss->faults_seen++;
        rx_fault(app);
    }
}

/* One queue item (app->rx). Returns false when the service session must end. */
static bool handle_item(dsp_app_t *app, dsp_app_session_t *ss)
{
    dsp_app_qitem_t *it = &app->rx;
    switch (it->kind) {
    case DSP_QI_BUTTON:
        owner_button(app, &it->button);
        return true;
    case DSP_QI_FRAME: {
        if (!ss->conn_active || it->gen != ss->conn_gen || !app->ble_active) {
            app->rep.frames_stale++;
            return true;
        }
        app->rep.frames_in++;
        if (sec_session(app)) {
            app->cfg->security->on_frame(app->cfg->security->ctx, app, it->bytes, it->len);
            return true;
        }
        dsp_link_hello_t h;
        fill_hello(app, &h);
        dsp_link_result_t r =
            dsp_link_on_frame(&app->link, it->bytes, it->len, &h, app->tx, sizeof app->tx);
        if (r.actions & DSP_LINK_ACT_REPLY) {
            send_tx(app, r.out_len);
        }
        if (r.actions & DSP_LINK_ACT_CLOSE) {
            link_drop(app);
        }
        return true;
    }
    default:
        return true;
    }
}

static void stage_ble(dsp_app_t *app)
{
    trace(app, DSP_STAGE_BLE);
    const dsp_app_config_t *cfg = app->cfg;
    const dsp_ble_port_t *ble = cfg->ble;
    dsp_app_session_t *ss = &app->ss;
    memset(ss, 0, sizeof *ss);
    uint32_t idle_s = cfg->service_idle_s != 0 ? cfg->service_idle_s : DSP_APP_SERVICE_IDLE_DEFAULT_S;
    uint32_t max_s = cfg->service_max_s != 0 ? cfg->service_max_s : DSP_APP_SERVICE_MAX_DEFAULT_S;
    if (max_s < idle_s) {
        max_s = idle_s;
    }
    q_open(app, true);
    if (!ble->start(ble->ctx)) {
        q_open(app, false);
        app->rep.ble_end = DSP_BLE_END_START_FAILED;
        return;
    }
    app->ble_active = true;
    app->rep.ble_session = true;
    ss->start_ms = mono_ms(app);
    owner_session_start(app);
    publish_info(app);
    ss->idle_ms = (uint64_t)idle_s * 1000u;
    ss->max_ms = ss->start_ms + (uint64_t)max_s * 1000u;
    ss->idle_deadline_ms = ss->start_ms + ss->idle_ms;
    bool stalled = false; /* a due dose made no progress: wait before re-checking */
    for (uint32_t it = 0; it < DSP_APP_BLE_MAX_ITERATIONS && ss->end == DSP_BLE_END_NONE; it++) {
        uint64_t now = mono_ms(app);
        if (now < ss->start_ms) {
            ss->end = DSP_BLE_END_CLOCK; /* a monotonic clock never goes back: end conservatively */
            break;
        }
        if (now >= ss->max_ms) {
            ss->end = DSP_BLE_END_MAX;
            break;
        }
        dsp_app_link_t snap;
        link_snapshot(app, &snap);
        link_sync(app, ss, &snap);
        bool conn = ble_connected(app);
        if (conn) {
            ss->was_connected = true;
        } else if (ss->was_connected) {
            ss->was_connected = false; /* disconnected: the idle timeout restarts */
            ss->idle_deadline_ms = now + ss->idle_ms;
        }
        if (!conn && now >= ss->idle_deadline_ms) {
            ss->end = DSP_BLE_END_IDLE;
            break;
        }
        owner_confirm_tick(app); /* recovery confirmation deadline */
        if (!stalled && dose_due_now(app)) {
            /* Q8: scheduled dosing first; BLE suspended for the pump, resumed afterwards. A
             * pending recovery confirmation is aborted (the session is interrupted). */
            owner_abort_confirm(app);
            size_t before = app->rep.dosed + app->rep.pump_failures;
            ble_suspend(app);
            stage_dose(app);
            stalled = app->rep.dosed + app->rep.pump_failures == before;
            if (!ble_resume(app, ss, conn)) {
                break;
            }
            continue;
        }
        if (q_pop(app, &snap)) { /* items that arrived before a deadline are served first */
            link_sync(app, ss, &snap);
            if (!handle_item(app, ss)) {
                break;
            }
            continue;
        }
        uint64_t sec_deadline = 0;
        if (sec_session(app)) {
            const dsp_app_security_t *sec = app->cfg->security;
            if (sec->on_tick != NULL) {
                sec_deadline = sec->on_tick(sec->ctx, app, now); /* step and window deadlines */
            }
            if (ss->end != DSP_BLE_END_NONE) {
                break;
            }
        } else if (ss->conn_active && dsp_link_expired(&app->link, now)) {
            link_drop(app); /* unauthenticated for 30 s (the M1.10c.1 gate) */
            continue;
        }
        uint64_t deadline =
            conn || ss->idle_deadline_ms > ss->max_ms ? ss->max_ms : ss->idle_deadline_ms;
        uint64_t wait_ms = deadline - now;
        dsp_utc_t at = 0;
        dsp_utc_t wall = raw_now(app);
        if (dosing_usable(app) && next_due(app, &at)) {
            uint64_t due_ms = at <= wall ? 0 : (uint64_t)(at - wall) * 1000u;
            if (due_ms < wait_ms) {
                wait_ms = due_ms;
            }
        }
        if (sec_deadline != 0) {
            uint64_t left = sec_deadline > now ? sec_deadline - now : 0;
            if (left < wait_ms) {
                wait_ms = left;
            }
        } else if (ss->conn_active && !sec_session(app)) {
            /* wake at the unauthenticated-connection deadline (the gate drops it on time) */
            uint64_t expiry = app->link.opened_ms + DSP_LINK_UNAUTH_MS;
            uint64_t left = expiry > now ? expiry - now : 0;
            if (left < wait_ms) {
                wait_ms = left;
            }
        }
        if (stalled || wait_ms < 1000u) {
            wait_ms = 1000u;
        }
        if (owner_confirm_pending(app) && wait_ms > 200u) {
            wait_ms = 200u; /* evaluate the confirmation deadline promptly */
        }
        stalled = false;
        if (wait_ms > 3600000u) {
            wait_ms = 3600000u;
        }
        ble->wait(ble->ctx, (uint32_t)wait_ms);
    }
    if (ss->end == DSP_BLE_END_NONE) {
        ss->end = DSP_BLE_END_MAX; /* iteration bound */
    }
    app->rep.ble_end = ss->end;
    conn_end(app);
    owner_session_end(app); /* handshake, authorization and confirmation end with the session */
    /* Unprocessed commands are dropped (the session is over); BLE stopped. */
    q_open(app, false);
    ble->stop(ble->ctx);
    app->ble_active = false;
}

/* ---- SYNC, PLAN, SHUTDOWN ------------------------------------------------------------- */

static void stage_sync(dsp_app_t *app)
{
    trace(app, DSP_STAGE_SYNC);
    if (!app->loaded) {
        return;
    }
    bool ok = dsp_notify_flush(&app->notify) == DSP_NOTIFY_OK;
    if (app->tk.meta_dirty) {
        if (dsp_storage_save_time(&app->st, &app->tk.meta) == DSP_STORAGE_OK) {
            app->tk.meta_dirty = false;
        } else {
            ok = false;
        }
    }
    app->rep.synced_ok = ok;
}

static dsp_wake_plan_t compute_plan(dsp_app_t *app)
{
    dsp_power_inputs_t in;
    memset(&in, 0, sizeof in);
    in.max_sleep_s = app->cfg->max_sleep_s;
    if (app->loaded) {
        /* Dose wakes only while allowed (Q13); a pump fault or storage failure in this wake
         * does not suppress them (the next wake reloads everything). */
        in.dose_known = !app->inhibited && next_due(app, &in.dose_at);
        in.sync_known = app->cfg->wifi != NULL;
        in.sync_at = dsp_timekeeper_next_sync(&app->tk);
    }
    return dsp_power_plan(&in, raw_now(app));
}

static void stage_plan(dsp_app_t *app)
{
    trace(app, DSP_STAGE_PLAN);
    dsp_wake_plan_t p = compute_plan(app);
    /* A dose became due during this wake: run it now instead of a zero-length sleep. */
    for (int round = 0; round < 2 && p.reason == DSP_WAKE_DOSE && p.sleep_s <= 0 && dose_due_now(app);
         round++) {
        stage_dose(app);
        p = compute_plan(app);
    }
    if (p.sleep_s < DSP_POWER_MIN_SLEEP_S) {
        p.sleep_s = DSP_POWER_MIN_SLEEP_S;
        p.wake_at = raw_now(app) + DSP_POWER_MIN_SLEEP_S;
    }
    app->rep.plan = p;
}

static uint32_t checklist(dsp_app_t *app)
{
    const dsp_app_config_t *cfg = app->cfg;
    uint32_t f = 0;
    bool off = app->loaded ? dsp_dosing_safe_off(&app->dosing) : dsp_dosing_port_safe_off(cfg->pump);
    if (!off) {
        f |= DSP_POWER_CHK_PUMP;
    }
    if (cfg->https != NULL && cfg->https->in_session) {
        dsp_https_end(cfg->https);
        if (cfg->https->in_session) {
            f |= DSP_POWER_CHK_HTTPS;
        }
    }
    if (cfg->wifi != NULL && cfg->wifi->initialized) {
        dsp_wifi_shutdown(cfg->wifi);
        if (dsp_wifi_state(cfg->wifi) != DSP_WIFI_DISABLED) {
            f |= DSP_POWER_CHK_WIFI;
        }
    }
    if (cfg->ble != NULL) {
        cfg->ble->stop(cfg->ble->ctx);
        app->ble_active = false;
        if (cfg->ble->is_off == NULL || !cfg->ble->is_off(cfg->ble->ctx)) {
            f |= DSP_POWER_CHK_BLE;
        }
    }
    return f;
}

/* noinline: its RTC buffer stays off the dosing call chain (stack budget) */
__attribute__((noinline)) static void stage_shutdown_and_sleep(dsp_app_t *app)
{
    trace(app, DSP_STAGE_SHUTDOWN);
    const dsp_power_port_t *power = app->cfg->power;
    uint32_t f = 0;
    do {
        f = checklist(app);
        app->rep.shutdown_attempts++;
    } while (f != 0 && app->rep.shutdown_attempts < DSP_APP_SHUTDOWN_ATTEMPTS);
    app->rep.shutdown_failures = f;
    /* Retained block: time retry state, hourly attempt limit, plan, fail-safe counter. */
    if (app->loaded) {
        app->retained.time = app->tk.retained;
    }
    owner_check_clock(app);
    app->retained.saved_raw = raw_now(app) >= 0 && raw_now(app) <= DSP_UTC_MAX ? raw_now(app) : 0;
    app->retained.last_reason = (uint8_t)app->rep.plan.reason;
    app->retained.last_wake_at = app->rep.plan.wake_at >= DSP_UTC_MIN && app->rep.plan.wake_at <= DSP_UTC_MAX
                                     ? app->rep.plan.wake_at
                                     : 0;
    if (f == 0) {
        app->retained.failsafe_count = 0;
    } else if (app->retained.failsafe_count < DSP_POWER_FAILSAFE_LIMIT) {
        app->retained.failsafe_count++;
    }
    uint8_t buf[DSP_POWER_RETAINED_BYTES];
    app->rep.retained_saved = dsp_power_retained_encode(&app->retained, buf) &&
                              power->save_retained(power->ctx, buf, sizeof buf);
    dsp_secure_zero(buf, sizeof buf); /* may hold an invite token */
    if (f == 0) {
        trace(app, DSP_STAGE_SLEEP);
        app->rep.slept = true;
        power->sleep(power->ctx, app->rep.plan.sleep_s);
    } else {
        trace(app, DSP_STAGE_FAIL_SAFE);
        app->rep.fail_safe = true;
        power->fail_safe(power->ctx, f);
    }
}

/* ---- M1.10b.1 operations ------------------------------------------------------------------ */

static dsp_app_op_result_t op(dsp_app_op_status_t status, uint32_t detail, uint32_t aux)
{
    dsp_app_op_result_t r = {status, detail, aux};
    return r;
}

/* Common preconditions, in this order: application task inside a service session, owner
 * authorization (fail closed: nothing else is looked at without it), storage loaded. */
static dsp_app_op_result_t guard(dsp_app_t *app, const dsp_app_auth_t *auth)
{
    if (app == NULL || app->cfg == NULL) {
        return op(DSP_OP_INVALID, 0, 0);
    }
    app->rep.ops++;
    if (!app->accepting) {
        return op(DSP_OP_BUSY, 0, 0);
    }
    if (auth == NULL || auth->role != DSP_APP_ROLE_OWNER || auth->slot >= DSP_APP_PHONE_SLOTS ||
        !issued_ok(app, auth)) {
        return op(DSP_OP_FORBIDDEN, 0, 0); /* only an authorization issued for this connection */
    }
    if (owner_confirm_pending(app)) {
        return op(DSP_OP_BUSY, DSP_OP_B_CONFIRMING, 0); /* only END_SESSION while confirming */
    }
    uint16_t ops_max = app->cfg->session_ops_max != 0 ? app->cfg->session_ops_max
                                                      : DSP_APP_SESSION_OPS_MAX_DEFAULT;
    if (app->ss.ops >= ops_max) {
        app->rep.rate_limited++;
        return op(DSP_OP_BUSY, DSP_OP_B_RATE_LIMITED, 0);
    }
    app->ss.ops++;
    if (!app->loaded) {
        return op(DSP_OP_DEGRADED, 0, 0);
    }
    return op(DSP_OP_OK, 0, 0);
}

/* Session-local write limits (v1: reset by a reconnect or reset; documented): at most
 * session_writes_max durable writes per session and writes_per_minute per 60 s window. */
static dsp_app_op_result_t write_budget(dsp_app_t *app)
{
    uint16_t max = app->cfg->session_writes_max != 0 ? app->cfg->session_writes_max
                                                     : DSP_APP_SESSION_WRITES_MAX_DEFAULT;
    uint16_t per_min = app->cfg->writes_per_minute != 0 ? app->cfg->writes_per_minute
                                                        : DSP_APP_WRITES_PER_MINUTE_DEFAULT;
    uint64_t now = app->cfg->mono != NULL ? mono_ms(app) : 0;
    if (now < app->ss.win_start_ms || now - app->ss.win_start_ms >= 60000u) {
        app->ss.win_start_ms = now;
        app->ss.win_writes = 0;
    }
    if (app->ss.writes >= max) {
        app->rep.rate_limited++;
        return op(DSP_OP_BUSY, DSP_OP_B_RATE_LIMITED, 0);
    }
    if (app->ss.win_writes >= per_min) {
        app->rep.rate_limited++;
        uint64_t left = 60000u - (now - app->ss.win_start_ms);
        return op(DSP_OP_BUSY, DSP_OP_B_RATE_LIMITED, (uint32_t)((left + 999u) / 1000u));
    }
    return op(DSP_OP_OK, 0, 0);
}

/* A durable write is about to be attempted. */
static void write_used(dsp_app_t *app)
{
    app->ss.writes++;
    app->ss.win_writes++;
}

static dsp_app_op_result_t done(dsp_app_t *app, dsp_app_op_result_t r)
{
    if (r.status == DSP_OP_OK) {
        app->rep.ops_ok++;
    }
    return r;
}

static bool write_failed(dsp_storage_status_t st)
{
    return st == DSP_STORAGE_ERR_WRITE || st == DSP_STORAGE_ERR_NO_SPACE ||
           st == DSP_STORAGE_ERR_COMMIT || st == DSP_STORAGE_ERR_CONFIRM;
}

/* A write failed: the durable value is the old or the new one. Dosing ends for this wake. */
static void storage_failed(dsp_app_t *app)
{
    app->storage_fault = true;
    app->rep.op_storage_fault = true;
}

/* Base of a new edit floor (D16): the trusted current time (SYNCED or HOLDOVER, plausible);
 * during a rollback hold the highest trusted time if that is later (nothing up to it may
 * become executable through an edit). 0 = the clock is untrusted: no floor can be
 * established, so the edit is refused (an untrusted clock can regain trust without a sync,
 * e.g. a continuous clock that read behind the last sync, so a floor of "now" or none would
 * not be safe). */
static dsp_utc_t floor_base(const dsp_app_t *app)
{
    dsp_utc_t now = raw_now(app);
    if (app->tk.trust == DSP_TRUST_UNKNOWN || now < DSP_UTC_MIN || now >= DSP_UTC_MAX) {
        return 0;
    }
    if (dsp_timekeeper_hold_active(&app->tk, now) && app->tk.meta.high_water_utc > now) {
        return app->tk.meta.high_water_utc;
    }
    return now;
}

dsp_app_op_result_t dsp_app_get_config(dsp_app_t *app, const dsp_app_auth_t *auth,
                                       dsp_config_t *out, uint32_t *hw_max_runtime_ms)
{
    dsp_app_op_result_t g = guard(app, auth);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    if (out == NULL) {
        return op(DSP_OP_INVALID, 0, 0);
    }
    if (dsp_storage_load_config(&app->st, out) != DSP_STORAGE_OK) {
        return op(DSP_OP_DEGRADED, 0, 0);
    }
    if (hw_max_runtime_ms != NULL) {
        *hw_max_runtime_ms = app->cfg->hw_max_runtime_ms;
    }
    return done(app, op(DSP_OP_OK, 0, 0));
}

dsp_app_op_result_t dsp_app_set_config(dsp_app_t *app, const dsp_app_auth_t *auth,
                                       uint32_t expected_revision, uint16_t field_mask,
                                       const dsp_config_t *values, uint32_t *new_revision)
{
    dsp_app_op_result_t g = guard(app, auth);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    if (values == NULL) {
        return op(DSP_OP_INVALID, 0, 0);
    }
    dsp_config_t cur;
    if (dsp_storage_load_config(&app->st, &cur) != DSP_STORAGE_OK) {
        return op(DSP_OP_DEGRADED, 0, 0);
    }
    if (expected_revision != cur.revision || cur.revision == UINT32_MAX) {
        return op(DSP_OP_CONFLICT, cur.revision, 0);
    }
    const dsp_dosing_state_t *ds = dsp_dosing_state(&app->dosing);
    if (ds == NULL) {
        return op(DSP_OP_DEGRADED, 0, 0);
    }
    if (ds->flags & DSP_DOSING_F_REFILL_PENDING) {
        return op(DSP_OP_REFUSED, DSP_OP_R_REFILL_PENDING, 0);
    }
    if (ds->readiness == DSP_READINESS_READY) {
        uint32_t refused = field_mask & DSP_CONFIG_F_SERVICE_ONLY;
        if ((field_mask & DSP_CONFIG_F_RESERVOIR_CAPACITY) &&
            (cur.reservoir_capacity_ul == 0 || values->reservoir_capacity_ul == 0)) {
            refused |= DSP_CONFIG_F_RESERVOIR_CAPACITY; /* would open or close a dosing gate */
        }
        if (refused != 0) {
            return op(DSP_OP_REFUSED, DSP_OP_R_READY, refused);
        }
    }
    dsp_config_t next;
    uint32_t e =
        dsp_config_apply_update(&cur, field_mask, values, app->cfg->hw_max_runtime_ms, &next);
    if (e != 0) {
        return op(DSP_OP_INVALID, e, 0);
    }
    {
        dsp_app_op_result_t wb = write_budget(app);
        if (wb.status != DSP_OP_OK) {
            return wb;
        }
        write_used(app);
    }
    dsp_storage_status_t st = dsp_storage_save_config(&app->st, &next);
    if (st != DSP_STORAGE_OK) {
        if (write_failed(st)) {
            storage_failed(app);
            return op(DSP_OP_STORAGE, 0, 0);
        }
        return op(st == DSP_STORAGE_ERR_REVISION ? DSP_OP_CONFLICT : DSP_OP_DEGRADED, 0, 0);
    }
    if (field_mask & DSP_CONFIG_F_SYNC_INTERVAL) {
        (void)dsp_timekeeper_set_interval(&app->tk, next.time_sync_interval_h);
    }
    if (field_mask & DSP_CONFIG_F_NOTIFY_MASK) {
        dsp_notify_set_mask(&app->notify, next.notify_mask);
    }
    if (field_mask & (DSP_CONFIG_F_RESERVOIR_CAPACITY | DSP_CONFIG_F_LOW_STOCK)) {
        /* Low-stock episodes follow the new capacity/threshold; the BLOCKED latch is only
         * ever cleared by a refill. */
        (void)dsp_dosing_observe_reservoir(&app->dosing, notify_clock(app));
    }
    if (cur.reservoir_capacity_ul != next.reservoir_capacity_ul ||
        cur.reservoir_remaining_ul != next.reservoir_remaining_ul) {
        /* Phase C: after the durable save; the revision makes the key unique. */
        dsp_dosing_log_reservoir(&app->dosing, &cur, &next, DSP_EVKEY_CONFIG | next.revision,
                                 notify_clock(app));
    }
    if (new_revision != NULL) {
        *new_revision = next.revision;
    }
    return done(app, op(DSP_OP_OK, 0, 0));
}

/* Repository result -> operation result. */
static dsp_app_op_result_t from_repo(dsp_app_t *app, dsp_storage_status_t st, uint32_t sched_errors)
{
    switch (st) {
    case DSP_STORAGE_OK:
        return op(DSP_OP_OK, 0, 0);
    case DSP_STORAGE_ERR_INVALID:
        return op(DSP_OP_INVALID, sched_errors, 0);
    case DSP_STORAGE_ERR_ARG:
        return op(DSP_OP_INVALID, 0, 0);
    case DSP_STORAGE_ERR_NOT_FOUND:
        return op(DSP_OP_NOT_FOUND, 0, 0);
    case DSP_STORAGE_ERR_DUPLICATE_ID:
        return op(DSP_OP_DUPLICATE, 0, 0);
    case DSP_STORAGE_ERR_ID_REUSED:
        return op(DSP_OP_DUPLICATE, 0, 1);
    case DSP_STORAGE_ERR_CAPACITY:
        return op(DSP_OP_CAPACITY, 0, DSP_SCHEDULE_CAPACITY);
    case DSP_STORAGE_ERR_REVISION:
        return op(DSP_OP_CONFLICT, 0, 0); /* revision space exhausted */
    default:
        if (write_failed(st)) {
            storage_failed(app);
            /* The durable set is old or new: re-read it so that planning keeps working. */
            (void)dsp_schedule_repo_load(&app->repo, &app->st);
            return op(DSP_OP_STORAGE, 0, 0);
        }
        return op(DSP_OP_DEGRADED, 0, 0);
    }
}

static dsp_app_op_result_t repo_ready(const dsp_app_t *app)
{
    return dsp_schedule_repo_set(&app->repo) != NULL ? op(DSP_OP_OK, 0, 0)
                                                     : op(DSP_OP_DEGRADED, 0, 0);
}

dsp_app_op_result_t dsp_app_list_schedules(dsp_app_t *app, const dsp_app_auth_t *auth,
                                           uint8_t page, dsp_app_schedule_page_t *out)
{
    dsp_app_op_result_t g = guard(app, auth);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    g = repo_ready(app);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    if (out == NULL) {
        return op(DSP_OP_INVALID, 0, 0);
    }
    const dsp_schedule_set_t *set = dsp_schedule_repo_set(&app->repo);
    size_t first = (size_t)page * DSP_APP_SCHEDULE_PAGE;
    if (page != 0 && first >= set->count) {
        return op(DSP_OP_INVALID, 0, 0);
    }
    memset(out, 0, sizeof *out);
    out->set_revision = set->revision;
    out->total = set->count;
    out->page = page;
    for (size_t i = first; i < set->count && out->n < DSP_APP_SCHEDULE_PAGE; i++) {
        memcpy(out->items[out->n].id, set->schedules[i].id, sizeof out->items[0].id);
        out->items[out->n].revision = set->schedules[i].revision;
        out->n++;
    }
    return done(app, op(DSP_OP_OK, 0, 0));
}

dsp_app_op_result_t dsp_app_get_history(dsp_app_t *app, const dsp_app_auth_t *auth,
                                        uint32_t before_seq, uint8_t max,
                                        dsp_app_history_page_t *out)
{
    dsp_app_op_result_t g = guard(app, auth);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    if (out == NULL || max < 1 || max > DSP_APP_HISTORY_PAGE) {
        return op(DSP_OP_INVALID, 0, 0);
    }
    memset(out, 0, sizeof *out);
    if (!app->evlog.open) {
        return op(DSP_OP_DEGRADED, 0, 0);
    }
    out->n = (uint8_t)dsp_evlog_read(&app->evlog, before_seq, out->items, max);
    out->newest_seq = dsp_evlog_newest(&app->evlog);
    out->oldest_seq = dsp_evlog_oldest(&app->evlog);
    out->faults = app->evlog.faults != 0;
    return done(app, op(DSP_OP_OK, 0, 0));
}

dsp_app_op_result_t dsp_app_get_schedule(dsp_app_t *app, const dsp_app_auth_t *auth,
                                         const char *id, dsp_schedule_t *out)
{
    dsp_app_op_result_t g = guard(app, auth);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    g = repo_ready(app);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    if (out == NULL || !dsp_schedule_id_is_valid(id)) {
        return op(DSP_OP_INVALID, id == NULL || out == NULL ? 0 : DSP_SCHED_E_ID, 0);
    }
    const dsp_schedule_t *s = dsp_schedule_repo_find(&app->repo, id);
    if (s == NULL) {
        return op(DSP_OP_NOT_FOUND, 0, 0);
    }
    *out = *s;
    return done(app, op(DSP_OP_OK, 0, 0));
}

/* Shared input checks of create/update: validation, then the per-dose limit. */
static dsp_app_op_result_t check_input(dsp_app_t *app, const dsp_schedule_t *s)
{
    uint32_t e = dsp_schedule_validate(s, app->cfg->tz);
    if (e != DSP_SCHED_OK) {
        return op(DSP_OP_INVALID, e, 0);
    }
    dsp_config_t cfg;
    if (dsp_storage_load_config(&app->st, &cfg) != DSP_STORAGE_OK) {
        return op(DSP_OP_DEGRADED, 0, 0);
    }
    if (cfg.max_dose_ul != 0 && s->amount_ul > cfg.max_dose_ul) {
        return op(DSP_OP_LIMIT, DSP_DOSING_L_DOSE, cfg.max_dose_ul);
    }
    return op(DSP_OP_OK, 0, 0);
}

static dsp_utc_t raise_floor(dsp_utc_t floor, dsp_utc_t base)
{
    return base + 1 > floor ? base + 1 : floor; /* monotonic per schedule */
}

static bool timing_changed(const dsp_schedule_t *a, const dsp_schedule_t *b)
{
    return dsp_date_compare(a->start_date, b->start_date) != 0 || a->hour != b->hour ||
           a->minute != b->minute || a->kind != b->kind || a->interval != b->interval ||
           a->weekday_mask != b->weekday_mask || a->has_end_date != b->has_end_date ||
           (a->has_end_date && dsp_date_compare(a->end_date, b->end_date) != 0);
}

dsp_app_op_result_t dsp_app_create_schedule(dsp_app_t *app, const dsp_app_auth_t *auth,
                                            const dsp_schedule_t *s, uint32_t *set_revision)
{
    dsp_app_op_result_t g = guard(app, auth);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    g = repo_ready(app);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    if (s == NULL) {
        return op(DSP_OP_INVALID, 0, 0);
    }
    dsp_utc_t base = floor_base(app);
    if (base == 0) {
        return op(DSP_OP_REFUSED, DSP_OP_R_CLOCK, 0);
    }
    dsp_schedule_t in = *s;
    in.not_before = raise_floor(0, base);
    g = check_input(app, &in);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    if (in.revision != 1) {
        return op(DSP_OP_INVALID, DSP_SCHED_E_REVISION, 0);
    }
    {
        dsp_app_op_result_t wb = write_budget(app);
        if (wb.status != DSP_OP_OK) {
            return wb;
        }
        write_used(app);
    }
    g = from_repo(app, dsp_schedule_repo_create(&app->repo, &in, &app->hist), 0);
    if (g.status == DSP_OP_OK && set_revision != NULL) {
        *set_revision = dsp_schedule_repo_set(&app->repo)->revision;
    }
    return done(app, g);
}

dsp_app_op_result_t dsp_app_update_schedule(dsp_app_t *app, const dsp_app_auth_t *auth,
                                            uint32_t expected_revision, const dsp_schedule_t *s,
                                            uint32_t *revision, uint32_t *set_revision)
{
    dsp_app_op_result_t g = guard(app, auth);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    g = repo_ready(app);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    if (s == NULL || !dsp_schedule_id_is_valid(s->id)) {
        return op(DSP_OP_INVALID, s == NULL ? 0 : DSP_SCHED_E_ID, 0);
    }
    const dsp_schedule_t *cur = dsp_schedule_repo_find(&app->repo, s->id);
    if (cur == NULL) {
        return op(DSP_OP_NOT_FOUND, 0, 0);
    }
    if (expected_revision != cur->revision || cur->revision == UINT32_MAX) {
        return op(DSP_OP_CONFLICT, cur->revision, 0);
    }
    dsp_schedule_t in = *s;
    in.not_before = cur->not_before;
    if (timing_changed(cur, &in) || (!cur->enabled && in.enabled)) {
        dsp_utc_t base = floor_base(app);
        if (base == 0) {
            return op(DSP_OP_REFUSED, DSP_OP_R_CLOCK, 0);
        }
        in.not_before = raise_floor(cur->not_before, base);
    }
    g = check_input(app, &in);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    if (in.revision != expected_revision + 1u) {
        return op(DSP_OP_INVALID, DSP_SCHED_E_REVISION, 0);
    }
    {
        dsp_app_op_result_t wb = write_budget(app);
        if (wb.status != DSP_OP_OK) {
            return wb;
        }
        write_used(app);
    }
    g = from_repo(app, dsp_schedule_repo_update(&app->repo, &in), 0);
    if (g.status == DSP_OP_OK) {
        if (revision != NULL) {
            *revision = in.revision;
        }
        if (set_revision != NULL) {
            *set_revision = dsp_schedule_repo_set(&app->repo)->revision;
        }
    }
    return done(app, g);
}

dsp_app_op_result_t dsp_app_delete_schedule(dsp_app_t *app, const dsp_app_auth_t *auth,
                                            const char *id, uint32_t expected_revision,
                                            uint32_t *set_revision)
{
    dsp_app_op_result_t g = guard(app, auth);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    g = repo_ready(app);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    if (!dsp_schedule_id_is_valid(id)) {
        return op(DSP_OP_INVALID, id == NULL ? 0 : DSP_SCHED_E_ID, 0);
    }
    const dsp_schedule_t *cur = dsp_schedule_repo_find(&app->repo, id);
    if (cur == NULL) {
        return op(DSP_OP_NOT_FOUND, 0, 0);
    }
    if (expected_revision != cur->revision) {
        return op(DSP_OP_CONFLICT, cur->revision, 0);
    }
    {
        dsp_app_op_result_t wb = write_budget(app);
        if (wb.status != DSP_OP_OK) {
            return wb;
        }
        write_used(app);
    }
    g = from_repo(app, dsp_schedule_repo_delete(&app->repo, id, expected_revision), 0);
    if (g.status == DSP_OP_OK && set_revision != NULL) {
        *set_revision = dsp_schedule_repo_set(&app->repo)->revision;
    }
    return done(app, g);
}

dsp_app_op_result_t dsp_app_set_phone_time(dsp_app_t *app, const dsp_app_auth_t *auth,
                                           int64_t phone_utc_ms, uint32_t uncertainty_ms,
                                           dsp_app_time_result_t *out)
{
    dsp_app_op_result_t g = guard(app, auth);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    if (out == NULL) {
        return op(DSP_OP_INVALID, 0, 0);
    }
    memset(out, 0, sizeof *out);
    /* Current trust first (the session may have crossed the SYNCED/HOLDOVER boundary). */
    stage_clock_eval(app);
    dsp_timekeeper_t before = app->tk;
    dsp_sync_decision_t d;
    dsp_phone_time_decision_t dec =
        dsp_timekeeper_on_phone_time(&app->tk, phone_utc_ms, uncertainty_ms, raw_now(app), &d);
    out->decision = (uint8_t)dec;
    if (dec == DSP_PHONE_TIME_ACCEPTED) {
        dsp_app_op_result_t wb = write_budget(app);
        if (wb.status != DSP_OP_OK) {
            app->tk = before; /* nothing applied */
            out->decision = 0;
            return wb;
        }
        write_used(app);
        dsp_storage_status_t ps =
            dsp_time_apply(&app->tk, &before, &app->st, app->cfg->clock, &d, NULL);
        owner_check_clock(app); /* a step clears the ownership overlays */
        if (ps != DSP_STORAGE_OK) {
            out->decision = 0;
            if (write_failed(ps)) {
                storage_failed(app);
                return op(DSP_OP_STORAGE, 0, 0);
            }
            return op(DSP_OP_DEGRADED, 0, 0);
        }
        out->offset_s = d.offset_s;
        (void)dsp_notify_observe_time(&app->notify, &app->tk, raw_now(app));
    }
    return done(app, op(DSP_OP_OK, 0, 0));
}

/* ---- M1.10b.2: manual dose with durable anti-replay ---------------------------------------- */

static void fill_manual(const dsp_app_t *app, uint8_t slot, dsp_app_manual_result_t *out,
                        uint8_t state, uint32_t detail)
{
    out->state = state;
    out->detail = detail;
    out->floor = dsp_dosing_manual_floor(&app->dosing, slot);
}

dsp_app_op_result_t dsp_app_manual_dose(dsp_app_t *app, const dsp_app_auth_t *auth, uint64_t seq,
                                        uint32_t volume_ul, dsp_app_manual_result_t *out)
{
    dsp_app_op_result_t g = guard(app, auth);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    if (out == NULL) {
        return op(DSP_OP_INVALID, 0, 0);
    }
    memset(out, 0, sizeof *out);
    const uint8_t slot = auth->slot; /* the verified session's slot, never the request's */
    if (seq == 0 || seq > DSP_MANUAL_SEQ_MAX || volume_ul == 0) {
        fill_manual(app, slot, out, DSP_MRES_NOT_DECIDED, 0);
        return op(DSP_OP_INVALID, 0, 0); /* not consumed */
    }
    /* Rule 1: already decided -> never executed, no side effect (BLE untouched). */
    dsp_manual_outcome_t lk = dsp_dosing_manual_lookup(&app->dosing, slot, seq);
    if (lk.kind == DSP_MANUAL_DECIDED) {
        fill_manual(app, slot, out, lk.state, lk.detail);
        return done(app, op(DSP_OP_DUPLICATE, lk.state, lk.detail));
    }
    if (app->storage_fault || dsp_dosing_degraded(&app->dosing)) {
        fill_manual(app, slot, out, DSP_MRES_NOT_DECIDED, 0);
        return op(DSP_OP_DEGRADED, 0, 0); /* nothing can be made durable in this wake */
    }
    /* Admission (M1.10b.3): after the decided lookup (retries are always answered), before
     * anything else; a refusal here writes nothing and leaves the request undecided. */
    uint16_t mmax = app->cfg->session_manual_max != 0 ? app->cfg->session_manual_max
                                                      : DSP_APP_SESSION_MANUAL_MAX_DEFAULT;
    uint32_t retry_s = 0;
    if (app->ss.manual >= mmax ||
        !dsp_dosing_manual_admissible(&app->dosing, dose_clock(app), &retry_s)) {
        app->rep.rate_limited++;
        fill_manual(app, slot, out, DSP_MRES_NOT_DECIDED, 0);
        return op(DSP_OP_BUSY, DSP_OP_B_RATE_LIMITED, app->ss.manual >= mmax ? 0 : retry_s);
    }
    app->ss.manual++;
    /* Rule 2: BLE off before the pump; a due scheduled dose goes first (Q8). */
    bool was_connected = ble_connected(app);
    ble_suspend(app);
    if (dose_due_now(app)) {
        stage_dose(app);
    }
    dsp_manual_outcome_t o;
    if (app->storage_fault || dsp_dosing_degraded(&app->dosing)) {
        memset(&o, 0, sizeof o);
        o.kind = DSP_MANUAL_NOT_DURABLE;
        o.dose.gates = DSP_DOSING_G_DEGRADED;
    } else {
        uint32_t refusal = app->inhibited || app->pump_fault ? DSP_MREFUSE_DEGRADED : 0;
        o = dsp_dosing_manual_request(&app->dosing, slot, seq, volume_ul, dose_clock(app), refusal);
    }
    if (o.kind == DSP_MANUAL_EXECUTED) {
        pump_mark(app); /* the pump was started (or attempted) */
    }
    if (o.kind == DSP_MANUAL_EXECUTED && o.dose.status == DSP_DOSING_OK) {
        app->rep.manual_doses++;
    }
    if (o.dose.status == DSP_DOSING_PUMP_FAILED) {
        app->pump_fault = true; /* no further activation in this wake */
        app->rep.pump_fault = true;
    }
    app->rep.dosing_degraded = dsp_dosing_degraded(&app->dosing);
    (void)ble_resume(app, &app->ss, was_connected); /* false: the session ends (ss.end set) */
    fill_manual(app, slot, out, o.state, o.detail);
    out->volume_ul = o.dose.volume_ul;
    out->runtime_ms = o.dose.runtime_ms;
    dsp_app_op_result_t r;
    switch (o.kind) {
    case DSP_MANUAL_EXECUTED:
        if (o.dose.status == DSP_DOSING_OK || o.dose.status == DSP_DOSING_PUMP_FAILED) {
            out->outcome = o.dose.status == DSP_DOSING_OK ? 0 : 1;
            r = op(DSP_OP_OK, 0, 0);
        } else {
            storage_failed(app); /* decided (PENDING) but not completed: recovery finishes it */
            r = op(DSP_OP_STORAGE, 0, 0);
        }
        break;
    case DSP_MANUAL_REFUSED:
        if (o.dose.status == DSP_DOSING_LIMIT) {
            r = op(DSP_OP_LIMIT, o.dose.limits, 0);
        } else if (o.dose.status == DSP_DOSING_NOT_COVERED) {
            r = op(DSP_OP_NOT_COVERED, 0, 0);
        } else {
            r = op(DSP_OP_REFUSED, o.dose.gates, o.detail); /* aux: refusal code */
        }
        break;
    case DSP_MANUAL_DECIDED:
        r = op(DSP_OP_DUPLICATE, o.state, o.detail);
        break;
    case DSP_MANUAL_INVALID:
        r = op(DSP_OP_INVALID, 0, 0);
        break;
    case DSP_MANUAL_RATE_LIMITED: /* e.g. the date changed back meanwhile: still undecided */
        app->rep.rate_limited++;
        r = op(DSP_OP_BUSY, DSP_OP_B_RATE_LIMITED, 0);
        break;
    default: /* NOT_DURABLE */
        if (o.dose.gates & (DSP_DOSING_G_NOT_LOADED | DSP_DOSING_G_DEGRADED)) {
            r = op(DSP_OP_DEGRADED, 0, 0);
        } else {
            storage_failed(app);
            r = op(DSP_OP_STORAGE, 0, 0);
        }
        break;
    }
    return done(app, r);
}

dsp_app_op_result_t dsp_app_get_result(dsp_app_t *app, const dsp_app_auth_t *auth, uint64_t seq,
                                       uint8_t *state, uint32_t *detail)
{
    dsp_app_op_result_t g = guard(app, auth);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    if (state == NULL || detail == NULL) {
        return op(DSP_OP_INVALID, 0, 0);
    }
    dsp_manual_outcome_t lk = dsp_dosing_manual_lookup(&app->dosing, auth->slot, seq);
    if (lk.kind == DSP_MANUAL_INVALID) {
        return op(DSP_OP_INVALID, 0, 0);
    }
    *state = lk.state; /* NOT_DECIDED above the high-water mark */
    *detail = lk.detail;
    return done(app, op(DSP_OP_OK, 0, 0));
}

dsp_app_op_result_t dsp_app_manual_floor(dsp_app_t *app, const dsp_app_auth_t *auth,
                                         uint64_t *floor)
{
    dsp_app_op_result_t g = guard(app, auth);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    if (floor == NULL) {
        return op(DSP_OP_INVALID, 0, 0);
    }
    *floor = dsp_dosing_manual_floor(&app->dosing, auth->slot);
    return *floor != 0 ? done(app, op(DSP_OP_OK, 0, 0)) : op(DSP_OP_DEGRADED, 0, 0);
}


/* ---- M1.10c.1: status, diagnostics, refill, readiness --------------------------------------- */

static uint8_t proto_severity(uint8_t sev)
{
    return sev == DSP_SEV_CRITICAL ? 2 : (sev == DSP_SEV_WARNING ? 1 : 0);
}

static void build_status(dsp_app_t *app, uint8_t slot, dsp_ble_status_rec_t *s)
{
    memset(s, 0, sizeof *s);
    s->battery_percent = 255; /* no battery measurement yet */
    uint64_t now_ms = app->cfg->mono != NULL ? mono_ms(app) : 0;
    if (app->accepting && app->ss.max_ms > now_ms) {
        uint64_t left = (app->ss.max_ms - now_ms) / 1000u;
        s->session_seconds_left = (uint16_t)(left > 65535u ? 65535u : left);
    }
    s->device_utc = raw_now(app);
    if (!app->loaded) {
        return;
    }
    s->storage_ok = !app->storage_fault && !dsp_dosing_degraded(&app->dosing) &&
                    !dsp_notify_storage_degraded(&app->notify);
    const dsp_dosing_state_t *ds = dsp_dosing_state(&app->dosing);
    if (ds != NULL) {
        s->readiness = ds->readiness;
        s->dosing_flags = ds->flags;
    }
    dsp_config_t cfg;
    if (dsp_storage_load_config(&app->st, &cfg) == DSP_STORAGE_OK) {
        s->capacity_ul = cfg.reservoir_capacity_ul;
        s->remaining_ul = cfg.reservoir_remaining_ul;
        s->low_percent = cfg.low_stock_percent;
    }
    s->gates = dsp_dosing_gates(&app->dosing, dose_clock(app).valid, &s->pump_problems);
    s->estimate_uncertain = (app->notify.s.flags & DSP_NOTIFY_F_ESTIMATE_UNCERTAIN) != 0;
    s->clock_trust = (uint8_t)app->tk.trust;
    s->clock_reason = (uint8_t)app->tk.reason;
    dsp_utc_t at = 0;
    s->next_due_known = next_due(app, &at);
    s->next_due_at = s->next_due_known ? at : 0;
    for (size_t i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY; i++) {
        const dsp_note_t *n = &app->notify.s.queue[i];
        if (n->seq != 0) {
            s->pending_notes++;
            uint8_t sev = proto_severity(n->severity);
            if (sev > s->highest_severity) {
                s->highest_severity = sev;
            }
        }
    }
    s->manual_floor = dsp_dosing_manual_floor(&app->dosing, slot);
}

dsp_app_op_result_t dsp_app_get_status(dsp_app_t *app, const dsp_app_auth_t *auth,
                                       dsp_ble_status_rec_t *out)
{
    dsp_app_op_result_t g = guard(app, auth);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    if (out == NULL) {
        return op(DSP_OP_INVALID, 0, 0);
    }
    build_status(app, auth->slot, out);
    return done(app, op(DSP_OP_OK, 0, 0));
}

dsp_app_op_result_t dsp_app_get_diagnostics(dsp_app_t *app, const dsp_app_auth_t *auth,
                                            dsp_ble_diag_rec_t *out)
{
    dsp_app_op_result_t g = guard(app, auth);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    if (out == NULL) {
        return op(DSP_OP_INVALID, 0, 0);
    }
    memset(out, 0, sizeof *out);
    out->notify_storage_ok = !dsp_notify_storage_degraded(&app->notify);
    out->tx_diag = app->notify.s.tx_diag;
    out->tx_diag_status = app->notify.s.tx_diag_status;
    out->rejections_unread = app->notify.s.rejections.unread;
    uint32_t total = app->notify.s.rejections.total;
    out->rejections_total = (uint16_t)(total > 65535u ? 65535u : total);
    out->clock_trust = (uint8_t)app->tk.trust;
    out->clock_reason = (uint8_t)app->tk.reason;
    out->last_sync_utc = app->tk.meta.last_sync_utc;
    out->last_wake_reason = app->retained.last_reason;
    return done(app, op(DSP_OP_OK, 0, 0));
}

dsp_app_op_result_t dsp_app_get_battery(dsp_app_t *app, const dsp_app_auth_t *auth,
                                        dsp_app_battery_t *out)
{
    dsp_app_op_result_t g = guard(app, auth);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    if (out == NULL) {
        return op(DSP_OP_INVALID, 0, 0);
    }
    uint64_t now = mono_or_0(app);
    bool stale = !app->batt_measured || now < app->batt_ms ||
                 now - app->batt_ms >= DSP_APP_BATTERY_REFRESH_MS;
    bool quiet = !app->pump_ran || (now >= app->pump_ms && now - app->pump_ms >= DSP_APP_BATTERY_PUMP_QUIET_MS);
    if (stale && quiet) {
        battery_measure(app);
        now = app->batt_measured ? app->batt_ms : now;
    }
    memset(out, 0, sizeof *out);
    if (!app->batt_measured) {
        out->sample.state = DSP_BATT_NOT_MEASURED;
    } else {
        out->sample = app->batt;
        if (app->batt.state == DSP_BATT_VALID && now >= app->batt_ms) {
            uint64_t age = (now - app->batt_ms) / 1000u;
            out->age_s = age > UINT32_MAX ? UINT32_MAX : (uint32_t)age;
        }
    }
    return done(app, op(DSP_OP_OK, 0, 0));
}

dsp_app_op_result_t dsp_app_ack_rejections(dsp_app_t *app, const dsp_app_auth_t *auth)
{
    dsp_app_op_result_t g = guard(app, auth);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    if (app->notify.s.rejections.unread == 0 && app->notify.s.rejections.lost_unread == 0) {
        return done(app, op(DSP_OP_OK, 0, 0)); /* nothing to acknowledge: no write */
    }
    g = write_budget(app);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    write_used(app);
    if (dsp_notify_ack_rejections(&app->notify) != DSP_NOTIFY_OK) {
        storage_failed(app);
        return op(DSP_OP_STORAGE, 0, 0);
    }
    return done(app, op(DSP_OP_OK, 0, 0));
}

dsp_app_op_result_t dsp_app_end_service_session(dsp_app_t *app, const dsp_app_auth_t *auth)
{
    if (app == NULL || app->cfg == NULL) {
        return op(DSP_OP_INVALID, 0, 0);
    }
    app->rep.ops++;
    if (!app->accepting) {
        return op(DSP_OP_BUSY, 0, 0);
    }
    if (auth == NULL || auth->role != DSP_APP_ROLE_OWNER || auth->slot >= DSP_APP_PHONE_SLOTS ||
        !issued_ok(app, auth)) {
        return op(DSP_OP_FORBIDDEN, 0, 0); /* C10: never without an issued owner authorization */
    }
    /* no rate limit, no storage precondition: ending is always possible for the owner */
    app->ss.end = DSP_BLE_END_COMMAND; /* the BLE loop ends after this item */
    return op(DSP_OP_OK, 0, 0);
}

dsp_app_op_result_t dsp_app_refill(dsp_app_t *app, const dsp_app_auth_t *auth,
                                   uint32_t capacity_ul, uint32_t remaining_ul,
                                   dsp_ble_status_rec_t *status)
{
    dsp_app_op_result_t g = guard(app, auth);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    if (app->storage_fault || dsp_dosing_degraded(&app->dosing)) {
        return op(DSP_OP_DEGRADED, 0, 0);
    }
    g = write_budget(app);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    write_used(app);
    dsp_dosing_status_t ds = dsp_dosing_refill(&app->dosing, capacity_ul, remaining_ul, notify_clock(app));
    dsp_app_op_result_t r;
    switch (ds) {
    case DSP_DOSING_OK:
        r = op(DSP_OP_OK, 0, 0);
        break;
    case DSP_DOSING_ERR_ARG:
        r = op(DSP_OP_INVALID, 0, 0);
        break;
    case DSP_DOSING_ERR_STORAGE:
        storage_failed(app);
        r = op(DSP_OP_STORAGE, 0, 0);
        break;
    default:
        r = op(DSP_OP_REFUSED, 0, 0);
        break;
    }
    if (status != NULL) {
        build_status(app, auth->slot, status);
    }
    return done(app, r);
}

/*
 * READY dosing floor (incident 2026-11-20, F1): entering READY must not release occurrences that
 * fell due while the device was NOT READY (they are still inside their 10-minute due window and
 * would run at once). Before READY is stored, the timekeeper's durable dosing floor
 * (time.not_before_utc, the same floor a large forward clock step raises) becomes the transition
 * instant + 1 s: every occurrence scheduled at or before the transition, including one in the same
 * second or minute, is MISSED; later occurrences (also later the same day, and every following day)
 * are unaffected. The floor is written, committed and read back BEFORE the readiness write, so a
 * power loss can only leave NOT READY with a raised floor (harmless), never READY with an older
 * floor. Without a trusted time no floor is needed: no automatic dose runs before the first
 * trusted time, and that first trust raises the floor to the sync instant + 1 s. Manual doses and
 * the per-schedule edit floors are not involved.
 */
static dsp_app_op_result_t ready_floor(dsp_app_t *app)
{
    dsp_utc_t base = floor_base(app);
    if (base == 0 || base + 1 <= app->tk.meta.not_before_utc) {
        return op(DSP_OP_OK, 0, 0);
    }
    dsp_time_meta_t m = app->tk.meta;
    m.not_before_utc = base + 1; /* monotonic: never lowered */
    write_used(app); /* a durable write of its own (the budget was checked by the caller) */
    if (dsp_storage_save_time(&app->st, &m) != DSP_STORAGE_OK) {
        storage_failed(app);
        return op(DSP_OP_STORAGE, 0, 0); /* READY is not entered */
    }
    app->tk.meta = m; /* any pending timekeeper change was persisted with it */
    app->tk.meta_dirty = false;
    return op(DSP_OP_OK, 0, 0);
}

dsp_app_op_result_t dsp_app_set_readiness(dsp_app_t *app, const dsp_app_auth_t *auth,
                                          uint8_t readiness, dsp_ble_status_rec_t *status)
{
    dsp_app_op_result_t g = guard(app, auth);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    if (readiness != DSP_READINESS_READY && readiness != DSP_READINESS_SERVICE_REQUIRED) {
        return op(DSP_OP_INVALID, 0, 0);
    }
    const dsp_dosing_state_t *cur = dsp_dosing_state(&app->dosing);
    if (cur == NULL) {
        return op(DSP_OP_DEGRADED, 0, 0);
    }
    if (cur->readiness != readiness) {
        g = write_budget(app);
        if (g.status != DSP_OP_OK) {
            return g;
        }
    }
    uint32_t gates = 0, problems = 0;
    bool attempt = cur->readiness != readiness;
    if (attempt && readiness == DSP_READINESS_READY &&
        dsp_dosing_ready_gates(&app->dosing, &problems) == 0) {
        dsp_app_op_result_t f = ready_floor(app);
        if (f.status != DSP_OP_OK) {
            if (status != NULL) {
                build_status(app, auth->slot, status);
            }
            return done(app, f);
        }
    }
    dsp_dosing_status_t ds = dsp_dosing_set_readiness(&app->dosing, (dsp_readiness_t)readiness,
                                                      &gates, &problems);
    if (attempt && ds != DSP_DOSING_REFUSED) {
        write_used(app);
    }
    dsp_app_op_result_t r;
    switch (ds) {
    case DSP_DOSING_OK:
        r = op(DSP_OP_OK, 0, 0);
        break;
    case DSP_DOSING_REFUSED:
        r = op(DSP_OP_REFUSED, gates, problems);
        break;
    case DSP_DOSING_ERR_STORAGE:
        storage_failed(app);
        r = op(DSP_OP_STORAGE, 0, 0);
        break;
    default:
        r = op(DSP_OP_INVALID, 0, 0);
        break;
    }
    if (status != NULL) {
        build_status(app, auth->slot, status);
    }
    return done(app, r);
}

/* ---- M1.10b.3: ownership --------------------------------------------------------------------
 *
 * docs/OWNERSHIP_DESIGN.md (approved revision 2). The Application Service keeps the public view
 * of the "auth" record, the credential state and the per-session evidence (handshake handle,
 * issued authorization, recovery confirmation). Key material is read only when a transition or
 * the security layer needs it, into st.auth_scratch, and wiped afterwards. */

/* noinline: keeps this frame off the dosing call chain (stack budget) */
__attribute__((noinline)) static void owner_publish(dsp_app_t *app)
{
    app->rep.cred_state = app->own.cred;
    app->rep.cred_inhibit = app->own.cred == DSP_CRED_DEGRADED || app->own.key_fault;
    if (app->loaded) {
        dsp_dosing_set_credentials_inhibit(&app->dosing, app->rep.cred_inhibit);
        /* DEVICE_ERROR (storage) while the credentials are damaged; this service is the only
         * owner of that condition. No write unless it changes. */
        (void)dsp_notify_set_device_condition(&app->notify, DSP_COND_STORAGE, app->rep.cred_inhibit,
                                              1u, notify_clock(app));
    }
}

/* Public cache from a full record (the private key never enters the cache). */
static void owner_set_cache(dsp_app_t *app, const dsp_auth_t *full)
{
    app->own.cache = *full;
    dsp_secure_zero(app->own.cache.id_priv, sizeof app->own.cache.id_priv);
    if (app->own.key_fault) {
        /* a key fault reported in this wake wins over any reload, durable or not */
        app->own.cred = DSP_CRED_DEGRADED;
        app->own.epoch = DSP_OWNER_EPOCH_DEGRADED;
    } else if (dsp_auth_usable(full)) {
        app->own.cred = DSP_CRED_CLAIMED;
        app->own.epoch = full->owner_epoch;
    } else if (dsp_auth_is_valid(full) && full->claim_state == DSP_AUTH_UNCLAIMED) {
        app->own.cred = DSP_CRED_UNCLAIMED;
        app->own.epoch = full->owner_epoch;
    } else {
        app->own.cred = DSP_CRED_DEGRADED; /* KEY_FAULT */
        app->own.epoch = DSP_OWNER_EPOCH_DEGRADED;
    }
}

static void owner_set_degraded(dsp_app_t *app)
{
    dsp_auth_wipe(&app->own.cache);
    app->own.cred = DSP_CRED_DEGRADED;
    app->own.epoch = DSP_OWNER_EPOCH_DEGRADED;
}

/* Loads the full record into st.auth_scratch (caller wipes). */
static bool owner_load_full(dsp_app_t *app)
{
    return dsp_storage_load_auth(&app->st, &app->st.auth_scratch) == DSP_STORAGE_OK;
}

static bool retained_save_now(dsp_app_t *app)
{
    dsp_utc_t now = raw_now(app);
    app->retained.saved_raw = now >= 0 && now <= DSP_UTC_MAX ? now : 0;
    uint8_t buf[DSP_POWER_RETAINED_BYTES];
    bool ok = dsp_power_retained_encode(&app->retained, buf) &&
              app->cfg->power->save_retained(app->cfg->power->ctx, buf, sizeof buf);
    dsp_secure_zero(buf, sizeof buf); /* may hold an invite token */
    return ok;
}

static void owner_clear_overlays(dsp_app_t *app)
{
    if (app->retained.invite.valid || app->retained.arm.armed) {
        app->rep.overlays_cleared = true;
    }
    dsp_owner_invite_clear(&app->retained);
    dsp_owner_arm_clear(&app->retained);
}

/* noinline: keeps this frame off the dosing call chain (stack budget) */
__attribute__((noinline)) static void owner_wake_start(dsp_app_t *app)
{
    bool had = app->retained.invite.valid || app->retained.arm.armed;
    dsp_owner_on_wake(&app->retained, app->rep.cause, app->rep.retained_valid, raw_now(app));
    if (app->cfg->ble == NULL || app->cfg->mono == NULL) {
        owner_clear_overlays(app); /* no service mode: nothing could ever use them */
    }
    if (app->retained.overlays_dropped || (had && !app->retained.invite.valid && !app->retained.arm.armed)) {
        app->rep.overlays_cleared = true;
    }
    if (app->cfg->mono != NULL) {
        app->own.ref_valid = true;
        app->own.ref_raw = raw_now(app);
        app->own.ref_mono = mono_ms(app);
    }
}

/* Clock-step detection within the wake: the raw clock must advance with the monotonic clock
 * (tolerance 2 s). Any step clears both overlays (rule 3). */
/* noinline: keeps this frame off the dosing call chain (stack budget) */
__attribute__((noinline)) static void owner_check_clock(dsp_app_t *app)
{
    if (!app->own.ref_valid || app->cfg->mono == NULL) {
        return;
    }
    uint64_t mono = mono_ms(app);
    dsp_utc_t raw = raw_now(app);
    bool step = mono < app->own.ref_mono;
    if (!step) {
        int64_t expected = app->own.ref_raw + (int64_t)((mono - app->own.ref_mono) / 1000u);
        int64_t diff = raw - expected;
        step = diff > 2 || diff < -2;
    }
    if (step) {
        if (app->retained.invite.valid || app->retained.arm.armed) {
            owner_clear_overlays(app);
            (void)retained_save_now(app);
        }
        app->own.ref_raw = raw;
        app->own.ref_mono = mono;
    }
}

/* noinline: keeps this frame off the dosing call chain (stack budget) */
__attribute__((noinline)) static void owner_load(dsp_app_t *app)
{
    dsp_auth_wipe(&app->own.cache);
    app->own.cred = DSP_CRED_UNKNOWN;
    app->own.epoch = DSP_OWNER_EPOCH_DEGRADED;
    if (app->loaded) {
        if (app->st.auth_status == DSP_STORAGE_OK && owner_load_full(app)) {
            owner_set_cache(app, &app->st.auth_scratch);
        } else {
            owner_set_degraded(app); /* never "unclaimed" */
        }
        dsp_auth_wipe(&app->st.auth_scratch);
        /* Recovery arm (O4): verified hold on a power-on or button wake, not for a valid
         * unclaimed device (CLAIM applies), only with service mode. */
        if (app->cfg->recovery_hold_ms >= DSP_OWNER_ARM_HOLD_MS &&
            (app->rep.cause == DSP_CAUSE_POWER_ON || app->rep.cause == DSP_CAUSE_BUTTON) &&
            app->own.cred != DSP_CRED_UNCLAIMED && app->cfg->ble != NULL && app->cfg->mono != NULL) {
            dsp_owner_arm_set(&app->retained, app->own.epoch, raw_now(app));
            app->rep.recovery_armed = retained_save_now(app);
        }
    }
    owner_publish(app);
}

/* ---- Sessions, handshake evidence, issued authorization --------------------------------- */

static bool owner_random(dsp_app_t *app, void *out, size_t len)
{
    const dsp_app_security_t *sec = app->cfg->security;
    return sec != NULL && sec->random != NULL && sec->random(sec->ctx, (uint8_t *)out, len);
}

static void owner_drop_session_evidence(dsp_app_t *app)
{
    app->own.authz_valid = false;
    app->own.authz_token = 0;
    app->own.hs_active = false;
    app->own.hs_nonce = 0;
}

/* noinline: keeps this frame off the dosing call chain (stack budget) */
__attribute__((noinline)) static void owner_session_start(dsp_app_t *app)
{
    owner_drop_session_evidence(app);
    dsp_confirm_reset(&app->own.confirm);
    app->ss.session_id = 0; /* per connection (dsp_app_sec_connection_begin) */
}

void dsp_app_sec_connection_begin(dsp_app_t *app)
{
    if (app == NULL || app->cfg == NULL) {
        return;
    }
    dsp_app_sec_connection_end(app);
    uint32_t id = 0;
    if (app->cfg->security != NULL && owner_random(app, &id, sizeof id) && id == 0) {
        id = 1;
    }
    app->ss.session_id = id;
}

bool dsp_app_sec_connected(const dsp_app_t *app)
{
    return app != NULL && app->cfg != NULL && app->accepting && app->ble_active && app->ss.conn_active;
}

void dsp_app_sec_hello(dsp_app_t *app, dsp_link_hello_t *out)
{
    if (app == NULL || app->cfg == NULL || out == NULL) {
        return;
    }
    fill_hello(app, out);
}

bool dsp_app_sec_send(dsp_app_t *app, const uint8_t *frame, size_t len)
{
    if (!dsp_app_sec_connected(app) || frame == NULL || len == 0 || len > DSP_BLE_FRAME_MAX) {
        return false;
    }
    if (!app->cfg->ble->send_frame(app->cfg->ble->ctx, app->ss.conn_gen, frame, len)) {
        return false;
    }
    app->rep.frames_out++;
    return true;
}

void dsp_app_sec_drop(dsp_app_t *app)
{
    if (dsp_app_sec_connected(app)) {
        link_drop(app);
    }
}

uint64_t dsp_app_sec_now_ms(const dsp_app_t *app)
{
    return app != NULL && app->cfg != NULL && app->cfg->mono != NULL ? mono_ms(app) : 0;
}

uint32_t dsp_app_sec_window_seconds_left(const dsp_app_t *app)
{
    if (app == NULL || app->cfg == NULL || !app->accepting || app->cfg->mono == NULL) {
        return 0;
    }
    uint64_t now = mono_ms(app);
    return app->ss.max_ms > now ? (uint32_t)((app->ss.max_ms - now) / 1000u) : 0;
}

uint32_t dsp_app_sec_epoch(const dsp_app_t *app)
{
    return app != NULL ? app->own.epoch : 0;
}

void dsp_app_sec_connection_end(dsp_app_t *app)
{
    if (app == NULL || app->cfg == NULL) {
        return;
    }
    owner_abort_confirm(app);
    dsp_confirm_reset(&app->own.confirm);
    owner_drop_session_evidence(app);
    app->ss.session_id = 0;
}

static bool owner_confirm_pending(const dsp_app_t *app)
{
    return app->own.confirm.state == DSP_CONFIRM_PENDING;
}

/* A confirmation ended without success: counts against the arm (saved at once). */
static void owner_confirm_failed(dsp_app_t *app)
{
    dsp_confirm_reset(&app->own.confirm);
    dsp_owner_arm_abort(&app->retained);
    (void)retained_save_now(app);
}

static void owner_abort_confirm(dsp_app_t *app)
{
    if (owner_confirm_pending(app)) {
        owner_confirm_failed(app);
    }
}

static void owner_confirm_tick(dsp_app_t *app)
{
    if (owner_confirm_pending(app) && app->cfg->mono != NULL &&
        dsp_confirm_poll(&app->own.confirm, mono_ms(app)) == DSP_CONFIRM_ABORTED) {
        owner_confirm_failed(app);
    }
}

/* noinline: keeps this frame off the dosing call chain (stack budget) */
__attribute__((noinline)) static void owner_session_end(dsp_app_t *app)
{
    owner_abort_confirm(app);
    dsp_confirm_reset(&app->own.confirm); /* also an unused CONFIRMED result */
    owner_drop_session_evidence(app);
    app->ss.session_id = 0;
}

static void owner_button(dsp_app_t *app, const dsp_button_event_t *ev)
{
    if (!owner_confirm_pending(app)) {
        return; /* FR-026 gestures are not part of this milestone */
    }
    dsp_confirm_event(&app->own.confirm, ev);
    owner_confirm_tick(app);
}

/* Voids queued button events (taken before the confirmation started): kind 0 is ignored when
 * popped (no compaction, no copy of frame-sized items). */
static void q_flush_buttons(dsp_app_t *app)
{
    q_lock(app);
    for (size_t i = 0; i < app->q_count; i++) {
        dsp_app_qitem_t *c = &app->q[(app->q_head + i) % DSP_APP_CMDQ_CAPACITY];
        if (c->kind == DSP_QI_BUTTON) {
            c->kind = 0;
        }
    }
    q_unlock(app);
}

/* Preconditions of every security-layer entry: a registered security layer, an active
 * button-wake service session (physical presence), storage loaded, no confirmation pending. */
static dsp_app_op_result_t sec_ready(dsp_app_t *app)
{
    if (app == NULL || app->cfg == NULL) {
        return op(DSP_OP_INVALID, 0, 0);
    }
    if (app->cfg->security == NULL || app->cfg->security->random == NULL) {
        return op(DSP_OP_FORBIDDEN, 0, 0);
    }
    if (!app->accepting || !presence_wake(app) || app->ss.session_id == 0) {
        return op(DSP_OP_BUSY, DSP_OP_B_SESSION, 0);
    }
    if (!app->loaded) {
        return op(DSP_OP_DEGRADED, 0, 0);
    }
    return op(DSP_OP_OK, 0, 0);
}

/* Validates the handshake evidence for this session; consume = single use. Any mismatch also
 * invalidates the stored handle (fail closed). */
static dsp_app_op_result_t hs_check(dsp_app_t *app, const dsp_app_hs_t *hs, uint8_t purpose,
                                    uint8_t purpose2, bool consume)
{
    dsp_app_op_result_t r = sec_ready(app);
    if (r.status != DSP_OP_OK) {
        return r;
    }
    if (owner_confirm_pending(app)) {
        return op(DSP_OP_BUSY, DSP_OP_B_CONFIRMING, 0);
    }
    dsp_app_owner_t *o = &app->own;
    bool ok = hs != NULL && o->hs_active && (hs->purpose == purpose || hs->purpose == purpose2) &&
              hs->purpose == o->hs_purpose && hs->session_id == app->ss.session_id &&
              o->hs_session == app->ss.session_id && hs->nonce == o->hs_nonce && o->hs_nonce != 0;
    if (ok) {
        uint64_t now = mono_ms(app);
        ok = now >= o->hs_start_ms && now - o->hs_start_ms <= DSP_APP_SEC_HS_MAX_MS &&
             o->hs_epoch == o->epoch;
    }
    if (!ok || consume) {
        o->hs_active = false;
        o->hs_nonce = 0;
    }
    return ok ? op(DSP_OP_OK, 0, 0) : op(DSP_OP_FORBIDDEN, 0, 0);
}

static void owner_issue(dsp_app_t *app, uint8_t slot, dsp_app_auth_t *out)
{
    dsp_app_owner_t *o = &app->own;
    uint64_t token = 0;
    if (!owner_random(app, &token, sizeof token) || token == 0) {
        o->authz_valid = false;
        memset(out, 0, sizeof *out);
        return;
    }
    o->authz_valid = true;
    o->authz_slot = slot;
    o->authz_session = app->ss.session_id;
    o->authz_token = token;
    o->authz_epoch = o->epoch;
    out->role = DSP_APP_ROLE_OWNER;
    out->slot = slot;
    out->session_id = app->ss.session_id;
    out->token = token;
}

/* Owner operation guard: guard() plus the ISSUED authorization of this session and epoch. */
static dsp_app_op_result_t owner_guard(dsp_app_t *app, const dsp_app_auth_t *auth)
{
    dsp_app_op_result_t g = guard(app, auth);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    if (!app->own.cache.slot[auth->slot].used) {
        return op(DSP_OP_FORBIDDEN, 0, 0);
    }
    return op(DSP_OP_OK, 0, 0);
}

/* The authorization was issued by this service to the security layer for the current connection
 * (session ID), carries the issued random token and slot, and the ownership epoch is unchanged. */
static bool issued_ok(const dsp_app_t *app, const dsp_app_auth_t *auth)
{
    const dsp_app_owner_t *o = &app->own;
    return app->cfg->security != NULL && o->authz_valid && app->ss.session_id != 0 &&
           auth->session_id == app->ss.session_id && o->authz_session == app->ss.session_id &&
           auth->token == o->authz_token && o->authz_token != 0 && auth->slot == o->authz_slot &&
           o->authz_epoch == o->epoch && o->cred == DSP_CRED_CLAIMED;
}

static dsp_app_op_result_t from_auth(dsp_auth_result_t r)
{
    switch (r) {
    case DSP_AUTH_OK:
        return op(DSP_OP_OK, 0, 0);
    case DSP_AUTH_ERR_SLOTS_FULL:
        return op(DSP_OP_SLOTS_FULL, 0, 0);
    case DSP_AUTH_ERR_DUPLICATE_KEY:
        return op(DSP_OP_DUPLICATE, 0, 0);
    case DSP_AUTH_ERR_NOT_FOUND:
        return op(DSP_OP_NOT_FOUND, 0, 0);
    case DSP_AUTH_ERR_LAST_SLOT:
        return op(DSP_OP_REFUSED, DSP_OP_R_LAST_SLOT, 0);
    case DSP_AUTH_ERR_STATE:
    case DSP_AUTH_ERR_EPOCH:
        return op(DSP_OP_REFUSED, DSP_OP_R_NOT_CLAIMED, 0);
    default:
        return op(DSP_OP_INVALID, 0, 0);
    }
}

/* Persists st.auth_scratch (then wiped) and refreshes the cache; a write failure ends dosing for
 * the wake and re-reads the durable record (old or new). */
static dsp_app_op_result_t owner_commit(dsp_app_t *app, bool ownership_change)
{
    dsp_storage_status_t s = dsp_storage_save_auth(&app->st, &app->st.auth_scratch);
    dsp_auth_wipe(&app->st.auth_scratch);
    if (s != DSP_STORAGE_OK) {
        if (write_failed(s)) {
            storage_failed(app);
        }
        if (owner_load_full(app)) {
            owner_set_cache(app, &app->st.auth_scratch);
        } else {
            owner_set_degraded(app);
        }
        dsp_auth_wipe(&app->st.auth_scratch);
        owner_publish(app);
        return op(write_failed(s) ? DSP_OP_STORAGE : DSP_OP_DEGRADED, 0, 0);
    }
    if (owner_load_full(app)) { /* the durable record is the truth */
        owner_set_cache(app, &app->st.auth_scratch);
    } else {
        owner_set_degraded(app);
    }
    dsp_auth_wipe(&app->st.auth_scratch);
    if (ownership_change) {
        app->rep.owner_changes++;
    }
    owner_publish(app);
    return op(DSP_OP_OK, 0, 0);
}

static dsp_utc_t trusted_now_or_0(const dsp_app_t *app)
{
    dsp_clock_t c = dose_clock(app);
    return c.valid && c.now >= DSP_UTC_MIN && c.now <= DSP_UTC_MAX ? c.now : 0;
}

/* ---- Owner operations ------------------------------------------------------------------- */

dsp_app_op_result_t dsp_app_phones_list(dsp_app_t *app, const dsp_app_auth_t *auth,
                                        dsp_app_phone_t out[DSP_APP_PHONE_SLOTS], uint8_t *n)
{
    dsp_app_op_result_t g = owner_guard(app, auth);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    if (out == NULL || n == NULL) {
        return op(DSP_OP_INVALID, 0, 0);
    }
    *n = 0;
    for (uint8_t i = 0; i < DSP_AUTH_SLOTS; i++) {
        const dsp_auth_slot_t *s = &app->own.cache.slot[i];
        if (!s->used) {
            continue;
        }
        dsp_app_phone_t *p = &out[(*n)++];
        memset(p, 0, sizeof *p);
        p->slot = i;
        p->name_len = s->name_len;
        memcpy(p->name, s->name, sizeof p->name);
        p->enrolled_at = s->enrolled_at;
        p->last_seen = s->last_seen;
        p->is_self = i == auth->slot;
    }
    return done(app, op(DSP_OP_OK, 0, 0));
}

dsp_app_op_result_t dsp_app_phone_remove(dsp_app_t *app, const dsp_app_auth_t *auth, uint8_t slot,
                                         bool *session_ends)
{
    if (session_ends != NULL) {
        *session_ends = false;
    }
    dsp_app_op_result_t g = owner_guard(app, auth);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    if (slot >= DSP_AUTH_SLOTS) {
        return op(DSP_OP_INVALID, 0, 0);
    }
    if (!app->own.cache.slot[slot].used) {
        return op(DSP_OP_NOT_FOUND, 0, 0);
    }
    if (dsp_auth_used_count(&app->own.cache) <= 1) {
        return op(DSP_OP_REFUSED, DSP_OP_R_LAST_SLOT, 0);
    }
    g = write_budget(app);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    if (!owner_load_full(app)) {
        dsp_auth_wipe(&app->st.auth_scratch);
        return op(DSP_OP_DEGRADED, 0, 0);
    }
    dsp_auth_result_t ar = dsp_auth_remove(&app->st.auth_scratch, slot);
    if (ar != DSP_AUTH_OK) {
        dsp_auth_wipe(&app->st.auth_scratch);
        return from_auth(ar);
    }
    write_used(app);
    bool self = slot == auth->slot;
    g = owner_commit(app, true);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    if (self) {
        owner_drop_session_evidence(app);
        app->ss.end = DSP_BLE_END_REVOKED; /* the session ends after this response */
        if (session_ends != NULL) {
            *session_ends = true;
        }
    } else {
        app->own.authz_epoch = app->own.epoch; /* the caller stays authorized */
    }
    return done(app, op(DSP_OP_OK, 0, 0));
}

dsp_app_op_result_t dsp_app_invite_create(dsp_app_t *app, const dsp_app_auth_t *auth,
                                          uint8_t token_out[16], uint16_t *expires_s)
{
    dsp_app_op_result_t g = owner_guard(app, auth);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    if (token_out == NULL || expires_s == NULL) {
        return op(DSP_OP_INVALID, 0, 0);
    }
    if (dsp_auth_free_slot(&app->own.cache) < 0) {
        return op(DSP_OP_SLOTS_FULL, 0, 0);
    }
    owner_check_clock(app);
    uint8_t token[16];
    bool nz = false;
    if (!owner_random(app, token, sizeof token)) {
        dsp_secure_zero(token, sizeof token);
        return op(DSP_OP_DEGRADED, 0, 0);
    }
    for (size_t i = 0; i < sizeof token; i++) {
        nz = nz || token[i] != 0;
    }
    if (!nz) {
        return op(DSP_OP_DEGRADED, 0, 0);
    }
    dsp_owner_invite_set(&app->retained, auth->slot, app->own.epoch, token, raw_now(app));
    if (!retained_save_now(app)) {
        dsp_owner_invite_clear(&app->retained);
        dsp_secure_zero(token, sizeof token);
        return op(DSP_OP_DEGRADED, 0, 0);
    }
    memcpy(token_out, token, sizeof token);
    dsp_secure_zero(token, sizeof token);
    *expires_s = DSP_OWNER_INVITE_S;
    return done(app, op(DSP_OP_OK, 0, 0));
}

dsp_app_op_result_t dsp_app_invite_cancel(dsp_app_t *app, const dsp_app_auth_t *auth)
{
    dsp_app_op_result_t g = owner_guard(app, auth);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    dsp_owner_invite_clear(&app->retained);
    return retained_save_now(app) ? done(app, op(DSP_OP_OK, 0, 0)) : op(DSP_OP_DEGRADED, 0, 0);
}

dsp_app_op_result_t dsp_app_recovery_cancel(dsp_app_t *app, const dsp_app_auth_t *auth)
{
    dsp_app_op_result_t g = owner_guard(app, auth);
    if (g.status != DSP_OP_OK) {
        return g;
    }
    dsp_owner_arm_clear(&app->retained);
    return retained_save_now(app) ? done(app, op(DSP_OP_OK, 0, 0)) : op(DSP_OP_DEGRADED, 0, 0);
}

/* Producer side: fills the tail slot under the lock (no frame-sized copy on the caller's stack). */
static bool q_push(dsp_app_t *app, uint8_t kind, uint32_t gen, uint16_t mtu, const uint8_t *bytes,
                   size_t len, const dsp_button_event_t *ev)
{
    if (app == NULL || app->cfg == NULL || len > DSP_BLE_FRAME_MAX || (len > 0 && bytes == NULL)) {
        return false;
    }
    q_lock(app);
    bool ok = app->accepting && app->q_count < DSP_APP_CMDQ_CAPACITY;
    if (ok) {
        dsp_app_qitem_t *it = &app->q[(app->q_head + app->q_count) % DSP_APP_CMDQ_CAPACITY];
        it->kind = kind;
        it->gen = gen;
        it->mtu = mtu;
        it->len = (uint16_t)len;
        if (len > 0) {
            memcpy(it->bytes, bytes, len);
        }
        if (ev != NULL) {
            it->button = *ev;
        } else {
            memset(&it->button, 0, sizeof it->button);
        }
        app->q_count++;
    }
    q_unlock(app);
    return ok;
}

bool dsp_app_post_button(dsp_app_t *app, const dsp_button_event_t *ev)
{
    if (ev == NULL || (ev->kind != DSP_BUTTON_SHORT && ev->kind != DSP_BUTTON_LONG)) {
        return false;
    }
    return q_push(app, DSP_QI_BUTTON, 0, 0, NULL, 0, ev);
}

bool dsp_app_post_frame(dsp_app_t *app, uint32_t gen, const uint8_t *frame, size_t len)
{
    if (len == 0) {
        return false;
    }
    return q_push(app, DSP_QI_FRAME, gen, 0, frame, len, NULL);
}

bool dsp_app_post_conn_event(dsp_app_t *app, bool connected, uint32_t gen, uint16_t mtu)
{
    if (app == NULL || app->cfg == NULL || gen == 0) {
        return false;
    }
    q_lock(app);
    bool ok = app->accepting;
    if (ok && connected) {
        ok = gen > app->lnk.gen; /* a new connection is always a new, larger generation */
        if (ok) {
            app->lnk.gen = gen;
            app->lnk.up = true;
            app->lnk.mtu = mtu;
            app->lnk.faults = 0;
        }
    } else if (ok && gen >= app->lnk.gen) {
        app->lnk.gen = gen; /* an older generation's disconnect changes nothing */
        app->lnk.up = false;
        app->lnk.faults = 0;
    }
    q_unlock(app);
    return ok;
}

bool dsp_app_post_rx_fault(dsp_app_t *app, uint32_t gen)
{
    if (app == NULL || app->cfg == NULL || gen == 0) {
        return false;
    }
    q_lock(app);
    bool ok = app->accepting && app->lnk.up && gen == app->lnk.gen;
    if (ok && app->lnk.faults < UINT8_MAX) {
        app->lnk.faults++;
    }
    q_unlock(app);
    return ok;
}

/* ---- Security-layer operations (dsp_app_sec.h) ------------------------------------------ */

static bool invite_ok(const dsp_app_t *app)
{
    return app->own.cred == DSP_CRED_CLAIMED &&
           dsp_owner_invite_valid(&app->retained, raw_now(app), app->own.epoch);
}

static bool arm_ok(const dsp_app_t *app)
{
    return (app->own.cred == DSP_CRED_CLAIMED || app->own.cred == DSP_CRED_DEGRADED) &&
           dsp_owner_arm_valid(&app->retained, raw_now(app), app->own.epoch);
}

void dsp_app_sec_view(dsp_app_t *app, dsp_app_sec_view_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    if (app == NULL || app->cfg == NULL) {
        return;
    }
    owner_check_clock(app);
    out->cred_state = app->own.cred;
    out->recovery_armed = arm_ok(app);
    out->invite_active = invite_ok(app);
    out->slot_free = app->own.cred == DSP_CRED_CLAIMED && dsp_auth_free_slot(&app->own.cache) >= 0;
    if (app->own.cred == DSP_CRED_CLAIMED) {
        memcpy(out->id_pub, app->own.cache.id_pub, sizeof out->id_pub);
    }
}

dsp_app_op_result_t dsp_app_sec_admit(dsp_app_t *app, dsp_sec_purpose_t purpose, dsp_app_hs_t *hs)
{
    dsp_app_op_result_t r = sec_ready(app);
    if (r.status != DSP_OP_OK) {
        return r;
    }
    if (hs == NULL) {
        return op(DSP_OP_INVALID, 0, 0);
    }
    memset(hs, 0, sizeof *hs);
    if (owner_confirm_pending(app)) {
        return op(DSP_OP_BUSY, DSP_OP_B_CONFIRMING, 0);
    }
    owner_check_clock(app);
    dsp_app_owner_t *o = &app->own;
    o->hs_active = false; /* a new admission replaces any unconsumed one */
    o->hs_nonce = 0;
    bool allowed;
    switch (purpose) {
    case DSP_SEC_SESSION:
        allowed = o->cred == DSP_CRED_CLAIMED;
        break;
    case DSP_SEC_CLAIM:
        allowed = o->cred == DSP_CRED_UNCLAIMED;
        break;
    case DSP_SEC_ADD:
        allowed = invite_ok(app) && dsp_auth_free_slot(&o->cache) >= 0;
        break;
    case DSP_SEC_RECOVERY:
        allowed = arm_ok(app);
        break;
    default:
        return op(DSP_OP_INVALID, 0, 0);
    }
    if (!allowed) {
        return op(DSP_OP_FORBIDDEN, 0, 0);
    }
    uint64_t nonce = 0;
    if (!owner_random(app, &nonce, sizeof nonce) || nonce == 0) {
        return op(DSP_OP_DEGRADED, 0, 0);
    }
    o->hs_active = true;
    o->hs_purpose = (uint8_t)purpose;
    o->hs_session = app->ss.session_id;
    o->hs_nonce = nonce;
    o->hs_start_ms = mono_ms(app);
    o->hs_epoch = o->epoch;
    hs->purpose = (uint8_t)purpose;
    hs->session_id = app->ss.session_id;
    hs->nonce = nonce;
    return op(DSP_OP_OK, 0, 0);
}

dsp_app_op_result_t dsp_app_sec_identity(dsp_app_t *app, const dsp_app_hs_t *hs,
                                         uint8_t pub[DSP_AUTH_KEY_BYTES],
                                         uint8_t priv[DSP_AUTH_KEY_BYTES])
{
    dsp_app_op_result_t r = hs_check(app, hs, DSP_SEC_SESSION, DSP_SEC_ADD, false);
    if (r.status != DSP_OP_OK) {
        return r;
    }
    if (pub == NULL || priv == NULL) {
        return op(DSP_OP_INVALID, 0, 0);
    }
    if (app->own.cred != DSP_CRED_CLAIMED || !owner_load_full(app) ||
        !dsp_auth_usable(&app->st.auth_scratch)) {
        dsp_auth_wipe(&app->st.auth_scratch);
        return op(DSP_OP_FORBIDDEN, 0, 0);
    }
    memcpy(pub, app->st.auth_scratch.id_pub, DSP_AUTH_KEY_BYTES);
    memcpy(priv, app->st.auth_scratch.id_priv, DSP_AUTH_KEY_BYTES);
    dsp_auth_wipe(&app->st.auth_scratch);
    return op(DSP_OP_OK, 0, 0);
}

dsp_app_op_result_t dsp_app_sec_invite_token(dsp_app_t *app, const dsp_app_hs_t *hs,
                                             uint8_t token[16])
{
    dsp_app_op_result_t r = hs_check(app, hs, DSP_SEC_ADD, DSP_SEC_ADD, false);
    if (r.status != DSP_OP_OK) {
        return r;
    }
    if (token == NULL) {
        return op(DSP_OP_INVALID, 0, 0);
    }
    owner_check_clock(app);
    if (!invite_ok(app)) {
        return op(DSP_OP_FORBIDDEN, 0, 0);
    }
    memcpy(token, app->retained.invite.token, sizeof app->retained.invite.token);
    return op(DSP_OP_OK, 0, 0);
}

dsp_app_op_result_t dsp_app_sec_lookup(dsp_app_t *app, const dsp_app_hs_t *hs,
                                       const uint8_t phone_pub[DSP_AUTH_KEY_BYTES],
                                       dsp_app_auth_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    dsp_app_op_result_t r = hs_check(app, hs, DSP_SEC_SESSION, DSP_SEC_SESSION, true);
    if (r.status != DSP_OP_OK) {
        return r;
    }
    if (phone_pub == NULL || out == NULL) {
        return op(DSP_OP_INVALID, 0, 0);
    }
    int slot = app->own.cred == DSP_CRED_CLAIMED ? dsp_auth_find(&app->own.cache, phone_pub) : -1;
    if (slot < 0) {
        return op(DSP_OP_FORBIDDEN, 0, 0);
    }
    owner_issue(app, (uint8_t)slot, out);
    if (!app->own.authz_valid) {
        return op(DSP_OP_DEGRADED, 0, 0);
    }
    /* last_seen (O8): trusted clock only, at most hourly; diagnostic. */
    dsp_utc_t now = trusted_now_or_0(app);
    if (now != 0 && dsp_auth_touch(&app->own.cache, (uint8_t)slot, now)) {
        if (owner_load_full(app) && dsp_auth_touch(&app->st.auth_scratch, (uint8_t)slot, now)) {
            (void)owner_commit(app, false); /* diagnostic write, not an ownership change */
        } else {
            dsp_auth_wipe(&app->st.auth_scratch);
        }
    }
    return op(DSP_OP_OK, 0, 0);
}

dsp_app_op_result_t dsp_app_sec_enroll(dsp_app_t *app, const dsp_app_hs_t *hs,
                                       const uint8_t phone_pub[DSP_AUTH_KEY_BYTES],
                                       const char *name, size_t name_len,
                                       const uint8_t *new_priv, const uint8_t *new_pub,
                                       dsp_app_auth_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    dsp_app_op_result_t r = hs_check(app, hs, DSP_SEC_CLAIM, DSP_SEC_ADD, true);
    if (r.status != DSP_OP_OK) {
        return r;
    }
    if (phone_pub == NULL || out == NULL) {
        return op(DSP_OP_INVALID, 0, 0);
    }
    bool claim = hs->purpose == DSP_SEC_CLAIM;
    if (claim ? (new_priv == NULL || new_pub == NULL) : (new_priv != NULL || new_pub != NULL)) {
        return op(DSP_OP_INVALID, 0, 0);
    }
    owner_check_clock(app);
    if (claim ? app->own.cred != DSP_CRED_UNCLAIMED : !invite_ok(app)) {
        return op(DSP_OP_FORBIDDEN, 0, 0);
    }
    r = write_budget(app);
    if (r.status != DSP_OP_OK) {
        return r;
    }
    if (!owner_load_full(app)) {
        dsp_auth_wipe(&app->st.auth_scratch);
        return op(DSP_OP_DEGRADED, 0, 0);
    }
    uint8_t slot = 0;
    dsp_utc_t now = trusted_now_or_0(app);
    dsp_auth_result_t ar =
        claim ? dsp_auth_claim(&app->st.auth_scratch, new_priv, new_pub, phone_pub, name, name_len, now)
              : dsp_auth_add(&app->st.auth_scratch, phone_pub, name, name_len, now, &slot);
    if (ar != DSP_AUTH_OK) {
        dsp_auth_wipe(&app->st.auth_scratch);
        return from_auth(ar);
    }
    if (!claim) {
        /* Single use: the invite is consumed (and saved) BEFORE the commit. */
        dsp_owner_invite_clear(&app->retained);
        if (!retained_save_now(app)) {
            dsp_auth_wipe(&app->st.auth_scratch);
            return op(DSP_OP_DEGRADED, 0, 0);
        }
    }
    write_used(app);
    r = owner_commit(app, true);
    if (r.status != DSP_OP_OK) {
        return r;
    }
    owner_issue(app, slot, out);
    return op(app->own.authz_valid ? DSP_OP_OK : DSP_OP_DEGRADED, 0, 0);
}

dsp_app_op_result_t dsp_app_sec_recovery_begin(dsp_app_t *app, const dsp_app_hs_t *hs,
                                               const uint8_t transcript_hash[32])
{
    dsp_app_op_result_t r = hs_check(app, hs, DSP_SEC_RECOVERY, DSP_SEC_RECOVERY, true);
    if (r.status != DSP_OP_OK) {
        return r;
    }
    if (transcript_hash == NULL) {
        return op(DSP_OP_INVALID, 0, 0);
    }
    owner_check_clock(app);
    if (!arm_ok(app)) {
        return op(DSP_OP_FORBIDDEN, 0, 0);
    }
    q_flush_buttons(app); /* presses queued before the operation never count */
    dsp_confirm_begin(&app->own.confirm, app->ss.session_id, transcript_hash, mono_ms(app));
    return op(DSP_OP_OK, 0, 0);
}

dsp_confirm_state_t dsp_app_sec_recovery_state(dsp_app_t *app)
{
    if (app == NULL || app->cfg == NULL || app->cfg->mono == NULL) {
        return DSP_CONFIRM_NONE;
    }
    if (!owner_confirm_pending(app)) {
        return (dsp_confirm_state_t)app->own.confirm.state;
    }
    dsp_confirm_state_t st = dsp_confirm_poll(&app->own.confirm, mono_ms(app));
    if (st == DSP_CONFIRM_ABORTED) {
        owner_confirm_failed(app);
    }
    return st;
}

dsp_app_op_result_t dsp_app_sec_recovery_apply(dsp_app_t *app, const uint8_t transcript_hash[32],
                                               const uint8_t phone_pub[DSP_AUTH_KEY_BYTES],
                                               const char *name, size_t name_len,
                                               const uint8_t new_priv[DSP_AUTH_KEY_BYTES],
                                               const uint8_t new_pub[DSP_AUTH_KEY_BYTES],
                                               dsp_app_auth_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    dsp_app_op_result_t r = sec_ready(app);
    if (r.status != DSP_OP_OK) {
        return r;
    }
    (void)dsp_app_sec_recovery_state(app);
    dsp_confirm_t *c = &app->own.confirm;
    bool bound = c->state == DSP_CONFIRM_CONFIRMED && c->session_id == app->ss.session_id &&
                 transcript_hash != NULL && memcmp(c->transcript, transcript_hash, 32) == 0;
    dsp_confirm_reset(c); /* single use, whatever happens next */
    if (!bound) {
        return op(DSP_OP_FORBIDDEN, 0, 0);
    }
    if (phone_pub == NULL || new_priv == NULL || new_pub == NULL || out == NULL) {
        return op(DSP_OP_INVALID, 0, 0);
    }
    owner_check_clock(app);
    if (!arm_ok(app)) {
        return op(DSP_OP_FORBIDDEN, 0, 0);
    }
    if (!dsp_dosing_state(&app->dosing)) {
        return op(DSP_OP_DEGRADED, 0, 0); /* never without a durable SERVICE_REQUIRED */
    }
    /* New epoch above every epoch an RTC overlay could still carry. */
    uint32_t min_epoch = 1;
    if (app->retained.invite.valid && app->retained.invite.owner_epoch < UINT32_MAX - 2) {
        min_epoch = app->retained.invite.owner_epoch + 1;
    }
    if (app->retained.arm.owner_epoch != DSP_OWNER_EPOCH_DEGRADED &&
        app->retained.arm.owner_epoch + 1 > min_epoch && app->retained.arm.owner_epoch < UINT32_MAX - 2) {
        min_epoch = app->retained.arm.owner_epoch + 1;
    }
    /* 1. The arm (and any invite) is consumed and saved first. */
    owner_clear_overlays(app);
    app->rep.overlays_cleared = false; /* an intended consumption, not an anomaly */
    if (!retained_save_now(app)) {
        return op(DSP_OP_DEGRADED, 0, 0);
    }
    /* 2. Restricting write: SERVICE_REQUIRED (no-op when already). */
    uint32_t gates = 0, problems = 0;
    if (dsp_dosing_set_readiness(&app->dosing, DSP_READINESS_SERVICE_REQUIRED, &gates, &problems) !=
        DSP_DOSING_OK) {
        storage_failed(app);
        return op(DSP_OP_STORAGE, 0, 0);
    }
    /* 3. The new credential record. */
    const dsp_auth_t *prev = NULL;
    if (app->st.auth_status == DSP_STORAGE_OK && owner_load_full(app)) {
        prev = &app->st.auth_scratch;
    }
    dsp_auth_t *next = &app->st.auth_scratch;
    dsp_auth_result_t ar = dsp_auth_recover(prev, min_epoch, new_priv, new_pub, phone_pub, name,
                                            name_len, trusted_now_or_0(app), next);
    if (ar != DSP_AUTH_OK) {
        dsp_auth_wipe(next);
        return from_auth(ar);
    }
    write_used(app);
    r = owner_commit(app, true);
    if (r.status != DSP_OP_OK) {
        return r;
    }
    app->own.key_fault = false; /* the new identity replaces the faulty one */
    owner_publish(app);
    owner_issue(app, 0, out);
    return op(app->own.authz_valid ? DSP_OP_OK : DSP_OP_DEGRADED, 0, 0);
}

dsp_app_op_result_t dsp_app_sec_key_fault(dsp_app_t *app)
{
    if (app == NULL || app->cfg == NULL) {
        return op(DSP_OP_INVALID, 0, 0);
    }
    if (app->cfg->security == NULL) {
        return op(DSP_OP_FORBIDDEN, 0, 0);
    }
    /* 1. Enforcement first, in RAM, independent of any write. */
    app->own.key_fault = true;
    app->rep.key_fault = true;
    bool was_claimed = app->own.cred == DSP_CRED_CLAIMED;
    owner_set_degraded(app);
    owner_drop_session_evidence(app);
    owner_publish(app);
    if (!app->loaded || !was_claimed) {
        return op(DSP_OP_OK, 0, 0);
    }
    /* 2. Diagnostic persistence (supports recovery in later wakes). */
    if (!owner_load_full(app) || dsp_auth_mark_key_fault(&app->st.auth_scratch) != DSP_AUTH_OK) {
        dsp_auth_wipe(&app->st.auth_scratch);
        return op(DSP_OP_DEGRADED, 0, 0);
    }
    dsp_app_op_result_t r = owner_commit(app, true);
    owner_set_degraded(app); /* whatever the write did: degraded for the rest of the wake */
    owner_publish(app);
    return r;
}

void dsp_app_sec_handshake_failed(dsp_app_t *app)
{
    if (sec_ready(app).status != DSP_OP_OK || app->ss.failed_hs_counted) {
        return;
    }
    app->ss.failed_hs_counted = true;
    if (app->own.cred == DSP_CRED_DEGRADED || app->own.cred == DSP_CRED_UNKNOWN) {
        return; /* no valid record to update */
    }
    if (owner_load_full(app) && dsp_auth_count_failed_session(&app->st.auth_scratch)) {
        (void)owner_commit(app, false); /* diagnostic write */
    } else {
        dsp_auth_wipe(&app->st.auth_scratch);
    }
}

/* ---- Public API ------------------------------------------------------------------------ */

static bool config_valid(const dsp_app_config_t *cfg)
{
    return cfg != NULL && cfg->kv != NULL && cfg->tz != NULL && cfg->pump != NULL &&
           cfg->power != NULL && cfg->power->wake_cause != NULL && cfg->power->load_retained != NULL &&
           cfg->power->save_retained != NULL && cfg->power->sleep != NULL &&
           cfg->power->fail_safe != NULL && cfg->clock != NULL && cfg->clock->now != NULL &&
           cfg->time != NULL && cfg->time->reading != NULL &&
           (cfg->ble == NULL ||
            (cfg->ble->start != NULL && cfg->ble->connected != NULL && cfg->ble->suspend != NULL &&
             cfg->ble->resume != NULL && cfg->ble->stop != NULL && cfg->ble->wait != NULL &&
             cfg->ble->send_frame != NULL && cfg->ble->disconnect != NULL && cfg->mono != NULL &&
             cfg->mono->now_ms != NULL)) &&
           (cfg->sync == NULL || (cfg->sync->lock != NULL && cfg->sync->unlock != NULL));
}

/* Its buffer (the whole RTC block) is not on the dosing call chain: noinline (stack budget). */
__attribute__((noinline)) static void load_retained(dsp_app_t *app)
{
    const dsp_app_config_t *cfg = app->cfg;
    dsp_power_retained_defaults(&app->retained);
    if (dsp_power_cause_keeps_retained(app->rep.cause)) {
        uint8_t buf[DSP_POWER_RETAINED_BYTES + 1];
        size_t len = cfg->power->load_retained(cfg->power->ctx, buf, sizeof buf);
        app->rep.retained_valid = dsp_power_retained_decode(buf, len, &app->retained);
        dsp_secure_zero(buf, sizeof buf); /* may hold an invite token */
    }
}

dsp_app_report_t dsp_app_run_wake(dsp_app_t *app, const dsp_app_config_t *cfg)
{
    dsp_app_report_t empty;
    memset(&empty, 0, sizeof empty);
    if (app == NULL) {
        return empty;
    }
    memset(app, 0, sizeof *app);
    app->cfg = cfg;
    /* 1. SAFE_OFF before anything else (even before validating the rest of the wiring). */
    trace(app, DSP_STAGE_SAFE_OFF);
    bool off = cfg != NULL && dsp_dosing_port_safe_off(cfg->pump);
    if (!config_valid(cfg)) {
        app->rep.pump_unconfirmed = !off;
        return app->rep; /* unusable wiring: nothing else can be done safely */
    }
    app->rep.pump_unconfirmed = !off;
    app->rep.cause = cfg->power->wake_cause(cfg->power->ctx);
    load_retained(app);
    app->retained.wake_count++;
    owner_wake_start(app); /* overlay rules 1-4 (reset reason, CRC, raw clock, expiry) */
    app->inhibited = app->retained.failsafe_count >= DSP_POWER_FAILSAFE_LIMIT;
    app->rep.inhibited = app->inhibited;

    if (!app->rep.pump_unconfirmed) {
        /* Battery telemetry: once per wake (POWER_ON, BUTTON, TIMER), before any dosing, Wi-Fi or
         * BLE activity, with the pump confirmed OFF. Never a reason to wake. */
        battery_measure(app);
        /* 2. LOAD (failure = DEGRADED). */
        trace(app, DSP_STAGE_LOAD);
        app->loaded = load(app);
        app->rep.degraded = !app->loaded;
        owner_load(app); /* credentials gate before any dosing (O3: damaged -> no dosing) */
        if (app->loaded) {
            stage_recover(app);
            stage_clock(app);
            stage_observe(app);
            stage_dose(app);
            stage_connect(app);
            if (app->rep.conn_runs > 0 && app->rep.conn[0] != DSP_CONN_SKIPPED_GUARD &&
                app->rep.conn[0] != DSP_CONN_SKIPPED_DOSE_SOON &&
                app->rep.conn[0] != DSP_CONN_SKIPPED_NOT_DUE) {
                stage_dose(app); /* the clock may have become trusted or stepped */
            }
        }
        if (presence_wake(app) && cfg->ble != NULL) {
            stage_ble(app);
            if (app->loaded && app->rep.ops > 0) {
                stage_connect(app); /* e.g. a CRITICAL event from a manual dose */
            }
        }
        stage_sync(app);
    }
    stage_plan(app);
    stage_shutdown_and_sleep(app);
    return app->rep;
}

