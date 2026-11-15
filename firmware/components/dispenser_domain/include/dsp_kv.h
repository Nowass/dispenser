#ifndef DSP_KV_H
#define DSP_KV_H

/*
 * Minimal key-value storage interface used by the persistent repositories (M1.3).
 * Implemented by the ESP-IDF NVS adapter (components/dispenser_platform) and by host
 * test fakes. Pure interface: no ESP-IDF types.
 *
 * Contract (verified against the ESP-IDF v6.1 NVS sources, see firmware/README.md):
 *  - Keys: 1..15 printable ASCII characters (NVS limit). Values: opaque byte blobs.
 *  - set(key, value) replaces ONE key atomically: after a power loss at any point the key
 *    holds the old or the new value, never a mix. Nothing spanning several keys is
 *    atomic.
 *  - set() may already be durable when it returns (NVS: it is); commit() returning
 *    DSP_KV_OK guarantees durability of all previous successful set() calls.
 *  - get() returns DSP_KV_NOT_FOUND for a missing key. A corrupted value may ALSO appear
 *    as missing (NVS erases items that fail its CRC checks), which is why every key is
 *    provisioned explicitly and a missing key is treated as corruption (dsp_storage.h).
 *  - There is deliberately no erase operation: keys are never deleted, only overwritten.
 *  - Not thread-safe; single owner (the future dosing service task).
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_KV_KEY_MAX_LEN 15

typedef enum {
    DSP_KV_OK = 0,
    DSP_KV_NOT_FOUND,     /* key absent (never written, or erased by NVS as corrupt) */
    DSP_KV_ERR_TOO_LARGE, /* stored value larger than the caller's buffer */
    DSP_KV_ERR_READ,
    DSP_KV_ERR_WRITE,
    DSP_KV_ERR_NO_SPACE,  /* partition full */
    DSP_KV_ERR_COMMIT,
    DSP_KV_ERR_ARG,
} dsp_kv_status_t;

typedef struct {
    void *ctx;
    /* Copies the value into buf (capacity cap) and sets *len to its size. */
    dsp_kv_status_t (*get)(void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *len);
    dsp_kv_status_t (*set)(void *ctx, const char *key, const uint8_t *data, size_t len);
    dsp_kv_status_t (*commit)(void *ctx);
} dsp_kv_t;

#ifdef __cplusplus
}
#endif

#endif /* DSP_KV_H */
