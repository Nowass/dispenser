#include "dsp_esp_notify.h"

#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_tls_errors.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dsp_https"; /* numbers only: never URL, token or body */

/* One HTTP client per device session: platform-level singleton (single owner). */
static struct {
    char url[DSP_ESP_URL_MAX_LEN + 1];
    esp_http_client_handle_t client;
    char auth[8 + DSP_API_TOKEN_MAX_LEN + 1]; /* "Bearer " + token: wiped after every use */
} s_http;

static void wipe(void *p, size_t len)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    for (size_t i = 0; i < len; i++) {
        v[i] = 0;
    }
}

static bool https_url(const char *url)
{
    return url != NULL && strncmp(url, "https://", 8) == 0 && strlen(url) > 8 &&
           strlen(url) <= DSP_ESP_URL_MAX_LEN;
}

static void quiet_logs(void)
{
    /* Defense in depth: the build's maximum log level (INFO) already excludes the
     * debug-level header dumps of these components; never raise it for them. */
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_WARN);
    esp_log_level_set("esp-tls", ESP_LOG_WARN);
    esp_log_level_set("esp-tls-mbedtls", ESP_LOG_WARN);
    esp_log_level_set("transport_base", ESP_LOG_WARN);
}

bool dsp_esp_http_configure(const char *url)
{
    if (!https_url(url)) {
        return false;
    }
    strncpy(s_http.url, url, sizeof s_http.url - 1);
    s_http.url[sizeof s_http.url - 1] = '\0';
    return true;
}

static esp_http_client_handle_t new_client(const char *url, esp_http_client_method_t method)
{
    esp_http_client_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.url = url;
    cfg.method = method;
    cfg.timeout_ms = (int)DSP_ESP_HTTP_TIMEOUT_MS;
    cfg.crt_bundle_attach = esp_crt_bundle_attach; /* verification always on (D3) */
    cfg.disable_auto_redirect = true;              /* a redirect means a wrong URL */
    cfg.max_redirection_count = 0;
    cfg.keep_alive_enable = true; /* one TLS handshake per session */
    cfg.buffer_size = 1024;
    cfg.buffer_size_tx = 1024;
    cfg.user_agent = "dispenser-v2";
    return esp_http_client_init(&cfg);
}

/* Connect failures before any byte was sent: DNS/TCP vs TLS (diagnostic only; both are
 * FAILED for the domain). */
static dsp_http_phase_t connect_phase(esp_http_client_handle_t c)
{
    int tls_code = 0;
    int tls_flags = 0;
    esp_err_t last = esp_http_client_get_and_clear_last_tls_error(c, &tls_code, &tls_flags);
    if (last == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME ||
        last == ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST ||
        last == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT) {
        return DSP_HTTP_CONNECT_FAILED;
    }
    if (last == ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED || tls_flags != 0 || tls_code != 0) {
        return DSP_HTTP_TLS_FAILED;
    }
    return DSP_HTTP_CONNECT_FAILED;
}

/* Reads the response after the request was written. Keeps the first
 * DSP_PROTO_RESPONSE_MAX bytes, drains (bounded) the rest so the connection can be reused. */
static void read_response(esp_http_client_handle_t c, dsp_http_outcome_t *out)
{
    int64_t len = esp_http_client_fetch_headers(c);
    if (len < 0) {
        out->phase = DSP_HTTP_NO_RESPONSE; /* timeout or reset before the status line */
        esp_http_client_close(c);
        return;
    }
    int status = esp_http_client_get_status_code(c);
    if (status < 100 || status > 599) {
        out->phase = DSP_HTTP_NO_RESPONSE;
        esp_http_client_close(c);
        return;
    }
    out->phase = DSP_HTTP_RESPONSE;
    out->status = (uint16_t)status;
    size_t total = 0;
    char scratch[128];
    for (int guard = 0; guard < 64; guard++) { /* bounded: at most 64 reads */
        size_t room = total < DSP_PROTO_RESPONSE_MAX ? DSP_PROTO_RESPONSE_MAX - total : 0;
        char *dst = room > 0 ? out->body + total : scratch;
        int cap = room > 0 ? (int)room : (int)sizeof scratch;
        int n = esp_http_client_read(c, dst, cap);
        if (n <= 0) {
            break;
        }
        if (room == 0) {
            out->body_truncated = true;
        }
        total += (size_t)n;
    }
    out->body_len = (uint16_t)(total < DSP_PROTO_RESPONSE_MAX ? total : DSP_PROTO_RESPONSE_MAX);
    if (total > DSP_PROTO_RESPONSE_MAX) {
        out->body_truncated = true;
    }
    if (!esp_http_client_is_complete_data_received(c)) {
        esp_http_client_close(c); /* do not reuse a half-read connection */
    }
}

