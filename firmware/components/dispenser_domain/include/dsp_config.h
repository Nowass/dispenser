#ifndef DSP_CONFIG_H
#define DSP_CONFIG_H

/*
 * Persistent device configuration, schema v1 (milestone M1.3).
 *
 * Only fields whose meaning is already defined are included. Calibration (FR-011/012),
 * battery thresholds, device identity and any credentials are NOT part of v1; their
 * proposed schema is documented in firmware/README.md and will be added with a new
 * schema version in their milestones. This record never contains secrets.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Notification types (confirmed: low reservoir, low battery, device error). */
enum {
    DSP_NOTIFY_LOW_RESERVOIR = 1u << 0,
    DSP_NOTIFY_LOW_BATTERY = 1u << 1,
    DSP_NOTIFY_DEVICE_ERROR = 1u << 2,
};
#define DSP_NOTIFY_MASK_ALL 0x07u

#define DSP_CONFIG_LOW_STOCK_DEFAULT_PERCENT 20 /* FR-021 PROPOSED value */
#define DSP_CONFIG_SYNC_INTERVAL_MIN_H 24       /* NTP roughly every 24-48 h (confirmed) */
#define DSP_CONFIG_SYNC_INTERVAL_MAX_H 48
#define DSP_CONFIG_SYNC_INTERVAL_DEFAULT_H 24

typedef struct {
    uint32_t revision;               /* 0 = provisioned defaults; +1 on every save */
    uint32_t reservoir_capacity_ul;  /* configured reservoir volume; 0 = not configured */
    uint32_t reservoir_remaining_ul; /* ESTIMATED remaining volume (FR-020), <= capacity */
    uint8_t low_stock_percent;       /* low-reservoir warning threshold, 1..99 (FR-021) */
    uint16_t time_sync_interval_h;   /* NTP interval, 24..48 hours */
    uint8_t notify_mask;             /* enabled DSP_NOTIFY_* types */
    /* Format 2 (M1.9a): pump calibration and safety limits. 0 = not configured; the
     * provisioning value is 0 everywhere (no invented production defaults). Any missing
     * or invalid value prevents pump activation (dsp_config_pump_problems). */
    uint32_t pump_flow_ul_per_s; /* calibrated flow (FR-011) */
    int64_t calibrated_at;       /* trusted UTC of the calibration, 0 = never */
    uint32_t max_dose_ul;        /* per-dose limit (FR-016) */
    uint32_t max_daily_ul;       /* per local day (Europe/Prague) */
    uint32_t max_runtime_ms;     /* per dose, also <= the platform's hardware cutoff */
    uint32_t min_interval_s;     /* minimum time between two pump starts */
} dsp_config_t;

/* Validation result: bitwise OR of problems, 0 = valid. */
enum {
    DSP_CONFIG_E_REMAINING = 1u << 0,     /* remaining > capacity */
    DSP_CONFIG_E_LOW_STOCK = 1u << 1,     /* threshold outside 1..99 */
    DSP_CONFIG_E_SYNC_INTERVAL = 1u << 2, /* outside 24..48 h */
    DSP_CONFIG_E_NOTIFY_MASK = 1u << 3,   /* unknown bits */
    DSP_CONFIG_E_CALIBRATED_AT = 1u << 4, /* calibration time neither 0 nor a valid instant */
};

/* M1.10b.1: additional problems reported only by dsp_config_apply_update (stored records
 * are never refused for them, so existing data stays readable). */
enum {
    DSP_CONFIG_E_FIELD_MASK = 1u << 5, /* no field selected, or bits outside DSP_CONFIG_F_ALL */
    DSP_CONFIG_E_LIMITS = 1u << 6,     /* max_daily_ul below max_dose_ul (both set) */
    DSP_CONFIG_E_RUNTIME_HW = 1u << 7, /* max_runtime_ms above the known hardware cutoff */
};

/* Updatable fields (bit numbers = BLE protocol v1 SET_CONFIG field_mask, section 7.4).
 * revision, reservoir_remaining_ul (REFILL only) and the hardware cutoff are read-only. */
