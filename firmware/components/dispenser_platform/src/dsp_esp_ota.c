/*
 * Firmware update port (docs/FIRMWARE_UPDATE.md) over esp_https_ota: verified HTTPS (certificate
 * bundle, redirects to GitHub's asset host), streamed into the inactive OTA slot, checked before
 * activation (project, version, size, SHA-256 of the written image, then esp_https_ota_finish:
 * image validation including the RSA application signature with
 * CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT), boot partition switched by finish. Rollback by
 * the bootloader (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) until the application confirms.
 * Compiles to a NULL port without CONFIG_DSP_OTA.
 */
#include "dsp_esp_app.h"

#include "sdkconfig.h"

#if CONFIG_DSP_OTA

#include <stddef.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_secure_boot.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"

#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE || !CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT
#error "CONFIG_DSP_OTA needs CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE and CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT"
#endif
#if CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP
#error "CONFIG_DSP_OTA refuses CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP"
#endif

static const char *TAG = "dsp_ota";

#define DSP_OTA_DOWNLOAD_BUDGET_US (240LL * 1000 * 1000) /* whole download */
#define DSP_OTA_HTTP_TIMEOUT_MS 15000
#define RTC_MAGIC 0x4453504Fu /* "DSPO" */
#define RTC_VERSION 1u

/* Result of an attempt in this power cycle (survives deep sleep and software resets of THIS
 * build only: another build validates magic, version and CRC and ignores foreign content). */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t result;
    char ver[DSP_OTA_VERSION_MAX + 1];
    uint32_t crc;
} rtc_ota_t;

static RTC_NOINIT_ATTR rtc_ota_t s_rtc;

static uint32_t rtc_crc(void)
{
    return esp_rom_crc32_le(0, (const uint8_t *)&s_rtc, offsetof(rtc_ota_t, crc));
}

static const char *ota_running_version(void *ctx)
{
    (void)ctx;
    return esp_app_get_description()->version;
}

static bool ota_capable(void *ctx)
{
    (void)ctx;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run == NULL || esp_ota_get_next_update_partition(NULL) == NULL) {
        return false;
    }
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        return false; /* confirm this image first */
    }
    /* the trusted key comes from the running application's signature block */
    esp_image_sig_public_key_digests_t d;
    memset(&d, 0, sizeof d);
    return esp_secure_boot_get_signature_blocks_for_running_app(true, &d) == ESP_OK && d.num_digests > 0;
}

/* SHA-256 of the first len bytes written to part (PSA Crypto, mbedtls 4). */
static bool partition_sha(const esp_partition_t *part, size_t len, uint8_t out[32])
{
    static uint8_t buf[4096];
    if (psa_crypto_init() != PSA_SUCCESS) {
        return false;
    }
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    bool ok = psa_hash_setup(&op, PSA_ALG_SHA_256) == PSA_SUCCESS;
    for (size_t off = 0, n = 0; ok && off < len; off += n) {
        n = len - off < sizeof buf ? len - off : sizeof buf;
        ok = esp_partition_read(part, off, buf, n) == ESP_OK && psa_hash_update(&op, buf, n) == PSA_SUCCESS;
        if ((off / sizeof buf) % 32u == 31u) {
            vTaskDelay(1); /* let the idle task run (task watchdog) */
        }
    }
    size_t got = 0;
    ok = ok && psa_hash_finish(&op, out, 32, &got) == PSA_SUCCESS && got == 32;
    if (!ok) {
        (void)psa_hash_abort(&op);
    }
    return ok;
}

