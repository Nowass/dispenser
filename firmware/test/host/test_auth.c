/*
 * M1.10b.3 unit tests (pure domain code): the credential record "auth" (model, strict
 * validation, golden bytes, transitions, ownership epoch, zeroization), the RTC-retained block
 * format 2 with the ownership overlays, the overlay validity rules (reset reasons, expiry,
 * clock continuity, epoch binding) and the recovery confirmation state machine.
 *
 * All key material is synthetic (repeated byte patterns generated here); nothing is printed.
 */
#include "dsp_auth.h"
#include "dsp_codec.h"
#include "dsp_owner.h"
#include "dsp_power.h"
#include "dsp_test.h"

#include <stdint.h>
#include <string.h>

#define T1 ((dsp_utc_t)1795158000) /* 2026-11-20T07:00:00Z */

static void key(uint8_t out[32], uint8_t v)
{
    memset(out, v, 32);
}

static dsp_auth_t claimed(void)
{
    dsp_auth_t a;
    dsp_auth_provisioned(&a);
    uint8_t ip[32], iu[32], p0[32];
    key(ip, 0xA1);
    key(iu, 0xB1);
    key(p0, 0x11);
    DSP_CHECK_EQ_INT(DSP_AUTH_OK, dsp_auth_claim(&a, ip, iu, p0, "Phone A", 7, T1));
    return a;
}

/* ---- Codec ------------------------------------------------------------------------------- */

static void test_auth_golden_bytes(void)
{
    dsp_auth_t a = claimed();
    uint8_t p1[32];
    key(p1, 0x22);
    uint8_t slot = 9;
    DSP_CHECK_EQ_INT(DSP_AUTH_OK, dsp_auth_add(&a, p1, "B", 1, 0, &slot));
    DSP_CHECK_EQ_INT(1, slot);
    a.slot[0].last_seen = T1 + 3600;
    a.recoveries = 0x01020304u;
    a.failed_sessions = 7;
    uint8_t b[DSP_AUTH_RECORD_BYTES + 4];
    DSP_CHECK_EQ_INT(DSP_AUTH_RECORD_BYTES, dsp_codec_encode_auth(&a, b, sizeof b));
    /* Expected image, built field by field (independent of the encoder). */
    uint8_t e[DSP_AUTH_RECORD_BYTES];
    memset(e, 0, sizeof e);
    e[0] = 8;   /* type */
    e[1] = 1;   /* format */
    e[2] = 1;   /* CLAIMED */
    e[3] = 0;   /* flags */
    e[4] = 2;   /* epoch 2 (claim + add) */
    memset(e + 8, 0xA1, 32);
    memset(e + 40, 0xB1, 32);
    const uint8_t t1[8] = {0xF0, 0xEF, 0xFF, 0x6A, 0, 0, 0, 0}; /* 1795158000 = 0x6AFFEFF0 */
    memcpy(e + 72, t1, 8);                                      /* claimed_at */
    e[88] = 0x04; e[89] = 0x03; e[90] = 0x02; e[91] = 0x01;     /* recoveries */
    e[92] = 7;                                                  /* failed_sessions */
    uint8_t *s0 = e + 96, *s1 = e + 96 + 80;
    s0[0] = 1; s0[1] = 7;
    memset(s0 + 4, 0x11, 32);
    memcpy(s0 + 36, "Phone A", 7);
    memcpy(s0 + 64, t1, 8);
    const uint8_t t2[8] = {0x00, 0xFE, 0xFF, 0x6A, 0, 0, 0, 0}; /* + 3600 = 0x6AFFFE00 */
    memcpy(s0 + 72, t2, 8);
    s1[0] = 1; s1[1] = 1;
    memset(s1 + 4, 0x22, 32);
    s1[36] = 'B';
    DSP_CHECK(memcmp(b, e, sizeof e) == 0);
    dsp_auth_t back;
    DSP_CHECK_EQ_INT(DSP_CODEC_OK, dsp_codec_decode_auth(b, DSP_AUTH_RECORD_BYTES, &back));
    DSP_CHECK(dsp_auth_equal(&a, &back));
    /* Provisioning value: type, format, then zeros. */
    dsp_auth_t p;
    dsp_auth_provisioned(&p);
    DSP_CHECK_EQ_INT(DSP_AUTH_RECORD_BYTES, dsp_codec_encode_auth(&p, b, sizeof b));
    DSP_CHECK(b[0] == 8 && b[1] == 1);
    for (size_t i = 2; i < DSP_AUTH_RECORD_BYTES; i++) {
        DSP_CHECK(b[i] == 0);
    }
    DSP_CHECK_EQ_INT(DSP_CODEC_OK, dsp_codec_decode_auth(b, DSP_AUTH_RECORD_BYTES, &back));
    DSP_CHECK(dsp_auth_is_provisioning(&back));
    /* Size, type and version. */
    DSP_CHECK_EQ_INT(0, dsp_codec_encode_auth(&a, b, DSP_AUTH_RECORD_BYTES - 1));
    DSP_CHECK_EQ_INT(DSP_AUTH_RECORD_BYTES, dsp_codec_encode_auth(&a, b, sizeof b));
    DSP_CHECK_EQ_INT(DSP_CODEC_ERR_SIZE, dsp_codec_decode_auth(b, DSP_AUTH_RECORD_BYTES - 1, &back));
    DSP_CHECK_EQ_INT(DSP_CODEC_ERR_SIZE, dsp_codec_decode_auth(b, DSP_AUTH_RECORD_BYTES + 1, &back));
    b[1] = 2;
    DSP_CHECK_EQ_INT(DSP_CODEC_ERR_VERSION, dsp_codec_decode_auth(b, DSP_AUTH_RECORD_BYTES, &back));
    b[1] = 1;
    b[0] = 7;
    DSP_CHECK_EQ_INT(DSP_CODEC_ERR_TYPE, dsp_codec_decode_auth(b, DSP_AUTH_RECORD_BYTES, &back));
}

