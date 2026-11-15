#include "dsp_config.h"

#include <stddef.h>
#include <string.h>

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
               DSP_CONFIG_E_NOTIFY_MASK;
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
    return e;
}

bool dsp_config_equal(const dsp_config_t *a, const dsp_config_t *b)
{
    return a->revision == b->revision && a->reservoir_capacity_ul == b->reservoir_capacity_ul &&
           a->reservoir_remaining_ul == b->reservoir_remaining_ul &&
           a->low_stock_percent == b->low_stock_percent &&
           a->time_sync_interval_h == b->time_sync_interval_h && a->notify_mask == b->notify_mask;
}
