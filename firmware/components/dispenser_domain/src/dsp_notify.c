#include "dsp_notify.h"

#include <stdio.h>
#include <string.h>

/* ---- Helpers ---------------------------------------------------------------------- */

static uint8_t severity_of(uint8_t type)
{
    return (type == DSP_NOTE_RESERVOIR_LOW || type == DSP_NOTE_BATTERY_LOW) ? DSP_SEV_WARNING
                                                                             : DSP_SEV_CRITICAL;
}

static uint8_t type_of(dsp_condition_t c)
{
    switch (c) {
    case DSP_COND_RESERVOIR_LOW:
        return DSP_NOTE_RESERVOIR_LOW;
    case DSP_COND_RESERVOIR_BLOCKED:
        return DSP_NOTE_RESERVOIR_BLOCKED;
    case DSP_COND_BATTERY_LOW:
        return DSP_NOTE_BATTERY_LOW;
    default:
        return DSP_NOTE_DEVICE_ERROR;
    }
}

static uint16_t code_of(dsp_condition_t c)
{
    switch (c) {
    case DSP_COND_STORAGE:
        return DSP_DEVERR_STORAGE;
    case DSP_COND_TIME_SYNC:
        return DSP_DEVERR_TIME_SYNC;
    case DSP_COND_CLOCK:
        return DSP_DEVERR_CLOCK;
    default:
        return 0;
    }
}

static bool trusted_now(dsp_notify_clock_t c)
{
    return c.trusted && c.now >= DSP_TIME_PLAUSIBLE_MIN && c.now <= DSP_UTC_MAX;
}

static bool persist(dsp_notify_t *n)
{
    if (dsp_storage_save_notify(n->st, &n->s) != DSP_STORAGE_OK) {
        n->dirty = true;
        return false;
    }
    n->dirty = false;
    return true;
}

static dsp_notify_status_t finish(dsp_notify_t *n, bool changed, dsp_notify_status_t st)
{
    if ((changed || n->dirty) && !persist(n)) {
        return DSP_NOTIFY_ERR_STORAGE;
    }
    return st;
}

/* Queue placement (approved): same condition's pending entry -> free slot -> oldest entry of
 * lower severity. CRITICAL entries are never evicted. Returns false if nothing fits. */
static bool enqueue(dsp_notify_t *n, uint8_t type, uint8_t cond, uint16_t code, uint32_t value,
                    uint32_t value2, uint8_t flags, uint8_t reminder_no, dsp_notify_clock_t clock)
{
    dsp_notify_state_t *s = &n->s;
    uint8_t sev = severity_of(type);
    int slot = -1;
    if (cond != DSP_COND_NONE) {
        for (int i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY && slot < 0; i++) {
            if (s->queue[i].seq != 0 && s->queue[i].condition == cond) {
                slot = i; /* the newer event of the same condition supersedes */
            }
        }
    }
    for (int i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY && slot < 0; i++) {
        if (s->queue[i].seq == 0) {
            slot = i;
        }
    }
    if (slot < 0) { /* evict the oldest entry of strictly LOWER severity */
        for (int i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY; i++) {
            if (s->queue[i].seq != 0 && s->queue[i].severity < sev &&
                (slot < 0 || s->queue[i].seq < s->queue[slot].seq)) {
                slot = i;
            }
        }
    }
    if (slot < 0) {
        return false;
    }
    if (s->queue[slot].seq != 0 && s->queue[slot].condition != cond &&
        s->queue[slot].condition < DSP_COND_COUNT) {
        /* Evicted entry of another condition: not lost, re-raised when space frees. */
        dsp_condition_state_t *ev = &s->cond[s->queue[slot].condition];
        if (ev->flags & DSP_COND_F_ACTIVE) {
            ev->flags |= DSP_COND_F_RAISE_PENDING;
        }
    }
    if (s->next_seq == UINT32_MAX) {
        return false; /* practically unreachable (4e9 events) */
    }
    if (s->epoch == 0) {
        s->epoch = n->random != 0 ? n->random : 1;
    }
    if (s->next_seq == 0) {
        s->next_seq = 1;
    }
    dsp_note_t *e = &s->queue[slot];
    memset(e, 0, sizeof *e);
    e->seq = s->next_seq++;
    e->type = type;
    e->severity = sev;
    e->condition = cond;
    e->flags = flags;
    e->reminder_no = reminder_no;
    e->code = code;
    e->value = value;
    e->value2 = value2;
    if (trusted_now(clock)) {
        e->created_at = clock.now;
        e->flags |= DSP_NOTE_F_TIME_VALID;
    }
    return true;
}

