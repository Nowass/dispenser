/*
 * Notification protocol v1 tests (M1.7): the exact request body produced from a queued
 * notification (golden strings, validation, bounds) and the strict classification of the
 * backend's responses (approved R1 rules: systemic problems are FAILED with a persistent
 * diagnostic, only responses about THIS event are REJECTED, ACK needs strict confirmation).
 */
#include "dsp_notify_proto.h"
#include "dsp_test.h"
#include "dsp_timekeeping.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DEVICE 0x0123456789ABull
#define EPOCH 0xA5A5A5A5u
#define T0 ((dsp_utc_t)1795158000)
#define ID42 "dsp-0123456789ab-a5a5a5a5-42"

static dsp_note_t note(uint8_t type, uint16_t code)
{
    dsp_note_t e;
    memset(&e, 0, sizeof e);
    e.seq = 42;
    e.type = type;
    e.severity = (type == DSP_NOTE_RESERVOIR_LOW || type == DSP_NOTE_BATTERY_LOW) ? DSP_SEV_WARNING
                                                                                 : DSP_SEV_CRITICAL;
    e.condition = DSP_COND_NONE;
    e.code = code;
    e.attempts = 1;
    e.created_at = T0;
    e.flags = DSP_NOTE_F_TIME_VALID;
    return e;
}

static void expect_body(const dsp_note_t *e, const char *expected)
{
    char buf[DSP_PROTO_BODY_MAX];
    size_t len = dsp_proto_encode_event(DEVICE, EPOCH, e, buf, sizeof buf);
    DSP_CHECK_EQ_INT(strlen(expected), len);
    DSP_CHECK_EQ_STR(expected, buf);
}

static void test_encode_golden(void)
{
    /* The PROTOCOL.md example, byte for byte. */
    dsp_note_t e = note(DSP_NOTE_RESERVOIR_LOW, 0);
    e.condition = DSP_COND_RESERVOIR_LOW;
    e.value = 8000;
    e.value2 = 100000;
    expect_body(&e, "{\"schema_version\":1,\"event_id\":\"" ID42 "\",\"device_id\":\"0123456789ab\","
                    "\"type\":\"RESERVOIR_LOW\",\"severity\":\"WARNING\",\"code\":null,"
                    "\"reminder_no\":0,\"created_at\":1795158000,\"uncertain\":false,"
                    "\"manual\":false,\"value\":8000,\"value2\":100000,\"attempt\":1}");

    /* Manual dosing error, clock untrusted (created_at null). */
    e = note(DSP_NOTE_DOSING_ERROR, DSP_DOSEERR_INTERRUPTED);
    e.flags = DSP_NOTE_F_MANUAL;
    e.created_at = 0;
    e.value = 2000;
    e.attempts = 3;
    expect_body(&e, "{\"schema_version\":1,\"event_id\":\"" ID42 "\",\"device_id\":\"0123456789ab\","
                    "\"type\":\"DOSING_ERROR\",\"severity\":\"CRITICAL\",\"code\":\"INTERRUPTED\","
                    "\"reminder_no\":0,\"created_at\":null,\"uncertain\":false,"
                    "\"manual\":true,\"value\":2000,\"value2\":0,\"attempt\":3}");

    /* Reminder of an uncertain BLOCKED episode; extreme values. */
    e = note(DSP_NOTE_RESERVOIR_BLOCKED, 0);
    e.condition = DSP_COND_RESERVOIR_BLOCKED;
    e.flags = DSP_NOTE_F_TIME_VALID | DSP_NOTE_F_REMINDER | DSP_NOTE_F_UNCERTAIN;
    e.reminder_no = 255;
    e.seq = 4294967294u;
    e.created_at = DSP_UTC_MAX;
    e.value = 4294967295u;
    e.value2 = 4294967295u;
    e.attempts = 65535;
    expect_body(&e, "{\"schema_version\":1,\"event_id\":\"dsp-0123456789ab-a5a5a5a5-4294967294\","
                    "\"device_id\":\"0123456789ab\",\"type\":\"RESERVOIR_BLOCKED\","
                    "\"severity\":\"CRITICAL\",\"code\":null,\"reminder_no\":255,"
                    "\"created_at\":4133980799,\"uncertain\":true,\"manual\":false,"
                    "\"value\":4294967295,\"value2\":4294967295,\"attempt\":65535}");

    /* Every code name of the closed sets. */
    const char *dose[] = {"START_FAILED", "PUMP_FAILED", "INTERRUPTED", "STORAGE", "BLOCKED"};
    for (uint16_t c = 1; c <= DSP_DOSEERR_MAX; c++) {
        e = note(DSP_NOTE_DOSING_ERROR, c);
        char buf[DSP_PROTO_BODY_MAX];
        char want[48];
        snprintf(want, sizeof want, "\"code\":\"%s\"", dose[c - 1]);
        DSP_CHECK(dsp_proto_encode_event(DEVICE, EPOCH, &e, buf, sizeof buf) > 0);
        DSP_CHECK(strstr(buf, want) != NULL);
    }
    const char *dev[] = {"STORAGE", "TIME_SYNC", "CLOCK", "QUEUE_OVERFLOW"};
    for (uint16_t c = DSP_DEVERR_STORAGE; c <= DSP_DEVERR_QUEUE_OVERFLOW; c++) {
        e = note(DSP_NOTE_DEVICE_ERROR, c);
        char buf[DSP_PROTO_BODY_MAX];
        char want[48];
        snprintf(want, sizeof want, "\"code\":\"%s\"", dev[c - 1]);
        DSP_CHECK(dsp_proto_encode_event(DEVICE, EPOCH, &e, buf, sizeof buf) > 0);
        DSP_CHECK(strstr(buf, want) != NULL);
    }
    /* Battery: no code, millivolts. */
    e = note(DSP_NOTE_BATTERY_LOW, 0);
    e.value = 3300;
    char buf[DSP_PROTO_BODY_MAX];
    DSP_CHECK(dsp_proto_encode_event(DEVICE, EPOCH, &e, buf, sizeof buf) > 0);
    DSP_CHECK(strstr(buf, "\"type\":\"BATTERY_LOW\",\"severity\":\"WARNING\",\"code\":null") != NULL);
    /* Device ID: only the 48 MAC bits. */
    e = note(DSP_NOTE_BATTERY_LOW, 0);
    DSP_CHECK(dsp_proto_encode_event(0xFFFF000000000001ull, 1, &e, buf, sizeof buf) > 0);
    DSP_CHECK(strstr(buf, "\"event_id\":\"dsp-000000000001-00000001-42\",\"device_id\":\"000000000001\"") != NULL);
}

