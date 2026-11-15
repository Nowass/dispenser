/*
 * Persistent storage tests (M1.3): codec, layout provisioning, explicit presence,
 * configuration, schedule repository, M1.2 history compatibility, power loss and faults.
 * Uses the in-memory fake_kv (NVS-like and staged durability modes).
 */
#include "dsp_codec.h"
#include "dsp_exec.h"
#include "dsp_schedule_repo.h"
#include "dsp_storage.h"
#include "dsp_test.h"
#include "fake_kv.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const dsp_tz_t *const TZ = &DSP_TZ_EUROPE_PRAGUE;

static fake_kv_t kv;
static dsp_storage_t st;
static dsp_schedule_repo_t repo;
static dsp_history_store_t hist;

#define DATA_KEYS (DSP_STORAGE_HISTORY_SLOTS + 4) /* + sched, config, time, notify (layout v3) */
#define ALL_KEYS (DATA_KEYS + 1)

static dsp_storage_status_t reopen(bool allow)
{
    memset(&st, 0xA5, sizeof st); /* fresh RAM after a restart */
    dsp_storage_status_t s = dsp_storage_open(&st, &kv.iface, TZ, allow);
    dsp_storage_history_store(&st, &hist);
    return s;
}

/* Restart: power cycle, open (provisioning allowed), load schedules. */
static void boot_ok(void)
{
    fake_kv_power_cycle(&kv);
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, reopen(true));
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_schedule_repo_load(&repo, &st));
}

static void fresh(bool durable_on_set)
{
    fake_kv_init(&kv, durable_on_set);
    boot_ok();
    fake_kv_clear_faults(&kv);
}

static dsp_date_t D(int y, int m, int d)
{
    dsp_date_t date = {(int16_t)y, (uint8_t)m, (uint8_t)d};
    return date;
}

static dsp_schedule_t sched(const char *id, int hour)
{
    dsp_schedule_t s;
    memset(&s, 0, sizeof s);
    strcpy(s.id, id);
    s.revision = 1;
    s.enabled = true;
    s.amount_ul = 5000;
    s.start_date = D(2026, 11, 2);
    s.hour = (uint8_t)hour;
    s.kind = DSP_REC_DAILY;
    s.interval = 1;
    return s;
}

static bool sched_equal(const dsp_schedule_t *a, const dsp_schedule_t *b)
{
    return strcmp(a->id, b->id) == 0 && a->revision == b->revision && a->enabled == b->enabled &&
           a->amount_ul == b->amount_ul && dsp_date_compare(a->start_date, b->start_date) == 0 &&
           a->hour == b->hour && a->minute == b->minute && a->kind == b->kind &&
           a->interval == b->interval && a->weekday_mask == b->weekday_mask &&
           a->has_end_date == b->has_end_date &&
           (!a->has_end_date || dsp_date_compare(a->end_date, b->end_date) == 0);
}

static bool set_equal(const dsp_schedule_set_t *a, const dsp_schedule_set_t *b)
{
    if (a->revision != b->revision || a->count != b->count ||
        a->tombstone_count != b->tombstone_count) {
        return false;
    }
    for (size_t i = 0; i < a->count; i++) {
        if (!sched_equal(&a->schedules[i], &b->schedules[i])) {
            return false;
        }
    }
    for (size_t i = 0; i < a->tombstone_count; i++) {
        if (strcmp(a->tombstones[i], b->tombstones[i]) != 0) {
            return false;
        }
    }
    return true;
}

/* ---- 1. First boot / reopen --------------------------------------------------------- */

static void test_first_boot_provisions_every_key(void)
{
    fake_kv_init(&kv, true);
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, reopen(true));
    DSP_CHECK(st.open && st.provisioned_now);
    DSP_CHECK_EQ_INT(ALL_KEYS, fake_kv_count(&kv));
    DSP_CHECK(fake_kv_find(&kv, "layout") != NULL);
    DSP_CHECK(fake_kv_find(&kv, "h00") != NULL && fake_kv_find(&kv, "h31") != NULL);
    DSP_CHECK(fake_kv_find(&kv, "h32") == NULL);
    /* Explicit EMPTY records, empty set, default configuration. */
    for (size_t slot = 0; slot < DSP_STORAGE_HISTORY_SLOTS; slot++) {
        dsp_history_record_t rec;
        DSP_CHECK_EQ_INT(DSP_STORE_EMPTY, hist.read(hist.ctx, slot, &rec));
    }
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_schedule_repo_load(&repo, &st));
    DSP_CHECK_EQ_INT(0, repo.set.count);
    DSP_CHECK_EQ_INT(0, repo.set.revision);
    dsp_config_t cfg;
    dsp_config_t def;
    dsp_config_defaults(&def);
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_load_config(&st, &cfg));
    DSP_CHECK(dsp_config_equal(&cfg, &def));
    DSP_CHECK_EQ_INT(0, def.reservoir_capacity_ul); /* reservoir not configured */

    /* Reopen: no provisioning, no writes at all. */
    fake_kv_clear_faults(&kv);
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, reopen(true));
    DSP_CHECK(!st.provisioned_now);
    DSP_CHECK_EQ_INT(0, kv.sets + kv.commits);

    /* Read-only open of fresh storage never provisions. */
    fake_kv_init(&kv, true);
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_NOT_PROVISIONED, reopen(false));
    DSP_CHECK(!st.open);
    DSP_CHECK_EQ_INT(0, kv.sets);
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_NOT_OPEN, dsp_storage_load_config(&st, &cfg));
}

/* ---- 2. Configuration --------------------------------------------------------------- */

static void test_config_save_and_reload(void)
{
    fresh(true);
    dsp_config_t cfg;
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_load_config(&st, &cfg));
    cfg.revision = 1;
    cfg.reservoir_capacity_ul = 500000;
    cfg.reservoir_remaining_ul = 250000;
    cfg.low_stock_percent = 15;
    cfg.time_sync_interval_h = 48;
    cfg.notify_mask = DSP_NOTIFY_LOW_RESERVOIR | DSP_NOTIFY_DEVICE_ERROR;
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_save_config(&st, &cfg));
    /* Same revision again: conflict. Skipped revision: conflict. */
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_REVISION, dsp_storage_save_config(&st, &cfg));
    dsp_config_t skip = cfg;
    skip.revision = 3;
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_REVISION, dsp_storage_save_config(&st, &skip));
    /* Invalid values are refused before any write. */
    const struct { uint32_t cap, rem; uint8_t low; uint16_t sync; uint8_t mask; } bad[] = {
        {100, 101, 20, 24, 7}, {100, 50, 0, 24, 7}, {100, 50, 100, 24, 7},
        {100, 50, 20, 23, 7},  {100, 50, 20, 49, 7}, {100, 50, 20, 24, 8},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        dsp_config_t b = cfg;
        b.revision = 2;
        b.reservoir_capacity_ul = bad[i].cap;
        b.reservoir_remaining_ul = bad[i].rem;
        b.low_stock_percent = bad[i].low;
        b.time_sync_interval_h = bad[i].sync;
        b.notify_mask = bad[i].mask;
        fake_kv_clear_faults(&kv);
        DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_INVALID, dsp_storage_save_config(&st, &b));
        DSP_CHECK_EQ_INT(0, kv.sets);
    }
    boot_ok();
    dsp_config_t back;
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_load_config(&st, &back));
    DSP_CHECK(dsp_config_equal(&cfg, &back));
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_ARG, dsp_storage_save_config(&st, NULL));
}

