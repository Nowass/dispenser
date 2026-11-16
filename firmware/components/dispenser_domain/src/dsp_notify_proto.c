#include "dsp_notify_proto.h"

#include <stdio.h>
#include <string.h>

#include "dsp_timekeeping.h"

/* ---- Encoder ------------------------------------------------------------------------ */

static const char *type_name(uint8_t type)
{
    switch (type) {
    case DSP_NOTE_RESERVOIR_LOW:
        return "RESERVOIR_LOW";
    case DSP_NOTE_RESERVOIR_BLOCKED:
        return "RESERVOIR_BLOCKED";
    case DSP_NOTE_BATTERY_LOW:
        return "BATTERY_LOW";
    case DSP_NOTE_DOSING_ERROR:
        return "DOSING_ERROR";
    case DSP_NOTE_DEVICE_ERROR:
        return "DEVICE_ERROR";
    default:
        return NULL;
    }
}

static uint8_t expected_severity(uint8_t type)
{
    return (type == DSP_NOTE_RESERVOIR_LOW || type == DSP_NOTE_BATTERY_LOW) ? DSP_SEV_WARNING
                                                                             : DSP_SEV_CRITICAL;
}

/* Code name, or "" for "no code" (JSON null); NULL if the code is invalid for the type. */
static const char *code_name(uint8_t type, uint16_t code)
{
    if (type == DSP_NOTE_DOSING_ERROR) {
        static const char *const names[] = {NULL, "START_FAILED", "PUMP_FAILED", "INTERRUPTED",
                                            "STORAGE", "BLOCKED"};
        return code >= 1 && code <= DSP_DOSEERR_MAX ? names[code] : NULL;
    }
    if (type == DSP_NOTE_DEVICE_ERROR) {
        static const char *const names[] = {NULL, "STORAGE", "TIME_SYNC", "CLOCK", "QUEUE_OVERFLOW"};
        return code >= DSP_DEVERR_STORAGE && code <= DSP_DEVERR_QUEUE_OVERFLOW ? names[code] : NULL;
    }
    return code == 0 ? "" : NULL;
}

static bool one_shot(uint8_t type, uint16_t code)
{
    return type == DSP_NOTE_DOSING_ERROR ||
           (type == DSP_NOTE_DEVICE_ERROR && code == DSP_DEVERR_QUEUE_OVERFLOW);
}

size_t dsp_proto_encode_event(uint64_t device_id, uint32_t epoch, const dsp_note_t *note,
                              char *buf, size_t cap)
{
    if (buf != NULL && cap > 0) {
        buf[0] = '\0';
    }
    if (buf == NULL || note == NULL || epoch == 0 || note->seq == 0 || note->attempts == 0) {
        return 0;
    }
    const char *type = type_name(note->type);
    const char *code = type != NULL ? code_name(note->type, note->code) : NULL;
    if (type == NULL || code == NULL || note->severity != expected_severity(note->type)) {
        return 0;
    }
    if (note->reminder_no > 0 && one_shot(note->type, note->code)) {
        return 0;
    }
    bool manual = (note->flags & DSP_NOTE_F_MANUAL) != 0;
    if (manual && note->type != DSP_NOTE_DOSING_ERROR) {
        return 0;
    }
    bool time_valid = (note->flags & DSP_NOTE_F_TIME_VALID) != 0;
    if (time_valid &&
        (note->created_at < DSP_TIME_PLAUSIBLE_MIN || note->created_at > DSP_UTC_MAX)) {
        return 0; /* outside the protocol range: never sent as something else */
    }
    char id[DSP_NOTIFY_ID_BUF_LEN];
    if (!dsp_notify_format_id(device_id, epoch, note->seq, id, sizeof id)) {
        return 0;
    }
    char code_json[24];
    char created_json[24];
    int w;
    if (code[0] == '\0') {
        w = snprintf(code_json, sizeof code_json, "null");
    } else {
        w = snprintf(code_json, sizeof code_json, "\"%s\"", code);
    }
    if (w < 0 || (size_t)w >= sizeof code_json) {
        return 0;
    }
    if (time_valid) {
        w = snprintf(created_json, sizeof created_json, "%lld", (long long)note->created_at);
    } else {
        w = snprintf(created_json, sizeof created_json, "null");
    }
    if (w < 0 || (size_t)w >= sizeof created_json) {
        return 0;
    }
    w = snprintf(buf, cap,
                 "{\"schema_version\":1,\"event_id\":\"%s\",\"device_id\":\"%012llx\","
                 "\"type\":\"%s\",\"severity\":\"%s\",\"code\":%s,\"reminder_no\":%u,"
                 "\"created_at\":%s,\"uncertain\":%s,\"manual\":%s,\"value\":%lu,"
                 "\"value2\":%lu,\"attempt\":%u}",
                 id, (unsigned long long)(device_id & 0xFFFFFFFFFFFFull), type,
                 note->severity == DSP_SEV_WARNING ? "WARNING" : "CRITICAL", code_json,
                 (unsigned)note->reminder_no, created_json,
                 (note->flags & DSP_NOTE_F_UNCERTAIN) ? "true" : "false",
                 manual ? "true" : "false", (unsigned long)note->value,
                 (unsigned long)note->value2, (unsigned)note->attempts);
    if (w <= 0 || (size_t)w >= cap) {
        if (cap > 0) {
            buf[0] = '\0';
        }
        return 0;
    }
    return (size_t)w;
}

