#ifndef DSP_BLESEC_H
#define DSP_BLESEC_H

/*
 * BLE security session (M1.10c.2a; docs/SECURITY_INTEGRATION_DESIGN.md sections 2-7,
 * docs/BLE_PROTOCOL.md section 11). Per connection: the M1.10c.1 pre-authentication gate
 * (dsp_link) for HELLO / END_SESSION / errors, the Noise handshakes (XXpsk3 for CLAIM, ADD,
 * RECOVERY; IK for SESSION) with the pinned noise-c, encrypted frames (header = AD) and the
 * authorization-bound dispatcher with the response cache.
 *
 * Registration: dsp_blesec_init() fills a dsp_app_security_t for dsp_app_config_t.security.
 * Everything runs in the application task. State is static (no allocation except noise-c's own
 * handshake and cipher objects, freed and zeroized at every close).
 *
 * Randomness (decision C2): noise-c draws from libsodium (randombytes_buf); the platform registers
 * the hardware implementation before sodium_init() (dsp_esp_rng, ESP-IDF) and reports through
 * entropy_ready() whether a hardware entropy source is active. No handshake starts and no session
 * ID or token is generated otherwise. Deterministic randomness exists only in the host tests.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dsp_app.h"
#include "dsp_bleproto.h"
#include "dsp_link.h"
#include "dsp_rescache.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_BLESEC_STEP_MS 10000u           /* per handshake message */
#define DSP_BLESEC_MSG_LIMIT 0xFFFFFFFFu     /* messages per direction (2^32 - 1), then close */
#define DSP_BLESEC_CLOSING_NOTICE_S 30u     /* EV_SESSION_CLOSING before the window maximum */
#define DSP_BLESEC_NAME_MAX 24

typedef struct {
    /* Hardware entropy active (BT controller enabled). false: no handshake, no session ID. */
    bool (*entropy_ready)(void *ctx);
    /* The same source for the Application Service's session IDs, tokens and nonces. */
    bool (*random)(void *ctx, uint8_t *out, size_t len);
    /* 16-byte per-device setup code (nvs_sec); the caller of this hook never keeps it. */
    bool (*read_setup_code)(void *ctx, uint8_t out[16]);
    void *ctx;
    uint8_t device_id[6]; /* factory MAC: device_id_hash of an unclaimed device */
} dsp_blesec_config_t;

typedef enum {
    DSP_BLESEC_IDLE = 0,
    DSP_BLESEC_OPEN,        /* connected, before HELLO */
    DSP_BLESEC_NEGOTIATED,  /* HELLO answered */
    DSP_BLESEC_HANDSHAKING, /* XX message 1 answered, waiting for message 3 */
    DSP_BLESEC_CONFIRMING,  /* RECOVERY: transport keys exist, 3-press confirmation pending */
    DSP_BLESEC_AUTH,        /* authenticated: encrypted requests */
} dsp_blesec_state_t;

struct NoiseHandshakeState_s;
struct NoiseCipherState_s;

typedef struct {
    dsp_blesec_config_t cfg;
    /* per connection */
    uint8_t state;
    uint32_t conn_count;   /* increments at every close: detects a close during an operation */
    dsp_link_t link;
    dsp_link_hello_t hello; /* the values of this connection's HELLO response */
    uint8_t proto;
    uint8_t purpose;
    uint16_t hs_txn;        /* txn of HS message 3 (ENROLL_RESULT) */
    /* the Application Service's handshake evidence handle (dsp_app_hs_t, restricted type),
     * kept from admission to the commit; zeroized at close */
    uint64_t hs_handle[3];
    uint64_t step_deadline_ms;
    struct NoiseHandshakeState_s *hs;
    struct NoiseCipherState_s *tx_cs;
    struct NoiseCipherState_s *rx_cs;
    uint32_t tx_count, rx_count;
    bool closing_sent;
    /* issued owner authorization of this session (AUTH) */
    dsp_app_auth_t auth;
    bool authed;
    /* pending enrollment / recovery material (zeroized at close) */
    uint8_t new_priv[32], new_pub[32];
    uint8_t phone_pub[32];
    uint8_t name[DSP_BLESEC_NAME_MAX];
    uint8_t name_len;
    uint8_t transcript[32];
    /* diagnostics (not secret): source line of the last handshake failure path, and counts */
    uint16_t fail_line;
    uint32_t hs_failures;
    /* identity key consistency checked once per boot (wake) */
    bool key_checked;
    /* buffers (static: frame-sized, never on the stack) */
    uint8_t plain[DSP_BLE_FRAME_MAX];
    uint8_t out[DSP_BLE_FRAME_MAX];
    dsp_ble_msg_t req;
    dsp_ble_msg_t rsp;
    dsp_rescache_t cache;
} dsp_blesec_t;

/* Fills *sec (hooks + ctx = s) for dsp_app_config_t.security. */
void dsp_blesec_init(dsp_blesec_t *s, const dsp_blesec_config_t *cfg, dsp_app_security_t *sec);

#ifdef __cplusplus
}
#endif

#endif /* DSP_BLESEC_H */