/* ---- 3./4. Schedules: create, modify, delete, restart ------------------------------- */

static void test_schedule_crud_survives_restart(void)
{
    fresh(true);
    dsp_schedule_t a = sched("6f1c2e9a-0001", 8);
    dsp_schedule_t b = sched("6f1c2e9a-0002", 20);
    b.kind = DSP_REC_WEEKLY;
    b.weekday_mask = DSP_WEEKDAY_BIT(DSP_MONDAY) | DSP_WEEKDAY_BIT(DSP_THURSDAY);
    b.interval = 2;
    b.has_end_date = true;
    b.end_date = D(2027, 1, 31);
    dsp_schedule_t c = sched("6f1c2e9a-0003", 9);
    c.kind = DSP_REC_ONCE;
    c.start_date = D(2026, 12, 24);
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_schedule_repo_create(&repo, &a, &hist));
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_schedule_repo_create(&repo, &b, &hist));
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_schedule_repo_create(&repo, &c, &hist));
    DSP_CHECK_EQ_INT(3, repo.set.revision);

    dsp_schedule_t a2 = a; /* edit: time and volume; same ID, next revision */
    a2.revision = 2;
    a2.hour = 21;
    a2.amount_ul = 7500;
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_schedule_repo_update(&repo, &a2));
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_schedule_repo_delete(&repo, c.id, 1));

    boot_ok(); /* restart after successful writes */
    DSP_CHECK_EQ_INT(2, repo.set.count);
    DSP_CHECK_EQ_INT(5, repo.set.revision);
    const dsp_schedule_t *fa = dsp_schedule_repo_find(&repo, a.id);
    const dsp_schedule_t *fb = dsp_schedule_repo_find(&repo, b.id);
    DSP_CHECK(fa != NULL && sched_equal(fa, &a2));
    DSP_CHECK(fb != NULL && sched_equal(fb, &b));
    DSP_CHECK(dsp_schedule_repo_find(&repo, c.id) == NULL);
    DSP_CHECK_EQ_INT(1, repo.set.tombstone_count);
    DSP_CHECK_EQ_STR(c.id, repo.set.tombstones[0]);

    /* Revision rules (optimistic concurrency). */
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_REVISION, dsp_schedule_repo_update(&repo, &a2)); /* stale */
    dsp_schedule_t jump = a2;
    jump.revision = 9;
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_REVISION, dsp_schedule_repo_update(&repo, &jump));
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_REVISION, dsp_schedule_repo_delete(&repo, b.id, 7));
    dsp_schedule_t r2 = sched("6f1c2e9a-0009", 8);
    r2.revision = 2;
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_REVISION, dsp_schedule_repo_create(&repo, &r2, &hist));
    dsp_schedule_t missing = sched("nope", 8);
    missing.revision = 2;
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_NOT_FOUND, dsp_schedule_repo_update(&repo, &missing));
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_NOT_FOUND, dsp_schedule_repo_delete(&repo, "nope", 1));
    /* Invalid schedule data is refused before any write. */
    fake_kv_clear_faults(&kv);
    dsp_schedule_t bad = sched("bad-1", 8);
    bad.interval = 0;
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_INVALID, dsp_schedule_repo_create(&repo, &bad, &hist));
    dsp_schedule_t badu = a2;
    badu.revision = 3;
    badu.kind = 9;
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_INVALID, dsp_schedule_repo_update(&repo, &badu));
    DSP_CHECK_EQ_INT(0, kv.sets);
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_ARG, dsp_schedule_repo_delete(&repo, "a@b", 1));
}

/* ---- 9. Duplicate and reused IDs ---------------------------------------------------- */

static void test_schedule_id_rules(void)
{
    fresh(true);
    dsp_schedule_t a = sched("uuid-a", 8);
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_schedule_repo_create(&repo, &a, &hist));
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_DUPLICATE_ID, dsp_schedule_repo_create(&repo, &a, &hist));
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_schedule_repo_delete(&repo, "uuid-a", 1));
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_ID_REUSED, dsp_schedule_repo_create(&repo, &a, &hist));

    /* An ID that was executed (history record) cannot be created again. */
    dsp_exec_t ex;
    dsp_exec_config_t cfg = {&hist, TZ, DSP_EXEC_DUE_WINDOW_DEFAULT_S, DSP_EXEC_RETENTION_DAYS_DEFAULT};
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_init(&ex, &cfg));
    dsp_schedule_t h = sched("uuid-h", 8);
    dsp_occurrence_t o;
    DSP_CHECK_EQ_INT(DSP_REC_OK, dsp_rec_occurrence_on(&h, TZ, D(2026, 11, 20), &o));
    dsp_exec_token_t tok;
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_claim(&ex, &h, &o, (dsp_clock_t){o.at, true, 0}, &tok));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_finish(&ex, &tok, DSP_HISTORY_COMPLETED));
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_ID_REUSED, dsp_schedule_repo_create(&repo, &h, &hist));
    /* History read failure: create is refused (fail safe), nothing written. */
    fake_kv_clear_faults(&kv);
    strcpy(kv.fail_get_key, "h05");
    dsp_schedule_t n = sched("uuid-n", 8);
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_READ, dsp_schedule_repo_create(&repo, &n, &hist));
    DSP_CHECK_EQ_INT(0, kv.sets);
    fake_kv_clear_faults(&kv);

    /* A stored set with a duplicate ID (crafted bytes) is corrupt. */
    dsp_schedule_set_t dup;
    memset(&dup, 0, sizeof dup);
    dup.count = 2;
    dup.schedules[0] = sched("same", 8);
    dup.schedules[1] = sched("other", 9);
    uint8_t buf[DSP_SCHEDULE_SET_MAX_BYTES];
    size_t len = dsp_codec_encode_schedule_set(&dup, TZ, buf, sizeof buf);
    DSP_CHECK(len > 0);
    memcpy(buf + DSP_SCHEDULE_SET_HEADER_BYTES + DSP_SCHEDULE_ENTRY_BYTES,
           buf + DSP_SCHEDULE_SET_HEADER_BYTES, 65); /* second ID := "same" */
    fake_kv_put_raw(&kv, "sched", buf, len);
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_CORRUPT, reopen(true));
    /* Encoder refuses to produce such a set, too (active vs tombstone duplicate). */
    dsp_schedule_set_t dup2;
    memset(&dup2, 0, sizeof dup2);
    dup2.count = 1;
    dup2.schedules[0] = sched("x", 8);
    dup2.tombstone_count = 1;
    strcpy(dup2.tombstones[0], "x");
    DSP_CHECK_EQ_INT(0, dsp_codec_encode_schedule_set(&dup2, TZ, buf, sizeof buf));
}

/* ---- 10. Capacity and the bounded tombstone ring ------------------------------------ */

