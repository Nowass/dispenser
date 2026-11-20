/*
 * HTTPS notification transport and connectivity session tests (M1.7), over fakes only:
 * a scripted HTTP port, token provider and monotonic clock; fake Wi-Fi, SNTP and clock
 * port; the real M1.3 storage over the in-memory fake KV and the real M1.5 engine.
 * No network, no real credentials (generated dummy tokens only).
 */
#include "dsp_connectivity.h"
#include "dsp_https_transport.h"
#include "dsp_notify.h"
#include "dsp_notify_proto.h"
#include "dsp_storage.h"
#include "dsp_test.h"
#include "dsp_time_service.h"
#include "fake_kv.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define T0 ((dsp_utc_t)1795158000) /* 2026-11-20T07:00:00Z */
#define DEVICE 0x0123456789ABull
#define EPOCH 0xA5A5A5A5u
#define H 3600

/* Dummy token generated for the tests (not a credential). */
static const char TOKEN[] = "test-0000000000000000000000000000000000000000000000000000000000ff";

/* ---- Fake HTTP port ----------------------------------------------------------------- */

#define MAX_POSTS 128
static struct {
    dsp_http_outcome_t script[MAX_POSTS];
    int script_len;
    dsp_http_outcome_t fallback;
    bool auto_ack; /* answer every post with the exact 201 confirmation */
    bool open_fails;
    int opens, closes, posts;
    char bodies[MAX_POSTS][DSP_PROTO_BODY_MAX];
    char tokens[MAX_POSTS][DSP_API_TOKEN_MAX_LEN + 1];
    uint64_t advance_ms; /* monotonic time consumed per post */
    bool in_session;
} port;

static uint64_t mono_ms;
static uint64_t mono_now(void *ctx)
{
    (void)ctx;
    return mono_ms;
}
static const dsp_monotonic_t MONO = {NULL, mono_now};

static bool port_open(void *ctx)
{
    (void)ctx;
    port.opens++;
    if (port.open_fails) {
        return false;
    }
    port.in_session = true;
    return true;
}

static void extract_id(const char *body, char *id, size_t cap)
{
    const char *p = strstr(body, "\"event_id\":\"");
    id[0] = '\0';
    if (p == NULL) {
        return;
    }
    p += strlen("\"event_id\":\"");
    size_t n = 0;
    while (p[n] != '"' && p[n] != '\0' && n + 1 < cap) {
        id[n] = p[n];
        n++;
    }
    id[n] = '\0';
}

static dsp_http_outcome_t response(uint16_t status, const char *body)
{
    dsp_http_outcome_t o;
    memset(&o, 0, sizeof o);
    o.phase = DSP_HTTP_RESPONSE;
    o.status = status;
    size_t len = strlen(body);
    memcpy(o.body, body, len);
    o.body_len = (uint16_t)len;
    return o;
}

static dsp_http_outcome_t phase_only(dsp_http_phase_t phase)
{
    dsp_http_outcome_t o;
    memset(&o, 0, sizeof o);
    o.phase = phase;
    return o;
}

static void port_post(void *ctx, const char *body, size_t len, const char *token,
                      dsp_http_outcome_t *out)
{
    (void)ctx;
    DSP_CHECK(port.in_session);
    int i = port.posts++;
    if (i < MAX_POSTS) {
        DSP_CHECK(len < DSP_PROTO_BODY_MAX);
        memcpy(port.bodies[i], body, len);
        port.bodies[i][len] = '\0';
        snprintf(port.tokens[i], sizeof port.tokens[i], "%s", token);
    }
    DSP_CHECK(strstr(body, token) == NULL); /* the token never travels in the body */
    mono_ms += port.advance_ms;
    if (port.auto_ack) {
        char id[DSP_NOTIFY_ID_BUF_LEN];
        char b[128];
        extract_id(i < MAX_POSTS ? port.bodies[i] : body, id, sizeof id);
        snprintf(b, sizeof b, "{\"status\":\"accepted\",\"event_id\":\"%s\"}", id);
        *out = response(201, b);
        return;
    }
    *out = i < port.script_len ? port.script[i] : port.fallback;
}

static void port_close(void *ctx)
{
    (void)ctx;
    port.closes++;
    port.in_session = false;
}

static const dsp_http_port_t PORT = {NULL, port_open, port_post, port_close};

static void port_reset(void)
{
    memset(&port, 0, sizeof port);
    port.fallback = phase_only(DSP_HTTP_CONNECT_FAILED);
}

/* ---- Fake token provider ------------------------------------------------------------ */

static const char *token_value = TOKEN;
static dsp_cred_status_t token_status = DSP_CRED_OK;
static int token_gets;
static dsp_cred_status_t token_get(void *ctx, char *buf, size_t cap)
{
    (void)ctx;
    token_gets++;
    if (token_status == DSP_CRED_OK) {
        snprintf(buf, cap, "%s", token_value);
    }
    return token_status;
}
static const dsp_token_provider_t TOKENS = {NULL, token_get};

/* ---- Storage, engine, transport ----------------------------------------------------- */

static fake_kv_t kv;
static dsp_storage_t st;
static dsp_notify_t n;
static dsp_https_transport_t https;

static void boot(void)
{
    fake_kv_power_cycle(&kv);
    fake_kv_clear_faults(&kv);
    DSP_CHECK_EQ_INT(DSP_STORAGE_OK, dsp_storage_open(&st, &kv.iface, &DSP_TZ_EUROPE_PRAGUE, true));
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_init(&n, &st, EPOCH));
}

