/*
 * Runs the shared recurrence test vectors (repository test-vectors/ directory, format v1) against the
 * C engine. The same files are consumed by the Android Kotlin reference engine.
 *
 * Expected UTC strings are converted with a naive, test-local day counter (not with
 * the date arithmetic under test), so a bug in dsp_time.c cannot cancel itself out.
 */
#include "dsp_recurrence.h"
#include "dsp_test.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DSP_VECTORS_DIR
#error "DSP_VECTORS_DIR must be defined by the build"
#endif

#define MAX_LINE 2048
#define MAX_TOKENS 32
#define MAX_SCHEDULES 128
#define MAX_LIST 16

static const dsp_tz_t *const TZ = &DSP_TZ_EUROPE_PRAGUE;

/* ---- Naive reference date arithmetic (test-local, deliberately simple) ---------- */

static int ref_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int ref_dim(int y, int m)
{
    static const int dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return (m == 2 && ref_leap(y)) ? 29 : dim[m - 1];
}

/* Days since 1970-01-01 by counting whole years and months (years >= 1970 only). */
static int64_t ref_days(int y, int m, int d)
{
    int64_t days = 0;
    for (int yy = 1970; yy < y; yy++) {
        days += ref_leap(yy) ? 366 : 365;
    }
    for (int mm = 1; mm < m; mm++) {
        days += ref_dim(y, mm);
    }
    return days + d - 1;
}

/* ---- Parsing helpers -------------------------------------------------------------- */

static int line_no;
static int parse_errors;
static char where[96]; /* label of the current record for failure messages */

static void parse_fail(const char *what, const char *value)
{
    parse_errors++;
    fprintf(stderr, "vector line %d: cannot parse %s '%s'\n", line_no, what, value ? value : "");
}

typedef struct {
    char *key[MAX_TOKENS];
    char *val[MAX_TOKENS];
    int n;
} kv_t;

static const char *kv_get(const kv_t *kv, const char *key)
{
    for (int i = 0; i < kv->n; i++) {
        if (kv->key[i] != NULL && strcmp(kv->key[i], key) == 0) {
            return kv->val[i];
        }
    }
    return NULL;
}

static const char *kv_req(const kv_t *kv, const char *key)
{
    const char *v = kv_get(kv, key);
    if (v == NULL) {
        parse_fail("missing key", key);
        return "";
    }
    return v;
}

static bool parse_uint(const char *s, unsigned long long *out)
{
    if (s == NULL || *s == '\0') {
        return false;
    }
    char *end = NULL;
    *out = strtoull(s, &end, 10);
    return end != NULL && *end == '\0';
}

static bool parse_date(const char *s, dsp_date_t *out)
{
    int y, m, d;
    char tail;
    if (s == NULL || sscanf(s, "%4d-%2d-%2d%c", &y, &m, &d, &tail) != 3) {
        return false;
    }
    out->year = (int16_t)y;
    out->month = (uint8_t)m;
    out->day = (uint8_t)d;
    return true;
}

static bool parse_instant(const char *s, dsp_utc_t *out)
{
    int y, mo, d, h, mi, se;
    char z, tail;
    if (s == NULL ||
        sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d%c%c", &y, &mo, &d, &h, &mi, &se, &z, &tail) != 7 ||
        z != 'Z' || y < 1970 || mo < 1 || mo > 12 || d < 1 || d > ref_dim(y, mo)) {
        return false;
    }
    *out = ref_days(y, mo, d) * 86400 + h * 3600 + mi * 60 + se;
    return true;
}

static bool parse_time(const char *s, int *h, int *m)
{
    char tail;
    return s != NULL && sscanf(s, "%2d:%2d%c", h, m, &tail) == 2;
}

static bool date_eq(dsp_date_t a, dsp_date_t b)
{
    return a.year == b.year && a.month == b.month && a.day == b.day;
}

/* "<date>@<instant>" */
typedef struct {
    dsp_date_t date;
    dsp_utc_t at;
} expected_occ_t;

