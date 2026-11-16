#ifndef DSP_APP_H
#define DSP_APP_H

/*
 * Application Service (milestone M1.9b): one wake cycle, run to completion by a single
 * owner (one task, static memory, no allocation). It coordinates the existing domain
 * services and is their only caller:
 *
 *   SAFE_OFF -> LOAD -> RECOVER -> CLOCK -> OBSERVE -> DOSE -> CONNECT [-> DOSE]
 *            -> BLE (button wake only) [-> CONNECT] -> SYNC -> PLAN [-> DOSE] -> SHUTDOWN
 *            -> SLEEP | FAIL_SAFE
 *
 * Approved rules (M1.9 design and the M1.9b authorization):
 *  - SAFE_OFF is the first action: the pump port is handed to the Dosing Service's safe-off
 *    (this service never drives the pump; doses only through dsp_dosing_*). If OFF cannot
 *    be confirmed, nothing else runs: straight to SHUTDOWN, which then fails safe.
 *  - LOAD failure (storage open or any record) = DEGRADED: no recovery, no dosing, no
 *    notification transmission, no Wi-Fi; a button wake gets a read-only service session;
 *    the plan is the re-evaluation cap only (Q12). The retained block is preserved.
 *  - Dosing has priority over connectivity and BLE: due doses run before CONNECT; CONNECT
 *    is guarded by the next dose (incl. a same-time dose waiting for the minimum interval);
 *    a BLE session is suspended before any pump run (scheduled or manual) and resumed if
 *    its window remains; BLE commands are queued (bounded) and processed by this task only.
 *  - A pump fault or a storage failure ends dosing for the rest of the wake.
 *  - Wi-Fi, BLE and the pump are never active at the same time (sequential stages).
 *  - SHUTDOWN: pump OFF confirmed, HTTPS ended, Wi-Fi DISABLED, BLE stopped (retried up to
 *    DSP_APP_SHUTDOWN_ATTEMPTS times), then the retained block is saved; sleep only if
 *    every item is confirmed, otherwise the fail-safe port (never sleep with an active pump
 *    or radio). DSP_POWER_FAILSAFE_LIMIT consecutive fail-safes inhibit dosing until a
 *    shutdown is confirmed again.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dsp_connectivity.h"
#include "dsp_dosing.h"
#include "dsp_power.h"
#include "dsp_schedule_repo.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_APP_CMDQ_CAPACITY 8
#define DSP_APP_SERVICE_IDLE_DEFAULT_S 180u /* approved: advertising without a client */
#define DSP_APP_SERVICE_MAX_DEFAULT_S 900u  /* approved: maximum total session (15 min) */
#define DSP_APP_SHUTDOWN_ATTEMPTS 3
#define DSP_APP_TRACE_MAX 48
#define DSP_APP_DOSE_PASSES (DSP_SCHEDULE_CAPACITY + 2)
#define DSP_APP_BLE_MAX_ITERATIONS 100000u /* bound for a misbehaving wait() */

typedef enum {
    DSP_STAGE_SAFE_OFF = 1,
    DSP_STAGE_LOAD,
    DSP_STAGE_RECOVER,
    DSP_STAGE_CLOCK,
    DSP_STAGE_OBSERVE,
    DSP_STAGE_DOSE,
    DSP_STAGE_CONNECT,
    DSP_STAGE_BLE,
    DSP_STAGE_SYNC,
    DSP_STAGE_PLAN,
    DSP_STAGE_SHUTDOWN,
    DSP_STAGE_SLEEP,
    DSP_STAGE_FAIL_SAFE,
} dsp_app_stage_t;

/* ---- Service-mode commands (bounded queue) --------------------------------------------- */

typedef enum {
    DSP_CMD_STATUS = 1,
    DSP_CMD_MANUAL_DOSE,   /* volume_ul */
    DSP_CMD_REFILL,        /* capacity_ul, remaining_ul */
    DSP_CMD_SET_READINESS, /* readiness */
    DSP_CMD_END_SESSION,
} dsp_app_cmd_kind_t;

typedef struct {
    uint8_t kind; /* dsp_app_cmd_kind_t */
    uint32_t id;  /* echoed in the response */
    uint32_t volume_ul;
    uint32_t capacity_ul;
    uint32_t remaining_ul;
    uint8_t readiness; /* dsp_readiness_t */
} dsp_app_cmd_t;