static void test_encode_refuses_invalid(void)
{
    char buf[DSP_PROTO_BODY_MAX];
    dsp_note_t ok = note(DSP_NOTE_RESERVOIR_LOW, 0);
    DSP_CHECK(dsp_proto_encode_event(DEVICE, EPOCH, &ok, buf, sizeof buf) > 0);
    typedef struct {
        const char *what;
        dsp_note_t e;
    } bad_t;
    bad_t bad[16];
    size_t nb = 0;
    dsp_note_t e = ok;
    e.seq = 0;
    bad[nb++] = (bad_t){"seq 0", e};
    e = ok;
    e.attempts = 0;
    bad[nb++] = (bad_t){"never started", e};
    e = ok;
    e.type = 9;
    bad[nb++] = (bad_t){"unknown type", e};
    e = ok;
    e.severity = DSP_SEV_CRITICAL;
    bad[nb++] = (bad_t){"severity mismatch", e};
    e = ok;
    e.code = 1;
    bad[nb++] = (bad_t){"code on a codeless type", e};
    e = note(DSP_NOTE_DOSING_ERROR, 0);
    bad[nb++] = (bad_t){"dosing code 0", e};
    e = note(DSP_NOTE_DOSING_ERROR, DSP_DOSEERR_MAX + 1);
    bad[nb++] = (bad_t){"dosing code too high", e};
    e = note(DSP_NOTE_DEVICE_ERROR, 5);
    bad[nb++] = (bad_t){"device code too high", e};
    e = note(DSP_NOTE_DOSING_ERROR, 1);
    e.reminder_no = 1;
    bad[nb++] = (bad_t){"reminder of a one-shot", e};
    e = note(DSP_NOTE_DEVICE_ERROR, DSP_DEVERR_QUEUE_OVERFLOW);
    e.reminder_no = 2;
    bad[nb++] = (bad_t){"reminder of an overflow report", e};
    e = ok;
    e.flags |= DSP_NOTE_F_MANUAL;
    bad[nb++] = (bad_t){"manual outside DOSING_ERROR", e};
    e = ok;
    e.created_at = DSP_TIME_PLAUSIBLE_MIN - 1;
    bad[nb++] = (bad_t){"created_at before the protocol range", e};
    e = ok;
    e.created_at = DSP_UTC_MAX + 1;
    bad[nb++] = (bad_t){"created_at after the protocol range", e};
    for (size_t i = 0; i < nb; i++) {
        buf[0] = 'x';
        size_t len = dsp_proto_encode_event(DEVICE, EPOCH, &bad[i].e, buf, sizeof buf);
        if (len != 0) {
            fprintf(stderr, "encoded invalid note: %s\n", bad[i].what);
        }
        DSP_CHECK_EQ_INT(0, len);
        DSP_CHECK_EQ_INT('\0', buf[0]);
    }
    DSP_CHECK_EQ_INT(0, dsp_proto_encode_event(DEVICE, 0, &ok, buf, sizeof buf)); /* epoch 0 */
    DSP_CHECK_EQ_INT(0, dsp_proto_encode_event(DEVICE, EPOCH, NULL, buf, sizeof buf));
    DSP_CHECK_EQ_INT(0, dsp_proto_encode_event(DEVICE, EPOCH, &ok, NULL, 100));
    /* Too small a buffer: refused, never truncated JSON. */
    size_t full = dsp_proto_encode_event(DEVICE, EPOCH, &ok, buf, sizeof buf);
    char small[DSP_PROTO_BODY_MAX];
    DSP_CHECK_EQ_INT(0, dsp_proto_encode_event(DEVICE, EPOCH, &ok, small, full));
    DSP_CHECK_EQ_INT('\0', small[0]);
    DSP_CHECK_EQ_INT(full, dsp_proto_encode_event(DEVICE, EPOCH, &ok, small, full + 1));
}

