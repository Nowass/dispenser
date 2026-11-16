#ifndef DSP_HTTPS_TRANSPORT_H
#define DSP_HTTPS_TRANSPORT_H

/*
 * HTTPS notification transport (milestone M1.7). Pure domain code: implements the M1.5
 * dsp_notify_transport_t over an injected HTTP port (ESP-IDF esp_http_client on the device,
 * fakes in host tests), using the protocol-v1 encoder and classifier (dsp_notify_proto.h).
 *
 *  - TLS, certificate verification, URL and timeouts belong to the port. The port must
 *    never follow redirects, never disable certificate verification and never log headers.
 *  - The API token is fetched from the provider for each request into a buffer that is
 *    wiped right after the request. It is never logged, never part of the body.
 *  - Session budget: an entry is started only while its worst case still fits before the
 *    session deadline (can_send). Entries that are not started stay untouched in the queue.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dsp_net.h"
#include "dsp_notify.h"
#include "dsp_notify_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_API_TOKEN_MIN_LEN 32  /* backend rule for DEVICE_API_TOKEN */
#define DSP_API_TOKEN_MAX_LEN 512
#define DSP_HTTPS_REQUEST_WORST_MS_DEFAULT 20000u /* connect+TLS 10 s + response 10 s */

/* HTTP port (platform). One port instance serves one session at a time. */
typedef struct {
    void *ctx;
    /* Starts a session (may prepare a client; network I/O optional). false = unusable. */
    bool (*open)(void *ctx);
    /* One POST of body (application/json) with "Authorization: Bearer <token>". The port
     * must fill *out completely (phase, status, body prefix). Bounded by its timeouts. */
    void (*post)(void *ctx, const char *body, size_t len, const char *token,
                 dsp_http_outcome_t *out);
    /* Ends the session: closes the connection, releases every copy of the token header. */
    void (*close)(void *ctx);
} dsp_http_port_t;

/* API token provider: writes a NUL-terminated token into buf (cap bytes). */
typedef struct {
    void *ctx;
    dsp_cred_status_t (*get)(void *ctx, char *buf, size_t cap);
} dsp_token_provider_t;

/* Monotonic milliseconds (unaffected by clock steps). */
typedef struct {
    void *ctx;
    uint64_t (*now_ms)(void *ctx);
} dsp_monotonic_t;

/* Caller-owned transport state. Treat as opaque. */
typedef struct {
    const dsp_http_port_t *port;
    const dsp_token_provider_t *token;
    const dsp_monotonic_t *mono;
    uint64_t device_id;
    uint32_t request_worst_ms;
    uint64_t deadline_ms; /* valid while in_session */
    bool in_session;
    bool initialized;
    char body[DSP_PROTO_BODY_MAX];
    char secret[DSP_API_TOKEN_MAX_LEN + 1]; /* wiped after every use */
    dsp_http_outcome_t outcome;
} dsp_https_transport_t;

/* request_worst_ms: 0 = DSP_HTTPS_REQUEST_WORST_MS_DEFAULT. */
bool dsp_https_init(dsp_https_transport_t *t, const dsp_http_port_t *port,
                    const dsp_token_provider_t *token, const dsp_monotonic_t *mono,
                    uint64_t device_id, uint32_t request_worst_ms);

/* Printable ASCII without spaces, DSP_API_TOKEN_MIN_LEN..DSP_API_TOKEN_MAX_LEN. */
bool dsp_https_token_valid(const char *token);

/* True if a valid token is provisioned (fetched and wiped again). */
bool dsp_https_ready(dsp_https_transport_t *t);

/* Opens a delivery session with a time budget (ms from now). false: port unusable. */
bool dsp_https_begin(dsp_https_transport_t *t, uint32_t budget_ms);

/* Ends the session (port close). Idempotent. */
void dsp_https_end(dsp_https_transport_t *t);

/* The M1.5 transport view of t (send + can_send). Valid while t lives. */
dsp_notify_transport_t dsp_https_transport(dsp_https_transport_t *t);

#ifdef __cplusplus
}
#endif

#endif /* DSP_HTTPS_TRANSPORT_H */
