/*
 * Time management tests (M1.4): trust model, correction policy, sync scheduling, Wi-Fi
 * session lifecycle, wake planning and end-to-end dosing safety across clock events.
 *
 * A simulated world keeps the TRUE time (real_now) separately from the device clock
 * (real_now + rtc_offset, or a small uptime counter after a power loss). NTP returns true
 * time. Every automatic dose is recorded by occurrence identity AND by the true local date
 * on which it happened; neither may ever repeat.
 */
#include "dsp_exec.h"
#include "dsp_net.h"
#include "dsp_storage.h"
#include "dsp_test.h"
#include "dsp_time_service.h"
#include "dsp_timekeeping.h"
#include "dsp_wakeup.h"
#include "fake_kv.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const dsp_tz_t *const TZ = &DSP_TZ_EUROPE_PRAGUE;

#define H 3600
#define DAY 86400
/* 2026-11-20T07:00:00Z = 08:00 CET */
#define T0 ((dsp_utc_t)1795158000)

/* ---- Simulated world ------------------------------------------------------------------ */

static dsp_utc_t real_now;
static int64_t rtc_offset; /* device clock - true time */
static bool rtc_lost;      /* after power loss: device clock = seconds since power-on */
static dsp_utc_t power_on_at;
static bool marker_valid;  /* RTC-retained continuity marker */
static dsp_utc_t marker;
static bool set_fails;
static int clock_sets;

static dsp_utc_t device_now(void)
{
    return rtc_lost ? real_now - power_on_at : real_now + rtc_offset;
}

static dsp_utc_t port_now(void *ctx)
{
    (void)ctx;
    return device_now();
}

static bool port_set(void *ctx, dsp_utc_t utc)
{
    (void)ctx;
    if (set_fails) {
        return false;
    }
    clock_sets++;
    rtc_lost = false;
    rtc_offset = utc - real_now;
    return true;
}

static void port_mark(void *ctx, dsp_utc_t last_sync)
{
    (void)ctx;
    marker = last_sync;
    marker_valid = true;
}

static const dsp_clock_port_t CLOCK = {NULL, port_now, port_set, port_mark};

/* Fake NTP: true time (+ error), scripted failures. */
static dsp_sync_status_t ntp_script;
static int64_t ntp_error;
static int ntp_calls;

static dsp_sync_status_t ntp_sync(void *ctx, uint32_t timeout_ms, dsp_time_sample_t *out)
{
    (void)ctx;
    (void)timeout_ms;
    ntp_calls++;
    if (ntp_script != DSP_SYNC_OK) {
        return ntp_script;
    }
    out->source_utc = real_now + ntp_error;
    out->clock_utc = device_now();
    return DSP_SYNC_OK;
}

static const dsp_time_source_t NTP = {NULL, ntp_sync};

/* Fake Wi-Fi driver. */
static struct {
    int starts, connects, disconnects, stops;
    bool radio_on;
    dsp_net_status_t start_result;
    dsp_net_status_t connect_result[8];
    int script_len;
} wf;

static dsp_net_status_t wf_start(void *ctx)
{
    (void)ctx;
    wf.starts++;
    if (wf.start_result == DSP_NET_OK) {
        wf.radio_on = true;
    }
    return wf.start_result;
}

static dsp_net_status_t wf_connect(void *ctx, const dsp_wifi_credentials_t *c, uint32_t timeout_ms)
{
    (void)ctx;
    (void)timeout_ms;
    DSP_CHECK(dsp_wifi_credentials_valid(c));
    DSP_CHECK(wf.radio_on);
    int i = wf.connects++;
    return i < wf.script_len ? wf.connect_result[i] : DSP_NET_OK;
}

static void wf_disconnect(void *ctx)
{
    (void)ctx;
    wf.disconnects++;
}

static void wf_stop(void *ctx)
{
    (void)ctx;
    wf.stops++;
    wf.radio_on = false;
}

static const dsp_wifi_driver_t WIFI_DRV = {NULL, wf_start, wf_connect, wf_disconnect, wf_stop};

/* Test credentials: an OPEN network (no passphrase), clearly not a real secret. */
static bool creds_available = true;
static dsp_cred_status_t creds_get(void *ctx, dsp_wifi_credentials_t *out)
{
    (void)ctx;
    if (!creds_available) {
        return DSP_CRED_UNAVAILABLE;
    }
    strcpy(out->ssid, "test-network");
    out->passphrase[0] = '\0';
    return DSP_CRED_OK;
}
static const dsp_wifi_credentials_provider_t CREDS = {NULL, creds_get};

/* ---- Device under test ---------------------------------------------------------------- */

static fake_kv_t kv;
static dsp_storage_t st;
static dsp_history_store_t hist;
static dsp_exec_t ex;
static dsp_timekeeper_t tk;
static dsp_wifi_manager_t wifi;
static dsp_time_service_t svc;
static dsp_time_retained_t rtc_mem; /* platform RTC memory */
static bool rtc_mem_valid;
static uint16_t interval_h = 24;
static dsp_schedule_t S;

#define MAX_DOSES 512
static char dose_ids[MAX_DOSES][DSP_OCCURRENCE_ID_BUF_LEN];
static int32_t dose_real_days[MAX_DOSES];
static int n_doses;
static int duplicate_ids;
static int duplicate_days;

static int32_t real_local_day(void)
{
    dsp_date_t d;
    DSP_CHECK(dsp_utc_to_local_date(TZ, real_now, &d));
    return dsp_date_to_days(d);
}

static dsp_time_reading_t reading(void)
{
    dsp_time_reading_t r;
    r.now = device_now();
    r.time_base_continuous = !rtc_lost && marker_valid && marker == tk.meta.last_sync_utc;
    return r;
}

/* Restart: RAM lost. power_loss additionally resets the RTC and the RTC memory. */
static void boot(bool power_loss)
{
    if (power_loss) {
        rtc_lost = true;
        power_on_at = real_now;
        marker_valid = false;
        rtc_mem_valid = false;
    }
    fake_kv_power_cycle(&kv);
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_open(&st, &kv.iface, TZ, true));
    dsp_storage_history_store(&st, &hist);
    dsp_time_meta_t meta;
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_load_time(&st, &meta));
    DSP_CHECK(dsp_timekeeper_init(&tk, &meta, interval_h, rtc_mem_valid ? &rtc_mem : NULL));
    dsp_exec_config_t ec = {&hist, TZ, DSP_EXEC_DUE_WINDOW_DEFAULT_S, DSP_EXEC_RETENTION_DAYS_DEFAULT};
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_init(&ex, &ec));
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_recover(&ex, NULL));
    dsp_wifi_config_t wc = {DSP_WIFI_TIMEOUT_DEFAULT_MS, DSP_WIFI_ATTEMPTS_DEFAULT};
    DSP_CHECK_EQ_INT(DSP_NET_OK, dsp_wifi_init(&wifi, &WIFI_DRV, &CREDS, &wc));
    svc = (dsp_time_service_t){&tk, &wifi, &NTP, &CLOCK, &st, 0};
}

static void reset_world(void)
{
    fake_kv_init(&kv, true);
    real_now = T0 - 2 * H;
    rtc_offset = 0;
    rtc_lost = true; /* factory fresh: RTC counts from power-on */
    power_on_at = real_now;
    marker_valid = false;
    rtc_mem_valid = false;
    set_fails = false;
    clock_sets = 0;
    ntp_script = DSP_SYNC_OK;
    ntp_error = 0;
    ntp_calls = 0;
    memset(&wf, 0, sizeof wf);
    creds_available = true;
    interval_h = 24;
    n_doses = 0;
    duplicate_ids = 0;
    duplicate_days = 0;
    S = (dsp_schedule_t){0};
    strcpy(S.id, "daily-0800");
    S.revision = 1;
    S.enabled = true;
    S.amount_ul = 5000;
    S.start_date = (dsp_date_t){2026, 1, 1};
    S.hour = 8;
    S.kind = DSP_REC_DAILY;
    S.interval = 1;
    boot(false);
}

/* Dosing step of the future dosing service: the latest occurrence not older than the due
 * window, claimed through the M1.2 coordinator with the timekeeper's clock snapshot. */