/* Every validity rule, at the byte level (decoder) and the model level (encoder). */
static void test_auth_strict_validation(void)
{
    dsp_auth_t a = claimed();
    uint8_t good[DSP_AUTH_RECORD_BYTES];
    DSP_CHECK_EQ_INT(DSP_AUTH_RECORD_BYTES, dsp_codec_encode_auth(&a, good, sizeof good));
    struct {
        size_t off;
        uint8_t val;
    } bad[] = {
        {2, 2},            /* claim state */
        {3, 2},            /* unknown flag */
        {4, 0}, {5, 0},    /* epoch 0 with CLAIMED (offset 4 = 1 -> 0) */
        {96 + 0, 2},       /* used not 0/1 */
        {96 + 1, 25},      /* name too long */
        {96 + 2, 1},       /* reserved */
        {96 + 60, 1},      /* reserved */
        {96 + 36 + 7, 'x'},/* byte after the name (padding) */
        {96 + 36, 0x07},   /* control character in the name */
        {96 + 36, 0xC0},   /* invalid UTF-8 */
        {96 + 80, 1},      /* unused slot marked used with a zero key */
        {96 + 80 + 4, 1},  /* unused slot with key bytes */
        {96 + 80 + 64, 1}, /* unused slot with a time */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        uint8_t v[DSP_AUTH_RECORD_BYTES];
        memcpy(v, good, sizeof v);
        if (bad[i].off == 4) {
            v[4] = 0; /* epoch 0 */
        } else if (bad[i].off != 5) {
            v[bad[i].off] = bad[i].val;
        } else {
            continue;
        }
        dsp_auth_t out;
        memset(&out, 0x5A, sizeof out);
        DSP_CHECK_EQ_INT(DSP_CODEC_ERR_FIELD, dsp_codec_decode_auth(v, sizeof v, &out));
    }
    /* Identity zero while CLAIMED; private/public separately. */
    for (int part = 0; part < 2; part++) {
        uint8_t v[DSP_AUTH_RECORD_BYTES];
        memcpy(v, good, sizeof v);
        memset(v + (part == 0 ? 8 : 40), 0, 32);
        dsp_auth_t out;
        DSP_CHECK_EQ_INT(DSP_CODEC_ERR_FIELD, dsp_codec_decode_auth(v, sizeof v, &out));
    }
    /* Claimed without any used slot. */
    {
        uint8_t v[DSP_AUTH_RECORD_BYTES];
        memcpy(v, good, sizeof v);
        memset(v + 96, 0, 80);
        dsp_auth_t out;
        DSP_CHECK_EQ_INT(DSP_CODEC_ERR_FIELD, dsp_codec_decode_auth(v, sizeof v, &out));
    }
    /* Model level. */
    dsp_auth_t x;
    x = a; x.slot[1] = x.slot[0]; /* duplicate phone key */
    DSP_CHECK(!dsp_auth_is_valid(&x));
    DSP_CHECK_EQ_INT(0, dsp_codec_encode_auth(&x, good, sizeof good));
    x = a; memcpy(x.slot[0].pub, x.id_pub, 32); /* phone key = identity key */
    DSP_CHECK(!dsp_auth_is_valid(&x));
    x = a; x.slot[0].enrolled_at = 5; /* implausible instant */
    DSP_CHECK(!dsp_auth_is_valid(&x));
    x = a; x.claimed_at = DSP_UTC_MAX + 1;
    DSP_CHECK(!dsp_auth_is_valid(&x));
    /* UNCLAIMED with residue. */
    dsp_auth_t u;
    dsp_auth_provisioned(&u);
    u.owner_epoch = 9;
    u.recoveries = 2;
    u.recovered_at = T1;
    DSP_CHECK(dsp_auth_is_valid(&u)); /* epoch and counters may remain */
    DSP_CHECK(!dsp_auth_is_provisioning(&u));
    u.id_pub[0] = 1;
    DSP_CHECK(!dsp_auth_is_valid(&u));
    dsp_auth_provisioned(&u);
    u.flags = DSP_AUTH_F_KEY_FAULT;
    DSP_CHECK(!dsp_auth_is_valid(&u)); /* KEY_FAULT only with CLAIMED */
    dsp_auth_provisioned(&u);
    u.claimed_at = T1;
    DSP_CHECK(!dsp_auth_is_valid(&u));
    /* KEY_FAULT: valid record, not usable. */
    x = a;
    DSP_CHECK_EQ_INT(DSP_AUTH_OK, dsp_auth_mark_key_fault(&x));
    DSP_CHECK(dsp_auth_is_valid(&x) && !dsp_auth_usable(&x));
    DSP_CHECK_EQ_INT(a.owner_epoch + 1, x.owner_epoch);
}

