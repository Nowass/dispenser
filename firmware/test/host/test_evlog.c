/* Phase C: persistent event history (dsp_evlog): codec, ordering, pagination, retention,
 * duplicates, interrupted and failed writes, corruption, restart. */
#include "dsp_evlog.h"

#include <stdio.h>
#include <string.h>

#include "dsp_test.h"
#include "fake_kv.h"

#define T0 ((dsp_utc_t)1795158000)

static fake_kv_t kv;
static dsp_evlog_t lg;

static dsp_event_t dose(const char *id, uint32_t vol, uint64_t key)
{
    dsp_event_t e;
    memset(&e, 0, sizeof e);
    e.type = DSP_EV_DOSE_SCHEDULED;
    e.result = DSP_EVR_OK;
    e.flags = DSP_EVF_CLOCK_TRUSTED | DSP_EVF_PUMP_BENCH_GPIO;
    e.at = T0;
    e.volume_ul = vol;
    e.before_ul = 100000;
    e.after_ul = 100000 - vol;
    e.capacity_ul = 100000;
    e.key = key;
    snprintf(e.schedule_id, sizeof e.schedule_id, "%s", id);
    return e;
}

static dsp_event_t refill(uint32_t before, uint32_t after)
{
    dsp_event_t e;
    memset(&e, 0, sizeof e);
    e.type = DSP_EV_RESERVOIR_REFILL;
    e.at = 0; /* no clock */
    e.before_ul = before;
    e.after_ul = after;
    e.capacity_ul = 200000;
    return e;
}

static void fresh(void)
{
    fake_kv_init(&kv, true);
    DSP_CHECK(dsp_evlog_open(&lg, &kv.iface));
}

static void reboot(void)
{
    fake_kv_power_cycle(&kv);
    fake_kv_clear_faults(&kv);
    memset(&lg, 0xA5, sizeof lg); /* fresh RAM */
    DSP_CHECK(dsp_evlog_open(&lg, &kv.iface));
}

