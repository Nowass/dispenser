/*
 * Pump priming (fw 0.3.3, dsp_prime.h, docs/BLE_PROTOCOL.md 7.9, 10.9): the pure admission rules
 * and the Application Service flow on the shared harness of test_app_ops.c (complete wakes, KV,
 * clock, BLE fake running operations in the application task) with the harness's priming fake
 * (output + independent cutoff on the monotonic clock) and a battery port.
 *
 * Oracles (harness): no dose pump run and no deep sleep while the priming output is active; here:
 * START/STOP, repeated commands, heartbeat expiry, the 30 s maximum without automatic restart,
 * disconnect, end of the session, faults (cutoff fired), refusals, one pump owner, nothing
 * written (no dose, no history, reservoir estimate unchanged) and nothing restarted after a reset.
 */
#define DSP_APP_OPS_NO_MAIN
#include "test_app_ops.c"

#include "dsp_prime.h"

/* ==== Pure rules ============================================================================ */

static dsp_prime_conditions_t good(void)
{
    dsp_prime_conditions_t c;
    memset(&c, 0, sizeof c);
    c.capable = c.reservoir_ok = c.battery_valid = c.clock_trusted = true;
    c.battery_mv = DSP_PRIME_MIN_BATTERY_MV;
    c.now = 1000000;
    c.next_dose_known = true;
    c.next_dose_at = c.now + DSP_PRIME_DOSE_MARGIN_S + 1;
    return c;
}

static void test_rules(void)
{
    dsp_prime_conditions_t c = good();
    DSP_CHECK_EQ_INT(DSP_PRIME_V_OK, dsp_prime_check(&c));
    c.next_dose_known = false;
    DSP_CHECK_EQ_INT(DSP_PRIME_V_OK, dsp_prime_check(&c));
    c = good(); c.clock_trusted = false; c.next_dose_at = c.now; /* no dose without a trusted clock */
    DSP_CHECK_EQ_INT(DSP_PRIME_V_OK, dsp_prime_check(&c));
    c = good(); c.next_dose_at = c.now + DSP_PRIME_DOSE_MARGIN_S;
    DSP_CHECK_EQ_INT(DSP_PRIME_V_DOSE_SOON, dsp_prime_check(&c));
    c = good(); c.next_dose_at = c.now - 1;
    DSP_CHECK_EQ_INT(DSP_PRIME_V_DOSE_SOON, dsp_prime_check(&c));
    c = good(); c.reservoir_ok = false;
    DSP_CHECK_EQ_INT(DSP_PRIME_V_RESERVOIR, dsp_prime_check(&c));
    c = good(); c.battery_mv = DSP_PRIME_MIN_BATTERY_MV - 1;
    DSP_CHECK_EQ_INT(DSP_PRIME_V_BATTERY, dsp_prime_check(&c));
    c = good(); c.battery_valid = false;
    DSP_CHECK_EQ_INT(DSP_PRIME_V_BATTERY, dsp_prime_check(&c));
    c = good(); c.battery_low_open = true;
    DSP_CHECK_EQ_INT(DSP_PRIME_V_BATTERY, dsp_prime_check(&c));
    c = good(); c.busy = true; c.battery_valid = false;
    DSP_CHECK_EQ_INT(DSP_PRIME_V_BUSY, dsp_prime_check(&c));
    c = good(); c.fault = true; c.busy = true;
    DSP_CHECK_EQ_INT(DSP_PRIME_V_FAULT, dsp_prime_check(&c));
    c = good(); c.capable = false; c.fault = true;
    DSP_CHECK_EQ_INT(DSP_PRIME_V_NOT_CAPABLE, dsp_prime_check(&c));
    DSP_CHECK_EQ_INT(DSP_PRIME_V_NOT_CAPABLE, dsp_prime_check(NULL));
    DSP_CHECK_EQ_INT(30000, (int)dsp_prime_max_ms(60000));
    DSP_CHECK_EQ_INT(30000, (int)dsp_prime_max_ms(30000));
    DSP_CHECK_EQ_INT(0, (int)dsp_prime_max_ms(0));
}

