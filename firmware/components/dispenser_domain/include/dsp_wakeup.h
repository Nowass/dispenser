#ifndef DSP_WAKEUP_H
#define DSP_WAKEUP_H

/*
 * Next-wakeup calculation for the future power manager (milestone M1.4). Pure and
 * testable: the earliest of the pending wake reasons, as a sleep duration relative to the
 * current raw clock reading. Works even while the clock is untrusted (then only the time
 * sync is scheduled; the RTC still counts time since power-on). No timing guarantee is
 * implied: RTC accuracy in deep sleep is a hardware question still to be measured.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dsp_exec.h"
#include "dsp_recurrence.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DSP_WAKE_NONE = 0,
    DSP_WAKE_DOSE,
    DSP_WAKE_TIME_SYNC,
    DSP_WAKE_NOTIFICATION, /* future: pending alert delivery */
    DSP_WAKE_OTHER,        /* future: any further reason */
    DSP_WAKE_MAX_SLEEP,    /* nothing earlier: periodic re-evaluation */
} dsp_wake_reason_t;

typedef struct {
    bool has_dose;
    dsp_utc_t dose_at;
    bool has_sync;
    dsp_utc_t sync_at;
    bool has_notification;
    dsp_utc_t notification_at;
    bool has_other;
    dsp_utc_t other_at;
} dsp_wake_inputs_t;

typedef struct {
    dsp_wake_reason_t reason;
    dsp_utc_t wake_at;
    int64_t sleep_s; /* 0 = act now */
} dsp_wake_plan_t;

/*
 * Earliest pending reason; past or current instants give sleep_s = 0. Ties: dose first,
 * then sync, notification, other. max_sleep_s (> 0) caps the sleep (reason MAX_SLEEP).
 */
dsp_wake_plan_t dsp_wake_plan(const dsp_wake_inputs_t *in, dsp_utc_t now, int64_t max_sleep_s);

/* Next dose instant strictly after clock.now across enabled schedules (M1.1/M1.2 rules).
 * false when the clock is not valid (no dose can be scheduled) or there is none. */
bool dsp_wake_next_dose(const dsp_schedule_t *schedules, size_t n, const dsp_tz_t *tz,
                        dsp_clock_t clock, dsp_utc_t *at);

#ifdef __cplusplus
}
#endif

#endif /* DSP_WAKEUP_H */
