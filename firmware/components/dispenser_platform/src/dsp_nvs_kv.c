#include "dsp_nvs_kv.h"

#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"

static bool key_ok(const char *key)
{
    if (key == NULL) {
        return false;
    }
    size_t n = strnlen(key, DSP_KV_KEY_MAX_LEN + 1);
    return n >= 1 && n <= DSP_KV_KEY_MAX_LEN;
}

static dsp_kv_status_t nvs_get(void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *len)
{
    dsp_nvs_kv_t *n = ctx;
    if (n == NULL || !n->open || !key_ok(key) || buf == NULL || len == NULL) {
        return DSP_KV_ERR_ARG;
    }
    *len = 0;
    if (n->ns_missing) {
        return DSP_KV_NOT_FOUND;
    }
    size_t size = 0;
    esp_err_t err = nvs_get_blob((nvs_handle_t)n->handle, key, NULL, &size);
    n->last_err = err;
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return DSP_KV_NOT_FOUND;
    }
    if (err != ESP_OK) {
        /* includes ESP_ERR_NVS_TYPE_MISMATCH: a non-blob value under our key */
        return DSP_KV_ERR_READ;
    }
    if (size > cap) {
        return DSP_KV_ERR_TOO_LARGE;
    }
    err = nvs_get_blob((nvs_handle_t)n->handle, key, buf, &size);
    n->last_err = err;
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return DSP_KV_NOT_FOUND; /* NVS dropped the item (e.g. data CRC failure) */
    }
    if (err != ESP_OK) {
        return DSP_KV_ERR_READ;
    }
    *len = size;
    return DSP_KV_OK;
}

static dsp_kv_status_t nvs_set(void *ctx, const char *key, const uint8_t *data, size_t len)
{
    dsp_nvs_kv_t *n = ctx;
    if (n == NULL || !n->open || n->read_only || !key_ok(key) || data == NULL || len == 0) {
        return DSP_KV_ERR_ARG;
    }
    esp_err_t err = nvs_set_blob((nvs_handle_t)n->handle, key, data, len);
    n->last_err = err;
    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        return DSP_KV_ERR_NO_SPACE;
    }
    return err == ESP_OK ? DSP_KV_OK : DSP_KV_ERR_WRITE;
}

static dsp_kv_status_t nvs_do_commit(void *ctx)
{
    dsp_nvs_kv_t *n = ctx;
    if (n == NULL || !n->open || n->read_only) {
        return DSP_KV_ERR_ARG;
    }
    esp_err_t err = nvs_commit((nvs_handle_t)n->handle);
    n->last_err = err;
    return err == ESP_OK ? DSP_KV_OK : DSP_KV_ERR_COMMIT;
}

dsp_nvs_status_t dsp_nvs_kv_open(dsp_nvs_kv_t *nvs, const char *partition, const char *ns,
                                 bool read_only, dsp_kv_t *kv)
{
    if (nvs == NULL || partition == NULL || ns == NULL || kv == NULL) {
        return DSP_NVS_ERR_ARG;
    }
    memset(nvs, 0, sizeof *nvs);
    esp_err_t err = nvs_flash_init_partition(partition);
    nvs->last_err = err;
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        return DSP_NVS_ERR_NO_FREE_PAGES; /* deliberately no erase requested */
    }
    if (err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        return DSP_NVS_ERR_NEW_VERSION; /* deliberately no erase requested */
    }
    if (err != ESP_OK) {
        return DSP_NVS_ERR_INIT;
    }
    nvs_handle_t handle = 0;
    err = nvs_open_from_partition(partition, ns, read_only ? NVS_READONLY : NVS_READWRITE, &handle);
    nvs->last_err = err;
    if (err == ESP_ERR_NVS_NOT_FOUND && read_only) {
        nvs->ns_missing = true; /* fresh device: namespace not created yet */
    } else if (err != ESP_OK) {
        return DSP_NVS_ERR_OPEN;
    } else {
        nvs->handle = (uint32_t)handle;
    }
    nvs->open = true;
    nvs->read_only = read_only;
    kv->ctx = nvs;
    kv->get = nvs_get;
    kv->set = nvs_set;
    kv->commit = nvs_do_commit;
    return DSP_NVS_OK;
}

void dsp_nvs_kv_close(dsp_nvs_kv_t *nvs)
{
    if (nvs != NULL && nvs->open && !nvs->ns_missing) {
        nvs_close((nvs_handle_t)nvs->handle);
    }
    if (nvs != NULL) {
        nvs->open = false;
    }
}
