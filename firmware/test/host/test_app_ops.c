/*
 * M1.10b.1 tests: configuration, schedule management and phone time through the
 * Application Service (dsp_app_*), exercised the way the future BLE dispatcher will call
 * them: from the application task, inside a service session (the BLE fake runs scripted
 * operations from its wait(), synchronously in the application task).
 *
 * Complete wakes over the real domain services (storage, coordinator, Dosing Service,
 * notifications, timekeeper) with fakes for the KV store (power cuts that ABORT the wake at
 * any write of an operation, via longjmp, with the write landed or not), the device clock
 * and its RTC continuity, the pump, the power port and BLE. No Wi-Fi: time comes from the
 * phone only.
 *
 * Oracles on EVERY pump start: no BLE active, no operation in progress, the automatic
 * occurrence never executed before and inside its due window, and not before the
 * schedule's edit floor.
 */
#include "dsp_app.h"
#include "dsp_app_sec.h"
#include "dsp_codec.h"
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
#define MINUTE 60
#define T0 ((dsp_utc_t)1795158000) /* 2026-11-20T07:00:00Z = 08:00 CET */
#define HW_MAX_MS 60000u
#define CAP 100000u
#define WINDOW DSP_EXEC_DUE_WINDOW_DEFAULT_S

static const dsp_app_auth_t OWNER = {DSP_APP_ROLE_OWNER, 0, 0, 0};
static const dsp_app_auth_t OWNER3 = {DSP_APP_ROLE_OWNER, 3, 0, 0};
static const dsp_app_auth_t NOBODY = {DSP_APP_ROLE_NONE, 0, 0, 0};
static const dsp_app_auth_t BAD_SLOT = {DSP_APP_ROLE_OWNER, 4, 0, 0};
static const dsp_app_auth_t BAD_ROLE = {7, 0, 0, 0};

/* ---- Power cuts ------------------------------------------------------------------------ */

static jmp_buf cut_env;
static bool in_wake;
static bool armed;       /* cut points count only while an operation runs */
static long op_writes;   /* KV sets during armed operations */
static long cut_at;      /* 0 = no cut */
static bool cut_after;   /* the cut write lands before the power goes */
static bool cut_happened;
static void power_lost(void);

/* ---- Clock ------------------------------------------------------------------------------- */

static dsp_utc_t g_true;   /* true UTC */
static int64_t g_offset;   /* device raw clock = g_true + g_offset */
static dsp_utc_t g_marker; /* RTC continuity marker (last accepted sync), 0 = none */
static bool clock_set_fails;

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
    if (clock_set_fails) {
        return false;
    }
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
static uint64_t mono_ms;
static uint64_t mono_now(void *ctx)
{
    (void)ctx;
    return mono_ms;
}
static const dsp_monotonic_t MONO = {NULL, mono_now};

static void advance(dsp_utc_t to)
{
    if (to > g_true) {
        mono_ms += (uint64_t)(to - g_true) * 1000u;
        g_true = to;
    }
}

/* ---- Storage proxy ------------------------------------------------------------------------ */

static fake_kv_t kv;
static dsp_kv_t proxy;
static char fail_set_key[16]; /* sets of this key fail (write error), "" = none */

static dsp_kv_status_t px_get(void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *len)
{
    (void)ctx;
    return kv.iface.get(kv.iface.ctx, key, buf, cap, len);
}
static dsp_kv_status_t px_set(void *ctx, const char *key, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (fail_set_key[0] != '\0' && strcmp(key, fail_set_key) == 0) {
        return DSP_KV_ERR_WRITE;
    }
    if (in_wake && armed) {
        op_writes++;
        if (cut_at > 0 && op_writes == cut_at) {
            if (cut_after) {
                (void)kv.iface.set(kv.iface.ctx, key, data, len);
            }
            power_lost();
        }
    }
    return kv.iface.set(kv.iface.ctx, key, data, len);
}
static dsp_kv_status_t px_commit(void *ctx)
{
    (void)ctx;
    return kv.iface.commit(kv.iface.ctx);
}

/* ---- Pump ------------------------------------------------------------------------------------ */

#define MAX_RUNS 256
static bool ble_on;
static bool op_running;
static bool op_allows_pump; /* the running operation is a manual dose (M1.10b.2) */
static bool pump_cut;       /* armed operation: the power goes during the pump run */
static struct {
    int calls;
    int violations;
    char ids[MAX_RUNS][96];
    dsp_utc_t at[MAX_RUNS];
} pump;

static dsp_schedule_set_t durable_set(void);
static bool pump_fail_next;
static void (*pump_hook)(void); /* called while the pump runs (test-specific probes) */
/* M1.10c.2a: manual doses arrive as encrypted BLE requests (dispatched after the hook that posted
 * them), not as scripted OP_MANUAL steps; the durable-decision oracle below still applies. */
static bool manual_via_ble;

static dsp_pump_result_t pump_run(void *ctx, uint32_t runtime_ms)
{
    (void)ctx;
    int i = pump.calls++;
    if (ble_on || (op_running && !op_allows_pump)) {
        pump.violations++;
        fprintf(stderr, "pump started while BLE is on (%d) or an operation runs (%d)\n", ble_on,
                op_running);
    }
    dsp_utc_t now = raw();
    char id[96] = "manual";
    /* M1.10b.2 oracle: a manual run is bound to a request whose decision is durable BEFORE the
     * pump: marker with (slot, req), manual_hw[slot] >= req, ring entry PENDING. */
    {
        const fake_kv_entry_t *de = fake_kv_find(&kv, "dosing");
        dsp_dosing_state_t ds;
        if (de != NULL && dsp_codec_decode_dosing(de->value, de->len, &ds) == DSP_CODEC_OK &&
            (ds.flags & DSP_DOSING_F_MANUAL_ACTIVE)) {
            const dsp_manual_result_t *e = dsp_dosing_state_find_result(&ds, ds.marker_slot, ds.marker_req);
            if (ds.marker_req == 0 || ds.manual_hw[ds.marker_slot] < ds.marker_req || e == NULL ||
                e->state != DSP_MRES_PENDING || !(op_allows_pump || manual_via_ble)) {
                pump.violations++;
                fprintf(stderr, "manual pump run without a durable decision\n");
            }
            snprintf(id, sizeof id, "m%u:%llu", ds.marker_slot, (unsigned long long)ds.marker_req);
        }
    }
    for (size_t s = 0; s < DSP_STORAGE_HISTORY_SLOTS; s++) {
        char key[8];
        dsp_storage_history_key(s, key, sizeof key);
        const fake_kv_entry_t *e = fake_kv_find(&kv, key);
        dsp_history_record_t rec;
        if (e != NULL && dsp_codec_decode_history(e->value, e->len, &rec) == DSP_CODEC_OK &&
            rec.state == DSP_HISTORY_STARTED && rec.claimed_at == now) {
            snprintf(id, sizeof id, "%s@%04d-%02u-%02u", rec.schedule_id, rec.local_date.year,
                     rec.local_date.month, rec.local_date.day);
            if (now < rec.scheduled_at || now - rec.scheduled_at > WINDOW) {
                pump.violations++;
                fprintf(stderr, "pump outside the due window: %s\n", id);
            }
            dsp_schedule_set_t set = durable_set();
            for (size_t k = 0; k < set.count; k++) {
                if (strcmp(set.schedules[k].id, rec.schedule_id) == 0 &&
                    set.schedules[k].not_before != 0 && rec.scheduled_at < set.schedules[k].not_before) {
                    pump.violations++;
                    fprintf(stderr, "pump for an occurrence before the edit floor: %s\n", id);
                }
            }
        }
    }
    for (int k = 0; k < i && k < MAX_RUNS; k++) {
        if (strcmp(id, "manual") != 0 && strcmp(pump.ids[k], id) == 0) {
            pump.violations++;
            fprintf(stderr, "DUPLICATE pump run: %s\n", id); /* automatic or manual request */
        }
    }
    if (i < MAX_RUNS) {
        snprintf(pump.ids[i], sizeof pump.ids[i], "%s", id);
        pump.at[i] = g_true;
    }
    if (pump_hook != NULL) {
        void (*h)(void) = pump_hook;
        pump_hook = NULL;
        h();
    }
    advance(g_true + (dsp_utc_t)((runtime_ms + 999u) / 1000u));
    if (in_wake && armed && pump_cut) {
        power_lost(); /* power lost during the pump run */
    }
    if (pump_fail_next) {
        pump_fail_next = false;
        return DSP_PUMP_FAILED;
    }
    return DSP_PUMP_OK;
}
static void pump_off(void *ctx)
{
    (void)ctx;
}
static bool pump_is_off(void *ctx)
{
    (void)ctx;
    return true;
}
static const dsp_pump_port_t PUMP = {NULL, pump_run, pump_off, pump_is_off};

static int runs_of(const char *id)
{
    int c = 0;
    for (int i = 0; i < pump.calls && i < MAX_RUNS; i++) {
        c += strcmp(pump.ids[i], id) == 0;
    }
    return c;
}

/* ---- Power port ----------------------------------------------------------------------------- */

static dsp_wake_cause_t g_cause;
static uint8_t rtc[DSP_POWER_RETAINED_BYTES];
static size_t rtc_len;
static int sleeps, fail_safes;

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
    if (len > sizeof rtc) {
        return false;
    }
    memcpy(rtc, buf, len);
    rtc_len = len;
    return true;
}
static void pw_sleep(void *ctx, int64_t s)
{
    (void)ctx;
    (void)s;
    sleeps++;
}
static void pw_fail_safe(void *ctx, uint32_t failures)
{
    (void)ctx;
    (void)failures;
    fail_safes++;
}
static const dsp_power_port_t POWER = {NULL, pw_cause, pw_load, pw_save, pw_sleep, pw_fail_safe};

/* ---- Scripted operations (the future dispatcher) --------------------------------------------- */

static dsp_app_t app;

enum {
    OP_TIME = 1,
    OP_GET_CONFIG,
    OP_SET_CONFIG,
    OP_SET_CONFIG_CUR, /* expected revision = the current one (read first) */
    OP_LIST,
    OP_GET,
    OP_CREATE,
    OP_UPDATE,
    OP_UPDATE_CUR, /* expected revision = the stored one, record revision = + 1 */
    OP_DELETE,
    OP_DELETE_CUR,
    OP_FRAME,      /* M1.10c.1: post a raw protocol frame (o.frame) */
    OP_END,        /* authorized end of the service window (C10: dsp_app_end_service_session) */
    OP_END_TIMEOUT, /* no authorization at all: the window ends by its maximum (monotonic jump) */
    OP_REFILL,     /* M1.10c.1: dsp_app_refill (formerly a queue command) */
    OP_READINESS,  /* M1.10c.1: dsp_app_set_readiness */
    OP_MANUAL,     /* M1.10b.2 */
    OP_GET_RESULT,
    OP_FLOOR,
    OP_HOOK,       /* M1.10b.3: a test-specific call inside the session (hook(i)) */
};

typedef struct {
    int kind;
    const dsp_app_auth_t *auth;
    bool no_auth; /* pass NULL as the authorization */
    dsp_schedule_t s;
    dsp_config_t cfg;
    uint32_t rev;
    uint16_t mask;
    uint8_t page;
    int64_t ms;
    uint32_t unc;
    char id[DSP_SCHEDULE_ID_MAX_LEN + 1];
    uint32_t capacity, remaining; /* OP_REFILL */
    uint8_t readiness;            /* OP_READINESS */
    uint8_t frame[64];            /* OP_FRAME */
    size_t frame_len;
    uint64_t seq;
    uint32_t volume;
    void (*hook)(int i); /* OP_HOOK */
    int a, b;            /* OP_HOOK arguments */
} op_t;

typedef struct {
    bool ran;
    dsp_app_op_result_t r;
    uint32_t rev, set_rev, hw;
    dsp_app_time_result_t t;
    dsp_config_t cfg;
    dsp_schedule_t s;
    dsp_app_schedule_page_t page;
    dsp_app_manual_result_t manual;
    uint8_t state;
    uint32_t detail;
    uint64_t floor;
    int pumps;   /* pump runs during the operation */
    long writes; /* KV sets during the operation */
} res_t;

#define MAX_STEPS 64
static struct {
    dsp_utc_t at[MAX_STEPS];
    op_t op[MAX_STEPS];
    res_t res[MAX_STEPS];
    int len, next;
    bool arm[MAX_STEPS]; /* power cuts allowed during this step */
    bool start_fails;
    int starts, suspends, resumes, responses, disconnects;
    uint8_t resp[16][64]; /* frames sent by the device (first 64 bytes) */
    size_t resp_len[16];
    int nresp;
    uint32_t gen;         /* connection generation of the simulated phone */
} sc;

/* ---- M1.10c.1: issued authorizations ----------------------------------------------------------
 * Every operation requires an authorization issued by the Application Service for the current
 * connection. The harness plays the security layer: a hand-made {OWNER, k} in a step is replaced
 * at execution time by an authorization obtained through dsp_app_sec_admit(SESSION) +
 * dsp_app_sec_lookup() with slot k's enrolled public key from the durable record (as an IK
 * handshake would produce). Authorizations that are malformed (NULL, NONE, bad slot or role)
 * or already issued (token != 0) pass through unchanged; raw_auth disables the substitution. */
static uint32_t harness_rng = 0x2468ACE1u;
static bool harness_random(void *ctx, uint8_t *out, size_t len)
{
    (void)ctx;
    for (size_t i = 0; i < len; i++) {
        harness_rng = harness_rng * 1664525u + 1013904223u;
        out[i] = (uint8_t)(harness_rng >> 24);
    }
    return true;
}
static const dsp_app_security_t HARNESS_SEC = {.ctx = NULL, .random = harness_random};
static bool raw_auth;           /* a step's hand-made authorization is passed as it is */
static dsp_app_auth_t issued;   /* the last authorization obtained by the harness */