static void fresh(void)
{
    fake_kv_init(&kv, true);
    boot();
    port_reset();
    mono_ms = 1000;
    token_value = TOKEN;
    token_status = DSP_CRED_OK;
    token_gets = 0;
    DSP_CHECK(dsp_https_init(&https, &PORT, &TOKENS, &MONO, DEVICE, 0));
}

static dsp_notify_clock_t at(dsp_utc_t t)
{
    dsp_notify_clock_t c = {t, true};
    return c;
}

static void add_dosing_errors(int count)
{
    for (int i = 0; i < count; i++) {
        dsp_dosing_error_t e = {DSP_DOSEERR_PUMP_FAILED, false, (uint32_t)(500 + i)};
        DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_report_dosing_error(&n, &e, at(T0)));
    }
}

static void add_reservoir_low(void)
{
    dsp_config_t c;
    dsp_config_defaults(&c);
    c.reservoir_capacity_ul = 100000;
    c.reservoir_remaining_ul = 5000;
    DSP_CHECK_EQ_INT(DSP_NOTIFY_OK, dsp_notify_observe_reservoir(&n, &c, false, at(T0)));
}

static dsp_notify_delivery_report_t session(uint32_t budget_ms)
{
    dsp_notify_delivery_report_t r;
    memset(&r, 0, sizeof r);
    DSP_CHECK(dsp_https_begin(&https, budget_ms));
    dsp_notify_transport_t tx = dsp_https_transport(&https);
    r = dsp_notify_deliver(&n, &tx, DSP_NOTIFY_QUEUE_CAPACITY, at(T0));
    dsp_https_end(&https);
    return r;
}

static bool all_zero(const void *p, size_t len)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < len; i++) {
        if (b[i] != 0) {
            return false;
        }
    }
    return true;
}

static void id_for(uint32_t seq, char *buf)
{
    DSP_CHECK(dsp_notify_format_id(DEVICE, EPOCH, seq, buf, DSP_NOTIFY_ID_BUF_LEN));
}

/* Body without its "attempt" member: identical across retries (frozen payload). */
static void strip_attempt(const char *body, char *out, size_t cap)
{
    snprintf(out, cap, "%s", body);
    char *p = strstr(out, ",\"attempt\":");
    if (p != NULL) {
        *p = '\0';
    }
}

/* ---- Transport ------------------------------------------------------------------------ */

static void test_ack_body_and_token_handling(void)
{
    fresh();
    add_dosing_errors(2);
    port.auto_ack = true;
    dsp_notify_delivery_report_t r = session(60000);
    DSP_CHECK_EQ_INT(2, r.acked);
    DSP_CHECK_EQ_INT(0, dsp_notify_pending_count(&n));
    DSP_CHECK_EQ_INT(2, port.posts);
    DSP_CHECK_EQ_INT(1, port.opens);
    DSP_CHECK_EQ_INT(1, port.closes);
    /* The exact protocol body, and the provisioned token handed to the port. */
    dsp_note_t e;
    memset(&e, 0, sizeof e);
    e.seq = 1;
    e.type = DSP_NOTE_DOSING_ERROR;
    e.severity = DSP_SEV_CRITICAL;
    e.condition = DSP_COND_NONE;
    e.code = DSP_DOSEERR_PUMP_FAILED;
    e.flags = DSP_NOTE_F_TIME_VALID;
    e.created_at = T0;
    e.value = 500;
    e.attempts = 1;
    char expected[DSP_PROTO_BODY_MAX];
    DSP_CHECK(dsp_proto_encode_event(DEVICE, EPOCH, &e, expected, sizeof expected) > 0);
    DSP_CHECK_EQ_STR(expected, port.bodies[0]);
    DSP_CHECK_EQ_STR(TOKEN, port.tokens[0]);
    /* The token buffer is wiped after every request and at the end of the session. */
    DSP_CHECK(all_zero(https.secret, sizeof https.secret));
    DSP_CHECK_EQ_INT(0, dsp_notify_tx_diag(&n, NULL));
}

static void test_token_validation(void)
{
    fresh();
    DSP_CHECK(dsp_https_ready(&https));
    DSP_CHECK(all_zero(https.secret, sizeof https.secret));
    token_status = DSP_CRED_UNAVAILABLE;
    DSP_CHECK(!dsp_https_ready(&https));
    token_status = DSP_CRED_ERR;
    DSP_CHECK(!dsp_https_ready(&https));
    token_status = DSP_CRED_OK;
    char t[DSP_API_TOKEN_MAX_LEN + 8];
    memset(t, 'a', sizeof t);
    t[DSP_API_TOKEN_MIN_LEN - 1] = '\0'; /* 31 chars */
    DSP_CHECK(!dsp_https_token_valid(t));
    t[DSP_API_TOKEN_MIN_LEN - 1] = 'a';
    t[DSP_API_TOKEN_MIN_LEN] = '\0';
    DSP_CHECK(dsp_https_token_valid(t));
    t[5] = ' ';
    DSP_CHECK(!dsp_https_token_valid(t)); /* no spaces */
    t[5] = '\n';
    DSP_CHECK(!dsp_https_token_valid(t)); /* no control characters (header injection) */
    t[5] = 'a';
    t[DSP_API_TOKEN_MIN_LEN] = 'a';
    t[DSP_API_TOKEN_MAX_LEN] = '\0';
    DSP_CHECK(dsp_https_token_valid(t)); /* 512 */
    t[DSP_API_TOKEN_MAX_LEN] = 'a';
    t[DSP_API_TOKEN_MAX_LEN + 1] = '\0';
    DSP_CHECK(!dsp_https_token_valid(t)); /* 513 */
    DSP_CHECK(!dsp_https_token_valid(NULL));
    /* A token that disappears mid-session: nothing is sent, the event is kept. */
    add_dosing_errors(1);
    token_value = "short";
    dsp_notify_delivery_report_t r = session(60000);
    DSP_CHECK_EQ_INT(0, port.posts);
    DSP_CHECK(r.stopped_early);
    DSP_CHECK_EQ_INT(1, dsp_notify_pending_count(&n));
    DSP_CHECK_EQ_INT(DSP_TXDIAG_AUTH, dsp_notify_tx_diag(&n, NULL));
    DSP_CHECK(all_zero(https.secret, sizeof https.secret));
    /* Invalid construction. */
    dsp_https_transport_t bad;
    DSP_CHECK(!dsp_https_init(&bad, NULL, &TOKENS, &MONO, DEVICE, 0));
    DSP_CHECK(!dsp_https_init(&bad, &PORT, &TOKENS, &MONO, 1ull << 48, 0)); /* not a MAC */
    DSP_CHECK(!dsp_https_begin(&bad, 1000));
}

