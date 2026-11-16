/*
 * Shared notification protocol v1 vectors (M1.8): ../../test-vectors/notification_protocol_v1.txt,
 * also consumed by the backend's Vitest suite. The firmware encoder must produce exactly the
 * vector bodies, refuse the "noencode" notes, and classify every backend response as listed.
 * Expected values come from an independent generator, never from this implementation.
 */
#include "dsp_notify_proto.h"
#include "dsp_test.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DSP_VECTORS_DIR
#error "DSP_VECTORS_DIR must be defined by the build"
#endif

#define LINE_MAX_LEN 2048
#define MAX_TOKENS 32

typedef struct {
    char *key[MAX_TOKENS];
    char *val[MAX_TOKENS];
    int n;
} rec_t;

static const char *get(const rec_t *r, const char *key)
{
    for (int i = 0; i < r->n; i++) {
        if (strcmp(r->key[i], key) == 0) {
            return r->val[i];
        }
    }
    fprintf(stderr, "missing key %s\n", key);
    DSP_CHECK(0);
    return "";
}

static unsigned long long num(const rec_t *r, const char *key, int base)
{
    return strtoull(get(r, key), NULL, base);
}

static uint8_t type_enum(const char *s)
{
    const char *names[] = {"", "RESERVOIR_LOW", "RESERVOIR_BLOCKED", "BATTERY_LOW", "DOSING_ERROR",
                           "DEVICE_ERROR"};
    for (uint8_t i = 1; i < 6; i++) {
        if (strcmp(s, names[i]) == 0) {
            return i;
        }
    }
    return 99;
}

static uint16_t code_enum(uint8_t type, const char *s)
{
    if (strcmp(s, "-") == 0) {
        return 0;
    }
    if (s[0] >= '0' && s[0] <= '9') {
        return (uint16_t)strtoul(s, NULL, 10); /* numeric (invalid) code in noencode vectors */
    }
    const char *dose[] = {"", "START_FAILED", "PUMP_FAILED", "INTERRUPTED", "STORAGE", "BLOCKED"};
    const char *dev[] = {"", "STORAGE", "TIME_SYNC", "CLOCK", "QUEUE_OVERFLOW"};
    const char **names = type == DSP_NOTE_DOSING_ERROR ? dose : dev;
    int count = type == DSP_NOTE_DOSING_ERROR ? 6 : 5;
    for (int i = 1; i < count; i++) {
        if (strcmp(s, names[i]) == 0) {
            return (uint16_t)i;
        }
    }
    return 999;
}

static dsp_note_t note_of(const rec_t *r, uint64_t *device, uint32_t *epoch)
{
    dsp_note_t e;
    memset(&e, 0, sizeof e);
    *device = num(r, "device", 16);
    *epoch = (uint32_t)num(r, "epoch", 16);
    e.seq = (uint32_t)num(r, "seq", 10);
    e.type = type_enum(get(r, "type"));
    e.severity = strcmp(get(r, "severity"), "WARNING") == 0 ? DSP_SEV_WARNING : DSP_SEV_CRITICAL;
    e.code = code_enum(e.type, get(r, "code"));
    e.reminder_no = (uint8_t)num(r, "reminder", 10);
    const char *created = get(r, "created");
    if (strcmp(created, "-") != 0) {
        e.created_at = (dsp_utc_t)strtoll(created, NULL, 10);
        e.flags |= DSP_NOTE_F_TIME_VALID;
    }
    if (e.reminder_no > 0) {
        e.flags |= DSP_NOTE_F_REMINDER;
    }
    if (num(r, "uncertain", 10)) {
        e.flags |= DSP_NOTE_F_UNCERTAIN;
    }
    if (num(r, "manual", 10)) {
        e.flags |= DSP_NOTE_F_MANUAL;
    }
    e.value = (uint32_t)num(r, "value", 10);
    e.value2 = (uint32_t)num(r, "value2", 10);
    e.attempts = (uint16_t)num(r, "attempt", 10);
    return e;
}

static dsp_tx_result_t result_enum(const char *s)
{
    if (strcmp(s, "ACK") == 0) {
        return DSP_TX_ACK;
    }
    if (strcmp(s, "REJECTED") == 0) {
        return DSP_TX_REJECTED;
    }
    if (strcmp(s, "UNCERTAIN") == 0) {
        return DSP_TX_UNCERTAIN;
    }
    return DSP_TX_FAILED;
}

static uint8_t diag_enum(const char *s)
{
    if (strcmp(s, "AUTH") == 0) {
        return DSP_TXDIAG_AUTH;
    }
    if (strcmp(s, "ENDPOINT") == 0) {
        return DSP_TXDIAG_ENDPOINT;
    }
    if (strcmp(s, "PROTOCOL") == 0) {
        return DSP_TXDIAG_PROTOCOL;
    }
    return 0;
}

static uint8_t error_enum(const char *s)
{
    const char *names[] = {"-",
                           "malformed_json",
                           "unsupported_schema_version",
                           "missing_field",
                           "unknown_field",
                           "invalid_field",
                           "payload_too_large",
                           "unsupported_media_type",
                           "not_found",
                           "method_not_allowed",
                           "conflict"};
    for (uint8_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (strcmp(s, names[i]) == 0) {
            return i; /* same order as dsp_txerr_t */
        }
    }
    return DSP_TXERR_OTHER;
}