/* ==== Application flow ====================================================================== */

#define PMAX 30000u

static dsp_batt_sample_t batt_now;
static dsp_batt_sample_t b_measure(void *ctx) { (void)ctx; return batt_now; }
static const dsp_battery_port_t BATTP = {NULL, b_measure};

/* After provision(): priming enabled with a good battery. */
static void with_prime(void)
{
    batt_now.state = DSP_BATT_VALID;
    batt_now.battery_mv = 4050;
    CFG.battery = &BATTP;
    CFG.prime_max_ms = PMAX;
    CFG.session_ops_max = 4000;
    memset(&pr, 0, sizeof pr);
}

static void ready_world(void)
{
    provision(true);
    with_prime();
    advance(g_true + MINUTE);
}

/* A daily schedule at the local wall-clock time of the instant t. */
static dsp_schedule_t sched_at(const char *id, dsp_utc_t t)
{
    int32_t off = 0;
    DSP_CHECK(dsp_tz_offset_at(TZ, t, &off));
    int64_t loc = (t + off) % DAY;
    return sched(id, (int)(loc / HOUR), (int)((loc % HOUR) / MINUTE), 1000);
}

static int prime_step(dsp_utc_t at, int kind)
{
    return step(at, mk(kind));
}

static void check_stopped(int i, uint8_t reason)
{
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[i].r.status);
    DSP_CHECK(!sc.res[i].prime.running);
    DSP_CHECK_EQ_INT(reason, sc.res[i].prime.stop_reason);
}

static void test_not_capable(void)
{
    ready_world();
    CFG.prime_max_ms = 0;
    clear_script();
    int i = prime_step(g_true, OP_PRIME_START);
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[i].r.status);
    DSP_CHECK_EQ_INT(DSP_PRIME_V_NOT_CAPABLE, sc.res[i].prime.verdict);
    DSP_CHECK_EQ_INT(0, (int)sc.res[i].prime.max_ms);
    DSP_CHECK_EQ_INT(0, pr.starts);
    invariants();
}

static void test_start_heartbeat_stop(void)
{
    ready_world();
    dsp_config_t before = durable_cfg();
    int pumps0 = pump.calls;
    clear_script();
    dsp_utc_t t = g_true;
    int s0 = prime_step(t, OP_PRIME_START);
    int h[5];
    for (int k = 0; k < 5; k++) {
        h[k] = prime_step(t + 1 + k, OP_PRIME_STATUS);
    }
    int again = prime_step(t + 6, OP_PRIME_START); /* repeated START: not restarted */
    int stop = prime_step(t + 7, OP_PRIME_STOP);
    int stop2 = prime_step(t + 7, OP_PRIME_STOP); /* repeated STOP: harmless */
    int st = prime_step(t + 8, OP_PRIME_STATUS);
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[s0].r.status);
    DSP_CHECK_EQ_INT(DSP_PRIME_V_OK, sc.res[s0].prime.verdict);
    DSP_CHECK(sc.res[s0].prime.running);
    DSP_CHECK_EQ_INT((int)PMAX, (int)sc.res[s0].prime.max_ms);
    DSP_CHECK_EQ_INT((int)DSP_PRIME_LEASE_MS, sc.res[s0].prime.lease_ms);
    for (int k = 0; k < 5; k++) {
        DSP_CHECK(sc.res[h[k]].prime.running);
        DSP_CHECK_EQ_INT((k + 1) * 1000, (int)sc.res[h[k]].prime.elapsed_ms);
    }
    DSP_CHECK_EQ_INT(DSP_PRIME_V_OK, sc.res[again].prime.verdict);
    DSP_CHECK(sc.res[again].prime.running);
    DSP_CHECK_EQ_INT(6000, (int)sc.res[again].prime.elapsed_ms);
    DSP_CHECK_EQ_INT(1, pr.starts); /* never restarted */
    check_stopped(stop, DSP_PRIME_STOP_USER);
    DSP_CHECK_EQ_INT(7000, (int)sc.res[stop].prime.elapsed_ms);
    check_stopped(stop2, DSP_PRIME_STOP_USER);
    check_stopped(st, DSP_PRIME_STOP_USER);
    DSP_CHECK_EQ_INT(7000, (int)pr.on_ms);
    DSP_CHECK(!pr.active);
    DSP_CHECK_EQ_INT(1, (int)rep.primes);
    DSP_CHECK_EQ_INT(7000, (int)rep.prime_ms);
    /* no dose, nothing durable written by priming (only the session's own time sync may write) */
    DSP_CHECK_EQ_INT(pumps0, pump.calls);
    for (int k = s0; k <= st; k++) {
        DSP_CHECK_EQ_INT(0, (int)sc.res[k].writes);
    }
    dsp_config_t after = durable_cfg();
    DSP_CHECK_EQ_INT((int)before.reservoir_remaining_ul, (int)after.reservoir_remaining_ul);
    DSP_CHECK_EQ_INT((int)before.revision, (int)after.revision);
    DSP_CHECK(rep.slept);
    invariants();
}