/* R1 regression: a systemic problem answered for EVERY request can never drain the
 * queue, no matter how many sessions run. */
static void test_systemic_statuses_never_drain_queue(void)
{
    struct {
        uint16_t status;
        const char *body;
        uint8_t diag;
    } cases[] = {
        {401, "{\"status\":\"unauthorized\"}", DSP_TXDIAG_AUTH},
        {404, "{\"status\":\"invalid\",\"error\":\"not_found\"}", DSP_TXDIAG_ENDPOINT},
        {405, "{\"status\":\"invalid\",\"error\":\"method_not_allowed\"}", DSP_TXDIAG_ENDPOINT},
        {413, "{\"status\":\"invalid\",\"error\":\"payload_too_large\"}", DSP_TXDIAG_PROTOCOL},
        {415, "{\"status\":\"invalid\",\"error\":\"unsupported_media_type\"}", DSP_TXDIAG_PROTOCOL},
        {400, "{\"status\":\"invalid\",\"error\":\"malformed_json\"}", DSP_TXDIAG_PROTOCOL},
        {400, "{\"status\":\"invalid\",\"error\":\"unsupported_schema_version\"}", DSP_TXDIAG_PROTOCOL},
        {400, "{\"status\":\"invalid\",\"error\":\"missing_field\",\"field\":\"attempt\"}",
         DSP_TXDIAG_PROTOCOL},
        {400, "{\"status\":\"invalid\",\"error\":\"invalid_field\",\"field\":\"device_id\"}",
         DSP_TXDIAG_PROTOCOL},
        {400, "<html>Bad Request</html>", DSP_TXDIAG_PROTOCOL},
        {409, "{\"status\":\"conflict\",\"event_id\":\"dsp-ffffffffffff-ffffffff-1\"}", DSP_TXDIAG_PROTOCOL},
        {302, "", DSP_TXDIAG_ENDPOINT},
        {429, "", 0},
        {503, "{\"status\":\"retry\"}", 0},
        {500, "", 0},
    };
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        fresh();
        add_dosing_errors(5);
        add_reservoir_low();
        port.fallback = response(cases[c].status, cases[c].body);
        for (int s = 0; s < 20; s++) {
            dsp_notify_delivery_report_t r = session(60000);
            DSP_CHECK_EQ_INT(1, r.attempted); /* stops at the first failure */
            DSP_CHECK_EQ_INT(0, r.rejected);
            DSP_CHECK(r.stopped_early);
        }
        boot();
        if (dsp_notify_pending_count(&n) != 6 || n.s.rejections.count != 0) {
            fprintf(stderr, "case %zu (HTTP %u) drained the queue\n", c, (unsigned)cases[c].status);
        }
        DSP_CHECK_EQ_INT(6, dsp_notify_pending_count(&n));
        DSP_CHECK_EQ_INT(0, n.s.rejections.count);
        DSP_CHECK_EQ_INT(0, n.s.rejections.total);
        uint16_t status = 0;
        DSP_CHECK_EQ_INT(cases[c].diag, dsp_notify_tx_diag(&n, &status));
        if (cases[c].diag != 0) {
            DSP_CHECK_EQ_INT(cases[c].status, status);
        }
        /* Fixed (e.g. a firmware update): everything is delivered and the flag clears. */
        port_reset();
        port.auto_ack = true;
        DSP_CHECK_EQ_INT(6, session(60000).acked);
        DSP_CHECK_EQ_INT(0, dsp_notify_tx_diag(&n, NULL));
    }
}