static int dose_step(void)
{
    dsp_clock_t c = dsp_timekeeper_clock(&tk, device_now());
    if (!c.valid) {
        return 0;
    }
    dsp_occurrence_t occ;
    if (dsp_rec_next(&S, TZ, c.now - DSP_EXEC_DUE_WINDOW_DEFAULT_S - 1, &occ) != DSP_REC_OK ||
        occ.at > c.now) {
        return 0;
    }
    dsp_exec_token_t tok;
    if (dsp_exec_claim(&ex, &S, &occ, c, &tok) != DSP_EXEC_OK) {
        return 0;
    }
    DSP_CHECK_EQ_INT(DSP_EXEC_OK, dsp_exec_finish(&ex, &tok, DSP_HISTORY_COMPLETED));
    char id[DSP_OCCURRENCE_ID_BUF_LEN];
    DSP_CHECK_EQ_INT(DSP_REC_OK, dsp_occurrence_id_format(S.id, occ.local_date, id, sizeof id));
    int32_t day = real_local_day();
    for (int i = 0; i < n_doses; i++) {
        duplicate_ids += strcmp(dose_ids[i], id) == 0;
        duplicate_days += dose_real_days[i] == day;
    }
    if (n_doses < MAX_DOSES) {
        strcpy(dose_ids[n_doses], id);
        dose_real_days[n_doses] = day;
        n_doses++;
    }
    return 1;
}

/* One wake: evaluate trust, dose what is due, sync if due, dose again (time may just have
 * become valid), keep the retained state in "RTC memory" for the next deep sleep. */
static dsp_time_sync_report_t last_report;

static int wake(void)
{
    dsp_timekeeper_evaluate(&tk, reading());
    int d = dose_step();
    last_report = dsp_time_service_run(&svc, false, false);
    DSP_CHECK(!wf.radio_on); /* Wi-Fi never left running */
    d += dose_step();
    rtc_mem = tk.retained;
    rtc_mem_valid = true;
    return d;
}

static void sleep_until(dsp_utc_t t)
{
    real_now = t;
}

/* ---- 1./2./20. First boot, first sync, recovery --------------------------------------- */

static void test_first_boot_without_valid_time(void)
{
    reset_world();
    dsp_timekeeper_evaluate(&tk, reading());
    DSP_CHECK_EQ_INT(DSP_TRUST_UNKNOWN, tk.trust);
    DSP_CHECK_EQ_INT(DSP_UNTRUSTED_NEVER_SYNCED, tk.reason);
    DSP_CHECK(!dsp_timekeeper_clock(&tk, device_now()).valid);
    DSP_CHECK(dsp_timekeeper_sync_due(&tk, device_now()));

    /* No credentials: Wi-Fi is never started, trust unchanged, retry backed off. */
    creds_available = false;
    sleep_until(T0);
    DSP_CHECK_EQ_INT(0, wake());
    DSP_CHECK(last_report.attempted);
    DSP_CHECK_EQ_INT(DSP_NET_ERR_NO_CREDENTIALS, last_report.wifi);
    DSP_CHECK_EQ_INT(DSP_SYNC_ERR_NO_CREDENTIALS, last_report.decision.status);
    DSP_CHECK_EQ_INT(0, wf.starts);
    DSP_CHECK_EQ_INT(DSP_TRUST_UNKNOWN, tk.trust);
    DSP_CHECK_EQ_INT(device_now() + DSP_TIME_RETRY_UNKNOWN_MIN_S, dsp_timekeeper_next_sync(&tk));
    DSP_CHECK_EQ_INT(0, n_doses); /* never doses without valid time */

    /* Credentials arrive; the next attempt synchronizes and dosing becomes possible. */
    creds_available = true;
    sleep_until(T0 + 5 * 60); /* not yet due (15 min backoff) */
    wake();
    DSP_CHECK(!last_report.attempted);
    sleep_until(T0 + 16 * 60); /* due; 08:16 is outside the 08:00 window */
    DSP_CHECK_EQ_INT(0, wake());
    DSP_CHECK_EQ_INT(DSP_SYNC_OK, last_report.decision.status);
    DSP_CHECK_EQ_INT(DSP_CORRECTION_INITIAL, last_report.decision.kind);
    DSP_CHECK(last_report.clock_set);
    DSP_CHECK_EQ_INT(DSP_TRUST_SYNCED, tk.trust);
    DSP_CHECK_EQ_INT(real_now, device_now());
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, last_report.persist);
    DSP_CHECK_EQ_INT(1, wf.starts);
    DSP_CHECK_EQ_INT(1, wf.stops);
    /* Next day the dose runs normally. */
    sleep_until(T0 + DAY + 20);
    DSP_CHECK_EQ_INT(1, wake());
    DSP_CHECK_EQ_INT(0, duplicate_ids + duplicate_days);
}

/* ---- 3./4./18. Interval, configured 48 h, long outage --------------------------------- */

static void synced_world(uint16_t hours)
{
    reset_world();
    interval_h = hours;
    boot(false);
    sleep_until(T0 - H);
    wake(); /* initial sync at 07:00 CET */
    DSP_CHECK_EQ_INT(DSP_TRUST_SYNCED, tk.trust);
}

static void test_sync_interval_24h_and_48h(void)
{
    synced_world(24);
    dsp_utc_t first = tk.meta.last_sync_utc;
    DSP_CHECK_EQ_INT(first + 24 * H, dsp_timekeeper_next_sync(&tk));
    int calls = ntp_calls;
    sleep_until(first + 23 * H);
    wake();
    DSP_CHECK_EQ_INT(calls, ntp_calls); /* not due: no Wi-Fi, no NTP */
    DSP_CHECK_EQ_INT(0, wf.starts - 1);
    sleep_until(first + 24 * H);
    wake();
    DSP_CHECK_EQ_INT(calls + 1, ntp_calls);
    DSP_CHECK_EQ_INT(first + 24 * H, tk.meta.last_sync_utc);

    synced_world(48);
    first = tk.meta.last_sync_utc;
    DSP_CHECK_EQ_INT(first + 48 * H, dsp_timekeeper_next_sync(&tk));
    sleep_until(first + 30 * H);
    wake();
    DSP_CHECK_EQ_INT(DSP_TRUST_SYNCED, tk.trust); /* within 48 h */
    DSP_CHECK_EQ_INT(1, ntp_calls);
    /* Invalid interval configuration is refused. */
    dsp_timekeeper_t t2;
    dsp_time_meta_t z = {0, 0, 0, 0, 0};
    DSP_CHECK(!dsp_timekeeper_init(&t2, &z, 23, NULL));
    DSP_CHECK(!dsp_timekeeper_init(&t2, &z, 49, NULL));
}

static void test_long_internet_outage_keeps_dosing(void)
{
    synced_world(24);
    ntp_script = DSP_SYNC_ERR_TIMEOUT;
    int doses = 0;
    for (int day = 0; day < 60; day++) { /* two months without NTP */
        sleep_until(T0 + day * DAY + 30);
        doses += wake();
    }
    DSP_CHECK_EQ_INT(60, doses); /* every dose executed */
    DSP_CHECK_EQ_INT(DSP_TRUST_HOLDOVER, tk.trust);
    DSP_CHECK(dsp_timekeeper_sync_overdue(&tk, device_now())); /* diagnostic only */
    DSP_CHECK(dsp_timekeeper_clock(&tk, device_now()).valid);
    DSP_CHECK_EQ_INT(0, duplicate_ids + duplicate_days);
    /* Bounded retries: never more than one session per wake, backoff capped at 24 h. */
    DSP_CHECK(ntp_calls <= 61);
    DSP_CHECK(dsp_timekeeper_next_sync(&tk) - device_now() <= 24 * H);
}

/* ---- 5./6./7. Wi-Fi unavailable, NTP timeout, repeated failures ------------------------ */