static uint8_t condition_flags(const dsp_notify_t *n, dsp_condition_t c)
{
    bool reservoir = c == DSP_COND_RESERVOIR_LOW || c == DSP_COND_RESERVOIR_BLOCKED;
    return reservoir && (n->s.flags & DSP_NOTIFY_F_ESTIMATE_UNCERTAIN) ? DSP_NOTE_F_UNCERTAIN : 0;
}

/* Creates the condition's event (episode start or reminder). Sets RAISE_PENDING if full.
 * Payload: the condition's latest value/value2; code only for DEVICE_ERROR. */
static bool raise(dsp_notify_t *n, dsp_condition_t c, uint8_t reminder_no, dsp_notify_clock_t clock)
{
    dsp_condition_state_t *cs = &n->s.cond[c];
    uint8_t flags = condition_flags(n, c) | (reminder_no > 0 ? DSP_NOTE_F_REMINDER : 0);
    if (enqueue(n, type_of(c), (uint8_t)c, code_of(c), cs->value, cs->value2, flags, reminder_no,
                clock)) {
        cs->flags &= (uint8_t)~DSP_COND_F_RAISE_PENDING;
        return true;
    }
    cs->flags |= DSP_COND_F_RAISE_PENDING;
    return false;
}

static bool open_episode(dsp_notify_t *n, dsp_condition_t c, uint32_t value, uint32_t value2,
                         dsp_notify_clock_t clock)
{
    dsp_condition_state_t *cs = &n->s.cond[c];
    memset(cs, 0, sizeof *cs);
    cs->flags = DSP_COND_F_ACTIVE;
    cs->value = value;
    cs->value2 = value2;
    if (trusted_now(clock)) {
        cs->opened_at = clock.now;
        cs->next_reminder_at = clock.now + DSP_NOTIFY_REMINDER_INTERVAL_S;
    }
    return raise(n, c, 0, clock);
}

/* Ends the episode; its undelivered notifications are withdrawn (stale). */
static void close_episode(dsp_notify_t *n, dsp_condition_t c)
{
    memset(&n->s.cond[c], 0, sizeof n->s.cond[c]);
    for (int i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY; i++) {
        if (n->s.queue[i].seq != 0 && n->s.queue[i].condition == (uint8_t)c) {
            memset(&n->s.queue[i], 0, sizeof n->s.queue[i]);
        }
    }
}

static bool active(const dsp_notify_t *n, dsp_condition_t c)
{
    return (n->s.cond[c].flags & DSP_COND_F_ACTIVE) != 0;
}

/* Generic condition update: returns whether persistent state changed; *full on overflow. */
static bool update_condition(dsp_notify_t *n, dsp_condition_t c, bool on, bool off,
                             uint32_t value, uint32_t value2, dsp_notify_clock_t clock, bool *full)
{
    if (!active(n, c)) {
        if (on) {
            *full |= !open_episode(n, c, value, value2, clock);
            return true;
        }
        return false;
    }
    if (off) {
        close_episode(n, c);
        return true;
    }
    /* Still active: refresh the payload in RAM only (written with the next real change, or
     * right before the first transmission). An entry that was already transmitted keeps
     * its payload: a retry with the same ID never changes content. */
    n->s.cond[c].value = value;
    n->s.cond[c].value2 = value2;
    for (int i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY; i++) {
        dsp_note_t *e = &n->s.queue[i];
        if (e->seq != 0 && e->condition == (uint8_t)c && e->attempts == 0 &&
            (e->value != value || e->value2 != value2)) {
            e->value = value;
            e->value2 = value2; /* persisted by the freeze write before the first send */
        }
    }
    return false;
}

