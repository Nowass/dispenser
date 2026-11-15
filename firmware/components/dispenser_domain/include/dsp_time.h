#ifndef DSP_TIME_H
#define DSP_TIME_H

/*
 * Calendar dates, UTC instants and the schedule timezone (pure domain code).
 *
 * - Instants are dsp_utc_t: signed 64-bit seconds since 1970-01-01T00:00:00Z
 *   (POSIX time, leap seconds not counted). Never a local time.
 * - Dates are proleptic Gregorian civil dates. All arithmetic is integer-only and
 *   independent of the C library time functions, so results do not depend on the
 *   host's TZ setting, newlib vs glibc, or the system clock.
 * - Timezones are explicit dsp_tz_t rule values (see below), never the process TZ.
 *
 * Input ranges: every function either accepts its whole input domain or returns
 * false / DSP_LOCAL_INVALID for out-of-range input and leaves its outputs untouched.
 * No input can cause undefined or implementation-defined behavior.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int64_t dsp_utc_t;

/* Supported range for SCHEDULE dates (inclusive). The EU DST rule used for
 * Europe/Prague matches the IANA tz database for all of these years (it does not
 * before 1996). */
#define DSP_DATE_MIN_YEAR 2000
#define DSP_DATE_MAX_YEAR 2099

/* Supported range of INSTANTS for timezone conversion (inclusive): one year of margin
 * around the schedule dates, 1999-01-01T00:00:00Z .. 2100-12-31T23:59:59Z. */
#define DSP_UTC_MIN ((dsp_utc_t)915148800)
#define DSP_UTC_MAX ((dsp_utc_t)4133980799)

/* Supported day numbers for dsp_date_from_days (inclusive): 1900-01-01 .. 2199-12-31.
 * Wide enough for the local date of any supported instant at any offset (+/-14 h). */
#define DSP_DAYS_MIN ((int32_t)-25567)
#define DSP_DAYS_MAX ((int32_t)84005)

#define DSP_SECONDS_PER_DAY 86400

typedef struct {
    int16_t year;  /* DSP_DATE_MIN_YEAR..DSP_DATE_MAX_YEAR for schedule dates */
    uint8_t month; /* 1..12 */
    uint8_t day;   /* 1..days in month */
} dsp_date_t;

/* Weekday numbering: 0 = Monday ... 6 = Sunday (matches the weekday mask bits). */
enum {
    DSP_MONDAY = 0,
    DSP_TUESDAY,
    DSP_WEDNESDAY,
    DSP_THURSDAY,
    DSP_FRIDAY,
    DSP_SATURDAY,
    DSP_SUNDAY,
};

/* True for a real calendar date in DSP_DATE_MIN_YEAR..DSP_DATE_MAX_YEAR. */
bool dsp_date_is_valid(dsp_date_t d);

/* Days since 1970-01-01 (negative before). Defined (no overflow) for every input
 * value; the result is meaningful only for real calendar dates. */
int32_t dsp_date_to_days(dsp_date_t d);

/* Date of a day number. Returns false (and leaves *out untouched) if out is NULL or
 * days is outside DSP_DAYS_MIN..DSP_DAYS_MAX. */
bool dsp_date_from_days(int32_t days, dsp_date_t *out);

/* Weekday of any day number (0 = Monday). */
int dsp_weekday_from_days(int32_t days);

/* -1, 0, 1 like strcmp. */
int dsp_date_compare(dsp_date_t a, dsp_date_t b);

/* ---- Timezone rules ----------------------------------------------------------- */

typedef enum {
    DSP_TZ_DST_NONE = 0, /* fixed offset all year */
    /* EU rule (Directive 2000/84/EC): DST from the last Sunday of March 01:00 UTC to
     * the last Sunday of October 01:00 UTC. Same instant for every EU zone. */
    DSP_TZ_DST_EU = 1,
} dsp_tz_dst_rule_t;

typedef struct {
    int32_t std_offset_s;   /* standard-time offset east of UTC, e.g. +3600 for CET */
    int32_t dst_offset_s;   /* daylight-time offset east of UTC, e.g. +7200 for CEST */
    dsp_tz_dst_rule_t rule; /* when dst_offset_s applies */
} dsp_tz_t;

/* Europe/Prague: CET (UTC+1) / CEST (UTC+2), EU rule. The default schedule zone
 * (FR-031). A change of DST legislation is handled by a firmware update. */
extern const dsp_tz_t DSP_TZ_EUROPE_PRAGUE;

/* True if tz is non-NULL, offsets are within +/-14 h, the rule is known and, for DST
 * rules, dst_offset_s > std_offset_s. */
bool dsp_tz_is_valid(const dsp_tz_t *tz);

/* UTC offset (seconds east) in force at instant t. Returns false (and leaves *offset_s
 * untouched) if tz is invalid, offset_s is NULL or t is outside DSP_UTC_MIN..DSP_UTC_MAX. */
bool dsp_tz_offset_at(const dsp_tz_t *tz, dsp_utc_t t, int32_t *offset_s);

/* UTC instants of DST start and end in `year` (EU rule, years 1999..2100 only;
 * returns false otherwise, outputs untouched). */
bool dsp_tz_transitions(const dsp_tz_t *tz, int year, dsp_utc_t *dst_start, dsp_utc_t *dst_end);

typedef enum {
    DSP_LOCAL_UNIQUE = 0,  /* local time exists exactly once */
    DSP_LOCAL_OVERLAP = 1, /* occurs twice (fall-back); *out = the EARLIER instant */
    DSP_LOCAL_GAP = 2,     /* does not exist (spring-forward); *out untouched */
    DSP_LOCAL_INVALID = 3, /* invalid tz, date (see dsp_date_is_valid), hour/minute or
                              NULL out; *out untouched */
} dsp_local_kind_t;

/* Converts a local wall-clock date/time in tz to UTC. hour 0..23, minute 0..59. */
dsp_local_kind_t dsp_local_to_utc(const dsp_tz_t *tz, dsp_date_t date, int hour, int minute,
                                  dsp_utc_t *out);

/* Local calendar date (in tz) of instant t. Returns false (and leaves *out untouched)
 * if tz is invalid, out is NULL or t is outside DSP_UTC_MIN..DSP_UTC_MAX. */
bool dsp_utc_to_local_date(const dsp_tz_t *tz, dsp_utc_t t, dsp_date_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DSP_TIME_H */