enum {
    DSP_CONFIG_F_RESERVOIR_CAPACITY = 1u << 0,
    DSP_CONFIG_F_LOW_STOCK = 1u << 1,
    DSP_CONFIG_F_SYNC_INTERVAL = 1u << 2,
    DSP_CONFIG_F_NOTIFY_MASK = 1u << 3,
    DSP_CONFIG_F_PUMP_FLOW = 1u << 4,
    DSP_CONFIG_F_CALIBRATED_AT = 1u << 5,
    DSP_CONFIG_F_MAX_DOSE = 1u << 6,
    DSP_CONFIG_F_MAX_DAILY = 1u << 7,
    DSP_CONFIG_F_MAX_RUNTIME = 1u << 8,
    DSP_CONFIG_F_MIN_INTERVAL = 1u << 9,
};
#define DSP_CONFIG_F_ALL 0x03FFu
/* Calibration and safety limits: changeable only while SERVICE_REQUIRED (D12). */
#define DSP_CONFIG_F_SERVICE_ONLY                                                            \
    (DSP_CONFIG_F_PUMP_FLOW | DSP_CONFIG_F_CALIBRATED_AT | DSP_CONFIG_F_MAX_DOSE |            \
     DSP_CONFIG_F_MAX_DAILY | DSP_CONFIG_F_MAX_RUNTIME | DSP_CONFIG_F_MIN_INTERVAL)
/* Earliest accepted calibration time in an update (= the timekeeper's plausibility floor,
 * 2025-01-01T00:00:00Z). */
#define DSP_CONFIG_CALIBRATED_AT_MIN ((int64_t)1735689600)

/* Pump calibration/limit problems (bitmask, 0 = the pump may be used). */
enum {
    DSP_PUMPCFG_E_FLOW = 1u << 0,         /* no calibrated flow */
    DSP_PUMPCFG_E_CALIBRATED_AT = 1u << 1, /* no calibration time */
    DSP_PUMPCFG_E_MAX_DOSE = 1u << 2,      /* per-dose limit missing */
    DSP_PUMPCFG_E_MAX_DAILY = 1u << 3,     /* daily limit missing or below the per-dose limit */
    DSP_PUMPCFG_E_MAX_RUNTIME = 1u << 4,   /* runtime limit missing */
    DSP_PUMPCFG_E_MIN_INTERVAL = 1u << 5,  /* interval missing (must be >= 1 s) */
    DSP_PUMPCFG_E_HW_LIMIT = 1u << 6,      /* no hardware cutoff known, or runtime limit above it */
};

/* Problems that prevent pump activation. hw_max_runtime_ms: the platform's hardware
 * cutoff (0 = unknown: the pump can never be used). */
uint32_t dsp_config_pump_problems(const dsp_config_t *cfg, uint32_t hw_max_runtime_ms);

/* Defaults written at provisioning: reservoir not configured (capacity and remaining 0,
 * so no dose can pass a future stock check), 20 %, 24 h, all notifications enabled. */
void dsp_config_defaults(dsp_config_t *cfg);

/* Bitmask of DSP_CONFIG_E_*; 0 = valid. A NULL pointer yields all bits set. */
uint32_t dsp_config_validate(const dsp_config_t *cfg);

bool dsp_config_equal(const dsp_config_t *a, const dsp_config_t *b);

/*
 * Revisioned partial update (M1.10b.1), pure: *next = *cur with the fields selected by
 * field_mask taken from *values, revision + 1; every other field (including the read-only
 * ones, whatever *values holds) is preserved. Returns DSP_CONFIG_E_* (0 = valid):
 * dsp_config_validate on the result, plus FIELD_MASK, CALIBRATED_AT (a selected calibration
 * time must be 0 or >= DSP_CONFIG_CALIBRATED_AT_MIN), LIMITS (checked when max_dose or
 * max_daily is selected) and RUNTIME_HW (when max_runtime is selected and
 * hw_max_runtime_ms != 0). The caller checks the expected revision and refuses
 * cur->revision == UINT32_MAX (no next revision). State rules (readiness, refill) belong
 * to the Application Service.
 */
uint32_t dsp_config_apply_update(const dsp_config_t *cur, uint16_t field_mask,
                                 const dsp_config_t *values, uint32_t hw_max_runtime_ms,
                                 dsp_config_t *next);

#ifdef __cplusplus
}
#endif

#endif /* DSP_CONFIG_H */
