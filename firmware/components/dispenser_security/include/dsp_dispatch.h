#ifndef DSP_DISPATCH_H
#define DSP_DISPATCH_H

/*
 * Authorization-bound dispatcher (M1.10c.2a, docs/SECURITY_INTEGRATION_DESIGN.md section 5.2):
 * one decrypted, decoded request of an AUTHENTICATED session -> exactly one dsp_app_* operation,
 * called with the authorization the Application Service issued to this session (never one built
 * from frame contents; no frame field selects a slot or a role). Every operation re-verifies the
 * authorization at runtime (M1.10c.1). Results map 1:1 to protocol errors (BLE_PROTOCOL.md 10.1);
 * recognized operations this firmware does not provide answer UNAVAILABLE (C5).
 *
 * Uses only the public dsp_app.h operations (never the restricted dsp_app_sec.h).
 */

#include <stdbool.h>

#include "dsp_app.h"
#include "dsp_bleproto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool no_response;  /* the operation closed the connection (MANUAL_DOSE suspends BLE) */
    bool close_after;  /* the caller's own slot was removed: close after the response */
} dsp_dispatch_fx_t;

/* req: decoded plaintext request (h.flags without ENCRYPTED). *rsp: the plaintext response
 * (type and txn of the request; RESPONSE or RESPONSE|ERROR). */
void dsp_dispatch(dsp_app_t *app, const dsp_app_auth_t *auth, const dsp_ble_msg_t *req,
                  dsp_ble_msg_t *rsp, dsp_dispatch_fx_t *fx);

/* Protocol error code for an operation result (0 for DSP_OP_OK). */
uint8_t dsp_dispatch_error_code(dsp_app_op_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* DSP_DISPATCH_H */
