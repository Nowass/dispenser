#ifndef DSP_TIME_META_H
#define DSP_TIME_META_H

/*
 * Persistent time metadata (milestone M1.4), stored as the "time" record (storage layout
 * v2). Written only when it changes: after a successful synchronization (<= once per sync
 * interval) and when a rollback hold starts or ends. A stored timestamp is NEVER proof
 * that the current time is valid (see dsp_timekeeping.h).
 */

#include <stdbool.h>
#include <stdint.h>

#include "dsp_time.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    dsp_utc_t last_sync_utc;   /* time of the last accepted synchronization; 0 = never */
    dsp_utc_t high_water_utc;  /* highest trusted time ever observed; 0 = none */
    dsp_utc_t hold_until_utc;  /* automatic dosing held while trusted now < this; 0 = none */
    int64_t last_correction_s; /* new minus previous clock at the last sync (diagnostic;
                                  0 when the previous clock was not trusted) */
    dsp_utc_t not_before_utc;  /* dosing floor: occurrences scheduled before it are never
                                  executed automatically. Raised by a large forward
                                  correction and by every initial sync; MONOTONIC (never
                                  lowered); 0 = none */
} dsp_time_meta_t;

/* Instants (incl. not_before_utc) are 0 or within DSP_UTC_MIN..DSP_UTC_MAX;
 * |last_correction_s| <= DSP_UTC_MAX. */
bool dsp_time_meta_is_valid(const dsp_time_meta_t *meta);

bool dsp_time_meta_equal(const dsp_time_meta_t *a, const dsp_time_meta_t *b);

#ifdef __cplusplus
}
#endif

#endif /* DSP_TIME_META_H */