static const dsp_app_auth_t *issue_for(const dsp_app_auth_t *a)
{
    if (raw_auth || a == NULL || a->role != DSP_APP_ROLE_OWNER || a->slot >= DSP_APP_PHONE_SLOTS ||
        a->token != 0) {
        return a;
    }
    const dsp_app_owner_t *o = &app.own;
    if (o->authz_valid && o->authz_slot == a->slot && o->authz_session == app.ss.session_id &&
        app.ss.session_id != 0 && o->authz_epoch == o->epoch && issued.token == o->authz_token) {
        return &issued;
    }
    if (o->cred != DSP_CRED_CLAIMED || !o->cache.slot[a->slot].used || !app.accepting) {
        return a; /* nothing could authenticate this phone: the operation refuses it */
    }
    uint8_t pub[32];
    memcpy(pub, o->cache.slot[a->slot].pub, sizeof pub);
    dsp_app_sec_connection_begin(&app); /* the phone of slot k (re)connects */
    dsp_app_hs_t hs;
    if (dsp_app_sec_admit(&app, DSP_SEC_SESSION, &hs).status != DSP_OP_OK ||
        dsp_app_sec_lookup(&app, &hs, pub, &issued).status != DSP_OP_OK) {
        return a;
    }
    return &issued;
}

/* A claimed record with four harness phones (keys 0x31 + slot), written into the store. */
static void harness_claim_record(void)
{
    dsp_auth_t a;
    dsp_auth_provisioned(&a);
    uint8_t ip[32], iu[32], p[32];
    memset(ip, 0x71, sizeof ip);
    memset(iu, 0x72, sizeof iu);
    memset(p, 0x31, sizeof p);
    (void)dsp_auth_claim(&a, ip, iu, p, "H0", 2, 0);
    for (uint8_t k = 1; k < DSP_AUTH_SLOTS; k++) {
        memset(p, 0x31 + k, sizeof p);
        (void)dsp_auth_add(&a, p, "H", 1, 0, NULL);
    }
    uint8_t b[DSP_AUTH_RECORD_BYTES];
    if (dsp_codec_encode_auth(&a, b, sizeof b) == DSP_AUTH_RECORD_BYTES) {
        fake_kv_put_raw(&kv, "auth", b, sizeof b);
    }
    dsp_auth_wipe(&a);
}

static void exec_op(int i)
{
    const op_t *o = &sc.op[i];
    res_t *r = &sc.res[i];
    /* hooks play the security layer themselves: never substitute for them */
    /* hooks play the security layer themselves; frames and END carry no authorization */
    bool no_issue = o->kind == OP_HOOK || o->kind == OP_FRAME || o->kind == OP_END_TIMEOUT;
    const dsp_app_auth_t *auth = o->no_auth ? NULL : (no_issue ? o->auth : issue_for(o->auth));
    r->ran = true;
    long before = kv.sets;
    int pumps_before = pump.calls;
    op_running = true;
    op_allows_pump = o->kind == OP_MANUAL;
    armed = sc.arm[i];
    switch (o->kind) {
    case OP_TIME:
        r->r = dsp_app_set_phone_time(&app, auth, o->ms, o->unc, &r->t);
        break;
    case OP_GET_CONFIG:
        r->r = dsp_app_get_config(&app, auth, &r->cfg, &r->hw);
        break;
    case OP_SET_CONFIG:
        r->r = dsp_app_set_config(&app, auth, o->rev, o->mask, &o->cfg, &r->rev);
        break;
    case OP_SET_CONFIG_CUR: {
        dsp_config_t cur;
        memset(&cur, 0, sizeof cur);
        (void)dsp_app_get_config(&app, auth, &cur, NULL);
        r->r = dsp_app_set_config(&app, auth, cur.revision, o->mask, &o->cfg, &r->rev);
        break;
    }
    case OP_LIST:
        r->r = dsp_app_list_schedules(&app, auth, o->page, &r->page);
        break;
    case OP_GET:
        r->r = dsp_app_get_schedule(&app, auth, o->id, &r->s);
        break;
    case OP_CREATE:
        r->r = dsp_app_create_schedule(&app, auth, &o->s, &r->set_rev);
        break;
    case OP_UPDATE:
        r->r = dsp_app_update_schedule(&app, auth, o->rev, &o->s, &r->rev, &r->set_rev);
        break;
    case OP_UPDATE_CUR: {
        dsp_schedule_t cur;
        memset(&cur, 0, sizeof cur);
        (void)dsp_app_get_schedule(&app, auth, o->s.id, &cur);
        dsp_schedule_t s = o->s;
        s.revision = cur.revision + 1;
        r->r = dsp_app_update_schedule(&app, auth, cur.revision, &s, &r->rev, &r->set_rev);
        break;
    }
    case OP_DELETE:
        r->r = dsp_app_delete_schedule(&app, auth, o->id, o->rev, &r->set_rev);
        break;
    case OP_DELETE_CUR: {
        dsp_schedule_t cur;
        memset(&cur, 0, sizeof cur);
        (void)dsp_app_get_schedule(&app, auth, o->id, &cur);
        r->r = dsp_app_delete_schedule(&app, auth, o->id, cur.revision, &r->set_rev);
        break;
    }
    case OP_FRAME:
        r->r.status = dsp_app_post_frame(&app, sc.gen, o->frame, o->frame_len) ? DSP_OP_OK : DSP_OP_BUSY;
        break;
    case OP_END:
        r->r = dsp_app_end_service_session(&app, auth);
        break;
    case OP_END_TIMEOUT:
        mono_ms += 24u * 3600u * 1000u; /* past any session maximum: an internal transition */
        r->r.status = DSP_OP_OK;
        break;
    case OP_REFILL:
        r->r = dsp_app_refill(&app, auth, o->capacity, o->remaining, NULL);
        break;
    case OP_READINESS:
        r->r = dsp_app_set_readiness(&app, auth, o->readiness, NULL);
        break;
    case OP_MANUAL:
        r->r = dsp_app_manual_dose(&app, auth, o->seq, o->volume, &r->manual);
        break;
    case OP_GET_RESULT:
        r->r = dsp_app_get_result(&app, auth, o->seq, &r->state, &r->detail);
        break;
    case OP_FLOOR:
        r->r = dsp_app_manual_floor(&app, auth, &r->floor);
        break;
    case OP_HOOK:
        if (o->hook != NULL) {
            o->hook(i);
        }
        break;
    default:
        break;
    }
    armed = false;
    op_running = false;
    op_allows_pump = false;
    r->writes = kv.sets - before;
    r->pumps = pump.calls - pumps_before;
}

static void run_due_steps(void)
{
    while (sc.next < sc.len && sc.at[sc.next] <= g_true) {
        if ((sc.op[sc.next].kind == OP_END || sc.op[sc.next].kind == OP_END_TIMEOUT) &&
            app.q_count > 0) {
            break; /* frames posted before the end are processed first (the loop drains them) */
        }
        exec_op(sc.next);
        sc.next++;
    }
}

static int step(dsp_utc_t at, op_t o)
{
    if (sc.len >= MAX_STEPS) {
        return MAX_STEPS - 1;
    }
    if (o.auth == NULL) {
        o.auth = &OWNER;
    }
    sc.at[sc.len] = at;
    sc.op[sc.len] = o;
    memset(&sc.res[sc.len], 0, sizeof sc.res[0]);
    sc.arm[sc.len] = false;
    return sc.len++;
}
static int step_armed(dsp_utc_t at, op_t o)
{
    int i = step(at, o);
    sc.arm[i] = true;
    return i;
}
static op_t mk(int kind)
{
    op_t o;
    memset(&o, 0, sizeof o);
    o.kind = kind;
    return o;
}
static op_t op_time(int64_t ms, uint32_t unc)
{
    op_t o = mk(OP_TIME);
    o.ms = ms;
    o.unc = unc;
    return o;
}
static op_t op_now(void) /* the phone's (true) time, half a second of uncertainty */
{
    return op_time((int64_t)g_true * 1000 + 250, 500);
}
static op_t op_set_cfg(uint32_t rev, uint16_t mask, dsp_config_t c)
{
    op_t o = mk(OP_SET_CONFIG);
    o.rev = rev;
    o.mask = mask;
    o.cfg = c;
    return o;
}
static op_t op_set_cfg_cur(uint16_t mask, dsp_config_t c)
{
    op_t o = op_set_cfg(0, mask, c);
    o.kind = OP_SET_CONFIG_CUR;
    return o;
}
static op_t op_create(dsp_schedule_t s)
{
    op_t o = mk(OP_CREATE);
    o.s = s;
    return o;
}
static op_t op_update(uint32_t expected, dsp_schedule_t s)
{
    op_t o = mk(OP_UPDATE);
    o.rev = expected;
    o.s = s;
    return o;
}
static op_t op_update_cur(dsp_schedule_t s)
{
    op_t o = op_update(0, s);
    o.kind = OP_UPDATE_CUR;
    return o;
}
static op_t op_delete(const char *id, uint32_t rev)
{
    op_t o = mk(OP_DELETE);
    snprintf(o.id, sizeof o.id, "%s", id);
    o.rev = rev;
    return o;
}
static op_t op_delete_cur(const char *id)
{
    op_t o = op_delete(id, 0);
    o.kind = OP_DELETE_CUR;
    return o;
}
static op_t op_get(const char *id)
{
    op_t o = mk(OP_GET);
    snprintf(o.id, sizeof o.id, "%s", id);
    return o;
}
static op_t op_list(uint8_t page)
{
    op_t o = mk(OP_LIST);
    o.page = page;
    return o;
}
static op_t op_ready(void)
{
    op_t o = mk(OP_READINESS);
    o.readiness = DSP_READINESS_READY;
    return o;
}
static op_t op_manual(uint64_t seq, uint32_t volume)
{
    op_t o = mk(OP_MANUAL);
    o.seq = seq;
    o.volume = volume;
    return o;
}
static op_t op_result(uint64_t seq)
{
    op_t o = mk(OP_GET_RESULT);
    o.seq = seq;
    return o;
}
static op_t op_floor(void)
{
    return mk(OP_FLOOR);
}
static op_t as(op_t o, const dsp_app_auth_t *auth)
{
    o.auth = auth;
    o.no_auth = auth == NULL;
    return o;
}

/* ---- BLE port --------------------------------------------------------------------------------- */

static bool ble_start(void *ctx)
{
    (void)ctx;
    sc.starts++;
    if (sc.start_fails) {
        return false;
    }
    ble_on = true;
    (void)dsp_app_post_conn_event(&app, true, ++sc.gen, 247); /* the phone connects */
    return true;
}
static bool ble_connected(void *ctx)
{
    (void)ctx;
    return ble_on; /* the phone stays connected while the radio is on */
}
static void ble_suspend(void *ctx)
{
    (void)ctx;
    sc.suspends++;
    ble_on = false;
}
static bool ble_resume(void *ctx)
{
    (void)ctx;
    sc.resumes++;
    ble_on = true;
    (void)dsp_app_post_conn_event(&app, true, ++sc.gen, 247); /* the phone reconnects */
    return true;
}
static void ble_stop(void *ctx)
{
    (void)ctx;
    ble_on = false;
}
static bool ble_is_off(void *ctx)
{
    (void)ctx;
    return !ble_on;
}
static void ble_wait(void *ctx, uint32_t max_ms)
{
    (void)ctx;
    dsp_utc_t until = g_true + (dsp_utc_t)((max_ms + 999u) / 1000u);
    if (sc.next < sc.len && sc.at[sc.next] < until) {
        until = sc.at[sc.next] > g_true ? sc.at[sc.next] : g_true;
    }
    advance(until);
    run_due_steps();
}
static bool ble_send_frame(void *ctx, const uint8_t *f, size_t n)
{
    (void)ctx;
    sc.responses++;
    if (sc.nresp < 16) {
        size_t c = n < sizeof sc.resp[0] ? n : sizeof sc.resp[0];
        memcpy(sc.resp[sc.nresp], f, c);
        sc.resp_len[sc.nresp++] = n;
    }
    return true;
}
static void ble_disconnect(void *ctx)
{
    (void)ctx;
    sc.disconnects++;
    /* the simulated phone reconnects at once (e.g. after the 30 s unauthenticated limit) */
    (void)dsp_app_post_conn_event(&app, true, ++sc.gen, 247);
}
static const dsp_ble_port_t BLE = {NULL,       ble_start,      ble_connected, ble_suspend,
                                   ble_resume, ble_stop,       ble_is_off,    ble_wait,
                                   ble_send_frame, ble_disconnect, NULL};

/* ---- World ------------------------------------------------------------------------------------- */

static dsp_app_config_t CFG;
static dsp_app_report_t rep;

/* M1.10b.3: tests that write in bulk within one session raise the session-local limits. */
static void bulk_limits(void)
{
    CFG.session_writes_max = 1000;
    CFG.writes_per_minute = 1000;
    CFG.session_ops_max = 4000;
}

static void world(void)
{
    fake_kv_init(&kv, true);
    proxy.ctx = NULL;
    proxy.get = px_get;
    proxy.set = px_set;
    proxy.commit = px_commit;
    in_wake = armed = cut_after = cut_happened = false;
    pump_cut = pump_fail_next = op_allows_pump = false;
    pump_hook = NULL;
    op_writes = cut_at = 0;
    fail_set_key[0] = '\0';
    g_true = T0 - HOUR; /* 07:00 local */
    g_offset = 5 - g_true; /* power-on: the raw clock counts from ~0 (untrusted) */
    g_marker = 0;
    clock_set_fails = false;
    mono_ms = 0;
    ble_on = op_running = false;
    memset(&pump, 0, sizeof pump);
    g_cause = DSP_CAUSE_POWER_ON;
    memset(rtc, 0xA5, sizeof rtc);
    rtc_len = 0;
    sleeps = fail_safes = 0;
    memset(&sc, 0, sizeof sc);
    memset(&CFG, 0, sizeof CFG);
    CFG.kv = &proxy;
    CFG.tz = TZ;
    CFG.pump = &PUMP;
    CFG.hw_max_runtime_ms = HW_MAX_MS;
    CFG.power = &POWER;
    CFG.clock = &CLOCK;
    CFG.time = &TIMEP;
    CFG.ble = &BLE;
    CFG.mono = &MONO;
    CFG.random = 0x5EED1234u;
    CFG.security = &HARNESS_SEC; /* M1.10c.1: authorizations are issued (see issue_for) */
    raw_auth = false;
    memset(&issued, 0, sizeof issued);
}

static void power_lost(void)
{
    cut_happened = true;
    ble_on = false;
    op_running = false;
    armed = false;
    longjmp(cut_env, 1);
}

