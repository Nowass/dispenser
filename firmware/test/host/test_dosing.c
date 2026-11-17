/*
 * Dosing Service tests (M1.9a): the sole pump owner over the real M1.2 coordinator, M1.3
 * storage (fake KV with power cuts and a recording proxy), M1.5 reservoir/notifications and
 * the M1.9a dosing record. The fake pump verifies, at the moment it is switched on, that
 * every required piece of state is DURABLE (claim or manual marker with the claimed volume,
 * ledger and last start, the reservoir consumption) and counts runs per occurrence.
 */
#include "dsp_codec.h"
#include "dsp_dosing.h"
#include "dsp_reservoir.h"
#include "dsp_storage.h"
#include "dsp_test.h"
#include "fake_kv.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const dsp_tz_t *const TZ = &DSP_TZ_EUROPE_PRAGUE;

#define DAY 86400
#define T0 ((dsp_utc_t)1795158000) /* 2026-11-20T07:00:00Z = 08:00 CET */
#define HW_MAX_MS 60000u
#define CAP 100000u

/* ---- Storage behind a recording proxy -------------------------------------------------- */

static fake_kv_t kv;
static dsp_kv_t proxy;
#define KEYLOG 256
static char keylog[KEYLOG][DSP_KV_KEY_MAX_LEN + 1];
static int keylog_len;
static uint32_t cfg_prev_remaining; /* durable remaining before the last config write */
static uint32_t cfg_new_remaining;  /* remaining written by the last config write */
static int cfg_write_index;         /* keylog index of the last config write */
static bool fail_notify_sets;       /* the "notify" record cannot be written (power stays on) */

static dsp_kv_status_t px_get(void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *len)
{
    (void)ctx;
    return kv.iface.get(kv.iface.ctx, key, buf, cap, len);
}

static dsp_kv_status_t px_set(void *ctx, const char *key, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (strcmp(key, "config") == 0) {
        dsp_config_t old;
        dsp_config_t nw;
        const fake_kv_entry_t *e = fake_kv_find(&kv, "config");
        if (e != NULL && dsp_codec_decode_config(e->value, e->len, &old) == DSP_CODEC_OK &&
            dsp_codec_decode_config(data, len, &nw) == DSP_CODEC_OK) {
            cfg_prev_remaining = old.reservoir_remaining_ul;
            cfg_new_remaining = nw.reservoir_remaining_ul;
            cfg_write_index = keylog_len;
        }
    }
    if (keylog_len < KEYLOG) {
        snprintf(keylog[keylog_len], sizeof keylog[0], "%s", key);
    }
    keylog_len++;
    if (fail_notify_sets && strcmp(key, "notify") == 0) {
        return DSP_KV_ERR_WRITE;
    }
    return kv.iface.set(kv.iface.ctx, key, data, len);
}

static dsp_kv_status_t px_commit(void *ctx)
{
    (void)ctx;
    return kv.iface.commit(kv.iface.ctx);
}

/* ---- Clock --------------------------------------------------------------------------- */

static dsp_utc_t g_now = T0;
static bool g_trusted = true;

static dsp_clock_t clk(void)
{
    dsp_clock_t c = {g_now, g_trusted, 0};
    return c;
}

/* M1.10b.2: every manual dose goes through the anti-replay request API (slot 0, a fresh
 * request ID each time, so every call is evaluated exactly once). */
static uint64_t g_mseq;
static dsp_dosing_result_t manual_dose(dsp_dosing_t *dd, uint32_t volume, dsp_clock_t c)
{
    return dsp_dosing_manual_request(dd, 0, ++g_mseq, volume, c, 0).dose;
}


static dsp_notify_clock_t nclk(void)
{
    dsp_notify_clock_t c = {g_now, g_trusted};
    return c;
}

static dsp_clock_t clock_read(void *ctx)
{
    (void)ctx;
    return clk();
}
static const dsp_dosing_clock_t CLOCK = {NULL, clock_read};

/* ---- Fake pump (verifies durable state at activation) ------------------------------------ */

#define MAX_RUNS 4096
static struct {
    dsp_pump_result_t script[16];
    int script_len;
    int calls;
    int force_offs;
    uint32_t last_runtime;
    uint32_t last_volume;
    bool advance_clock; /* the run takes its runtime of device time */
    int violations;
    char ids[MAX_RUNS][80]; /* occurrence/manual identity of every run */
    uint32_t volumes[MAX_RUNS];
    dsp_utc_t starts[MAX_RUNS];
    bool forbid; /* recovery phase: any run is a violation */
} pump;

static void pump_reset(void)
{
    memset(&pump, 0, sizeof pump);
    pump.advance_clock = true;
}

static bool durable_dosing(dsp_dosing_state_t *out)
{
    const fake_kv_entry_t *e = fake_kv_find(&kv, "dosing");
    return e != NULL && dsp_codec_decode_dosing(e->value, e->len, out) == DSP_CODEC_OK;
}

static dsp_pump_result_t pump_run(void *ctx, uint32_t runtime_ms)
{
    (void)ctx;
    int i = pump.calls++;
    if (pump.forbid) {
        pump.violations++;
        fprintf(stderr, "pump run during a forbidden phase\n");
    }
    pump.last_runtime = runtime_ms;
    /* Evidence: exactly one durable STARTED claim at "now", or a durable manual marker. */
    dsp_dosing_state_t ds;
    bool ok = durable_dosing(&ds);
    uint32_t volume = 0;
    char id[80] = "";
    int evidence = 0;
    if (ok && (ds.flags & DSP_DOSING_F_MANUAL_ACTIVE)) {
        volume = ds.manual_volume_ul;
        snprintf(id, sizeof id, "manual#%lu", (unsigned long)ds.manual_seq);
        evidence++;
    }
    for (size_t s = 0; s < DSP_STORAGE_HISTORY_SLOTS; s++) {
        char key[8];
        dsp_storage_history_key(s, key, sizeof key);
        const fake_kv_entry_t *e = fake_kv_find(&kv, key);
        dsp_history_record_t rec;
        if (e != NULL && dsp_codec_decode_history(e->value, e->len, &rec) == DSP_CODEC_OK &&
            rec.state == DSP_HISTORY_STARTED && rec.claimed_at == g_now) {
            volume = rec.volume_ul;
            snprintf(id, sizeof id, "%s@%04d-%02u-%02u", rec.schedule_id, rec.local_date.year,
                     rec.local_date.month, rec.local_date.day);
            evidence++;
        }
    }
    bool good = ok && evidence == 1 && volume > 0 && ds.last_start_utc == g_now &&
                ds.ledger_sum_ul >= volume;
    /* The most recent durable config write is the consumption of exactly this volume. */
    const fake_kv_entry_t *ce = fake_kv_find(&kv, "config");
    dsp_config_t cfg;
    bool cfg_ok = ce != NULL && dsp_codec_decode_config(ce->value, ce->len, &cfg) == DSP_CODEC_OK;
    uint32_t expect_delta = cfg_prev_remaining < volume ? cfg_prev_remaining : volume;
    good = good && cfg_ok && cfg.reservoir_remaining_ul == cfg_new_remaining &&
           cfg_prev_remaining - cfg_new_remaining == expect_delta;
    uint32_t expect_rt = 0;
    good = good && cfg_ok && dsp_dosing_runtime_ms(volume, cfg.pump_flow_ul_per_s, &expect_rt) &&
           expect_rt == runtime_ms && runtime_ms <= cfg.max_runtime_ms && runtime_ms <= HW_MAX_MS;
    if (!good) {
        pump.violations++;
        fprintf(stderr, "pump activated without complete durable state (evidence %d, volume %lu)\n",
                evidence, (unsigned long)volume);
    }
    pump.last_volume = volume;
    if (i < MAX_RUNS) {
        snprintf(pump.ids[i], sizeof pump.ids[i], "%s", id);
        pump.volumes[i] = volume;
        pump.starts[i] = g_now;
    }
    if (pump.advance_clock) {
        g_now += (dsp_utc_t)((runtime_ms + 999u) / 1000u);
    }
    return i < pump.script_len ? pump.script[i] : DSP_PUMP_OK;
}

static void pump_off(void *ctx)
{
    (void)ctx;
    pump.force_offs++;
}

static bool pump_is_off(void *ctx)
{
    (void)ctx;
    return true; /* the fake pump is OFF on every return of run() */
}

static const dsp_pump_port_t PUMP = {NULL, pump_run, pump_off, pump_is_off};

/* ---- World ------------------------------------------------------------------------------ */

static dsp_storage_t st;
static dsp_history_store_t hist;
static dsp_exec_t ex;
static dsp_notify_t n;
static dsp_dosing_t d;
static uint32_t hw_max = HW_MAX_MS;

