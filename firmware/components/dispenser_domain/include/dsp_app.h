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
#include "dsp_auth.h"
#include "dsp_bleproto.h"
#include "dsp_dosing.h"
#include "dsp_link.h"
#include "dsp_owner.h"
#include "dsp_power.h"
#include "dsp_schedule_repo.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_APP_CMDQ_CAPACITY 4 /* opaque frames (M1.10c.1): 4 x one maximum frame */
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

/* ---- Service-mode queue (bounded) ------------------------------------------------------- *
 *
 * M1.10c.1: the queue carries OPAQUE protocol frames, transport events and button events only.
 * The M1.9b command structs (STATUS, MANUAL_DOSE, REFILL, SET_READINESS, END_SESSION) are
 * retired: every request arrives as a protocol frame and is decoded, gated and (from M1.10c.2,
 * after authentication) dispatched in the application task; every privileged operation needs a
 * security-layer-issued authorization (see the operations below). */
typedef enum {
    DSP_QI_FRAME = 1,     /* one complete frame of connection `gen` (reassembled by the BLE task) */
    DSP_QI_BUTTON = 4,    /* a debounced button event (recovery confirmation) */
} dsp_app_qkind_t;
/* M1.10d (decision T2): connection events are NOT queue items. The transport reports them into
 * an authoritative connection-state snapshot beside the queue (dsp_app_link_t), which is never
 * refused for lack of space; the application reconciles with it before every queue item. */

typedef struct {
    uint8_t kind;  /* dsp_app_qkind_t */
    uint32_t gen;
    uint16_t mtu;
    uint16_t len;
    dsp_button_event_t button;
    uint8_t bytes[DSP_BLE_FRAME_MAX];
} dsp_app_qitem_t;

/* ---- Ports ---------------------------------------------------------------------------- */

/* BLE service mode (no NimBLE/GATT in this milestone). The platform BLE task is a byte pipe: it
 * posts complete frames and connection events (dsp_app_post_frame / _conn_event) and never touches
 * domain state; it sends the frames handed to send_frame (fragmenting them) and serves the Info
 * record and advertising payload published by publish_info.
 *
 * Session lifetime (approved policy), measured on the MONOTONIC clock (never wall time):
 *  - no client connected: the session ends DSP_APP_SERVICE_IDLE_DEFAULT_S (configurable)
 *    after it started or after the last client disconnected;
 *  - while a client is connected the session stays open;
 *  - it always ends at the maximum total duration (DSP_APP_SERVICE_MAX_DEFAULT_S, configurable);
 *  - a due scheduled dose suspends it (connection closed, "busy: dosing"); it resumes only
 *    while the deadline still permits, and a connection closed by the suspension restarts the
 *    idle timeout at resume. Suspension also ends the connection's security session. */