/* ---- Classification ----------------------------------------------------------------- */

static dsp_http_outcome_t resp(uint16_t status, const char *body)
{
    dsp_http_outcome_t o;
    memset(&o, 0, sizeof o);
    o.phase = DSP_HTTP_RESPONSE;
    o.status = status;
    size_t len = body != NULL ? strlen(body) : 0;
    if (len > DSP_PROTO_RESPONSE_MAX) {
        len = DSP_PROTO_RESPONSE_MAX;
        o.body_truncated = true;
    }
    if (len > 0) {
        memcpy(o.body, body, len);
    }
    o.body_len = (uint16_t)len;
    return o;
}

typedef struct {
    uint16_t status;
    const char *body;
    dsp_tx_result_t result;
    uint8_t diag;
    uint8_t error;
    uint8_t field;
} case_t;

static void run_cases(const case_t *cases, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        dsp_http_outcome_t o = resp(cases[i].status, cases[i].body);
        dsp_tx_detail_t d;
        memset(&d, 0xEE, sizeof d);
        dsp_tx_result_t r = dsp_proto_classify(&o, ID42, &d);
        if (r != cases[i].result || d.diag != cases[i].diag || d.error != cases[i].error ||
            d.field != cases[i].field) {
            fprintf(stderr, "case %zu: %u %s -> result %d diag %u error %u field %u\n", i,
                    (unsigned)cases[i].status, cases[i].body ? cases[i].body : "(none)", (int)r,
                    (unsigned)d.diag, (unsigned)d.error, (unsigned)d.field);
        }
        DSP_CHECK_EQ_INT(cases[i].result, r);
        DSP_CHECK_EQ_INT(cases[i].diag, d.diag);
        DSP_CHECK_EQ_INT(cases[i].error, d.error);
        DSP_CHECK_EQ_INT(cases[i].field, d.field);
        DSP_CHECK_EQ_INT(cases[i].status, d.http_status);
        DSP_CHECK_EQ_INT(DSP_HTTP_RESPONSE, d.phase);
    }
}