static void test_keys_and_codec(void)
{
    char k[8];
    DSP_CHECK(dsp_evlog_key(0, k, sizeof k));
    DSP_CHECK_EQ_STR("ev00", k);
    DSP_CHECK(dsp_evlog_key(63, k, sizeof k));
    DSP_CHECK_EQ_STR("ev63", k);
    DSP_CHECK(!dsp_evlog_key(64, k, sizeof k));
    DSP_CHECK(!dsp_evlog_key(1, k, 4));

    /* Golden bytes (format 1). */
    dsp_event_t e = dose("a.b", 5000, 0x0102030405060708ull);
    e.seq = 0x11223344u;
    uint8_t b[DSP_EVLOG_RECORD_MAX];
    size_t n = dsp_event_encode(&e, b, sizeof b);
    DSP_CHECK_EQ_INT(44, n);
    const uint8_t golden[44] = {1, 1, 0, 9, 0x44, 0x33, 0x22, 0x11, 0xF0, 0xEF, 0xFF, 0x6A, 0, 0, 0, 0,
                                0x88, 0x13, 0, 0, 0xA0, 0x86, 1, 0, 0x18, 0x73, 1, 0, 0xA0, 0x86, 1, 0,
                                8, 7, 6, 5, 4, 3, 2, 1, 3, 'a', '.', 'b'};
    DSP_CHECK(memcmp(golden, b, sizeof golden) == 0);
    dsp_event_t d;
    DSP_CHECK(dsp_event_decode(b, n, &d));
    DSP_CHECK_EQ_INT(e.seq, d.seq);
    DSP_CHECK_EQ_INT(e.at, d.at);
    DSP_CHECK_EQ_STR("a.b", d.schedule_id);
    DSP_CHECK(d.key == e.key);

    /* Boundaries: maximum id (64), extreme volumes and instants. */
    dsp_event_t m = dose("", 0xFFFFFFFFu, UINT64_MAX);
    memset(m.schedule_id, 'x', DSP_SCHEDULE_ID_MAX_LEN);
    m.schedule_id[DSP_SCHEDULE_ID_MAX_LEN] = '\0';
    m.before_ul = m.after_ul = m.capacity_ul = 0xFFFFFFFFu;
    m.at = DSP_UTC_MAX;
    m.seq = UINT32_MAX;
    n = dsp_event_encode(&m, b, sizeof b);
    DSP_CHECK_EQ_INT(DSP_EVLOG_RECORD_MAX, n);
    DSP_CHECK(dsp_event_decode(b, n, &d));
    DSP_CHECK_EQ_INT(DSP_SCHEDULE_ID_MAX_LEN, (int)strlen(d.schedule_id));
    DSP_CHECK_EQ_INT(0, dsp_event_encode(&m, b, n - 1)); /* buffer too small */
    m.at = DSP_UTC_MIN;
    DSP_CHECK(dsp_event_encode(&m, b, sizeof b) == DSP_EVLOG_RECORD_MAX);

    /* Malformed records are rejected (never partially accepted). */
    n = dsp_event_encode(&e, b, sizeof b);
    uint8_t t[DSP_EVLOG_RECORD_MAX + 1];
    struct { size_t at; uint8_t val; } muts[] = {
        {0, 2},    /* unknown format */
        {1, 0},    /* type 0 */
        {1, 6},    /* type beyond the known ones */
        {2, 5},    /* result */
        {3, 0x10}, /* unknown flag */
        {4, 0},    /* seq low byte -> seq stays non-zero; checked below with all zero */
        {40, 4},   /* id length vs record length */
        {41, '/'}, /* id character */
        {41, 0},   /* embedded NUL */
    };
    for (size_t i = 0; i < sizeof muts / sizeof muts[0]; i++) {
        memcpy(t, b, n);
        t[muts[i].at] = muts[i].val;
        if (muts[i].at == 4) {
            memset(t + 4, 0, 4); /* seq 0 */
        }
        DSP_CHECK(!dsp_event_decode(t, n, &d));
    }
    memcpy(t, b, n);
    t[n] = 0;
    DSP_CHECK(!dsp_event_decode(t, n + 1, &d)); /* trailing byte */
    DSP_CHECK(!dsp_event_decode(b, n - 1, &d)); /* truncated */
    DSP_CHECK(!dsp_event_decode(b, 0, &d));
    DSP_CHECK(!dsp_event_decode(NULL, n, &d));

    /* Field rules. */
    dsp_event_t v = dose("s", 1, 0);
    v.schedule_id[0] = '\0';
    DSP_CHECK(!dsp_event_is_valid(&v)); /* scheduled dose needs its schedule */
    v = refill(1, 2);
    snprintf(v.schedule_id, sizeof v.schedule_id, "x");
    DSP_CHECK(!dsp_event_is_valid(&v)); /* reservoir events carry no schedule */
    v = refill(1, 2);
    v.result = DSP_EVR_PUMP_FAILED;
    DSP_CHECK(!dsp_event_is_valid(&v));
    v = refill(1, 2);
    v.volume_ul = 3;
    DSP_CHECK(!dsp_event_is_valid(&v));
    v = refill(1, 2);
    v.flags = DSP_EVF_RESERVOIR_UNKNOWN;
    DSP_CHECK(!dsp_event_is_valid(&v)); /* unknown means zeros */
    v.before_ul = v.after_ul = 0;
    DSP_CHECK(dsp_event_is_valid(&v));
    v.at = DSP_UTC_MAX + 1;
    DSP_CHECK(!dsp_event_is_valid(&v));
    DSP_CHECK(!dsp_event_is_valid(NULL));
}

