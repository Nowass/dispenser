#ifndef DSP_SCHEDULE_SET_H
#define DSP_SCHEDULE_SET_H

/*
 * The persistent set of dosing schedules (milestone M1.3), stored as ONE record so that
 * any change (create, edit, delete, or a full sync from Android) replaces the whole set
 * atomically (approved 2026-11-15).
 */

#include <stdint.h>

#include "dsp_recurrence.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_SCHEDULE_CAPACITY 16  /* approved maximum number of stored schedules */
#define DSP_TOMBSTONE_CAPACITY 16 /* recently deleted IDs kept to refuse their reuse */

typedef struct {
    uint32_t revision; /* set revision: 0 = provisioned empty set, +1 per change */
    uint8_t count;     /* number of valid entries in schedules[] */
    dsp_schedule_t schedules[DSP_SCHEDULE_CAPACITY];
    uint8_t tombstone_count; /* entries in tombstones[], oldest first */
    char tombstones[DSP_TOMBSTONE_CAPACITY][DSP_SCHEDULE_ID_MAX_LEN + 1];
} dsp_schedule_set_t;

#ifdef __cplusplus
}
#endif

#endif /* DSP_SCHEDULE_SET_H */
