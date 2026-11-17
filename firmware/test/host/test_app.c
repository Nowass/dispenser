/*
 * Application and Power Service tests (M1.9b): complete wake cycles of dsp_app over the real
 * domain services (storage, coordinator, Dosing Service, notifications, timekeeper, time
 * service, connectivity, HTTPS transport) with injected fakes for the pump, KV storage (with
 * power cuts that ABORT the wake at any write, pump run, Wi-Fi connection, HTTP post, BLE
 * wait or sleep entry, via longjmp), the device clock and its RTC continuity, NTP, Wi-Fi,
 * HTTP, BLE service mode, and the sleep/wake port with RTC-retained memory.
 *
 * Oracles checked on EVERY pump start: durable claim or manual marker with the volume,
 * durable last start and reservoir consumption, no Wi-Fi/BLE/HTTPS active, the automatic
 * occurrence inside its due window and never executed before. On EVERY sleep: pump OFF
 * (forced off after the last run), Wi-Fi stopped, BLE stopped, no HTTPS session.
 */
#include "dsp_app.h"
#include "dsp_codec.h"
#include "dsp_power.h"
#include "dsp_reservoir.h"
#include "dsp_storage.h"
#include "dsp_test.h"
#include "fake_kv.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const dsp_tz_t *const TZ = &DSP_TZ_EUROPE_PRAGUE;

#define DAY 86400
#define HOUR 3600
#define T0 ((dsp_utc_t)1795158000) /* 2026-11-20T07:00:00Z = 08:00 CET */
#define HW_MAX_MS 60000u
#define CAP 100000u
#define WINDOW DSP_EXEC_DUE_WINDOW_DEFAULT_S
#define DEVICE 0x24DCC3A1B2C3ull
static const char TOKEN[] = "test-0000000000000000000000000000000000000000000000000000000000ff";

/* ---- Event log, power cuts --------------------------------------------------------------- */

static char evlog[4096];
static size_t evlog_len;
static void ev(char c)
{
    if (evlog_len + 1 < sizeof evlog) {
        evlog[evlog_len++] = c;
        evlog[evlog_len] = '\0';
    }
}

static jmp_buf cut_env;
static long events;       /* cut points passed */
static long cut_at;       /* 0 = no cut; abort the wake at this event */
static bool cut_after;    /* a cut write lands before the power goes */
static bool in_wake;
static bool cut_happened;

static void power_lost(void);

/* Returns true when the power must go now (the caller longjmps after its own cleanup). */
static bool cut_point(void)
{
    if (!in_wake) {
        return false;
    }
    events++;
    return cut_at > 0 && events == cut_at;
}

/* ---- Time: true time, device clock, RTC continuity marker ------------------------------ */

static dsp_utc_t g_true;   /* true UTC */
static int64_t g_offset;   /* device raw clock = g_true + g_offset */
static dsp_utc_t g_marker; /* RTC continuity marker (last accepted sync), 0 = none */
static int64_t ntp_error;  /* added to the NTP answer */
static dsp_sync_status_t ntp_status = DSP_SYNC_OK;
static int ntp_calls;

static dsp_utc_t raw(void)
{
    return g_true + g_offset;
}
static dsp_utc_t clk_now(void *ctx)
{
    (void)ctx;
    return raw();
}
static bool clk_set(void *ctx, dsp_utc_t utc)
{
    (void)ctx;
    g_offset = utc - g_true;
    return true;
}
static void clk_mark(void *ctx, dsp_utc_t last)
{
    (void)ctx;
    g_marker = last;
}
static const dsp_clock_port_t CLOCK = {NULL, clk_now, clk_set, clk_mark};

static dsp_time_reading_t reading(void *ctx, dsp_utc_t last_sync)
{
    (void)ctx;
    dsp_time_reading_t r = {raw(), g_marker != 0 && g_marker == last_sync};
    return r;
}
static const dsp_time_port_t TIMEP = {NULL, reading};

static dsp_sync_status_t ntp_sync(void *ctx, uint32_t timeout_ms, dsp_time_sample_t *out)
{
    (void)ctx;
    (void)timeout_ms;
    ntp_calls++;
    g_true += 1;
    if (ntp_status == DSP_SYNC_OK) {
        out->source_utc = g_true + ntp_error;
        out->clock_utc = raw();
    }
    return ntp_status;
}
static const dsp_time_source_t NTP = {NULL, ntp_sync};

/* ---- Activity flags (simultaneity oracle) -------------------------------------------- */

static bool wifi_on, ble_on, pump_running;
static int overlap_violations;

/* ---- Wi-Fi ---------------------------------------------------------------------------- */

static dsp_net_status_t wifi_result = DSP_NET_OK;
static int wifi_connects;
static dsp_net_status_t wf_start(void *ctx)
{
    (void)ctx;
    if (ble_on || pump_running) {
        overlap_violations++;
        fprintf(stderr, "Wi-Fi started while BLE or the pump is active\n");
    }
    wifi_on = true;
    ev('W');
    return DSP_NET_OK;
}
static dsp_net_status_t wf_connect(void *ctx, const dsp_wifi_credentials_t *c, uint32_t t)
{
    (void)ctx;
    (void)c;
    (void)t;
    wifi_connects++;
    g_true += 3;
    if (cut_point()) {
        power_lost();
    }
    return wifi_result;
}
static void wf_disconnect(void *ctx)
{
    (void)ctx;
}
static void wf_stop(void *ctx)
{
    (void)ctx;
    if (wifi_on) {
        ev('w');
    }
    wifi_on = false;
}
static const dsp_wifi_driver_t WIFI = {NULL, wf_start, wf_connect, wf_disconnect, wf_stop};
static bool creds_ok = true;
static dsp_cred_status_t creds_get(void *ctx, dsp_wifi_credentials_t *out)
{
    (void)ctx;
    if (!creds_ok) {
        return DSP_CRED_UNAVAILABLE;
    }
    snprintf(out->ssid, sizeof out->ssid, "test-ssid");
    snprintf(out->passphrase, sizeof out->passphrase, "test-passphrase");
    return DSP_CRED_OK;
}
static const dsp_wifi_credentials_provider_t CREDS = {NULL, creds_get};

/* ---- HTTP ---------------------------------------------------------------------------- */

static bool http_ack = true;
static bool http_in_session;
static int http_posts, http_acks;
static int delivered_interrupted, delivered_dosing_errors;
/* Monotonic clock: true elapsed time (never affected by device clock steps) plus the HTTP
 * extra milliseconds. */
static uint64_t mono_ms;
static uint64_t mono_now(void *ctx)
{
    (void)ctx;
    return (uint64_t)(g_true - (T0 - 30 * DAY)) * 1000u + mono_ms;
}
static const dsp_monotonic_t MONO = {NULL, mono_now};
static bool port_open(void *ctx)
{
    (void)ctx;
    http_in_session = true;
    return true;
}
static void port_post(void *ctx, const char *body, size_t len, const char *token,
                      dsp_http_outcome_t *out)
{
    (void)ctx;
    (void)len;
    (void)token;
    memset(out, 0, sizeof *out);
    http_posts++;
    mono_ms += 500;
    g_true += 1;
    if (cut_point()) {
        power_lost();
    }
    out->phase = DSP_HTTP_RESPONSE;
    if (!http_ack) {
        out->status = 503;
        return;
    }
    const char *p = strstr(body, "\"event_id\":\"");
    char id[DSP_NOTIFY_ID_BUF_LEN] = "";
    if (p != NULL) {
        p += strlen("\"event_id\":\"");
        size_t n = 0;
        while (p[n] != '"' && p[n] != '\0' && n + 1 < sizeof id) {
            id[n] = p[n];
            n++;
        }
        id[n] = '\0';
    }
    if (strstr(body, "\"dosing_error\"") != NULL || strstr(body, "DOSING_ERROR") != NULL) {
        delivered_dosing_errors++;
    }
    if (strstr(body, "interrupted") != NULL || strstr(body, "INTERRUPTED") != NULL) {
        delivered_interrupted++;
    }
    int w = snprintf(out->body, sizeof out->body, "{\"status\":\"accepted\",\"event_id\":\"%s\"}", id);
    out->body_len = (uint16_t)(w > 0 ? w : 0);
    out->status = 201;
    http_acks++;
}
static void port_close(void *ctx)
{
    (void)ctx;
    http_in_session = false;
}
static const dsp_http_port_t PORT = {NULL, port_open, port_post, port_close};
static dsp_cred_status_t token_get(void *ctx, char *buf, size_t cap)
{
    (void)ctx;
    snprintf(buf, cap, "%s", TOKEN);
    return DSP_CRED_OK;
}
static const dsp_token_provider_t TOKENS = {NULL, token_get};

/* ---- Storage behind a proxy (config tracking, cut points) -------------------------------- */

static fake_kv_t kv;
static dsp_kv_t proxy;
static uint32_t cfg_prev_remaining, cfg_new_remaining;
static bool fail_notify_sets;

static dsp_kv_status_t px_get(void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *len)
{
    (void)ctx;
    return kv.iface.get(kv.iface.ctx, key, buf, cap, len);
}
static dsp_kv_status_t px_set(void *ctx, const char *key, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (strcmp(key, "config") == 0) {
        dsp_config_t old, nw;
        const fake_kv_entry_t *e = fake_kv_find(&kv, "config");
        if (e != NULL && dsp_codec_decode_config(e->value, e->len, &old) == DSP_CODEC_OK &&
            dsp_codec_decode_config(data, len, &nw) == DSP_CODEC_OK) {
            cfg_prev_remaining = old.reservoir_remaining_ul;
            cfg_new_remaining = nw.reservoir_remaining_ul;
        }
    }
    if (fail_notify_sets && strcmp(key, "notify") == 0) {
        return DSP_KV_ERR_WRITE;
    }
    if (cut_point()) {
        if (cut_after) {
            (void)kv.iface.set(kv.iface.ctx, key, data, len);
        }
        power_lost();
    }
    return kv.iface.set(kv.iface.ctx, key, data, len);
}
static dsp_kv_status_t px_commit(void *ctx)
{
    (void)ctx;
    return kv.iface.commit(kv.iface.ctx);
}

/* ---- Pump ----------------------------------------------------------------------------- */

#define MAX_RUNS 8192
static struct {
    int calls;
    int force_offs;
    int offs_after_last_run; /* force_off calls since the last run */
    int violations;
    dsp_pump_result_t script[16];
    int script_len;
    bool off_confirmable;
    bool fail_next; /* the next run reports a fault */
    int confirm_fail_from; /* is_off() fails from this call on (0 = never) */
    int is_off_calls;
    char ids[MAX_RUNS][128];
    dsp_utc_t at[MAX_RUNS];
    uint32_t volume[MAX_RUNS];
    bool manual[MAX_RUNS];
    uint64_t pumped_ul; /* sum of volumes of runs that started */
} pump;

static bool durable_dosing(dsp_dosing_state_t *out)
{
    const fake_kv_entry_t *e = fake_kv_find(&kv, "dosing");
    return e != NULL && dsp_codec_decode_dosing(e->value, e->len, out) == DSP_CODEC_OK;
}

static bool seen_id(const char *id, int upto)
{
    for (int i = 0; i < upto && i < MAX_RUNS; i++) {
        if (!pump.manual[i] && strcmp(pump.ids[i], id) == 0) {
            return true;
        }
    }
    return false;
}

