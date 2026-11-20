#include "dsp_esp_time.h"

#include <stddef.h>
#include <string.h>
#include <sys/time.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_system.h"

#define RTC_MAGIC 0x44535054u /* "DSPT" */
#define RTC_VERSION 1u

/* Survives deep sleep and software/watchdog resets; garbage after power-on (validated by
 * magic, version and CRC). Hardware-bound state, hence a platform-level static. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    int64_t last_sync;
    int64_t next_attempt;
    uint32_t failures;
    uint32_t crc;
} rtc_block_t;

static RTC_NOINIT_ATTR rtc_block_t s_rtc;

static uint32_t block_crc(const rtc_block_t *b)
{
    return esp_rom_crc32_le(0, (const uint8_t *)b, offsetof(rtc_block_t, crc));
}

static bool block_valid(void)
{
    return s_rtc.magic == RTC_MAGIC && s_rtc.version == RTC_VERSION && s_rtc.crc == block_crc(&s_rtc);
}

static void block_seal(void)
{
    s_rtc.magic = RTC_MAGIC;
    s_rtc.version = RTC_VERSION;
    s_rtc.crc = block_crc(&s_rtc);
}

static dsp_utc_t clock_now(void *ctx)
{
    (void)ctx;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (dsp_utc_t)tv.tv_sec;
}

static bool clock_set(void *ctx, dsp_utc_t utc)
{
    (void)ctx;
    struct timeval before;
    gettimeofday(&before, NULL);
    struct timeval tv = {.tv_sec = (time_t)utc, .tv_usec = 0};
    bool ok = settimeofday(&tv, NULL) == 0; /* immediate step (no adjtime slewing) */
    ESP_LOGI("dsp_clock", "clock %s by %lld s (accepted by the timekeeper)", ok ? "stepped" : "step FAILED",
             (long long)((int64_t)utc - (int64_t)before.tv_sec));
    return ok;
}

static void clock_mark_synced(void *ctx, dsp_utc_t last_sync)
{
    (void)ctx;
    if (!block_valid()) {
        memset(&s_rtc, 0, sizeof s_rtc);
    }
    s_rtc.last_sync = last_sync;
    block_seal();
}

void dsp_esp_clock_port(dsp_clock_port_t *out)
{
    out->ctx = NULL;
    out->now = clock_now;
    out->set = clock_set;
    out->mark_synced = clock_mark_synced;
}

static bool reset_keeps_rtc(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_DEEPSLEEP:
    case ESP_RST_SW:
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
        return true;
    default:
        return false; /* power-on, brownout, external pin, unknown: time base lost */
    }
}

dsp_time_reading_t dsp_esp_clock_reading(dsp_utc_t persisted_last_sync)
{
    dsp_time_reading_t r;
    r.now = clock_now(NULL);
    r.time_base_continuous = persisted_last_sync != 0 && reset_keeps_rtc(esp_reset_reason()) &&
                             block_valid() && s_rtc.last_sync == persisted_last_sync;
    return r;
}

bool dsp_esp_clock_load_retained(dsp_time_retained_t *out)
{
    if (out == NULL || !block_valid() || !reset_keeps_rtc(esp_reset_reason())) {
        return false;
    }
    out->consecutive_failures = s_rtc.failures > UINT16_MAX ? UINT16_MAX : (uint16_t)s_rtc.failures;
    out->next_attempt = s_rtc.next_attempt;
    return true;
}

void dsp_esp_clock_save_retained(const dsp_time_retained_t *retained)
{
    if (retained == NULL) {
        return;
    }
    if (!block_valid()) {
        memset(&s_rtc, 0, sizeof s_rtc);
    }
    s_rtc.failures = retained->consecutive_failures;
    s_rtc.next_attempt = retained->next_attempt;
    block_seal();
}
