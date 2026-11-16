/*
 * M1.8 integration harness: the REAL firmware domain code (M1.5 engine, M1.3 storage over
 * the fake KV with power-cut injection, M1.7 protocol/HTTPS transport/connectivity session)
 * driven over stdin by notify_e2e.mjs, talking real TLS (posix_https_port.c) to a
 * fault-injection proxy in front of a local `wrangler dev` Worker with local D1.
 *
 * Protocol: one command per line on stdin, exactly one JSON line per command on stdout.
 * Test infrastructure only; synthetic credentials only.
 */
#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dsp_connectivity.h"
#include "dsp_https_transport.h"
#include "dsp_notify.h"
#include "dsp_notify_proto.h"
#include "dsp_storage.h"
#include "dsp_time_service.h"
#include "fake_kv.h"
#include "posix_https_port.h"

#define T0 ((dsp_utc_t)1795158000) /* 2026-11-20T07:00:00Z */

/* ---- JSON output -------------------------------------------------------------------- */

static char out[65536];
static size_t out_len;

static void emit(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(out + out_len, sizeof out - out_len, fmt, ap);
    va_end(ap);
    if (w > 0) {
        out_len += (size_t)w < sizeof out - out_len ? (size_t)w : sizeof out - out_len - 1;
    }
}

static void emit_str(const char *s, size_t len)
{
    emit("\"");
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            emit("\\%c", c);
        } else if (c < 0x20 || c > 0x7E) {
            emit("\\u%04x", c);
        } else {
            emit("%c", c);
        }
    }
    emit("\"");
}

