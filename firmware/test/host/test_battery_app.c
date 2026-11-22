/*
 * Battery telemetry through the Application Service (dsp_app_get_battery, GET_BATTERY), on the
 * shared harness of test_app_ops.c (complete wakes, KV, clock, pump, power port, BLE fake that
 * runs operations in the application task) with a scripted battery port.
 *
 * Oracles: one measurement per wake of every cause, taken before any pump run of that wake;
 * never a measurement within DSP_APP_BATTERY_PUMP_QUIET_MS after a pump run; the sleep plan is
 * the same with and without the battery port (no wake is ever added); a failed or missing
 * measurement is a state of its own and never 0 mV; nothing is written to storage.
 */
#define DSP_APP_OPS_NO_MAIN
#include "test_app_ops.c"

#define VOL 1000u /* 0.84 s at the harness calibration: the pump advances the clock by 1 s */

static struct {
    int calls;
    int violations;          /* measured within the quiet time after a pump run */
    dsp_batt_sample_t next[8];
    int nnext;
    dsp_batt_sample_t dflt;
} batt;

static dsp_batt_sample_t batt_measure(void *ctx)
{
    (void)ctx;
    int i = batt.calls++;
    if (pump_ever && mono_ms - pump_end_mono < DSP_APP_BATTERY_PUMP_QUIET_MS) {
        batt.violations++;
        fprintf(stderr, "battery measured %llu ms after a pump run\n",
                (unsigned long long)(mono_ms - pump_end_mono));
    }
    return i < batt.nnext ? batt.next[i] : batt.dflt;
}
static const dsp_battery_port_t BATT = {NULL, batt_measure};

static dsp_batt_sample_t sample(uint8_t state, uint16_t mv)
{
    dsp_batt_sample_t s = {state, mv};
    return s;
}

/* After provision() (which resets the configuration): the port returns dflt on every call. */
static void with_battery(dsp_batt_sample_t dflt)
{
    memset(&batt, 0, sizeof batt);
    batt.dflt = dflt;
    CFG.battery = &BATT;
}

static op_t op_battery(void)
{
    return mk(OP_BATTERY);
}

/* ==== Sampling policy: once per wake, no extra wake ======================================== */

static void test_measured_once_per_wake_of_every_cause(void)
{
    /* reference world without the port: the sleep plan of a timer wake */
    provision(true);
    advance(g_true + rep.plan.sleep_s);
    DSP_CHECK(wake(DSP_CAUSE_TIMER));
    dsp_wake_plan_t ref = rep.plan;
    DSP_CHECK_EQ_INT(0, (int)rep.battery_reads);
    DSP_CHECK_EQ_INT(DSP_BATT_VALID, rep.battery.state); /* zeroed report: nothing measured */
    DSP_CHECK_EQ_INT(0, rep.battery.battery_mv);

    /* the same world with the port */
    provision(true);
    with_battery(sample(DSP_BATT_VALID, 3970));
    advance(g_true + rep.plan.sleep_s);
    DSP_CHECK(wake(DSP_CAUSE_TIMER));
    DSP_CHECK_EQ_INT(1, batt.calls);
    DSP_CHECK_EQ_INT(1, (int)rep.battery_reads);
    DSP_CHECK_EQ_INT(DSP_BATT_VALID, rep.battery.state);
    DSP_CHECK_EQ_INT(3970, rep.battery.battery_mv);
    /* no extra wake, the plan is unchanged */
    DSP_CHECK_EQ_INT((int)ref.reason, (int)rep.plan.reason);
    DSP_CHECK(ref.wake_at == rep.plan.wake_at);
    DSP_CHECK(ref.sleep_s == rep.plan.sleep_s);
    DSP_CHECK(rep.slept);

    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    DSP_CHECK_EQ_INT(2, batt.calls);
    clear_script();
    DSP_CHECK(session()); /* BUTTON wake, no request: still one measurement */
    DSP_CHECK_EQ_INT(3, batt.calls);
    DSP_CHECK_EQ_INT(1, (int)rep.battery_reads);
    /* runtime memory only: no battery key is ever written */
    for (size_t i = 0; i < FAKE_KV_MAX_KEYS; i++) {
        DSP_CHECK(!kv.durable[i].used || strstr(kv.durable[i].key, "bat") == NULL);
    }
    DSP_CHECK_EQ_INT(0, batt.violations);
    invariants();
}

