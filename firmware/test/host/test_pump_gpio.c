/*
 * Bench GPIO pump control signal (dsp_pump_gpio, pure; compiled unchanged into the firmware):
 * activation/deactivation timing on a virtual clock, polarity, the driver's own runtime limit,
 * no activation without an inactive readback and an armed cutoff, the independent cutoff,
 * readback faults, interrupted waits, force_off/is_off, the deep-sleep hold and the state after a
 * reset (RAM re-initialized, pad back to an undriven input).
 */
#include <stdio.h>
#include <string.h>

#include "dsp_pump_gpio.h"
#include "dsp_test.h"

/* ---- Fake pad, cutoff timer and clock ------------------------------------------------------ */

static struct {
    uint32_t now;          /* virtual ms */
    bool driven;           /* output enabled */
    int level;             /* driven level */
    int pull;              /* level of the undriven pad (external resistor) */
    bool held;
    bool cutoff_armed;
    uint32_t cutoff_at;
    /* faults */
    bool fail_configure, fail_set, fail_arm, wait_interrupt, stuck_active, read_error;
    int read_override;     /* -2 = none, else the value read back once while active */
    /* timeline of electrical activity */
    uint32_t active_since;
    bool active;
    uint32_t active_total, activations;
    uint32_t last_on, last_off;
    int set_while_held;    /* level writes while held (ignored by the pad) */
} pad;

static dsp_pump_gpio_t drv;
static int active_level = 1;

static int electrical(void)
{
    return pad.driven ? pad.level : pad.pull;
}

static void track(void)
{
    bool a = electrical() == active_level;
    if (a && !pad.active) {
        pad.active = true;
        pad.active_since = pad.now;
        pad.activations++;
        pad.last_on = pad.now;
    } else if (!a && pad.active) {
        pad.active = false;
        pad.active_total += pad.now - pad.active_since;
        pad.last_off = pad.now;
    }
}

static bool f_configure(void *ctx, int level)
{
    (void)ctx;
    if (pad.fail_configure) {
        return false;
    }
    if (!pad.held) {
        pad.driven = true;
        pad.level = level;
    }
    track();
    return true;
}
static bool f_set(void *ctx, int level)
{
    (void)ctx;
    if (pad.fail_set) {
        return false;
    }
    if (pad.held) {
        pad.set_while_held++;
        return true; /* a held pad keeps its level */
    }
    if (pad.stuck_active && level != active_level) {
        return true; /* driver fault: the pad stays active */
    }
    pad.level = level;
    track();
    return true;
}
static int f_read(void *ctx)
{
    (void)ctx;
    if (pad.read_error) {
        return -1;
    }
    if (pad.read_override != -2 && pad.active) {
        int v = pad.read_override;
        pad.read_override = -2;
        return v;
    }
    return electrical();
}
static void f_hold(void *ctx, bool on)
{
    (void)ctx;
    pad.held = on;
}
static void advance_to(uint32_t t);
static bool f_arm(void *ctx, uint32_t ms)
{
    (void)ctx;
    if (pad.fail_arm) {
        return false;
    }
    pad.cutoff_armed = true;
    pad.cutoff_at = pad.now + ms;
    return true;
}
static void f_disarm(void *ctx)
{
    (void)ctx;
    pad.cutoff_armed = false;
}
/* Virtual time passes; the armed cutoff fires at its instant like the esp_timer callback. */
static void advance_to(uint32_t t)
{
    if (pad.cutoff_armed && pad.cutoff_at <= t) {
        pad.now = pad.cutoff_at;
        pad.cutoff_armed = false;
        if (!pad.held && !pad.stuck_active) {
            pad.level = active_level ? 0 : 1;
        }
        track();
        dsp_pump_gpio_cutoff_fired(&drv);
    }
    pad.now = t;
    track();
}
static uint32_t wait_extra; /* simulated task stall beyond the requested wait */
static bool f_wait(void *ctx, uint32_t ms)
{
    (void)ctx;
    if (pad.wait_interrupt) {
        advance_to(pad.now + ms / 2);
        return false;
    }
    advance_to(pad.now + ms + wait_extra);
    return true;
}
static const dsp_pump_gpio_hw_t HW = {NULL, f_configure, f_set, f_read, f_hold, f_arm, f_disarm, f_wait};

static dsp_pump_port_t port;

/* A reset: RAM state lost, pad back to an undriven input (no hold), external resistor only. */
static void power_on(bool active_high, int pull)
{
    memset(&pad, 0, sizeof pad);
    pad.read_override = -2;
    pad.pull = pull;
    pad.now = 1000;
    wait_extra = 0;
    active_level = active_high ? 1 : 0;
    dsp_pump_gpio_init(&drv, &HW, active_high, 10000);
    dsp_pump_gpio_port(&drv, &port);
    track();
}

static void check_off_and_held(void)
{
    DSP_CHECK(!pad.active);
    DSP_CHECK(pad.held);
    DSP_CHECK(!pad.cutoff_armed);
    DSP_CHECK(port.is_off(port.ctx));
}