static void test_event_specific_rejections(void)
{
    fresh();
    add_dosing_errors(3);
    char id1[DSP_NOTIFY_ID_BUF_LEN];
    char id3[DSP_NOTIFY_ID_BUF_LEN];
    char b[160];
    id_for(1, id1);
    id_for(3, id3);
    snprintf(b, sizeof b, "{\"status\":\"conflict\",\"event_id\":\"%s\"}", id1);
    port.script[0] = response(409, b);
    port.script[1] = response(400, "{\"status\":\"invalid\",\"error\":\"invalid_field\",\"field\":\"value\"}");
    snprintf(b, sizeof b, "{\"status\":\"accepted\",\"event_id\":\"%s\"}", id3);
    port.script[2] = response(201, b);
    port.script_len = 3;
    dsp_notify_delivery_report_t r = session(60000);
    DSP_CHECK_EQ_INT(2, r.rejected);
    DSP_CHECK_EQ_INT(0, r.acked); /* the rejection limit (2) ended the session */
    DSP_CHECK(r.stopped_early);
    DSP_CHECK_EQ_INT(1, dsp_notify_pending_count(&n));
    const dsp_rejection_log_t *log = dsp_notify_rejections(&n);
    DSP_CHECK_EQ_INT(2, log->count);
    DSP_CHECK_EQ_INT(409, log->entry[0].http_status);
    DSP_CHECK_EQ_INT(DSP_TXERR_CONFLICT, log->entry[0].error);
    DSP_CHECK_EQ_INT(400, log->entry[1].http_status);
    DSP_CHECK_EQ_INT(DSP_TXERR_INVALID_FIELD, log->entry[1].error);
    DSP_CHECK_EQ_INT(DSP_TXFIELD_VALUE, log->entry[1].field);
    /* The next session delivers the remaining event. */
    port_reset();
    port.auto_ack = true;
    DSP_CHECK_EQ_INT(1, session(60000).acked);
    DSP_CHECK_EQ_INT(0, dsp_notify_pending_count(&n));
    boot();
    DSP_CHECK_EQ_INT(2, dsp_notify_rejections(&n)->count);
}

static void test_bad_ack_is_uncertain_then_duplicate(void)
{
    fresh();
    add_dosing_errors(1);
    char id[DSP_NOTIFY_ID_BUF_LEN];
    char b[160];
    id_for(1, id);
    port.script[0] = response(201, "{\"status\":\"accepted\",\"event_id\":\"dsp-0123456789ab-a5a5a5a5-2\"}");
    port.script_len = 1;
    dsp_notify_delivery_report_t r = session(60000);
    DSP_CHECK_EQ_INT(0, r.acked);
    DSP_CHECK_EQ_INT(1, dsp_notify_pending_count(&n));
    DSP_CHECK_EQ_INT(DSP_TXDIAG_PROTOCOL, dsp_notify_tx_diag(&n, NULL));
    char first[DSP_PROTO_BODY_MAX];
    snprintf(first, sizeof first, "%s", port.bodies[0]);
    boot(); /* restart in between */
    port_reset();
    snprintf(b, sizeof b, "{\"status\":\"duplicate\",\"event_id\":\"%s\"}", id);
    port.script[0] = response(200, b);
    port.script_len = 1;
    DSP_CHECK_EQ_INT(1, session(60000).acked);
    DSP_CHECK_EQ_INT(0, dsp_notify_tx_diag(&n, NULL)); /* cleared by the acknowledgement */
    char a1[DSP_PROTO_BODY_MAX];
    char a2[DSP_PROTO_BODY_MAX];
    strip_attempt(first, a1, sizeof a1);
    strip_attempt(port.bodies[0], a2, sizeof a2);
    DSP_CHECK_EQ_STR(a1, a2); /* same ID, identical payload */
    DSP_CHECK(strstr(first, "\"attempt\":1}") != NULL);
    DSP_CHECK(strstr(port.bodies[0], "\"attempt\":2}") != NULL);
}

/* The backend stored the event but the response was lost (timeout): resent with the same
 * ID and identical payload, answered as a duplicate. */
static void test_timeout_after_accept_and_power_loss(void)
{
    fresh();
    add_dosing_errors(2);
    port.script[0] = phase_only(DSP_HTTP_NO_RESPONSE);
    port.script_len = 1;
    dsp_notify_delivery_report_t r = session(60000);
    DSP_CHECK(r.stopped_early);
    DSP_CHECK_EQ_INT(2, dsp_notify_pending_count(&n));
    char first[DSP_PROTO_BODY_MAX];
    snprintf(first, sizeof first, "%s", port.bodies[0]);
    boot(); /* power loss before the next session */
    port_reset();
    char id[DSP_NOTIFY_ID_BUF_LEN];
    char b[160];
    id_for(1, id);
    snprintf(b, sizeof b, "{\"status\":\"duplicate\",\"event_id\":\"%s\"}", id);
    port.script[0] = response(200, b);
    id_for(2, id);
    snprintf(b, sizeof b, "{\"status\":\"accepted\",\"event_id\":\"%s\"}", id);
    port.script[1] = response(201, b);
    port.script_len = 2;
    DSP_CHECK_EQ_INT(2, session(60000).acked);
    char a1[DSP_PROTO_BODY_MAX];
    char a2[DSP_PROTO_BODY_MAX];
    strip_attempt(first, a1, sizeof a1);
    strip_attempt(port.bodies[0], a2, sizeof a2);
    DSP_CHECK_EQ_STR(a1, a2);
    /* Send failure (part of the request may have left): UNCERTAIN, kept. */
    add_dosing_errors(1);
    port_reset();
    port.script[0] = phase_only(DSP_HTTP_SEND_FAILED);
    port.script_len = 1;
    DSP_CHECK(session(60000).stopped_early);
    DSP_CHECK_EQ_INT(1, dsp_notify_pending_count(&n));
    DSP_CHECK_EQ_INT(0, dsp_notify_tx_diag(&n, NULL));
}

