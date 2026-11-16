#ifndef DSP_RECURRENCE_H
#define DSP_RECURRENCE_H

/*
 * Recurrence engine (milestone M1.1): which local dates a schedule occurs on and at
 * which UTC instant. Pure, deterministic C11; behavioral reference is the Android
 * Kotlin RecurrenceEngine (android/app/src/main/java/cz/nowass/dispenser/domain/).
 *
 * Rules:
 *   ONCE    exactly on start_date.
 *   DAILY   every `interval` calendar days counted from start_date.
 *   WEEKLY  selected weekdays in weeks (Monday-based) whose distance from the week
 *           containing start_date is divisible by `interval`; never before start_date.
 *   Optional end date is inclusive. Dates after DSP_DATE_MAX_YEAR never occur.
 *
 * DST (FR-015, in the given timezone):
 *   nonexistent local time (spring forward): that day is skipped, never shifted;
 *   repeated local time (fall back): one occurrence, at the earlier UTC instant.
 *
 * At most one occurrence per schedule per local date. Disabled schedules have no
 * occurrences in every query below.
 *
 * SAFETY: this module only CALCULATES planned occurrences. A returned occurrence is
 * not a command to dose; eligibility (already handled, missed, reservoir, service
 * mode, clock validity) is decided by the future dosing coordinator. There is no
 * catch-up: queries return only occurrences strictly after a given instant.
 *
 * Memory: no allocation, no global mutable state. All buffers are caller-owned.
 * Execution time: every call is bounded (see firmware/README.md "Recurrence engine").
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dsp_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Schedule id: 1..64 characters of [A-Za-z0-9._-] (same rule as the Android backup
 * format; keeps '@', the occurrence-id separator, out). */
#define DSP_SCHEDULE_ID_MAX_LEN 64
/* "<id>@YYYY-MM-DD" plus NUL. */
#define DSP_OCCURRENCE_ID_BUF_LEN (DSP_SCHEDULE_ID_MAX_LEN + 1 + 10 + 1)
/* Schedule display name: at most 32 bytes of UTF-8 (approved D9). */
#define DSP_SCHEDULE_NAME_MAX_BYTES 32
/* Upper bound of `interval` (Kotlin Int.MAX_VALUE, so every Android schedule fits). */
#define DSP_REC_INTERVAL_MAX 2147483647u

/* Recurrence kinds (stored as uint8_t for a stable serialized size). */
enum {
    DSP_REC_ONCE = 0,
    DSP_REC_DAILY = 1,
    DSP_REC_WEEKLY = 2,
};

/* Weekday mask: bit 0 = Monday ... bit 6 = Sunday (as in Android `weekdays_mask`). */
#define DSP_WEEKDAY_BIT(wd) ((uint8_t)(1u << (wd)))
#define DSP_WEEKDAY_MASK_ALL 0x7Fu

/*
 * One dosing schedule in canonical form (as stored and transferred). Fixed-size,
 * no pointers. ONCE: interval = 1, weekday_mask = 0, no end date.
 * DAILY: weekday_mask = 0.
 */
typedef struct {
    char id[DSP_SCHEDULE_ID_MAX_LEN + 1]; /* NUL-terminated */
    uint32_t revision;                    /* >= 1, incremented on every edit */
    bool enabled;
    uint32_t amount_ul;    /* requested volume, integer microlitres, >= 1 */
    dsp_date_t start_date; /* local date in the schedule timezone */
    uint8_t hour;          /* local time 0..23 */
    uint8_t minute;        /* 0..59 */
    uint8_t kind;          /* DSP_REC_ONCE / DAILY / WEEKLY */
    uint32_t interval;     /* days (DAILY) or weeks (WEEKLY), 1..DSP_REC_INTERVAL_MAX */
    uint8_t weekday_mask;  /* WEEKLY only */
    bool has_end_date;
    dsp_date_t end_date;   /* inclusive; valid only if has_end_date */
    /* M1.10b.1 (schedule set format 2). Both default to zero = format 1 behavior. */
    char name[DSP_SCHEDULE_NAME_MAX_BYTES + 1]; /* display name: UTF-8, 0..32 bytes, no
                                                   control characters, NUL-terminated,
                                                   stored as sent (not normalized) */
    dsp_utc_t not_before; /* device-maintained edit floor: occurrences scheduled before it
                             are never executed automatically (they are MISSED). Raised to
                             the trusted edit time + 1 s by create, by edits of timing
                             fields and by re-enabling; never lowered; never sent over BLE.
                             0 = none */
} dsp_schedule_t;