static void test_classify_acknowledgements(void)
{
    const case_t cases[] = {
        /* The exact M1.6a production bodies (JSON.stringify, compact). */
        {201, "{\"status\":\"accepted\",\"event_id\":\"" ID42 "\"}", DSP_TX_ACK, 0, 0, 0},
        {200, "{\"status\":\"duplicate\",\"event_id\":\"" ID42 "\"}", DSP_TX_ACK, 0, 0, 0},
        /* Whitespace and key order do not matter; extra members are tolerated. */
        {201, " {\n \"event_id\" : \"" ID42 "\", \"status\":\"accepted\", \"n\": 1.5e3, \"x\":null } ",
         DSP_TX_ACK, 0, 0, 0},
        /* Anything less than strict confirmation: UNCERTAIN, never ACK. */
        {201, "{\"status\":\"duplicate\",\"event_id\":\"" ID42 "\"}", DSP_TX_UNCERTAIN,
         DSP_TXDIAG_PROTOCOL, 0, 0},
        {200, "{\"status\":\"accepted\",\"event_id\":\"" ID42 "\"}", DSP_TX_UNCERTAIN,
         DSP_TXDIAG_PROTOCOL, 0, 0},
        {200, "{\"status\":\"ok\",\"service\":\"dispenser-notify\",\"protocol\":1}", DSP_TX_UNCERTAIN,
         DSP_TXDIAG_PROTOCOL, 0, 0}, /* /health answered instead */
        {201, "{\"status\":\"accepted\",\"event_id\":\"dsp-0123456789ab-a5a5a5a5-43\"}",
         DSP_TX_UNCERTAIN, DSP_TXDIAG_PROTOCOL, 0, 0},
        {201, "{\"status\":\"accepted\",\"event_id\":\"dsp-0123456789ab-a5a5a5a5-4\"}",
         DSP_TX_UNCERTAIN, DSP_TXDIAG_PROTOCOL, 0, 0},
        {201, "{\"status\":\"accepted\"}", DSP_TX_UNCERTAIN, DSP_TXDIAG_PROTOCOL, 0, 0},
        {201, "", DSP_TX_UNCERTAIN, DSP_TXDIAG_PROTOCOL, DSP_TXERR_NONE, 0},
        {201, "<html>ok</html>", DSP_TX_UNCERTAIN, DSP_TXDIAG_PROTOCOL, DSP_TXERR_OTHER, 0},
        {201, "{\"status\":\"accepted\",\"event_id\":\"" ID42 "\"", DSP_TX_UNCERTAIN,
         DSP_TXDIAG_PROTOCOL, DSP_TXERR_OTHER, 0}, /* unterminated */
        {201, "{\"status\":\"accepted\",\"event_id\":\"" ID42 "\"} x", DSP_TX_UNCERTAIN,
         DSP_TXDIAG_PROTOCOL, DSP_TXERR_OTHER, 0}, /* trailing garbage */
        {201, "{\"status\":\"accepted\",\"status\":\"accepted\",\"event_id\":\"" ID42 "\"}",
         DSP_TX_UNCERTAIN, DSP_TXDIAG_PROTOCOL, DSP_TXERR_OTHER, 0}, /* duplicate key */
        {201, "{\"status\":\"accepted\",\"event_id\":\"dsp-0123456789ab-a5a5a5a5-4\\u0032\"}",
         DSP_TX_UNCERTAIN, DSP_TXDIAG_PROTOCOL, 0, 0}, /* escaped: never treated as equal */
        {201, "{\"status\":\"accepted\",\"event_id\":42}", DSP_TX_UNCERTAIN, DSP_TXDIAG_PROTOCOL,
         DSP_TXERR_OTHER, 0},
        {201, "[\"accepted\"]", DSP_TX_UNCERTAIN, DSP_TXDIAG_PROTOCOL, DSP_TXERR_OTHER, 0},
        /* Other 2xx: not an acknowledgement. */
        {202, "{\"status\":\"accepted\",\"event_id\":\"" ID42 "\"}", DSP_TX_FAILED, 0, 0, 0},
        {204, NULL, DSP_TX_FAILED, 0, 0, 0},
    };
    run_cases(cases, sizeof cases / sizeof cases[0]);
    /* Truncated body: never ACK even if the prefix looks right. */
    char big[DSP_PROTO_RESPONSE_MAX + 64];
    int w = snprintf(big, sizeof big, "{\"status\":\"accepted\",\"event_id\":\"" ID42 "\",\"pad\":\"");
    memset(big + w, 'a', sizeof big - (size_t)w - 3);
    memcpy(big + sizeof big - 3, "\"}", 3);
    dsp_http_outcome_t o = resp(201, big);
    DSP_CHECK(o.body_truncated);
    dsp_tx_detail_t d;
    DSP_CHECK_EQ_INT(DSP_TX_UNCERTAIN, dsp_proto_classify(&o, ID42, &d));
}

