#include "dsp_dosing.h"

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
    if (!save_state(d, &next)) {
        if (report(d, DSP_DOSEERR_STORAGE, false, volume, key, nc)) {
            (void)dsp_exec_finish(d->ex, &token, DSP_HISTORY_FAILED);
        } /* else: stays STARTED, the durable source for boot recovery */
        r.status = DSP_DOSING_ERR_STORAGE;
        return r;
    }
    /* 3. Durable consumption -> single-use permit (enables). */
    dsp_dose_permit_t permit;
    if (dsp_reservoir_consume(d->st, volume, DSP_DOSE_AUTOMATIC, NULL, &permit) != DSP_STORAGE_OK) {
        d->degraded = true;
        if (report(d, DSP_DOSEERR_STORAGE, false, volume, key, nc)) {
            (void)dsp_exec_finish(d->ex, &token, DSP_HISTORY_FAILED);
        } /* else: stays STARTED */
        r.status = DSP_DOSING_ERR_STORAGE;
        return r;
    }
    /* 4. Redeem the permit, then (and only then) the pump. */
    uint32_t permitted = 0;
    if (!dsp_dose_permit_take(&permit, &permitted) || permitted != volume) {
        d->degraded = true; /* cannot happen; never actuate on an inconsistent permit */
        (void)dsp_exec_finish(d->ex, &token, DSP_HISTORY_FAILED);
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
        }
    } /* else: the fault report is not durable; the record stays STARTED for recovery */
    (void)dsp_dosing_observe_reservoir(d, nc);
    r.status = r.pump == DSP_PUMP_OK ? DSP_DOSING_OK : DSP_DOSING_PUMP_FAILED;
    return r;
}

dsp_dosing_result_t dsp_dosing_manual(dsp_dosing_t *d, uint32_t volume_ul, dsp_clock_t clock)
{
    dsp_dosing_result_t r;
    memset(&r, 0, sizeof r);
    if (d == NULL) {
        r.status = DSP_DOSING_ERR_ARG;
        return r;
    }
    dsp_notify_clock_t nc = nclock_of(clock);
    dsp_config_t cfg;
    dsp_date_t ledger_date;
    uint32_t ledger_sum = 0;
    if (!prepare_gates(d, clock, volume_ul, &cfg, &r) ||
        !prepare_limits(d, clock, volume_ul, &cfg, &r, &ledger_date, &ledger_sum)) {
        return r;
    }
    if (!dsp_reservoir_covers(&cfg, volume_ul)) {
        r.status = DSP_DOSING_NOT_COVERED; /* refused; the latch is for automatic doses */
        return r;
    }
    dsp_exec_token_t token;
    r.exec = dsp_exec_manual_begin(d->ex, &token);
    if (r.exec != DSP_EXEC_OK) {
        r.status = DSP_DOSING_NOT_ELIGIBLE; /* BUSY */
        return r;
    }
    /* 1. Marker + ledger + last start in one write (restricts). */
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
    if (!save_state(d, &next)) {
        (void)dsp_exec_finish(d->ex, &token, DSP_HISTORY_FAILED);
        r.status = DSP_DOSING_ERR_STORAGE;
        return r;
    }
    uint64_t key = dsp_dosing_key_manual(seq, clock.now, volume_ul);
    /* 2. Durable consumption -> permit. */
    dsp_dose_permit_t permit;
    if (dsp_reservoir_consume(d->st, volume_ul, DSP_DOSE_MANUAL, NULL, &permit) != DSP_STORAGE_OK) {
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
    /* 3. Clear the marker (a failure leaves it for recovery: reported once, keyed). */
    dsp_dosing_state_t done = d->s;
    done.flags &= (uint8_t)~DSP_DOSING_F_MANUAL_ACTIVE;
    done.manual_volume_ul = 0;
    done.manual_started_at = 0;
    done.manual_seq = 0;
    if (reported) {
        (void)save_state(d, &done);
    } /* else: the fault report is not durable; the marker stays for recovery */
    (void)dsp_exec_finish(d->ex, &token,
                          r.pump == DSP_PUMP_OK ? DSP_HISTORY_COMPLETED : DSP_HISTORY_FAILED);
    (void)dsp_dosing_observe_reservoir(d, nc);
    r.status = r.pump == DSP_PUMP_OK ? DSP_DOSING_OK : DSP_DOSING_PUMP_FAILED;
    return r;
}

/* R2..R4 of the refill (idempotent; dosing stays disabled until R4). */
static dsp_dosing_status_t roll_forward_refill(dsp_dosing_t *d, dsp_notify_clock_t clock)
{
    dsp_config_t cfg;
    if (dsp_storage_load_config(d->st, &cfg) != DSP_STORAGE_OK) {
        d->degraded = true;
        return DSP_DOSING_ERR_STORAGE;
    }
    dsp_config_t target = cfg;
    target.reservoir_capacity_ul = d->s.refill_capacity_ul;
    target.reservoir_remaining_ul = d->s.refill_remaining_ul;
    /* R2: notification state (uncertainty cleared, BLOCKED closed, LOW re-evaluated). */
    if (dsp_notify_on_refill(d->n, &target, clock) != DSP_NOTIFY_OK) {
        d->degraded = true;
        return DSP_DOSING_ERR_STORAGE;
    }
    /* R3: the estimate (skipped if a previous attempt already stored it). */
    if (cfg.reservoir_capacity_ul != target.reservoir_capacity_ul ||
        cfg.reservoir_remaining_ul != target.reservoir_remaining_ul) {
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
    return save_state(d, &next) ? DSP_DOSING_OK : DSP_DOSING_ERR_STORAGE;
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
        next.flags &= (uint8_t)~DSP_DOSING_F_MANUAL_ACTIVE;
        next.manual_volume_ul = 0;
        next.manual_started_at = 0;
        next.manual_seq = 0;
        if (!save_state(d, &next)) {
            st = DSP_DOSING_ERR_STORAGE;
            goto done;
        }
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
        uint32_t g = dsp_dosing_gates(d, true, pump_problems) & ~(uint32_t)DSP_DOSING_G_NOT_READY;
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

dsp_dosing_due_report_t dsp_dosing_process_due(dsp_dosing_t *d, const dsp_schedule_t *schedules,
                                               size_t n, const dsp_dosing_clock_t *clock)
{
    dsp_dosing_due_report_t rep;
    memset(&rep, 0, sizeof rep);
    if (d == NULL || clock == NULL || clock->read == NULL || (schedules == NULL && n > 0) ||
        n > DSP_SCHEDULE_CAPACITY) {
        rep.stop = DSP_DOSING_ERR_ARG;
        return rep;
    }
    dsp_clock_t now = clock->read(clock->ctx);
    candidate_t cand[MAX_CANDIDATES];
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
    /* Time order, then schedule ID (insertion sort; at most 32 entries). */
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
