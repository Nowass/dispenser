#include "dsp_time.h"

#include <stddef.h>

const dsp_tz_t DSP_TZ_EUROPE_PRAGUE = {
    .std_offset_s = 3600,
    .dst_offset_s = 7200,
    .rule = DSP_TZ_DST_EU,
};

#define MAX_ABS_OFFSET_S (14 * 3600)
#define EU_TRANSITION_UTC_S 3600 /* 01:00 UTC */

static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        q--;
    }
    return q;
}

static int64_t floor_mod(int64_t a, int64_t b)
{
    return a - floor_div(a, b) * b;
}

static bool is_leap(int32_t y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int days_in_month(int32_t y, int m)
{
    static const uint8_t dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return (m == 2 && is_leap(y)) ? 29 : dim[m - 1];
}

bool dsp_date_is_valid(dsp_date_t d)
{
    if (d.year < DSP_DATE_MIN_YEAR || d.year > DSP_DATE_MAX_YEAR) {
        return false;
    }
    if (d.month < 1 || d.month > 12) {
        return false;
    }
    return d.day >= 1 && d.day <= days_in_month(d.year, d.month);
}

/* Howard Hinnant's days_from_civil (public domain algorithm). For |year| <= 32768,
 * month <= 255 and day <= 255 every intermediate stays far below 2^31: no overflow. */
int32_t dsp_date_to_days(dsp_date_t d)
{
    int32_t y = (int32_t)d.year - (d.month <= 2 ? 1 : 0);
    int32_t m = d.month;
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    int32_t yoe = y - era * 400;                                        /* [0, 399] */
    int32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + (int32_t)d.day - 1;
    int32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* Howard Hinnant's civil_from_days. Unchecked: callers guarantee
 * DSP_DAYS_MIN <= days <= DSP_DAYS_MAX, so the year fits int16_t. */
static dsp_date_t civil_from_days(int32_t days)
{
    int32_t z = days + 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    int32_t doe = z - era * 146097;                                     /* [0, 146096] */
    int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; /* [0, 399] */
    int32_t y = yoe + era * 400;
    int32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100); /* [0, 365] */
    int32_t mp = (5 * doy + 2) / 153;                      /* [0, 11] */
    int32_t d = doy - (153 * mp + 2) / 5 + 1;              /* [1, 31] */
    int32_t m = mp < 10 ? mp + 3 : mp - 9;                 /* [1, 12] */
    dsp_date_t out = {
        .year = (int16_t)(y + (m <= 2 ? 1 : 0)),
        .month = (uint8_t)m,
        .day = (uint8_t)d,
    };
    return out;
}

bool dsp_date_from_days(int32_t days, dsp_date_t *out)
{
    if (out == NULL || days < DSP_DAYS_MIN || days > DSP_DAYS_MAX) {
        return false;
    }
    *out = civil_from_days(days);
    return true;
}

int dsp_weekday_from_days(int32_t days)
{
    /* 1970-01-01 was a Thursday (= 3 with Monday = 0). */
    return (int)floor_mod((int64_t)days + 3, 7);
}

int dsp_date_compare(dsp_date_t a, dsp_date_t b)
{
    if (a.year != b.year) {
        return a.year < b.year ? -1 : 1;
    }
    if (a.month != b.month) {
        return a.month < b.month ? -1 : 1;
    }
    if (a.day != b.day) {
        return a.day < b.day ? -1 : 1;
    }
    return 0;
}

/* ---- Timezone ------------------------------------------------------------------ */

bool dsp_tz_is_valid(const dsp_tz_t *tz)
{
    if (tz == NULL) {
        return false;
    }
    if (tz->std_offset_s < -MAX_ABS_OFFSET_S || tz->std_offset_s > MAX_ABS_OFFSET_S ||
        tz->dst_offset_s < -MAX_ABS_OFFSET_S || tz->dst_offset_s > MAX_ABS_OFFSET_S) {
        return false;
    }
    switch (tz->rule) {
    case DSP_TZ_DST_NONE:
        return true;
    case DSP_TZ_DST_EU:
        return tz->dst_offset_s > tz->std_offset_s;
    default:
        return false;
    }
}

static int32_t last_sunday_days(int32_t year, int month)
{
    dsp_date_t last = {.year = (int16_t)year, .month = (uint8_t)month,
                       .day = (uint8_t)days_in_month(year, month)};
    int32_t dn = dsp_date_to_days(last);
    return dn - (dsp_weekday_from_days(dn) + 1) % 7;
}

/* year within 1999..2100 (callers guarantee). */
static void eu_transitions(int32_t year, dsp_utc_t *start, dsp_utc_t *end)
{
    *start = (dsp_utc_t)last_sunday_days(year, 3) * DSP_SECONDS_PER_DAY + EU_TRANSITION_UTC_S;
    *end = (dsp_utc_t)last_sunday_days(year, 10) * DSP_SECONDS_PER_DAY + EU_TRANSITION_UTC_S;
}

bool dsp_tz_transitions(const dsp_tz_t *tz, int year, dsp_utc_t *dst_start, dsp_utc_t *dst_end)
{
    if (!dsp_tz_is_valid(tz) || tz->rule != DSP_TZ_DST_EU || dst_start == NULL ||
        dst_end == NULL || year < DSP_DATE_MIN_YEAR - 1 || year > DSP_DATE_MAX_YEAR + 1) {
        return false;
    }
    eu_transitions(year, dst_start, dst_end);
    return true;
}

static bool utc_in_range(dsp_utc_t t)
{
    return t >= DSP_UTC_MIN && t <= DSP_UTC_MAX;
}

/* Unchecked: tz valid and DSP_UTC_MIN <= t <= DSP_UTC_MAX, so the UTC day number is
 * within DSP_DAYS_MIN..DSP_DAYS_MAX and the UTC year within 1999..2100. */
static int32_t offset_at(const dsp_tz_t *tz, dsp_utc_t t)
{
    if (tz->rule != DSP_TZ_DST_EU) {
        return tz->std_offset_s;
    }
    /* The EU transitions are months away from New Year, so the UTC year is the
     * right year to evaluate. */
    int32_t year = civil_from_days((int32_t)floor_div(t, DSP_SECONDS_PER_DAY)).year;
    dsp_utc_t start;
    dsp_utc_t end;
    eu_transitions(year, &start, &end);
    return (t >= start && t < end) ? tz->dst_offset_s : tz->std_offset_s;
}

bool dsp_tz_offset_at(const dsp_tz_t *tz, dsp_utc_t t, int32_t *offset_s)
{
    if (!dsp_tz_is_valid(tz) || offset_s == NULL || !utc_in_range(t)) {
        return false;
    }
    *offset_s = offset_at(tz, t);
    return true;
}

dsp_local_kind_t dsp_local_to_utc(const dsp_tz_t *tz, dsp_date_t date, int hour, int minute,
                                  dsp_utc_t *out)
{
    if (!dsp_tz_is_valid(tz) || !dsp_date_is_valid(date) || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || out == NULL) {
        return DSP_LOCAL_INVALID;
    }
    /* A valid date (2000..2099) with any offset within +/-14 h yields candidates inside
     * DSP_UTC_MIN..DSP_UTC_MAX, so offset_at() is always called in range. */
    dsp_utc_t local_s = (dsp_utc_t)dsp_date_to_days(date) * DSP_SECONDS_PER_DAY +
                        (dsp_utc_t)hour * 3600 + (dsp_utc_t)minute * 60;
    dsp_utc_t t_std = local_s - tz->std_offset_s;
    if (tz->rule == DSP_TZ_DST_NONE) {
        *out = t_std;
        return DSP_LOCAL_UNIQUE;
    }
    dsp_utc_t t_dst = local_s - tz->dst_offset_s;
    bool std_ok = offset_at(tz, t_std) == tz->std_offset_s;
    bool dst_ok = offset_at(tz, t_dst) == tz->dst_offset_s;
    if (std_ok && dst_ok) {
        *out = t_dst < t_std ? t_dst : t_std; /* earlier instant */
        return DSP_LOCAL_OVERLAP;
    }
    if (std_ok) {
        *out = t_std;
        return DSP_LOCAL_UNIQUE;
    }
    if (dst_ok) {
        *out = t_dst;
        return DSP_LOCAL_UNIQUE;
    }
    return DSP_LOCAL_GAP;
}

bool dsp_utc_to_local_date(const dsp_tz_t *tz, dsp_utc_t t, dsp_date_t *out)
{
    if (!dsp_tz_is_valid(tz) || out == NULL || !utc_in_range(t)) {
        return false;
    }
    /* In range: local day number stays within DSP_DAYS_MIN..DSP_DAYS_MAX. */
    dsp_utc_t local_s = t + offset_at(tz, t);
    *out = civil_from_days((int32_t)floor_div(local_s, DSP_SECONDS_PER_DAY));
    return true;
}