/* After an aborted wake the power came back: RTC memory, clock and marker are lost. */
static void reboot_power_on(void)
{
    fake_kv_power_cycle(&kv);
    fake_kv_clear_faults(&kv);
    g_cause = DSP_CAUSE_POWER_ON;
    memset(rtc, 0x5A, sizeof rtc);
    rtc_len = 0;
    g_marker = 0;
    g_offset = 3 - g_true;
    advance(g_true + 2);
}

static void clear_script(void)
{
    sc.len = sc.next = 0;
    sc.nresp = 0;
}

/* One wake. Returns false when it was aborted by a power cut. */
static bool wake(dsp_wake_cause_t cause)
{
    g_cause = cause;
    in_wake = true;
    cut_happened = false;
    op_writes = 0;
    if (setjmp(cut_env) == 0) {
        rep = dsp_app_run_wake(&app, &CFG);
        in_wake = false;
        return true;
    }
    in_wake = false;
    return false;
}

/* A button wake running the script; the owner ends the window after the last step (authorized,
 * C10) unless the script already ends it (OP_END / OP_END_TIMEOUT). */
static bool session(void)
{
    dsp_utc_t last = sc.len > 0 ? sc.at[sc.len - 1] : g_true;
    bool ends = sc.len > 0 && (sc.op[sc.len - 1].kind == OP_END ||
                               sc.op[sc.len - 1].kind == OP_END_TIMEOUT);
    if (!ends) {
        step(last, mk(OP_END));
    }
    bool ok = wake(DSP_CAUSE_BUTTON);
    return ok;
}

static void sleep_and_wake(void)
{
    advance(g_true + rep.plan.sleep_s);
    DSP_CHECK(wake(DSP_CAUSE_TIMER));
}

static void run_until(dsp_utc_t end)
{
    for (int guard = 0; g_true < end && guard < 5000; guard++) {
        if (g_true + rep.plan.sleep_s > end) {
            advance(end);
            DSP_CHECK(wake(DSP_CAUSE_TIMER));
        } else {
            sleep_and_wake();
        }
    }
}

/* ---- Durable state ------------------------------------------------------------------------------ */

static dsp_storage_t side;
static bool side_open(void)
{
    return dsp_storage_open(&side, &proxy, TZ, false) == DSP_STORAGE_OK;
}
static dsp_config_t durable_cfg(void)
{
    dsp_config_t c;
    memset(&c, 0, sizeof c);
    DSP_CHECK(side_open());
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_load_config(&side, &c));
    return c;
}
static dsp_schedule_set_t durable_set(void)
{
    dsp_schedule_set_t set;
    memset(&set, 0, sizeof set);
    const fake_kv_entry_t *e = fake_kv_find(&kv, "sched");
    if (e != NULL) {
        (void)dsp_codec_decode_schedule_set(e->value, e->len, TZ, &set);
    }
    return set;
}
static const dsp_schedule_t *find_in(const dsp_schedule_set_t *set, const char *id)
{
    for (size_t i = 0; i < set->count; i++) {
        if (strcmp(set->schedules[i].id, id) == 0) {
            return &set->schedules[i];
        }
    }
    return NULL;
}
static dsp_time_meta_t durable_time(void)
{
    dsp_time_meta_t m;
    memset(&m, 0, sizeof m);
    const fake_kv_entry_t *e = fake_kv_find(&kv, "time");
    if (e != NULL) {
        DSP_CHECK_EQ_INT(DSP_CODEC_OK, dsp_codec_decode_time(e->value, e->len, &m));
    }
    return m;
}
static uint8_t durable_readiness(void)
{
    dsp_dosing_state_t d;
    memset(&d, 0, sizeof d);
    const fake_kv_entry_t *e = fake_kv_find(&kv, "dosing");
    DSP_CHECK(e != NULL && dsp_codec_decode_dosing(e->value, e->len, &d) == DSP_CODEC_OK);
    return d.readiness;
}
static void put_time_meta(dsp_time_meta_t m)
{
    uint8_t b[DSP_TIME_RECORD_BYTES];
    DSP_CHECK_EQ_INT(DSP_TIME_RECORD_BYTES, dsp_codec_encode_time(&m, b, sizeof b));
    fake_kv_put_raw(&kv, "time", b, sizeof b);
}

/* ---- Fixtures ------------------------------------------------------------------------------------ */

static dsp_date_t D(int y, int m, int d)
{
    dsp_date_t x = {(int16_t)y, (uint8_t)m, (uint8_t)d};
    return x;
}
static dsp_date_t date_of(dsp_utc_t t)
{
    dsp_date_t d;
    memset(&d, 0, sizeof d);
    DSP_CHECK(dsp_utc_to_local_date(TZ, t, &d));
    return d;
}
static dsp_utc_t local(dsp_date_t d, int h, int m)
{
    dsp_utc_t t = 0;
    DSP_CHECK(dsp_local_to_utc(TZ, d, h, m, &t) != DSP_LOCAL_INVALID);
    return t;
}
static void occ_id(char *buf, size_t cap, const char *id, dsp_utc_t at)
{
    dsp_date_t d = date_of(at);
    snprintf(buf, cap, "%s@%04d-%02u-%02u", id, d.year, d.month, d.day);
}

static dsp_schedule_t sched(const char *id, int hour, int minute, uint32_t amount)
{
    dsp_schedule_t s;
    memset(&s, 0, sizeof s);
    snprintf(s.id, sizeof s.id, "%s", id);
    s.revision = 1;
    s.enabled = true;
    s.amount_ul = amount;
    s.start_date = D(2026, 11, 2);
    s.hour = (uint8_t)hour;
    s.minute = (uint8_t)minute;
    s.kind = DSP_REC_DAILY;
    s.interval = 1;
    snprintf(s.name, sizeof s.name, "%s", "Morning");
    return s;
}

static dsp_config_t calibration(void)
{
    dsp_config_t c;
    memset(&c, 0, sizeof c);
    c.reservoir_capacity_ul = CAP;
    c.low_stock_percent = 20;
    c.time_sync_interval_h = 24;
    c.notify_mask = DSP_NOTIFY_MASK_ALL;
    c.pump_flow_ul_per_s = 1200;
    c.calibrated_at = T0 - DAY;
    c.max_dose_ul = 20000;
    c.max_daily_ul = 60000;
    c.max_runtime_ms = 30000;
    c.min_interval_s = 60;
    return c;
}
#define CAL_MASK                                                                                  \
    (DSP_CONFIG_F_RESERVOIR_CAPACITY | DSP_CONFIG_F_PUMP_FLOW | DSP_CONFIG_F_CALIBRATED_AT |       \
     DSP_CONFIG_F_MAX_DOSE | DSP_CONFIG_F_MAX_DAILY | DSP_CONFIG_F_MAX_RUNTIME |                   \
     DSP_CONFIG_F_MIN_INTERVAL)

/* Fills the reservoir (REFILL is the only way to set the remaining estimate). */
static op_t op_refill(uint32_t remaining)
{
    op_t o = mk(OP_REFILL);
    o.capacity = CAP;
    o.remaining = remaining;
    return o;
}

/* Power-on boot, then one button session: phone time, calibration, refill, READY. */
static void provision(bool ready)
{
    world();
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    harness_claim_record(); /* M1.10c.1: enrolled phones for issued authorizations */
    DSP_CHECK(rep.slept);
    harness_claim_record(); /* M1.10c.1: four enrolled harness phones (slots 0..3) */
    clear_script();
    advance(g_true + 5);
    int t = step(g_true, op_now());
    int c = step(g_true, op_set_cfg_cur(CAL_MASK, calibration()));
    step(g_true, op_refill(CAP));
    if (ready) {
        step(g_true, op_ready());
    }
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[t].r.status);
    DSP_CHECK_EQ_INT(DSP_PHONE_TIME_ACCEPTED, sc.res[t].t.decision);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[c].r.status);
    DSP_CHECK_EQ_INT(ready ? DSP_READINESS_READY : DSP_READINESS_SERVICE_REQUIRED,
                     durable_readiness());
    DSP_CHECK(rep.slept);
    clear_script();
}

static void invariants(void)
{
    DSP_CHECK_EQ_INT(0, pump.violations);
    DSP_CHECK_EQ_INT(0, fail_safes);
}

/* ==== 1. Authorization fails closed, session and storage preconditions ========================= */

static void test_authorization_fails_closed(void)
{
    provision(true);
    advance(g_true + MINUTE);
    const dsp_app_auth_t *bad[] = {NULL, &NOBODY, &BAD_SLOT, &BAD_ROLE};
    dsp_schedule_t s = sched("auth-a", 9, 0, 1000);
    int first = sc.len;
    for (size_t k = 0; k < sizeof bad / sizeof bad[0]; k++) {
        step(g_true, as(op_now(), bad[k]));
        step(g_true, as(mk(OP_GET_CONFIG), bad[k]));
        step(g_true, as(op_set_cfg(1, DSP_CONFIG_F_LOW_STOCK, calibration()), bad[k]));
        step(g_true, as(op_list(0), bad[k]));
        step(g_true, as(op_get("auth-a"), bad[k]));
        step(g_true, as(op_create(s), bad[k]));
        step(g_true, as(op_update(1, s), bad[k]));
        step(g_true, as(op_delete("auth-a", 1), bad[k]));
        step(g_true, as(op_manual(1, 1000), bad[k])); /* M1.10b.2 */
        step(g_true, as(op_result(1), bad[k]));
        step(g_true, as(op_floor(), bad[k]));
    }
    int last = sc.len;
    int ok = step(g_true, as(op_create(s), &OWNER3)); /* slot 3 is a valid owner slot */
    dsp_config_t before = durable_cfg();
    DSP_CHECK(session());
    for (int i = first; i < last; i++) {
        DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[i].r.status);
        DSP_CHECK_EQ_INT(0, sc.res[i].writes);
    }
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[ok].r.status);
    dsp_config_t after = durable_cfg();
    DSP_CHECK(dsp_config_equal(&before, &after));
    /* Outside a service session nothing runs, not even for an owner. */
    dsp_config_t c;
    DSP_CHECK_EQ_INT(DSP_OP_BUSY, dsp_app_get_config(&app, &OWNER, &c, NULL).status);
    DSP_CHECK_EQ_INT(DSP_OP_BUSY, dsp_app_create_schedule(&app, &OWNER, &s, NULL).status);
    dsp_app_time_result_t tr;
    DSP_CHECK_EQ_INT(DSP_OP_BUSY,
                     dsp_app_set_phone_time(&app, &OWNER, (int64_t)g_true * 1000, 0, &tr).status);
    DSP_CHECK_EQ_INT(DSP_OP_INVALID, dsp_app_get_config(NULL, &OWNER, &c, NULL).status);
    invariants();
}

static void test_degraded_storage_refuses_operations(void)
{
    provision(true);
    /* A malformed config record: storage does not open, the session is read-only. */
    uint8_t junk[DSP_CONFIG_RECORD_BYTES];
    memset(junk, 0xEE, sizeof junk);
    junk[0] = DSP_REC_TYPE_CONFIG;
    junk[1] = 2;
    fake_kv_put_raw(&kv, "config", junk, sizeof junk);
    long sets = kv.sets;
    int a = step(g_true, mk(OP_GET_CONFIG));
    int b = step(g_true, op_create(sched("deg", 9, 0, 1000)));
    int c = step(g_true, op_now());
    int d = step(g_true, as(op_list(0), &NOBODY));
    DSP_CHECK(session());
    DSP_CHECK(rep.degraded);
    /* M1.10c.1: with the storage unreadable the credential record is not loaded either, so no
     * phone can be authenticated and no authorization exists: every operation is FORBIDDEN */
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[a].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[b].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[c].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[d].r.status);
    DSP_CHECK_EQ_INT(sets, kv.sets); /* nothing written */
    invariants();
}

/* ==== 2. Phone time ======================================================================== */

static void test_phone_time_establishes_trust_without_catch_up(void)
{
    /* Configured and READY with schedules at 07:55 and 08:20, then a power loss at 07:01:
     * the clock is untrusted and nothing may run. */
    provision(true);
    clear_script();
    int cr = step(g_true, op_create(sched("early", 7, 55, 1000)));
    int cl = step(g_true, op_create(sched("later", 8, 20, 1000)));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[cr].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[cl].r.status);
    advance(T0 - HOUR + MINUTE);
    reboot_power_on();
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    DSP_CHECK_EQ_INT(DSP_READINESS_READY, durable_readiness());
    /* 08:02: inside the due window of "early" (07:55). The phone supplies the time. */
    clear_script();
    advance(T0 + 2 * MINUTE); /* the raw clock still runs on the untrusted power-on base */
    DSP_CHECK_EQ_INT(0, pump.calls);
    int t = step(g_true, op_now());
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[t].r.status);
    DSP_CHECK_EQ_INT(DSP_PHONE_TIME_ACCEPTED, sc.res[t].t.decision);
    DSP_CHECK_EQ_INT(0, sc.res[t].t.offset_s); /* the previous clock was not trusted */
    dsp_time_meta_t m = durable_time();
    DSP_CHECK_EQ_INT(T0 + 2 * MINUTE, m.last_sync_utc);
    DSP_CHECK_EQ_INT(T0 + 2 * MINUTE + 1, m.not_before_utc); /* initial-sync floor */
    DSP_CHECK_EQ_INT(T0 + 2 * MINUTE, raw());               /* clock stepped */
    DSP_CHECK_EQ_INT(m.last_sync_utc, g_marker);         /* continuity marker refreshed */
    char early[96];
    occ_id(early, sizeof early, "early", local(date_of(T0), 7, 55));
    DSP_CHECK_EQ_INT(0, runs_of(early)); /* missed dose never caught up */
    /* The plan now targets "later" at 08:20 and it runs there. */
    DSP_CHECK_EQ_INT(DSP_WAKE_DOSE, rep.plan.reason);
    DSP_CHECK_EQ_INT(local(date_of(T0), 8, 20), rep.plan.wake_at);
    run_until(T0 + HOUR);
    char later[96];
    occ_id(later, sizeof later, "later", local(date_of(T0), 8, 20));
    DSP_CHECK_EQ_INT(1, runs_of(later));
    DSP_CHECK_EQ_INT(0, runs_of(early));
    /* A second phone sample while SYNCED is ignored (NTP preferred), nothing written. */
    clear_script();
    int t2 = step(g_true, op_time((int64_t)(g_true + 30) * 1000, 100));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_PHONE_TIME_IGNORED_NTP_PREFERRED, sc.res[t2].t.decision);
    DSP_CHECK_EQ_INT(0, sc.res[t2].writes);
    invariants();
}

