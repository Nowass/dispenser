#ifndef DSP_NVS_KV_H
#define DSP_NVS_KV_H

/*
 * ESP-IDF NVS implementation of dsp_kv_t (milestone M1.3).
 *
 * - One namespace per instance; values are stored as NVS blobs (nvs_set_blob), which
 *   NVS replaces atomically per key (old or new value after power loss), EXCEPT for one
 *   window found in M1.10b.1 (firmware/test/nvs_linux): a power cut after the new version's
 *   last data chunk and before its blob index makes ESP-IDF v6.1's recovery erase the old
 *   version too, so the key is missing (reported by dsp_storage as CORRUPT, fail closed).
 * - nvs_commit() is called by commit(). In ESP-IDF v6.1 nvs_set_blob() is already
 *   durable when it returns OK and nvs_commit() is a no-op; the adapter still calls it,
 *   as the API requires.
 * - Never requests an erase or reformat (no nvs_flash_erase*() calls):
 *   ESP_ERR_NVS_NO_FREE_PAGES and ESP_ERR_NVS_NEW_VERSION_FOUND are reported to the caller
 *   (examples erase here; this project preserves the data for maintenance and recovery).
 *   ESP-IDF's nvs_flash_init_partition() itself may still write to flash internally:
 *   initialize blank pages, complete or revert an interrupted operation, or erase and
 *   rewrite individual pages during its recovery. That is library behavior this adapter
 *   neither performs nor can prevent.
 * - Runtime behavior on real flash has NOT been verified yet (no hardware).
 */

#include <stdbool.h>
#include <stdint.h>

#include "dsp_kv.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_NVS_DEFAULT_PARTITION "nvs"
#define DSP_NVS_NAMESPACE "dispenser"

typedef enum {
    DSP_NVS_OK = 0,
    DSP_NVS_ERR_ARG,
    DSP_NVS_ERR_NO_FREE_PAGES,  /* partition needs maintenance; no erase requested */
    DSP_NVS_ERR_NEW_VERSION,    /* NVS data from a newer ESP-IDF format; no erase requested */
    DSP_NVS_ERR_INIT,           /* any other nvs_flash_init_partition failure */
    DSP_NVS_ERR_OPEN,           /* nvs_open_from_partition failure */
} dsp_nvs_status_t;

typedef struct {
    uint32_t handle;   /* nvs_handle_t */
    bool open;
    bool read_only;
    bool ns_missing;   /* read-only open of a namespace that does not exist yet */
    int32_t last_err;  /* last esp_err_t, for diagnostics/logging */
} dsp_nvs_kv_t;

/*
 * Initializes the NVS partition (the adapter requests no erase; ESP-IDF's init may perform
 * its internal recovery, see above) and opens the namespace.
 * read_only: open with NVS_READONLY; a missing namespace is reported through get()
 * returning DSP_KV_NOT_FOUND for every key (set/commit fail). On success *kv is bound.
 */
dsp_nvs_status_t dsp_nvs_kv_open(dsp_nvs_kv_t *nvs, const char *partition, const char *ns,
                                 bool read_only, dsp_kv_t *kv);

void dsp_nvs_kv_close(dsp_nvs_kv_t *nvs);

#ifdef __cplusplus
}
#endif

#endif /* DSP_NVS_KV_H */
