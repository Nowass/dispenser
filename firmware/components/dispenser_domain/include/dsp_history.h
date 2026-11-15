#ifndef DSP_HISTORY_H
#define DSP_HISTORY_H

/*
 * Automatic-execution history (milestone M1.2): durable deduplication records and the
 * storage contract that a future ESP-IDF NVS adapter must implement.
 *
 * Representation (approved 2026-11-15): one record per schedule ID, holding the LAST
 * claimed local date (a per-schedule watermark). An automatic occurrence
 * <scheduleId>@<localDate> is blocked if a record for that schedule has
 * local_date >= the occurrence's local date, whatever its state. This is sufficient
 * because a schedule has at most one occurrence per local date and missed occurrences
 * are never caught up, so a schedule's claimed dates only move forward. While a record
 * exists it also blocks its dates if the clock moves backwards; once a record has been
 * recycled (retention) that protection is gone for its dates (see firmware/README.md).
 *
 * This history exists for deduplication only. It is not the user-visible event journal
 * (FR-040), which keeps every attempt and outcome and is a separate, later component.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dsp_recurrence.h"
#include "dsp_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Record states (stored as uint8_t). EVERY state blocks automatic re-execution. */
enum {
    DSP_HISTORY_STARTED = 1,     /* claimed durably; pump authorized; outcome not recorded */
    DSP_HISTORY_COMPLETED = 2,   /* pump run finished and recorded */
    DSP_HISTORY_FAILED = 3,      /* claimed, then aborted or failed (no retry) */
    DSP_HISTORY_INTERRUPTED = 4, /* found STARTED after a restart: outcome uncertain */
};

/* Slot count: approved default 32; upper bound 64 keeps every scan and the duplicate
 * check bounded (one 32-bit hash per slot on the stack). */
#define DSP_HISTORY_DEFAULT_SLOTS 32
#define DSP_HISTORY_MAX_SLOTS 64

typedef struct {
    char schedule_id[DSP_SCHEDULE_ID_MAX_LEN + 1]; /* NUL-terminated, see dsp_schedule_id_is_valid */
    dsp_date_t local_date;      /* watermark: last claimed local date */
    uint8_t state;              /* DSP_HISTORY_* */
    uint32_t schedule_revision; /* revision at claim time (diagnostic; not part of identity) */
    dsp_utc_t scheduled_at;     /* scheduled instant of the claimed occurrence */
    dsp_utc_t claimed_at;       /* clock time of the claim */
} dsp_history_record_t;

/* True if the id is valid, the date valid, the state known and the instants in range. */
bool dsp_history_record_is_valid(const dsp_history_record_t *rec);

/* Field-by-field equality (no memcmp over padding). */
bool dsp_history_record_equal(const dsp_history_record_t *a, const dsp_history_record_t *b);

/* ---- Storage contract ------------------------------------------------------------
 *
 * Fixed array of `slot_count` record slots (1..DSP_HISTORY_MAX_SLOTS), each empty or
 * holding one record. Implementations (future: ESP-IDF NVS, one blob key per slot with
 * a versioned encoding) MUST guarantee:
 *
 *  1. read(slot): returns DSP_STORE_OK and the record, DSP_STORE_EMPTY for an empty slot,
 *     or DSP_STORE_ERR_READ. Never returns a partially written record.
 *  2. write(slot, rec): stages a replacement of ONE slot. DSP_STORE_OK or
 *     DSP_STORE_ERR_WRITE.
 *  3. commit(): makes staged writes durable. When it returns DSP_STORE_OK the record
 *     survives any later power loss. On DSP_STORE_ERR_COMMIT the slot holds either the
 *     old or the new record (unknown which), never a mix.
 *  4. Per-slot atomicity only: a power loss at any point leaves every slot with its old
 *     or its new value. No atomicity ACROSS slots is required or assumed; the
 *     coordinator stages and commits exactly one slot per transition.
 *  5. Not thread-safe; called only by the coordinator's single owner (see dsp_exec.h).
 */
