#ifndef DSP_NOTIFY_PROTO_H
#define DSP_NOTIFY_PROTO_H

/*
 * Notification protocol v1 (milestone M1.7), pure functions: the exact JSON request body
 * for one queued notification and the classification of the backend's response into a
 * transport result. Contract: cloudflare/dispenser-notify/PROTOCOL.md. No I/O, no
 * allocation, no ESP-IDF.
 *
 * Classification (approved 2026-11-16, R1): the priority is that a systemic configuration
 * or protocol problem can never drain the queue. Only responses that unambiguously concern
 * THIS event are REJECTED (409 conflict, 400 invalid_field naming an event-content field);
 * everything that is the same for every event (401, 404, 405, 413, 415, other 400s) is
 * FAILED with a persistent diagnostic, so the queue is kept until the problem is fixed.
 */

#include <stddef.h>
#include <stdint.h>

#include "dsp_notify.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_PROTO_BODY_MAX 512     /* a v1 body is about 300 bytes */
#define DSP_PROTO_RESPONSE_MAX 256 /* bytes of the response body that are examined */

/*
 * Encodes the protocol-v1 request body for note (fields in the documented order):
 * {"schema_version":1,"event_id":…,"device_id":…,"type":…,"severity":…,"code":…,
 *  "reminder_no":…,"created_at":…,"uncertain":…,"manual":…,"value":…,"value2":…,
 *  "attempt":…}
 * device_id: the 48-bit factory MAC. Returns the length (without NUL) or 0 if note is not
 * a valid, already started entry (seq/epoch 0, unknown type/code, severity mismatch,
 * reminder on a one-shot event, MANUAL outside DOSING_ERROR, created_at outside the
 * protocol range, attempts 0) or cap is too small. buf is always NUL-terminated if cap > 0.
 */
size_t dsp_proto_encode_event(uint64_t device_id, uint32_t epoch, const dsp_note_t *note,
                              char *buf, size_t cap);

/* How far one HTTP exchange got (decides FAILED vs UNCERTAIN when there is no response). */
typedef enum {
    DSP_HTTP_NOT_STARTED = 0, /* nothing attempted (e.g. port not open) */
    DSP_HTTP_CONNECT_FAILED,  /* DNS/TCP failure: nothing sent */
    DSP_HTTP_TLS_FAILED,      /* TLS handshake or certificate verification failed: nothing sent */
    DSP_HTTP_SEND_FAILED,     /* writing the request failed: part of it may have left */
    DSP_HTTP_NO_RESPONSE,     /* request sent, no (complete) status line: timeout or reset */
    DSP_HTTP_RESPONSE,        /* status (and possibly a body) received */
} dsp_http_phase_t;

typedef struct {
    dsp_http_phase_t phase;
    uint16_t status;                    /* valid when phase == DSP_HTTP_RESPONSE */
    char body[DSP_PROTO_RESPONSE_MAX];  /* first bytes of the body (not NUL-terminated) */
    uint16_t body_len;
    bool body_truncated;                /* the body was longer than DSP_PROTO_RESPONSE_MAX */
} dsp_http_outcome_t;

/*
 * Maps one exchange to a transport result; fills *detail (status, closed error/field
 * enums, diagnostics to set, phase). expected_event_id: the ID that was sent.
 *
 *   201 + {"status":"accepted","event_id":<expected>}   ACK
 *   200 + {"status":"duplicate","event_id":<expected>}  ACK
 *   200/201 otherwise (missing/malformed/mismatched)    UNCERTAIN (+PROTOCOL)
 *   409 + {"status":"conflict","event_id":<expected>}   REJECTED (CONFLICT)
 *   400 + {"status":"invalid","error":"invalid_field","field":<event-content field>}
 *                                                       REJECTED (INVALID_FIELD, field)
 *   400 otherwise (ambiguous)                           FAILED (+PROTOCOL)
 *   401                                                 FAILED (+AUTH)
 *   404, 405, 3xx (redirects are never followed)        FAILED (+ENDPOINT)
 *   413, 415, 409 without the confirming body           FAILED (+PROTOCOL)
 *   429, 5xx, any other status                          FAILED
 *   connect/TLS failure (nothing sent)                  FAILED
 *   send failure or no response after sending           UNCERTAIN
 * Event-content fields: type, severity, code, reminder_no, created_at, uncertain, manual,
 * value, value2. (event_id, device_id, schema_version, attempt, body and unknown names are
 * the same for every event: ambiguous -> FAILED.)
 */
dsp_tx_result_t dsp_proto_classify(const dsp_http_outcome_t *o, const char *expected_event_id,
                                   dsp_tx_detail_t *detail);

#ifdef __cplusplus
}
#endif

#endif /* DSP_NOTIFY_PROTO_H */