typedef enum {
    DSP_RESP_OK = 0,
    DSP_RESP_REFUSED,  /* gates / limits / coverage (see dosing detail) */
    DSP_RESP_DEGRADED, /* storage not available, dosing inhibited or a fault in this wake */
    DSP_RESP_ERR_ARG,
    DSP_RESP_STORAGE,  /* a write failed */
    DSP_RESP_FAILED,   /* the pump reported a fault */
} dsp_app_resp_status_t;

typedef struct {
    uint32_t id;
    uint8_t kind;
    dsp_app_resp_status_t status;
    dsp_dosing_status_t dosing; /* detail of MANUAL_DOSE / REFILL / SET_READINESS */
    uint32_t gates;
    uint32_t limits;
    uint32_t pump_problems;
    /* Status snapshot (every response). */
    bool storage_available;
    uint8_t readiness;
    uint8_t dosing_flags;
    uint32_t capacity_ul;
    uint32_t remaining_ul;
    bool next_due_known;
    dsp_utc_t next_due_at;
} dsp_app_response_t;

/* ---- Ports ---------------------------------------------------------------------------- */

/* BLE service mode (no NimBLE/GATT in this milestone). The platform BLE task only posts
 * commands with dsp_app_post() and never touches domain state.
 *
 * Session lifetime (approved policy), measured on the MONOTONIC clock (never wall time):
 *  - no client connected: the session ends DSP_APP_SERVICE_IDLE_DEFAULT_S (configurable)
 *    after it started or after the last client disconnected;
 *  - while a client is connected the session stays open;
 *  - it always ends at the maximum total duration (DSP_APP_SERVICE_MAX_DEFAULT_S, configurable);
 *  - a due scheduled dose suspends it (connection closed, "busy: dosing"); it resumes only
 *    while the deadline still permits, and a connection closed by the suspension restarts the
 *    idle timeout at resume. */
typedef struct {
    void *ctx;
    bool (*start)(void *ctx);    /* advertising on; false = unavailable */
    bool (*connected)(void *ctx); /* a client is connected (platform connection state) */
    void (*suspend)(void *ctx);  /* advertising off, connection closed ("busy: dosing") */
    bool (*resume)(void *ctx);   /* false = ends the session */
    void (*stop)(void *ctx);     /* idempotent: radio off */
    bool (*is_off)(void *ctx);   /* checklist confirmation; NULL = unconfirmed */
    /* Blocks until a command was posted or max_ms elapsed. */
    void (*wait)(void *ctx, uint32_t max_ms);
    void (*respond)(void *ctx, const dsp_app_response_t *resp);
} dsp_ble_port_t;

/* Optional synchronization boundary for the command queue (see dsp_app_post). NULL = the
 * producer runs in the application task itself (host tests). */
typedef struct {
    void *ctx;
    void (*lock)(void *ctx);   /* e.g. a FreeRTOS critical section or mutex (M1.9c/BLE) */
    void (*unlock)(void *ctx);
} dsp_app_sync_t;

/* Clock continuity for the timekeeper (reset reason + RTC marker; platform decides). */
typedef struct {
    void *ctx;
    dsp_time_reading_t (*reading)(void *ctx, dsp_utc_t last_sync_utc);
} dsp_time_port_t;

typedef struct {
    const dsp_kv_t *kv;
    const dsp_tz_t *tz;
    const dsp_pump_port_t *pump;   /* passed to the Dosing Service only */
    uint32_t hw_max_runtime_ms;    /* 0 = unknown: never actuates */
    const dsp_power_port_t *power;
    const dsp_clock_port_t *clock;
    const dsp_time_port_t *time;
    dsp_wifi_manager_t *wifi;      /* initialized by the platform; NULL = no connectivity */
    const dsp_time_source_t *ntp;
    uint32_t sntp_timeout_ms;
    dsp_https_transport_t *https;  /* initialized by the platform; NULL = no connectivity */
    uint32_t https_budget_ms;
    const dsp_ble_port_t *ble;     /* NULL = no service mode */
    const dsp_monotonic_t *mono;   /* required with ble: session lifetime */
    const dsp_app_sync_t *sync;    /* optional queue lock (see dsp_app_post) */
    uint32_t random;               /* non-zero entropy for the notification epoch */
    int64_t max_sleep_s;           /* 0 = DSP_POWER_MAX_SLEEP_DEFAULT_S */
    uint32_t service_idle_s;       /* 0 = DSP_APP_SERVICE_IDLE_DEFAULT_S */
    uint32_t service_max_s;        /* 0 = DSP_APP_SERVICE_MAX_DEFAULT_S (>= idle) */
} dsp_app_config_t;