static void test_schedule_capacity_and_tombstones(void)
{
    fresh(true);
    char id[16];
    for (int i = 0; i < DSP_SCHEDULE_CAPACITY; i++) {
        snprintf(id, sizeof id, "cap-%02d", i);
        dsp_schedule_t s = sched(id, i % 24);
        DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_schedule_repo_create(&repo, &s, &hist));
    }
    fake_kv_clear_faults(&kv);
    dsp_schedule_t extra = sched("cap-extra", 8);
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_CAPACITY, dsp_schedule_repo_create(&repo, &extra, &hist));
    DSP_CHECK_EQ_INT(0, kv.sets);
    /* Delete all 16, then one more cycle: the ring keeps the newest 16 IDs. */
    for (int i = 0; i < DSP_SCHEDULE_CAPACITY; i++) {
        snprintf(id, sizeof id, "cap-%02d", i);
        DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_schedule_repo_delete(&repo, id, 1));
    }
    DSP_CHECK_EQ_INT(DSP_TOMBSTONE_CAPACITY, repo.set.tombstone_count);
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_schedule_repo_create(&repo, &extra, &hist));
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_schedule_repo_delete(&repo, "cap-extra", 1));
    DSP_CHECK_EQ_INT(DSP_TOMBSTONE_CAPACITY, repo.set.tombstone_count);
    DSP_CHECK_EQ_STR("cap-01", repo.set.tombstones[0]); /* cap-00 dropped (oldest) */
    DSP_CHECK_EQ_STR("cap-extra", repo.set.tombstones[DSP_TOMBSTONE_CAPACITY - 1]);
    boot_ok();
    DSP_CHECK_EQ_INT(DSP_TOMBSTONE_CAPACITY, repo.set.tombstone_count);
    /* Documented bound: an ID older than the ring (never executed) is accepted again. */
    dsp_schedule_t old = sched("cap-00", 8);
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_schedule_repo_create(&repo, &old, &hist));
    dsp_schedule_t again = sched("cap-05", 8);
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_ID_REUSED, dsp_schedule_repo_create(&repo, &again, &hist));
}

/* ---- 5./6./12. Power loss and interrupted updates ----------------------------------- */

static void test_power_loss_around_commit(void)
{
    /* Staged mode: power lost after set, before commit -> the old set survives. */
    fresh(false);
    dsp_schedule_t a = sched("pl-a", 8);
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_schedule_repo_create(&repo, &a, &hist));
    dsp_schedule_t b = sched("pl-b", 9);
    kv.power_cut_after_sets = 1;
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_COMMIT, dsp_schedule_repo_create(&repo, &b, &hist));
    DSP_CHECK(!repo.loaded); /* durable state unknown */
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_RELOAD_REQUIRED, dsp_schedule_repo_create(&repo, &b, &hist));
    boot_ok();
    DSP_CHECK_EQ_INT(1, repo.set.count);
    DSP_CHECK(dsp_schedule_repo_find(&repo, "pl-b") == NULL);

    /* NVS mode: the same cut leaves the NEW set (set() already durable). */
    fresh(true);
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_schedule_repo_create(&repo, &a, &hist));
    kv.power_cut_after_sets = 1;
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_COMMIT, dsp_schedule_repo_create(&repo, &b, &hist));
    boot_ok();
    DSP_CHECK_EQ_INT(2, repo.set.count);

    /* Power lost after a successful commit, before the read-back: new set survives. */
    fresh(false);
    kv.power_cut_after_sets = 0;
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_schedule_repo_create(&repo, &a, &hist));
    dsp_config_t cfg;
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_load_config(&st, &cfg));
    cfg.revision = 1;
    cfg.reservoir_capacity_ul = 100000;
    cfg.reservoir_remaining_ul = 100000;
    kv.corrupt_next_readback = true; /* models "power gone": confirmation not obtained */
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_CONFIRM, dsp_storage_save_config(&st, &cfg));
    boot_ok();
    dsp_config_t back;
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_load_config(&st, &back));
    DSP_CHECK(dsp_config_equal(&cfg, &back));
}

static void test_interrupted_provisioning_resumes(void)
{
    for (int mode = 0; mode < 2; mode++) {
        for (int k = 1; k <= ALL_KEYS; k++) {
            fake_kv_init(&kv, mode == 1);
            kv.power_cut_after_sets = k;
            dsp_storage_status_t s = reopen(true);
            /* Every write before the marker leaves a resumable state. */
            if (k < ALL_KEYS) {
                DSP_CHECK(s != DSP_STORAGE_OK);
            }
            fake_kv_power_cycle(&kv);
            DSP_CHECK_EQ_INT(DSP_STORAGE_OK, reopen(true));
            DSP_CHECK_EQ_INT(ALL_KEYS, fake_kv_count(&kv));
        }
    }
}

static void test_marker_lost_with_data_is_corrupt(void)
{
    fresh(true);
    dsp_schedule_t a = sched("m-a", 8);
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_schedule_repo_create(&repo, &a, &hist));
    fake_kv_remove(&kv, "layout");
    fake_kv_clear_faults(&kv);
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_CORRUPT, reopen(true));
    DSP_CHECK_EQ_INT(0, kv.sets); /* never re-provisioned over real data */
    /* Marker lost on a never-used device: re-provisioning is harmless. */
    fresh(true);
    fake_kv_remove(&kv, "layout");
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, reopen(true));
}

/* ---- 7./8./14. Invalid data, unsupported versions, missing keys --------------------- */

static void expect_open(dsp_storage_status_t expected)
{
    fake_kv_clear_faults(&kv);
    DSP_CHECK_EQ_INT(expected, reopen(true));
    DSP_CHECK(st.open == (expected == DSP_STORAGE_OK));
    DSP_CHECK_EQ_INT(0, kv.sets); /* nothing erased or rewritten */
}

