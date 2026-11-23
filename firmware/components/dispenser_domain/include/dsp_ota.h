/*
 * Firmware update over the air (docs/FIRMWARE_UPDATE.md, docs/BLE_PROTOCOL.md 7.8, 10.8).
 *
 * Pure domain logic: request validation, the release URL built from a compiled base (the phone
 * never sends a URL), the safety conditions and the platform port. The platform adapter
 * (dsp_esp_ota.c) downloads over verified HTTPS, checks the image (project, version, size,
 * SHA-256, signature) and switches the boot partition; the bootloader rolls back an image that
 * never confirms its boot.
 */
#ifndef DSP_OTA_H
#define DSP_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_OTA_VERSION_MAX 17      /* "65535.65535.65535" */
#define DSP_OTA_TAG_MAX 40
#define DSP_OTA_BASE_MAX 80
#define DSP_OTA_URL_MAX 192
#define DSP_OTA_SLOT_SIZE 0x1C0000u /* ota_0 / ota_1 (partitions.csv) */
#define DSP_OTA_DOSE_MARGIN_S 900   /* no update when a dose is due within 15 min */
#define DSP_OTA_MIN_BATTERY_MV 3800 /* VALID measurement of this wake (uncalibrated ADC: conservative) */
#define DSP_OTA_ASSET_PREFIX "dispenser-firmware-"
#define DSP_OTA_ASSET_SUFFIX ".bin"

/* Outcome of the most recent attempt (UpdateStatus.last_result, wire values). */
typedef enum {
    DSP_OTA_NONE = 0,
    DSP_OTA_INSTALLED = 1,       /* installed by an update and boot confirmed (also: activated, reboot pending) */
    DSP_OTA_ROLLED_BACK = 2,
    DSP_OTA_WIFI_FAILED = 3,
    DSP_OTA_DOWNLOAD_FAILED = 4,
    DSP_OTA_IMAGE_REJECTED = 5,  /* signature, project, chip, version, size or SHA-256 */
    DSP_OTA_FLASH_FAILED = 6,
    DSP_OTA_CANCELLED = 7,       /* conditions no longer met at the start */
} dsp_ota_result_t;

/* START_UPDATE verdict (wire values). */
typedef enum {
    DSP_OTA_V_ACCEPTED = 0,
    DSP_OTA_V_NOT_CAPABLE = 1,
    DSP_OTA_V_NOT_NEWER = 2,
    DSP_OTA_V_INVALID_REQUEST = 3,
    DSP_OTA_V_DOSE_SOON = 4,
    DSP_OTA_V_BATTERY = 5,
    DSP_OTA_V_NO_WIFI = 6,
    DSP_OTA_V_BUSY = 7,
} dsp_ota_verdict_t;

typedef struct {
    uint16_t major, minor, patch;
    bool pre; /* a suffix after X.Y.Z ("-dev"): older than the same X.Y.Z */
} dsp_semver_t;

/* Strict release version "X.Y.Z" (decimal, no leading zeros, each <= 65535, no suffix). */
bool dsp_ota_parse_release(const char *s, size_t len, dsp_semver_t *out);
/* Running version "X.Y.Z" optionally followed by "-<anything>" (pre-release). */
bool dsp_ota_parse_running(const char *s, dsp_semver_t *out);
/* <0, 0, >0 */
int dsp_ota_compare(const dsp_semver_t *a, const dsp_semver_t *b);

/* strnlen (not in strict C): length of s, at most max. */
size_t dsp_ota_strnlen(const char *s, size_t max);

/* [A-Za-z0-9._-], 1..max */
bool dsp_ota_id_ok(const uint8_t *s, size_t len, size_t max);

typedef struct {
    char version[DSP_OTA_VERSION_MAX + 1];
    char tag[DSP_OTA_TAG_MAX + 1];
    uint32_t size;
    uint8_t sha256[32];
    char url[DSP_OTA_URL_MAX];
} dsp_ota_job_t;

/* Validates a START_UPDATE request against the running version and builds the job with
 * url = base + tag + "/dispenser-firmware-" + version + ".bin". base must be
 * "https://github.com/<owner>/<repo>/releases/download/". Returns ACCEPTED (job filled),
 * INVALID_REQUEST or NOT_NEWER. */
dsp_ota_verdict_t dsp_ota_prepare(const char *base, const char *running, const uint8_t *version,
                                  size_t version_len, const uint8_t *tag, size_t tag_len,
                                  uint32_t size, const uint8_t sha256[32], dsp_ota_job_t *out);

/* Momentary conditions (evaluated at START_UPDATE and again right before the download). */
typedef struct {
    bool capable;          /* platform can update (rollback bootloader, signed running app) */
    bool busy;             /* update pending, degraded, inhibited or storage fault */
    bool wifi_configured;
    bool clock_trusted;
    int64_t now;
    bool next_dose_known;
    int64_t next_dose_at;
    bool battery_valid;    /* VALID measurement in this wake */
    uint16_t battery_mv;
    bool battery_low_open; /* BATTERY_LOW episode open */
} dsp_ota_conditions_t;

/* Order: NOT_CAPABLE, BUSY, NO_WIFI, DOSE_SOON (also untrusted clock), BATTERY; else ACCEPTED. */
dsp_ota_verdict_t dsp_ota_check(const dsp_ota_conditions_t *c);

/* Platform port (all members required when the port is given). */
typedef struct {
    void *ctx;
    /* The running application's version string (esp_app_desc_t.version). */
    const char *(*running_version)(void *ctx);
    /* Rollback-capable bootloader, signed running application, OTA partitions present. */
    bool (*capable)(void *ctx);
    /* Download, verify and activate (set the boot partition). Wi-Fi is already connected.
     * DSP_OTA_INSTALLED = activated: the caller restarts. Never writes the running slot. */
    dsp_ota_result_t (*install)(void *ctx, const dsp_ota_job_t *job);
    /* Software restart into the activated image (does not return on hardware). */
    void (*restart)(void *ctx);
    /* The running image waits for its boot confirmation (first boot after an update). */
    bool (*pending_verify)(void *ctx);
    /* ok: confirm (cancel rollback); !ok: mark invalid and reboot into the previous image
     * (does not return on hardware). */
    void (*confirm)(void *ctx, bool ok);
    /* The last result (record of this power cycle, else a rollback detected at boot). */
    dsp_ota_result_t (*last)(void *ctx, char version[DSP_OTA_VERSION_MAX + 1]);
    void (*record)(void *ctx, dsp_ota_result_t result, const char *version);
} dsp_ota_port_t;

#ifdef __cplusplus
}
#endif

#endif /* DSP_OTA_H */