static bool open_world(void)
{
    proxy.ctx = NULL;
    proxy.get = px_get;
    proxy.set = px_set;
    proxy.commit = px_commit;
    if (dsp_storage_open(&st, &proxy, TZ, true) != DSP_STORAGE_OK) {
        return false;
    }
    dsp_storage_history_store(&st, &hist);
    dsp_exec_config_t ecfg = {&hist, TZ, DSP_EXEC_DUE_WINDOW_DEFAULT_S, DSP_EXEC_RETENTION_DAYS_DEFAULT};
    if (dsp_exec_init(&ex, &ecfg) != DSP_EXEC_OK) {
        return false;
    }
    if (dsp_notify_init(&n, &st, 0xA5A5A5A5u) != DSP_NOTIFY_OK) {
        return false;
    }
    return dsp_dosing_init(&d, &st, &ex, &n, &PUMP, TZ, hw_max) == DSP_DOSING_OK;
}

/* Restart: RAM lost, flash kept. */
static void boot(void)
{
    fake_kv_power_cycle(&kv);
    fake_kv_clear_faults(&kv);
    fail_notify_sets = false;
    DSP_CHECK(open_world());
}

static void fresh(bool durable)
{
    fake_kv_init(&kv, durable);
    keylog_len = 0;
    fail_notify_sets = false;
    g_now = T0;
    g_trusted = true;
    hw_max = HW_MAX_MS;
    pump_reset();
    DSP_CHECK(open_world());
}

static dsp_config_t load_cfg(void)
{
    dsp_config_t c;
    memset(&c, 0, sizeof c);
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_load_config(&st, &c));
    return c;
}

static void save_cfg(dsp_config_t c)
{
    dsp_config_t cur = load_cfg();
    c.revision = cur.revision + 1;
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_save_config(&st, &c));
}

/* Valid calibration and limits (test values only; production values come from pump tests). */
static void set_pump_config(dsp_config_t *c)
{
    c->pump_flow_ul_per_s = 1200;
    c->calibrated_at = T0 - DAY;
    c->max_dose_ul = 20000;
    c->max_daily_ul = 60000;
    c->max_runtime_ms = 30000;
    c->min_interval_s = 60;
}

static void configure(uint32_t remaining)
{
    dsp_config_t c = load_cfg();
    set_pump_config(&c);
    c.reservoir_capacity_ul = CAP;
    c.reservoir_remaining_ul = remaining;
    save_cfg(c);
}

static void make_ready(void)
{
    configure(CAP);
    uint32_t g = 0;
    uint32_t p = 0;
    DSP_CHECK_EQ_INT(DSP_DOSING_OK, dsp_dosing_set_readiness(&d, DSP_READINESS_READY, &g, &p));
    DSP_CHECK_EQ_INT(0, g);
    DSP_CHECK_EQ_INT(0, p);
}

static dsp_schedule_t sched(const char *id, int hour, uint32_t amount)
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
    s.kind = DSP_REC_DAILY;
    s.interval = 1;
    return s;
}

static dsp_occurrence_t occ_at(const dsp_schedule_t *s, dsp_utc_t around)
{
    dsp_occurrence_t o;
    memset(&o, 0, sizeof o);
    DSP_CHECK_EQ_INT(DSP_REC_OK, dsp_rec_next(s, TZ, around - 1, &o));
    return o;
}

static int count_events(uint16_t code)
{
    int c = 0;
    for (int i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY; i++) {
        const dsp_note_t *e = &n.s.queue[i];
        c += e->seq != 0 && e->type == DSP_NOTE_DOSING_ERROR && (code == 0 || e->code == code);
    }
    return c;
}

static const dsp_note_t *find_event(uint16_t code)
{
    for (int i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY; i++) {
        const dsp_note_t *e = &n.s.queue[i];
        if (e->seq != 0 && e->type == DSP_NOTE_DOSING_ERROR && e->code == code) {
            return e;
        }
    }
    return NULL;
}

static bool blocked_condition(void)
{
    return (n.s.cond[DSP_COND_RESERVOIR_BLOCKED].flags & DSP_COND_F_ACTIVE) != 0;
}

/* ---- Readiness, configuration, gates ------------------------------------------------------ */

static void test_defaults_and_readiness(void)
{
    fresh(true);
    const dsp_dosing_state_t *s = dsp_dosing_state(&d);
    DSP_CHECK(s != NULL);
    DSP_CHECK_EQ_INT(DSP_READINESS_SERVICE_REQUIRED, s->readiness); /* approved default */
    dsp_dosing_result_t r = manual_dose(&d, 1000, clk());
    DSP_CHECK_EQ_INT(DSP_DOSING_REFUSED, r.status);
    DSP_CHECK(r.gates & DSP_DOSING_G_NOT_READY);
    DSP_CHECK_EQ_INT(0, pump.calls);
    /* READY needs calibration, limits and a configured reservoir. */
    uint32_t g = 0;
    uint32_t p = 0;
    DSP_CHECK_EQ_INT(DSP_DOSING_REFUSED, dsp_dosing_set_readiness(&d, DSP_READINESS_READY, &g, &p));
    DSP_CHECK(g & DSP_DOSING_G_PUMP_CONFIG);
    DSP_CHECK(g & DSP_DOSING_G_RESERVOIR_UNCONFIGURED);
    DSP_CHECK_EQ_INT(DSP_PUMPCFG_E_FLOW | DSP_PUMPCFG_E_CALIBRATED_AT | DSP_PUMPCFG_E_MAX_DOSE |
                         DSP_PUMPCFG_E_MAX_DAILY | DSP_PUMPCFG_E_MAX_RUNTIME |
                         DSP_PUMPCFG_E_MIN_INTERVAL,
                     p);
    dsp_config_t c = load_cfg();
    set_pump_config(&c);
    save_cfg(c);
    DSP_CHECK_EQ_INT(DSP_DOSING_REFUSED, dsp_dosing_set_readiness(&d, DSP_READINESS_READY, &g, &p));
    DSP_CHECK_EQ_INT(DSP_DOSING_G_RESERVOIR_UNCONFIGURED, g);
    DSP_CHECK_EQ_INT(DSP_READINESS_SERVICE_REQUIRED, dsp_dosing_state(&d)->readiness);
    make_ready();
    boot();
    DSP_CHECK_EQ_INT(DSP_READINESS_READY, dsp_dosing_state(&d)->readiness); /* durable */
    DSP_CHECK_EQ_INT(DSP_DOSING_OK,
                     dsp_dosing_set_readiness(&d, DSP_READINESS_SERVICE_REQUIRED, NULL, NULL));
    r = manual_dose(&d, 1000, clk());
    DSP_CHECK(r.status == DSP_DOSING_REFUSED && (r.gates & DSP_DOSING_G_NOT_READY));
    DSP_CHECK_EQ_INT(0, pump.calls);
    /* No hardware cutoff known: never READY, never actuates. */
    hw_max = 0;
    boot();
    DSP_CHECK_EQ_INT(DSP_DOSING_REFUSED, dsp_dosing_set_readiness(&d, DSP_READINESS_READY, &g, &p));
    DSP_CHECK(p & DSP_PUMPCFG_E_HW_LIMIT);
    /* Runtime limit above the hardware maximum. */
    hw_max = 20000;
    boot();
    DSP_CHECK_EQ_INT(DSP_DOSING_REFUSED, dsp_dosing_set_readiness(&d, DSP_READINESS_READY, &g, &p));
    DSP_CHECK_EQ_INT(DSP_PUMPCFG_E_HW_LIMIT, p);
    DSP_CHECK_EQ_INT(DSP_DOSING_ERR_ARG, dsp_dosing_set_readiness(&d, (dsp_readiness_t)7, NULL, NULL));
}

static void test_pump_config_required(void)
{
    struct {
        const char *what;
        uint32_t problem;
    } cases[] = {
        {"flow", DSP_PUMPCFG_E_FLOW},       {"calibrated_at", DSP_PUMPCFG_E_CALIBRATED_AT},
        {"max_dose", DSP_PUMPCFG_E_MAX_DOSE}, {"max_daily", DSP_PUMPCFG_E_MAX_DAILY},
        {"daily<dose", DSP_PUMPCFG_E_MAX_DAILY}, {"max_runtime", DSP_PUMPCFG_E_MAX_RUNTIME},
        {"min_interval", DSP_PUMPCFG_E_MIN_INTERVAL},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        fresh(true);
        make_ready();
        dsp_config_t c = load_cfg();
        switch (i) {
        case 0: c.pump_flow_ul_per_s = 0; break;
        case 1: c.calibrated_at = 0; break;
        case 2: c.max_dose_ul = 0; break;
        case 3: c.max_daily_ul = 0; break;
        case 4: c.max_daily_ul = c.max_dose_ul - 1; break;
        case 5: c.max_runtime_ms = 0; break;
        default: c.min_interval_s = 0; break;
        }
        save_cfg(c);
        dsp_schedule_t s = sched("cfg-a", 8, 5000);
        dsp_occurrence_t o = occ_at(&s, T0);
        dsp_dosing_result_t r = dsp_dosing_run_automatic(&d, &s, &o, clk());
        if (!(r.status == DSP_DOSING_REFUSED && (r.pump_problems & cases[i].problem))) {
            fprintf(stderr, "missing %s accepted\n", cases[i].what);
        }
        DSP_CHECK_EQ_INT(DSP_DOSING_REFUSED, r.status);
        DSP_CHECK(r.gates & DSP_DOSING_G_PUMP_CONFIG);
        DSP_CHECK(r.pump_problems & cases[i].problem);
        r = manual_dose(&d, 1000, clk());
        DSP_CHECK_EQ_INT(DSP_DOSING_REFUSED, r.status);
        DSP_CHECK_EQ_INT(0, pump.calls);
        DSP_CHECK(!(dsp_dosing_state(&d)->flags & DSP_DOSING_F_BLOCKED)); /* never a false BLOCKED */
        DSP_CHECK(!blocked_condition());
    }
}