/* ---- Public API ------------------------------------------------------------------- */

dsp_notify_status_t dsp_notify_init(dsp_notify_t *n, dsp_storage_t *st, uint32_t random)
{
    if (n == NULL) {
        return DSP_NOTIFY_ERR_ARG;
    }
    memset(n, 0, sizeof *n);
    if (st == NULL) {
        return DSP_NOTIFY_ERR_ARG;
    }
    n->st = st;
    n->random = random;
    if (dsp_storage_load_notify(st, &n->s) != DSP_STORAGE_OK) {
        return DSP_NOTIFY_ERR_STORAGE; /* never treated as an empty queue */
    }
    n->loaded = true;
    return DSP_NOTIFY_OK;
}

dsp_notify_status_t dsp_notify_observe_reservoir(dsp_notify_t *n, const dsp_config_t *cfg,
                                                 bool blocked, dsp_notify_clock_t clock)
{
    if (n == NULL || !n->loaded || cfg == NULL || dsp_config_validate(cfg) != 0) {
        return DSP_NOTIFY_ERR_ARG;
    }
    uint64_t cap = cfg->reservoir_capacity_ul;
    uint64_t rem = cfg->reservoir_remaining_ul;
    uint64_t low_pct = cfg->low_stock_percent;
    uint64_t rec_pct = low_pct + DSP_NOTIFY_LOW_RECOVERY_MARGIN_PCT;
    if (rec_pct > 100) {
        rec_pct = 100;
    }
    /* Capacity 0 = reservoir not configured: unknown, not empty. */
    bool low = cap > 0 && rem * 100 < cap * low_pct;
    bool recovered = cap == 0 || rem * 100 >= cap * rec_pct;
    bool full = false;
    bool changed = update_condition(n, DSP_COND_RESERVOIR_LOW, low, recovered,
                                    cfg->reservoir_remaining_ul, cfg->reservoir_capacity_ul, clock, &full);
    changed |= update_condition(n, DSP_COND_RESERVOIR_BLOCKED, blocked, !blocked,
                                cfg->reservoir_remaining_ul, cfg->reservoir_capacity_ul, clock, &full);
    return finish(n, changed, full ? DSP_NOTIFY_QUEUE_FULL : DSP_NOTIFY_OK);
}

dsp_notify_status_t dsp_notify_observe_battery(dsp_notify_t *n, dsp_battery_obs_t obs,
                                               dsp_notify_clock_t clock)
{
    if (n == NULL || !n->loaded || obs.status > DSP_BATTERY_LOW) {
        return DSP_NOTIFY_ERR_ARG;
    }
    if (obs.status == DSP_BATTERY_UNKNOWN) {
        return finish(n, false, DSP_NOTIFY_OK); /* no valid measurement: nothing changes */
    }
    bool full = false;
    bool changed = update_condition(n, DSP_COND_BATTERY_LOW, obs.status == DSP_BATTERY_LOW,
                                    obs.status == DSP_BATTERY_OK, obs.millivolts, 0, clock, &full);
    return finish(n, changed, full ? DSP_NOTIFY_QUEUE_FULL : DSP_NOTIFY_OK);
}

