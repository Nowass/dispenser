/*
 * Notification engine tests (M1.5): condition detection, deduplication, 7-day reminders,
 * the bounded persistent queue (capacity, eviction, overflow reporting), at-least-once
 * delivery with stable identities, durability across restarts and power loss, clock
 * corrections, reservoir consumption accounting, and the "notify" record codec.
 *
 * Storage is the M1.3 storage layer over the in-memory fake KV; re-opening storage on the
 * same fake models a restart with persistent flash.
 */
#include "dsp_codec.h"
#include "dsp_notify.h"
#include "dsp_reservoir.h"
#include "dsp_storage.h"
#include "dsp_test.h"
#include "dsp_timekeeping.h"
#include "fake_kv.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const dsp_tz_t *const TZ = &DSP_TZ_EUROPE_PRAGUE;

#define DAY 86400
#define WEEK (7 * DAY)
/* 2026-11-20T07:00:00Z */
#define T0 ((dsp_utc_t)1795158000)
#define RANDOM_EPOCH 0xA5A5A5A5u
#define CAP 100000u /* 100 ml */

static fake_kv_t kv;
static dsp_storage_t st;
static dsp_notify_t n;

static dsp_notify_clock_t at(dsp_utc_t t)
{
    dsp_notify_clock_t c = {t, true};
    return c;
}

static const dsp_notify_clock_t UNKNOWN_CLOCK = {1000, false};

/* Restart: RAM lost, flash kept. */
static void boot_with(uint32_t random)
{
    fake_kv_power_cycle(&kv);
    fake_kv_clear_faults(&kv);
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_open(&st, &kv.iface, TZ, true));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_init(&n, &st, random));
    kv.sets = 0;
}

static void boot(void)
{
    boot_with(RANDOM_EPOCH);
}

static void fresh(void)
{
    fake_kv_init(&kv, true);
    boot();
}

static dsp_config_t reservoir(uint32_t cap, uint32_t rem)
{
    dsp_config_t c;
    dsp_config_defaults(&c); /* 20 % threshold */
    c.reservoir_capacity_ul = cap;
    c.reservoir_remaining_ul = rem;
    return c;
}

static dsp_notify_status_t obs_res(uint32_t rem, bool blocked, dsp_notify_clock_t clock)
{
    dsp_config_t c = reservoir(CAP, rem);
    return dsp_notify_observe_reservoir(&n, &c, blocked, clock);
}

static dsp_notify_state_t stored(void)
{
    dsp_notify_state_t s;
    memset(&s, 0xEE, sizeof s);
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_load_notify(&st, &s));
    return s;
}

static bool state_equal(const dsp_notify_state_t *a, const dsp_notify_state_t *b)
{
    uint8_t ea[DSP_NOTIFY_RECORD_BYTES];
    uint8_t eb[DSP_NOTIFY_RECORD_BYTES];
    return dsp_codec_encode_notify(a, ea, sizeof ea) == DSP_NOTIFY_RECORD_BYTES &&
           dsp_codec_encode_notify(b, eb, sizeof eb) == DSP_NOTIFY_RECORD_BYTES &&
           memcmp(ea, eb, sizeof ea) == 0;
}

/* The persisted state equals the RAM state (nothing pending a write). */
static bool in_sync(void)
{
    dsp_notify_state_t s = stored();
    return !n.dirty && state_equal(&s, &n.s);
}

static const dsp_note_t *find_cond(uint8_t cond)
{
    for (int i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY; i++) {
        if (n.s.queue[i].seq != 0 && n.s.queue[i].condition == cond) {
            return &n.s.queue[i];
        }
    }
    return NULL;
}

static int count_type(uint8_t type)
{
    int c = 0;
    for (int i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY; i++) {
        c += n.s.queue[i].seq != 0 && n.s.queue[i].type == type;
    }
    return c;
}

/* ---- Fake transport ------------------------------------------------------------------- */

#define TX_LOG 256
static struct {
    dsp_tx_result_t script[64];
    int script_len;
    dsp_tx_result_t fallback;
    int calls;
    dsp_note_t log[TX_LOG];
    uint32_t epochs[TX_LOG];
    bool fail_storage_after_send; /* simulate losing the post-delivery write */
    dsp_tx_detail_t detail[64];   /* detail returned with script[i] (M1.7) */
    int budget;                   /* can_send(): -1 = unlimited, else sends still allowed */
    int can_send_calls;
} tx;

static dsp_tx_result_t tx_send(void *ctx, const dsp_note_t *note, uint32_t epoch,
                               dsp_tx_detail_t *detail)
{
    (void)ctx;
    DSP_CHECK(detail != NULL);
    if (detail != NULL) {
        /* The engine hands over a zeroed detail for every attempt. */
        DSP_CHECK_EQ_INT(0, detail->http_status + detail->error + detail->field + detail->diag);
    }
    int i = tx.calls++;
    if (i < TX_LOG) {
        tx.log[i] = *note;
        tx.epochs[i] = epoch;
    }
    /* Durable before sent: whatever is sent is already in flash, unchanged. */
    dsp_notify_state_t s = stored();
    bool found = false;
    for (int k = 0; k < DSP_NOTIFY_QUEUE_CAPACITY; k++) {
        if (s.queue[k].seq == note->seq) {
            /* Same content AND durably frozen (attempts > 0) before it leaves the device. */
            found = s.queue[k].type == note->type && s.queue[k].reminder_no == note->reminder_no &&
                    s.queue[k].value == note->value && s.queue[k].value2 == note->value2 &&
                    s.queue[k].code == note->code && s.queue[k].flags == note->flags &&
                    s.queue[k].attempts > 0;
        }
    }
    DSP_CHECK(found);
    DSP_CHECK_EQ_INT(s.epoch, epoch);
    if (tx.fail_storage_after_send) {
        kv.fail_sets = true;
    }
    if (i < tx.script_len && detail != NULL) {
        *detail = tx.detail[i];
    }
    return i < tx.script_len ? tx.script[i] : tx.fallback;
}

static bool tx_can_send(void *ctx)
{
    (void)ctx;
    tx.can_send_calls++;
    if (tx.budget < 0) {
        return true;
    }
    if (tx.budget == 0) {
        return false;
    }
    tx.budget--;
    return true;
}

static const dsp_notify_transport_t TX = {NULL, tx_send, tx_can_send};

static void tx_reset(dsp_tx_result_t fallback)
{
    memset(&tx, 0, sizeof tx);
    tx.fallback = fallback;
    tx.budget = -1;
}

static dsp_notify_delivery_report_t deliver_all(dsp_tx_result_t result, dsp_notify_clock_t clock)
{
    tx_reset(result);
    return dsp_notify_deliver(&n, &TX, 100, clock);
}

static void id_of(const dsp_note_t *e, uint32_t epoch, char *buf)
{
    DSP_CHECK(dsp_notify_format_id(0x0123456789ABull, epoch, e->seq, buf, DSP_NOTIFY_ID_BUF_LEN));
}

/* ---- Detection ------------------------------------------------------------------------ */

static void test_reservoir_threshold_crossing(void)
{
    fresh();
    DSP_CHECK_EQ_INT(0, dsp_notify_pending_count(&n));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(25000, false, at(T0)));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(20000, false, at(T0))); /* exactly 20 %: not low */
    DSP_CHECK_EQ_INT(0, dsp_notify_pending_count(&n));
    DSP_CHECK_EQ_INT(0, kv.sets); /* nothing changed, nothing written */

    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(19999, false, at(T0 + 60)));
    DSP_CHECK_EQ_INT(1, dsp_notify_pending_count(&n));
    DSP_CHECK_EQ_INT(1, kv.sets); /* one atomic write: latch + entry */
    const dsp_note_t *e = find_cond(DSP_COND_RESERVOIR_LOW);
    DSP_CHECK(e != NULL);
    if (e != NULL) {
        DSP_CHECK_EQ_INT(DSP_NOTE_RESERVOIR_LOW, e->type);
        DSP_CHECK_EQ_INT(DSP_SEV_WARNING, e->severity);
        DSP_CHECK_EQ_INT(19999, e->value);
        DSP_CHECK_EQ_INT(CAP, e->value2);
        DSP_CHECK_EQ_INT(0, e->reminder_no);
        DSP_CHECK_EQ_INT(DSP_NOTE_F_TIME_VALID, e->flags);
        DSP_CHECK_EQ_INT(T0 + 60, e->created_at);
        DSP_CHECK_EQ_INT(1, e->seq);
        DSP_CHECK_EQ_INT(0, e->attempts);
    }
    DSP_CHECK_EQ_INT(RANDOM_EPOCH, n.s.epoch);
    DSP_CHECK(n.s.cond[DSP_COND_RESERVOIR_LOW].flags & DSP_COND_F_ACTIVE);
    DSP_CHECK_EQ_INT(T0 + 60, n.s.cond[DSP_COND_RESERVOIR_LOW].opened_at);
    DSP_CHECK_EQ_INT(T0 + 60 + WEEK, n.s.cond[DSP_COND_RESERVOIR_LOW].next_reminder_at);
    DSP_CHECK(in_sync());

    /* Unconfigured reservoir (capacity 0) is "unknown", never low; it ends an episode. */
    dsp_config_t unconf = reservoir(0, 0);
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_reservoir(&n, &unconf, false, at(T0)));
    DSP_CHECK_EQ_INT(0, dsp_notify_pending_count(&n));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_reservoir(&n, &unconf, false, at(T0)));
    DSP_CHECK_EQ_INT(0, dsp_notify_pending_count(&n));

    /* Invalid configuration / arguments. */
    dsp_config_t bad = reservoir(100, 200);
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_ARG, dsp_notify_observe_reservoir(&n, &bad, false, at(T0)));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_ARG, dsp_notify_observe_reservoir(&n, NULL, false, at(T0)));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_ARG, dsp_notify_observe_reservoir(NULL, &unconf, false, at(T0)));
}

static void test_low_threshold_variants(void)
{
    /* Threshold 99 % -> recovery capped at 100 %; threshold 1 %. */
    fresh();
    dsp_config_t c = reservoir(CAP, 98000);
    c.low_stock_percent = 99;
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_reservoir(&n, &c, false, at(T0)));
    DSP_CHECK_EQ_INT(1, dsp_notify_pending_count(&n));
    c.reservoir_remaining_ul = 99999;
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_reservoir(&n, &c, false, at(T0)));
    DSP_CHECK_EQ_INT(1, dsp_notify_pending_count(&n));
    c.reservoir_remaining_ul = CAP;
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_reservoir(&n, &c, false, at(T0)));
    DSP_CHECK_EQ_INT(0, dsp_notify_pending_count(&n));

    c.low_stock_percent = 1;
    c.reservoir_remaining_ul = 1000; /* exactly 1 % */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_reservoir(&n, &c, false, at(T0)));
    DSP_CHECK_EQ_INT(0, dsp_notify_pending_count(&n));
    c.reservoir_remaining_ul = 999;
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_reservoir(&n, &c, false, at(T0)));
    DSP_CHECK_EQ_INT(1, dsp_notify_pending_count(&n));
    /* Large capacities: no 32-bit overflow in the percentage arithmetic. */
    c.reservoir_capacity_ul = UINT32_MAX;
    c.reservoir_remaining_ul = UINT32_MAX / 2;
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_reservoir(&n, &c, false, at(T0)));
    DSP_CHECK_EQ_INT(0, dsp_notify_pending_count(&n));
}

static void test_refill_ends_episode(void)
{
    fresh();
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_reservoir_refill(&st, CAP, 15000, NULL));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_set_estimate_uncertain(&n, true));
    dsp_config_t cfg;
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_load_config(&st, &cfg));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_reservoir(&n, &cfg, false, at(T0)));
    const dsp_note_t *e = find_cond(DSP_COND_RESERVOIR_LOW);
    DSP_CHECK(e != NULL && (e->flags & DSP_NOTE_F_UNCERTAIN));
    /* Delivered, then the user refills (FR-023): episode ends, uncertainty cleared. */
    DSP_CHECK_EQ_INT(1, deliver_all(DSP_TX_ACK, at(T0)).acked);
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_reservoir_refill(&st, CAP, CAP, &cfg));
    DSP_CHECK_EQ_INT(CAP, cfg.reservoir_remaining_ul);
    kv.sets = 0;
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_on_refill(&n, &cfg, at(T0 + DAY)));
    DSP_CHECK_EQ_INT(1, kv.sets); /* uncertainty cleared + episode closed: one write */
    DSP_CHECK_EQ_INT(0, n.s.cond[DSP_COND_RESERVOIR_LOW].flags);
    DSP_CHECK_EQ_INT(0, n.s.flags);
    DSP_CHECK_EQ_INT(0, dsp_notify_pending_count(&n));
    DSP_CHECK(in_sync());
    /* A later drop opens a NEW episode with a new identity, no uncertainty. */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(5000, false, at(T0 + 2 * DAY)));
    e = find_cond(DSP_COND_RESERVOIR_LOW);
    DSP_CHECK(e != NULL && e->seq == 2 && !(e->flags & DSP_NOTE_F_UNCERTAIN));
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_INVALID, dsp_reservoir_refill(&st, 10, 11, NULL));
    /* Refill also ends BLOCKED (FR-023) and persists a lone uncertainty change. */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(0, true, at(T0 + 3 * DAY)));
    DSP_CHECK(find_cond(DSP_COND_RESERVOIR_BLOCKED) != NULL);
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_reservoir_refill(&st, CAP, CAP, &cfg));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_on_refill(&n, &cfg, at(T0 + 3 * DAY)));
    DSP_CHECK(find_cond(DSP_COND_RESERVOIR_BLOCKED) == NULL);
    DSP_CHECK_EQ_INT(0, n.s.cond[DSP_COND_RESERVOIR_BLOCKED].flags);
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_set_estimate_uncertain(&n, true));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_on_refill(&n, &cfg, at(T0 + 3 * DAY)));
    DSP_CHECK_EQ_INT(0, n.s.flags);
    DSP_CHECK(in_sync());
    dsp_config_t bad = reservoir(10, 11);
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_ARG, dsp_notify_on_refill(&n, &bad, at(T0)));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_ARG, dsp_notify_on_refill(&n, NULL, at(T0)));
}

static void test_hysteresis(void)
{
    fresh();
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(19000, false, at(T0)));
    uint32_t first = find_cond(DSP_COND_RESERVOIR_LOW)->seq;
    kv.sets = 0;
    /* Oscillation around the threshold: no new event, no write. */
    const uint32_t wobble[] = {20000, 19999, 21000, 24999, 19500, 24000};
    for (size_t i = 0; i < sizeof wobble / sizeof wobble[0]; i++) {
        DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(wobble[i], false, at(T0 + (dsp_utc_t)i)));
    }
    DSP_CHECK_EQ_INT(0, kv.sets);
    DSP_CHECK_EQ_INT(1, dsp_notify_pending_count(&n));
    DSP_CHECK_EQ_INT(first, find_cond(DSP_COND_RESERVOIR_LOW)->seq);
    /* Recovery at threshold + 5 points (25 %): episode ends, undelivered warning withdrawn. */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(25000, false, at(T0 + 10)));
    DSP_CHECK_EQ_INT(0, dsp_notify_pending_count(&n));
    DSP_CHECK_EQ_INT(1, kv.sets);
    DSP_CHECK(in_sync());
    /* Dropping again: a new episode, a new sequence number (never reused). */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(19999, false, at(T0 + 20)));
    DSP_CHECK_EQ_INT(first + 1, find_cond(DSP_COND_RESERVOIR_LOW)->seq);
}

static void test_repeated_observations_and_dedup(void)
{
    fresh();
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(10000, true, at(T0)));
    DSP_CHECK_EQ_INT(2, dsp_notify_pending_count(&n)); /* LOW (warning) + BLOCKED (critical) */
    DSP_CHECK_EQ_INT(1, kv.sets);                      /* both in one write */
    const dsp_note_t *b = find_cond(DSP_COND_RESERVOIR_BLOCKED);
    DSP_CHECK(b != NULL && b->severity == DSP_SEV_CRITICAL && b->type == DSP_NOTE_RESERVOIR_BLOCKED);
    kv.sets = 0;
    for (int i = 0; i < 50; i++) {
        DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(10000 - (uint32_t)i * 10, true, at(T0 + i * 3600)));
        DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_tick(&n, at(T0 + i * 3600)));
    }
    DSP_CHECK_EQ_INT(2, dsp_notify_pending_count(&n));
    DSP_CHECK_EQ_INT(0, kv.sets); /* the payload refresh stays in RAM */
    DSP_CHECK_EQ_INT(9510, n.s.cond[DSP_COND_RESERVOIR_LOW].value);
    DSP_CHECK_EQ_INT(9510, find_cond(DSP_COND_RESERVOIR_LOW)->value); /* unsent: latest value */
    /* BLOCKED clears independently of LOW. */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(9000, false, at(T0 + DAY)));
    DSP_CHECK(find_cond(DSP_COND_RESERVOIR_BLOCKED) == NULL);
    DSP_CHECK(find_cond(DSP_COND_RESERVOIR_LOW) != NULL);
    DSP_CHECK(in_sync());
}

