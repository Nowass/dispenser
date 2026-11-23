#include "dsp_prime.h"

#include <stddef.h>

uint32_t dsp_prime_max_ms(uint32_t cfg_ms)
{
    return cfg_ms > DSP_PRIME_HARD_MAX_MS ? DSP_PRIME_HARD_MAX_MS : cfg_ms;
}

dsp_prime_verdict_t dsp_prime_check(const dsp_prime_conditions_t *c)
{
    if (c == NULL || !c->capable) {
        return DSP_PRIME_V_NOT_CAPABLE;
    }
    if (c->fault) {
        return DSP_PRIME_V_FAULT;
    }
    if (c->busy) {
        return DSP_PRIME_V_BUSY;
    }
    if (!c->battery_valid || c->battery_mv < DSP_PRIME_MIN_BATTERY_MV || c->battery_low_open) {
        return DSP_PRIME_V_BATTERY;
    }
    if (!c->reservoir_ok) {
        return DSP_PRIME_V_RESERVOIR;
    }
    if (c->clock_trusted && c->next_dose_known &&
        (c->next_dose_at <= c->now || c->next_dose_at - c->now <= (dsp_utc_t)DSP_PRIME_DOSE_MARGIN_S)) {
        return DSP_PRIME_V_DOSE_SOON;
    }
    return DSP_PRIME_V_OK;
}
