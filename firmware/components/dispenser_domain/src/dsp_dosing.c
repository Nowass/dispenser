#include "dsp_dosing.h"

#include "dsp_wakeup.h"

#include <string.h>

#include "dsp_reservoir.h"

/* ---- Helpers -------------------------------------------------------------------------- */

static dsp_notify_clock_t nclock_of(dsp_clock_t c)
{
    dsp_notify_clock_t n = {c.now, c.valid};
    return n;
}

static uint64_t fnv64(uint64_t h, const void *data, size_t len)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

static uint64_t fnv64_u64(uint64_t h, uint64_t v)
{
    uint8_t b[8];
    for (int i = 0; i < 8; i++) {
        b[i] = (uint8_t)(v >> (8 * i));
    }
    return fnv64(h, b, sizeof b);
}

uint64_t dsp_dosing_key_automatic(const dsp_history_record_t *rec)
{
    uint64_t h = fnv64(0xCBF29CE484222325ull, "A", 1);
    h = fnv64(h, rec->schedule_id, strlen(rec->schedule_id));
    h = fnv64_u64(h, ((uint64_t)(uint16_t)rec->local_date.year << 16) |
                         ((uint64_t)rec->local_date.month << 8) | rec->local_date.day);
    h = fnv64_u64(h, (uint64_t)rec->claimed_at);
    h = fnv64_u64(h, rec->volume_ul);
    return h != 0 ? h : 1;
}

uint64_t dsp_dosing_key_manual(uint32_t seq, dsp_utc_t started_at, uint32_t volume_ul)
{
    uint64_t h = fnv64(0xCBF29CE484222325ull, "M", 1);
    h = fnv64_u64(h, seq);
    h = fnv64_u64(h, (uint64_t)started_at);
    h = fnv64_u64(h, volume_ul);
    return h != 0 ? h : 1;
}

bool dsp_dosing_runtime_ms(uint32_t volume_ul, uint32_t flow_ul_per_s, uint32_t *runtime_ms)
{
    if (volume_ul == 0 || flow_ul_per_s == 0 || runtime_ms == NULL) {
        return false;
    }
    uint64_t r = ((uint64_t)volume_ul * 1000u + flow_ul_per_s - 1u) / flow_ul_per_s;
    if (r > UINT32_MAX) {
        return false;
    }
    *runtime_ms = (uint32_t)r;
    return true;
}

/* Writes the next dosing state; RAM follows only a confirmed write. A failure makes the
 * rest of the wake DEGRADED (the write may or may not have landed). */
static bool save_state(dsp_dosing_t *d, const dsp_dosing_state_t *next)
{
    if (dsp_storage_save_dosing(d->st, next) != DSP_STORAGE_OK) {
        d->degraded = true;
        return false;
    }
    d->s = *next;
    return true;
}

/* Keyed report of a dose-bound error. Returns false if the notification record could not be
 * persisted: the report then exists only in RAM (retried at the next notify write of this
 * wake), so the caller must KEEP its durable source (STARTED record / manual marker) for
 * boot recovery, and the rest of the wake is DEGRADED. The key makes both paths converge on
 * exactly one report. */
static bool report(dsp_dosing_t *d, uint16_t code, bool manual, uint32_t volume, uint64_t key,
                   dsp_notify_clock_t clock)
{
    dsp_dosing_error_t e = {code, manual, volume};
    if (dsp_notify_report_dosing_error_once(d->n, &e, key, clock, NULL) ==
        DSP_NOTIFY_ERR_STORAGE) {
        d->degraded = true;
        return false;
    }
    return true;
}

/* ---- Phase C event history (informational; see dsp_evlog.h) ----------------------------- */

void dsp_dosing_set_event_log(dsp_dosing_t *d, dsp_evlog_t *log, uint8_t pump_flags)
{
    if (d != NULL) {
        d->log = log;
        d->ev_pump_flags = (uint8_t)(pump_flags & (DSP_EVF_PUMP_SIMULATED | DSP_EVF_PUMP_BENCH_GPIO));
    }
}

/* Appends a dose event AFTER its durable transition. The result is deliberately ignored: a
 * failed append is counted by the log and must not influence any dosing decision. before/after
 * NULL = reservoir values unknown. */
static void log_dose(dsp_dosing_t *d, bool manual, uint8_t result, dsp_utc_t at, bool trusted,
                     uint32_t volume, const uint32_t *before, const uint32_t *after,
                     uint32_t capacity, uint64_t key, const char *schedule_id)
{
    if (d->log == NULL) {
        return;
    }
    dsp_event_t e;
    memset(&e, 0, sizeof e);
    e.type = manual ? DSP_EV_DOSE_MANUAL : DSP_EV_DOSE_SCHEDULED;
    e.result = result;
    e.flags = d->ev_pump_flags | (trusted ? DSP_EVF_CLOCK_TRUSTED : 0u);
    e.at = at;
    e.volume_ul = volume;
    if (before != NULL && after != NULL) {
        e.before_ul = *before;
        e.after_ul = *after;
    } else {
        e.flags |= DSP_EVF_RESERVOIR_UNKNOWN;
    }
    e.capacity_ul = capacity;
    e.key = key;
    if (!manual && schedule_id != NULL) {
        memcpy(e.schedule_id, schedule_id, sizeof e.schedule_id);
        e.schedule_id[sizeof e.schedule_id - 1] = '\0';
    }
    (void)dsp_evlog_append(d->log, &e, NULL);
}

static uint8_t pump_result(dsp_pump_result_t p)
{
    return p == DSP_PUMP_OK ? DSP_EVR_OK
                            : p == DSP_PUMP_START_FAILED ? DSP_EVR_START_FAILED : DSP_EVR_PUMP_FAILED;
}

/* Gates that need neither the clock nor the configuration. */
static uint32_t state_gates(const dsp_dosing_t *d)
{
    uint32_t g = 0;
    if (!d->loaded) {
        return DSP_DOSING_G_NOT_LOADED;
    }
    if (d->degraded) {
        g |= DSP_DOSING_G_DEGRADED;
    }
    if (d->s.readiness != DSP_READINESS_READY) {
        g |= DSP_DOSING_G_NOT_READY;
    }
    if (d->s.flags & DSP_DOSING_F_BLOCKED) {
        g |= DSP_DOSING_G_BLOCKED;
    }
    if (d->s.flags & DSP_DOSING_F_REFILL_PENDING) {
        g |= DSP_DOSING_G_REFILL_PENDING;
    }
    if (d->s.flags & DSP_DOSING_F_MANUAL_ACTIVE) {
        g |= DSP_DOSING_G_MANUAL_ACTIVE;
    }
    if (d->cred_inhibit) {
        g |= DSP_DOSING_G_CREDENTIALS;
    }
    return g;
}

