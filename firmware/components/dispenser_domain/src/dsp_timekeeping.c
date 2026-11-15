#include "dsp_timekeeping.h"

#include <stddef.h>
#include <string.h>

static bool instant_ok(dsp_utc_t t)
{
    return t == 0 || (t >= DSP_UTC_MIN && t <= DSP_UTC_MAX);
}

bool dsp_time_meta_is_valid(const dsp_time_meta_t *m)
{
    return m != NULL && instant_ok(m->last_sync_utc) && instant_ok(m->high_water_utc) &&
           instant_ok(m->hold_until_utc) && instant_ok(m->not_before_utc) &&
           m->last_correction_s >= -(int64_t)DSP_UTC_MAX &&
           m->last_correction_s <= (int64_t)DSP_UTC_MAX;
}

bool dsp_time_meta_equal(const dsp_time_meta_t *a, const dsp_time_meta_t *b)
{
    return a->last_sync_utc == b->last_sync_utc && a->high_water_utc == b->high_water_utc &&
           a->hold_until_utc == b->hold_until_utc && a->last_correction_s == b->last_correction_s &&
           a->not_before_utc == b->not_before_utc;
}

static bool plausible(dsp_utc_t t)
{
    return t >= DSP_TIME_PLAUSIBLE_MIN && t <= DSP_UTC_MAX;
}

static bool trusted(const dsp_timekeeper_t *tk)
{
    return tk->trust != DSP_TRUST_UNKNOWN;
}

bool dsp_timekeeper_init(dsp_timekeeper_t *tk, const dsp_time_meta_t *meta, uint16_t interval_h,
                         const dsp_time_retained_t *retained)
{
    if (tk == NULL) {
        return false;
    }
    memset(tk, 0, sizeof *tk);
    if (!dsp_time_meta_is_valid(meta) || interval_h < 24 || interval_h > 48) {
        return false;
    }
    tk->meta = *meta;
    tk->interval_s = (int64_t)interval_h * 3600;
    tk->trust = DSP_TRUST_UNKNOWN;
    tk->reason = meta->last_sync_utc == 0 ? DSP_UNTRUSTED_NEVER_SYNCED : DSP_UNTRUSTED_TIME_BASE_LOST;
    if (retained != NULL) {
        tk->retained = *retained;
    }
    tk->initialized = true;
    return true;
}

void dsp_timekeeper_evaluate(dsp_timekeeper_t *tk, dsp_time_reading_t r)
{
    if (tk == NULL || !tk->initialized) {
        return;
    }
    tk->trust = DSP_TRUST_UNKNOWN;
    if (tk->meta.last_sync_utc == 0) {
        tk->reason = DSP_UNTRUSTED_NEVER_SYNCED;
    } else if (!r.time_base_continuous) {
        tk->reason = DSP_UNTRUSTED_TIME_BASE_LOST;
    } else if (!plausible(r.now)) {
        tk->reason = DSP_UNTRUSTED_OUT_OF_RANGE;
    } else if (r.now < tk->meta.last_sync_utc - DSP_TIME_CONTINUITY_TOLERANCE_S) {
        tk->reason = DSP_UNTRUSTED_BEHIND_LAST_SYNC; /* a continuous clock never runs back */
    } else {
        tk->reason = DSP_UNTRUSTED_NONE;
        tk->trust = r.now - tk->meta.last_sync_utc < tk->interval_s ? DSP_TRUST_SYNCED
                                                                     : DSP_TRUST_HOLDOVER;
        if (tk->meta.hold_until_utc != 0 && r.now >= tk->meta.hold_until_utc) {
            tk->meta.hold_until_utc = 0; /* rollback hold expired */
            tk->meta_dirty = true;
        }
    }
    /* Keep the retry schedule sane (it may come from RTC memory or be absent). */
    int64_t cap = trusted(tk) ? tk->interval_s : DSP_TIME_RETRY_UNKNOWN_MAX_S;
    if (tk->retained.next_attempt > r.now + cap || tk->retained.next_attempt < 0) {
        tk->retained.next_attempt =
            trusted(tk) ? tk->meta.last_sync_utc + tk->interval_s : r.now;
        if (tk->retained.next_attempt > r.now + cap) {
            tk->retained.next_attempt = r.now + cap;
        }
    } else if (trusted(tk) && tk->retained.next_attempt == 0) {
        tk->retained.next_attempt = tk->meta.last_sync_utc + tk->interval_s;
    }
}

bool dsp_timekeeper_hold_active(const dsp_timekeeper_t *tk, dsp_utc_t now)
{
    return tk != NULL && tk->meta.hold_until_utc != 0 && now < tk->meta.hold_until_utc;
}

dsp_clock_t dsp_timekeeper_clock(const dsp_timekeeper_t *tk, dsp_utc_t now)
{
    dsp_clock_t c = {now, false, 0};
    if (tk != NULL && tk->initialized && trusted(tk) && plausible(now) &&
        !dsp_timekeeper_hold_active(tk, now)) {
        c.valid = true;
        /* Persisted, monotonic floor set by large forward corrections: occurrences that were
         * jumped over are never executed, also after a restart, a deep-sleep wake, a power
         * loss with re-synchronization or any number of later (small) corrections. */
        c.not_before = tk->meta.not_before_utc;
    }
    return c;
}