dsp_notify_status_t dsp_notify_report_dosing_error(dsp_notify_t *n, const dsp_dosing_error_t *e,
                                                   dsp_notify_clock_t clock)
{
    if (n == NULL || !n->loaded || e == NULL || e->code < 1 || e->code > DSP_DOSEERR_MAX) {
        return DSP_NOTIFY_ERR_ARG;
    }
    if (e->code == DSP_DOSEERR_START_FAILED || e->code == DSP_DOSEERR_PUMP_FAILED ||
        e->code == DSP_DOSEERR_INTERRUPTED) {
        /* The volume was consumed before the pump: the estimate is now uncertain. */
        n->s.flags |= DSP_NOTIFY_F_ESTIMATE_UNCERTAIN;
    }
    if (enqueue(n, DSP_NOTE_DOSING_ERROR, DSP_COND_NONE, e->code, e->requested_ul, 0,
                e->manual ? DSP_NOTE_F_MANUAL : 0, 0, clock)) {
        return finish(n, true, DSP_NOTIFY_OK);
    }
    if (n->s.dropped < UINT16_MAX) {
        n->s.dropped++;
    }
    return finish(n, true, DSP_NOTIFY_QUEUE_FULL);
}

dsp_notify_status_t dsp_notify_set_device_condition(dsp_notify_t *n, dsp_condition_t cond,
                                                    bool on, uint32_t detail,
                                                    dsp_notify_clock_t clock)
{
    if (n == NULL || !n->loaded ||
        (cond != DSP_COND_STORAGE && cond != DSP_COND_TIME_SYNC && cond != DSP_COND_CLOCK)) {
        return DSP_NOTIFY_ERR_ARG;
    }
    bool full = false;
    bool changed = update_condition(n, cond, on, !on, detail, 0, clock, &full);
    return finish(n, changed, full ? DSP_NOTIFY_QUEUE_FULL : DSP_NOTIFY_OK);
}

dsp_notify_status_t dsp_notify_observe_time(dsp_notify_t *n, const dsp_timekeeper_t *tk,
                                            dsp_utc_t now)
{
    if (n == NULL || !n->loaded || tk == NULL || !tk->initialized) {
        return DSP_NOTIFY_ERR_ARG;
    }
    dsp_notify_clock_t clock = {now, tk->trust != DSP_TRUST_UNKNOWN};
    bool overdue = clock.trusted && dsp_timekeeper_sync_overdue(tk, now);
    bool unknown_long = !clock.trusted &&
                        tk->retained.consecutive_failures >= DSP_NOTIFY_CLOCK_FAILURES;
    dsp_notify_status_t a = dsp_notify_set_device_condition(
        n, DSP_COND_TIME_SYNC, overdue, (uint32_t)tk->retained.consecutive_failures, clock);
    dsp_notify_status_t b = dsp_notify_set_device_condition(
        n, DSP_COND_CLOCK, unknown_long, (uint32_t)tk->retained.consecutive_failures, clock);
    return a != DSP_NOTIFY_OK ? a : b;
}

dsp_notify_status_t dsp_notify_set_estimate_uncertain(dsp_notify_t *n, bool uncertain)
{
    if (n == NULL || !n->loaded) {
        return DSP_NOTIFY_ERR_ARG;
    }
    uint8_t flags = uncertain ? (uint8_t)(n->s.flags | DSP_NOTIFY_F_ESTIMATE_UNCERTAIN)
                              : (uint8_t)(n->s.flags & ~DSP_NOTIFY_F_ESTIMATE_UNCERTAIN);
    bool changed = flags != n->s.flags;
    n->s.flags = flags;
    return finish(n, changed, DSP_NOTIFY_OK);
}

dsp_notify_status_t dsp_notify_on_refill(dsp_notify_t *n, const dsp_config_t *cfg,
                                         dsp_notify_clock_t clock)
{
    if (n == NULL || !n->loaded || cfg == NULL || dsp_config_validate(cfg) != 0) {
        return DSP_NOTIFY_ERR_ARG;
    }
    bool changed = (n->s.flags & DSP_NOTIFY_F_ESTIMATE_UNCERTAIN) != 0;
    n->s.flags &= (uint8_t)~DSP_NOTIFY_F_ESTIMATE_UNCERTAIN;
    if (changed) {
        n->dirty = true; /* written by the observation below, even if nothing else changes */
    }
    return dsp_notify_observe_reservoir(n, cfg, false, clock);
}