/* ---- Strict, bounded parser for the backend's flat JSON objects ---------------------- */

#define STR_MAX 64

typedef struct {
    bool present;
    bool simple;          /* plain printable ASCII without escapes, <= STR_MAX */
    char text[STR_MAX + 1];
} jstr_t;

typedef struct {
    jstr_t status;
    jstr_t event_id;
    jstr_t error;
    jstr_t field;
} resp_t;

typedef struct {
    const char *p;
    const char *end;
} cursor_t;

static void ws(cursor_t *c)
{
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')) {
        c->p++;
    }
}

static bool hexdig(char ch)
{
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

/* Parses a JSON string at c->p (which must be '"'). out may be NULL (skip). */
static bool parse_string(cursor_t *c, jstr_t *out)
{
    if (c->p >= c->end || *c->p != '"') {
        return false;
    }
    c->p++;
    size_t n = 0;
    bool simple = true;
    while (c->p < c->end) {
        char ch = *c->p++;
        if (ch == '"') {
            if (out != NULL) {
                out->text[n < STR_MAX ? n : STR_MAX] = '\0';
                out->simple = simple && n <= STR_MAX;
            }
            return true;
        }
        if ((unsigned char)ch < 0x20) {
            return false; /* control characters must be escaped */
        }
        if (ch == '\\') {
            simple = false;
            if (c->p >= c->end) {
                return false;
            }
            char esc = *c->p++;
            if (esc == 'u') {
                for (int i = 0; i < 4; i++) {
                    if (c->p >= c->end || !hexdig(*c->p)) {
                        return false;
                    }
                    c->p++;
                }
            } else if (strchr("\"\\/bfnrt", esc) == NULL) {
                return false;
            }
            ch = '?';
        } else if ((unsigned char)ch > 0x7E) {
            simple = false;
        }
        if (out != NULL && n < STR_MAX) {
            out->text[n] = ch;
        }
        n++;
    }
    return false; /* unterminated */
}

static bool parse_literal(cursor_t *c, const char *lit)
{
    size_t len = strlen(lit);
    if ((size_t)(c->end - c->p) < len || memcmp(c->p, lit, len) != 0) {
        return false;
    }
    c->p += len;
    return true;
}

static bool parse_number(cursor_t *c)
{
    const char *start = c->p;
    if (c->p < c->end && *c->p == '-') {
        c->p++;
    }
    bool digits = false;
    while (c->p < c->end && ((*c->p >= '0' && *c->p <= '9') || *c->p == '.' || *c->p == 'e' ||
                             *c->p == 'E' || *c->p == '+' || *c->p == '-')) {
        digits |= (*c->p >= '0' && *c->p <= '9');
        c->p++;
    }
    return digits && c->p > start;
}

/* Parses a flat object whose values are strings, numbers, booleans or null. Duplicate keys
 * of interest make the body ambiguous (rejected). */
static bool parse_object(const char *body, size_t len, resp_t *out)
{
    memset(out, 0, sizeof *out);
    cursor_t c = {body, body + len};
    ws(&c);
    if (c.p >= c.end || *c.p != '{') {
        return false;
    }
    c.p++;
    ws(&c);
    if (c.p < c.end && *c.p == '}') {
        c.p++;
    } else {
        for (;;) {
            jstr_t key;
            memset(&key, 0, sizeof key);
            ws(&c);
            if (!parse_string(&c, &key)) {
                return false;
            }
            ws(&c);
            if (c.p >= c.end || *c.p != ':') {
                return false;
            }
            c.p++;
            ws(&c);
            jstr_t *slot = NULL;
            if (key.simple) {
                if (strcmp(key.text, "status") == 0) {
                    slot = &out->status;
                } else if (strcmp(key.text, "event_id") == 0) {
                    slot = &out->event_id;
                } else if (strcmp(key.text, "error") == 0) {
                    slot = &out->error;
                } else if (strcmp(key.text, "field") == 0) {
                    slot = &out->field;
                }
            }
            if (slot != NULL && slot->present) {
                return false; /* duplicate key: ambiguous */
            }
            if (c.p < c.end && *c.p == '"') {
                jstr_t value;
                memset(&value, 0, sizeof value);
                if (!parse_string(&c, &value)) {
                    return false;
                }
                if (slot != NULL) {
                    *slot = value;
                    slot->present = true;
                }
            } else {
                if (slot != NULL) {
                    return false; /* the fields of interest are strings */
                }
                if (!parse_literal(&c, "true") && !parse_literal(&c, "false") &&
                    !parse_literal(&c, "null") && !parse_number(&c)) {
                    return false;
                }
            }
            ws(&c);
            if (c.p < c.end && *c.p == ',') {
                c.p++;
                continue;
            }
            if (c.p < c.end && *c.p == '}') {
                c.p++;
                break;
            }
            return false;
        }
    }
    ws(&c);
    return c.p == c.end; /* nothing after the object */
}

static bool is(const jstr_t *s, const char *text)
{
    return s->present && s->simple && strcmp(s->text, text) == 0;
}

static uint8_t error_enum(const jstr_t *s)
{
    static const struct {
        const char *name;
        uint8_t value;
    } map[] = {
        {"malformed_json", DSP_TXERR_MALFORMED_JSON},
        {"unsupported_schema_version", DSP_TXERR_UNSUPPORTED_SCHEMA},
        {"missing_field", DSP_TXERR_MISSING_FIELD},
        {"unknown_field", DSP_TXERR_UNKNOWN_FIELD},
        {"invalid_field", DSP_TXERR_INVALID_FIELD},
        {"payload_too_large", DSP_TXERR_PAYLOAD_TOO_LARGE},
        {"unsupported_media_type", DSP_TXERR_UNSUPPORTED_MEDIA},
        {"not_found", DSP_TXERR_NOT_FOUND},
        {"method_not_allowed", DSP_TXERR_METHOD_NOT_ALLOWED},
    };
    if (!s->present) {
        return DSP_TXERR_NONE;
    }
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        if (is(s, map[i].name)) {
            return map[i].value;
        }
    }
    return DSP_TXERR_OTHER;
}