static void test_battery(void)
{
    fresh();
    dsp_battery_obs_t unknown = {DSP_BATTERY_UNKNOWN, 0};
    dsp_battery_obs_t low = {DSP_BATTERY_LOW, 3310};
    dsp_battery_obs_t ok = {DSP_BATTERY_OK, 3900};
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_battery(&n, unknown, at(T0)));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_battery(&n, ok, at(T0)));
    DSP_CHECK_EQ_INT(0, dsp_notify_pending_count(&n));
    DSP_CHECK_EQ_INT(0, kv.sets);
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_battery(&n, low, at(T0)));
    const dsp_note_t *e = find_cond(DSP_COND_BATTERY_LOW);
    DSP_CHECK(e != NULL && e->type == DSP_NOTE_BATTERY_LOW && e->severity == DSP_SEV_WARNING &&
              e->value == 3310 && e->value2 == 0);
    kv.sets = 0;
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_battery(&n, low, at(T0 + 1)));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_battery(&n, unknown, at(T0 + 2)));
    DSP_CHECK_EQ_INT(0, kv.sets);
    DSP_CHECK(find_cond(DSP_COND_BATTERY_LOW) != NULL); /* unknown never resolves */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_battery(&n, ok, at(T0 + 3)));
    DSP_CHECK(find_cond(DSP_COND_BATTERY_LOW) == NULL);
    DSP_CHECK_EQ_INT(0, n.s.cond[DSP_COND_BATTERY_LOW].flags);
    dsp_battery_obs_t bad = {(dsp_battery_status_t)7, 0};
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_ARG, dsp_notify_observe_battery(&n, bad, at(T0)));
}

static void test_dosing_failure(void)
{
    fresh();
    dsp_dosing_error_t e1 = {DSP_DOSEERR_PUMP_FAILED, false, 1500};
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_report_dosing_error(&n, &e1, at(T0)));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_report_dosing_error(&n, &e1, at(T0 + DAY)));
    DSP_CHECK_EQ_INT(2, count_type(DSP_NOTE_DOSING_ERROR)); /* distinct failures, one-shot each */
    const dsp_note_t *e = &n.s.queue[0];
    DSP_CHECK_EQ_INT(DSP_SEV_CRITICAL, e->severity);
    DSP_CHECK_EQ_INT(DSP_COND_NONE, e->condition);
    DSP_CHECK_EQ_INT(DSP_DOSEERR_PUMP_FAILED, e->code);
    DSP_CHECK_EQ_INT(1500, e->value);
    DSP_CHECK(!(e->flags & DSP_NOTE_F_MANUAL));
    DSP_CHECK(in_sync());
    /* One-shot: never reminded, even a month later. */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_tick(&n, at(T0 + 30 * DAY)));
    DSP_CHECK_EQ_INT(2, dsp_notify_pending_count(&n));
    for (int i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY; i++) {
        DSP_CHECK(!(n.s.queue[i].flags & DSP_NOTE_F_REMINDER));
    }
    /* Every defined code is accepted; others are rejected. */
    for (uint16_t code = 1; code <= DSP_DOSEERR_MAX; code++) {
        dsp_dosing_error_t ec = {code, true, 10};
        DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_report_dosing_error(&n, &ec, at(T0)));
    }
    dsp_dosing_error_t bad0 = {0, false, 1};
    dsp_dosing_error_t bad6 = {DSP_DOSEERR_MAX + 1, false, 1};
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_ARG, dsp_notify_report_dosing_error(&n, &bad0, at(T0)));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_ARG, dsp_notify_report_dosing_error(&n, &bad6, at(T0)));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_ARG, dsp_notify_report_dosing_error(&n, NULL, at(T0)));
    DSP_CHECK_EQ_INT(7, dsp_notify_pending_count(&n));
}

static void test_interrupted_dosing_uncertain(void)
{
    fresh();
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_reservoir_refill(&st, CAP, 21500, NULL));
    /* Manual dose of 2 ml authorized: consumed BEFORE the pump runs, then interrupted. */
    dsp_config_t cfg;
    dsp_dose_permit_t permit;
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_reservoir_consume(&st, 2000, DSP_DOSE_MANUAL, &cfg, &permit));
    DSP_CHECK(permit.granted);
    dsp_dosing_error_t e = {DSP_DOSEERR_INTERRUPTED, true, 2000};
    kv.sets = 0;
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_report_dosing_error(&n, &e, at(T0)));
    DSP_CHECK_EQ_INT(1, kv.sets); /* event and uncertainty in ONE write */
    DSP_CHECK(stored().flags & DSP_NOTIFY_F_ESTIMATE_UNCERTAIN);
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_reservoir(&n, &cfg, false, at(T0)));
    const dsp_note_t *d = NULL;
    for (int i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY; i++) {
        if (n.s.queue[i].type == DSP_NOTE_DOSING_ERROR) {
            d = &n.s.queue[i];
        }
    }
    DSP_CHECK(d != NULL && d->code == DSP_DOSEERR_INTERRUPTED && (d->flags & DSP_NOTE_F_MANUAL) &&
              d->value == 2000 && !(d->flags & DSP_NOTE_F_UNCERTAIN));
    const dsp_note_t *low = find_cond(DSP_COND_RESERVOIR_LOW);
    DSP_CHECK(low != NULL && (low->flags & DSP_NOTE_F_UNCERTAIN) && low->value == 19500);
    /* The uncertainty survives restarts until refill. */
    boot();
    DSP_CHECK(n.s.flags & DSP_NOTIFY_F_ESTIMATE_UNCERTAIN);
    kv.sets = 0;
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_set_estimate_uncertain(&n, true));
    DSP_CHECK_EQ_INT(0, kv.sets); /* idempotent */
    /* A reminder of the uncertain estimate is marked uncertain too. */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_tick(&n, at(T0 + WEEK)));
    low = find_cond(DSP_COND_RESERVOIR_LOW);
    DSP_CHECK(low != NULL && low->reminder_no == 1 && (low->flags & DSP_NOTE_F_UNCERTAIN));
    /* Which dosing errors make the estimate uncertain: only those after consumption. */
    const struct {
        uint16_t code;
        bool uncertain;
    } codes[] = {{DSP_DOSEERR_START_FAILED, true}, {DSP_DOSEERR_PUMP_FAILED, true},
                 {DSP_DOSEERR_INTERRUPTED, true},  {DSP_DOSEERR_STORAGE, false},
                 {DSP_DOSEERR_BLOCKED, false}};
    for (size_t i = 0; i < sizeof codes / sizeof codes[0]; i++) {
        fresh();
        dsp_dosing_error_t ec = {codes[i].code, false, 100};
        DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_report_dosing_error(&n, &ec, at(T0)));
        DSP_CHECK_EQ_INT(codes[i].uncertain, (stored().flags & DSP_NOTIFY_F_ESTIMATE_UNCERTAIN) != 0);
    }
    /* Even when the event itself does not fit (queue full), the flag is persisted. */
    fresh();
    dsp_dosing_error_t blk = {DSP_DOSEERR_BLOCKED, false, 1};
    for (int i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY; i++) {
        DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_report_dosing_error(&n, &blk, at(T0)));
    }
    dsp_dosing_error_t pf = {DSP_DOSEERR_PUMP_FAILED, false, 1};
    DSP_CHECK_EQ_INT(DSP_NOTIFY_QUEUE_FULL, dsp_notify_report_dosing_error(&n, &pf, at(T0)));
    DSP_CHECK(stored().flags & DSP_NOTIFY_F_ESTIMATE_UNCERTAIN);
}

static void test_storage_and_device_errors(void)
{
    fresh();
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_set_device_condition(&n, DSP_COND_STORAGE, true, 4, at(T0)));
    const dsp_note_t *e = find_cond(DSP_COND_STORAGE);
    DSP_CHECK(e != NULL && e->type == DSP_NOTE_DEVICE_ERROR && e->code == DSP_DEVERR_STORAGE &&
              e->severity == DSP_SEV_CRITICAL && e->value == 4);
    kv.sets = 0;
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_set_device_condition(&n, DSP_COND_STORAGE, true, 5, at(T0)));
    DSP_CHECK_EQ_INT(0, kv.sets);
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_set_device_condition(&n, DSP_COND_STORAGE, false, 0, at(T0)));
    DSP_CHECK(find_cond(DSP_COND_STORAGE) == NULL);
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_ARG,
                     dsp_notify_set_device_condition(&n, DSP_COND_BATTERY_LOW, true, 0, at(T0)));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_ARG,
                     dsp_notify_set_device_condition(&n, DSP_COND_RESERVOIR_LOW, true, 0, at(T0)));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_ARG,
                     dsp_notify_set_device_condition(&n, DSP_COND_COUNT, true, 0, at(T0)));

    /* Time conditions mapped from the M1.4 timekeeper. */
    dsp_time_meta_t meta;
    memset(&meta, 0, sizeof meta);
    dsp_time_retained_t ret;
    memset(&ret, 0, sizeof ret);
    ret.consecutive_failures = DSP_NOTIFY_CLOCK_FAILURES - 1;
    dsp_timekeeper_t tk;
    DSP_CHECK(dsp_timekeeper_init(&tk, &meta, 24, &ret));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_time(&n, &tk, 5000));
    DSP_CHECK(find_cond(DSP_COND_CLOCK) == NULL);
    tk.retained.consecutive_failures = DSP_NOTIFY_CLOCK_FAILURES;
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_time(&n, &tk, 5000));
    e = find_cond(DSP_COND_CLOCK);
    DSP_CHECK(e != NULL && e->code == DSP_DEVERR_CLOCK && e->value == DSP_NOTIFY_CLOCK_FAILURES &&
              e->created_at == 0 && !(e->flags & DSP_NOTE_F_TIME_VALID));
    DSP_CHECK(find_cond(DSP_COND_TIME_SYNC) == NULL);
    /* Synchronized: CLOCK ends; later the sync becomes overdue (> 2 intervals). */
    tk.trust = DSP_TRUST_SYNCED;
    tk.retained.consecutive_failures = 0;
    tk.meta.last_sync_utc = T0;
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_time(&n, &tk, T0 + DAY));
    DSP_CHECK(find_cond(DSP_COND_CLOCK) == NULL);
    DSP_CHECK(find_cond(DSP_COND_TIME_SYNC) == NULL);
    tk.trust = DSP_TRUST_HOLDOVER;
    tk.retained.consecutive_failures = 9;
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_time(&n, &tk, T0 + 2 * DAY + 1));
    e = find_cond(DSP_COND_TIME_SYNC);
    DSP_CHECK(e != NULL && e->code == DSP_DEVERR_TIME_SYNC && e->created_at == T0 + 2 * DAY + 1);
    DSP_CHECK(find_cond(DSP_COND_CLOCK) == NULL); /* trusted: not a clock problem */
    tk.meta.last_sync_utc = T0 + 2 * DAY;
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_time(&n, &tk, T0 + 2 * DAY + 2));
    DSP_CHECK(find_cond(DSP_COND_TIME_SYNC) == NULL);
    tk.initialized = false;
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_ARG, dsp_notify_observe_time(&n, &tk, T0));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_ARG, dsp_notify_observe_time(&n, NULL, T0));
}

/* ---- Delivery ------------------------------------------------------------------------- */

static void test_wifi_failure_keeps_entry(void)
{
    fresh();
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(1000, false, at(T0)));
    DSP_CHECK(dsp_notify_delivery_requested(&n));
    dsp_notify_delivery_report_t r = deliver_all(DSP_TX_FAILED, at(T0));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, r.status);
    DSP_CHECK_EQ_INT(1, r.attempted);
    DSP_CHECK_EQ_INT(0, r.acked);
    DSP_CHECK(r.stopped_early);
    DSP_CHECK_EQ_INT(1, dsp_notify_pending_count(&n));
    DSP_CHECK_EQ_INT(1, find_cond(DSP_COND_RESERVOIR_LOW)->attempts);
    DSP_CHECK(in_sync()); /* attempt count persisted */
    DSP_CHECK(dsp_notify_delivery_requested(&n));
}

static void test_ack_removes_entry(void)
{
    fresh();
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(1000, false, at(T0)));
    dsp_dosing_error_t e = {DSP_DOSEERR_START_FAILED, false, 100};
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_report_dosing_error(&n, &e, at(T0)));
    dsp_notify_delivery_report_t r = deliver_all(DSP_TX_ACK, at(T0));
    DSP_CHECK_EQ_INT(2, r.attempted);
    DSP_CHECK_EQ_INT(2, r.acked);
    DSP_CHECK(!r.stopped_early);
    DSP_CHECK_EQ_INT(0, dsp_notify_pending_count(&n));
    DSP_CHECK(!dsp_notify_delivery_requested(&n));
    DSP_CHECK_EQ_INT(0, dsp_notify_highest_pending_severity(&n));
    DSP_CHECK(in_sync());
    /* The condition stays latched (episode open): no new event without a change. */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(900, false, at(T0 + 1)));
    DSP_CHECK_EQ_INT(0, dsp_notify_pending_count(&n));
    boot();
    DSP_CHECK_EQ_INT(0, dsp_notify_pending_count(&n));
    /* Nothing pending: delivery sends nothing and writes nothing. */
    kv.sets = 0;
    r = deliver_all(DSP_TX_ACK, at(T0));
    DSP_CHECK_EQ_INT(0, r.attempted);
    DSP_CHECK_EQ_INT(0, tx.calls);
    DSP_CHECK_EQ_INT(0, kv.sets);
    /* max_sends bounds a session. */
    for (int i = 0; i < 5; i++) {
        DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_report_dosing_error(&n, &e, at(T0)));
    }
    tx_reset(DSP_TX_ACK);
    r = dsp_notify_deliver(&n, &TX, 3, at(T0));
    DSP_CHECK_EQ_INT(3, r.acked);
    DSP_CHECK_EQ_INT(2, dsp_notify_pending_count(&n));
    tx_reset(DSP_TX_ACK);
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_ARG, dsp_notify_deliver(&n, NULL, 3, at(T0)).status);
    dsp_notify_transport_t no_send = {NULL, NULL, NULL};
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_ARG, dsp_notify_deliver(&n, &no_send, 3, at(T0)).status);
}

static void test_uncertain_and_retry_same_id(void)
{
    fresh();
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_set_device_condition(&n, DSP_COND_STORAGE, true, 1, at(T0)));
    dsp_dosing_error_t e = {DSP_DOSEERR_PUMP_FAILED, false, 100};
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_report_dosing_error(&n, &e, at(T0)));
    char ids[3][DSP_NOTIFY_ID_BUF_LEN];
    /* Session 1: accepted by the server, but the response timed out. */
    tx_reset(DSP_TX_UNCERTAIN);
    dsp_notify_delivery_report_t r = dsp_notify_deliver(&n, &TX, 10, at(T0));
    DSP_CHECK_EQ_INT(1, r.attempted);
    DSP_CHECK(r.stopped_early);
    id_of(&tx.log[0], tx.epochs[0], ids[0]);
    DSP_CHECK_EQ_INT(2, dsp_notify_pending_count(&n));
    /* Session 2 after a restart: plain failure, same entry, same ID. */
    boot();
    tx_reset(DSP_TX_FAILED);
    dsp_notify_deliver(&n, &TX, 10, at(T0 + DAY));
    id_of(&tx.log[0], tx.epochs[0], ids[1]);
    /* Session 3 (different RNG value at boot: identity must not change): acknowledged. */
    boot_with(0x12345678u);
    tx_reset(DSP_TX_ACK);
    r = dsp_notify_deliver(&n, &TX, 10, at(T0 + 2 * DAY));
    DSP_CHECK_EQ_INT(2, r.acked);
    id_of(&tx.log[0], tx.epochs[0], ids[2]);
    DSP_CHECK_EQ_STR(ids[0], ids[1]);
    DSP_CHECK_EQ_STR(ids[0], ids[2]);
    DSP_CHECK_EQ_INT(3, tx.log[0].attempts); /* third started attempt (two without ACK) */
    DSP_CHECK_EQ_INT(DSP_TX_ACK, tx.fallback);
    DSP_CHECK_EQ_INT(RANDOM_EPOCH, tx.epochs[0]);
    DSP_CHECK_EQ_INT(0, dsp_notify_pending_count(&n));
}