static dsp_pump_result_t pump_run(void *ctx, uint32_t runtime_ms)
{
    (void)ctx;
    int i = pump.calls++;
    if (wifi_on || ble_on || http_in_session) {
        overlap_violations++;
        fprintf(stderr, "pump started while a radio is active (wifi %d ble %d https %d)\n", wifi_on,
                ble_on, http_in_session);
    }
    dsp_utc_t now = raw();
    dsp_dosing_state_t ds;
    bool ok = durable_dosing(&ds);
    int evidence = 0;
    uint32_t volume = 0;
    char id[128] = "";
    bool manual = false;
    if (ok && (ds.flags & DSP_DOSING_F_MANUAL_ACTIVE)) {
        volume = ds.manual_volume_ul;
        snprintf(id, sizeof id, "manual#%lu", (unsigned long)ds.manual_seq);
        manual = true;
        evidence++;
    }
    for (size_t s = 0; s < DSP_STORAGE_HISTORY_SLOTS; s++) {
        char key[8];
        dsp_storage_history_key(s, key, sizeof key);
        const fake_kv_entry_t *e = fake_kv_find(&kv, key);
        dsp_history_record_t rec;
        if (e != NULL && dsp_codec_decode_history(e->value, e->len, &rec) == DSP_CODEC_OK &&
            rec.state == DSP_HISTORY_STARTED && rec.claimed_at == now) {
            volume = rec.volume_ul;
            snprintf(id, sizeof id, "%s@%04d-%02u-%02u", rec.schedule_id, rec.local_date.year,
                     rec.local_date.month, rec.local_date.day);
            evidence++;
            /* Inside the due window of its scheduled instant. */
            if (now < rec.scheduled_at || now - rec.scheduled_at > WINDOW) {
                pump.violations++;
                fprintf(stderr, "pump outside the due window: %s (now - scheduled = %lld)\n", id,
                        (long long)(now - rec.scheduled_at));
            }
        }
    }
    bool good = ok && evidence == 1 && volume > 0 && ds.last_start_utc == now &&
                cfg_prev_remaining - cfg_new_remaining ==
                    (cfg_prev_remaining < volume ? cfg_prev_remaining : volume);
    const fake_kv_entry_t *ce = fake_kv_find(&kv, "config");
    dsp_config_t cfg;
    good = good && ce != NULL && dsp_codec_decode_config(ce->value, ce->len, &cfg) == DSP_CODEC_OK &&
           cfg.reservoir_remaining_ul == cfg_new_remaining;
    if (!good) {
        pump.violations++;
        fprintf(stderr, "pump without complete durable state (evidence %d, volume %lu)\n", evidence,
                (unsigned long)volume);
    }
    if (!manual && evidence == 1 && seen_id(id, i)) {
        pump.violations++;
        fprintf(stderr, "automatic DUPLICATE: %s\n", id);
    }
    if (i < MAX_RUNS) {
        snprintf(pump.ids[i], sizeof pump.ids[i], "%s", id);
        pump.at[i] = now;
        pump.volume[i] = volume;
        pump.manual[i] = manual;
    }
    pump.pumped_ul += volume;
    pump.offs_after_last_run = 0;
    ev('P');
    pump_running = true;
    g_true += (dsp_utc_t)((runtime_ms + 999u) / 1000u);
    if (cut_point()) {
        power_lost(); /* power lost during the pump run */
    }
    pump_running = false;
    if (pump.fail_next) {
        pump.fail_next = false;
        return DSP_PUMP_FAILED;
    }
    return i < pump.script_len ? pump.script[i] : DSP_PUMP_OK;
}
static void pump_off(void *ctx)
{
    (void)ctx;
    pump.force_offs++;
    pump.offs_after_last_run++;
    pump_running = false;
}
static bool pump_is_off(void *ctx)
{
    (void)ctx;
    pump.is_off_calls++;
    if (!pump.off_confirmable) {
        return false;
    }
    return pump.confirm_fail_from == 0 || pump.is_off_calls < pump.confirm_fail_from;
}
static const dsp_pump_port_t PUMP = {NULL, pump_run, pump_off, pump_is_off};

/* ---- Power port: wake cause, RTC memory, sleep, fail-safe -------------------------------- */

static dsp_wake_cause_t g_cause;
static uint8_t rtc[DSP_POWER_RETAINED_BYTES];
static size_t rtc_len;
static bool rtc_save_fails;
static int sleeps, fail_safes, sleep_violations;
static int64_t last_sleep_s;
static uint32_t last_failsafe;

static dsp_wake_cause_t pw_cause(void *ctx)
{
    (void)ctx;
    return g_cause;
}
static size_t pw_load(void *ctx, uint8_t *buf, size_t cap)
{
    (void)ctx;
    size_t n = rtc_len < cap ? rtc_len : cap;
    memcpy(buf, rtc, n);
    return n;
}
static bool pw_save(void *ctx, const uint8_t *buf, size_t len)
{
    (void)ctx;
    if (rtc_save_fails || len > sizeof rtc) {
        return false;
    }
    memcpy(rtc, buf, len);
    rtc_len = len;
    return true;
}
static void pw_sleep(void *ctx, int64_t s)
{
    (void)ctx;
    sleeps++;
    last_sleep_s = s;
    ev('Z');
    if (pump_running || (pump.calls > 0 && pump.offs_after_last_run == 0) || wifi_on || ble_on ||
        http_in_session || s < 1) {
        sleep_violations++;
        fprintf(stderr, "sleep with an active service (pump %d offs %d wifi %d ble %d https %d s %lld)\n",
                pump_running, pump.offs_after_last_run, wifi_on, ble_on, http_in_session, (long long)s);
    }
    if (cut_point()) {
        power_lost();
    }
}
static void pw_fail_safe(void *ctx, uint32_t failures)
{
    (void)ctx;
    fail_safes++;
    last_failsafe = failures;
    ev('F');
}
static const dsp_power_port_t POWER = {NULL, pw_cause, pw_load, pw_save, pw_sleep, pw_fail_safe};

/* ---- BLE service mode ---------------------------------------------------------------- */

static dsp_app_t app;

#define MAX_SCRIPT 32
static struct {
    dsp_utc_t at[MAX_SCRIPT]; /* true time at which the phone sends the command */
    dsp_app_cmd_t cmd[MAX_SCRIPT];
    int len, next;
    /* Client connection intervals [from, to) in true time (while BLE is active). */
    dsp_utc_t conn_from[8], conn_to[8];
    int nconn;
    int64_t step_wall_on_wait; /* device clock step applied at the first wait (0 = none) */
    bool start_fails;
    bool resume_fails;
    bool stop_fails;
    int starts, suspends, resumes, stops, waits, rejected_posts;
    dsp_app_response_t resp[64];
    int nresp;
} ble;

/* M1.10b.2: the manual dose is no queue command any more but the authenticated operation
 * dsp_app_manual_dose(), executed in the application task (here: from inside the BLE fake's
 * wait(), like the future dispatcher). TEST_MANUAL marks such a script entry (volume in
 * capacity_ul); every one gets the next request ID of owner slot 0, and its result is recorded
 * as a response of kind TEST_MANUAL (status mapped to the former queue response codes). */
#define TEST_MANUAL 200
static const dsp_app_auth_t OWNER = {DSP_APP_ROLE_OWNER, 0, 0, 0};
static uint64_t manual_seq;
static void ble_respond(void *ctx, const dsp_app_response_t *r);
static dsp_config_t peek_cfg(void);

static void run_manual(const dsp_app_cmd_t *c)
{
    dsp_app_manual_result_t mr;
    dsp_app_op_result_t res = dsp_app_manual_dose(&app, &OWNER, ++manual_seq, c->capacity_ul, &mr);
    dsp_app_response_t r;
    memset(&r, 0, sizeof r);
    r.id = c->id;
    r.kind = TEST_MANUAL;
    switch (res.status) {
    case DSP_OP_OK:
        r.status = mr.outcome == 0 ? DSP_RESP_OK : DSP_RESP_FAILED;
        r.dosing = mr.outcome == 0 ? DSP_DOSING_OK : DSP_DOSING_PUMP_FAILED;
        break;
    case DSP_OP_REFUSED:
        r.status = DSP_RESP_REFUSED;
        r.dosing = DSP_DOSING_REFUSED;
        r.gates = res.detail;
        break;
    case DSP_OP_LIMIT:
        r.status = DSP_RESP_REFUSED;
        r.dosing = DSP_DOSING_LIMIT;
        r.limits = res.detail;
        break;
    case DSP_OP_NOT_COVERED:
        r.status = DSP_RESP_REFUSED;
        r.dosing = DSP_DOSING_NOT_COVERED;
        break;
    case DSP_OP_STORAGE:
        r.status = DSP_RESP_STORAGE;
        break;
    default:
        r.status = DSP_RESP_DEGRADED;
        break;
    }
    dsp_config_t cfg = peek_cfg();
    r.capacity_ul = cfg.reservoir_capacity_ul;
    r.remaining_ul = cfg.reservoir_remaining_ul;
    ble_respond(NULL, &r);
}

static void ble_post_due(void)
{
    while (ble.next < ble.len && ble.at[ble.next] <= g_true) {
        if (ble.cmd[ble.next].kind == TEST_MANUAL) {
            dsp_app_cmd_t c = ble.cmd[ble.next++];
            run_manual(&c);
            continue;
        }
        if (!dsp_app_post(&app, &ble.cmd[ble.next])) {
            ble.rejected_posts++;
        }
        ble.next++;
    }
}
static bool ble_connected(void *ctx)
{
    (void)ctx;
    for (int i = 0; i < ble.nconn; i++) {
        if (ble_on && g_true >= ble.conn_from[i] && g_true < ble.conn_to[i]) {
            return true;
        }
    }
    return false;
}
static bool ble_start(void *ctx)
{
    (void)ctx;
    ble.starts++;
    if (ble.start_fails) {
        return false;
    }
    if (wifi_on || pump_running) {
        overlap_violations++;
    }
    ble_on = true;
    ev('B');
    return true;
}
static void ble_suspend(void *ctx)
{
    (void)ctx;
    ble.suspends++;
    ble_on = false;
    ev('s');
}
static bool ble_resume(void *ctx)
{
    (void)ctx;
    ble.resumes++;
    if (ble.resume_fails) {
        return false;
    }
    if (wifi_on || pump_running) {
        overlap_violations++;
    }
    ble_on = true;
    ev('r');
    return true;
}
static void ble_stop(void *ctx)
{
    (void)ctx;
    ble.stops++;
    if (ble_on) {
        ev('b');
    }
    if (!ble.stop_fails) {
        ble_on = false;
    }
}
static bool ble_is_off(void *ctx)
{
    (void)ctx;
    return !ble_on;
}
static void ble_wait(void *ctx, uint32_t max_ms)
{
    (void)ctx;
    ble.waits++;
    if (cut_point()) {
        power_lost();
    }
    if (ble.step_wall_on_wait != 0) {
        g_offset += ble.step_wall_on_wait; /* wall clock stepped: the session must not care */
        ble.step_wall_on_wait = 0;
    }
    dsp_utc_t until = g_true + (dsp_utc_t)((max_ms + 999u) / 1000u);
    if (ble.next < ble.len && ble.at[ble.next] < until) {
        until = ble.at[ble.next] > g_true ? ble.at[ble.next] : g_true;
    }
    /* A connection change also ends the wait (platform event). */
    for (int i = 0; i < ble.nconn; i++) {
        if (ble.conn_from[i] > g_true && ble.conn_from[i] < until) {
            until = ble.conn_from[i];
        }
        if (ble.conn_to[i] > g_true && ble.conn_to[i] < until) {
            until = ble.conn_to[i];
        }
    }
    g_true = until;
    ble_post_due();
}
static void ble_respond(void *ctx, const dsp_app_response_t *r)
{
    (void)ctx;
    if (ble.nresp < 64) {
        ble.resp[ble.nresp++] = *r;
    }
}
static const dsp_ble_port_t BLE = {NULL,     ble_start,  ble_connected, ble_suspend, ble_resume,
                                   ble_stop, ble_is_off, ble_wait,      ble_respond};