static void test_classify_r1_rules(void)
{
    const case_t cases[] = {
        /* 409: REJECTED only with the confirming conflict body for THIS event. */
        {409, "{\"status\":\"conflict\",\"event_id\":\"" ID42 "\"}", DSP_TX_REJECTED, 0,
         DSP_TXERR_CONFLICT, 0},
        {409, "{\"status\":\"conflict\",\"event_id\":\"dsp-0123456789ab-a5a5a5a5-7\"}", DSP_TX_FAILED,
         DSP_TXDIAG_PROTOCOL, DSP_TXERR_NONE, 0},
        {409, NULL, DSP_TX_FAILED, DSP_TXDIAG_PROTOCOL, DSP_TXERR_NONE, 0},
        /* 400: REJECTED only for invalid_field naming an event-content field. */
        {400, "{\"status\":\"invalid\",\"error\":\"invalid_field\",\"field\":\"value\"}",
         DSP_TX_REJECTED, 0, DSP_TXERR_INVALID_FIELD, DSP_TXFIELD_VALUE},
        {400, "{\"status\":\"invalid\",\"error\":\"invalid_field\",\"field\":\"created_at\"}",
         DSP_TX_REJECTED, 0, DSP_TXERR_INVALID_FIELD, DSP_TXFIELD_CREATED_AT},
        {400, "{\"status\":\"invalid\",\"error\":\"invalid_field\",\"field\":\"code\"}",
         DSP_TX_REJECTED, 0, DSP_TXERR_INVALID_FIELD, DSP_TXFIELD_CODE},
        /* ...ambiguous (same for every event) -> FAILED + PROTOCOL. */
        {400, "{\"status\":\"invalid\",\"error\":\"invalid_field\",\"field\":\"device_id\"}",
         DSP_TX_FAILED, DSP_TXDIAG_PROTOCOL, DSP_TXERR_INVALID_FIELD, DSP_TXFIELD_DEVICE_ID},
        {400, "{\"status\":\"invalid\",\"error\":\"invalid_field\",\"field\":\"event_id\"}",
         DSP_TX_FAILED, DSP_TXDIAG_PROTOCOL, DSP_TXERR_INVALID_FIELD, DSP_TXFIELD_EVENT_ID},
        {400, "{\"status\":\"invalid\",\"error\":\"invalid_field\",\"field\":\"attempt\"}",
         DSP_TX_FAILED, DSP_TXDIAG_PROTOCOL, DSP_TXERR_INVALID_FIELD, DSP_TXFIELD_ATTEMPT},
        {400, "{\"status\":\"invalid\",\"error\":\"invalid_field\",\"field\":\"body\"}",
         DSP_TX_FAILED, DSP_TXDIAG_PROTOCOL, DSP_TXERR_INVALID_FIELD, DSP_TXFIELD_BODY},
        {400, "{\"status\":\"invalid\",\"error\":\"invalid_field\",\"field\":\"zzz\"}", DSP_TX_FAILED,
         DSP_TXDIAG_PROTOCOL, DSP_TXERR_INVALID_FIELD, DSP_TXFIELD_OTHER},
        {400, "{\"status\":\"invalid\",\"error\":\"invalid_field\"}", DSP_TX_FAILED,
         DSP_TXDIAG_PROTOCOL, DSP_TXERR_INVALID_FIELD, DSP_TXFIELD_NONE},
        {400, "{\"status\":\"invalid\",\"error\":\"malformed_json\"}", DSP_TX_FAILED,
         DSP_TXDIAG_PROTOCOL, DSP_TXERR_MALFORMED_JSON, 0},
        {400, "{\"status\":\"invalid\",\"error\":\"unsupported_schema_version\"}", DSP_TX_FAILED,
         DSP_TXDIAG_PROTOCOL, DSP_TXERR_UNSUPPORTED_SCHEMA, 0},
        {400, "{\"status\":\"invalid\",\"error\":\"missing_field\",\"field\":\"value\"}",
         DSP_TX_FAILED, DSP_TXDIAG_PROTOCOL, DSP_TXERR_MISSING_FIELD, DSP_TXFIELD_VALUE},
        {400, "{\"status\":\"invalid\",\"error\":\"unknown_field\",\"field\":\"extra\"}",
         DSP_TX_FAILED, DSP_TXDIAG_PROTOCOL, DSP_TXERR_UNKNOWN_FIELD, DSP_TXFIELD_OTHER},
        {400, "{\"error\":\"invalid_field\",\"field\":\"value\"}", DSP_TX_FAILED,
         DSP_TXDIAG_PROTOCOL, DSP_TXERR_INVALID_FIELD, DSP_TXFIELD_VALUE}, /* no status */
        {400, "Bad Request", DSP_TX_FAILED, DSP_TXDIAG_PROTOCOL, DSP_TXERR_OTHER, 0},
        {400, NULL, DSP_TX_FAILED, DSP_TXDIAG_PROTOCOL, DSP_TXERR_NONE, 0},
        /* Systemic: FAILED with a persistent diagnostic, never REJECTED. */
        {401, "{\"status\":\"unauthorized\"}", DSP_TX_FAILED, DSP_TXDIAG_AUTH, DSP_TXERR_NONE, 0},
        {404, "{\"status\":\"invalid\",\"error\":\"not_found\"}", DSP_TX_FAILED,
         DSP_TXDIAG_ENDPOINT, DSP_TXERR_NOT_FOUND, 0},
        {405, "{\"status\":\"invalid\",\"error\":\"method_not_allowed\"}", DSP_TX_FAILED,
         DSP_TXDIAG_ENDPOINT, DSP_TXERR_METHOD_NOT_ALLOWED, 0},
        {413, "{\"status\":\"invalid\",\"error\":\"payload_too_large\"}", DSP_TX_FAILED,
         DSP_TXDIAG_PROTOCOL, DSP_TXERR_PAYLOAD_TOO_LARGE, 0},
        {415, "{\"status\":\"invalid\",\"error\":\"unsupported_media_type\"}", DSP_TX_FAILED,
         DSP_TXDIAG_PROTOCOL, DSP_TXERR_UNSUPPORTED_MEDIA, 0},
        {301, NULL, DSP_TX_FAILED, DSP_TXDIAG_ENDPOINT, 0, 0},
        {308, NULL, DSP_TX_FAILED, DSP_TXDIAG_ENDPOINT, 0, 0},
        /* Temporary: FAILED, no diagnostic. */
        {429, NULL, DSP_TX_FAILED, 0, 0, 0},
        {503, "{\"status\":\"retry\"}", DSP_TX_FAILED, 0, DSP_TXERR_NONE, 0},
        {500, NULL, DSP_TX_FAILED, 0, 0, 0},
        {502, "<html>bad gateway</html>", DSP_TX_FAILED, 0, DSP_TXERR_OTHER, 0},
        {403, NULL, DSP_TX_FAILED, 0, 0, 0},
        {408, NULL, DSP_TX_FAILED, 0, 0, 0},
        {418, NULL, DSP_TX_FAILED, 0, 0, 0},
        {100, NULL, DSP_TX_FAILED, 0, 0, 0},
        {0, NULL, DSP_TX_FAILED, 0, 0, 0},
    };
    run_cases(cases, sizeof cases / sizeof cases[0]);
}