static void test_wifi_and_ntp_failures(void)
{
    synced_world(24);
    dsp_utc_t base = tk.meta.last_sync_utc;
    /* Network not found on all 3 attempts: radio stopped, trust unchanged. */
    wf.connect_result[0] = wf.connect_result[1] = wf.connect_result[2] = DSP_NET_ERR_NO_AP;
    wf.script_len = 3;
    wf.connects = 0;
    sleep_until(base + 24 * H);
    wake();
    DSP_CHECK_EQ_INT(DSP_NET_ERR_NO_AP, last_report.wifi);
    DSP_CHECK_EQ_INT(3, wf.connects);
    DSP_CHECK(!wf.radio_on);
    DSP_CHECK_EQ_INT(DSP_TRUST_HOLDOVER, tk.trust);
    DSP_CHECK_EQ_INT(1, tk.retained.consecutive_failures);
    DSP_CHECK_EQ_INT(device_now() + DSP_TIME_RETRY_TRUSTED_MIN_S, dsp_timekeeper_next_sync(&tk));
    /* Rejected credentials: no retry within the session. */
    wf.connects = 0;
    wf.connect_result[0] = DSP_NET_ERR_AUTH;
    wf.script_len = 1;
    sleep_until(dsp_timekeeper_next_sync(&tk));
    wake();
    DSP_CHECK_EQ_INT(DSP_NET_ERR_AUTH, last_report.wifi);
    DSP_CHECK_EQ_INT(1, wf.connects);
    /* NTP timeout after a good connection. */
    wf.script_len = 0;
    ntp_script = DSP_SYNC_ERR_TIMEOUT;
    sleep_until(dsp_timekeeper_next_sync(&tk));
    wake();
    DSP_CHECK_EQ_INT(DSP_SYNC_ERR_TIMEOUT, last_report.decision.status);
    DSP_CHECK_EQ_INT(3, tk.retained.consecutive_failures);
    /* Backoff doubles and is capped at the interval. */
    for (int i = 0; i < 10; i++) {
        sleep_until(dsp_timekeeper_next_sync(&tk));
        wake();
    }
    DSP_CHECK_EQ_INT(24 * H, dsp_timekeeper_next_sync(&tk) - device_now());
    DSP_CHECK_EQ_INT(DSP_TRUST_HOLDOVER, tk.trust);
    /* Implausible NTP answers are rejected and never applied. */
    ntp_script = DSP_SYNC_OK;
    int sets = clock_sets;
    sleep_until(dsp_timekeeper_next_sync(&tk));
    ntp_error = -(real_now - (DSP_TIME_PLAUSIBLE_MIN - DAY)); /* answer: 2024-12-31 */
    wake();
    DSP_CHECK_EQ_INT(DSP_SYNC_ERR_IMPLAUSIBLE, last_report.decision.status);
    DSP_CHECK_EQ_INT(sets, clock_sets);
    /* 20. Recovery after a successful sync. */
    ntp_error = 0;
    sleep_until(dsp_timekeeper_next_sync(&tk));
    wake();
    DSP_CHECK_EQ_INT(DSP_SYNC_OK, last_report.decision.status);
    DSP_CHECK_EQ_INT(DSP_TRUST_SYNCED, tk.trust);
    DSP_CHECK_EQ_INT(0, tk.retained.consecutive_failures);
    DSP_CHECK_EQ_INT(device_now() + 24 * H, dsp_timekeeper_next_sync(&tk));
}

/* ---- 8./9. Deep-sleep continuity vs complete power loss ------------------------------- */

static void test_deep_sleep_vs_power_loss(void)
{
    synced_world(24);
    /* Deep sleep / soft reset: RTC and marker kept -> trusted without Wi-Fi. */
    sleep_until(T0 + 10);
    boot(false);
    dsp_timekeeper_evaluate(&tk, reading());
    DSP_CHECK_EQ_INT(DSP_TRUST_SYNCED, tk.trust);
    DSP_CHECK_EQ_INT(1, dose_step());

    /* Power loss: RTC restarts from zero. The stored last_sync is NOT the current time. */
    sleep_until(T0 + DAY - 5 * 60);
    boot(true);
    dsp_timekeeper_evaluate(&tk, reading());
    DSP_CHECK_EQ_INT(DSP_TRUST_UNKNOWN, tk.trust);
    DSP_CHECK_EQ_INT(DSP_UNTRUSTED_TIME_BASE_LOST, tk.reason);
    DSP_CHECK(tk.meta.last_sync_utc != 0); /* known, but not used as "now" */
    DSP_CHECK(!dsp_timekeeper_clock(&tk, device_now()).valid);
    DSP_CHECK(dsp_timekeeper_sync_due(&tk, device_now()));
    /* RTC lost but marker somehow valid (impossible combination) is still not trusted:
     * the clock reads ~0 s, outside the plausible range. */
    dsp_time_reading_t odd = {device_now(), true};
    dsp_timekeeper_evaluate(&tk, odd);
    DSP_CHECK_EQ_INT(DSP_TRUST_UNKNOWN, tk.trust);
    DSP_CHECK_EQ_INT(DSP_UNTRUSTED_OUT_OF_RANGE, tk.reason);
    /* Sync with Wi-Fi down: no dosing while unknown; the 08:00 dose after power-up waits for
     * valid time and is missed if that comes too late. */
    ntp_script = DSP_SYNC_ERR_TIMEOUT;
    sleep_until(T0 + DAY + 30);
    DSP_CHECK_EQ_INT(0, wake());
    ntp_script = DSP_SYNC_OK;
    sleep_until(T0 + DAY + 20 * 60);
    DSP_CHECK_EQ_INT(0, wake()); /* synced at 08:20: outside the window */
    DSP_CHECK_EQ_INT(DSP_TRUST_SYNCED, tk.trust);
    /* Clock behind the last sync while "continuous": RTC anomaly -> not trusted. */
    dsp_time_reading_t behind = {tk.meta.last_sync_utc - 2 * H, true};
    dsp_timekeeper_evaluate(&tk, behind);
    DSP_CHECK_EQ_INT(DSP_UNTRUSTED_BEHIND_LAST_SYNC, tk.reason);
    DSP_CHECK_EQ_INT(0, duplicate_ids + duplicate_days);
}

/* ---- 10.-13. Clock corrections --------------------------------------------------------- */

static void test_small_corrections(void)
{
    synced_world(24);
    /* RTC drifted 4 min fast. The wake doses at device "08:00" (true 07:56), then performs
     * the due sync: small backward correction, accepted, no hold. The 08:00 occurrence is
     * "due" again on the corrected clock, but the second dose step finds it claimed. */
    rtc_offset = 4 * 60;
    sleep_until(T0 + DAY - 4 * 60);
    DSP_CHECK_EQ_INT(1, wake()); /* exactly one dose in this wake */
    DSP_CHECK(last_report.attempted);
    DSP_CHECK_EQ_INT(DSP_CORRECTION_SMALL, last_report.decision.kind);
    DSP_CHECK_EQ_INT(-4 * 60, last_report.decision.offset_s);
    DSP_CHECK(!dsp_timekeeper_hold_active(&tk, device_now()));
    sleep_until(T0 + DAY + 60);
    DSP_CHECK_EQ_INT(0, wake());
    /* RTC 4 min slow: the dose runs at device 08:00:10, then the small forward correction.
     * Small corrections of a trusted clock never touch the floor (it stays at the value
     * set by the initial sync of synced_world, far in the past). */
    dsp_utc_t floor_before = tk.meta.not_before_utc;
    DSP_CHECK(floor_before != 0 && floor_before < T0);
    rtc_offset = -4 * 60;
    sleep_until(T0 + 2 * DAY + 4 * 60 + 10);
    DSP_CHECK_EQ_INT(1, wake());
    DSP_CHECK_EQ_INT(DSP_CORRECTION_SMALL, last_report.decision.kind);
    DSP_CHECK_EQ_INT(4 * 60, last_report.decision.offset_s);
    DSP_CHECK_EQ_INT(floor_before, dsp_timekeeper_clock(&tk, device_now()).not_before);
    DSP_CHECK_EQ_INT(0, dose_step());
    DSP_CHECK_EQ_INT(0, duplicate_ids + duplicate_days);
}

static void test_large_forward_jump_never_doses_twice(void)
{
    synced_world(24);
    sleep_until(T0 + 30);
    DSP_CHECK_EQ_INT(1, wake()); /* true day D dose */
    /* RTC glitch: the clock falls back a whole day (still "continuous" to the platform). */
    rtc_offset = -DAY + 5 * 60; /* at true D+1 08:00 the device shows D 08:05 */
    sleep_until(T0 + DAY);
    dsp_timekeeper_evaluate(&tk, reading());
    DSP_CHECK_EQ_INT(DSP_UNTRUSTED_NONE, tk.reason);
    DSP_CHECK_EQ_INT(0, dose_step()); /* D's occurrence again: history blocks it */
    /* Next true day: device shows D+1 07:55 ... then 08:00 at true 08:-5 ... */
    rtc_offset = -DAY;
    sleep_until(T0 + 2 * DAY + 30); /* device: D+1 08:00:30 -> doses identity D+1 */
    DSP_CHECK_EQ_INT(1, dose_step());
    /* Sync 3 minutes later: +1 day forward step. D+2 08:00 is now only 3.5 min old, but it
     * was jumped over: never executed (would be a second dose on the same true day). */
    sleep_until(T0 + 2 * DAY + 3 * 60);
    dsp_timekeeper_evaluate(&tk, reading());
    dsp_time_sync_report_t r = dsp_time_service_run(&svc, true, false);
    DSP_CHECK_EQ_INT(DSP_CORRECTION_FORWARD_LARGE, r.decision.kind);
    DSP_CHECK(dsp_timekeeper_clock(&tk, device_now()).not_before > T0 + 2 * DAY);
    DSP_CHECK_EQ_INT(0, dose_step());
    /* The floor survives a restart inside the window (derived from persisted metadata). */
    sleep_until(T0 + 2 * DAY + 5 * 60);
    boot(false);
    dsp_timekeeper_evaluate(&tk, reading());
    DSP_CHECK_EQ_INT(0, dose_step());
    /* Next day: normal. */
    sleep_until(T0 + 3 * DAY + 10);
    DSP_CHECK_EQ_INT(1, wake());
    DSP_CHECK_EQ_INT(0, duplicate_ids + duplicate_days);
}