typedef struct {
    void *ctx;
    bool (*start)(void *ctx);    /* advertising on; false = unavailable */
    bool (*connected)(void *ctx); /* a client is connected (platform connection state) */
    void (*suspend)(void *ctx);  /* advertising off, connection closed ("busy: dosing") */
    bool (*resume)(void *ctx);   /* false = ends the session */
    void (*stop)(void *ctx);     /* idempotent: radio off */
    bool (*is_off)(void *ctx);   /* checklist confirmation; NULL = unconfirmed */
    /* Blocks until something was posted or max_ms elapsed. */
    void (*wait)(void *ctx, uint32_t max_ms);
    /* One complete protocol frame for connection generation `gen` (the transport fragments it).
     * M1.10d: the transport refuses (false) unless `gen` is its current, open connection, so a
     * frame of an ended connection can never reach a newer one. */
    bool (*send_frame)(void *ctx, uint32_t gen, const uint8_t *frame, size_t len);
    /* Drops connection `gen` (security close) if it is still the current one; advertising
     * continues. */
    void (*disconnect)(void *ctx, uint32_t gen);
    /* Optional: the public Info record and advertising payload (protocol 7.1). */
    void (*publish_info)(void *ctx, const uint8_t info[DSP_BLE_INFO_LEN],
                         const uint8_t advert[DSP_BLE_ADVERT_LEN]);
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

/* Registration of the security layer (M1.10c, dsp_blesec). NULL until it exists: then no
 * ownership authorization can be produced at all (every dsp_app_sec_* call and every owner
 * operation is FORBIDDEN). random: the hardware RNG (esp_fill_random with RF on), used for
 * session IDs, authorization tokens, handshake nonces and invite tokens; false = unavailable. */
struct dsp_app;
typedef struct {
    void *ctx;
    bool (*random)(void *ctx, uint8_t *out, size_t len);
    /* Optional (M1.10c.2): HELLO/prologue device hash (SHA-256 based, docs/BLE_PROTOCOL.md);
     * id_pub NULL = unclaimed or damaged credentials (device-ID form). NULL: 8 zero bytes. */
    bool (*device_id_hash)(void *ctx, const uint8_t *id_pub, uint8_t out[8]);
    /* Optional security session (M1.10c.2a, dsp_blesec). When on_frame is set, every frame of
     * the current connection goes to it instead of the M1.10c.1 gate (dsp_link); it answers
     * through dsp_app_sec_send / dsp_app_sec_drop. All hooks run in the application task.
     *  on_connection: a connection opened (after dsp_app_sec_connection_begin) or is closing
     *                 (before dsp_app_sec_connection_end; also on BLE suspension and session end);
     *  on_tick:       every BLE loop iteration; returns the next monotonic deadline (0 = none);
     *  on_suspend:    a pump run is about to suspend BLE (best-effort EV_SUSPENDING_FOR_DOSE). */
    void (*on_connection)(void *ctx, struct dsp_app *app, bool open);
    void (*on_frame)(void *ctx, struct dsp_app *app, const uint8_t *frame, size_t len);
    uint64_t (*on_tick)(void *ctx, struct dsp_app *app, uint64_t now_ms);
    void (*on_suspend)(void *ctx, struct dsp_app *app);
    /* M1.10d (decision T3): the transport discarded a malformed fragment sequence of the current
     * connection. Before authentication the hook may answer a bounded plain MALFORMED (protocol
     * 10.6 gate); in a handshake or after authentication it must close the connection (never a
     * plaintext error). NULL with on_frame set: the connection is closed. */
    void (*on_rx_fault)(void *ctx, struct dsp_app *app);
} dsp_app_security_t;

/* Public identity for the Info record and the HELLO hash (platform: factory MAC, version). */
typedef struct {
    uint8_t device_id[6];
    uint8_t fw_major, fw_minor;
    uint16_t fw_patch;
} dsp_app_identity_t;

/* Session-local limits (M1.10b.3, v1: reset by a reconnect or a reset; documented). */
#define DSP_APP_SESSION_OPS_MAX_DEFAULT 256u
#define DSP_APP_SESSION_WRITES_MAX_DEFAULT 48u /* a full setup: 16 schedules + config + time */
#define DSP_APP_WRITES_PER_MINUTE_DEFAULT 16u
#define DSP_APP_SESSION_MANUAL_MAX_DEFAULT 8u

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
    dsp_https_transport_t *https;  /* initialized by the platform; NULL = no notifications (time-only with wifi+ntp) */
    uint32_t https_budget_ms;
    const dsp_ble_port_t *ble;     /* NULL = no service mode */
    const dsp_monotonic_t *mono;   /* required with ble: session lifetime */
    const dsp_app_sync_t *sync;    /* optional queue lock (see dsp_app_post) */
    uint32_t random;               /* non-zero entropy for the notification epoch */
    int64_t max_sleep_s;           /* 0 = DSP_POWER_MAX_SLEEP_DEFAULT_S */
    uint32_t service_idle_s;       /* 0 = DSP_APP_SERVICE_IDLE_DEFAULT_S */
    uint32_t service_max_s;        /* 0 = DSP_APP_SERVICE_MAX_DEFAULT_S (>= idle) */
    /* M1.10b.3 */
    const dsp_app_security_t *security; /* NULL = no security layer (no ownership changes) */
    const dsp_app_identity_t *identity; /* optional; NULL = zeros */
    uint32_t recovery_hold_ms;     /* wake input: verified continuous button hold from the
                                      first sample after this boot (button driver, M1.10d) */
    uint16_t manual_decisions_per_day; /* 0 = DSP_MANUAL_DECISIONS_PER_DAY_DEFAULT (build-time
                                          configuration, never changed over BLE) */
    uint16_t session_ops_max;      /* 0 = DSP_APP_SESSION_OPS_MAX_DEFAULT */
    uint16_t session_writes_max;   /* 0 = DSP_APP_SESSION_WRITES_MAX_DEFAULT */
    uint16_t writes_per_minute;    /* 0 = DSP_APP_WRITES_PER_MINUTE_DEFAULT */
    uint16_t session_manual_max;   /* 0 = DSP_APP_SESSION_MANUAL_MAX_DEFAULT */
    /* M1.10d (decision T7), BUILD-ONLY hardware bring-up until the button driver (M10): a
     * power-on wake counts as physical presence like a button wake (service window and the
     * security layer's session precondition). Authentication, the entropy prerequisite and
     * every authorization rule are unchanged. The firmware build refuses it together with the
     * production endpoint; false everywhere else. */
    bool service_on_power_on;
    /* Phase C: DSP_EVF_PUMP_SIMULATED / DSP_EVF_PUMP_BENCH_GPIO, marking every dose event of this
     * build in the user-visible history (0 = neither). Build-time, never changed over BLE. */
    uint8_t event_pump_flags;
} dsp_app_config_t;

