/*
 * Unit tests of the recurrence engine and date/time helpers that complement the
 * shared vectors: exhaustive date arithmetic, jump search vs brute force, buffer
 * handling, argument errors and extreme inputs.
 */
#include "dsp_recurrence.h"
#include "dsp_test.h"

#include <stdint.h>
#include <string.h>

static const dsp_tz_t *const TZ = &DSP_TZ_EUROPE_PRAGUE;

static dsp_date_t D(int y, int m, int d)
{
    dsp_date_t date = {(int16_t)y, (uint8_t)m, (uint8_t)d};
    return date;
}

/* Checked wrappers: an unexpected `false` from the API is a test failure. */
static dsp_date_t days_to_date(int32_t days)
{
    dsp_date_t out = {0, 0, 0};
    DSP_CHECK(dsp_date_from_days(days, &out));
    return out;
}

static dsp_date_t local_date_of(dsp_utc_t t)
{
    dsp_date_t out = {0, 0, 0};
    DSP_CHECK(dsp_utc_to_local_date(TZ, t, &out));
    return out;
}

static dsp_schedule_t daily(const char *id, dsp_date_t start, int h, int m)
{
    dsp_schedule_t s;
    memset(&s, 0, sizeof s);
    strcpy(s.id, id);
    s.revision = 1;
    s.enabled = true;
    s.amount_ul = 1000;
    s.start_date = start;
    s.hour = (uint8_t)h;
    s.minute = (uint8_t)m;
    s.kind = DSP_REC_DAILY;
    s.interval = 1;
    return s;
}

/* 2026-11-14T07:00:00Z etc. (CET = UTC+1 in mid-November). */
#define T_2026_11_14_0700Z ((dsp_utc_t)1794639600)
#define QUERY_MAX_2100_01_02 ((dsp_utc_t)4102531200) /* 2100-01-02T00:00:00Z */

/* ---- Date arithmetic -------------------------------------------------------------- */

static void test_dates_match_naive_calendar_walk(void)
{
    /* Walk every day 1999-01-01 .. 2101-12-31 with a naive calendar counter and
     * compare day numbers, round trip and weekday. 1999-01-01 was a Friday. */
    static const int dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int32_t expected_days = 10592; /* 1999-01-01 */
    int weekday = DSP_FRIDAY;
    int mismatches = 0;
    for (int y = 1999; y <= 2101; y++) {
        int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
        for (int m = 1; m <= 12; m++) {
            int n = dim[m - 1] + (m == 2 && leap ? 1 : 0);
            for (int d = 1; d <= n; d++) {
                dsp_date_t date = D(y, m, d);
                int32_t days = dsp_date_to_days(date);
                dsp_date_t back = {0, 0, 0};
                bool back_ok = dsp_date_from_days(days, &back);
                if (days != expected_days || !back_ok || dsp_date_compare(back, date) != 0 ||
                    dsp_weekday_from_days(days) != weekday ||
                    dsp_date_is_valid(date) != (y >= 2000 && y <= 2099)) {
                    mismatches++;
                }
                expected_days++;
                weekday = (weekday + 1) % 7;
            }
        }
    }
    DSP_CHECK_EQ_INT(0, mismatches);
    DSP_CHECK_EQ_INT(0, dsp_date_to_days(D(1970, 1, 1)));
    DSP_CHECK_EQ_INT(DSP_THURSDAY, dsp_weekday_from_days(0));
}

static void test_invalid_dates_rejected(void)
{
    DSP_CHECK(!dsp_date_is_valid(D(2026, 2, 29)));
    DSP_CHECK(dsp_date_is_valid(D(2028, 2, 29)));
    DSP_CHECK(!dsp_date_is_valid(D(2026, 4, 31)));
    DSP_CHECK(!dsp_date_is_valid(D(2026, 0, 1)));
    DSP_CHECK(!dsp_date_is_valid(D(2026, 13, 1)));
    DSP_CHECK(!dsp_date_is_valid(D(2026, 1, 0)));
    DSP_CHECK(!dsp_date_is_valid(D(1999, 12, 31)));
    DSP_CHECK(!dsp_date_is_valid(D(2100, 1, 1)));
    DSP_CHECK(dsp_date_is_valid(D(2000, 2, 29))); /* divisible by 400 */
}

