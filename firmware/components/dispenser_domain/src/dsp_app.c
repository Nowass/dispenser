#include "dsp_app.h"

#include <string.h>

/* ---- Helpers --------------------------------------------------------------------------- */

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
    return app->loaded && !app->inhibited && !app->pump_fault && !dsp_dosing_degraded(&app->dosing);
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

static void stage_clock(dsp_app_t *app)
{
    trace(app, DSP_STAGE_CLOCK);
    dsp_timekeeper_evaluate(&app->tk,
                            app->cfg->time->reading(app->cfg->time->ctx, app->tk.meta.last_sync_utc));
}

static void stage_observe(dsp_app_t *app)
{
    trace(app, DSP_STAGE_OBSERVE);
    (void)dsp_notify_observe_time(&app->notify, &app->tk, raw_now(app));
    (void)dsp_notify_tick(&app->notify, notify_clock(app));
    (void)dsp_dosing_observe_reservoir(&app->dosing, notify_clock(app));
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
    if (app->cfg->wifi == NULL || app->cfg->https == NULL || app->cfg->ntp == NULL ||
        app->rep.conn_runs >= 2) {
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
}

/* ---- Service mode ------------------------------------------------------------------------ */

static void ble_suspend(dsp_app_t *app)
{
    if (app->ble_active) {
        app->cfg->ble->suspend(app->cfg->ble->ctx);
        app->ble_active = false;
        app->rep.ble_suspends++;
    }
}

/* Service-session lifetime on the monotonic clock (milliseconds). */
typedef struct {
    uint64_t start_ms;
    uint64_t max_ms;           /* absolute end of the maximum total duration */
    uint64_t idle_deadline_ms; /* absolute end of the idle timeout (no client) */
    uint64_t idle_ms;
    bool was_connected;
    uint8_t end;               /* dsp_app_ble_end_t */
} session_t;

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
static bool ble_resume(dsp_app_t *app, session_t *ss, bool was_connected)
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

static bool q_pop(dsp_app_t *app, dsp_app_cmd_t *out)
{
    q_lock(app);
    bool got = app->q_count > 0;
    if (got) {
        *out = app->q[app->q_head];
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
    q_unlock(app);
}

static void fill_status(dsp_app_t *app, dsp_app_response_t *r)
{
    r->storage_available = app->loaded;
    if (!app->loaded) {
        return;
    }
    const dsp_dosing_state_t *ds = dsp_dosing_state(&app->dosing);
    if (ds != NULL) {
        r->readiness = ds->readiness;
        r->dosing_flags = ds->flags;
    }
    dsp_config_t cfg;
    if (dsp_storage_load_config(&app->st, &cfg) == DSP_STORAGE_OK) {
        r->capacity_ul = cfg.reservoir_capacity_ul;
        r->remaining_ul = cfg.reservoir_remaining_ul;
    }
    r->gates = dsp_dosing_gates(&app->dosing, dose_clock(app).valid, &r->pump_problems);
    r->next_due_known = next_due(app, &r->next_due_at);
}

static dsp_app_resp_status_t from_dosing(dsp_dosing_status_t st)
{
    switch (st) {
    case DSP_DOSING_OK:
        return DSP_RESP_OK;
    case DSP_DOSING_ERR_ARG:
        return DSP_RESP_ERR_ARG;
    case DSP_DOSING_ERR_STORAGE:
        return DSP_RESP_STORAGE;
    case DSP_DOSING_PUMP_FAILED:
        return DSP_RESP_FAILED;
    default:
        return DSP_RESP_REFUSED;
    }
}

/* Returns false when the session must end. */
static bool handle_command(dsp_app_t *app, const dsp_app_cmd_t *cmd, session_t *ss)
{
    dsp_app_response_t r;
    memset(&r, 0, sizeof r);
    r.id = cmd->id;
    r.kind = cmd->kind;
    bool keep = true;
    app->rep.commands++;
    if (cmd->kind == DSP_CMD_END_SESSION) {
        keep = false;
        ss->end = DSP_BLE_END_COMMAND;
    } else if (cmd->kind == DSP_CMD_STATUS) {
        r.status = DSP_RESP_OK;
    } else if (!app->loaded) {
        r.status = DSP_RESP_DEGRADED; /* read-only session (Q12) */
    } else if (cmd->kind == DSP_CMD_MANUAL_DOSE) {
        if (!dosing_usable(app)) {
            r.status = DSP_RESP_DEGRADED;
        } else {
            /* P9: BLE off before the pump; a due scheduled dose goes first (Q8). */
            bool was_connected = ble_connected(app);
            ble_suspend(app);
            if (dose_due_now(app)) {
                stage_dose(app);
            }
            if (!dosing_usable(app)) {
                r.status = DSP_RESP_DEGRADED;
            } else {
                dsp_dosing_result_t d = dsp_dosing_manual(&app->dosing, cmd->volume_ul, dose_clock(app));
                r.dosing = d.status;
                r.status = from_dosing(d.status);
                r.limits = d.limits;
                if (d.status == DSP_DOSING_OK) {
                    app->rep.manual_doses++;
                }
                if (d.status == DSP_DOSING_PUMP_FAILED) {
                    app->pump_fault = true;
                    app->rep.pump_fault = true;
                }
                app->rep.dosing_degraded = dsp_dosing_degraded(&app->dosing);
            }
            keep = ble_resume(app, ss, was_connected);
        }
    } else if (cmd->kind == DSP_CMD_REFILL) {
        r.dosing = dsp_dosing_refill(&app->dosing, cmd->capacity_ul, cmd->remaining_ul,
                                     notify_clock(app));
        r.status = from_dosing(r.dosing);
    } else if (cmd->kind == DSP_CMD_SET_READINESS) {
        uint32_t gates = 0;
        r.dosing = dsp_dosing_set_readiness(&app->dosing, (dsp_readiness_t)cmd->readiness, &gates,
                                            &r.pump_problems);
        r.status = from_dosing(r.dosing);
        r.gates = gates;
    } else {
        r.status = DSP_RESP_ERR_ARG;
    }
    uint32_t refused_gates = r.gates;
    fill_status(app, &r);
    if (cmd->kind == DSP_CMD_SET_READINESS && r.status != DSP_RESP_OK) {
        r.gates = refused_gates; /* why READY was refused */
    }
    app->cfg->ble->respond(app->cfg->ble->ctx, &r);
    app->rep.responses++;
    return keep;
}

static void stage_ble(dsp_app_t *app)
{
    trace(app, DSP_STAGE_BLE);
    const dsp_app_config_t *cfg = app->cfg;
    const dsp_ble_port_t *ble = cfg->ble;
    session_t ss;
    memset(&ss, 0, sizeof ss);
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
    ss.start_ms = mono_ms(app);
    ss.idle_ms = (uint64_t)idle_s * 1000u;
    ss.max_ms = ss.start_ms + (uint64_t)max_s * 1000u;
    ss.idle_deadline_ms = ss.start_ms + ss.idle_ms;
    bool stalled = false; /* a due dose made no progress: wait before re-checking */
    for (uint32_t it = 0; it < DSP_APP_BLE_MAX_ITERATIONS && ss.end == DSP_BLE_END_NONE; it++) {
        uint64_t now = mono_ms(app);
        if (now < ss.start_ms) {
            ss.end = DSP_BLE_END_CLOCK; /* a monotonic clock never goes back: end conservatively */
            break;
        }
        if (now >= ss.max_ms) {
            ss.end = DSP_BLE_END_MAX;
            break;
        }
        bool conn = ble_connected(app);
        if (conn) {
            ss.was_connected = true;
        } else if (ss.was_connected) {
            ss.was_connected = false; /* disconnected: the idle timeout restarts */
            ss.idle_deadline_ms = now + ss.idle_ms;
        }
        if (!conn && now >= ss.idle_deadline_ms) {
            ss.end = DSP_BLE_END_IDLE;
            break;
        }
        if (!stalled && dose_due_now(app)) {
            /* Q8: scheduled dosing first; BLE suspended for the pump, resumed afterwards. */
            size_t before = app->rep.dosed + app->rep.pump_failures;
            ble_suspend(app);
            stage_dose(app);
            stalled = app->rep.dosed + app->rep.pump_failures == before;
            if (!ble_resume(app, &ss, conn)) {
                break;
            }
            continue;
        }
        dsp_app_cmd_t cmd;
        if (q_pop(app, &cmd)) {
            if (!handle_command(app, &cmd, &ss)) {
                break;
            }
            continue;
        }
        uint64_t deadline = conn || ss.idle_deadline_ms > ss.max_ms ? ss.max_ms : ss.idle_deadline_ms;
        uint64_t wait_ms = deadline - now;
        dsp_utc_t at = 0;
        dsp_utc_t wall = raw_now(app);
        if (dosing_usable(app) && next_due(app, &at)) {
            uint64_t due_ms = at <= wall ? 0 : (uint64_t)(at - wall) * 1000u;
            if (due_ms < wait_ms) {
                wait_ms = due_ms;
            }
        }
        if (stalled || wait_ms < 1000u) {
            wait_ms = 1000u;
        }
        stalled = false;
        if (wait_ms > 3600000u) {
            wait_ms = 3600000u;
        }
        ble->wait(ble->ctx, (uint32_t)wait_ms);
    }
    if (ss.end == DSP_BLE_END_NONE) {
        ss.end = DSP_BLE_END_MAX; /* iteration bound */
    }
    app->rep.ble_end = ss.end;
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

static void stage_shutdown_and_sleep(dsp_app_t *app)
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
             cfg->ble->respond != NULL && cfg->mono != NULL && cfg->mono->now_ms != NULL)) &&
           (cfg->sync == NULL || (cfg->sync->lock != NULL && cfg->sync->unlock != NULL));
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
    dsp_power_retained_defaults(&app->retained);
    if (dsp_power_cause_keeps_retained(app->rep.cause)) {
        uint8_t buf[DSP_POWER_RETAINED_BYTES + 1];
        size_t len = cfg->power->load_retained(cfg->power->ctx, buf, sizeof buf);
        app->rep.retained_valid = dsp_power_retained_decode(buf, len, &app->retained);
    }
    app->retained.wake_count++;
    app->inhibited = app->retained.failsafe_count >= DSP_POWER_FAILSAFE_LIMIT;
    app->rep.inhibited = app->inhibited;

    if (!app->rep.pump_unconfirmed) {
        /* 2. LOAD (failure = DEGRADED). */
        trace(app, DSP_STAGE_LOAD);
        app->loaded = load(app);
        app->rep.degraded = !app->loaded;
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
        if (app->rep.cause == DSP_CAUSE_BUTTON && cfg->ble != NULL) {
            stage_ble(app);
            if (app->loaded && app->rep.commands > 0) {
                stage_connect(app); /* e.g. a CRITICAL event from a manual dose */
            }
        }
        stage_sync(app);
    }
    stage_plan(app);
    stage_shutdown_and_sleep(app);
    return app->rep;
}

bool dsp_app_post(dsp_app_t *app, const dsp_app_cmd_t *cmd)
{
    if (app == NULL || app->cfg == NULL || cmd == NULL || cmd->kind < DSP_CMD_STATUS ||
        cmd->kind > DSP_CMD_END_SESSION) {
        return false;
    }
    q_lock(app);
    bool ok = app->accepting && app->q_count < DSP_APP_CMDQ_CAPACITY;
    if (ok) {
        app->q[(app->q_head + app->q_count) % DSP_APP_CMDQ_CAPACITY] = *cmd;
        app->q_count++;
    }
    q_unlock(app);
    return ok;
}