static void test_runtime_arithmetic(void)
{
    uint32_t rt = 0;
    DSP_CHECK(dsp_dosing_runtime_ms(5000, 1200, &rt));
    DSP_CHECK_EQ_INT(4167, rt); /* rounded up */
    DSP_CHECK(dsp_dosing_runtime_ms(1, 3, &rt));
    DSP_CHECK_EQ_INT(334, rt);
    DSP_CHECK(dsp_dosing_runtime_ms(1, UINT32_MAX, &rt));
    DSP_CHECK_EQ_INT(1, rt);
    DSP_CHECK(dsp_dosing_runtime_ms(UINT32_MAX, 1000, &rt));
    DSP_CHECK_EQ_INT(UINT32_MAX, rt);
    DSP_CHECK(!dsp_dosing_runtime_ms(UINT32_MAX, 999, &rt)); /* would not fit 32 bits */
    DSP_CHECK(!dsp_dosing_runtime_ms(UINT32_MAX, 1, &rt));
    DSP_CHECK(!dsp_dosing_runtime_ms(0, 1200, &rt));
    DSP_CHECK(!dsp_dosing_runtime_ms(5000, 0, &rt));
    DSP_CHECK(!dsp_dosing_runtime_ms(5000, 1200, NULL));
}

/* ---- Automatic dosing -------------------------------------------------------------------- */

static int keylog_find(const char *prefix, int from)
{
    for (int i = from; i < keylog_len && i < KEYLOG; i++) {
        if (strncmp(keylog[i], prefix, strlen(prefix)) == 0) {
            return i;
        }
    }
    return -1;
}

static void test_automatic_dose_order_and_volume(void)
{
    fresh(true);
    make_ready();
    dsp_schedule_t s = sched("auto-a", 8, 5000);
    dsp_occurrence_t o = occ_at(&s, T0);
    keylog_len = 0;
    dsp_dosing_result_t r = dsp_dosing_run_automatic(&d, &s, &o, clk());
    DSP_CHECK_EQ_INT(DSP_DOSING_OK, r.status);
    DSP_CHECK(r.pumped);
    DSP_CHECK_EQ_INT(4167, r.runtime_ms);
    DSP_CHECK_EQ_INT(1, pump.calls);
    DSP_CHECK_EQ_INT(0, pump.violations);
    DSP_CHECK(pump.force_offs >= 2); /* at init and after the run */
    /* Order: claim (history) -> ledger (dosing) -> consumption (config) -> finish (history). */
    int claim = keylog_find("h", 0);
    int ledger = keylog_find("dosing", 0);
    int consume = keylog_find("config", 0);
    int finish = keylog_find("h", claim + 1);
    DSP_CHECK(claim >= 0 && claim < ledger && ledger < consume && consume < finish);
    /* Durable claimed volume and outcome. */
    dsp_history_record_t rec;
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_lookup(&ex, "auto-a", &rec));
    DSP_CHECK_EQ_INT(5000, rec.volume_ul);
    DSP_CHECK_EQ_INT(DSP_HISTORY_COMPLETED, rec.state);
    DSP_CHECK_EQ_INT(CAP - 5000, load_cfg().reservoir_remaining_ul);
    const dsp_dosing_state_t *ds = dsp_dosing_state(&d);
    DSP_CHECK_EQ_INT(5000, ds->ledger_sum_ul);
    DSP_CHECK_EQ_INT(T0, ds->last_start_utc);
    /* At-most-once: the same occurrence never runs again (also after a restart). */
    r = dsp_dosing_run_automatic(&d, &s, &o, clk());
    DSP_CHECK_EQ_INT(DSP_DOSING_NOT_ELIGIBLE, r.status);
    DSP_CHECK_EQ_INT(DSP_EXEC_ALREADY_CLAIMED, r.exec);
    boot();
    r = dsp_dosing_run_automatic(&d, &s, &o, clk());
    DSP_CHECK_EQ_INT(DSP_EXEC_ALREADY_CLAIMED, r.exec);
    DSP_CHECK_EQ_INT(1, pump.calls);
    /* Editing the schedule afterwards does not change the durable claimed volume. */
    s.amount_ul = 9000;
    s.revision = 2;
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_lookup(&ex, "auto-a", &rec));
    DSP_CHECK_EQ_INT(5000, rec.volume_ul);
    /* A permit is never reusable. */
    dsp_dose_permit_t permit;
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_reservoir_consume(&st, 10, DSP_DOSE_MANUAL, NULL, &permit));
    uint32_t v = 0;
    DSP_CHECK(dsp_dose_permit_take(&permit, &v));
    DSP_CHECK(!dsp_dose_permit_take(&permit, &v));
}

static void test_process_due_order_and_window(void)
{
    fresh(true);
    make_ready();
    /* Same-time schedules run one after another only if the minimum interval allows it
     * (time advances by each runtime): 1 s here. With a longer interval the second one is
     * refused (LIMIT, unclaimed) and must be retried within its due window: see below. */
    dsp_config_t c = load_cfg();
    c.min_interval_s = 1;
    save_cfg(c);
    dsp_schedule_t set[3] = {sched("b-sched", 8, 3000), sched("a-sched", 8, 2000),
                             sched("c-later", 20, 1000)};
    set[2].enabled = true;
    dsp_dosing_due_report_t rep = dsp_dosing_process_due(&d, set, 3, &CLOCK);
    DSP_CHECK_EQ_INT(2, rep.candidates); /* the 20:00 one is not due */
    DSP_CHECK_EQ_INT(2, rep.dosed);      /* one after another (time advances with each run) */
    DSP_CHECK_EQ_STR("a-sched@2026-11-20", pump.ids[0]); /* same time: schedule ID order */
    DSP_CHECK_EQ_STR("b-sched@2026-11-20", pump.ids[1]);
    DSP_CHECK_EQ_INT(0, pump.violations);
    /* Again in the same window: already claimed, nothing runs. */
    rep = dsp_dosing_process_due(&d, set, 3, &CLOCK);
    DSP_CHECK_EQ_INT(0, rep.dosed);
    DSP_CHECK_EQ_INT(2, pump.calls);
    /* Next day: woken 11 minutes late = missed, never caught up. */
    g_now = T0 + DAY + 11 * 60;
    rep = dsp_dosing_process_due(&d, set, 3, &CLOCK);
    DSP_CHECK_EQ_INT(0, rep.dosed);
    DSP_CHECK_EQ_INT(2, pump.calls);
    /* Disabled schedules are ignored; untrusted clock: no candidates. */
    g_now = T0 + 2 * DAY;
    set[0].enabled = set[1].enabled = false;
    rep = dsp_dosing_process_due(&d, set, 3, &CLOCK);
    DSP_CHECK_EQ_INT(0, rep.candidates);
    set[0].enabled = true;
    g_trusted = false;
    rep = dsp_dosing_process_due(&d, set, 3, &CLOCK);
    DSP_CHECK_EQ_INT(0, rep.candidates);
    DSP_CHECK_EQ_INT(2, pump.calls);
    g_trusted = true;
    /* A longer interval: the second same-time dose is refused, NOT claimed, and runs when
     * re-evaluated after the interval, still inside its due window. */
    c = load_cfg();
    c.min_interval_s = 120;
    save_cfg(c);
    set[1].enabled = true;
    g_now = T0 + 3 * DAY;
    rep = dsp_dosing_process_due(&d, set, 3, &CLOCK);
    DSP_CHECK_EQ_INT(1, rep.dosed);
    DSP_CHECK_EQ_INT(DSP_DOSING_LIMIT, rep.last.status);
    DSP_CHECK_EQ_INT(DSP_DOSING_L_INTERVAL, rep.last.limits);
    dsp_history_record_t rec;
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_lookup(&ex, "b-sched", &rec));
    DSP_CHECK(rec.local_date.day == 20); /* the previous claim: today (23rd) not claimed */
    g_now = dsp_dosing_state(&d)->last_start_utc + 120;
    rep = dsp_dosing_process_due(&d, set, 3, &CLOCK);
    DSP_CHECK_EQ_INT(1, rep.dosed);
    DSP_CHECK_EQ_INT(4, pump.calls);
    /* The refused dose is NOT re-evaluated inside its due window (the interval outlasts it):
     * it becomes MISSED and is never caught up, neither later that day nor after restarts. */
    c = load_cfg();
    c.min_interval_s = DSP_EXEC_DUE_WINDOW_DEFAULT_S + 60;
    save_cfg(c);
    g_now = T0 + 4 * DAY;
    rep = dsp_dosing_process_due(&d, set, 3, &CLOCK);
    DSP_CHECK_EQ_INT(1, rep.dosed);
    DSP_CHECK_EQ_INT(DSP_DOSING_LIMIT, rep.last.status);
    DSP_CHECK_EQ_INT(5, pump.calls);
    dsp_occurrence_t ob = occ_at(&set[0], T0 + 4 * DAY);
    for (int k = 0; k < 3; k++) {
        g_now = T0 + 4 * DAY + DSP_EXEC_DUE_WINDOW_DEFAULT_S + 61 + (dsp_utc_t)k * 3 * 3600;
        if (k == 2) {
            boot();
            dsp_dosing_recovery_t rv;
            DSP_CHECK_EQ_INT(DSP_DOSING_OK, dsp_dosing_recover(&d, nclk(), &rv));
        }
        rep = dsp_dosing_process_due(&d, set, 3, &CLOCK);
        DSP_CHECK_EQ_INT(0, rep.candidates);
        DSP_CHECK_EQ_INT(0, rep.dosed);
        dsp_dosing_result_t r = dsp_dosing_run_automatic(&d, &set[0], &ob, clk());
        DSP_CHECK_EQ_INT(DSP_DOSING_NOT_ELIGIBLE, r.status);
        DSP_CHECK_EQ_INT(DSP_EXEC_MISSED, r.exec);
        DSP_CHECK(!r.pumped);
    }
    DSP_CHECK_EQ_INT(5, pump.calls);
    DSP_CHECK_EQ_INT(0, pump.violations);
}