static void test_phone_time_rejections_and_boundaries(void)
{
    world();
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    harness_claim_record(); /* M1.10c.1: enrolled phones for issued authorizations */
    clear_script();
    const int64_t min_ms = (int64_t)1735689600 * 1000; /* 2025-01-01T00:00:00.000Z */
    const int64_t max_ms = (int64_t)DSP_UTC_MAX * 1000;
    int r[8];
    r[0] = step(g_true, op_time((int64_t)g_true * 1000, 2001)); /* uncertainty */
    r[1] = step(g_true, op_time(min_ms - 1, 0));
    r[2] = step(g_true, op_time(max_ms + 1, 0));
    r[3] = step(g_true, op_time(INT64_MIN, 0));
    r[4] = step(g_true, op_time(INT64_MAX, 0));
    r[5] = step(g_true, op_time(-1, 0));
    r[6] = step(g_true, op_time((int64_t)g_true * 1000 + 999, 2000)); /* boundary: accepted */
    r[7] = step(g_true, op_time((int64_t)g_true * 1000, 0));          /* now SYNCED */
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_PHONE_TIME_REJECTED_UNCERTAIN, sc.res[r[0]].t.decision);
    for (int i = 1; i <= 5; i++) {
        DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[r[i]].r.status);
        DSP_CHECK_EQ_INT(DSP_PHONE_TIME_REJECTED_IMPLAUSIBLE, sc.res[r[i]].t.decision);
        DSP_CHECK_EQ_INT(0, sc.res[r[i]].writes);
    }
    DSP_CHECK_EQ_INT(0, sc.res[r[0]].writes);
    DSP_CHECK_EQ_INT(DSP_PHONE_TIME_ACCEPTED, sc.res[r[6]].t.decision);
    DSP_CHECK_EQ_INT(g_true, durable_time().last_sync_utc); /* milliseconds truncated */
    DSP_CHECK_EQ_INT(DSP_PHONE_TIME_IGNORED_NTP_PREFERRED, sc.res[r[7]].t.decision);

    /* Exactly the plausibility floor is accepted on an untrusted clock. */
    world();
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    harness_claim_record(); /* M1.10c.1: enrolled phones for issued authorizations */
    clear_script();
    int e = step(g_true, op_time(min_ms, 0));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_PHONE_TIME_ACCEPTED, sc.res[e].t.decision);
    DSP_CHECK_EQ_INT(1735689600, durable_time().last_sync_utc);
    invariants();
}

static void test_phone_time_holdover_rollback_and_hold(void)
{
    provision(true);
    /* A daily dose at 08:00 runs on day D. */
    clear_script();
    step(g_true, op_create(sched("roll", 8, 0, 1000)));
    DSP_CHECK(session());
    run_until(T0 + 30 * MINUTE);
    char occ[96];
    occ_id(occ, sizeof occ, "roll", T0);
    DSP_CHECK_EQ_INT(1, runs_of(occ));
    /* D+1: the 08:00 dose runs; at 09:00 (26 h after the last sync: HOLDOVER, phone time
     * accepted again) the phone's clock is 2 h behind. The step back (within 7 days) is
     * applied, and the already claimed occurrence of D+1 does not run again at the device's
     * second 08:00. */
    run_until(T0 + DAY + 2 * HOUR);
    char occ2[96];
    occ_id(occ2, sizeof occ2, "roll", T0 + DAY);
    DSP_CHECK_EQ_INT(1, runs_of(occ2));
    clear_script();
    int t = step(g_true, op_time((int64_t)(g_true - 2 * HOUR) * 1000, 0));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_PHONE_TIME_ACCEPTED, sc.res[t].t.decision);
    DSP_CHECK_EQ_INT(-2 * HOUR, sc.res[t].t.offset_s);
    DSP_CHECK_EQ_INT(-2 * HOUR, raw() - g_true); /* device clock now 2 h behind true time */
    run_until(T0 + DAY + 5 * HOUR);
    DSP_CHECK_EQ_INT(1, runs_of(occ2));
    DSP_CHECK_EQ_INT(1, runs_of(occ));

    /* More than 7 days behind the high water: refused (HOLD), nothing applied. Needs
     * HOLDOVER: 25 h later. */
    advance(g_true + 25 * HOUR);
    DSP_CHECK(wake(DSP_CAUSE_TIMER));
    dsp_time_meta_t before = durable_time();
    int64_t off = g_offset;
    clear_script();
    int h = step(g_true, op_time((int64_t)(raw() - 8 * DAY) * 1000, 0));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_PHONE_TIME_HOLD, sc.res[h].t.decision);
    DSP_CHECK_EQ_INT(0, sc.res[h].writes);
    dsp_time_meta_t after = durable_time();
    DSP_CHECK(dsp_time_meta_equal(&before, &after));
    DSP_CHECK_EQ_INT(off, g_offset);

    /* A recorded rollback hold (from an NTP correction): phone time never lifts it, even
     * with a time past the hold, and nothing is applied. */
    dsp_time_meta_t held = durable_time();
    held.hold_until_utc = raw() + 2 * DAY;
    put_time_meta(held);
    reboot_power_on(); /* untrusted clock: the hold cannot be proven over */
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    clear_script();
    int h2 = step(g_true, op_time((int64_t)(g_true + 3 * DAY) * 1000, 0));
    int h3 = step(g_true, op_now());
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_PHONE_TIME_HOLD, sc.res[h2].t.decision);
    DSP_CHECK_EQ_INT(DSP_PHONE_TIME_HOLD, sc.res[h3].t.decision);
    DSP_CHECK_EQ_INT(held.hold_until_utc, durable_time().hold_until_utc);
    DSP_CHECK_EQ_INT(0, g_marker);
    invariants();
}

static void test_phone_time_forward_step_skips_jumped_occurrences(void)
{
    provision(true);
    clear_script();
    step(g_true, op_create(sched("fwd", 9, 0, 1000)));
    DSP_CHECK(session());
    /* The device clock lags 30 min (drift); 26 h after the sync (HOLDOVER) at true 09:05 the
     * device believes 08:35. The phone moves it forward past 09:00: jumped over, missed. */
    advance(local(date_of(T0 + DAY), 8, 0));
    DSP_CHECK(wake(DSP_CAUSE_TIMER));
    advance(local(date_of(T0 + DAY), 9, 5));
    g_offset -= 30 * MINUTE;
    DSP_CHECK(wake(DSP_CAUSE_TIMER)); /* device time 08:35: nothing due */
    char occ[96];
    occ_id(occ, sizeof occ, "fwd", T0 + DAY);
    DSP_CHECK_EQ_INT(0, runs_of(occ));
    clear_script();
    int t = step(g_true, op_now());
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_PHONE_TIME_ACCEPTED, sc.res[t].t.decision);
    DSP_CHECK_EQ_INT(30 * MINUTE, sc.res[t].t.offset_s);
    DSP_CHECK_EQ_INT(g_true + 1 >= durable_time().not_before_utc, 1);
    DSP_CHECK(durable_time().not_before_utc > local(date_of(T0 + DAY), 9, 0));
    run_until(g_true + HOUR);
    DSP_CHECK_EQ_INT(0, runs_of(occ)); /* inside the window, but jumped over: never run */
    invariants();
}

static void test_phone_time_persist_failure_and_power_cuts(void)
{
    /* Write failure: nothing applied, dosing ends for the wake. */
    world();
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    harness_claim_record(); /* M1.10c.1: enrolled phones for issued authorizations */
    clear_script();
    strcpy(fail_set_key, "time");
    int64_t off = g_offset;
    int t = step(g_true, op_now());
    DSP_CHECK(session());
    fail_set_key[0] = '\0';
    DSP_CHECK_EQ_INT(DSP_OP_STORAGE, sc.res[t].r.status);
    DSP_CHECK_EQ_INT(0, durable_time().last_sync_utc);
    DSP_CHECK_EQ_INT(off, g_offset); /* clock not stepped */
    DSP_CHECK_EQ_INT(0, g_marker);
    DSP_CHECK(rep.op_storage_fault);

    /* Clock that cannot be set: metadata durable, trust UNKNOWN, no marker. */
    world();
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    harness_claim_record(); /* M1.10c.1: enrolled phones for issued authorizations */
    clear_script();
    clock_set_fails = true;
    int u = step(g_true, op_now());
    DSP_CHECK(session());
    clock_set_fails = false;
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[u].r.status);
    DSP_CHECK_EQ_INT(DSP_PHONE_TIME_ACCEPTED, sc.res[u].t.decision);
    DSP_CHECK_EQ_INT(0, g_marker);

    /* Power cut at every write of the operation, landed or not. */
    for (int landed = 0; landed < 2; landed++) {
        for (long k = 1; k < 8; k++) {
            world();
            DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
            clear_script();
            step_armed(g_true, op_now());
            cut_at = k;
            cut_after = landed != 0;
            bool done_ok = session();
            cut_at = 0;
            if (done_ok) {
                break; /* fewer writes than k */
            }
            reboot_power_on();
            DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
            DSP_CHECK(rep.slept && !rep.degraded);
            dsp_time_meta_t m = durable_time();
            DSP_CHECK(m.last_sync_utc == 0 || m.not_before_utc == m.last_sync_utc + 1);
            if (k == 1) {
                DSP_CHECK_EQ_INT(landed != 0, m.last_sync_utc != 0); /* the time record write */
            }
            /* The cut wake never stepped the clock: the device is untrusted again. */
            DSP_CHECK_EQ_INT(0, g_marker);
        }
    }
    invariants();
}

/* ==== 3. Configuration ====================================================================== */

static void test_config_read_and_update_rules(void)
{
    provision(true);
    dsp_config_t base = durable_cfg();
    clear_script();
    int g = step(g_true, mk(OP_GET_CONFIG));
    /* Non-safety fields while READY; read-only fields in the input are ignored. */
    dsp_config_t v = calibration();
    v.low_stock_percent = 35;
    v.time_sync_interval_h = 48;
    v.notify_mask = DSP_NOTIFY_LOW_RESERVOIR;
    v.reservoir_remaining_ul = 1; /* read-only: ignored */
    v.revision = 999;             /* read-only: ignored */
    v.pump_flow_ul_per_s = 1;     /* not selected: ignored */
    int ok = step(g_true, op_set_cfg(base.revision,
                                     DSP_CONFIG_F_LOW_STOCK | DSP_CONFIG_F_SYNC_INTERVAL |
                                         DSP_CONFIG_F_NOTIFY_MASK,
                                     v));
    /* Stale revision now (it advanced). */
    int stale = step(g_true, op_set_cfg(base.revision, DSP_CONFIG_F_LOW_STOCK, v));
    /* Calibration and limits refused while READY, one bit at a time. */
    int refused[6];
    const uint16_t svc[6] = {DSP_CONFIG_F_PUMP_FLOW,   DSP_CONFIG_F_CALIBRATED_AT,
                             DSP_CONFIG_F_MAX_DOSE,    DSP_CONFIG_F_MAX_DAILY,
                             DSP_CONFIG_F_MAX_RUNTIME, DSP_CONFIG_F_MIN_INTERVAL};
    for (int i = 0; i < 6; i++) {
        refused[i] = step(g_true, op_set_cfg_cur(svc[i] | DSP_CONFIG_F_LOW_STOCK, calibration()));
    }
    /* Capacity while READY: changes allowed, never to 0; never below the estimate. */
    dsp_config_t cap = calibration();
    cap.reservoir_capacity_ul = CAP + 5000;
    int cap_ok = step(g_true, op_set_cfg_cur(DSP_CONFIG_F_RESERVOIR_CAPACITY, cap));
    cap.reservoir_capacity_ul = 0;
    int cap_zero = step(g_true, op_set_cfg_cur(DSP_CONFIG_F_RESERVOIR_CAPACITY, cap));
    cap.reservoir_capacity_ul = CAP - 1; /* remaining is CAP */
    int cap_low = step(g_true, op_set_cfg_cur(DSP_CONFIG_F_RESERVOIR_CAPACITY, cap));
    /* Field mask: none, or bits beyond the protocol's. */
    int m0 = step(g_true, op_set_cfg_cur(0, calibration()));
    int m10 = step(g_true, op_set_cfg_cur(1u << 10, calibration()));
    int m15 = step(g_true, op_set_cfg_cur((uint16_t)(DSP_CONFIG_F_LOW_STOCK | 0x8000u), calibration()));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[g].r.status);
    DSP_CHECK(dsp_config_equal(&base, &sc.res[g].cfg));
    DSP_CHECK_EQ_INT(HW_MAX_MS, sc.res[g].hw);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[ok].r.status);
    DSP_CHECK_EQ_INT(base.revision + 1, sc.res[ok].rev);
    DSP_CHECK_EQ_INT(DSP_OP_CONFLICT, sc.res[stale].r.status);
    DSP_CHECK_EQ_INT(base.revision + 1, sc.res[stale].r.detail);
    DSP_CHECK_EQ_INT(0, sc.res[stale].writes);
    for (int i = 0; i < 6; i++) {
        DSP_CHECK_EQ_INT(DSP_OP_REFUSED, sc.res[refused[i]].r.status);
        DSP_CHECK_EQ_INT(DSP_OP_R_READY, sc.res[refused[i]].r.detail);
        DSP_CHECK_EQ_INT(svc[i], sc.res[refused[i]].r.aux);
        DSP_CHECK_EQ_INT(0, sc.res[refused[i]].writes);
    }
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[cap_ok].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_REFUSED, sc.res[cap_zero].r.status);
    DSP_CHECK_EQ_INT(DSP_CONFIG_F_RESERVOIR_CAPACITY, sc.res[cap_zero].r.aux);
    DSP_CHECK_EQ_INT(DSP_OP_INVALID, sc.res[cap_low].r.status);
    DSP_CHECK_EQ_INT(DSP_CONFIG_E_REMAINING, sc.res[cap_low].r.detail);
    DSP_CHECK_EQ_INT(DSP_OP_INVALID, sc.res[m0].r.status);
    DSP_CHECK_EQ_INT(DSP_CONFIG_E_FIELD_MASK, sc.res[m0].r.detail);
    DSP_CHECK_EQ_INT(DSP_CONFIG_E_FIELD_MASK, sc.res[m10].r.detail);
    DSP_CHECK_EQ_INT(DSP_CONFIG_E_FIELD_MASK, sc.res[m15].r.detail);
    dsp_config_t now = durable_cfg();
    DSP_CHECK_EQ_INT(base.revision + 2, now.revision);
    DSP_CHECK_EQ_INT(35, now.low_stock_percent);
    DSP_CHECK_EQ_INT(48, now.time_sync_interval_h);
    DSP_CHECK_EQ_INT(DSP_NOTIFY_LOW_RESERVOIR, now.notify_mask);
    DSP_CHECK_EQ_INT(CAP + 5000, now.reservoir_capacity_ul);
    DSP_CHECK_EQ_INT(base.reservoir_remaining_ul, now.reservoir_remaining_ul);
    DSP_CHECK_EQ_INT(base.pump_flow_ul_per_s, now.pump_flow_ul_per_s);
    DSP_CHECK_EQ_INT(base.calibrated_at, now.calibrated_at);
    DSP_CHECK_EQ_INT(base.max_dose_ul, now.max_dose_ul);
    DSP_CHECK_EQ_INT(base.min_interval_s, now.min_interval_s);
    DSP_CHECK_EQ_INT(DSP_READINESS_READY, durable_readiness());
    DSP_CHECK_EQ_INT(0, pump.calls);
    /* The longer sync interval is effective in this wake's timekeeper (and persisted). */
    DSP_CHECK_EQ_INT(48 * 3600, app.tk.interval_s);
    invariants();
}

