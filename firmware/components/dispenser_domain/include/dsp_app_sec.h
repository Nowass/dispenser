#ifndef DSP_APP_SEC_H
#define DSP_APP_SEC_H

/*
 * RESTRICTED INTERFACE (M1.10b.3): only the M1.10c security layer (dsp_blesec.c) and host tests
 * may include this header; a build check (firmware/components/dispenser_domain/
 * sec_header_guard.cmake) fails the build otherwise. The header boundary is defense in depth,
 * NOT authorization: every function below validates at runtime
 *  - a registered security layer (cfg->security != NULL; NULL in every build before M1.10c, so
 *    nothing here can succeed and no ownership authorization can be manufactured);
 *  - an active button-wake service session (physical presence: the device advertises only
 *    after a button press);
 *  - the handshake evidence issued by dsp_app_sec_admit for THIS session: the purpose, the
 *    random nonce, the start time (DSP_APP_SEC_HS_MAX_MS), the ownership epoch at admission;
 *    it is single use (any commit attempt consumes it), so a replayed, expired or mismatched
 *    handle is refused;
 *  - the purpose's ownership preconditions, again at commit time;
 *  - for recovery additionally the RTC arm and the 3-press confirmation bound to
 *    (session ID, handshake transcript hash), consumed once.
 * The Application Service cannot verify the cryptographic handshake itself; that is the
 * security layer's job in M1.10c (Noise XXpsk3 / IK with the setup code).
 */

#include "dsp_app.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_APP_SEC_HS_MAX_MS 30000u /* admission -> commit (3 handshake messages x 10 s) */

typedef enum {
    DSP_SEC_SESSION = 1,
    DSP_SEC_CLAIM = 2,
    DSP_SEC_ADD = 3,
    DSP_SEC_RECOVERY = 4,
} dsp_sec_purpose_t;

/* Handshake evidence handle (copied to the security layer by dsp_app_sec_admit). */
typedef struct {
    uint8_t purpose;
    uint32_t session_id;
    uint64_t nonce;
} dsp_app_hs_t;

typedef struct {
    uint8_t cred_state; /* dsp_cred_state_t (CRED_DEGRADED is reported, never UNCLAIMED) */
    bool recovery_armed;
    bool invite_active;
    bool slot_free;
    uint8_t id_pub[DSP_AUTH_KEY_BYTES]; /* public identity key (zero unless CLAIMED) */
} dsp_app_sec_view_t;

/* Public state for HELLO / Info / advertising (no authorization needed, nothing secret). */
void dsp_app_sec_view(dsp_app_t *app, dsp_app_sec_view_t *out);

/* HS message 1: checks the purpose against the device state and issues the evidence handle.
 * SESSION: CLAIMED; CLAIM: UNCLAIMED (valid record); ADD: CLAIMED, valid invite, free slot;
 * RECOVERY: valid arm (CLAIMED or CRED_DEGRADED). Replaces any earlier unconsumed handle. */
dsp_app_op_result_t dsp_app_sec_admit(dsp_app_t *app, dsp_sec_purpose_t purpose, dsp_app_hs_t *hs);

/* The stored identity key pair for a SESSION or ADD handshake (does not consume the handle).
 * Read afresh from storage; the CALLER WIPES priv after use. */
dsp_app_op_result_t dsp_app_sec_identity(dsp_app_t *app, const dsp_app_hs_t *hs,
                                         uint8_t pub[DSP_AUTH_KEY_BYTES],
                                         uint8_t priv[DSP_AUTH_KEY_BYTES]);

/* The active invite token for the PSK of an ADD handshake (does not consume the handle). */
dsp_app_op_result_t dsp_app_sec_invite_token(dsp_app_t *app, const dsp_app_hs_t *hs,
                                             uint8_t token[16]);

/* SESSION commit (IK completed): consumes the handle; the phone key must be enrolled. Issues
 * the session's owner authorization (*out). last_seen is updated per the O8 policy. */
dsp_app_op_result_t dsp_app_sec_lookup(dsp_app_t *app, const dsp_app_hs_t *hs,
                                       const uint8_t phone_pub[DSP_AUTH_KEY_BYTES],
                                       dsp_app_auth_t *out);

/* CLAIM / ADD commit (XXpsk3 completed): consumes the handle, re-checks the preconditions and
 * writes "auth" once (CLAIM: with the new identity key pair new_priv/new_pub; ADD: they must be
 * NULL; the ADD invite is cleared and saved BEFORE the write). Issues the authorization. */
dsp_app_op_result_t dsp_app_sec_enroll(dsp_app_t *app, const dsp_app_hs_t *hs,
                                       const uint8_t phone_pub[DSP_AUTH_KEY_BYTES],
                                       const char *name, size_t name_len,
                                       const uint8_t *new_priv, const uint8_t *new_pub,
                                       dsp_app_auth_t *out);

/* RECOVERY: the handshake completed; consumes the handle and starts the confirmation bound to
 * (session, transcript_hash). The security layer then sends EV_CONFIRM_ON_DEVICE. */
dsp_app_op_result_t dsp_app_sec_recovery_begin(dsp_app_t *app, const dsp_app_hs_t *hs,
                                               const uint8_t transcript_hash[32]);
/* Current confirmation state (evaluates the deadline). An abort counts against the arm. */
dsp_confirm_state_t dsp_app_sec_recovery_state(dsp_app_t *app);
/* Applies a CONFIRMED recovery with the same session and transcript hash (single use): arm
 * cleared and saved, readiness SERVICE_REQUIRED (durable), then the new "auth" record.
 * Preserves history, schedules, configuration, time, notifications and every dosing field
 * except readiness (manual_hw never lowered). Issues the authorization for slot 0. */
dsp_app_op_result_t dsp_app_sec_recovery_apply(dsp_app_t *app, const uint8_t transcript_hash[32],
                                               const uint8_t phone_pub[DSP_AUTH_KEY_BYTES],
                                               const char *name, size_t name_len,
                                               const uint8_t new_priv[DSP_AUTH_KEY_BYTES],
                                               const uint8_t new_pub[DSP_AUTH_KEY_BYTES],
                                               dsp_app_auth_t *out);

/* The stored identity key pair is inconsistent: dosing is inhibited IMMEDIATELY for the rest of
 * the wake, then KEY_FAULT is persisted (epoch + 1) so later wakes stay CRED_DEGRADED. The
 * inhibit does not depend on the write. */
dsp_app_op_result_t dsp_app_sec_key_fault(dsp_app_t *app);

/* A handshake failed: counted in failed_sessions at most once per session. */
void dsp_app_sec_handshake_failed(dsp_app_t *app);

#ifdef __cplusplus
}
#endif

#endif /* DSP_APP_SEC_H */