static void test_timezone_validation(void)
{
    DSP_CHECK(dsp_tz_is_valid(&DSP_TZ_EUROPE_PRAGUE));
    DSP_CHECK(!dsp_tz_is_valid(NULL));
    dsp_tz_t tz = DSP_TZ_EUROPE_PRAGUE;
    tz.dst_offset_s = tz.std_offset_s; /* DST rule without a DST shift */
    DSP_CHECK(!dsp_tz_is_valid(&tz));
    tz = DSP_TZ_EUROPE_PRAGUE;
    tz.std_offset_s = 15 * 3600;
    DSP_CHECK(!dsp_tz_is_valid(&tz));
    tz = DSP_TZ_EUROPE_PRAGUE;
    tz.rule = (dsp_tz_dst_rule_t)7;
    DSP_CHECK(!dsp_tz_is_valid(&tz));

    /* Fixed-offset zone: every local time is unique. */
    dsp_tz_t utc = {0, 0, DSP_TZ_DST_NONE};
    DSP_CHECK(dsp_tz_is_valid(&utc));
    dsp_utc_t t = 0;
    DSP_CHECK_EQ_INT(DSP_LOCAL_UNIQUE, dsp_local_to_utc(&utc, D(2027, 3, 28), 2, 30, &t));
    dsp_utc_t s = 0, e = 0;
    DSP_CHECK(!dsp_tz_transitions(&utc, 2027, &s, &e));

    /* Invalid timezone makes every schedule invalid. */
    dsp_schedule_t sch = daily("s", D(2026, 11, 2), 8, 0);
    DSP_CHECK_EQ_INT(DSP_SCHED_E_TZ, dsp_schedule_validate(&sch, NULL));
    dsp_occurrence_t occ;
    DSP_CHECK_EQ_INT(DSP_REC_ERR_SCHEDULE, dsp_rec_next(&sch, NULL, 0, &occ));
}

static void test_local_date_of_instants_around_midnight_and_dst(void)
{
    /* 2026-11-14T22:59:59Z = 23:59:59 CET on the 14th; 23:00Z = 00:00 on the 15th. */
    DSP_CHECK_EQ_INT(0, dsp_date_compare(D(2026, 11, 14),
                                         local_date_of((dsp_utc_t)1794697199)));
    DSP_CHECK_EQ_INT(0, dsp_date_compare(D(2026, 11, 15),
                                         local_date_of((dsp_utc_t)1794697200)));
    /* Summer: 2027-06-01T22:00:00Z = 00:00 CEST on 2 June. */
    DSP_CHECK_EQ_INT(0, dsp_date_compare(D(2027, 6, 2),
                                         local_date_of((dsp_utc_t)1811887200)));
}

/* ---- Jump search equals the brute-force definition -------------------------------- */

static uint32_t rng_state = 20261115u;

static uint32_t rnd(uint32_t bound)
{
    rng_state = rng_state * 1664525u + 1013904223u; /* LCG, deterministic */
    return (rng_state >> 8) % bound;
}

static void test_jump_search_matches_brute_force(void)
{
    /* Same idea as Kotlin RecurrenceEdgeCaseTest.jumpSearch_matchesBruteForce:
     * random schedules around 2026-2028, including DST days, compared with a
     * day-by-day scan built only from dsp_rec_occurrence_on. */
    int mismatches = 0;
    int compared = 0;
    for (int iter = 0; iter < 600; iter++) {
        int32_t base = dsp_date_to_days(D(2026, 1, 1));
        dsp_schedule_t s = daily("r", days_to_date(base + (int32_t)rnd(900)),
                                 (int)rnd(24), (int)(rnd(4) * 15));
        switch (rnd(3)) {
        case 0:
            s.kind = DSP_REC_ONCE;
            break;
        case 1:
            s.kind = DSP_REC_DAILY;
            s.interval = 1 + rnd(40);
            break;
        default:
            s.kind = DSP_REC_WEEKLY;
            s.interval = 1 + rnd(5);
            s.weekday_mask = (uint8_t)rnd(128);
            if (s.weekday_mask == 0) {
                s.weekday_mask = DSP_WEEKDAY_BIT(DSP_FRIDAY);
            }
            break;
        }
        if (s.kind != DSP_REC_ONCE && rnd(2) == 1) {
            s.has_end_date = true;
            s.end_date = days_to_date(dsp_date_to_days(s.start_date) + (int32_t)rnd(400));
        }
        if (dsp_schedule_validate(&s, TZ) != DSP_SCHED_OK) {
            continue; /* ONCE in a DST gap */
        }
        int32_t after_day = base + (int32_t)rnd(1000);
        dsp_utc_t after;
        (void)dsp_local_to_utc(TZ, days_to_date(after_day), (int)rnd(24), 0, &after);

        /* Brute force: day by day from the day before `after`, 1500 days. */
        dsp_occurrence_t expected[5];
        int n_expected = 0;
        for (int32_t d = after_day - 1; d < after_day + 1500 && n_expected < 5; d++) {
            dsp_occurrence_t occ;
            if (dsp_rec_occurrence_on(&s, TZ, days_to_date(d), &occ) == DSP_REC_OK &&
                occ.at > after) {
                expected[n_expected++] = occ;
            }
        }
        dsp_utc_t cursor = after;
        for (int i = 0; i < 5; i++) {
            dsp_occurrence_t occ;
            dsp_rec_status_t st = dsp_rec_next(&s, TZ, cursor, &occ);
            if (i >= n_expected) {
                mismatches += st != DSP_REC_NONE;
                break;
            }
            if (st != DSP_REC_OK || occ.at != expected[i].at ||
                dsp_date_compare(occ.local_date, expected[i].local_date) != 0) {
                mismatches++;
                break;
            }
            cursor = occ.at;
            compared++;
        }
    }
    DSP_CHECK_EQ_INT(0, mismatches);
    DSP_CHECK(compared > 1000); /* the comparison really ran */
}