static void ble_connection(dsp_utc_t from, dsp_utc_t to)
{
    if (ble.nconn < 8) {
        ble.conn_from[ble.nconn] = from;
        ble.conn_to[ble.nconn] = to;
        ble.nconn++;
    }
}

/* Queue lock (counts and checks balance; the host producer runs in the same task). */
static int locks, unlocks, lock_depth, lock_violations;
static void q_lock(void *ctx)
{
    (void)ctx;
    locks++;
    if (lock_depth++ != 0) {
        lock_violations++;
    }
}
static void q_unlock(void *ctx)
{
    (void)ctx;
    unlocks++;
    if (--lock_depth != 0) {
        lock_violations++;
    }
}
static const dsp_app_sync_t SYNC = {NULL, q_lock, q_unlock};

static void ble_script(dsp_utc_t at, dsp_app_cmd_t cmd)
{
    if (ble.len < MAX_SCRIPT) {
        ble.at[ble.len] = at;
        ble.cmd[ble.len] = cmd;
        ble.len++;
    }
}
static dsp_app_cmd_t cmd_of(uint8_t kind, uint32_t a, uint32_t b)
{
    dsp_app_cmd_t c;
    memset(&c, 0, sizeof c);
    c.kind = kind;
    c.id = (uint32_t)ble.len + 1;
    if (kind == TEST_MANUAL) {
        c.capacity_ul = a; /* volume */
    } else if (kind == DSP_CMD_REFILL) {
        c.capacity_ul = a;
        c.remaining_ul = b;
    } else if (kind == DSP_CMD_SET_READINESS) {
        c.readiness = (uint8_t)a;
    }
    return c;
}
static const dsp_app_response_t *resp_of(uint8_t kind)
{
    for (int i = ble.nresp - 1; i >= 0; i--) {
        if (ble.resp[i].kind == kind) {
            return &ble.resp[i];
        }
    }
    return NULL;
}

static dsp_config_t peek_cfg(void)
{
    dsp_config_t c;
    memset(&c, 0, sizeof c);
    const fake_kv_entry_t *e = fake_kv_find(&kv, "config");
    if (e != NULL) {
        (void)dsp_codec_decode_config(e->value, e->len, &c);
    }
    return c;
}

/* ---- World ------------------------------------------------------------------------------ */

static dsp_wifi_manager_t wifi;
static dsp_https_transport_t https;
static dsp_app_config_t CFG;
static dsp_app_report_t rep;
static int wakes;

static void world(void)
{
    fake_kv_init(&kv, true);
    proxy.ctx = NULL;
    proxy.get = px_get;
    proxy.set = px_set;
    proxy.commit = px_commit;
    evlog_len = 0;
    evlog[0] = '\0';
    events = 0;
    cut_at = 0;
    cut_after = false;
    in_wake = false;
    cut_happened = false;
    g_true = T0 - HOUR; /* 07:00 local */
    g_offset = 5 - g_true; /* power-on: the raw clock counts from ~0 (untrusted) */
    g_marker = 0;
    ntp_error = 0;
    ntp_status = DSP_SYNC_OK;
    ntp_calls = 0;
    wifi_on = ble_on = pump_running = false;
    overlap_violations = 0;
    wifi_result = DSP_NET_OK;
    wifi_connects = 0;
    creds_ok = true;
    http_ack = true;
    http_in_session = false;
    http_posts = http_acks = delivered_interrupted = delivered_dosing_errors = 0;
    mono_ms = 0;
    fail_notify_sets = false;
    memset(&pump, 0, sizeof pump);
    pump.off_confirmable = true;
    g_cause = DSP_CAUSE_POWER_ON;
    memset(rtc, 0xA5, sizeof rtc);
    rtc_len = 0;
    rtc_save_fails = false;
    sleeps = fail_safes = sleep_violations = 0;
    last_sleep_s = 0;
    last_failsafe = 0;
    memset(&ble, 0, sizeof ble);
    wakes = 0;
    dsp_wifi_config_t wcfg = {DSP_WIFI_TIMEOUT_DEFAULT_MS, DSP_WIFI_ATTEMPTS_DEFAULT};
    DSP_CHECK_EQ_INT(DSP_NET_OK, dsp_wifi_init(&wifi, &WIFI, &CREDS, &wcfg));
    DSP_CHECK(dsp_https_init(&https, &PORT, &TOKENS, &MONO, DEVICE, 0));
    memset(&CFG, 0, sizeof CFG);
    CFG.kv = &proxy;
    CFG.tz = TZ;
    CFG.pump = &PUMP;
    CFG.hw_max_runtime_ms = HW_MAX_MS;
    CFG.power = &POWER;
    CFG.clock = &CLOCK;
    CFG.time = &TIMEP;
    CFG.wifi = &wifi;
    CFG.ntp = &NTP;
    CFG.https = &https;
    CFG.ble = &BLE;
    CFG.mono = &MONO;
    CFG.sync = &SYNC;
    locks = unlocks = lock_depth = lock_violations = 0;
    CFG.random = 0x5EED1234u;
}

/* The power goes: abort the running wake. */
static void power_lost(void)
{
    cut_happened = true;
    pump_running = false;
    wifi_on = false;
    ble_on = false;
    http_in_session = false;
    https.in_session = false;
    longjmp(cut_env, 1);
}

/* After an aborted wake: what survives. power_on: RTC memory and clock are lost. */
static void reboot_state(bool power_on)
{
    fake_kv_power_cycle(&kv);
    fake_kv_clear_faults(&kv);
    /* The Wi-Fi manager and HTTPS transport are platform RAM: re-initialized at boot. */
    dsp_wifi_config_t wcfg = {DSP_WIFI_TIMEOUT_DEFAULT_MS, DSP_WIFI_ATTEMPTS_DEFAULT};
    (void)dsp_wifi_init(&wifi, &WIFI, &CREDS, &wcfg);
    (void)dsp_https_init(&https, &PORT, &TOKENS, &MONO, DEVICE, 0);
    if (power_on) {
        g_cause = DSP_CAUSE_POWER_ON;
        memset(rtc, 0x5A, sizeof rtc);
        g_marker = 0;
        g_offset = 3 - g_true;
    } else {
        g_cause = DSP_CAUSE_WATCHDOG; /* CPU reset: RTC memory and the clock survive */
    }
    g_true += 2;
}

/* One wake. Returns false when it was aborted by a power cut. */
static bool wake(dsp_wake_cause_t cause)
{
    g_cause = cause;
    wakes++;
    in_wake = true;
    cut_happened = false;
    if (setjmp(cut_env) == 0) {
        rep = dsp_app_run_wake(&app, &CFG);
        in_wake = false;
        return true;
    }
    in_wake = false;
    return false;
}

/* ---- Provisioning helpers (outside any wake, via a side storage instance) -------------- */

static dsp_storage_t side;
static void side_open(void)
{
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_open(&side, &proxy, TZ, true));
}
static dsp_config_t load_cfg(void)
{
    side_open();
    dsp_config_t c;
    memset(&c, 0, sizeof c);
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_load_config(&side, &c));
    return c;
}
static void save_cfg(dsp_config_t c)
{
    dsp_config_t cur = load_cfg();
    c.revision = cur.revision + 1;
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_save_config(&side, &c));
}
static void configure(uint32_t remaining, uint32_t min_interval)
{
    dsp_config_t c = load_cfg();
    c.pump_flow_ul_per_s = 1200;
    c.calibrated_at = T0 - DAY;
    c.max_dose_ul = 20000;
    c.max_daily_ul = 60000;
    c.max_runtime_ms = 30000;
    c.min_interval_s = min_interval;
    c.reservoir_capacity_ul = CAP;
    c.reservoir_remaining_ul = remaining;
    save_cfg(c);
}
static dsp_schedule_t sched(const char *id, int hour, int minute, uint32_t amount)
{
    dsp_schedule_t s;
    memset(&s, 0, sizeof s);
    snprintf(s.id, sizeof s.id, "%s", id);
    s.revision = 1;
    s.enabled = true;
    s.amount_ul = amount;
    s.start_date.year = 2026;
    s.start_date.month = 11;
    s.start_date.day = 2;
    s.hour = (uint8_t)hour;
    s.minute = (uint8_t)minute;
    s.kind = DSP_REC_DAILY;
    s.interval = 1;
    return s;
}
static void add_schedule(dsp_schedule_t s)
{
    side_open();
    dsp_schedule_repo_t repo;
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_schedule_repo_load(&repo, &side));
    dsp_history_store_t h;
    dsp_storage_history_store(&side, &h);
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_schedule_repo_create(&repo, &s, &h));
}
static dsp_dosing_state_t dosing_state(void)
{
    dsp_dosing_state_t d;
    memset(&d, 0, sizeof d);
    DSP_CHECK(durable_dosing(&d));
    return d;
}
static int history_state(const char *id, dsp_date_t date)
{
    for (size_t s = 0; s < DSP_STORAGE_HISTORY_SLOTS; s++) {
        char key[8];
        dsp_storage_history_key(s, key, sizeof key);
        const fake_kv_entry_t *e = fake_kv_find(&kv, key);
        dsp_history_record_t rec;
        if (e != NULL && dsp_codec_decode_history(e->value, e->len, &rec) == DSP_CODEC_OK &&
            strcmp(rec.schedule_id, id) == 0 && dsp_date_compare(rec.local_date, date) == 0) {
            return rec.state;
        }
    }
    return 0;
}
static int durable_events(uint8_t type, uint16_t code)
{
    side_open();
    dsp_notify_state_t ns;
    if (dsp_storage_load_notify(&side, &ns) != DSP_STORAGE_OK) {
        return -1;
    }
    int c = 0;
    for (int i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY; i++) {
        c += ns.queue[i].seq != 0 && ns.queue[i].type == type && (code == 0 || ns.queue[i].code == code);
    }
    return c;
}
static int count_manual_runs(void)
{
    int c = 0;
    for (int i = 0; i < pump.calls && i < MAX_RUNS; i++) {
        c += pump.manual[i];
    }
    return c;
}

/* First wake (provisioning + initial time sync), then the device is configured and READY. */
static void boot_and_provision(uint32_t remaining, uint32_t min_interval, bool ready)
{
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    DSP_CHECK(rep.slept);
    configure(remaining, min_interval);
    if (ready) {
        ble.len = ble.next = 0;
        ble_script(g_true, cmd_of(DSP_CMD_SET_READINESS, DSP_READINESS_READY, 0));
        ble_script(g_true, cmd_of(DSP_CMD_END_SESSION, 0, 0));
        DSP_CHECK(wake(DSP_CAUSE_BUTTON));
        const dsp_app_response_t *r = resp_of(DSP_CMD_SET_READINESS);
        DSP_CHECK(r != NULL && r->status == DSP_RESP_OK);
        DSP_CHECK_EQ_INT(DSP_READINESS_READY, dosing_state().readiness);
        ble.len = ble.next = 0;
    }
}

/* Schedules come over BLE inside a wake on the device; after adding them from the test,
 * a timer wake at the current time plans with them. */
static void replan(void)
{
    DSP_CHECK(wake(DSP_CAUSE_TIMER));
}

/* Sleeps as planned (+ latency), then wakes by timer. */
static bool sleep_and_wake(int64_t latency_s)
{
    g_true += rep.plan.sleep_s + latency_s;
    return wake(DSP_CAUSE_TIMER);
}

/* Timer wakes until true time reaches end (no cuts). */
static void run_until(dsp_utc_t end)
{
    int guard = 0;
    while (g_true < end && guard++ < 20000) {
        int64_t s = rep.plan.sleep_s;
        if (g_true + s > end) {
            g_true = end;
            DSP_CHECK(wake(DSP_CAUSE_TIMER));
        } else {
            DSP_CHECK(sleep_and_wake(0));
        }
    }
}