static void test_config_service_required_never_enables_dosing(void)
{
    provision(false);
    clear_script();
    dsp_config_t v = calibration();
    v.pump_flow_ul_per_s = 2000;
    v.max_dose_ul = 30000;
    v.max_daily_ul = 90000;
    int a = step(g_true, op_set_cfg_cur(CAL_MASK, v));
    int s = step(g_true, op_create(sched("svc", 8, 10, 1000)));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[a].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[s].r.status);
    DSP_CHECK_EQ_INT(2000, durable_cfg().pump_flow_ul_per_s);
    DSP_CHECK_EQ_INT(DSP_READINESS_SERVICE_REQUIRED, durable_readiness());
    run_until(T0 + 2 * DAY);
    DSP_CHECK_EQ_INT(0, pump.calls);
    invariants();
}

static void test_config_field_boundaries(void)
{
    provision(false);
    bulk_limits(); /* more writes in one session than the M1.10b.3 defaults allow */
    typedef struct {
        uint16_t mask;
        dsp_config_t v;
        dsp_app_op_status_t want;
        uint32_t detail;
    } case_t;
    case_t cases[32];
    int n = 0;
#define CASE(m, field, value, st, det)                                                            \
    do {                                                                                          \
        cases[n].mask = (m);                                                                      \
        cases[n].v = calibration();                                                               \
        cases[n].v.field = (value);                                                               \
        cases[n].want = (st);                                                                     \
        cases[n].detail = (det);                                                                  \
        n++;                                                                                      \
    } while (0)
    CASE(DSP_CONFIG_F_LOW_STOCK, low_stock_percent, 0, DSP_OP_INVALID, DSP_CONFIG_E_LOW_STOCK);
    CASE(DSP_CONFIG_F_LOW_STOCK, low_stock_percent, 1, DSP_OP_OK, 0);
    CASE(DSP_CONFIG_F_LOW_STOCK, low_stock_percent, 99, DSP_OP_OK, 0);
    CASE(DSP_CONFIG_F_LOW_STOCK, low_stock_percent, 100, DSP_OP_INVALID, DSP_CONFIG_E_LOW_STOCK);
    CASE(DSP_CONFIG_F_SYNC_INTERVAL, time_sync_interval_h, 23, DSP_OP_INVALID, DSP_CONFIG_E_SYNC_INTERVAL);
    CASE(DSP_CONFIG_F_SYNC_INTERVAL, time_sync_interval_h, 24, DSP_OP_OK, 0);
    CASE(DSP_CONFIG_F_SYNC_INTERVAL, time_sync_interval_h, 48, DSP_OP_OK, 0);
    CASE(DSP_CONFIG_F_SYNC_INTERVAL, time_sync_interval_h, 49, DSP_OP_INVALID, DSP_CONFIG_E_SYNC_INTERVAL);
    CASE(DSP_CONFIG_F_NOTIFY_MASK, notify_mask, 0x08, DSP_OP_INVALID, DSP_CONFIG_E_NOTIFY_MASK);
    CASE(DSP_CONFIG_F_NOTIFY_MASK, notify_mask, 0, DSP_OP_OK, 0);
    CASE(DSP_CONFIG_F_CALIBRATED_AT, calibrated_at, (int64_t)1735689599, DSP_OP_INVALID,
         DSP_CONFIG_E_CALIBRATED_AT);
    CASE(DSP_CONFIG_F_CALIBRATED_AT, calibrated_at, (int64_t)1735689600, DSP_OP_OK, 0);
    CASE(DSP_CONFIG_F_CALIBRATED_AT, calibrated_at, DSP_UTC_MAX + 1, DSP_OP_INVALID,
         DSP_CONFIG_E_CALIBRATED_AT);
    CASE(DSP_CONFIG_F_CALIBRATED_AT, calibrated_at, -1, DSP_OP_INVALID, DSP_CONFIG_E_CALIBRATED_AT);
    CASE(DSP_CONFIG_F_CALIBRATED_AT, calibrated_at, 0, DSP_OP_OK, 0); /* "not calibrated" */
    CASE(DSP_CONFIG_F_MAX_RUNTIME, max_runtime_ms, HW_MAX_MS, DSP_OP_OK, 0);
    CASE(DSP_CONFIG_F_MAX_RUNTIME, max_runtime_ms, HW_MAX_MS + 1, DSP_OP_INVALID, DSP_CONFIG_E_RUNTIME_HW);
    CASE(DSP_CONFIG_F_MAX_DAILY, max_daily_ul, 19999, DSP_OP_INVALID, DSP_CONFIG_E_LIMITS);
    CASE(DSP_CONFIG_F_MAX_DAILY, max_daily_ul, 20000, DSP_OP_OK, 0);
    CASE(DSP_CONFIG_F_MAX_DOSE, max_dose_ul, UINT32_MAX, DSP_OP_INVALID, DSP_CONFIG_E_LIMITS);
    CASE(DSP_CONFIG_F_MAX_DOSE | DSP_CONFIG_F_MAX_DAILY, max_dose_ul, 60000, DSP_OP_OK, 0);
    CASE(DSP_CONFIG_F_PUMP_FLOW, pump_flow_ul_per_s, UINT32_MAX, DSP_OP_OK, 0);
    CASE(DSP_CONFIG_F_PUMP_FLOW, pump_flow_ul_per_s, 0, DSP_OP_OK, 0); /* "not calibrated" */
    CASE(DSP_CONFIG_F_MIN_INTERVAL, min_interval_s, UINT32_MAX, DSP_OP_OK, 0);
    CASE(DSP_CONFIG_F_RESERVOIR_CAPACITY, reservoir_capacity_ul, UINT32_MAX, DSP_OP_OK, 0);
#undef CASE
    clear_script();
    int idx[32];
    for (int i = 0; i < n; i++) {
        idx[i] = step(g_true, op_set_cfg_cur(cases[i].mask, cases[i].v));
    }
    DSP_CHECK(session());
    for (int i = 0; i < n; i++) {
        if (sc.res[idx[i]].r.status != cases[i].want || sc.res[idx[i]].r.detail != cases[i].detail) {
            fprintf(stderr, "config case %d: status %d detail 0x%x\n", i, sc.res[idx[i]].r.status,
                    (unsigned)sc.res[idx[i]].r.detail);
        }
        DSP_CHECK_EQ_INT(cases[i].want, sc.res[idx[i]].r.status);
        DSP_CHECK_EQ_INT(cases[i].detail, sc.res[idx[i]].r.detail);
        DSP_CHECK(cases[i].want == DSP_OP_OK ? sc.res[idx[i]].writes >= 1 : sc.res[idx[i]].writes == 0);
    }
    DSP_CHECK_EQ_INT(0, pump.calls);
    invariants();
}

static void test_config_storage_failure_and_power_cuts(void)
{
    /* A failed config write ends dosing for the wake; the next wake doses again. */
    provision(true);
    clear_script();
    step(g_true, op_create(sched("cfgfail", 7, 30, 1000)));
    DSP_CHECK(session());
    advance(local(date_of(T0), 7, 29));
    clear_script();
    strcpy(fail_set_key, "config");
    dsp_config_t v = calibration();
    v.low_stock_percent = 30;
    int f = step(g_true, op_set_cfg_cur(DSP_CONFIG_F_LOW_STOCK, v));
    step(g_true + 2 * MINUTE, op_list(0)); /* keep the session open across 07:30 */
    DSP_CHECK(session());
    fail_set_key[0] = '\0';
    DSP_CHECK_EQ_INT(DSP_OP_STORAGE, sc.res[f].r.status);
    DSP_CHECK(rep.op_storage_fault);
    char occ[96];
    occ_id(occ, sizeof occ, "cfgfail", local(date_of(T0), 7, 30));
    DSP_CHECK_EQ_INT(0, runs_of(occ)); /* no dosing after the storage failure */
    DSP_CHECK_EQ_INT(DSP_WAKE_DOSE, rep.plan.reason); /* still planned */
    sleep_and_wake(); /* within the due window: dosed by the next wake */
    DSP_CHECK_EQ_INT(1, runs_of(occ));

    /* Power cut at the config write: old or new record, both valid; never READY changes. */
    for (int landed = 0; landed < 2; landed++) {
        provision(false);
        dsp_config_t before = durable_cfg();
        clear_script();
        dsp_config_t w = calibration();
        w.max_dose_ul = 15000;
        step_armed(g_true, op_set_cfg_cur(DSP_CONFIG_F_MAX_DOSE, w));
        cut_at = 1;
        cut_after = landed != 0;
        DSP_CHECK(!session());
        cut_at = 0;
        reboot_power_on();
        DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
        DSP_CHECK(!rep.degraded);
        dsp_config_t c = durable_cfg();
        DSP_CHECK_EQ_INT(landed ? before.revision + 1 : before.revision, c.revision);
        DSP_CHECK_EQ_INT(landed ? 15000 : before.max_dose_ul, c.max_dose_ul);
        DSP_CHECK_EQ_INT(DSP_READINESS_SERVICE_REQUIRED, durable_readiness());
    }
    invariants();
}

/* A refill interrupted at boot (recovery could not complete it in this wake) must be
 * completed before the configuration changes: the roll-forward rewrites the record. */
static void test_config_refused_while_refill_pending(void)
{
    provision(false);
    const fake_kv_entry_t *e = fake_kv_find(&kv, "dosing");
    dsp_dosing_state_t d;
    DSP_CHECK(e != NULL && dsp_codec_decode_dosing(e->value, e->len, &d) == DSP_CODEC_OK);
    d.flags |= DSP_DOSING_F_REFILL_PENDING;
    d.refill_capacity_ul = CAP;
    d.refill_remaining_ul = CAP / 2;
    d.refill_seq = d.next_seq;
    d.next_seq++;
    uint8_t b[DSP_DOSING_RECORD_BYTES];
    DSP_CHECK_EQ_INT(DSP_DOSING_RECORD_BYTES, dsp_codec_encode_dosing(&d, b, sizeof b));
    fake_kv_put_raw(&kv, "dosing", b, sizeof b);
    strcpy(fail_set_key, "config"); /* the roll-forward stops at its estimate write */
    clear_script();
    dsp_config_t v = calibration();
    v.low_stock_percent = 40;
    int r = step(g_true, op_set_cfg_cur(DSP_CONFIG_F_LOW_STOCK, v));
    DSP_CHECK(session());
    fail_set_key[0] = '\0';
    DSP_CHECK_EQ_INT(DSP_OP_REFUSED, sc.res[r].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_R_REFILL_PENDING, sc.res[r].r.detail);
    DSP_CHECK_EQ_INT(0, sc.res[r].writes);
    /* The next wake completes the refill; then the change is accepted. */
    clear_script();
    int r2 = step(g_true, op_set_cfg_cur(DSP_CONFIG_F_LOW_STOCK, v));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[r2].r.status);
    DSP_CHECK_EQ_INT(CAP / 2, durable_cfg().reservoir_remaining_ul);
    DSP_CHECK_EQ_INT(40, durable_cfg().low_stock_percent);
    invariants();
}

/* ==== D16: the edit floor and trusted time ======================================================
 * Invariant: an edit never makes an already elapsed occurrence executable. */

static dsp_schedule_t disabled(const char *id, int h, int m)
{
    dsp_schedule_t x = sched(id, h, m, 1000);
    x.enabled = false;
    return x;
}