static uint32_t config_gates(const dsp_dosing_t *d, const dsp_config_t *cfg, uint32_t *problems)
{
    uint32_t g = 0;
    uint32_t p = dsp_config_pump_problems(cfg, d->hw_max_runtime_ms);
    if (p != 0) {
        g |= DSP_DOSING_G_PUMP_CONFIG;
    }
    if (cfg->reservoir_capacity_ul == 0) {
        g |= DSP_DOSING_G_RESERVOIR_UNCONFIGURED;
    }
    if (problems != NULL) {
        *problems = p;
    }
    return g;
}

/* Limits for volume at clock.now; on success the new ledger values. */
static uint32_t check_limits(const dsp_dosing_t *d, const dsp_config_t *cfg, uint32_t volume,
                             dsp_utc_t now, uint32_t *runtime, dsp_date_t *ledger_date,
                             uint32_t *ledger_sum, bool *date_ok)
{
    uint32_t l = 0;
    *date_ok = true;
    if (volume == 0 || volume > cfg->max_dose_ul) {
        l |= DSP_DOSING_L_DOSE;
    }
    uint32_t rt = 0;
    if (!dsp_dosing_runtime_ms(volume, cfg->pump_flow_ul_per_s, &rt) || rt > cfg->max_runtime_ms ||
        rt > d->hw_max_runtime_ms) {
        l |= DSP_DOSING_L_RUNTIME;
    }
    *runtime = rt;
    dsp_date_t today;
    if (!dsp_utc_to_local_date(d->tz, now, &today)) {
        *date_ok = false;
        return l;
    }
    /* Daily total per local date. The ledger keeps the HIGHEST date seen: a clock moved
     * back to an earlier date keeps counting against it (never a fresh budget). */
    bool none = d->s.ledger_date.year == 0;
    uint32_t base = 0;
    dsp_date_t date = today;
    if (!none && dsp_date_compare(today, d->s.ledger_date) <= 0) {
        base = d->s.ledger_sum_ul;
        date = d->s.ledger_date;
    }
    if ((uint64_t)base + volume > cfg->max_daily_ul) {
        l |= DSP_DOSING_L_DAILY;
    }
    *ledger_date = date;
    *ledger_sum = (uint64_t)base + volume > UINT32_MAX ? UINT32_MAX : base + volume;
    /* Minimum interval since the last start; a clock behind the last start waits. */
    if (d->s.last_start_utc != 0 &&
        (now < d->s.last_start_utc ||
         (uint64_t)(now - d->s.last_start_utc) < (uint64_t)cfg->min_interval_s)) {
        l |= DSP_DOSING_L_INTERVAL;
    }
    return l;
}

/* ---- Public API ------------------------------------------------------------------------ */

dsp_dosing_status_t dsp_dosing_init(dsp_dosing_t *d, dsp_storage_t *st, dsp_exec_t *ex,
                                    dsp_notify_t *n, const dsp_pump_port_t *pump,
                                    const dsp_tz_t *tz, uint32_t hw_max_runtime_ms)
{
    if (d == NULL) {
        return DSP_DOSING_ERR_ARG;
    }
    memset(d, 0, sizeof *d);
    if (st == NULL || ex == NULL || n == NULL || pump == NULL || pump->run == NULL ||
        pump->force_off == NULL || !dsp_tz_is_valid(tz)) {
        return DSP_DOSING_ERR_ARG;
    }
    d->st = st;
    d->ex = ex;
    d->n = n;
    d->pump = pump;
    d->tz = tz;
    d->hw_max_runtime_ms = hw_max_runtime_ms;
    d->adm_limit = DSP_MANUAL_DECISIONS_PER_DAY_DEFAULT;
    pump->force_off(pump->ctx); /* safe-off before anything else */
    if (dsp_storage_load_dosing(st, &d->s) != DSP_STORAGE_OK) {
        return DSP_DOSING_ERR_STORAGE; /* not loaded: every gate stays closed */
    }
    d->loaded = true;
    return DSP_DOSING_OK;
}

const dsp_dosing_state_t *dsp_dosing_state(const dsp_dosing_t *d)
{
    return d != NULL && d->loaded ? &d->s : NULL;
}

uint32_t dsp_dosing_gates(dsp_dosing_t *d, bool clock_trusted, uint32_t *pump_problems)
{
    if (pump_problems != NULL) {
        *pump_problems = 0;
    }
    if (d == NULL) {
        return DSP_DOSING_G_NOT_LOADED;
    }
    uint32_t g = state_gates(d);
    if (!clock_trusted) {
        g |= DSP_DOSING_G_CLOCK;
    }
    if (d->loaded) {
        dsp_config_t cfg;
        if (dsp_storage_load_config(d->st, &cfg) != DSP_STORAGE_OK) {
            g |= DSP_DOSING_G_DEGRADED;
        } else {
            g |= config_gates(d, &cfg, pump_problems);
        }
    }
    return g;
}

dsp_notify_status_t dsp_dosing_observe_reservoir(dsp_dosing_t *d, dsp_notify_clock_t clock)
{
    if (d == NULL || !d->loaded) {
        return DSP_NOTIFY_ERR_ARG;
    }
    dsp_config_t cfg;
    if (dsp_storage_load_config(d->st, &cfg) != DSP_STORAGE_OK) {
        return DSP_NOTIFY_ERR_STORAGE;
    }
    return dsp_notify_observe_reservoir(d->n, &cfg, (d->s.flags & DSP_DOSING_F_BLOCKED) != 0, clock);
}

/* Gates and configuration (front part of automatic and manual doses). */
static bool prepare_gates(dsp_dosing_t *d, dsp_clock_t clock, uint32_t volume, dsp_config_t *cfg,
                          dsp_dosing_result_t *r)
{
    r->volume_ul = volume;
    r->gates = state_gates(d);
    if (!clock.valid) {
        r->gates |= DSP_DOSING_G_CLOCK;
    }
    if (d->loaded) {
        if (dsp_storage_load_config(d->st, cfg) != DSP_STORAGE_OK) {
            d->degraded = true;
            r->status = DSP_DOSING_ERR_STORAGE;
            return false;
        }
        r->gates |= config_gates(d, cfg, &r->pump_problems);
    }
    if (r->gates != 0) {
        r->status = DSP_DOSING_REFUSED;
        return false;
    }
    return true;
}

/* Safety limits; on success the new ledger values. */
static bool prepare_limits(dsp_dosing_t *d, dsp_clock_t clock, uint32_t volume,
                           const dsp_config_t *cfg, dsp_dosing_result_t *r,
                           dsp_date_t *ledger_date, uint32_t *ledger_sum)
{
    bool date_ok;
    r->limits = check_limits(d, cfg, volume, clock.now, &r->runtime_ms, ledger_date, ledger_sum,
                             &date_ok);
    if (!date_ok) {
        r->gates |= DSP_DOSING_G_CLOCK;
        r->status = DSP_DOSING_REFUSED;
        return false;
    }
    if (r->limits != 0) {
        r->status = DSP_DOSING_LIMIT;
        return false;
    }
    return true;
}

