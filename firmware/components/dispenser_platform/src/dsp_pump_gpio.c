#include "dsp_pump_gpio.h"

#include <stddef.h>
#include <string.h>

static int inactive(const dsp_pump_gpio_t *d)
{
    return d->active_level ? 0 : 1;
}

void dsp_pump_gpio_init(dsp_pump_gpio_t *d, const dsp_pump_gpio_hw_t *hw, bool active_high,
                        uint32_t max_runtime_ms)
{
    if (d == NULL) {
        return;
    }
    memset(d, 0, sizeof *d);
    d->hw = hw;
    d->active_level = active_high ? 1 : 0;
    d->max_runtime_ms = max_runtime_ms;
}

void dsp_pump_gpio_cutoff_fired(dsp_pump_gpio_t *d)
{
    if (d != NULL) {
        d->cutoff_fired = true;
    }
}

static bool hw_ok(const dsp_pump_gpio_t *d)
{
    const dsp_pump_gpio_hw_t *h = d->hw;
    return h != NULL && h->configure != NULL && h->set_level != NULL && h->read_level != NULL &&
           h->hold != NULL && h->cutoff_arm != NULL && h->cutoff_disarm != NULL && h->wait_ms != NULL;
}

/* Drives the pad inactive (configuring it first if needed) and holds it. True = reads inactive. */
static bool drive_off(dsp_pump_gpio_t *d)
{
    const dsp_pump_gpio_hw_t *h = d->hw;
    h->cutoff_disarm(h->ctx);
    h->hold(h->ctx, false); /* a held pad ignores level changes */
    bool ok = d->configured ? h->set_level(h->ctx, inactive(d)) : h->configure(h->ctx, inactive(d));
    if (ok) {
        d->configured = true;
    } else if (d->configured) {
        (void)h->configure(h->ctx, inactive(d)); /* second path: reconfigure the pad */
    }
    bool off = h->read_level(h->ctx) == inactive(d);
    h->hold(h->ctx, true); /* keep the inactive level through deep sleep */
    return off;
}

static bool prime_hw_ok(const dsp_pump_gpio_t *d)
{
    return hw_ok(d) && d->hw->cutoff_rearm != NULL && d->hw->now_ms != NULL && d->prime_max_ms != 0;
}

/* Ends a priming run in the bookkeeping (the pad is already driven off by the caller). */
static void prime_end(dsp_pump_gpio_t *d)
{
    if (d->priming) {
        uint64_t now = d->hw->now_ms(d->hw->ctx);
        d->st.prime_ms += now > d->prime_t0 ? now - d->prime_t0 : 0;
    }
    d->priming = false;
    d->running = false;
}

static void gpio_force_off(void *ctx)
{
    dsp_pump_gpio_t *d = ctx;
    if (d == NULL || !hw_ok(d)) {
        return;
    }
    d->st.force_offs++;
    (void)drive_off(d);
    prime_end(d);
    d->running = false;
}

static bool gpio_is_off(void *ctx)
{
    dsp_pump_gpio_t *d = ctx;
    return d != NULL && hw_ok(d) && d->configured && !d->running &&
           d->hw->read_level(d->hw->ctx) == inactive(d);
}

static dsp_pump_result_t gpio_run(void *ctx, uint32_t runtime_ms)
{
    dsp_pump_gpio_t *d = ctx;
    if (d == NULL || !hw_ok(d)) {
        return DSP_PUMP_START_FAILED;
    }
    const dsp_pump_gpio_hw_t *h = d->hw;
    if (d->priming || d->running) {
        d->st.refused++; /* one owner: never a dose while priming (the pad is left as it is) */
        return DSP_PUMP_START_FAILED;
    }
    /* own limit (defense in depth: the Dosing Service already applied max_runtime/hw_max) */
    if (runtime_ms == 0 || runtime_ms > d->max_runtime_ms) {
        d->st.refused++;
        (void)drive_off(d);
        return DSP_PUMP_START_FAILED;
    }
    /* start only from a configured pad that reads back inactive, with the independent cutoff armed
     * (an unconfigured pad can read inactive through the external resistor alone) */
    if (!drive_off(d) || !d->configured) {
        d->st.refused++;
        return DSP_PUMP_START_FAILED;
    }
    h->hold(h->ctx, false);
    d->cutoff_fired = false;
    if (!h->cutoff_arm(h->ctx, runtime_ms + DSP_PUMP_GPIO_CUTOFF_MARGIN_MS)) {
        d->st.refused++;
        (void)drive_off(d);
        return DSP_PUMP_START_FAILED;
    }
    d->running = true;
    d->st.runs++;
    bool fault = !h->set_level(h->ctx, d->active_level) || h->read_level(h->ctx) != d->active_level;
    if (!fault) {
        fault = !h->wait_ms(h->ctx, runtime_ms);
    }
    /* deactivate on every path, then verify */
    bool deactivated = h->set_level(h->ctx, inactive(d));
    h->cutoff_disarm(h->ctx);
    bool off = drive_off(d) && deactivated;
    d->running = false;
    if (d->cutoff_fired) {
        d->st.cutoffs++;
        fault = true;
    }
    if (fault || !off) {
        d->st.failures++;
        return DSP_PUMP_FAILED;
    }
    d->st.total_ms += runtime_ms;
    return DSP_PUMP_OK;
}

/* ---- Priming (fw 0.3.3): nonblocking, own maximum, cutoff re-armed by the caller's lease ---- */

