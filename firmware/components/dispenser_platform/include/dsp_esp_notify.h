#ifndef DSP_ESP_NOTIFY_H
#define DSP_ESP_NOTIFY_H

/*
 * ESP-IDF adapters for the HTTPS notification transport (milestone M1.7). Compiled and
 * cross-built; NOT verified on hardware yet (TLS heap peak, handshake time, energy and
 * Wi-Fi/BLE runtime memory are pending board measurements).
 *
 *  - HTTP port: esp_http_client over TLS with the full ESP-IDF certificate bundle
 *    (approved D3). Certificate verification and its date checks are never disabled, no
 *    certificate pinning, redirects are never followed, only https:// URLs are accepted.
 *    Timeouts: DSP_ESP_HTTP_TIMEOUT_MS per socket operation. The Authorization header is
 *    built in a static buffer that is wiped right after esp_http_client copies it, and the
 *    client's copy is deleted after every request and at the end of the session (ESP-IDF
 *    does not wipe freed heap: residual risk, documented). Headers are never logged.
 *  - Device ID: the 48-bit factory MAC (esp_efuse_mac_get_default); an identifier, never a
 *    credential.
 *  - Credentials (approved D5, development only): a dedicated NVS partition "nvs_sec",
 *    namespace "dsp_sec", keys "wifi_ssid", "wifi_pass", "api_token", provisioned with
 *    firmware/tools/provision_dev_secrets.py. Opened read-only; never written, never
 *    erased by the firmware; values are never logged. NOT encrypted: anyone with physical
 *    access can read them (documented risk; no eFuse is burned).
 */

#include <stdbool.h>
#include <stdint.h>

#include "dsp_https_transport.h"
#include "dsp_net.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_ESP_HTTP_TIMEOUT_MS 10000u /* connect+TLS and response: each <= 10 s */
#define DSP_ESP_URL_MAX_LEN 191

#define DSP_NVS_SEC_PARTITION "nvs_sec"
#define DSP_NVS_SEC_NAMESPACE "dsp_sec"
#define DSP_NVS_SEC_KEY_SSID "wifi_ssid"
#define DSP_NVS_SEC_KEY_PASS "wifi_pass"
#define DSP_NVS_SEC_KEY_TOKEN "api_token"
/* M1.10b.3: the 128-bit setup code (16-byte blob, provision_dev_secrets.py); read only by the
 * M1.10c security layer (PSK input), never logged, never returned over BLE. */
#define DSP_NVS_SEC_KEY_SETUP_CODE "setup_code"
#define DSP_SETUP_CODE_BYTES 16

/* HTTP port singleton. url must start with "https://" (else false). */
bool dsp_esp_http_configure(const char *url);
const dsp_http_port_t *dsp_esp_http_port(void);

/* Diagnostic only: one unauthenticated TLS GET of url (e.g. the backend's /health),
 * verifying certificate and clock without sending any event or credential. */
bool dsp_esp_http_probe(const char *url, dsp_http_outcome_t *out);

/* Factory MAC as 48-bit device ID. */
bool dsp_esp_device_id(uint64_t *out);

/* Monotonic milliseconds (esp_timer). */
void dsp_esp_monotonic(dsp_monotonic_t *out);

/* Initializes the "nvs_sec" partition (read-only use; never erased). false = absent,
 * unformatted or unreadable: the providers then report DSP_CRED_UNAVAILABLE / ERR. */
bool dsp_esp_secrets_init(void);
const dsp_wifi_credentials_provider_t *dsp_esp_wifi_credentials(void);
const dsp_token_provider_t *dsp_esp_api_token(void);

#ifdef __cplusplus
}
#endif

#endif /* DSP_ESP_NOTIFY_H */