static dsp_dosing_status_t from_exec_storage(dsp_dosing_t *d, dsp_exec_status_t es)
{
    if (es == DSP_EXEC_ERR_STORAGE_READ || es == DSP_EXEC_ERR_STORAGE_WRITE ||
        es == DSP_EXEC_ERR_STORAGE_COMMIT || es == DSP_EXEC_ERR_PERSIST_CONFIRM ||
        es == DSP_EXEC_ERR_INTERNAL) {
        d->degraded = true;
        return DSP_DOSING_ERR_STORAGE;
    }
    return DSP_DOSING_NOT_ELIGIBLE;
}

dsp_dosing_result_t dsp_dosing_run_automatic(dsp_dosing_t *d, const dsp_schedule_t *schedule,
                                             const dsp_occurrence_t *occ, dsp_clock_t clock)
{
    dsp_dosing_result_t r;
    memset(&r, 0, sizeof r);
    if (d == NULL || schedule == NULL || occ == NULL) {
        r.status = DSP_DOSING_ERR_ARG;
        return r;
    }
    dsp_notify_clock_t nc = nclock_of(clock);
    dsp_config_t cfg;
    dsp_date_t ledger_date;
    uint32_t ledger_sum = 0;
    if (!prepare_gates(d, clock, schedule->amount_ul, &cfg, &r)) {
        return r;
    }
    r.exec = dsp_exec_check(d->ex, schedule, occ, clock);
    if (r.exec != DSP_EXEC_OK) {
        r.status = from_exec_storage(d, r.exec);
        return r;
    }
    if (!prepare_limits(d, clock, schedule->amount_ul, &cfg, &r, &ledger_date, &ledger_sum)) {
        return r;
    }
    uint32_t volume = schedule->amount_ul;
    if (!dsp_reservoir_covers(&cfg, volume)) {
        /* FR-022: set the latch first (restricts), then let the engine report BLOCKED. */
        if (!(d->s.flags & DSP_DOSING_F_BLOCKED)) {
            dsp_dosing_state_t next = d->s;
            next.flags |= DSP_DOSING_F_BLOCKED;
            if (!save_state(d, &next)) {
                r.status = DSP_DOSING_ERR_STORAGE;
                return r;
            }
        }
        (void)dsp_notify_observe_reservoir(d->n, &cfg, true, nc);
        r.status = DSP_DOSING_NOT_COVERED;
        return r;
    }
    /* 1. Durable claim (STARTED with the claimed volume). */
    dsp_exec_token_t token;
    r.exec = dsp_exec_claim(d->ex, schedule, occ, clock, &token);
    if (r.exec != DSP_EXEC_OK) {
        r.status = from_exec_storage(d, r.exec);
        return r;
    }
    uint64_t key = dsp_dosing_key_automatic(&token.record);
    /* 2. Daily ledger and last start (restrict). */
    dsp_dosing_state_t next = d->s;
    next.ledger_date = ledger_date;
    next.ledger_sum_ul = ledger_sum;
    next.last_start_utc = clock.now;
    uint32_t before = cfg.reservoir_remaining_ul;
    if (!save_state(d, &next)) {
        if (report(d, DSP_DOSEERR_STORAGE, false, volume, key, nc) &&
            dsp_exec_finish(d->ex, &token, DSP_HISTORY_FAILED) == DSP_EXEC_OK) {
            /* nothing consumed: the estimate is unchanged */
            log_dose(d, false, DSP_EVR_ABORTED, clock.now, clock.valid, volume, &before, &before,
                     cfg.reservoir_capacity_ul, key, schedule->id);
        } /* else: stays STARTED, the durable source for boot recovery */
        r.status = DSP_DOSING_ERR_STORAGE;
        return r;
    }
    /* 3. Durable consumption -> single-use permit (enables). */
    dsp_dose_permit_t permit;
    dsp_config_t after_cfg;
    if (dsp_reservoir_consume(d->st, volume, DSP_DOSE_AUTOMATIC, &after_cfg, &permit) !=
        DSP_STORAGE_OK) {
        d->degraded = true;
        if (report(d, DSP_DOSEERR_STORAGE, false, volume, key, nc) &&
            dsp_exec_finish(d->ex, &token, DSP_HISTORY_FAILED) == DSP_EXEC_OK) {
            /* the consumption may or may not have landed: reservoir unknown */
            log_dose(d, false, DSP_EVR_ABORTED, clock.now, clock.valid, volume, NULL, NULL,
                     cfg.reservoir_capacity_ul, key, schedule->id);
        } /* else: stays STARTED */
        r.status = DSP_DOSING_ERR_STORAGE;
        return r;
    }
    uint32_t after = after_cfg.reservoir_remaining_ul;
    /* 4. Redeem the permit, then (and only then) the pump. */
    uint32_t permitted = 0;
    if (!dsp_dose_permit_take(&permit, &permitted) || permitted != volume) {
        d->degraded = true; /* cannot happen; never actuate on an inconsistent permit */
        if (dsp_exec_finish(d->ex, &token, DSP_HISTORY_FAILED) == DSP_EXEC_OK) {
            log_dose(d, false, DSP_EVR_ABORTED, clock.now, clock.valid, volume, &before, &after,
                     cfg.reservoir_capacity_ul, key, schedule->id);
        }
        r.status = DSP_DOSING_ERR_STORAGE;
        return r;
    }
    r.pumped = true;
    r.pump = d->pump->run(d->pump->ctx, r.runtime_ms);
    d->pump->force_off(d->pump->ctx);
    /* 5. Outcome: a fault is reported (keyed to this dose, uncertainty set) BEFORE the
     *    history record is finished, so a cut in between repeats nothing at recovery. */
    bool reported = true;
    if (r.pump != DSP_PUMP_OK) {
        reported = report(d, r.pump == DSP_PUMP_START_FAILED ? DSP_DOSEERR_START_FAILED
                                                             : DSP_DOSEERR_PUMP_FAILED,
                          false, volume, key, nc);
    }
    if (reported) {
        dsp_exec_status_t fs = dsp_exec_finish(d->ex, &token, r.pump == DSP_PUMP_OK
                                                                  ? DSP_HISTORY_COMPLETED
                                                                  : DSP_HISTORY_FAILED);
        if (fs != DSP_EXEC_OK) {
            d->degraded = true; /* stays STARTED: reported as interrupted at the next boot */
        } else {
            log_dose(d, false, pump_result(r.pump), clock.now, clock.valid, volume, &before, &after,
                     cfg.reservoir_capacity_ul, key, schedule->id);
        }
    } /* else: the fault report is not durable; the record stays STARTED for recovery */
    (void)dsp_dosing_observe_reservoir(d, nc);
    r.status = r.pump == DSP_PUMP_OK ? DSP_DOSING_OK : DSP_DOSING_PUMP_FAILED;
    return r;
}

/* ---- Admission (M1.10b.3) ------------------------------------------------------------------ */