static void test_durable_before_sent(void)
{
    fresh();
    kv.fail_sets = true;
    DSP_CHECK(!dsp_notify_storage_degraded(&n));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_STORAGE, obs_res(1000, false, at(T0)));
    DSP_CHECK(n.dirty);
    DSP_CHECK(dsp_notify_storage_degraded(&n)); /* observable until a write succeeds */
    DSP_CHECK_EQ_INT(1, dsp_notify_pending_count(&n)); /* kept in RAM */
    dsp_notify_delivery_report_t r = deliver_all(DSP_TX_ACK, at(T0));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_STORAGE, r.status);
    DSP_CHECK_EQ_INT(0, tx.calls); /* never sent before it is durable */
    /* Unchanged observations still retry the pending write. */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_STORAGE, obs_res(1000, false, at(T0)));
    fake_kv_clear_faults(&kv);
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(1000, false, at(T0)));
    DSP_CHECK(in_sync());
    DSP_CHECK(!dsp_notify_storage_degraded(&n));
    /* ...or delivery persists first, then sends. */
    fresh();
    kv.fail_sets = true;
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_STORAGE, obs_res(1000, false, at(T0)));
    fake_kv_clear_faults(&kv);
    r = deliver_all(DSP_TX_ACK, at(T0));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, r.status);
    DSP_CHECK_EQ_INT(1, r.acked); /* tx_send verified the entry was in flash */
    DSP_CHECK(in_sync());
}

static uint32_t sent_value_of(uint32_t seq)
{
    for (int i = 0; i < tx.calls && i < TX_LOG; i++) {
        if (tx.log[i].seq == seq) {
            return tx.log[i].value;
        }
    }
    return 0xFFFFFFFFu;
}

static void test_payload_freshness(void)
{
    /* LOW opens just under the 20 % threshold, the estimate falls to 8 %, then the regular
     * session delivers: the receiver gets the LATEST estimate (8 %) because nothing was
     * transmitted yet. The refresh itself costs no write. */
    fresh();
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(19999, false, at(T0)));
    uint32_t seq = find_cond(DSP_COND_RESERVOIR_LOW)->seq;
    kv.sets = 0;
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(8000, false, at(T0 + 3600)));
    DSP_CHECK_EQ_INT(0, kv.sets);
    DSP_CHECK_EQ_INT(19999, stored().cond[DSP_COND_RESERVOIR_LOW].value); /* RAM only */
    dsp_notify_delivery_report_t r = deliver_all(DSP_TX_FAILED, at(T0 + 7200));
    DSP_CHECK_EQ_INT(1, r.attempted);
    DSP_CHECK_EQ_INT(seq, tx.log[0].seq);
    DSP_CHECK_EQ_INT(8000, tx.log[0].value); /* tx_send verified it was in flash first */
    DSP_CHECK_EQ_INT(CAP, tx.log[0].value2);
    DSP_CHECK(in_sync());
    char id1[DSP_NOTIFY_ID_BUF_LEN];
    id_of(&tx.log[0], tx.epochs[0], id1);
    /* After the first attempt the payload is frozen: a retry with the same ID carries
     * identical content, even though the estimate keeps falling. */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(5000, false, at(T0 + DAY)));
    DSP_CHECK_EQ_INT(8000, find_cond(DSP_COND_RESERVOIR_LOW)->value);
    DSP_CHECK_EQ_INT(5000, n.s.cond[DSP_COND_RESERVOIR_LOW].value);
    boot();
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(4000, false, at(T0 + DAY)));
    r = deliver_all(DSP_TX_UNCERTAIN, at(T0 + DAY));
    DSP_CHECK_EQ_INT(8000, sent_value_of(seq));
    r = deliver_all(DSP_TX_ACK, at(T0 + 2 * DAY));
    DSP_CHECK_EQ_INT(1, r.acked);
    char id2[DSP_NOTIFY_ID_BUF_LEN];
    id_of(&tx.log[0], tx.epochs[0], id2);
    DSP_CHECK_EQ_STR(id1, id2);
    DSP_CHECK_EQ_INT(8000, tx.log[0].value);
    /* The next reminder is a new event (new ID) with the latest known estimate. */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_tick(&n, at(T0 + WEEK)));
    const dsp_note_t *rem = find_cond(DSP_COND_RESERVOIR_LOW);
    DSP_CHECK(rem != NULL && rem->seq != seq && rem->reminder_no == 1 && rem->value == 4000);

    /* Restart between the refresh and the delivery: the RAM-only refresh is lost (approved
     * trade-off); the caller's next observation restores the latest estimate. */
    fresh();
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(19999, false, at(T0)));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(8000, false, at(T0 + 1)));
    boot();
    DSP_CHECK_EQ_INT(19999, find_cond(DSP_COND_RESERVOIR_LOW)->value);
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(7000, false, at(T0 + 2)));
    deliver_all(DSP_TX_ACK, at(T0 + 3));
    DSP_CHECK_EQ_INT(7000, tx.log[0].value);

    /* The refresh cannot be persisted before the first transmission: nothing is sent, the
     * caller sees ERR_STORAGE, and flash still holds the old payload. */
    fresh();
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(19999, false, at(T0)));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(8000, false, at(T0 + 1)));
    kv.fail_sets = true;
    r = deliver_all(DSP_TX_ACK, at(T0 + 2));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_STORAGE, r.status);
    DSP_CHECK_EQ_INT(0, tx.calls);
    DSP_CHECK(dsp_notify_storage_degraded(&n));
    fake_kv_clear_faults(&kv);
    DSP_CHECK_EQ_INT(19999, stored().queue[0].value);
    r = deliver_all(DSP_TX_ACK, at(T0 + 3));
    DSP_CHECK_EQ_INT(1, r.acked);
    DSP_CHECK_EQ_INT(8000, tx.log[0].value);
    DSP_CHECK(!dsp_notify_storage_degraded(&n));

    /* The same rule for the other conditions (battery millivolts, device detail). */
    fresh();
    dsp_battery_obs_t b1 = {DSP_BATTERY_LOW, 3300};
    dsp_battery_obs_t b2 = {DSP_BATTERY_LOW, 3150};
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_battery(&n, b1, at(T0)));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_battery(&n, b2, at(T0 + 1)));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_set_device_condition(&n, DSP_COND_STORAGE, true, 1, at(T0)));
    deliver_all(DSP_TX_FAILED, at(T0 + 2)); /* STORAGE (critical) attempted first */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_set_device_condition(&n, DSP_COND_STORAGE, true, 9, at(T0)));
    DSP_CHECK_EQ_INT(1, find_cond(DSP_COND_STORAGE)->value);      /* attempted: frozen */
    DSP_CHECK_EQ_INT(3150, find_cond(DSP_COND_BATTERY_LOW)->value); /* unsent: latest */

    /* A refresh already written by another change is not written again: per session one
     * freeze write per entry sent for the first time, plus one final write. */
    fresh();
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(19999, false, at(T0)));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(8000, false, at(T0 + 1)));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_battery(&n, b1, at(T0 + 2))); /* writes */
    DSP_CHECK(in_sync());
    kv.sets = 0;
    r = deliver_all(DSP_TX_ACK, at(T0 + 3));
    DSP_CHECK_EQ_INT(2, r.acked);
    DSP_CHECK_EQ_INT(3, kv.sets);
    /* Retries of already frozen entries need no extra write: one per session. */
    dsp_dosing_error_t de = {DSP_DOSEERR_PUMP_FAILED, false, 10};
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_report_dosing_error(&n, &de, at(T0 + 4)));
    deliver_all(DSP_TX_FAILED, at(T0 + 5)); /* freezes it */
    kv.sets = 0;
    tx_reset(DSP_TX_FAILED);
    tx.script_len = 0;
    deliver_all(DSP_TX_FAILED, at(T0 + 6));
    DSP_CHECK_EQ_INT(1, kv.sets);
}

static void test_freeze_survives_power_loss(void)
{
    /* Required invariant: once a notification can possibly reach the receiver, its ID
     * identifies exactly the same payload, also after power loss, reboot and retries. */
    for (int mode = 0; mode < 4; mode++) {
        fake_kv_init(&kv, mode % 2 == 0); /* NVS-like and staged/commit storage */
        boot();
        DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(19999, false, at(T0)));    /* 1. LOW ~20 % */
        DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(8000, false, at(T0 + 1))); /* 2. update 8 % */
        /* 3./4. First attempt; the receiver accepts (ACK, or UNCERTAIN: timeout after
         * acceptance). 5. Power lost before the result is persisted. */
        tx_reset(mode < 2 ? DSP_TX_ACK : DSP_TX_UNCERTAIN);
        tx.fail_storage_after_send = true;
        dsp_notify_delivery_report_t r = dsp_notify_deliver(&n, &TX, 100, at(T0 + 2));
        DSP_CHECK_EQ_INT(1, tx.calls);
        DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_STORAGE, r.status);
        dsp_note_t first = tx.log[0];
        char id1[DSP_NOTIFY_ID_BUF_LEN];
        id_of(&first, tx.epochs[0], id1);
        DSP_CHECK_EQ_INT(8000, first.value);
        boot(); /* 6. restart: RAM lost, power back */
        /* Not lost: the result (ACK) was never durably processed. Frozen in flash. */
        DSP_CHECK_EQ_INT(1, dsp_notify_pending_count(&n));
        const dsp_note_t *e = find_cond(DSP_COND_RESERVOIR_LOW);
        DSP_CHECK(e != NULL && e->attempts >= 1 && e->value == 8000);
        DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(5000, false, at(T0 + 3))); /* 7. 5 % */
        DSP_CHECK_EQ_INT(8000, find_cond(DSP_COND_RESERVOIR_LOW)->value);
        DSP_CHECK_EQ_INT(5000, n.s.cond[DSP_COND_RESERVOIR_LOW].value);
        tx_reset(DSP_TX_ACK); /* 8. retry */
        r = dsp_notify_deliver(&n, &TX, 100, at(T0 + 4));
        DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, r.status);
        DSP_CHECK_EQ_INT(1, tx.calls);
        char id2[DSP_NOTIFY_ID_BUF_LEN];
        id_of(&tx.log[0], tx.epochs[0], id2);
        DSP_CHECK_EQ_STR(id1, id2);                     /* same event ID */
        DSP_CHECK_EQ_INT(first.value, tx.log[0].value); /* exactly the same payload */
        DSP_CHECK_EQ_INT(first.value2, tx.log[0].value2);
        DSP_CHECK_EQ_INT(first.flags, tx.log[0].flags);
        DSP_CHECK_EQ_INT(first.created_at, tx.log[0].created_at);
        DSP_CHECK_EQ_INT(first.type, tx.log[0].type);
        /* ACK durably processed only now: gone after another restart. */
        boot();
        DSP_CHECK_EQ_INT(0, dsp_notify_pending_count(&n));
    }

    /* Power lost (write failure) on the freeze write itself, before the first transmission:
     * zero transport calls; the event is never sent with a payload that is not durable. */
    for (int mode = 0; mode < 2; mode++) {
        fake_kv_init(&kv, mode == 0);
        boot();
        DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(19999, false, at(T0)));
        DSP_CHECK(in_sync()); /* clean state: only the freeze write is left to fail */
        if (mode == 0) {
            kv.fail_sets = true;
        } else {
            kv.power_cut_after_sets = 0;
            kv.fail_commits = true;
        }
        tx_reset(DSP_TX_ACK);
        dsp_notify_delivery_report_t r = dsp_notify_deliver(&n, &TX, 100, at(T0 + 1));
        DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_STORAGE, r.status);
        DSP_CHECK_EQ_INT(0, tx.calls);
        DSP_CHECK_EQ_INT(0, r.attempted);
        DSP_CHECK(dsp_notify_storage_degraded(&n));
        boot(); /* never sent, never frozen in flash: may still refresh */
        const dsp_note_t *e = find_cond(DSP_COND_RESERVOIR_LOW);
        DSP_CHECK(e != NULL && e->attempts == 0);
        DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(8000, false, at(T0 + 2)));
        tx_reset(DSP_TX_ACK);
        r = dsp_notify_deliver(&n, &TX, 100, at(T0 + 3));
        DSP_CHECK_EQ_INT(1, r.acked);
        DSP_CHECK_EQ_INT(8000, tx.log[0].value);
    }

    /* Second entry of a session: an earlier ACK in the same session is durable with its
     * freeze write, and a failed freeze write for the next entry sends nothing more. */
    fresh();
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(19999, true, at(T0)));
    tx_reset(DSP_TX_ACK);
    tx.fail_storage_after_send = true; /* first entry frozen + sent, then power is gone */
    dsp_notify_delivery_report_t r = dsp_notify_deliver(&n, &TX, 100, at(T0 + 1));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_STORAGE, r.status);
    DSP_CHECK_EQ_INT(1, tx.calls);
    uint32_t sent_seq = tx.log[0].seq;
    boot();
    DSP_CHECK_EQ_INT(2, dsp_notify_pending_count(&n)); /* nothing lost */
    tx_reset(DSP_TX_ACK);
    r = dsp_notify_deliver(&n, &TX, 100, at(T0 + 2));
    DSP_CHECK_EQ_INT(2, r.acked);
    DSP_CHECK_EQ_INT(sent_seq, tx.log[0].seq); /* the possibly received one first, same ID */
}

static void test_persistence_after_restart(void)
{
    fresh();
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(1000, true, at(T0)));
    dsp_battery_obs_t low = {DSP_BATTERY_LOW, 3300};
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_battery(&n, low, UNKNOWN_CLOCK));
    dsp_notify_state_t before = n.s;
    boot_with(0x0BADBEEFu);
    DSP_CHECK(state_equal(&before, &n.s));
    DSP_CHECK_EQ_INT(3, dsp_notify_pending_count(&n));
    DSP_CHECK_EQ_INT(RANDOM_EPOCH, n.s.epoch); /* assigned once, kept */
    /* Repeated observations after the restart do not duplicate. */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(1000, true, at(T0 + 10)));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_battery(&n, low, at(T0 + 10)));
    DSP_CHECK_EQ_INT(3, dsp_notify_pending_count(&n));
    DSP_CHECK_EQ_INT(0, kv.sets);
    /* Sequence numbers continue across restarts. */
    dsp_dosing_error_t e = {DSP_DOSEERR_STORAGE, false, 0};
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_report_dosing_error(&n, &e, at(T0 + 20)));
    DSP_CHECK_EQ_INT(4, n.s.next_seq - 1);
    /* Epoch fallback when the RNG gives 0. */
    fake_kv_init(&kv, true);
    boot_with(0);
    DSP_CHECK_EQ_INT(0, n.s.epoch);
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_report_dosing_error(&n, &e, at(T0)));
    DSP_CHECK_EQ_INT(1, n.s.epoch);
}

static void test_storage_unavailable_at_init(void)
{
    fresh();
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(1000, false, at(T0)));
    fake_kv_power_cycle(&kv);
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_open(&st, &kv.iface, TZ, true));
    kv.fail_gets = true;
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_STORAGE, dsp_notify_init(&n, &st, 1));
    DSP_CHECK(dsp_notify_storage_degraded(&n));
    DSP_CHECK(dsp_notify_storage_degraded(NULL));
    /* Never treated as an empty queue: the engine refuses to work. */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_ARG, obs_res(1000, false, at(T0)));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_ARG, dsp_notify_tick(&n, at(T0)));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_ARG, deliver_all(DSP_TX_ACK, at(T0)).status);
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_ARG, dsp_notify_set_estimate_uncertain(&n, true));
    DSP_CHECK_EQ_INT(0, tx.calls);
    /* A corrupt record is reported, not replaced. */
    fake_kv_clear_faults(&kv);
    const fake_kv_entry_t *ent = fake_kv_find(&kv, "notify");
    DSP_CHECK(ent != NULL);
    if (ent != NULL) {
        uint8_t raw[DSP_NOTIFY_RECORD_BYTES];
        memcpy(raw, ent->value, sizeof raw);
        raw[13] = 1; /* header padding byte */
        fake_kv_put_raw(&kv, "notify", raw, sizeof raw);
    }
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_STORAGE, dsp_notify_init(&n, &st, 1));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_ARG, dsp_notify_init(NULL, &st, 1));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_ARG, dsp_notify_init(&n, NULL, 1));
}