/* Parses a comma-separated list, "-" = empty. Returns count or -1. */
static int parse_occ_list(const char *s, expected_occ_t *out, int max)
{
    if (strcmp(s, "-") == 0) {
        return 0;
    }
    char buf[MAX_LINE];
    snprintf(buf, sizeof buf, "%s", s);
    int n = 0;
    for (char *item = buf; item != NULL && *item != '\0';) {
        char *comma = strchr(item, ',');
        if (comma != NULL) {
            *comma = '\0';
        }
        char *at = strchr(item, '@');
        if (at == NULL || n == max) {
            return -1;
        }
        *at = '\0';
        if (!parse_date(item, &out[n].date) || !parse_instant(at + 1, &out[n].at)) {
            return -1;
        }
        n++;
        item = comma != NULL ? comma + 1 : NULL;
    }
    return n;
}

/* ---- Schedules -------------------------------------------------------------------- */

typedef struct {
    char name[48];
    dsp_schedule_t s;
} named_schedule_t;

static named_schedule_t schedules[MAX_SCHEDULES];
static int n_schedules;

static const dsp_schedule_t *find_schedule(const char *name)
{
    for (int i = 0; i < n_schedules; i++) {
        if (strcmp(schedules[i].name, name) == 0) {
            return &schedules[i].s;
        }
    }
    parse_fail("unknown schedule", name);
    return NULL;
}

static uint8_t parse_weekdays(const char *s)
{
    static const char *const names[7] = {"MO", "TU", "WE", "TH", "FR", "SA", "SU"};
    if (strcmp(s, "-") == 0) {
        return 0;
    }
    uint8_t mask = 0;
    for (const char *p = s; *p != '\0';) {
        int found = -1;
        for (int i = 0; i < 7; i++) {
            if (strncmp(p, names[i], 2) == 0) {
                found = i;
            }
        }
        if (found < 0) {
            parse_fail("weekdays", s);
            return 0;
        }
        mask = (uint8_t)(mask | DSP_WEEKDAY_BIT(found));
        p += 2;
        if (*p == ',') {
            p++;
        }
    }
    return mask;
}

static void add_schedule(const kv_t *kv)
{
    if (n_schedules == MAX_SCHEDULES) {
        parse_fail("too many schedules", "");
        return;
    }
    named_schedule_t *ns = &schedules[n_schedules++];
    memset(ns, 0, sizeof *ns);
    snprintf(ns->name, sizeof ns->name, "%s", kv_req(kv, "name"));
    dsp_schedule_t *s = &ns->s;

    /* Copy up to sizeof id bytes; a 65-character id deliberately leaves no NUL. */
    const char *id = kv_req(kv, "id");
    size_t id_len = strlen(id);
    memcpy(s->id, id, id_len < sizeof s->id ? id_len : sizeof s->id);

    unsigned long long u;
    if (!parse_uint(kv_req(kv, "rev"), &u) || u > UINT32_MAX) {
        parse_fail("rev", kv_get(kv, "rev"));
    }
    s->revision = (uint32_t)u;
    s->enabled = strcmp(kv_req(kv, "enabled"), "true") == 0;
    if (!parse_uint(kv_req(kv, "amount_ul"), &u) || u > UINT32_MAX) {
        parse_fail("amount_ul", kv_get(kv, "amount_ul"));
    }
    s->amount_ul = (uint32_t)u;
    if (!parse_date(kv_req(kv, "start"), &s->start_date)) {
        parse_fail("start", kv_get(kv, "start"));
    }
    int h, m;
    if (!parse_time(kv_req(kv, "time"), &h, &m)) {
        parse_fail("time", kv_get(kv, "time"));
    }
    s->hour = (uint8_t)h;
    s->minute = (uint8_t)m;

    const char *kind = kv_req(kv, "kind");
    s->kind = strcmp(kind, "ONCE") == 0     ? DSP_REC_ONCE
              : strcmp(kind, "DAILY") == 0  ? DSP_REC_DAILY
              : strcmp(kind, "WEEKLY") == 0 ? DSP_REC_WEEKLY
                                            : 3; /* unknown kind, e.g. MONTHLY */
    if (!parse_uint(kv_req(kv, "interval"), &u) || u > UINT32_MAX) {
        parse_fail("interval", kv_get(kv, "interval"));
    }
    s->interval = (uint32_t)u;
    s->weekday_mask = parse_weekdays(kv_req(kv, "weekdays"));
    const char *end = kv_req(kv, "end");
    if (strcmp(end, "-") != 0) {
        s->has_end_date = true;
        if (!parse_date(end, &s->end_date)) {
            parse_fail("end", end);
        }
    }
}