/* ---- Bounded output buffers ------------------------------------------------------- */

static void test_between_buffer_full_is_reported_and_resumable(void)
{
    dsp_schedule_t s = daily("s", D(2026, 11, 1), 8, 0);
    dsp_utc_t after = T_2026_11_14_0700Z - 1;     /* just before 14 Nov 08:00 */
    dsp_utc_t until = after + 30 * 86400;         /* 30 occurrences */
    dsp_occurrence_t all[40];
    size_t n_all = 0;
    DSP_CHECK_EQ_INT(DSP_REC_OK, dsp_rec_between(&s, TZ, after, until, all, 40, &n_all));
    DSP_CHECK_EQ_INT(30, n_all);

    /* Page through with a 7-entry buffer; pages concatenate to the full list. */
    dsp_occurrence_t page[7];
    size_t total = 0;
    dsp_utc_t cursor = after;
    int pages = 0;
    for (;;) {
        size_t n = 99;
        dsp_rec_status_t st = dsp_rec_between(&s, TZ, cursor, until, page, 7, &n);
        DSP_CHECK(st == DSP_REC_OK || st == DSP_REC_ERR_BUFFER_FULL);
        for (size_t i = 0; i < n && total < 40; i++, total++) {
            DSP_CHECK_EQ_INT(all[total].at, page[i].at);
        }
        pages++;
        if (st != DSP_REC_ERR_BUFFER_FULL || pages > 10) {
            break;
        }
        DSP_CHECK_EQ_INT(7, n);
        cursor = page[n - 1].at;
    }
    DSP_CHECK_EQ_INT(30, total);
    DSP_CHECK_EQ_INT(5, pages); /* 7 + 7 + 7 + 7 + 2 */

    /* Capacity 0 with occurrences present: reported, nothing written. */
    size_t n = 99;
    DSP_CHECK_EQ_INT(DSP_REC_ERR_BUFFER_FULL, dsp_rec_between(&s, TZ, after, until, NULL, 0, &n));
    DSP_CHECK_EQ_INT(0, n);
}

static void test_between_whole_supported_range_is_bounded(void)
{
    /* A daily schedule over 100 years: 36525 occurrences. A small buffer reports
     * BUFFER_FULL quickly instead of expanding the whole range. */
    dsp_schedule_t s = daily("s", D(2000, 1, 1), 8, 0);
    dsp_occurrence_t buf[4];
    size_t n = 0;
    DSP_CHECK_EQ_INT(DSP_REC_ERR_BUFFER_FULL,
                     dsp_rec_between(&s, TZ, INT64_MIN, INT64_MAX, buf, 4, &n));
    DSP_CHECK_EQ_INT(4, n);
    DSP_CHECK_EQ_INT(0, dsp_date_compare(D(2000, 1, 4), buf[3].local_date));
}

