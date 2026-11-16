/*
 * Platform adapter logic tests (M1.9c): the pure part of the ESP-IDF adapters
 * (dsp_platform_logic.c, compiled unchanged into the firmware) plus the RTC-retained block
 * end to end (domain encode -> RTC buffer -> domain decode), and a two-thread check of the
 * command-queue boundary (a producer thread posting while the application task runs a
 * service session, under a pthread mutex standing in for the platform lock).
 * The ESP-IDF calls themselves (esp_reset_reason, esp_sleep_*, RTC_NOINIT, portMUX) are
 * only cross-compiled: hardware-unverified.
 */
#define _POSIX_C_SOURCE 200809L /* clock_gettime, nanosleep (host test only) */
#include "dsp_app.h"
#include "dsp_platform_logic.h"
#include "dsp_power.h"
#include "dsp_test.h"
#include "fake_kv.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---- Mapping rules ----------------------------------------------------------------------- */

static void test_wake_cause_mapping(void)
{
    DSP_CHECK_EQ_INT(DSP_CAUSE_TIMER, dsp_plat_wake_cause(DSP_PLAT_RST_DEEP_SLEEP, DSP_PLAT_WAKE_TIMER));
    DSP_CHECK_EQ_INT(DSP_CAUSE_BUTTON, dsp_plat_wake_cause(DSP_PLAT_RST_DEEP_SLEEP, DSP_PLAT_WAKE_GPIO));
    DSP_CHECK_EQ_INT(DSP_CAUSE_BUTTON, dsp_plat_wake_cause(DSP_PLAT_RST_DEEP_SLEEP,
                                                           DSP_PLAT_WAKE_GPIO | DSP_PLAT_WAKE_TIMER));
    DSP_CHECK_EQ_INT(DSP_CAUSE_TIMER, dsp_plat_wake_cause(DSP_PLAT_RST_DEEP_SLEEP, 0));
    DSP_CHECK_EQ_INT(DSP_CAUSE_SOFTWARE, dsp_plat_wake_cause(DSP_PLAT_RST_SOFTWARE, 0));
    DSP_CHECK_EQ_INT(DSP_CAUSE_PANIC, dsp_plat_wake_cause(DSP_PLAT_RST_PANIC, 0));
    DSP_CHECK_EQ_INT(DSP_CAUSE_WATCHDOG, dsp_plat_wake_cause(DSP_PLAT_RST_WATCHDOG, 0));
    DSP_CHECK_EQ_INT(DSP_CAUSE_BROWNOUT, dsp_plat_wake_cause(DSP_PLAT_RST_BROWNOUT, 0));
    DSP_CHECK_EQ_INT(DSP_CAUSE_POWER_ON, dsp_plat_wake_cause(DSP_PLAT_RST_POWER_ON, DSP_PLAT_WAKE_GPIO));
    DSP_CHECK_EQ_INT(DSP_CAUSE_POWER_ON, dsp_plat_wake_cause(DSP_PLAT_RST_EXTERNAL, 0));
    DSP_CHECK_EQ_INT(DSP_CAUSE_POWER_ON, dsp_plat_wake_cause(DSP_PLAT_RST_UNKNOWN, 0));
    /* The retained block and the clock continuity always agree on what survives a reset
     * (cold boot and brownout invalidate both; deep sleep and soft resets keep both). */
    for (int r = DSP_PLAT_RST_UNKNOWN; r <= DSP_PLAT_RST_BROWNOUT; r++) {
        for (uint32_t w = 0; w < 4; w++) {
            dsp_wake_cause_t c = dsp_plat_wake_cause((dsp_plat_reset_t)r, w);
            DSP_CHECK_EQ_INT(dsp_plat_reset_keeps_rtc((dsp_plat_reset_t)r), dsp_power_cause_keeps_retained(c));
        }
    }
    DSP_CHECK(!dsp_plat_reset_keeps_rtc(DSP_PLAT_RST_POWER_ON));
    DSP_CHECK(!dsp_plat_reset_keeps_rtc(DSP_PLAT_RST_BROWNOUT));
    DSP_CHECK(dsp_plat_reset_keeps_rtc(DSP_PLAT_RST_DEEP_SLEEP));
}

