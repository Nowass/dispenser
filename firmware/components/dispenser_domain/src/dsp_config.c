#include "dsp_config.h"

#include <stddef.h>
#include <string.h>

#include "dsp_time.h"

void dsp_config_defaults(dsp_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof *cfg);
    cfg->revision = 0;
    cfg->reservoir_capacity_ul = 0;
    cfg->reservoir_remaining_ul = 0;
    cfg->low_stock_percent = DSP_CONFIG_LOW_STOCK_DEFAULT_PERCENT;
    cfg->time_sync_interval_h = DSP_CONFIG_SYNC_INTERVAL_DEFAULT_H;
    cfg->notify_mask = DSP_NOTIFY_MASK_ALL;
}

uint32_t dsp_config_validate(const dsp_config_t *cfg)
{
    if (cfg == NULL) {
        return DSP_CONFIG_E_REMAINING | DSP_CONFIG_E_LOW_STOCK | DSP_CONFIG_E_SYNC_INTERVAL |
               DSP_CONFIG_E_NOTIFY_MASK | DSP_CONFIG_E_CALIBRATED_AT;
    }
    uint32_t e = 0;
    if (cfg->reservoir_remaining_ul > cfg->reservoir_capacity_ul) {
        e |= DSP_CONFIG_E_REMAINING;
    }
    if (cfg->low_stock_percent < 1 || cfg->low_stock_percent > 99) {
        e |= DSP_CONFIG_E_LOW_STOCK;
    }
    if (cfg->time_sync_interval_h < DSP_CONFIG_SYNC_INTERVAL_MIN_H ||
        cfg->time_sync_interval_h > DSP_CONFIG_SYNC_INTERVAL_MAX_H) {
        e |= DSP_CONFIG_E_SYNC_INTERVAL;
    }
    if ((cfg->notify_mask & ~DSP_NOTIFY_MASK_ALL) != 0) {
        e |= DSP_CONFIG_E_NOTIFY_MASK;
    }
    if (cfg->calibrated_at != 0 &&
        (cfg->calibrated_at < DSP_UTC_MIN || cfg->calibrated_at > DSP_UTC_MAX)) {
        e |= DSP_CONFIG_E_CALIBRATED_AT;
    }
    return e;
}

uint32_t dsp_config_pump_problems(const dsp_config_t *cfg, uint32_t hw_max_runtime_ms)
{
    if (cfg == NULL) {
        return DSP_PUMPCFG_E_FLOW | DSP_PUMPCFG_E_CALIBRATED_AT | DSP_PUMPCFG_E_MAX_DOSE |
               DSP_PUMPCFG_E_MAX_DAILY | DSP_PUMPCFG_E_MAX_RUNTIME | DSP_PUMPCFG_E_MIN_INTERVAL |
               DSP_PUMPCFG_E_HW_LIMIT;
    }
    uint32_t e = 0;
    if (cfg->pump_flow_ul_per_s == 0) {
        e |= DSP_PUMPCFG_E_FLOW;
    }
    if (cfg->calibrated_at == 0 || cfg->calibrated_at < DSP_UTC_MIN ||
        cfg->calibrated_at > DSP_UTC_MAX) {
        e |= DSP_PUMPCFG_E_CALIBRATED_AT;
    }
    if (cfg->max_dose_ul == 0) {
        e |= DSP_PUMPCFG_E_MAX_DOSE;
    }
    if (cfg->max_daily_ul == 0 || cfg->max_daily_ul < cfg->max_dose_ul) {
        e |= DSP_PUMPCFG_E_MAX_DAILY;
    }
    if (cfg->max_runtime_ms == 0) {
        e |= DSP_PUMPCFG_E_MAX_RUNTIME;
    }
    if (cfg->min_interval_s == 0) {
        e |= DSP_PUMPCFG_E_MIN_INTERVAL;
    }
    if (hw_max_runtime_ms == 0 || cfg->max_runtime_ms > hw_max_runtime_ms) {
        e |= DSP_PUMPCFG_E_HW_LIMIT;
    }
    return e;
}

bool dsp_config_equal(const dsp_config_t *a, const dsp_config_t *b)
{
    return a->revision == b->revision && a->reservoir_capacity_ul == b->reservoir_capacity_ul &&
           a->reservoir_remaining_ul == b->reservoir_remaining_ul &&
           a->low_stock_percent == b->low_stock_percent &&
           a->time_sync_interval_h == b->time_sync_interval_h && a->notify_mask == b->notify_mask &&
           a->pump_flow_ul_per_s == b->pump_flow_ul_per_s && a->calibrated_at == b->calibrated_at &&
           a->max_dose_ul == b->max_dose_ul && a->max_daily_ul == b->max_daily_ul &&
           a->max_runtime_ms == b->max_runtime_ms && a->min_interval_s == b->min_interval_s;
}