typedef enum {
    DSP_BLE_END_NONE = 0,
    DSP_BLE_END_IDLE,       /* no client within the idle timeout */
    DSP_BLE_END_MAX,        /* maximum total duration reached */
    DSP_BLE_END_COMMAND,    /* END_SESSION */
    DSP_BLE_END_RESUME,     /* resume refused by the platform */
    DSP_BLE_END_CLOCK,      /* monotonic clock went backwards (ends conservatively) */
    DSP_BLE_END_START_FAILED,
} dsp_app_ble_end_t;

/* ---- Report --------------------------------------------------------------------------- */

typedef struct {
    dsp_wake_cause_t cause;
    bool retained_valid;
    bool pump_unconfirmed;      /* SAFE_OFF could not confirm OFF */
    bool inhibited;             /* fail-safe limit reached: no dosing in this wake */
    bool degraded;              /* LOAD failed */
    dsp_storage_status_t open_status;
    dsp_dosing_status_t recover_status;
    dsp_dosing_recovery_t recovery;
    size_t dose_passes;
    size_t dosed;               /* automatic pump runs that finished */
    size_t pump_failures;
    size_t manual_doses;
    bool pump_fault;            /* dosing ended for this wake after a pump fault */
    bool dosing_degraded;       /* a storage failure in the Dosing Service */
    size_t conn_runs;
    dsp_conn_outcome_t conn[2];
    bool ble_session;
    size_t ble_suspends;
    uint8_t ble_end;            /* dsp_app_ble_end_t */
    size_t commands;
    size_t responses;
    size_t ops;                 /* M1.10b.1 operations called (any result) */
    size_t ops_ok;
    bool op_storage_fault;      /* an operation's write failed (dosing ended for the wake) */
    bool synced_ok;             /* SYNC left nothing dirty */
    dsp_wake_plan_t plan;
    uint32_t shutdown_attempts;
    uint32_t shutdown_failures; /* DSP_POWER_CHK_* of the last attempt (0 = confirmed) */
    bool retained_saved;
    bool slept;
    bool fail_safe;
    size_t trace_len;
    uint8_t trace[DSP_APP_TRACE_MAX]; /* dsp_app_stage_t in execution order */
} dsp_app_report_t;

typedef struct dsp_app dsp_app_t;

/* ---- M1.10b.1: configuration, schedules and phone time ------------------------------------
 *
 * Operations for the future BLE dispatcher (M1.10c/d), which will decode a frame with
 * dsp_bleproto, authorize it in the security layer and call exactly one of these. They run
 * in the APPLICATION TASK only, inside an active service session (between the BLE port's
 * start and stop, i.e. from the dispatcher that handle_command will host); outside a session
 * they return DSP_OP_BUSY. They reuse the existing modules (dsp_config, dsp_schedule_repo,
 * dsp_timekeeper, dsp_storage) and never drive the pump.
 *
 * Authorization is an explicit input produced by the security layer (M1.10c). Until that
 * layer exists nothing produces an owner authorization, so every operation fails closed:
 * a NULL authorization or any role other than OWNER returns DSP_OP_FORBIDDEN before any
 * storage access.
 *
 * A storage WRITE failure (old or new value durable) answers DSP_OP_STORAGE and ends dosing
 * for the rest of the wake (as a Dosing Service storage failure does); the schedule
 * repository is re-read at once so that planning keeps using the durable set.
 *
 * Results map 1:1 to BLE protocol v1 error codes (docs/BLE_PROTOCOL.md 5.1); detail and aux
 * travel as the error body's `detail` and `aux`. */

typedef enum {
    DSP_APP_ROLE_NONE = 0,
    DSP_APP_ROLE_OWNER = 1,
} dsp_app_role_t;

#define DSP_APP_PHONE_SLOTS 4

typedef struct {
    uint8_t role; /* dsp_app_role_t, set by the security layer after a verified session */
    uint8_t slot; /* enrolled phone slot, 0..DSP_APP_PHONE_SLOTS - 1 */
} dsp_app_auth_t;

