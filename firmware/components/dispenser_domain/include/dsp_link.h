#ifndef DSP_LINK_H
#define DSP_LINK_H

/*
 * Per-connection protocol gate before authentication (M1.10c.1). Pure domain code over the
 * dsp_bleproto codec; called by the Application Service (application task) for every complete
 * frame of the current connection. docs/SECURITY_INTEGRATION_DESIGN.md section 3.
 *
 * In M1.10c.1 there is NO security layer: nothing can be authenticated, so this gate is the
 * whole frame path and it never reaches a privileged operation:
 *   - HELLO (plain, OPEN only): version negotiation, HELLO response with the public state;
 *   - END_SESSION (plain): empty response, then THIS connection is closed; the service window
 *     (button session) continues (decision C10: only an authorized owner ends it);
 *   - HS (plain, NEGOTIATED only): HANDSHAKE_FAILED and the connection is closed (M1.10c.2
 *     hands HS to the security layer instead);
 *   - any ENCRYPTED frame: the connection is closed without a reply (no session exists);
 *   - any other request type: UNAUTHENTICATED (privileged types never decoded further);
 *   - malformed / unknown / unsupported version: the matching plain error; RESPONSE or EVENT
 *     frames from the client are malformed. Three errors close the connection.
 * Errors before authentication carry no detail that depends on secret state.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_LINK_ERRORS_MAX 3
#define DSP_LINK_UNAUTH_MS 30000u /* an unauthenticated connection is dropped after 30 s */
#define DSP_LINK_PROTO_MIN 1
#define DSP_LINK_PROTO_MAX 1

typedef enum {
    DSP_LINK_IDLE = 0,
    DSP_LINK_OPEN,
    DSP_LINK_NEGOTIATED,
} dsp_link_state_t;

typedef struct {
    uint8_t state;     /* dsp_link_state_t */
    uint32_t gen;      /* connection generation (frames of another generation are stale) */
    uint8_t errors;
    uint8_t proto;     /* negotiated major (NEGOTIATED) */
    uint64_t opened_ms;
} dsp_link_t;

/* Public state reported by HELLO (dsp_app_sec_view + the connection's session ID). */
typedef struct {
    uint8_t claim_state; /* 0 UNCLAIMED, 1 CLAIMED (also when the credentials are damaged) */
    uint8_t flags;       /* DSP_BLE_HELLO_F_* */
    uint32_t caps;
    uint32_t session_id;
    uint8_t device_id_hash[8];
} dsp_link_hello_t;

enum {
    DSP_LINK_ACT_REPLY = 1u << 0,       /* out[0..out_len) is a complete frame to send */
    DSP_LINK_ACT_CLOSE = 1u << 1,       /* drop the connection (after the reply, if any) */
};

typedef struct {
    uint8_t actions;
    size_t out_len;
    uint8_t type;   /* request type (0 when the header could not be read) */
    uint8_t error;  /* protocol error code sent (0 = none) */
} dsp_link_result_t;

void dsp_link_open(dsp_link_t *l, uint32_t gen, uint64_t now_ms);
void dsp_link_close(dsp_link_t *l);
bool dsp_link_is_open(const dsp_link_t *l);
/* Unauthenticated for longer than DSP_LINK_UNAUTH_MS (or the clock went back). */
bool dsp_link_expired(const dsp_link_t *l, uint64_t now_ms);

/* out must hold DSP_BLE_FRAME_MAX bytes. */
dsp_link_result_t dsp_link_on_frame(dsp_link_t *l, const uint8_t *frame, size_t len,
                                    const dsp_link_hello_t *hello, uint8_t *out, size_t cap);

/* A plain error frame (header + code u8, detail u32, aux u32) for any type byte. */
size_t dsp_link_error_frame(uint8_t type, uint16_t txn, uint8_t code, uint32_t detail,
                            uint32_t aux, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* DSP_LINK_H */
