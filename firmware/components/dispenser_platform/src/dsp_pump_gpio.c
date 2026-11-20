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

static void gpio_force_off(void *ctx)
{
    dsp_pump_gpio_t *d = ctx;
    if (d == NULL || !hw_ok(d)) {
        return;
    }
    d->st.force_offs++;
    (void)drive_off(d);
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

void dsp_pump_gpio_port(dsp_pump_gpio_t *d, dsp_pump_port_t *out)
{
    if (out == NULL) {
        return;
    }
    out->ctx = d;
    out->run = gpio_run;
    out->force_off = gpio_force_off;
    out->is_off = gpio_is_off;
}

uint32_t dsp_pump_gpio_delay_ticks(uint32_t ms, uint32_t tick_ms)
{
    if (ms == 0 || tick_ms == 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)ms + tick_ms - 1u) / tick_ms);
}