static void test_occurrence_id_buffer_handling(void)
{
    char buf[DSP_OCCURRENCE_ID_BUF_LEN];
    DSP_CHECK_EQ_INT(DSP_REC_OK, dsp_occurrence_id_format("abc", D(2026, 11, 20), buf, sizeof buf));
    DSP_CHECK_EQ_STR("abc@2026-11-20", buf);

    char exact[15]; /* strlen("abc@2026-11-20") + 1 */
    DSP_CHECK_EQ_INT(DSP_REC_OK, dsp_occurrence_id_format("abc", D(2026, 11, 20), exact, sizeof exact));
    DSP_CHECK_EQ_STR("abc@2026-11-20", exact);

    char small[14];
    memset(small, 'X', sizeof small);
    DSP_CHECK_EQ_INT(DSP_REC_ERR_BUFFER_FULL,
                     dsp_occurrence_id_format("abc", D(2026, 11, 20), small, sizeof small));
    DSP_CHECK_EQ_STR("", small); /* never a truncated, plausible-looking id */

    char id64[DSP_SCHEDULE_ID_MAX_LEN + 1];
    memset(id64, 'a', DSP_SCHEDULE_ID_MAX_LEN);
    id64[DSP_SCHEDULE_ID_MAX_LEN] = '\0';
    DSP_CHECK_EQ_INT(DSP_REC_OK, dsp_occurrence_id_format(id64, D(2099, 12, 31), buf, sizeof buf));
    DSP_CHECK_EQ_INT(DSP_OCCURRENCE_ID_BUF_LEN - 1, strlen(buf));

    DSP_CHECK_EQ_INT(DSP_REC_ERR_ARG, dsp_occurrence_id_format("a@b", D(2026, 11, 20), buf, sizeof buf));
    DSP_CHECK_EQ_STR("", buf);
    DSP_CHECK_EQ_INT(DSP_REC_ERR_ARG, dsp_occurrence_id_format("", D(2026, 11, 20), buf, sizeof buf));
    DSP_CHECK_EQ_INT(DSP_REC_ERR_ARG, dsp_occurrence_id_format("a", D(2026, 2, 29), buf, sizeof buf));
    DSP_CHECK_EQ_INT(DSP_REC_ERR_ARG, dsp_occurrence_id_format(NULL, D(2026, 11, 20), buf, sizeof buf));
    DSP_CHECK_EQ_INT(DSP_REC_ERR_ARG, dsp_occurrence_id_format("a", D(2026, 11, 20), NULL, 10));
    DSP_CHECK_EQ_INT(DSP_REC_ERR_ARG, dsp_occurrence_id_format("a", D(2026, 11, 20), buf, 0));
}

/* ---- Identity across edits -------------------------------------------------------- */

static void test_identity_stable_across_revision_and_time_edit(void)
{
    /* Revision 1 doses at 08:00 on 20 Nov. The user then edits the schedule to 20:00
     * (revision 2). The next occurrence after the handled instant falls on the SAME
     * local date with the SAME identity, so a durable "handled" record keyed by the
     * identity (M1.2/M1.3) will block a second dose that day. */
    dsp_schedule_t r1 = daily("s", D(2026, 11, 2), 8, 0);
    dsp_occurrence_t handled;
    DSP_CHECK_EQ_INT(DSP_REC_OK, dsp_rec_occurrence_on(&r1, TZ, D(2026, 11, 20), &handled));

    dsp_schedule_t r2 = r1;
    r2.revision = 2;
    r2.hour = 20;
    r2.amount_ul = 2500;
    dsp_occurrence_t next;
    DSP_CHECK_EQ_INT(DSP_REC_OK, dsp_rec_next(&r2, TZ, handled.at, &next));
    DSP_CHECK_EQ_INT(0, dsp_date_compare(handled.local_date, next.local_date));

    char id1[DSP_OCCURRENCE_ID_BUF_LEN];
    char id2[DSP_OCCURRENCE_ID_BUF_LEN];
    DSP_CHECK_EQ_INT(DSP_REC_OK, dsp_occurrence_id_format(r1.id, handled.local_date, id1, sizeof id1));
    DSP_CHECK_EQ_INT(DSP_REC_OK, dsp_occurrence_id_format(r2.id, next.local_date, id2, sizeof id2));
    DSP_CHECK_EQ_STR(id1, id2);
    DSP_CHECK_EQ_STR("s@2026-11-20", id2);
}

/* ---- Argument errors and extreme inputs ------------------------------------------- */