static void test_append_order_and_pagination(void)
{
    fresh();
    DSP_CHECK_EQ_INT(0, dsp_evlog_newest(&lg));
    DSP_CHECK_EQ_INT(0, dsp_evlog_oldest(&lg));
    dsp_event_t out[8];
    DSP_CHECK_EQ_INT(0, dsp_evlog_read(&lg, 0, out, 8)); /* empty history */
    for (uint32_t i = 1; i <= 10; i++) {
        uint32_t seq = 0;
        dsp_event_t e = i % 3 == 0 ? refill(i, i + 1) : dose("sched-1", i * 100, 1000 + i);
        DSP_CHECK_EQ_INT(DSP_EVLOG_OK, dsp_evlog_append(&lg, &e, &seq));
        DSP_CHECK_EQ_INT(i, seq);
    }
    DSP_CHECK_EQ_INT(10, dsp_evlog_newest(&lg));
    DSP_CHECK_EQ_INT(1, dsp_evlog_oldest(&lg));
    /* Pages of 4 newest-first: 10..7, 6..3, 2..1, then nothing. */
    uint32_t before = 0, expect = 10;
    size_t total = 0;
    for (int page = 0; page < 4; page++) {
        size_t n = dsp_evlog_read(&lg, before, out, 4);
        for (size_t i = 0; i < n; i++) {
            DSP_CHECK_EQ_INT(expect, out[i].seq);
            expect--;
        }
        total += n;
        if (n == 0) {
            break;
        }
        before = out[n - 1].seq;
    }
    DSP_CHECK_EQ_INT(10, total);
    /* Appending during pagination: new events are above the cursor, the cursor's page is stable. */
    size_t n = dsp_evlog_read(&lg, 0, out, 4);
    DSP_CHECK_EQ_INT(4, n);
    dsp_event_t e = dose("late", 1, 0);
    DSP_CHECK_EQ_INT(DSP_EVLOG_OK, dsp_evlog_append(&lg, &e, NULL));
    n = dsp_evlog_read(&lg, out[3].seq, out, 4);
    DSP_CHECK_EQ_INT(6, out[0].seq);
    DSP_CHECK_EQ_INT(11, dsp_evlog_newest(&lg));
    /* A cursor above the head reads from the newest; max 0 reads nothing. */
    DSP_CHECK_EQ_INT(4, dsp_evlog_read(&lg, 1000, out, 4));
    DSP_CHECK_EQ_INT(11, out[0].seq);
    DSP_CHECK_EQ_INT(0, dsp_evlog_read(&lg, 0, out, 0));
    DSP_CHECK_EQ_INT(0, dsp_evlog_read(&lg, 1, out, 4)); /* nothing below seq 1 */
    /* Only "evNN" keys were written. */
    for (int i = 0; i < FAKE_KV_MAX_KEYS; i++) {
        if (kv.durable[i].used) {
            DSP_CHECK(strncmp(kv.durable[i].key, "ev", 2) == 0);
        }
    }
    DSP_CHECK_EQ_INT(0, lg.faults);
}

static void test_wraparound_and_restart(void)
{
    fresh();
    for (uint32_t i = 1; i <= 200; i++) {
        dsp_event_t e = dose("wrap", i, 0);
        DSP_CHECK_EQ_INT(DSP_EVLOG_OK, dsp_evlog_append(&lg, &e, NULL));
    }
    DSP_CHECK_EQ_INT(200, dsp_evlog_newest(&lg));
    DSP_CHECK_EQ_INT(200 - DSP_EVLOG_SLOTS + 1, dsp_evlog_oldest(&lg));
    DSP_CHECK_EQ_INT(DSP_EVLOG_SLOTS, fake_kv_count(&kv)); /* bounded: one key per slot */
    reboot();
    DSP_CHECK_EQ_INT(0, lg.faults);
    DSP_CHECK_EQ_INT(200, dsp_evlog_newest(&lg));
    DSP_CHECK_EQ_INT(137, dsp_evlog_oldest(&lg));
    dsp_event_t out[4];
    size_t total = 0;
    uint32_t before = 0, expect = 200;
    for (;;) {
        size_t n = dsp_evlog_read(&lg, before, out, 4);
        if (n == 0) {
            break;
        }
        for (size_t i = 0; i < n; i++) {
            DSP_CHECK_EQ_INT(expect, out[i].seq);
            DSP_CHECK_EQ_INT(expect, out[i].volume_ul); /* content follows its seq */
            expect--;
        }
        total += n;
        before = out[n - 1].seq;
    }
    DSP_CHECK_EQ_INT(DSP_EVLOG_SLOTS, total);
    /* The next seq continues after the restart. */
    dsp_event_t e = dose("wrap", 201, 0);
    uint32_t seq = 0;
    DSP_CHECK_EQ_INT(DSP_EVLOG_OK, dsp_evlog_append(&lg, &e, &seq));
    DSP_CHECK_EQ_INT(201, seq);
}

static void test_duplicates_are_suppressed(void)
{
    fresh();
    dsp_event_t a = dose("dup", 10, 0xABCDu);
    uint32_t s1 = 0, s2 = 0;
    DSP_CHECK_EQ_INT(DSP_EVLOG_OK, dsp_evlog_append(&lg, &a, &s1));
    int sets = kv.sets;
    DSP_CHECK_EQ_INT(DSP_EVLOG_DUPLICATE, dsp_evlog_append(&lg, &a, &s2));
    DSP_CHECK_EQ_INT(s1, s2);
    DSP_CHECK_EQ_INT(sets, kv.sets); /* nothing written */
    /* Also after a restart (repeated recovery). */
    reboot();
    DSP_CHECK_EQ_INT(DSP_EVLOG_DUPLICATE, dsp_evlog_append(&lg, &a, NULL));
    /* Key 0 never deduplicates. */
    dsp_event_t r = refill(1, 2);
    DSP_CHECK_EQ_INT(DSP_EVLOG_OK, dsp_evlog_append(&lg, &r, NULL));
    DSP_CHECK_EQ_INT(DSP_EVLOG_OK, dsp_evlog_append(&lg, &r, NULL));
    DSP_CHECK_EQ_INT(3, dsp_evlog_newest(&lg));
    /* Once its event has been overwritten by wraparound, a key can be logged again (the log
     * dedups within its retention only; callers log after durable transitions). */
    for (int i = 0; i < DSP_EVLOG_SLOTS; i++) {
        dsp_event_t f = refill(1, 2);
        (void)dsp_evlog_append(&lg, &f, NULL);
    }
    DSP_CHECK_EQ_INT(DSP_EVLOG_OK, dsp_evlog_append(&lg, &a, NULL));
}