static void test_connect_and_tls_failures(void)
{
    dsp_http_phase_t phases[] = {DSP_HTTP_CONNECT_FAILED, DSP_HTTP_TLS_FAILED};
    for (size_t p = 0; p < 2; p++) {
        fresh();
        add_dosing_errors(2);
        port.fallback = phase_only(phases[p]);
        for (int s = 0; s < 5; s++) {
            DSP_CHECK(session(60000).stopped_early);
        }
        DSP_CHECK_EQ_INT(2, dsp_notify_pending_count(&n));
        DSP_CHECK_EQ_INT(0, dsp_notify_tx_diag(&n, NULL)); /* network problems: no flag */
        DSP_CHECK_EQ_INT(5, port.posts);
        char a1[DSP_PROTO_BODY_MAX];
        char a2[DSP_PROTO_BODY_MAX];
        strip_attempt(port.bodies[0], a1, sizeof a1);
        strip_attempt(port.bodies[4], a2, sizeof a2);
        DSP_CHECK_EQ_STR(a1, a2);
    }
    /* Port unusable: nothing attempted at all. */
    fresh();
    add_dosing_errors(1);
    port.open_fails = true;
    DSP_CHECK(!dsp_https_begin(&https, 60000));
    DSP_CHECK_EQ_INT(0, port.posts);
}

static void test_budget_with_monotonic_clock(void)
{
    fresh();
    add_dosing_errors(16);
    port.auto_ack = true;
    port.advance_ms = 15000; /* each request takes 15 s */
    dsp_notify_delivery_report_t r = session(60000);
    /* Started at t=0, 15, 30 s; at 45 s only 15 s are left (< 20 s worst case). */
    DSP_CHECK_EQ_INT(3, r.attempted);
    DSP_CHECK_EQ_INT(3, r.acked);
    DSP_CHECK(r.budget_exhausted);
    DSP_CHECK_EQ_INT(13, dsp_notify_pending_count(&n));
    for (int i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY; i++) {
        if (n.s.queue[i].seq != 0) {
            DSP_CHECK_EQ_INT(0, n.s.queue[i].attempts);
        }
    }
    r = session(60000);
    DSP_CHECK_EQ_INT(3, r.acked); /* the rest goes in the following sessions */
    DSP_CHECK_EQ_INT(10, dsp_notify_pending_count(&n));
    /* A budget smaller than one worst-case request: nothing is started. */
    r = session(19999);
    DSP_CHECK_EQ_INT(0, r.attempted);
    DSP_CHECK(r.budget_exhausted);
}

/* ---- Connectivity session orchestration --------------------------------------------- */

static dsp_utc_t device_clock;
static dsp_utc_t true_time;
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

static dsp_sync_status_t ntp_result;
static int ntp_calls;
static dsp_sync_status_t ntp_sync(void *ctx, uint32_t timeout_ms, dsp_time_sample_t *out)
{
    (void)ctx;
    (void)timeout_ms;
    ntp_calls++;
    if (ntp_result == DSP_SYNC_OK) {
        out->source_utc = true_time;
        out->clock_utc = device_clock;
    }
    return ntp_result;
}
static const dsp_time_source_t NTP = {NULL, ntp_sync};

static struct {
    int starts, stops, connects;
    dsp_net_status_t connect_result;
} wf;
static dsp_net_status_t wf_start(void *ctx)
{
    (void)ctx;
    wf.starts++;
    return DSP_NET_OK;
}
static dsp_net_status_t wf_connect(void *ctx, const dsp_wifi_credentials_t *c, uint32_t t)
{
    (void)ctx;
    (void)c;
    (void)t;
    wf.connects++;
    return wf.connect_result;
}
static void wf_disconnect(void *ctx)
{
    (void)ctx;
}
static void wf_stop(void *ctx)
{
    (void)ctx;
    wf.stops++;
}
static const dsp_wifi_driver_t WIFI = {NULL, wf_start, wf_connect, wf_disconnect, wf_stop};
static dsp_cred_status_t creds_get(void *ctx, dsp_wifi_credentials_t *out)
{
    (void)ctx;
    snprintf(out->ssid, sizeof out->ssid, "test-ssid");
    snprintf(out->passphrase, sizeof out->passphrase, "test-passphrase");
    return DSP_CRED_OK;
}
static const dsp_wifi_credentials_provider_t CREDS = {NULL, creds_get};

static dsp_timekeeper_t tk;
static dsp_wifi_manager_t wifi;
static dsp_time_service_t svc;
static dsp_connectivity_retained_t retained;
static dsp_connectivity_t conn;
static const dsp_connectivity_guard_t IDLE = {false, false, false, false, 0};

static void world(void)
{
    fresh();
    true_time = T0;
    device_clock = T0;
    ntp_result = DSP_SYNC_OK;
    ntp_calls = 0;
    memset(&wf, 0, sizeof wf);
    wf.connect_result = DSP_NET_OK;
    dsp_time_meta_t meta;
    memset(&meta, 0, sizeof meta);
    DSP_CHECK(dsp_timekeeper_init(&tk, &meta, 24, NULL)); /* never synced: UNKNOWN */
    dsp_wifi_config_t cfg = {DSP_WIFI_TIMEOUT_DEFAULT_MS, DSP_WIFI_ATTEMPTS_DEFAULT};
    DSP_CHECK_EQ_INT(DSP_NET_OK, dsp_wifi_init(&wifi, &WIFI, &CREDS, &cfg));
    dsp_time_service_t s = {&tk, &wifi, &NTP, &CLOCK, &st, 0};
    svc = s;
    memset(&retained, 0, sizeof retained);
    dsp_connectivity_t c = {&svc, &n, &https, &retained, 0};
    conn = c;
    port.auto_ack = true;
}

static void advance(dsp_utc_t s)
{
    true_time += s;
    device_clock += s;
}

/* Wi-Fi is off after every session, whatever happened. */
static void check_wifi_off(void)
{
    DSP_CHECK_EQ_INT(DSP_WIFI_DISABLED, dsp_wifi_state(&wifi));
    DSP_CHECK_EQ_INT(wf.starts, wf.stops);
}