typedef struct {
    dsp_date_t date;
    uint16_t count; /* count AFTER this decision */
} adm_t;

void dsp_dosing_set_admission_limit(dsp_dosing_t *d, uint16_t per_day)
{
    if (d == NULL) {
        return;
    }
    if (per_day == 0) {
        per_day = DSP_MANUAL_DECISIONS_PER_DAY_DEFAULT;
    }
    d->adm_limit = per_day > DSP_MANUAL_DECISIONS_PER_DAY_MAX ? DSP_MANUAL_DECISIONS_PER_DAY_MAX
                                                             : per_day;
}

void dsp_dosing_set_credentials_inhibit(dsp_dosing_t *d, bool inhibit)
{
    if (d != NULL) {
        d->cred_inhibit = inhibit;
    }
}

/* Effective count now and the values the next decision write stores. The count restarts only
 * for a trusted local date AFTER the stored one (no restart without trust or after rollback). */
static bool admission(const dsp_dosing_t *d, dsp_clock_t clock, adm_t *next, uint32_t *retry_s)
{
    dsp_date_t today;
    bool trusted = clock.valid && dsp_utc_to_local_date(d->tz, clock.now, &today);
    bool restart = trusted && (d->s.adm_date.year == 0 || dsp_date_compare(today, d->s.adm_date) > 0);
    uint16_t count = restart ? 0 : d->s.adm_count;
    uint16_t limit = d->adm_limit != 0 ? d->adm_limit : DSP_MANUAL_DECISIONS_PER_DAY_DEFAULT;
    if (retry_s != NULL) {
        *retry_s = 0;
        if (trusted) {
            /* seconds until the next local midnight (approximate across DST: refreshed by the
             * date rule, never early) */
            int32_t off = 0;
            if (dsp_tz_offset_at(d->tz, clock.now, &off)) {
                int64_t local = clock.now + off;
                int64_t into = ((local % 86400) + 86400) % 86400;
                *retry_s = (uint32_t)(86400 - into);
            }
        }
    }
    if (count >= limit) {
        return false;
    }
    if (next != NULL) {
        next->date = restart ? today : d->s.adm_date;
        next->count = (uint16_t)(count + 1);
    }
    return true;
}

bool dsp_dosing_manual_admissible(const dsp_dosing_t *d, dsp_clock_t clock, uint32_t *retry_s)
{
    if (retry_s != NULL) {
        *retry_s = 0;
    }
    return d != NULL && d->loaded && admission(d, clock, NULL, retry_s);
}

static void apply_admission(dsp_dosing_state_t *next, adm_t a)
{
    next->adm_date = a.date;
    next->adm_count = a.count;
}

/* Writes the result of request (slot, req) into the ring: updates its entry, or takes the next
 * entry that is not the active marker's PENDING one (the oldest is evicted). */
static void ring_set(dsp_dosing_state_t *s, uint8_t slot, uint64_t req, uint8_t state,
                     uint32_t detail)
{
    for (size_t i = 0; i < DSP_MANUAL_RING; i++) {
        if (s->ring[i].seq == req && s->ring[i].slot == slot) {
            s->ring[i].state = state;
            s->ring[i].detail = detail;
            return;
        }
    }
    for (size_t k = 0; k < DSP_MANUAL_RING; k++) {
        size_t i = (s->ring_next + k) % DSP_MANUAL_RING;
        if (s->ring[i].seq != 0 && s->ring[i].state == DSP_MRES_PENDING) {
            continue; /* never evict the in-flight request */
        }
        s->ring[i].slot = slot;
        s->ring[i].state = state;
        s->ring[i].seq = req;
        s->ring[i].detail = detail;
        s->ring_next = (uint8_t)((i + 1) % DSP_MANUAL_RING);
        return;
    }
}

/* Clears the manual marker in *next and records the request's final state (same write). */
static void clear_marker(dsp_dosing_state_t *next, uint8_t final_state)
{
    if (next->marker_req != 0) {
        ring_set(next, next->marker_slot, next->marker_req, final_state, next->manual_volume_ul);
    }
    next->flags &= (uint8_t)~DSP_DOSING_F_MANUAL_ACTIVE;
    next->manual_volume_ul = 0;
    next->manual_started_at = 0;
    next->manual_seq = 0;
    next->marker_slot = 0;
    next->marker_req = 0;
}

/* Executes an accepted request (gates, limits and coverage already checked; *cfg, ledger
 * values from that check). Write 1 makes the decision durable before anything else. */
static dsp_dosing_result_t manual_execute(dsp_dosing_t *d, uint8_t slot, uint64_t req,
                                          uint32_t volume_ul, dsp_clock_t clock,
                                          dsp_dosing_result_t r, dsp_date_t ledger_date,
                                          uint32_t ledger_sum, adm_t adm, bool *decided)
{
    *decided = false;
    dsp_notify_clock_t nc = nclock_of(clock);
    dsp_exec_token_t token;
    r.exec = dsp_exec_manual_begin(d->ex, &token);
    if (r.exec != DSP_EXEC_OK) {
        r.status = DSP_DOSING_NOT_ELIGIBLE; /* BUSY: nothing decided */
        return r;
    }
    /* 1. Decision + marker + ledger + last start in one write (restricts). */
    uint32_t seq = d->s.next_seq != 0 ? d->s.next_seq : 1;
    dsp_dosing_state_t next = d->s;
    next.flags |= DSP_DOSING_F_MANUAL_ACTIVE;
    next.manual_volume_ul = volume_ul;
    next.manual_started_at = clock.now;
    next.manual_seq = seq;
    next.next_seq = seq + 1;
    next.ledger_date = ledger_date;
    next.ledger_sum_ul = ledger_sum;
    next.last_start_utc = clock.now;
    next.manual_hw[slot] = req;
    next.marker_slot = slot;
    next.marker_req = req;
    ring_set(&next, slot, req, DSP_MRES_PENDING, volume_ul);
    apply_admission(&next, adm); /* counted in the decision write itself */
    if (!save_state(d, &next)) {
        (void)dsp_exec_finish(d->ex, &token, DSP_HISTORY_FAILED);
        r.status = DSP_DOSING_ERR_STORAGE; /* not durable (or unknown): not executed */
        return r;
    }
    *decided = true;
    uint64_t key = dsp_dosing_key_manual(seq, clock.now, volume_ul);
    /* 2. Durable consumption -> permit. */
    dsp_dose_permit_t permit;
    dsp_config_t after_cfg;
    if (dsp_reservoir_consume(d->st, volume_ul, DSP_DOSE_MANUAL, &after_cfg, &permit) !=
        DSP_STORAGE_OK) {
        d->degraded = true;
        (void)report(d, DSP_DOSEERR_STORAGE, true, volume_ul, key, nc);
        (void)dsp_exec_finish(d->ex, &token, DSP_HISTORY_FAILED);
        r.status = DSP_DOSING_ERR_STORAGE; /* the marker stays: recovery handles it */
        return r;
    }
    uint32_t permitted = 0;
    if (!dsp_dose_permit_take(&permit, &permitted) || permitted != volume_ul) {
        d->degraded = true;
        (void)dsp_exec_finish(d->ex, &token, DSP_HISTORY_FAILED);
        r.status = DSP_DOSING_ERR_STORAGE;
        return r;
    }
    r.pumped = true;
    r.pump = d->pump->run(d->pump->ctx, r.runtime_ms);
    d->pump->force_off(d->pump->ctx);
    bool reported = true;
    if (r.pump != DSP_PUMP_OK) {
        reported = report(d, r.pump == DSP_PUMP_START_FAILED ? DSP_DOSEERR_START_FAILED
                                                             : DSP_DOSEERR_PUMP_FAILED,
                          true, volume_ul, key, nc);
    }
    /* 3. Clear the marker + final result (a failure leaves it for recovery: reported once,
     *    keyed; the request stays decided through manual_hw). */
    next = d->s; /* reused for write 3 (one state copy on the stack) */
    clear_marker(&next, r.pump == DSP_PUMP_OK ? DSP_MRES_DONE : DSP_MRES_FAILED);
    if (reported && save_state(d, &next)) {
        /* Coverage was checked before the decision (remaining >= volume), so the consumption
         * did not saturate and before = after + volume exactly. */
        uint32_t after = after_cfg.reservoir_remaining_ul;
        uint32_t before = after <= UINT32_MAX - volume_ul ? after + volume_ul : UINT32_MAX;
        log_dose(d, true, pump_result(r.pump), clock.now, clock.valid, volume_ul, &before, &after,
                 after_cfg.reservoir_capacity_ul, key, NULL);
    } /* else: the fault report or write 3 is not durable; the marker stays for recovery */
    (void)dsp_exec_finish(d->ex, &token,
                          r.pump == DSP_PUMP_OK ? DSP_HISTORY_COMPLETED : DSP_HISTORY_FAILED);
    (void)dsp_dosing_observe_reservoir(d, nc);
    r.status = r.pump == DSP_PUMP_OK ? DSP_DOSING_OK : DSP_DOSING_PUMP_FAILED;
    return r;
}