/* ---- Transitions ------------------------------------------------------------------------- */

static void test_auth_transitions(void)
{
    uint8_t ip[32], iu[32], k[6][32];
    key(ip, 0xA1);
    key(iu, 0xB1);
    for (int i = 0; i < 6; i++) {
        key(k[i], (uint8_t)(0x11 + i));
    }
    dsp_auth_t a, before;
    dsp_auth_provisioned(&a);
    /* Claim argument checks leave *a unchanged. */
    uint8_t zero[32];
    memset(zero, 0, sizeof zero);
    before = a;
    DSP_CHECK_EQ_INT(DSP_AUTH_ERR_INVALID, dsp_auth_claim(&a, zero, iu, k[0], "", 0, T1));
    DSP_CHECK_EQ_INT(DSP_AUTH_ERR_INVALID, dsp_auth_claim(&a, ip, zero, k[0], "", 0, T1));
    DSP_CHECK_EQ_INT(DSP_AUTH_ERR_INVALID, dsp_auth_claim(&a, ip, iu, zero, "", 0, T1));
    DSP_CHECK_EQ_INT(DSP_AUTH_ERR_DUPLICATE_KEY, dsp_auth_claim(&a, ip, iu, iu, "", 0, T1));
    char long_name[26];
    memset(long_name, 'n', sizeof long_name);
    DSP_CHECK_EQ_INT(DSP_AUTH_ERR_INVALID, dsp_auth_claim(&a, ip, iu, k[0], long_name, 25, T1));
    DSP_CHECK_EQ_INT(DSP_AUTH_ERR_INVALID, dsp_auth_claim(&a, ip, iu, k[0], "a\nb", 3, T1));
    DSP_CHECK(dsp_auth_equal(&a, &before));
    /* 24 bytes of UTF-8 are fine; an untrusted clock leaves the instants 0. */
    static const char NAME24[] = "\xC4\x8C" "e" "\xC5\xA1" "k" "\xC3\xBD" " telefon 1234567";
    DSP_CHECK_EQ_INT(24, (int)sizeof NAME24 - 1);
    DSP_CHECK_EQ_INT(DSP_AUTH_OK, dsp_auth_claim(&a, ip, iu, k[0], NAME24, 24, 0));
    DSP_CHECK_EQ_INT(DSP_AUTH_CLAIMED, a.claim_state);
    DSP_CHECK_EQ_INT(1, a.owner_epoch);
    DSP_CHECK_EQ_INT(0, a.claimed_at);
    DSP_CHECK_EQ_INT(0, a.slot[0].enrolled_at);
    DSP_CHECK_EQ_INT(DSP_AUTH_ERR_STATE, dsp_auth_claim(&a, ip, iu, k[1], "", 0, T1));
    /* Add up to four; the fifth is refused. */
    for (int i = 1; i < 4; i++) {
        uint8_t slot = 0;
        DSP_CHECK_EQ_INT(DSP_AUTH_OK, dsp_auth_add(&a, k[i], "p", 1, T1, &slot));
        DSP_CHECK_EQ_INT(i, slot);
        DSP_CHECK_EQ_INT((uint32_t)(1 + i), a.owner_epoch);
    }
    before = a;
    DSP_CHECK_EQ_INT(DSP_AUTH_ERR_SLOTS_FULL, dsp_auth_add(&a, k[4], "p", 1, T1, NULL));
    DSP_CHECK_EQ_INT(DSP_AUTH_ERR_DUPLICATE_KEY, dsp_auth_add(&a, k[2], "p", 1, T1, NULL));
    DSP_CHECK(dsp_auth_equal(&a, &before));
    /* Remove one: only that slot changes; slot reuse takes the lowest free slot. */
    DSP_CHECK_EQ_INT(DSP_AUTH_OK, dsp_auth_remove(&a, 2));
    DSP_CHECK(!a.slot[2].used);
    for (int i = 0; i < 4; i++) {
        if (i != 2) {
            DSP_CHECK(memcmp(&a.slot[i], &before.slot[i], sizeof a.slot[i]) == 0);
        }
    }
    DSP_CHECK_EQ_INT(before.owner_epoch + 1, a.owner_epoch);
    DSP_CHECK_EQ_INT(DSP_AUTH_ERR_NOT_FOUND, dsp_auth_remove(&a, 2));
    DSP_CHECK_EQ_INT(DSP_AUTH_ERR_INVALID, dsp_auth_remove(&a, 4));
    uint8_t slot = 0;
    DSP_CHECK_EQ_INT(DSP_AUTH_OK, dsp_auth_add(&a, k[5], "", 0, T1, &slot));
    DSP_CHECK_EQ_INT(2, slot);
    DSP_CHECK_EQ_INT(DSP_AUTH_OK, dsp_auth_remove(&a, 0));
    DSP_CHECK_EQ_INT(DSP_AUTH_OK, dsp_auth_remove(&a, 1));
    DSP_CHECK_EQ_INT(DSP_AUTH_OK, dsp_auth_remove(&a, 2));
    before = a;
    DSP_CHECK_EQ_INT(DSP_AUTH_ERR_LAST_SLOT, dsp_auth_remove(&a, 3));
    DSP_CHECK(dsp_auth_equal(&a, &before));
    /* Key fault blocks add/remove; recovery from it carries the counters, new identity. */
    a.failed_sessions = 3;
    a.recoveries = 1;
    DSP_CHECK_EQ_INT(DSP_AUTH_OK, dsp_auth_mark_key_fault(&a));
    uint32_t e = a.owner_epoch;
    DSP_CHECK_EQ_INT(DSP_AUTH_OK, dsp_auth_mark_key_fault(&a)); /* idempotent, no new epoch */
    DSP_CHECK_EQ_INT(e, a.owner_epoch);
    DSP_CHECK_EQ_INT(DSP_AUTH_ERR_STATE, dsp_auth_add(&a, k[4], "", 0, T1, NULL));
    DSP_CHECK_EQ_INT(DSP_AUTH_ERR_STATE, dsp_auth_remove(&a, 3));
    uint8_t ip2[32], iu2[32];
    key(ip2, 0xA2);
    key(iu2, 0xB2);
    dsp_auth_t r;
    DSP_CHECK_EQ_INT(DSP_AUTH_OK, dsp_auth_recover(&a, 0, ip2, iu2, k[4], "R", 1, T1, &r));
    DSP_CHECK(dsp_auth_usable(&r));
    DSP_CHECK_EQ_INT(e + 1, r.owner_epoch);
    DSP_CHECK_EQ_INT(2, r.recoveries);
    DSP_CHECK_EQ_INT(3, r.failed_sessions);
    DSP_CHECK_EQ_INT(T1, r.recovered_at);
    DSP_CHECK(r.slot[0].used && memcmp(r.slot[0].pub, k[4], 32) == 0);
    DSP_CHECK(!r.slot[1].used && !r.slot[2].used && !r.slot[3].used);
    DSP_CHECK(memcmp(r.id_priv, ip2, 32) == 0 && r.flags == 0);
    /* In place (prev == out) and from a missing record with a minimum epoch. */
    dsp_auth_t inplace = a;
    DSP_CHECK_EQ_INT(DSP_AUTH_OK, dsp_auth_recover(&inplace, 0, ip2, iu2, k[4], "R", 1, T1, &inplace));
    DSP_CHECK(dsp_auth_equal(&inplace, &r));
    DSP_CHECK_EQ_INT(DSP_AUTH_OK, dsp_auth_recover(NULL, 40, ip2, iu2, k[4], "", 0, 0, &r));
    DSP_CHECK_EQ_INT(40, r.owner_epoch);
    DSP_CHECK_EQ_INT(1, r.recoveries);
    DSP_CHECK_EQ_INT(DSP_AUTH_OK, dsp_auth_recover(NULL, 0, ip2, iu2, k[4], "", 0, 0, &r));
    DSP_CHECK_EQ_INT(1, r.owner_epoch);
    DSP_CHECK_EQ_INT(DSP_AUTH_ERR_DUPLICATE_KEY, dsp_auth_recover(NULL, 0, ip2, iu2, iu2, "", 0, 0, &r));
    /* Epoch saturation: refused, never wraps (UINT32_MAX is the degraded sentinel). */
    dsp_auth_t sat = claimed();
    sat.owner_epoch = UINT32_MAX - 1;
    DSP_CHECK_EQ_INT(DSP_AUTH_ERR_EPOCH, dsp_auth_add(&sat, k[3], "", 0, T1, NULL));
    DSP_CHECK_EQ_INT(DSP_AUTH_ERR_EPOCH, dsp_auth_recover(&sat, 0, ip2, iu2, k[4], "", 0, 0, &r));
}