static void test_d16_untrusted_clock_refuses_floor_edits(void)
{
    /* UNKNOWN after a power loss (a high water exists). */
    provision(true);
    clear_script();
    step(g_true, op_create(disabled("u-off", 7, 30)));
    step(g_true, op_create(sched("u-on", 7, 45, 1000)));
    DSP_CHECK(session());
    advance(local(date_of(T0), 7, 40));
    reboot_power_on();
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    dsp_schedule_set_t before = durable_set();
    clear_script();
    dsp_schedule_t reen = disabled("u-off", 7, 30);
    reen.enabled = true;
    int r1 = step(g_true, op_update_cur(reen));                       /* re-enable */
    (void)step(g_true, op_update_cur(sched("u-on", 7, 35, 1000))); /* timing edit */
    int r3 = step(g_true, op_create(sched("u-new", 7, 20, 1000)));     /* create */
    dsp_schedule_t nm = sched("u-on", 7, 45, 1500);
    snprintf(nm.name, sizeof nm.name, "%s", "Renamed");
    int ok1 = step(g_true, op_update_cur(nm));                         /* name + volume */
    dsp_schedule_t dis = sched("u-on", 7, 45, 1500);
    dis.enabled = false;
    snprintf(dis.name, sizeof dis.name, "%s", "Renamed");
    (void)step(g_true, op_update_cur(dis));                            /* disable */
    int ok3 = step(g_true, op_delete_cur("u-off"));                    /* delete */
    DSP_CHECK(session());
    for (int i = r1; i <= r3; i++) {
        DSP_CHECK_EQ_INT(DSP_OP_REFUSED, sc.res[i].r.status);
        DSP_CHECK_EQ_INT(DSP_OP_R_CLOCK, sc.res[i].r.detail);
        DSP_CHECK_EQ_INT(0, sc.res[i].writes);
    }
    for (int i = ok1; i <= ok3; i++) {
        DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[i].r.status);
    }
    dsp_schedule_set_t after = durable_set();
    DSP_CHECK_EQ_INT(find_in(&before, "u-on")->not_before, find_in(&after, "u-on")->not_before);
    DSP_CHECK(find_in(&after, "u-off") == NULL && find_in(&after, "u-new") == NULL);
    DSP_CHECK_EQ_INT(0, pump.calls);

    /* UNKNOWN because a continuous clock reads behind the last sync: it regains trust later
     * WITHOUT a sync, so an edit made now must not be possible. */
    provision(true);
    clear_script();
    step(g_true, op_create(disabled("g", 7, 30)));
    DSP_CHECK(session());
    advance(local(date_of(T0), 7, 40));
    g_offset -= 50 * MINUTE; /* device reads 06:50, behind the 07:00 sync; marker intact */
    clear_script();
    dsp_schedule_t g = disabled("g", 7, 30);
    g.enabled = true;
    int rg = step(g_true, op_update_cur(g));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_TRUST_UNKNOWN, app.tk.trust);
    DSP_CHECK_EQ_INT(DSP_OP_REFUSED, sc.res[rg].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_R_CLOCK, sc.res[rg].r.detail);
    advance(g_true + 45 * MINUTE); /* device 07:35: trusted again, no sync happened */
    DSP_CHECK(wake(DSP_CAUSE_TIMER));
    DSP_CHECK(app.tk.trust != DSP_TRUST_UNKNOWN);
    DSP_CHECK_EQ_INT(0, pump.calls);
    invariants();
}

static void test_d16_floor_in_trusted_states(void)
{
    /* SYNCED: floor = now + 1. */
    provision(true);
    advance(T0 + 3 * MINUTE);
    clear_script();
    int c = step(g_true, op_create(sched("s", 8, 0, 1000))); /* 08:00 elapsed 3 min ago */
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[c].r.status);
    DSP_CHECK_EQ_INT(DSP_TRUST_SYNCED, app.tk.trust);
    dsp_schedule_set_t set = durable_set();
    DSP_CHECK_EQ_INT(T0 + 3 * MINUTE + 1, find_in(&set, "s")->not_before);
    run_until(T0 + HOUR);
    DSP_CHECK_EQ_INT(0, pump.calls);

    /* HOLDOVER (26 h after the sync): floor = now + 1; elapsed occurrence never runs, the
     * floor is never lowered by a later edit. */
    run_until(T0 + DAY + 2 * HOUR); /* D+1 10:00; "s" ran at 08:00 */
    DSP_CHECK_EQ_INT(DSP_TRUST_HOLDOVER, app.tk.trust);
    DSP_CHECK_EQ_INT(1, pump.calls);
    clear_script();
    int u = step(g_true, op_update_cur(sched("s", 9, 55, 1000))); /* 5 min ago, same date */
    int c2 = step(g_true, op_create(sched("h", 9, 58, 1000)));      /* 2 min ago */
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[u].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[c2].r.status);
    set = durable_set();
    DSP_CHECK_EQ_INT(g_true + 1, find_in(&set, "h")->not_before);
    dsp_utc_t floor_s = find_in(&set, "s")->not_before;
    DSP_CHECK(floor_s > local(date_of(T0 + DAY), 9, 55));
    run_until(T0 + DAY + 4 * HOUR);
    DSP_CHECK_EQ_INT(1, pump.calls);

    /* A backward clock step (2 h, accepted in HOLDOVER): elapsed relative to the corrected
     * clock stays blocked by the floor; a date already claimed stays claimed (watermark). */
    run_until(T0 + 2 * DAY + 3 * HOUR); /* D+2 11:00; "s" ran at 09:55, "h" at 09:58 */
    DSP_CHECK_EQ_INT(3, pump.calls);
    run_until(T0 + 3 * DAY + 2 * HOUR); /* D+3 10:00, still HOLDOVER */
    DSP_CHECK_EQ_INT(5, pump.calls);
    DSP_CHECK_EQ_INT(DSP_TRUST_HOLDOVER, app.tk.trust);
    clear_script();
    int t = step(g_true, op_time((int64_t)(g_true - 2 * HOUR) * 1000, 0)); /* device 08:00 */
    int u2 = step(g_true, op_update_cur(sched("s", 8, 30, 1000)));   /* future, but claimed date */
    int c3 = step(g_true, op_create(sched("b1", 7, 50, 1000)));      /* elapsed: floor */
    /* future and after the last pump start (09:58 before the step: a clock behind the last
     * start waits, M1.9a), so it runs */
    int c4 = step(g_true, op_create(sched("b2", 10, 5, 1000)));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_PHONE_TIME_ACCEPTED, sc.res[t].t.decision);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[u2].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[c3].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[c4].r.status);
    dsp_utc_t dev = raw();
    run_until(g_true + 2 * HOUR + 15 * MINUTE);
    char id[96];
    occ_id(id, sizeof id, "s", dev);
    DSP_CHECK_EQ_INT(1, runs_of(id)); /* ran at 09:55 before the step, not again at 08:30 */
    occ_id(id, sizeof id, "b1", dev);
    DSP_CHECK_EQ_INT(0, runs_of(id));
    occ_id(id, sizeof id, "b2", dev);
    DSP_CHECK_EQ_INT(1, runs_of(id));
    invariants();
}

static void test_d16_floor_under_rollback_hold(void)
{
    provision(true);
    advance(T0 + HOUR);
    DSP_CHECK(wake(DSP_CAUSE_TIMER));
    /* A rollback hold from an earlier NTP correction: trusted clock, high water 9 days ahead,
     * dosing held for 2 more days. */
    dsp_time_meta_t m = durable_time();
    DSP_CHECK(m.last_sync_utc != 0 && g_marker == m.last_sync_utc);
    m.high_water_utc = raw() + 9 * DAY;
    m.hold_until_utc = raw() + 2 * DAY;
    put_time_meta(m);
    clear_script();
    int c = step(g_true, op_create(sched("held", 12, 0, 1000)));
    dsp_schedule_t x = disabled("held2", 12, 0);
    int c2 = step(g_true, op_create(x));
    x.enabled = true;
    x.revision = 2;
    int u = step(g_true, op_update(1, x));
    DSP_CHECK(session());
    DSP_CHECK(dsp_timekeeper_hold_active(&app.tk, raw()));
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[c].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[c2].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[u].r.status);
    dsp_schedule_set_t set = durable_set();
    DSP_CHECK_EQ_INT(m.high_water_utc + 1, find_in(&set, "held")->not_before);
    DSP_CHECK_EQ_INT(m.high_water_utc + 1, find_in(&set, "held2")->not_before);
    /* After the hold has ended (clock past hold_until, still before the high water) a timing
     * edit never lowers the floor. */
    run_until(g_true + 2 * DAY + HOUR);
    DSP_CHECK(!dsp_timekeeper_hold_active(&app.tk, raw()));
    DSP_CHECK(raw() < m.high_water_utc);
    clear_script();
    int u2 = step(g_true, op_update_cur(sched("held", 12, 30, 1000)));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[u2].r.status);
    set = durable_set();
    DSP_CHECK_EQ_INT(m.high_water_utc + 1, find_in(&set, "held")->not_before);
    DSP_CHECK_EQ_INT(0, pump.calls);
    /* Nothing up to the high water runs; the first dose is the one after it. */
    run_until(m.high_water_utc + DAY);
    DSP_CHECK(pump.calls >= 1);
    for (int i = 0; i < pump.calls; i++) {
        DSP_CHECK(pump.at[i] + (raw() - g_true) > m.high_water_utc);
    }
    invariants();
}

static void test_d16_same_id_recreated_after_tombstone_eviction(void)
{
    provision(true);
    bulk_limits(); /* more writes in one session than the M1.10b.3 defaults allow */
    clear_script();
    step(g_true, op_create(disabled("same", 8, 0))); /* never executed: not in the history */
    DSP_CHECK(session());
    advance(T0 + 3 * MINUTE);
    clear_script();
    step(g_true, op_delete_cur("same"));
    int dup = step(g_true, op_create(sched("same", 8, 0, 1000)));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_DUPLICATE, sc.res[dup].r.status);
    DSP_CHECK_EQ_INT(1, sc.res[dup].r.aux);
    /* 16 more deletions push "same" out of the tombstone ring: the repository accepts the ID
     * again (non-reuse then rests on UUIDs), and the floor still blocks today's 08:00. */
    for (int round = 0; round < 2; round++) {
        clear_script();
        for (int i = 0; i < 8; i++) {
            char id[16];
            snprintf(id, sizeof id, "t-%d-%d", round, i);
            step(g_true, op_create(disabled(id, 9, 0)));
            step(g_true, op_delete_cur(id));
        }
        DSP_CHECK(session());
    }
    clear_script();
    int re = step(g_true, op_create(sched("same", 8, 0, 1000)));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[re].r.status);
    run_until(T0 + HOUR);
    DSP_CHECK_EQ_INT(0, pump.calls);
    run_until(T0 + DAY + HOUR);
    char id[96];
    occ_id(id, sizeof id, "same", T0 + DAY);
    DSP_CHECK_EQ_INT(1, runs_of(id));
    invariants();
}

/* Every configuration field at its representable extreme, through real doses, limits,
 * reservoir accounting and planning (UBSan checks every operation). No physical limit is
 * invented: these values are accepted and the arithmetic must stay exact. */
static void test_config_extreme_values_are_overflow_safe(void)
{
    provision(false);
    clear_script();
    dsp_config_t v = calibration();
    v.reservoir_capacity_ul = UINT32_MAX;
    v.pump_flow_ul_per_s = UINT32_MAX;
    v.max_dose_ul = UINT32_MAX;
    v.max_daily_ul = UINT32_MAX;
    v.max_runtime_ms = HW_MAX_MS;
    v.min_interval_s = UINT32_MAX;
    v.low_stock_percent = 99;
    v.calibrated_at = DSP_UTC_MAX;
    int a = step(g_true, op_set_cfg_cur(CAL_MASK | DSP_CONFIG_F_LOW_STOCK, v));
    op_t refill = op_refill(UINT32_MAX);
    refill.capacity = UINT32_MAX;
    step(g_true, refill);
    step(g_true, op_ready());
    int c1 = step(g_true, op_create(sched("max1", 8, 0, UINT32_MAX)));
    int c2 = step(g_true, op_create(sched("max2", 9, 0, UINT32_MAX)));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[a].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[c1].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[c2].r.status);
    DSP_CHECK_EQ_INT(DSP_READINESS_READY, durable_readiness());
    /* 4,294,967,295 µl at 4,294,967,295 µl/s = exactly 1,000 ms: dosed once. The second dose
     * is refused (interval of 2^32 - 1 s, daily total beyond 32 bits, estimate exhausted). */
    run_until(T0 + 3 * DAY);
    DSP_CHECK_EQ_INT(1, pump.calls);
    DSP_CHECK_EQ_INT(0, durable_cfg().reservoir_remaining_ul);

    /* Flow 1 µl/s: the runtime of a large dose does not fit 32 bits and is refused (LIMIT),
     * never truncated. */
    provision(false);
    clear_script();
    dsp_config_t w = calibration();
    w.pump_flow_ul_per_s = 1;
    w.max_dose_ul = UINT32_MAX;
    w.max_daily_ul = UINT32_MAX;
    int b = step(g_true, op_set_cfg_cur(CAL_MASK, w));
    step(g_true, op_ready());
    int c3 = step(g_true, op_create(sched("slow", 8, 0, 20000)));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[b].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[c3].r.status);
    run_until(T0 + DAY);
    DSP_CHECK_EQ_INT(0, pump.calls); /* 20,000 s runtime > the 30 s limit */
    DSP_CHECK(dsp_dosing_runtime_ms(UINT32_MAX, 1, &(uint32_t){0}) == false);
    invariants();
}

/* ==== 4. Schedules ========================================================================== */

static void test_schedule_crud_across_restarts(void)
{
    provision(true);
    clear_script();
    dsp_schedule_t a = sched("uuid-a", 9, 0, 1500);
    dsp_schedule_t b = sched("uuid-b", 21, 30, 2500);
    b.kind = DSP_REC_WEEKLY;
    b.weekday_mask = 0x41;
    b.has_end_date = true;
    b.end_date = D(2027, 6, 30);
    snprintf(b.name, sizeof b.name, "%s", "Ve\xC4\x8D" "er \xF0\x9F\x8C\x99"); /* "Večer 🌙" */
    int ca = step(g_true, op_create(a));
    int cb = step(g_true, op_create(b));
    int l0 = step(g_true, op_list(0));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[ca].r.status);
    DSP_CHECK_EQ_INT(1, sc.res[ca].set_rev);
    DSP_CHECK_EQ_INT(2, sc.res[cb].set_rev);
    DSP_CHECK_EQ_INT(2, sc.res[l0].page.total);
    DSP_CHECK_EQ_INT(2, sc.res[l0].page.n);
    DSP_CHECK_EQ_STR("uuid-a", sc.res[l0].page.items[0].id);
    DSP_CHECK_EQ_INT(1, sc.res[l0].page.items[1].revision);
    /* Restart (power loss): everything is durable, names included. */
    reboot_power_on();
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    clear_script();
    step(g_true, op_now());
    int gb = step(g_true, op_get("uuid-b"));
    dsp_schedule_t b2 = b;
    b2.revision = 2;
    b2.amount_ul = 3000;
    snprintf(b2.name, sizeof b2.name, "%s", "Night");
    int ub = step(g_true, op_update(1, b2));
    int da = step(g_true, op_delete("uuid-a", 1));
    int ga = step(g_true, op_get("uuid-a"));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[gb].r.status);
    DSP_CHECK_EQ_STR("Ve\xC4\x8D" "er \xF0\x9F\x8C\x99", sc.res[gb].s.name);
    DSP_CHECK_EQ_INT(0x41, sc.res[gb].s.weekday_mask);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[ub].r.status);
    DSP_CHECK_EQ_INT(2, sc.res[ub].rev);
    DSP_CHECK_EQ_INT(3, sc.res[ub].set_rev);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[da].r.status);
    DSP_CHECK_EQ_INT(4, sc.res[da].set_rev);
    DSP_CHECK_EQ_INT(DSP_OP_NOT_FOUND, sc.res[ga].r.status);
    /* Another restart: the edit and the deletion are durable; the ID is a tombstone. */
    reboot_power_on();
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    dsp_schedule_set_t set = durable_set();
    DSP_CHECK_EQ_INT(1, set.count);
    DSP_CHECK_EQ_STR("Night", set.schedules[0].name);
    DSP_CHECK_EQ_INT(3000, set.schedules[0].amount_ul);
    DSP_CHECK_EQ_INT(1, set.tombstone_count);
    DSP_CHECK_EQ_STR("uuid-a", set.tombstones[0]);
    const fake_kv_entry_t *e = fake_kv_find(&kv, "sched");
    DSP_CHECK(e != NULL && e->value[1] == DSP_SCHEDULE_SET_FORMAT_V2);
    invariants();
}