static void test_heartbeat_expiry(void)
{
    ready_world();
    clear_script();
    dsp_utc_t t = g_true;
    int s0 = prime_step(t, OP_PRIME_START);
    int hb = prime_step(t + 1, OP_PRIME_STATUS);
    int late = prime_step(t + 6, OP_PRIME_STATUS); /* 5 s gap: the lease expired at t + 4 */
    DSP_CHECK(session());
    DSP_CHECK(sc.res[s0].prime.running && sc.res[hb].prime.running);
    check_stopped(late, DSP_PRIME_STOP_HEARTBEAT);
    DSP_CHECK(pr.on_ms <= 1000 + DSP_PRIME_LEASE_MS);
    DSP_CHECK(!pr.fired); /* the service stopped it before the independent cutoff */
    DSP_CHECK_EQ_INT(1, pr.starts); /* a late heartbeat never restarts it */
    DSP_CHECK(!pr.active);
    invariants();
}

static void test_maximum_needs_a_new_start(void)
{
    ready_world();
    clear_script();
    dsp_utc_t t = g_true;
    int s0 = prime_step(t, OP_PRIME_START);
    int hb[34];
    for (int k = 0; k < 34; k++) {
        hb[k] = prime_step(t + 1 + k, OP_PRIME_STATUS);
    }
    int again = prime_step(t + 36, OP_PRIME_START); /* an explicit new activation */
    int stop = prime_step(t + 37, OP_PRIME_STOP);
    DSP_CHECK(session());
    DSP_CHECK(sc.res[s0].prime.running);
    DSP_CHECK(sc.res[hb[28]].prime.running); /* 29 s */
    check_stopped(hb[29], DSP_PRIME_STOP_MAX_TIME); /* 30 s */
    DSP_CHECK_EQ_INT((int)PMAX, (int)sc.res[hb[29]].prime.elapsed_ms);
    check_stopped(hb[33], DSP_PRIME_STOP_MAX_TIME); /* heartbeats never restart it */
    DSP_CHECK(pr.max_on_ms <= PMAX);
    DSP_CHECK_EQ_INT(DSP_PRIME_V_OK, sc.res[again].prime.verdict);
    DSP_CHECK(sc.res[again].prime.running);
    check_stopped(stop, DSP_PRIME_STOP_USER);
    DSP_CHECK_EQ_INT(2, pr.starts);
    invariants();
}