static void test_worst_case_and_dose_guard(void)
{
    world();
    DSP_CHECK_EQ_INT(115, dsp_connectivity_worst_case_s(&conn)); /* 3 x 15 + 10 + 60 s */
    add_dosing_errors(1);
    dsp_connectivity_guard_t g = IDLE;
    g.dosing_active = true;
    DSP_CHECK_EQ_INT(DSP_CONN_SKIPPED_GUARD, dsp_connectivity_run(&conn, &g).outcome);
    g = IDLE;
    g.permit_outstanding = true;
    DSP_CHECK_EQ_INT(DSP_CONN_SKIPPED_GUARD, dsp_connectivity_run(&conn, &g).outcome);
    g = IDLE;
    g.ble_active = true;
    DSP_CHECK_EQ_INT(DSP_CONN_SKIPPED_GUARD, dsp_connectivity_run(&conn, &g).outcome);
    g = IDLE;
    g.next_dose_known = true;
    g.next_dose_at = device_clock + 115 + DSP_CONN_DOSE_MARGIN_S; /* exactly at the limit */
    DSP_CHECK_EQ_INT(DSP_CONN_SKIPPED_DOSE_SOON, dsp_connectivity_run(&conn, &g).outcome);
    g.next_dose_at = device_clock - 5; /* a dose is due right now */
    DSP_CHECK_EQ_INT(DSP_CONN_SKIPPED_DOSE_SOON, dsp_connectivity_run(&conn, &g).outcome);
    DSP_CHECK_EQ_INT(0, wf.starts); /* none of these touched the radio */
    DSP_CHECK_EQ_INT(0, port.posts);
    DSP_CHECK_EQ_INT(1, dsp_notify_pending_count(&n));
    g.next_dose_at = device_clock + 115 + DSP_CONN_DOSE_MARGIN_S + 1;
    DSP_CHECK_EQ_INT(DSP_CONN_DONE, dsp_connectivity_run(&conn, &g).outcome);
    check_wifi_off();
    DSP_CHECK_EQ_INT(0, dsp_notify_pending_count(&n));
}

static void test_regular_session_delivers_everything(void)
{
    world();
    add_reservoir_low(); /* WARNING */
    add_dosing_errors(1);
    dsp_connectivity_report_t r = dsp_connectivity_run(&conn, &IDLE);
    DSP_CHECK_EQ_INT(DSP_CONN_DONE, r.outcome);
    DSP_CHECK_EQ_INT(DSP_CONN_REASON_SYNC, r.reason); /* first sync is due */
    DSP_CHECK(r.delivery_ran);
    DSP_CHECK_EQ_INT(2, r.delivery.acked);
    DSP_CHECK(tk.trust != DSP_TRUST_UNKNOWN);
    DSP_CHECK(strstr(port.bodies[0], "DOSING_ERROR") != NULL); /* CRITICAL first */
    DSP_CHECK_EQ_INT(0, retained.last_notify_attempt); /* not notification-initiated */
    check_wifi_off();
    /* Nothing pending and no sync due: no session at all. */
    advance(H);
    DSP_CHECK_EQ_INT(DSP_CONN_SKIPPED_NOT_DUE, dsp_connectivity_run(&conn, &IDLE).outcome);
}

static void test_warning_waits_for_regular_session(void)
{
    world();
    DSP_CHECK_EQ_INT(DSP_CONN_DONE, dsp_connectivity_run(&conn, &IDLE).outcome); /* synced */
    int starts = wf.starts;
    advance(H);
    add_reservoir_low(); /* WARNING only */
    for (int i = 0; i < 20; i++) {
        advance(H);
        if (dsp_timekeeper_sync_due(&tk, device_clock)) {
            break;
        }
        DSP_CHECK_EQ_INT(DSP_CONN_SKIPPED_NOT_DUE, dsp_connectivity_run(&conn, &IDLE).outcome);
    }
    DSP_CHECK_EQ_INT(starts, wf.starts);
    DSP_CHECK_EQ_INT(1, dsp_notify_pending_count(&n));
    while (!dsp_timekeeper_sync_due(&tk, device_clock)) {
        advance(H);
    }
    dsp_connectivity_report_t r = dsp_connectivity_run(&conn, &IDLE);
    DSP_CHECK_EQ_INT(DSP_CONN_REASON_SYNC, r.reason);
    DSP_CHECK_EQ_INT(1, r.delivery.acked);
}

static void test_critical_on_existing_wake_rate_limited(void)
{
    world();
    DSP_CHECK_EQ_INT(DSP_CONN_DONE, dsp_connectivity_run(&conn, &IDLE).outcome); /* synced */
    advance(2 * H);
    add_dosing_errors(1);
    dsp_connectivity_report_t r = dsp_connectivity_run(&conn, &IDLE);
    DSP_CHECK_EQ_INT(DSP_CONN_DONE, r.outcome);
    DSP_CHECK_EQ_INT(DSP_CONN_REASON_CRITICAL, r.reason);
    DSP_CHECK_EQ_INT(1, r.delivery.acked);
    DSP_CHECK_EQ_INT(device_clock, retained.last_notify_attempt);
    check_wifi_off();
    /* Another CRITICAL 10 minutes later: at most one attempt per hour. */
    advance(600);
    add_dosing_errors(1);
    int starts = wf.starts;
    DSP_CHECK_EQ_INT(DSP_CONN_SKIPPED_NOT_DUE, dsp_connectivity_run(&conn, &IDLE).outcome);
    DSP_CHECK_EQ_INT(starts, wf.starts);
    advance(H - 600 - 1);
    DSP_CHECK_EQ_INT(DSP_CONN_SKIPPED_NOT_DUE, dsp_connectivity_run(&conn, &IDLE).outcome);
    advance(1);
    r = dsp_connectivity_run(&conn, &IDLE);
    DSP_CHECK_EQ_INT(DSP_CONN_REASON_CRITICAL, r.reason);
    DSP_CHECK_EQ_INT(1, r.delivery.acked);
    /* A backward clock step does not lock notifications out. */
    advance(600);
    add_dosing_errors(1);
    device_clock = retained.last_notify_attempt - 3 * H;
    true_time = device_clock;
    r = dsp_connectivity_run(&conn, &IDLE);
    DSP_CHECK_EQ_INT(DSP_CONN_REASON_CRITICAL, r.reason);
}

