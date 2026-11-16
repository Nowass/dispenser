#include "dsp_history.h"

#include <string.h>

bool dsp_history_record_is_valid(const dsp_history_record_t *rec)
{
    if (rec == NULL || !dsp_schedule_id_is_valid(rec->schedule_id) ||
        !dsp_date_is_valid(rec->local_date)) {
        return false;
    }
    if (rec->state < DSP_HISTORY_STARTED || rec->state > DSP_HISTORY_INTERRUPTED) {
        return false;
    }
    return rec->scheduled_at >= DSP_UTC_MIN && rec->scheduled_at <= DSP_UTC_MAX &&
           rec->claimed_at >= DSP_UTC_MIN && rec->claimed_at <= DSP_UTC_MAX;
}

bool dsp_history_record_equal(const dsp_history_record_t *a, const dsp_history_record_t *b)
{
    return strncmp(a->schedule_id, b->schedule_id, sizeof a->schedule_id) == 0 &&
           dsp_date_compare(a->local_date, b->local_date) == 0 && a->state == b->state &&
           a->schedule_revision == b->schedule_revision &&
           a->scheduled_at == b->scheduled_at && a->claimed_at == b->claimed_at &&
           a->volume_ul == b->volume_ul;
}

bool dsp_history_store_is_valid(const dsp_history_store_t *store)
{
    return store != NULL && store->read != NULL && store->write != NULL &&
           store->commit != NULL && store->slot_count >= 1 &&
           store->slot_count <= DSP_HISTORY_MAX_SLOTS;
}

/* FNV-1a, 32 bit, over a validated (NUL-terminated, <= 64 chars) schedule ID. */
static uint32_t id_hash(const char *id)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i <= DSP_SCHEDULE_ID_MAX_LEN && id[i] != '\0'; i++) {
        h ^= (uint8_t)id[i];
        h *= 16777619u;
    }
    return h;
}

/* Exact comparison of two slots whose ID hashes are equal. */
static dsp_exec_status_t check_collision(const dsp_history_store_t *store, size_t a, size_t b)
{
    dsp_history_record_t ra;
    dsp_history_record_t rb;
    if (store->read(store->ctx, a, &ra) != DSP_STORE_OK ||
        store->read(store->ctx, b, &rb) != DSP_STORE_OK) {
        return DSP_EXEC_ERR_STORAGE_READ; /* also: a slot changed to empty meanwhile */
    }
    if (!dsp_history_record_is_valid(&ra) || !dsp_history_record_is_valid(&rb) ||
        strcmp(ra.schedule_id, rb.schedule_id) == 0) {
        return DSP_EXEC_ERR_INTERNAL;
    }
    return DSP_EXEC_OK; /* genuine hash collision of different IDs */
}

dsp_exec_status_t dsp_history_scan(const dsp_history_store_t *store, const char *schedule_id,
                                   int32_t prune_before_days, dsp_history_scan_t *out)
{
    if (!dsp_history_store_is_valid(store) ||
        (schedule_id != NULL && !dsp_schedule_id_is_valid(schedule_id)) || out == NULL) {
        return DSP_EXEC_ERR_ARG;
    }
    memset(out, 0, sizeof *out);
    uint32_t hashes[DSP_HISTORY_MAX_SLOTS];
    uint8_t slots[DSP_HISTORY_MAX_SLOTS];
    size_t occupied = 0;
    int32_t prunable_days = 0;
    for (size_t slot = 0; slot < store->slot_count; slot++) {
        dsp_history_record_t rec;
        dsp_store_status_t st = store->read(store->ctx, slot, &rec);
        if (st == DSP_STORE_EMPTY) {
            if (!out->has_empty) {
                out->has_empty = true;
                out->empty_slot = slot;
            }
            continue;
        }
        if (st != DSP_STORE_OK) {
            return DSP_EXEC_ERR_STORAGE_READ;
        }
        if (!dsp_history_record_is_valid(&rec)) {
            return DSP_EXEC_ERR_INTERNAL; /* corrupted: the history cannot be trusted */
        }
        hashes[occupied] = id_hash(rec.schedule_id);
        slots[occupied] = (uint8_t)slot; /* slot < DSP_HISTORY_MAX_SLOTS <= 255 */
        occupied++;
        bool same = schedule_id != NULL && strcmp(rec.schedule_id, schedule_id) == 0;
        if (same) {
            if (out->found) {
                return DSP_EXEC_ERR_INTERNAL; /* two records for one schedule ID */
            }
            out->found = true;
            out->found_slot = slot;
            out->found_record = rec;
            continue;
        }
        int32_t days = dsp_date_to_days(rec.local_date);
        if (days < prune_before_days && (!out->has_prunable || days < prunable_days)) {
            out->has_prunable = true;
            out->prunable_slot = slot;
            prunable_days = days;
        }
    }
    /* Global duplicate check over all occupied slots (at most 64*63/2 comparisons). */
    for (size_t i = 1; i < occupied; i++) {
        for (size_t j = 0; j < i; j++) {
            if (hashes[i] == hashes[j]) {
                dsp_exec_status_t st = check_collision(store, slots[j], slots[i]);
                if (st != DSP_EXEC_OK) {
                    return st;
                }
            }
        }
    }
    return DSP_EXEC_OK;
}

dsp_exec_status_t dsp_history_put(const dsp_history_store_t *store, size_t slot,
                                  const dsp_history_record_t *rec)
{
    if (!dsp_history_store_is_valid(store) || slot >= store->slot_count ||
        !dsp_history_record_is_valid(rec)) {
        return DSP_EXEC_ERR_ARG;
    }
    if (store->write(store->ctx, slot, rec) != DSP_STORE_OK) {
        return DSP_EXEC_ERR_STORAGE_WRITE;
    }
    if (store->commit(store->ctx) != DSP_STORE_OK) {
        return DSP_EXEC_ERR_STORAGE_COMMIT;
    }
    dsp_history_record_t back;
    if (store->read(store->ctx, slot, &back) != DSP_STORE_OK ||
        !dsp_history_record_equal(&back, rec)) {
        return DSP_EXEC_ERR_PERSIST_CONFIRM;
    }
    return DSP_EXEC_OK;
}