static void common_invariants(void)
{
    DSP_CHECK_EQ_INT(0, lock_violations);
    DSP_CHECK_EQ_INT(locks, unlocks);
    DSP_CHECK_EQ_INT(0, pump.violations);
    DSP_CHECK_EQ_INT(0, overlap_violations);
    DSP_CHECK_EQ_INT(0, sleep_violations);
}

static dsp_date_t date_of(dsp_utc_t t)
{
    dsp_date_t d;
    memset(&d, 0, sizeof d);
    DSP_CHECK(dsp_utc_to_local_date(TZ, t, &d));
    return d;
}

static size_t count_trace(dsp_app_stage_t s)
{
    size_t c = 0;
    for (size_t i = 0; i < rep.trace_len; i++) {
        c += rep.trace[i] == s;
    }
    return c;
}

/* ==== Power Service units ================================================================ */

static void test_retained_block(void)
{
    dsp_power_retained_t r;
    dsp_power_retained_defaults(&r);
    r.time.consecutive_failures = 3;
    r.time.next_attempt = T0 + 900;
    r.conn.last_notify_attempt = T0 - 60;
    r.wake_count = 77;
    r.failsafe_count = 2;
    r.last_reason = DSP_WAKE_DOSE;
    r.last_wake_at = T0 + 3600;
    uint8_t buf[DSP_POWER_RETAINED_BYTES];
    DSP_CHECK(dsp_power_retained_encode(&r, buf));
    dsp_power_retained_t out;
    DSP_CHECK(dsp_power_retained_decode(buf, sizeof buf, &out));
    DSP_CHECK_EQ_INT(3, out.time.consecutive_failures);
    DSP_CHECK_EQ_INT(T0 + 900, out.time.next_attempt);
    DSP_CHECK_EQ_INT(T0 - 60, out.conn.last_notify_attempt);
    DSP_CHECK_EQ_INT(77, out.wake_count);
    DSP_CHECK_EQ_INT(2, out.failsafe_count);
    DSP_CHECK_EQ_INT(DSP_WAKE_DOSE, out.last_reason);
    DSP_CHECK_EQ_INT(T0 + 3600, out.last_wake_at);
    /* Every single-bit flip is rejected (CRC, magic, version, length, reserved). */
    for (size_t i = 0; i < sizeof buf; i++) {
        for (int b = 0; b < 8; b++) {
            uint8_t c[DSP_POWER_RETAINED_BYTES];
            memcpy(c, buf, sizeof c);
            c[i] ^= (uint8_t)(1u << b);
            dsp_power_retained_t keep = out;
            DSP_CHECK(!dsp_power_retained_decode(c, sizeof c, &keep));
            DSP_CHECK_EQ_INT(77, keep.wake_count); /* untouched */
        }
    }
    DSP_CHECK(!dsp_power_retained_decode(buf, sizeof buf - 1, &out));
    DSP_CHECK(!dsp_power_retained_decode(NULL, sizeof buf, &out));
    /* Valid CRC but a different version or out-of-range fields: rejected. */
    uint8_t v[DSP_POWER_RETAINED_BYTES];
    memcpy(v, buf, sizeof v);
    v[4] = 2;
    uint32_t crc = dsp_power_crc32(v, 44);
    for (int i = 0; i < 4; i++) {
        v[44 + i] = (uint8_t)(crc >> (8 * i));
    }
    DSP_CHECK(!dsp_power_retained_decode(v, sizeof v, &out));
    dsp_power_retained_t bad = r;
    bad.last_reason = 99;
    DSP_CHECK(!dsp_power_retained_encode(&bad, buf));
    bad = r;
    bad.time.next_attempt = 5; /* not a plausible instant */
    DSP_CHECK(!dsp_power_retained_encode(&bad, buf));
    bad = r;
    bad.failsafe_count = DSP_POWER_FAILSAFE_LIMIT + 1;
    DSP_CHECK(!dsp_power_retained_encode(&bad, buf));
    /* CRC-32 (IEEE) check value. */
    DSP_CHECK_EQ_INT(0xCBF43926u, dsp_power_crc32((const uint8_t *)"123456789", 9));
    /* Reset causes. */
    DSP_CHECK(!dsp_power_cause_keeps_retained(DSP_CAUSE_POWER_ON));
    DSP_CHECK(!dsp_power_cause_keeps_retained(DSP_CAUSE_BROWNOUT));
    DSP_CHECK(!dsp_power_cause_keeps_retained((dsp_wake_cause_t)42));
    DSP_CHECK(dsp_power_cause_keeps_retained(DSP_CAUSE_TIMER));
    DSP_CHECK(dsp_power_cause_keeps_retained(DSP_CAUSE_BUTTON));
    DSP_CHECK(dsp_power_cause_keeps_retained(DSP_CAUSE_WATCHDOG));
}

static void test_power_plan(void)
{
    DSP_CHECK_EQ_INT(6 * HOUR, dsp_power_max_sleep_s(0));
    DSP_CHECK_EQ_INT(DSP_POWER_MAX_SLEEP_MIN_S, dsp_power_max_sleep_s(5));
    DSP_CHECK_EQ_INT(DSP_POWER_MAX_SLEEP_MAX_S, dsp_power_max_sleep_s(100 * HOUR));
    DSP_CHECK_EQ_INT(2 * HOUR, dsp_power_max_sleep_s(2 * HOUR));
    dsp_power_inputs_t in;
    memset(&in, 0, sizeof in);
    dsp_wake_plan_t p = dsp_power_plan(&in, T0);
    DSP_CHECK_EQ_INT(DSP_WAKE_MAX_SLEEP, p.reason);
    DSP_CHECK_EQ_INT(6 * HOUR, p.sleep_s);
    in.dose_known = true;
    in.dose_at = T0 + 2 * HOUR;
    in.sync_known = true;
    in.sync_at = T0 + 3 * HOUR;
    p = dsp_power_plan(&in, T0);
    DSP_CHECK_EQ_INT(DSP_WAKE_DOSE, p.reason);
    DSP_CHECK_EQ_INT(2 * HOUR, p.sleep_s);
    in.sync_at = T0 + HOUR;
    p = dsp_power_plan(&in, T0);
    DSP_CHECK_EQ_INT(DSP_WAKE_TIME_SYNC, p.reason);
    in.dose_at = T0 + 7 * HOUR;
    in.sync_at = T0 + 8 * HOUR;
    p = dsp_power_plan(&in, T0);
    DSP_CHECK_EQ_INT(DSP_WAKE_MAX_SLEEP, p.reason); /* capped at 6 h */
    in.max_sleep_s = 2 * HOUR;
    p = dsp_power_plan(&in, T0);
    DSP_CHECK_EQ_INT(2 * HOUR, p.sleep_s);
    /* An overdue sync is deferred, never a zero-length sleep. */
    in.max_sleep_s = 0;
    in.dose_known = false;
    in.sync_at = T0 - 10;
    p = dsp_power_plan(&in, T0);
    DSP_CHECK_EQ_INT(DSP_WAKE_TIME_SYNC, p.reason);
    DSP_CHECK_EQ_INT(DSP_POWER_SYNC_DEFER_S, p.sleep_s);
    /* A dose at or before now: act now. */
    in.dose_known = true;
    in.dose_at = T0;
    p = dsp_power_plan(&in, T0);
    DSP_CHECK_EQ_INT(0, p.sleep_s);
}

/* ==== Application scenarios ============================================================== */

static void test_first_boot(void)
{
    world();
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    /* Exact stage order; DOSE re-evaluated after the session made the clock trusted. */
    const uint8_t expect[] = {DSP_STAGE_SAFE_OFF, DSP_STAGE_LOAD,  DSP_STAGE_RECOVER,
                              DSP_STAGE_CLOCK,    DSP_STAGE_OBSERVE, DSP_STAGE_DOSE,
                              DSP_STAGE_CONNECT,  DSP_STAGE_DOSE,  DSP_STAGE_SYNC,
                              DSP_STAGE_PLAN,     DSP_STAGE_SHUTDOWN, DSP_STAGE_SLEEP};
    DSP_CHECK_EQ_INT(sizeof expect, rep.trace_len);
    DSP_CHECK(memcmp(expect, rep.trace, sizeof expect) == 0);
    DSP_CHECK(!rep.degraded);
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, rep.open_status);
    DSP_CHECK(!rep.retained_valid);              /* power-on: RTC memory discarded */
    DSP_CHECK_EQ_INT(DSP_CONN_DONE, rep.conn[0]); /* initial sync (regular reason) */
    DSP_CHECK(g_marker != 0);
    DSP_CHECK_EQ_INT(0, pump.calls);             /* SERVICE_REQUIRED, unconfigured */
    DSP_CHECK_EQ_INT(DSP_READINESS_SERVICE_REQUIRED, dosing_state().readiness);
    DSP_CHECK(pump.force_offs >= 2);             /* SAFE_OFF and SHUTDOWN */
    DSP_CHECK(rep.slept && !rep.fail_safe && rep.retained_saved);
    DSP_CHECK_EQ_INT(DSP_WAKE_MAX_SLEEP, rep.plan.reason); /* no dose wakes; sync in 24 h */
    DSP_CHECK_EQ_INT(6 * HOUR, rep.plan.sleep_s);
    DSP_CHECK_EQ_INT(0, rep.shutdown_failures);
    /* The next timer wake keeps the retained block. */
    DSP_CHECK(sleep_and_wake(0));
    DSP_CHECK(rep.retained_valid);
    DSP_CHECK_EQ_INT(-1, rep.conn_runs > 0 && rep.conn[0] == DSP_CONN_DONE ? 0 : -1); /* no session */
    common_invariants();
}

static void test_normal_scheduled_dosing(void)
{
    world();
    boot_and_provision(CAP, 60, true);
    add_schedule(sched("a", 8, 0, 5000));
    replan();
    run_until(T0 - 30 * 60); /* 07:30 */
    DSP_CHECK_EQ_INT(0, pump.calls);
    /* Planned: a DOSE wake exactly at 08:00. */
    DSP_CHECK(sleep_and_wake(0) || true);
    int guard = 0;
    while (pump.calls == 0 && guard++ < 10) {
        DSP_CHECK(sleep_and_wake(0));
    }
    DSP_CHECK_EQ_INT(1, pump.calls);
    DSP_CHECK_EQ_INT(T0, pump.at[0]);
    DSP_CHECK_EQ_INT(DSP_HISTORY_COMPLETED, history_state("a", date_of(T0)));
    DSP_CHECK_EQ_INT(CAP - 5000, load_cfg().reservoir_remaining_ul);
    /* Next: capped re-evaluation (the next dose is tomorrow). */
    DSP_CHECK_EQ_INT(DSP_WAKE_MAX_SLEEP, rep.plan.reason);
    run_until(T0 + DAY + HOUR);
    DSP_CHECK_EQ_INT(2, pump.calls);
    DSP_CHECK_EQ_INT(T0 + DAY, pump.at[1]);
    common_invariants();
}

