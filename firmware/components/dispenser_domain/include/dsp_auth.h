#ifndef DSP_AUTH_H
#define DSP_AUTH_H

/*
 * Credential record "auth" (milestone M1.10b.3, storage layout v5): the device identity key
 * pair, up to four independently revocable phone slots, the ownership epoch and diagnostic
 * counters. Pure domain code: no cryptography and no I/O. The domain stores and validates key
 * BYTES; key generation, X25519 and the handshakes belong to the M1.10c security layer.
 * Design: docs/OWNERSHIP_DESIGN.md (revision 2, approved).
 *
 * The setup code is NOT part of this record (it lives only in the read-only nvs_sec partition
 * and is read by the security layer), and nothing derived from it is stored.
 *
 * Validity (strict; anything else is CRED_DEGRADED for the device, never "unclaimed"):
 *  - UNCLAIMED: identity, flags and every slot all zero (epoch and counters may be non-zero);
 *  - CLAIMED: identity private and public key non-zero, at least one used slot;
 *  - used slot: public key non-zero, different from the identity public key and from every
 *    other used slot's key, name well-formed UTF-8 without control characters (<= 24 bytes),
 *    instants 0 or in range; unused slot: all zero;
 *  - KEY_FAULT only together with CLAIMED (decodes, but the device is CRED_DEGRADED).
 *
 * owner_epoch increases by one at every ownership change (claim, add, remove, recovery, key
 * fault, future factory reset) and never decreases; RTC invites and recovery arms are bound to
 * it, so any ownership change invalidates every older one.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dsp_time.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_AUTH_SLOTS 4
#define DSP_AUTH_KEY_BYTES 32
#define DSP_AUTH_NAME_MAX 24

typedef enum {
    DSP_AUTH_UNCLAIMED = 0,
    DSP_AUTH_CLAIMED = 1,
} dsp_auth_claim_t;

enum {
    DSP_AUTH_F_KEY_FAULT = 1u << 0, /* identity key pair found inconsistent */
};
#define DSP_AUTH_F_ALL 0x01u

typedef struct {
    bool used;
    uint8_t name_len;
    uint8_t pub[DSP_AUTH_KEY_BYTES];
    char name[DSP_AUTH_NAME_MAX]; /* not NUL-terminated; zero padded */
    dsp_utc_t enrolled_at;        /* 0 = clock untrusted at enrollment */
    dsp_utc_t last_seen;          /* 0 = never / clock untrusted */
} dsp_auth_slot_t;

typedef struct {
    uint8_t claim_state; /* dsp_auth_claim_t */
    uint8_t flags;       /* DSP_AUTH_F_* */
    uint32_t owner_epoch;
    uint8_t id_priv[DSP_AUTH_KEY_BYTES];
    uint8_t id_pub[DSP_AUTH_KEY_BYTES];
    dsp_utc_t claimed_at;   /* current ownership began (claim or recovery); 0 = untrusted */
    dsp_utc_t recovered_at; /* last recovery; 0 = never / untrusted */
    uint32_t recoveries;      /* saturating */
    uint32_t failed_sessions; /* sessions with >= 1 failed handshake; saturating */
    dsp_auth_slot_t slot[DSP_AUTH_SLOTS];
} dsp_auth_t;

typedef enum {
    DSP_AUTH_OK = 0,
    DSP_AUTH_ERR_INVALID,   /* argument, key or name */
    DSP_AUTH_ERR_STATE,     /* claimed/unclaimed/fault precondition */
    DSP_AUTH_ERR_SLOTS_FULL,
    DSP_AUTH_ERR_DUPLICATE_KEY,
    DSP_AUTH_ERR_NOT_FOUND, /* slot not in use */
    DSP_AUTH_ERR_LAST_SLOT, /* removing the last used slot is refused */
    DSP_AUTH_ERR_EPOCH,     /* owner_epoch would overflow (unreachable in practice) */
} dsp_auth_result_t;

/* Provisioning value: UNCLAIMED, epoch 0, everything zero. */
void dsp_auth_provisioned(dsp_auth_t *a);
bool dsp_auth_is_valid(const dsp_auth_t *a);
bool dsp_auth_is_provisioning(const dsp_auth_t *a);
bool dsp_auth_equal(const dsp_auth_t *x, const dsp_auth_t *y);

/* A valid CLAIMED record without KEY_FAULT. */
bool dsp_auth_usable(const dsp_auth_t *a);

int dsp_auth_find(const dsp_auth_t *a, const uint8_t pub[DSP_AUTH_KEY_BYTES]); /* slot, -1 */
int dsp_auth_free_slot(const dsp_auth_t *a);                                   /* lowest, -1 */
uint8_t dsp_auth_used_count(const dsp_auth_t *a);

/* Name rule of a slot (also used for the enrollment input). */
bool dsp_auth_name_valid(const char *name, size_t len);

/* Pure transitions on *a (a valid record); *a is unchanged on refusal. Every ownership
 * transition increments owner_epoch. now_or_0: trusted UTC, or 0 when the clock is untrusted. */
dsp_auth_result_t dsp_auth_claim(dsp_auth_t *a, const uint8_t id_priv[DSP_AUTH_KEY_BYTES],
                                 const uint8_t id_pub[DSP_AUTH_KEY_BYTES],
                                 const uint8_t phone_pub[DSP_AUTH_KEY_BYTES], const char *name,
                                 size_t name_len, dsp_utc_t now_or_0);
dsp_auth_result_t dsp_auth_add(dsp_auth_t *a, const uint8_t phone_pub[DSP_AUTH_KEY_BYTES],
                               const char *name, size_t name_len, dsp_utc_t now_or_0,
                               uint8_t *slot_out);
dsp_auth_result_t dsp_auth_remove(dsp_auth_t *a, uint8_t slot);
dsp_auth_result_t dsp_auth_mark_key_fault(dsp_auth_t *a);

/* Recovery: builds a NEW record in *out (new identity, the new phone in slot 0, every other
 * slot cleared, KEY_FAULT cleared). prev: the previous record when it was decodable (its
 * counters carry over), or NULL when it was missing/corrupt. min_epoch: the new epoch is
 * max(prev epoch + 1, min_epoch, 1). */
dsp_auth_result_t dsp_auth_recover(const dsp_auth_t *prev, uint32_t min_epoch,
                                   const uint8_t id_priv[DSP_AUTH_KEY_BYTES],
                                   const uint8_t id_pub[DSP_AUTH_KEY_BYTES],
                                   const uint8_t phone_pub[DSP_AUTH_KEY_BYTES], const char *name,
                                   size_t name_len, dsp_utc_t now_or_0, dsp_auth_t *out);

/* last_seen policy (approved O8): updated only with a trusted clock and when the stored value
 * is 0 or at least DSP_AUTH_LAST_SEEN_MIN_S older. Returns true when *a changed (write due). */
#define DSP_AUTH_LAST_SEEN_MIN_S 3600
bool dsp_auth_touch(dsp_auth_t *a, uint8_t slot, dsp_utc_t now_or_0);

/* Counts a failed-handshake session (saturating). Returns true when *a changed. */
bool dsp_auth_count_failed_session(dsp_auth_t *a);

/* Volatile zeroization of a whole record (key material). */
void dsp_auth_wipe(dsp_auth_t *a);
/* Volatile zeroization of any buffer. */
void dsp_secure_zero(void *p, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* DSP_AUTH_H */
