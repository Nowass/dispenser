#include "dsp_exec.h"

#include <string.h>

static bool config_valid(const dsp_exec_config_t *cfg)
{
    return cfg != NULL && dsp_history_store_is_valid(cfg->store) && dsp_tz_is_valid(cfg->tz) &&
           cfg->due_window_s >= 0 && cfg->due_window_s <= DSP_EXEC_DUE_WINDOW_MAX_S &&
           cfg->retention_days >= DSP_EXEC_RETENTION_DAYS_MIN &&
           cfg->retention_days <= DSP_EXEC_RETENTION_DAYS_MAX;
}

dsp_exec_status_t dsp_exec_init(dsp_exec_t *ex, const dsp_exec_config_t *cfg)
{
    if (ex == NULL) {
        return DSP_EXEC_ERR_ARG;
    }
    memset(ex, 0, sizeof *ex);
    if (!config_valid(cfg)) {
        return DSP_EXEC_ERR_ARG;
    }
    ex->cfg = *cfg;
    ex->initialized = true;
    ex->next_seq = 1;
    return DSP_EXEC_OK;
}

static bool ready(const dsp_exec_t *ex)
{
    return ex != NULL && ex->initialized;
}

/* Everything except history: shared by check and claim. *prune_before_days receives the
 * retention cutoff (day number) for a subsequent history scan. */
static dsp_exec_status_t precheck(const dsp_exec_t *ex, const dsp_schedule_t *s,
                                  const dsp_occurrence_t *occ, dsp_clock_t clock,
                                  int32_t *prune_before_days)
{
    if (!ready(ex) || s == NULL || occ == NULL) {
        return DSP_EXEC_ERR_ARG;
    }
    const dsp_tz_t *tz = ex->cfg.tz;
    if (dsp_schedule_validate(s, tz) != DSP_SCHED_OK) {
        return DSP_EXEC_ERR_SCHEDULE;
    }
    dsp_date_t today;
    if (!clock.valid || !dsp_utc_to_local_date(tz, clock.now, &today)) {
        return DSP_EXEC_ERR_CLOCK;
    }
    if (!s->enabled) {
        return DSP_EXEC_DISABLED;
    }
    /* Identity: the occurrence must be exactly what the current schedule yields on that
     * local date. Rejects fabricated occurrences and stale ones computed before an edit. */
    dsp_occurrence_t expected;
    if (!dsp_date_is_valid(occ->local_date) ||
        dsp_rec_occurrence_on(s, tz, occ->local_date, &expected) != DSP_REC_OK ||
        expected.at != occ->at) {
        return DSP_EXEC_ERR_OCCURRENCE;
    }
    /* Both instants are within the supported range: no overflow below. */
    if (clock.now < occ->at) {
        return DSP_EXEC_NOT_DUE;
    }
    if (clock.now - occ->at > (dsp_utc_t)ex->cfg.due_window_s) {
        return DSP_EXEC_MISSED;
    }
    if (clock.not_before != 0 && occ->at < clock.not_before) {
        return DSP_EXEC_MISSED; /* jumped over by a forward clock correction (M1.4) */
    }
    if (ex->active.kind != DSP_EXEC_KIND_NONE) {
        return DSP_EXEC_BUSY;
    }
    *prune_before_days = dsp_date_to_days(today) - ex->cfg.retention_days;
    return DSP_EXEC_OK;
}

/* Eligibility including history. On DSP_EXEC_OK *scan holds the history scan. */
static dsp_exec_status_t evaluate(const dsp_exec_t *ex, const dsp_schedule_t *s,
                                  const dsp_occurrence_t *occ, dsp_clock_t clock,
                                  dsp_history_scan_t *scan)
{
    int32_t prune_before = 0;
    dsp_exec_status_t st = precheck(ex, s, occ, clock, &prune_before);
    if (st != DSP_EXEC_OK) {
        return st;
    }
    st = dsp_history_scan(ex->cfg.store, s->id, prune_before, scan);
    if (st != DSP_EXEC_OK) {
        return st;
    }
    /* Watermark: a claim on this date or a later one blocks (any state). */
    if (scan->found && dsp_date_compare(scan->found_record.local_date, occ->local_date) >= 0) {
        return DSP_EXEC_ALREADY_CLAIMED;
    }
    if (!scan->found && !scan->has_empty && !scan->has_prunable) {
        return DSP_EXEC_ERR_CAPACITY;
    }
    return DSP_EXEC_OK;
}