static void test_auth_touch_and_counters(void)
{
    dsp_auth_t a = claimed();
    DSP_CHECK(!dsp_auth_touch(&a, 0, 0));         /* untrusted clock: never */
    DSP_CHECK(!dsp_auth_touch(&a, 1, T1));        /* unused slot */
    DSP_CHECK(!dsp_auth_touch(&a, 0, 1000));      /* implausible */
    DSP_CHECK(dsp_auth_touch(&a, 0, T1));         /* first */
    DSP_CHECK(!dsp_auth_touch(&a, 0, T1 + 3599)); /* rate limited (O8) */
    DSP_CHECK(!dsp_auth_touch(&a, 0, T1 - 100));  /* clock went back */
    DSP_CHECK(dsp_auth_touch(&a, 0, T1 + 3600));
    DSP_CHECK_EQ_INT(T1 + 3600, a.slot[0].last_seen);
    a.failed_sessions = UINT32_MAX - 1;
    DSP_CHECK(dsp_auth_count_failed_session(&a));
    DSP_CHECK(!dsp_auth_count_failed_session(&a)); /* saturating */
    /* Zeroization. */
    dsp_auth_wipe(&a);
    const uint8_t *p = (const uint8_t *)&a;
    for (size_t i = 0; i < sizeof a; i++) {
        DSP_CHECK(p[i] == 0);
    }
}

