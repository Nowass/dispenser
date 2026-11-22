/* NimBLE adapter for the BLE byte transport (M1.10d.1). See dsp_esp_ble.h. Cross-built only. */
#include "dsp_esp_ble.h"

#include <string.h>

#include "esp_bt.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "dsp_ble";

#define SYNC_TIMEOUT_MS 3000u
#define ADV_FAST_ITVL 160u /* 100 ms in 0.625 ms units (T4) */
#define ADV_SLOW_ITVL 800u /* 500 ms */

/* 36b6000x-3457-4e8e-9cba-8126de3f52e4, little-endian byte order. */
#define DSP_UUID(x)                                                                              \
    BLE_UUID128_INIT(0xe4, 0x52, 0x3f, 0xde, 0x26, 0x81, 0xba, 0x9c, 0x8e, 0x4e, 0x57, 0x34, (x), \
                     0x00, 0xb6, 0x36)
static const ble_uuid128_t UUID_SVC = DSP_UUID(0x01);
static const ble_uuid128_t UUID_INFO = DSP_UUID(0x02);
static const ble_uuid128_t UUID_REQ = DSP_UUID(0x03);
static const ble_uuid128_t UUID_RSP = DSP_UUID(0x04);

static struct {
    dsp_ble_transport_t tr;
    dsp_ble_radio_t radio;
    SemaphoreHandle_t lock;
    StaticSemaphore_t lock_buf;
    SemaphoreHandle_t wake;
    StaticSemaphore_t wake_buf;
    SemaphoreHandle_t synced;
    StaticSemaphore_t synced_buf;
    struct ble_npl_callout retry;
    bool retry_ready;
    bool running;   /* NimBLE port initialized and host task started */
    uint16_t rsp_handle;
    uint8_t wbuf[DSP_BLE_TR_MTU_PREFERRED]; /* one fragment (host task only) */
} s;

/* ---- GATT ------------------------------------------------------------------------------- */

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr;
    (void)arg;
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR: {
        if (ble_uuid_cmp(ctxt->chr->uuid, &UUID_INFO.u) != 0) {
            return BLE_ATT_ERR_READ_NOT_PERMITTED;
        }
        uint8_t info[DSP_BLE_INFO_LEN];
        dsp_ble_tr_on_info_read(&s.tr, info);
        return os_mbuf_append(ctxt->om, info, sizeof info) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        if (ble_uuid_cmp(ctxt->chr->uuid, &UUID_REQ.u) != 0) {
            return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
        }
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > sizeof s.wbuf) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN; /* e.g. a prepared (long) write */
        }
        if (ble_hs_mbuf_to_flat(ctxt->om, s.wbuf, sizeof s.wbuf, &len) != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        int r = dsp_ble_tr_on_write(&s.tr, conn, s.wbuf, len);
        return r == DSP_BLE_WRITE_OK ? 0 : BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static const struct ble_gatt_svc_def SVCS[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SVC.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {.uuid = &UUID_INFO.u, .access_cb = chr_access, .flags = BLE_GATT_CHR_F_READ},
                {.uuid = &UUID_REQ.u, .access_cb = chr_access, .flags = BLE_GATT_CHR_F_WRITE},
                {.uuid = &UUID_RSP.u,
                 .access_cb = chr_access,
                 .flags = BLE_GATT_CHR_F_NOTIFY,
                 .val_handle = &s.rsp_handle},
                {0},
            },
    },
    {0},
};

/* ---- GAP (host task) -------------------------------------------------------------------- */

static int gap_event(struct ble_gap_event *e, void *arg)
{
    (void)arg;
    switch (e->type) {
    case BLE_GAP_EVENT_CONNECT:
        dsp_ble_tr_on_connect(&s.tr, e->connect.conn_handle, e->connect.status == 0);
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        dsp_ble_tr_on_disconnect(&s.tr, e->disconnect.conn.conn_handle);
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (e->adv_complete.reason != 0) {
            dsp_ble_tr_on_adv_stopped(&s.tr); /* not a connection: restarted by the app task */
        }
        break;
    case BLE_GAP_EVENT_MTU:
        dsp_ble_tr_on_mtu(&s.tr, e->mtu.conn_handle, e->mtu.value);
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (e->subscribe.attr_handle == s.rsp_handle) {
            dsp_ble_tr_on_subscribe(&s.tr, e->subscribe.conn_handle, e->subscribe.cur_notify != 0);
        }
        break;
    case BLE_GAP_EVENT_NOTIFY_TX:
        dsp_ble_tr_on_tx_done(&s.tr, e->notify_tx.conn_handle);
        break;
    default:
        break;
    }
    return 0;
}

static void on_sync(void)
{
    xSemaphoreGive(s.synced);
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset (reason %d)", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run(); /* returns after nimble_port_stop() */
    nimble_port_freertos_deinit();
}

static void retry_cb(struct ble_npl_event *ev)
{
    (void)ev;
    dsp_ble_tr_on_retry(&s.tr); /* host task (default event queue) */
}

/* ---- Radio interface ---------------------------------------------------------------------- */

static void r_deinit(void *ctx);

static bool r_init(void *ctx)
{
    (void)ctx;
    if (s.running) {
        return true;
    }
    (void)xSemaphoreTake(s.synced, 0);
    if (nimble_port_init() != ESP_OK) { /* controller initialized and enabled */
        ESP_LOGE(TAG, "NimBLE port init failed");
        return false;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(SVCS);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(SVCS);
    }
    if (rc == 0) {
        rc = ble_svc_gap_device_name_set("DSP2");
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT registration failed (%d)", rc);
        (void)nimble_port_deinit();
        return false;
    }
    ble_npl_callout_init(&s.retry, nimble_port_get_dflt_eventq(), retry_cb, NULL);
    s.retry_ready = true;
    nimble_port_freertos_init(host_task);
    s.running = true;
    if (xSemaphoreTake(s.synced, pdMS_TO_TICKS(SYNC_TIMEOUT_MS)) != pdTRUE ||
        ble_hs_util_ensure_addr(0) != 0) {
        ESP_LOGE(TAG, "host did not synchronize");
        r_deinit(NULL); /* never leave the controller enabled behind a failed start */
        return false;
    }
    ESP_LOGI(TAG, "radio up (service window, advertising follows)");
    return true;
}