static uint8_t field_enum(const char *s)
{
    const char *names[] = {"-",          "schema_version", "event_id", "device_id", "type",
                           "severity",   "code",           "reminder_no", "created_at", "uncertain",
                           "manual",     "value",          "value2",   "attempt",   "body"};
    for (uint8_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (strcmp(s, names[i]) == 0) {
            return i; /* same order as dsp_txfield_t */
        }
    }
    return DSP_TXFIELD_OTHER; /* "-other-" */
}

static void check_classification(const rec_t *r, const char *body, const char *sent)
{
    dsp_http_outcome_t o;
    memset(&o, 0, sizeof o);
    o.phase = DSP_HTTP_RESPONSE;
    o.status = (uint16_t)num(r, "status", 10);
    if (strcmp(body, "-") != 0) {
        size_t len = strlen(body);
        DSP_CHECK(len <= DSP_PROTO_RESPONSE_MAX);
        memcpy(o.body, body, len);
        o.body_len = (uint16_t)len;
    }
    dsp_tx_detail_t d;
    dsp_tx_result_t res = dsp_proto_classify(&o, sent, &d);
    const char *name = get(r, "name");
    if (res != result_enum(get(r, "result")) || d.diag != diag_enum(get(r, "diag"))) {
        fprintf(stderr, "vector %s: result %d diag %u\n", name, (int)res, (unsigned)d.diag);
    }
    DSP_CHECK_EQ_INT(result_enum(get(r, "result")), res);
    DSP_CHECK_EQ_INT(diag_enum(get(r, "diag")), d.diag);
    DSP_CHECK_EQ_INT(o.status, d.http_status);
    if (res == DSP_TX_REJECTED || res == DSP_TX_FAILED) {
        DSP_CHECK_EQ_INT(error_enum(get(r, "error")), d.error);
        DSP_CHECK_EQ_INT(field_enum(get(r, "field")), d.field);
    }
}

static int counts[4];

static void run_record(char *line)
{
    rec_t r;
    memset(&r, 0, sizeof r);
    char *kind = strtok(line, " \n"); /* ISO C11 (single-threaded test) */
    for (char *tok = strtok(NULL, " \n"); tok != NULL && r.n < MAX_TOKENS;
         tok = strtok(NULL, " \n")) {
        char *eq = strchr(tok, '=');
        DSP_CHECK(eq != NULL);
        if (eq == NULL) {
            return;
        }
        *eq = '\0';
        r.key[r.n] = tok;
        r.val[r.n] = eq + 1;
        r.n++;
    }
    const char *name = get(&r, "name");
    if (strcmp(kind, "encode") == 0) {
        counts[0]++;
        uint64_t device;
        uint32_t epoch;
        dsp_note_t e = note_of(&r, &device, &epoch);
        char buf[DSP_PROTO_BODY_MAX];
        size_t len = dsp_proto_encode_event(device, epoch, &e, buf, sizeof buf);
        if (strcmp(buf, get(&r, "body")) != 0) {
            fprintf(stderr, "vector %s:\n  got  %s\n  want %s\n", name, buf, get(&r, "body"));
        }
        DSP_CHECK_EQ_STR(get(&r, "body"), buf);
        DSP_CHECK_EQ_INT(strlen(get(&r, "body")), len);
        /* The canonical payload is the body without schema_version and attempt. */
        char canon[DSP_PROTO_BODY_MAX];
        const char *prefix = "{\"schema_version\":1,";
        DSP_CHECK(strncmp(buf, prefix, strlen(prefix)) == 0);
        snprintf(canon, sizeof canon, "{%s", buf + strlen(prefix));
        char *att = strstr(canon, ",\"attempt\":");
        DSP_CHECK(att != NULL);
        if (att != NULL) {
            strcpy(att, "}");
        }
        DSP_CHECK_EQ_STR(get(&r, "canonical"), canon);
    } else if (strcmp(kind, "noencode") == 0) {
        counts[1]++;
        uint64_t device;
        uint32_t epoch;
        dsp_note_t e = note_of(&r, &device, &epoch);
        char buf[DSP_PROTO_BODY_MAX];
        size_t len = dsp_proto_encode_event(device, epoch, &e, buf, sizeof buf);
        if (len != 0) {
            fprintf(stderr, "vector %s: encoded %s\n", name, buf);
        }
        DSP_CHECK_EQ_INT(0, len);
    } else if (strcmp(kind, "request") == 0) {
        counts[2]++;
        /* The firmware's reaction to the backend's exact answer for this request. */
        check_classification(&r, get(&r, "response"), "dsp-0123456789ab-a5a5a5a5-42");
    } else if (strcmp(kind, "response") == 0) {
        counts[3]++;
        check_classification(&r, get(&r, "body"), get(&r, "sent"));
    } else {
        fprintf(stderr, "unknown record %s\n", kind);
        DSP_CHECK(0);
    }
}

static void test_protocol_vectors(void)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", DSP_VECTORS_DIR, "notification_protocol_v1.txt");
    FILE *f = fopen(path, "r");
    DSP_CHECK(f != NULL);
    if (f == NULL) {
        return;
    }
    char line[LINE_MAX_LEN];
    while (fgets(line, sizeof line, f) != NULL) {
        DSP_CHECK(strchr(line, '\n') != NULL || feof(f)); /* no truncated line */
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') {
            continue;
        }
        run_record(line);
    }
    fclose(f);
    printf("vectors: %d encode, %d noencode, %d request, %d response\n", counts[0], counts[1],
           counts[2], counts[3]);
    DSP_CHECK(counts[0] >= 16 && counts[1] >= 13 && counts[2] >= 13 && counts[3] >= 19);
}

int main(void)
{
    DSP_RUN(test_protocol_vectors);
    return dsp_test_finish();
}