static void test_disconnect_and_session_end(void)
{
    ready_world();
    clear_script();
    dsp_utc_t t = g_true;
    prime_step(t, OP_PRIME_START);
    prime_step(t + 1, OP_DISCONNECT);
    DSP_CHECK(session());
    DSP_CHECK(!pr.active);
    DSP_CHECK_EQ_INT(DSP_PRIME_STOP_DISCONNECT, rep.prime_stop);
    DSP_CHECK(pr.on_ms <= 2000);
    DSP_CHECK(rep.slept);
    invariants();

    /* the owner ends the window while priming: stopped before BLE stops, then sleep */
    with_prime();
    advance(g_true + HOUR);
    clear_script();
    t = g_true;
    prime_step(t, OP_PRIME_START);
    prime_step(t + 1, OP_PRIME_STATUS);
    step(t + 1, mk(OP_END));
    DSP_CHECK(session());
    DSP_CHECK(!pr.active);
    DSP_CHECK_EQ_INT(DSP_PRIME_STOP_SESSION, rep.prime_stop);
    DSP_CHECK(rep.slept);
    invariants();

    /* the window maximum ends the session (no authorization needed for that) */
    with_prime();
    advance(g_true + HOUR);
    clear_script();
    t = g_true;
    prime_step(t, OP_PRIME_START);
    step(t + 1, mk(OP_END_TIMEOUT));
    DSP_CHECK(session());
    DSP_CHECK(!pr.active);
    DSP_CHECK(rep.slept);
    invariants();
}

static void test_one_owner(void)
{
    ready_world();
    clear_script();
    dsp_utc_t t = g_true;
    int pumps0 = pump.calls;
    prime_step(t, OP_PRIME_START);
    int m = step(t + 1, op_manual(1, 1000)); /* refused while priming, not consumed */
    int stop = prime_step(t + 2, OP_PRIME_STOP);
    int m2 = step(t + 3, op_manual(1, 1000)); /* the same request afterwards: executed */
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_BUSY, sc.res[m].r.status);
    DSP_CHECK_EQ_INT(0, (int)sc.res[m].writes);
    check_stopped(stop, DSP_PRIME_STOP_USER);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[m2].r.status);
    DSP_CHECK_EQ_INT(pumps0 + 1, pump.calls);
    invariants();
}

static void test_refusals(void)
{
    /* battery */
    ready_world();
    batt_now.battery_mv = 3790;
    clear_script();
    int i = prime_step(g_true, OP_PRIME_START);
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_PRIME_V_BATTERY, sc.res[i].prime.verdict);
    batt_now.state = DSP_BATT_UNSTABLE;
    batt_now.battery_mv = 4100;
    clear_script();
    i = prime_step(g_true, OP_PRIME_START);
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_PRIME_V_BATTERY, sc.res[i].prime.verdict);
    CFG.battery = NULL; /* never measured */
    clear_script();
    i = prime_step(g_true, OP_PRIME_START);
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_PRIME_V_BATTERY, sc.res[i].prime.verdict);
    DSP_CHECK_EQ_INT(0, pr.starts);

    /* reservoir estimated empty (a refill of 1 ml, then a manual dose of 1 ml) */
    with_prime();
    clear_script();
    step(g_true, op_refill(1000));
    int md = step(g_true + 1, op_manual(1, 1000));
    i = prime_step(g_true + 20, OP_PRIME_START);
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[md].r.status);
    DSP_CHECK_EQ_INT(0, (int)durable_cfg().reservoir_remaining_ul);
    DSP_CHECK_EQ_INT(DSP_PRIME_V_RESERVOIR, sc.res[i].prime.verdict);
    DSP_CHECK_EQ_INT(0, pr.starts);

    /* a dose due within 15 min */
    ready_world();
    clear_script();
    step(g_true, op_create(sched_at("soon", g_true + 10 * MINUTE)));
    i = prime_step(g_true, OP_PRIME_START);
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_PRIME_V_DOSE_SOON, sc.res[i].prime.verdict);
    DSP_CHECK_EQ_INT(0, pr.starts);

    /* unauthorized: nothing started; STOP without authorization changes nothing either */
    ready_world();
    clear_script();
    i = step(g_true, as(mk(OP_PRIME_START), &NOBODY));
    int j = step(g_true, as(mk(OP_PRIME_START), NULL));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[i].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[j].r.status);
    DSP_CHECK_EQ_INT(0, pr.starts);
    invariants();
}