static void r_deinit(void *ctx)
{
    (void)ctx;
    if (!s.running) {
        return;
    }
    if (s.retry_ready) {
        ble_npl_callout_stop(&s.retry);
    }
    if (nimble_port_stop() == 0) {
        if (s.retry_ready) {
            ble_npl_callout_deinit(&s.retry);
            s.retry_ready = false;
        }
        (void)nimble_port_deinit(); /* controller disabled and deinitialized */
        s.running = false;
        /* R7 / 6.1 evidence: heap after the BLE window (it must recover before Wi-Fi/TLS) and the
         * calling (application) task's stack high-water mark. Numbers only. */
        ESP_LOGI(TAG, "radio down: heap free %u B, min since boot %u B, app stack free min %u B",
                 (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    } else {
        ESP_LOGE(TAG, "NimBLE stop failed: controller left enabled (is_off reports it)");
    }
}

static bool r_is_off(void *ctx)
{
    (void)ctx;
    return !s.running && esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE;
}

static bool r_adv_start(void *ctx, const uint8_t advert[DSP_BLE_ADVERT_LEN], bool fast)
{
    (void)ctx;
    if (ble_gap_adv_active()) {
        (void)ble_gap_adv_stop();
    }
    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof f);
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = &UUID_SVC;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof rsp);
    static const uint8_t NAME[] = {'D', 'S', 'P', '2'};
    rsp.name = NAME;
    rsp.name_len = sizeof NAME;
    rsp.name_is_complete = 0; /* shortened local name */
    uint8_t sd[16 + DSP_BLE_ADVERT_LEN];
    memcpy(sd, UUID_SVC.value, 16);
    memcpy(sd + 16, advert, DSP_BLE_ADVERT_LEN);
    rsp.svc_data_uuid128 = sd;
    rsp.svc_data_uuid128_len = sizeof sd;
    struct ble_gap_adv_params p;
    memset(&p, 0, sizeof p);
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = fast ? ADV_FAST_ITVL : ADV_SLOW_ITVL;
    p.itvl_max = p.itvl_min;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc == 0) {
        rc = ble_gap_adv_rsp_set_fields(&rsp);
    }
    if (rc == 0) {
        rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    }
    return rc == 0;
}

static void r_adv_stop(void *ctx)
{
    (void)ctx;
    if (ble_gap_adv_active()) {
        (void)ble_gap_adv_stop();
    }
}

static void r_terminate(void *ctx, uint16_t handle)
{
    (void)ctx;
    (void)ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
}

static int r_notify(void *ctx, uint16_t handle, const uint8_t *frag, size_t len)
{
    (void)ctx;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(frag, (uint16_t)len);
    if (om == NULL) {
        return DSP_BLE_NOTIFY_BUSY; /* buffer pool exhausted: asynchronous retry */
    }
    int rc = ble_gatts_notify_custom(handle, s.rsp_handle, om); /* consumes om */
    if (rc == 0) {
        return DSP_BLE_NOTIFY_OK;
    }
    return rc == BLE_HS_ENOMEM ? DSP_BLE_NOTIFY_BUSY : DSP_BLE_NOTIFY_ERR;
}

static void r_retry_arm(void *ctx, uint32_t ms)
{
    (void)ctx;
    if (s.retry_ready) {
        (void)ble_npl_callout_reset(&s.retry, ble_npl_time_ms_to_ticks32(ms));
    }
}

static void r_signal(void *ctx)
{
    (void)ctx;
    xSemaphoreGive(s.wake);
}

static void r_wait(void *ctx, uint32_t max_ms)
{
    (void)ctx;
    (void)xSemaphoreTake(s.wake, pdMS_TO_TICKS(max_ms));
}

static uint32_t r_now(void *ctx)
{
    (void)ctx;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void r_lock(void *ctx)
{
    (void)ctx;
    (void)xSemaphoreTake(s.lock, portMAX_DELAY);
}

static void r_unlock(void *ctx)
{
    (void)ctx;
    (void)xSemaphoreGive(s.lock);
}

bool dsp_esp_ble_port(dsp_app_t *app, dsp_ble_port_t *port)
{
    s.lock = xSemaphoreCreateMutexStatic(&s.lock_buf);
    s.wake = xSemaphoreCreateBinaryStatic(&s.wake_buf);
    s.synced = xSemaphoreCreateBinaryStatic(&s.synced_buf);
    if (s.lock == NULL || s.wake == NULL || s.synced == NULL) {
        return false;
    }
    s.radio = (dsp_ble_radio_t){
        .ctx = NULL,
        .init = r_init,
        .deinit = r_deinit,
        .is_off = r_is_off,
        .adv_start = r_adv_start,
        .adv_stop = r_adv_stop,
        .terminate = r_terminate,
        .notify = r_notify,
        .retry_arm = r_retry_arm,
        .signal = r_signal,
        .wait = r_wait,
        .now_ms = r_now,
        .lock = r_lock,
        .unlock = r_unlock,
    };
    dsp_ble_tr_init(&s.tr, &s.radio, app, port);
    return true;
}

void dsp_esp_ble_stats(dsp_ble_tr_stats_t *out)
{
    dsp_ble_tr_stats(&s.tr, out);
}
