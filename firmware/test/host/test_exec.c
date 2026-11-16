/*
 * Execution coordinator and history (M1.2) tests with an injected fake store that
 * models durable vs uncommitted writes, power loss and storage faults.
 */
#include "dsp_exec.h"
#include "dsp_test.h"
#include "fake_store.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const dsp_tz_t *const TZ = &DSP_TZ_EUROPE_PRAGUE;

static fake_store_t store;
static dsp_exec_t ex;
static dsp_exec_config_t config;

/* "Restart": fresh in-RAM coordinator on the same (persistent) store. */
static void boot(void)
{
    fake_store_power_cycle(&store);
    config.store = &store.iface;
    config.tz = TZ;
    config.due_window_s = DSP_EXEC_DUE_WINDOW_DEFAULT_S;
    config.retention_days = DSP_EXEC_RETENTION_DAYS_DEFAULT;
    memset(&ex, 0xA5, sizeof ex); /* garbage, as after a real reset */
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_init(&ex, &config));
}

static void fresh(size_t slots)
{
    fake_store_init(&store, slots);
    boot();
}

static dsp_date_t D(int y, int m, int d)
{
    dsp_date_t date = {(int16_t)y, (uint8_t)m, (uint8_t)d};
    return date;
}

static dsp_schedule_t daily(const char *id, dsp_date_t start, int h, int m)
{
    dsp_schedule_t s;
    memset(&s, 0, sizeof s);
    strcpy(s.id, id);
    s.revision = 1;
    s.enabled = true;
    s.amount_ul = 5000;
    s.start_date = start;
    s.hour = (uint8_t)h;
    s.minute = (uint8_t)m;
    s.kind = DSP_REC_DAILY;
    s.interval = 1;
    return s;
}

static dsp_occurrence_t occ_on(const dsp_schedule_t *s, dsp_date_t date)
{
    dsp_occurrence_t occ = {0, {0, 0, 0}};
    DSP_CHECK_EQ_INT(DSP_REC_OK, dsp_rec_occurrence_on(s, TZ, date, &occ));
    return occ;
}

static dsp_clock_t clk(dsp_utc_t t)
{
    dsp_clock_t c = {t, true, 0};
    return c;
}

static dsp_exec_status_t claim(const dsp_schedule_t *s, const dsp_occurrence_t *o,
                               dsp_utc_t now, dsp_exec_token_t *tok)
{
    return dsp_exec_claim(&ex, s, o, clk(now), tok);
}

/* Claim + finish COMPLETED; returns the claim status. */
static dsp_exec_status_t run_once(const dsp_schedule_t *s, const dsp_occurrence_t *o, dsp_utc_t now)
{
    dsp_exec_token_t tok;
    dsp_exec_status_t st = claim(s, o, now, &tok);
    if (st == DSP_EXEC_OK) {
        DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_finish(&ex, &tok, DSP_HISTORY_COMPLETED));
    }
    return st;
}

static int record_state(const char *id)
{
    dsp_history_record_t rec;
    dsp_exec_status_t st = dsp_exec_lookup(&ex, id, &rec);
    return st == DSP_EXEC_OK ? rec.state : -(int)st;
}

static dsp_schedule_t S;     /* daily 08:00 from 2026-11-02 */
static dsp_occurrence_t O20; /* its occurrence on 2026-11-20, 07:00Z */

static void setup_default(void)
{
    fresh(8);
    S = daily("schedule-001", D(2026, 11, 2), 8, 0);
    O20 = occ_on(&S, D(2026, 11, 20));
}

/* ---- 1. Normal automatic execution ----------------------------------------------- */

static void test_normal_execution(void)
{
    setup_default();
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_check(&ex, &S, &O20, clk(O20.at + 30)));
    DSP_CHECK_EQ_INT(0, store.writes); /* check is read-only */
    dsp_exec_token_t tok;
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, claim(&S, &O20, O20.at + 30, &tok));
    DSP_CHECK_EQ_INT(DSP_EXEC_KIND_AUTOMATIC, tok.kind);
    /* Claim is durable BEFORE authorization: write + commit happened, record STARTED. */
    DSP_CHECK_EQ_INT(1, store.writes);
    DSP_CHECK_EQ_INT(1, store.commits);
    DSP_CHECK_EQ_INT(DSP_HISTORY_STARTED, record_state("schedule-001"));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_finish(&ex, &tok, DSP_HISTORY_COMPLETED));
    DSP_CHECK_EQ_INT(2, store.writes);
    dsp_history_record_t rec;
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_lookup(&ex, "schedule-001", &rec));
    DSP_CHECK_EQ_INT(DSP_HISTORY_COMPLETED, rec.state);
    DSP_CHECK_EQ_INT(0, dsp_date_compare(D(2026, 11, 20), rec.local_date));
    DSP_CHECK_EQ_INT(O20.at, rec.scheduled_at);
    DSP_CHECK_EQ_INT(O20.at + 30, rec.claimed_at);
    DSP_CHECK_EQ_INT(1, rec.schedule_revision);
}

/* ---- 2. Same occurrence twice / duplicate requests --------------------------------- */

static void test_same_occurrence_twice(void)
{
    setup_default();
    dsp_exec_token_t tok;
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, claim(&S, &O20, O20.at, &tok));
    /* Duplicate request while the first is active: serialized, refused. */
    dsp_exec_token_t tok2;
    DSP_CHECK_EQ_INT(DSP_EXEC_BUSY, claim(&S, &O20, O20.at + 1, &tok2));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_finish(&ex, &tok, DSP_HISTORY_COMPLETED));
    int writes = store.writes;
    DSP_CHECK_EQ_INT(DSP_EXEC_ALREADY_CLAIMED, claim(&S, &O20, O20.at + 2, &tok2));
    DSP_CHECK_EQ_INT(DSP_EXEC_ALREADY_CLAIMED, dsp_exec_check(&ex, &S, &O20, clk(O20.at + 3)));
    DSP_CHECK_EQ_INT(writes, store.writes); /* refusals write nothing */
    /* Finishing twice / a stale token is rejected. */
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_ARG, dsp_exec_finish(&ex, &tok, DSP_HISTORY_COMPLETED));
}

/* ---- 3./4. Restart before / after an occurrence ------------------------------------ */

static void test_restart_before_occurrence(void)
{
    setup_default();
    dsp_exec_token_t tok;
    DSP_CHECK_EQ_INT(DSP_EXEC_NOT_DUE, claim(&S, &O20, O20.at - 60, &tok));
    boot();
    size_t n = 99;
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_recover(&ex, &n));
    DSP_CHECK_EQ_INT(0, n);
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&S, &O20, O20.at + 5));
}

