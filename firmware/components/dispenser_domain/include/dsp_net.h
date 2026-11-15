#ifndef DSP_NET_H
#define DSP_NET_H

/*
 * Connectivity interfaces and the Wi-Fi session manager (milestone M1.4). Pure domain
 * code: the ESP-IDF implementations live in components/dispenser_platform.
 *
 * Wi-Fi is never kept running: a session is opened for a bounded number of connection
 * attempts, used (SNTP now; HTTPS notifications later can share the same open session),
 * then shut down. No credentials in source, logs or tests; the credential provider may
 * report DSP_CRED_UNAVAILABLE (approved: interface only until BLE provisioning).
 */

#include <stdbool.h>
#include <stdint.h>

#include "dsp_timekeeping.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Credentials (held in RAM only for the duration of a connection attempt) ------- */

#define DSP_WIFI_SSID_MAX_LEN 32
#define DSP_WIFI_PASSPHRASE_MAX_LEN 63

typedef struct {
    char ssid[DSP_WIFI_SSID_MAX_LEN + 1];             /* 1..32 bytes */
    char passphrase[DSP_WIFI_PASSPHRASE_MAX_LEN + 1]; /* "" (open) or 8..63 (WPA2/WPA3) */
} dsp_wifi_credentials_t;

typedef enum {
    DSP_CRED_OK = 0,
    DSP_CRED_UNAVAILABLE, /* not provisioned (the only state before BLE provisioning) */
    DSP_CRED_ERR,         /* storage/read failure */
} dsp_cred_status_t;

typedef struct {
    void *ctx;
    dsp_cred_status_t (*get)(void *ctx, dsp_wifi_credentials_t *out);
} dsp_wifi_credentials_provider_t;

/* Provider that always reports DSP_CRED_UNAVAILABLE (the M1.4 default). */
extern const dsp_wifi_credentials_provider_t DSP_WIFI_NO_CREDENTIALS;

bool dsp_wifi_credentials_valid(const dsp_wifi_credentials_t *c);

/* ---- Wi-Fi driver (platform) ------------------------------------------------------ */

typedef enum {
    DSP_NET_OK = 0,
    DSP_NET_ERR_ARG,
    DSP_NET_ERR_STATE,
    DSP_NET_ERR_NO_CREDENTIALS,
    DSP_NET_ERR_START,   /* radio/driver could not start */
    DSP_NET_ERR_TIMEOUT, /* no connection within the timeout */
    DSP_NET_ERR_AUTH,    /* rejected credentials: not retried */
    DSP_NET_ERR_NO_AP,   /* network not found */
    DSP_NET_ERR_CONNECT, /* other connection failure */
} dsp_net_status_t;

typedef struct {
    void *ctx;
    dsp_net_status_t (*start)(void *ctx);
    /* One blocking attempt, bounded by timeout_ms; DSP_NET_OK once an IP is obtained. */
    dsp_net_status_t (*connect)(void *ctx, const dsp_wifi_credentials_t *creds, uint32_t timeout_ms);
    void (*disconnect)(void *ctx);
    void (*stop)(void *ctx); /* idempotent: radio off, resources released */
} dsp_wifi_driver_t;

/* ---- Wi-Fi session manager ---------------------------------------------------------- */

typedef enum {
    DSP_WIFI_DISABLED = 0,
    DSP_WIFI_STARTING,
    DSP_WIFI_CONNECTING,
    DSP_WIFI_CONNECTED,
    DSP_WIFI_FAILED,   /* last session failed; radio already stopped */
    DSP_WIFI_STOPPING,
} dsp_wifi_state_t;

#define DSP_WIFI_TIMEOUT_DEFAULT_MS 15000u
#define DSP_WIFI_TIMEOUT_MIN_MS 1000u
#define DSP_WIFI_TIMEOUT_MAX_MS 60000u
#define DSP_WIFI_ATTEMPTS_DEFAULT 3u
#define DSP_WIFI_ATTEMPTS_MAX 5u

typedef struct {
    uint32_t connect_timeout_ms; /* per attempt */
    uint8_t max_attempts;        /* 1..DSP_WIFI_ATTEMPTS_MAX */
} dsp_wifi_config_t;

typedef struct {
    const dsp_wifi_driver_t *drv;
    const dsp_wifi_credentials_provider_t *creds;
    dsp_wifi_config_t cfg;
    dsp_wifi_state_t state;
    dsp_net_status_t last_error;
    uint8_t attempts_used;
    bool initialized;
} dsp_wifi_manager_t;

dsp_net_status_t dsp_wifi_init(dsp_wifi_manager_t *m, const dsp_wifi_driver_t *drv,
                               const dsp_wifi_credentials_provider_t *creds,
                               const dsp_wifi_config_t *cfg);

/*
 * Opens a session: credentials -> start -> up to max_attempts connection attempts (an
 * authentication failure ends the session at once). DSP_NET_OK: CONNECTED (an already
 * connected session is reused). Any failure: radio stopped, state FAILED. Never loops
 * beyond max_attempts; credentials are wiped from RAM before returning.
 */
dsp_net_status_t dsp_wifi_connect(dsp_wifi_manager_t *m);

/* Ends any session: disconnect (if connected) and stop. Idempotent. State DISABLED. */
void dsp_wifi_shutdown(dsp_wifi_manager_t *m);

dsp_wifi_state_t dsp_wifi_state(const dsp_wifi_manager_t *m);

/* ---- Time source (platform SNTP) and clock port ----------------------------------- */

typedef struct {
    void *ctx;
    /* One bounded synchronization over the open network. On DSP_SYNC_OK *out holds the
     * source time and the local clock at the same moment. MUST NOT change the clock. */
    dsp_sync_status_t (*sync)(void *ctx, uint32_t timeout_ms, dsp_time_sample_t *out);
} dsp_time_source_t;

typedef struct {
    void *ctx;
    dsp_utc_t (*now)(void *ctx);              /* raw system time, seconds */
    bool (*set)(void *ctx, dsp_utc_t utc);    /* step the system clock */
    void (*mark_synced)(void *ctx, dsp_utc_t last_sync); /* refresh the continuity marker */
} dsp_clock_port_t;

#ifdef __cplusplus
}
#endif

#endif /* DSP_NET_H */