static void test_faults(void)
{
    /* the start cannot be confirmed: FAULT, the output is off */
    ready_world();
    pr.fail_start = true;
    clear_script();
    int i = prime_step(g_true, OP_PRIME_START);
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_PRIME_V_FAULT, sc.res[i].prime.verdict);
    DSP_CHECK(!pr.active);

    /* the lease re-arm fails (cutoff fired / not restartable): stopped with FAULT, no further
     * pump use in this wake (a later START is refused), allowed again in the next wake */
    with_prime();
    advance(g_true + HOUR);
    clear_script();
    dsp_utc_t t = g_true;
    prime_step(t, OP_PRIME_START);
    int hb = prime_step(t + 1, OP_PRIME_STATUS);
    int st = prime_step(t + 1, OP_PRIME_STATUS);
    int again = prime_step(t + 2, OP_PRIME_START);
    pr.fail_extend = true; /* the first renewal fails */
    DSP_CHECK(session());
    check_stopped(hb, DSP_PRIME_STOP_FAULT);
    check_stopped(st, DSP_PRIME_STOP_FAULT);
    DSP_CHECK_EQ_INT(DSP_PRIME_V_FAULT, sc.res[again].prime.verdict);
    DSP_CHECK(rep.pump_fault);
    DSP_CHECK(!pr.active);
    DSP_CHECK(rep.slept);
    pr.fail_extend = false;
    advance(g_true + HOUR);
    clear_script();
    i = prime_step(g_true, OP_PRIME_START);
    prime_step(g_true + 1, OP_PRIME_STOP);
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_PRIME_V_OK, sc.res[i].prime.verdict);
    invariants();
}

static bool cut_now;
static void cut_hook(int i)
{
    (void)i;
    cut_now = true;
    power_lost(); /* power lost while the priming output is active */
}

static void test_reset_never_restarts(void)
{
    ready_world();
    clear_script();
    dsp_utc_t t = g_true;
    prime_step(t, OP_PRIME_START);
    op_t h = mk(OP_HOOK);
    h.hook = cut_hook;
    step(t + 1, h);
    cut_now = false;
    DSP_CHECK(!session()); /* aborted by the power cut */
    DSP_CHECK(cut_now);
    /* power back: RAM state gone; the hardware output is undriven (pull-down) after a reset */
    pr.active = false;
    reboot_power_on();
    int starts = pr.starts;
    with_prime();
    pr.starts = starts;
    clear_script();
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    DSP_CHECK(!pr.active);
    DSP_CHECK_EQ_INT(1, pr.starts); /* nothing started by the boot */
    DSP_CHECK_EQ_INT(0, (int)rep.primes);
    /* the next button session: status only, still nothing running */
    advance(g_true + MINUTE);
    clear_script();
    int st = prime_step(g_true, OP_PRIME_STATUS);
    DSP_CHECK(session());
    DSP_CHECK(!sc.res[st].prime.running);
    DSP_CHECK_EQ_INT(0, (int)sc.res[st].prime.elapsed_ms);
    DSP_CHECK_EQ_INT(1, pr.starts);
    invariants();
}

static void test_update_refused_while_priming(void)
{
    ready_world();
    clear_script();
    dsp_utc_t t = g_true;
    prime_step(t, OP_PRIME_START);
    int u = step(t + 1, mk(OP_UPDATE_STATUS)); /* readable while priming */
    int stop = prime_step(t + 2, OP_PRIME_STOP);
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[u].r.status);
    check_stopped(stop, DSP_PRIME_STOP_USER);
    DSP_CHECK(app.prime_active == false);
    invariants();
}

int main(void)
{
    DSP_RUN(test_rules);
    DSP_RUN(test_not_capable);
    DSP_RUN(test_start_heartbeat_stop);
    DSP_RUN(test_heartbeat_expiry);
    DSP_RUN(test_maximum_needs_a_new_start);
    DSP_RUN(test_disconnect_and_session_end);
    DSP_RUN(test_one_owner);
    DSP_RUN(test_refusals);
    DSP_RUN(test_faults);
    DSP_RUN(test_reset_never_restarts);
    DSP_RUN(test_update_refused_while_priming);
    return dsp_test_finish();
}