static void test_argument_errors(void)
{
    dsp_schedule_t s = daily("s", D(2026, 11, 2), 8, 0);
    dsp_occurrence_t occ;
    dsp_occurrence_t buf[2];
    size_t n = 7;
    size_t idx = 7;
    DSP_CHECK_EQ_INT(DSP_SCHED_E_ARG, dsp_schedule_validate(NULL, TZ));
    DSP_CHECK_EQ_INT(DSP_REC_ERR_ARG, dsp_rec_next(NULL, TZ, 0, &occ));
    DSP_CHECK_EQ_INT(DSP_REC_ERR_ARG, dsp_rec_next(&s, TZ, 0, NULL));
    DSP_CHECK_EQ_INT(DSP_REC_ERR_ARG, dsp_rec_occurrence_on(&s, TZ, D(2026, 11, 2), NULL));
    DSP_CHECK_EQ_INT(DSP_REC_ERR_ARG, dsp_rec_between(&s, TZ, 0, 1, buf, 2, NULL));
    DSP_CHECK_EQ_INT(DSP_REC_ERR_ARG, dsp_rec_between(&s, TZ, 0, 1, NULL, 2, &n));
    DSP_CHECK_EQ_INT(0, n);
    DSP_CHECK_EQ_INT(DSP_REC_ERR_ARG, dsp_rec_between(NULL, TZ, 0, 1, buf, 2, &n));
    DSP_CHECK_EQ_INT(DSP_REC_ERR_RANGE, dsp_rec_between(&s, TZ, 10, 9, buf, 2, &n));
    DSP_CHECK_EQ_INT(0, n);
    DSP_CHECK_EQ_INT(DSP_REC_ERR_ARG, dsp_rec_next_of_set(NULL, 1, TZ, 0, &idx, &occ));
    DSP_CHECK_EQ_INT(DSP_REC_ERR_ARG, dsp_rec_next_of_set(&s, 1, TZ, 0, NULL, &occ));
    DSP_CHECK_EQ_INT(DSP_REC_ERR_ARG, dsp_rec_next_of_set(&s, 1, TZ, 0, &idx, NULL));
    DSP_CHECK_EQ_INT(DSP_REC_NONE, dsp_rec_next_of_set(NULL, 0, TZ, 0, &idx, &occ));

    /* Invalid schedules are refused by every query, never evaluated. */
    dsp_schedule_t bad = s;
    bad.interval = 0;
    DSP_CHECK_EQ_INT(DSP_REC_ERR_SCHEDULE, dsp_rec_next(&bad, TZ, 0, &occ));
    DSP_CHECK_EQ_INT(DSP_REC_ERR_SCHEDULE, dsp_rec_between(&bad, TZ, 0, 1, buf, 2, &n));
    DSP_CHECK_EQ_INT(DSP_REC_ERR_SCHEDULE, dsp_rec_occurrence_on(&bad, TZ, D(2026, 11, 2), &occ));
    dsp_schedule_t set[3] = {s, bad, s};
    DSP_CHECK_EQ_INT(DSP_REC_ERR_SCHEDULE, dsp_rec_next_of_set(set, 3, TZ, 0, &idx, &occ));
    DSP_CHECK_EQ_INT(1, idx);

    /* Unterminated id (65 non-NUL bytes) is detected without reading past the array. */
    dsp_schedule_t unterminated = s;
    memset(unterminated.id, 'a', sizeof unterminated.id);
    DSP_CHECK_EQ_INT(DSP_SCHED_E_ID, dsp_schedule_validate(&unterminated, TZ));
}

static void test_extreme_instants_are_safe(void)
{
    dsp_schedule_t s = daily("s", D(2026, 11, 2), 8, 0);
    dsp_occurrence_t occ;
    DSP_CHECK_EQ_INT(DSP_REC_OK, dsp_rec_next(&s, TZ, INT64_MIN, &occ));
    DSP_CHECK_EQ_INT(0, dsp_date_compare(D(2026, 11, 2), occ.local_date));
    DSP_CHECK_EQ_INT(DSP_REC_NONE, dsp_rec_next(&s, TZ, INT64_MAX, &occ));
    DSP_CHECK_EQ_INT(DSP_REC_NONE, dsp_rec_next(&s, TZ, QUERY_MAX_2100_01_02, &occ));
    /* Last supported occurrence: 2099-12-31 08:00 CET. */
    DSP_CHECK_EQ_INT(DSP_REC_OK, dsp_rec_next(&s, TZ, QUERY_MAX_2100_01_02 - 3 * 86400, &occ));
    DSP_CHECK_EQ_INT(DSP_REC_OK, dsp_rec_next(&s, TZ, occ.at, &occ));
    DSP_CHECK_EQ_INT(0, dsp_date_compare(D(2099, 12, 31), occ.local_date));
    DSP_CHECK_EQ_INT(DSP_REC_NONE, dsp_rec_next(&s, TZ, occ.at, &occ));

    size_t n = 0;
    dsp_occurrence_t buf[1];
    DSP_CHECK_EQ_INT(DSP_REC_OK, dsp_rec_between(&s, TZ, INT64_MAX - 1, INT64_MAX, buf, 1, &n));
    DSP_CHECK_EQ_INT(0, n);

    /* Weekly with the maximum interval and every weekday: only the start week. */
    dsp_schedule_t w = s;
    w.kind = DSP_REC_WEEKLY;
    w.interval = DSP_REC_INTERVAL_MAX;
    w.weekday_mask = DSP_WEEKDAY_MASK_ALL;
    dsp_occurrence_t week[8];
    DSP_CHECK_EQ_INT(DSP_REC_OK, dsp_rec_between(&w, TZ, INT64_MIN, INT64_MAX, week, 8, &n));
    DSP_CHECK_EQ_INT(7, n); /* Mon 2 .. Sun 8 Nov 2026 */
}