/* ---- Records ---------------------------------------------------------------------- */

static int n_by_type[9];
enum { R_VALIDATE, R_LOCAL, R_NEXT, R_SEQ, R_BETWEEN, R_ON, R_ID, R_SET, R_TZ };

static void check_occ(const dsp_occurrence_t *got, const expected_occ_t *exp)
{
    DSP_CHECK_EQ_INT(exp->at, got->at);
    DSP_CHECK(date_eq(exp->date, got->local_date));
}

static dsp_sched_errors_t parse_errors_list(const char *s)
{
    static const struct {
        const char *name;
        dsp_sched_errors_t bit;
    } map[] = {
        {"AMOUNT_NOT_POSITIVE", DSP_SCHED_E_AMOUNT_NOT_POSITIVE},
        {"TIME_NONEXISTENT", DSP_SCHED_E_TIME_NONEXISTENT},
        {"INTERVAL_TOO_SMALL", DSP_SCHED_E_INTERVAL_TOO_SMALL},
        {"NO_WEEKDAYS", DSP_SCHED_E_NO_WEEKDAYS},
        {"END_BEFORE_START", DSP_SCHED_E_END_BEFORE_START},
        {"ID", DSP_SCHED_E_ID},
        {"REVISION", DSP_SCHED_E_REVISION},
        {"START_DATE", DSP_SCHED_E_START_DATE},
        {"END_DATE", DSP_SCHED_E_END_DATE},
        {"TIME", DSP_SCHED_E_TIME},
        {"KIND", DSP_SCHED_E_KIND},
        {"INTERVAL_TOO_LARGE", DSP_SCHED_E_INTERVAL_TOO_LARGE},
        {"WEEKDAYS", DSP_SCHED_E_WEEKDAYS},
        {"ONCE_FORM", DSP_SCHED_E_ONCE_FORM},
    };
    if (strcmp(s, "OK") == 0) {
        return DSP_SCHED_OK;
    }
    dsp_sched_errors_t bits = 0;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", s);
    for (char *tok = buf; tok != NULL;) {
        char *comma = strchr(tok, ',');
        if (comma != NULL) {
            *comma = '\0';
        }
        bool found = false;
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
            if (strcmp(map[i].name, tok) == 0) {
                bits |= map[i].bit;
                found = true;
            }
        }
        if (!found) {
            parse_fail("error name", tok);
        }
        tok = comma != NULL ? comma + 1 : NULL;
    }
    return bits;
}

static void run_validate(const kv_t *kv)
{
    const dsp_schedule_t *s = find_schedule(kv_req(kv, "schedule"));
    if (s == NULL) {
        return;
    }
    dsp_sched_errors_t expected = parse_errors_list(kv_req(kv, "expect"));
    DSP_CHECK_EQ_INT(expected, dsp_schedule_validate(s, TZ));
    n_by_type[R_VALIDATE]++;
}