static void test_sleep_and_button(void)
{
    DSP_CHECK_EQ_INT(1000000, dsp_plat_sleep_us(0));
    DSP_CHECK_EQ_INT(1000000, dsp_plat_sleep_us(-5));
    DSP_CHECK_EQ_INT(21600000000LL, dsp_plat_sleep_us(6 * 3600));
    DSP_CHECK_EQ_INT((int64_t)DSP_POWER_MAX_SLEEP_MAX_S * 1000000, dsp_plat_sleep_us(INT64_MAX));
    DSP_CHECK_EQ_INT(DSP_PLAT_BUTTON_DISABLED, dsp_plat_button_check(-1));
    DSP_CHECK_EQ_INT(DSP_PLAT_BUTTON_OK, dsp_plat_button_check(0));
    DSP_CHECK_EQ_INT(DSP_PLAT_BUTTON_OK, dsp_plat_button_check(3));
    DSP_CHECK_EQ_INT(DSP_PLAT_BUTTON_OK, dsp_plat_button_check(5));
    DSP_CHECK_EQ_INT(DSP_PLAT_BUTTON_STRAPPING, dsp_plat_button_check(2));
    DSP_CHECK_EQ_INT(DSP_PLAT_BUTTON_NOT_WAKE_CAPABLE, dsp_plat_button_check(6));
    DSP_CHECK_EQ_INT(DSP_PLAT_BUTTON_NOT_WAKE_CAPABLE, dsp_plat_button_check(8));
    DSP_CHECK_EQ_INT(DSP_PLAT_BUTTON_NOT_WAKE_CAPABLE, dsp_plat_button_check(9));
    DSP_CHECK_EQ_INT(DSP_PLAT_BUTTON_NOT_WAKE_CAPABLE, dsp_plat_button_check(21));
}

static void test_rtc_retained_storage(void)
{
    dsp_power_retained_t r;
    dsp_power_retained_defaults(&r);
    r.time.consecutive_failures = 4;
    r.time.next_attempt = 1795158000 + 7200;
    r.conn.last_notify_attempt = 1795158000;
    r.wake_count = 9;
    uint8_t img[DSP_POWER_RETAINED_BYTES];
    DSP_CHECK(dsp_power_retained_encode(&r, img));
    dsp_plat_rtc_t rtc;
    memset(&rtc, 0xA5, sizeof rtc); /* power-on garbage */
    uint8_t buf[DSP_POWER_RETAINED_BYTES + 1];
    DSP_CHECK_EQ_INT(0, dsp_plat_rtc_load(&rtc, buf, sizeof buf));
    /* Deep-sleep retention: store, then load and decode (backoff and hourly limit kept). */
    DSP_CHECK(dsp_plat_rtc_store(&rtc, img, sizeof img));
    size_t n = dsp_plat_rtc_load(&rtc, buf, sizeof buf);
    DSP_CHECK_EQ_INT(DSP_POWER_RETAINED_BYTES, n);
    dsp_power_retained_t out;
    DSP_CHECK(dsp_power_retained_decode(buf, n, &out));
    DSP_CHECK_EQ_INT(4, out.time.consecutive_failures);
    DSP_CHECK_EQ_INT(1795158000 + 7200, out.time.next_attempt);
    DSP_CHECK_EQ_INT(1795158000, out.conn.last_notify_attempt);
    /* Corrupt data with a valid header: the domain CRC rejects it. */
    rtc.data[20] ^= 0x01;
    n = dsp_plat_rtc_load(&rtc, buf, sizeof buf);
    DSP_CHECK(!dsp_power_retained_decode(buf, n, &out));
    /* Torn or random headers: nothing is copied. */
    DSP_CHECK(dsp_plat_rtc_store(&rtc, img, sizeof img));
    rtc.len_inv ^= 1u;
    DSP_CHECK_EQ_INT(0, dsp_plat_rtc_load(&rtc, buf, sizeof buf));
    rtc.len = DSP_PLAT_RTC_CAP + 1;
    rtc.len_inv = ~rtc.len;
    DSP_CHECK_EQ_INT(0, dsp_plat_rtc_load(&rtc, buf, sizeof buf));
    rtc.len = 0;
    rtc.len_inv = ~0u;
    DSP_CHECK_EQ_INT(0, dsp_plat_rtc_load(&rtc, buf, sizeof buf));
    /* A different stored length (e.g. an older format) is rejected by the decoder. */
    DSP_CHECK(dsp_plat_rtc_store(&rtc, img, 40));
    n = dsp_plat_rtc_load(&rtc, buf, sizeof buf);
    DSP_CHECK_EQ_INT(40, n);
    DSP_CHECK(!dsp_power_retained_decode(buf, n, &out));
    DSP_CHECK(!dsp_plat_rtc_store(&rtc, img, 0));
    DSP_CHECK(!dsp_plat_rtc_store(&rtc, img, DSP_PLAT_RTC_CAP + 1));
    /* No clock trust is restored from retained data: trust needs the continuity evidence
     * (reset reason + RTC marker + persisted last sync), which the block does not hold. */
    dsp_time_meta_t meta;
    memset(&meta, 0, sizeof meta);
    dsp_timekeeper_t tk;
    DSP_CHECK(dsp_timekeeper_init(&tk, &meta, 24, &out.time));
    dsp_time_reading_t rd = {1795158000, false};
    dsp_timekeeper_evaluate(&tk, rd);
    DSP_CHECK_EQ_INT(DSP_TRUST_UNKNOWN, tk.trust);
}