static void flush_line(void)
{
    fputs(out, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    out_len = 0;
    out[0] = '\0';
}

/* ---- World -------------------------------------------------------------------------- */

static fake_kv_t kv;
static dsp_storage_t st;
static dsp_notify_t n;
static uint32_t epoch_random = 0xA5A5A5A5u;
static uint64_t device_id = 0x0123456789ABull;

static posix_port_t port;
static dsp_http_port_t port_iface;
static char token_value[DSP_API_TOKEN_MAX_LEN + 1] = "";
static bool token_present;
static dsp_https_transport_t https;
static uint32_t request_worst_ms = 3000;

static uint64_t mono_now(void *ctx)
{
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
static const dsp_monotonic_t MONO = {NULL, mono_now};

static dsp_cred_status_t token_get(void *ctx, char *buf, size_t cap)
{
    (void)ctx;
    if (!token_present) {
        return DSP_CRED_UNAVAILABLE;
    }
    snprintf(buf, cap, "%s", token_value);
    return DSP_CRED_OK;
}
static const dsp_token_provider_t TOKEN = {NULL, token_get};

/* Fake clock, NTP and Wi-Fi for the connectivity session. */
static dsp_utc_t device_clock = T0;
static dsp_utc_t true_time = T0;
static bool ntp_ok = true;
static bool wifi_ok = true;
static int wifi_starts, wifi_stops;

static dsp_utc_t clock_now(void *ctx)
{
    (void)ctx;
    return device_clock;
}
static bool clock_set(void *ctx, dsp_utc_t utc)
{
    (void)ctx;
    device_clock = utc;
    return true;
}
static void clock_mark(void *ctx, dsp_utc_t last)
{
    (void)ctx;
    (void)last;
}
static const dsp_clock_port_t CLOCK = {NULL, clock_now, clock_set, clock_mark};
static dsp_sync_status_t ntp_sync(void *ctx, uint32_t timeout_ms, dsp_time_sample_t *s)
{
    (void)ctx;
    (void)timeout_ms;
    if (!ntp_ok) {
        return DSP_SYNC_ERR_TIMEOUT;
    }
    s->source_utc = true_time;
    s->clock_utc = device_clock;
    return DSP_SYNC_OK;
}
static const dsp_time_source_t NTP = {NULL, ntp_sync};
static dsp_net_status_t wf_start(void *ctx)
{
    (void)ctx;
    wifi_starts++;
    return DSP_NET_OK;
}
static dsp_net_status_t wf_connect(void *ctx, const dsp_wifi_credentials_t *c, uint32_t t)
{
    (void)ctx;
    (void)c;
    (void)t;
    return wifi_ok ? DSP_NET_OK : DSP_NET_ERR_NO_AP;
}
static void wf_disconnect(void *ctx)
{
    (void)ctx;
}
static void wf_stop(void *ctx)
{
    (void)ctx;
    wifi_stops++;
}
static const dsp_wifi_driver_t WIFI = {NULL, wf_start, wf_connect, wf_disconnect, wf_stop};
static dsp_cred_status_t creds_get(void *ctx, dsp_wifi_credentials_t *o)
{
    (void)ctx;
    snprintf(o->ssid, sizeof o->ssid, "e2e-ssid");
    snprintf(o->passphrase, sizeof o->passphrase, "e2e-passphrase");
    return DSP_CRED_OK;
}
static const dsp_wifi_credentials_provider_t CREDS = {NULL, creds_get};

static dsp_timekeeper_t tk;
static dsp_wifi_manager_t wifi;
static dsp_time_service_t svc;
static dsp_connectivity_retained_t retained;
static dsp_connectivity_t conn;
static uint32_t https_budget_ms = 0;
static bool booted;
static dsp_storage_status_t last_storage_status;

static bool open_all(void)
{
    last_storage_status = dsp_storage_open(&st, &kv.iface, &DSP_TZ_EUROPE_PRAGUE, true);
    if (last_storage_status != DSP_STORAGE_OK) {
        return false;
    }
    if (dsp_notify_init(&n, &st, epoch_random) != DSP_NOTIFY_OK) {
        return false;
    }
    dsp_time_meta_t meta;
    if (dsp_storage_load_time(&st, &meta) != DSP_STORAGE_OK) {
        return false;
    }
    if (!dsp_timekeeper_init(&tk, &meta, 24, NULL)) { /* power-on: RTC state lost, UNKNOWN */
        return false;
    }
    dsp_wifi_config_t cfg = {DSP_WIFI_TIMEOUT_DEFAULT_MS, DSP_WIFI_ATTEMPTS_DEFAULT};
    if (dsp_wifi_init(&wifi, &WIFI, &CREDS, &cfg) != DSP_NET_OK) {
        return false;
    }
    dsp_time_service_t s = {&tk, &wifi, &NTP, &CLOCK, &st, 0};
    svc = s;
    memset(&retained, 0, sizeof retained);
    port_iface = posix_port_iface(&port);
    if (!dsp_https_init(&https, &port_iface, &TOKEN, &MONO, device_id, request_worst_ms)) {
        return false;
    }
    dsp_connectivity_t c = {&svc, &n, &https, &retained, https_budget_ms};
    conn = c;
    booted = true;
    return true;
}

/* ---- Reporting ---------------------------------------------------------------------- */

static const char *phase_name(int p)
{
    static const char *names[] = {"not_started", "connect_failed", "tls_failed",
                                  "send_failed", "no_response",    "response"};
    return p >= 0 && p <= 5 ? names[p] : "?";
}

static const char *result_name(dsp_tx_result_t r)
{
    switch (r) {
    case DSP_TX_ACK:
        return "ACK";
    case DSP_TX_REJECTED:
        return "REJECTED";
    case DSP_TX_UNCERTAIN:
        return "UNCERTAIN";
    default:
        return "FAILED";
    }
}

/* Exchanges logged by the port since the last command, classified like the transport. */
static void emit_exchanges(void)
{
    emit("\"exchanges\":[");
    for (int i = 0; i < port.log_len; i++) {
        const posix_port_log_t *l = &port.log[i];
        char id[DSP_NOTIFY_ID_BUF_LEN] = "";
        const char *p = strstr(l->body, "\"event_id\":\"");
        if (p != NULL) {
            p += 12;
            size_t k = 0;
            while (p[k] != '"' && p[k] != '\0' && k + 1 < sizeof id) {
                id[k] = p[k];
                k++;
            }
            id[k] = '\0';
        }
        dsp_tx_detail_t d;
        dsp_tx_result_t r = dsp_proto_classify(&l->outcome, id, &d);
        emit("%s{\"event_id\":", i ? "," : "");
        emit_str(id, strlen(id));
        emit(",\"body\":");
        emit_str(l->body, strlen(l->body));
        emit(",\"phase\":\"%s\",\"status\":%u,\"response\":", phase_name(l->outcome.phase),
             (unsigned)l->outcome.status);
        emit_str(l->outcome.body, l->outcome.body_len);
        emit(",\"result\":\"%s\",\"diag\":%u,\"error\":%u,\"field\":%u}", result_name(r),
             (unsigned)d.diag, (unsigned)d.error, (unsigned)d.field);
    }
    emit("]");
    port.log_len = 0;
}

static void emit_state(void)
{
    emit("\"pending\":%zu,\"queue\":[", dsp_notify_pending_count(&n));
    int k = 0;
    for (int i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY; i++) {
        const dsp_note_t *e = &n.s.queue[i];
        if (e->seq == 0) {
            continue;
        }
        char id[DSP_NOTIFY_ID_BUF_LEN];
        dsp_notify_format_id(device_id, n.s.epoch, e->seq, id, sizeof id);
        emit("%s{\"seq\":%lu,\"event_id\":\"%s\",\"type\":%u,\"severity\":%u,\"code\":%u,"
             "\"attempts\":%u,\"value\":%lu}",
             k++ ? "," : "", (unsigned long)e->seq, id, (unsigned)e->type, (unsigned)e->severity,
             (unsigned)e->code, (unsigned)e->attempts, (unsigned long)e->value);
    }
    const dsp_rejection_log_t *log = &n.s.rejections;
    emit("],\"rejections\":{\"total\":%lu,\"lost_unread\":%u,\"unread\":%u,\"count\":%u,"
         "\"entries\":[",
         (unsigned long)log->total, (unsigned)log->lost_unread, (unsigned)log->unread,
         (unsigned)log->count);
    for (int i = 0; i < log->count; i++) {
        const dsp_rejection_t *r = &log->entry[i];
        char id[DSP_NOTIFY_ID_BUF_LEN];
        dsp_notify_format_id(device_id, n.s.epoch, r->seq, id, sizeof id);
        emit("%s{\"seq\":%lu,\"event_id\":\"%s\",\"http_status\":%u,\"error\":%u,\"field\":%u,"
             "\"attempts\":%u}",
             i ? "," : "", (unsigned long)r->seq, id, (unsigned)r->http_status, (unsigned)r->error,
             (unsigned)r->field, (unsigned)r->attempts);
    }
    uint16_t dstatus = 0;
    uint8_t diag = dsp_notify_tx_diag(&n, &dstatus);
    /* Durable state equals RAM unless a write is outstanding. */
    dsp_notify_state_t stored;
    bool load_ok = dsp_storage_load_notify(&st, &stored) == DSP_STORAGE_OK;
    uint8_t a[DSP_NOTIFY_RECORD_BYTES];
    uint8_t b[DSP_NOTIFY_RECORD_BYTES];
    bool in_sync = load_ok && !n.dirty &&
                   dsp_codec_encode_notify(&stored, a, sizeof a) == DSP_NOTIFY_RECORD_BYTES &&
                   dsp_codec_encode_notify(&n.s, b, sizeof b) == DSP_NOTIFY_RECORD_BYTES &&
                   memcmp(a, b, sizeof a) == 0;
    emit("]},\"diag\":%u,\"diag_status\":%u,\"dirty\":%s,\"valid\":%s,\"in_sync\":%s,"
         "\"epoch\":\"%08lx\",\"next_seq\":%lu,\"dropped\":%u,\"trust\":%d,\"failures\":%u,"
         "\"last_notify_attempt\":%lld,\"clock\":%lld,\"wifi_starts\":%d,\"wifi_stops\":%d,"
         "\"wifi_state\":%d,\"posts_total\":%d",
         (unsigned)diag, (unsigned)dstatus, n.dirty ? "true" : "false",
         dsp_notify_state_is_valid(&n.s) ? "true" : "false", in_sync ? "true" : "false",
         (unsigned long)n.s.epoch, (unsigned long)n.s.next_seq, (unsigned)n.s.dropped,
         (int)tk.trust, (unsigned)tk.retained.consecutive_failures,
         (long long)retained.last_notify_attempt, (long long)device_clock, wifi_starts, wifi_stops,
         (int)dsp_wifi_state(&wifi), port.posts_total);
}

static const char *conn_outcome(dsp_conn_outcome_t o)
{
    static const char *names[] = {"ERR_ARG",     "SKIPPED_GUARD", "SKIPPED_DOSE_SOON",
                                  "SKIPPED_NOT_DUE", "WIFI_FAILED", "TIME_UNTRUSTED",
                                  "NO_TOKEN",    "PORT_FAILED",   "DONE"};
    return (int)o >= 0 && (int)o <= 8 ? names[o] : "?";
}

/* ---- Commands ----------------------------------------------------------------------- */

static void reply_error(const char *msg)
{
    emit("{\"ok\":false,\"error\":");
    emit_str(msg, strlen(msg));
    emit("}");
}

static dsp_notify_clock_t nclock(void)
{
    dsp_notify_clock_t c = {device_clock, true};
    return c;
}

static void cmd(char *line)
{
    char *argv[16];
    int argc = 0;
    for (char *tok = strtok(line, " \t\r\n"); tok != NULL && argc < 16; tok = strtok(NULL, " \t\r\n")) {
        argv[argc++] = tok;
    }
    if (argc == 0) {
        reply_error("empty");
        return;
    }
    const char *c = argv[0];
    if (strcmp(c, "init") == 0 && argc >= 2) {
        /* init <durable_on_set 0|1> [epoch_hex]: fresh flash (or a storage reset). */
        fake_kv_init(&kv, atoi(argv[1]) != 0);
        if (argc >= 3) {
            epoch_random = (uint32_t)strtoul(argv[2], NULL, 16);
        }
        device_clock = true_time = T0;
        if (!open_all()) {
            reply_error("init failed");
            return;
        }
        emit("{\"ok\":true,");
        emit_state();
        emit("}");
        return;
    }
    if (strcmp(c, "boot") == 0) {
        /* Power loss: RAM and RTC state lost, flash kept. */
        fake_kv_power_cycle(&kv);
        fake_kv_clear_faults(&kv);
        if (!open_all()) {
            reply_error("boot failed");
            return;
        }
        emit("{\"ok\":true,");
        emit_state();
        emit("}");
        return;
    }
    if (!booted) {
        reply_error("not initialized");
        return;
    }
    if (strcmp(c, "target") == 0 && argc >= 4) {
        posix_port_set_target(&port, argv[1], (uint16_t)atoi(argv[2]), argv[3]);
        emit("{\"ok\":true}");
    } else if (strcmp(c, "token") == 0 && argc >= 2) {
        token_present = strcmp(argv[1], "-") != 0;
        snprintf(token_value, sizeof token_value, "%s", token_present ? argv[1] : "");
        emit("{\"ok\":true}");
    } else if (strcmp(c, "timeouts") == 0 && argc >= 4) {
        /* timeouts <socket_ms> <request_worst_ms> <https_budget_ms> */
        port.timeout_ms = (uint32_t)atoi(argv[1]);
        request_worst_ms = (uint32_t)atoi(argv[2]);
        https_budget_ms = (uint32_t)atoi(argv[3]);
        https.request_worst_ms = request_worst_ms;
        conn.https_budget_ms = https_budget_ms;
        emit("{\"ok\":true}");
    } else if (strcmp(c, "dosing") == 0 && argc >= 2) {
        dsp_dosing_error_t e = {(uint16_t)atoi(argv[1]), argc >= 3 && strcmp(argv[2], "manual") == 0,
                                (uint32_t)(argc >= 4 ? atoi(argv[3]) : 1000)};
        uint32_t before = n.s.next_seq;
        dsp_notify_status_t s = dsp_notify_report_dosing_error(&n, &e, nclock());
        emit("{\"ok\":true,\"status\":%d,\"seq\":%lu,", (int)s,
             (unsigned long)(n.s.next_seq != before ? n.s.next_seq - 1 : 0));
        emit_state();
        emit("}");
    } else if (strcmp(c, "reservoir") == 0 && argc >= 3) {
        dsp_config_t cfg;
        dsp_config_defaults(&cfg);
        cfg.reservoir_capacity_ul = (uint32_t)atoi(argv[2]);
        cfg.reservoir_remaining_ul = (uint32_t)atoi(argv[1]);
        dsp_notify_status_t s = dsp_notify_observe_reservoir(&n, &cfg, false, nclock());
        emit("{\"ok\":true,\"status\":%d,", (int)s);
        emit_state();
        emit("}");
    } else if (strcmp(c, "tick") == 0) {
        dsp_notify_status_t s = dsp_notify_tick(&n, nclock());
        emit("{\"ok\":true,\"status\":%d,", (int)s);
        emit_state();
        emit("}");
    } else if (strcmp(c, "cut") == 0 && argc >= 2) {
        kv.power_cut_after_sets = atoi(argv[1]);
        emit("{\"ok\":true}");
    } else if (strcmp(c, "deliver") == 0) {
        /* deliver [budget_ms]: an already open session (bypasses the orchestrator). */
        uint32_t budget = argc >= 2 ? (uint32_t)atoi(argv[1]) : 60000u;
        port.log_len = 0;
        dsp_notify_delivery_report_t r;
        memset(&r, 0, sizeof r);
        bool begun = dsp_https_begin(&https, budget);
        if (begun) {
            dsp_notify_transport_t tx = dsp_https_transport(&https);
            r = dsp_notify_deliver(&n, &tx, DSP_NOTIFY_QUEUE_CAPACITY, nclock());
            dsp_https_end(&https);
        }
        emit("{\"ok\":%s,\"report\":{\"status\":%d,\"attempted\":%zu,\"acked\":%zu,"
             "\"rejected\":%zu,\"stopped_early\":%s,\"budget_exhausted\":%s},",
             begun ? "true" : "false", (int)r.status, r.attempted, r.acked, r.rejected,
             r.stopped_early ? "true" : "false", r.budget_exhausted ? "true" : "false");
        emit_exchanges();
        emit(",");
        emit_state();
        emit("}");
    } else if (strcmp(c, "session") == 0 && argc >= 5) {
        /* session <dosing 0|1> <permit 0|1> <ble 0|1> <next_dose_in_s|-1> */
        dsp_connectivity_guard_t g = {atoi(argv[1]) != 0, atoi(argv[2]) != 0, atoi(argv[3]) != 0,
                                      atoi(argv[4]) >= 0,
                                      device_clock + (dsp_utc_t)(atoi(argv[4]) >= 0 ? atoi(argv[4]) : 0)};
        port.log_len = 0;
        dsp_connectivity_report_t r = dsp_connectivity_run(&conn, &g);
        emit("{\"ok\":true,\"outcome\":\"%s\",\"reason\":%d,\"delivery_ran\":%s,"
             "\"report\":{\"status\":%d,\"attempted\":%zu,\"acked\":%zu,\"rejected\":%zu,"
             "\"budget_exhausted\":%s},",
             conn_outcome(r.outcome), (int)r.reason, r.delivery_ran ? "true" : "false",
             (int)r.delivery.status, r.delivery.attempted, r.delivery.acked, r.delivery.rejected,
             r.delivery.budget_exhausted ? "true" : "false");
        emit_exchanges();
        emit(",");
        emit_state();
        emit("}");
    } else if (strcmp(c, "wifi") == 0 && argc >= 2) {
        wifi_ok = strcmp(argv[1], "ok") == 0;
        emit("{\"ok\":true}");
    } else if (strcmp(c, "ntp") == 0 && argc >= 2) {
        ntp_ok = strcmp(argv[1], "ok") == 0;
        emit("{\"ok\":true}");
    } else if (strcmp(c, "advance") == 0 && argc >= 2) {
        dsp_utc_t s = (dsp_utc_t)atoll(argv[1]);
        device_clock += s;
        true_time += s;
        emit("{\"ok\":true,\"clock\":%lld,\"sync_due\":%s}", (long long)device_clock,
             dsp_timekeeper_sync_due(&tk, device_clock) ? "true" : "false");
    } else if (strcmp(c, "ack_rejections") == 0) {
        dsp_notify_status_t s = dsp_notify_ack_rejections(&n);
        emit("{\"ok\":true,\"status\":%d,", (int)s);
        emit_state();
        emit("}");
    } else if (strcmp(c, "state") == 0) {
        emit("{\"ok\":true,");
        emit_state();
        emit("}");
    } else {
        reply_error("unknown command");
    }
}

int main(int argc, char **argv)
{
    /* A peer reset while writing must surface as a failed write (SEND_FAILED), not kill
     * the process (POSIX default for SIGPIPE; the ESP-IDF/lwIP target has no SIGPIPE). */
    signal(SIGPIPE, SIG_IGN);
    if (argc < 2 || !posix_port_init(&port, argv[1])) {
        fprintf(stderr, "usage: notify_harness <ca.pem>\n");
        return 2;
    }
    static char line[4096];
    while (fgets(line, sizeof line, stdin) != NULL) {
        if (strncmp(line, "quit", 4) == 0) {
            break;
        }
        out_len = 0;
        out[0] = '\0';
        cmd(line);
        flush_line();
    }
    posix_port_free(&port);
    return 0;
}