static void run_local(const kv_t *kv)
{
    dsp_date_t date;
    int h, m;
    if (!parse_date(kv_req(kv, "date"), &date) || !parse_time(kv_req(kv, "time"), &h, &m)) {
        parse_fail("local_to_utc", "");
        return;
    }
    const char *kind = kv_req(kv, "kind");
    dsp_local_kind_t exp_kind = strcmp(kind, "unique") == 0    ? DSP_LOCAL_UNIQUE
                                : strcmp(kind, "overlap") == 0 ? DSP_LOCAL_OVERLAP
                                                               : DSP_LOCAL_GAP;
    dsp_utc_t got = 0;
    DSP_CHECK_EQ_INT(exp_kind, dsp_local_to_utc(TZ, date, h, m, &got));
    if (exp_kind != DSP_LOCAL_GAP) {
        dsp_utc_t exp;
        if (!parse_instant(kv_req(kv, "expect"), &exp)) {
            parse_fail("expect", kv_get(kv, "expect"));
            return;
        }
        DSP_CHECK_EQ_INT(exp, got);
    }
    n_by_type[R_LOCAL]++;
}

static void run_next(const kv_t *kv)
{
    const dsp_schedule_t *s = find_schedule(kv_req(kv, "schedule"));
    dsp_utc_t after;
    if (s == NULL || !parse_instant(kv_req(kv, "after"), &after)) {
        parse_fail("next", "");
        return;
    }
    const char *expect = kv_req(kv, "expect");
    dsp_occurrence_t occ;
    dsp_rec_status_t st = dsp_rec_next(s, TZ, after, &occ);
    if (strcmp(expect, "none") == 0) {
        DSP_CHECK_EQ_INT(DSP_REC_NONE, st);
    } else {
        expected_occ_t exp;
        if (parse_occ_list(expect, &exp, 1) != 1) {
            parse_fail("expect", expect);
            return;
        }
        DSP_CHECK_EQ_INT(DSP_REC_OK, st);
        if (st == DSP_REC_OK) {
            check_occ(&occ, &exp);
        }
    }
    n_by_type[R_NEXT]++;
}

static void run_seq(const kv_t *kv)
{
    const dsp_schedule_t *s = find_schedule(kv_req(kv, "schedule"));
    dsp_utc_t after;
    unsigned long long n;
    expected_occ_t exp[MAX_LIST];
    int n_exp = parse_occ_list(kv_req(kv, "expect"), exp, MAX_LIST);
    if (s == NULL || !parse_instant(kv_req(kv, "after"), &after) ||
        !parse_uint(kv_req(kv, "n"), &n) || n > MAX_LIST || n_exp < 0) {
        parse_fail("seq", "");
        return;
    }
    int got = 0;
    for (unsigned long long i = 0; i < n; i++) {
        dsp_occurrence_t occ;
        dsp_rec_status_t st = dsp_rec_next(s, TZ, after, &occ);
        if (st == DSP_REC_NONE) {
            break;
        }
        DSP_CHECK_EQ_INT(DSP_REC_OK, st);
        if (st != DSP_REC_OK) {
            break;
        }
        if (got < n_exp) {
            check_occ(&occ, &exp[got]);
        }
        got++;
        after = occ.at;
    }
    DSP_CHECK_EQ_INT(n_exp, got);
    n_by_type[R_SEQ]++;
}

static void run_between(const kv_t *kv)
{
    const dsp_schedule_t *s = find_schedule(kv_req(kv, "schedule"));
    dsp_utc_t after, until;
    unsigned long long limit;
    if (s == NULL || !parse_instant(kv_req(kv, "after"), &after) ||
        !parse_instant(kv_req(kv, "until"), &until) ||
        !parse_uint(kv_req(kv, "limit"), &limit) || limit > MAX_LIST) {
        parse_fail("between", "");
        return;
    }
    const char *expect = kv_req(kv, "expect");
    dsp_occurrence_t buf[MAX_LIST];
    size_t count = 99;
    dsp_rec_status_t st = dsp_rec_between(s, TZ, after, until, limit > 0 ? buf : NULL,
                                          (size_t)limit, &count);
    if (strcmp(expect, "overflow") == 0) {
        DSP_CHECK_EQ_INT(DSP_REC_ERR_BUFFER_FULL, st);
        DSP_CHECK_EQ_INT(limit, count);
    } else {
        expected_occ_t exp[MAX_LIST];
        int n_exp = parse_occ_list(expect, exp, MAX_LIST);
        if (n_exp < 0) {
            parse_fail("expect", expect);
            return;
        }
        DSP_CHECK_EQ_INT(DSP_REC_OK, st);
        DSP_CHECK_EQ_INT(n_exp, count);
        for (int i = 0; i < n_exp && (size_t)i < count; i++) {
            check_occ(&buf[i], &exp[i]);
        }
    }
    n_by_type[R_BETWEEN]++;
}