/* ---- RTC-retained block format 2 ------------------------------------------------------- */

static dsp_power_retained_t block(void)
{
    dsp_power_retained_t r;
    dsp_power_retained_defaults(&r);
    r.failsafe_count = 2;
    r.wake_count = 9;
    r.saved_raw = T1;
    uint8_t tok[16];
    memset(tok, 0x5C, sizeof tok);
    dsp_owner_invite_set(&r, 1, 4, tok, T1 - 10);
    dsp_owner_arm_set(&r, 4, T1 - 20);
    return r;
}

static void test_retained_format2(void)
{
    dsp_power_retained_t r = block();
    uint8_t img[DSP_POWER_RETAINED_BYTES];
    DSP_CHECK_EQ_INT(120, DSP_POWER_RETAINED_BYTES);
    DSP_CHECK(dsp_power_retained_encode(&r, img));
    DSP_CHECK_EQ_INT(2, img[4]);
    DSP_CHECK_EQ_INT(120, img[5]);
    dsp_power_retained_t out;
    DSP_CHECK(dsp_power_retained_decode(img, sizeof img, &out));
    DSP_CHECK(!out.overlays_dropped);
    DSP_CHECK_EQ_INT(1, out.invite.creator_slot);
    DSP_CHECK_EQ_INT(4, out.arm.owner_epoch);
    DSP_CHECK_EQ_INT(T1 - 10 + DSP_OWNER_INVITE_S, out.invite.deadline_raw);
    DSP_CHECK_EQ_INT(T1 - 20 + DSP_OWNER_ARM_S, out.arm.deadline_raw);
    DSP_CHECK(memcmp(out.invite.token, r.invite.token, 16) == 0);
    /* A torn byte: the CRC rejects the whole block. */
    img[60] ^= 1;
    DSP_CHECK(!dsp_power_retained_decode(img, sizeof img, &out));
    /* Format 1 (48 bytes, M1.9b) is discarded after the update. */
    DSP_CHECK(!dsp_power_retained_decode(img, 48, &out));
    /* CRC-valid but inconsistent overlays: only the overlays are dropped (CRC is not
     * authentication); the fail-safe counter and the rest survive. */
    struct {
        int which;
    } cases[] = {{0}, {1}, {2}, {3}, {4}, {5}, {6}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        dsp_power_retained_t x = block();
        switch (cases[i].which) {
        case 0: x.invite.deadline_raw += 1; break;            /* window length */
        case 1: x.arm.deadline_raw += 3600; break;             /* extended arm */
        case 2: x.arm.aborts = 3; break;
        case 3: x.invite.creator_slot = 4; break;
        case 4: x.invite.valid = 0; break;                     /* unused but not zero */
        case 5: memset(x.invite.token, 0, 16); break;          /* zero token */
        default: x.arm.owner_epoch = 0; break;
        }
        /* encode refuses an invalid block, so forge the image with a correct CRC */
        dsp_power_retained_t ok = block();
        uint8_t im[DSP_POWER_RETAINED_BYTES];
        DSP_CHECK(dsp_power_retained_encode(&ok, im));
        if (cases[i].which == 0) {
            im[84] = (uint8_t)(im[84] + 1);
        } else if (cases[i].which == 1) {
            uint64_t v = 0;
            for (int b = 0; b < 8; b++) v |= (uint64_t)im[108 + b] << (8 * b);
            v += 3600;
            for (int b = 0; b < 8; b++) im[108 + b] = (uint8_t)(v >> (8 * b));
        } else if (cases[i].which == 2) {
            im[93] = 3;
        } else if (cases[i].which == 3) {
            im[53] = 4;
        } else if (cases[i].which == 4) {
            im[52] = 0;
        } else if (cases[i].which == 5) {
            memset(im + 60, 0, 16);
        } else {
            memset(im + 96, 0, 4);
        }
        uint32_t crc = dsp_power_crc32(im, 116);
        for (int b = 0; b < 4; b++) im[116 + b] = (uint8_t)(crc >> (8 * b));
        dsp_power_retained_t o2;
        DSP_CHECK(dsp_power_retained_decode(im, sizeof im, &o2));
        DSP_CHECK(o2.overlays_dropped);
        DSP_CHECK_EQ_INT(0, o2.invite.valid);
        DSP_CHECK_EQ_INT(0, o2.arm.armed);
        DSP_CHECK_EQ_INT(2, o2.failsafe_count);
        DSP_CHECK_EQ_INT(9, o2.wake_count);
        (void)x;
    }
}

