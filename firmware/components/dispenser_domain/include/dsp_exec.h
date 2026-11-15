#ifndef DSP_EXEC_H
#define DSP_EXEC_H

/*
 * Execution coordinator (milestone M1.2): decides whether an automatic occurrence may be
 * executed, claims it durably BEFORE the pump may start, and records the outcome.
 * Pure domain code: no ESP-IDF, no GPIO, no motor, no clock reads, no allocation.
 *
 * Automatic execution protocol (approved):
 *   1. dsp_exec_check()   optional read-only pre-check (e.g. before stock/battery checks)
 *   2. dsp_exec_claim()   re-checks eligibility, then durably stores STARTED (write,
 *                         commit, read-back). DSP_EXEC_OK is the ONLY result that
 *                         authorizes pump activation. Any other result: pump stays off.
 *   3. (caller runs the pump; not part of this component)
 *   4. dsp_exec_finish()  records COMPLETED or FAILED.
 *
 * Guarantees: at most one automatic execution per occurrence <scheduleId>@<localDate>,
 * across restarts and power loss (at-most-once). A record in ANY state blocks the
 * occurrence; nothing is retried automatically. A crash after the claim but before or
 * during the pump run loses that dose (accepted). Missed occurrences (later than the due
 * window) are never executed; there is no catch-up.
 *
 * Concurrency: NOT thread-safe. Exactly one owner (the future dosing service task)
 * creates the dsp_exec_t and serializes every call, automatic and manual. The
 * coordinator allows one active execution at a time (DSP_EXEC_BUSY otherwise).
 */

#include <stdbool.h>
#include <stdint.h>

#include "dsp_history.h"
#include "dsp_recurrence.h"
#include "dsp_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Due window (approved default: 10 minutes): an occurrence may be executed while
 * scheduled_at <= now <= scheduled_at + due_window_s. Later it is MISSED forever. */
#define DSP_EXEC_DUE_WINDOW_DEFAULT_S 600
#define DSP_EXEC_DUE_WINDOW_MAX_S 3600

/* Retention (approved default: 7 days): a record of ANOTHER schedule whose local date is
 * more than retention_days before today's local date may be overwritten (oldest first)
 * when the history is full. Deduplication of a recycled record ASSUMES the valid clock
 * never moves back by more than retention_days; M1.2 does not detect such jumps (see
 * firmware/README.md, "Backward clock adjustments"). */
#define DSP_EXEC_RETENTION_DAYS_DEFAULT 7
#define DSP_EXEC_RETENTION_DAYS_MIN 1
#define DSP_EXEC_RETENTION_DAYS_MAX 366

typedef struct {
    const dsp_history_store_t *store; /* caller-owned, outlives the coordinator */
    const dsp_tz_t *tz;               /* schedule timezone (DSP_TZ_EUROPE_PRAGUE) */
    int32_t due_window_s;             /* 0..DSP_EXEC_DUE_WINDOW_MAX_S */
    int32_t retention_days;           /* DSP_EXEC_RETENTION_DAYS_MIN..MAX */
} dsp_exec_config_t;

/* Clock snapshot supplied by the caller. Automatic dosing requires valid == true
 * (time established by SNTP/Android, FR-031/FR-033) and now within DSP_UTC_MIN..MAX. */
typedef struct {
    dsp_utc_t now;
    bool valid;
} dsp_clock_t;

enum {
    DSP_EXEC_KIND_NONE = 0,
    DSP_EXEC_KIND_AUTOMATIC = 1,
    DSP_EXEC_KIND_MANUAL = 2,
};

/* Handle of an active execution, returned by claim / manual_begin. Copyable value;
 * only the most recent one is accepted by dsp_exec_finish. */
typedef struct {
    uint8_t kind;                /* DSP_EXEC_KIND_* */
    uint32_t seq;                /* per-coordinator sequence; detects stale tokens */
    size_t slot;                 /* automatic: history slot of the claim */
    dsp_history_record_t record; /* automatic: the durable STARTED record */
} dsp_exec_token_t;

/* Caller-owned coordinator state (no hidden globals). Treat as opaque. */
typedef struct {
    dsp_exec_config_t cfg;
    bool initialized;
    dsp_exec_token_t active; /* kind NONE when idle */
    uint32_t next_seq;
} dsp_exec_t;

/* Validates the config and resets in-RAM state. No storage I/O. A restart is modeled by
 * calling it again on fresh memory with the same store. */
dsp_exec_status_t dsp_exec_init(dsp_exec_t *ex, const dsp_exec_config_t *cfg);

/*
 * Boot recovery: every record still STARTED belongs to an execution interrupted by a
 * reset or power loss; it is rewritten as INTERRUPTED (still blocking) so the outcome can
 * be reported as uncertain. Never re-executes anything. *interrupted (optional) receives
 * the number converted. Stops at the first storage error (records stay STARTED, which
 * blocks equally); safe to call again. DSP_EXEC_BUSY while an execution is active.
 */
dsp_exec_status_t dsp_exec_recover(dsp_exec_t *ex, size_t *interrupted);

/*
 * Read-only eligibility check of an automatic occurrence (from dsp_rec_next() etc.).
 * Order: arguments, schedule validity, clock, enabled, occurrence identity (must be
 * exactly what the CURRENT schedule yields on that local date), timing, busy, history.
 * DSP_EXEC_OK means "would be claimable now"; it does NOT authorize the pump.
 */
dsp_exec_status_t dsp_exec_check(const dsp_exec_t *ex, const dsp_schedule_t *schedule,
                                 const dsp_occurrence_t *occ, dsp_clock_t clock);

/*
 * Checks eligibility again and durably claims the occurrence (STARTED). On DSP_EXEC_OK
 * *token describes the active execution and the pump MAY be started. On any other
 * result nothing was authorized and *token is untouched. Storage failures during the
 * claim (WRITE, COMMIT, PERSIST_CONFIRM) leave the occurrence unclaimed or claimed
 * (unknown); it is never automatically retried by the coordinator, and a later claim
 * attempt is decided only by the durable history.
 */
dsp_exec_status_t dsp_exec_claim(dsp_exec_t *ex, const dsp_schedule_t *schedule,
                                 const dsp_occurrence_t *occ, dsp_clock_t clock,
                                 dsp_exec_token_t *token);

/*
 * Ends the active execution. Automatic: outcome DSP_HISTORY_COMPLETED or
 * DSP_HISTORY_FAILED is stored durably in the claim's slot. Manual: no storage access
 * (outcome must be COMPLETED or FAILED, used for symmetry only). The active execution is
 * released even if storing fails; the record then stays STARTED and still blocks.
 */
dsp_exec_status_t dsp_exec_finish(dsp_exec_t *ex, const dsp_exec_token_t *token, uint8_t outcome);

/*
 * Starts a manual dose (user-requested, FR-043). Independent of occurrence identity:
 * never reads, writes or clears automatic history. Only mutual exclusion with automatic
 * execution is enforced (one pump). Logging and limits belong to later milestones.
 */
dsp_exec_status_t dsp_exec_manual_begin(dsp_exec_t *ex, dsp_exec_token_t *token);

/* Durable history record of a schedule: DSP_EXEC_OK, DSP_EXEC_NOT_FOUND or an error. */
dsp_exec_status_t dsp_exec_lookup(const dsp_exec_t *ex, const char *schedule_id,
                                  dsp_history_record_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DSP_EXEC_H */