static void test_restart_after_occurrence_is_missed(void)
{
    setup_default();
    boot(); /* device was off at 08:00 and wakes 10 min + 1 s later */
    dsp_exec_token_t tok;
    DSP_CHECK_EQ_INT(DSP_EXEC_MISSED, claim(&S, &O20, O20.at + DSP_EXEC_DUE_WINDOW_DEFAULT_S + 1, &tok));
    DSP_CHECK_EQ_INT(0, store.writes);
    DSP_CHECK_EQ_INT(-(int)DSP_EXEC_NOT_FOUND, record_state("schedule-001"));
    /* Next day's occurrence executes normally (no accumulation, no catch-up). */
    dsp_occurrence_t o21 = occ_on(&S, D(2026, 11, 21));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&S, &o21, o21.at + 10));
    /* A day later still, the 20th is long gone. */
    DSP_CHECK_EQ_INT(DSP_EXEC_MISSED, claim(&S, &O20, o21.at + 20, &tok));
}

/* ---- 5./6. Restart or power loss during execution (after the durable claim) -------- */

static void test_restart_during_execution(void)
{
    setup_default();
    dsp_exec_token_t tok;
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, claim(&S, &O20, O20.at + 10, &tok));
    boot(); /* reset while the pump runs: finish never called */
    size_t n = 0;
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_recover(&ex, &n));
    DSP_CHECK_EQ_INT(1, n);
    DSP_CHECK_EQ_INT(DSP_HISTORY_INTERRUPTED, record_state("schedule-001"));
    /* Still inside the due window: never resumed or repeated. */
    DSP_CHECK_EQ_INT(DSP_EXEC_ALREADY_CLAIMED, claim(&S, &O20, O20.at + 60, &tok));
    /* Recovery is idempotent. */
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_recover(&ex, &n));
    DSP_CHECK_EQ_INT(0, n);
    /* The old token is meaningless after the restart. */
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_ARG, dsp_exec_finish(&ex, &tok, DSP_HISTORY_COMPLETED));
    /* Next day executes normally. */
    dsp_occurrence_t o21 = occ_on(&S, D(2026, 11, 21));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&S, &o21, o21.at));
}

static void test_power_loss_right_after_durable_claim(void)
{
    setup_default();
    dsp_exec_token_t tok;
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, claim(&S, &O20, O20.at, &tok));
    /* Power lost before the motor started (the accepted crash window). No recovery run:
     * the STARTED record alone blocks. */
    boot();
    DSP_CHECK_EQ_INT(DSP_HISTORY_STARTED, record_state("schedule-001"));
    DSP_CHECK_EQ_INT(DSP_EXEC_ALREADY_CLAIMED, claim(&S, &O20, O20.at + 30, &tok));
}

/* ---- 7. Power loss before the claim became durable --------------------------------- */

static void test_power_loss_before_durable_claim(void)
{
    setup_default();
    dsp_exec_token_t tok;
    memset(&tok, 0, sizeof tok);
    store.power_loss_at_commit = true;
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_STORAGE_COMMIT, claim(&S, &O20, O20.at, &tok));
    DSP_CHECK_EQ_INT(DSP_EXEC_KIND_NONE, tok.kind); /* nothing authorized */
    fake_store_clear_faults(&store);
    boot();
    DSP_CHECK_EQ_INT(-(int)DSP_EXEC_NOT_FOUND, record_state("schedule-001"));
    /* The pump never ran, so executing now (still on time) is the first execution. */
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&S, &O20, O20.at + 120));
    DSP_CHECK_EQ_INT(DSP_EXEC_ALREADY_CLAIMED, run_once(&S, &O20, O20.at + 130));
}

/* ---- 8. Failed persistence --------------------------------------------------------- */

static void expect_not_authorized(dsp_exec_status_t expected, int expected_writes)
{
    dsp_exec_token_t tok;
    memset(&tok, 0, sizeof tok);
    DSP_CHECK_EQ_INT(expected, claim(&S, &O20, O20.at, &tok));
    DSP_CHECK_EQ_INT(DSP_EXEC_KIND_NONE, tok.kind);
    DSP_CHECK_EQ_INT(DSP_EXEC_KIND_NONE, ex.active.kind); /* coordinator not left busy */
    DSP_CHECK_EQ_INT(expected_writes, store.writes);     /* failure path really reached */
    fake_store_clear_faults(&store);
}

static void test_failed_persistence_never_authorizes(void)
{
    setup_default();
    store.fail_writes = true;
    expect_not_authorized(DSP_EXEC_ERR_STORAGE_WRITE, 0);
    DSP_CHECK_EQ_INT(-(int)DSP_EXEC_NOT_FOUND, record_state("schedule-001"));

    store.fail_commits = true;
    expect_not_authorized(DSP_EXEC_ERR_STORAGE_COMMIT, 1);
    DSP_CHECK_EQ_INT(-(int)DSP_EXEC_NOT_FOUND, record_state("schedule-001"));

    store.fail_reads = true;
    expect_not_authorized(DSP_EXEC_ERR_STORAGE_READ, 0); /* history unknown: no write */

    /* Read-back after commit differs: persistence not confirmed, no pump. */
    store.corrupt_next_readback = true;
    expect_not_authorized(DSP_EXEC_ERR_PERSIST_CONFIRM, 1);
    /* The record did become durable: the occurrence is now blocked (lost, not doubled). */
    dsp_exec_token_t tok;
    DSP_CHECK_EQ_INT(DSP_EXEC_ALREADY_CLAIMED, claim(&S, &O20, O20.at + 1, &tok));

    /* Commit reports failure although the data persisted: blocked as well. */
    setup_default();
    store.commit_fails_but_persists = true;
    expect_not_authorized(DSP_EXEC_ERR_STORAGE_COMMIT, 1);
    DSP_CHECK_EQ_INT(DSP_EXEC_ALREADY_CLAIMED, claim(&S, &O20, O20.at + 1, &tok));

    /* The read-back itself fails: 8 scan reads succeed, the 9th read (read-back) fails. */
    setup_default();
    store.fail_read_after = 9;
    expect_not_authorized(DSP_EXEC_ERR_PERSIST_CONFIRM, 1);
}

/* ---- 9. Completed / failed occurrences stay blocked -------------------------------- */

static void test_completed_and_failed_block_forever(void)
{
    setup_default();
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&S, &O20, O20.at));
    boot();
    size_t n = 99;
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_recover(&ex, &n));
    DSP_CHECK_EQ_INT(0, n); /* COMPLETED is not touched by recovery */
    dsp_exec_token_t tok;
    DSP_CHECK_EQ_INT(DSP_EXEC_ALREADY_CLAIMED, claim(&S, &O20, O20.at + 5, &tok));

    dsp_occurrence_t o21 = occ_on(&S, D(2026, 11, 21));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, claim(&S, &o21, o21.at, &tok));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_finish(&ex, &tok, DSP_HISTORY_FAILED));
    DSP_CHECK_EQ_INT(DSP_HISTORY_FAILED, record_state("schedule-001"));
    DSP_CHECK_EQ_INT(DSP_EXEC_ALREADY_CLAIMED, claim(&S, &o21, o21.at + 5, &tok)); /* no retry */
}