static void test_invalid_serialized_data(void)
{
    const char *keys[] = {"layout", "h00", "h31", "sched", "config", "time", "notify"};
    for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++) {
        fresh(true);
        const fake_kv_entry_t *e = fake_kv_find(&kv, keys[k]);
        DSP_CHECK(e != NULL);
        uint8_t orig[FAKE_KV_MAX_VALUE];
        size_t len = e->len;
        memcpy(orig, e->value, len);

        uint8_t junk[4] = {0xDE, 0xAD, 0xBE, 0xEF};
        fake_kv_put_raw(&kv, keys[k], junk, sizeof junk); /* foreign data */
        expect_open(DSP_STORAGE_ERR_CORRUPT);
        fake_kv_put_raw(&kv, keys[k], orig, len - 1); /* truncated */
        expect_open(DSP_STORAGE_ERR_CORRUPT);
        uint8_t longer[FAKE_KV_MAX_VALUE];
        memcpy(longer, orig, len);
        longer[len] = 0;
        fake_kv_put_raw(&kv, keys[k], longer, len + 1); /* trailing byte */
        expect_open(DSP_STORAGE_ERR_CORRUPT);
        memset(longer, 0, sizeof longer);
        fake_kv_put_raw(&kv, keys[k], longer, sizeof longer); /* larger than any record */
        expect_open(DSP_STORAGE_ERR_CORRUPT);
        uint8_t wrong[FAKE_KV_MAX_VALUE];
        memcpy(wrong, orig, len);
        wrong[0] = (uint8_t)(wrong[0] == DSP_REC_TYPE_CONFIG ? DSP_REC_TYPE_HISTORY : DSP_REC_TYPE_CONFIG);
        fake_kv_put_raw(&kv, keys[k], wrong, len); /* wrong record type */
        expect_open(DSP_STORAGE_ERR_CORRUPT);
        fake_kv_remove(&kv, keys[k]); /* NVS dropped a corrupt item */
        if (strcmp(keys[k], "layout") != 0) {
            expect_open(DSP_STORAGE_ERR_CORRUPT);
        } else {
            /* Marker gone on never-used storage: re-provisioned (only the marker is
             * written). With real data the same case is CORRUPT, see
             * test_marker_lost_with_data_is_corrupt. */
            fake_kv_clear_faults(&kv);
            DSP_CHECK_EQ_INT(DSP_STORAGE_OK, reopen(true));
            DSP_CHECK_EQ_INT(1, kv.sets);
        }
        fake_kv_put_raw(&kv, keys[k], orig, len); /* restored: healthy again */
        expect_open(DSP_STORAGE_OK);
    }
    /* Two individually valid history records for the same schedule ID: the whole-history
     * validation at open rejects the storage (fail safe), nothing is rewritten. */
    fresh(true);
    dsp_history_record_t dup;
    memset(&dup, 0, sizeof dup);
    strcpy(dup.schedule_id, "twin");
    dup.local_date = D(2026, 11, 20);
    dup.state = DSP_HISTORY_COMPLETED;
    dup.schedule_revision = 1;
    dup.scheduled_at = 1795158000;
    dup.claimed_at = 1795158000;
    uint8_t hrec[DSP_HISTORY_RECORD_BYTES];
    DSP_CHECK_EQ_INT(DSP_HISTORY_RECORD_BYTES, dsp_codec_encode_history(&dup, hrec, sizeof hrec));
    fake_kv_put_raw(&kv, "h03", hrec, sizeof hrec);
    expect_open(DSP_STORAGE_OK); /* one record: fine */
    dup.local_date = D(2026, 11, 19);
    DSP_CHECK_EQ_INT(DSP_HISTORY_RECORD_BYTES, dsp_codec_encode_history(&dup, hrec, sizeof hrec));
    fake_kv_put_raw(&kv, "h09", hrec, sizeof hrec);
    expect_open(DSP_STORAGE_ERR_CORRUPT);

    /* Field-level corruption: invalid content in otherwise well-formed records. */
    fresh(true);
    const fake_kv_entry_t *h = fake_kv_find(&kv, "h07");
    uint8_t rec[DSP_HISTORY_RECORD_BYTES];
    memcpy(rec, h->value, sizeof rec);
    rec[50] = 1; /* non-zero byte inside an EMPTY record */
    fake_kv_put_raw(&kv, "h07", rec, sizeof rec);
    expect_open(DSP_STORAGE_ERR_CORRUPT);
    fresh(true);
    const fake_kv_entry_t *c = fake_kv_find(&kv, "config");
    uint8_t cb[DSP_CONFIG_RECORD_BYTES];
    memcpy(cb, c->value, sizeof cb);
    cb[14] = 0; /* low-stock threshold 0 */
    fake_kv_put_raw(&kv, "config", cb, sizeof cb);
    expect_open(DSP_STORAGE_ERR_CORRUPT);
}

static void test_unsupported_versions(void)
{
    const char *keys[] = {"layout", "h12", "sched", "config", "time", "notify"};
    for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++) {
        fresh(true);
        const fake_kv_entry_t *e = fake_kv_find(&kv, keys[k]);
        uint8_t v[FAKE_KV_MAX_VALUE];
        size_t len = e->len;
        memcpy(v, e->value, len);
        v[1] = 2; /* format version from a newer firmware */
        fake_kv_put_raw(&kv, keys[k], v, len);
        expect_open(DSP_STORAGE_ERR_UNSUPPORTED);
    }
    /* Layout marker of another layout version / slot count. */
    const dsp_layout_t layouts[] = {{4, 32, 16, 16}, {0, 32, 16, 16}, {3, 16, 16, 16},
                                    {3, 32, 8, 16},  {3, 32, 16, 4},  {1, 16, 16, 16},
                                    {2, 32, 16, 4}};
    for (size_t i = 0; i < sizeof layouts / sizeof layouts[0]; i++) {
        fresh(true);
        uint8_t v[DSP_LAYOUT_RECORD_BYTES];
        DSP_CHECK_EQ_INT(DSP_LAYOUT_RECORD_BYTES, dsp_codec_encode_layout(&layouts[i], v, sizeof v));
        fake_kv_put_raw(&kv, "layout", v, sizeof v);
        expect_open(DSP_STORAGE_ERR_UNSUPPORTED);
    }
}

/* ---- 11. M1.2 compatibility: coordinator on top of the persistent history ----------- */

static dsp_exec_t ex;

static void exec_boot(void)
{
    boot_ok();
    dsp_exec_config_t cfg = {&hist, TZ, DSP_EXEC_DUE_WINDOW_DEFAULT_S, DSP_EXEC_RETENTION_DAYS_DEFAULT};
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_init(&ex, &cfg));
}

static void test_history_compatibility_with_m12(void)
{
    fresh(true);
    exec_boot();
    dsp_schedule_t s = sched("hist-1", 8);
    dsp_occurrence_t o;
    DSP_CHECK_EQ_INT(DSP_REC_OK, dsp_rec_occurrence_on(&s, TZ, D(2026, 11, 20), &o));
    dsp_exec_token_t tok;
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_claim(&ex, &s, &o, (dsp_clock_t){o.at + 5, true, 0}, &tok));
    /* Restart during the pump run: STARTED persisted, recovered as INTERRUPTED. */
    exec_boot();
    size_t n = 0;
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_recover(&ex, &n));
    DSP_CHECK_EQ_INT(1, n);
    DSP_CHECK_EQ_INT(DSP_EXEC_ALREADY_CLAIMED, dsp_exec_claim(&ex, &s, &o, (dsp_clock_t){o.at + 60, true, 0}, &tok));
    dsp_history_record_t rec;
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_lookup(&ex, "hist-1", &rec));
    DSP_CHECK_EQ_INT(DSP_HISTORY_INTERRUPTED, rec.state);
    /* Next day: normal execution, in place. */
    dsp_occurrence_t o2;
    DSP_CHECK_EQ_INT(DSP_REC_OK, dsp_rec_occurrence_on(&s, TZ, D(2026, 11, 21), &o2));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_claim(&ex, &s, &o2, (dsp_clock_t){o2.at, true, 0}, &tok));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_finish(&ex, &tok, DSP_HISTORY_COMPLETED));
    exec_boot();
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_lookup(&ex, "hist-1", &rec));
    DSP_CHECK_EQ_INT(DSP_HISTORY_COMPLETED, rec.state);
    DSP_CHECK_EQ_INT(0, dsp_date_compare(D(2026, 11, 21), rec.local_date));

    /* Corruption of the claim slot is NEVER read as empty: no second dose. */
    fake_kv_remove(&kv, "h00"); /* NVS dropped the item while the device was running */
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_STORAGE_READ, dsp_exec_claim(&ex, &s, &o2, (dsp_clock_t){o2.at + 10, true, 0}, &tok));
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_STORAGE_READ, dsp_exec_check(&ex, &s, &o2, (dsp_clock_t){o2.at + 10, true, 0}));
    fake_kv_power_cycle(&kv);
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_CORRUPT, reopen(true)); /* and at the next boot */

    /* Failed claim writes never authorize the pump. */
    fresh(true);
    exec_boot();
    fake_kv_clear_faults(&kv);
    kv.fail_sets = true;
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_STORAGE_WRITE, dsp_exec_claim(&ex, &s, &o, (dsp_clock_t){o.at, true, 0}, &tok));
    fake_kv_clear_faults(&kv);
    kv.no_space = true;
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_STORAGE_WRITE, dsp_exec_claim(&ex, &s, &o, (dsp_clock_t){o.at, true, 0}, &tok));
    fake_kv_clear_faults(&kv);
    kv.fail_commits = true;
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_STORAGE_COMMIT, dsp_exec_claim(&ex, &s, &o, (dsp_clock_t){o.at, true, 0}, &tok));
    fake_kv_clear_faults(&kv);
    /* NVS mode: the commit "failed" but set() was durable -> occurrence now blocked. */
    DSP_CHECK_EQ_INT(DSP_EXEC_ALREADY_CLAIMED, dsp_exec_claim(&ex, &s, &o, (dsp_clock_t){o.at + 1, true, 0}, &tok));
    fresh(true);
    exec_boot();
    kv.corrupt_next_readback = true;
    DSP_CHECK_EQ_INT(DSP_EXEC_ERR_PERSIST_CONFIRM, dsp_exec_claim(&ex, &s, &o, (dsp_clock_t){o.at, true, 0}, &tok));
}