static void test_schedule_validation_and_revisions(void)
{
    provision(true);
    clear_script();
    int n = 0;
    int idx[40];
    uint32_t want_bits[40];
    dsp_app_op_status_t want[40];
#define BAD(mutation, st, bits)                                                                   \
    do {                                                                                          \
        dsp_schedule_t x = sched("v-x", 10, 0, 1000);                                             \
        mutation;                                                                                 \
        idx[n] = step(g_true, op_create(x));                                                      \
        want[n] = (st);                                                                           \
        want_bits[n] = (bits);                                                                    \
        n++;                                                                                      \
    } while (0)
    BAD(x.hour = 24, DSP_OP_INVALID, DSP_SCHED_E_TIME);
    BAD(x.minute = 60, DSP_OP_INVALID, DSP_SCHED_E_TIME);
    BAD((x.hour = 23, x.minute = 59, x.id[2] = 'y'), DSP_OP_OK, 0);
    BAD(x.amount_ul = 0, DSP_OP_INVALID, DSP_SCHED_E_AMOUNT_NOT_POSITIVE);
    BAD(x.amount_ul = 20001, DSP_OP_LIMIT, DSP_DOSING_L_DOSE); /* above max_dose_ul */
    BAD((x.amount_ul = 20000, x.id[2] = 'z'), DSP_OP_OK, 0);
    BAD(x.amount_ul = UINT32_MAX, DSP_OP_LIMIT, DSP_DOSING_L_DOSE);
    BAD((x.kind = DSP_REC_WEEKLY, x.weekday_mask = 0), DSP_OP_INVALID, DSP_SCHED_E_NO_WEEKDAYS);
    BAD((x.kind = DSP_REC_WEEKLY, x.weekday_mask = 0x80), DSP_OP_INVALID,
        DSP_SCHED_E_NO_WEEKDAYS | DSP_SCHED_E_WEEKDAYS);
    BAD((x.kind = DSP_REC_WEEKLY, x.weekday_mask = 0xFF), DSP_OP_INVALID, DSP_SCHED_E_WEEKDAYS);
    BAD(x.weekday_mask = 0x01, DSP_OP_INVALID, DSP_SCHED_E_WEEKDAYS); /* on DAILY */
    BAD(x.kind = 3, DSP_OP_INVALID, DSP_SCHED_E_KIND);
    BAD(x.interval = 0, DSP_OP_INVALID, DSP_SCHED_E_INTERVAL_TOO_SMALL);
    BAD(x.revision = 2, DSP_OP_INVALID, DSP_SCHED_E_REVISION);
    BAD(x.revision = 0, DSP_OP_INVALID, DSP_SCHED_E_REVISION);
    BAD(x.id[0] = '@', DSP_OP_INVALID, DSP_SCHED_E_ID);
    BAD((x.has_end_date = true, x.end_date = D(2026, 11, 1)), DSP_OP_INVALID, DSP_SCHED_E_END_BEFORE_START);
    BAD(x.start_date = D(2026, 2, 30), DSP_OP_INVALID, DSP_SCHED_E_START_DATE);
    /* ONCE at a nonexistent local time (spring forward 2027-03-28 02:30 in Prague). */
    BAD((x.kind = DSP_REC_ONCE, x.start_date = D(2027, 3, 28), x.hour = 2, x.minute = 30),
        DSP_OP_INVALID, DSP_SCHED_E_TIME_NONEXISTENT);
    /* Names: 32 bytes ok, multi-byte up to 32 bytes ok, no NUL in 33 bytes, malformed UTF-8,
     * control characters. */
    BAD((memset(x.name, 'n', 32), x.name[32] = '\0', x.id[2] = '1'), DSP_OP_OK, 0);
    BAD((strcpy(x.name, "\xC4\x8D\xC4\x8D\xC4\x8D\xC4\x8D\xC4\x8D\xC4\x8D\xC4\x8D\xC4\x8D"
                        "\xC4\x8D\xC4\x8D\xC4\x8D\xC4\x8D\xC4\x8D\xC4\x8D\xC4\x8D\xC4\x8D"),
         x.id[2] = '2'),
        DSP_OP_OK, 0);
    BAD(memset(x.name, 'n', sizeof x.name), DSP_OP_INVALID, DSP_SCHED_E_NAME);
    BAD(strcpy(x.name, "\xC0\x80"), DSP_OP_INVALID, DSP_SCHED_E_NAME);         /* overlong NUL */
    BAD(strcpy(x.name, "\xED\xA0\x80"), DSP_OP_INVALID, DSP_SCHED_E_NAME);     /* surrogate */
    BAD(strcpy(x.name, "\xF4\x90\x80\x80"), DSP_OP_INVALID, DSP_SCHED_E_NAME); /* > U+10FFFF */
    BAD(strcpy(x.name, "abc\xC3"), DSP_OP_INVALID, DSP_SCHED_E_NAME);          /* truncated */
    BAD(strcpy(x.name, "a\x01"), DSP_OP_INVALID, DSP_SCHED_E_NAME);
    BAD(strcpy(x.name, "a\x7F"), DSP_OP_INVALID, DSP_SCHED_E_NAME);
    BAD((strcpy(x.name, ""), x.id[2] = '3'), DSP_OP_OK, 0);
    BAD(x.not_before = -5, DSP_OP_OK, 0); /* an input floor is ignored: the device sets its own */
#undef BAD
    DSP_CHECK(session());
    for (int i = 0; i < n; i++) {
        if (sc.res[idx[i]].r.status != want[i] || sc.res[idx[i]].r.detail != want_bits[i]) {
            fprintf(stderr, "schedule case %d: status %d detail 0x%x\n", i, sc.res[idx[i]].r.status,
                    (unsigned)sc.res[idx[i]].r.detail);
        }
        DSP_CHECK_EQ_INT(want[i], sc.res[idx[i]].r.status);
        DSP_CHECK_EQ_INT(want_bits[i], sc.res[idx[i]].r.detail);
        DSP_CHECK_EQ_INT(want[i] == DSP_OP_OK ? 1 : 0, sc.res[idx[i]].writes);
    }
    dsp_schedule_set_t set = durable_set();
    const dsp_schedule_t *f = find_in(&set, "v-x");
    DSP_CHECK(f != NULL && f->not_before == g_true + 1);

    /* Revisions. */
    clear_script();
    dsp_schedule_t r = sched("rev", 10, 0, 1000);
    step(g_true, op_create(r));
    r.revision = 2;
    r.amount_ul = 1100;
    int u1 = step(g_true, op_update(1, r));
    int u_stale = step(g_true, op_update(1, r));      /* stored is 2 */
    r.revision = 4;
    int u_badrec = step(g_true, op_update(2, r));     /* record must be expected + 1 */
    int d_stale = step(g_true, op_delete("rev", 1));
    int d_missing = step(g_true, op_delete("nope", 1));
    int u_missing = step(g_true, op_update(1, sched("nope", 1, 0, 1)));
    int dup = step(g_true, op_create(sched("rev", 11, 0, 1000)));
    int d_ok = step(g_true, op_delete("rev", 2));
    int reuse = step(g_true, op_create(sched("rev", 11, 0, 1000)));
    int g_bad = step(g_true, op_get("bad id"));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[u1].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_CONFLICT, sc.res[u_stale].r.status);
    DSP_CHECK_EQ_INT(2, sc.res[u_stale].r.detail);
    DSP_CHECK_EQ_INT(DSP_OP_INVALID, sc.res[u_badrec].r.status);
    DSP_CHECK_EQ_INT(DSP_SCHED_E_REVISION, sc.res[u_badrec].r.detail);
    DSP_CHECK_EQ_INT(DSP_OP_CONFLICT, sc.res[d_stale].r.status);
    DSP_CHECK_EQ_INT(2, sc.res[d_stale].r.detail);
    DSP_CHECK_EQ_INT(DSP_OP_NOT_FOUND, sc.res[d_missing].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_NOT_FOUND, sc.res[u_missing].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_DUPLICATE, sc.res[dup].r.status);
    DSP_CHECK_EQ_INT(0, sc.res[dup].r.aux);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[d_ok].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_DUPLICATE, sc.res[reuse].r.status);
    DSP_CHECK_EQ_INT(1, sc.res[reuse].r.aux);
    DSP_CHECK_EQ_INT(DSP_OP_INVALID, sc.res[g_bad].r.status);
    for (int i = u_stale; i <= u_missing; i++) {
        DSP_CHECK_EQ_INT(0, sc.res[i].writes);
    }
    invariants();
}

static void test_schedule_capacity_and_paging(void)
{
    provision(true);
    bulk_limits(); /* more writes in one session than the M1.10b.3 defaults allow */
    clear_script();
    int empty = step(g_true, op_list(0));
    int empty1 = step(g_true, op_list(1));
    int c[17];
    for (int i = 0; i < 17; i++) {
        char id[16];
        snprintf(id, sizeof id, "cap-%02d", i);
        c[i] = step(g_true, op_create(sched(id, 10, i, 100)));
    }
    int p[5];
    for (int i = 0; i < 5; i++) {
        p[i] = step(g_true, op_list((uint8_t)i));
    }
    int p255 = step(g_true, op_list(255));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[empty].r.status);
    DSP_CHECK_EQ_INT(0, sc.res[empty].page.n);
    DSP_CHECK_EQ_INT(DSP_OP_INVALID, sc.res[empty1].r.status);
    for (int i = 0; i < 16; i++) {
        DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[c[i]].r.status);
    }
    DSP_CHECK_EQ_INT(DSP_OP_CAPACITY, sc.res[c[16]].r.status);
    DSP_CHECK_EQ_INT(DSP_SCHEDULE_CAPACITY, sc.res[c[16]].r.aux);
    DSP_CHECK_EQ_INT(0, sc.res[c[16]].writes);
    for (int i = 0; i < 4; i++) {
        DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[p[i]].r.status);
        DSP_CHECK_EQ_INT(4, sc.res[p[i]].page.n);
        DSP_CHECK_EQ_INT(16, sc.res[p[i]].page.total);
        DSP_CHECK_EQ_INT(16, sc.res[p[i]].page.set_revision);
        char id[16];
        snprintf(id, sizeof id, "cap-%02d", 4 * i + 3);
        DSP_CHECK_EQ_STR(id, sc.res[p[i]].page.items[3].id);
    }
    DSP_CHECK_EQ_INT(DSP_OP_INVALID, sc.res[p[4]].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_INVALID, sc.res[p255].r.status);
    invariants();
}

/* At-most-once after edits: a claimed occurrence is never resurrected. */
static void test_schedule_edit_never_resurrects_claimed_occurrence(void)
{
    provision(true);
    clear_script();
    step(g_true, op_create(sched("once-a-day", 8, 0, 1000)));
    DSP_CHECK(session());
    run_until(T0 + 5 * MINUTE);
    char occ[96];
    occ_id(occ, sizeof occ, "once-a-day", T0);
    DSP_CHECK_EQ_INT(1, runs_of(occ));
    /* 08:10: move it to 08:30, then later to 08:40, change the volume, disable/enable. */
    advance(T0 + 10 * MINUTE);
    clear_script();
    dsp_schedule_t s = sched("once-a-day", 8, 30, 1200);
    int u = step(g_true, op_update_cur(s));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[u].r.status);
    run_until(T0 + 45 * MINUTE);
    DSP_CHECK_EQ_INT(1, runs_of(occ));
    clear_script();
    s.hour = 8;
    s.minute = 50;
    step(g_true, op_update_cur(s));
    s.enabled = false;
    step(g_true, op_update_cur(s));
    s.enabled = true;
    step(g_true, op_update_cur(s));
    DSP_CHECK(session());
    run_until(T0 + 2 * HOUR);
    DSP_CHECK_EQ_INT(1, runs_of(occ)); /* still once on that date */
    /* The next day it runs at the new time. */
    run_until(T0 + DAY + 2 * HOUR);
    char occ2[96];
    occ_id(occ2, sizeof occ2, "once-a-day", T0 + DAY);
    DSP_CHECK_EQ_INT(1, runs_of(occ2));
    DSP_CHECK_EQ_INT(local(date_of(T0 + DAY), 8, 50), pump.at[pump.calls - 1]);
    invariants();
}

