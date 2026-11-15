#include "dsp_esp_time.h"

#include <sys/time.h>

#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#ifndef CONFIG_DSP_SNTP_SERVER
#define CONFIG_DSP_SNTP_SERVER "pool.ntp.org"
#endif

/* One SNTP client per device: platform-level singleton. */
static SemaphoreHandle_t s_sem;
static StaticSemaphore_t s_sem_buf;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static dsp_time_sample_t s_sample;
static bool s_have_sample;

/*
 * Replaces lwIP's weak sntp_sync_time() (documented as replaceable in esp_sntp.h): the
 * received time is NOT applied here. The sample (server time + local clock at the same
 * moment) is handed to the domain, which decides; the clock port applies the step.
 * Note: this bypasses esp_netif_sntp_sync_wait(), which relies on the default hook.
 */
void sntp_sync_time(struct timeval *tv)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    portENTER_CRITICAL(&s_lock);
    s_sample.source_utc = (dsp_utc_t)tv->tv_sec;
    s_sample.clock_utc = (dsp_utc_t)now.tv_sec;
    s_have_sample = true;
    portEXIT_CRITICAL(&s_lock);
    sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);
    if (s_sem != NULL) {
        xSemaphoreGive(s_sem);
    }
}

static dsp_sync_status_t sntp_sync(void *ctx, uint32_t timeout_ms, dsp_time_sample_t *out)
{
    (void)ctx;
    if (out == NULL) {
        return DSP_SYNC_ERR_SOURCE;
    }
    if (s_sem == NULL) {
        s_sem = xSemaphoreCreateBinaryStatic(&s_sem_buf);
    }
    xSemaphoreTake(s_sem, 0); /* drop a stale signal */
    portENTER_CRITICAL(&s_lock);
    s_have_sample = false;
    portEXIT_CRITICAL(&s_lock);

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_DSP_SNTP_SERVER);
    cfg.smooth_sync = false; /* immediate step, decided by the domain */
    cfg.start = true;
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        return DSP_SYNC_ERR_SOURCE;
    }
    bool got = xSemaphoreTake(s_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    esp_netif_sntp_deinit(); /* no further network activity */
    if (!got) {
        return DSP_SYNC_ERR_TIMEOUT;
    }
    portENTER_CRITICAL(&s_lock);
    bool have = s_have_sample;
    dsp_time_sample_t sample = s_sample;
    portEXIT_CRITICAL(&s_lock);
    if (!have) {
        return DSP_SYNC_ERR_SOURCE;
    }
    *out = sample;
    return DSP_SYNC_OK;
}

static const dsp_time_source_t s_source = {NULL, sntp_sync};

const dsp_time_source_t *dsp_esp_sntp_source(void)
{
    return &s_source;
}