static uint32_t prime_cap_ms(const dsp_pump_gpio_t *d)
{
    return d->prime_max_ms + DSP_PUMP_GPIO_PRIME_MARGIN_MS;
}

/* Drives the pad off after a priming fault; prime_stop() reports it. */
static void prime_fault_off(dsp_pump_gpio_t *d)
{
    const dsp_pump_gpio_hw_t *h = d->hw;
    (void)h->set_level(h->ctx, inactive(d));
    h->cutoff_disarm(h->ctx);
    (void)drive_off(d);
    d->prime_fault = true;
    d->running = false; /* priming stays set until prime_stop()/force_off() */
}

static dsp_pump_result_t gpio_prime_start(void *ctx, uint32_t cutoff_ms)
{
    dsp_pump_gpio_t *d = ctx;
    if (d == NULL || !prime_hw_ok(d)) {
        return DSP_PUMP_START_FAILED;
    }
    const dsp_pump_gpio_hw_t *h = d->hw;
    if (d->priming || d->running) {
        d->st.refused++;
        return DSP_PUMP_START_FAILED; /* never restarted or doubled (the pad is left as it is) */
    }
    if (cutoff_ms == 0 || cutoff_ms > prime_cap_ms(d)) {
        d->st.refused++;
        (void)drive_off(d);
        return DSP_PUMP_START_FAILED;
    }
    /* same preconditions as run(): configured pad reading inactive, independent cutoff armed */
    if (!drive_off(d) || !d->configured) {
        d->st.refused++;
        return DSP_PUMP_START_FAILED;
    }
    h->hold(h->ctx, false);
    d->cutoff_fired = false;
    d->prime_fault = false;
    if (!h->cutoff_arm(h->ctx, cutoff_ms)) {
        d->st.refused++;
        (void)drive_off(d);
        return DSP_PUMP_START_FAILED;
    }
    d->prime_t0 = h->now_ms(h->ctx);
    d->priming = true;
    d->running = true;
    d->st.primes++;
    if (!h->set_level(h->ctx, d->active_level) || h->read_level(h->ctx) != d->active_level) {
        prime_fault_off(d);
        d->st.failures++;
        prime_end(d);
        return DSP_PUMP_FAILED;
    }
    return DSP_PUMP_OK;
}

static bool gpio_prime_active(void *ctx)
{
    dsp_pump_gpio_t *d = ctx;
    return d != NULL && prime_hw_ok(d) && d->priming && d->running && !d->cutoff_fired &&
           !d->prime_fault && d->hw->read_level(d->hw->ctx) == d->active_level;
}

static bool gpio_prime_extend(void *ctx, uint32_t cutoff_ms)
{
    dsp_pump_gpio_t *d = ctx;
    if (d == NULL || !prime_hw_ok(d) || !d->priming || !d->running) {
        return false;
    }
    const dsp_pump_gpio_hw_t *h = d->hw;
    if (d->cutoff_fired || h->read_level(h->ctx) != d->active_level) {
        prime_fault_off(d);
        return false;
    }
    uint64_t now = h->now_ms(h->ctx);
    uint64_t elapsed = now > d->prime_t0 ? now - d->prime_t0 : 0;
    uint64_t cap = prime_cap_ms(d);
    if (cutoff_ms == 0 || elapsed >= cap) {
        prime_fault_off(d); /* the caller let the maximum pass: off now */
        return false;
    }
    uint64_t left = cap - elapsed;
    uint32_t ms = (uint64_t)cutoff_ms < left ? cutoff_ms : (uint32_t)left;
    if (!h->cutoff_rearm(h->ctx, ms)) {
        prime_fault_off(d); /* fired meanwhile or not restartable: never active without cutoff */
        return false;
    }
    return true;
}

static dsp_pump_result_t gpio_prime_stop(void *ctx)
{
    dsp_pump_gpio_t *d = ctx;
    if (d == NULL || !hw_ok(d)) {
        return DSP_PUMP_FAILED;
    }
    const dsp_pump_gpio_hw_t *h = d->hw;
    bool was = d->priming;
    bool deactivated = h->set_level(h->ctx, inactive(d)) || !d->configured;
    h->cutoff_disarm(h->ctx);
    bool off = drive_off(d) && deactivated;
    bool fault = was && (d->cutoff_fired || d->prime_fault);
    if (was && d->cutoff_fired) {
        d->st.cutoffs++;
    }
    if (d->hw->now_ms != NULL) {
        prime_end(d);
    }
    d->priming = false;
    d->running = false;
    d->prime_fault = false;
    if (fault || !off) {
        d->st.failures++;
        return DSP_PUMP_FAILED;
    }
    return DSP_PUMP_OK;
}

void dsp_pump_gpio_set_prime_max(dsp_pump_gpio_t *d, uint32_t prime_max_ms)
{
    if (d != NULL) {
        d->prime_max_ms = prime_max_ms;
    }
}

void dsp_pump_gpio_port(dsp_pump_gpio_t *d, dsp_pump_port_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    out->ctx = d;
    out->run = gpio_run;
    out->force_off = gpio_force_off;
    out->is_off = gpio_is_off;
    if (d != NULL && prime_hw_ok(d)) {
        out->prime_start = gpio_prime_start;
        out->prime_extend = gpio_prime_extend;
        out->prime_stop = gpio_prime_stop;
        out->prime_active = gpio_prime_active;
    }
}

uint32_t dsp_pump_gpio_delay_ticks(uint32_t ms, uint32_t tick_ms)
{
    if (ms == 0 || tick_ms == 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)ms + tick_ms - 1u) / tick_ms);
}