static void test_power_loss_during_queue_update(void)
{
    for (int mode = 0; mode < 2; mode++) {
        /* (a) The write is lost (power cut before it completes): the event is not durable, it
         * was never sent; after the restart the unchanged condition raises it again. */
        fake_kv_init(&kv, mode == 0);
        boot();
        DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(30000, false, at(T0)));
        kv.power_cut_after_sets = 0;
        kv.fail_sets = true;
        DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_STORAGE, obs_res(1000, false, at(T0)));
        boot();
        DSP_CHECK_EQ_INT(0, dsp_notify_pending_count(&n));
        DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(1000, false, at(T0 + 5)));
        DSP_CHECK_EQ_INT(1, dsp_notify_pending_count(&n));
        DSP_CHECK(in_sync());
        /* (b) Power lost right after the write: the event is there after the restart. */
        dsp_dosing_error_t e = {DSP_DOSEERR_INTERRUPTED, false, 50};
        kv.power_cut_after_sets = 1;
        dsp_notify_status_t s = dsp_notify_report_dosing_error(&n, &e, at(T0 + 6));
        DSP_CHECK(s == DSP_NOTIFY_OK || s == DSP_NOTIFY_ERR_STORAGE); /* commit may be cut */
        boot();
        DSP_CHECK(dsp_notify_state_is_valid(&n.s));
        DSP_CHECK(in_sync());
        /* (c) ACK received but the removal is lost: re-sent with the SAME ID (at least once). */
        tx_reset(DSP_TX_ACK);
        size_t before = dsp_notify_pending_count(&n);
        DSP_CHECK(before >= 1);
        tx.fail_storage_after_send = true;
        dsp_notify_delivery_report_t r = dsp_notify_deliver(&n, &TX, 100, at(T0 + 7));
        DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_STORAGE, r.status);
        uint32_t first_seq = tx.log[0].seq;
        boot();
        DSP_CHECK_EQ_INT(before, dsp_notify_pending_count(&n));
        tx_reset(DSP_TX_ACK);
        r = dsp_notify_deliver(&n, &TX, 100, at(T0 + 8));
        DSP_CHECK_EQ_INT(before, r.acked);
        DSP_CHECK_EQ_INT(first_seq, tx.log[0].seq);
        boot();
        DSP_CHECK_EQ_INT(0, dsp_notify_pending_count(&n));
    }
}

/* ---- Reminders and clock -------------------------------------------------------------- */

static void test_weekly_reminders(void)
{
    fresh();
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(1000, false, at(T0)));
    deliver_all(DSP_TX_ACK, at(T0));
    kv.sets = 0;
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_tick(&n, at(T0 + WEEK - 1)));
    DSP_CHECK_EQ_INT(0, dsp_notify_pending_count(&n));
    DSP_CHECK_EQ_INT(0, kv.sets);
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_tick(&n, at(T0 + WEEK)));
    const dsp_note_t *e = find_cond(DSP_COND_RESERVOIR_LOW);
    DSP_CHECK(e != NULL && e->reminder_no == 1 && (e->flags & DSP_NOTE_F_REMINDER) &&
              e->created_at == T0 + WEEK && e->seq == 2);
    DSP_CHECK_EQ_INT(1, kv.sets);
    DSP_CHECK(in_sync());
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_tick(&n, at(T0 + WEEK + DAY)));
    DSP_CHECK_EQ_INT(1, dsp_notify_pending_count(&n));
    /* Undelivered reminder superseded by the next one (one entry per condition). */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_tick(&n, at(T0 + 2 * WEEK)));
    e = find_cond(DSP_COND_RESERVOIR_LOW);
    DSP_CHECK(e != NULL && e->reminder_no == 2 && e->seq == 3);
    DSP_CHECK_EQ_INT(1, dsp_notify_pending_count(&n));
    /* The reminder carries the latest estimate (refreshed in RAM). */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(700, false, at(T0 + 2 * WEEK + 1)));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_tick(&n, at(T0 + 3 * WEEK)));
    DSP_CHECK_EQ_INT(700, find_cond(DSP_COND_RESERVOIR_LOW)->value);
    /* Resolution: no more reminders. */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(CAP, false, at(T0 + 3 * WEEK + 1)));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_tick(&n, at(T0 + 10 * WEEK)));
    DSP_CHECK_EQ_INT(0, dsp_notify_pending_count(&n));
    /* Reminders survive restarts (schedule persisted). */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_set_device_condition(&n, DSP_COND_STORAGE, true, 2, at(T0)));
    boot();
    DSP_CHECK_EQ_INT(T0 + WEEK, n.s.cond[DSP_COND_STORAGE].next_reminder_at);
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_tick(&n, at(T0 + WEEK)));
    e = find_cond(DSP_COND_STORAGE);
    DSP_CHECK(e != NULL && e->reminder_no == 1 && e->code == DSP_DEVERR_STORAGE && e->value == 2);
}

static void test_backward_correction(void)
{
    fresh();
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(1000, false, at(T0)));
    deliver_all(DSP_TX_ACK, at(T0));
    /* Clock corrected 30 days back: reminder pulled in to (corrected now + 7 d), none sent. */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_tick(&n, at(T0 - 30 * DAY)));
    DSP_CHECK_EQ_INT(0, dsp_notify_pending_count(&n));
    DSP_CHECK_EQ_INT(T0 - 23 * DAY, n.s.cond[DSP_COND_RESERVOIR_LOW].next_reminder_at);
    DSP_CHECK(in_sync());
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_tick(&n, at(T0 - 23 * DAY - 1)));
    DSP_CHECK_EQ_INT(0, dsp_notify_pending_count(&n));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_tick(&n, at(T0 - 23 * DAY)));
    DSP_CHECK_EQ_INT(1, dsp_notify_pending_count(&n));
    DSP_CHECK_EQ_INT(1, n.s.cond[DSP_COND_RESERVOIR_LOW].reminders);
    deliver_all(DSP_TX_ACK, at(T0));
    /* Back to the original timeline: exactly one more reminder, never a repeat burst. */
    for (int d = -22; d <= 7; d++) {
        DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_tick(&n, at(T0 + d * DAY)));
    }
    DSP_CHECK_EQ_INT(5, n.s.cond[DSP_COND_RESERVOIR_LOW].reminders); /* -16, -9, -2, +5 */
    DSP_CHECK_EQ_INT(1, dsp_notify_pending_count(&n));
}

static void test_forward_correction(void)
{
    fresh();
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(1000, false, at(T0)));
    dsp_battery_obs_t low = {DSP_BATTERY_LOW, 3300};
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_battery(&n, low, at(T0)));
    deliver_all(DSP_TX_ACK, at(T0));
    /* Clock jumps 60 days forward: ONE reminder per condition, not eight. */
    uint32_t seq_before = n.s.next_seq;
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_tick(&n, at(T0 + 60 * DAY)));
    DSP_CHECK_EQ_INT(2, dsp_notify_pending_count(&n));
    DSP_CHECK_EQ_INT(seq_before + 2, n.s.next_seq);
    DSP_CHECK_EQ_INT(1, n.s.cond[DSP_COND_RESERVOIR_LOW].reminders);
    DSP_CHECK_EQ_INT(T0 + 67 * DAY, n.s.cond[DSP_COND_RESERVOIR_LOW].next_reminder_at);
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_tick(&n, at(T0 + 60 * DAY + 1)));
    DSP_CHECK_EQ_INT(seq_before + 2, n.s.next_seq);
}

static void test_unknown_clock(void)
{
    fresh();
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(1000, false, UNKNOWN_CLOCK));
    const dsp_note_t *e = find_cond(DSP_COND_RESERVOIR_LOW);
    DSP_CHECK(e != NULL && e->created_at == 0 && !(e->flags & DSP_NOTE_F_TIME_VALID));
    DSP_CHECK_EQ_INT(0, n.s.cond[DSP_COND_RESERVOIR_LOW].opened_at);
    DSP_CHECK_EQ_INT(0, n.s.cond[DSP_COND_RESERVOIR_LOW].next_reminder_at);
    /* A "trusted" flag with an implausible time is not trusted either. */
    dsp_notify_clock_t implausible = {DSP_TIME_PLAUSIBLE_MIN - 1, true};
    dsp_dosing_error_t de = {DSP_DOSEERR_PUMP_FAILED, false, 5};
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_report_dosing_error(&n, &de, implausible));
    DSP_CHECK(count_type(DSP_NOTE_DOSING_ERROR) == 1);
    for (int i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY; i++) {
        DSP_CHECK_EQ_INT(0, n.s.queue[i].created_at);
    }
    /* No reminders, no writes while untrusted, events preserved and deliverable. */
    kv.sets = 0;
    for (int i = 0; i < 100; i++) {
        dsp_notify_clock_t c = {1000 + (dsp_utc_t)i * WEEK, false};
        DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_tick(&n, c));
    }
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_tick(&n, implausible));
    DSP_CHECK_EQ_INT(0, kv.sets);
    DSP_CHECK_EQ_INT(2, dsp_notify_pending_count(&n));
    dsp_notify_delivery_report_t r = deliver_all(DSP_TX_ACK, UNKNOWN_CLOCK);
    DSP_CHECK_EQ_INT(2, r.acked);
    /* First trusted time: schedule from now (no immediate reminder). */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_tick(&n, at(T0)));
    DSP_CHECK_EQ_INT(0, dsp_notify_pending_count(&n));
    DSP_CHECK_EQ_INT(T0, n.s.cond[DSP_COND_RESERVOIR_LOW].opened_at);
    DSP_CHECK_EQ_INT(T0 + WEEK, n.s.cond[DSP_COND_RESERVOIR_LOW].next_reminder_at);
    DSP_CHECK(in_sync());
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_tick(&n, at(T0 + WEEK)));
    DSP_CHECK_EQ_INT(1, dsp_notify_pending_count(&n));
}

/* ---- Capacity and priority ------------------------------------------------------------ */

static void test_capacity_exhaustion(void)
{
    fresh();
    dsp_dosing_error_t e = {DSP_DOSEERR_PUMP_FAILED, false, 10};
    for (int i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY; i++) {
        DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_report_dosing_error(&n, &e, at(T0 + i)));
    }
    DSP_CHECK_EQ_INT(DSP_NOTIFY_QUEUE_CAPACITY, dsp_notify_pending_count(&n));
    /* Full of CRITICAL entries: nothing can be evicted. */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_QUEUE_FULL, dsp_notify_report_dosing_error(&n, &e, at(T0 + 20)));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_QUEUE_FULL, dsp_notify_report_dosing_error(&n, &e, at(T0 + 21)));
    DSP_CHECK_EQ_INT(2, n.s.dropped);
    DSP_CHECK_EQ_INT(DSP_NOTIFY_QUEUE_FULL, obs_res(1000, false, at(T0 + 22)));
    DSP_CHECK(n.s.cond[DSP_COND_RESERVOIR_LOW].flags & DSP_COND_F_RAISE_PENDING);
    DSP_CHECK_EQ_INT(DSP_NOTIFY_QUEUE_FULL,
                     dsp_notify_set_device_condition(&n, DSP_COND_STORAGE, true, 3, at(T0 + 23)));
    DSP_CHECK(n.s.cond[DSP_COND_STORAGE].flags & DSP_COND_F_RAISE_PENDING);
    DSP_CHECK(in_sync()); /* latches and the drop counter are durable */
    /* Repeated observation of a latched condition: still no duplicate, still pending. */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(900, false, at(T0 + 24)));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_QUEUE_FULL, dsp_notify_tick(&n, at(T0 + 25)));
    /* Restart keeps everything. */
    boot();
    DSP_CHECK_EQ_INT(DSP_NOTIFY_QUEUE_CAPACITY, dsp_notify_pending_count(&n));
    DSP_CHECK_EQ_INT(2, n.s.dropped);
    /* One slot frees: LOW takes it, then STORAGE (critical) evicts LOW again (LOW stays
     * pending), and the overflow report must wait (it cannot evict a critical entry). */
    tx_reset(DSP_TX_ACK);
    dsp_notify_delivery_report_t r = dsp_notify_deliver(&n, &TX, 1, at(T0 + 30));
    DSP_CHECK_EQ_INT(1, r.acked);
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, r.status);
    DSP_CHECK(find_cond(DSP_COND_STORAGE) != NULL);
    DSP_CHECK(find_cond(DSP_COND_RESERVOIR_LOW) == NULL);
    DSP_CHECK(n.s.cond[DSP_COND_RESERVOIR_LOW].flags & DSP_COND_F_RAISE_PENDING);
    DSP_CHECK(!(n.s.cond[DSP_COND_STORAGE].flags & DSP_COND_F_RAISE_PENDING));
    DSP_CHECK_EQ_INT(2, n.s.dropped);
    DSP_CHECK(in_sync());
    /* No reminder is scheduled while the start event itself is still pending. */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_QUEUE_FULL, dsp_notify_tick(&n, at(T0 + 3 * WEEK)));
    DSP_CHECK_EQ_INT(0, n.s.cond[DSP_COND_RESERVOIR_LOW].reminders);
    /* Connectivity recovers: everything drains, the overflow is reported with the count. */
    r = deliver_all(DSP_TX_ACK, at(T0 + 3 * WEEK));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_QUEUE_CAPACITY, r.acked);
    DSP_CHECK_EQ_INT(0, n.s.dropped);
    DSP_CHECK_EQ_INT(2, dsp_notify_pending_count(&n)); /* LOW + overflow report raised */
    const dsp_note_t *low = find_cond(DSP_COND_RESERVOIR_LOW);
    /* The payload refresh (900) was RAM-only and lost by the restart: the deferred event
     * carries the last persisted payload (documented trade-off: no write per observation). */
    DSP_CHECK(low != NULL && low->reminder_no == 0 && low->value == 1000);
    const dsp_note_t *ov = NULL;
    for (int i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY; i++) {
        if (n.s.queue[i].code == DSP_DEVERR_QUEUE_OVERFLOW && n.s.queue[i].type == DSP_NOTE_DEVICE_ERROR) {
            ov = &n.s.queue[i];
        }
    }
    DSP_CHECK(ov != NULL && ov->value == 2 && ov->condition == DSP_COND_NONE &&
              ov->severity == DSP_SEV_CRITICAL);
    r = deliver_all(DSP_TX_ACK, at(T0 + 3 * WEEK));
    DSP_CHECK_EQ_INT(2, r.acked);
    DSP_CHECK_EQ_INT(0, dsp_notify_pending_count(&n));
    DSP_CHECK(in_sync());
}

static void test_deferred_reminder_keeps_number(void)
{
    fresh();
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(1000, false, at(T0)));
    deliver_all(DSP_TX_ACK, at(T0));
    dsp_dosing_error_t e = {DSP_DOSEERR_PUMP_FAILED, false, 10};
    for (int i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY; i++) {
        DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_report_dosing_error(&n, &e, at(T0)));
    }
    /* The weekly reminder finds no space (criticals only): latched, then raised later as
     * reminder 1, not as a new episode start. */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_QUEUE_FULL, dsp_notify_tick(&n, at(T0 + WEEK)));
    DSP_CHECK_EQ_INT(1, n.s.cond[DSP_COND_RESERVOIR_LOW].reminders);
    DSP_CHECK(n.s.cond[DSP_COND_RESERVOIR_LOW].flags & DSP_COND_F_RAISE_PENDING);
    tx_reset(DSP_TX_ACK);
    DSP_CHECK_EQ_INT(1, dsp_notify_deliver(&n, &TX, 1, at(T0 + WEEK)).acked);
    const dsp_note_t *low = find_cond(DSP_COND_RESERVOIR_LOW);
    DSP_CHECK(low != NULL && low->reminder_no == 1 && (low->flags & DSP_NOTE_F_REMINDER));
    DSP_CHECK(dsp_notify_state_is_valid(&n.s));
}