typedef enum {
    DSP_BLE_END_NONE = 0,
    DSP_BLE_END_IDLE,       /* no client within the idle timeout */
    DSP_BLE_END_MAX,        /* maximum total duration reached */
    DSP_BLE_END_COMMAND,    /* authorized owner END_SESSION (dsp_app_end_service_session) */
    DSP_BLE_END_RESUME,     /* resume refused by the platform */
    DSP_BLE_END_CLOCK,      /* monotonic clock went backwards (ends conservatively) */
    DSP_BLE_END_START_FAILED,
    DSP_BLE_END_REVOKED,    /* M1.10b.3: the session's own phone slot was removed */
} dsp_app_ble_end_t;

/* M1.10b.3: credential state (docs/OWNERSHIP_DESIGN.md 3.1). */
typedef enum {
    DSP_CRED_UNKNOWN = 0,   /* storage not loaded */
    DSP_CRED_UNCLAIMED,     /* valid record, no owner */
    DSP_CRED_CLAIMED,       /* valid record, >= 1 phone, no key fault */
    DSP_CRED_DEGRADED,      /* missing / corrupt / unsupported / KEY_FAULT: dosing blocked */
} dsp_cred_state_t;

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
    size_t frames_in;           /* M1.10c.1: frames of the current connection processed */
    size_t frames_out;          /* frames sent */
    size_t frames_stale;        /* frames of another connection generation, dropped */
    size_t link_closes;         /* connections closed by the protocol gate */
    size_t rx_faults;           /* M1.10d: fragment errors reported by the transport */
    size_t links_opened;        /* M1.10d: connection generations opened by the application */
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
    /* M1.10b.3 */
    uint8_t cred_state;         /* dsp_cred_state_t after LOAD (and after ownership changes) */
    bool cred_inhibit;          /* dosing blocked by the credentials gate in this wake */
    bool key_fault;             /* an identity key fault was reported in this wake */
    bool recovery_armed;        /* the recovery arm was taken in this wake */
    bool overlays_cleared;      /* invite/arm cleared by a reset, a clock step or an anomaly */
    size_t owner_changes;       /* durable ownership changes in this wake */
    size_t rate_limited;        /* operations answered BUSY/RATE_LIMITED */
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
 * storage access. NOTE: these operations do not yet verify that the authorization was ISSUED
 * (session ID, token, epoch) as the M1.10b.3 ownership operations do; M1.10c must add that
 * check before any of them is reachable over BLE (see the integration blocker above).
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
    /* M1.10b.3: evidence of an authorization ISSUED by the Application Service to the security
     * layer (dsp_app_sec_lookup / enroll / recovery). Ownership operations require it to match
     * the issued one for the current session and ownership epoch; the M1.10b.1/b.2 operations
     * do not check it yet (M1.10c routes every request through an issued authorization). */
    uint32_t session_id;
    uint64_t token;
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
    DSP_OP_DUPLICATE, /* schedule ID active (aux 0) or deleted/executed before (aux 1);
                         manual dose: already decided (detail state, aux detail)
                                                                              wire DUPLICATE */
    DSP_OP_NOT_COVERED, /* manual dose: the reservoir estimate does not cover it
                                                                              wire NOT_COVERED */
    DSP_OP_SLOTS_FULL,  /* M1.10b.3: four phones enrolled                     wire SLOTS_FULL */
} dsp_app_op_status_t;