static void test_next_of_set_ties_and_duplicates(void)
{
    /* Same id twice (not expected in practice): lower array index wins, deterministically. */
    dsp_schedule_t set[3] = {
        daily("b", D(2026, 11, 2), 8, 0),
        daily("a", D(2026, 11, 2), 8, 0),
        daily("a", D(2026, 11, 2), 8, 0),
    };
    size_t idx = 99;
    dsp_occurrence_t occ;
    DSP_CHECK_EQ_INT(DSP_REC_OK, dsp_rec_next_of_set(set, 3, TZ, T_2026_11_14_0700Z - 60, &idx, &occ));
    DSP_CHECK_EQ_INT(1, idx);
    DSP_CHECK_EQ_INT(T_2026_11_14_0700Z, occ.at);
}

/* ---- Supported input ranges of the public time API ---------------------------- */

static void test_time_api_range_boundaries(void)
{
    /* dsp_tz_offset_at: inclusive DSP_UTC_MIN..DSP_UTC_MAX; outside -> false, untouched. */
    int32_t off = 12345;
    DSP_CHECK(dsp_tz_offset_at(TZ, DSP_UTC_MIN, &off));
    DSP_CHECK_EQ_INT(3600, off); /* 1999-01-01: CET */
    off = 12345;
    DSP_CHECK(dsp_tz_offset_at(TZ, DSP_UTC_MAX, &off));
    DSP_CHECK_EQ_INT(3600, off); /* 2100-12-31: CET */
    const dsp_utc_t outside[] = {DSP_UTC_MIN - 1, DSP_UTC_MAX + 1, INT64_MIN, INT64_MAX, 0};
    for (size_t i = 0; i < sizeof outside / sizeof outside[0]; i++) {
        off = 12345;
        DSP_CHECK(!dsp_tz_offset_at(TZ, outside[i], &off));
        DSP_CHECK_EQ_INT(12345, off);
    }
    DSP_CHECK(!dsp_tz_offset_at(NULL, DSP_UTC_MIN, &off));
    DSP_CHECK(!dsp_tz_offset_at(TZ, DSP_UTC_MIN, NULL));
    dsp_tz_t bad = DSP_TZ_EUROPE_PRAGUE;
    bad.rule = (dsp_tz_dst_rule_t)9;
    DSP_CHECK(!dsp_tz_offset_at(&bad, DSP_UTC_MIN, &off));
    /* Summer instant inside the range, and the 2100 transitions at the upper edge. */
    DSP_CHECK(dsp_tz_offset_at(TZ, (dsp_utc_t)4118083200, &off)); /* 2100-07-01T00:00Z */
    DSP_CHECK_EQ_INT(7200, off);

    /* dsp_utc_to_local_date: same range; local dates may lie just outside 2000..2099. */
    dsp_date_t date = {7, 7, 7};
    DSP_CHECK(dsp_utc_to_local_date(TZ, DSP_UTC_MIN, &date));
    DSP_CHECK(date.year == 1999 && date.month == 1 && date.day == 1);
    DSP_CHECK(dsp_utc_to_local_date(TZ, DSP_UTC_MAX, &date));
    DSP_CHECK(date.year == 2101 && date.month == 1 && date.day == 1); /* 00:59:59 CET */
    for (size_t i = 0; i < sizeof outside / sizeof outside[0]; i++) {
        dsp_date_t untouched = {7, 7, 7};
        DSP_CHECK(!dsp_utc_to_local_date(TZ, outside[i], &untouched));
        DSP_CHECK(untouched.year == 7 && untouched.month == 7 && untouched.day == 7);
    }
    DSP_CHECK(!dsp_utc_to_local_date(TZ, DSP_UTC_MIN, NULL));
    DSP_CHECK(!dsp_utc_to_local_date(NULL, DSP_UTC_MIN, &date));
    /* Extreme fixed offsets at the range edges still produce representable dates. */
    dsp_tz_t west = {-14 * 3600, -14 * 3600, DSP_TZ_DST_NONE};
    dsp_tz_t east = {14 * 3600, 14 * 3600, DSP_TZ_DST_NONE};
    DSP_CHECK(dsp_utc_to_local_date(&west, DSP_UTC_MIN, &date));
    DSP_CHECK(date.year == 1998 && date.month == 12 && date.day == 31);
    DSP_CHECK(dsp_utc_to_local_date(&east, DSP_UTC_MAX, &date));
    DSP_CHECK(date.year == 2101 && date.month == 1 && date.day == 1);

    /* dsp_date_from_days: inclusive DSP_DAYS_MIN..DSP_DAYS_MAX. */
    DSP_CHECK(dsp_date_from_days(DSP_DAYS_MIN, &date));
    DSP_CHECK(date.year == 1900 && date.month == 1 && date.day == 1);
    DSP_CHECK(dsp_date_from_days(DSP_DAYS_MAX, &date));
    DSP_CHECK(date.year == 2199 && date.month == 12 && date.day == 31);
    const int32_t bad_days[] = {DSP_DAYS_MIN - 1, DSP_DAYS_MAX + 1, INT32_MIN, INT32_MAX};
    for (size_t i = 0; i < sizeof bad_days / sizeof bad_days[0]; i++) {
        dsp_date_t untouched = {7, 7, 7};
        DSP_CHECK(!dsp_date_from_days(bad_days[i], &untouched));
        DSP_CHECK(untouched.year == 7);
    }
    DSP_CHECK(!dsp_date_from_days(0, NULL));

    /* dsp_date_to_days and dsp_weekday_from_days are total: extreme inputs must not
     * overflow (UBSan would report it). */
    dsp_date_t extreme[] = {{INT16_MIN, 255, 255}, {INT16_MAX, 255, 255}, {0, 0, 0}};
    for (size_t i = 0; i < sizeof extreme / sizeof extreme[0]; i++) {
        (void)dsp_date_to_days(extreme[i]);
    }
    DSP_CHECK_EQ_INT(DSP_FRIDAY, dsp_weekday_from_days(INT32_MAX)); /* 2147483647 + 3 = 7k + 4 */
    DSP_CHECK(dsp_weekday_from_days(INT32_MIN) >= 0 && dsp_weekday_from_days(INT32_MIN) <= 6);

    /* dsp_local_to_utc: invalid inputs are reported, output untouched. */
    dsp_utc_t t = 777;
    DSP_CHECK_EQ_INT(DSP_LOCAL_INVALID, dsp_local_to_utc(TZ, D(1999, 12, 31), 8, 0, &t));
    DSP_CHECK_EQ_INT(DSP_LOCAL_INVALID, dsp_local_to_utc(TZ, D(2100, 1, 1), 8, 0, &t));
    DSP_CHECK_EQ_INT(DSP_LOCAL_INVALID, dsp_local_to_utc(TZ, D(2026, 2, 29), 8, 0, &t));
    DSP_CHECK_EQ_INT(DSP_LOCAL_INVALID, dsp_local_to_utc(TZ, D(2026, 11, 2), -1, 0, &t));
    DSP_CHECK_EQ_INT(DSP_LOCAL_INVALID, dsp_local_to_utc(TZ, D(2026, 11, 2), 24, 0, &t));
    DSP_CHECK_EQ_INT(DSP_LOCAL_INVALID, dsp_local_to_utc(TZ, D(2026, 11, 2), 8, -1, &t));
    DSP_CHECK_EQ_INT(DSP_LOCAL_INVALID, dsp_local_to_utc(TZ, D(2026, 11, 2), 8, 60, &t));
    DSP_CHECK_EQ_INT(DSP_LOCAL_INVALID, dsp_local_to_utc(NULL, D(2026, 11, 2), 8, 0, &t));
    DSP_CHECK_EQ_INT(DSP_LOCAL_INVALID, dsp_local_to_utc(TZ, D(2026, 11, 2), 8, 0, NULL));
    DSP_CHECK_EQ_INT(777, t);
    /* First and last supported local minutes convert inside the UTC range. */
    DSP_CHECK_EQ_INT(DSP_LOCAL_UNIQUE, dsp_local_to_utc(TZ, D(2000, 1, 1), 0, 0, &t));
    DSP_CHECK(t >= DSP_UTC_MIN);
    DSP_CHECK_EQ_INT(DSP_LOCAL_UNIQUE, dsp_local_to_utc(TZ, D(2099, 12, 31), 23, 59, &t));
    DSP_CHECK(t <= DSP_UTC_MAX);

    /* dsp_tz_transitions: years 1999..2100 only. */
    dsp_utc_t a = 1, b = 2;
    DSP_CHECK(dsp_tz_transitions(TZ, 1999, &a, &b));
    DSP_CHECK(dsp_tz_transitions(TZ, 2100, &a, &b));
    DSP_CHECK(b <= DSP_UTC_MAX && a > DSP_UTC_MIN);
    a = 1;
    DSP_CHECK(!dsp_tz_transitions(TZ, 1998, &a, &b));
    DSP_CHECK(!dsp_tz_transitions(TZ, 2101, &a, &b));
    DSP_CHECK_EQ_INT(1, a);
}

