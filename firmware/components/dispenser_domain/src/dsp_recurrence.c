#include "dsp_recurrence.h"

#include "dsp_text.h"

#include <stdio.h>
#include <string.h>

/*
 * Candidate dates examined by one next-occurrence search. The search starts on the
 * local date before `after`, so at most two candidates can lie at or before it, and a
 * DST-gap day is never followed by another gap day; 4 suffices, 8 leaves margin.
 * Exceeding it returns DSP_REC_ERR_INTERNAL instead of looping.
 */
#define MAX_CANDIDATES_PER_SEARCH 8

static const dsp_date_t LAST_SUPPORTED_DATE = {DSP_DATE_MAX_YEAR, 12, 31};

/*
 * Query instants are clamped into this window before any conversion. Both bounds are a
 * full day away from the supported schedule dates (so clamping never changes a
 * result) and lie inside DSP_UTC_MIN..DSP_UTC_MAX (so every conversion is in range).
 */
#define QUERY_MIN ((dsp_utc_t)946512000)  /* 1999-12-30T00:00:00Z */
#define QUERY_MAX ((dsp_utc_t)4102531200) /* 2100-01-02T00:00:00Z */
_Static_assert(QUERY_MIN >= DSP_UTC_MIN && QUERY_MAX <= DSP_UTC_MAX,
               "query window must lie inside the supported UTC range");

/* ---- Validation ---------------------------------------------------------------- */

static bool id_char_ok(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '-';
}

/* Valid id within the first DSP_SCHEDULE_ID_MAX_LEN + 1 bytes (never reads further). */
static bool id_valid(const char *id)
{
    if (id == NULL) {
        return false;
    }
    size_t i = 0;
    for (; i <= DSP_SCHEDULE_ID_MAX_LEN && id[i] != '\0'; i++) {
        if (!id_char_ok(id[i])) {
            return false;
        }
    }
    return i >= 1 && i <= DSP_SCHEDULE_ID_MAX_LEN;
}

bool dsp_schedule_id_is_valid(const char *id)
{
    return id_valid(id);
}

/* NUL within the buffer, then the protocol's `text` rule over the bytes before it. */
static bool name_valid(const char *name)
{
    size_t n = 0;
    while (n <= DSP_SCHEDULE_NAME_MAX_BYTES && name[n] != '\0') {
        n++;
    }
    return n <= DSP_SCHEDULE_NAME_MAX_BYTES && dsp_utf8_valid((const uint8_t *)name, n, true);
}

dsp_sched_errors_t dsp_schedule_validate(const dsp_schedule_t *s, const dsp_tz_t *tz)
{
    if (s == NULL) {
        return DSP_SCHED_E_ARG;
    }
    dsp_sched_errors_t e = DSP_SCHED_OK;
    bool tz_ok = dsp_tz_is_valid(tz);
    if (!tz_ok) {
        e |= DSP_SCHED_E_TZ;
    }
    if (!id_valid(s->id)) {
        e |= DSP_SCHED_E_ID;
    }
    if (s->revision == 0) {
        e |= DSP_SCHED_E_REVISION;
    }
    if (s->amount_ul == 0) {
        e |= DSP_SCHED_E_AMOUNT_NOT_POSITIVE;
    }
    bool start_ok = dsp_date_is_valid(s->start_date);
    if (!start_ok) {
        e |= DSP_SCHED_E_START_DATE;
    }
    if (!name_valid(s->name)) {
        e |= DSP_SCHED_E_NAME;
    }
    if (s->not_before != 0 && (s->not_before < DSP_UTC_MIN || s->not_before > DSP_UTC_MAX)) {
        e |= DSP_SCHED_E_NOT_BEFORE;
    }
    bool time_ok = s->hour <= 23 && s->minute <= 59;
    if (!time_ok) {
        e |= DSP_SCHED_E_TIME;
    }

    switch (s->kind) {
    case DSP_REC_ONCE:
        if (s->interval != 1 || s->has_end_date) {
            e |= DSP_SCHED_E_ONCE_FORM;
        }
        if (s->weekday_mask != 0) {
            e |= DSP_SCHED_E_WEEKDAYS;
        }
        if (tz_ok && start_ok && time_ok) {
            dsp_utc_t unused;
            if (dsp_local_to_utc(tz, s->start_date, s->hour, s->minute, &unused) ==
                DSP_LOCAL_GAP) {
                e |= DSP_SCHED_E_TIME_NONEXISTENT;
            }
        }
        break;
    case DSP_REC_DAILY:
    case DSP_REC_WEEKLY:
        if (s->interval == 0) {
            e |= DSP_SCHED_E_INTERVAL_TOO_SMALL;
        } else if (s->interval > DSP_REC_INTERVAL_MAX) {
            e |= DSP_SCHED_E_INTERVAL_TOO_LARGE;
        }
        if (s->kind == DSP_REC_DAILY) {
            if (s->weekday_mask != 0) {
                e |= DSP_SCHED_E_WEEKDAYS;
            }
        } else {
            if ((s->weekday_mask & DSP_WEEKDAY_MASK_ALL) == 0) {
                e |= DSP_SCHED_E_NO_WEEKDAYS;
            }
            if ((s->weekday_mask & ~DSP_WEEKDAY_MASK_ALL) != 0) {
                e |= DSP_SCHED_E_WEEKDAYS;
            }
        }
        if (s->has_end_date) {
            if (!dsp_date_is_valid(s->end_date)) {
                e |= DSP_SCHED_E_END_DATE;
            } else if (start_ok && dsp_date_compare(s->end_date, s->start_date) < 0) {
                e |= DSP_SCHED_E_END_BEFORE_START;
            }
        }
        break;
    default:
        e |= DSP_SCHED_E_KIND;
        break;
    }
    return e;
}