/* DSP_OP_BUSY details (M1.10b.3; detail 0 = not inside a service session). */
enum {
    DSP_OP_B_SESSION = 0,
    DSP_OP_B_RATE_LIMITED = 1, /* aux = seconds until a retry can succeed (0 = unknown) */
    DSP_OP_B_CONFIRMING = 2,   /* a recovery confirmation is pending on the device */
};

enum {
    DSP_OP_R_READY = 1,          /* field changeable only while SERVICE_REQUIRED */
    DSP_OP_R_REFILL_PENDING = 2, /* an interrupted refill must be completed first */
    DSP_OP_R_CLOCK = 3,          /* time not trusted: create / timing edit / re-enable need a
                                    trusted clock for the edit floor (send SET_TIME first) */
    DSP_OP_R_LAST_SLOT = 4,      /* M1.10b.3: the last enrolled phone cannot be removed */
    DSP_OP_R_NOT_CLAIMED = 5,    /* M1.10b.3: ownership state does not allow it */
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

/* Phase C: event history (GET_HISTORY, docs/BLE_PROTOCOL.md 10.7). Newest first: up to max
 * (1..DSP_APP_HISTORY_PAGE) events with seq < before_seq (0 = from the newest). Read-only: no
 * write budget, never changes state. newest/oldest: the seq range currently held (0 = empty);
 * faults: since this boot the log saw unreadable slots or failed appends (events may be missing;
 * not persisted, a failure in an earlier wake is not reported). */
#define DSP_APP_HISTORY_PAGE 4
typedef struct {
    uint32_t newest_seq, oldest_seq;
    bool faults;
    uint8_t n;
    dsp_event_t items[DSP_APP_HISTORY_PAGE];
} dsp_app_history_page_t;
dsp_app_op_result_t dsp_app_get_history(dsp_app_t *app, const dsp_app_auth_t *auth,
                                        uint32_t before_seq, uint8_t max,
                                        dsp_app_history_page_t *out);

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

/* ---- M1.10b.2: manual dose with durable anti-replay -------------------------------------
 *
 * The SOLE manual-dose entry point. Application task only, inside a service session, with an
 * owner authorization (fail closed otherwise, exactly like the M1.10b.1 operations). The
 * phone slot is auth->slot (the security layer's verified context); nothing in the request
 * selects it. seq is the phone's request ID for its slot (BLE MANUAL_DOSE `seq`).
 *
 * Results:
 *  - OK: executed; out->outcome 0 DONE / 1 PUMP_FAILED (protocol MANUAL_DOSE response),
 *    volume and runtime.
 *  - DUPLICATE: already decided (seq <= the slot's high-water mark) - never executed again;
 *    detail = result state (DSP_MRES_*: PENDING..REFUSED, or UNAVAILABLE when no longer
 *    retained), aux = result detail.
 *  - REFUSED / LIMIT / NOT_COVERED: refused now, durably (the ID can never execute later);
 *    detail = gates / limits.
 *  - INVALID: seq 0, seq > 2^63 - 1 or volume 0 - not consumed.
 *  - STORAGE: the decision could not be made durable: nothing executed (pump never started);
 *    a retry is decided by the durable state.
 *  - DEGRADED: the dosing state is unavailable (storage not loaded or failed earlier in this
 *    wake): nothing executed, nothing consumed.
 * Refusals decided by the Application Service itself (a pump fault earlier in this wake, the
 * fail-safe inhibit) are durable too (refusal code DEGRADED).
 * A fresh request suspends BLE first, runs a due scheduled dose first (Q8), decides, runs the
 * pump if accepted, then resumes BLE if the session deadline still permits (else the session
 * ends). Already-decided and invalid requests never touch BLE or the pump. out->floor is the
 * slot's next acceptable request ID afterwards. */
typedef struct {
    uint8_t outcome;     /* 0 DONE, 1 PUMP_FAILED (status OK only) */
    uint32_t volume_ul;
    uint32_t runtime_ms;
    uint8_t state;       /* DSP_MRES_* of this request after the call */
    uint32_t detail;
    uint64_t floor;      /* the slot's next acceptable seq (protocol Status manual_floor) */
} dsp_app_manual_result_t;

dsp_app_op_result_t dsp_app_manual_dose(dsp_app_t *app, const dsp_app_auth_t *auth, uint64_t seq,
                                        uint32_t volume_ul, dsp_app_manual_result_t *out);

/* GET_RESULT: OK with the state and detail (DSP_MRES_NOT_DECIDED for an ID above the high-water
 * mark, DSP_MRES_UNAVAILABLE for a decided ID no longer retained); no side effect. */
dsp_app_op_result_t dsp_app_get_result(dsp_app_t *app, const dsp_app_auth_t *auth, uint64_t seq,
                                       uint8_t *state, uint32_t *detail);

/* The authenticated slot's counter floor (manual_hw + 1, 2^63 when exhausted), reported in the
 * Status record at session start so a phone that lost its counter resynchronizes. Only for an
 * owner authorization (never through HELLO, Info or advertising). */
dsp_app_op_result_t dsp_app_manual_floor(dsp_app_t *app, const dsp_app_auth_t *auth,
                                         uint64_t *floor);

/* ---- M1.10c.1: status, diagnostics, refill, readiness (formerly unauthenticated queue commands)
 *
 * Same preconditions as every operation (session, ISSUED authorization, storage). The records are
 * the protocol's (docs/BLE_PROTOCOL.md 7.2, 7.3); manual_floor is the authorized slot's. */
dsp_app_op_result_t dsp_app_get_status(dsp_app_t *app, const dsp_app_auth_t *auth,
                                       dsp_ble_status_rec_t *out);
dsp_app_op_result_t dsp_app_get_diagnostics(dsp_app_t *app, const dsp_app_auth_t *auth,
                                            dsp_ble_diag_rec_t *out);
dsp_app_op_result_t dsp_app_ack_rejections(dsp_app_t *app, const dsp_app_auth_t *auth);
/* Decision C10: ends the whole service window (BLE stops after the current item). Only with an
 * authorization issued for this connection (an unauthenticated END_SESSION closes only its own
 * connection, dsp_link). Allowed while a recovery confirmation is pending and without loaded
 * storage; not rate limited. Failed handshakes never end the window. */
dsp_app_op_result_t dsp_app_end_service_session(dsp_app_t *app, const dsp_app_auth_t *auth);
/* User-confirmed refill (dsp_dosing_refill): INVALID for bad values, STORAGE when not durable.
 * *status (optional) afterwards. */
dsp_app_op_result_t dsp_app_refill(dsp_app_t *app, const dsp_app_auth_t *auth,
                                   uint32_t capacity_ul, uint32_t remaining_ul,
                                   dsp_ble_status_rec_t *status);
/* READY only if every gate except the clock is open: REFUSED, detail = gates, aux = pump
 * problems; SERVICE_REQUIRED always. */
dsp_app_op_result_t dsp_app_set_readiness(dsp_app_t *app, const dsp_app_auth_t *auth,
                                          uint8_t readiness, dsp_ble_status_rec_t *status);

/* ---- M1.10b.3: ownership (docs/OWNERSHIP_DESIGN.md, approved revision 2) --------------------
 *
 * Owner operations for the future dispatcher. Same session/role preconditions as above, and in
 * addition the authorization must be one ISSUED by this service in the current session and
 * ownership epoch (auth->session_id, auth->token): a hand-made {OWNER, slot} is FORBIDDEN.
 * Without a registered security layer (cfg->security == NULL, every build before M1.10c) no
 * such authorization can exist. Writes are single atomic "auth" record writes. */
typedef struct {
    uint8_t slot;
    uint8_t name_len;
    char name[DSP_AUTH_NAME_MAX];
    dsp_utc_t enrolled_at;
    dsp_utc_t last_seen;
    bool is_self;
} dsp_app_phone_t;

dsp_app_op_result_t dsp_app_phones_list(dsp_app_t *app, const dsp_app_auth_t *auth,
                                        dsp_app_phone_t out[DSP_APP_PHONE_SLOTS], uint8_t *n);
/* Removes one slot (also the caller's own; never the last). epoch + 1, so every invite and arm
 * of the previous epoch becomes invalid. *session_ends: the caller's own slot was removed (the
 * session ends after the response). */
dsp_app_op_result_t dsp_app_phone_remove(dsp_app_t *app, const dsp_app_auth_t *auth, uint8_t slot,
                                         bool *session_ends);
/* One active invite (replaces an older one): 16-byte token from the device RNG, valid for
 * DSP_OWNER_INVITE_S, bound to the current epoch, kept in RTC memory only (saved at once). */
dsp_app_op_result_t dsp_app_invite_create(dsp_app_t *app, const dsp_app_auth_t *auth,
                                          uint8_t token_out[16], uint16_t *expires_s);
dsp_app_op_result_t dsp_app_invite_cancel(dsp_app_t *app, const dsp_app_auth_t *auth);
dsp_app_op_result_t dsp_app_recovery_cancel(dsp_app_t *app, const dsp_app_auth_t *auth);

/* Producer side for button events (button driver, M10): queued like dsp_app_post; consumed by
 * the recovery confirmation only (FR-026 gestures are not part of this milestone). */
bool dsp_app_post_button(dsp_app_t *app, const dsp_button_event_t *ev);

/* Service-session lifetime state (monotonic milliseconds). Internal. */
typedef struct {
    uint64_t start_ms;
    uint64_t max_ms;           /* absolute end of the maximum total duration */
    uint64_t idle_deadline_ms; /* absolute end of the idle timeout (no client) */
    uint64_t idle_ms;
    bool was_connected;
    uint8_t end;               /* dsp_app_ble_end_t */
    /* M1.10b.3: identity and session-local limits */
    uint32_t session_id;       /* random per CONNECTION (security session, M1.10c.1); 0 = none */
    uint32_t conn_gen;         /* last generation opened by the application (0 = none); stays
                                  set after its close so that it is never reopened */
    uint8_t faults_seen;       /* M1.10d: fragment errors of conn_gen already handled */
    bool conn_active;
    uint16_t ops;
    uint16_t writes;
    uint16_t manual;
    uint16_t win_writes;
    uint64_t win_start_ms;
    bool failed_hs_counted;    /* failed_sessions written at most once per session */
} dsp_app_session_t;

/* Authoritative connection-state snapshot (M1.10d, decision T2). Written by the transport
 * (dsp_app_post_conn_event / dsp_app_post_rx_fault) under cfg->sync; read by the application in
 * one locked copy before every queue item. Internal. */
typedef struct {
    uint32_t gen;      /* newest generation reported (strictly increasing within a wake) */
    bool up;           /* that generation is connected */
    uint16_t mtu;
    uint8_t faults;    /* fragment errors of `gen` (saturating) */
} dsp_app_link_t;

/* Ownership runtime state (RAM; internal). The cache never holds the private key. */
typedef struct {
    uint8_t cred;              /* dsp_cred_state_t */
    uint32_t epoch;            /* durable owner_epoch, DSP_OWNER_EPOCH_DEGRADED when damaged */
    bool key_fault;            /* inhibits dosing for the rest of the wake even if not durable */
    dsp_auth_t cache;          /* public view of the record (id_priv always zero) */
    /* issued authorization (one per session) */
    bool authz_valid;
    uint8_t authz_slot;
    uint32_t authz_session;
    uint64_t authz_token;
    uint32_t authz_epoch;
    /* the single handshake in progress (dsp_app_sec_admit -> one commit) */
    bool hs_active;
    uint8_t hs_purpose;
    uint32_t hs_session;
    uint64_t hs_nonce;
    uint64_t hs_start_ms;
    uint32_t hs_epoch;
    /* recovery confirmation, bound to (session, transcript) */
    dsp_confirm_t confirm;
    /* raw-vs-monotonic reference for clock-step detection within the wake */
    bool ref_valid;
    dsp_utc_t ref_raw;
    uint64_t ref_mono;
} dsp_app_owner_t;

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
    dsp_app_qitem_t q[DSP_APP_CMDQ_CAPACITY];
    size_t q_head;
    size_t q_count;
    dsp_app_link_t lnk;   /* M1.10d (T2): connection snapshot, under cfg->sync */
    bool storage_fault; /* M1.10b.1: an operation's write failed: no dosing in this wake */
    dsp_app_session_t ss; /* current service session (valid while accepting) */
    dsp_app_owner_t own;  /* M1.10b.3 */
    dsp_link_t link;      /* M1.10c.1: protocol gate of the current connection */
    dsp_app_qitem_t rx;   /* the queue item being processed (not on the stack) */
    uint8_t tx[DSP_BLE_FRAME_MAX]; /* reply frame buffer */
    dsp_app_report_t rep;
    dsp_evlog_t evlog;    /* Phase C: user-visible event history (separate keys, informational) */
};