static dsp_ota_result_t ota_install(void *ctx, const dsp_ota_job_t *job)
{
    (void)ctx;
    esp_http_client_config_t http = {
        .url = job->url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = DSP_OTA_HTTP_TIMEOUT_MS,
        .buffer_size = 4096,    /* GitHub's redirect Location is long */
        .buffer_size_tx = 2048, /* the redirected request line as well */
        .max_redirection_count = 5,
        .keep_alive_enable = true,
        .user_agent = "dispenser-v2",
    };
    esp_https_ota_config_t cfg = {.http_config = &http};
    esp_https_ota_handle_t h = NULL;
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    ESP_LOGI(TAG, "update %s: download %s (%u B) into %s", job->version, job->url, (unsigned)job->size,
             part != NULL ? part->label : "?");
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err != ESP_OK || part == NULL) {
        ESP_LOGE(TAG, "begin failed (0x%x)", (unsigned)err);
        return DSP_OTA_DOWNLOAD_FAILED;
    }
    esp_app_desc_t d;
    memset(&d, 0, sizeof d);
    if (esp_https_ota_get_img_desc(h, &d) != ESP_OK || strncmp(d.project_name, "dispenser", sizeof d.project_name) != 0 ||
        strncmp(d.version, job->version, sizeof d.version) != 0) {
        ESP_LOGE(TAG, "rejected: image project/version does not match the request");
        (void)esp_https_ota_abort(h);
        return DSP_OTA_IMAGE_REJECTED;
    }
    int total = esp_https_ota_get_image_size(h);
    if (total != (int)job->size) {
        ESP_LOGE(TAG, "rejected: size %d, requested %u", total, (unsigned)job->size);
        (void)esp_https_ota_abort(h);
        return DSP_OTA_IMAGE_REJECTED;
    }
    int64_t deadline = esp_timer_get_time() + DSP_OTA_DOWNLOAD_BUDGET_US;
    do {
        err = esp_https_ota_perform(h);
        if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS && esp_timer_get_time() > deadline) {
            ESP_LOGE(TAG, "download budget exceeded");
            (void)esp_https_ota_abort(h);
            return DSP_OTA_DOWNLOAD_FAILED;
        }
    } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);
    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
        ESP_LOGE(TAG, "download failed (0x%x)", (unsigned)err);
        (void)esp_https_ota_abort(h);
        return DSP_OTA_DOWNLOAD_FAILED;
    }
    int len = esp_https_ota_get_image_len_read(h);
    uint8_t sha[32];
    if (len != (int)job->size || !partition_sha(part, (size_t)len, sha) || memcmp(sha, job->sha256, 32) != 0) {
        ESP_LOGE(TAG, "rejected: length %d or SHA-256 differs from the request", len);
        (void)esp_https_ota_abort(h);
        return DSP_OTA_IMAGE_REJECTED;
    }
    err = esp_https_ota_finish(h); /* image + signature verification, then the boot partition */
    if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
        ESP_LOGE(TAG, "rejected: image or signature verification failed");
        return DSP_OTA_IMAGE_REJECTED;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "finish failed (0x%x)", (unsigned)err);
        return DSP_OTA_FLASH_FAILED;
    }
    ESP_LOGI(TAG, "update %s verified and activated: restart", job->version);
    return DSP_OTA_INSTALLED;
}

static void ota_restart(void *ctx)
{
    (void)ctx;
    esp_restart();
}

static bool ota_pending_verify(void *ctx)
{
    (void)ctx;
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    return run != NULL && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}

static void ota_confirm(void *ctx, bool ok)
{
    (void)ctx;
#if CONFIG_DSP_OTA_TEST_FAIL_BOOT_CHECK
    ESP_LOGW(TAG, "TEST BUILD: the boot check fails on purpose (rollback test)");
    ok = false;
#endif
    if (ok) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "boot of %s confirmed (0x%x)", esp_app_get_description()->version, (unsigned)err);
        return;
    }
    ESP_LOGE(TAG, "boot check failed: rollback to the previous firmware");
    (void)esp_ota_mark_app_invalid_rollback_and_reboot();
    esp_restart(); /* still PENDING_VERIFY: the bootloader rolls back on this reset */
}

static dsp_ota_result_t ota_last(void *ctx, char ver[DSP_OTA_VERSION_MAX + 1])
{
    (void)ctx;
    memset(ver, 0, DSP_OTA_VERSION_MAX + 1);
    if (s_rtc.magic == RTC_MAGIC && s_rtc.version == RTC_VERSION && s_rtc.crc == rtc_crc() &&
        s_rtc.result <= DSP_OTA_CANCELLED) {
        memcpy(ver, s_rtc.ver, DSP_OTA_VERSION_MAX);
        return (dsp_ota_result_t)s_rtc.result;
    }
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    esp_app_desc_t d;
    if (bad != NULL && esp_ota_get_partition_description(bad, &d) == ESP_OK) {
        strncpy(ver, d.version, DSP_OTA_VERSION_MAX);
        return DSP_OTA_ROLLED_BACK;
    }
    return DSP_OTA_NONE;
}

static void ota_record(void *ctx, dsp_ota_result_t r, const char *ver)
{
    (void)ctx;
    memset(&s_rtc, 0, sizeof s_rtc);
    s_rtc.magic = RTC_MAGIC;
    s_rtc.version = RTC_VERSION;
    s_rtc.result = (uint32_t)r;
    if (ver != NULL) {
        strncpy(s_rtc.ver, ver, DSP_OTA_VERSION_MAX);
    }
    s_rtc.crc = rtc_crc();
    ESP_LOGI(TAG, "update result %u (%s)", (unsigned)r, s_rtc.ver);
}

static const dsp_ota_port_t s_port = {NULL, ota_running_version, ota_capable, ota_install, ota_restart,
                                      ota_pending_verify, ota_confirm, ota_last, ota_record};

const dsp_ota_port_t *dsp_esp_ota_port(void)
{
    return &s_port;
}

#else

const dsp_ota_port_t *dsp_esp_ota_port(void)
{
    return NULL;
}

#endif
