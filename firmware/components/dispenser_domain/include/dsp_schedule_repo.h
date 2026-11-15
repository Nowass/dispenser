#ifndef DSP_SCHEDULE_REPO_H
#define DSP_SCHEDULE_REPO_H

/*
 * Persistent schedule repository (milestone M1.3). The whole schedule set is one record
 * ("sched"), so every change is atomic: after a power loss the device sees the old set or
 * the new set, never a partial update (approved 2026-11-15).
 *
 * Rules:
 *  - At most DSP_SCHEDULE_CAPACITY (16) schedules; every stored schedule passes
 *    dsp_schedule_validate (M1.1 model, unchanged).
 *  - create: revision must be 1; the ID must not be active (DUPLICATE_ID), nor a recently
 *    deleted ID (tombstone ring, 16 entries), nor present in the execution history
 *    (ID_REUSED). Beyond these bounded checks, non-reuse relies on Android generating
 *    UUIDs (approved: "UUID + bounded tombstones").
 *  - update: same ID, revision must be exactly stored revision + 1 (optimistic
 *    concurrency, as in the Android repository). The ID never changes.
 *  - delete: expected revision must match; the ID moves to the tombstone ring (the oldest
 *    tombstone is dropped when the ring is full). Execution history is not touched.
 *  - A failed write leaves the durable set unknown (old or new); the repository then
 *    refuses further changes until dsp_schedule_repo_load() re-reads it.
 * Caller-owned state, single owner, no allocation.
 */

#include <stdbool.h>
#include <stdint.h>

#include "dsp_history.h"
#include "dsp_schedule_set.h"
#include "dsp_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    dsp_storage_t *st;
    bool loaded;
    dsp_schedule_set_t set; /* authoritative copy of the durable set while loaded */
} dsp_schedule_repo_t;

/* Reads and validates "sched". Corrupt or unsupported data -> error, repo not loaded. */
dsp_storage_status_t dsp_schedule_repo_load(dsp_schedule_repo_t *repo, dsp_storage_t *st);

/* Loaded set, or NULL. */
const dsp_schedule_set_t *dsp_schedule_repo_set(const dsp_schedule_repo_t *repo);

/* Active schedule with this ID, or NULL. */
const dsp_schedule_t *dsp_schedule_repo_find(const dsp_schedule_repo_t *repo, const char *id);

/* `history` is the execution history bound to the same storage (ID reuse check). */
dsp_storage_status_t dsp_schedule_repo_create(dsp_schedule_repo_t *repo, const dsp_schedule_t *s,
                                              const dsp_history_store_t *history);
dsp_storage_status_t dsp_schedule_repo_update(dsp_schedule_repo_t *repo, const dsp_schedule_t *s);
dsp_storage_status_t dsp_schedule_repo_delete(dsp_schedule_repo_t *repo, const char *id,
                                              uint32_t expected_revision);

#ifdef __cplusplus
}
#endif

#endif /* DSP_SCHEDULE_REPO_H */