bool dsp_notify_storage_degraded(const dsp_notify_t *n)
{
    return n == NULL || !n->loaded || n->dirty;
}

/* Deferred work: conditions waiting for a slot, then the overflow report. */
static bool raise_deferred(dsp_notify_t *n, dsp_notify_clock_t clock, bool *full)
{
    bool changed = false;
    for (int c = 0; c < DSP_COND_COUNT; c++) {
        dsp_condition_state_t *cs = &n->s.cond[c];
        if ((cs->flags & DSP_COND_F_ACTIVE) && (cs->flags & DSP_COND_F_RAISE_PENDING)) {
            if (raise(n, (dsp_condition_t)c, cs->reminders, clock)) {
                changed = true;
            } else {
                *full = true;
            }
        }
    }
    if (n->s.dropped > 0) {
        if (enqueue(n, DSP_NOTE_DEVICE_ERROR, DSP_COND_NONE, DSP_DEVERR_QUEUE_OVERFLOW,
                    n->s.dropped, 0, 0, 0, clock)) {
            n->s.dropped = 0;
            changed = true;
        } else {
            *full = true;
        }
    }
    return changed;
}

dsp_notify_status_t dsp_notify_tick(dsp_notify_t *n, dsp_notify_clock_t clock)
{
    if (n == NULL || !n->loaded) {
        return DSP_NOTIFY_ERR_ARG;
    }
    bool changed = false;
    bool full = false;
    if (trusted_now(clock)) {
        for (int c = 0; c < DSP_COND_COUNT; c++) {
            dsp_condition_state_t *cs = &n->s.cond[c];
            if (!(cs->flags & DSP_COND_F_ACTIVE) || (cs->flags & DSP_COND_F_RAISE_PENDING)) {
                continue;
            }
            dsp_utc_t latest = clock.now + DSP_NOTIFY_REMINDER_INTERVAL_S;
            if (cs->next_reminder_at == 0 || cs->next_reminder_at > latest) {
                /* First trusted time of the episode, or a backward correction: schedule /
                 * pull in, never further away than one interval. */
                cs->next_reminder_at = latest;
                if (cs->opened_at == 0) {
                    cs->opened_at = clock.now;
                }
                changed = true;
            } else if (clock.now >= cs->next_reminder_at) {
                /* One reminder, then the next interval from NOW: a forward jump never
                 * produces a burst of reminders. */
                if (cs->reminders < UINT8_MAX) {
                    cs->reminders++;
                }
                cs->next_reminder_at = latest;
                changed = true;
                if (!raise(n, (dsp_condition_t)c, cs->reminders, clock)) {
                    full = true;
                }
            }
        }
    }
    changed |= raise_deferred(n, clock, &full);
    return finish(n, changed, full ? DSP_NOTIFY_QUEUE_FULL : DSP_NOTIFY_OK);
}

size_t dsp_notify_pending_count(const dsp_notify_t *n)
{
    size_t count = 0;
    for (int i = 0; n != NULL && i < DSP_NOTIFY_QUEUE_CAPACITY; i++) {
        count += n->s.queue[i].seq != 0;
    }
    return count;
}

int dsp_notify_highest_pending_severity(const dsp_notify_t *n)
{
    int sev = 0;
    for (int i = 0; n != NULL && i < DSP_NOTIFY_QUEUE_CAPACITY; i++) {
        if (n->s.queue[i].seq != 0 && n->s.queue[i].severity > sev) {
            sev = n->s.queue[i].severity;
        }
    }
    return sev;
}

bool dsp_notify_delivery_requested(const dsp_notify_t *n)
{
    return dsp_notify_pending_count(n) > 0;
}