dsp_exec_status_t dsp_exec_check(const dsp_exec_t *ex, const dsp_schedule_t *schedule,
                                 const dsp_occurrence_t *occ, dsp_clock_t clock)
{
    dsp_history_scan_t scan;
    return evaluate(ex, schedule, occ, clock, &scan);
}

dsp_exec_status_t dsp_exec_claim(dsp_exec_t *ex, const dsp_schedule_t *schedule,
                                 const dsp_occurrence_t *occ, dsp_clock_t clock,
                                 dsp_exec_token_t *token)
{
    if (token == NULL) {
        return DSP_EXEC_ERR_ARG;
    }
    dsp_history_scan_t scan;
    dsp_exec_status_t st = evaluate(ex, schedule, occ, clock, &scan);
    if (st != DSP_EXEC_OK) {
        return st;
    }
    /* Slot: update this schedule's record in place, else an empty slot, else the oldest
     * record past retention. One slot, one write, one commit. */
    size_t slot = scan.found ? scan.found_slot : scan.has_empty ? scan.empty_slot
                                                                : scan.prunable_slot;
    dsp_history_record_t rec;
    memset(&rec, 0, sizeof rec);
    memcpy(rec.schedule_id, schedule->id, sizeof rec.schedule_id);
    rec.local_date = occ->local_date;
    rec.state = DSP_HISTORY_STARTED;
    rec.schedule_revision = schedule->revision;
    rec.scheduled_at = occ->at;
    rec.claimed_at = clock.now;
    rec.volume_ul = schedule->amount_ul; /* durable claimed volume (M1.9a, Q11) */

    st = dsp_history_put(ex->cfg.store, slot, &rec);
    if (st != DSP_EXEC_OK) {
        return st; /* pump NOT authorized */
    }
    ex->active.kind = DSP_EXEC_KIND_AUTOMATIC;
    ex->active.seq = ex->next_seq++;
    ex->active.slot = slot;
    ex->active.record = rec;
    *token = ex->active;
    return DSP_EXEC_OK;
}

static bool token_is_active(const dsp_exec_t *ex, const dsp_exec_token_t *token)
{
    return token->kind != DSP_EXEC_KIND_NONE && token->kind == ex->active.kind &&
           token->seq == ex->active.seq &&
           (token->kind == DSP_EXEC_KIND_MANUAL ||
            (token->slot == ex->active.slot &&
             dsp_history_record_equal(&token->record, &ex->active.record)));
}

dsp_exec_status_t dsp_exec_finish(dsp_exec_t *ex, const dsp_exec_token_t *token, uint8_t outcome)
{
    if (!ready(ex) || token == NULL ||
        (outcome != DSP_HISTORY_COMPLETED && outcome != DSP_HISTORY_FAILED) ||
        !token_is_active(ex, token)) {
        return DSP_EXEC_ERR_ARG;
    }
    dsp_exec_token_t active = ex->active;
    memset(&ex->active, 0, sizeof ex->active); /* released in every case below */
    if (active.kind == DSP_EXEC_KIND_MANUAL) {
        return DSP_EXEC_OK;
    }
    /* The slot must still hold our STARTED claim; never overwrite anything else. */
    dsp_history_record_t current;
    dsp_store_status_t rs = ex->cfg.store->read(ex->cfg.store->ctx, active.slot, &current);
    if (rs == DSP_STORE_ERR_READ) {
        return DSP_EXEC_ERR_STORAGE_READ;
    }
    if (rs != DSP_STORE_OK || !dsp_history_record_equal(&current, &active.record)) {
        return DSP_EXEC_ERR_INTERNAL;
    }
    dsp_history_record_t done = active.record;
    done.state = outcome;
    return dsp_history_put(ex->cfg.store, active.slot, &done);
}

dsp_exec_status_t dsp_exec_manual_begin(dsp_exec_t *ex, dsp_exec_token_t *token)
{
    if (!ready(ex) || token == NULL) {
        return DSP_EXEC_ERR_ARG;
    }
    if (ex->active.kind != DSP_EXEC_KIND_NONE) {
        return DSP_EXEC_BUSY;
    }
    memset(&ex->active, 0, sizeof ex->active);
    ex->active.kind = DSP_EXEC_KIND_MANUAL;
    ex->active.seq = ex->next_seq++;
    *token = ex->active;
    return DSP_EXEC_OK;
}