static void test_critical_respects_wifi_backoff(void)
{
    world();
    DSP_CHECK_EQ_INT(DSP_CONN_DONE, dsp_connectivity_run(&conn, &IDLE).outcome); /* synced */
    advance(2 * H);
    add_dosing_errors(1);
    wf.connect_result = DSP_NET_ERR_NO_AP;
    dsp_connectivity_report_t r = dsp_connectivity_run(&conn, &IDLE);
    DSP_CHECK_EQ_INT(DSP_CONN_WIFI_FAILED, r.outcome);
    DSP_CHECK_EQ_INT(DSP_CONN_REASON_CRITICAL, r.reason);
    DSP_CHECK_EQ_INT(1, tk.retained.consecutive_failures); /* fed into the M1.4 backoff */
    DSP_CHECK_EQ_INT(1, dsp_notify_pending_count(&n));
    DSP_CHECK_EQ_INT(0, port.posts);
    check_wifi_off();
    /* An hour later the interval allows it, but the Wi-Fi backoff does not. */
    wf.connect_result = DSP_NET_OK;
    advance(H);
    while (!dsp_timekeeper_sync_due(&tk, device_clock)) {
        int starts = wf.starts;
        DSP_CHECK_EQ_INT(DSP_CONN_SKIPPED_NOT_DUE, dsp_connectivity_run(&conn, &IDLE).outcome);
        DSP_CHECK_EQ_INT(starts, wf.starts);
        advance(600);
    }
    r = dsp_connectivity_run(&conn, &IDLE); /* the retry is due: a regular session */
    DSP_CHECK_EQ_INT(DSP_CONN_REASON_SYNC, r.reason);
    DSP_CHECK_EQ_INT(1, r.delivery.acked);
}

static void test_untrusted_time_sends_nothing(void)
{
    world();
    add_dosing_errors(2);
    ntp_result = DSP_SYNC_ERR_TIMEOUT; /* Wi-Fi works, NTP does not: clock stays UNKNOWN */
    dsp_connectivity_report_t r = dsp_connectivity_run(&conn, &IDLE);
    DSP_CHECK_EQ_INT(DSP_CONN_TIME_UNTRUSTED, r.outcome);
    DSP_CHECK(!r.delivery_ran);
    DSP_CHECK_EQ_INT(0, port.posts);
    DSP_CHECK_EQ_INT(0, port.opens);
    DSP_CHECK_EQ_INT(2, dsp_notify_pending_count(&n));
    for (int i = 0; i < DSP_NOTIFY_QUEUE_CAPACITY; i++) {
        DSP_CHECK_EQ_INT(0, n.s.queue[i].attempts); /* untouched */
    }
    check_wifi_off();
    /* Trusted (synced earlier) and NTP failing now: HOLDOVER is trusted, delivery runs. */
    ntp_result = DSP_SYNC_OK;
    advance(15 * 60);
    while (!dsp_timekeeper_sync_due(&tk, device_clock)) {
        advance(60);
    }
    r = dsp_connectivity_run(&conn, &IDLE);
    DSP_CHECK_EQ_INT(DSP_CONN_DONE, r.outcome);
    DSP_CHECK_EQ_INT(2, r.delivery.acked);
    advance(2 * H);
    add_dosing_errors(1);
    ntp_result = DSP_SYNC_ERR_TIMEOUT;
    r = dsp_connectivity_run(&conn, &IDLE);
    DSP_CHECK_EQ_INT(DSP_CONN_DONE, r.outcome);
    DSP_CHECK_EQ_INT(1, r.delivery.acked);
}

static void test_missing_token_or_port(void)
{
    world();
    add_dosing_errors(1);
    token_status = DSP_CRED_UNAVAILABLE;
    dsp_connectivity_report_t r = dsp_connectivity_run(&conn, &IDLE);
    DSP_CHECK_EQ_INT(DSP_CONN_NO_TOKEN, r.outcome);
    DSP_CHECK_EQ_INT(0, port.posts);
    DSP_CHECK_EQ_INT(0, n.s.queue[0].attempts + n.s.queue[1].attempts);
    DSP_CHECK_EQ_INT(1, dsp_notify_pending_count(&n));
    DSP_CHECK(tk.trust != DSP_TRUST_UNKNOWN); /* the sync itself still happened */
    check_wifi_off();
    token_status = DSP_CRED_OK;
    port.open_fails = true;
    advance(2 * H);
    r = dsp_connectivity_run(&conn, &IDLE);
    DSP_CHECK_EQ_INT(DSP_CONN_PORT_FAILED, r.outcome);
    DSP_CHECK_EQ_INT(1, dsp_notify_pending_count(&n));
    check_wifi_off();
    DSP_CHECK_EQ_INT(DSP_CONN_ERR_ARG, dsp_connectivity_run(NULL, &IDLE).outcome);
    DSP_CHECK_EQ_INT(DSP_CONN_ERR_ARG, dsp_connectivity_run(&conn, NULL).outcome);
}