static void test_large_backward_jump_within_and_beyond_7_days(void)
{
    /* RTC 3 days fast: doses run under "future" identities; NTP pulls back 3 days. */
    synced_world(24);
    rtc_offset = 3 * DAY; /* device is 3 days ahead: doses "future" identity D+4 */
    sleep_until(T0 + DAY + 30);
    DSP_CHECK_EQ_INT(1, wake()); /* dose, then the due sync pulls the clock back 3 days */
    DSP_CHECK_EQ_INT(DSP_CORRECTION_BACKWARD_LARGE, last_report.decision.kind);
    DSP_CHECK(!dsp_timekeeper_hold_active(&tk, device_now())); /* <= 7 days: no hold */
    dsp_time_sync_report_t r;
    for (int d = 4; d <= 9; d++) {
        sleep_until(T0 + d * DAY + 30);
        wake();
    }
    DSP_CHECK_EQ_INT(0, duplicate_ids + duplicate_days); /* watermark protected every day */

    /* RTC 20 days fast, then corrected: hold until (high water - 7 days). */
    synced_world(24);
    rtc_offset = 20 * DAY;
    sleep_until(T0 + DAY + 30);
    dsp_utc_t high = device_now(); /* highest trusted time before the correction */
    DSP_CHECK_EQ_INT(1, wake());   /* dose, then the due sync pulls back 20 days */
    r = last_report;
    DSP_CHECK_EQ_INT(DSP_CORRECTION_ROLLBACK_HOLD, r.decision.kind);
    DSP_CHECK(dsp_timekeeper_hold_active(&tk, device_now()));
    DSP_CHECK(tk.meta.hold_until_utc >= high - 7 * DAY && tk.meta.hold_until_utc <= high - 7 * DAY + H);
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, r.persist);
    /* The hold survives restarts and power loss (persisted), and blocks every dose. */
    int doses = 0;
    for (int d = 2; d < 14; d++) {
        sleep_until(T0 + d * DAY + 30);
        if (d == 5) {
            boot(false);
        }
        if (d == 8) {
            boot(true);
        }
        doses += wake();
        DSP_CHECK(!dsp_timekeeper_clock(&tk, device_now()).valid || device_now() >= tk.meta.hold_until_utc);
    }
    DSP_CHECK_EQ_INT(0, doses); /* until true time reaches high - 7 days (~day 14) */
    /* The hold ends at high - 7 days (~day 14). This schedule's M1.2 watermark is the
     * "future" date D+21 it was claimed under while the clock was ahead, so its own
     * occurrences stay blocked through day 21 (missed, never doubled) and resume on day 22.
     * Schedules without such a future claim resume as soon as the hold ends. */
    int blocked_by_watermark = 0;
    for (int d = 15; d <= 21; d++) {
        sleep_until(T0 + d * DAY + 30);
        blocked_by_watermark += wake();
        DSP_CHECK(dsp_timekeeper_clock(&tk, device_now()).valid); /* hold is over */
    }
    DSP_CHECK_EQ_INT(0, blocked_by_watermark);
    DSP_CHECK_EQ_INT(0, tk.meta.hold_until_utc);
    for (int d = 22; d <= 30; d++) {
        sleep_until(T0 + d * DAY + 30);
        doses += wake();
    }
    DSP_CHECK_EQ_INT(9, doses);
    DSP_CHECK_EQ_INT(0, duplicate_ids + duplicate_days);
}

static void test_hold_lifted_by_later_forward_sync(void)
{
    /* A bogus far-back NTP answer creates a hold; the next good sync jumps past it. */
    synced_world(24);
    sleep_until(T0 + 30);
    wake();
    ntp_error = -30 * DAY;
    sleep_until(T0 + H);
    dsp_timekeeper_evaluate(&tk, reading());
    dsp_time_sync_report_t r = dsp_time_service_run(&svc, true, false);
    DSP_CHECK_EQ_INT(DSP_CORRECTION_ROLLBACK_HOLD, r.decision.kind);
    ntp_error = 0;
    sleep_until(T0 + DAY - H);
    dsp_timekeeper_evaluate(&tk, reading());
    r = dsp_time_service_run(&svc, true, false);
    DSP_CHECK_EQ_INT(DSP_CORRECTION_FORWARD_LARGE, r.decision.kind);
    DSP_CHECK_EQ_INT(0, tk.meta.hold_until_utc);
    sleep_until(T0 + DAY + 30);
    DSP_CHECK_EQ_INT(1, wake());
    DSP_CHECK_EQ_INT(0, duplicate_ids + duplicate_days);
}

/* ---- 14.-17. DST, restarts around a dose, late wakeup --------------------------------- */

static void test_dst_and_restarts_around_dose(void)
{
    /* Spring 2027 (28 Mar) and autumn 2026 (25 Oct): doses at 08:00 local each day. */
    reset_world();
    real_now = (dsp_utc_t)1792472400 - 2 * H; /* 2026-10-20T08:00Z - 2h */
    boot(false);
    wake(); /* sync */
    int doses = 0;
    for (int d = 0; d < 10; d++) { /* crosses 25 Oct 2026 */
        dsp_utc_t at;
        dsp_clock_t c = dsp_timekeeper_clock(&tk, device_now());
        DSP_CHECK(dsp_wake_next_dose(&S, 1, TZ, c, &at));
        sleep_until(at + 15);
        doses += wake();
        dsp_date_t local;
        DSP_CHECK(dsp_utc_to_local_date(TZ, at + 7 * H, &local)); /* sanity: same local day */
    }
    DSP_CHECK_EQ_INT(10, doses);
    DSP_CHECK_EQ_INT(0, duplicate_ids + duplicate_days);
    /* Next-dose instant across the autumn change: 08:00 CEST = 06:00Z, 08:00 CET = 07:00Z. */
    dsp_clock_t c24 = {(dsp_utc_t)1792828800 /* 2026-10-24T08:00Z */, true, 0};
    dsp_utc_t at;
    DSP_CHECK(dsp_wake_next_dose(&S, 1, TZ, c24, &at));
    DSP_CHECK_EQ_INT((dsp_utc_t)1792911600, at); /* 2026-10-25T07:00Z = 08:00 CET */
    dsp_clock_t c27 = {(dsp_utc_t)1806127200 /* 2027-03-27T06:00Z */, true, 0};
    DSP_CHECK(dsp_wake_next_dose(&S, 1, TZ, c27, &at));
    DSP_CHECK_EQ_INT((dsp_utc_t)1806130800, at); /* 2027-03-27T07:00Z = 08:00 CET */

    /* 15. Restart one minute before the dose: executes normally after the restart. */
    synced_world(24);
    sleep_until(T0 - 60);
    boot(false);
    wake();
    sleep_until(T0 + 5);
    DSP_CHECK_EQ_INT(1, wake());
    /* 16. Restart right after the dose (inside the window): no repeat. */
    sleep_until(T0 + 60);
    boot(false);
    DSP_CHECK_EQ_INT(0, wake());
    /* 16b. Power loss right after the dose and quick time recovery: still no repeat. */
    sleep_until(T0 + 90);
    boot(true);
    sleep_until(T0 + 120);
    DSP_CHECK_EQ_INT(0, wake());
    DSP_CHECK_EQ_INT(DSP_TRUST_SYNCED, tk.trust);
    /* 17. Late wake outside the 10-minute window: skipped, never caught up. */
    sleep_until(T0 + DAY + 11 * 60);
    DSP_CHECK_EQ_INT(0, wake());
    sleep_until(T0 + DAY + 12 * 60);
    DSP_CHECK_EQ_INT(0, wake());
    sleep_until(T0 + 2 * DAY + 2);
    DSP_CHECK_EQ_INT(1, wake());
    DSP_CHECK_EQ_INT(0, duplicate_ids + duplicate_days);
}