/* Runs one complete wake (ends with the sleep or fail-safe port call). */
dsp_app_report_t dsp_app_run_wake(dsp_app_t *app, const dsp_app_config_t *cfg);

/* Producer side of the bounded queue (frames) and of the connection snapshot. post_frame false:
 * queue full, invalid input or no session active -> the frame is dropped (M1.10d: no plaintext
 * busy notice; the client's retry after its timeout is served from the response cache).
 *
 * Connection events (decision T2): post_conn_event updates the snapshot and is never refused for
 * lack of space. A CONNECTED generation must be strictly greater than every generation reported
 * before in this wake (false otherwise: ignored). DISCONNECTED of the newest generation marks it
 * down; of an older one it is ignored (true). The application closes its connection whenever the
 * snapshot's generation differs from it or is down, and opens the snapshot's generation if it is
 * up and newer: a disconnect can never be hidden by a full queue, a disconnect followed by a
 * connect never preserves the old session, and frames of any other generation are stale.
 * post_rx_fault: one fragment error of generation `gen` (ignored unless it is the newest, up).
 *
 * Synchronization contract:
 *  - These and dsp_app_post_button are the ONLY functions another task may call. They touch
 *    nothing but the queue (q[], q_head, q_count), the snapshot (lnk) and the session flag;
 *    never domain state.
 *  - The queue is accessed under cfg->sync (lock/unlock) on both sides. With cfg->sync == NULL
 *    the caller guarantees that the producer runs in the application task (host tests).
 *  - Posting is accepted only while a service session is active (between the BLE port's start
 *    and stop); the application resets the queue only outside a session.
 *  - Items are copied; replies go back only through the BLE port's send_frame(), called from the
 *    application task. */
bool dsp_app_post_frame(dsp_app_t *app, uint32_t gen, const uint8_t *frame, size_t len);
bool dsp_app_post_conn_event(dsp_app_t *app, bool connected, uint32_t gen, uint16_t mtu);
bool dsp_app_post_rx_fault(dsp_app_t *app, uint32_t gen);

#ifdef __cplusplus
}
#endif

#endif /* DSP_APP_H */