/* ---- Overlay rules ------------------------------------------------------------------------ */

static void test_overlay_reset_reasons(void)
{
    const dsp_wake_cause_t causes[] = {DSP_CAUSE_POWER_ON, DSP_CAUSE_TIMER,    DSP_CAUSE_BUTTON,
                                       DSP_CAUSE_SOFTWARE, DSP_CAUSE_WATCHDOG, DSP_CAUSE_PANIC,
                                       DSP_CAUSE_BROWNOUT};
    for (size_t i = 0; i < sizeof causes / sizeof causes[0]; i++) {
        for (int valid = 0; valid < 2; valid++) {
            dsp_power_retained_t r = block();
            dsp_owner_on_wake(&r, causes[i], valid == 1, T1);
            bool keep = valid == 1 && (causes[i] == DSP_CAUSE_TIMER || causes[i] == DSP_CAUSE_BUTTON);
            DSP_CHECK_EQ_INT(keep ? 1 : 0, r.invite.valid);
            DSP_CHECK_EQ_INT(keep ? 1 : 0, r.arm.armed);
            DSP_CHECK_EQ_INT(2, r.failsafe_count); /* the rest of the block is not touched */
        }
    }
    /* Repeated resets: each clears; nothing comes back. */
    dsp_power_retained_t r = block();
    dsp_owner_on_wake(&r, DSP_CAUSE_SOFTWARE, true, T1);
    dsp_owner_on_wake(&r, DSP_CAUSE_TIMER, true, T1 + 1);
    DSP_CHECK(!r.invite.valid && !r.arm.armed);
    /* Raw clock below the last save (or the creation time): cleared. */
    r = block();
    dsp_owner_on_wake(&r, DSP_CAUSE_BUTTON, true, T1 - 1);
    DSP_CHECK(!r.invite.valid && !r.arm.armed);
    r = block();
    r.saved_raw = 0;
    dsp_owner_on_wake(&r, DSP_CAUSE_BUTTON, true, T1 - 15); /* after arm, before invite */
    DSP_CHECK(!r.invite.valid && r.arm.armed);
}