/* ---- 19. Storage errors ---------------------------------------------------------------- */

static void test_storage_errors_do_not_stop_dosing(void)
{
    synced_world(24);
    dsp_time_meta_t before = tk.meta;
    int sets = clock_sets;
    kv.fail_sets = true; /* metadata cannot be persisted */
    sleep_until(tk.meta.last_sync_utc + 24 * H);
    wake();
    /* Durable before effective: the accepted time is NOT applied, trust and metadata are
     * unchanged, a retry is scheduled, dosing continues on the (still trusted) clock. */
    DSP_CHECK_EQ_INT(DSP_SYNC_ERR_PERSIST, last_report.decision.status);
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_WRITE, last_report.persist);
    DSP_CHECK(!last_report.clock_set);
    DSP_CHECK_EQ_INT(sets, clock_sets);
    DSP_CHECK(dsp_time_meta_equal(&before, &tk.meta));
    DSP_CHECK_EQ_INT(DSP_TRUST_HOLDOVER, tk.trust);
    DSP_CHECK_EQ_INT(1, tk.retained.consecutive_failures);
    kv.fail_sets = false;
    sleep_until(T0 + 2 * DAY + 10); /* dose runs, the retried sync succeeds */
    DSP_CHECK_EQ_INT(1, wake());
    DSP_CHECK_EQ_INT(DSP_SYNC_OK, last_report.decision.status);
    DSP_CHECK(!tk.meta_dirty);
    /* Clock step failure: the clock is not trusted (old time still in the RTC). */
    set_fails = true;
    rtc_offset = 3 * H;
    sleep_until(tk.meta.last_sync_utc + 24 * H + 5);
    dsp_timekeeper_evaluate(&tk, reading());
    dsp_time_sync_report_t r = dsp_time_service_run(&svc, true, false);
    DSP_CHECK(!r.clock_set);
    DSP_CHECK_EQ_INT(DSP_TRUST_UNKNOWN, tk.trust);
    DSP_CHECK(!dsp_timekeeper_clock(&tk, device_now()).valid);
    set_fails = false;
    DSP_CHECK_EQ_INT(0, duplicate_ids + duplicate_days);
}

/* ---- Wi-Fi session manager -------------------------------------------------------------- */

static void test_wifi_manager_lifecycle(void)
{
    memset(&wf, 0, sizeof wf);
    creds_available = true;
    dsp_wifi_manager_t m;
    dsp_wifi_config_t bad = {500, 3};
    DSP_CHECK_EQ_INT(DSP_NET_ERR_ARG, dsp_wifi_init(&m, &WIFI_DRV, &CREDS, &bad));
    bad = (dsp_wifi_config_t){15000, 0};
    DSP_CHECK_EQ_INT(DSP_NET_ERR_ARG, dsp_wifi_init(&m, &WIFI_DRV, &CREDS, &bad));
    bad = (dsp_wifi_config_t){15000, DSP_WIFI_ATTEMPTS_MAX + 1};
    DSP_CHECK_EQ_INT(DSP_NET_ERR_ARG, dsp_wifi_init(&m, &WIFI_DRV, &CREDS, &bad));
    dsp_wifi_config_t cfg = {5000, 2};
    DSP_CHECK_EQ_INT(DSP_NET_OK, dsp_wifi_init(&m, &WIFI_DRV, &CREDS, &cfg));
    DSP_CHECK_EQ_INT(DSP_WIFI_DISABLED, dsp_wifi_state(&m));
    /* Success, then a shared second user, then explicit shutdown. */
    DSP_CHECK_EQ_INT(DSP_NET_OK, dsp_wifi_connect(&m));
    DSP_CHECK_EQ_INT(DSP_WIFI_CONNECTED, dsp_wifi_state(&m));
    DSP_CHECK_EQ_INT(DSP_NET_OK, dsp_wifi_connect(&m)); /* reused (future notifications) */
    DSP_CHECK_EQ_INT(1, wf.starts);
    dsp_wifi_shutdown(&m);
    DSP_CHECK_EQ_INT(DSP_WIFI_DISABLED, dsp_wifi_state(&m));
    DSP_CHECK(!wf.radio_on);
    DSP_CHECK_EQ_INT(1, wf.disconnects);
    dsp_wifi_shutdown(&m); /* idempotent */
    DSP_CHECK_EQ_INT(1, wf.stops);
    /* Bounded retries on timeout, radio off afterwards, FAILED state. */
    memset(&wf, 0, sizeof wf);
    wf.connect_result[0] = wf.connect_result[1] = wf.connect_result[2] = DSP_NET_ERR_TIMEOUT;
    wf.script_len = 3;
    DSP_CHECK_EQ_INT(DSP_NET_ERR_TIMEOUT, dsp_wifi_connect(&m));
    DSP_CHECK_EQ_INT(2, wf.connects); /* max_attempts = 2 */
    DSP_CHECK_EQ_INT(DSP_WIFI_FAILED, dsp_wifi_state(&m));
    DSP_CHECK(!wf.radio_on);
    dsp_wifi_shutdown(&m);
    DSP_CHECK_EQ_INT(1, wf.stops); /* not stopped twice */
    /* Start failure. */
    memset(&wf, 0, sizeof wf);
    wf.start_result = DSP_NET_ERR_START;
    DSP_CHECK_EQ_INT(DSP_NET_ERR_START, dsp_wifi_connect(&m));
    DSP_CHECK_EQ_INT(0, wf.connects);
    DSP_CHECK_EQ_INT(DSP_WIFI_FAILED, dsp_wifi_state(&m));
    /* Second attempt after failure succeeds (new session). */
    wf.start_result = DSP_NET_OK;
    DSP_CHECK_EQ_INT(DSP_NET_OK, dsp_wifi_connect(&m));
    dsp_wifi_shutdown(&m);
    /* Unavailable or invalid credentials: radio never started. */
    memset(&wf, 0, sizeof wf);
    dsp_wifi_manager_t none;
    DSP_CHECK_EQ_INT(DSP_NET_OK, dsp_wifi_init(&none, &WIFI_DRV, &DSP_WIFI_NO_CREDENTIALS, &cfg));
    DSP_CHECK_EQ_INT(DSP_NET_ERR_NO_CREDENTIALS, dsp_wifi_connect(&none));
    DSP_CHECK_EQ_INT(0, wf.starts);
    DSP_CHECK_EQ_INT(DSP_WIFI_DISABLED, dsp_wifi_state(&none));
    dsp_wifi_credentials_t c;
    memset(&c, 0, sizeof c);
    DSP_CHECK(!dsp_wifi_credentials_valid(&c)); /* empty SSID */
    strcpy(c.ssid, "net");
    strcpy(c.passphrase, "short");
    DSP_CHECK(!dsp_wifi_credentials_valid(&c)); /* WPA passphrase < 8 */
    memset(c.ssid, 'a', sizeof c.ssid);           /* no NUL within 33 bytes */
    c.passphrase[0] = '\0';
    DSP_CHECK(!dsp_wifi_credentials_valid(&c));
    DSP_CHECK_EQ_INT(DSP_NET_ERR_ARG, dsp_wifi_connect(NULL));
}

/* ---- Wake planning ---------------------------------------------------------------------- */