bool dsp_timekeeper_sync_due(const dsp_timekeeper_t *tk, dsp_utc_t now)
{
    return tk != NULL && tk->initialized && now >= tk->retained.next_attempt;
}

dsp_utc_t dsp_timekeeper_next_sync(const dsp_timekeeper_t *tk)
{
    return tk != NULL ? tk->retained.next_attempt : 0;
}

bool dsp_timekeeper_sync_overdue(const dsp_timekeeper_t *tk, dsp_utc_t now)
{
    return tk != NULL && trusted(tk) && now - tk->meta.last_sync_utc > 2 * tk->interval_s;
}

static int64_t backoff_s(const dsp_timekeeper_t *tk)
{
    bool t = trusted(tk);
    int64_t delay = t ? DSP_TIME_RETRY_TRUSTED_MIN_S : DSP_TIME_RETRY_UNKNOWN_MIN_S;
    int64_t cap = t ? tk->interval_s : DSP_TIME_RETRY_UNKNOWN_MAX_S;
    for (uint16_t i = 1; i < tk->retained.consecutive_failures && delay < cap; i++) {
        delay *= 2;
    }
    return delay < cap ? delay : cap;
}

static dsp_sync_decision_t fail(dsp_timekeeper_t *tk, dsp_sync_status_t status, dsp_utc_t now)
{
    dsp_sync_decision_t d = {false, 0, DSP_CORRECTION_NONE, 0, status};
    if (tk->retained.consecutive_failures < UINT16_MAX) {
        tk->retained.consecutive_failures++;
    }
    tk->retained.next_attempt = now + backoff_s(tk); /* trust unchanged */
    return d;
}

dsp_sync_decision_t dsp_timekeeper_on_sync(dsp_timekeeper_t *tk, dsp_sync_status_t status,
                                           const dsp_time_sample_t *sample, dsp_utc_t now)
{
    dsp_sync_decision_t d = {false, 0, DSP_CORRECTION_NONE, 0, DSP_SYNC_ERR_SOURCE};
    if (tk == NULL || !tk->initialized) {
        return d;
    }
    if (status != DSP_SYNC_OK) {
        return fail(tk, status, now);
    }
    if (sample == NULL || !plausible(sample->source_utc)) {
        return fail(tk, DSP_SYNC_ERR_IMPLAUSIBLE, now);
    }
    dsp_utc_t source = sample->source_utc;
    bool was_trusted = trusted(tk) && plausible(sample->clock_utc);
    dsp_utc_t prev_high = tk->meta.high_water_utc;
    if (was_trusted && sample->clock_utc > prev_high) {
        prev_high = sample->clock_utc;
    }
    d.status = DSP_SYNC_OK;
    d.offset_s = was_trusted ? source - sample->clock_utc : 0;
    if (prev_high != 0 && source < prev_high - DSP_TIME_ROLLBACK_LIMIT_S) {
        dsp_utc_t hold = prev_high - DSP_TIME_ROLLBACK_LIMIT_S;
        if (hold > tk->meta.hold_until_utc) {
            tk->meta.hold_until_utc = hold;
        }
        d.kind = DSP_CORRECTION_ROLLBACK_HOLD;
    } else {
        if (tk->meta.hold_until_utc != 0 && source >= tk->meta.hold_until_utc) {
            tk->meta.hold_until_utc = 0; /* a later sync jumped past the hold */
        }
        if (!was_trusted) {
            d.kind = DSP_CORRECTION_INITIAL;
        } else if (d.offset_s <= DSP_TIME_SIGNIFICANT_S && d.offset_s >= -DSP_TIME_SIGNIFICANT_S) {
            d.kind = DSP_CORRECTION_SMALL;
        } else {
            d.kind = d.offset_s > 0 ? DSP_CORRECTION_FORWARD_LARGE : DSP_CORRECTION_BACKWARD_LARGE;
        }
        if (d.kind == DSP_CORRECTION_FORWARD_LARGE && source + 1 > tk->meta.not_before_utc) {
            tk->meta.not_before_utc = source + 1; /* monotonic: never lowered */
        }
    }
    /* Initial synchronization (the previous clock was not trusted: first boot, power loss,
     * lost continuity): nothing scheduled at or before the moment time is re-established
     * may run, because what the device did before (under an unverifiable clock) is
     * unknown. Approved 2026-11-15: one missed dose is preferable to a possible extra one.
     * Monotonic, persisted before the time is applied (durable before effective). */
    if (!was_trusted && source + 1 > tk->meta.not_before_utc) {
        tk->meta.not_before_utc = source + 1;
    }
    tk->meta.last_sync_utc = source;
    tk->meta.high_water_utc = source > prev_high ? source : prev_high;
    tk->meta.last_correction_s = d.offset_s;
    tk->meta_dirty = true;
    tk->trust = DSP_TRUST_SYNCED;
    tk->reason = DSP_UNTRUSTED_NONE;
    tk->retained.consecutive_failures = 0;
    tk->retained.next_attempt = source + tk->interval_s;
    d.apply = true;
    d.set_to = source;
    return d;
}