/* ---- 10. Edited schedule, unchanged identity --------------------------------------- */

static void test_edited_schedule_does_not_repeat(void)
{
    setup_default();
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&S, &O20, O20.at)); /* 08:00 rev 1 done */

    dsp_schedule_t edited = S; /* user moves it to 20:00 and changes the volume */
    edited.revision = 2;
    edited.hour = 20;
    edited.amount_ul = 9000;
    dsp_occurrence_t evening = occ_on(&edited, D(2026, 11, 20));
    DSP_CHECK(evening.at > O20.at);
    dsp_exec_token_t tok;
    DSP_CHECK_EQ_INT(DSP_EXEC_ALREADY_CLAIMED, claim(&edited, &evening, evening.at, &tok));
    /* The pre-edit occurrence is no longer an occurrence of the edited schedule. */
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_OCCURRENCE, claim(&edited, &O20, O20.at, &tok));
    /* Next day the edited schedule runs at its new time. */
    dsp_occurrence_t next = occ_on(&edited, D(2026, 11, 21));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&edited, &next, next.at));
    dsp_history_record_t rec;
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_lookup(&ex, "schedule-001", &rec));
    DSP_CHECK_EQ_INT(2, rec.schedule_revision);

    /* Edit to an EARLIER time after the occurrence ran: also blocked. */
    setup_default();
    dsp_schedule_t evening_s = S;
    evening_s.hour = 20;
    dsp_occurrence_t e20 = occ_on(&evening_s, D(2026, 11, 20));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&evening_s, &e20, e20.at));
    dsp_schedule_t late = evening_s;
    late.revision = 2;
    late.hour = 21;
    dsp_occurrence_t l20 = occ_on(&late, D(2026, 11, 20));
    DSP_CHECK_EQ_INT(DSP_EXEC_ALREADY_CLAIMED, claim(&late, &l20, l20.at, &tok));
}

/* ---- 11. Separate automatic occurrences with the same instant ---------------------- */

static void test_same_instant_different_schedules(void)
{
    setup_default();
    dsp_schedule_t a = daily("a", D(2026, 11, 2), 8, 0);
    dsp_schedule_t b = daily("b", D(2026, 11, 2), 8, 0);
    dsp_occurrence_t oa = occ_on(&a, D(2026, 11, 20));
    dsp_occurrence_t ob = occ_on(&b, D(2026, 11, 20));
    DSP_CHECK_EQ_INT(oa.at, ob.at);
    dsp_exec_token_t ta, tb;
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, claim(&a, &oa, oa.at, &ta));
    DSP_CHECK_EQ_INT(DSP_EXEC_BUSY, claim(&b, &ob, ob.at, &tb)); /* one pump: serialized */
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_finish(&ex, &ta, DSP_HISTORY_COMPLETED));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, claim(&b, &ob, ob.at + 40, &tb)); /* its own identity */
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_finish(&ex, &tb, DSP_HISTORY_COMPLETED));
    DSP_CHECK_EQ_INT(DSP_EXEC_ALREADY_CLAIMED, claim(&a, &oa, oa.at + 50, &ta));
    DSP_CHECK_EQ_INT(DSP_EXEC_ALREADY_CLAIMED, claim(&b, &ob, ob.at + 50, &tb));
}

/* ---- 12. Different local dates, midnight, clock moving backwards ------------------- */

static void test_different_local_dates_and_clock_rollback(void)
{
    setup_default();
    dsp_occurrence_t o21 = occ_on(&S, D(2026, 11, 21));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&S, &O20, O20.at));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&S, &o21, o21.at));
    DSP_CHECK_EQ_INT(4, store.writes); /* same slot updated in place */
    DSP_CHECK(store.durable[0].used && !store.durable[1].used);

    /* Clock corrected backwards to 20 Nov 08:05: the 20th is still blocked (watermark). */
    dsp_exec_token_t tok;
    DSP_CHECK_EQ_INT(DSP_EXEC_ALREADY_CLAIMED, claim(&S, &O20, O20.at + 300, &tok));

    /* Due window crossing local midnight: 23:55 occurrence executed at 00:03. */
    dsp_schedule_t late = daily("late", D(2026, 11, 2), 23, 55);
    dsp_occurrence_t ol = occ_on(&late, D(2026, 11, 20));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&late, &ol, ol.at + 8 * 60));
    dsp_history_record_t rec;
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_lookup(&ex, "late", &rec));
    DSP_CHECK_EQ_INT(0, dsp_date_compare(D(2026, 11, 20), rec.local_date));
    /* The 21st's 23:55 occurrence is still a separate, later date. */
    dsp_occurrence_t ol21 = occ_on(&late, D(2026, 11, 21));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&late, &ol21, ol21.at));
}

/* ---- 13. Invalid clock --------------------------------------------------------------- */

static void test_invalid_clock_blocks(void)
{
    setup_default();
    dsp_exec_token_t tok;
    dsp_clock_t invalid = {O20.at, false, 0};
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_CLOCK, dsp_exec_claim(&ex, &S, &O20, invalid, &tok));
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_CLOCK, dsp_exec_check(&ex, &S, &O20, invalid));
    const dsp_utc_t bad[] = {DSP_UTC_MIN - 1, DSP_UTC_MAX + 1, INT64_MIN, INT64_MAX};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        DSP_CHECK_EQ_INT(DSP_EXEC_ERR_CLOCK, dsp_exec_claim(&ex, &S, &O20, clk(bad[i]), &tok));
    }
    DSP_CHECK_EQ_INT(0, store.reads + store.writes); /* no storage touched */
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&S, &O20, O20.at)); /* once valid: normal */
}

/* ---- 14. Missed occurrence / due-window boundaries ---------------------------------- */

static void test_due_window_boundaries(void)
{
    setup_default();
    dsp_exec_token_t tok;
    DSP_CHECK_EQ_INT(DSP_EXEC_NOT_DUE, claim(&S, &O20, O20.at - 1, &tok));
    DSP_CHECK_EQ_INT(DSP_EXEC_MISSED, claim(&S, &O20, O20.at + 601, &tok));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_check(&ex, &S, &O20, clk(O20.at)));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_check(&ex, &S, &O20, clk(O20.at + 600)));
    DSP_CHECK_EQ_INT(0, store.writes);
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&S, &O20, O20.at + 600));

    /* Window 0: only the exact instant. */
    fake_store_init(&store, 4);
    config.due_window_s = 0;
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_init(&ex, &config));
    DSP_CHECK_EQ_INT(DSP_EXEC_MISSED, claim(&S, &O20, O20.at + 1, &tok));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&S, &O20, O20.at));

    /* DST fall-back: 02:30 runs at the earlier instant only; the repeated 02:30 one
     * hour later is not an occurrence and the real one is missed by then. */
    setup_default();
    dsp_schedule_t night = daily("night", D(2026, 10, 1), 2, 30);
    dsp_occurrence_t on = occ_on(&night, D(2026, 10, 25));
    dsp_occurrence_t repeated = on;
    repeated.at += 3600;
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_OCCURRENCE, claim(&night, &repeated, repeated.at, &tok));
    DSP_CHECK_EQ_INT(DSP_EXEC_MISSED, claim(&night, &on, on.at + 3600, &tok));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&night, &on, on.at + 60));
    DSP_CHECK_EQ_INT(DSP_EXEC_ALREADY_CLAIMED, claim(&night, &on, on.at + 120, &tok));
}