static void test_priority_and_no_critical_eviction(void)
{
    fresh();
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(1000, false, at(T0)));     /* seq 1, warning */
    dsp_battery_obs_t low = {DSP_BATTERY_LOW, 3300};
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_battery(&n, low, at(T0))); /* seq 2 */
    dsp_dosing_error_t e = {DSP_DOSEERR_START_FAILED, false, 10};
    for (int i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY - 2; i++) {
        DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_report_dosing_error(&n, &e, at(T0)));
    }
    DSP_CHECK_EQ_INT(DSP_NOTIFY_QUEUE_CAPACITY, dsp_notify_pending_count(&n));
    DSP_CHECK_EQ_INT(DSP_SEV_CRITICAL, dsp_notify_highest_pending_severity(&n));
    /* Delivery order: critical first, oldest first; warnings last. */
    tx_reset(DSP_TX_FAILED);
    tx.script_len = 3;
    tx.script[0] = DSP_TX_ACK;
    tx.script[1] = DSP_TX_ACK;
    tx.script[2] = DSP_TX_ACK;
    dsp_notify_delivery_report_t r = dsp_notify_deliver(&n, &TX, 100, at(T0));
    DSP_CHECK_EQ_INT(4, r.attempted); /* the 4th failed and ended the session */
    DSP_CHECK_EQ_INT(3, tx.log[0].seq);
    DSP_CHECK_EQ_INT(4, tx.log[1].seq);
    DSP_CHECK_EQ_INT(5, tx.log[2].seq);
    DSP_CHECK_EQ_INT(6, tx.log[3].seq);
    /* 13 entries left: three new criticals fill the free slots; the next evicts the oldest
     * WARNING (LOW, seq 1); then BATTERY; then nothing can be evicted. */
    for (int i = 0; i < 3; i++) {
        DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_report_dosing_error(&n, &e, at(T0)));
    }
    DSP_CHECK(find_cond(DSP_COND_RESERVOIR_LOW) != NULL);
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_report_dosing_error(&n, &e, at(T0)));
    DSP_CHECK(find_cond(DSP_COND_RESERVOIR_LOW) == NULL);
    DSP_CHECK(find_cond(DSP_COND_BATTERY_LOW) != NULL);
    DSP_CHECK(n.s.cond[DSP_COND_RESERVOIR_LOW].flags & DSP_COND_F_RAISE_PENDING);
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_report_dosing_error(&n, &e, at(T0)));
    DSP_CHECK(find_cond(DSP_COND_BATTERY_LOW) == NULL);
    DSP_CHECK(n.s.cond[DSP_COND_BATTERY_LOW].flags & DSP_COND_F_RAISE_PENDING);
    DSP_CHECK_EQ_INT(DSP_NOTIFY_QUEUE_FULL, dsp_notify_report_dosing_error(&n, &e, at(T0)));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_QUEUE_CAPACITY, count_type(DSP_NOTE_DOSING_ERROR));
    /* A WARNING never evicts anything when the queue holds only criticals. */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_battery(&n, low, at(T0)));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_QUEUE_CAPACITY, count_type(DSP_NOTE_DOSING_ERROR));
    DSP_CHECK(dsp_notify_state_is_valid(&n.s));
    DSP_CHECK(in_sync());
    /* A blocked reservoir (critical) evicts nothing either, it stays pending. */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_QUEUE_FULL, obs_res(1000, true, at(T0)));
    DSP_CHECK(n.s.cond[DSP_COND_RESERVOIR_BLOCKED].flags & DSP_COND_F_RAISE_PENDING);
    /* Drain: every deferred condition and the overflow report are eventually raised. */
    for (int round = 0; round < 4; round++) {
        deliver_all(DSP_TX_ACK, at(T0 + 1));
    }
    DSP_CHECK_EQ_INT(0, dsp_notify_pending_count(&n));
    for (int c = 0; c < DSP_COND_COUNT; c++) {
        DSP_CHECK(!(n.s.cond[c].flags & DSP_COND_F_RAISE_PENDING));
    }
    DSP_CHECK_EQ_INT(0, n.s.dropped);
}

static void test_long_outage_and_recovery(void)
{
    fresh();
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(1000, false, at(T0)));
    dsp_battery_obs_t low = {DSP_BATTERY_LOW, 3300};
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_battery(&n, low, at(T0)));
    int writes = 0;
    /* 120 days without connectivity, one wake per day with a (failing) regular session. */
    for (int d = 1; d <= 120; d++) {
        kv.sets = 0;
        dsp_notify_clock_t c = at(T0 + d * DAY);
        DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(1000, false, c));
        DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_tick(&n, c));
        DSP_CHECK(dsp_notify_delivery_requested(&n));
        dsp_notify_delivery_report_t r = deliver_all(DSP_TX_FAILED, c);
        DSP_CHECK_EQ_INT(1, r.attempted);
        writes += kv.sets;
        DSP_CHECK(dsp_notify_pending_count(&n) <= 2);
        if (d % 30 == 0) {
            boot(); /* restarts during the outage */
        }
    }
    DSP_CHECK_EQ_INT(2, dsp_notify_pending_count(&n)); /* bounded: one entry per condition */
    DSP_CHECK_EQ_INT(17, n.s.cond[DSP_COND_RESERVOIR_LOW].reminders);
    DSP_CHECK_EQ_INT(17, find_cond(DSP_COND_BATTERY_LOW)->reminder_no);
    /* About one write per session, plus per weekly reminder one tick write and at most two
     * freeze writes (new entries are frozen before their first transmission). */
    DSP_CHECK(writes <= 120 + 3 * 17 + 2);
    DSP_CHECK(dsp_notify_state_is_valid(&n.s));
    /* Connectivity returns: the queue drains in one session. */
    dsp_notify_delivery_report_t r = deliver_all(DSP_TX_ACK, at(T0 + 121 * DAY));
    DSP_CHECK_EQ_INT(2, r.acked);
    DSP_CHECK(!dsp_notify_delivery_requested(&n));
    DSP_CHECK(in_sync());
}

static void test_delivery_waits_for_regular_session(void)
{
    /* The engine only reports that something is pending; it has no wake-up request and a
     * critical event does not change that (approved: no extra Wi-Fi wake-ups). */
    fresh();
    tx_reset(DSP_TX_ACK);
    DSP_CHECK(!dsp_notify_delivery_requested(&n));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(1000, false, at(T0)));
    DSP_CHECK(dsp_notify_delivery_requested(&n));
    DSP_CHECK_EQ_INT(DSP_SEV_WARNING, dsp_notify_highest_pending_severity(&n));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_set_device_condition(&n, DSP_COND_STORAGE, true, 0, at(T0)));
    DSP_CHECK_EQ_INT(DSP_SEV_CRITICAL, dsp_notify_highest_pending_severity(&n));
    DSP_CHECK_EQ_INT(0, tx.calls);
    DSP_CHECK_EQ_INT(0, dsp_notify_pending_count(NULL));
    DSP_CHECK(!dsp_notify_delivery_requested(NULL));
}

/* ---- Reservoir accounting ------------------------------------------------------------- */

static void test_reservoir_consumption(void)
{
    fresh();
    dsp_dose_permit_t permit;
    dsp_config_t cfg;
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_load_config(&st, &cfg));
    DSP_CHECK(!dsp_reservoir_covers(&cfg, 1)); /* unconfigured: no dose passes (FR-022) */
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_reservoir_refill(&st, CAP, 22000, &cfg));
    uint32_t rev = cfg.revision;
    DSP_CHECK(dsp_reservoir_covers(&cfg, 22000));
    DSP_CHECK(!dsp_reservoir_covers(&cfg, 22001));
    DSP_CHECK(!dsp_reservoir_covers(&cfg, 0));
    DSP_CHECK(!dsp_reservoir_covers(NULL, 1));
    /* Manual and automatic doses consume alike (full requested volume). */
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_reservoir_consume(&st, 1500, DSP_DOSE_MANUAL, &cfg, &permit));
    DSP_CHECK_EQ_INT(20500, cfg.reservoir_remaining_ul);
    DSP_CHECK_EQ_INT(rev + 1, cfg.revision);
    /* The permit exists only after the durable subtraction and is redeemable once. */
    DSP_CHECK(permit.granted && permit.volume_ul == 1500 && permit.kind == DSP_DOSE_MANUAL &&
              permit.config_revision == cfg.revision);
    uint32_t vol = 0;
    DSP_CHECK(dsp_dose_permit_take(&permit, &vol));
    DSP_CHECK_EQ_INT(1500, vol);
    DSP_CHECK(!dsp_dose_permit_take(&permit, &vol));
    DSP_CHECK(!permit.granted);
    DSP_CHECK(!dsp_dose_permit_take(NULL, &vol));
    dsp_dose_permit_t forged = {false, DSP_DOSE_MANUAL, 100, 1}; /* not issued by consume */
    DSP_CHECK(!dsp_dose_permit_take(&forged, &vol));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_reservoir(&n, &cfg, false, at(T0)));
    DSP_CHECK_EQ_INT(0, dsp_notify_pending_count(&n));
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_reservoir_consume(&st, 600, DSP_DOSE_AUTOMATIC, &cfg, &permit));
    DSP_CHECK_EQ_INT(19900, cfg.reservoir_remaining_ul);
    DSP_CHECK_EQ_INT(rev + 2, cfg.revision);
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_reservoir(&n, &cfg, false, at(T0)));
    DSP_CHECK(find_cond(DSP_COND_RESERVOIR_LOW) != NULL);
    /* Durable: the estimate survives a restart. */
    boot();
    dsp_config_t back;
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_load_config(&st, &back));
    DSP_CHECK(dsp_config_equal(&cfg, &back));
    /* Saturation at 0, never wraps; FR-022 then blocks automatic doses. */
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_reservoir_consume(&st, 50000, DSP_DOSE_MANUAL, &cfg, &permit));
    DSP_CHECK_EQ_INT(0, cfg.reservoir_remaining_ul);
    DSP_CHECK(!dsp_reservoir_covers(&cfg, 1));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_reservoir(&n, &cfg, true, at(T0)));
    DSP_CHECK(find_cond(DSP_COND_RESERVOIR_BLOCKED) != NULL);
    /* Invalid requests change nothing. */
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_INVALID, dsp_reservoir_consume(&st, 0, DSP_DOSE_MANUAL, NULL, &permit));
    DSP_CHECK(!permit.granted);
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_INVALID, dsp_reservoir_consume(&st, 10, DSP_DOSE_MANUAL, NULL, NULL));
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_INVALID,
                     dsp_reservoir_consume(&st, 10, (dsp_dose_kind_t)5, NULL, &permit));
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_load_config(&st, &back));
    DSP_CHECK(dsp_config_equal(&cfg, &back));
    /* A failed write is reported (the caller must not run the pump). */
    kv.fail_sets = true;
    dsp_config_t untouched = cfg;
    permit.granted = true; /* stale value from the caller is cleared */
    DSP_CHECK(dsp_reservoir_consume(&st, 10, DSP_DOSE_AUTOMATIC, &untouched, &permit) != DSP_STORAGE_OK);
    DSP_CHECK(dsp_config_equal(&cfg, &untouched));
    DSP_CHECK(!permit.granted); /* no permit: the pump cannot be authorized */
    DSP_CHECK(!dsp_dose_permit_take(&permit, &vol));
    fake_kv_clear_faults(&kv);
}

/* ---- Identity and codec --------------------------------------------------------------- */

static void test_format_id(void)
{
    char buf[DSP_NOTIFY_ID_BUF_LEN];
    DSP_CHECK(dsp_notify_format_id(0x0123456789ABull, 0xA5A5A5A5u, 42, buf, sizeof buf));
    DSP_CHECK_EQ_STR("dsp-0123456789ab-a5a5a5a5-42", buf);
    DSP_CHECK(dsp_notify_format_id(0xFFFF0000000000FFull, 1, UINT32_MAX, buf, sizeof buf));
    DSP_CHECK_EQ_STR("dsp-0000000000ff-00000001-4294967295", buf);
    DSP_CHECK(!dsp_notify_format_id(1, 1, 0, buf, sizeof buf));
    DSP_CHECK_EQ_STR("", buf);
    DSP_CHECK(!dsp_notify_format_id(1, 0, 1, buf, sizeof buf));
    DSP_CHECK(!dsp_notify_format_id(1, 1, 1, buf, sizeof buf - 1));
    DSP_CHECK(!dsp_notify_format_id(1, 1, 1, NULL, sizeof buf));
}

static dsp_notify_state_t sample_state(void)
{
    dsp_notify_state_t s;
    memset(&s, 0, sizeof s);
    s.epoch = 0x11223344u;
    s.next_seq = 0x10;
    s.dropped = 0x0102;
    s.flags = DSP_NOTIFY_F_ESTIMATE_UNCERTAIN;
    dsp_condition_state_t *c = &s.cond[DSP_COND_RESERVOIR_LOW];
    c->flags = DSP_COND_F_ACTIVE;
    c->reminders = 2;
    c->opened_at = T0;
    c->next_reminder_at = T0 + WEEK;
    c->value = 1000;
    c->value2 = CAP;
    dsp_note_t *e = &s.queue[3];
    e->seq = 0x0F;
    e->type = DSP_NOTE_RESERVOIR_LOW;
    e->severity = DSP_SEV_WARNING;
    e->condition = DSP_COND_RESERVOIR_LOW;
    e->flags = DSP_NOTE_F_REMINDER | DSP_NOTE_F_TIME_VALID | DSP_NOTE_F_UNCERTAIN;
    e->reminder_no = 2;
    e->attempts = 0x0304;
    e->created_at = T0;
    e->value = 1000;
    e->value2 = CAP;
    dsp_note_t *d = &s.queue[0];
    d->seq = 0x0E;
    d->type = DSP_NOTE_DOSING_ERROR;
    d->severity = DSP_SEV_CRITICAL;
    d->condition = DSP_COND_NONE;
    d->flags = DSP_NOTE_F_MANUAL;
    d->code = DSP_DOSEERR_INTERRUPTED;
    d->value = 2000;
    return s;
}