/* Appends a rejection to the log (oldest dropped when full; an unread drop is counted). */
static void log_rejection(dsp_notify_state_t *s, const dsp_note_t *e, const dsp_tx_detail_t *d)
{
    dsp_rejection_log_t *log = &s->rejections;
    if (log->count == DSP_REJECTION_LOG_CAPACITY) {
        if (log->unread == DSP_REJECTION_LOG_CAPACITY && log->lost_unread < UINT16_MAX) {
            log->lost_unread++; /* the dropped oldest entry had not been read */
        }
        memmove(&log->entry[0], &log->entry[1],
                (DSP_REJECTION_LOG_CAPACITY - 1) * sizeof log->entry[0]);
        log->count--;
        if (log->unread > log->count) {
            log->unread = log->count;
        }
    }
    dsp_rejection_t *r = &log->entry[log->count++];
    memset(r, 0, sizeof *r);
    r->seq = e->seq;
    r->type = e->type;
    r->flags = e->flags;
    r->code = e->code;
    r->http_status = d->http_status;
    r->error = d->error <= DSP_TXERR_MAX ? d->error : DSP_TXERR_OTHER;
    r->field = d->field <= DSP_TXFIELD_MAX ? d->field : DSP_TXFIELD_OTHER;
    r->attempts = e->attempts;
    if (log->unread < log->count) {
        log->unread++;
    }
    if (log->total < UINT32_MAX) {
        log->total++;
    }
}

/* A rejection must describe a 4xx response; anything else is treated as FAILED. */
static bool rejection_detail_ok(const dsp_tx_detail_t *d)
{
    return d->http_status >= 400 && d->http_status <= 499;
}

dsp_notify_delivery_report_t dsp_notify_deliver(dsp_notify_t *n, const dsp_notify_transport_t *tx,
                                                size_t max_sends, dsp_notify_clock_t clock)
{
    dsp_notify_delivery_report_t r = {0, 0, 0, false, false, DSP_NOTIFY_OK};
    if (n == NULL || !n->loaded || tx == NULL || tx->send == NULL) {
        r.status = DSP_NOTIFY_ERR_ARG;
        return r;
    }
    /* Durable before sent: no session while a write is outstanding. Each entry's final
     * payload is persisted by its freeze write below. */
    if (n->dirty && !persist(n)) {
        r.status = DSP_NOTIFY_ERR_STORAGE;
        return r;
    }
    bool changed = false;
    bool sent[DSP_NOTIFY_QUEUE_CAPACITY] = {false};
    while (r.attempted < max_sends) {
        int pick = -1; /* highest severity, then oldest */
        for (int i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY; i++) {
            const dsp_note_t *e = &n->s.queue[i];
            if (e->seq == 0 || sent[i]) {
                continue;
            }
            if (pick < 0 || e->severity > n->s.queue[pick].severity ||
                (e->severity == n->s.queue[pick].severity && e->seq < n->s.queue[pick].seq)) {
                pick = i;
            }
        }
        if (pick < 0) {
            break;
        }
        if (tx->can_send != NULL && !tx->can_send(tx->ctx)) {
            r.budget_exhausted = true; /* the entry is not touched: no attempt, no freeze */
            break;
        }
        sent[pick] = true;
        dsp_note_t *e = &n->s.queue[pick];
        bool first = e->attempts == 0;
        if (e->attempts < UINT16_MAX) {
            e->attempts++; /* counts STARTED attempts; attempts > 0 freezes the payload */
        }
        changed = true;
        /* Freeze durably before the first transmission: once the receiver may have the
         * event, flash must say so, or a restart could refresh the payload under the same
         * ID. If this write fails, nothing is sent (the entry stays frozen in RAM). */
        if (first && !persist(n)) {
            r.stopped_early = true;
            r.status = DSP_NOTIFY_ERR_STORAGE;
            return r;
        }
        r.attempted++;
        dsp_tx_detail_t detail;
        memset(&detail, 0, sizeof detail);
        dsp_tx_result_t res = tx->send(tx->ctx, e, n->s.epoch, &detail);
        if (res == DSP_TX_ACK) {
            memset(e, 0, sizeof *e);
            r.acked++;
            n->s.tx_diag = 0; /* end-to-end success: credentials, endpoint, protocol work */
            n->s.tx_diag_status = 0;
            continue;
        }
        if (res == DSP_TX_REJECTED && rejection_detail_ok(&detail)) {
            /* Never silent: log + removal in ONE write, before anything else. On failure
             * the entry stays queued exactly as before (RAM restored) and we stop. */
            dsp_note_t saved_entry = *e;
            dsp_rejection_log_t saved_log = n->s.rejections;
            log_rejection(&n->s, e, &detail);
            memset(e, 0, sizeof *e);
            if (!persist(n)) {
                *e = saved_entry;
                n->s.rejections = saved_log;
                r.stopped_early = true;
                r.status = DSP_NOTIFY_ERR_STORAGE;
                return r;
            }
            changed = false; /* that write persisted the whole RAM state */
            r.rejected++;
            if (r.rejected >= DSP_NOTIFY_MAX_REJECTIONS_PER_SESSION) {
                r.stopped_early = true; /* safeguard: the rest waits for the next session */
                break;
            }
            continue;
        }
        /* FAILED / UNCERTAIN (or an inconsistent rejection): same entry, same id, retried
         * next session. Systemic causes leave a persistent diagnostic. */
        uint8_t diag = (uint8_t)(detail.diag & DSP_TXDIAG_ALL);
        if (res == DSP_TX_REJECTED) {
            diag |= DSP_TXDIAG_PROTOCOL; /* transport bug: REJECTED without a 4xx */
        }
        if (diag != 0) {
            n->s.tx_diag |= diag;
            n->s.tx_diag_status = detail.http_status >= 100 && detail.http_status <= 599
                                      ? detail.http_status
                                      : 0;
        }
        r.stopped_early = true;
        break;
    }
    bool full = false;
    if (r.acked > 0 || r.rejected > 0) {
        changed |= raise_deferred(n, clock, &full);
    }
    r.status = finish(n, changed, DSP_NOTIFY_OK);
    return r;
}