/* ---- 15. Manual dosing independence ------------------------------------------------- */

static void test_manual_dosing_is_independent(void)
{
    setup_default();
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&S, &O20, O20.at));
    fake_store_clear_faults(&store); /* reset counters */
    dsp_exec_token_t m;
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_manual_begin(&ex, &m));
    DSP_CHECK_EQ_INT(DSP_EXEC_KIND_MANUAL, m.kind);
    dsp_exec_token_t tok;
    DSP_CHECK_EQ_INT(DSP_EXEC_BUSY, claim(&S, &O20, O20.at + 5, &tok)); /* one pump */
    dsp_exec_token_t m2;
    DSP_CHECK_EQ_INT(DSP_EXEC_BUSY, dsp_exec_manual_begin(&ex, &m2));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_finish(&ex, &m, DSP_HISTORY_COMPLETED));
    DSP_CHECK_EQ_INT(0, store.reads + store.writes + store.commits); /* history untouched */
    /* Manual dose neither clears nor satisfies automatic history. */
    DSP_CHECK_EQ_INT(DSP_EXEC_ALREADY_CLAIMED, claim(&S, &O20, O20.at + 6, &tok));
    DSP_CHECK_EQ_INT(DSP_HISTORY_COMPLETED, record_state("schedule-001"));
    dsp_occurrence_t o21 = occ_on(&S, D(2026, 11, 21));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_manual_begin(&ex, &m));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_finish(&ex, &m, DSP_HISTORY_FAILED));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&S, &o21, o21.at)); /* automatic unaffected */
    /* Manual while an automatic execution is active is refused. */
    dsp_occurrence_t o22 = occ_on(&S, D(2026, 11, 22));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, claim(&S, &o22, o22.at, &tok));
    DSP_CHECK_EQ_INT(DSP_EXEC_BUSY, dsp_exec_manual_begin(&ex, &m));
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_ARG, dsp_exec_finish(&ex, &m, DSP_HISTORY_COMPLETED)); /* stale manual token */
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_finish(&ex, &tok, DSP_HISTORY_COMPLETED));
}

/* ---- Capacity and retention --------------------------------------------------------- */

static void test_capacity_and_retention(void)
{
    fresh(2);
    dsp_schedule_t a = daily("a", D(2026, 11, 2), 8, 0);
    dsp_schedule_t b = daily("b", D(2026, 11, 2), 9, 0);
    dsp_schedule_t c = daily("c", D(2026, 11, 2), 10, 0);
    dsp_occurrence_t oa = occ_on(&a, D(2026, 11, 10));
    dsp_occurrence_t ob = occ_on(&b, D(2026, 11, 11));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&a, &oa, oa.at));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&b, &ob, ob.at));
    /* Both slots hold records within retention: a third schedule cannot be recorded, so
     * it is not executed (fail-safe). */
    dsp_exec_token_t tok;
    dsp_occurrence_t oc = occ_on(&c, D(2026, 11, 12));
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_CAPACITY, claim(&c, &oc, oc.at, &tok));
    /* Existing schedules keep working (records updated in place). */
    dsp_occurrence_t oa2 = occ_on(&a, D(2026, 11, 12));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&a, &oa2, oa2.at));
    /* b's record (11 Nov) is prunable only when it is MORE than 7 days old: on 18 Nov
     * (exactly 7 days) not yet, on 19 Nov yes. */
    dsp_occurrence_t oc18 = occ_on(&c, D(2026, 11, 18));
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_CAPACITY, claim(&c, &oc18, oc18.at, &tok));
    dsp_occurrence_t oc19 = occ_on(&c, D(2026, 11, 19));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&c, &oc19, oc19.at));
    DSP_CHECK_EQ_INT(-(int)DSP_EXEC_NOT_FOUND, record_state("b")); /* reused slot */
    DSP_CHECK_EQ_INT(DSP_HISTORY_COMPLETED, record_state("a"));
    DSP_CHECK_EQ_INT(DSP_HISTORY_COMPLETED, record_state("c"));
    /* b's dates up to 11 Nov are long missed, so losing its record cannot cause a
     * duplicate: its old occurrence is refused by timing. */
    DSP_CHECK_EQ_INT(DSP_EXEC_MISSED, claim(&b, &ob, oc19.at, &tok));

    /* Oldest prunable record is chosen. */
    fresh(3);
    dsp_occurrence_t a1 = occ_on(&a, D(2026, 11, 3));
    dsp_occurrence_t b1 = occ_on(&b, D(2026, 11, 1 + 1));
    dsp_occurrence_t c1 = occ_on(&c, D(2026, 11, 4));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&a, &a1, a1.at));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&b, &b1, b1.at));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&c, &c1, c1.at));
    dsp_schedule_t d = daily("d", D(2026, 11, 2), 11, 0);
    dsp_occurrence_t d1 = occ_on(&d, D(2026, 11, 20));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&d, &d1, d1.at));
    DSP_CHECK_EQ_INT(-(int)DSP_EXEC_NOT_FOUND, record_state("b")); /* 2 Nov = oldest */
    DSP_CHECK_EQ_INT(DSP_HISTORY_COMPLETED, record_state("a"));
}

/* ---- Duplicate schedule IDs and corrupted history ---------------------------------- */

static void test_duplicate_ids_and_corruption(void)
{
    /* Two schedules wrongly sharing one ID share one watermark: the second is blocked on
     * the same date. This is why IDs must be unique and never reused. */
    setup_default();
    dsp_schedule_t twin = daily("schedule-001", D(2026, 11, 2), 18, 0);
    dsp_occurrence_t t20 = occ_on(&twin, D(2026, 11, 20));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&S, &O20, O20.at));
    dsp_exec_token_t tok;
    DSP_CHECK_EQ_INT(DSP_EXEC_ALREADY_CLAIMED, claim(&twin, &t20, t20.at, &tok));

    /* Two records with the same schedule ID in storage: inconsistent, no dosing. */
    store.durable[3] = store.durable[0];
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_INTERNAL, claim(&S, &O20, O20.at, &tok));
    dsp_occurrence_t o21 = occ_on(&S, D(2026, 11, 21));
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_INTERNAL, claim(&S, &o21, o21.at, &tok));
    dsp_history_record_t rec;
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_INTERNAL, dsp_exec_lookup(&ex, "schedule-001", &rec));

    /* A corrupted record (unknown state / bad id / bad date) blocks every schedule. */
    const int kinds = 3;
    for (int k = 0; k < kinds; k++) {
        setup_default();
        DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&S, &O20, O20.at));
        if (k == 0) {
            store.durable[0].rec.state = 9;
        } else if (k == 1) {
            store.durable[0].rec.schedule_id[0] = '@';
        } else {
            store.durable[0].rec.local_date.month = 13;
        }
        dsp_schedule_t other = daily("other", D(2026, 11, 2), 8, 0);
        dsp_occurrence_t oo = occ_on(&other, D(2026, 11, 21));
        DSP_CHECK_EQ_INT(DSP_EXEC_ERR_INTERNAL, claim(&other, &oo, oo.at, &tok));
        size_t n = 0;
        DSP_CHECK_EQ_INT(DSP_EXEC_ERR_INTERNAL, dsp_exec_recover(&ex, &n));
    }
}