/* ---- Limits and clock -------------------------------------------------------------------- */

static void test_limits(void)
{
    fresh(true);
    make_ready();
    /* Per-dose volume and runtime. */
    dsp_dosing_result_t r = manual_dose(&d, 20001, clk());
    DSP_CHECK_EQ_INT(DSP_DOSING_LIMIT, r.status);
    DSP_CHECK(r.limits & DSP_DOSING_L_DOSE);
    dsp_config_t c = load_cfg();
    c.max_runtime_ms = 4000; /* 5000 ul at 1200 ul/s needs 4167 ms */
    save_cfg(c);
    keylog_len = 0;
    r = manual_dose(&d, 5000, clk());
    DSP_CHECK_EQ_INT(DSP_DOSING_LIMIT, r.status);
    DSP_CHECK_EQ_INT(DSP_DOSING_L_RUNTIME, r.limits);
    /* M1.10b.2: the refusal is durable (anti-replay): exactly one write, the dosing record;
     * no consumption, no history, no pump. */
    DSP_CHECK_EQ_INT(1, keylog_len);
    DSP_CHECK_EQ_STR("dosing", keylog[0]);
    DSP_CHECK_EQ_INT(DSP_MRES_REFUSED,
                     dsp_dosing_state_find_result(dsp_dosing_state(&d), 0, g_mseq)->state);
    c.max_runtime_ms = 30000;
    save_cfg(c);
    /* Interval: a second start within 60 s is refused, also after a restart. */
    DSP_CHECK_EQ_INT(DSP_DOSING_OK, manual_dose(&d, 5000, clk()).status);
    g_now += 30;
    r = manual_dose(&d, 5000, clk());
    DSP_CHECK(r.status == DSP_DOSING_LIMIT && r.limits == DSP_DOSING_L_INTERVAL);
    boot();
    r = manual_dose(&d, 5000, clk());
    DSP_CHECK(r.status == DSP_DOSING_LIMIT && r.limits == DSP_DOSING_L_INTERVAL);
    g_now = dsp_dosing_state(&d)->last_start_utc + 60;
    DSP_CHECK_EQ_INT(DSP_DOSING_OK, manual_dose(&d, 5000, clk()).status);
    /* Daily: 60000 per local day. 10000 so far; 50000 more fit, then nothing. */
    for (int i = 0; i < 5; i++) {
        g_now += 120;
        DSP_CHECK_EQ_INT(DSP_DOSING_OK, manual_dose(&d, 10000, clk()).status);
    }
    g_now += 120;
    r = manual_dose(&d, 1, clk());
    DSP_CHECK(r.status == DSP_DOSING_LIMIT && r.limits == DSP_DOSING_L_DAILY);
    DSP_CHECK_EQ_INT(60000, dsp_dosing_state(&d)->ledger_sum_ul);
    DSP_CHECK_EQ_INT(7, pump.calls);
    DSP_CHECK_EQ_INT(0, pump.violations);
}

static void test_daily_ledger_and_clock_changes(void)
{
    fresh(true);
    make_ready();
    dsp_config_t c = load_cfg();
    c.max_daily_ul = 20000;
    c.max_dose_ul = 20000;
    save_cfg(c);
    /* 23:59 local (22:59Z), then 00:01 local next day: a new local date, a new budget. */
    g_now = 1795215540; /* 2026-11-20T22:59:00Z = 23:59 CET */
    DSP_CHECK_EQ_INT(DSP_DOSING_OK, manual_dose(&d, 20000, clk()).status);
    g_now = 1795215540 + 120 + 17; /* 2026-11-21 00:01 CET */
    DSP_CHECK_EQ_INT(DSP_DOSING_OK, manual_dose(&d, 20000, clk()).status);
    dsp_date_t dd = dsp_dosing_state(&d)->ledger_date;
    DSP_CHECK(dd.year == 2026 && dd.month == 11 && dd.day == 21);
    /* Rollback to the previous local date: counted against the highest date (full), and the
     * interval waits for the clock to pass the last start. */
    g_now = 1795215540 - 3600; /* back to 2026-11-20 22:59 CET */
    dsp_dosing_result_t r = manual_dose(&d, 1, clk());
    DSP_CHECK_EQ_INT(DSP_DOSING_LIMIT, r.status);
    DSP_CHECK(r.limits & DSP_DOSING_L_DAILY);
    DSP_CHECK(r.limits & DSP_DOSING_L_INTERVAL);
    /* Forward again to the next local date and beyond the interval: allowed. */
    g_now = 1795215540 + DAY + 600; /* 2026-11-22 00:09 CET */
    DSP_CHECK_EQ_INT(DSP_DOSING_OK, manual_dose(&d, 20000, clk()).status);
    /* Untrusted clock: refused, ledger untouched. */
    dsp_dosing_state_t before = *dsp_dosing_state(&d);
    g_trusted = false;
    g_now += 3 * DAY;
    r = manual_dose(&d, 1000, clk());
    DSP_CHECK(r.status == DSP_DOSING_REFUSED && (r.gates & DSP_DOSING_G_CLOCK));
    /* ledger and everything else untouched; only the durable refusal (hw + ring) changed */
    dsp_dosing_state_t after = *dsp_dosing_state(&d);
    DSP_CHECK_EQ_INT(g_mseq, after.manual_hw[0]);
    memcpy(before.manual_hw, after.manual_hw, sizeof before.manual_hw);
    memcpy(before.ring, after.ring, sizeof before.ring);
    before.ring_next = after.ring_next;
    /* M1.10b.3: the refusal is counted in the same write (no trusted date: date unchanged) */
    DSP_CHECK(dsp_date_compare(before.adm_date, after.adm_date) == 0);
    DSP_CHECK_EQ_INT(before.adm_count + 1, after.adm_count);
    before.adm_count = after.adm_count;
    DSP_CHECK(dsp_dosing_state_equal(&before, &after));
    g_trusted = true;
    /* DST days are single local dates: 2027-03-28 (23 h) and 2026-10-25 (25 h). */
    struct {
        dsp_utc_t first, second;
    } dst[] = {{1806188400, 1806271200}, /* 2027-03-28 00:00 CET .. 2027-03-29 00:00 CEST (23 h) */
               {1792879200, 1792969200}}; /* 2026-10-25 00:00 CEST .. 2026-10-26 00:00 CET (25 h) */
    for (size_t i = 0; i < 2; i++) {
        fresh(true);
        make_ready();
        c = load_cfg();
        c.max_daily_ul = 20000;
        c.max_dose_ul = 20000;
        save_cfg(c);
        g_now = dst[i].first + 60;
        DSP_CHECK_EQ_INT(DSP_DOSING_OK, manual_dose(&d, 20000, clk()).status);
        g_now = dst[i].second - 60; /* the same local date, 23 or 25 hours later */
        r = manual_dose(&d, 1, clk());
        DSP_CHECK(r.status == DSP_DOSING_LIMIT && r.limits == DSP_DOSING_L_DAILY);
        g_now = dst[i].second + 60; /* the next local date */
        DSP_CHECK_EQ_INT(DSP_DOSING_OK, manual_dose(&d, 20000, clk()).status);
    }
    DSP_CHECK_EQ_INT(0, pump.violations);
}

