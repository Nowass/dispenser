#include "dsp_esp_time.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

/* Outcome and timing only: never the SSID, passphrase or any address. */
static const char *TAG = "dsp_wifi";

#define BIT_CONNECTED BIT0
#define BIT_FAILED BIT1

/* One radio per device: platform-level singleton owned by the Wi-Fi session manager. */
static struct {
    esp_netif_t *netif;
    EventGroupHandle_t events;
    StaticEventGroup_t events_buf;
    esp_event_handler_instance_t h_wifi;
    esp_event_handler_instance_t h_ip;
    volatile int last_reason;
    bool started;
} s_wifi;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        s_wifi.last_reason = d != NULL ? d->reason : 0;
        xEventGroupSetBits(s_wifi.events, BIT_FAILED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi.events, BIT_CONNECTED);
    }
}

static void wifi_stop(void *ctx);

static dsp_net_status_t wifi_start(void *ctx)
{
    (void)ctx;
    if (s_wifi.started) {
        return DSP_NET_ERR_STATE;
    }
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return DSP_NET_ERR_START;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return DSP_NET_ERR_START;
    }
    if (s_wifi.events == NULL) {
        s_wifi.events = xEventGroupCreateStatic(&s_wifi.events_buf);
    }
    if (s_wifi.netif == NULL) {
        s_wifi.netif = esp_netif_create_default_wifi_sta();
        if (s_wifi.netif == NULL) {
            return DSP_NET_ERR_START;
        }
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) {
        return DSP_NET_ERR_START;
    }
    s_wifi.started = true; /* from here on wifi_stop() releases everything */
    if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK || /* never persist credentials */
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL,
                                            &s_wifi.h_wifi) != ESP_OK ||
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL,
                                            &s_wifi.h_ip) != ESP_OK ||
        esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK || esp_wifi_start() != ESP_OK) {
        wifi_stop(NULL);
        return DSP_NET_ERR_START;
    }
    return DSP_NET_OK;
}

static dsp_net_status_t wifi_connect(void *ctx, const dsp_wifi_credentials_t *creds, uint32_t timeout_ms)
{
    (void)ctx;
    if (!s_wifi.started || !dsp_wifi_credentials_valid(creds)) {
        return DSP_NET_ERR_ARG;
    }
    wifi_config_t wc;
    memset(&wc, 0, sizeof wc);
    memcpy(wc.sta.ssid, creds->ssid, strnlen(creds->ssid, sizeof wc.sta.ssid));
    size_t plen = strnlen(creds->passphrase, sizeof wc.sta.password);
    memcpy(wc.sta.password, creds->passphrase, plen);
    wc.sta.threshold.authmode = plen == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    memset(&wc, 0, sizeof wc); /* the driver keeps its own RAM copy (storage = RAM) */
    if (err != ESP_OK) {
        return DSP_NET_ERR_CONNECT;
    }
    xEventGroupClearBits(s_wifi.events, BIT_CONNECTED | BIT_FAILED);
    s_wifi.last_reason = 0;
    int64_t t0 = esp_timer_get_time();
    if (esp_wifi_connect() != ESP_OK) {
        return DSP_NET_ERR_CONNECT;
    }
    EventBits_t bits = xEventGroupWaitBits(s_wifi.events, BIT_CONNECTED | BIT_FAILED, pdTRUE,
                                           pdFALSE, pdMS_TO_TICKS(timeout_ms));
    unsigned ms = (unsigned)((esp_timer_get_time() - t0) / 1000);
    if (bits & BIT_CONNECTED) {
        ESP_LOGI(TAG, "connected (IP) in %u ms", ms);
        return DSP_NET_OK;
    }
    if (!(bits & BIT_FAILED)) {
        ESP_LOGW(TAG, "connect timeout after %u ms", ms);
        return DSP_NET_ERR_TIMEOUT;
    }
    ESP_LOGW(TAG, "connect failed after %u ms (reason %d)", ms, (int)s_wifi.last_reason);
    switch (s_wifi.last_reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return DSP_NET_ERR_AUTH;
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return DSP_NET_ERR_NO_AP;
    default:
        return DSP_NET_ERR_CONNECT;
    }
}

static void wifi_disconnect(void *ctx)
{
    (void)ctx;
    if (s_wifi.started) {
        (void)esp_wifi_disconnect();
    }
}

static void wifi_stop(void *ctx)
{
    (void)ctx;
    if (!s_wifi.started) {
        return;
    }
    if (s_wifi.h_wifi != NULL) {
        (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi.h_wifi);
        s_wifi.h_wifi = NULL;
    }
    if (s_wifi.h_ip != NULL) {
        (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_wifi.h_ip);
        s_wifi.h_ip = NULL;
    }
    (void)esp_wifi_stop();
    (void)esp_wifi_deinit();
    if (s_wifi.netif != NULL) {
        esp_netif_destroy_default_wifi(s_wifi.netif);
        s_wifi.netif = NULL;
    }
    s_wifi.started = false;
    ESP_LOGI(TAG, "stopped (radio off)");
}

static const dsp_wifi_driver_t s_driver = {NULL, wifi_start, wifi_connect, wifi_disconnect, wifi_stop};

const dsp_wifi_driver_t *dsp_esp_wifi_driver(void)
{
    return &s_driver;
}