/* ---- Invalid input ------------------------------------------------------------------ */

static void test_invalid_input(void)
{
    setup_default();
    dsp_exec_token_t tok;
    dsp_history_record_t rec;
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_ARG, dsp_exec_init(NULL, &config));
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_ARG, dsp_exec_claim(&ex, NULL, &O20, clk(O20.at), &tok));
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_ARG, dsp_exec_claim(&ex, &S, NULL, clk(O20.at), &tok));
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_ARG, dsp_exec_claim(&ex, &S, &O20, clk(O20.at), NULL));
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_ARG, dsp_exec_claim(NULL, &S, &O20, clk(O20.at), &tok));
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_ARG, dsp_exec_check(NULL, &S, &O20, clk(O20.at)));
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_ARG, dsp_exec_finish(&ex, NULL, DSP_HISTORY_COMPLETED));
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_ARG, dsp_exec_manual_begin(&ex, NULL));
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_ARG, dsp_exec_manual_begin(NULL, &tok));
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_ARG, dsp_exec_recover(NULL, NULL));
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_ARG, dsp_exec_lookup(&ex, "bad id", &rec));
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_ARG, dsp_exec_lookup(&ex, "ok", NULL));

    /* Invalid configurations. */
    dsp_exec_t e2;
    dsp_exec_config_t c = config;
    c.due_window_s = -1;
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_ARG, dsp_exec_init(&e2, &c));
    c.due_window_s = DSP_EXEC_DUE_WINDOW_MAX_S + 1;
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_ARG, dsp_exec_init(&e2, &c));
    c = config;
    c.retention_days = 0;
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_ARG, dsp_exec_init(&e2, &c));
    c.retention_days = DSP_EXEC_RETENTION_DAYS_MAX + 1;
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_ARG, dsp_exec_init(&e2, &c));
    c = config;
    c.tz = NULL;
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_ARG, dsp_exec_init(&e2, &c));
    c = config;
    c.store = NULL;
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_ARG, dsp_exec_init(&e2, &c));
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_ARG, dsp_exec_init(&e2, NULL));
    /* A failed init leaves the coordinator unusable, never half-configured. */
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_ARG, dsp_exec_claim(&e2, &S, &O20, clk(O20.at), &tok));
    dsp_history_store_t bad_store = store.iface;
    bad_store.slot_count = 0;
    c = config;
    c.store = &bad_store;
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_ARG, dsp_exec_init(&e2, &c));
    bad_store.slot_count = DSP_HISTORY_MAX_SLOTS + 1;
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_ARG, dsp_exec_init(&e2, &c));
    bad_store = store.iface;
    bad_store.commit = NULL;
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_ARG, dsp_exec_init(&e2, &c));

    /* Invalid schedule, disabled schedule, invalid occurrence identity. */
    dsp_schedule_t bad = S;
    bad.interval = 0;
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_SCHEDULE, claim(&bad, &O20, O20.at, &tok));
    dsp_schedule_t off = S;
    off.enabled = false;
    DSP_CHECK_EQ_INT(DSP_EXEC_DISABLED, claim(&off, &O20, O20.at, &tok));
    dsp_occurrence_t forged = O20;
    forged.at += 60; /* not the schedule's instant */
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_OCCURRENCE, claim(&S, &forged, forged.at, &tok));
    forged = O20;
    forged.local_date = D(2026, 2, 30); /* not a date */
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_OCCURRENCE, claim(&S, &forged, O20.at, &tok));
    dsp_schedule_t every3 = S;
    every3.interval = 3; /* 2, 5, ... 20 Nov is a match, 21 Nov is not */
    dsp_occurrence_t o21 = occ_on(&S, D(2026, 11, 21));
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_OCCURRENCE, claim(&every3, &o21, o21.at, &tok));
    forged = O20;
    forged.local_date = D(2026, 11, 1); /* before the start date */
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_OCCURRENCE, claim(&S, &forged, O20.at, &tok));
    /* Spring-forward gap day: no occurrence exists, a fabricated one is refused. */
    dsp_schedule_t gap = daily("gap", D(2027, 3, 1), 2, 30);
    dsp_occurrence_t g = {(dsp_utc_t)1806197400 /* 2027-03-28T01:30Z */, {2027, 3, 28}};
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_OCCURRENCE, claim(&gap, &g, g.at, &tok));
    /* Invalid finish outcome. */
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, claim(&S, &O20, O20.at, &tok));
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_ARG, dsp_exec_finish(&ex, &tok, DSP_HISTORY_STARTED));
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_ARG, dsp_exec_finish(&ex, &tok, DSP_HISTORY_INTERRUPTED));
    dsp_exec_token_t forged_tok = tok;
    forged_tok.record.local_date.day = 21;
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_ARG, dsp_exec_finish(&ex, &forged_tok, DSP_HISTORY_COMPLETED));
    size_t n;
    DSP_CHECK_EQ_INT(DSP_EXEC_BUSY, dsp_exec_recover(&ex, &n)); /* never during an execution */
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_finish(&ex, &tok, DSP_HISTORY_COMPLETED));
}

/* ---- Finish and recovery failure paths ---------------------------------------------- */