/* ---- BLOCKED latch, missing configuration, refill ----------------------------------------- */

static void test_blocked_latch(void)
{
    fresh(true);
    make_ready();
    configure(4000); /* less than the 5000 ul dose */
    dsp_schedule_t s = sched("blk-a", 8, 5000);
    dsp_occurrence_t o = occ_at(&s, T0);
    dsp_dosing_result_t r = dsp_dosing_run_automatic(&d, &s, &o, clk());
    DSP_CHECK_EQ_INT(DSP_DOSING_NOT_COVERED, r.status);
    DSP_CHECK(!r.pumped);
    DSP_CHECK(dsp_dosing_state(&d)->flags & DSP_DOSING_F_BLOCKED);
    DSP_CHECK(blocked_condition()); /* RESERVOIR_BLOCKED reported */
    dsp_history_record_t rec;
    DSP_CHECK_EQ_INT(DSP_EXEC_NOT_FOUND, dsp_exec_lookup(&ex, "blk-a", &rec)); /* not claimed */
    boot();
    DSP_CHECK(dsp_dosing_state(&d)->flags & DSP_DOSING_F_BLOCKED); /* durable */
    /* Manual dosing is refused too (Q7), even for a covered volume. */
    r = manual_dose(&d, 1000, clk());
    DSP_CHECK(r.status == DSP_DOSING_REFUSED && (r.gates & DSP_DOSING_G_BLOCKED));
    /* Every automatic dose stops until the refill, whatever its volume. */
    dsp_schedule_t small = sched("blk-small", 8, 100);
    dsp_occurrence_t so = occ_at(&small, T0);
    r = dsp_dosing_run_automatic(&d, &small, &so, clk());
    DSP_CHECK(r.status == DSP_DOSING_REFUSED && (r.gates & DSP_DOSING_G_BLOCKED));
    uint32_t g = 0;
    DSP_CHECK_EQ_INT(DSP_DOSING_REFUSED, dsp_dosing_set_readiness(&d, DSP_READINESS_READY, &g, NULL));
    DSP_CHECK(g & DSP_DOSING_G_BLOCKED);
    DSP_CHECK_EQ_INT(0, pump.calls);
    /* Refill clears the latch and the condition; dosing resumes. */
    DSP_CHECK_EQ_INT(DSP_DOSING_OK, dsp_dosing_refill(&d, CAP, CAP, nclk()));
    DSP_CHECK(!(dsp_dosing_state(&d)->flags & DSP_DOSING_F_BLOCKED));
    DSP_CHECK(!blocked_condition());
    DSP_CHECK_EQ_INT(CAP, load_cfg().reservoir_remaining_ul);
    r = dsp_dosing_run_automatic(&d, &small, &so, clk());
    DSP_CHECK_EQ_INT(DSP_DOSING_OK, r.status);
    DSP_CHECK_EQ_INT(0, pump.violations);
}

static void test_missing_configuration_never_blocks(void)
{
    fresh(true);
    make_ready();
    /* Reservoir not configured (capacity 0): refused, no latch, no BLOCKED notification. */
    dsp_config_t c = load_cfg();
    c.reservoir_capacity_ul = 0;
    c.reservoir_remaining_ul = 0;
    save_cfg(c);
    dsp_schedule_t s = sched("cfg-b", 8, 5000);
    dsp_occurrence_t o = occ_at(&s, T0);
    dsp_dosing_result_t r = dsp_dosing_run_automatic(&d, &s, &o, clk());
    DSP_CHECK(r.status == DSP_DOSING_REFUSED && (r.gates & DSP_DOSING_G_RESERVOIR_UNCONFIGURED));
    DSP_CHECK(!(dsp_dosing_state(&d)->flags & DSP_DOSING_F_BLOCKED));
    DSP_CHECK(!blocked_condition());
    DSP_CHECK_EQ_INT(0, pump.calls);
    uint32_t p = 0;
    uint32_t g = dsp_dosing_gates(&d, true, &p);
    DSP_CHECK(g & DSP_DOSING_G_RESERVOIR_UNCONFIGURED); /* exposed as a diagnostic */
}

static void test_refill_power_cut_every_write(void)
{
    for (int durable = 0; durable < 2; durable++) {
        for (int cut = 1; cut <= 8; cut++) {
            fresh(durable == 1);
            make_ready();
            configure(4000);
            dsp_schedule_t s = sched("rf-a", 8, 5000);
            dsp_occurrence_t o = occ_at(&s, T0);
            DSP_CHECK_EQ_INT(DSP_DOSING_NOT_COVERED, dsp_dosing_run_automatic(&d, &s, &o, clk()).status);
            /* An earlier interrupted dose left the estimate uncertain. */
            dsp_dosing_error_t e = {DSP_DOSEERR_INTERRUPTED, false, 1};
            DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_report_dosing_error(&n, &e, nclk()));
            DSP_CHECK(n.s.flags & DSP_NOTIFY_F_ESTIMATE_UNCERTAIN);
            kv.power_cut_after_sets = cut;
            dsp_dosing_status_t rs = dsp_dosing_refill(&d, CAP, 90000, nclk());
            boot();
            /* Before recovery: dosing is enabled only if the refill fully completed. */
            dsp_dosing_state_t ds = *dsp_dosing_state(&d);
            bool enabled = !(ds.flags & (DSP_DOSING_F_BLOCKED | DSP_DOSING_F_REFILL_PENDING));
            if (enabled) {
                DSP_CHECK_EQ_INT(90000, load_cfg().reservoir_remaining_ul);
                DSP_CHECK(!(n.s.flags & DSP_NOTIFY_F_ESTIMATE_UNCERTAIN));
                DSP_CHECK(!blocked_condition());
            } else {
                dsp_dosing_result_t r = manual_dose(&d, 1000, clk());
                DSP_CHECK_EQ_INT(DSP_DOSING_REFUSED, r.status);
            }
            if (rs == DSP_DOSING_OK) {
                DSP_CHECK(enabled);
            }
            /* Recovery completes it (rolled forward, idempotent). */
            dsp_dosing_recovery_t rec;
            DSP_CHECK_EQ_INT(DSP_DOSING_OK, dsp_dosing_recover(&d, nclk(), &rec));
            ds = *dsp_dosing_state(&d);
            DSP_CHECK(!(ds.flags & (DSP_DOSING_F_BLOCKED | DSP_DOSING_F_REFILL_PENDING)) ||
                      (ds.flags & DSP_DOSING_F_BLOCKED && !(ds.flags & DSP_DOSING_F_REFILL_PENDING)));
            if (ds.flags & DSP_DOSING_F_BLOCKED) {
                /* The cut happened before the intent was durable: nothing changed. */
                DSP_CHECK_EQ_INT(4000, load_cfg().reservoir_remaining_ul);
                continue;
            }
            DSP_CHECK_EQ_INT(90000, load_cfg().reservoir_remaining_ul);
            DSP_CHECK_EQ_INT(CAP, load_cfg().reservoir_capacity_ul);
            DSP_CHECK(!(n.s.flags & DSP_NOTIFY_F_ESTIMATE_UNCERTAIN));
            DSP_CHECK(!blocked_condition());
            DSP_CHECK_EQ_INT(0, ds.refill_capacity_ul + ds.refill_remaining_ul + ds.refill_seq);
        }
    }
    /* Invalid refill values; refill refused while a manual marker awaits recovery. */
    fresh(true);
    DSP_CHECK_EQ_INT(DSP_DOSING_ERR_ARG, dsp_dosing_refill(&d, 0, 0, nclk()));
    DSP_CHECK_EQ_INT(DSP_DOSING_ERR_ARG, dsp_dosing_refill(&d, 100, 101, nclk()));
    DSP_CHECK_EQ_INT(DSP_DOSING_ERR_ARG, dsp_dosing_refill(&d, 100, 0, nclk()));
}

/* ---- Pump faults, storage failures --------------------------------------------------------- */