/* ---- 13. Storage failures for configuration and schedules ------------------------- */

static void test_storage_failures(void)
{
    fresh(true);
    dsp_schedule_t a = sched("f-a", 8);
    const struct { int fault; dsp_storage_status_t expected; } cases[] = {
        {0, DSP_STORAGE_ERR_WRITE}, {1, DSP_STORAGE_ERR_NO_SPACE}, {2, DSP_STORAGE_ERR_COMMIT},
        {3, DSP_STORAGE_ERR_CONFIRM}, {4, DSP_STORAGE_ERR_CONFIRM},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        fresh(true);
        fake_kv_clear_faults(&kv);
        switch (cases[i].fault) {
        case 0: kv.fail_sets = true; break;
        case 1: kv.no_space = true; break;
        case 2: kv.fail_commits = true; break;
        case 3: kv.corrupt_next_readback = true; break;
        default: strcpy(kv.fail_get_key, "sched"); break; /* read-back read fails */
        }
        DSP_CHECK_EQ_INT(cases[i].expected, dsp_schedule_repo_create(&repo, &a, &hist));
        DSP_CHECK(!repo.loaded);
        fake_kv_clear_faults(&kv);
        boot_ok(); /* whatever is durable is a complete, valid set */
        DSP_CHECK(repo.set.count <= 1);

        dsp_config_t cfg;
        DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_load_config(&st, &cfg));
        cfg.revision++;
        cfg.reservoir_capacity_ul = 1000;
        switch (cases[i].fault) {
        case 0: kv.fail_sets = true; break;
        case 1: kv.no_space = true; break;
        case 2: kv.fail_commits = true; break;
        case 3: kv.corrupt_next_readback = true; break;
        default: break;
        }
        if (cases[i].fault != 4) {
            DSP_CHECK_EQ_INT(cases[i].expected, dsp_storage_save_config(&st, &cfg));
        }
        fake_kv_clear_faults(&kv);
    }
    /* Read failures during open and loads. */
    fresh(true);
    kv.fail_gets = true;
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_READ, reopen(true));
    fake_kv_clear_faults(&kv);
    strcpy(kv.fail_get_key, "h20");
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_READ, reopen(true));
    fake_kv_clear_faults(&kv);
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, reopen(true));
    strcpy(kv.fail_get_key, "sched");
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_READ, dsp_schedule_repo_load(&repo, &st));
    strcpy(kv.fail_get_key, "config");
    dsp_config_t cfg;
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_READ, dsp_storage_load_config(&st, &cfg));
    /* Provisioning on a full partition fails without a marker; nothing claimed valid. */
    fake_kv_init(&kv, true);
    kv.no_space = true;
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_NO_SPACE, reopen(true));
    DSP_CHECK(fake_kv_find(&kv, "layout") == NULL);
    /* Argument checks. */
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_ARG, dsp_storage_open(NULL, &kv.iface, TZ, true));
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_ARG, dsp_storage_open(&st, NULL, TZ, true));
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_ARG, dsp_storage_open(&st, &kv.iface, NULL, true));
}

/* ---- Codec: golden bytes (computed independently with Python struct) and bounds ----- */

static void test_codec_golden_bytes(void)
{
    dsp_history_record_t rec;
    memset(&rec, 0, sizeof rec);
    strcpy(rec.schedule_id, "ab");
    rec.local_date = D(2026, 11, 20);
    rec.state = DSP_HISTORY_STARTED;
    rec.schedule_revision = 3;
    rec.scheduled_at = 1795158000;
    rec.claimed_at = 1795158030;
    uint8_t h[DSP_HISTORY_RECORD_BYTES];
    DSP_CHECK_EQ_INT(92, dsp_codec_encode_history(&rec, h, sizeof h));
    static const uint8_t head[] = {0x02, 0x01, 0x01, 0x02, 0x61, 0x62, 0x00, 0x00};
    static const uint8_t tail[] = {0xEA, 0x07, 0x0B, 0x14, 0x03, 0x00, 0x00, 0x00, 0xF0, 0xEF,
                                   0xFF, 0x6A, 0x00, 0x00, 0x00, 0x00, 0x0E, 0xF0, 0xFF, 0x6A,
                                   0x00, 0x00, 0x00, 0x00};
    DSP_CHECK(memcmp(h, head, sizeof head) == 0);
    DSP_CHECK(memcmp(h + 68, tail, sizeof tail) == 0);
    for (size_t i = 6; i < 68; i++) {
        DSP_CHECK(h[i] == 0);
    }
    dsp_history_record_t back;
    DSP_CHECK_EQ_INT(DSP_CODEC_OK, dsp_codec_decode_history(h, sizeof h, &back));
    DSP_CHECK(dsp_history_record_equal(&rec, &back));

    dsp_config_t cfg = {2, 500000, 250000, 20, 24, 7};
    uint8_t c[DSP_CONFIG_RECORD_BYTES];
    static const uint8_t cexp[] = {0x04, 0x01, 0x02, 0x00, 0x00, 0x00, 0x20, 0xA1, 0x07, 0x00,
                                   0x90, 0xD0, 0x03, 0x00, 0x14, 0x18, 0x00, 0x07};
    DSP_CHECK_EQ_INT(18, dsp_codec_encode_config(&cfg, c, sizeof c));
    DSP_CHECK(memcmp(c, cexp, sizeof cexp) == 0);

    dsp_schedule_set_t set;
    memset(&set, 0, sizeof set);
    set.revision = 7;
    set.count = 1;
    dsp_schedule_t *s = &set.schedules[0];
    strcpy(s->id, "x1");
    s->revision = 2;
    s->enabled = true;
    s->amount_ul = 12500;
    s->start_date = D(2026, 11, 2);
    s->hour = 8;
    s->minute = 30;
    s->kind = DSP_REC_WEEKLY;
    s->interval = 3;
    s->weekday_mask = 0x09;
    s->has_end_date = true;
    s->end_date = D(2027, 1, 31);
    set.tombstone_count = 1;
    strcpy(set.tombstones[0], "old");
    uint8_t b[DSP_SCHEDULE_SET_MAX_BYTES];
    DSP_CHECK_EQ_INT(164, dsp_codec_encode_schedule_set(&set, TZ, b, sizeof b));
    static const uint8_t shdr[] = {0x03, 0x01, 0x07, 0x00, 0x00, 0x00, 0x01, 0x01};
    static const uint8_t etail[] = {0x02, 0x00, 0x00, 0x00, 0x01, 0xD4, 0x30, 0x00, 0x00,
                                    0xEA, 0x07, 0x0B, 0x02, 0x08, 0x1E, 0x02, 0x03, 0x00,
                                    0x00, 0x00, 0x09, 0x01, 0xEB, 0x07, 0x01, 0x1F};
    static const uint8_t thead[] = {0x03, 0x6F, 0x6C, 0x64};
    DSP_CHECK(memcmp(b, shdr, sizeof shdr) == 0);
    DSP_CHECK(b[8] == 2 && b[9] == 'x' && b[10] == '1');
    DSP_CHECK(memcmp(b + 8 + 65, etail, sizeof etail) == 0);
    DSP_CHECK(memcmp(b + 8 + 91, thead, sizeof thead) == 0);
    dsp_schedule_set_t sb;
    DSP_CHECK_EQ_INT(DSP_CODEC_OK, dsp_codec_decode_schedule_set(b, 164, TZ, &sb));
    DSP_CHECK(set_equal(&set, &sb));
}