dsp_exec_status_t dsp_exec_recover(dsp_exec_t *ex, size_t *interrupted)
{
    if (!ready(ex)) {
        return DSP_EXEC_ERR_ARG;
    }
    if (interrupted != NULL) {
        *interrupted = 0;
    }
    if (ex->active.kind != DSP_EXEC_KIND_NONE) {
        return DSP_EXEC_BUSY;
    }
    const dsp_history_store_t *store = ex->cfg.store;
    /* Validate the whole history first (corruption, duplicate IDs): fail-safe. */
    dsp_history_scan_t scan;
    dsp_exec_status_t vst = dsp_history_scan(store, NULL, INT32_MIN, &scan);
    if (vst != DSP_EXEC_OK) {
        return vst;
    }
    for (size_t slot = 0; slot < store->slot_count; slot++) {
        dsp_history_record_t rec;
        dsp_store_status_t rs = store->read(store->ctx, slot, &rec);
        if (rs == DSP_STORE_EMPTY) {
            continue;
        }
        if (rs != DSP_STORE_OK) {
            return DSP_EXEC_ERR_STORAGE_READ;
        }
        if (!dsp_history_record_is_valid(&rec)) {
            return DSP_EXEC_ERR_INTERNAL;
        }
        if (rec.state != DSP_HISTORY_STARTED) {
            continue;
        }
        rec.state = DSP_HISTORY_INTERRUPTED;
        dsp_exec_status_t st = dsp_history_put(store, slot, &rec);
        if (st != DSP_EXEC_OK) {
            return st;
        }
        if (interrupted != NULL) {
            (*interrupted)++;
        }
    }
    return DSP_EXEC_OK;
}

dsp_exec_status_t dsp_exec_find_started(const dsp_exec_t *ex, size_t *slot,
                                        dsp_history_record_t *rec)
{
    if (!ready(ex) || slot == NULL || rec == NULL) {
        return DSP_EXEC_ERR_ARG;
    }
    const dsp_history_store_t *store = ex->cfg.store;
    dsp_history_scan_t scan;
    dsp_exec_status_t vst = dsp_history_scan(store, NULL, INT32_MIN, &scan);
    if (vst != DSP_EXEC_OK) {
        return vst;
    }
    for (size_t i = 0; i < store->slot_count; i++) {
        dsp_history_record_t r;
        dsp_store_status_t rs = store->read(store->ctx, i, &r);
        if (rs == DSP_STORE_EMPTY) {
            continue;
        }
        if (rs != DSP_STORE_OK) {
            return DSP_EXEC_ERR_STORAGE_READ;
        }
        if (!dsp_history_record_is_valid(&r)) {
            return DSP_EXEC_ERR_INTERNAL;
        }
        if (r.state == DSP_HISTORY_STARTED) {
            *slot = i;
            *rec = r;
            return DSP_EXEC_OK;
        }
    }
    return DSP_EXEC_NOT_FOUND;
}

dsp_exec_status_t dsp_exec_mark_interrupted(dsp_exec_t *ex, size_t slot,
                                            const dsp_history_record_t *rec)
{
    if (!ready(ex) || rec == NULL || slot >= ex->cfg.store->slot_count ||
        rec->state != DSP_HISTORY_STARTED) {
        return DSP_EXEC_ERR_ARG;
    }
    if (ex->active.kind != DSP_EXEC_KIND_NONE) {
        return DSP_EXEC_BUSY;
    }
    const dsp_history_store_t *store = ex->cfg.store;
    dsp_history_record_t cur;
    dsp_store_status_t rs = store->read(store->ctx, slot, &cur);
    if (rs != DSP_STORE_OK) {
        return rs == DSP_STORE_EMPTY ? DSP_EXEC_ERR_ARG : DSP_EXEC_ERR_STORAGE_READ;
    }
    if (!dsp_history_record_equal(&cur, rec)) {
        return DSP_EXEC_ERR_ARG; /* the slot changed: never rewrite something else */
    }
    cur.state = DSP_HISTORY_INTERRUPTED;
    return dsp_history_put(store, slot, &cur);
}

dsp_exec_status_t dsp_exec_lookup(const dsp_exec_t *ex, const char *schedule_id,
                                  dsp_history_record_t *out)
{
    if (!ready(ex) || out == NULL || !dsp_schedule_id_is_valid(schedule_id)) {
        return DSP_EXEC_ERR_ARG;
    }
    dsp_history_scan_t scan;
    dsp_exec_status_t st = dsp_history_scan(ex->cfg.store, schedule_id, INT32_MIN, &scan);
    if (st != DSP_EXEC_OK) {
        return st;
    }
    if (!scan.found) {
        return DSP_EXEC_NOT_FOUND;
    }
    *out = scan.found_record;
    return DSP_EXEC_OK;
}