static void probe_measured_before_pump(void)
{
    /* inside the pump run of the wake: this wake's measurement already exists */
    DSP_CHECK_EQ_INT(1, (int)app.rep.battery_reads);
    DSP_CHECK(app.batt_measured);
    DSP_CHECK(app.batt_ms <= mono_ms);
}

static void test_measured_before_the_scheduled_dose(void)
{
    provision(true);
    clear_script();
    dsp_schedule_t once = sched("one-shot", 8, 0, 1000);
    once.kind = DSP_REC_ONCE;
    once.start_date = date_of(T0);
    step(g_true, op_create(once));
    DSP_CHECK(session());
    with_battery(sample(DSP_BATT_VALID, 3900));
    pump_hook = probe_measured_before_pump;
    run_until(T0 + 5 * MINUTE);
    DSP_CHECK_EQ_INT(1, pump.calls);
    DSP_CHECK(pump_hook == NULL); /* the probe ran */
    DSP_CHECK(batt.calls >= 1);
    DSP_CHECK_EQ_INT(0, batt.violations);
    invariants();
}

/* ==== GET_BATTERY: cached value, refresh, age =============================================== */

static void test_get_battery_cache_refresh_and_age(void)
{
    provision(true);
    with_battery(sample(DSP_BATT_VALID, 3950));
    batt.next[0] = sample(DSP_BATT_VALID, 3970); /* the wake's measurement */
    batt.nnext = 1;
    clear_script();
    dsp_utc_t t0 = g_true;
    int a = step(t0, op_battery());
    int b = step(t0 + 30, op_battery());
    int c = step(t0 + 61, op_battery()); /* older than 60 s: measured again */
    int d = step(t0 + 62, op_battery());
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[a].r.status);
    DSP_CHECK_EQ_INT(DSP_BATT_VALID, sc.res[a].batt.sample.state);
    DSP_CHECK_EQ_INT(3970, sc.res[a].batt.sample.battery_mv);
    DSP_CHECK_EQ_INT(0, (int)sc.res[a].batt.age_s);
    DSP_CHECK_EQ_INT(3970, sc.res[b].batt.sample.battery_mv); /* cached */
    DSP_CHECK_EQ_INT(30, (int)sc.res[b].batt.age_s);
    DSP_CHECK_EQ_INT(3950, sc.res[c].batt.sample.battery_mv); /* refreshed */
    DSP_CHECK_EQ_INT(0, (int)sc.res[c].batt.age_s);
    DSP_CHECK_EQ_INT(3950, sc.res[d].batt.sample.battery_mv);
    DSP_CHECK_EQ_INT(1, (int)sc.res[d].batt.age_s);
    DSP_CHECK_EQ_INT(2, batt.calls);
    DSP_CHECK_EQ_INT(2, (int)rep.battery_reads);
    DSP_CHECK_EQ_INT(0, (int)sc.res[a].writes + (int)sc.res[c].writes); /* nothing stored */
    DSP_CHECK_EQ_INT(0, batt.violations);
    invariants();
}

static void test_no_measurement_right_after_a_pump_run(void)
{
    provision(true);
    advance(g_true + 2 * MINUTE);
    with_battery(sample(DSP_BATT_VALID, 3800));
    batt.next[0] = sample(DSP_BATT_VALID, 3970);
    batt.nnext = 1;
    clear_script();
    dsp_utc_t t0 = g_true;
    int m = step(t0 + 70, op_manual(1, VOL));  /* the pump ends at t0 + 71 */
    int b = step(t0 + 70, op_battery());       /* stale, but right after the pump: cached */
    int c = step(t0 + 72, op_battery());       /* 1 s after the pump: still cached */
    int d = step(t0 + 73, op_battery());       /* 2 s after the pump: measured again */
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[m].r.status);
    DSP_CHECK_EQ_INT(1, pump.calls);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[b].r.status);
    DSP_CHECK_EQ_INT(3970, sc.res[b].batt.sample.battery_mv);
    DSP_CHECK_EQ_INT(71, (int)sc.res[b].batt.age_s);
    DSP_CHECK_EQ_INT(3970, sc.res[c].batt.sample.battery_mv);
    DSP_CHECK_EQ_INT(3800, sc.res[d].batt.sample.battery_mv);
    DSP_CHECK_EQ_INT(0, (int)sc.res[d].batt.age_s);
    DSP_CHECK_EQ_INT(2, batt.calls);
    DSP_CHECK_EQ_INT(0, batt.violations);
    invariants();
}

/* ==== Unavailable telemetry: explicit states, never 0 V ===================================== */