static void run_on(const kv_t *kv)
{
    const dsp_schedule_t *s = find_schedule(kv_req(kv, "schedule"));
    dsp_date_t date;
    if (s == NULL || !parse_date(kv_req(kv, "date"), &date)) {
        parse_fail("occurrence_on", "");
        return;
    }
    const char *expect = kv_req(kv, "expect");
    dsp_occurrence_t occ;
    dsp_rec_status_t st = dsp_rec_occurrence_on(s, TZ, date, &occ);
    if (strcmp(expect, "none") == 0) {
        DSP_CHECK_EQ_INT(DSP_REC_NONE, st);
    } else {
        expected_occ_t exp = {.date = date};
        if (!parse_instant(expect, &exp.at)) {
            parse_fail("expect", expect);
            return;
        }
        DSP_CHECK_EQ_INT(DSP_REC_OK, st);
        if (st == DSP_REC_OK) {
            check_occ(&occ, &exp);
        }
    }
    n_by_type[R_ON]++;
}

static void run_id(const kv_t *kv)
{
    const dsp_schedule_t *s = find_schedule(kv_req(kv, "schedule"));
    dsp_date_t date;
    if (s == NULL || !parse_date(kv_req(kv, "date"), &date)) {
        parse_fail("occurrence_id", "");
        return;
    }
    char buf[DSP_OCCURRENCE_ID_BUF_LEN];
    DSP_CHECK_EQ_INT(DSP_REC_OK, dsp_occurrence_id_format(s->id, date, buf, sizeof buf));
    DSP_CHECK_EQ_STR(kv_req(kv, "expect"), buf);
    n_by_type[R_ID]++;
}

static void run_set(const kv_t *kv)
{
    dsp_schedule_t set[MAX_LIST];
    size_t n = 0;
    char names[MAX_LINE];
    snprintf(names, sizeof names, "%s", kv_req(kv, "schedules"));
    if (strcmp(names, "-") != 0) {
        for (char *tok = names; tok != NULL && n < MAX_LIST;) {
            char *comma = strchr(tok, ',');
            if (comma != NULL) {
                *comma = '\0';
            }
            const dsp_schedule_t *s = find_schedule(tok);
            if (s == NULL) {
                return;
            }
            set[n++] = *s;
            tok = comma != NULL ? comma + 1 : NULL;
        }
    }
    dsp_utc_t after;
    if (!parse_instant(kv_req(kv, "after"), &after)) {
        parse_fail("after", kv_get(kv, "after"));
        return;
    }
    const char *expect = kv_req(kv, "expect");
    size_t index = 0;
    dsp_occurrence_t occ;
    dsp_rec_status_t st = dsp_rec_next_of_set(n > 0 ? set : NULL, n, TZ, after, &index, &occ);
    if (strcmp(expect, "none") == 0) {
        DSP_CHECK_EQ_INT(DSP_REC_NONE, st);
    } else {
        char id[DSP_SCHEDULE_ID_MAX_LEN + 1];
        const char *at = strchr(expect, '@');
        dsp_utc_t exp_at;
        if (at == NULL || (size_t)(at - expect) >= sizeof id || !parse_instant(at + 1, &exp_at)) {
            parse_fail("expect", expect);
            return;
        }
        memcpy(id, expect, (size_t)(at - expect));
        id[at - expect] = '\0';
        DSP_CHECK_EQ_INT(DSP_REC_OK, st);
        if (st == DSP_REC_OK) {
            DSP_CHECK_EQ_STR(id, set[index].id);
            DSP_CHECK_EQ_INT(exp_at, occ.at);
        }
    }
    n_by_type[R_SET]++;
}