/* ---- Time-only sessions (CONFIG_DSP_APP_WIFI_TIME: https == NULL) ------------------------- */

static void test_time_only_syncs_and_never_delivers(void)
{
    world();
    conn.https = NULL;
    DSP_CHECK_EQ_INT(55, dsp_connectivity_worst_case_s(&conn)); /* 3 x 15 + 10 s, no HTTPS budget */
    add_dosing_errors(1); /* CRITICAL pending */
    dsp_connectivity_report_t r = dsp_connectivity_run(&conn, &IDLE);
    DSP_CHECK_EQ_INT(DSP_CONN_DONE, r.outcome);
    DSP_CHECK_EQ_INT(DSP_CONN_REASON_SYNC, r.reason);
    DSP_CHECK(!r.delivery_ran);
    DSP_CHECK_EQ_INT(0, port.posts);
    DSP_CHECK(tk.trust != DSP_TRUST_UNKNOWN);   /* trusted through the unchanged M1.4 logic */
    DSP_CHECK_EQ_INT(1, dsp_notify_pending_count(&n)); /* the queue is untouched */
    check_wifi_off();
    /* a pending CRITICAL never starts a time-only session; no sync due: nothing at all */
    int starts = wf.starts;
    for (int i = 0; i < 6; i++) {
        advance(H);
        DSP_CHECK_EQ_INT(DSP_CONN_SKIPPED_NOT_DUE, dsp_connectivity_run(&conn, &IDLE).outcome);
    }
    DSP_CHECK_EQ_INT(starts, wf.starts);
    DSP_CHECK_EQ_INT(0, retained.last_notify_attempt);
    DSP_CHECK_EQ_INT(0, port.posts);
}

static void test_time_only_failures_never_trust_and_back_off(void)
{
    world();
    conn.https = NULL;
    /* Wi-Fi unavailable: bounded attempts, Wi-Fi off, no SNTP, still UNKNOWN */
    wf.connect_result = DSP_NET_ERR_NO_AP;
    dsp_connectivity_report_t r = dsp_connectivity_run(&conn, &IDLE);
    DSP_CHECK_EQ_INT(DSP_CONN_WIFI_FAILED, r.outcome);
    DSP_CHECK(wf.connects >= 1 && wf.connects <= (int)DSP_WIFI_ATTEMPTS_DEFAULT);
    DSP_CHECK_EQ_INT(0, ntp_calls);
    DSP_CHECK_EQ_INT(DSP_TRUST_UNKNOWN, tk.trust);
    check_wifi_off();
    /* backoff: far fewer sessions than hourly wakes over a day, each bounded */
    int sessions = 1;
    for (int i = 0; i < 24; i++) {
        advance(H);
        int before = wf.starts;
        r = dsp_connectivity_run(&conn, &IDLE);
        if (wf.starts != before) {
            sessions++;
            DSP_CHECK_EQ_INT(DSP_CONN_WIFI_FAILED, r.outcome);
        } else {
            DSP_CHECK_EQ_INT(DSP_CONN_SKIPPED_NOT_DUE, r.outcome);
        }
        check_wifi_off();
    }
    DSP_CHECK(sessions >= 2 && sessions <= 8);
    DSP_CHECK(wf.connects <= sessions * (int)DSP_WIFI_ATTEMPTS_DEFAULT);
    DSP_CHECK_EQ_INT(DSP_TRUST_UNKNOWN, tk.trust);
    /* Wi-Fi works, SNTP does not answer: no false trust */
    world();
    conn.https = NULL;
    ntp_result = DSP_SYNC_ERR_TIMEOUT;
    r = dsp_connectivity_run(&conn, &IDLE);
    DSP_CHECK_EQ_INT(DSP_CONN_TIME_UNTRUSTED, r.outcome);
    DSP_CHECK_EQ_INT(DSP_TRUST_UNKNOWN, tk.trust);
    DSP_CHECK_EQ_INT(0, port.posts);
    check_wifi_off();
    /* the next due attempt with a working SNTP server establishes trust */
    ntp_result = DSP_SYNC_OK;
    for (int i = 0; i < 48 && tk.trust == DSP_TRUST_UNKNOWN; i++) {
        advance(H);
        (void)dsp_connectivity_run(&conn, &IDLE);
        check_wifi_off();
    }
    DSP_CHECK(tk.trust != DSP_TRUST_UNKNOWN);
}

int main(void)
{
    DSP_RUN(test_ack_body_and_token_handling);
    DSP_RUN(test_token_validation);
    DSP_RUN(test_systemic_statuses_never_drain_queue);
    DSP_RUN(test_event_specific_rejections);
    DSP_RUN(test_bad_ack_is_uncertain_then_duplicate);
    DSP_RUN(test_timeout_after_accept_and_power_loss);
    DSP_RUN(test_connect_and_tls_failures);
    DSP_RUN(test_budget_with_monotonic_clock);
    DSP_RUN(test_worst_case_and_dose_guard);
    DSP_RUN(test_regular_session_delivers_everything);
    DSP_RUN(test_warning_waits_for_regular_session);
    DSP_RUN(test_critical_on_existing_wake_rate_limited);
    DSP_RUN(test_critical_respects_wifi_backoff);
    DSP_RUN(test_untrusted_time_sends_nothing);
    DSP_RUN(test_missing_token_or_port);
    DSP_RUN(test_time_only_syncs_and_never_delivers);
    DSP_RUN(test_time_only_failures_never_trust_and_back_off);
    return dsp_test_finish();
}