typedef enum {
    DSP_OP_OK = 0,
    DSP_OP_FORBIDDEN, /* no owner authorization (fail closed)                 wire FORBIDDEN */
    DSP_OP_BUSY,      /* not inside a service session                         wire BUSY */
    DSP_OP_REFUSED,   /* state precondition: detail DSP_OP_R_*, aux the refused
                         field bits                                           wire REFUSED */
    DSP_OP_LIMIT,     /* detail DSP_DOSING_L_DOSE, aux max_dose_ul             wire LIMIT */
    DSP_OP_CAPACITY,  /* schedule capacity reached, aux = 16                   wire LIMIT */
    DSP_OP_INVALID,   /* detail = DSP_CONFIG_E_* / DSP_SCHED_E_* bits (0 = argument)
                                                                              wire INVALID */
    DSP_OP_CONFLICT,  /* stale revision: detail = current revision            wire CONFLICT */
    DSP_OP_NOT_FOUND, /*                                                      wire NOT_FOUND */
    DSP_OP_STORAGE,   /* a write failed; durable state is the old or the new value
                                                                              wire STORAGE */
    DSP_OP_DEGRADED,  /* storage not loaded or unreadable                      wire DEGRADED */
    DSP_OP_DUPLICATE, /* schedule ID active (aux 0) or deleted/executed before (aux 1)
                                                                              wire DUPLICATE */
} dsp_app_op_status_t;

enum {
    DSP_OP_R_READY = 1,          /* field changeable only while SERVICE_REQUIRED */
    DSP_OP_R_REFILL_PENDING = 2, /* an interrupted refill must be completed first */
    DSP_OP_R_CLOCK = 3,          /* time not trusted: create / timing edit / re-enable need a
                                    trusted clock for the edit floor (send SET_TIME first) */
};

typedef struct {
    dsp_app_op_status_t status;
    uint32_t detail;
    uint32_t aux;
} dsp_app_op_result_t;

/* Configuration (M1). *out: the stored record; *hw_max_runtime_ms (optional): the
 * platform's hardware cutoff (read-only, as in the protocol's Config record). */
dsp_app_op_result_t dsp_app_get_config(dsp_app_t *app, const dsp_app_auth_t *auth,
                                       dsp_config_t *out, uint32_t *hw_max_runtime_ms);

/* Revisioned partial update: expected_revision must equal the stored revision (else
 * CONFLICT with the current one; note that every dose also advances it, because the
 * reservoir estimate lives in this record). Only the fields in field_mask
 * (DSP_CONFIG_F_*) change; all others, including the read-only ones, are preserved.
 * Calibration and limits (DSP_CONFIG_F_SERVICE_ONLY) only while SERVICE_REQUIRED; while
 * READY the reservoir capacity may change but neither from nor to 0. Readiness is never
 * changed (a configuration change never enables dosing). Strictly validated
 * (dsp_config_apply_update). One atomic record write with read-back. *new_revision
 * (optional) on success. */
dsp_app_op_result_t dsp_app_set_config(dsp_app_t *app, const dsp_app_auth_t *auth,
                                       uint32_t expected_revision, uint16_t field_mask,
                                       const dsp_config_t *values, uint32_t *new_revision);

/* Schedules (M2). Pages of DSP_APP_SCHEDULE_PAGE entries in stored order. */
#define DSP_APP_SCHEDULE_PAGE 4

typedef struct {
    uint32_t set_revision;
    uint8_t total; /* schedules stored */
    uint8_t page;
    uint8_t n;     /* entries on this page */
    struct {
        char id[DSP_SCHEDULE_ID_MAX_LEN + 1];
        uint32_t revision;
    } items[DSP_APP_SCHEDULE_PAGE];
} dsp_app_schedule_page_t;

/* page beyond the last one -> INVALID (page 0 of an empty set is valid and empty). */
dsp_app_op_result_t dsp_app_list_schedules(dsp_app_t *app, const dsp_app_auth_t *auth,
                                           uint8_t page, dsp_app_schedule_page_t *out);

/* *out includes the name; not_before is device-internal (never sent over BLE). */
dsp_app_op_result_t dsp_app_get_schedule(dsp_app_t *app, const dsp_app_auth_t *auth,
                                         const char *id, dsp_schedule_t *out);

/*
 * Create / update / delete through dsp_schedule_repo (IDs never reused, tombstones,
 * execution history untouched, one atomic write of the whole set). In addition:
 *  - create: revision must be 1; update: s->revision must be expected_revision + 1 and
 *    expected_revision the stored one (else CONFLICT with the current revision).
 *  - s->not_before is ignored on input. The device sets the schedule's edit floor (D16,
 *    approved) to the trusted current time + 1 s on create, on an update that changes any
 *    timing field (start date, time, kind, interval, weekdays, end date) and when an update
 *    re-enables the schedule; during a rollback hold to the highest trusted time + 1 s if
 *    later; otherwise it keeps the stored floor (never lowered). Occurrences before the
 *    floor are never executed: an edit never makes an already elapsed occurrence
 *    executable, and deleting and recreating a schedule cannot repeat today's dose. With an
 *    untrusted clock these three edits are REFUSED (DSP_OP_R_CLOCK); name, volume and
 *    disable edits and deletion stay possible. Claimed occurrences stay blocked by the M1.2
 *    watermark.
 *  - amount_ul above a configured max_dose_ul -> LIMIT (detail DSP_DOSING_L_DOSE,
 *    aux = max_dose_ul); 0 = no limit configured yet (dosing refuses then anyway).
 * The next dose and wake are always derived from the durable set (nothing is cached), so
 * a successful change takes effect for the session's dose checks and the wake plan at once.
 */