static bool request_valid(uint8_t slot, uint64_t seq, uint32_t volume_ul)
{
    return slot < DSP_MANUAL_SLOTS && seq != 0 && seq <= DSP_MANUAL_SEQ_MAX && volume_ul != 0;
}

static dsp_manual_outcome_t outcome(dsp_manual_kind_t kind, uint8_t state, uint32_t detail)
{
    dsp_manual_outcome_t o;
    memset(&o, 0, sizeof o);
    o.kind = kind;
    o.state = state;
    o.detail = detail;
    return o;
}

dsp_manual_outcome_t dsp_dosing_manual_lookup(const dsp_dosing_t *d, uint8_t slot, uint64_t seq)
{
    if (slot >= DSP_MANUAL_SLOTS || seq == 0 || seq > DSP_MANUAL_SEQ_MAX) {
        return outcome(DSP_MANUAL_INVALID, DSP_MRES_NOT_DECIDED, 0);
    }
    if (d == NULL || !d->loaded) {
        return outcome(DSP_MANUAL_NOT_DURABLE, DSP_MRES_NOT_DECIDED, 0);
    }
    if (seq > d->s.manual_hw[slot]) {
        return outcome(DSP_MANUAL_NOT_DURABLE, DSP_MRES_NOT_DECIDED, 0);
    }
    const dsp_manual_result_t *e = dsp_dosing_state_find_result(&d->s, slot, seq);
    return e != NULL ? outcome(DSP_MANUAL_DECIDED, e->state, e->detail)
                     : outcome(DSP_MANUAL_DECIDED, DSP_MRES_UNAVAILABLE, 0);
}

uint64_t dsp_dosing_manual_floor(const dsp_dosing_t *d, uint8_t slot)
{
    if (d == NULL || !d->loaded || slot >= DSP_MANUAL_SLOTS) {
        return 0;
    }
    return d->s.manual_hw[slot] + 1u; /* <= 2^63: no overflow */
}

/* Durable refusal: manual_hw[slot] = seq and the ring entry in one write. */
/* always inline: its state copy then shares stack slots with the disjoint execution path */
__attribute__((always_inline)) static inline dsp_manual_outcome_t
refuse(dsp_dosing_t *d, uint8_t slot, uint64_t seq, uint32_t code, dsp_dosing_result_t dose,
       adm_t adm)
{
    dsp_dosing_state_t next = d->s;
    next.manual_hw[slot] = seq;
    ring_set(&next, slot, seq, DSP_MRES_REFUSED, code);
    apply_admission(&next, adm); /* counted in the refusal write itself */
    dsp_manual_outcome_t o;
    if (save_state(d, &next)) {
        o = outcome(DSP_MANUAL_REFUSED, DSP_MRES_REFUSED, code);
    } else {
        o = outcome(DSP_MANUAL_NOT_DURABLE, DSP_MRES_NOT_DECIDED, 0); /* nothing promised */
        dose.status = DSP_DOSING_ERR_STORAGE;
    }
    o.dose = dose;
    return o;
}