static void test_finish_and_recover_failures(void)
{
    /* Storing COMPLETED fails: the record stays STARTED (still blocks), coordinator freed. */
    setup_default();
    dsp_exec_token_t tok;
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, claim(&S, &O20, O20.at, &tok));
    store.fail_writes = true;
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_STORAGE_WRITE, dsp_exec_finish(&ex, &tok, DSP_HISTORY_COMPLETED));
    fake_store_clear_faults(&store);
    DSP_CHECK_EQ_INT(DSP_HISTORY_STARTED, record_state("schedule-001"));
    DSP_CHECK_EQ_INT(DSP_EXEC_ALREADY_CLAIMED, claim(&S, &O20, O20.at + 5, &tok));
    dsp_exec_token_t m;
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_manual_begin(&ex, &m)); /* not stuck busy */
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_finish(&ex, &m, DSP_HISTORY_COMPLETED));

    /* Read failure while finishing. */
    setup_default();
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, claim(&S, &O20, O20.at, &tok));
    store.fail_reads = true;
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_STORAGE_READ, dsp_exec_finish(&ex, &tok, DSP_HISTORY_COMPLETED));
    fake_store_clear_faults(&store);

    /* The slot changed underneath the active execution: never overwritten. */
    setup_default();
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, claim(&S, &O20, O20.at, &tok));
    store.durable[0].rec.claimed_at += 7;
    int writes = store.writes;
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_INTERNAL, dsp_exec_finish(&ex, &tok, DSP_HISTORY_COMPLETED));
    DSP_CHECK_EQ_INT(writes, store.writes);
    store.durable[0].used = false; /* slot emptied underneath */
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, claim(&S, &O20, O20.at + 1, &tok));
    store.durable[0].used = false;
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_INTERNAL, dsp_exec_finish(&ex, &tok, DSP_HISTORY_COMPLETED));

    /* Recovery: read and write failures leave records STARTED (blocking); retry works. */
    setup_default();
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, claim(&S, &O20, O20.at, &tok));
    boot();
    size_t n = 7;
    store.fail_reads = true;
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_STORAGE_READ, dsp_exec_recover(&ex, &n));
    DSP_CHECK_EQ_INT(0, n);
    fake_store_clear_faults(&store);
    store.fail_commits = true;
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_STORAGE_COMMIT, dsp_exec_recover(&ex, &n));
    fake_store_clear_faults(&store);
    DSP_CHECK_EQ_INT(DSP_HISTORY_STARTED, record_state("schedule-001"));
    DSP_CHECK_EQ_INT(DSP_EXEC_ALREADY_CLAIMED, claim(&S, &O20, O20.at + 9, &tok));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_recover(&ex, &n));
    DSP_CHECK_EQ_INT(1, n);
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_recover(&ex, NULL)); /* optional output */
}

/* ---- Randomized long run: never more than one authorization per occurrence ---------- */

static uint32_t rng = 12345u;
static uint32_t rnd(uint32_t bound)
{
    rng = rng * 1664525u + 1013904223u;
    return (rng >> 8) % bound;
}

#define MAX_AUTH 512
static char auth_ids[MAX_AUTH][DSP_OCCURRENCE_ID_BUF_LEN];
static int n_auth;

static int auth_count(const char *id)
{
    int n = 0;
    for (int i = 0; i < n_auth; i++) {
        n += strcmp(auth_ids[i], id) == 0;
    }
    return n;
}

static void test_randomized_no_duplicate_authorization(void)
{
    fresh(8);
    n_auth = 0;
    dsp_schedule_t sched[3] = {
        daily("a", D(2026, 10, 1), 8, 0),
        daily("b", D(2026, 10, 1), 2, 30), /* crosses the 25 Oct overlap */
        daily("c", D(2026, 10, 1), 23, 58), /* window crosses midnight */
    };
    sched[1].interval = 2;
    sched[2].kind = DSP_REC_WEEKLY;
    sched[2].weekday_mask = DSP_WEEKDAY_BIT(DSP_MONDAY) | DSP_WEEKDAY_BIT(DSP_THURSDAY);
    int authorized_late = 0;
    int duplicates = 0;
    int faults = 0;
    int restarts = 0;
    int32_t first = dsp_date_to_days(D(2026, 10, 1));
    for (int32_t day = first; day < first + 60; day++) {
        dsp_date_t date;
        DSP_CHECK(dsp_date_from_days(day, &date));
        for (int step = 0; step < 4; step++) {
            int k = step;
            if (step == 3) { /* mid-day edit of `a` (08:00 -> 20:00, new revision) */
                if (day != first + 20) {
                    continue;
                }
                k = 0;
                sched[0].hour = 20;
                sched[0].revision = 2;
            }
            dsp_occurrence_t occ;
            if (dsp_rec_occurrence_on(&sched[k], TZ, date, &occ) != DSP_REC_OK) {
                continue;
            }
            char id[DSP_OCCURRENCE_ID_BUF_LEN];
            DSP_CHECK_EQ_INT(DSP_REC_OK, dsp_occurrence_id_format(sched[k].id, date, id, sizeof id));
            for (int attempt = 0; attempt < 6; attempt++) {
                dsp_utc_t now = occ.at - 300 + (dsp_utc_t)rnd(1300); /* -5 .. +16 min */
                switch (rnd(10)) {
                case 0: store.fail_writes = true; faults++; break;
                case 1: store.power_loss_at_commit = true; faults++; break;
                case 2: store.commit_fails_but_persists = true; faults++; break;
                case 3: store.corrupt_next_readback = true; faults++; break;
                default: break;
                }
                dsp_exec_token_t tok;
                dsp_exec_status_t st = claim(&sched[k], &occ, now, &tok);
                fake_store_clear_faults(&store);
                if (st == DSP_EXEC_OK) {
                    if (auth_count(id) > 0) {
                        duplicates++;
                    }
                    if (now < occ.at || now - occ.at > DSP_EXEC_DUE_WINDOW_DEFAULT_S) {
                        authorized_late++;
                    }
                    if (n_auth < MAX_AUTH) {
                        strcpy(auth_ids[n_auth++], id);
                    }
                    uint32_t r = rnd(10);
                    if (r < 7) {
                        DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_finish(&ex, &tok, DSP_HISTORY_COMPLETED));
                    } else if (r < 8) {
                        DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_finish(&ex, &tok, DSP_HISTORY_FAILED));
                    } else {
                        boot(); /* reset during the pump run */
                        restarts++;
                        if (rnd(2) == 0) {
                            DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_recover(&ex, NULL));
                        }
                    }
                } else if (rnd(8) == 0) {
                    boot();
                    restarts++;
                }
            }
        }
    }
    DSP_CHECK_EQ_INT(0, duplicates);
    DSP_CHECK_EQ_INT(0, authorized_late);
    DSP_CHECK(n_auth > 60);  /* the scenario really executed doses ... */
    DSP_CHECK(faults > 50);  /* ... under many injected faults ... */
    DSP_CHECK(restarts > 10); /* ... and restarts */
}

static void test_fault_free_executes_every_occurrence_once(void)
{
    fresh(8);
    dsp_schedule_t a = daily("a", D(2026, 10, 1), 8, 0);
    dsp_schedule_t b = daily("b", D(2026, 10, 1), 8, 0); /* same instant as a */
    dsp_schedule_t c = daily("c", D(2026, 10, 1), 2, 30);
    int expected = 0;
    int executed = 0;
    int32_t first = dsp_date_to_days(D(2026, 10, 1));
    for (int32_t day = first; day < first + 200; day++) { /* incl. both DST changes */
        dsp_date_t date;
        DSP_CHECK(dsp_date_from_days(day, &date));
        dsp_schedule_t *all[3] = {&a, &b, &c};
        for (int k = 0; k < 3; k++) {
            dsp_occurrence_t occ;
            if (dsp_rec_occurrence_on(all[k], TZ, date, &occ) != DSP_REC_OK) {
                continue; /* 2027-03-28 02:30 does not exist */
            }
            expected++;
            executed += run_once(all[k], &occ, occ.at + 45) == DSP_EXEC_OK;
            executed += run_once(all[k], &occ, occ.at + 90) == DSP_EXEC_OK; /* duplicate wake */
        }
    }
    DSP_CHECK_EQ_INT(expected, executed);
    DSP_CHECK_EQ_INT(599, expected); /* 200 + 200 + 199 (spring gap skipped) */
}