static void test_codec_boundaries(void)
{
    /* Maximum record: 16 schedules and 16 tombstones with 64-character IDs. */
    static dsp_schedule_set_t max;
    memset(&max, 0, sizeof max);
    max.revision = UINT32_MAX - 1;
    for (int i = 0; i < DSP_SCHEDULE_CAPACITY; i++) {
        dsp_schedule_t s = sched("x", 23);
        memset(s.id, 'a' + i, DSP_SCHEDULE_ID_MAX_LEN);
        s.id[DSP_SCHEDULE_ID_MAX_LEN] = '\0';
        s.revision = UINT32_MAX;
        s.amount_ul = UINT32_MAX;
        s.minute = 59;
        s.interval = DSP_REC_INTERVAL_MAX;
        s.has_end_date = true;
        s.end_date = D(2099, 12, 31);
        max.schedules[i] = s;
        memset(max.tombstones[i], 'A' + i, DSP_SCHEDULE_ID_MAX_LEN);
        max.tombstones[i][DSP_SCHEDULE_ID_MAX_LEN] = '\0';
    }
    max.count = DSP_SCHEDULE_CAPACITY;
    max.tombstone_count = DSP_TOMBSTONE_CAPACITY;
    static uint8_t buf[DSP_SCHEDULE_SET_MAX_BYTES + 8];
    DSP_CHECK_EQ_INT(2504, DSP_SCHEDULE_SET_MAX_BYTES);
    DSP_CHECK_EQ_INT(DSP_SCHEDULE_SET_MAX_BYTES, dsp_codec_encode_schedule_set(&max, TZ, buf, sizeof buf));
    DSP_CHECK_EQ_INT(0, dsp_codec_encode_schedule_set(&max, TZ, buf, DSP_SCHEDULE_SET_MAX_BYTES - 1));
    static dsp_schedule_set_t back;
    DSP_CHECK_EQ_INT(DSP_CODEC_OK, dsp_codec_decode_schedule_set(buf, DSP_SCHEDULE_SET_MAX_BYTES, TZ, &back));
    DSP_CHECK(set_equal(&max, &back));
    /* Every truncation of the maximum record is rejected. */
    int accepted = 0;
    for (size_t len = 0; len < DSP_SCHEDULE_SET_MAX_BYTES; len++) {
        accepted += dsp_codec_decode_schedule_set(buf, len, TZ, &back) == DSP_CODEC_OK;
    }
    DSP_CHECK_EQ_INT(0, accepted);
    /* Count above capacity, non-zero ID padding, bad booleans, end-date bytes without flag. */
    buf[6] = DSP_SCHEDULE_CAPACITY + 1;
    DSP_CHECK_EQ_INT(DSP_CODEC_ERR_FIELD, dsp_codec_decode_schedule_set(buf, DSP_SCHEDULE_SET_MAX_BYTES, TZ, &back));
    dsp_schedule_set_t one;
    memset(&one, 0, sizeof one);
    one.count = 1;
    one.schedules[0] = sched("pad", 8);
    size_t n = dsp_codec_encode_schedule_set(&one, TZ, buf, sizeof buf);
    DSP_CHECK_EQ_INT(8 + 91, n);
    const size_t e = DSP_SCHEDULE_SET_HEADER_BYTES;
    const size_t mutate[] = {e + 10 /* ID padding */, e + 69 /* enabled=2 */, e + 86 /* has_end=2 */,
                             e + 88 /* end bytes w/o flag */, e + 80 /* kind */};
    for (size_t i = 0; i < sizeof mutate / sizeof mutate[0]; i++) {
        uint8_t copy[8 + 91];
        memcpy(copy, buf, n);
        copy[mutate[i]] = (uint8_t)(i == 4 ? 7 : 2);
        DSP_CHECK_EQ_INT(DSP_CODEC_ERR_FIELD, dsp_codec_decode_schedule_set(copy, n, TZ, &back));
    }
    /* History record with extreme valid values round-trips; invalid ones are refused. */
    dsp_history_record_t rec;
    memset(&rec, 0, sizeof rec);
    memset(rec.schedule_id, 'z', DSP_SCHEDULE_ID_MAX_LEN);
    rec.local_date = D(2099, 12, 31);
    rec.state = DSP_HISTORY_INTERRUPTED;
    rec.schedule_revision = UINT32_MAX;
    rec.scheduled_at = DSP_UTC_MAX;
    rec.claimed_at = DSP_UTC_MIN;
    uint8_t h[DSP_HISTORY_RECORD_BYTES];
    DSP_CHECK_EQ_INT(92, dsp_codec_encode_history(&rec, h, sizeof h));
    dsp_history_record_t hb;
    DSP_CHECK_EQ_INT(DSP_CODEC_OK, dsp_codec_decode_history(h, sizeof h, &hb));
    DSP_CHECK(dsp_history_record_equal(&rec, &hb));
    rec.state = 5;
    DSP_CHECK_EQ_INT(0, dsp_codec_encode_history(&rec, h, sizeof h));
    DSP_CHECK_EQ_INT(0, dsp_codec_encode_history(NULL, h, sizeof h - 1));
    h[2] = 9; /* unknown state in a stored record */
    DSP_CHECK_EQ_INT(DSP_CODEC_ERR_FIELD, dsp_codec_decode_history(h, sizeof h, &hb));
    h[2] = DSP_HISTORY_COMPLETED;
    h[76 + 7] = 0x80; /* scheduled_at negative (sign bit) */
    DSP_CHECK_EQ_INT(DSP_CODEC_ERR_FIELD, dsp_codec_decode_history(h, sizeof h, &hb));
    DSP_CHECK_EQ_INT(DSP_CODEC_ERR_SIZE, dsp_codec_decode_history(h, 1, &hb));
    DSP_CHECK_EQ_INT(DSP_CODEC_ERR_SIZE, dsp_codec_decode_history(NULL, 92, &hb));
    /* Invalid config is never encoded. */
    dsp_config_t cfg;
    dsp_config_defaults(&cfg);
    cfg.reservoir_remaining_ul = 1;
    uint8_t c[DSP_CONFIG_RECORD_BYTES];
    DSP_CHECK_EQ_INT(0, dsp_codec_encode_config(&cfg, c, sizeof c));
    DSP_CHECK(dsp_config_validate(NULL) != 0);
}