dsp_manual_outcome_t dsp_dosing_manual_request(dsp_dosing_t *d, uint8_t slot, uint64_t seq,
                                               uint32_t volume_ul, dsp_clock_t clock,
                                               uint32_t app_refusal)
{
    if (!request_valid(slot, seq, volume_ul)) {
        return outcome(DSP_MANUAL_INVALID, DSP_MRES_NOT_DECIDED, 0);
    }
    if (d == NULL || !d->loaded) {
        dsp_manual_outcome_t o = outcome(DSP_MANUAL_NOT_DURABLE, DSP_MRES_NOT_DECIDED, 0);
        o.dose.status = DSP_DOSING_ERR_STORAGE;
        o.dose.gates = DSP_DOSING_G_NOT_LOADED;
        return o;
    }
    /* Rule 1: already decided -> never executed (result if retained). */
    if (seq <= d->s.manual_hw[slot]) {
        return dsp_dosing_manual_lookup(d, slot, seq);
    }
    dsp_dosing_result_t r;
    memset(&r, 0, sizeof r);
    if (d->degraded) { /* no write is attempted after a storage failure in this wake */
        dsp_manual_outcome_t o = outcome(DSP_MANUAL_NOT_DURABLE, DSP_MRES_NOT_DECIDED, 0);
        o.dose.status = DSP_DOSING_ERR_STORAGE;
        o.dose.gates = DSP_DOSING_G_DEGRADED;
        return o;
    }
    /* Admission (M1.10b.3): after the decided lookup, before any decision; nothing written. */
    adm_t adm;
    if (!admission(d, clock, &adm, NULL)) {
        return outcome(DSP_MANUAL_RATE_LIMITED, DSP_MRES_NOT_DECIDED, 0);
    }
    /* Rule 2: evaluated exactly once. */
    if (app_refusal != 0) {
        r.status = DSP_DOSING_REFUSED;
        return refuse(d, slot, seq, app_refusal, r, adm);
    }
    dsp_config_t cfg;
    dsp_date_t ledger_date;
    uint32_t ledger_sum = 0;
    if (!prepare_gates(d, clock, volume_ul, &cfg, &r)) {
        if (r.status == DSP_DOSING_ERR_STORAGE) { /* configuration unreadable */
            dsp_manual_outcome_t o = outcome(DSP_MANUAL_NOT_DURABLE, DSP_MRES_NOT_DECIDED, 0);
            o.dose = r;
            return o;
        }
        return refuse(d, slot, seq, DSP_MREFUSE_GATES, r, adm);
    }
    if (!prepare_limits(d, clock, volume_ul, &cfg, &r, &ledger_date, &ledger_sum)) {
        uint32_t code = r.status == DSP_DOSING_LIMIT ? DSP_MREFUSE_LIMIT : DSP_MREFUSE_GATES;
        return refuse(d, slot, seq, code, r, adm);
    }
    if (!dsp_reservoir_covers(&cfg, volume_ul)) {
        r.status = DSP_DOSING_NOT_COVERED; /* refused; the latch is for automatic doses */
        return refuse(d, slot, seq, DSP_MREFUSE_NOT_COVERED, r, adm);
    }
    bool decided = false;
    r = manual_execute(d, slot, seq, volume_ul, clock, r, ledger_date, ledger_sum, adm, &decided);
    if (!decided) {
        dsp_manual_outcome_t o = outcome(DSP_MANUAL_NOT_DURABLE, DSP_MRES_NOT_DECIDED, 0);
        o.dose = r;
        return o;
    }
    const dsp_manual_result_t *e = dsp_dosing_state_find_result(&d->s, slot, seq);
    dsp_manual_outcome_t o = outcome(DSP_MANUAL_EXECUTED,
                                     e != NULL ? e->state : DSP_MRES_UNAVAILABLE,
                                     e != NULL ? e->detail : 0);
    o.dose = r;
    return o;
}

/* R2..R4 of the refill (idempotent; dosing stays disabled until R4). */
/* Phase C: the reservoir event of a completed refill, classified from the estimate before it
 * (before == NULL: R3 had already landed before a power cut, so the old values are unknown). */
void dsp_dosing_log_reservoir(dsp_dosing_t *d, const dsp_config_t *before,
                              const dsp_config_t *target, uint64_t key, dsp_notify_clock_t clock)
{
    if (d == NULL || target == NULL) {
        return;
    }
    if (d->log == NULL) {
        return;
    }
    dsp_event_t e;
    memset(&e, 0, sizeof e);
    e.result = DSP_EVR_OK;
    e.flags = clock.trusted ? DSP_EVF_CLOCK_TRUSTED : 0u;
    e.at = clock.now >= DSP_UTC_MIN && clock.now <= DSP_UTC_MAX ? clock.now : 0; /* untrusted: flag clear */
    e.capacity_ul = target->reservoir_capacity_ul;
    e.after_ul = target->reservoir_remaining_ul;
    if (before == NULL) {
        e.type = DSP_EV_RESERVOIR_REFILL;
        e.flags |= DSP_EVF_RESERVOIR_UNKNOWN;
        e.after_ul = 0;
    } else {
        e.before_ul = before->reservoir_remaining_ul;
        e.type = before->reservoir_capacity_ul == 0 ? DSP_EV_RESERVOIR_INIT
                 : before->reservoir_capacity_ul != target->reservoir_capacity_ul
                     ? DSP_EV_RESERVOIR_ADJUST
                 : target->reservoir_remaining_ul > before->reservoir_remaining_ul
                     ? DSP_EV_RESERVOIR_REFILL
                     : DSP_EV_RESERVOIR_ADJUST;
    }
    e.key = key;
    (void)dsp_evlog_append(d->log, &e, NULL);
}

static dsp_dosing_status_t roll_forward_refill(dsp_dosing_t *d, dsp_notify_clock_t clock)
{
    dsp_config_t cfg;
    if (dsp_storage_load_config(d->st, &cfg) != DSP_STORAGE_OK) {
        d->degraded = true;
        return DSP_DOSING_ERR_STORAGE;
    }
    const uint32_t refill_seq = d->s.refill_seq;
    dsp_config_t target = cfg;
    target.reservoir_capacity_ul = d->s.refill_capacity_ul;
    target.reservoir_remaining_ul = d->s.refill_remaining_ul;
    /* R2: notification state (uncertainty cleared, BLOCKED closed, LOW re-evaluated). */
    if (dsp_notify_on_refill(d->n, &target, clock) != DSP_NOTIFY_OK) {
        d->degraded = true;
        return DSP_DOSING_ERR_STORAGE;
    }
    /* R3: the estimate (skipped if a previous attempt already stored it). */
    const bool before_known = cfg.reservoir_capacity_ul != target.reservoir_capacity_ul ||
                              cfg.reservoir_remaining_ul != target.reservoir_remaining_ul;
    if (before_known) {
        if (dsp_reservoir_refill(d->st, target.reservoir_capacity_ul, target.reservoir_remaining_ul,
                                 NULL) != DSP_STORAGE_OK) {
            d->degraded = true;
            return DSP_DOSING_ERR_STORAGE;
        }
    }
    /* R4: latch and intent cleared together, last. */
    dsp_dosing_state_t next = d->s;
    next.flags &= (uint8_t)~(DSP_DOSING_F_BLOCKED | DSP_DOSING_F_REFILL_PENDING);
    next.refill_capacity_ul = 0;
    next.refill_remaining_ul = 0;
    next.refill_seq = 0;
    if (!save_state(d, &next)) {
        return DSP_DOSING_ERR_STORAGE;
    }
    /* After R4 (durable): the refill can no longer be rolled forward again, so it is logged
     * exactly once (a power cut before this append loses the event, never duplicates it). */
    dsp_dosing_log_reservoir(d, before_known ? &cfg : NULL, &target,
                             DSP_EVKEY_REFILL | refill_seq, clock); /* refill intent seq: unique */
    return DSP_DOSING_OK;
}

dsp_dosing_status_t dsp_dosing_refill(dsp_dosing_t *d, uint32_t capacity_ul,
                                      uint32_t remaining_ul, dsp_notify_clock_t clock)
{
    if (d == NULL || !d->loaded || capacity_ul == 0 || remaining_ul == 0 ||
        remaining_ul > capacity_ul) {
        return DSP_DOSING_ERR_ARG;
    }
    if (d->degraded || (d->s.flags & DSP_DOSING_F_MANUAL_ACTIVE)) {
        return DSP_DOSING_REFUSED; /* recovery first */
    }
    /* R1: the intent (dosing disabled from here until R4). */
    uint32_t seq = d->s.next_seq != 0 ? d->s.next_seq : 1;
    dsp_dosing_state_t next = d->s;
    next.flags |= DSP_DOSING_F_REFILL_PENDING;
    next.refill_capacity_ul = capacity_ul;
    next.refill_remaining_ul = remaining_ul;
    next.refill_seq = seq;
    next.next_seq = seq + 1;
    if (!save_state(d, &next)) {
        return DSP_DOSING_ERR_STORAGE;
    }
    return roll_forward_refill(d, clock);
}

