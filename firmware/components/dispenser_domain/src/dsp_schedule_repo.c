#include "dsp_schedule_repo.h"

#include <string.h>

#define KEY_SCHED "sched"

dsp_storage_status_t dsp_schedule_repo_load(dsp_schedule_repo_t *repo, dsp_storage_t *st)
{
    if (repo == NULL) {
        return DSP_STORAGE_ERR_ARG;
    }
    repo->loaded = false;
    repo->st = st;
    size_t len;
    dsp_storage_status_t s = dsp_storage_get(st, KEY_SCHED, &len);
    if (s != DSP_STORAGE_OK) {
        return s;
    }
    dsp_codec_status_t cs = dsp_codec_decode_schedule_set(st->buf, len, st->tz, &repo->set);
    if (cs != DSP_CODEC_OK) {
        return dsp_storage_from_codec(cs);
    }
    repo->loaded = true;
    return DSP_STORAGE_OK;
}

const dsp_schedule_set_t *dsp_schedule_repo_set(const dsp_schedule_repo_t *repo)
{
    return repo != NULL && repo->loaded ? &repo->set : NULL;
}

static int find_index(const dsp_schedule_set_t *set, const char *id)
{
    for (int i = 0; i < set->count; i++) {
        if (strcmp(set->schedules[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

const dsp_schedule_t *dsp_schedule_repo_find(const dsp_schedule_repo_t *repo, const char *id)
{
    if (repo == NULL || !repo->loaded || !dsp_schedule_id_is_valid(id)) {
        return NULL;
    }
    int i = find_index(&repo->set, id);
    return i < 0 ? NULL : &repo->set.schedules[i];
}

static dsp_storage_status_t ready(const dsp_schedule_repo_t *repo)
{
    if (repo == NULL || repo->st == NULL) {
        return DSP_STORAGE_ERR_ARG;
    }
    if (!repo->st->open) {
        return DSP_STORAGE_ERR_NOT_OPEN;
    }
    return repo->loaded ? DSP_STORAGE_OK : DSP_STORAGE_ERR_RELOAD_REQUIRED;
}

/* Persists repo->set (already modified in RAM). On failure the durable state is unknown,
 * so the repository is marked unloaded. */
static dsp_storage_status_t persist(dsp_schedule_repo_t *repo)
{
    dsp_storage_t *st = repo->st;
    size_t len = dsp_codec_encode_schedule_set(&repo->set, st->tz, st->buf, sizeof st->buf);
    dsp_storage_status_t s = len == 0 ? DSP_STORAGE_ERR_INVALID : dsp_storage_put(st, KEY_SCHED, len);
    if (s != DSP_STORAGE_OK) {
        repo->loaded = false;
    }
    return s;
}

static bool bump_revision(dsp_schedule_set_t *set)
{
    if (set->revision == UINT32_MAX) {
        return false;
    }
    set->revision++;
    return true;
}

dsp_storage_status_t dsp_schedule_repo_create(dsp_schedule_repo_t *repo, const dsp_schedule_t *s,
                                              const dsp_history_store_t *history)
{
    dsp_storage_status_t rs = ready(repo);
    if (rs != DSP_STORAGE_OK) {
        return rs;
    }
    if (s == NULL || history == NULL) {
        return DSP_STORAGE_ERR_ARG;
    }
    if (dsp_schedule_validate(s, repo->st->tz) != DSP_SCHED_OK) {
        return DSP_STORAGE_ERR_INVALID;
    }
    if (s->revision != 1) {
        return DSP_STORAGE_ERR_REVISION;
    }
    dsp_schedule_set_t *set = &repo->set;
    if (find_index(set, s->id) >= 0) {
        return DSP_STORAGE_ERR_DUPLICATE_ID;
    }
    for (size_t i = 0; i < set->tombstone_count; i++) {
        if (strcmp(set->tombstones[i], s->id) == 0) {
            return DSP_STORAGE_ERR_ID_REUSED;
        }
    }
    dsp_history_scan_t scan;
    dsp_exec_status_t es = dsp_history_scan(history, s->id, INT32_MIN, &scan);
    if (es == DSP_EXEC_ERR_STORAGE_READ) {
        return DSP_STORAGE_ERR_READ;
    }
    if (es != DSP_EXEC_OK) {
        return es == DSP_EXEC_ERR_ARG ? DSP_STORAGE_ERR_ARG : DSP_STORAGE_ERR_CORRUPT;
    }
    if (scan.found) {
        return DSP_STORAGE_ERR_ID_REUSED; /* executed before under this ID */
    }
    if (set->count >= DSP_SCHEDULE_CAPACITY) {
        return DSP_STORAGE_ERR_CAPACITY;
    }
    if (!bump_revision(set)) {
        return DSP_STORAGE_ERR_REVISION;
    }
    set->schedules[set->count++] = *s;
    return persist(repo);
}

dsp_storage_status_t dsp_schedule_repo_update(dsp_schedule_repo_t *repo, const dsp_schedule_t *s)
{
    dsp_storage_status_t rs = ready(repo);
    if (rs != DSP_STORAGE_OK) {
        return rs;
    }
    if (s == NULL) {
        return DSP_STORAGE_ERR_ARG;
    }
    if (dsp_schedule_validate(s, repo->st->tz) != DSP_SCHED_OK) {
        return DSP_STORAGE_ERR_INVALID;
    }
    dsp_schedule_set_t *set = &repo->set;
    int i = find_index(set, s->id);
    if (i < 0) {
        return DSP_STORAGE_ERR_NOT_FOUND;
    }
    if (set->schedules[i].revision == UINT32_MAX ||
        s->revision != set->schedules[i].revision + 1) {
        return DSP_STORAGE_ERR_REVISION;
    }
    if (!bump_revision(set)) {
        return DSP_STORAGE_ERR_REVISION;
    }
    set->schedules[i] = *s;
    return persist(repo);
}

dsp_storage_status_t dsp_schedule_repo_delete(dsp_schedule_repo_t *repo, const char *id,
                                              uint32_t expected_revision)
{
    dsp_storage_status_t rs = ready(repo);
    if (rs != DSP_STORAGE_OK) {
        return rs;
    }
    if (!dsp_schedule_id_is_valid(id)) {
        return DSP_STORAGE_ERR_ARG;
    }
    dsp_schedule_set_t *set = &repo->set;
    int i = find_index(set, id);
    if (i < 0) {
        return DSP_STORAGE_ERR_NOT_FOUND;
    }
    if (set->schedules[i].revision != expected_revision) {
        return DSP_STORAGE_ERR_REVISION;
    }
    if (!bump_revision(set)) {
        return DSP_STORAGE_ERR_REVISION;
    }
    char removed[DSP_SCHEDULE_ID_MAX_LEN + 1];
    memcpy(removed, set->schedules[i].id, sizeof removed);
    for (int j = i; j + 1 < set->count; j++) {
        set->schedules[j] = set->schedules[j + 1];
    }
    set->count--;
    memset(&set->schedules[set->count], 0, sizeof set->schedules[0]);
    if (set->tombstone_count == DSP_TOMBSTONE_CAPACITY) { /* drop the oldest */
        memmove(set->tombstones[0], set->tombstones[1],
                (DSP_TOMBSTONE_CAPACITY - 1) * sizeof set->tombstones[0]);
        set->tombstone_count--;
    }
    memcpy(set->tombstones[set->tombstone_count++], removed, sizeof removed);
    return persist(repo);
}