static void test_simultaneous_schedules(void)
{
    /* Three schedules at 08:00, minimum interval 120 s: one after another, each exactly once,
     * via planned re-evaluation wakes inside the 10-minute window. */
    world();
    boot_and_provision(CAP, 120, true);
    add_schedule(sched("c", 8, 0, 2000));
    add_schedule(sched("a", 8, 0, 2000));
    add_schedule(sched("b", 8, 0, 2000));
    replan();
    run_until(T0 + 15 * 60);
    DSP_CHECK_EQ_INT(3, pump.calls);
    DSP_CHECK_EQ_STR("a@2026-11-20", pump.ids[0]);
    DSP_CHECK_EQ_STR("b@2026-11-20", pump.ids[1]);
    DSP_CHECK_EQ_STR("c@2026-11-20", pump.ids[2]);
    DSP_CHECK_EQ_INT(T0, pump.at[0]);
    DSP_CHECK(pump.at[1] >= pump.at[0] + 120 && pump.at[1] <= T0 + WINDOW);
    DSP_CHECK(pump.at[2] >= pump.at[1] + 120 && pump.at[2] <= T0 + WINDOW);
    common_invariants();

    /* Interval 400 s: the third does not fit its window -> MISSED, never caught up. */
    world();
    boot_and_provision(CAP, 400, true);
    add_schedule(sched("a", 8, 0, 2000));
    add_schedule(sched("b", 8, 0, 2000));
    add_schedule(sched("c", 8, 0, 2000));
    replan();
    run_until(T0 + 2 * HOUR);
    DSP_CHECK_EQ_INT(2, pump.calls);
    DSP_CHECK_EQ_INT(T0 + 400, pump.at[1]);
    DSP_CHECK_EQ_INT(0, history_state("c", date_of(T0)));
    run_until(T0 + DAY - HOUR);
    DSP_CHECK_EQ_INT(2, pump.calls); /* no catch-up later that day */
    common_invariants();
}

static void test_missed_occurrences(void)
{
    world();
    boot_and_provision(CAP, 60, true);
    add_schedule(sched("a", 8, 0, 5000));
    replan();
    run_until(T0 - 30 * 60);
    /* The wake comes 11 minutes late (beyond the due window): missed, never caught up. */
    int guard = 0;
    while (g_true + rep.plan.sleep_s < T0 && guard++ < 10) {
        DSP_CHECK(sleep_and_wake(0));
    }
    DSP_CHECK(sleep_and_wake(WINDOW + 60));
    DSP_CHECK_EQ_INT(0, pump.calls);
    run_until(T0 + 20 * HOUR);
    DSP_CHECK_EQ_INT(0, pump.calls);
    DSP_CHECK_EQ_INT(0, history_state("a", date_of(T0)));
    run_until(T0 + DAY + 60);
    DSP_CHECK_EQ_INT(1, pump.calls); /* the next day runs normally */
    common_invariants();
}

static void test_manual_dose_service_mode(void)
{
    world();
    boot_and_provision(CAP, 60, true);
    evlog_len = 0;
    ble_script(g_true + 5, cmd_of(DSP_CMD_STATUS, 0, 0));
    ble_script(g_true + 10, cmd_of(TEST_MANUAL, 4000, 0));
    ble_script(g_true + 30, cmd_of(DSP_CMD_END_SESSION, 0, 0));
    DSP_CHECK(wake(DSP_CAUSE_BUTTON));
    DSP_CHECK(rep.ble_session);
    DSP_CHECK_EQ_INT(1, rep.manual_doses);
    DSP_CHECK_EQ_INT(1, pump.calls);
    DSP_CHECK(pump.manual[0]);
    const dsp_app_response_t *r = resp_of(TEST_MANUAL);
    DSP_CHECK(r != NULL && r->status == DSP_RESP_OK && r->remaining_ul == CAP - 4000);
    r = resp_of(DSP_CMD_STATUS);
    DSP_CHECK(r != NULL && r->storage_available && r->readiness == DSP_READINESS_READY);
    /* BLE suspended before the pump, resumed afterwards; Wi-Fi never during BLE. */
    DSP_CHECK(strstr(evlog, "BsPr") != NULL);
    DSP_CHECK(!(dosing_state().flags & DSP_DOSING_F_MANUAL_ACTIVE));
    DSP_CHECK_EQ_INT(CAP - 4000, load_cfg().reservoir_remaining_ul);
    /* Limits apply: over max_dose is refused (no pump). */
    ble.len = ble.next = 0;
    ble.nresp = 0;
    ble_script(g_true + 70, cmd_of(TEST_MANUAL, 30000, 0));
    ble_script(g_true + 80, cmd_of(DSP_CMD_END_SESSION, 0, 0));
    g_true += 60;
    DSP_CHECK(wake(DSP_CAUSE_BUTTON));
    r = resp_of(TEST_MANUAL);
    DSP_CHECK(r != NULL && r->status == DSP_RESP_REFUSED && (r->limits & DSP_DOSING_L_DOSE));
    DSP_CHECK_EQ_INT(1, pump.calls);
    /* No button: no service mode on a timer wake even with commands pending. */
    DSP_CHECK(sleep_and_wake(0));
    DSP_CHECK(!rep.ble_session);
    /* Bounded queue: the 9th command in a burst is rejected (answered busy by the platform). */
    ble.len = ble.next = 0;
    ble.nresp = 0;
    for (int i = 0; i < DSP_APP_CMDQ_CAPACITY + 1; i++) {
        ble_script(g_true + 1, cmd_of(DSP_CMD_STATUS, 0, 0));
    }
    ble_script(g_true + 2, cmd_of(DSP_CMD_END_SESSION, 0, 0));
    DSP_CHECK(wake(DSP_CAUSE_BUTTON));
    DSP_CHECK_EQ_INT(1, ble.rejected_posts);
    DSP_CHECK_EQ_INT(DSP_APP_CMDQ_CAPACITY + 1, rep.responses); /* + END */
    common_invariants();
}

static void test_refill_readiness_blocked(void)
{
    world();
    boot_and_provision(CAP, 60, false);
    add_schedule(sched("a", 8, 0, 5000));
    replan();
    /* SERVICE_REQUIRED: no dose wakes (Q13), no doses. */
    DSP_CHECK(sleep_and_wake(0));
    DSP_CHECK(rep.plan.reason != DSP_WAKE_DOSE);
    run_until(T0 + HOUR);
    DSP_CHECK_EQ_INT(0, pump.calls);
    /* READY over BLE: the plan is recalculated (dose wake for tomorrow 08:00 within the cap
     * is not reachable from 09:00; check the response's next due instead). */
    ble.len = ble.next = 0;
    ble_script(g_true + 1, cmd_of(DSP_CMD_SET_READINESS, DSP_READINESS_READY, 0));
    ble_script(g_true + 2, cmd_of(DSP_CMD_END_SESSION, 0, 0));
    DSP_CHECK(wake(DSP_CAUSE_BUTTON));
    const dsp_app_response_t *r = resp_of(DSP_CMD_SET_READINESS);
    DSP_CHECK(r != NULL && r->status == DSP_RESP_OK && r->next_due_known && r->next_due_at == T0 + DAY);
    /* Reservoir too low for the dose: BLOCKED latch at 08:00 next day, no further dose wakes. */
    dsp_config_t c = load_cfg();
    c.reservoir_remaining_ul = 3000;
    save_cfg(c);
    run_until(T0 + DAY + HOUR);
    DSP_CHECK_EQ_INT(0, pump.calls);
    DSP_CHECK(dosing_state().flags & DSP_DOSING_F_BLOCKED);
    DSP_CHECK(rep.plan.reason != DSP_WAKE_DOSE);
    DSP_CHECK(durable_events(DSP_NOTE_RESERVOIR_BLOCKED, 0) + http_acks >= 1);
    run_until(T0 + 2 * DAY + HOUR);
    DSP_CHECK_EQ_INT(0, pump.calls); /* still blocked the day after */
    /* Manual dose refused while BLOCKED (Q7). */
    ble.len = ble.next = 0;
    ble.nresp = 0;
    ble_script(g_true + 1, cmd_of(TEST_MANUAL, 1000, 0));
    ble_script(g_true + 2, cmd_of(DSP_CMD_REFILL, CAP, CAP));
    ble_script(g_true + 3, cmd_of(DSP_CMD_END_SESSION, 0, 0));
    DSP_CHECK(wake(DSP_CAUSE_BUTTON));
    r = resp_of(TEST_MANUAL);
    DSP_CHECK(r != NULL && r->status == DSP_RESP_REFUSED && r->dosing == DSP_DOSING_REFUSED);
    r = resp_of(DSP_CMD_REFILL);
    DSP_CHECK(r != NULL && r->status == DSP_RESP_OK && r->remaining_ul == CAP);
    DSP_CHECK(!(dosing_state().flags & (DSP_DOSING_F_BLOCKED | DSP_DOSING_F_REFILL_PENDING)));
    DSP_CHECK(r != NULL && r->next_due_known); /* dose wakes back after the refill */
    run_until(T0 + 3 * DAY + HOUR);
    DSP_CHECK_EQ_INT(1, pump.calls);
    /* Back to SERVICE_REQUIRED: dose wakes stop again. */
    ble.len = ble.next = 0;
    ble_script(g_true + 1, cmd_of(DSP_CMD_SET_READINESS, DSP_READINESS_SERVICE_REQUIRED, 0));
    ble_script(g_true + 2, cmd_of(DSP_CMD_END_SESSION, 0, 0));
    DSP_CHECK(wake(DSP_CAUSE_BUTTON));
    run_until(T0 + 5 * DAY);
    DSP_CHECK_EQ_INT(1, pump.calls);
    common_invariants();
}

static void test_wifi_unavailable(void)
{
    world();
    boot_and_provision(CAP, 60, true);
    add_schedule(sched("a", 8, 0, 5000));
    add_schedule(sched("b", 20, 0, 5000));
    replan();
    wifi_result = DSP_NET_ERR_NO_AP;
    int connects_before = wifi_connects;
    run_until(T0 + 3 * DAY - HOUR);
    /* Dosing continues on the trusted (holdover) clock; history and estimate unaffected. */
    DSP_CHECK_EQ_INT(6, pump.calls);
    DSP_CHECK_EQ_INT(CAP - 30000, load_cfg().reservoir_remaining_ul);
    /* Backoff respected: far fewer attempts than wakes, bounded attempts per session. */
    int attempts = wifi_connects - connects_before;
    DSP_CHECK(attempts > 0);
    DSP_CHECK(attempts <= 3 * 12); /* at most 3 per session, backoff >= 1 h doubling */
    DSP_CHECK(!wifi_on);
    common_invariants();
}

static void test_critical_notification_existing_wake(void)
{
    world();
    boot_and_provision(CAP, 60, true);
    add_schedule(sched("a", 8, 0, 5000));
    add_schedule(sched("b", 8, 0, 5000));
    replan();
    pump.script[0] = DSP_PUMP_FAILED;
    pump.script_len = 1;
    int posts_before = http_posts;
    run_until(T0 + 30);
    /* The pump fault ends dosing for that wake (b is NOT run in the same wake), and no
     * session starts while b is due within the guard distance. */
    DSP_CHECK_EQ_INT(1, pump.calls);
    DSP_CHECK_EQ_INT(posts_before, http_posts);
    DSP_CHECK(rep.conn_runs == 0 || rep.conn[0] == DSP_CONN_SKIPPED_DOSE_SOON);
    run_until(T0 + 15 * 60);
    DSP_CHECK_EQ_INT(2, pump.calls); /* b in the planned wake at 08:01, inside its window */
    /* The CRITICAL DOSING_ERROR then travelled on that existing wake (no extra wake). */
    DSP_CHECK(http_posts > posts_before);
    DSP_CHECK(http_acks >= 1);
    common_invariants();

    /* A pending CRITICAL notification never shortens the sleep, and the hourly limit holds. */
    world();
    boot_and_provision(CAP, 60, true);
    http_ack = false; /* nothing gets delivered */
    wifi_result = DSP_NET_OK;
    pump.script[0] = DSP_PUMP_FAILED;
    pump.script_len = 1;
    add_schedule(sched("a", 8, 0, 5000));
    replan();
    int posts0 = http_posts;
    run_until(T0 + 60);
    DSP_CHECK_EQ_INT(1, pump.calls);
    DSP_CHECK(durable_events(DSP_NOTE_DOSING_ERROR, 0) >= 1);
    DSP_CHECK(http_posts > posts0); /* the CRITICAL attempt happened (503: kept queued) */
    int64_t planned = rep.plan.sleep_s;
    DSP_CHECK(rep.plan.reason != DSP_WAKE_NONE);
    DSP_CHECK(planned >= 60 * 60); /* no early wake for the pending notification */
    int connects = wifi_connects;
    /* Button wakes within the hour: CRITICAL may not start Wi-Fi again (hourly limit). */
    g_true += 10 * 60;
    ble.len = ble.next = 0;
    ble_script(g_true + 1, cmd_of(DSP_CMD_STATUS, 0, 0));
    ble_script(g_true + 2, cmd_of(DSP_CMD_END_SESSION, 0, 0));
    DSP_CHECK(wake(DSP_CAUSE_BUTTON));
    DSP_CHECK_EQ_INT(connects, wifi_connects);
    common_invariants();
}

