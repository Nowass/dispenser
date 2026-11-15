#ifndef DSP_IDENTITY_H
#define DSP_IDENTITY_H

/*
 * Firmware identification (pure domain code).
 *
 * The platform layer collects build facts (app version, target, ESP-IDF version)
 * and passes them in; this module only validates and formats them. It has no
 * ESP-IDF dependency and is unit-tested on the Linux host.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Human-readable product name used in logs and, later, in BLE device info. */
#define DSP_PRODUCT_NAME "Dispenser V2"

typedef enum {
    DSP_IDENTITY_OK = 0,
    DSP_IDENTITY_ERR_ARG,       /* NULL pointer, zero-size buffer or empty field */
    DSP_IDENTITY_ERR_TRUNCATED, /* buffer too small; output is still NUL-terminated */
} dsp_identity_result_t;

typedef struct {
    const char *fw_version;  /* e.g. "0.1.0-dev" (from firmware/version.txt) */
    const char *target;      /* e.g. "esp32c3" */
    const char *sdk_version; /* e.g. "v6.1"; "host" in host tests */
} dsp_identity_t;

/*
 * Formats "<product> firmware <fw_version> (<target>, <sdk_version>)" into buf.
 *
 * - All fields must be non-NULL and non-empty, otherwise DSP_IDENTITY_ERR_ARG
 *   (buf is set to "" when it is usable).
 * - If buf is too small, the output is truncated, always NUL-terminated, and
 *   DSP_IDENTITY_ERR_TRUNCATED is returned.
 */
dsp_identity_result_t dsp_identity_format(const dsp_identity_t *id, char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* DSP_IDENTITY_H */
