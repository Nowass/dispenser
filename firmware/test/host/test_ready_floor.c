/*
 * Incident 2026-11-20 (F1): entering READY must not release occurrences that fell due while the
 * device was NOT READY. On the bench, "Nuli" (8 ml, 17:17) and "Bar" (5 ml, 17:20) ran at 17:20:22,
 * right after the owner's READY: both were still inside their 10-minute due window. The READY
 * transition now stores the durable dosing floor (time.not_before_utc = transition + 1 s) BEFORE
 * the readiness itself. Shared harness of test_app_ops.c (complete wakes, power cuts, fake pump).
 * Times here are 08:xx local (T0 = 08:00); the incident's 17:xx maps 1:1.
 */
#define DSP_APP_OPS_NO_MAIN
#include "test_app_ops.c"

static dsp_schedule_t once_at(const char *id, int h, int m, uint32_t vol)
{
    dsp_schedule_t s = sched(id, h, m, vol);
    s.kind = DSP_REC_ONCE;
    s.start_date = date_of(T0);
    return s;
}
static op_t op_not_ready(void)
{
    op_t o = mk(OP_READINESS);
    o.readiness = DSP_READINESS_SERVICE_REQUIRED;
    return o;
}
static void reboot_and_trust(void)
{
    reboot_power_on();
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    clear_script();
    step(g_true, op_now()); /* trusted time again (phone) */
    DSP_CHECK(session());
}

/* The bench incident, reproduced: NOT READY through both due times, READY at 08:20:22. */
static void test_incident_ready_after_due_times(void)
{
    provision(false);
    clear_script();
    step(g_true, op_create(once_at("nuli", 8, 17, 8000)));
    step(g_true, op_create(once_at("bar", 8, 20, 5000)));
    step(g_true, op_create(once_at("later", 8, 45, 1000)));
    dsp_schedule_t daily = sched("daily", 8, 18, 1000); /* also due 08:18 while NOT READY */
    step(g_true, op_create(daily));
    DSP_CHECK(session());
    run_until(T0 + 20 * MINUTE + 22);
    DSP_CHECK_EQ_INT(0, pump.calls); /* NOT READY: nothing */
    advance(T0 + 20 * MINUTE + 22);
    clear_script();
    int r = step(g_true, op_ready());
    step(g_true + 60, op_list(0)); /* the session stays open while the old occurrences are due */
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[r].r.status);
    DSP_CHECK_EQ_INT(DSP_READINESS_READY, durable_readiness());
    DSP_CHECK_EQ_INT(T0 + 20 * MINUTE + 22 + 1, durable_time().not_before_utc); /* durable floor */
    DSP_CHECK_EQ_INT(0, pump.calls); /* Nuli, Bar and today's daily never run */
    run_until(T0 + HOUR);
    char id[96];
    occ_id(id, sizeof id, "later", T0);
    DSP_CHECK_EQ_INT(1, runs_of(id)); /* a later occurrence the same day runs */
    DSP_CHECK_EQ_INT(1, pump.calls);
    run_until(T0 + DAY + HOUR);
    occ_id(id, sizeof id, "daily", T0 + DAY);
    DSP_CHECK_EQ_INT(1, runs_of(id)); /* the next day's recurring occurrence runs */
    occ_id(id, sizeof id, "daily", T0);
    DSP_CHECK_EQ_INT(0, runs_of(id));
    DSP_CHECK_EQ_INT(2, pump.calls);
    invariants();
}

/* Exact boundary: an occurrence at the transition second (same minute) is skipped; one second
 * before the occurrence, READY lets it run at its time. */
static void test_same_minute_boundary(void)
{
    provision(false);
    clear_script();
    step(g_true, op_create(once_at("at", 8, 20, 1000)));
    step(g_true, op_create(once_at("next", 8, 21, 1000)));
    DSP_CHECK(session());
    advance(T0 + 20 * MINUTE); /* READY exactly at 08:20:00 */
    clear_script();
    step(g_true, op_ready());
    DSP_CHECK(session());
    run_until(T0 + 30 * MINUTE);
    char id[96];
    occ_id(id, sizeof id, "at", T0);
    DSP_CHECK_EQ_INT(0, runs_of(id));
    occ_id(id, sizeof id, "next", T0);
    DSP_CHECK_EQ_INT(1, runs_of(id));

    provision(false);
    clear_script();
    step(g_true, op_create(once_at("at", 8, 20, 1000)));
    DSP_CHECK(session());
    advance(T0 + 20 * MINUTE - 1); /* READY at 08:19:59 */
    clear_script();
    step(g_true, op_ready());
    DSP_CHECK(session());
    run_until(T0 + 30 * MINUTE);
    occ_id(id, sizeof id, "at", T0);
    DSP_CHECK_EQ_INT(1, runs_of(id));
    DSP_CHECK_EQ_INT(T0 + 20 * MINUTE, pump.at[pump.calls - 1]);
    invariants();
}

