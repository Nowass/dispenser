/*
 * MVP wake button (XIAO D1 = GPIO3): the platform maps a deep-sleep GPIO wake to
 * DSP_CAUSE_BUTTON (test_platform.c covers the mapping, the arming of both wake sources and the
 * held-button rule). These tests pin down what the APPLICATION does with the causes, over the
 * shared harness of test_app_ops.c (complete wakes, real domain services, pump oracles: no pump
 * while BLE is on or an operation runs, never twice for one occurrence, only inside its due
 * window):
 *  - a button wake opens the BLE service window, a timer wake never does (also not in the
 *    bring-up build that opens a window on power-on);
 *  - the button is NOT a dosing trigger: readiness, schedules, the due window and anti-replay
 *    decide exactly as on a timer wake, and a button wake never raises the READY floor or
 *    changes readiness;
 *  - a held/stuck button (the platform then wakes by timer at most every 60 s, without a
 *    window) changes nothing for dosing.
 * Times are 08:xx local (T0 = 08:00).
 */
#define DSP_APP_OPS_NO_MAIN
#include "test_app_ops.c"

static void create_daily(const char *id, int h, int m, uint32_t vol)
{
    clear_script();
    step(g_true, op_create(sched(id, h, m, vol)));
    DSP_CHECK(session());
    clear_script();
}

/* A button wake with nobody connecting a command: the window ends by the owner (harness END). */
static void button_wake(void)
{
    clear_script();
    DSP_CHECK(session());
}

static void test_button_opens_window_timer_does_not(void)
{
    provision(false);
    int s0 = sc.starts;
    advance(g_true + 10 * MINUTE);
    DSP_CHECK(wake(DSP_CAUSE_TIMER));
    DSP_CHECK(rep.slept);
    DSP_CHECK_EQ_INT(s0, sc.starts); /* timer: no service window */
    button_wake();
    DSP_CHECK(rep.slept);
    DSP_CHECK_EQ_INT(s0 + 1, sc.starts); /* button: the window, then deep sleep again */
    advance(g_true + 10 * MINUTE);
    DSP_CHECK(wake(DSP_CAUSE_TIMER));
    DSP_CHECK_EQ_INT(s0 + 1, sc.starts);

    /* bring-up build (power-on window): a timer wake still never opens one */
    CFG.service_on_power_on = true;
    advance(g_true + 10 * MINUTE);
    DSP_CHECK(wake(DSP_CAUSE_TIMER));
    DSP_CHECK_EQ_INT(s0 + 1, sc.starts);
    button_wake();
    DSP_CHECK_EQ_INT(s0 + 2, sc.starts);
    CFG.service_on_power_on = false;
    DSP_CHECK(sleeps > 0);
    invariants();
}

/* NOT READY: a button wake at the due time of a schedule doses nothing, and changes nothing. */
static void test_button_never_doses_not_ready(void)
{
    provision(false);
    create_daily("d", 8, 10, 1000);
    dsp_time_meta_t t0 = durable_time();
    advance(T0 + 10 * MINUTE + 5);
    button_wake();
    DSP_CHECK_EQ_INT(0, pump.calls);
    advance(T0 + 12 * MINUTE);
    button_wake();
    DSP_CHECK_EQ_INT(0, pump.calls);
    DSP_CHECK_EQ_INT(DSP_READINESS_SERVICE_REQUIRED, durable_readiness());
    DSP_CHECK_EQ_INT(t0.not_before_utc, durable_time().not_before_utc);
    run_until(T0 + DAY + HOUR);
    DSP_CHECK_EQ_INT(0, pump.calls);
    invariants();
}

/* READY: the button wake doses exactly what a timer wake would (the due occurrence, once,
 * before the window; the pump oracle checks BLE is off), and nothing else. */
static void test_button_is_not_a_dosing_trigger_ready(void)
{
    provision(true);
    create_daily("d", 8, 10, 1000);
    dsp_utc_t floor0 = durable_time().not_before_utc;
    char id[96];
    occ_id(id, sizeof id, "d", T0);

    advance(T0 + 9 * MINUTE); /* before the due time: nothing */
    button_wake();
    DSP_CHECK_EQ_INT(0, pump.calls);

    advance(T0 + 10 * MINUTE + 5); /* due: dosed once, like on a timer wake */
    button_wake();
    DSP_CHECK_EQ_INT(1, runs_of(id));
    DSP_CHECK_EQ_INT(1, pump.calls);
    for (int k = 1; k <= 5; k++) { /* repeated presses inside the due window: no repeat */
        advance(T0 + 10 * MINUTE + 5 + k * MINUTE);
        button_wake();
    }
    advance(T0 + HOUR);
    button_wake(); /* nothing due */
    DSP_CHECK_EQ_INT(1, pump.calls);
    DSP_CHECK_EQ_INT(DSP_READINESS_READY, durable_readiness());
    DSP_CHECK_EQ_INT(floor0, durable_time().not_before_utc); /* never raises the READY floor */

    run_until(T0 + DAY + HOUR); /* the schedule continues on timer wakes */
    occ_id(id, sizeof id, "d", T0 + DAY);
    DSP_CHECK_EQ_INT(1, runs_of(id));
    DSP_CHECK_EQ_INT(2, pump.calls);
    invariants();
}

/* A button wake after the due window (missed occurrence) never catches up. */
static void test_button_after_due_window_skips(void)
{
    provision(true);
    create_daily("d", 8, 10, 1000);
    /* the device slept through 08:10 (e.g. a long planned sleep without the dose wake) */
    advance(T0 + 10 * MINUTE + WINDOW + 1);
    button_wake();
    DSP_CHECK_EQ_INT(0, pump.calls);
    advance(g_true + 2 * MINUTE);
    DSP_CHECK(wake(DSP_CAUSE_TIMER));
    DSP_CHECK_EQ_INT(0, pump.calls);
    invariants();
}

/* Held/stuck button: the platform arms the timer only, capped to 60 s, so the application sees
 * a run of short TIMER wakes. Dosing stays exactly once at the due time, and no window opens. */
static void test_held_button_short_timer_wakes(void)
{
    provision(true);
    create_daily("d", 8, 10, 1000);
    int s0 = sc.starts;
    int guard = 0;
    while (g_true < T0 + 30 * MINUTE && guard++ < 1000) {
        int64_t s = rep.plan.sleep_s;
        if (s > 60) {
            s = 60; /* DSP_PLAT_BUTTON_HELD_RETRY_S */
        }
        advance(g_true + (s < 1 ? 1 : s));
        DSP_CHECK(wake(DSP_CAUSE_TIMER));
        DSP_CHECK(rep.slept);
    }
    char id[96];
    occ_id(id, sizeof id, "d", T0);
    DSP_CHECK_EQ_INT(1, runs_of(id));
    DSP_CHECK_EQ_INT(1, pump.calls);
    DSP_CHECK_EQ_INT(s0, sc.starts);
    /* released: the next press is a normal button wake with a window */
    button_wake();
    DSP_CHECK_EQ_INT(s0 + 1, sc.starts);
    DSP_CHECK_EQ_INT(1, pump.calls);
    invariants();
}

int main(void)
{
    DSP_RUN(test_button_opens_window_timer_does_not);
    DSP_RUN(test_button_never_doses_not_ready);
    DSP_RUN(test_button_is_not_a_dosing_trigger_ready);
    DSP_RUN(test_button_after_due_window_skips);
    DSP_RUN(test_held_button_short_timer_wakes);
    return dsp_test_finish();
}
