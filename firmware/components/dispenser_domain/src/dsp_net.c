#include "dsp_net.h"

#include <stddef.h>
#include <string.h>

static dsp_cred_status_t no_credentials(void *ctx, dsp_wifi_credentials_t *out)
{
    (void)ctx;
    (void)out;
    return DSP_CRED_UNAVAILABLE;
}

const dsp_wifi_credentials_provider_t DSP_WIFI_NO_CREDENTIALS = {NULL, no_credentials};

/* Bounded strlen (strnlen is POSIX, not ISO C11). Returns cap if no NUL within cap. */
static size_t bounded_len(const char *str, size_t cap)
{
    size_t n = 0;
    while (n < cap && str[n] != '\0') {
        n++;
    }
    return n;
}

bool dsp_wifi_credentials_valid(const dsp_wifi_credentials_t *c)
{
    if (c == NULL) {
        return false;
    }
    size_t s = bounded_len(c->ssid, sizeof c->ssid);
    size_t p = bounded_len(c->passphrase, sizeof c->passphrase);
    return s >= 1 && s <= DSP_WIFI_SSID_MAX_LEN && p <= DSP_WIFI_PASSPHRASE_MAX_LEN &&
           (p == 0 || p >= 8);
}

/* Overwrite secrets; volatile prevents the compiler from removing the stores. */
static void wipe(dsp_wifi_credentials_t *c)
{
    volatile uint8_t *p = (volatile uint8_t *)c;
    for (size_t i = 0; i < sizeof *c; i++) {
        p[i] = 0;
    }
}

dsp_net_status_t dsp_wifi_init(dsp_wifi_manager_t *m, const dsp_wifi_driver_t *drv,
                               const dsp_wifi_credentials_provider_t *creds,
                               const dsp_wifi_config_t *cfg)
{
    if (m == NULL) {
        return DSP_NET_ERR_ARG;
    }
    memset(m, 0, sizeof *m);
    if (drv == NULL || drv->start == NULL || drv->connect == NULL || drv->disconnect == NULL ||
        drv->stop == NULL || creds == NULL || creds->get == NULL || cfg == NULL ||
        cfg->connect_timeout_ms < DSP_WIFI_TIMEOUT_MIN_MS ||
        cfg->connect_timeout_ms > DSP_WIFI_TIMEOUT_MAX_MS || cfg->max_attempts < 1 ||
        cfg->max_attempts > DSP_WIFI_ATTEMPTS_MAX) {
        return DSP_NET_ERR_ARG;
    }
    m->drv = drv;
    m->creds = creds;
    m->cfg = *cfg;
    m->state = DSP_WIFI_DISABLED;
    m->initialized = true;
    return DSP_NET_OK;
}

static dsp_net_status_t fail_session(dsp_wifi_manager_t *m, dsp_net_status_t err)
{
    m->state = DSP_WIFI_STOPPING;
    m->drv->stop(m->drv->ctx);
    m->state = DSP_WIFI_FAILED;
    m->last_error = err;
    return err;
}

dsp_net_status_t dsp_wifi_connect(dsp_wifi_manager_t *m)
{
    if (m == NULL || !m->initialized) {
        return DSP_NET_ERR_ARG;
    }
    if (m->state == DSP_WIFI_CONNECTED) {
        return DSP_NET_OK; /* shared session (e.g. NTP, then notifications) */
    }
    if (m->state != DSP_WIFI_DISABLED && m->state != DSP_WIFI_FAILED) {
        return DSP_NET_ERR_STATE;
    }
    m->attempts_used = 0;
    dsp_wifi_credentials_t creds;
    memset(&creds, 0, sizeof creds);
    dsp_cred_status_t cs = m->creds->get(m->creds->ctx, &creds);
    if (cs != DSP_CRED_OK || !dsp_wifi_credentials_valid(&creds)) {
        wipe(&creds);
        m->state = DSP_WIFI_DISABLED; /* radio never started */
        m->last_error = DSP_NET_ERR_NO_CREDENTIALS;
        return DSP_NET_ERR_NO_CREDENTIALS;
    }
    m->state = DSP_WIFI_STARTING;
    dsp_net_status_t st = m->drv->start(m->drv->ctx);
    if (st != DSP_NET_OK) {
        wipe(&creds);
        return fail_session(m, DSP_NET_ERR_START);
    }
    m->state = DSP_WIFI_CONNECTING;
    st = DSP_NET_ERR_CONNECT;
    while (m->attempts_used < m->cfg.max_attempts) { /* bounded */
        m->attempts_used++;
        st = m->drv->connect(m->drv->ctx, &creds, m->cfg.connect_timeout_ms);
        if (st == DSP_NET_OK || st == DSP_NET_ERR_AUTH) {
            break; /* success, or credentials rejected: retrying cannot help */
        }
        m->drv->disconnect(m->drv->ctx);
    }
    wipe(&creds);
    if (st != DSP_NET_OK) {
        return fail_session(m, st);
    }
    m->state = DSP_WIFI_CONNECTED;
    m->last_error = DSP_NET_OK;
    return DSP_NET_OK;
}

void dsp_wifi_shutdown(dsp_wifi_manager_t *m)
{
    if (m == NULL || !m->initialized) {
        return;
    }
    if (m->state == DSP_WIFI_DISABLED) {
        return;
    }
    if (m->state == DSP_WIFI_CONNECTED) {
        m->drv->disconnect(m->drv->ctx);
    }
    if (m->state != DSP_WIFI_FAILED) { /* FAILED: already stopped */
        m->state = DSP_WIFI_STOPPING;
        m->drv->stop(m->drv->ctx);
    }
    m->state = DSP_WIFI_DISABLED;
}

dsp_wifi_state_t dsp_wifi_state(const dsp_wifi_manager_t *m)
{
    return m != NULL ? m->state : DSP_WIFI_DISABLED;
}
