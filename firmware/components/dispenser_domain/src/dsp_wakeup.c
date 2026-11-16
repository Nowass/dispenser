#include "dsp_wakeup.h"

static void consider(dsp_wake_plan_t *p, bool present, dsp_utc_t at, dsp_wake_reason_t reason)
{
    /* Strictly earlier wins, so on ties the reason considered first is kept. */
    if (present && (p->reason == DSP_WAKE_NONE || at < p->wake_at)) {
        p->reason = reason;
        p->wake_at = at;
    }
}

dsp_wake_plan_t dsp_wake_plan(const dsp_wake_inputs_t *in, dsp_utc_t now, int64_t max_sleep_s)
{
    dsp_wake_plan_t p = {DSP_WAKE_NONE, now, 0};
    if (in == NULL || max_sleep_s <= 0) {
        return p; /* act now / re-evaluate */
    }
    consider(&p, in->has_dose, in->dose_at, DSP_WAKE_DOSE);
    consider(&p, in->has_sync, in->sync_at, DSP_WAKE_TIME_SYNC);
    consider(&p, in->has_other, in->other_at, DSP_WAKE_OTHER);
    /* Overflow-safe cap: compare differences, never add to an arbitrary instant. */
    if (p.reason == DSP_WAKE_NONE || (p.wake_at > now && p.wake_at - now > max_sleep_s)) {
        p.reason = DSP_WAKE_MAX_SLEEP;
        p.wake_at = now + max_sleep_s;
    }
    p.sleep_s = p.wake_at > now ? p.wake_at - now : 0;
    return p;
}

bool dsp_wake_next_dose(const dsp_schedule_t *schedules, size_t n, const dsp_tz_t *tz,
                        dsp_clock_t clock, dsp_utc_t *at)
{
    if (!clock.valid || at == NULL) {
        return false;
    }
    size_t index = 0;
    dsp_occurrence_t occ;
    if (dsp_rec_next_of_set(schedules, n, tz, clock.now, &index, &occ) != DSP_REC_OK) {
        return false;
    }
    *at = occ.at;
    return true;
}