dsp_app_op_result_t dsp_app_create_schedule(dsp_app_t *app, const dsp_app_auth_t *auth,
                                            const dsp_schedule_t *s, uint32_t *set_revision);
dsp_app_op_result_t dsp_app_update_schedule(dsp_app_t *app, const dsp_app_auth_t *auth,
                                            uint32_t expected_revision, const dsp_schedule_t *s,
                                            uint32_t *revision, uint32_t *set_revision);
dsp_app_op_result_t dsp_app_delete_schedule(dsp_app_t *app, const dsp_app_auth_t *auth,
                                            const char *id, uint32_t expected_revision,
                                            uint32_t *set_revision);

/* Phone time (M3): dsp_timekeeper_on_phone_time, then dsp_time_apply (durable before
 * effective). status OK carries the decision (DSP_PHONE_TIME_*) and, when accepted, the
 * applied offset (0 when the previous clock was untrusted). STORAGE: the metadata could
 * not be made durable, nothing applied. Accepted time never executes missed doses: the
 * initial-sync and forward-step floors apply, and dose checks use the new clock at once. */
typedef struct {
    uint8_t decision; /* dsp_phone_time_decision_t */
    int64_t offset_s;
} dsp_app_time_result_t;

dsp_app_op_result_t dsp_app_set_phone_time(dsp_app_t *app, const dsp_app_auth_t *auth,
                                           int64_t phone_utc_ms, uint32_t uncertainty_ms,
                                           dsp_app_time_result_t *out);

/* Caller-owned, statically allocated application state. Treat as opaque. */
struct dsp_app {
    const dsp_app_config_t *cfg;
    dsp_storage_t st;
    dsp_history_store_t hist;
    dsp_exec_t ex;
    dsp_notify_t notify;
    dsp_dosing_t dosing;
    dsp_schedule_repo_t repo;
    dsp_timekeeper_t tk;
    dsp_time_service_t time_svc;
    dsp_connectivity_t conn;
    dsp_power_retained_t retained;
    bool loaded;     /* LOAD completed */
    bool ble_active;
    bool pump_fault;
    bool inhibited;
    bool accepting; /* queue open for producers (service session active) */
    dsp_app_cmd_t q[DSP_APP_CMDQ_CAPACITY];
    size_t q_head;
    size_t q_count;
    bool storage_fault; /* M1.10b.1: an operation's write failed: no dosing in this wake */
    dsp_app_report_t rep;
};

/* Runs one complete wake (ends with the sleep or fail-safe port call). */
dsp_app_report_t dsp_app_run_wake(dsp_app_t *app, const dsp_app_config_t *cfg);

/* Producer side of the bounded command queue. false: queue full, invalid command or no
 * session active -> the platform answers "busy".
 *
 * Synchronization contract:
 *  - This is the ONLY function another task may call. It touches nothing but the queue
 *    (q[], q_head, q_count) and the session flag; it never reads or writes domain state.
 *  - The queue is accessed under cfg->sync (lock/unlock) on both sides: here, and where the
 *    application task pops a command. With cfg->sync == NULL the caller guarantees that the
 *    producer runs in the application task (host tests: the BLE fake posts from inside
 *    wait(), synchronously), so there is no concurrent access.
 *  - Posting is accepted only while a service session is active (between the BLE port's
 *    start and stop); the application resets the queue only outside a session. On hardware
 *    the platform must also not call it before dsp_app_run_wake() has started the session.
 *  - Commands are copied; responses go back only through the BLE port's respond(), called
 *    from the application task.
 *  The FreeRTOS lock and the BLE task belong to M1.9c / the BLE milestone. */
bool dsp_app_post(dsp_app_t *app, const dsp_app_cmd_t *cmd);

#ifdef __cplusplus
}
#endif

#endif /* DSP_APP_H */