/* ---- Randomized operation sequences with faults and restarts ------------------------ */

static uint32_t rng = 777u;
static uint32_t rnd(uint32_t bound)
{
    rng = rng * 1664525u + 1013904223u;
    return (rng >> 8) % bound;
}

static void test_randomized_atomicity(void)
{
    static dsp_schedule_set_t model;
    static dsp_schedule_set_t attempted;
    int failures = 0;
    int both_states = 0;
    int bad = 0;
    for (int mode = 0; mode < 2; mode++) {
        fresh(mode == 1);
        model = repo.set;
        int next_id = 0;
        for (int step = 0; step < 400; step++) {
            uint32_t op = rnd(10);
            switch (rnd(8)) {
            case 0: kv.fail_sets = true; break;
            case 1: kv.fail_commits = true; break;
            case 2: kv.power_cut_after_sets = 1; break;
            case 3: kv.corrupt_next_readback = true; break;
            default: break;
            }
            dsp_storage_status_t s;
            if (op < 4 || repo.set.count == 0) {
                char id[24];
                snprintf(id, sizeof id, "rnd-%04d", next_id++);
                dsp_schedule_t sc = sched(id, (int)rnd(24));
                s = dsp_schedule_repo_create(&repo, &sc, &hist);
            } else if (op < 7) {
                dsp_schedule_t u = repo.set.schedules[rnd(repo.set.count)];
                u.revision++;
                u.hour = (uint8_t)rnd(24);
                u.amount_ul = 1000 + rnd(9000);
                s = dsp_schedule_repo_update(&repo, &u);
            } else {
                const dsp_schedule_t *d = &repo.set.schedules[rnd(repo.set.count)];
                s = dsp_schedule_repo_delete(&repo, d->id, d->revision);
            }
            attempted = repo.set; /* in-RAM result of the attempt (old set if refused early) */
            fake_kv_clear_faults(&kv);
            if (s == DSP_STORAGE_OK) {
                model = repo.set;
            } else if (s != DSP_STORAGE_ERR_CAPACITY) {
                failures++;
            }
            if (s != DSP_STORAGE_OK || rnd(6) == 0) {
                boot_ok(); /* restart: the durable set must be exactly old or new */
                bool is_old = set_equal(&repo.set, &model);
                bool is_new = set_equal(&repo.set, &attempted);
                if (!is_old && !is_new) {
                    bad++;
                }
                if (is_new && !is_old) {
                    both_states++;
                }
                model = repo.set;
            }
        }
    }
    DSP_CHECK_EQ_INT(0, bad);
    DSP_CHECK(failures > 100);  /* faults really happened ... */
    DSP_CHECK(both_states > 10); /* ... and some failed calls had already persisted */
}

/* ---- M1.4: layout v1 (M1.3) -> v2 migration ----------------------------------------- */

/* Rewrites the current storage into exactly what older firmware wrote: M1.3 (v1: no "time",
 * no "notify") or M1.4 (v2: no "notify"); all other records have identical formats. */
static void make_old_storage(int version)
{
    if (version == 1) {
        fake_kv_remove(&kv, "time");
    }
    fake_kv_remove(&kv, "notify");
    const dsp_layout_t old = {(uint16_t)version, 32, 16, 16};
    uint8_t m[DSP_LAYOUT_RECORD_BYTES];
    DSP_CHECK_EQ_INT(DSP_LAYOUT_RECORD_BYTES, dsp_codec_encode_layout(&old, m, sizeof m));
    fake_kv_put_raw(&kv, "layout", m, sizeof m);
}

static void make_v1_storage(void)
{
    make_old_storage(1);
    const dsp_layout_t v1 = {1, 32, 16, 16};
    uint8_t m[DSP_LAYOUT_RECORD_BYTES];
    DSP_CHECK_EQ_INT(DSP_LAYOUT_RECORD_BYTES, dsp_codec_encode_layout(&v1, m, sizeof m));
    static const uint8_t golden_v1[] = {0x01, 0x01, 0x01, 0x00, 0x20, 0x10, 0x10};
    DSP_CHECK(memcmp(m, golden_v1, sizeof m) == 0); /* the M1.3 marker bytes */
}