/* ---- Rule matching (day numbers, int64 so that huge intervals cannot overflow) ---- */

static int64_t last_day(const dsp_schedule_t *s)
{
    return dsp_date_to_days(s->has_end_date ? s->end_date : LAST_SUPPORTED_DATE);
}

/* Weekday (0 = Monday) of any int64 day number; candidates far beyond the supported
 * range can occur transiently with huge intervals before the range check. */
static int weekday64(int64_t day)
{
    int64_t r = (day + 3) % 7; /* 1970-01-01 was a Thursday */
    return (int)(r < 0 ? r + 7 : r);
}

static int64_t monday_of(int64_t day)
{
    return day - weekday64(day);
}

static bool weekday_selected(const dsp_schedule_t *s, int64_t day)
{
    return (s->weekday_mask & DSP_WEEKDAY_BIT(weekday64(day))) != 0;
}

/* Whether the rule matches `day` (ignores DST and enabled; requires a valid schedule). */
static bool occurs_on(const dsp_schedule_t *s, int64_t day)
{
    int64_t start = dsp_date_to_days(s->start_date);
    if (day < start || day > last_day(s)) {
        return false;
    }
    switch (s->kind) {
    case DSP_REC_ONCE:
        return day == start;
    case DSP_REC_DAILY:
        return (day - start) % (int64_t)s->interval == 0;
    default: /* DSP_REC_WEEKLY */
        return weekday_selected(s, day) &&
               ((day - monday_of(start)) / 7) % (int64_t)s->interval == 0;
    }
}

/* First selected weekday in the week starting `monday` that is >= not_before. */
static bool first_selected_in_week(const dsp_schedule_t *s, int64_t monday, int64_t not_before,
                                   int64_t *out)
{
    for (int64_t d = monday; d < monday + 7; d++) {
        if (d >= not_before && weekday_selected(s, d)) {
            *out = d;
            return true;
        }
    }
    return false;
}

/*
 * First day >= from (and >= start date) matching the rule, ignoring the end date and
 * DST; O(1). Returns false only for a ONCE schedule whose date has passed.
 */
static bool first_match_on_or_after(const dsp_schedule_t *s, int64_t from, int64_t *out)
{
    int64_t start = dsp_date_to_days(s->start_date);
    int64_t d0 = from > start ? from : start;
    int64_t interval = (int64_t)s->interval;
    switch (s->kind) {
    case DSP_REC_ONCE:
        if (d0 != start) {
            return false;
        }
        *out = start;
        return true;
    case DSP_REC_DAILY: {
        int64_t rem = (d0 - start) % interval; /* d0 >= start: non-negative */
        *out = rem == 0 ? d0 : d0 + (interval - rem);
        return true;
    }
    default: { /* DSP_REC_WEEKLY; weekday mask is non-empty (validated) */
        int64_t start_monday = monday_of(start);
        int64_t monday = monday_of(d0);
        int64_t off = ((monday - start_monday) / 7) % interval;
        if (off != 0) {
            monday += 7 * (interval - off);
        }
        if (first_selected_in_week(s, monday, d0, out)) {
            return true;
        }
        return first_selected_in_week(s, monday + 7 * interval, d0, out);
    }
    }
}