/* Offset at t; an out-of-range failure is a test failure (and yields a sentinel). */
static int32_t offset_of(dsp_utc_t t)
{
    int32_t off = INT32_MIN;
    DSP_CHECK(dsp_tz_offset_at(TZ, t, &off));
    return off;
}

/* One reference DST year: transition instants plus the local-time behavior around them. */
static void run_tz(const kv_t *kv)
{
    unsigned long long year;
    dsp_utc_t exp_start, exp_end;
    if (!parse_uint(kv_req(kv, "year"), &year) ||
        !parse_instant(kv_req(kv, "dst_start"), &exp_start) ||
        !parse_instant(kv_req(kv, "dst_end"), &exp_end)) {
        parse_fail("tz_transition", "");
        return;
    }
    dsp_utc_t start = 0, end = 0;
    DSP_CHECK(dsp_tz_transitions(TZ, (int)year, &start, &end));
    DSP_CHECK_EQ_INT(exp_start, start);
    DSP_CHECK_EQ_INT(exp_end, end);

    /* Offsets just before/at each transition. */
    DSP_CHECK_EQ_INT(3600, offset_of(exp_start - 1));
    DSP_CHECK_EQ_INT(7200, offset_of(exp_start));
    DSP_CHECK_EQ_INT(7200, offset_of(exp_end - 1));
    DSP_CHECK_EQ_INT(3600, offset_of(exp_end));

    /* Local transition days: 01:00Z is 02:00 CET (spring) / 03:00 CEST (autumn), same date.
     * The local date is derived from the reference instant with the naive helper. */
    int64_t spring_day = (exp_start + 3600) / 86400;
    int64_t autumn_day = (exp_end + 7200) / 86400;
    dsp_date_t spring = {(int16_t)year, 3, (uint8_t)(spring_day - ref_days((int)year, 3, 1) + 1)};
    dsp_date_t autumn = {(int16_t)year, 10, (uint8_t)(autumn_day - ref_days((int)year, 10, 1) + 1)};

    dsp_utc_t t = 0;
    DSP_CHECK_EQ_INT(DSP_LOCAL_UNIQUE, dsp_local_to_utc(TZ, spring, 1, 59, &t));
    DSP_CHECK_EQ_INT(exp_start - 60, t);
    DSP_CHECK_EQ_INT(DSP_LOCAL_GAP, dsp_local_to_utc(TZ, spring, 2, 0, &t));
    DSP_CHECK_EQ_INT(DSP_LOCAL_GAP, dsp_local_to_utc(TZ, spring, 2, 59, &t));
    DSP_CHECK_EQ_INT(DSP_LOCAL_UNIQUE, dsp_local_to_utc(TZ, spring, 3, 0, &t));
    DSP_CHECK_EQ_INT(exp_start, t);

    DSP_CHECK_EQ_INT(DSP_LOCAL_UNIQUE, dsp_local_to_utc(TZ, autumn, 1, 59, &t));
    DSP_CHECK_EQ_INT(exp_end - 3600 - 60, t);
    DSP_CHECK_EQ_INT(DSP_LOCAL_OVERLAP, dsp_local_to_utc(TZ, autumn, 2, 0, &t));
    DSP_CHECK_EQ_INT(exp_end - 3600, t); /* earlier instant (CEST) */
    DSP_CHECK_EQ_INT(DSP_LOCAL_OVERLAP, dsp_local_to_utc(TZ, autumn, 2, 59, &t));
    DSP_CHECK_EQ_INT(exp_end - 60, t);
    DSP_CHECK_EQ_INT(DSP_LOCAL_UNIQUE, dsp_local_to_utc(TZ, autumn, 3, 0, &t));
    DSP_CHECK_EQ_INT(exp_end + 3600, t);
    n_by_type[R_TZ]++;
}