static void test_wake_plan(void)
{
    dsp_utc_t now = T0;
    dsp_wake_inputs_t in = {true, now + 3 * H, true, now + 5 * H, false, 0, false, 0};
    dsp_wake_plan_t p = dsp_wake_plan(&in, now, 24 * H);
    DSP_CHECK_EQ_INT(DSP_WAKE_DOSE, p.reason);
    DSP_CHECK_EQ_INT(3 * H, p.sleep_s);
    in.sync_at = now + H;
    p = dsp_wake_plan(&in, now, 24 * H);
    DSP_CHECK_EQ_INT(DSP_WAKE_TIME_SYNC, p.reason);
    in.has_notification = true;
    in.notification_at = now + 30 * 60;
    p = dsp_wake_plan(&in, now, 24 * H);
    DSP_CHECK_EQ_INT(DSP_WAKE_NOTIFICATION, p.reason);
    in.has_other = true;
    in.other_at = now - 5; /* already due */
    p = dsp_wake_plan(&in, now, 24 * H);
    DSP_CHECK_EQ_INT(DSP_WAKE_OTHER, p.reason);
    DSP_CHECK_EQ_INT(0, p.sleep_s);
    /* Tie: dose wins. */
    dsp_wake_inputs_t tie = {true, now + H, true, now + H, false, 0, false, 0};
    DSP_CHECK_EQ_INT(DSP_WAKE_DOSE, dsp_wake_plan(&tie, now, 24 * H).reason);
    /* Nothing pending / far future: capped. */
    dsp_wake_inputs_t none = {false, 0, false, 0, false, 0, false, 0};
    p = dsp_wake_plan(&none, now, 12 * H);
    DSP_CHECK_EQ_INT(DSP_WAKE_MAX_SLEEP, p.reason);
    DSP_CHECK_EQ_INT(12 * H, p.sleep_s);
    dsp_wake_inputs_t far = {true, INT64_MAX, false, 0, false, 0, false, 0};
    p = dsp_wake_plan(&far, now, 12 * H);
    DSP_CHECK_EQ_INT(DSP_WAKE_MAX_SLEEP, p.reason);
    DSP_CHECK_EQ_INT(0, dsp_wake_plan(NULL, now, 10).sleep_s);
    /* Unknown time (raw clock ~ seconds since power-on): only the sync is planned. */
    dsp_clock_t invalid = {1000, false, 0};
    dsp_utc_t at = 0;
    DSP_CHECK(!dsp_wake_next_dose(&S, 1, TZ, invalid, &at));
    dsp_wake_inputs_t boot_in = {false, 0, true, 1000 + 15 * 60, false, 0, false, 0};
    p = dsp_wake_plan(&boot_in, 1000, 24 * H);
    DSP_CHECK_EQ_INT(DSP_WAKE_TIME_SYNC, p.reason);
    DSP_CHECK_EQ_INT(15 * 60, p.sleep_s);
}

/* ---- Randomized long run: clock chaos never causes a repeated dose -------------------- */

static uint32_t rng = 4242u;
static uint32_t rnd(uint32_t bound)
{
    rng = rng * 1664525u + 1013904223u;
    return (rng >> 8) % bound;
}

static void clock_chaos(bool rtc_glitches)
{
    reset_world();
    real_now = T0 - 3 * H;
    int glitches = 0;
    int power_losses = 0;
    int unknown_wakes = 0;
    for (int step = 0; step < 3000; step++) {
        /* Advance true time: mostly to around the next 08:00, sometimes random. */
        if (rnd(3) == 0) {
            real_now += (dsp_utc_t)rnd(6 * H);
        } else {
            dsp_utc_t next = T0 + ((real_now - T0) / DAY + 1) * DAY;
            real_now = next - 5 * 60 + (dsp_utc_t)rnd(20 * 60);
        }
        if (!rtc_lost) {
            rtc_offset += (int64_t)rnd(121) - 60; /* drift up to +-1 min per step */
        }
        uint32_t ev = rnd(40);
        if (!rtc_glitches && ev <= 1) {
            ev = 39; /* realistic RTC: drift only, no jumps between syncs */
        }
        switch (ev) {
        case 0: rtc_offset += (int64_t)rnd(40) * DAY / 4 - 5 * DAY; glitches++; break; /* +-days */
        case 1: rtc_offset += (int64_t)rnd(3 * H) - H; glitches++; break;
        case 2: boot(true); power_losses++; break;
        case 3: boot(false); break;
        case 4: ntp_script = DSP_SYNC_ERR_TIMEOUT; break;
        case 5: case 6: case 7: ntp_script = DSP_SYNC_OK; break;
        case 8: creds_available = false; break;
        case 9: case 10: creds_available = true; break;
        case 11: ntp_error = (rnd(2) ? 1 : -1) * (int64_t)rnd(40) * DAY; break; /* bogus server */
        case 12: case 13: ntp_error = 0; break;
        default: break;
        }
        if (real_now > T0 + 2500 * DAY) { /* about 7 years, inside the 2000-2099 range */
            break;
        }
        int before = n_doses;
        dsp_timekeeper_evaluate(&tk, reading());
        bool trusted_now = tk.trust != DSP_TRUST_UNKNOWN;
        unknown_wakes += !trusted_now;
        wake();
        /* A dose can only happen with a trusted, non-held clock. */
        if (n_doses > before) {
            DSP_CHECK(tk.trust != DSP_TRUST_UNKNOWN);
        }
    }
    DSP_CHECK_EQ_INT(0, duplicate_ids); /* never the same occurrence twice, in any case */
    printf("chaos(rtc_glitches=%d): doses=%d glitches=%d power_losses=%d unknown_wakes=%d "
           "same-true-day repeats=%d\n",
           rtc_glitches, n_doses, glitches, power_losses, unknown_wakes, duplicate_days);
    DSP_CHECK(n_doses > 100);
    DSP_CHECK(power_losses > 20);
    if (!rtc_glitches) {
        /* Within the trust assumption (RTC runs continuously between syncs, drift only):
         * never two automatic doses on the same true local day. */
        DSP_CHECK_EQ_INT(0, duplicate_days);
    } else {
        DSP_CHECK(glitches > 50);
    }
}

static void test_randomized_clock_chaos_realistic(void)
{
    clock_chaos(false);
}

static void test_randomized_clock_chaos_rtc_glitches(void)
{
    clock_chaos(true);
}

/* ---- Review: restart after a large forward correction (floor must survive) ---------- */

/* Device clock one day slow: at true D+1 08:00:30 it shows D 08:00:30 and doses identity D
 * (one dose on true day D+1). NTP at 08:03 then corrects forward by one day. */
static dsp_utc_t setup_slow_clock_dose(void)
{
    synced_world(24);
    rtc_offset = -DAY;
    sleep_until(T0 + DAY + 30);
    dsp_timekeeper_evaluate(&tk, reading());
    DSP_CHECK_EQ_INT(1, dose_step());
    return T0 + DAY; /* true D+1 08:00: the occurrence that must never run on true D+1 */
}

static int count_doses_for_true_day(int32_t day)
{
    int n = 0;
    for (int i = 0; i < n_doses; i++) {
        n += dose_real_days[i] == day;
    }
    return n;
}

static void correct_forward_at(dsp_utc_t t)
{
    sleep_until(t);
    dsp_timekeeper_evaluate(&tk, reading());
    dsp_time_sync_report_t r = dsp_time_service_run(&svc, true, false);
    DSP_CHECK_EQ_INT(DSP_CORRECTION_FORWARD_LARGE, r.decision.kind);
    DSP_CHECK(r.clock_set);
    DSP_CHECK_EQ_INT(t + 1, tk.meta.not_before_utc); /* persisted floor */
}

