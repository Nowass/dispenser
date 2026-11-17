#include "dsp_esp_notify.h"

#include <string.h>

#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

/* ---- Device identity and monotonic time --------------------------------------------- */

bool dsp_esp_device_id(uint64_t *out)
{
    uint8_t mac[6];
    if (out == NULL || esp_efuse_mac_get_default(mac) != ESP_OK) {
        return false;
    }
    uint64_t id = 0;
    for (int i = 0; i < 6; i++) {
        id = (id << 8) | mac[i];
    }
    *out = id;
    return true;
}

static uint64_t mono_now(void *ctx)
{
    (void)ctx;
    int64_t us = esp_timer_get_time();
    return us > 0 ? (uint64_t)us / 1000u : 0;
}

void dsp_esp_monotonic(dsp_monotonic_t *out)
{
    if (out != NULL) {
        out->ctx = NULL;
        out->now_ms = mono_now;
    }
}

/* ---- Credentials in the dedicated "nvs_sec" partition (read-only) ------------------- */

static bool s_secrets_ready;

bool dsp_esp_secrets_init(void)
{
    if (s_secrets_ready) {
        return true;
    }
    /* Never erased or reformatted here: a damaged partition is reported, not "repaired"
     * (re-provision it with the developer tool). */
    esp_err_t err = nvs_flash_init_partition(DSP_NVS_SEC_PARTITION);
    s_secrets_ready = err == ESP_OK;
    return s_secrets_ready;
}

/* Reads one string key; DSP_CRED_UNAVAILABLE if the namespace or key does not exist. */
static dsp_cred_status_t read_str(const char *key, char *buf, size_t cap)
{
    if (!s_secrets_ready) {
        return DSP_CRED_UNAVAILABLE;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(DSP_NVS_SEC_PARTITION, DSP_NVS_SEC_NAMESPACE,
                                            NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return DSP_CRED_UNAVAILABLE;
    }
    if (err != ESP_OK) {
        return DSP_CRED_ERR;
    }
    size_t len = cap;
    err = nvs_get_str(h, key, buf, &len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return DSP_CRED_UNAVAILABLE;
    }
    return err == ESP_OK ? DSP_CRED_OK : DSP_CRED_ERR;
}

static void wipe(void *p, size_t len)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    for (size_t i = 0; i < len; i++) {
        v[i] = 0;
    }
}

static dsp_cred_status_t wifi_get(void *ctx, dsp_wifi_credentials_t *out)
{
    (void)ctx;
    memset(out, 0, sizeof *out);
    dsp_cred_status_t a = read_str(DSP_NVS_SEC_KEY_SSID, out->ssid, sizeof out->ssid);
    if (a != DSP_CRED_OK) {
        wipe(out, sizeof *out);
        return a;
    }
    dsp_cred_status_t b = read_str(DSP_NVS_SEC_KEY_PASS, out->passphrase, sizeof out->passphrase);
    if (b == DSP_CRED_UNAVAILABLE) {
        out->passphrase[0] = '\0'; /* open network */
        b = DSP_CRED_OK;
    }
    if (b != DSP_CRED_OK) {
        wipe(out, sizeof *out);
    }
    return b;
}

bool dsp_esp_setup_code_read(void *ctx, uint8_t out[DSP_SETUP_CODE_BYTES])
{
    (void)ctx;
    if (out == NULL) {
        return false;
    }
    memset(out, 0, DSP_SETUP_CODE_BYTES);
    if (!s_secrets_ready) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open_from_partition(DSP_NVS_SEC_PARTITION, DSP_NVS_SEC_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = DSP_SETUP_CODE_BYTES;
    esp_err_t err = nvs_get_blob(h, DSP_NVS_SEC_KEY_SETUP_CODE, out, &len);
    nvs_close(h);
    if (err != ESP_OK || len != DSP_SETUP_CODE_BYTES) {
        wipe(out, DSP_SETUP_CODE_BYTES);
        return false;
    }
    return true;
}

static dsp_cred_status_t token_get(void *ctx, char *buf, size_t cap)
{
    (void)ctx;
    if (buf == NULL || cap == 0) {
        return DSP_CRED_ERR;
    }
    dsp_cred_status_t st = read_str(DSP_NVS_SEC_KEY_TOKEN, buf, cap);
    if (st != DSP_CRED_OK) {
        wipe(buf, cap);
    }
    return st;
}

static const dsp_wifi_credentials_provider_t s_wifi = {NULL, wifi_get};
static const dsp_token_provider_t s_token = {NULL, token_get};

const dsp_wifi_credentials_provider_t *dsp_esp_wifi_credentials(void)
{
    return &s_wifi;
}

const dsp_token_provider_t *dsp_esp_api_token(void)
{
    return &s_token;
}