/* Edits near the due window never create a catch-up dose. */
static void test_schedule_edits_near_due_window(void)
{
    provision(true);
    clear_script();
    dsp_schedule_t late = sched("late", 10, 0, 1000);
    dsp_schedule_t off = sched("off", 8, 0, 1000);
    off.enabled = false;
    step(g_true, op_create(late));
    step(g_true, op_create(off));
    DSP_CHECK(session());
    /* 08:03: move "late" to 08:00 (in the window of now) and re-enable "off" (08:00): both
     * would be inside their due window, both are before the edit floor: neither runs.
     * Create "fresh" for 08:01 (also already past) and "soon" for 08:05 (future: runs). */
    advance(T0 + 3 * MINUTE);
    clear_script();
    late.hour = 8;
    int u1 = step(g_true, op_update_cur(late));
    off.enabled = true;
    int u2 = step(g_true, op_update_cur(off));
    int c1 = step(g_true, op_create(sched("fresh", 8, 1, 1000)));
    int c2 = step(g_true, op_create(sched("soon", 8, 5, 1000)));
    step(T0 + 12 * MINUTE, op_list(0)); /* the session stays open past 08:05 */
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[u1].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[u2].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[c1].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[c2].r.status);
    dsp_schedule_set_t set = durable_set();
    DSP_CHECK_EQ_INT(T0 + 3 * MINUTE + 1, find_in(&set, "late")->not_before);
    DSP_CHECK_EQ_INT(T0 + 3 * MINUTE + 1, find_in(&set, "off")->not_before);
    char id[96];
    occ_id(id, sizeof id, "soon", T0);
    DSP_CHECK_EQ_INT(1, runs_of(id)); /* dosed inside the session, BLE suspended around it */
    DSP_CHECK(sc.suspends >= 1 && sc.resumes >= 1);
    run_until(T0 + HOUR);
    const char *skipped[] = {"late", "off", "fresh"};
    for (int i = 0; i < 3; i++) {
        occ_id(id, sizeof id, skipped[i], T0);
        DSP_CHECK_EQ_INT(0, runs_of(id));
    }
    /* A name-only or volume-only edit keeps the floor (no new floor is needed). */
    clear_script();
    late.name[0] = 'L';
    late.amount_ul = 1300;
    step(g_true, op_update_cur(late));
    DSP_CHECK(session());
    set = durable_set();
    DSP_CHECK_EQ_INT(T0 + 3 * MINUTE + 1, find_in(&set, "late")->not_before);
    /* The next day every one of them runs once. */
    run_until(T0 + DAY + HOUR);
    for (int i = 0; i < 3; i++) {
        occ_id(id, sizeof id, skipped[i], T0 + DAY);
        DSP_CHECK_EQ_INT(1, runs_of(id));
    }
    invariants();
}

/* Delete + recreate under a new ID cannot repeat today's dose. */
static void test_schedule_delete_recreate_same_time(void)
{
    provision(true);
    clear_script();
    step(g_true, op_create(sched("orig", 8, 0, 1000)));
    DSP_CHECK(session());
    run_until(T0 + MINUTE);
    char id[96];
    occ_id(id, sizeof id, "orig", T0);
    DSP_CHECK_EQ_INT(1, runs_of(id));
    advance(T0 + 2 * MINUTE);
    clear_script();
    int d = step(g_true, op_delete_cur("orig"));
    int c = step(g_true, op_create(sched("copy", 8, 0, 1000)));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[d].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[c].r.status);
    run_until(T0 + HOUR);
    occ_id(id, sizeof id, "copy", T0);
    DSP_CHECK_EQ_INT(0, runs_of(id));
    DSP_CHECK_EQ_INT(1, pump.calls);
    /* Deletion keeps the execution history: the old occurrence record still exists. */
    side_open();
    dsp_history_store_t hs;
    dsp_storage_history_store(&side, &hs);
    dsp_history_scan_t scan;
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_history_scan(&hs, "orig", INT32_MIN, &scan));
    DSP_CHECK(scan.found);
    run_until(T0 + DAY + HOUR);
    occ_id(id, sizeof id, "copy", T0 + DAY);
    DSP_CHECK_EQ_INT(1, runs_of(id));
    invariants();
}

/* Prague DST: fall back (2027-10-31 03:00 CEST -> 02:00 CET) and spring forward
 * (2027-03-28 02:00 CET -> 03:00 CEST). */
static void test_schedule_edits_across_dst(void)
{
    provision(true);
    /* Move the world to 2027-10-30 22:00 local with a trusted clock (phone, HOLDOVER). */
    clear_script();
    step(g_true, op_create(sched("dst", 2, 30, 1000)));
    DSP_CHECK(session());
    dsp_date_t fb = D(2027, 10, 31);
    advance(local(D(2027, 10, 30), 22, 0));
    g_marker = 0; /* months later; time re-established by the phone */
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    clear_script();
    step(g_true, op_now());
    DSP_CHECK(session());
    /* 02:30 occurs twice on 2027-10-31: one occurrence, at the earlier (CEST) instant. */
    dsp_utc_t first = local(fb, 2, 30);
    run_until(first + 5 * MINUTE);
    char id[96];
    occ_id(id, sizeof id, "dst", first);
    DSP_CHECK_EQ_INT(1, runs_of(id));
    /* Second pass of 02:10 (CET, = first + 40 min): edit to 02:40. Its earlier instant (CEST
     * 02:40 = first + 10 min) is before the edit floor; the CET 02:40 is the same local
     * date's already-claimed occurrence: nothing more runs that day. */
    advance(first + 40 * MINUTE);
    clear_script();
    dsp_schedule_t s = sched("dst", 2, 40, 1000);
    int u = step(g_true, op_update_cur(s));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[u].r.status);
    run_until(first + 3 * HOUR);
    DSP_CHECK_EQ_INT(1, runs_of(id));
    /* The next day it runs once at 02:40 (CET). */
    run_until(local(D(2027, 11, 1), 4, 0));
    occ_id(id, sizeof id, "dst", local(D(2027, 11, 1), 2, 40));
    DSP_CHECK_EQ_INT(1, runs_of(id));
    invariants();

    /* Spring forward: a DAILY schedule edited to 02:30 is skipped on 2027-03-28 and runs at
     * 02:30 the day after. */
    provision(true);
    clear_script();
    step(g_true, op_create(sched("spring", 6, 0, 1000)));
    DSP_CHECK(session());
    advance(local(D(2027, 3, 27), 12, 0));
    g_marker = 0;
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    clear_script();
    step(g_true, op_now());
    dsp_schedule_t sp = sched("spring", 2, 30, 1000);
    int us = step(g_true, op_update_cur(sp));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[us].r.status);
    run_until(local(D(2027, 3, 29), 4, 0));
    occ_id(id, sizeof id, "spring", local(D(2027, 3, 29), 2, 30));
    DSP_CHECK_EQ_INT(1, runs_of(id));
    int on_28 = 0;
    for (int i = 0; i < pump.calls; i++) {
        on_28 += strstr(pump.ids[i], "spring@2027-03-28") != NULL;
    }
    DSP_CHECK_EQ_INT(0, on_28);
    invariants();
}

static void test_schedule_plan_follows_changes(void)
{
    provision(true);
    advance(T0 + 2 * HOUR); /* 10:00 local */
    DSP_CHECK(wake(DSP_CAUSE_TIMER));
    DSP_CHECK(rep.plan.reason != DSP_WAKE_DOSE);
    clear_script();
    int c = step(g_true, op_create(sched("plan", 15, 0, 1000)));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[c].r.status);
    DSP_CHECK_EQ_INT(DSP_WAKE_DOSE, rep.plan.reason);
    DSP_CHECK_EQ_INT(local(date_of(T0), 15, 0), rep.plan.wake_at);
    clear_script();
    step(g_true, op_update_cur(sched("plan", 12, 0, 1000)));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(local(date_of(T0), 12, 0), rep.plan.wake_at);
    clear_script();
    step(g_true, op_delete_cur("plan"));
    DSP_CHECK(session());
    DSP_CHECK(rep.plan.reason != DSP_WAKE_DOSE);
    invariants();
}

static void test_schedule_storage_failure_and_power_cuts(void)
{
    /* Write failure: STORAGE, repository re-read (old set), no dosing in this wake, but the
     * plan still has the durable schedules. */
    provision(true);
    clear_script();
    step(g_true, op_create(sched("keep", 9, 0, 1000)));
    DSP_CHECK(session());
    clear_script();
    strcpy(fail_set_key, "sched");
    int f = step(g_true, op_create(sched("lost", 11, 0, 1000)));
    int l = step(g_true, op_list(0));
    DSP_CHECK(session());
    fail_set_key[0] = '\0';
    DSP_CHECK_EQ_INT(DSP_OP_STORAGE, sc.res[f].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[l].r.status);
    DSP_CHECK_EQ_INT(1, sc.res[l].page.total);
    DSP_CHECK_EQ_INT(DSP_WAKE_DOSE, rep.plan.reason);
    DSP_CHECK_EQ_INT(local(date_of(T0), 9, 0), rep.plan.wake_at);

    /* Power cuts in create, timing update and delete, write landed or not. Afterwards the
     * set is the old or the new one, opens cleanly, and nothing runs twice or as catch-up. */
    for (int kind = 0; kind < 4; kind++) {
        for (int landed = 0; landed < 2; landed++) {
            provision(true);
            clear_script();
            step(g_true, op_create(sched("pc", 7, 30, 1000)));
            DSP_CHECK(session());
            run_until(local(date_of(T0), 7, 31));
            char id[96];
            occ_id(id, sizeof id, "pc", local(date_of(T0), 7, 30));
            DSP_CHECK_EQ_INT(1, runs_of(id));
            dsp_schedule_set_t before = durable_set();
            advance(local(date_of(T0), 7, 33));
            clear_script();
            if (kind == 0) {
                step_armed(g_true, op_create(sched("pc2", 7, 31, 1000))); /* already past */
            } else if (kind == 1) {
                step_armed(g_true, op_update_cur(sched("pc", 7, 32, 1000)));
            } else if (kind == 2) {
                step_armed(g_true, op_delete_cur("pc"));
            } else {
                dsp_schedule_t off = sched("pc", 7, 30, 1000);
                off.enabled = false;
                step(g_true, op_update_cur(off));                      /* disable: no floor */
                step_armed(g_true, op_update_cur(sched("pc", 7, 30, 1000))); /* re-enable */
            }
            cut_at = 1;
            cut_after = landed != 0;
            DSP_CHECK(!session());
            cut_at = 0;
            reboot_power_on();
            DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
            DSP_CHECK(!rep.degraded);
            clear_script();
            step(g_true, op_now());
            DSP_CHECK(session());
            dsp_schedule_set_t after = durable_set();
            DSP_CHECK_EQ_INT(before.revision + (kind == 3) + (landed ? 1 : 0), after.revision);
            if (landed && kind != 2) { /* the edit floor survived the power loss */
                const dsp_schedule_t *x = find_in(&after, kind == 0 ? "pc2" : "pc");
                DSP_CHECK(x != NULL && x->not_before == local(date_of(T0), 7, 33) + 1);
            }
            run_until(T0 + HOUR);
            DSP_CHECK_EQ_INT(1, runs_of(id));
            occ_id(id, sizeof id, "pc2", local(date_of(T0), 7, 31));
            DSP_CHECK_EQ_INT(0, runs_of(id));
            DSP_CHECK_EQ_INT(1, pump.calls);
        }
    }
    invariants();
}

/* A device with a format 1 schedule set (M1.3 .. M1.10a) keeps working; the first write
 * upgrades the record to format 2 without changing any schedule. */
static void test_schedule_set_format1_upgrade(void)
{
    provision(true);
    clear_script();
    step(g_true, op_create(sched("legacy", 9, 0, 1000)));
    DSP_CHECK(session());
    /* Rewrite "sched" in format 1 (what M1.10a stored), without name and floor. */
    const fake_kv_entry_t *e = fake_kv_find(&kv, "sched");
    DSP_CHECK(e != NULL && e->len == 8 + 132);
    uint8_t f1[8 + 91];
    memcpy(f1, e->value, sizeof f1);
    f1[1] = 1;
    fake_kv_put_raw(&kv, "sched", f1, sizeof f1);
    reboot_power_on();
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    DSP_CHECK(!rep.degraded);
    clear_script();
    step(g_true, op_now());
    int g = step(g_true, op_get("legacy"));
    dsp_schedule_t n = sched("legacy", 9, 0, 1000);
    snprintf(n.name, sizeof n.name, "%s", "Upgraded");
    int u = step(g_true, op_update_cur(n)); /* name only: the floor stays 0 */
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[g].r.status);
    DSP_CHECK_EQ_STR("", sc.res[g].s.name);
    DSP_CHECK_EQ_INT(0, sc.res[g].s.not_before);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[u].r.status);
    e = fake_kv_find(&kv, "sched");
    DSP_CHECK(e != NULL && e->value[1] == DSP_SCHEDULE_SET_FORMAT_V2 && e->len == 8 + 132);
    dsp_schedule_set_t set = durable_set();
    DSP_CHECK_EQ_STR("Upgraded", set.schedules[0].name);
    DSP_CHECK_EQ_INT(0, set.schedules[0].not_before);
    run_until(T0 + DAY + 3 * HOUR);
    char id[96];
    occ_id(id, sizeof id, "legacy", T0 + DAY);
    DSP_CHECK_EQ_INT(1, runs_of(id));
    invariants();
}

#ifndef DSP_APP_OPS_NO_MAIN
int main(void)
{
    DSP_RUN(test_authorization_fails_closed);
    DSP_RUN(test_degraded_storage_refuses_operations);
    DSP_RUN(test_phone_time_establishes_trust_without_catch_up);
    DSP_RUN(test_phone_time_rejections_and_boundaries);
    DSP_RUN(test_phone_time_holdover_rollback_and_hold);
    DSP_RUN(test_phone_time_forward_step_skips_jumped_occurrences);
    DSP_RUN(test_phone_time_persist_failure_and_power_cuts);
    DSP_RUN(test_config_read_and_update_rules);
    DSP_RUN(test_config_service_required_never_enables_dosing);
    DSP_RUN(test_config_field_boundaries);
    DSP_RUN(test_config_storage_failure_and_power_cuts);
    DSP_RUN(test_config_refused_while_refill_pending);
    DSP_RUN(test_schedule_crud_across_restarts);
    DSP_RUN(test_schedule_validation_and_revisions);
    DSP_RUN(test_schedule_capacity_and_paging);
    DSP_RUN(test_schedule_edit_never_resurrects_claimed_occurrence);
    DSP_RUN(test_schedule_edits_near_due_window);
    DSP_RUN(test_schedule_delete_recreate_same_time);
    DSP_RUN(test_schedule_edits_across_dst);
    DSP_RUN(test_schedule_plan_follows_changes);
    DSP_RUN(test_schedule_storage_failure_and_power_cuts);
    DSP_RUN(test_schedule_set_format1_upgrade);
    DSP_RUN(test_d16_untrusted_clock_refuses_floor_edits);
    DSP_RUN(test_d16_floor_in_trusted_states);
    DSP_RUN(test_d16_floor_under_rollback_hold);
    DSP_RUN(test_d16_same_id_recreated_after_tombstone_eviction);
    DSP_RUN(test_config_extreme_values_are_overflow_safe);
    return dsp_test_finish();
}
#endif /* DSP_APP_OPS_NO_MAIN */
