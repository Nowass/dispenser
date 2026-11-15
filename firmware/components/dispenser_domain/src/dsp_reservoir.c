#include "dsp_reservoir.h"

#include <stddef.h>
#include <string.h>

bool dsp_reservoir_covers(const dsp_config_t *cfg, uint32_t requested_ul)
{
    return cfg != NULL && cfg->reservoir_capacity_ul > 0 && requested_ul > 0 &&
           cfg->reservoir_remaining_ul >= requested_ul;
}

static dsp_storage_status_t save_next(dsp_storage_t *st, dsp_config_t *cfg, dsp_config_t *after)
{
    if (cfg->revision == UINT32_MAX) {
        return DSP_STORAGE_ERR_REVISION;
    }
    cfg->revision++;
    dsp_storage_status_t s = dsp_storage_save_config(st, cfg);
    if (s == DSP_STORAGE_OK && after != NULL) {
        *after = *cfg;
    }
    return s;
}

dsp_storage_status_t dsp_reservoir_consume(dsp_storage_t *st, uint32_t requested_ul,
                                           dsp_dose_kind_t kind, dsp_config_t *after,
                                           dsp_dose_permit_t *permit)
{
    if (permit == NULL) {
        return DSP_STORAGE_ERR_INVALID;
    }
    memset(permit, 0, sizeof *permit);
    /* manual and automatic doses consume alike */
    if (requested_ul == 0 || (kind != DSP_DOSE_AUTOMATIC && kind != DSP_DOSE_MANUAL)) {
        return DSP_STORAGE_ERR_INVALID;
    }
    dsp_config_t cfg;
    dsp_storage_status_t s = dsp_storage_load_config(st, &cfg);
    if (s != DSP_STORAGE_OK) {
        return s;
    }
    cfg.reservoir_remaining_ul =
        cfg.reservoir_remaining_ul > requested_ul ? cfg.reservoir_remaining_ul - requested_ul : 0;
    s = save_next(st, &cfg, after);
    if (s == DSP_STORAGE_OK) { /* durable: the pump may now run for this volume, once */
        permit->granted = true;
        permit->kind = (uint8_t)kind;
        permit->volume_ul = requested_ul;
        permit->config_revision = cfg.revision;
    }
    return s;
}

bool dsp_dose_permit_take(dsp_dose_permit_t *permit, uint32_t *volume_ul)
{
    if (permit == NULL) {
        return false;
    }
    bool ok = permit->granted && permit->volume_ul > 0;
    if (ok && volume_ul != NULL) {
        *volume_ul = permit->volume_ul;
    }
    memset(permit, 0, sizeof *permit);
    return ok;
}

dsp_storage_status_t dsp_reservoir_refill(dsp_storage_t *st, uint32_t capacity_ul,
                                          uint32_t remaining_ul, dsp_config_t *after)
{
    if (remaining_ul > capacity_ul) {
        return DSP_STORAGE_ERR_INVALID;
    }
    dsp_config_t cfg;
    dsp_storage_status_t s = dsp_storage_load_config(st, &cfg);
    if (s != DSP_STORAGE_OK) {
        return s;
    }
    cfg.reservoir_capacity_ul = capacity_ul;
    cfg.reservoir_remaining_ul = remaining_ul;
    return save_next(st, &cfg, after);
}