typedef enum {
    DSP_STORE_OK = 0,
    DSP_STORE_EMPTY,
    DSP_STORE_ERR_READ,
    DSP_STORE_ERR_WRITE,
    DSP_STORE_ERR_COMMIT,
} dsp_store_status_t;

typedef struct {
    void *ctx;
    size_t slot_count;
    dsp_store_status_t (*read)(void *ctx, size_t slot, dsp_history_record_t *out);
    dsp_store_status_t (*write)(void *ctx, size_t slot, const dsp_history_record_t *rec);
    dsp_store_status_t (*commit)(void *ctx);
} dsp_history_store_t;

/* True if all function pointers are set and 1 <= slot_count <= DSP_HISTORY_MAX_SLOTS. */
bool dsp_history_store_is_valid(const dsp_history_store_t *store);

/* ---- Status codes shared by history and coordinator ---------------------------- */

typedef enum {
    DSP_EXEC_OK = 0,
    DSP_EXEC_NOT_FOUND,          /* lookup: no record for the schedule */
    DSP_EXEC_NOT_DUE,            /* clock is before the scheduled instant */
    DSP_EXEC_MISSED,             /* clock is past the due window: skipped, never caught up */
    DSP_EXEC_ALREADY_CLAIMED,    /* history blocks this occurrence (any record state) */
    DSP_EXEC_DISABLED,           /* schedule disabled */
    DSP_EXEC_BUSY,               /* another execution (automatic or manual) is active */
    DSP_EXEC_ERR_ARG,            /* NULL pointer, bad config, unknown/stale token */
    DSP_EXEC_ERR_SCHEDULE,       /* schedule fails dsp_schedule_validate */
    DSP_EXEC_ERR_OCCURRENCE,     /* not an occurrence of this schedule (invalid identity) */
    DSP_EXEC_ERR_CLOCK,          /* clock invalid or outside the supported range */
    DSP_EXEC_ERR_CAPACITY,       /* no empty or prunable slot for a new schedule record */
    DSP_EXEC_ERR_STORAGE_READ,
    DSP_EXEC_ERR_STORAGE_WRITE,
    DSP_EXEC_ERR_STORAGE_COMMIT,
    DSP_EXEC_ERR_PERSIST_CONFIRM, /* read-back after commit failed or differs */
    DSP_EXEC_ERR_INTERNAL,        /* corrupted record, duplicate records, inconsistency */
} dsp_exec_status_t;

/* ---- History operations (used by the coordinator; public for tests) ------------- */

typedef struct {
    bool found;          /* a record for the schedule exists ... */
    size_t found_slot;   /* ... in this slot */
    dsp_history_record_t found_record;
    bool has_empty;      /* first empty slot */
    size_t empty_slot;
    bool has_prunable;   /* oldest record (other schedule) with local date before the cutoff */
    size_t prunable_slot;
} dsp_history_scan_t;

/*
 * Reads every slot once and validates the WHOLE history. `schedule_id` may be NULL
 * (validation only). `prune_before_days`: records whose local date's day number is below
 * it may be reused (retention). Returns DSP_EXEC_OK, DSP_EXEC_ERR_STORAGE_READ, or
 * DSP_EXEC_ERR_INTERNAL for any corrupted record or any two records sharing a schedule ID,
 * whichever schedule they belong to (fail-safe: no automatic dosing while the history
 * cannot be trusted). Duplicate detection: a 32-bit FNV-1a hash per occupied slot; on
 * equal hashes both slots are re-read and compared exactly. Bounded: slot_count reads plus
 * two re-reads per hash collision.
 */
dsp_exec_status_t dsp_history_scan(const dsp_history_store_t *store, const char *schedule_id,
                                   int32_t prune_before_days, dsp_history_scan_t *out);

/* Writes one slot, commits, reads it back and compares. DSP_EXEC_OK only if all three
 * succeed: ERR_STORAGE_WRITE, ERR_STORAGE_COMMIT or ERR_PERSIST_CONFIRM otherwise. */
dsp_exec_status_t dsp_history_put(const dsp_history_store_t *store, size_t slot,
                                  const dsp_history_record_t *rec);

#ifdef __cplusplus
}
#endif

#endif /* DSP_HISTORY_H */