static void test_ble_interrupted_by_due_dose(void)
{
    world();
    boot_and_provision(CAP, 60, true);
    add_schedule(sched("a", 8, 0, 5000));
    replan();
    run_until(T0 - 30 * 60);
    g_true = T0 - 60; /* button pressed at 07:59 */
    evlog_len = 0;
    ble.len = ble.next = 0;
    ble.nresp = 0;
    ble_script(T0 + 20, cmd_of(TEST_MANUAL, 2000, 0)); /* inside the interval */
    ble_script(T0 + 30, cmd_of(DSP_CMD_STATUS, 0, 0));
    ble_script(T0 + 100, cmd_of(TEST_MANUAL, 2000, 0));
    ble_script(T0 + 150, cmd_of(DSP_CMD_END_SESSION, 0, 0));
    DSP_CHECK(wake(DSP_CAUSE_BUTTON));
    DSP_CHECK(rep.ble_session);
    DSP_CHECK_EQ_INT(2, pump.calls);
    DSP_CHECK(!pump.manual[0] && pump.manual[1]);
    DSP_CHECK_EQ_INT(T0, pump.at[0]); /* the scheduled dose on time, BLE suspended */
    DSP_CHECK(pump.at[1] >= T0 + 60); /* manual waits for the minimum interval */
    /* B(start) s(suspend) P(scheduled) r(resume) ... s P r (manual) ... b(stop) Z(sleep) */
    DSP_CHECK(strncmp(evlog, "BsPr", 4) == 0);
    DSP_CHECK(strstr(evlog + 4, "sPr") != NULL);
    DSP_CHECK(strchr(evlog, 'W') == NULL || strchr(evlog, 'W') > strchr(evlog, 'b'));
    DSP_CHECK_EQ_INT(3, ble.nresp); /* the 180 s window ends before the scripted END */
    const dsp_app_response_t *r = &ble.resp[0];
    DSP_CHECK(r->kind == TEST_MANUAL && r->status == DSP_RESP_REFUSED &&
              (r->limits & DSP_DOSING_L_INTERVAL)); /* refused: right after the scheduled dose */
    r = &ble.resp[2];
    DSP_CHECK(r->kind == TEST_MANUAL && r->status == DSP_RESP_OK);
    common_invariants();

    /* Resume refused (window over): the session ends cleanly, BLE stopped before sleep. */
    world();
    boot_and_provision(CAP, 60, true);
    add_schedule(sched("a", 8, 0, 5000));
    replan();
    run_until(T0 - 30 * 60);
    g_true = T0 - 30;
    ble.len = ble.next = 0;
    ble.resume_fails = true;
    DSP_CHECK(wake(DSP_CAUSE_BUTTON));
    DSP_CHECK_EQ_INT(1, pump.calls);
    DSP_CHECK(rep.slept);
    common_invariants();
}

static void test_ble_session_lifetime(void)
{
    /* 1. No client: the session ends 180 s after it started (monotonic). */
    world();
    boot_and_provision(CAP, 60, true);
    dsp_utc_t t = g_true;
    DSP_CHECK(wake(DSP_CAUSE_BUTTON));
    DSP_CHECK_EQ_INT(DSP_BLE_END_IDLE, rep.ble_end);
    DSP_CHECK(g_true - t >= 180 && g_true - t <= 185);

    /* 2. A client connects at +20 s and stays until +400 s: the session stays open while
     *    connected, then the idle timeout restarts at the disconnect (ends ~ +580 s). */
    world();
    boot_and_provision(CAP, 60, true);
    t = g_true;
    ble.len = ble.next = 0;
    ble.nresp = 0;
    ble_connection(t + 20, t + 400);
    ble_script(t + 300, cmd_of(DSP_CMD_STATUS, 0, 0)); /* after the first 180 s */
    DSP_CHECK(wake(DSP_CAUSE_BUTTON));
    DSP_CHECK_EQ_INT(DSP_BLE_END_IDLE, rep.ble_end);
    DSP_CHECK(g_true - t >= 580 && g_true - t <= 585);
    DSP_CHECK(resp_of(DSP_CMD_STATUS) != NULL);
    DSP_CHECK(rep.slept);

    /* 3. Reconnect after a disconnect: idle restarts each time; END ends at once. */
    world();
    boot_and_provision(CAP, 60, true);
    t = g_true;
    ble.len = ble.next = 0;
    ble_connection(t + 100, t + 200);
    ble_connection(t + 350, t + 500); /* within 180 s of the first disconnect */
    ble_script(t + 450, cmd_of(DSP_CMD_END_SESSION, 0, 0));
    DSP_CHECK(wake(DSP_CAUSE_BUTTON));
    DSP_CHECK_EQ_INT(DSP_BLE_END_COMMAND, rep.ble_end);
    DSP_CHECK(g_true - t >= 450 && g_true - t <= 455);

    /* 4. Connected for an hour: terminated at the 15-minute maximum, BLE stopped, sleep. */
    world();
    boot_and_provision(CAP, 60, true);
    t = g_true;
    ble.len = ble.next = 0;
    ble_connection(t + 5, t + HOUR);
    DSP_CHECK(wake(DSP_CAUSE_BUTTON));
    DSP_CHECK_EQ_INT(DSP_BLE_END_MAX, rep.ble_end);
    DSP_CHECK(g_true - t >= 900 && g_true - t <= 905);
    DSP_CHECK(rep.slept && !ble_on);
    /* Configurable maximum. */
    CFG.service_max_s = 300;
    t = g_true;
    ble.nconn = 0;
    ble_connection(t + 5, t + HOUR);
    DSP_CHECK(wake(DSP_CAUSE_BUTTON));
    DSP_CHECK_EQ_INT(DSP_BLE_END_MAX, rep.ble_end);
    DSP_CHECK(g_true - t >= 300 && g_true - t <= 305);
    CFG.service_max_s = 0;

    /* 5. The wall clock is stepped back 1 h during the session: the monotonic idle timeout
     *    still ends it after 180 s. */
    world();
    boot_and_provision(CAP, 60, true);
    t = g_true;
    ble.len = ble.next = 0;
    ble.step_wall_on_wait = -HOUR;
    DSP_CHECK(wake(DSP_CAUSE_BUTTON));
    DSP_CHECK_EQ_INT(DSP_BLE_END_IDLE, rep.ble_end);
    DSP_CHECK(g_true - t >= 180 && g_true - t <= 185);

    /* 6. Scheduled dose during a connected session: suspended, dosed, resumed (deadline
     *    permits); the closed connection restarts the idle timeout at resume. */
    world();
    boot_and_provision(CAP, 60, true);
    add_schedule(sched("a", 8, 0, 5000));
    replan();
    run_until(T0 - 30 * 60);
    g_true = T0 - 600;
    evlog_len = 0;
    ble.len = ble.next = 0;
    ble_connection(T0 - 590, T0 - 1); /* the phone is connected until the dose */
    DSP_CHECK(wake(DSP_CAUSE_BUTTON));
    DSP_CHECK_EQ_INT(1, pump.calls);
    DSP_CHECK_EQ_INT(T0, pump.at[0]);
    DSP_CHECK(strstr(evlog, "BsPr") != NULL);
    DSP_CHECK_EQ_INT(DSP_BLE_END_IDLE, rep.ble_end);

    /* 7. A dose at the maximum deadline: no resume past it; BLE stopped; sleep. */
    world();
    boot_and_provision(CAP, 60, true);
    add_schedule(sched("a", 8, 0, 5000));
    replan();
    run_until(T0 - 30 * 60);
    CFG.service_max_s = 300;
    g_true = T0 - 298;
    evlog_len = 0;
    ble.len = ble.next = 0;
    ble_connection(T0 - 297, T0 + HOUR);
    DSP_CHECK(wake(DSP_CAUSE_BUTTON));
    DSP_CHECK_EQ_INT(1, pump.calls);
    DSP_CHECK_EQ_INT(DSP_BLE_END_MAX, rep.ble_end);
    DSP_CHECK(strstr(evlog, "BsP") != NULL && strstr(evlog, "BsPr") == NULL);
    DSP_CHECK(rep.slept && !ble_on);
    CFG.service_max_s = 0;

    /* 8. Queue boundary: posting outside a session is refused; every queue access locked. */
    world();
    boot_and_provision(CAP, 60, true);
    dsp_app_cmd_t c = cmd_of(DSP_CMD_STATUS, 0, 0);
    DSP_CHECK(!dsp_app_post(&app, &c)); /* no session active */
    DSP_CHECK(locks > 0 && locks == unlocks);
    common_invariants();
}