static void test_overlay_expiry_and_binding(void)
{
    dsp_power_retained_t r = block();
    dsp_utc_t inv_dl = r.invite.deadline_raw, arm_dl = r.arm.deadline_raw;
    /* Boundaries: valid at deadline - 1, invalid at the deadline. */
    DSP_CHECK(dsp_owner_invite_valid(&r, inv_dl - 1, 4));
    DSP_CHECK(!dsp_owner_invite_valid(&r, inv_dl, 4));
    DSP_CHECK(!dsp_owner_invite_valid(&r, inv_dl + 1, 4));
    DSP_CHECK(dsp_owner_arm_valid(&r, arm_dl - 1, 4));
    DSP_CHECK(!dsp_owner_arm_valid(&r, arm_dl, 4));
    /* Deep sleeps never extend: many TIMER wakes keep the original deadline. */
    for (int i = 0; i < 50; i++) {
        dsp_owner_on_wake(&r, DSP_CAUSE_TIMER, true, T1 + i * 10);
        r.saved_raw = T1 + i * 10;
    }
    DSP_CHECK_EQ_INT(inv_dl, r.invite.deadline_raw);
    DSP_CHECK_EQ_INT(arm_dl, r.arm.deadline_raw);
    dsp_owner_on_wake(&r, DSP_CAUSE_BUTTON, true, inv_dl);
    DSP_CHECK(!r.invite.valid && r.arm.armed);
    dsp_owner_on_wake(&r, DSP_CAUSE_BUTTON, true, arm_dl);
    DSP_CHECK(!r.arm.armed);
    /* Epoch binding. */
    r = block();
    DSP_CHECK(!dsp_owner_invite_valid(&r, T1, 5));
    DSP_CHECK(!dsp_owner_invite_valid(&r, T1, DSP_OWNER_EPOCH_DEGRADED));
    DSP_CHECK(!dsp_owner_arm_valid(&r, T1, 3));
    DSP_CHECK(!dsp_owner_invite_valid(&r, T1 - 11, 4)); /* before its creation (clock back) */
    dsp_owner_arm_set(&r, DSP_OWNER_EPOCH_DEGRADED, T1);
    DSP_CHECK(dsp_owner_arm_valid(&r, T1 + 1, DSP_OWNER_EPOCH_DEGRADED));
    DSP_CHECK(!dsp_owner_arm_valid(&r, T1 + 1, 4));
    /* Aborts: the third disarms. */
    dsp_owner_arm_abort(&r);
    dsp_owner_arm_abort(&r);
    DSP_CHECK(r.arm.armed && r.arm.aborts == 2);
    dsp_owner_arm_abort(&r);
    DSP_CHECK(!r.arm.armed && r.arm.aborts == 0);
    /* Raw clock near 1970 on a never-synced device is fine. */
    dsp_owner_arm_set(&r, 4, 7);
    DSP_CHECK(dsp_power_overlays_consistent(&r));
    DSP_CHECK(dsp_owner_arm_valid(&r, 8, 4));
}

/* ---- Confirmation ------------------------------------------------------------------------ */

static dsp_button_event_t press(uint64_t at, uint64_t len, uint8_t kind)
{
    dsp_button_event_t e = {kind, at, at + len};
    return e;
}

static const uint8_t TR[32] = {1, 2, 3};

static dsp_confirm_state_t run(const dsp_button_event_t *ev, size_t n, uint64_t poll_at)
{
    dsp_confirm_t c;
    dsp_confirm_begin(&c, 77, TR, 1000);
    for (size_t i = 0; i < n; i++) {
        dsp_confirm_event(&c, &ev[i]);
    }
    return dsp_confirm_poll(&c, poll_at);
}