/* ---- Review item 1: duplicates of ANY schedule ID are detected globally ------------ */

static void test_global_duplicate_detection(void)
{
    /* History holds two records of schedule "x" (not the schedule being claimed). */
    setup_default();
    dsp_schedule_t x = daily("x", D(2026, 11, 2), 6, 0);
    dsp_schedule_t y = daily("y", D(2026, 11, 2), 7, 0);
    dsp_occurrence_t ox = occ_on(&x, D(2026, 11, 19));
    dsp_occurrence_t oy = occ_on(&y, D(2026, 11, 19));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&x, &ox, ox.at));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&y, &oy, oy.at));
    store.durable[5] = store.durable[0]; /* identical duplicate of "x" */
    store.durable[5].rec.local_date = D(2026, 11, 18); /* and with a different date */
    fake_store_clear_faults(&store);
    dsp_exec_token_t tok;
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_INTERNAL, claim(&S, &O20, O20.at, &tok));
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_INTERNAL, dsp_exec_check(&ex, &S, &O20, clk(O20.at)));
    dsp_history_record_t rec;
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_INTERNAL, dsp_exec_lookup(&ex, "schedule-001", &rec));
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_INTERNAL, dsp_exec_lookup(&ex, "y", &rec));
    size_t n = 0;
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_INTERNAL, dsp_exec_recover(&ex, &n));
    dsp_history_scan_t scan;
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_INTERNAL, dsp_history_scan(&store.iface, NULL, INT32_MIN, &scan));
    DSP_CHECK_EQ_INT(0, store.writes + store.commits); /* nothing modified */

    /* Recovery validates before converting: a STARTED record next to a duplicate pair is
     * left untouched. */
    setup_default();
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, claim(&S, &O20, O20.at, &tok));
    boot();
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&x, &ox, ox.at));
    store.durable[6] = store.durable[1];
    fake_store_clear_faults(&store);
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_INTERNAL, dsp_exec_recover(&ex, &n));
    DSP_CHECK_EQ_INT(0, store.writes);
    DSP_CHECK_EQ_INT(DSP_HISTORY_STARTED, store.durable[0].rec.state);

    /* Different IDs with the same 32-bit FNV-1a hash are NOT duplicates: the collision
     * branch re-reads both slots and compares exactly. */
    setup_default();
    dsp_schedule_t c1 = daily("id-7e5ff", D(2026, 11, 2), 6, 0);
    dsp_schedule_t c2 = daily("id-a3e80", D(2026, 11, 2), 7, 0);
    dsp_occurrence_t o1 = occ_on(&c1, D(2026, 11, 19));
    dsp_occurrence_t o2 = occ_on(&c2, D(2026, 11, 19));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&c1, &o1, o1.at));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&c2, &o2, o2.at));
    fake_store_clear_faults(&store);
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, claim(&S, &O20, O20.at, &tok));
    DSP_CHECK_EQ_INT(8 + 2 + 1, store.reads); /* scan + 2 collision re-reads + read-back */
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_finish(&ex, &tok, DSP_HISTORY_COMPLETED));
    /* A read failure during the collision re-read: refused, nothing written. */
    fake_store_clear_faults(&store);
    dsp_occurrence_t o21 = occ_on(&S, D(2026, 11, 21));
    store.fail_read_after = 9; /* 8 scan reads succeed, the first re-read fails */
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_STORAGE_READ, claim(&S, &o21, o21.at, &tok));
    DSP_CHECK_EQ_INT(0, store.writes);
    fake_store_clear_faults(&store);
    /* Same colliding IDs, one of them duplicated: still detected. */
    store.durable[7] = store.durable[1];
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_INTERNAL, claim(&S, &o21, o21.at, &tok));
}

/* ---- Review item 2: backward clock jump after slot recycling (KNOWN LIMITATION) ---- */

static void test_limitation_backward_clock_after_recycling(void)
{
    dsp_schedule_t a = daily("a", D(2026, 11, 2), 8, 0);
    dsp_schedule_t b = daily("b", D(2026, 11, 2), 9, 0);
    dsp_schedule_t c = daily("c", D(2026, 11, 2), 10, 0);
    dsp_exec_token_t tok;

    /* Without recycling, a valid clock moved back any distance is still blocked. */
    fresh(8);
    dsp_occurrence_t a1 = occ_on(&a, D(2026, 11, 1 + 1));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&a, &a1, a1.at));
    dsp_occurrence_t c30 = occ_on(&c, D(2026, 11, 30));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&c, &c30, c30.at)); /* 4 weeks later, free slot */
    DSP_CHECK_EQ_INT(DSP_EXEC_ALREADY_CLAIMED, claim(&a, &a1, a1.at + 60, &tok)); /* back 28 days */

    /* Within retention the record cannot be recycled: back 7 days is blocked. */
    fresh(2);
    dsp_occurrence_t b11 = occ_on(&b, D(2026, 11, 11));
    dsp_occurrence_t a12 = occ_on(&a, D(2026, 11, 12));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&b, &b11, b11.at));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&a, &a12, a12.at));
    /* 18 Nov: b (11 Nov) is exactly 7 days old, a (12 Nov) 6 days: nothing recyclable. */
    dsp_occurrence_t c18 = occ_on(&c, D(2026, 11, 18));
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_CAPACITY, claim(&c, &c18, c18.at, &tok));
    DSP_CHECK_EQ_INT(DSP_EXEC_ALREADY_CLAIMED, claim(&b, &b11, b11.at + 60, &tok));

    /* 19 Nov: the slot of b (11 Nov, now more than 7 days old) is recycled for c. */
    dsp_occurrence_t c19 = occ_on(&c, D(2026, 11, 19));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&c, &c19, c19.at));
    DSP_CHECK_EQ_INT(-(int)DSP_EXEC_NOT_FOUND, record_state("b"));

    /* The VALID clock is then set back to 11 Nov 09:01 (e.g. a wrong time source).
     * Observed M1.2 behavior: refused with ERR_CAPACITY and storage untouched, because
     * recycling only happens when the history is full, always takes the oldest record and
     * M1.2 never empties a slot, so no slot is recyclable relative to the rolled-back
     * date. This is an EMERGENT property, not a designed guarantee. */
    fake_store_t before = store;
    fake_store_clear_faults(&store);
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_CAPACITY, claim(&b, &b11, b11.at + 60, &tok));
    DSP_CHECK_EQ_INT(0, store.writes + store.commits);
    DSP_CHECK(memcmp(before.durable, store.durable, sizeof store.durable) == 0);

    /* LIMITATION: if a slot were free (M1.2 never frees one; a future change such as
     * erasing records of deleted schedules could), b's recycled 11 Nov occurrence is
     * no longer known and would be authorized a SECOND time. Deduplication after
     * recycling therefore still relies on the stated assumption that the valid clock
     * never moves back more than 7 days; enforcing it is the future clock service's job. */
    DSP_CHECK_EQ_INT(0, strcmp(store.durable[0].rec.schedule_id, "c"));
    store.durable[0].used = false; /* simulate a freed slot (c's record) */
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, claim(&b, &b11, b11.at + 60, &tok)); /* duplicate! */
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_finish(&ex, &tok, DSP_HISTORY_COMPLETED));
    /* Schedules whose records survived stay protected even then. */
    DSP_CHECK_EQ_INT(DSP_EXEC_ALREADY_CLAIMED, claim(&a, &a12, a12.at + 60, &tok));
}