static void test_classify_phases(void)
{
    struct {
        dsp_http_phase_t phase;
        dsp_tx_result_t result;
    } cases[] = {
        {DSP_HTTP_NOT_STARTED, DSP_TX_FAILED},
        {DSP_HTTP_CONNECT_FAILED, DSP_TX_FAILED}, /* nothing left the device */
        {DSP_HTTP_TLS_FAILED, DSP_TX_FAILED},     /* certificate rejected: nothing sent */
        {DSP_HTTP_SEND_FAILED, DSP_TX_UNCERTAIN}, /* part of the request may have left */
        {DSP_HTTP_NO_RESPONSE, DSP_TX_UNCERTAIN}, /* the backend may have stored it */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        dsp_http_outcome_t o;
        memset(&o, 0, sizeof o);
        o.phase = cases[i].phase;
        o.status = 201; /* ignored without a response */
        dsp_tx_detail_t d;
        DSP_CHECK_EQ_INT(cases[i].result, dsp_proto_classify(&o, ID42, &d));
        DSP_CHECK_EQ_INT(0, d.http_status);
        DSP_CHECK_EQ_INT(0, d.diag);
        DSP_CHECK_EQ_INT(cases[i].phase, d.phase);
    }
    dsp_http_outcome_t o = resp(201, "{}");
    DSP_CHECK_EQ_INT(DSP_TX_FAILED, dsp_proto_classify(NULL, ID42, NULL));
    DSP_CHECK_EQ_INT(DSP_TX_FAILED, dsp_proto_classify(&o, NULL, NULL));
    o = resp(201, "{\"status\":\"accepted\",\"event_id\":\"" ID42 "\"}");
    DSP_CHECK_EQ_INT(DSP_TX_ACK, dsp_proto_classify(&o, ID42, NULL)); /* detail optional */
}