static void test_restart_after_large_forward_correction(void)
{
    /* (a) software restart at 08:04 (RAM lost; RTC, RTC memory and NVS kept) */
    dsp_utc_t occ = setup_slow_clock_dose();
    int32_t day = real_local_day();
    correct_forward_at(occ + 3 * 60);
    DSP_CHECK_EQ_INT(0, dose_step()); /* excluded by not_before */
    sleep_until(occ + 4 * 60);
    boot(false);
    DSP_CHECK_EQ_INT(0, wake());
    DSP_CHECK_EQ_INT(DSP_TRUST_SYNCED, tk.trust); /* trusted, still excluded */
    sleep_until(occ + 9 * 60); /* last minute of the due window */
    DSP_CHECK_EQ_INT(0, wake());
    DSP_CHECK_EQ_INT(1, count_doses_for_true_day(day));

    /* (b) deep-sleep wake at 08:04: same evidence (RTC + RTC memory), RAM rebuilt */
    occ = setup_slow_clock_dose();
    correct_forward_at(occ + 3 * 60);
    rtc_mem = tk.retained; /* saved before entering deep sleep */
    rtc_mem_valid = true;
    sleep_until(occ + 4 * 60);
    boot(false);
    DSP_CHECK_EQ_INT(0, wake());
    DSP_CHECK_EQ_INT(1, count_doses_for_true_day(day));

    /* (c) interrupted metadata persistence: the correction is NOT applied at all (durable
     * before effective). The clock stays one day slow; identity D is already claimed. */
    occ = setup_slow_clock_dose();
    dsp_utc_t floor_c = tk.meta.not_before_utc; /* from the initial sync, long ago */
    kv.fail_sets = true;
    sleep_until(occ + 3 * 60);
    dsp_timekeeper_evaluate(&tk, reading());
    dsp_time_sync_report_t r = dsp_time_service_run(&svc, true, false);
    DSP_CHECK_EQ_INT(DSP_SYNC_ERR_PERSIST, r.decision.status);
    DSP_CHECK(!r.clock_set);
    DSP_CHECK_EQ_INT(-DAY, rtc_offset); /* clock untouched */
    DSP_CHECK_EQ_INT(floor_c, tk.meta.not_before_utc); /* floor not raised (not applied) */
    DSP_CHECK_EQ_INT(0, dose_step());
    kv.fail_sets = false;
    sleep_until(occ + 4 * 60);
    boot(false);
    DSP_CHECK_EQ_INT(0, wake()); /* still slow: D claimed; retry not yet due */
    correct_forward_at(occ + 5 * 60); /* retry succeeds: floor durable from now on */
    DSP_CHECK_EQ_INT(0, dose_step());
    sleep_until(occ + 6 * 60);
    boot(false);
    DSP_CHECK_EQ_INT(0, wake());
    DSP_CHECK_EQ_INT(1, count_doses_for_true_day(day));

    /* (d) complete power loss at 08:04 after the correction: UNKNOWN (no dosing) until a
     * re-synchronization; the persisted floor still excludes the occurrence afterwards,
     * although the recovering sync is INITIAL (no correction measured). */
    occ = setup_slow_clock_dose();
    correct_forward_at(occ + 3 * 60);
    sleep_until(occ + 4 * 60);
    boot(true);
    dsp_timekeeper_evaluate(&tk, reading());
    DSP_CHECK_EQ_INT(DSP_TRUST_UNKNOWN, tk.trust);
    DSP_CHECK_EQ_INT(0, dose_step());
    sleep_until(occ + 5 * 60);
    DSP_CHECK_EQ_INT(0, wake()); /* INITIAL sync inside the due window */
    DSP_CHECK_EQ_INT(DSP_CORRECTION_INITIAL, last_report.decision.kind);
    DSP_CHECK_EQ_INT(DSP_TRUST_SYNCED, tk.trust);
    /* The persisted floor survived the power loss, and the initial sync raised it further
     * to max(floor, sync instant + 1 s). */
    DSP_CHECK_EQ_INT(occ + 5 * 60 + 1, tk.meta.not_before_utc);
    sleep_until(occ + 8 * 60);
    DSP_CHECK_EQ_INT(0, wake());
    DSP_CHECK_EQ_INT(1, count_doses_for_true_day(day));
    /* Next day: normal. */
    sleep_until(occ + DAY + 20);
    DSP_CHECK_EQ_INT(1, wake());
    DSP_CHECK_EQ_INT(0, duplicate_ids + duplicate_days);
}

static void test_floor_is_monotonic_and_deterministic(void)
{
    /* Repeated corrections: the floor only rises; small corrections never lower or set it. */
    synced_world(24);
    rtc_offset = -2 * H;
    correct_forward_at(T0 + 2 * H);
    dsp_utc_t f1 = tk.meta.not_before_utc;
    rtc_offset = 3 * 60;
    sleep_until(T0 + 3 * H);
    dsp_timekeeper_evaluate(&tk, reading());
    DSP_CHECK_EQ_INT(DSP_CORRECTION_SMALL, dsp_time_service_run(&svc, true, false).decision.kind);
    DSP_CHECK_EQ_INT(f1, tk.meta.not_before_utc);
    rtc_offset = 2 * H; /* large backward: floor kept (only rises) */
    sleep_until(T0 + 4 * H);
    dsp_timekeeper_evaluate(&tk, reading());
    DSP_CHECK_EQ_INT(DSP_CORRECTION_BACKWARD_LARGE, dsp_time_service_run(&svc, true, false).decision.kind);
    DSP_CHECK_EQ_INT(f1, tk.meta.not_before_utc);
    rtc_offset = -H;
    correct_forward_at(T0 + 5 * H);
    DSP_CHECK(tk.meta.not_before_utc > f1);

    /* Determinism: the same scripted sequence of corrections and restarts twice gives the
     * same doses and the same final metadata. */
    dsp_time_meta_t final_meta[2];
    int doses[2];
    char last_id[2][DSP_OCCURRENCE_ID_BUF_LEN];
    for (int run = 0; run < 2; run++) {
        synced_world(24);
        const int64_t offsets[] = {-DAY, 4 * 60, -40 * 60, 9 * DAY, -3 * H, 0, -DAY + 60};
        for (size_t i = 0; i < sizeof offsets / sizeof offsets[0]; i++) {
            rtc_offset += offsets[i];
            sleep_until(T0 + (dsp_utc_t)(i + 1) * DAY + 30);
            if (i == 2) {
                boot(true);
            }
            if (i == 4) {
                boot(false);
            }
            wake();
            sleep_until(T0 + (dsp_utc_t)(i + 1) * DAY + 4 * 60);
            dsp_timekeeper_evaluate(&tk, reading());
            dsp_time_service_run(&svc, true, false);
            dose_step();
        }
        final_meta[run] = tk.meta;
        doses[run] = n_doses;
        strcpy(last_id[run], n_doses > 0 ? dose_ids[n_doses - 1] : "");
        DSP_CHECK_EQ_INT(0, duplicate_ids + duplicate_days);
    }
    DSP_CHECK(dsp_time_meta_equal(&final_meta[0], &final_meta[1]));
    DSP_CHECK_EQ_INT(doses[0], doses[1]);
    DSP_CHECK_EQ_STR(last_id[0], last_id[1]);
}

static void test_unknown_clock_never_authorizes(void)
{
    /* Every UNKNOWN reason: the coordinator refuses with ERR_CLOCK, nothing is written. */
    reset_world();
    sleep_until(T0 + 30);
    const dsp_time_reading_t cases[] = {
        {T0 + 30, false},       /* never synced */
        {T0 + 30, true},        /* never synced, continuity claimed */
    };
    dsp_occurrence_t occ;
    DSP_CHECK_EQ_INT(DSP_REC_OK, dsp_rec_occurrence_on(&S, TZ, (dsp_date_t){2026, 11, 20}, &occ));
    dsp_exec_token_t tok;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        dsp_timekeeper_evaluate(&tk, cases[i]);
        DSP_CHECK_EQ_INT(DSP_TRUST_UNKNOWN, tk.trust);
        DSP_CHECK_EQ_INT(DSP_EXEC_ERR_CLOCK,
                         dsp_exec_claim(&ex, &S, &occ, dsp_timekeeper_clock(&tk, cases[i].now), &tok));
    }
    synced_world(24);
    const dsp_time_reading_t lost[] = {
        {T0 + 30, false},                  /* time base lost */
        {1000, true},                      /* out of range (power-on clock) */
        {tk.meta.last_sync_utc - 2 * H, true}, /* behind the last sync */
    };
    for (size_t i = 0; i < sizeof lost / sizeof lost[0]; i++) {
        dsp_timekeeper_evaluate(&tk, lost[i]);
        DSP_CHECK_EQ_INT(DSP_TRUST_UNKNOWN, tk.trust);
        dsp_clock_t c = dsp_timekeeper_clock(&tk, lost[i].now);
        DSP_CHECK(!c.valid);
        dsp_clock_t at_occ = dsp_timekeeper_clock(&tk, occ.at + 5); /* even at the right time */
        DSP_CHECK(!at_occ.valid);
        DSP_CHECK_EQ_INT(DSP_EXEC_ERR_CLOCK, dsp_exec_claim(&ex, &S, &occ, at_occ, &tok));
    }
    DSP_CHECK_EQ_INT(0, n_doses);
}

static void test_forward_correction_never_revives_missed(void)
{
    /* Occurrence missed by a late wake (08:15); a later forward correction and a restart
     * keep it missed; the next day runs normally. */
    synced_world(24);
    rtc_offset = -20 * 60; /* RTC 20 min slow */
    sleep_until(T0 + 35 * 60); /* device 08:15 -> missed */
    DSP_CHECK_EQ_INT(0, dose_step());
    correct_forward_at(T0 + 36 * 60); /* +20 min -> 08:36 */
    DSP_CHECK_EQ_INT(0, dose_step());
    boot(false);
    DSP_CHECK_EQ_INT(0, wake());
    boot(true);
    sleep_until(T0 + 37 * 60);
    DSP_CHECK_EQ_INT(0, wake());
    sleep_until(T0 + DAY + 20);
    DSP_CHECK_EQ_INT(1, wake());
    DSP_CHECK_EQ_INT(0, duplicate_ids + duplicate_days);
}