/* The floor write fails: READY is not entered, nothing runs; a later attempt succeeds. */
static void test_floor_write_failure_keeps_not_ready(void)
{
    provision(false);
    clear_script();
    step(g_true, op_create(once_at("old", 8, 17, 1000)));
    DSP_CHECK(session());
    advance(T0 + 20 * MINUTE);
    snprintf(fail_set_key, sizeof fail_set_key, "time");
    clear_script();
    int r = step(g_true, op_ready());
    DSP_CHECK(session());
    fail_set_key[0] = '\0';
    DSP_CHECK_EQ_INT(DSP_OP_STORAGE, sc.res[r].r.status);
    DSP_CHECK_EQ_INT(DSP_READINESS_SERVICE_REQUIRED, durable_readiness());
    run_until(T0 + 25 * MINUTE);
    DSP_CHECK_EQ_INT(0, pump.calls);
    clear_script();
    r = step(g_true, op_ready());
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[r].r.status);
    run_until(T0 + HOUR);
    DSP_CHECK_EQ_INT(0, pump.calls); /* the 08:17 occurrence stays skipped */
    invariants();
}

/* A power cut at every write of the READY operation: READY is never durable with an older floor,
 * and the overdue occurrence never runs, whatever landed. */
static void test_power_cut_at_every_ready_write(void)
{
    int cases = 0;
    for (int variant = 0; variant < 2; variant++) {
        for (long k = 1; k <= 10; k++) {
            provision(false);
            clear_script();
            step(g_true, op_create(once_at("old", 8, 17, 1000)));
            DSP_CHECK(session());
            dsp_utc_t t = T0 + 20 * MINUTE + 22;
            advance(t);
            clear_script();
            step_armed(g_true, op_ready());
            cut_at = k;
            cut_after = variant == 1;
            bool finished = session();
            cut_at = 0;
            if (finished) {
                break;
            }
            cases++;
            if (durable_readiness() == DSP_READINESS_READY) {
                DSP_CHECK(durable_time().not_before_utc >= t + 1);
            }
            reboot_and_trust();
            if (durable_readiness() == DSP_READINESS_READY) {
                DSP_CHECK(durable_time().not_before_utc >= t + 1);
            }
            run_until(T0 + HOUR);
            DSP_CHECK_EQ_INT(0, pump.calls);
            invariants();
        }
    }
    printf("  READY floor: %d power cuts, never READY with an older floor, no overdue dose\n", cases);
    DSP_CHECK(cases >= 2);
}

/* Only the NOT READY -> READY transition raises the floor; manual doses are not affected. */
static void test_only_the_transition_raises_the_floor(void)
{
    provision(true); /* READY at provisioning (floor = that instant + 1) */
    dsp_utc_t f0 = durable_time().not_before_utc;
    advance(g_true + 10 * MINUTE);
    clear_script();
    step(g_true, op_ready()); /* already READY: no change */
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(f0, durable_time().not_before_utc);
    advance(g_true + 10 * MINUTE);
    clear_script();
    step(g_true, op_not_ready());
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(f0, durable_time().not_before_utc); /* SERVICE_REQUIRED never raises it */
    /* NOT READY -> READY again raises it, and a manual dose right after works as before */
    advance(g_true + 2 * MINUTE);
    clear_script();
    int r = step(g_true, op_ready());
    int m = step(g_true + 1, op_manual(1, 500)); /* manual dose right after READY */
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[r].r.status);
    DSP_CHECK(durable_time().not_before_utc > f0);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[m].r.status);
    DSP_CHECK_EQ_INT(0, sc.res[m].manual.outcome); /* DONE: manual dosing unchanged */
    invariants();
}

int main(void)
{
    DSP_RUN(test_incident_ready_after_due_times);
    DSP_RUN(test_same_minute_boundary);
    DSP_RUN(test_floor_write_failure_keeps_not_ready);
    DSP_RUN(test_power_cut_at_every_ready_write);
    DSP_RUN(test_only_the_transition_raises_the_floor);
    return dsp_test_finish();
}