static uint32_t rng = 20261116u;
static uint32_t rnd(uint32_t bound)
{
    rng = rng * 1664525u + 1013904223u;
    return (rng >> 8) % bound;
}

/* Random and mutated bodies never crash and never produce ACK or REJECTED unless the body
 * really is the strict confirmation / conflict for this event. */
static void test_classify_fuzz(void)
{
    const char *seeds[] = {
        "{\"status\":\"accepted\",\"event_id\":\"" ID42 "\"}",
        "{\"status\":\"conflict\",\"event_id\":\"" ID42 "\"}",
        "{\"status\":\"invalid\",\"error\":\"invalid_field\",\"field\":\"value\"}",
    };
    const char alphabet[] = "{}[]\":,\\ \n\tabcdeftrunl0123456789.-+eE\x01\x7f\xc3";
    int unexpected = 0;
    for (int iter = 0; iter < 20000; iter++) {
        char body[DSP_PROTO_RESPONSE_MAX];
        size_t len;
        size_t s = rnd(3);
        if (rnd(2) == 0) {
            len = strlen(seeds[s]);
            memcpy(body, seeds[s], len);
            for (uint32_t k = 0, m = 1 + rnd(4); k < m; k++) {
                body[rnd((uint32_t)len)] = alphabet[rnd(sizeof alphabet - 1)];
            }
        } else {
            len = rnd(sizeof body);
            for (size_t k = 0; k < len; k++) {
                body[k] = alphabet[rnd(sizeof alphabet - 1)];
            }
        }
        uint16_t statuses[] = {200, 201, 400, 409};
        uint16_t status = statuses[rnd(4)];
        dsp_http_outcome_t o;
        memset(&o, 0, sizeof o);
        o.phase = DSP_HTTP_RESPONSE;
        o.status = status;
        memcpy(o.body, body, len);
        o.body_len = (uint16_t)len;
        dsp_tx_detail_t d;
        dsp_tx_result_t r = dsp_proto_classify(&o, ID42, &d);
        bool exact_ack = (status == 201 && len == strlen(seeds[0]) && memcmp(body, seeds[0], len) == 0);
        bool exact_conflict = (status == 409 && len == strlen(seeds[1]) && memcmp(body, seeds[1], len) == 0);
        if (r == DSP_TX_ACK && !exact_ack) {
            /* Only semantically identical bodies may ACK (e.g. whitespace mutations). */
            unexpected += strstr(body, "accepted") == NULL;
        }
        if (r == DSP_TX_REJECTED && status == 409 && !exact_conflict) {
            unexpected += strstr(body, "conflict") == NULL;
        }
        DSP_CHECK(r != DSP_TX_REJECTED || status == 400 || status == 409);
        DSP_CHECK(r != DSP_TX_ACK || status == 200 || status == 201);
        DSP_CHECK(d.error <= DSP_TXERR_MAX && d.field <= DSP_TXFIELD_MAX);
    }
    DSP_CHECK_EQ_INT(0, unexpected);
}

int main(void)
{
    DSP_RUN(test_encode_golden);
    DSP_RUN(test_encode_refuses_invalid);
    DSP_RUN(test_classify_acknowledgements);
    DSP_RUN(test_classify_r1_rules);
    DSP_RUN(test_classify_phases);
    DSP_RUN(test_classify_fuzz);
    return dsp_test_finish();
}