static void test_notify_codec(void)
{
    DSP_CHECK_EQ_INT(794, DSP_NOTIFY_RECORD_BYTES); /* format 3 (M1.9a) */
    DSP_CHECK_EQ_INT(758, DSP_NOTIFY_RECORD_V2_BYTES);
    DSP_CHECK_EQ_INT(682, DSP_NOTIFY_RECORD_V1_BYTES);
    dsp_notify_state_t s = sample_state();
    DSP_CHECK(dsp_notify_state_is_valid(&s));
    uint8_t b[DSP_NOTIFY_RECORD_BYTES];
    DSP_CHECK_EQ_INT(DSP_NOTIFY_RECORD_BYTES, dsp_codec_encode_notify(&s, b, sizeof b));
    /* Golden bytes of the header, one condition and one entry (little-endian). */
    const uint8_t header[14] = {6, 3, 0x44, 0x33, 0x22, 0x11, 0x10, 0, 0, 0, 0x02, 0x01, 1, 0};
    DSP_CHECK(memcmp(b, header, sizeof header) == 0);
    const uint8_t cond0[26] = {1,    2,    0xF0, 0xEF, 0xFF, 0x6A, 0, 0, 0, 0, /* T0 */
                               0x70, 0x2A, 0x09, 0x6B, 0,    0,    0, 0,       /* T0 + 7 d */
                               0xE8, 0x03, 0,    0,    0xA0, 0x86, 1, 0};
    DSP_CHECK(memcmp(b + 14, cond0, sizeof cond0) == 0);
    const uint8_t *q3 = b + 14 + 6 * 26 + 3 * 32;
    const uint8_t entry3[32] = {0x0F, 0, 0, 0, 1, 2, 0, 7, 2, 0, 0x04, 0x03,
                                0xF0, 0xEF, 0xFF, 0x6A, 0, 0, 0, 0, 0, 0,
                                0xE8, 0x03, 0, 0, 0xA0, 0x86, 1, 0, 0, 0};
    DSP_CHECK(memcmp(q3, entry3, sizeof entry3) == 0);
    const uint8_t *q0 = b + 14 + 6 * 26;
    DSP_CHECK_EQ_INT(0xFF, q0[6]); /* one-shot: no condition */
    DSP_CHECK_EQ_INT(8, q0[7]);
    DSP_CHECK_EQ_INT(3, q0[20]);
    /* Round trip. */
    dsp_notify_state_t out;
    DSP_CHECK_EQ_INT(DSP_CODEC_OK, dsp_codec_decode_notify(b, sizeof b, &out));
    DSP_CHECK(state_equal(&s, &out));
    /* Pristine (provisioned) record: all zero after type/version. */
    dsp_notify_state_t z;
    memset(&z, 0, sizeof z);
    DSP_CHECK_EQ_INT(DSP_NOTIFY_RECORD_BYTES, dsp_codec_encode_notify(&z, b, sizeof b));
    for (size_t i = 2; i < sizeof b; i++) {
        DSP_CHECK(b[i] == 0);
    }
    /* Strict decoding. */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_RECORD_BYTES, dsp_codec_encode_notify(&s, b, sizeof b));
    DSP_CHECK_EQ_INT(DSP_CODEC_ERR_SIZE, dsp_codec_decode_notify(b, sizeof b - 1, &out));
    DSP_CHECK_EQ_INT(0, dsp_codec_encode_notify(&s, b, sizeof b - 1));
    struct {
        size_t off;
        uint8_t val;
        dsp_codec_status_t expect;
    } bad[] = {
        {0, 5, DSP_CODEC_ERR_TYPE},
        {1, 4, DSP_CODEC_ERR_VERSION},                         /* newer format */
        {12, 2, DSP_CODEC_ERR_FIELD},                          /* unknown header flag */
        {13, 1, DSP_CODEC_ERR_FIELD},                          /* header padding */
        {14 + 26 + 18, 1, DSP_CODEC_ERR_FIELD},                /* inactive cond with payload */
        {14 + 26 * 2 + 0, 4, DSP_CODEC_ERR_FIELD},             /* unknown cond flag */
        {14 + 6 * 26 + 3 * 32 + 5, 3, DSP_CODEC_ERR_FIELD},    /* severity mismatch */
        {14 + 6 * 26 + 3 * 32 + 4, 9, DSP_CODEC_ERR_FIELD},    /* unknown type */
        {14 + 6 * 26 + 3 * 32 + 6, 2, DSP_CODEC_ERR_FIELD},    /* wrong condition */
        {14 + 6 * 26 + 3 * 32 + 7, 0x10, DSP_CODEC_ERR_FIELD}, /* unknown entry flag */
        {14 + 6 * 26 + 3 * 32 + 7, 0x05, DSP_CODEC_ERR_FIELD}, /* TIME_VALID cleared */
        {14 + 6 * 26 + 3 * 32 + 7, 0x0F, DSP_CODEC_ERR_FIELD}, /* MANUAL on a warning */
        {14 + 6 * 26 + 3 * 32 + 8, 0, DSP_CODEC_ERR_FIELD},    /* reminder without number */
        {14 + 6 * 26 + 3 * 32 + 9, 1, DSP_CODEC_ERR_FIELD},    /* entry padding */
        {14 + 6 * 26 + 3 * 32 + 31, 1, DSP_CODEC_ERR_FIELD},   /* entry padding */
        {14 + 6 * 26 + 3 * 32 + 0, 0x10, DSP_CODEC_ERR_FIELD}, /* seq == next_seq */
        {14 + 6 * 26 + 3 * 32 + 0, 0x0E, DSP_CODEC_ERR_FIELD}, /* duplicate seq */
        {14 + 6 * 26 + 0 + 20, 9, DSP_CODEC_ERR_FIELD},        /* unknown dosing code */
        {14 + 6 * 26 + 1 * 32 + 4, 1, DSP_CODEC_ERR_FIELD},    /* empty slot not zero */
        {2 + 0, 0, DSP_CODEC_OK},                              /* epoch byte (still non-zero) */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        uint8_t m[DSP_NOTIFY_RECORD_BYTES];
        memcpy(m, b, sizeof m);
        m[bad[i].off] = bad[i].val;
        dsp_codec_status_t got = dsp_codec_decode_notify(m, sizeof m, &out);
        if (got != bad[i].expect) {
            fprintf(stderr, "notify codec case %zu (offset %zu)\n", i, bad[i].off);
        }
        DSP_CHECK_EQ_INT(bad[i].expect, got);
    }
    /* Entries without an epoch, or with seq but next_seq 0, are invalid. */
    dsp_notify_state_t v = sample_state();
    v.epoch = 0;
    DSP_CHECK(!dsp_notify_state_is_valid(&v));
    v = sample_state();
    v.next_seq = 0;
    DSP_CHECK(!dsp_notify_state_is_valid(&v));
    v = sample_state();
    v.queue[3].created_at = DSP_UTC_MAX + 1;
    DSP_CHECK(!dsp_notify_state_is_valid(&v));
    v = sample_state();
    v.cond[DSP_COND_BATTERY_LOW].value = 1; /* inactive with a payload */
    DSP_CHECK(!dsp_notify_state_is_valid(&v));
    v = sample_state();
    v.queue[0].flags = DSP_NOTE_F_REMINDER; /* reminder of a one-shot */
    v.queue[0].reminder_no = 1;
    DSP_CHECK(!dsp_notify_state_is_valid(&v));
    DSP_CHECK(!dsp_notify_state_is_valid(NULL));
    DSP_CHECK_EQ_INT(0, dsp_codec_encode_notify(&v, b, sizeof b));
}

/* Like in_sync(), but tolerates the RAM-only payload refresh of still active conditions and
 * of their never transmitted entries (written with the next real change or right before the
 * first transmission, see dsp_notify.c). */
static bool in_sync_but_payload(const dsp_notify_state_t *ram, const dsp_notify_state_t *flash)
{
    dsp_notify_state_t a = *ram;
    for (int c = 0; c < DSP_COND_COUNT; c++) {
        if ((a.cond[c].flags & DSP_COND_F_ACTIVE) && (flash->cond[c].flags & DSP_COND_F_ACTIVE)) {
            a.cond[c].value = flash->cond[c].value;
            a.cond[c].value2 = flash->cond[c].value2;
        }
    }
    for (int i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY; i++) { /* unsent entries follow it too */
        if (a.queue[i].seq != 0 && a.queue[i].seq == flash->queue[i].seq &&
            a.queue[i].attempts == 0 && flash->queue[i].attempts == 0) {
            a.queue[i].value = flash->queue[i].value;
            a.queue[i].value2 = flash->queue[i].value2;
        }
    }
    return state_equal(&a, flash);
}

/* ---- Randomized sequence -------------------------------------------------------------- */

static uint32_t rng_state = 0x2545F491u;
static uint32_t rnd(uint32_t mod)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return (rng_state >> 8) % mod;
}

/* Every ID ever transmitted maps to exactly one notification content. */
#define MAX_SEQ 4096
static struct {
    bool seen;
    uint8_t type, condition, reminder_no;
    uint16_t code;
    uint32_t value;
} sent_content[MAX_SEQ];
static bool acked_seq[MAX_SEQ];

static dsp_tx_result_t rnd_send(void *ctx, const dsp_note_t *note, uint32_t epoch,
                                dsp_tx_detail_t *detail)
{
    (void)detail;
    (void)ctx;
    DSP_CHECK_EQ_INT(RANDOM_EPOCH, epoch);
    DSP_CHECK(note->seq > 0 && note->seq < MAX_SEQ);
    if (note->seq == 0 || note->seq >= MAX_SEQ) {
        return DSP_TX_FAILED;
    }
    if (sent_content[note->seq].seen) {
        DSP_CHECK(sent_content[note->seq].type == note->type &&
                  sent_content[note->seq].condition == note->condition &&
                  sent_content[note->seq].reminder_no == note->reminder_no &&
                  sent_content[note->seq].code == note->code &&
                  sent_content[note->seq].value == note->value);
    } else {
        sent_content[note->seq].seen = true;
        sent_content[note->seq].type = note->type;
        sent_content[note->seq].condition = note->condition;
        sent_content[note->seq].reminder_no = note->reminder_no;
        sent_content[note->seq].code = note->code;
        sent_content[note->seq].value = note->value;
    }
    uint32_t r = rnd(10);
    if (r < 6) {
        acked_seq[note->seq] = true;
        return DSP_TX_ACK;
    }
    if (r < 8) {
        acked_seq[note->seq] = true; /* accepted by the receiver, response lost */
        return DSP_TX_UNCERTAIN;
    }
    return DSP_TX_FAILED;
}

static void test_randomized(void)
{
    memset(sent_content, 0, sizeof sent_content);
    memset(acked_seq, 0, sizeof acked_seq);
    fresh();
    const dsp_notify_transport_t rtx = {NULL, rnd_send, NULL};
    dsp_utc_t now = T0;
    uint32_t last_next_seq = 0;
    for (int step = 0; step < 4000; step++) {
        now += (dsp_utc_t)rnd(3 * DAY);
        if (rnd(50) == 0) {
            now -= (dsp_utc_t)rnd(20 * DAY); /* backward correction */
        }
        dsp_notify_clock_t c = {now, rnd(8) != 0};
        if (rnd(40) == 0) {
            kv.fail_sets = true; /* transient storage failure */
        }
        switch (rnd(9)) {
        case 0:
        case 1:
            (void)obs_res(rnd(CAP + 1), rnd(6) == 0, c);
            break;
        case 2: {
            dsp_battery_obs_t b = {(dsp_battery_status_t)rnd(3), 3000 + rnd(1000)};
            (void)dsp_notify_observe_battery(&n, b, c);
            break;
        }
        case 3: {
            dsp_dosing_error_t e = {(uint16_t)(1 + rnd(DSP_DOSEERR_MAX)), rnd(2) == 0, rnd(5000)};
            (void)dsp_notify_report_dosing_error(&n, &e, c);
            break;
        }
        case 4:
            (void)dsp_notify_set_device_condition(&n, (dsp_condition_t)(DSP_COND_STORAGE + rnd(3)),
                                                  rnd(2) == 0, rnd(100), c);
            break;
        case 5:
            (void)dsp_notify_set_estimate_uncertain(&n, rnd(2) == 0);
            break;
        case 6:
            (void)dsp_notify_tick(&n, c);
            break;
        case 7:
            (void)dsp_notify_deliver(&n, &rtx, 1 + rnd(20), c);
            break;
        default: {
            dsp_notify_state_t ram = n.s;
            bool was_dirty = n.dirty;
            boot();
            if (!was_dirty) {
                DSP_CHECK(in_sync_but_payload(&ram, &n.s));
            }
            break;
        }
        }
        fake_kv_clear_faults(&kv);
        DSP_CHECK(dsp_notify_state_is_valid(&n.s));
        DSP_CHECK(dsp_notify_pending_count(&n) <= DSP_NOTIFY_QUEUE_CAPACITY);
        if (!n.dirty) {
            dsp_notify_state_t flash = stored();
            DSP_CHECK(in_sync_but_payload(&n.s, &flash));
            DSP_CHECK(n.s.next_seq >= last_next_seq); /* durable seq never goes back */
            last_next_seq = n.s.next_seq;
        }
        /* At most one queued entry per condition. */
        for (int cnd = 0; cnd < DSP_COND_COUNT; cnd++) {
            int k = 0;
            for (int i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY; i++) {
                k += n.s.queue[i].seq != 0 && n.s.queue[i].condition == cnd;
            }
            DSP_CHECK(k <= 1);
            if (!(n.s.cond[cnd].flags & DSP_COND_F_ACTIVE)) {
                DSP_CHECK_EQ_INT(0, k); /* resolved: nothing stale queued */
            }
        }
    }
    DSP_CHECK(n.s.next_seq > 100);
}

/* ---- M1.7: permanent rejection, rejection log, diagnostics, budget ------------------ */

static void add_dosing_errors(int count)
{
    for (int i = 0; i < count; i++) {
        dsp_dosing_error_t e = {DSP_DOSEERR_PUMP_FAILED, false, (uint32_t)(1000 + i)};
        DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_report_dosing_error(&n, &e, at(T0)));
    }
}

static dsp_tx_detail_t det(uint16_t status, uint8_t error, uint8_t field, uint8_t diag)
{
    dsp_tx_detail_t d = {status, error, field, diag, 5};
    return d;
}

static void script(int i, dsp_tx_result_t res, dsp_tx_detail_t d)
{
    tx.script[i] = res;
    tx.detail[i] = d;
    if (tx.script_len < i + 1) {
        tx.script_len = i + 1;
    }
}

static bool queued(uint32_t seq)
{
    for (int i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY; i++) {
        if (n.s.queue[i].seq == seq) {
            return true;
        }
    }
    return false;
}

static bool logged(uint32_t seq)
{
    for (int i = 0; i < n.s.rejections.count; i++) {
        if (n.s.rejections.entry[i].seq == seq) {
            return true;
        }
    }
    return false;
}

static void test_rejected_logged_and_removed_atomically(void)
{
    fresh();
    add_dosing_errors(3); /* seq 1..3, CRITICAL */
    tx_reset(DSP_TX_ACK);
    script(0, DSP_TX_REJECTED, det(409, DSP_TXERR_CONFLICT, 0, 0));
    dsp_notify_delivery_report_t r = dsp_notify_deliver(&n, &TX, 100, at(T0));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, r.status);
    DSP_CHECK_EQ_INT(3, r.attempted);
    DSP_CHECK_EQ_INT(1, r.rejected);
    DSP_CHECK_EQ_INT(2, r.acked); /* the rejection never blocks later entries */
    DSP_CHECK(!r.stopped_early);
    DSP_CHECK_EQ_INT(0, dsp_notify_pending_count(&n));
    const dsp_rejection_log_t *log = dsp_notify_rejections(&n);
    DSP_CHECK_EQ_INT(1, log->count);
    DSP_CHECK_EQ_INT(1, log->unread);
    DSP_CHECK_EQ_INT(1, log->total);
    DSP_CHECK_EQ_INT(0, log->lost_unread);
    DSP_CHECK_EQ_INT(1, log->entry[0].seq);
    DSP_CHECK_EQ_INT(DSP_NOTE_DOSING_ERROR, log->entry[0].type);
    DSP_CHECK_EQ_INT(DSP_DOSEERR_PUMP_FAILED, log->entry[0].code);
    DSP_CHECK_EQ_INT(409, log->entry[0].http_status);
    DSP_CHECK_EQ_INT(DSP_TXERR_CONFLICT, log->entry[0].error);
    DSP_CHECK_EQ_INT(1, log->entry[0].attempts);
    DSP_CHECK(in_sync());
    boot(); /* survives a restart */
    DSP_CHECK_EQ_INT(1, dsp_notify_rejections(&n)->count);
    DSP_CHECK_EQ_INT(1, dsp_notify_rejections(&n)->entry[0].seq);
    DSP_CHECK_EQ_INT(0, dsp_notify_pending_count(&n));
}

static void test_rejection_write_failure_keeps_entry(void)
{
    /* 1. The rejection write fails outright (every write fails from the send on). */
    fresh();
    add_dosing_errors(2);
    tx_reset(DSP_TX_ACK);
    script(0, DSP_TX_REJECTED, det(409, DSP_TXERR_CONFLICT, 0, 0));
    tx.fail_storage_after_send = true;
    dsp_notify_delivery_report_t r = dsp_notify_deliver(&n, &TX, 100, at(T0));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_STORAGE, r.status);
    DSP_CHECK(r.stopped_early);
    DSP_CHECK_EQ_INT(0, r.rejected);
    DSP_CHECK_EQ_INT(1, tx.calls); /* nothing else was sent */
    DSP_CHECK(queued(1));          /* still queued in RAM, unchanged */
    DSP_CHECK_EQ_INT(0, n.s.rejections.count);
    DSP_CHECK(dsp_notify_storage_degraded(&n));
    boot();
    DSP_CHECK(queued(1)); /* and in flash: frozen (attempt 1), not lost, not logged */
    DSP_CHECK_EQ_INT(0, n.s.rejections.count);
    for (int i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY; i++) {
        if (n.s.queue[i].seq == 1) {
            DSP_CHECK_EQ_INT(1, n.s.queue[i].attempts);
        }
    }
    tx_reset(DSP_TX_ACK);
    script(0, DSP_TX_REJECTED, det(409, DSP_TXERR_CONFLICT, 0, 0));
    r = dsp_notify_deliver(&n, &TX, 100, at(T0));
    DSP_CHECK_EQ_INT(1, r.rejected);
    DSP_CHECK_EQ_INT(1, r.acked);
    DSP_CHECK(logged(1));
    DSP_CHECK_EQ_INT(2, n.s.rejections.entry[0].attempts);
    DSP_CHECK(in_sync());

    /* 2. The write lands but its confirmation fails (power cut right after the set, as
     *    with ESP-IDF NVS where a set is durable at once): applied exactly once. */
    fresh();
    add_dosing_errors(2);
    tx_reset(DSP_TX_ACK);
    script(0, DSP_TX_REJECTED, det(409, DSP_TXERR_CONFLICT, 0, 0));
    kv.power_cut_after_sets = 2; /* set 1 = freeze, set 2 = rejection, then no commit */
    r = dsp_notify_deliver(&n, &TX, 100, at(T0));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_STORAGE, r.status);
    DSP_CHECK(queued(1)); /* RAM conservatively keeps the entry */
    boot();
    DSP_CHECK(!queued(1)); /* flash holds the rejection: removed and logged together */
    DSP_CHECK(logged(1));
    DSP_CHECK_EQ_INT(1, dsp_notify_pending_count(&n));
}