dsp_dosing_status_t dsp_dosing_recover(dsp_dosing_t *d, dsp_notify_clock_t clock,
                                       dsp_dosing_recovery_t *out)
{
    dsp_dosing_recovery_t rec;
    memset(&rec, 0, sizeof rec);
    if (out != NULL) {
        *out = rec;
    }
    if (d == NULL || !d->loaded) {
        return DSP_DOSING_ERR_ARG;
    }
    dsp_dosing_status_t st = DSP_DOSING_OK;
    /* 1. A refill interrupted by a power loss is completed first. */
    if (d->s.flags & DSP_DOSING_F_REFILL_PENDING) {
        st = roll_forward_refill(d, clock);
        if (st != DSP_DOSING_OK) {
            goto done;
        }
        rec.refill_completed = true;
    }
    /* 2. Interrupted automatic doses: report (keyed, durable volume) BEFORE the rewrite. */
    for (size_t guard = 0; guard <= DSP_HISTORY_MAX_SLOTS; guard++) {
        size_t slot = 0;
        dsp_history_record_t hr;
        dsp_exec_status_t es = dsp_exec_find_started(d->ex, &slot, &hr);
        if (es == DSP_EXEC_NOT_FOUND) {
            break;
        }
        if (es != DSP_EXEC_OK) {
            d->degraded = true;
            st = DSP_DOSING_ERR_STORAGE;
            goto done;
        }
        bool dup = false;
        dsp_dosing_error_t e = {DSP_DOSEERR_INTERRUPTED, false, hr.volume_ul};
        if (dsp_notify_report_dosing_error_once(d->n, &e, dsp_dosing_key_automatic(&hr), clock,
                                                &dup) == DSP_NOTIFY_ERR_STORAGE) {
            d->degraded = true;
            st = DSP_DOSING_ERR_STORAGE;
            goto done;
        }
        if (dsp_exec_mark_interrupted(d->ex, slot, &hr) != DSP_EXEC_OK) {
            d->degraded = true;
            st = DSP_DOSING_ERR_STORAGE;
            goto done;
        }
        /* Claimed with a trusted clock; whether the consumption landed is unknown. */
        log_dose(d, false, DSP_EVR_INTERRUPTED, hr.claimed_at, true, hr.volume_ul, NULL, NULL, 0,
                 dsp_dosing_key_automatic(&hr), hr.schedule_id);
        rec.automatic++;
        rec.duplicates += dup;
    }
    /* 3. An interrupted manual dose: report (keyed) BEFORE clearing the marker. */
    if (d->s.flags & DSP_DOSING_F_MANUAL_ACTIVE) {
        bool dup = false;
        dsp_dosing_error_t e = {DSP_DOSEERR_INTERRUPTED, true, d->s.manual_volume_ul};
        uint64_t key = dsp_dosing_key_manual(d->s.manual_seq, d->s.manual_started_at,
                                             d->s.manual_volume_ul);
        if (dsp_notify_report_dosing_error_once(d->n, &e, key, clock, &dup) ==
            DSP_NOTIFY_ERR_STORAGE) {
            d->degraded = true;
            st = DSP_DOSING_ERR_STORAGE;
            goto done;
        }
        dsp_dosing_state_t next = d->s;
        clear_marker(&next, DSP_MRES_INTERRUPTED); /* same write: ring entry INTERRUPTED */
        const dsp_utc_t started = d->s.manual_started_at;
        const uint32_t mvol = d->s.manual_volume_ul;
        if (!save_state(d, &next)) {
            st = DSP_DOSING_ERR_STORAGE;
            goto done;
        }
        log_dose(d, true, DSP_EVR_INTERRUPTED, started, true, mvol, NULL, NULL, 0, key, NULL);
        rec.manual = 1;
        rec.duplicates += dup;
    }
    /* 4. The reservoir conditions follow the latch (e.g. BLOCKED after a migration). */
    (void)dsp_dosing_observe_reservoir(d, clock);
done:
    if (out != NULL) {
        *out = rec;
    }
    return st;
}

uint32_t dsp_dosing_ready_gates(dsp_dosing_t *d, uint32_t *pump_problems)
{
    return dsp_dosing_gates(d, true, pump_problems) & ~(uint32_t)DSP_DOSING_G_NOT_READY;
}

dsp_dosing_status_t dsp_dosing_set_readiness(dsp_dosing_t *d, dsp_readiness_t readiness,
                                             uint32_t *gates, uint32_t *pump_problems)
{
    if (gates != NULL) {
        *gates = 0;
    }
    if (pump_problems != NULL) {
        *pump_problems = 0;
    }
    if (d == NULL || !d->loaded ||
        (readiness != DSP_READINESS_READY && readiness != DSP_READINESS_SERVICE_REQUIRED)) {
        return DSP_DOSING_ERR_ARG;
    }
    if (readiness == DSP_READINESS_READY) {
        /* Every gate except the clock (READY is a configuration state, not a time state). */
        uint32_t g = dsp_dosing_ready_gates(d, pump_problems);
        if (gates != NULL) {
            *gates = g;
        }
        if (g != 0) {
            return DSP_DOSING_REFUSED;
        }
    }
    if (d->s.readiness == readiness) {
        return DSP_DOSING_OK;
    }
    dsp_dosing_state_t next = d->s;
    next.readiness = (uint8_t)readiness;
    return save_state(d, &next) ? DSP_DOSING_OK : DSP_DOSING_ERR_STORAGE;
}

/* ---- Due processing ------------------------------------------------------------------- */

typedef struct {
    size_t index;
    dsp_occurrence_t occ;
} candidate_t;

#define MAX_CANDIDATES (2 * DSP_SCHEDULE_CAPACITY)

/* Occurrences inside the due window of now (enabled schedules, valid clock), in time order,
 * then schedule ID (insertion sort; at most 2 per schedule). */