/* ---- Driver ----------------------------------------------------------------------- */

static int tokenize(char *line, kv_t *kv, char **type)
{
    *type = NULL;
    kv->n = 0;
    for (char *p = line; *p != '\0';) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            *p++ = '\0';
        }
        if (*p == '\0') {
            break;
        }
        char *tok = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
            p++;
        }
        if (*p != '\0') {
            *p++ = '\0';
        }
        if (*type == NULL) {
            *type = tok;
            continue;
        }
        char *eq = strchr(tok, '=');
        if (eq == NULL || kv->n == MAX_TOKENS) {
            return -1;
        }
        *eq = '\0';
        kv->key[kv->n] = tok;
        kv->val[kv->n] = eq + 1;
        kv->n++;
    }
    return 0;
}

static void run_file(const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", DSP_VECTORS_DIR, name);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        parse_fail("vector file", path);
        return;
    }
    char line[MAX_LINE];
    line_no = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        line_no++;
        if (strchr(line, '\n') == NULL && !feof(f)) {
            parse_fail("line too long", "");
            break;
        }
        char *hash = strchr(line, '#');
        if (hash != NULL) {
            *hash = '\0';
        }
        kv_t kv;
        char *type;
        if (tokenize(line, &kv, &type) != 0) {
            parse_fail("tokens", line);
            continue;
        }
        if (type == NULL) {
            continue;
        }
        snprintf(where, sizeof where, "%s line %d", name, line_no);
        dsp_test_current = where;
        if (strcmp(type, "schedule") == 0) {
            add_schedule(&kv);
        } else if (strcmp(type, "validate") == 0) {
            run_validate(&kv);
        } else if (strcmp(type, "local_to_utc") == 0) {
            run_local(&kv);
        } else if (strcmp(type, "next") == 0) {
            run_next(&kv);
        } else if (strcmp(type, "seq") == 0) {
            run_seq(&kv);
        } else if (strcmp(type, "between") == 0) {
            run_between(&kv);
        } else if (strcmp(type, "occurrence_on") == 0) {
            run_on(&kv);
        } else if (strcmp(type, "occurrence_id") == 0) {
            run_id(&kv);
        } else if (strcmp(type, "next_set") == 0) {
            run_set(&kv);
        } else if (strcmp(type, "tz_transition") == 0) {
            run_tz(&kv);
        } else {
            parse_fail("record type", type);
        }
    }
    fclose(f);
}

static void test_recurrence_vectors(void)
{
    run_file("recurrence_v1.txt");
}

static void test_dst_reference_2000_2099(void)
{
    run_file("europe_prague_dst_v1.txt");
}

static void test_all_records_parsed(void)
{
    DSP_CHECK_EQ_INT(0, parse_errors);
    DSP_CHECK_EQ_INT(100, n_by_type[R_TZ]);
    printf("vectors: schedules=%d validate=%d local_to_utc=%d next=%d seq=%d between=%d "
           "occurrence_on=%d occurrence_id=%d next_set=%d tz_years=%d\n",
           n_schedules, n_by_type[R_VALIDATE], n_by_type[R_LOCAL], n_by_type[R_NEXT],
           n_by_type[R_SEQ], n_by_type[R_BETWEEN], n_by_type[R_ON], n_by_type[R_ID],
           n_by_type[R_SET], n_by_type[R_TZ]);
    for (int i = 0; i < R_TZ; i++) {
        DSP_CHECK(n_by_type[i] > 0); /* every record type actually exercised */
    }
}

int main(void)
{
    DSP_RUN(test_recurrence_vectors);
    DSP_RUN(test_dst_reference_2000_2099);
    DSP_RUN(test_all_records_parsed);
    return dsp_test_finish();
}
