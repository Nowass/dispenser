#include "dsp_https_transport.h"

#include <string.h>

/* Overwrite secrets; volatile prevents the compiler from removing the stores. */
static void wipe(void *p, size_t len)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    for (size_t i = 0; i < len; i++) {
        v[i] = 0;
    }
}

bool dsp_https_token_valid(const char *token)
{
    if (token == NULL) {
        return false;
    }
    size_t n = 0;
    while (n <= DSP_API_TOKEN_MAX_LEN && token[n] != '\0') {
        unsigned char ch = (unsigned char)token[n];
        if (ch < 0x21 || ch > 0x7E) {
            return false; /* printable ASCII, no spaces or control characters */
        }
        n++;
    }
    return n >= DSP_API_TOKEN_MIN_LEN && n <= DSP_API_TOKEN_MAX_LEN;
}

bool dsp_https_init(dsp_https_transport_t *t, const dsp_http_port_t *port,
                    const dsp_token_provider_t *token, const dsp_monotonic_t *mono,
                    uint64_t device_id, uint32_t request_worst_ms)
{
    if (t == NULL) {
        return false;
    }
    memset(t, 0, sizeof *t);
    if (port == NULL || port->open == NULL || port->post == NULL || port->close == NULL ||
        token == NULL || token->get == NULL || mono == NULL || mono->now_ms == NULL ||
        (device_id & ~0xFFFFFFFFFFFFull) != 0) {
        return false;
    }
    t->port = port;
    t->token = token;
    t->mono = mono;
    t->device_id = device_id;
    t->request_worst_ms = request_worst_ms != 0 ? request_worst_ms : DSP_HTTPS_REQUEST_WORST_MS_DEFAULT;
    t->initialized = true;
    return true;
}

/* Fetches the token into t->secret; false (and wiped) if unavailable or invalid. */
static bool fetch_token(dsp_https_transport_t *t)
{
    wipe(t->secret, sizeof t->secret);
    dsp_cred_status_t cs = t->token->get(t->token->ctx, t->secret, sizeof t->secret);
    t->secret[sizeof t->secret - 1] = '\0';
    if (cs != DSP_CRED_OK || !dsp_https_token_valid(t->secret)) {
        wipe(t->secret, sizeof t->secret);
        return false;
    }
    return true;
}

bool dsp_https_ready(dsp_https_transport_t *t)
{
    if (t == NULL || !t->initialized) {
        return false;
    }
    bool ok = fetch_token(t);
    wipe(t->secret, sizeof t->secret);
    return ok;
}

bool dsp_https_begin(dsp_https_transport_t *t, uint32_t budget_ms)
{
    if (t == NULL || !t->initialized || t->in_session) {
        return false;
    }
    if (!t->port->open(t->port->ctx)) {
        return false;
    }
    t->deadline_ms = t->mono->now_ms(t->mono->ctx) + budget_ms;
    t->in_session = true;
    return true;
}

void dsp_https_end(dsp_https_transport_t *t)
{
    if (t == NULL || !t->initialized || !t->in_session) {
        return;
    }
    t->port->close(t->port->ctx);
    t->in_session = false;
    wipe(t->secret, sizeof t->secret);
}

static bool can_send(void *ctx)
{
    dsp_https_transport_t *t = ctx;
    if (t == NULL || !t->in_session) {
        return false;
    }
    uint64_t now = t->mono->now_ms(t->mono->ctx);
    return now < t->deadline_ms && t->deadline_ms - now >= t->request_worst_ms;
}

static dsp_tx_result_t send(void *ctx, const dsp_note_t *note, uint32_t epoch,
                            dsp_tx_detail_t *detail)
{
    dsp_https_transport_t *t = ctx;
    dsp_tx_detail_t scratch;
    dsp_tx_detail_t *d = detail != NULL ? detail : &scratch;
    memset(d, 0, sizeof *d);
    if (t == NULL || !t->in_session || note == NULL) {
        return DSP_TX_FAILED; /* nothing sent */
    }
    char id[DSP_NOTIFY_ID_BUF_LEN];
    size_t len = dsp_proto_encode_event(t->device_id, epoch, note, t->body, sizeof t->body);
    if (len == 0 || !dsp_notify_format_id(t->device_id, epoch, note->seq, id, sizeof id)) {
        d->diag = DSP_TXDIAG_PROTOCOL; /* local encoding failure: nothing sent, kept */
        return DSP_TX_FAILED;
    }
    if (!fetch_token(t)) {
        d->diag = DSP_TXDIAG_AUTH; /* token missing or invalid: nothing sent, kept */
        return DSP_TX_FAILED;
    }
    memset(&t->outcome, 0, sizeof t->outcome);
    t->outcome.phase = DSP_HTTP_NOT_STARTED;
    t->port->post(t->port->ctx, t->body, len, t->secret, &t->outcome);
    wipe(t->secret, sizeof t->secret);
    return dsp_proto_classify(&t->outcome, id, d);
}

dsp_notify_transport_t dsp_https_transport(dsp_https_transport_t *t)
{
    dsp_notify_transport_t tx = {t, send, can_send};
    return tx;
}