static void test_pump_failures(void)
{
    dsp_pump_result_t res[] = {DSP_PUMP_FAILED, DSP_PUMP_START_FAILED};
    uint16_t codes[] = {DSP_DOSEERR_PUMP_FAILED, DSP_DOSEERR_START_FAILED};
    for (int i = 0; i < 2; i++) {
        fresh(true);
        make_ready();
        pump.script[0] = res[i];
        pump.script_len = 1;
        dsp_schedule_t s = sched("pf-a", 8, 5000);
        dsp_occurrence_t o = occ_at(&s, T0);
        int offs = pump.force_offs;
        dsp_dosing_result_t r = dsp_dosing_run_automatic(&d, &s, &o, clk());
        DSP_CHECK_EQ_INT(DSP_DOSING_PUMP_FAILED, r.status);
        DSP_CHECK(pump.force_offs > offs); /* OFF after the failed run */
        dsp_history_record_t rec;
        DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_lookup(&ex, "pf-a", &rec));
        DSP_CHECK_EQ_INT(DSP_HISTORY_FAILED, rec.state);
        const dsp_note_t *e = find_event(codes[i]);
        DSP_CHECK(e != NULL);
        if (e != NULL) {
            DSP_CHECK_EQ_INT(5000, e->value);
        }
        DSP_CHECK(n.s.flags & DSP_NOTIFY_F_ESTIMATE_UNCERTAIN);
        DSP_CHECK_EQ_INT(CAP - 5000, load_cfg().reservoir_remaining_ul); /* counted conservatively */
        /* A later recovery does not report it again. */
        boot();
        dsp_dosing_recovery_t rv;
        DSP_CHECK_EQ_INT(DSP_DOSING_OK, dsp_dosing_recover(&d, nclk(), &rv));
        DSP_CHECK_EQ_INT(1, count_events(0));
    }
}

static void test_storage_failure_at_every_write(void)
{
    /* Writes of an automatic dose: claim, ledger, consumption, finish. A failure (power
     * still on: every later write fails too) at each of them. */
    for (int cut = 1; cut <= 5; cut++) {
        fresh(true);
        make_ready();
        dsp_schedule_t s = sched("sf-a", 8, 5000);
        dsp_schedule_t s2 = sched("sf-b", 8, 1000);
        dsp_occurrence_t o = occ_at(&s, T0);
        dsp_occurrence_t o2 = occ_at(&s2, T0);
        kv.power_cut_after_sets = cut;
        kv.durable_on_set = false; /* the failing write does not land */
        int before = pump.calls;
        dsp_dosing_result_t r = dsp_dosing_run_automatic(&d, &s, &o, clk());
        bool pumped = pump.calls > before;
        /* Pump only if claim, ledger and consumption were all durable (verified inside). */
        DSP_CHECK_EQ_INT(0, pump.violations);
        if (cut <= 3) {
            DSP_CHECK(!pumped);
            DSP_CHECK_EQ_INT(DSP_DOSING_ERR_STORAGE, r.status);
        }
        /* After a storage failure the rest of the wake is degraded: no further actuation. */
        if (r.status == DSP_DOSING_ERR_STORAGE || d.degraded) {
            before = pump.calls;
            g_now += 120;
            r = dsp_dosing_run_automatic(&d, &s2, &o2, clk());
            DSP_CHECK(pump.calls == before);
            DSP_CHECK(r.status == DSP_DOSING_REFUSED || r.status == DSP_DOSING_ERR_STORAGE ||
                      r.status == DSP_DOSING_NOT_ELIGIBLE);
        }
    }
}

static int durable_history_state(const char *id);

/* A dose-bound report whose notification write fails: the dose's durable source (STARTED
 * record / manual marker) is KEPT, dosing stays disabled for the wake, the dose is never
 * repeated, and exactly one report results, either the RAM copy flushed later in the same
 * wake (key known at recovery) or the boot-recovery INTERRUPTED report. */
static void test_unpersisted_report_keeps_durable_source(void)
{
    for (int manual = 0; manual < 2; manual++) {
        for (int flush = 0; flush < 2; flush++) {
            fresh(true);
            make_ready();
            pump.script[0] = DSP_PUMP_FAILED;
            pump.script_len = 1;
            dsp_schedule_t s = sched("up-a", 8, 5000);
            dsp_schedule_t s2 = sched("up-b", 8, 1000);
            dsp_occurrence_t o = occ_at(&s, T0);
            fail_notify_sets = true;
            dsp_dosing_result_t r = manual ? manual_dose(&d, 5000, clk())
                                           : dsp_dosing_run_automatic(&d, &s, &o, clk());
            DSP_CHECK_EQ_INT(DSP_DOSING_PUMP_FAILED, r.status);
            DSP_CHECK_EQ_INT(1, pump.calls);
            DSP_CHECK(d.degraded);
            /* 1. Dosing stays disabled for the rest of the wake. */
            g_now += 3600;
            dsp_occurrence_t o2 = occ_at(&s2, g_now);
            dsp_dosing_result_t r2 = dsp_dosing_run_automatic(&d, &s2, &o2, clk());
            DSP_CHECK(r2.status == DSP_DOSING_REFUSED && !r2.pumped);
            DSP_CHECK(r2.gates & DSP_DOSING_G_DEGRADED);
            uint64_t hw = dsp_dosing_state(&d)->manual_hw[0];
            r2 = manual_dose(&d, 1000, clk());
            /* M1.10b.2: nothing can be made durable after the storage failure: not executed,
             * not consumed (the request ID stays undecided) */
            DSP_CHECK(r2.status == DSP_DOSING_ERR_STORAGE && !r2.pumped);
            DSP_CHECK(r2.gates & DSP_DOSING_G_DEGRADED);
            DSP_CHECK_EQ_INT(hw, dsp_dosing_state(&d)->manual_hw[0]);
            DSP_CHECK_EQ_INT(1, pump.calls);
            /* The durable source is kept (the report is only in RAM). */
            dsp_dosing_state_t ds;
            DSP_CHECK(durable_dosing(&ds));
            if (manual) {
                DSP_CHECK(ds.flags & DSP_DOSING_F_MANUAL_ACTIVE);
                DSP_CHECK_EQ_INT(5000, ds.manual_volume_ul);
            } else {
                DSP_CHECK_EQ_INT(DSP_HISTORY_STARTED, durable_history_state("up-a"));
            }
            DSP_CHECK_EQ_INT(CAP - 5000, load_cfg().reservoir_remaining_ul);
            if (flush) {
                /* Storage recovers in the same wake: the next notify write flushes it. */
                fail_notify_sets = false;
                (void)dsp_dosing_observe_reservoir(&d, nclk());
                DSP_CHECK(!n.dirty);
            }
            /* 2./3. Restart: recovery reports once, never pumps, never changes the estimate. */
            pump.forbid = true;
            for (int k = 0; k < 3; k++) {
                boot();
                dsp_dosing_recovery_t rv;
                DSP_CHECK_EQ_INT(DSP_DOSING_OK, dsp_dosing_recover(&d, nclk(), &rv));
                DSP_CHECK_EQ_INT(k == 0 ? 1 : 0, rv.automatic + rv.manual);
                DSP_CHECK_EQ_INT(k == 0 && flush ? 1 : 0, rv.duplicates);
                DSP_CHECK_EQ_INT(1, count_events(0)); /* exactly one dose-bound report */
                DSP_CHECK_EQ_INT(flush ? 1 : 0, count_events(DSP_DOSEERR_PUMP_FAILED));
                DSP_CHECK_EQ_INT(flush ? 0 : 1, count_events(DSP_DOSEERR_INTERRUPTED));
                const dsp_note_t *e = find_event(flush ? DSP_DOSEERR_PUMP_FAILED
                                                       : DSP_DOSEERR_INTERRUPTED);
                DSP_CHECK(e != NULL && e->value == 5000);
                DSP_CHECK(e != NULL && ((e->flags & DSP_NOTE_F_MANUAL) != 0) == (manual == 1));
                DSP_CHECK(n.s.flags & DSP_NOTIFY_F_ESTIMATE_UNCERTAIN);
                DSP_CHECK_EQ_INT(CAP - 5000, load_cfg().reservoir_remaining_ul);
            }
            pump.forbid = false;
            DSP_CHECK_EQ_INT(1, pump.calls);
            DSP_CHECK_EQ_INT(0, pump.violations);
            DSP_CHECK(!(dsp_dosing_state(&d)->flags & DSP_DOSING_F_MANUAL_ACTIVE));
            if (!manual) {
                /* Never repeated automatically. */
                DSP_CHECK_EQ_INT(DSP_HISTORY_INTERRUPTED, durable_history_state("up-a"));
                g_now = T0;
                r = dsp_dosing_run_automatic(&d, &s, &o, clk());
                DSP_CHECK(r.status != DSP_DOSING_OK && !r.pumped);
            }
        }
    }
}

/* A report with NO durable source left (nothing restricting was written) is RAM-only by
 * design: it is lost at a restart and recovery does not invent one. */
static void test_unpersisted_report_without_durable_source(void)
{
    fresh(true);
    make_ready();
    /* Automatic: the claim itself fails, so there is no STARTED record and no pump run. */
    kv.fail_sets = true;
    dsp_schedule_t s = sched("ns-a", 8, 5000);
    dsp_occurrence_t o = occ_at(&s, T0);
    dsp_dosing_result_t r = dsp_dosing_run_automatic(&d, &s, &o, clk());
    DSP_CHECK_EQ_INT(DSP_DOSING_ERR_STORAGE, r.status);
    DSP_CHECK(!r.pumped && d.degraded);
    /* Manual: the marker write fails, so there is no marker and no pump run. */
    r = manual_dose(&d, 5000, clk());
    DSP_CHECK(!r.pumped);
    boot();
    dsp_dosing_recovery_t rv;
    DSP_CHECK_EQ_INT(DSP_DOSING_OK, dsp_dosing_recover(&d, nclk(), &rv));
    DSP_CHECK_EQ_INT(0, rv.automatic + rv.manual);
    DSP_CHECK_EQ_INT(0, count_events(0));
    DSP_CHECK_EQ_INT(0, durable_history_state("ns-a"));
    DSP_CHECK_EQ_INT(CAP, load_cfg().reservoir_remaining_ul);
    DSP_CHECK_EQ_INT(0, pump.calls);
}