static bool port_open(void *ctx)
{
    (void)ctx;
    if (!https_url(s_http.url) || s_http.client != NULL) {
        return false;
    }
    quiet_logs();
    /* 5.2 evidence: free heap before the TLS session (numbers only) */
    ESP_LOGI(TAG, "https session: heap free %u B before TLS", (unsigned)esp_get_free_heap_size());
    s_http.client = new_client(s_http.url, HTTP_METHOD_POST);
    return s_http.client != NULL;
}

static void port_post(void *ctx, const char *body, size_t len, const char *token,
                      dsp_http_outcome_t *out)
{
    (void)ctx;
    memset(out, 0, sizeof *out);
    out->phase = DSP_HTTP_NOT_STARTED;
    esp_http_client_handle_t c = s_http.client;
    if (c == NULL || body == NULL || token == NULL || len == 0 || len > DSP_PROTO_BODY_MAX) {
        return;
    }
    size_t tlen = strnlen(token, DSP_API_TOKEN_MAX_LEN + 1);
    if (tlen == 0 || tlen > DSP_API_TOKEN_MAX_LEN) {
        return;
    }
    memcpy(s_http.auth, "Bearer ", 7);
    memcpy(s_http.auth + 7, token, tlen);
    s_http.auth[7 + tlen] = '\0';
    esp_err_t err = esp_http_client_set_method(c, HTTP_METHOD_POST);
    if (err == ESP_OK) {
        err = esp_http_client_set_header(c, "Content-Type", "application/json");
    }
    if (err == ESP_OK) {
        err = esp_http_client_set_header(c, "Authorization", s_http.auth); /* copied */
    }
    wipe(s_http.auth, sizeof s_http.auth);
    if (err != ESP_OK) {
        (void)esp_http_client_delete_header(c, "Authorization");
        return; /* nothing sent */
    }
    err = esp_http_client_open(c, (int)len);
    if (err != ESP_OK) {
        out->phase = connect_phase(c);
        esp_http_client_close(c);
    } else {
        int written = esp_http_client_write(c, body, (int)len);
        if (written < 0 || (size_t)written != len) {
            out->phase = DSP_HTTP_SEND_FAILED; /* part of the request may have left */
            esp_http_client_close(c);
        } else {
            read_response(c, out);
        }
    }
    (void)esp_http_client_delete_header(c, "Authorization"); /* release the client's copy */
}

static void port_close(void *ctx)
{
    (void)ctx;
    if (s_http.client != NULL) {
        (void)esp_http_client_delete_header(s_http.client, "Authorization");
        esp_http_client_close(s_http.client);
        esp_http_client_cleanup(s_http.client);
        s_http.client = NULL;
        /* 5.2 / R7 evidence: heap after the TLS session, the lowest heap since boot (includes the
         * TLS peak) and the calling (application) task's stack high-water mark */
        ESP_LOGI(TAG, "https session closed: heap free %u B, min since boot %u B, app stack free min %u B",
                 (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
    wipe(s_http.auth, sizeof s_http.auth);
}

static const dsp_http_port_t s_port = {NULL, port_open, port_post, port_close};

const dsp_http_port_t *dsp_esp_http_port(void)
{
    return &s_port;
}

bool dsp_esp_http_probe(const char *url, dsp_http_outcome_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);
    out->phase = DSP_HTTP_NOT_STARTED;
    if (!https_url(url)) {
        return false;
    }
    quiet_logs();
    esp_http_client_handle_t c = new_client(url, HTTP_METHOD_GET);
    if (c == NULL) {
        return false;
    }
    if (esp_http_client_open(c, 0) != ESP_OK) {
        out->phase = connect_phase(c);
    } else {
        read_response(c, out);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return true;
}