static void test_failed_and_interrupted_writes(void)
{
    fresh();
    dsp_event_t e = dose("f", 1, 0);
    DSP_CHECK_EQ_INT(DSP_EVLOG_OK, dsp_evlog_append(&lg, &e, NULL)); /* seq 1 */
    /* set fails: the event is lost, its seq consumed, the log stays usable. */
    kv.fail_sets = true;
    DSP_CHECK_EQ_INT(DSP_EVLOG_ERR_WRITE, dsp_evlog_append(&lg, &e, NULL)); /* seq 2 lost */
    kv.fail_sets = false;
    kv.no_space = true;
    DSP_CHECK_EQ_INT(DSP_EVLOG_ERR_WRITE, dsp_evlog_append(&lg, &e, NULL)); /* seq 3: partition full */
    kv.no_space = false;
    kv.fail_commits = true;
    DSP_CHECK_EQ_INT(DSP_EVLOG_ERR_WRITE, dsp_evlog_append(&lg, &e, NULL)); /* seq 4: commit */
    kv.fail_commits = false;
    kv.corrupt_next_readback = true;
    DSP_CHECK_EQ_INT(DSP_EVLOG_ERR_WRITE, dsp_evlog_append(&lg, &e, NULL)); /* seq 5: read-back */
    DSP_CHECK_EQ_INT(4, lg.faults);
    uint32_t seq = 0;
    DSP_CHECK_EQ_INT(DSP_EVLOG_OK, dsp_evlog_append(&lg, &e, &seq));
    DSP_CHECK_EQ_INT(6, seq); /* never reused */
    dsp_event_t out[8];
    size_t n = dsp_evlog_read(&lg, 0, out, 8);
    /* seq 6 and 1 for sure; 4 and 5 landed durably (commit failure / bad read-back are
     * "unknown"), so after a restart they may reappear: they are real events with their own seq. */
    DSP_CHECK(n >= 2);
    DSP_CHECK_EQ_INT(6, out[0].seq);
    DSP_CHECK_EQ_INT(1, out[n - 1].seq);

    /* Power cut during an append (durable_on_set = false: the set is staged, lost at the cut). */
    fake_kv_init(&kv, false);
    DSP_CHECK(dsp_evlog_open(&lg, &kv.iface));
    DSP_CHECK_EQ_INT(DSP_EVLOG_OK, dsp_evlog_append(&lg, &e, NULL));
    kv.power_cut_after_sets = 1;
    DSP_CHECK_EQ_INT(DSP_EVLOG_ERR_WRITE, dsp_evlog_append(&lg, &e, NULL));
    reboot();
    DSP_CHECK_EQ_INT(1, dsp_evlog_newest(&lg)); /* old content intact, no partial event */
    DSP_CHECK_EQ_INT(0, lg.faults);
    DSP_CHECK_EQ_INT(DSP_EVLOG_OK, dsp_evlog_append(&lg, &e, &seq));
    DSP_CHECK_EQ_INT(2, seq);
}