/* ---- Power loss and idempotent recovery ---------------------------------------------------- */

static int durable_history_state(const char *id)
{
    for (size_t s = 0; s < DSP_STORAGE_HISTORY_SLOTS; s++) {
        char key[8];
        dsp_storage_history_key(s, key, sizeof key);
        const fake_kv_entry_t *e = fake_kv_find(&kv, key);
        dsp_history_record_t rec;
        if (e != NULL && dsp_codec_decode_history(e->value, e->len, &rec) == DSP_CODEC_OK &&
            strcmp(rec.schedule_id, id) == 0) {
            return rec.state;
        }
    }
    return 0;
}

static void power_loss_matrix(bool manual)
{
    for (int durable = 0; durable < 2; durable++) {
        for (int cut1 = 1; cut1 <= 7; cut1++) {
            for (int cut2 = 0; cut2 <= 4; cut2++) {
                fresh(durable == 1);
                make_ready();
                dsp_schedule_t s = sched("pl-a", 8, 5000);
                dsp_occurrence_t o = occ_at(&s, T0);
                /* 1. The dose, cut after cut1 writes; then a restart. */
                kv.power_cut_after_sets = cut1;
                if (manual) {
                    (void)manual_dose(&d, 5000, clk());
                } else {
                    (void)dsp_dosing_run_automatic(&d, &s, &o, clk());
                }
                int runs = pump.calls;
                boot();
                uint32_t remaining = load_cfg().reservoir_remaining_ul;
                int hstate = durable_history_state("pl-a");
                dsp_dosing_state_t ds = *dsp_dosing_state(&d);
                bool marker = (ds.flags & DSP_DOSING_F_MANUAL_ACTIVE) != 0;
                bool interrupted = manual ? marker : hstate == DSP_HISTORY_STARTED;
                bool pump_fault_reported = count_events(0) > 0; /* none scripted */
                DSP_CHECK(!pump_fault_reported);
                /* 2. Recovery, cut after cut2 writes (0 = no cut), restart, recovery again. */
                pump.forbid = true;
                if (cut2 > 0) {
                    kv.power_cut_after_sets = cut2;
                }
                dsp_dosing_recovery_t rv;
                (void)dsp_dosing_recover(&d, nclk(), &rv);
                boot();
                DSP_CHECK_EQ_INT(DSP_DOSING_OK, dsp_dosing_recover(&d, nclk(), &rv));
                boot();
                DSP_CHECK_EQ_INT(DSP_DOSING_OK, dsp_dosing_recover(&d, nclk(), &rv));
                DSP_CHECK_EQ_INT(0, rv.automatic + rv.manual); /* nothing left */
                pump.forbid = false;
                /* Never a pump run in recovery; never more than one for the dose. */
                DSP_CHECK_EQ_INT(runs, pump.calls);
                DSP_CHECK(runs <= 1);
                DSP_CHECK_EQ_INT(0, pump.violations);
                /* The interruption is reported exactly once (never lost, never twice). */
                int events = count_events(DSP_DOSEERR_INTERRUPTED);
                if (events != (interrupted ? 1 : 0)) {
                    fprintf(stderr, "%s durable=%d cut1=%d cut2=%d: %d interruption events (expected %d)\n",
                            manual ? "manual" : "automatic", durable, cut1, cut2, events,
                            interrupted ? 1 : 0);
                }
                DSP_CHECK_EQ_INT(interrupted ? 1 : 0, events);
                if (interrupted) {
                    const dsp_note_t *e = find_event(DSP_DOSEERR_INTERRUPTED);
                    DSP_CHECK(e != NULL && e->value == 5000); /* the durable claimed volume */
                    DSP_CHECK(e != NULL && ((e->flags & DSP_NOTE_F_MANUAL) != 0) == manual);
                    DSP_CHECK(n.s.flags & DSP_NOTIFY_F_ESTIMATE_UNCERTAIN);
                }
                /* Recovery never changes the reservoir estimate. */
                DSP_CHECK_EQ_INT(remaining, load_cfg().reservoir_remaining_ul);
                DSP_CHECK(remaining == CAP || remaining == CAP - 5000);
                if (runs == 1) {
                    DSP_CHECK_EQ_INT(CAP - 5000, remaining); /* pumped => consumed */
                }
                /* Marker cleared, record no longer STARTED, never re-executed. */
                DSP_CHECK(!(dsp_dosing_state(&d)->flags & DSP_DOSING_F_MANUAL_ACTIVE));
                if (!manual) {
                    DSP_CHECK(durable_history_state("pl-a") != DSP_HISTORY_STARTED);
                    if (hstate != 0) {
                        dsp_dosing_result_t r = dsp_dosing_run_automatic(&d, &s, &o, clk());
                        DSP_CHECK(r.status != DSP_DOSING_OK && !r.pumped);
                    }
                }
            }
        }
    }
}

static void test_power_loss_automatic(void)
{
    power_loss_matrix(false);
}

static void test_power_loss_manual(void)
{
    power_loss_matrix(true);
}

static void test_recovery_of_format1_record(void)
{
    fresh(true);
    make_ready();
    /* A STARTED record written by M1.2-era firmware (format 1, no volume). */
    dsp_history_record_t rec;
    memset(&rec, 0, sizeof rec);
    snprintf(rec.schedule_id, sizeof rec.schedule_id, "old-a");
    rec.local_date.year = 2026;
    rec.local_date.month = 11;
    rec.local_date.day = 19;
    rec.state = DSP_HISTORY_STARTED;
    rec.schedule_revision = 1;
    rec.scheduled_at = T0 - DAY;
    rec.claimed_at = T0 - DAY;
    uint8_t buf[DSP_HISTORY_RECORD_BYTES];
    DSP_CHECK_EQ_INT(DSP_HISTORY_RECORD_BYTES, dsp_codec_encode_history(&rec, buf, sizeof buf));
    buf[1] = 1;
    fake_kv_put_raw(&kv, "h05", buf, DSP_HISTORY_RECORD_V1_BYTES);
    boot();
    dsp_dosing_recovery_t rv;
    DSP_CHECK_EQ_INT(DSP_DOSING_OK, dsp_dosing_recover(&d, nclk(), &rv));
    DSP_CHECK_EQ_INT(1, rv.automatic);
    const dsp_note_t *e = find_event(DSP_DOSEERR_INTERRUPTED);
    DSP_CHECK(e != NULL && e->value == 0); /* volume unknown: never reconstructed */
    DSP_CHECK_EQ_INT(DSP_HISTORY_INTERRUPTED, durable_history_state("old-a"));
}

/* ---- Randomized long run -------------------------------------------------------------------- */

static uint32_t rng = 9101u;
static uint32_t rnd(uint32_t bound)
{
    rng = rng * 1664525u + 1013904223u;
    return (rng >> 8) % bound;
}