static void test_layout_migrations_to_v3(void)
{
    fresh(true);
    dsp_schedule_t a = sched("mig-a", 8);
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_schedule_repo_create(&repo, &a, &hist));
    dsp_config_t cfg;
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_load_config(&st, &cfg));
    cfg.revision = 1;
    cfg.reservoir_capacity_ul = 300000;
    cfg.reservoir_remaining_ul = 120000;
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_save_config(&st, &cfg));
    make_v1_storage();
    DSP_CHECK_EQ_INT(ALL_KEYS - 2, fake_kv_count(&kv));

    /* Read-only open never migrates. */
    fake_kv_clear_faults(&kv);
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_MIGRATION_REQUIRED, reopen(false));
    DSP_CHECK_EQ_INT(0, kv.sets);

    /* Migration: "time" and "notify" provisioned, marker v3 written last, data preserved. */
    fake_kv_clear_faults(&kv);
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, reopen(true));
    DSP_CHECK(st.migrated_now && !st.provisioned_now);
    DSP_CHECK_EQ_INT(3, kv.sets); /* "time", "notify", then "layout" */
    DSP_CHECK_EQ_INT(ALL_KEYS, fake_kv_count(&kv));
    dsp_layout_t layout;
    const fake_kv_entry_t *m = fake_kv_find(&kv, "layout");
    DSP_CHECK_EQ_INT(DSP_CODEC_OK, dsp_codec_decode_layout(m->value, m->len, &layout));
    DSP_CHECK_EQ_INT(3, layout.layout_version);
    dsp_time_meta_t meta;
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_load_time(&st, &meta));
    DSP_CHECK_EQ_INT(0, meta.last_sync_utc + meta.high_water_utc + meta.hold_until_utc);
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_schedule_repo_load(&repo, &st));
    DSP_CHECK(dsp_schedule_repo_find(&repo, "mig-a") != NULL);
    dsp_config_t back;
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_load_config(&st, &back));
    DSP_CHECK(dsp_config_equal(&cfg, &back));
    fake_kv_clear_faults(&kv);
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, reopen(true)); /* afterwards: plain v2 open, no writes */
    DSP_CHECK(!st.migrated_now);
    DSP_CHECK_EQ_INT(0, kv.sets);

    /* Interrupted migration (power lost after 1 or 2 new keys, before the marker): resumed. */
    for (int mode = 0; mode < 4; mode++) {
        fresh(mode % 2 == 1);
        make_v1_storage();
        kv.power_cut_after_sets = 1 + mode / 2;
        DSP_CHECK(reopen(true) != DSP_STORAGE_OK);
        fake_kv_power_cycle(&kv);
        DSP_CHECK_EQ_INT(DSP_STORAGE_OK, reopen(true));
        DSP_CHECK_EQ_INT(ALL_KEYS, fake_kv_count(&kv));
    }

    /* v1 marker next to a "time" record with real data: inconsistent, untouched. */
    fresh(true);
    dsp_time_meta_t real = {1795158000, 1795158000, 0, 5, 0};
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_save_time(&st, &real));
    const dsp_layout_t v1 = {1, 32, 16, 16};
    uint8_t mk[DSP_LAYOUT_RECORD_BYTES];
    DSP_CHECK_EQ_INT(DSP_LAYOUT_RECORD_BYTES, dsp_codec_encode_layout(&v1, mk, sizeof mk));
    fake_kv_put_raw(&kv, "layout", mk, sizeof mk);
    expect_open(DSP_STORAGE_ERR_CORRUPT);
    /* v3 marker with the "time" or "notify" key missing: corruption (explicit presence). */
    fresh(true);
    fake_kv_remove(&kv, "time");
    expect_open(DSP_STORAGE_ERR_CORRUPT);
    fresh(true);
    fake_kv_remove(&kv, "notify");
    expect_open(DSP_STORAGE_ERR_CORRUPT);

    /* v2 (M1.4) -> v3: "notify" provisioned, "time" metadata preserved. */
    fresh(true);
    dsp_time_meta_t tm = {1795158000, 1795158000, 0, 12, 1795158001};
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_save_time(&st, &tm));
    make_old_storage(2);
    DSP_CHECK_EQ_INT(ALL_KEYS - 1, fake_kv_count(&kv));
    fake_kv_clear_faults(&kv);
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_MIGRATION_REQUIRED, reopen(false));
    DSP_CHECK_EQ_INT(0, kv.sets);
    fake_kv_clear_faults(&kv);
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, reopen(true));
    DSP_CHECK(st.migrated_now);
    DSP_CHECK_EQ_INT(2, kv.sets); /* "notify", then "layout" */
    DSP_CHECK_EQ_INT(ALL_KEYS, fake_kv_count(&kv));
    dsp_time_meta_t tm_back;
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_load_time(&st, &tm_back));
    DSP_CHECK(dsp_time_meta_equal(&tm, &tm_back));
    dsp_notify_state_t ns;
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_load_notify(&st, &ns));
    DSP_CHECK_EQ_INT(0, ns.epoch + ns.next_seq);
    /* v2 marker next to a "notify" record with real data: inconsistent. */
    fresh(true);
    dsp_notify_state_t real_ns;
    memset(&real_ns, 0, sizeof real_ns);
    real_ns.epoch = 7;
    real_ns.next_seq = 1;
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_save_notify(&st, &real_ns));
    const dsp_layout_t v2m = {2, 32, 16, 16};
    uint8_t mk2[DSP_LAYOUT_RECORD_BYTES];
    DSP_CHECK_EQ_INT(DSP_LAYOUT_RECORD_BYTES, dsp_codec_encode_layout(&v2m, mk2, sizeof mk2));
    fake_kv_put_raw(&kv, "layout", mk2, sizeof mk2);
    expect_open(DSP_STORAGE_ERR_CORRUPT);
    /* Corrupted v1 data blocks the migration (nothing written). */
    fresh(true);
    make_v1_storage();
    fake_kv_remove(&kv, "h04");
    expect_open(DSP_STORAGE_ERR_CORRUPT);
}

static void test_time_record_codec_and_storage(void)
{
    dsp_time_meta_t m = {1795158000, 1795244400, 1794639600, -86400, 1795244401};
    uint8_t b[DSP_TIME_RECORD_BYTES];
    DSP_CHECK_EQ_INT(42, dsp_codec_encode_time(&m, b, sizeof b));
    /* Golden bytes (Python struct '<BBqqqqq'). */
    static const uint8_t golden[] = {0x05, 0x01, 0xF0, 0xEF, 0xFF, 0x6A, 0x00, 0x00, 0x00, 0x00,
                                     0x70, 0x41, 0x01, 0x6B, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x06,
                                     0xF8, 0x6A, 0x00, 0x00, 0x00, 0x00, 0x80, 0xAE, 0xFE, 0xFF,
                                     0xFF, 0xFF, 0xFF, 0xFF, 0x71, 0x41, 0x01, 0x6B, 0x00, 0x00,
                                     0x00, 0x00};
    DSP_CHECK(memcmp(b, golden, sizeof golden) == 0);
    dsp_time_meta_t back;
    DSP_CHECK_EQ_INT(DSP_CODEC_OK, dsp_codec_decode_time(b, sizeof b, &back));
    DSP_CHECK(dsp_time_meta_equal(&m, &back));
    DSP_CHECK_EQ_INT(DSP_CODEC_ERR_SIZE, dsp_codec_decode_time(b, sizeof b - 1, &back));
    b[1] = 2;
    DSP_CHECK_EQ_INT(DSP_CODEC_ERR_VERSION, dsp_codec_decode_time(b, sizeof b, &back));
    dsp_time_meta_t bad = m;
    bad.high_water_utc = 5; /* not 0 and below the supported range */
    DSP_CHECK_EQ_INT(0, dsp_codec_encode_time(&bad, b, sizeof b));

    fresh(true);
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_save_time(&st, &m));
    boot_ok();
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_load_time(&st, &back));
    DSP_CHECK(dsp_time_meta_equal(&m, &back));
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_INVALID, dsp_storage_save_time(&st, &bad));
    fake_kv_clear_faults(&kv);
    kv.fail_sets = true;
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_WRITE, dsp_storage_save_time(&st, &m));
    fake_kv_clear_faults(&kv);
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_ARG, dsp_storage_save_time(&st, NULL));
}

int main(void)
{
    DSP_RUN(test_first_boot_provisions_every_key);
    DSP_RUN(test_config_save_and_reload);
    DSP_RUN(test_schedule_crud_survives_restart);
    DSP_RUN(test_schedule_id_rules);
    DSP_RUN(test_schedule_capacity_and_tombstones);
    DSP_RUN(test_power_loss_around_commit);
    DSP_RUN(test_interrupted_provisioning_resumes);
    DSP_RUN(test_marker_lost_with_data_is_corrupt);
    DSP_RUN(test_invalid_serialized_data);
    DSP_RUN(test_unsupported_versions);
    DSP_RUN(test_history_compatibility_with_m12);
    DSP_RUN(test_storage_failures);
    DSP_RUN(test_codec_golden_bytes);
    DSP_RUN(test_codec_boundaries);
    DSP_RUN(test_randomized_atomicity);
    DSP_RUN(test_layout_migrations_to_v3);
    DSP_RUN(test_time_record_codec_and_storage);
    return dsp_test_finish();
}