static void test_confirmation(void)
{
    const uint8_t S = DSP_BUTTON_SHORT, L = DSP_BUTTON_LONG;
    /* Exactly three short presses, then 700 ms of silence. */
    dsp_button_event_t ok[] = {press(2000, 100, S), press(2400, 100, S), press(2800, 100, S)};
    DSP_CHECK_EQ_INT(DSP_CONFIRM_PENDING, run(ok, 3, 2900 + 699));
    DSP_CHECK_EQ_INT(DSP_CONFIRM_CONFIRMED, run(ok, 3, 2900 + 700));
    /* Two presses: aborted at the deadline (start + 20 s). */
    DSP_CHECK_EQ_INT(DSP_CONFIRM_PENDING, run(ok, 2, 1000 + 19999));
    DSP_CHECK_EQ_INT(DSP_CONFIRM_ABORTED, run(ok, 2, 1000 + 20000));
    /* A fourth press within the sequence aborts. */
    dsp_button_event_t four[] = {ok[0], ok[1], ok[2], press(3200, 100, S)};
    DSP_CHECK_EQ_INT(DSP_CONFIRM_ABORTED, run(four, 4, 5000));
    /* A long press or a too long short press aborts. */
    dsp_button_event_t lng[] = {ok[0], press(2400, 100, L), ok[2]};
    DSP_CHECK_EQ_INT(DSP_CONFIRM_ABORTED, run(lng, 3, 5000));
    dsp_button_event_t slow[] = {ok[0], press(2400, 401, S), press(3301, 100, S)};
    DSP_CHECK_EQ_INT(DSP_CONFIRM_ABORTED, run(slow, 3, 5000));
    /* A gap above 500 ms aborts (a new sequence is not a continuation). */
    dsp_button_event_t gap[] = {ok[0], press(2601, 100, S), press(3001, 100, S)};
    DSP_CHECK_EQ_INT(DSP_CONFIRM_ABORTED, run(gap, 3, 5000));
    dsp_button_event_t gap_ok[] = {ok[0], press(2600, 100, S), press(3200, 100, S)};
    DSP_CHECK_EQ_INT(DSP_CONFIRM_CONFIRMED, run(gap_ok, 3, 4000));
    /* Events before start + 300 ms (queued, held at entry, replayed) never count. */
    dsp_button_event_t early[] = {press(500, 100, S), press(1100, 100, S), press(1299, 50, S),
                                  ok[0], ok[1], ok[2]};
    DSP_CHECK_EQ_INT(DSP_CONFIRM_CONFIRMED, run(early, 6, 4000));
    dsp_button_event_t early_only[] = {press(500, 100, S), press(800, 100, S), press(1100, 100, S)};
    DSP_CHECK_EQ_INT(DSP_CONFIRM_ABORTED, run(early_only, 3, 1000 + 20000));
    /* Out of order / overlapping / malformed events abort. */
    dsp_button_event_t back[] = {ok[1], ok[0], ok[2]};
    DSP_CHECK_EQ_INT(DSP_CONFIRM_ABORTED, run(back, 3, 5000));
    dsp_button_event_t mal = {S, 3000, 2900};
    DSP_CHECK_EQ_INT(DSP_CONFIRM_ABORTED, run(&mal, 1, 3500));
    /* The three presses must end before the deadline. */
    dsp_button_event_t late[] = {press(20200, 100, S), press(20500, 100, S), press(20800, 300, S)};
    DSP_CHECK_EQ_INT(DSP_CONFIRM_ABORTED, run(late, 3, 22000));
    dsp_button_event_t edge[] = {press(20200, 100, S), press(20500, 100, S), press(20800, 200, S)};
    DSP_CHECK_EQ_INT(DSP_CONFIRM_CONFIRMED, run(edge, 3, 21700));
    /* Monotonic clock going back aborts; a finished state is final. */
    dsp_confirm_t c;
    dsp_confirm_begin(&c, 77, TR, 1000);
    DSP_CHECK_EQ_INT(DSP_CONFIRM_ABORTED, dsp_confirm_poll(&c, 999));
    dsp_confirm_event(&c, &ok[0]);
    DSP_CHECK_EQ_INT(0, c.presses);
    DSP_CHECK_EQ_INT(DSP_CONFIRM_ABORTED, dsp_confirm_poll(&c, 5000));
    dsp_confirm_reset(&c);
    DSP_CHECK_EQ_INT(DSP_CONFIRM_NONE, dsp_confirm_poll(&c, 5000));
    DSP_CHECK_EQ_INT(0, c.session_id);
}

int main(void)
{
    DSP_RUN(test_auth_golden_bytes);
    DSP_RUN(test_auth_strict_validation);
    DSP_RUN(test_auth_transitions);
    DSP_RUN(test_auth_touch_and_counters);
    DSP_RUN(test_retained_format2);
    DSP_RUN(test_overlay_reset_reasons);
    DSP_RUN(test_overlay_expiry_and_binding);
    DSP_RUN(test_confirmation);
    return dsp_test_finish();
}