/* ---- Two-thread queue boundary ------------------------------------------------------------ */

#define POSTS 400
static fake_kv_t kv;
static dsp_app_t app;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static int responses_by_id[POSTS + 2];
static int posted_ok, posted_busy;

static uint64_t real_ms(void *ctx)
{
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
static const dsp_monotonic_t MONO = {NULL, real_ms};
static dsp_utc_t clk_now(void *ctx)
{
    (void)ctx;
    return 1795158000;
}
static bool clk_set(void *ctx, dsp_utc_t t)
{
    (void)ctx;
    (void)t;
    return true;
}
static void clk_mark(void *ctx, dsp_utc_t t)
{
    (void)ctx;
    (void)t;
}
static const dsp_clock_port_t CLOCK = {NULL, clk_now, clk_set, clk_mark};
static dsp_time_reading_t reading(void *ctx, dsp_utc_t last)
{
    (void)ctx;
    (void)last;
    dsp_time_reading_t r = {1795158000, false};
    return r;
}
static const dsp_time_port_t TIMEP = {NULL, reading};
static dsp_pump_result_t p_run(void *ctx, uint32_t ms)
{
    (void)ctx;
    (void)ms;
    return DSP_PUMP_OK;
}
static void p_off(void *ctx)
{
    (void)ctx;
}
static bool p_is_off(void *ctx)
{
    (void)ctx;
    return true;
}
static const dsp_pump_port_t PUMP = {NULL, p_run, p_off, p_is_off};
static dsp_wake_cause_t pw_cause(void *ctx)
{
    (void)ctx;
    return DSP_CAUSE_BUTTON;
}
static size_t pw_load(void *ctx, uint8_t *b, size_t c)
{
    (void)ctx;
    (void)b;
    (void)c;
    return 0;
}
static bool pw_save(void *ctx, const uint8_t *b, size_t l)
{
    (void)ctx;
    (void)b;
    (void)l;
    return true;
}
static int slept;
static void pw_sleep(void *ctx, int64_t s)
{
    (void)ctx;
    (void)s;
    slept++;
}
static void pw_fail(void *ctx, uint32_t f)
{
    (void)ctx;
    (void)f;
}
static const dsp_power_port_t POWER = {NULL, pw_cause, pw_load, pw_save, pw_sleep, pw_fail};
static bool b_on;
static pthread_t producer_thread;
static bool producer_started;
static void *producer(void *arg);
static bool b_start(void *ctx)
{
    (void)ctx;
    b_on = true;
    /* The (future) BLE task exists only once BLE is started: that is when posting begins. */
    producer_started = pthread_create(&producer_thread, NULL, producer, NULL) == 0;
    return true;
}
static bool b_connected(void *ctx)
{
    (void)ctx;
    return true; /* the phone stays connected; END ends the session */
}
static void b_suspend(void *ctx)
{
    (void)ctx;
    b_on = false;
}
static bool b_resume(void *ctx)
{
    (void)ctx;
    b_on = true;
    return true;
}
static void b_stop(void *ctx)
{
    (void)ctx;
    b_on = false;
}
static bool b_is_off(void *ctx)
{
    (void)ctx;
    return !b_on;
}
static void b_wait(void *ctx, uint32_t ms)
{
    (void)ctx;
    (void)ms;
    struct timespec ts = {0, 200000}; /* 0.2 ms: the producer runs concurrently */
    nanosleep(&ts, NULL);
}
static void b_respond(void *ctx, const dsp_app_response_t *r)
{
    (void)ctx;
    if (r->id <= POSTS + 1) {
        responses_by_id[r->id]++; /* application task only */
    }
}
static const dsp_ble_port_t BLE = {NULL, b_start, b_connected, b_suspend, b_resume,
                                   b_stop, b_is_off, b_wait, b_respond};
static void m_lock(void *ctx)
{
    (void)ctx;
    pthread_mutex_lock(&mtx);
}
static void m_unlock(void *ctx)
{
    (void)ctx;
    pthread_mutex_unlock(&mtx);
}
static const dsp_app_sync_t SYNC = {NULL, m_lock, m_unlock};

static void *producer(void *arg)
{
    (void)arg;
    for (int i = 1; i <= POSTS; i++) {
        dsp_app_cmd_t c;
        memset(&c, 0, sizeof c);
        c.kind = DSP_CMD_STATUS;
        c.id = (uint32_t)i;
        while (!dsp_app_post(&app, &c)) {
            posted_busy++; /* full (or not yet open): the platform would answer busy; retry */
            struct timespec ts = {0, 50000};
            nanosleep(&ts, NULL);
        }
        posted_ok++;
    }
    dsp_app_cmd_t end;
    memset(&end, 0, sizeof end);
    end.kind = DSP_CMD_END_SESSION;
    end.id = POSTS + 1;
    while (!dsp_app_post(&app, &end)) {
        struct timespec ts = {0, 50000};
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static void test_queue_two_threads(void)
{
    fake_kv_init(&kv, true);
    dsp_app_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.kv = &kv.iface;
    cfg.tz = &DSP_TZ_EUROPE_PRAGUE;
    cfg.pump = &PUMP;
    cfg.power = &POWER;
    cfg.clock = &CLOCK;
    cfg.time = &TIMEP;
    cfg.ble = &BLE;
    cfg.mono = &MONO;
    cfg.sync = &SYNC;
    cfg.random = 7;
    cfg.service_max_s = 60;
    dsp_app_report_t rep = dsp_app_run_wake(&app, &cfg);
    DSP_CHECK(producer_started);
    if (producer_started) {
        pthread_join(producer_thread, NULL);
    }
    /* After the session: posting is refused (queue closed). */
    dsp_app_cmd_t late;
    memset(&late, 0, sizeof late);
    late.kind = DSP_CMD_STATUS;
    DSP_CHECK(!dsp_app_post(&app, &late));
    DSP_CHECK(rep.ble_session);
    DSP_CHECK_EQ_INT(DSP_BLE_END_COMMAND, rep.ble_end);
    DSP_CHECK_EQ_INT(1, slept);
    DSP_CHECK_EQ_INT(POSTS, posted_ok);
    /* Every accepted command answered exactly once, in a single owner (no loss, no dup). */
    int missing = 0, dup = 0;
    for (int i = 1; i <= POSTS + 1; i++) {
        missing += responses_by_id[i] == 0;
        dup += responses_by_id[i] > 1;
    }
    DSP_CHECK_EQ_INT(0, missing);
    DSP_CHECK_EQ_INT(0, dup);
    printf("queue: %d posted, %d busy retries, %zu responses\n", posted_ok, posted_busy, rep.responses);
}

int main(void)
{
    DSP_RUN(test_wake_cause_mapping);
    DSP_RUN(test_sleep_and_button);
    DSP_RUN(test_rtc_retained_storage);
    DSP_RUN(test_queue_two_threads);
    return dsp_test_finish();
}
