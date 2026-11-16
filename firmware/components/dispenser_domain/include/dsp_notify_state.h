#ifndef DSP_NOTIFY_STATE_H
#define DSP_NOTIFY_STATE_H

/*
 * Persistent notification state (milestone M1.5): condition latches and a bounded queue,
 * stored as ONE atomic record "notify" (storage layout v3, approved 2026-11-15) so that
 * creating an event, latching its condition and enqueuing it is a single write.
 * M1.7 adds (record format 2, same layout v3) persistent transport diagnostics and the
 * rejection log, in the same record: removing a rejected entry and logging it is one write.
 */

#include <stdbool.h>
#include <stdint.h>

#include "dsp_time.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_NOTIFY_QUEUE_CAPACITY 16 /* approved */

typedef enum {
    DSP_NOTE_NONE = 0,
    DSP_NOTE_RESERVOIR_LOW = 1,     /* WARNING, condition (estimate below threshold) */
    DSP_NOTE_RESERVOIR_BLOCKED = 2, /* CRITICAL, condition (FR-022: dose not covered) */
    DSP_NOTE_BATTERY_LOW = 3,       /* WARNING, condition (from a future battery monitor) */
    DSP_NOTE_DOSING_ERROR = 4,      /* CRITICAL, one-shot event */
    DSP_NOTE_DEVICE_ERROR = 5,      /* CRITICAL, condition (code = DSP_DEVERR_*) */
} dsp_note_type_t;

typedef enum {
    DSP_SEV_INFO = 1,
    DSP_SEV_WARNING = 2,
    DSP_SEV_CRITICAL = 3,
} dsp_severity_t;

/* Latched conditions (index into dsp_notify_state_t.cond). */
typedef enum {
    DSP_COND_RESERVOIR_LOW = 0,
    DSP_COND_RESERVOIR_BLOCKED,
    DSP_COND_BATTERY_LOW,
    DSP_COND_STORAGE,   /* persistent storage problem (other records than "notify") */
    DSP_COND_TIME_SYNC, /* synchronization overdue (M1.4 overdue flag) */
    DSP_COND_CLOCK,     /* clock untrusted and repeated sync attempts failed */
    DSP_COND_COUNT,
    DSP_COND_NONE = 0xFF,
} dsp_condition_t;

/* DEVICE_ERROR codes (entry.code). */
enum {
    DSP_DEVERR_STORAGE = 1,
    DSP_DEVERR_TIME_SYNC = 2,
    DSP_DEVERR_CLOCK = 3,
    DSP_DEVERR_QUEUE_OVERFLOW = 4, /* value = number of events that could not be queued */
};

/* DOSING_ERROR codes (entry.code). */
enum {
    DSP_DOSEERR_START_FAILED = 1, /* dose could not be started */
    DSP_DOSEERR_PUMP_FAILED = 2,  /* pump operation failed */
    DSP_DOSEERR_INTERRUPTED = 3,  /* execution interrupted (reset/power loss) */
    DSP_DOSEERR_STORAGE = 4,      /* required storage unavailable: not dosed */
    DSP_DOSEERR_BLOCKED = 5,      /* blocked by a critical system condition */
};
#define DSP_DOSEERR_MAX 5

/* Entry flags. */
enum {
    DSP_NOTE_F_REMINDER = 1u << 0,   /* reminder of a still unresolved episode */
    DSP_NOTE_F_TIME_VALID = 1u << 1, /* created_at is a trusted UTC time */
    DSP_NOTE_F_UNCERTAIN = 1u << 2,  /* reservoir value is an uncertain estimate */
    DSP_NOTE_F_MANUAL = 1u << 3,     /* dosing error of a manual (not automatic) dose */
};
#define DSP_NOTE_F_ALL 0x0Fu

typedef struct {
    uint32_t seq;       /* device-unique, never reused within a storage epoch; 0 = empty */
    uint8_t type;       /* dsp_note_type_t */
    uint8_t severity;   /* dsp_severity_t, fixed per type */
    uint8_t condition;  /* dsp_condition_t or DSP_COND_NONE (one-shot) */
    uint8_t flags;      /* DSP_NOTE_F_* */
    uint8_t reminder_no; /* 0 = episode start, 1.. = reminders */
    uint16_t attempts;  /* delivery attempts without acknowledgement */
    dsp_utc_t created_at; /* trusted UTC or 0 when the clock was unknown */
    uint16_t code;      /* type-specific code */
    uint32_t value;     /* type-specific value (e.g. estimated remaining µl) */
    uint32_t value2;    /* type-specific value (e.g. capacity µl) */
} dsp_note_t;

/* Condition latch flags. */
enum {
    DSP_COND_F_ACTIVE = 1u << 0,       /* episode open */
    DSP_COND_F_RAISE_PENDING = 1u << 1, /* event still to be queued (queue was full) */
};

typedef struct {
    uint8_t flags;            /* DSP_COND_F_* */
    uint8_t reminders;        /* reminders created in this episode */
    dsp_utc_t opened_at;      /* trusted time the episode opened, 0 = unknown */
    dsp_utc_t next_reminder_at; /* trusted time of the next reminder, 0 = not scheduled */
    uint32_t value;           /* latest payload (used for reminders and deferred events) */
    uint32_t value2;
} dsp_condition_state_t;

/* Header flags. */
enum {
    DSP_NOTIFY_F_ESTIMATE_UNCERTAIN = 1u << 0, /* reservoir estimate uncertain since refill */
};

/* ---- Transport diagnostics (M1.7, "notify" record format 2) ------------------------- */