static uint8_t field_enum(const jstr_t *s)
{
    static const char *const names[] = {
        NULL,          "schema_version", "event_id", "device_id", "type",   "severity",
        "code",        "reminder_no",    "created_at", "uncertain", "manual", "value",
        "value2",      "attempt",        "body",
    };
    if (!s->present) {
        return DSP_TXFIELD_NONE;
    }
    for (uint8_t i = 1; i < sizeof names / sizeof names[0]; i++) {
        if (is(s, names[i])) {
            return i;
        }
    }
    return DSP_TXFIELD_OTHER;
}

/* Fields whose invalidity is a property of this event's content (not of every request). */
static bool event_content_field(uint8_t field)
{
    switch (field) {
    case DSP_TXFIELD_TYPE:
    case DSP_TXFIELD_SEVERITY:
    case DSP_TXFIELD_CODE:
    case DSP_TXFIELD_REMINDER_NO:
    case DSP_TXFIELD_CREATED_AT:
    case DSP_TXFIELD_UNCERTAIN:
    case DSP_TXFIELD_MANUAL:
    case DSP_TXFIELD_VALUE:
    case DSP_TXFIELD_VALUE2:
        return true;
    default:
        return false;
    }
}

dsp_tx_result_t dsp_proto_classify(const dsp_http_outcome_t *o, const char *expected_event_id,
                                   dsp_tx_detail_t *detail)
{
    dsp_tx_detail_t scratch;
    dsp_tx_detail_t *d = detail != NULL ? detail : &scratch;
    memset(d, 0, sizeof *d);
    if (o == NULL || expected_event_id == NULL) {
        d->diag = DSP_TXDIAG_PROTOCOL;
        return DSP_TX_FAILED;
    }
    d->phase = (uint8_t)o->phase;
    switch (o->phase) {
    case DSP_HTTP_SEND_FAILED:
    case DSP_HTTP_NO_RESPONSE:
        return DSP_TX_UNCERTAIN; /* the backend may have stored it: same ID next time */
    case DSP_HTTP_RESPONSE:
        break;
    default:
        return DSP_TX_FAILED; /* nothing left the device */
    }
    uint16_t status = o->status;
    d->http_status = status;
    resp_t r;
    size_t len = o->body_len <= DSP_PROTO_RESPONSE_MAX ? o->body_len : DSP_PROTO_RESPONSE_MAX;
    bool parsed = !o->body_truncated && parse_object(o->body, len, &r);
    if (!parsed) {
        memset(&r, 0, sizeof r);
    }
    d->error = parsed ? error_enum(&r.error) : (len > 0 ? DSP_TXERR_OTHER : DSP_TXERR_NONE);
    d->field = parsed ? field_enum(&r.field) : DSP_TXFIELD_NONE;
    bool id_matches = parsed && is(&r.event_id, expected_event_id);

    if (status == 200 || status == 201) {
        if (id_matches && is(&r.status, status == 201 ? "accepted" : "duplicate")) {
            d->error = DSP_TXERR_NONE;
            d->field = DSP_TXFIELD_NONE;
            return DSP_TX_ACK;
        }
        d->diag = DSP_TXDIAG_PROTOCOL; /* never ACK without strict confirmation */
        return DSP_TX_UNCERTAIN;
    }
    if (status == 409) {
        if (id_matches && is(&r.status, "conflict")) {
            d->error = DSP_TXERR_CONFLICT;
            d->field = DSP_TXFIELD_NONE;
            return DSP_TX_REJECTED;
        }
        d->diag = DSP_TXDIAG_PROTOCOL;
        return DSP_TX_FAILED;
    }
    if (status == 400) {
        if (parsed && is(&r.status, "invalid") && d->error == DSP_TXERR_INVALID_FIELD &&
            event_content_field(d->field)) {
            return DSP_TX_REJECTED;
        }
        d->diag = DSP_TXDIAG_PROTOCOL; /* ambiguous: the same for every event */
        return DSP_TX_FAILED;
    }
    if (status == 401) {
        d->diag = DSP_TXDIAG_AUTH;
        return DSP_TX_FAILED;
    }
    if (status == 404 || status == 405 || (status >= 300 && status <= 399)) {
        d->diag = DSP_TXDIAG_ENDPOINT;
        return DSP_TX_FAILED;
    }
    if (status == 413 || status == 415) {
        d->diag = DSP_TXDIAG_PROTOCOL;
        return DSP_TX_FAILED;
    }
    return DSP_TX_FAILED; /* 429, 5xx and anything else: keep pending, no diagnostic */
}