uint8_t dsp_notify_tx_diag(const dsp_notify_t *n, uint16_t *status)
{
    if (status != NULL) {
        *status = n != NULL ? n->s.tx_diag_status : 0;
    }
    return n != NULL ? n->s.tx_diag : 0;
}

dsp_notify_status_t dsp_notify_clear_tx_diag(dsp_notify_t *n)
{
    if (n == NULL || !n->loaded) {
        return DSP_NOTIFY_ERR_ARG;
    }
    bool changed = n->s.tx_diag != 0 || n->s.tx_diag_status != 0;
    n->s.tx_diag = 0;
    n->s.tx_diag_status = 0;
    return finish(n, changed, DSP_NOTIFY_OK);
}

const dsp_rejection_log_t *dsp_notify_rejections(const dsp_notify_t *n)
{
    return n != NULL ? &n->s.rejections : NULL;
}

dsp_notify_status_t dsp_notify_ack_rejections(dsp_notify_t *n)
{
    if (n == NULL || !n->loaded) {
        return DSP_NOTIFY_ERR_ARG;
    }
    bool changed = n->s.rejections.unread != 0 || n->s.rejections.lost_unread != 0;
    n->s.rejections.unread = 0;
    n->s.rejections.lost_unread = 0;
    return finish(n, changed, DSP_NOTIFY_OK);
}

bool dsp_notify_format_id(uint64_t device_id, uint32_t epoch, uint32_t seq, char *buf, size_t len)
{
    if (buf == NULL || len < DSP_NOTIFY_ID_BUF_LEN || seq == 0 || epoch == 0) {
        if (buf != NULL && len > 0) {
            buf[0] = '\0';
        }
        return false;
    }
    int w = snprintf(buf, len, "dsp-%012llx-%08lx-%lu",
                     (unsigned long long)(device_id & 0xFFFFFFFFFFFFull), (unsigned long)epoch,
                     (unsigned long)seq);
    return w > 0 && (size_t)w < len;
}