/* ---- M1.10b.1: schedule names ------------------------------------------------------- */

static void test_schedule_name_validation(void)
{
    dsp_schedule_t s = daily("n", D(2026, 11, 2), 8, 0);
    struct {
        const char *name;
        bool ok;
    } cases[] = {
        {"", true},
        {"Morning dose", true},
        {"\xC5\x98\xC3\xA1no \xF0\x9F\x92\x8A", true}, /* "Řáno 💊" */
        {"\xE2\x82\xAC", true},                         /* U+20AC */
        {"\xF4\x8F\xBF\xBF", true},                     /* U+10FFFF */
        {"\xC0\xAF", false},                             /* overlong */
        {"\xE0\x80\xAF", false},                         /* overlong */
        {"\xED\xBF\xBF", false},                         /* surrogate */
        {"\xF4\x90\x80\x80", false},                     /* above U+10FFFF */
        {"\xF5\x80\x80\x80", false},
        {"\x80", false},                                 /* lone continuation */
        {"\xE2\x82", false},                             /* truncated */
        {"tab\there", false},                            /* control character */
        {"del\x7F", false},
        {"\x1F", false},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        memset(s.name, 0, sizeof s.name);
        strcpy(s.name, cases[i].name);
        DSP_CHECK_EQ_INT(cases[i].ok ? 0 : DSP_SCHED_E_NAME, dsp_schedule_validate(&s, TZ));
    }
    memset(s.name, 'a', DSP_SCHEDULE_NAME_MAX_BYTES);
    s.name[DSP_SCHEDULE_NAME_MAX_BYTES] = '\0';
    DSP_CHECK_EQ_INT(0, dsp_schedule_validate(&s, TZ)); /* exactly 32 bytes */
    s.name[DSP_SCHEDULE_NAME_MAX_BYTES] = 'a';         /* no terminator within 33 bytes */
    DSP_CHECK_EQ_INT(DSP_SCHED_E_NAME, dsp_schedule_validate(&s, TZ));
    /* A 3-byte character that would end at byte 33 does not fit. */
    memset(s.name, 'a', DSP_SCHEDULE_NAME_MAX_BYTES - 2);
    memcpy(s.name + DSP_SCHEDULE_NAME_MAX_BYTES - 2, "\xE2\x82", 2);
    s.name[DSP_SCHEDULE_NAME_MAX_BYTES] = '\0';
    DSP_CHECK_EQ_INT(DSP_SCHED_E_NAME, dsp_schedule_validate(&s, TZ)); /* truncated */
    memset(s.name, 0, sizeof s.name);
    /* Edit floor: 0 or a valid instant. */
    s.not_before = DSP_UTC_MIN;
    DSP_CHECK_EQ_INT(0, dsp_schedule_validate(&s, TZ));
    s.not_before = DSP_UTC_MIN - 1;
    DSP_CHECK_EQ_INT(DSP_SCHED_E_NOT_BEFORE, dsp_schedule_validate(&s, TZ));
    s.not_before = DSP_UTC_MAX + 1;
    DSP_CHECK_EQ_INT(DSP_SCHED_E_NOT_BEFORE, dsp_schedule_validate(&s, TZ));
    /* Names do not affect occurrences or identity. */
    dsp_schedule_t a = daily("n", D(2026, 11, 2), 8, 0);
    dsp_schedule_t b = a;
    strcpy(b.name, "x");
    dsp_occurrence_t oa, ob;
    DSP_CHECK_EQ_INT(DSP_REC_OK, dsp_rec_next(&a, TZ, T_2026_11_14_0700Z, &oa));
    DSP_CHECK_EQ_INT(DSP_REC_OK, dsp_rec_next(&b, TZ, T_2026_11_14_0700Z, &ob));
    DSP_CHECK_EQ_INT(oa.at, ob.at);
}

int main(void)
{
    DSP_RUN(test_schedule_name_validation);
    DSP_RUN(test_dates_match_naive_calendar_walk);
    DSP_RUN(test_invalid_dates_rejected);
    DSP_RUN(test_timezone_validation);
    DSP_RUN(test_local_date_of_instants_around_midnight_and_dst);
    DSP_RUN(test_jump_search_matches_brute_force);
    DSP_RUN(test_between_buffer_full_is_reported_and_resumable);
    DSP_RUN(test_between_whole_supported_range_is_bounded);
    DSP_RUN(test_occurrence_id_buffer_handling);
    DSP_RUN(test_identity_stable_across_revision_and_time_edit);
    DSP_RUN(test_argument_errors);
    DSP_RUN(test_extreme_instants_are_safe);
    DSP_RUN(test_next_of_set_ties_and_duplicates);
    DSP_RUN(test_time_api_range_boundaries);
    return dsp_test_finish();
}