/* Persistent transport problem flags (dsp_notify_state_t.tx_diag). Set by a FAILED
 * delivery whose cause is systemic; they survive restarts. Clearing conditions: every
 * flag is cleared by the next acknowledged delivery (proof that credentials, endpoint and
 * protocol work end to end) or explicitly by dsp_notify_clear_tx_diag() (future BLE). */
enum {
    DSP_TXDIAG_AUTH = 1u << 0,     /* credentials missing or rejected (HTTP 401) */
    DSP_TXDIAG_ENDPOINT = 1u << 1, /* wrong URL or method (HTTP 404/405, redirect) */
    DSP_TXDIAG_PROTOCOL = 1u << 2, /* protocol/encoder mismatch (HTTP 413/415, ambiguous 400,
                                      malformed acknowledgement, local encoding failure) */
};
#define DSP_TXDIAG_ALL 0x07u

/* Backend "error" values, closed set (never free text). Persisted: append only. */
typedef enum {
    DSP_TXERR_NONE = 0,             /* no body / not applicable */
    DSP_TXERR_MALFORMED_JSON = 1,
    DSP_TXERR_UNSUPPORTED_SCHEMA = 2,
    DSP_TXERR_MISSING_FIELD = 3,
    DSP_TXERR_UNKNOWN_FIELD = 4,
    DSP_TXERR_INVALID_FIELD = 5,
    DSP_TXERR_PAYLOAD_TOO_LARGE = 6,
    DSP_TXERR_UNSUPPORTED_MEDIA = 7,
    DSP_TXERR_NOT_FOUND = 8,
    DSP_TXERR_METHOD_NOT_ALLOWED = 9,
    DSP_TXERR_CONFLICT = 10,        /* HTTP 409: same event ID, different payload */
    DSP_TXERR_OTHER = 11,           /* a body that is not one of the above */
} dsp_txerr_t;
#define DSP_TXERR_MAX DSP_TXERR_OTHER

/* Backend "field" values, closed set. Persisted: append only. */
typedef enum {
    DSP_TXFIELD_NONE = 0,
    DSP_TXFIELD_SCHEMA_VERSION = 1,
    DSP_TXFIELD_EVENT_ID = 2,
    DSP_TXFIELD_DEVICE_ID = 3,
    DSP_TXFIELD_TYPE = 4,
    DSP_TXFIELD_SEVERITY = 5,
    DSP_TXFIELD_CODE = 6,
    DSP_TXFIELD_REMINDER_NO = 7,
    DSP_TXFIELD_CREATED_AT = 8,
    DSP_TXFIELD_UNCERTAIN = 9,
    DSP_TXFIELD_MANUAL = 10,
    DSP_TXFIELD_VALUE = 11,
    DSP_TXFIELD_VALUE2 = 12,
    DSP_TXFIELD_ATTEMPT = 13,
    DSP_TXFIELD_BODY = 14,
    DSP_TXFIELD_OTHER = 15, /* a field name outside the protocol */
} dsp_txfield_t;
#define DSP_TXFIELD_MAX DSP_TXFIELD_OTHER

/* One permanently rejected event (the queue entry was removed in the same write). */
typedef struct {
    uint32_t seq;         /* the event's sequence number (ID = device + epoch + seq); != 0 */
    uint8_t type;         /* dsp_note_type_t */
    uint8_t flags;        /* the entry's DSP_NOTE_F_* */
    uint16_t code;
    uint16_t http_status; /* 400..499 */
    uint8_t error;        /* dsp_txerr_t */
    uint8_t field;        /* dsp_txfield_t */
    uint16_t attempts;    /* started attempts including the rejected one (>= 1) */
} dsp_rejection_t;

#define DSP_REJECTION_LOG_CAPACITY 4 /* approved */

/* Rejection log (approved M1.7): the most recent rejections, oldest first.
 *  - Overflow: when full, the oldest entry is dropped. If that entry was still unread,
 *    lost_unread counts it, so information is never lost silently.
 *  - unread: number of newest entries not yet acknowledged (0..count). Acknowledging
 *    (dsp_notify_ack_rejections, future BLE) clears unread and lost_unread; the entries
 *    stay as history and total is never cleared. */
typedef struct {
    uint32_t total;       /* rejections ever recorded (saturating) */
    uint16_t lost_unread; /* unread entries dropped by overflow since the last ack (saturating) */
    uint8_t unread;
    uint8_t count;
    dsp_rejection_t entry[DSP_REJECTION_LOG_CAPACITY];
} dsp_rejection_log_t;

typedef struct {
    uint32_t epoch;     /* random, set once (0 = not yet assigned); IDs = device + epoch + seq */
    uint32_t next_seq;  /* next sequence number (starts at 1) */
    uint16_t dropped;   /* one-shot events that could not be queued (reported later) */
    uint8_t flags;      /* DSP_NOTIFY_F_* */
    dsp_condition_state_t cond[DSP_COND_COUNT];
    dsp_note_t queue[DSP_NOTIFY_QUEUE_CAPACITY];
    /* Format 2 (M1.7). A format-1 record decodes with these all zero. */
    uint8_t tx_diag;          /* DSP_TXDIAG_* */
    uint16_t tx_diag_status;  /* HTTP status that last set a flag (0 = none / local cause) */
    dsp_rejection_log_t rejections;
} dsp_notify_state_t;

/* Structural validity (used by the codec): known types/severities/conditions, empty
 * entries all zero, unique sequence numbers below next_seq, instants 0 or in range;
 * format 2: known diagnostic flags, a consistent rejection log (unused slots zero). */
bool dsp_notify_state_is_valid(const dsp_notify_state_t *s);

#ifdef __cplusplus
}
#endif

#endif /* DSP_NOTIFY_STATE_H */