/*
 * Occurrence on a rule-matching day (start date <= day <= last supported day).
 * DSP_REC_OK, DSP_REC_NONE if the local time does not exist that day (spring-forward
 * gap: skipped, never shifted), or DSP_REC_ERR_INTERNAL if a conversion unexpectedly
 * fails; a failure is never reported as "no occurrence".
 */
static dsp_rec_status_t occurrence_at_day(const dsp_schedule_t *s, const dsp_tz_t *tz,
                                          int64_t day, dsp_occurrence_t *out)
{
    dsp_date_t date;
    if (day < DSP_DAYS_MIN || day > DSP_DAYS_MAX || !dsp_date_from_days((int32_t)day, &date)) {
        return DSP_REC_ERR_INTERNAL;
    }
    dsp_utc_t at;
    switch (dsp_local_to_utc(tz, date, s->hour, s->minute, &at)) {
    case DSP_LOCAL_UNIQUE:
    case DSP_LOCAL_OVERLAP: /* earlier instant already chosen */
        out->at = at;
        out->local_date = date;
        return DSP_REC_OK;
    case DSP_LOCAL_GAP:
        return DSP_REC_NONE;
    default:
        return DSP_REC_ERR_INTERNAL;
    }
}

/* Next occurrence strictly after `after`; schedule valid and enabled. */
static dsp_rec_status_t next_unchecked(const dsp_schedule_t *s, const dsp_tz_t *tz,
                                       dsp_utc_t after, dsp_occurrence_t *out)
{
    if (after >= QUERY_MAX) {
        return DSP_REC_NONE; /* after the last supported date */
    }
    if (after < QUERY_MIN) {
        after = QUERY_MIN; /* every occurrence is later anyway */
    }
    int64_t limit = last_day(s);
    dsp_date_t after_local;
    if (!dsp_utc_to_local_date(tz, after, &after_local)) {
        return DSP_REC_ERR_INTERNAL; /* unreachable: after is within the query window */
    }
    /* Start one day early so a local date differing from the UTC date is never missed. */
    int64_t from = (int64_t)dsp_date_to_days(after_local) - 1;
    for (int i = 0; i < MAX_CANDIDATES_PER_SEARCH; i++) {
        int64_t day;
        if (!first_match_on_or_after(s, from, &day) || day > limit) {
            return DSP_REC_NONE;
        }
        dsp_occurrence_t occ;
        dsp_rec_status_t st = occurrence_at_day(s, tz, day, &occ);
        if (st == DSP_REC_OK && occ.at > after) {
            *out = occ;
            return DSP_REC_OK;
        }
        if (st != DSP_REC_OK && st != DSP_REC_NONE) {
            return st;
        }
        from = day + 1;
    }
    return DSP_REC_ERR_INTERNAL;
}

/* ---- Public queries ------------------------------------------------------------ */

dsp_rec_status_t dsp_rec_occurrence_on(const dsp_schedule_t *s, const dsp_tz_t *tz,
                                       dsp_date_t date, dsp_occurrence_t *out)
{
    if (s == NULL || out == NULL) {
        return DSP_REC_ERR_ARG;
    }
    if (dsp_schedule_validate(s, tz) != DSP_SCHED_OK) {
        return DSP_REC_ERR_SCHEDULE;
    }
    if (!s->enabled || !dsp_date_is_valid(date)) {
        return DSP_REC_NONE;
    }
    int64_t day = dsp_date_to_days(date);
    if (!occurs_on(s, day)) {
        return DSP_REC_NONE;
    }
    return occurrence_at_day(s, tz, day, out);
}