/* A power cut at every possible write of a session with rejections and ACKs: no event is
 * ever lost (it is queued, logged, or was acknowledged by the receiver), and never both
 * queued and logged. */
static void test_rejection_power_cut_every_position(void)
{
    for (int cut = 1; cut <= 10; cut++) {
        fresh();
        add_dosing_errors(5); /* seq 1..5 */
        tx_reset(DSP_TX_ACK);
        script(0, DSP_TX_REJECTED, det(409, DSP_TXERR_CONFLICT, 0, 0));
        script(1, DSP_TX_ACK, det(0, 0, 0, 0));
        script(2, DSP_TX_REJECTED, det(400, DSP_TXERR_INVALID_FIELD, DSP_TXFIELD_VALUE, 0));
        kv.power_cut_after_sets = cut;
        (void)dsp_notify_deliver(&n, &TX, 100, at(T0));
        bool acked[6] = {false};
        for (int i = 0; i < tx.calls && i < TX_LOG; i++) {
            if ((i < tx.script_len ? tx.script[i] : tx.fallback) == DSP_TX_ACK) {
                acked[tx.log[i].seq] = true;
            }
        }
        boot();
        for (uint32_t seq = 1; seq <= 5; seq++) {
            bool q = queued(seq);
            bool l = logged(seq);
            if (!(q || l || acked[seq]) || (q && l)) {
                fprintf(stderr, "cut %d: seq %u queued=%d logged=%d acked=%d\n", cut, seq, q, l,
                        acked[seq]);
            }
            DSP_CHECK(q || l || acked[seq]);
            DSP_CHECK(!(q && l));
        }
        DSP_CHECK(dsp_notify_state_is_valid(&n.s));
    }
}

static void test_rejection_limit_per_session(void)
{
    fresh();
    add_dosing_errors(5);
    tx_reset(DSP_TX_ACK);
    for (int i = 0; i < 5; i++) {
        script(i, DSP_TX_REJECTED, det(409, DSP_TXERR_CONFLICT, 0, 0));
    }
    dsp_notify_delivery_report_t r = dsp_notify_deliver(&n, &TX, 100, at(T0));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_MAX_REJECTIONS_PER_SESSION, r.rejected);
    DSP_CHECK_EQ_INT(2, r.attempted);
    DSP_CHECK(r.stopped_early);
    DSP_CHECK_EQ_INT(3, dsp_notify_pending_count(&n));
    for (int i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY; i++) {
        if (n.s.queue[i].seq != 0) {
            DSP_CHECK_EQ_INT(0, n.s.queue[i].attempts); /* untouched */
        }
    }
    DSP_CHECK(in_sync());
}

static void test_rejection_log_overflow_and_ack(void)
{
    fresh();
    add_dosing_errors(7); /* seq 1..7 */
    for (int session = 0; session < 3; session++) {
        tx_reset(DSP_TX_ACK);
        script(0, DSP_TX_REJECTED, det(409, DSP_TXERR_CONFLICT, 0, 0));
        script(1, DSP_TX_REJECTED, det(400, DSP_TXERR_INVALID_FIELD, DSP_TXFIELD_CODE, 0));
        DSP_CHECK_EQ_INT(2, dsp_notify_deliver(&n, &TX, 100, at(T0)).rejected);
    }
    const dsp_rejection_log_t *log = dsp_notify_rejections(&n);
    DSP_CHECK_EQ_INT(4, log->count);
    DSP_CHECK_EQ_INT(6, log->total);
    DSP_CHECK_EQ_INT(4, log->unread);
    DSP_CHECK_EQ_INT(2, log->lost_unread); /* seq 1 and 2 dropped unread: counted */
    for (int i = 0; i < 4; i++) {
        DSP_CHECK_EQ_INT(3 + i, log->entry[i].seq); /* oldest first */
    }
    DSP_CHECK_EQ_INT(DSP_TXFIELD_CODE, log->entry[3].field);
    boot();
    log = dsp_notify_rejections(&n);
    DSP_CHECK_EQ_INT(2, log->lost_unread);
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_ack_rejections(&n));
    DSP_CHECK_EQ_INT(0, log->unread);
    DSP_CHECK_EQ_INT(0, log->lost_unread);
    DSP_CHECK_EQ_INT(4, log->count); /* entries stay as history */
    DSP_CHECK_EQ_INT(6, log->total); /* never cleared */
    DSP_CHECK(in_sync());
    kv.sets = 0;
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_ack_rejections(&n));
    DSP_CHECK_EQ_INT(0, kv.sets); /* nothing to acknowledge: no write */
    /* A read entry dropped by overflow is not a loss. */
    tx_reset(DSP_TX_ACK);
    script(0, DSP_TX_REJECTED, det(409, DSP_TXERR_CONFLICT, 0, 0));
    DSP_CHECK_EQ_INT(1, dsp_notify_deliver(&n, &TX, 1, at(T0)).rejected);
    DSP_CHECK_EQ_INT(4, log->count);
    DSP_CHECK_EQ_INT(7, log->total);
    DSP_CHECK_EQ_INT(1, log->unread);
    DSP_CHECK_EQ_INT(0, log->lost_unread);
    DSP_CHECK_EQ_INT(4, log->entry[0].seq);
    DSP_CHECK_EQ_INT(7, log->entry[3].seq);
    DSP_CHECK(in_sync());
}

static void test_tx_diag_flags_persist_and_clear(void)
{
    fresh();
    add_dosing_errors(1);
    tx_reset(DSP_TX_ACK);
    script(0, DSP_TX_FAILED, det(401, 0, 0, DSP_TXDIAG_AUTH));
    dsp_notify_delivery_report_t r = dsp_notify_deliver(&n, &TX, 100, at(T0));
    DSP_CHECK(r.stopped_early);
    DSP_CHECK_EQ_INT(1, dsp_notify_pending_count(&n));
    uint16_t status = 0;
    DSP_CHECK_EQ_INT(DSP_TXDIAG_AUTH, dsp_notify_tx_diag(&n, &status));
    DSP_CHECK_EQ_INT(401, status);
    DSP_CHECK(in_sync());
    boot(); /* survives restarts */
    DSP_CHECK_EQ_INT(DSP_TXDIAG_AUTH, dsp_notify_tx_diag(&n, &status));
    tx_reset(DSP_TX_ACK);
    script(0, DSP_TX_FAILED, det(404, DSP_TXERR_NOT_FOUND, 0, DSP_TXDIAG_ENDPOINT));
    (void)dsp_notify_deliver(&n, &TX, 100, at(T0));
    DSP_CHECK_EQ_INT(DSP_TXDIAG_AUTH | DSP_TXDIAG_ENDPOINT, dsp_notify_tx_diag(&n, &status));
    DSP_CHECK_EQ_INT(404, status);
    tx_reset(DSP_TX_ACK);
    script(0, DSP_TX_UNCERTAIN, det(201, DSP_TXERR_OTHER, 0, DSP_TXDIAG_PROTOCOL));
    (void)dsp_notify_deliver(&n, &TX, 100, at(T0));
    DSP_CHECK_EQ_INT(DSP_TXDIAG_ALL, dsp_notify_tx_diag(&n, &status));
    DSP_CHECK_EQ_INT(1, dsp_notify_pending_count(&n));
    /* Clearing condition 1: an acknowledged delivery. */
    tx_reset(DSP_TX_ACK);
    DSP_CHECK_EQ_INT(1, dsp_notify_deliver(&n, &TX, 100, at(T0)).acked);
    DSP_CHECK_EQ_INT(0, dsp_notify_tx_diag(&n, &status));
    DSP_CHECK_EQ_INT(0, status);
    DSP_CHECK(in_sync());
    /* Clearing condition 2: the explicit clear (future BLE). */
    add_dosing_errors(1);
    tx_reset(DSP_TX_ACK);
    script(0, DSP_TX_FAILED, det(415, DSP_TXERR_UNSUPPORTED_MEDIA, 0, DSP_TXDIAG_PROTOCOL));
    (void)dsp_notify_deliver(&n, &TX, 100, at(T0));
    DSP_CHECK_EQ_INT(DSP_TXDIAG_PROTOCOL, dsp_notify_tx_diag(&n, NULL));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_clear_tx_diag(&n));
    DSP_CHECK_EQ_INT(0, dsp_notify_tx_diag(&n, NULL));
    DSP_CHECK(in_sync());
    boot();
    DSP_CHECK_EQ_INT(0, dsp_notify_tx_diag(&n, NULL));
    DSP_CHECK_EQ_INT(1, dsp_notify_pending_count(&n)); /* the event itself is still queued */
    /* Temporary failures leave no diagnostic. */
    tx_reset(DSP_TX_ACK);
    script(0, DSP_TX_FAILED, det(503, 0, 0, 0));
    (void)dsp_notify_deliver(&n, &TX, 100, at(T0));
    DSP_CHECK_EQ_INT(0, dsp_notify_tx_diag(&n, NULL));
}

static void test_rejected_without_4xx_is_failed(void)
{
    fresh();
    add_dosing_errors(2);
    tx_reset(DSP_TX_ACK);
    script(0, DSP_TX_REJECTED, det(0, 0, 0, 0)); /* inconsistent transport result */
    dsp_notify_delivery_report_t r = dsp_notify_deliver(&n, &TX, 100, at(T0));
    DSP_CHECK_EQ_INT(0, r.rejected);
    DSP_CHECK(r.stopped_early);
    DSP_CHECK_EQ_INT(2, dsp_notify_pending_count(&n));
    DSP_CHECK_EQ_INT(0, n.s.rejections.count);
    DSP_CHECK_EQ_INT(DSP_TXDIAG_PROTOCOL, dsp_notify_tx_diag(&n, NULL));
    tx_reset(DSP_TX_ACK);
    script(0, DSP_TX_REJECTED, det(503, 0, 0, 0));
    DSP_CHECK_EQ_INT(0, dsp_notify_deliver(&n, &TX, 100, at(T0)).rejected);
    DSP_CHECK_EQ_INT(2, dsp_notify_pending_count(&n));
}

static void test_budget_leaves_entries_untouched(void)
{
    fresh();
    add_dosing_errors(5);
    tx_reset(DSP_TX_ACK);
    tx.budget = 2;
    dsp_notify_delivery_report_t r = dsp_notify_deliver(&n, &TX, 100, at(T0));
    DSP_CHECK_EQ_INT(2, r.attempted);
    DSP_CHECK_EQ_INT(2, r.acked);
    DSP_CHECK(r.budget_exhausted);
    DSP_CHECK(!r.stopped_early);
    DSP_CHECK_EQ_INT(3, dsp_notify_pending_count(&n));
    DSP_CHECK(in_sync());
    dsp_notify_state_t s = stored();
    for (int i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY; i++) {
        if (s.queue[i].seq != 0) {
            DSP_CHECK_EQ_INT(0, s.queue[i].attempts); /* not started: not frozen, not counted */
        }
    }
    /* No budget at all: nothing touched, nothing written. */
    tx_reset(DSP_TX_ACK);
    tx.budget = 0;
    kv.sets = 0;
    r = dsp_notify_deliver(&n, &TX, 100, at(T0));
    DSP_CHECK_EQ_INT(0, r.attempted);
    DSP_CHECK(r.budget_exhausted);
    DSP_CHECK_EQ_INT(0, kv.sets);
    DSP_CHECK_EQ_INT(0, tx.calls);
    /* The rest goes in the next session, oldest first. */
    tx_reset(DSP_TX_ACK);
    DSP_CHECK_EQ_INT(3, dsp_notify_deliver(&n, &TX, 100, at(T0)).acked);
    DSP_CHECK_EQ_INT(3, tx.log[0].seq);
}

static dsp_notify_state_t sample_state_v2(void)
{
    dsp_notify_state_t s = sample_state();
    s.tx_diag = DSP_TXDIAG_AUTH;
    s.tx_diag_status = 401;
    s.rejections.total = 5;
    s.rejections.unread = 1;
    s.rejections.count = 2;
    dsp_rejection_t *a = &s.rejections.entry[0];
    a->seq = 3;
    a->type = DSP_NOTE_DOSING_ERROR;
    a->flags = DSP_NOTE_F_MANUAL | DSP_NOTE_F_TIME_VALID;
    a->code = DSP_DOSEERR_PUMP_FAILED;
    a->http_status = 409;
    a->error = DSP_TXERR_CONFLICT;
    a->attempts = 1;
    dsp_rejection_t *b = &s.rejections.entry[1];
    b->seq = 0x0A0B0C0D;
    b->type = DSP_NOTE_RESERVOIR_LOW;
    b->flags = DSP_NOTE_F_TIME_VALID;
    b->http_status = 400;
    b->error = DSP_TXERR_INVALID_FIELD;
    b->field = DSP_TXFIELD_VALUE;
    b->attempts = 0x0102;
    return s;
}

static void test_notify_codec_v2(void)
{
    dsp_notify_state_t s = sample_state_v2();
    DSP_CHECK(dsp_notify_state_is_valid(&s));
    uint8_t b[DSP_NOTIFY_RECORD_BYTES];
    DSP_CHECK_EQ_INT(DSP_NOTIFY_RECORD_BYTES, dsp_codec_encode_notify(&s, b, sizeof b));
    /* Golden bytes of the format-2 tail. */
    const uint8_t diag[4] = {1, 0, 0x91, 0x01};
    DSP_CHECK(memcmp(b + 682, diag, sizeof diag) == 0);
    const uint8_t hdr[8] = {5, 0, 0, 0, 0, 0, 1, 2};
    DSP_CHECK(memcmp(b + 686, hdr, sizeof hdr) == 0);
    const uint8_t e1[16] = {0x0D, 0x0C, 0x0B, 0x0A, 1, 2, 0, 0, 0x90, 0x01, 5, 11, 0x02, 0x01, 0, 0};
    DSP_CHECK(memcmp(b + 694 + 16, e1, sizeof e1) == 0);
    for (size_t i = 694 + 32; i < sizeof b; i++) {
        DSP_CHECK(b[i] == 0); /* unused slots */
    }
    dsp_notify_state_t out;
    DSP_CHECK_EQ_INT(DSP_CODEC_OK, dsp_codec_decode_notify(b, sizeof b, &out));
    DSP_CHECK(state_equal(&s, &out));
    DSP_CHECK_EQ_INT(0x0A0B0C0D, out.rejections.entry[1].seq);
    struct {
        size_t off;
        uint8_t val;
    } bad[] = {
        {682, 0x08},       /* unknown diagnostic flag */
        {683, 1},          /* padding */
        {682, 0},          /* status kept without a flag */
        {685, 0x02},       /* status 0x0291 = 657 */
        {693, 5},          /* count > capacity */
        {692, 3},          /* unread > count */
        {686, 1},          /* total < count */
        {690, 1},          /* lost_unread although not full of unread */
        {694 + 4, 9},      /* unknown type */
        {694 + 6, 9},      /* invalid code for the type */
        {694 + 5, 0x10},   /* unknown entry flag */
        {694 + 8, 0x8F},   /* http status 399 */
        {694 + 10, 12},    /* error out of range */
        {694 + 11, 16},    /* field out of range */
        {694 + 12, 0},     /* attempts 0 (low byte; high byte 0) */
        {694 + 14, 1},     /* padding */
        {694 + 15, 1},     /* padding */
        {694 + 0, 0},      /* seq 0 in a used slot */
        {694 + 32 + 4, 1}, /* unused slot not zero */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        uint8_t m[DSP_NOTIFY_RECORD_BYTES];
        memcpy(m, b, sizeof m);
        m[bad[i].off] = bad[i].val;
        dsp_codec_status_t got = dsp_codec_decode_notify(m, sizeof m, &out);
        if (got != DSP_CODEC_ERR_FIELD) {
            fprintf(stderr, "notify v2 case %zu (offset %zu): %d\n", i, bad[i].off, (int)got);
        }
        DSP_CHECK_EQ_INT(DSP_CODEC_ERR_FIELD, got);
    }
    /* A status with a flag but no HTTP status (local cause) is valid. */
    s.tx_diag_status = 0;
    DSP_CHECK(dsp_notify_state_is_valid(&s));
    /* The encoder refuses inconsistent logs. */
    s = sample_state_v2();
    s.rejections.lost_unread = 1;
    DSP_CHECK_EQ_INT(0, dsp_codec_encode_notify(&s, b, sizeof b));
    s.rejections.count = 4;
    s.rejections.unread = 4;
    s.rejections.entry[2] = s.rejections.entry[0];
    s.rejections.entry[3] = s.rejections.entry[1];
    DSP_CHECK_EQ_INT(DSP_NOTIFY_RECORD_BYTES, dsp_codec_encode_notify(&s, b, sizeof b));
}