/* ---- Review item 3: all 32 default slots occupied, nothing recyclable --------------- */

static void test_capacity_full_default_slots(void)
{
    fresh(DSP_HISTORY_DEFAULT_SLOTS);
    dsp_schedule_t sch[DSP_HISTORY_DEFAULT_SLOTS];
    for (int i = 0; i < DSP_HISTORY_DEFAULT_SLOTS; i++) {
        char id[8];
        snprintf(id, sizeof id, "s%02d", i);
        sch[i] = daily(id, D(2026, 11, 2), 6 + i / 4, (i % 4) * 15);
        dsp_occurrence_t o = occ_on(&sch[i], D(2026, 11, 20));
        DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&sch[i], &o, o.at));
    }
    for (size_t i = 0; i < DSP_HISTORY_DEFAULT_SLOTS; i++) {
        DSP_CHECK(store.durable[i].used);
    }
    /* New schedule, same day and the following days within retention: refused, and the
     * storage is not modified at all. */
    dsp_schedule_t fresh_s = daily("new", D(2026, 11, 2), 22, 0);
    fake_store_t before = store;
    fake_store_clear_faults(&store);
    for (int day = 20; day <= 27; day++) {
        dsp_occurrence_t o = occ_on(&fresh_s, D(2026, 11, day));
        dsp_exec_token_t tok;
        memset(&tok, 0, sizeof tok);
        DSP_CHECK_EQ_INT(DSP_EXEC_ERR_CAPACITY, dsp_exec_check(&ex, &fresh_s, &o, clk(o.at)));
        DSP_CHECK_EQ_INT(DSP_EXEC_ERR_CAPACITY, claim(&fresh_s, &o, o.at, &tok));
        DSP_CHECK_EQ_INT(DSP_EXEC_KIND_NONE, tok.kind);
    }
    DSP_CHECK_EQ_INT(0, store.writes + store.commits);
    DSP_CHECK(memcmp(before.durable, store.durable, sizeof store.durable) == 0);
    DSP_CHECK_EQ_INT(DSP_EXEC_KIND_NONE, ex.active.kind);

    /* Every existing schedule keeps working (record updated in place, no new slot). */
    for (int i = 0; i < DSP_HISTORY_DEFAULT_SLOTS; i++) {
        dsp_occurrence_t o = occ_on(&sch[i], D(2026, 11, 21));
        DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&sch[i], &o, o.at));
        dsp_occurrence_t again = o;
        dsp_exec_token_t tok;
        DSP_CHECK_EQ_INT(DSP_EXEC_ALREADY_CLAIMED, claim(&sch[i], &again, o.at + 1, &tok));
    }
    /* Once a record is more than 7 days old it can be recycled for the new schedule. */
    dsp_occurrence_t o29 = occ_on(&fresh_s, D(2026, 11, 29));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, run_once(&fresh_s, &o29, o29.at));
    int lost = 0;
    for (int i = 0; i < DSP_HISTORY_DEFAULT_SLOTS; i++) {
        lost += record_state(sch[i].id) < 0;
    }
    DSP_CHECK_EQ_INT(1, lost); /* exactly one old record (21 Nov) recycled */
}

/* ---- M1.10b.1: per-schedule edit floor --------------------------------------------- */

static void test_schedule_edit_floor(void)
{
    setup_default();
    S.not_before = O20.at + 1; /* edited one second after the scheduled instant */
    DSP_CHECK_EQ_INT(DSP_EXEC_MISSED, dsp_exec_check(&ex, &S, &O20, clk(O20.at + 30)));
    dsp_exec_token_t tok;
    DSP_CHECK_EQ_INT(DSP_EXEC_MISSED, claim(&S, &O20, O20.at + 30, &tok));
    DSP_CHECK_EQ_INT(0, store.writes); /* nothing claimed */
    S.not_before = O20.at; /* exactly at the instant: allowed */
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_check(&ex, &S, &O20, clk(O20.at + 30)));
    /* The next day's occurrence is after the floor. */
    S.not_before = O20.at + 1;
    dsp_occurrence_t o21 = occ_on(&S, D(2026, 11, 21));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_check(&ex, &S, &o21, clk(o21.at + 30)));
    /* An invalid floor makes the schedule invalid (never silently ignored). */
    S.not_before = 5;
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_SCHEDULE, dsp_exec_check(&ex, &S, &o21, clk(o21.at + 30)));
}

int main(void)
{
    DSP_RUN(test_schedule_edit_floor);
    DSP_RUN(test_normal_execution);
    DSP_RUN(test_same_occurrence_twice);
    DSP_RUN(test_restart_before_occurrence);
    DSP_RUN(test_restart_after_occurrence_is_missed);
    DSP_RUN(test_restart_during_execution);
    DSP_RUN(test_power_loss_right_after_durable_claim);
    DSP_RUN(test_power_loss_before_durable_claim);
    DSP_RUN(test_failed_persistence_never_authorizes);
    DSP_RUN(test_completed_and_failed_block_forever);
    DSP_RUN(test_edited_schedule_does_not_repeat);
    DSP_RUN(test_same_instant_different_schedules);
    DSP_RUN(test_different_local_dates_and_clock_rollback);
    DSP_RUN(test_invalid_clock_blocks);
    DSP_RUN(test_due_window_boundaries);
    DSP_RUN(test_manual_dosing_is_independent);
    DSP_RUN(test_capacity_and_retention);
    DSP_RUN(test_duplicate_ids_and_corruption);
    DSP_RUN(test_invalid_input);
    DSP_RUN(test_finish_and_recover_failures);
    DSP_RUN(test_randomized_no_duplicate_authorization);
    DSP_RUN(test_fault_free_executes_every_occurrence_once);
    DSP_RUN(test_global_duplicate_detection);
    DSP_RUN(test_limitation_backward_clock_after_recycling);
    DSP_RUN(test_capacity_full_default_slots);
    return dsp_test_finish();
}