static void test_randomized(void)
{
    fresh(true);
    make_ready();
    dsp_schedule_t set[2] = {sched("rnd-a", 8, 6000), sched("rnd-b", 20, 9000)};
    uint32_t true_remaining = CAP; /* what the pump actually took (worst case) */
    int prev_runs = 0;
    for (int step = 0; step < 3000; step++) {
        /* Advance: mostly to the next dose time, sometimes arbitrary, sometimes back. */
        uint32_t a = rnd(10);
        if (a < 6) {
            dsp_occurrence_t next;
            size_t idx;
            if (dsp_rec_next_of_set(set, 2, TZ, g_now, &idx, &next) == DSP_REC_OK) {
                g_now = next.at + (dsp_utc_t)rnd(700); /* sometimes late (missed) */
            }
        } else if (a < 9) {
            g_now += (dsp_utc_t)rnd(6 * 3600);
        } else {
            g_now -= (dsp_utc_t)rnd(2 * 3600); /* small backward correction */
        }
        g_trusted = rnd(20) != 0;
        if (rnd(8) == 0) {
            kv.power_cut_after_sets = 1 + (int)rnd(5);
        }
        uint32_t op = rnd(20);
        if (op < 15) {
            (void)dsp_dosing_process_due(&d, set, 2, &CLOCK);
        } else if (op < 18) {
            (void)manual_dose(&d, 1000 + rnd(9000), clk());
        } else if (op < 19) {
            /* The user physically refills when commanding it, whatever the storage outcome
             * (a cut refill is completed by the next recovery). */
            true_remaining = CAP;
            (void)dsp_dosing_refill(&d, CAP, CAP, nclk());
        } else {
            (void)dsp_dosing_set_readiness(&d, rnd(4) ? DSP_READINESS_READY
                                                      : DSP_READINESS_SERVICE_REQUIRED,
                                           NULL, NULL);
        }
        for (int i = prev_runs; i < pump.calls && i < MAX_RUNS; i++) {
            true_remaining = true_remaining > pump.volumes[i] ? true_remaining - pump.volumes[i] : 0;
        }
        prev_runs = pump.calls;
        if (kv.powered_off || rnd(10) == 0) {
            boot();
            dsp_dosing_recovery_t rv;
            (void)dsp_dosing_recover(&d, nclk(), &rv);
        }
        /* The estimate is never higher than what can really be left (conservative). */
        dsp_config_t c;
        if (dsp_storage_load_config(&st, &c) == DSP_STORAGE_OK) {
            DSP_CHECK(c.reservoir_remaining_ul <= true_remaining);
        }
    }
    DSP_CHECK_EQ_INT(0, pump.violations);
    DSP_CHECK(pump.calls > 50);
    /* Invariants over all runs: each occurrence at most once; starts >= min interval apart;
     * per local date at most max_daily (60000). */
    int runs = pump.calls < MAX_RUNS ? pump.calls : MAX_RUNS;
    for (int i = 0; i < runs; i++) {
        if (strncmp(pump.ids[i], "manual#", 7) != 0) {
            for (int j = 0; j < i; j++) {
                DSP_CHECK(strcmp(pump.ids[i], pump.ids[j]) != 0);
            }
        }
        if (i > 0) {
            DSP_CHECK(pump.starts[i] >= pump.starts[i - 1] + 60);
        }
        dsp_date_t di;
        DSP_CHECK(dsp_utc_to_local_date(TZ, pump.starts[i], &di));
        uint64_t sum = 0;
        for (int j = 0; j < runs; j++) {
            dsp_date_t dj;
            if (dsp_utc_to_local_date(TZ, pump.starts[j], &dj) && dsp_date_compare(di, dj) == 0) {
                sum += pump.volumes[j];
            }
        }
        DSP_CHECK(sum <= 60000);
    }
    printf("randomized: %d pump runs\n", pump.calls);
}

/* ---- M1.10b.2: anti-replay at the Dosing Service level (independent of the app layer) ---- */

static void test_manual_anti_replay_service_level(void)
{
    fresh(true);
    make_ready();
    /* Duplicates: decided requests never execute, without any write. */
    dsp_manual_outcome_t a = dsp_dosing_manual_request(&d, 1, 5, 1000, clk(), 0);
    DSP_CHECK_EQ_INT(DSP_MANUAL_EXECUTED, a.kind);
    DSP_CHECK_EQ_INT(DSP_MRES_DONE, a.state);
    int w = keylog_len;
    g_now += 120;
    dsp_manual_outcome_t b = dsp_dosing_manual_request(&d, 1, 5, 1000, clk(), 0);
    DSP_CHECK_EQ_INT(DSP_MANUAL_DECIDED, b.kind);
    DSP_CHECK_EQ_INT(DSP_MRES_DONE, b.state);
    b = dsp_dosing_manual_request(&d, 1, 4, 1000, clk(), 0); /* below the floor, never seen */
    DSP_CHECK_EQ_INT(DSP_MANUAL_DECIDED, b.kind);
    DSP_CHECK_EQ_INT(DSP_MRES_UNAVAILABLE, b.state);
    DSP_CHECK_EQ_INT(w, keylog_len);
    DSP_CHECK_EQ_INT(1, pump.calls);
    /* Invalid: not consumed, no write. */
    DSP_CHECK_EQ_INT(DSP_MANUAL_INVALID, dsp_dosing_manual_request(&d, 1, DSP_MANUAL_SEQ_MAX + 1u, 1000, clk(), 0).kind);
    DSP_CHECK_EQ_INT(DSP_MANUAL_INVALID, dsp_dosing_manual_request(&d, 1, 0, 1000, clk(), 0).kind);
    DSP_CHECK_EQ_INT(DSP_MANUAL_INVALID, dsp_dosing_manual_request(&d, 1, 6, 0, clk(), 0).kind);
    DSP_CHECK_EQ_INT(DSP_MANUAL_INVALID, dsp_dosing_manual_request(&d, DSP_MANUAL_SLOTS, 6, 1000, clk(), 0).kind);
    DSP_CHECK_EQ_INT(w, keylog_len);
    DSP_CHECK_EQ_INT(5, dsp_dosing_state(&d)->manual_hw[1]);
    DSP_CHECK_EQ_INT(6, dsp_dosing_manual_floor(&d, 1));
    DSP_CHECK_EQ_INT(1, dsp_dosing_manual_floor(&d, 0));
    /* The decision survives a restart (RAM lost). */
    DSP_CHECK(open_world());
    b = dsp_dosing_manual_request(&d, 1, 5, 1000, clk(), 0);
    DSP_CHECK_EQ_INT(DSP_MANUAL_DECIDED, b.kind);
    DSP_CHECK_EQ_INT(1, pump.calls);
}

/* While a marker is in flight (recovery not run yet), refusals are recorded durably but never
 * evict the marker's PENDING result; recovery then turns it into INTERRUPTED. */
static void test_manual_ring_keeps_in_flight_result(void)
{
    fresh(true);
    make_ready();
    dsp_dosing_state_t s = *dsp_dosing_state(&d);
    s.flags |= DSP_DOSING_F_MANUAL_ACTIVE;
    s.manual_volume_ul = 1000;
    s.manual_started_at = g_now;
    s.next_seq = s.next_seq != 0 ? s.next_seq : 1;
    s.manual_seq = s.next_seq++;
    s.manual_hw[0] = 3;
    s.marker_slot = 0;
    s.marker_req = 3;
    s.ring[0] = (dsp_manual_result_t){0, DSP_MRES_PENDING, 3, 1000};
    s.ring_next = 1;
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_save_dosing(&st, &s));
    DSP_CHECK(open_world()); /* no recovery yet */
    for (uint64_t q = 10; q < 10 + 2 * DSP_MANUAL_RING; q++) {
        dsp_manual_outcome_t o = dsp_dosing_manual_request(&d, 0, q, 1000, clk(), 0);
        DSP_CHECK_EQ_INT(DSP_MANUAL_REFUSED, o.kind); /* the in-flight marker is a gate */
    }
    const dsp_manual_result_t *e = dsp_dosing_state_find_result(dsp_dosing_state(&d), 0, 3);
    DSP_CHECK(e != NULL && e->state == DSP_MRES_PENDING);
    dsp_notify_clock_t nc = {g_now, true};
    dsp_dosing_recovery_t rec;
    DSP_CHECK_EQ_INT(DSP_DOSING_OK, dsp_dosing_recover(&d, nc, &rec));
    DSP_CHECK_EQ_INT(1, rec.manual);
    e = dsp_dosing_state_find_result(dsp_dosing_state(&d), 0, 3);
    DSP_CHECK(e != NULL && e->state == DSP_MRES_INTERRUPTED);
    DSP_CHECK_EQ_INT(0, pump.calls);
    /* A marker request above its slot's high-water mark is an invalid record. */
    s = *dsp_dosing_state(&d);
    s.flags |= DSP_DOSING_F_MANUAL_ACTIVE;
    s.manual_volume_ul = 1000;
    s.manual_started_at = g_now;
    s.next_seq = s.next_seq != 0 ? s.next_seq : 1;
    s.manual_seq = s.next_seq++;
    s.marker_slot = 1;
    s.marker_req = s.manual_hw[1] + 1u;
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_INVALID, dsp_storage_save_dosing(&st, &s));
}

int main(void)
{
    DSP_RUN(test_defaults_and_readiness);
    DSP_RUN(test_pump_config_required);
    DSP_RUN(test_runtime_arithmetic);
    DSP_RUN(test_automatic_dose_order_and_volume);
    DSP_RUN(test_process_due_order_and_window);
    DSP_RUN(test_limits);
    DSP_RUN(test_manual_anti_replay_service_level);
    DSP_RUN(test_manual_ring_keeps_in_flight_result);
    DSP_RUN(test_daily_ledger_and_clock_changes);
    DSP_RUN(test_blocked_latch);
    DSP_RUN(test_missing_configuration_never_blocks);
    DSP_RUN(test_refill_power_cut_every_write);
    DSP_RUN(test_pump_failures);
    DSP_RUN(test_storage_failure_at_every_write);
    DSP_RUN(test_unpersisted_report_keeps_durable_source);
    DSP_RUN(test_unpersisted_report_without_durable_source);
    DSP_RUN(test_power_loss_automatic);
    DSP_RUN(test_power_loss_manual);
    DSP_RUN(test_recovery_of_format1_record);
    DSP_RUN(test_randomized);
    return dsp_test_finish();
}