/* Format 1 (M1.5) records stay readable; the next write upgrades them to format 2. */
static void test_notify_format1_compat(void)
{
    dsp_notify_state_t s = sample_state();
    uint8_t b[DSP_NOTIFY_RECORD_BYTES];
    DSP_CHECK_EQ_INT(DSP_NOTIFY_RECORD_BYTES, dsp_codec_encode_notify(&s, b, sizeof b));
    b[1] = DSP_NOTIFY_FORMAT_V1;
    dsp_notify_state_t out;
    DSP_CHECK_EQ_INT(DSP_CODEC_OK, dsp_codec_decode_notify(b, DSP_NOTIFY_RECORD_V1_BYTES, &out));
    DSP_CHECK(state_equal(&s, &out));
    DSP_CHECK_EQ_INT(0, out.tx_diag);
    DSP_CHECK_EQ_INT(0, out.rejections.count);
    DSP_CHECK_EQ_INT(DSP_CODEC_ERR_SIZE, dsp_codec_decode_notify(b, DSP_NOTIFY_RECORD_BYTES, &out));
    b[1] = DSP_NOTIFY_FORMAT_V2;
    DSP_CHECK_EQ_INT(DSP_CODEC_ERR_SIZE, dsp_codec_decode_notify(b, DSP_NOTIFY_RECORD_V1_BYTES, &out));

    /* On "flash": an M1.5 device with a queued event and a format-1 record. */
    fresh();
    add_dosing_errors(2);
    uint8_t v1[DSP_NOTIFY_RECORD_BYTES];
    DSP_CHECK_EQ_INT(DSP_NOTIFY_RECORD_BYTES, dsp_codec_encode_notify(&n.s, v1, sizeof v1));
    v1[1] = DSP_NOTIFY_FORMAT_V1;
    fake_kv_put_raw(&kv, "notify", v1, DSP_NOTIFY_RECORD_V1_BYTES);
    boot();
    DSP_CHECK_EQ_INT(2, dsp_notify_pending_count(&n));
    DSP_CHECK_EQ_INT(0, n.s.rejections.count);
    tx_reset(DSP_TX_ACK);
    script(0, DSP_TX_REJECTED, det(409, DSP_TXERR_CONFLICT, 0, 0));
    DSP_CHECK_EQ_INT(1, dsp_notify_deliver(&n, &TX, 100, at(T0)).rejected);
    const fake_kv_entry_t *e = fake_kv_find(&kv, "notify");
    DSP_CHECK(e != NULL);
    if (e != NULL) {
        DSP_CHECK_EQ_INT(DSP_NOTIFY_RECORD_BYTES, e->len);
        DSP_CHECK_EQ_INT(DSP_NOTIFY_FORMAT_V3, e->value[1]);
    }
    DSP_CHECK(in_sync());
}

/* ---- M1.9a: dose-bound (idempotent) reports, notify format 3 ------------------------- */

static void test_report_once_idempotent(void)
{
    fresh();
    dsp_dosing_error_t e = {DSP_DOSEERR_INTERRUPTED, false, 1500};
    bool dup = true;
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_report_dosing_error_once(&n, &e, 0x1234, at(T0), &dup));
    DSP_CHECK(!dup);
    DSP_CHECK_EQ_INT(1, count_type(DSP_NOTE_DOSING_ERROR));
    DSP_CHECK(n.s.flags & DSP_NOTIFY_F_ESTIMATE_UNCERTAIN);
    DSP_CHECK(dsp_notify_report_key_known(&n, 0x1234));
    DSP_CHECK(in_sync());
    /* Same key again (e.g. recovery repeated after a power loss): nothing queued, no write. */
    kv.sets = 0;
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_report_dosing_error_once(&n, &e, 0x1234, at(T0), &dup));
    DSP_CHECK(dup);
    DSP_CHECK_EQ_INT(1, count_type(DSP_NOTE_DOSING_ERROR));
    DSP_CHECK_EQ_INT(0, kv.sets);
    /* Survives a restart. */
    boot();
    DSP_CHECK(dsp_notify_report_key_known(&n, 0x1234));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_report_dosing_error_once(&n, &e, 0x1234, at(T0), &dup));
    DSP_CHECK(dup);
    DSP_CHECK_EQ_INT(1, count_type(DSP_NOTE_DOSING_ERROR));
    /* Key 0 is not a key. */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_ARG, dsp_notify_report_dosing_error_once(&n, &e, 0, at(T0), &dup));
    /* A power cut before the write lands: neither the event nor the key are durable; the
     * repeat queues it (exactly once overall). */
    fresh();
    kv.power_cut_after_sets = 1;
    kv.durable_on_set = false;
    DSP_CHECK_EQ_INT(DSP_NOTIFY_ERR_STORAGE,
                     dsp_notify_report_dosing_error_once(&n, &e, 0x55, at(T0), &dup));
    boot();
    DSP_CHECK(!dsp_notify_report_key_known(&n, 0x55));
    DSP_CHECK_EQ_INT(0, count_type(DSP_NOTE_DOSING_ERROR));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_report_dosing_error_once(&n, &e, 0x55, at(T0), &dup));
    DSP_CHECK(!dup);
    DSP_CHECK_EQ_INT(1, count_type(DSP_NOTE_DOSING_ERROR));
    /* The ring keeps the 4 most recent keys (oldest dropped). */
    fresh();
    for (uint64_t k = 1; k <= 6; k++) {
        DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_report_dosing_error_once(&n, &e, k * 100, at(T0), &dup));
    }
    DSP_CHECK_EQ_INT(4, n.s.report_key_count);
    DSP_CHECK(!dsp_notify_report_key_known(&n, 100));
    DSP_CHECK(!dsp_notify_report_key_known(&n, 200));
    DSP_CHECK(dsp_notify_report_key_known(&n, 300));
    DSP_CHECK(dsp_notify_report_key_known(&n, 600));
    /* Queue full: the event is counted as dropped, and the key still recorded (a repeat
     * does not count it twice). */
    fresh();
    for (int k = 0; k < DSP_NOTIFY_QUEUE_CAPACITY; k++) {
        dsp_dosing_error_t f = {DSP_DOSEERR_PUMP_FAILED, false, 10};
        DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_report_dosing_error(&n, &f, at(T0)));
    }
    DSP_CHECK_EQ_INT(DSP_NOTIFY_QUEUE_FULL,
                     dsp_notify_report_dosing_error_once(&n, &e, 0x77, at(T0), &dup));
    DSP_CHECK_EQ_INT(1, n.s.dropped);
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_report_dosing_error_once(&n, &e, 0x77, at(T0), &dup));
    DSP_CHECK(dup);
    DSP_CHECK_EQ_INT(1, n.s.dropped);
}

static void test_notify_codec_v3_keys(void)
{
    dsp_notify_state_t s = sample_state_v2();
    s.report_key_count = 2;
    s.report_keys[0] = 0x0102030405060708ull;
    s.report_keys[1] = 0xFFFFFFFFFFFFFFFFull;
    DSP_CHECK(dsp_notify_state_is_valid(&s));
    uint8_t b[DSP_NOTIFY_RECORD_BYTES];
    DSP_CHECK_EQ_INT(DSP_NOTIFY_RECORD_BYTES, dsp_codec_encode_notify(&s, b, sizeof b));
    const uint8_t tail[12] = {2, 0, 0, 0, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    DSP_CHECK(memcmp(b + 758, tail, sizeof tail) == 0);
    dsp_notify_state_t out;
    DSP_CHECK_EQ_INT(DSP_CODEC_OK, dsp_codec_decode_notify(b, sizeof b, &out));
    DSP_CHECK(state_equal(&s, &out));
    DSP_CHECK(out.report_keys[1] == 0xFFFFFFFFFFFFFFFFull);
    size_t bad[] = {758 + 1, 758 + 4 + 16}; /* padding; an unused key slot not zero */
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        uint8_t m[DSP_NOTIFY_RECORD_BYTES];
        memcpy(m, b, sizeof m);
        m[bad[i]] = 1;
        DSP_CHECK_EQ_INT(DSP_CODEC_ERR_FIELD, dsp_codec_decode_notify(m, sizeof m, &out));
    }
    uint8_t m[DSP_NOTIFY_RECORD_BYTES];
    memcpy(m, b, sizeof m);
    m[758] = 5; /* count > capacity */
    DSP_CHECK_EQ_INT(DSP_CODEC_ERR_FIELD, dsp_codec_decode_notify(m, sizeof m, &out));
    memcpy(m, b, sizeof m);
    memcpy(m + 758 + 12, m + 758 + 4, 8); /* duplicate key */
    DSP_CHECK_EQ_INT(DSP_CODEC_ERR_FIELD, dsp_codec_decode_notify(m, sizeof m, &out));
    /* Format 2 (M1.7) still decodes, without keys. */
    s.report_key_count = 0;
    s.report_keys[0] = s.report_keys[1] = 0;
    DSP_CHECK_EQ_INT(DSP_NOTIFY_RECORD_BYTES, dsp_codec_encode_notify(&s, b, sizeof b));
    b[1] = DSP_NOTIFY_FORMAT_V2;
    DSP_CHECK_EQ_INT(DSP_CODEC_OK, dsp_codec_decode_notify(b, DSP_NOTIFY_RECORD_V2_BYTES, &out));
    DSP_CHECK(state_equal(&s, &out));
    DSP_CHECK_EQ_INT(DSP_CODEC_ERR_SIZE, dsp_codec_decode_notify(b, DSP_NOTIFY_RECORD_BYTES, &out));
}

/* notify_mask (user opt-out): a muted category queues nothing and sends nothing; its conditions
 * are still tracked; already queued entries wait (not pending) until it is enabled again. */
static void test_notify_mask_opt_out(void)
{
    fresh();
    dsp_battery_obs_t low = {DSP_BATTERY_LOW, 3850};
    dsp_battery_obs_t ok = {DSP_BATTERY_OK, 4100};
    dsp_notify_set_mask(&n, DSP_NOTIFY_LOW_RESERVOIR | DSP_NOTIFY_DEVICE_ERROR);
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_battery(&n, low, at(T0)));
    DSP_CHECK(n.s.cond[DSP_COND_BATTERY_LOW].flags & DSP_COND_F_ACTIVE); /* tracked */
    DSP_CHECK(!(n.s.cond[DSP_COND_BATTERY_LOW].flags & DSP_COND_F_RAISE_PENDING));
    DSP_CHECK_EQ_INT(0, count_type(DSP_NOTE_BATTERY_LOW));
    DSP_CHECK_EQ_INT(0, n.s.dropped);
    /* other categories are unaffected */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, obs_res(1000, false, at(T0)));
    DSP_CHECK_EQ_INT(1, count_type(DSP_NOTE_RESERVOIR_LOW));
    /* the 7-day reminder of a muted episode is not queued either */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_tick(&n, at(T0 + WEEK + 1)));
    DSP_CHECK_EQ_INT(0, count_type(DSP_NOTE_BATTERY_LOW));
    /* enabled again: no flood for the running episode; the next episode is reported */
    dsp_notify_set_mask(&n, DSP_NOTIFY_MASK_ALL);
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_battery(&n, low, at(T0 + WEEK + 2)));
    DSP_CHECK_EQ_INT(0, count_type(DSP_NOTE_BATTERY_LOW));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_battery(&n, ok, at(T0 + WEEK + 3)));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_battery(&n, low, at(T0 + WEEK + 4)));
    DSP_CHECK_EQ_INT(1, count_type(DSP_NOTE_BATTERY_LOW));

    /* a queued entry whose category gets muted: neither pending nor sent, kept */
    fresh();
    dsp_dosing_error_t e = {DSP_DOSEERR_PUMP_FAILED, false, 100};
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_report_dosing_error(&n, &e, at(T0)));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_battery(&n, low, at(T0)));
    dsp_notify_set_mask(&n, DSP_NOTIFY_LOW_BATTERY); /* reservoir and device errors muted */
    DSP_CHECK_EQ_INT(1, (int)dsp_notify_pending_count(&n));
    DSP_CHECK_EQ_INT(DSP_SEV_WARNING, dsp_notify_highest_pending_severity(&n)); /* the CRITICAL one is muted */
    dsp_notify_delivery_report_t r = deliver_all(DSP_TX_ACK, at(T0 + 1));
    DSP_CHECK_EQ_INT(1, r.acked);
    DSP_CHECK_EQ_INT(1, tx.calls);
    DSP_CHECK_EQ_INT(DSP_NOTE_BATTERY_LOW, tx.log[0].type);
    DSP_CHECK_EQ_INT(1, count_type(DSP_NOTE_DOSING_ERROR)); /* kept, untouched */
    DSP_CHECK_EQ_INT(0, (int)dsp_notify_pending_count(&n));
    /* a muted one-shot error is not queued and not counted as dropped */
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_report_dosing_error(&n, &e, at(T0 + 2)));
    DSP_CHECK_EQ_INT(1, count_type(DSP_NOTE_DOSING_ERROR));
    DSP_CHECK_EQ_INT(0, n.s.dropped);
    /* enabled again: the kept entry is delivered */
    dsp_notify_set_mask(&n, DSP_NOTIFY_MASK_ALL);
    DSP_CHECK_EQ_INT(1, (int)dsp_notify_pending_count(&n));
    r = deliver_all(DSP_TX_ACK, at(T0 + 3));
    DSP_CHECK_EQ_INT(1, r.acked);
    DSP_CHECK_EQ_INT(0, (int)dsp_notify_pending_count(&n));
    /* a reboot (fresh RAM) is unmuted until the application applies the configuration again */
    boot();
    DSP_CHECK_EQ_INT(0, n.muted);
}

int main(void)
{
    DSP_RUN(test_reservoir_threshold_crossing);
    DSP_RUN(test_low_threshold_variants);
    DSP_RUN(test_refill_ends_episode);
    DSP_RUN(test_hysteresis);
    DSP_RUN(test_repeated_observations_and_dedup);
    DSP_RUN(test_battery);
    DSP_RUN(test_notify_mask_opt_out);
    DSP_RUN(test_dosing_failure);
    DSP_RUN(test_interrupted_dosing_uncertain);
    DSP_RUN(test_storage_and_device_errors);
    DSP_RUN(test_wifi_failure_keeps_entry);
    DSP_RUN(test_ack_removes_entry);
    DSP_RUN(test_uncertain_and_retry_same_id);
    DSP_RUN(test_durable_before_sent);
    DSP_RUN(test_payload_freshness);
    DSP_RUN(test_freeze_survives_power_loss);
    DSP_RUN(test_persistence_after_restart);
    DSP_RUN(test_storage_unavailable_at_init);
    DSP_RUN(test_power_loss_during_queue_update);
    DSP_RUN(test_weekly_reminders);
    DSP_RUN(test_backward_correction);
    DSP_RUN(test_forward_correction);
    DSP_RUN(test_unknown_clock);
    DSP_RUN(test_capacity_exhaustion);
    DSP_RUN(test_deferred_reminder_keeps_number);
    DSP_RUN(test_priority_and_no_critical_eviction);
    DSP_RUN(test_long_outage_and_recovery);
    DSP_RUN(test_delivery_waits_for_regular_session);
    DSP_RUN(test_reservoir_consumption);
    DSP_RUN(test_format_id);
    DSP_RUN(test_notify_codec);
    DSP_RUN(test_randomized);
    DSP_RUN(test_rejected_logged_and_removed_atomically);
    DSP_RUN(test_rejection_write_failure_keeps_entry);
    DSP_RUN(test_rejection_power_cut_every_position);
    DSP_RUN(test_rejection_limit_per_session);
    DSP_RUN(test_rejection_log_overflow_and_ack);
    DSP_RUN(test_tx_diag_flags_persist_and_clear);
    DSP_RUN(test_rejected_without_4xx_is_failed);
    DSP_RUN(test_budget_leaves_entries_untouched);
    DSP_RUN(test_notify_codec_v2);
    DSP_RUN(test_notify_format1_compat);
    DSP_RUN(test_report_once_idempotent);
    DSP_RUN(test_notify_codec_v3_keys);
    return dsp_test_finish();
}