static void test_corruption_is_contained(void)
{
    fresh();
    for (int i = 0; i < 10; i++) {
        dsp_event_t e = dose("c", (uint32_t)i + 1, 0);
        DSP_CHECK_EQ_INT(DSP_EVLOG_OK, dsp_evlog_append(&lg, &e, NULL));
    }
    /* Garbage in slot 5, an event with a foreign seq in slot 7, an unreadable slot 9. */
    const uint8_t junk[3] = {1, 2, 3};
    fake_kv_put_raw(&kv, "ev05", junk, sizeof junk);
    dsp_event_t foreign = dose("c", 99, 0);
    foreign.seq = 8; /* belongs to slot 8 */
    uint8_t b[DSP_EVLOG_RECORD_MAX];
    size_t n = dsp_event_encode(&foreign, b, sizeof b);
    fake_kv_put_raw(&kv, "ev07", b, n);
    reboot();
    snprintf(kv.fail_get_key, sizeof kv.fail_get_key, "ev09");
    DSP_CHECK(dsp_evlog_open(&lg, &kv.iface));
    DSP_CHECK_EQ_INT(3, lg.faults);
    dsp_event_t out[16];
    n = dsp_evlog_read(&lg, 0, out, 16);
    DSP_CHECK_EQ_INT(7, n); /* 10 - 3 faulty slots; the rest in order */
    DSP_CHECK_EQ_INT(10, out[0].seq);
    DSP_CHECK_EQ_INT(8, out[1].seq);
    /* After a fault the next seq skips a full ring: a seq an unreadable slot may hold is never
     * reused. */
    dsp_event_t e = dose("c", 11, 0);
    uint32_t seq = 0;
    DSP_CHECK_EQ_INT(DSP_EVLOG_OK, dsp_evlog_append(&lg, &e, &seq));
    DSP_CHECK_EQ_INT(10 + DSP_EVLOG_SLOTS + 1, seq);
    kv.fail_get_key[0] = '\0';
    /* Order is by seq across the gap, also after the next restart (no cascade of skips while
     * the faulty slots stay as they are; each still counts as a fault). */
    reboot();
    n = dsp_evlog_read(&lg, 0, out, 16);
    DSP_CHECK_EQ_INT(seq, out[0].seq);
    DSP_CHECK_EQ_INT(10, out[1].seq);
    /* A transient read failure while paging skips that event (counted), never misorders. */
    snprintf(kv.fail_get_key, sizeof kv.fail_get_key, "ev10");
    uint16_t f0 = lg.faults;
    n = dsp_evlog_read(&lg, 0, out, 3);
    DSP_CHECK_EQ_INT(3, n);
    DSP_CHECK_EQ_INT(9, out[1].seq);
    DSP_CHECK_EQ_INT(8, out[2].seq);
    DSP_CHECK_EQ_INT(f0 + 1, lg.faults);
    kv.fail_get_key[0] = '\0';
}

static void test_arguments(void)
{
    fresh();
    dsp_event_t e = dose("a", 1, 0);
    DSP_CHECK(!dsp_evlog_open(NULL, &kv.iface));
    DSP_CHECK(!dsp_evlog_open(&lg, NULL));
    DSP_CHECK(dsp_evlog_open(&lg, &kv.iface));
    DSP_CHECK_EQ_INT(DSP_EVLOG_ERR_ARG, dsp_evlog_append(NULL, &e, NULL));
    DSP_CHECK_EQ_INT(DSP_EVLOG_ERR_ARG, dsp_evlog_append(&lg, NULL, NULL));
    dsp_event_t bad = e;
    bad.type = 9;
    DSP_CHECK_EQ_INT(DSP_EVLOG_ERR_ARG, dsp_evlog_append(&lg, &bad, NULL));
    dsp_evlog_t closed;
    memset(&closed, 0, sizeof closed);
    DSP_CHECK_EQ_INT(DSP_EVLOG_ERR_ARG, dsp_evlog_append(&closed, &e, NULL));
    dsp_event_t out[1];
    DSP_CHECK_EQ_INT(0, dsp_evlog_read(&closed, 0, out, 1));
    DSP_CHECK_EQ_INT(0, dsp_evlog_read(&lg, 0, NULL, 1));
    /* Seq exhaustion: the log stops instead of wrapping the seq. */
    lg.head = UINT32_MAX - 1;
    uint32_t seq = 0;
    DSP_CHECK_EQ_INT(DSP_EVLOG_OK, dsp_evlog_append(&lg, &e, &seq));
    DSP_CHECK_EQ_INT(UINT32_MAX, seq);
    DSP_CHECK_EQ_INT(DSP_EVLOG_ERR_ARG, dsp_evlog_append(&lg, &e, NULL));
}

int main(void)
{
    DSP_RUN(test_keys_and_codec);
    DSP_RUN(test_append_order_and_pagination);
    DSP_RUN(test_wraparound_and_restart);
    DSP_RUN(test_duplicates_are_suppressed);
    DSP_RUN(test_failed_and_interrupted_writes);
    DSP_RUN(test_corruption_is_contained);
    DSP_RUN(test_arguments);
    return dsp_test_finish();
}