/* Validation result: bitwise OR of all detected problems, 0 = valid. */
typedef uint32_t dsp_sched_errors_t;
enum {
    DSP_SCHED_OK = 0,
    DSP_SCHED_E_ARG = 1u << 0,                 /* schedule pointer is NULL */
    DSP_SCHED_E_TZ = 1u << 1,                  /* timezone NULL or invalid */
    DSP_SCHED_E_ID = 1u << 2,                  /* empty, too long, bad character, no NUL */
    DSP_SCHED_E_REVISION = 1u << 3,            /* revision 0 */
    DSP_SCHED_E_AMOUNT_NOT_POSITIVE = 1u << 4, /* amount_ul 0   (Kotlin AMOUNT_NOT_POSITIVE) */
    DSP_SCHED_E_START_DATE = 1u << 5,          /* not a date or outside 2000..2099 */
    DSP_SCHED_E_TIME = 1u << 6,                /* hour > 23 or minute > 59 */
    DSP_SCHED_E_KIND = 1u << 7,                /* unknown recurrence kind */
    DSP_SCHED_E_INTERVAL_TOO_SMALL = 1u << 8,  /* 0 for DAILY/WEEKLY (Kotlin INTERVAL_TOO_SMALL) */
    DSP_SCHED_E_INTERVAL_TOO_LARGE = 1u << 9,  /* > DSP_REC_INTERVAL_MAX */
    DSP_SCHED_E_NO_WEEKDAYS = 1u << 10,        /* WEEKLY with empty mask (Kotlin NO_WEEKDAYS) */
    DSP_SCHED_E_WEEKDAYS = 1u << 11,           /* bit 7 set, or mask on ONCE/DAILY */
    DSP_SCHED_E_END_DATE = 1u << 12,           /* end date not a date or outside 2000..2099 */
    DSP_SCHED_E_END_BEFORE_START = 1u << 13,   /* Kotlin END_BEFORE_START */
    DSP_SCHED_E_ONCE_FORM = 1u << 14,          /* ONCE with interval != 1 or an end date */
    DSP_SCHED_E_TIME_NONEXISTENT = 1u << 15,   /* ONCE in a DST gap (Kotlin TIME_NONEXISTENT) */
    DSP_SCHED_E_NAME = 1u << 16,               /* name: > 32 bytes, no NUL, malformed UTF-8 or
                                                  a control character (M1.10b.1) */
    DSP_SCHED_E_NOT_BEFORE = 1u << 17,         /* edit floor neither 0 nor a valid instant */
};

/* True for 1..DSP_SCHEDULE_ID_MAX_LEN characters of [A-Za-z0-9._-] followed by NUL
 * within the first DSP_SCHEDULE_ID_MAX_LEN + 1 bytes (never reads further). */
bool dsp_schedule_id_is_valid(const char *id);

/* Checks every field; reports all problems at once (like Kotlin ScheduleValidator). */
dsp_sched_errors_t dsp_schedule_validate(const dsp_schedule_t *schedule, const dsp_tz_t *tz);

typedef enum {
    DSP_REC_OK = 0,
    DSP_REC_NONE,             /* no (further) occurrence; not an error */
    DSP_REC_ERR_ARG,          /* NULL pointer / missing output */
    DSP_REC_ERR_SCHEDULE,     /* a schedule or the timezone fails validation */
    DSP_REC_ERR_RANGE,        /* until is before after */
    DSP_REC_ERR_BUFFER_FULL,  /* more results than capacity; see dsp_rec_between */
    DSP_REC_ERR_INTERNAL,     /* iteration bound exceeded; indicates a bug */
} dsp_rec_status_t;

/* A planned occurrence. Its identity is (schedule id, local_date). */
typedef struct {
    dsp_utc_t at;          /* scheduled instant */
    dsp_date_t local_date; /* local date in the schedule timezone */
} dsp_occurrence_t;

/*
 * Occurrence of `schedule` on local date `date`.
 * DSP_REC_OK: *out filled. DSP_REC_NONE: rule does not match, date out of range,
 * local time nonexistent that day, or schedule disabled.
 */
dsp_rec_status_t dsp_rec_occurrence_on(const dsp_schedule_t *schedule, const dsp_tz_t *tz,
                                       dsp_date_t date, dsp_occurrence_t *out);

/*
 * Earliest occurrence strictly after `after`.
 * DSP_REC_OK: *out filled. DSP_REC_NONE: none (ONCE passed, end date reached,
 * disabled, or beyond the supported range). *out is untouched unless OK.
 */
dsp_rec_status_t dsp_rec_next(const dsp_schedule_t *schedule, const dsp_tz_t *tz, dsp_utc_t after,
                              dsp_occurrence_t *out);

/*
 * All occurrences with after < at <= until, in time order, into buf[0..capacity).
 * *count is always set to the number of entries written.
 *
 * DSP_REC_OK: all occurrences in the interval are in buf (possibly zero).
 * DSP_REC_ERR_BUFFER_FULL: there are more than `capacity`; buf holds the EARLIEST
 *   `capacity` ones (never a silent truncation). To continue, call again with
 *   after = buf[capacity - 1].at. capacity 0 is allowed (buf may then be NULL).
 * DSP_REC_ERR_RANGE: until < after (*count = 0).
 */
dsp_rec_status_t dsp_rec_between(const dsp_schedule_t *schedule, const dsp_tz_t *tz,
                                 dsp_utc_t after, dsp_utc_t until, dsp_occurrence_t *buf,
                                 size_t capacity, size_t *count);

/*
 * Earliest occurrence strictly after `after` across `n` schedules (disabled ones
 * ignored). Ties: smaller schedule id (byte order), then smaller array index.
 * DSP_REC_OK: *index and *out filled. DSP_REC_NONE: none.
 * DSP_REC_ERR_SCHEDULE: schedules[*index] is invalid (first invalid one); every
 * schedule is validated before any result is returned.
 */
dsp_rec_status_t dsp_rec_next_of_set(const dsp_schedule_t *schedules, size_t n,
                                     const dsp_tz_t *tz, dsp_utc_t after, size_t *index,
                                     dsp_occurrence_t *out);

/*
 * Formats the occurrence identity "<schedule_id>@YYYY-MM-DD" (approved, FR-014):
 * independent of revision, time of day and volume. Relies on the invariant that
 * schedule IDs are unique within the device and never reused after deletion.
 * DSP_REC_OK on success. DSP_REC_ERR_ARG: invalid id or date, or NULL buffer.
 * DSP_REC_ERR_BUFFER_FULL: buf_len < needed. On any error buf is "" if buf_len > 0.
 */
dsp_rec_status_t dsp_occurrence_id_format(const char *schedule_id, dsp_date_t local_date,
                                          char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* DSP_RECURRENCE_H */