static void check_unavailable(dsp_batt_sample_t port, uint8_t want)
{
    provision(true);
    with_battery(port);
    clear_script();
    int a = step(g_true, op_battery());
    int b = step(g_true + 61, op_battery()); /* retried after the refresh period: same state */
    DSP_CHECK(session());
    for (int k = 0; k < 2; k++) {
        const res_t *r = &sc.res[k == 0 ? a : b];
        DSP_CHECK_EQ_INT(DSP_OP_OK, r->r.status);
        DSP_CHECK_EQ_INT(want, r->batt.sample.state);
        DSP_CHECK_EQ_INT(0, r->batt.sample.battery_mv);
        DSP_CHECK_EQ_INT(0, (int)r->batt.age_s);
    }
    DSP_CHECK_EQ_INT(want, rep.battery.state);
    DSP_CHECK_EQ_INT(2, batt.calls);
    invariants();
}

static void test_unavailable_states_are_explicit(void)
{
    check_unavailable(sample(DSP_BATT_ADC_FAILED, 0), DSP_BATT_ADC_FAILED);
    check_unavailable(sample(DSP_BATT_NO_CALIBRATION, 0), DSP_BATT_NO_CALIBRATION);
    check_unavailable(sample(DSP_BATT_UNSTABLE, 0), DSP_BATT_UNSTABLE);
    check_unavailable(sample(DSP_BATT_OUT_OF_RANGE, 0), DSP_BATT_OUT_OF_RANGE);
    /* a misbehaving port is normalized */
    check_unavailable(sample(DSP_BATT_ADC_FAILED, 3970), DSP_BATT_ADC_FAILED); /* stray voltage dropped */
    check_unavailable(sample(9, 3970), DSP_BATT_ADC_FAILED);                  /* unknown state */
    check_unavailable(sample(DSP_BATT_VALID, 0), DSP_BATT_OUT_OF_RANGE);      /* "0 V" is not valid */
    check_unavailable(sample(DSP_BATT_VALID, DSP_BATT_MIN_MV - 1), DSP_BATT_OUT_OF_RANGE);
    check_unavailable(sample(DSP_BATT_VALID, DSP_BATT_MAX_MV + 1), DSP_BATT_OUT_OF_RANGE);
}

static void test_window_boundaries_are_valid(void)
{
    provision(true);
    with_battery(sample(DSP_BATT_VALID, DSP_BATT_MAX_MV));
    batt.next[0] = sample(DSP_BATT_VALID, DSP_BATT_MIN_MV);
    batt.nnext = 1;
    clear_script();
    int a = step(g_true, op_battery());
    int b = step(g_true + 61, op_battery());
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_BATT_VALID, sc.res[a].batt.sample.state);
    DSP_CHECK_EQ_INT(DSP_BATT_MIN_MV, sc.res[a].batt.sample.battery_mv);
    DSP_CHECK_EQ_INT(DSP_BATT_VALID, sc.res[b].batt.sample.state);
    DSP_CHECK_EQ_INT(DSP_BATT_MAX_MV, sc.res[b].batt.sample.battery_mv);
    invariants();
}

static void test_without_port_not_measured(void)
{
    provision(true);
    clear_script();
    int a = step(g_true, op_battery());
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[a].r.status);
    DSP_CHECK_EQ_INT(DSP_BATT_NOT_MEASURED, sc.res[a].batt.sample.state);
    DSP_CHECK_EQ_INT(0, sc.res[a].batt.sample.battery_mv);
    DSP_CHECK_EQ_INT(0, (int)rep.battery_reads);
    invariants();
}

static void test_preconditions_fail_closed(void)
{
    provision(true);
    with_battery(sample(DSP_BATT_VALID, 3970));
    clear_script();
    int n = step(g_true + 61, as(op_battery(), &NOBODY));
    int z = step(g_true + 61, as(op_battery(), NULL));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[n].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[z].r.status);
    DSP_CHECK_EQ_INT(1, batt.calls); /* only the wake's measurement: a refused call measures nothing */
    invariants();
}

int main(void)
{
    DSP_RUN(test_measured_once_per_wake_of_every_cause);
    DSP_RUN(test_measured_before_the_scheduled_dose);
    DSP_RUN(test_get_battery_cache_refresh_and_age);
    DSP_RUN(test_no_measurement_right_after_a_pump_run);
    DSP_RUN(test_unavailable_states_are_explicit);
    DSP_RUN(test_window_boundaries_are_valid);
    DSP_RUN(test_without_port_not_measured);
    DSP_RUN(test_preconditions_fail_closed);
    return dsp_test_finish();
}