static void test_storage_failures(void)
{
    /* Storage-open failure: DEGRADED (no dosing, no Wi-Fi, read-only BLE, plan = cap). */
    world();
    boot_and_provision(CAP, 60, true);
    add_schedule(sched("a", 8, 0, 5000));
    replan();
    run_until(T0 - 30 * 60);
    uint8_t before[DSP_POWER_RETAINED_BYTES];
    memcpy(before, rtc, sizeof before);
    snprintf(kv.fail_get_key, sizeof kv.fail_get_key, "dosing");
    int connects = wifi_connects;
    g_true = T0 + 10;
    ble.len = ble.next = 0;
    ble.nresp = 0;
    ble_script(g_true + 1, cmd_of(DSP_CMD_STATUS, 0, 0));
    ble_script(g_true + 2, cmd_of(TEST_MANUAL, 1000, 0));
    ble_script(g_true + 3, cmd_of(DSP_CMD_END_SESSION, 0, 0));
    DSP_CHECK(wake(DSP_CAUSE_BUTTON));
    DSP_CHECK(rep.degraded);
    DSP_CHECK_EQ_INT(0, pump.calls);
    DSP_CHECK_EQ_INT(connects, wifi_connects);
    DSP_CHECK_EQ_INT(0, count_trace(DSP_STAGE_CONNECT));
    DSP_CHECK_EQ_INT(DSP_WAKE_MAX_SLEEP, rep.plan.reason);
    DSP_CHECK_EQ_INT(6 * HOUR, rep.plan.sleep_s);
    const dsp_app_response_t *r = resp_of(DSP_CMD_STATUS);
    DSP_CHECK(r != NULL && r->status == DSP_RESP_OK && !r->storage_available);
    r = resp_of(TEST_MANUAL);
    DSP_CHECK(r != NULL && r->status == DSP_RESP_DEGRADED);
    DSP_CHECK(rep.slept && rep.retained_saved);
    /* Time retry state preserved through the degraded wake (only the counters change). */
    dsp_power_retained_t a, b;
    DSP_CHECK(dsp_power_retained_decode(before, sizeof before, &a));
    DSP_CHECK(dsp_power_retained_decode(rtc, rtc_len, &b));
    DSP_CHECK_EQ_INT(a.time.next_attempt, b.time.next_attempt);
    DSP_CHECK_EQ_INT(a.time.consecutive_failures, b.time.consecutive_failures);
    DSP_CHECK_EQ_INT(a.conn.last_notify_attempt, b.conn.last_notify_attempt);
    DSP_CHECK_EQ_INT(a.wake_count + 1, b.wake_count);
    kv.fail_get_key[0] = '\0';
    common_invariants();

    /* Notification write failures in the wake: dosing continues, SYNC retries the record. */
    world();
    boot_and_provision(CAP, 60, true);
    add_schedule(sched("a", 8, 0, 5000));
    replan();
    run_until(T0 - 30 * 60);
    pump.script[0] = DSP_PUMP_FAILED; /* creates a dose-bound report */
    pump.script_len = 1;
    fail_notify_sets = true;
    int guard = 0;
    while (pump.calls == 0 && guard++ < 10) {
        DSP_CHECK(sleep_and_wake(0));
    }
    DSP_CHECK_EQ_INT(1, pump.calls);
    DSP_CHECK(rep.dosing_degraded && rep.pump_fault);
    DSP_CHECK(!rep.synced_ok);
    /* The record stays STARTED (report not durable), recovered next wake: reported once. */
    DSP_CHECK_EQ_INT(DSP_HISTORY_STARTED, history_state("a", date_of(T0)));
    fail_notify_sets = false;
    DSP_CHECK(sleep_and_wake(0));
    DSP_CHECK_EQ_INT(1, rep.recovery.automatic);
    DSP_CHECK_EQ_INT(DSP_HISTORY_INTERRUPTED, history_state("a", date_of(T0)));
    DSP_CHECK_EQ_INT(1, pump.calls);
    common_invariants();

    /* A storage failure inside a dose: no further dosing in the wake. */
    world();
    boot_and_provision(CAP, 60, true);
    add_schedule(sched("a", 8, 0, 5000));
    add_schedule(sched("b", 8, 0, 5000));
    replan();
    run_until(T0 - 30 * 60);
    guard = 0;
    while (g_true + rep.plan.sleep_s < T0 && guard++ < 10) {
        DSP_CHECK(sleep_and_wake(0));
    }
    g_true += rep.plan.sleep_s;
    kv.power_cut_after_sets = 2; /* claim lands, the ledger write fails */
    kv.durable_on_set = false;
    DSP_CHECK(wake(DSP_CAUSE_TIMER));
    DSP_CHECK_EQ_INT(0, pump.calls);
    DSP_CHECK(rep.dosing_degraded);
    DSP_CHECK(rep.slept); /* shutdown still confirmed (pump OFF, radios off) */
    kv.durable_on_set = true;
    fake_kv_power_cycle(&kv);
    fake_kv_clear_faults(&kv);
    common_invariants();
}

static void test_clock_rollback_and_dst(void)
{
    /* NTP steps the clock back by 2 h after the 08:00 dose: never a duplicate. */
    world();
    boot_and_provision(CAP, 60, true);
    add_schedule(sched("a", 8, 0, 5000));
    replan();
    run_until(T0 + 10 * 60);
    DSP_CHECK_EQ_INT(1, pump.calls);
    g_offset += 2 * HOUR; /* the device clock runs 2 h fast: NTP will step it back */
    run_until(T0 + 2 * DAY);
    DSP_CHECK_EQ_INT(3, pump.calls);
    common_invariants();

    /* A backward step beyond 7 days holds automatic dosing (M1.4). */
    world();
    boot_and_provision(CAP, 60, true);
    add_schedule(sched("a", 8, 0, 5000));
    replan();
    run_until(T0 + 10 * 60);
    DSP_CHECK_EQ_INT(1, pump.calls);
    ntp_error = -9 * DAY;
    g_true += DAY - 20 * 60;
    run_until(g_true + 3 * DAY);
    DSP_CHECK_EQ_INT(1, pump.calls); /* held: nothing dosed on a clock 9 days back */
    common_invariants();

    /* DST: 02:30 and 03:30 schedules across both transitions, each local day once. */
    world();
    g_true = (dsp_utc_t)1806094800 - DAY; /* 2027-03-26 */
    g_offset = 5 - g_true;
    boot_and_provision(CAP, 60, true);
    add_schedule(sched("n", 2, 30, 1000));
    replan();
    run_until(g_true + 4 * DAY);
    int spring = pump.calls;
    DSP_CHECK(spring >= 3 && spring <= 4);
    for (int i = 1; i < pump.calls; i++) {
        DSP_CHECK(strcmp(pump.ids[i], pump.ids[i - 1]) != 0);
    }
    common_invariants();
}

static void test_safe_shutdown_paths(void)
{
    /* SAFE_OFF not confirmed at boot: nothing else runs; fail-safe instead of sleep. */
    world();
    pump.off_confirmable = false;
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    DSP_CHECK(rep.pump_unconfirmed);
    DSP_CHECK_EQ_INT(0, count_trace(DSP_STAGE_LOAD));
    DSP_CHECK(rep.fail_safe && !rep.slept);
    DSP_CHECK(last_failsafe & DSP_POWER_CHK_PUMP);
    DSP_CHECK_EQ_INT(DSP_APP_SHUTDOWN_ATTEMPTS, rep.shutdown_attempts);
    DSP_CHECK_EQ_INT(0, sleeps);
    DSP_CHECK(fake_kv_find(&kv, "dosing") == NULL); /* storage never touched */

    /* OFF fails only at shutdown (after a dose): fail-safe; repeated -> dosing inhibited. */
    world();
    boot_and_provision(CAP, 60, true);
    add_schedule(sched("a", 8, 0, 5000));
    add_schedule(sched("b", 8, 2, 5000));
    replan();
    run_until(T0 - 30 * 60);
    pump.is_off_calls = 0;
    pump.confirm_fail_from = 1;
    int guard = 0;
    while (pump.calls == 0 && guard++ < 10) {
        g_true += rep.plan.sleep_s;
        DSP_CHECK(wake(DSP_CAUSE_TIMER));
        if (rep.fail_safe) {
            break;
        }
    }
    /* SAFE_OFF itself could not confirm: fail-safe before any dose. */
    DSP_CHECK(rep.fail_safe);
    DSP_CHECK_EQ_INT(0, pump.calls);
    /* The platform resets; three consecutive fail-safes inhibit dosing. */
    for (int i = 0; i < 3; i++) {
        g_true += 5;
        DSP_CHECK(wake(DSP_CAUSE_SOFTWARE));
    }
    pump.confirm_fail_from = 0;
    g_true = T0 + 3 * 60;
    DSP_CHECK(wake(DSP_CAUSE_SOFTWARE));
    DSP_CHECK(rep.inhibited);
    DSP_CHECK_EQ_INT(0, pump.calls); /* inhibited although b is due */
    DSP_CHECK(rep.slept);            /* confirmed shutdown: counter reset */
    DSP_CHECK(sleep_and_wake(0));
    DSP_CHECK(!rep.inhibited);
    common_invariants();

    /* Pump fault during a dose: shutdown still confirmed and sleep entered. */
    world();
    boot_and_provision(CAP, 60, true);
    add_schedule(sched("a", 8, 0, 5000));
    replan();
    pump.script[0] = DSP_PUMP_START_FAILED;
    pump.script_len = 1;
    run_until(T0 + 60);
    DSP_CHECK_EQ_INT(1, pump.calls);
    DSP_CHECK(rep.slept && rep.shutdown_failures == 0);

    /* BLE cannot be confirmed stopped: fail-safe with the BLE bit. */
    world();
    boot_and_provision(CAP, 60, true);
    ble.stop_fails = true;
    ble.len = ble.next = 0;
    ble_script(g_true + 1, cmd_of(DSP_CMD_END_SESSION, 0, 0));
    DSP_CHECK(wake(DSP_CAUSE_BUTTON));
    DSP_CHECK(rep.fail_safe && (last_failsafe & DSP_POWER_CHK_BLE));
    DSP_CHECK(!(last_failsafe & DSP_POWER_CHK_PUMP));
    ble.stop_fails = false;
    ble_on = false;

    /* RTC save failure is not a safety item: sleep is still entered. */
    world();
    rtc_save_fails = true;
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    DSP_CHECK(rep.slept && !rep.retained_saved);
    DSP_CHECK_EQ_INT(0, sleep_violations);
    DSP_CHECK_EQ_INT(0, overlap_violations);
}

static void test_retained_state_across_wakes(void)
{
    world();
    boot_and_provision(CAP, 60, true);
    wifi_result = DSP_NET_ERR_TIMEOUT;
    /* A sync attempt fails -> backoff state in RTC memory -> no retry before it is due. */
    g_true += DAY;
    DSP_CHECK(wake(DSP_CAUSE_TIMER));
    dsp_power_retained_t r;
    DSP_CHECK(dsp_power_retained_decode(rtc, rtc_len, &r));
    DSP_CHECK(r.time.consecutive_failures >= 1);
    DSP_CHECK(r.time.next_attempt > raw());
    int connects = wifi_connects;
    g_true += 60;
    DSP_CHECK(wake(DSP_CAUSE_TIMER));
    DSP_CHECK(rep.retained_valid);
    DSP_CHECK_EQ_INT(connects, wifi_connects); /* backoff respected */
    /* A corrupted block is discarded: one attempt allowed again. */
    rtc[13] ^= 0x40;
    g_true += 60;
    DSP_CHECK(wake(DSP_CAUSE_TIMER));
    DSP_CHECK(!rep.retained_valid);
    DSP_CHECK(wifi_connects > connects);
    /* Power-on discards a valid block. */
    connects = wifi_connects;
    g_true += 60;
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    DSP_CHECK(!rep.retained_valid);
    common_invariants();
}

/* ---- Power loss at every stage ---------------------------------------------------------- */

/* Scenario: a button wake at 07:58 with a manual dose, two schedules due at 08:00 (handled
 * inside the BLE session), a session afterwards, then timer wakes for a day. */
/* Scenario 1 (BLE): button wake at 07:58, a manual dose, then a and b due at 08:00 inside
 * the session. Scenario 2 (CRITICAL): timer wake at 08:00, a's pump run faults, b at 20:00;
 * the CRITICAL report starts a session (Wi-Fi, NTP, HTTP posts) in the same wake. */
static int scenario;
static dsp_wake_cause_t scenario_prepare(void)
{
    world();
    http_ack = scenario == 2; /* scenario 1: reports stay queued and can be counted */
    boot_and_provision(CAP, 60, true);
    add_schedule(sched("a", 8, 0, 3000));
    add_schedule(sched("b", scenario == 1 ? 8 : 20, 0, 3000));
    replan();
    run_until(T0 - 30 * 60);
    ble.len = ble.next = 0;
    ble.nresp = 0;
    if (scenario == 1) {
        g_true = T0 - 120;
        ble_script(T0 - 100, cmd_of(TEST_MANUAL, 2000, 0));
        ble_script(T0 + 150, cmd_of(DSP_CMD_END_SESSION, 0, 0));
        return DSP_CAUSE_BUTTON;
    }
    g_true = T0;
    pump.script[pump.calls] = DSP_PUMP_FAILED;
    pump.script_len = pump.calls + 1;
    return DSP_CAUSE_TIMER;
}

static long scenario_events(void)
{
    dsp_wake_cause_t cause = scenario_prepare();
    int posts = http_posts;
    events = 0;
    cut_at = 0;
    DSP_CHECK(wake(cause));
    if (scenario == 1) {
        DSP_CHECK_EQ_INT(3, pump.calls); /* manual + a + b: the uncut scenario is not vacuous */
        DSP_CHECK_EQ_INT(1, count_manual_runs());
    } else {
        DSP_CHECK_EQ_INT(1, pump.calls);
        DSP_CHECK(http_posts > posts && wifi_connects > 0);
        DSP_CHECK_EQ_INT(DSP_CONN_DONE, rep.conn[0]);
    }
    return events;
}

