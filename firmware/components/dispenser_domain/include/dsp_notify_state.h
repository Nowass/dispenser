#ifndef DSP_NOTIFY_STATE_H
#define DSP_NOTIFY_STATE_H

/*
 * Persistent notification state (milestone M1.5): condition latches and a bounded queue,
 * stored as ONE atomic record "notify" (storage layout v3, approved 2026-11-15) so that
 * creating an event, latching its condition and enqueuing it is a single write.
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

typedef struct {
    uint32_t epoch;     /* random, set once (0 = not yet assigned); IDs = device + epoch + seq */
    uint32_t next_seq;  /* next sequence number (starts at 1) */
    uint16_t dropped;   /* one-shot events that could not be queued (reported later) */
    uint8_t flags;      /* DSP_NOTIFY_F_* */
    dsp_condition_state_t cond[DSP_COND_COUNT];
    dsp_note_t queue[DSP_NOTIFY_QUEUE_CAPACITY];
} dsp_notify_state_t;

/* Structural validity (used by the codec): known types/severities/conditions, empty
 * entries all zero, unique sequence numbers below next_seq, instants 0 or in range. */
bool dsp_notify_state_is_valid(const dsp_notify_state_t *s);

#ifdef __cplusplus
}
#endif

#endif /* DSP_NOTIFY_STATE_H */