/* ---- Tests ---------------------------------------------------------------------------------- */

static void test_activation_timing_and_hold(void)
{
    power_on(true, 0);
    DSP_CHECK(!port.is_off(port.ctx)); /* never configured: not confirmed off */
    port.force_off(port.ctx);           /* SAFE_OFF at boot */
    check_off_and_held();
    DSP_CHECK_EQ_INT(0, pad.activations);
    uint32_t t0 = pad.now;
    DSP_CHECK_EQ_INT(DSP_PUMP_OK, port.run(port.ctx, 1234));
    DSP_CHECK_EQ_INT(1, pad.activations);
    DSP_CHECK_EQ_INT(1234, pad.active_total); /* exactly the commanded runtime */
    DSP_CHECK_EQ_INT(t0, pad.last_on);
    DSP_CHECK_EQ_INT(t0 + 1234, pad.last_off);
    check_off_and_held();
    DSP_CHECK_EQ_INT(0, pad.set_while_held);
    DSP_CHECK_EQ_INT(1, drv.st.runs);
    DSP_CHECK_EQ_INT(1234, (int)drv.st.total_ms);
    /* a second run, then force_off is idempotent */
    DSP_CHECK_EQ_INT(DSP_PUMP_OK, port.run(port.ctx, 10000)); /* exactly the maximum */
    DSP_CHECK_EQ_INT(11234, pad.active_total);
    port.force_off(port.ctx);
    port.force_off(port.ctx);
    check_off_and_held();
}

static void test_active_low_polarity(void)
{
    power_on(false, 1); /* active low with an external pull-up */
    port.force_off(port.ctx);
    check_off_and_held();
    DSP_CHECK_EQ_INT(1, electrical());
    DSP_CHECK_EQ_INT(DSP_PUMP_OK, port.run(port.ctx, 500));
    DSP_CHECK_EQ_INT(500, pad.active_total);
    DSP_CHECK_EQ_INT(1, electrical());
    check_off_and_held();
}

static void test_own_limit_never_activates(void)
{
    power_on(true, 0);
    DSP_CHECK_EQ_INT(DSP_PUMP_START_FAILED, port.run(port.ctx, 0));
    DSP_CHECK_EQ_INT(DSP_PUMP_START_FAILED, port.run(port.ctx, 10001));
    DSP_CHECK_EQ_INT(DSP_PUMP_START_FAILED, port.run(port.ctx, UINT32_MAX));
    DSP_CHECK_EQ_INT(0, pad.activations);
    DSP_CHECK_EQ_INT(3, drv.st.refused);
    check_off_and_held(); /* a refusal also leaves the pad driven inactive */
}

static void test_no_activation_without_preconditions(void)
{
    /* the pad cannot be configured */
    power_on(true, 0);
    pad.fail_configure = true;
    DSP_CHECK_EQ_INT(DSP_PUMP_START_FAILED, port.run(port.ctx, 500));
    DSP_CHECK_EQ_INT(0, pad.activations);
    DSP_CHECK(!port.is_off(port.ctx));
    /* the independent cutoff cannot be armed */
    power_on(true, 0);
    port.force_off(port.ctx);
    pad.fail_arm = true;
    DSP_CHECK_EQ_INT(DSP_PUMP_START_FAILED, port.run(port.ctx, 500));
    DSP_CHECK_EQ_INT(0, pad.activations);
    check_off_and_held();
    /* the pad does not read back inactive before the start (e.g. shorted to the active level) */
    power_on(true, 0);
    port.force_off(port.ctx);
    pad.read_error = true;
    DSP_CHECK_EQ_INT(DSP_PUMP_START_FAILED, port.run(port.ctx, 500));
    DSP_CHECK_EQ_INT(0, pad.activations);
    DSP_CHECK(!port.is_off(port.ctx)); /* cannot be confirmed: the application goes fail-safe */
}

static void test_independent_cutoff(void)
{
    power_on(true, 0);
    port.force_off(port.ctx);
    wait_extra = 5000; /* the task stalls far beyond the runtime */
    uint32_t t0 = pad.now;
    DSP_CHECK_EQ_INT(DSP_PUMP_FAILED, port.run(port.ctx, 2000));
    DSP_CHECK_EQ_INT(2000 + DSP_PUMP_GPIO_CUTOFF_MARGIN_MS, pad.active_total); /* bounded */
    DSP_CHECK_EQ_INT(t0 + 2000 + DSP_PUMP_GPIO_CUTOFF_MARGIN_MS, pad.last_off);
    DSP_CHECK_EQ_INT(1, drv.st.cutoffs);
    DSP_CHECK_EQ_INT(1, drv.st.failures);
    check_off_and_held();
    /* the next run starts clean */
    wait_extra = 0;
    DSP_CHECK_EQ_INT(DSP_PUMP_OK, port.run(port.ctx, 300));
}