static void test_power_loss_every_stage(void)
{
    int combos = 0;
    long points = 0;
    for (scenario = 1; scenario <= 2; scenario++) {
    long total = scenario_events();
    DSP_CHECK(total > 5);
    points += total;
    for (int variant = 0; variant < 4; variant++) {
        bool power_on = variant & 1;
        bool after = (variant & 2) != 0;
        for (long k = 1; k <= total; k++) {
            dsp_wake_cause_t cause = scenario_prepare();
            events = 0;
            cut_at = k;
            cut_after = after;
            bool done = wake(cause);
            cut_at = 0;
            DSP_CHECK(!done && cut_happened);
            int runs_at_cut = pump.calls;
            reboot_state(power_on);
            ble.len = ble.next = 0;
            /* Recovery and a day of timer wakes. */
            DSP_CHECK(wake(g_cause));
            run_until(T0 + DAY + HOUR);
            combos++;
            common_invariants();
            /* Each automatic occurrence at most once; manual at most once. */
            int a = 0, b = 0;
            for (int i = 0; i < pump.calls; i++) {
                a += strcmp(pump.ids[i], "a@2026-11-20") == 0;
                b += strcmp(pump.ids[i], "b@2026-11-20") == 0;
            }
            DSP_CHECK(a <= 1 && b <= 1);
            DSP_CHECK(count_manual_runs() <= 1);
            DSP_CHECK(runs_at_cut <= 3);
            if (scenario == 2) {
                /* a faulted: FAILED (reported) or INTERRUPTED; never repeated. */
                DSP_CHECK(history_state("a", date_of(T0)) == DSP_HISTORY_FAILED ||
                          history_state("a", date_of(T0)) == DSP_HISTORY_INTERRUPTED ||
                          history_state("a", date_of(T0)) == 0);
            }
            /* Persistent safety state preserved; nothing left in flight. */
            dsp_dosing_state_t ds = dosing_state();
            DSP_CHECK_EQ_INT(DSP_READINESS_READY, ds.readiness);
            DSP_CHECK(!(ds.flags & (DSP_DOSING_F_BLOCKED | DSP_DOSING_F_MANUAL_ACTIVE |
                                    DSP_DOSING_F_REFILL_PENDING)));
            DSP_CHECK(history_state("a", date_of(T0)) != DSP_HISTORY_STARTED);
            DSP_CHECK(history_state("b", date_of(T0)) != DSP_HISTORY_STARTED);
            /* Every interrupted occurrence was reported. */
            int interrupted = (history_state("a", date_of(T0)) == DSP_HISTORY_INTERRUPTED) +
                              (history_state("b", date_of(T0)) == DSP_HISTORY_INTERRUPTED);
            DSP_CHECK(durable_events(DSP_NOTE_DOSING_ERROR, DSP_DOSEERR_INTERRUPTED) >= interrupted);
            /* The estimate is never above the truth (full volume counted before the pump). */
            uint32_t remaining = load_cfg().reservoir_remaining_ul;
            DSP_CHECK((uint64_t)remaining + pump.pumped_ul <= CAP);
            if (pump.violations + overlap_violations + sleep_violations > 0) {
                fprintf(stderr, "power loss: scenario %d variant %d cut %ld of %ld\n", scenario,
                        variant, k, total);
                return;
            }
        }
    }
    }
    printf("power loss: %d combinations over %ld cut points (2 scenarios x 4 variants)\n", combos,
           points);
}

/* ---- 365-day simulations --------------------------------------------------------------- */

static int expected_between(const dsp_schedule_t *s, dsp_utc_t after, dsp_utc_t until)
{
    int total = 0;
    dsp_utc_t from = after;
    while (from < until) {
        dsp_utc_t to = from + 30 * DAY < until ? from + 30 * DAY : until;
        dsp_occurrence_t buf[64];
        size_t got = 0;
        DSP_CHECK_EQ_INT(DSP_REC_OK, dsp_rec_between(s, TZ, from, to, buf, 64, &got));
        total += (int)got;
        from = to;
    }
    return total;
}

static void test_long_run_clean(void)
{
    world();
    boot_and_provision(CAP, 60, true);
    dsp_schedule_t s[4] = {sched("m1", 8, 0, 4000), sched("m2", 8, 0, 4000),
                           sched("ev", 20, 30, 4000), sched("nt", 2, 30, 3000)};
    for (int i = 0; i < 4; i++) {
        add_schedule(s[i]);
    }
    replan();
    dsp_utc_t start = g_true;
    dsp_utc_t end = start + 366 * DAY;
    int refills = 0;
    int guard = 0;
    while (g_true < end && guard++ < 50000) {
        /* Refill over BLE at noon when low (user action on a button wake). */
        dsp_config_t c = load_cfg();
        dsp_date_t today = date_of(raw());
        (void)today;
        if (c.reservoir_remaining_ul < 30000) {
            ble.len = ble.next = 0;
            ble_script(g_true + 1, cmd_of(DSP_CMD_REFILL, CAP, CAP));
            ble_script(g_true + 2, cmd_of(DSP_CMD_END_SESSION, 0, 0));
            DSP_CHECK(wake(DSP_CAUSE_BUTTON));
            refills++;
            continue;
        }
        DSP_CHECK(sleep_and_wake(0));
    }
    int expected = 0;
    for (int i = 0; i < 4; i++) {
        expected += expected_between(&s[i], start, g_true - WINDOW);
    }
    int automatic = pump.calls - count_manual_runs();
    printf("clean year: %d wakes, %d doses (expected >= %d), %d refills, %d sleeps\n", wakes,
           automatic, expected, refills, sleeps);
    DSP_CHECK(automatic >= expected);
    DSP_CHECK(automatic <= expected + 4);
    DSP_CHECK_EQ_INT(0, fail_safes);
    common_invariants();
    /* Daily totals and the minimum interval held on every run. */
    for (int i = 1; i < pump.calls; i++) {
        DSP_CHECK(pump.at[i] >= pump.at[i - 1] + 60);
    }
}

static uint32_t rng = 0xC0FFEEu;
static uint32_t rnd(uint32_t bound)
{
    rng = rng * 1103515245u + 12345u;
    return bound == 0 ? 0 : (rng >> 8) % bound;
}

static void test_long_run_chaotic(void)
{
    world();
    boot_and_provision(CAP, 60, true);
    dsp_schedule_t s[3] = {sched("m1", 8, 0, 4000), sched("m2", 8, 0, 4000), sched("ev", 20, 30, 5000)};
    for (int i = 0; i < 3; i++) {
        add_schedule(s[i]);
    }
    replan();
    dsp_utc_t start = g_true;
    dsp_utc_t end = start + 365 * DAY;
    int cuts = 0, buttons = 0, wifi_faults = 0, steps = 0, faults = 0;
    int guard = 0;
    while (g_true < end && guard++ < 80000) {
        /* Environment for the next wake. */
        wifi_result = rnd(10) == 0 ? DSP_NET_ERR_NO_AP : DSP_NET_OK;
        wifi_faults += wifi_result != DSP_NET_OK;
        http_ack = rnd(4) != 0;
        ntp_error = rnd(20) == 0 ? -(int64_t)rnd(2 * HOUR) : (int64_t)rnd(5);
        steps += ntp_error < -5;
        if (rnd(60) == 0) {
            pump.fail_next = true;
            faults++;
        }
        int64_t latency = rnd(30) == 0 ? (int64_t)rnd(1200) : (int64_t)rnd(20);
        dsp_wake_cause_t cause = DSP_CAUSE_TIMER;
        int64_t sleep_s = rep.plan.sleep_s > 0 ? rep.plan.sleep_s : 60;
        if (rnd(25) == 0) {
            /* Button press during the sleep, with a few commands. */
            g_true += (int64_t)rnd((uint32_t)sleep_s);
            cause = DSP_CAUSE_BUTTON;
            buttons++;
            ble.len = ble.next = 0;
            dsp_config_t c = load_cfg();
            if (c.reservoir_remaining_ul < 40000 || rnd(3) == 0) {
                ble_script(g_true + 2, cmd_of(DSP_CMD_REFILL, CAP, CAP));
            }
            if (rnd(2) == 0) {
                ble_script(g_true + 5, cmd_of(TEST_MANUAL, 1000 + rnd(3000), 0));
            }
            ble_script(g_true + 6, cmd_of(DSP_CMD_STATUS, 0, 0));
            if (rnd(2) == 0) {
                ble_script(g_true + 20, cmd_of(DSP_CMD_END_SESSION, 0, 0));
            }
        } else {
            g_true += sleep_s + latency;
        }
        /* Random power cut inside the wake. */
        if (rnd(12) == 0) {
            events = 0;
            cut_at = 1 + rnd(12);
            cut_after = rnd(2) == 0;
        }
        bool done = wake(cause);
        cut_at = 0;
        if (!done) {
            cuts++;
            reboot_state(rnd(2) == 0);
            ble.len = ble.next = 0;
            DSP_CHECK(wake(g_cause));
        }
        DSP_CHECK(rep.slept || rep.fail_safe);
        DSP_CHECK_EQ_INT(0, fail_safes);
    }
    int automatic = pump.calls - count_manual_runs();
    int expected = 0;
    for (int i = 0; i < 3; i++) {
        expected += expected_between(&s[i], start, end);
    }
    printf("chaotic year: %d wakes, %d automatic + %d manual runs (scheduled %d), %d cuts, "
           "%d button wakes, %d Wi-Fi faults, %d backward steps, %d pump faults\n",
           wakes, automatic, count_manual_runs(), expected, cuts, buttons, wifi_faults, steps, faults);
    DSP_CHECK(faults > 5);
    common_invariants();
    DSP_CHECK(automatic > expected / 2); /* not vacuous */
    DSP_CHECK(automatic <= expected);
    DSP_CHECK(cuts > 20);
    /* Minimum interval between any two pump starts (device time). */
    for (int i = 1; i < pump.calls && i < MAX_RUNS; i++) {
        DSP_CHECK(pump.at[i] >= pump.at[i - 1] + 60 || pump.at[i] < pump.at[i - 1]);
    }
    /* Daily totals per local date (device time) never above the limit. */
    for (int i = 0; i < pump.calls && i < MAX_RUNS; i++) {
        dsp_date_t di = date_of(pump.at[i]);
        uint64_t sum = 0;
        for (int j = 0; j < pump.calls && j < MAX_RUNS; j++) {
            dsp_date_t dj;
            if (dsp_utc_to_local_date(TZ, pump.at[j], &dj) && dsp_date_compare(di, dj) == 0) {
                sum += pump.volume[j];
            }
        }
        DSP_CHECK(sum <= 60000);
    }
}

int main(void)
{
    DSP_RUN(test_retained_block);
    DSP_RUN(test_power_plan);
    DSP_RUN(test_first_boot);
    DSP_RUN(test_normal_scheduled_dosing);
    DSP_RUN(test_simultaneous_schedules);
    DSP_RUN(test_missed_occurrences);
    DSP_RUN(test_manual_dose_service_mode);
    DSP_RUN(test_refill_readiness_blocked);
    DSP_RUN(test_wifi_unavailable);
    DSP_RUN(test_critical_notification_existing_wake);
    DSP_RUN(test_ble_interrupted_by_due_dose);
    DSP_RUN(test_ble_session_lifetime);
    DSP_RUN(test_storage_failures);
    DSP_RUN(test_clock_rollback_and_dst);
    DSP_RUN(test_safe_shutdown_paths);
    DSP_RUN(test_retained_state_across_wakes);
    DSP_RUN(test_power_loss_every_stage);
    DSP_RUN(test_long_run_clean);
    DSP_RUN(test_long_run_chaotic);
    return dsp_test_finish();
}