dsp_rec_status_t dsp_rec_next(const dsp_schedule_t *s, const dsp_tz_t *tz, dsp_utc_t after,
                              dsp_occurrence_t *out)
{
    if (s == NULL || out == NULL) {
        return DSP_REC_ERR_ARG;
    }
    if (dsp_schedule_validate(s, tz) != DSP_SCHED_OK) {
        return DSP_REC_ERR_SCHEDULE;
    }
    if (!s->enabled) {
        return DSP_REC_NONE;
    }
    return next_unchecked(s, tz, after, out);
}

dsp_rec_status_t dsp_rec_between(const dsp_schedule_t *s, const dsp_tz_t *tz, dsp_utc_t after,
                                 dsp_utc_t until, dsp_occurrence_t *buf, size_t capacity,
                                 size_t *count)
{
    if (count == NULL) {
        return DSP_REC_ERR_ARG;
    }
    *count = 0;
    if (s == NULL || (buf == NULL && capacity > 0)) {
        return DSP_REC_ERR_ARG;
    }
    if (dsp_schedule_validate(s, tz) != DSP_SCHED_OK) {
        return DSP_REC_ERR_SCHEDULE;
    }
    if (until < after) {
        return DSP_REC_ERR_RANGE;
    }
    if (!s->enabled) {
        return DSP_REC_OK;
    }
    /* Bounded: at most capacity + 1 searches; each search is bounded (see above). */
    dsp_utc_t cursor = after;
    for (;;) {
        dsp_occurrence_t occ;
        dsp_rec_status_t st = next_unchecked(s, tz, cursor, &occ);
        if (st == DSP_REC_NONE || (st == DSP_REC_OK && occ.at > until)) {
            return DSP_REC_OK;
        }
        if (st != DSP_REC_OK) {
            return st;
        }
        if (*count == capacity) {
            return DSP_REC_ERR_BUFFER_FULL;
        }
        buf[(*count)++] = occ;
        cursor = occ.at; /* strictly increasing: one occurrence per local date */
    }
}

dsp_rec_status_t dsp_rec_next_of_set(const dsp_schedule_t *schedules, size_t n,
                                     const dsp_tz_t *tz, dsp_utc_t after, size_t *index,
                                     dsp_occurrence_t *out)
{
    if ((schedules == NULL && n > 0) || index == NULL || out == NULL) {
        return DSP_REC_ERR_ARG;
    }
    for (size_t i = 0; i < n; i++) {
        if (dsp_schedule_validate(&schedules[i], tz) != DSP_SCHED_OK) {
            *index = i;
            return DSP_REC_ERR_SCHEDULE;
        }
    }
    bool found = false;
    size_t best_index = 0;
    dsp_occurrence_t best = {0};
    for (size_t i = 0; i < n; i++) {
        if (!schedules[i].enabled) {
            continue;
        }
        dsp_occurrence_t occ;
        dsp_rec_status_t st = next_unchecked(&schedules[i], tz, after, &occ);
        if (st == DSP_REC_NONE) {
            continue;
        }
        if (st != DSP_REC_OK) {
            *index = i;
            return st;
        }
        if (!found || occ.at < best.at ||
            (occ.at == best.at && strcmp(schedules[i].id, schedules[best_index].id) < 0)) {
            found = true;
            best = occ;
            best_index = i;
        }
    }
    if (!found) {
        return DSP_REC_NONE;
    }
    *index = best_index;
    *out = best;
    return DSP_REC_OK;
}

dsp_rec_status_t dsp_occurrence_id_format(const char *schedule_id, dsp_date_t local_date,
                                          char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0) {
        return DSP_REC_ERR_ARG;
    }
    buf[0] = '\0';
    if (!id_valid(schedule_id) || !dsp_date_is_valid(local_date)) {
        return DSP_REC_ERR_ARG;
    }
    size_t needed = strlen(schedule_id) + 1 + 10 + 1;
    if (buf_len < needed) {
        return DSP_REC_ERR_BUFFER_FULL;
    }
    int n = snprintf(buf, buf_len, "%s@%04d-%02u-%02u", schedule_id, (int)local_date.year,
                     (unsigned)local_date.month, (unsigned)local_date.day);
    if (n < 0 || (size_t)n + 1 != needed) {
        buf[0] = '\0';
        return DSP_REC_ERR_ARG;
    }
    return DSP_REC_OK;
}