static void test_faults_during_the_run(void)
{
    /* activation does not read back active */
    power_on(true, 0);
    port.force_off(port.ctx);
    pad.read_override = 0;
    DSP_CHECK_EQ_INT(DSP_PUMP_FAILED, port.run(port.ctx, 800));
    DSP_CHECK(pad.active_total < 800); /* deactivated at once, never waited */
    check_off_and_held();
    /* the wait is interrupted (cancellation): deactivated, FAILED */
    power_on(true, 0);
    port.force_off(port.ctx);
    pad.wait_interrupt = true;
    DSP_CHECK_EQ_INT(DSP_PUMP_FAILED, port.run(port.ctx, 800));
    DSP_CHECK_EQ_INT(400, pad.active_total);
    check_off_and_held();
    /* the output stays active after the run (driver stuck): FAILED and not confirmed off */
    power_on(true, 0);
    port.force_off(port.ctx);
    pad.stuck_active = true;
    DSP_CHECK_EQ_INT(DSP_PUMP_FAILED, port.run(port.ctx, 300));
    DSP_CHECK(pad.active);
    DSP_CHECK(!port.is_off(port.ctx)); /* SHUTDOWN cannot confirm: fail-safe reset, never sleep */
    /* a level write error while switching off */
    power_on(true, 0);
    port.force_off(port.ctx);
    pad.fail_set = true;
    DSP_CHECK_EQ_INT(DSP_PUMP_FAILED, port.run(port.ctx, 300));
    DSP_CHECK_EQ_INT(0, pad.activations); /* the activation itself failed */
}

static void test_reset_during_a_run(void)
{
    power_on(true, 0);
    port.force_off(port.ctx);
    /* power is lost while active: after the reset the pad is an undriven input */
    pad.held = false;
    DSP_CHECK(f_set(NULL, 1));
    DSP_CHECK(pad.active);
    power_on(true, 0); /* reset: RAM and pad state gone; the external pull-down decides */
    DSP_CHECK(!pad.active);
    DSP_CHECK_EQ_INT(0, electrical());
    port.force_off(port.ctx); /* SAFE_OFF is the first action of the next wake */
    check_off_and_held();
    /* without the external resistor an undriven pad could float active: the reason it is required */
    power_on(true, 1);
    DSP_CHECK(pad.active);
    port.force_off(port.ctx);
    check_off_and_held();
}

/* Bench 2026-11-20: 8,000 ms requested, 7,991 ms measured at a 10 ms tick. The tick-based wait is
 * within (ms - tick, ms + tick) for every phase of the first tick, and never reaches the cutoff. */
static void test_tick_rounded_pulse_bounds(void)
{
    const uint32_t ticks[] = {1, 10, 20};
    const uint32_t runs[] = {1, 9, 10, 11, 999, 1000, 5000, 7991, 8000, 9999, 10000};
    for (size_t t = 0; t < sizeof ticks / sizeof ticks[0]; t++) {
        uint32_t tick = ticks[t];
        DSP_CHECK(tick < DSP_PUMP_GPIO_CUTOFF_MARGIN_MS);
        for (size_t r = 0; r < sizeof runs / sizeof runs[0]; r++) {
            uint32_t ms = runs[r];
            uint32_t n = dsp_pump_gpio_delay_ticks(ms, tick);
            /* vTaskDelay(n): (n - 1) * tick < elapsed <= n * tick */
            uint64_t shortest = (uint64_t)(n - 1) * tick, longest = (uint64_t)n * tick;
            DSP_CHECK(shortest < ms);                       /* may be shorter, by less than a tick */
            DSP_CHECK(shortest + tick >= ms);               /* ... never by a tick or more */
            DSP_CHECK(longest < (uint64_t)ms + tick);       /* longer by less than a tick */
            DSP_CHECK(longest < (uint64_t)ms + DSP_PUMP_GPIO_CUTOFF_MARGIN_MS); /* cutoff not reached */
        }
    }
    DSP_CHECK_EQ_INT(800, dsp_pump_gpio_delay_ticks(8000, 10)); /* the bench case: 7,990..8,000 ms */
    DSP_CHECK_EQ_INT(0, dsp_pump_gpio_delay_ticks(0, 10));
    DSP_CHECK_EQ_INT(0, dsp_pump_gpio_delay_ticks(10, 0));
    DSP_CHECK_EQ_INT(UINT32_MAX, dsp_pump_gpio_delay_ticks(UINT32_MAX, 1));
}

int main(void)
{
    DSP_RUN(test_tick_rounded_pulse_bounds);
    DSP_RUN(test_activation_timing_and_hold);
    DSP_RUN(test_active_low_polarity);
    DSP_RUN(test_own_limit_never_activates);
    DSP_RUN(test_no_activation_without_preconditions);
    DSP_RUN(test_independent_cutoff);
    DSP_RUN(test_faults_during_the_run);
    DSP_RUN(test_reset_during_a_run);
    return dsp_test_finish();
}