/* ---- Approved: floor raised at every initial synchronization ------------------------ */

/* Power loss, then a single forced sync at time t (the clock was untrusted before). */
static dsp_time_sync_report_t power_loss_then_sync_at(dsp_utc_t t)
{
    boot(true);
    sleep_until(t);
    dsp_timekeeper_evaluate(&tk, reading());
    DSP_CHECK_EQ_INT(DSP_TRUST_UNKNOWN, tk.trust);
    return dsp_time_service_run(&svc, true, false);
}

static void test_initial_sync_raises_floor(void)
{
    /* Initial synchronization BEFORE a scheduled dose: the dose runs normally. */
    reset_world();
    sleep_until(T0 - 10 * 60);
    dsp_timekeeper_evaluate(&tk, reading());
    dsp_time_sync_report_t r = dsp_time_service_run(&svc, true, false);
    DSP_CHECK_EQ_INT(DSP_CORRECTION_INITIAL, r.decision.kind);
    DSP_CHECK_EQ_INT(T0 - 10 * 60 + 1, tk.meta.not_before_utc);
    sleep_until(T0 + 5);
    DSP_CHECK_EQ_INT(1, wake());

    /* Initial synchronization immediately AFTER a scheduled dose time (never executed):
     * intentionally missed, also for the rest of the due window. */
    reset_world();
    sleep_until(T0 + 2 * 60);
    DSP_CHECK_EQ_INT(0, wake()); /* first-ever sync at 08:02 */
    DSP_CHECK_EQ_INT(DSP_TRUST_SYNCED, tk.trust);
    DSP_CHECK_EQ_INT(T0 + 2 * 60 + 1, tk.meta.not_before_utc);
    sleep_until(T0 + 9 * 60);
    DSP_CHECK_EQ_INT(0, wake());
    sleep_until(T0 + DAY + 1); /* next day normal */
    DSP_CHECK_EQ_INT(1, wake());

    /* Power loss right after a dose, then initial sync inside the window: no second dose
     * (history and floor both block). */
    synced_world(24);
    sleep_until(T0 + 10);
    DSP_CHECK_EQ_INT(1, wake());
    r = power_loss_then_sync_at(T0 + 3 * 60);
    DSP_CHECK_EQ_INT(DSP_CORRECTION_INITIAL, r.decision.kind);
    DSP_CHECK_EQ_INT(T0 + 3 * 60 + 1, tk.meta.not_before_utc);
    DSP_CHECK_EQ_INT(0, dose_step());
    DSP_CHECK_EQ_INT(0, duplicate_ids + duplicate_days);

    /* Existing HIGHER floor (left by a bogus far-forward answer) is never lowered. */
    synced_world(24);
    ntp_error = 3 * DAY;
    sleep_until(T0 - 30 * 60);
    dsp_timekeeper_evaluate(&tk, reading());
    r = dsp_time_service_run(&svc, true, false);
    DSP_CHECK_EQ_INT(DSP_CORRECTION_FORWARD_LARGE, r.decision.kind);
    dsp_utc_t high_floor = tk.meta.not_before_utc;
    DSP_CHECK(high_floor > T0 + 2 * DAY);
    ntp_error = 0;
    r = power_loss_then_sync_at(T0 - 20 * 60);
    DSP_CHECK_EQ_INT(DSP_SYNC_OK, r.decision.status);
    DSP_CHECK_EQ_INT(high_floor, tk.meta.not_before_utc); /* max(existing, sync + 1) */
    sleep_until(T0 + 10);
    DSP_CHECK_EQ_INT(0, wake()); /* still below the (higher) floor */

    /* Persistence failure during the initial sync: nothing applied, clock stays UNKNOWN,
     * floor unchanged, no dosing; the retry succeeds. */
    synced_world(24);
    dsp_utc_t floor0 = tk.meta.not_before_utc;
    kv.fail_sets = true;
    r = power_loss_then_sync_at(T0 - 5 * 60);
    DSP_CHECK_EQ_INT(DSP_SYNC_ERR_PERSIST, r.decision.status);
    DSP_CHECK(!r.clock_set);
    DSP_CHECK_EQ_INT(DSP_TRUST_UNKNOWN, tk.trust);
    DSP_CHECK_EQ_INT(floor0, tk.meta.not_before_utc);
    DSP_CHECK(!dsp_timekeeper_clock(&tk, device_now()).valid);
    dsp_time_meta_t stored;
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_load_time(&st, &stored));
    DSP_CHECK_EQ_INT(floor0, stored.not_before_utc);
    kv.fail_sets = false;
    sleep_until(T0 + 20); /* backoff passed? not yet (15 min): no sync, no dose */
    DSP_CHECK_EQ_INT(0, wake());
    DSP_CHECK_EQ_INT(DSP_TRUST_UNKNOWN, tk.trust);
    sleep_until(T0 + 11 * 60); /* retry succeeds at 08:11 (08:00 now outside the window) */
    DSP_CHECK_EQ_INT(0, wake());
    DSP_CHECK_EQ_INT(DSP_TRUST_SYNCED, tk.trust);
    DSP_CHECK_EQ_INT(T0 + 11 * 60 + 1, tk.meta.not_before_utc);

    /* Repeated initial synchronizations: the floor rises each time and an occurrence
     * skipped once never becomes eligible again. */
    synced_world(24);
    dsp_utc_t prev = tk.meta.not_before_utc;
    const dsp_utc_t times[] = {T0 + 60, T0 + 2 * 60, T0 + 4 * 60, T0 + 7 * 60};
    for (size_t i = 0; i < sizeof times / sizeof times[0]; i++) {
        r = power_loss_then_sync_at(times[i]);
        DSP_CHECK_EQ_INT(DSP_CORRECTION_INITIAL, r.decision.kind);
        DSP_CHECK(tk.meta.not_before_utc > prev);
        DSP_CHECK_EQ_INT(times[i] + 1, tk.meta.not_before_utc);
        prev = tk.meta.not_before_utc;
        DSP_CHECK_EQ_INT(0, dose_step()); /* 08:00 skipped, every time */
    }

    /* Restart after the floor has been persisted: excluded until the window closes. */
    reset_world();
    sleep_until(T0 + 60);
    DSP_CHECK_EQ_INT(0, wake()); /* initial sync at 08:01 */
    dsp_utc_t persisted = tk.meta.not_before_utc;
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_load_time(&st, &stored));
    DSP_CHECK_EQ_INT(persisted, stored.not_before_utc);
    sleep_until(T0 + 3 * 60);
    boot(false); /* software restart / deep-sleep wake */
    DSP_CHECK_EQ_INT(0, wake());
    DSP_CHECK_EQ_INT(DSP_TRUST_SYNCED, tk.trust);
    DSP_CHECK_EQ_INT(persisted, tk.meta.not_before_utc);
    sleep_until(T0 + 9 * 60);
    boot(false);
    DSP_CHECK_EQ_INT(0, wake());
    DSP_CHECK_EQ_INT(0, n_doses);
    DSP_CHECK_EQ_INT(0, duplicate_ids + duplicate_days);
}

int main(void)
{
    DSP_RUN(test_first_boot_without_valid_time);
    DSP_RUN(test_sync_interval_24h_and_48h);
    DSP_RUN(test_long_internet_outage_keeps_dosing);
    DSP_RUN(test_wifi_and_ntp_failures);
    DSP_RUN(test_deep_sleep_vs_power_loss);
    DSP_RUN(test_small_corrections);
    DSP_RUN(test_large_forward_jump_never_doses_twice);
    DSP_RUN(test_large_backward_jump_within_and_beyond_7_days);
    DSP_RUN(test_hold_lifted_by_later_forward_sync);
    DSP_RUN(test_dst_and_restarts_around_dose);
    DSP_RUN(test_storage_errors_do_not_stop_dosing);
    DSP_RUN(test_wifi_manager_lifecycle);
    DSP_RUN(test_wake_plan);
    DSP_RUN(test_restart_after_large_forward_correction);
    DSP_RUN(test_floor_is_monotonic_and_deterministic);
    DSP_RUN(test_unknown_clock_never_authorizes);
    DSP_RUN(test_forward_correction_never_revives_missed);
    DSP_RUN(test_initial_sync_raises_floor);
    DSP_RUN(test_randomized_clock_chaos_realistic);
    DSP_RUN(test_randomized_clock_chaos_rtc_glitches);
    return dsp_test_finish();
}