static size_t collect_candidates(const dsp_dosing_t *d, const dsp_schedule_t *schedules, size_t n,
                                 dsp_clock_t now, candidate_t *cand)
{
    size_t count = 0;
    int64_t window = d->ex->cfg.due_window_s;
    for (size_t i = 0; i < n && count < MAX_CANDIDATES; i++) {
        if (!schedules[i].enabled || !now.valid) {
            continue;
        }
        dsp_utc_t after = now.now - window - 1;
        if (after < DSP_UTC_MIN) {
            after = DSP_UTC_MIN;
        }
        dsp_occurrence_t buf[2];
        size_t got = 0;
        (void)dsp_rec_between(&schedules[i], d->tz, after, now.now, buf, 2, &got);
        for (size_t k = 0; k < got && count < MAX_CANDIDATES; k++) {
            cand[count].index = i;
            cand[count].occ = buf[k];
            count++;
        }
    }
    for (size_t i = 1; i < count; i++) {
        candidate_t c = cand[i];
        size_t j = i;
        while (j > 0 && (cand[j - 1].occ.at > c.occ.at ||
                         (cand[j - 1].occ.at == c.occ.at &&
                          strcmp(schedules[cand[j - 1].index].id, schedules[c.index].id) > 0))) {
            cand[j] = cand[j - 1];
            j--;
        }
        cand[j] = c;
    }
    return count;
}

dsp_dosing_due_report_t dsp_dosing_process_due(dsp_dosing_t *d, const dsp_schedule_t *schedules,
                                               size_t n, const dsp_dosing_clock_t *clock)
{
    dsp_dosing_due_report_t rep;
    memset(&rep, 0, sizeof rep);
    if (d == NULL || clock == NULL || clock->read == NULL || (schedules == NULL && n > 0) ||
        n > DSP_SCHEDULE_CAPACITY || d->ex == NULL) {
        rep.stop = DSP_DOSING_ERR_ARG;
        return rep;
    }
    dsp_clock_t now = clock->read(clock->ctx);
    candidate_t cand[MAX_CANDIDATES];
    size_t count = collect_candidates(d, schedules, n, now, cand);
    rep.candidates = count;
    for (size_t i = 0; i < count; i++) {
        dsp_clock_t c = clock->read(clock->ctx); /* time advances with every pump run */
        rep.last = dsp_dosing_run_automatic(d, &schedules[cand[i].index], &cand[i].occ, c);
        if (rep.last.status == DSP_DOSING_OK) {
            rep.dosed++;
        }
        if (rep.last.status == DSP_DOSING_PUMP_FAILED) {
            rep.pump_failures++;
        }
        if (rep.last.status == DSP_DOSING_ERR_STORAGE || rep.last.status == DSP_DOSING_NOT_COVERED ||
            rep.last.status == DSP_DOSING_PUMP_FAILED ||
            (rep.last.status == DSP_DOSING_REFUSED && rep.last.gates != 0)) {
            rep.stop = rep.last.status;
            break;
        }
    }
    return rep;
}

/* ---- Planning queries and safe-off (M1.9b) --------------------------------------------- */

/* Persistent gates that make every automatic dose impossible until a user action (no dose
 * wakes, Q13). DEGRADED is wake-local (state is reloaded at the next boot) and the clock is
 * handled by the caller, so neither suppresses planning. */
#define PLAN_SUPPRESS_GATES                                                                    \
    (DSP_DOSING_G_NOT_LOADED | DSP_DOSING_G_NOT_READY | DSP_DOSING_G_BLOCKED |                \
     DSP_DOSING_G_REFILL_PENDING | DSP_DOSING_G_MANUAL_ACTIVE | DSP_DOSING_G_PUMP_CONFIG |   \
     DSP_DOSING_G_RESERVOIR_UNCONFIGURED)

bool dsp_dosing_dose_wakes_allowed(dsp_dosing_t *d)
{
    return d != NULL && d->loaded && (dsp_dosing_gates(d, true, NULL) & PLAN_SUPPRESS_GATES) == 0;
}

static dsp_utc_t later(dsp_utc_t a, dsp_utc_t b)
{
    return a > b ? a : b;
}

bool dsp_dosing_next_due(dsp_dosing_t *d, const dsp_schedule_t *schedules, size_t n,
                         dsp_clock_t clock, dsp_utc_t *at)
{
    if (d == NULL || at == NULL || (schedules == NULL && n > 0) || n > DSP_SCHEDULE_CAPACITY ||
        d->ex == NULL || !clock.valid || !d->loaded) {
        return false;
    }
    dsp_config_t cfg;
    if ((state_gates(d) & PLAN_SUPPRESS_GATES) != 0 ||
        dsp_storage_load_config(d->st, &cfg) != DSP_STORAGE_OK ||
        (config_gates(d, &cfg, NULL) & PLAN_SUPPRESS_GATES) != 0) {
        return false;
    }
    int64_t window = d->ex->cfg.due_window_s;
    /* Earliest start the minimum interval allows (overflow-safe: both are bounded). */
    dsp_utc_t ready = d->s.last_start_utc != 0 ? d->s.last_start_utc + (dsp_utc_t)cfg.min_interval_s
                                               : DSP_UTC_MIN;
    bool found = false;
    dsp_utc_t best = 0;
    /* 1. Unclaimed occurrences still inside their due window that only the interval holds
     *    back (a refused same-time dose): retried when the interval allows, if that is
     *    still inside the window. Other refusals (dose, runtime, daily) do not retry. */
    candidate_t cand[MAX_CANDIDATES];
    size_t count = collect_candidates(d, schedules, n, clock, cand);
    for (size_t i = 0; i < count; i++) {
        const dsp_schedule_t *s = &schedules[cand[i].index];
        if (dsp_exec_check(d->ex, s, &cand[i].occ, clock) != DSP_EXEC_OK) {
            continue; /* claimed, missed (window or floor) or held */
        }
        uint32_t rt = 0;
        uint32_t sum = 0;
        dsp_date_t date;
        bool date_ok = true;
        uint32_t l = check_limits(d, &cfg, s->amount_ul, clock.now, &rt, &date, &sum, &date_ok);
        if (!date_ok || (l & ~(uint32_t)DSP_DOSING_L_INTERVAL) != 0) {
            continue;
        }
        dsp_utc_t t = later(cand[i].occ.at, ready);
        if (t > cand[i].occ.at + window) {
            continue; /* the interval outlasts the window: it will be missed */
        }
        if (!found || t < best) {
            best = t;
            found = true;
        }
    }
    /* 2. The next future occurrence (moved to the end of the interval if that is still
     *    inside its window; otherwise its own instant: it is then refused and missed). */
    dsp_utc_t next = 0;
    if (dsp_wake_next_dose(schedules, n, d->tz, clock, &next)) {
        dsp_utc_t t = later(next, ready) <= next + window ? later(next, ready) : next;
        if (!found || t < best) {
            best = t;
            found = true;
        }
    }
    if (found) {
        *at = best;
    }
    return found;
}

bool dsp_dosing_port_safe_off(const dsp_pump_port_t *pump)
{
    if (pump == NULL || pump->force_off == NULL) {
        return false;
    }
    pump->force_off(pump->ctx);
    return pump->is_off != NULL && pump->is_off(pump->ctx);
}

bool dsp_dosing_safe_off(dsp_dosing_t *d)
{
    return d != NULL && dsp_dosing_port_safe_off(d->pump);
}

bool dsp_dosing_degraded(const dsp_dosing_t *d)
{
    return d == NULL || d->degraded;
}
