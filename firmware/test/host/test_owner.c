/*
 * M1.10b.3 tests: device ownership through the Application Service, in complete wakes over the
 * real domain services (reuses the test_app_ops harness: KV power cuts at any write, device
 * clock, pump oracle, BLE fake that runs scripted steps from its wait()).
 *
 * The security layer (M1.10c) does not exist: a fake registration provides a deterministic
 * "RNG", and test hooks play its role (admit -> handshake -> commit). All keys are synthetic
 * byte patterns; nothing secret is printed.
 *
 * Covered: no security layer (nothing can be authorized), claim/add/revoke/recovery, issued
 * authorization (forged, replayed, other session, other epoch), handshake evidence (missing,
 * mismatched, expired, replayed), invite and arm RTC rules (expiry boundaries, every reset type,
 * clock steps, epoch binding, consumed invite restored from an old RTC image), the physical
 * confirmation (binding, queued and replayed events, aborts), recovery preservation (history,
 * schedules, configuration, time, notifications, every dosing field except readiness, manual
 * high-water marks, safety holds), power loss at every ownership write and inside recovery,
 * corrupt and unsupported credentials (dosing blocked, never "unclaimed"), KEY_FAULT (inhibited
 * at once even when not durable), slot reuse with the manual floor, durable admission and the
 * session-local limits.
 */
#define DSP_APP_OPS_NO_MAIN
#include "test_app_ops.c"

#include "dsp_app_sec.h"

/* ---- Fake security-layer registration ---------------------------------------------------- */

static uint32_t rng_state;
static bool rng_fail;
static bool fake_random(void *ctx, uint8_t *out, size_t len)
{
    (void)ctx;
    if (rng_fail) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        rng_state = rng_state * 1103515245u + 12345u;
        out[i] = (uint8_t)(rng_state >> 16);
    }
    return true;
}
static const dsp_app_security_t SEC = {NULL, fake_random};

static void k32(uint8_t out[32], uint8_t v)
{
    memset(out, v, 32);
}
#define PHONE(n) ((uint8_t)(0x11 + (n))) /* phone n's static public key pattern */
#define IDPRIV(n) ((uint8_t)(0xA0 + (n)))
#define IDPUB(n) ((uint8_t)(0xB0 + (n)))

/* ---- Hooks: the security layer's calls inside a session ---------------------------------- */

static dsp_app_auth_t g_auth[8]; /* issued authorization per phone index */
static dsp_app_hs_t g_hs;
static uint8_t g_token[16];
static uint8_t g_rtc_copy[DSP_POWER_RETAINED_BYTES];
static size_t g_rtc_copy_len;
static uint8_t TRANS[4][32];

static op_t hk(void (*h)(int), int a, int b)
{
    op_t o = mk(OP_HOOK);
    o.hook = h;
    o.a = a;
    o.b = b;
    return o;
}

static void h_admit(int i) /* a = purpose */
{
    sc.res[i].r = dsp_app_sec_admit(&app, (dsp_sec_purpose_t)sc.op[i].a, &g_hs);
}
static void h_tamper(int i) /* a: 0 nonce, 1 purpose, 2 session */
{
    if (sc.op[i].a == 0) {
        g_hs.nonce ^= 1u;
    } else if (sc.op[i].a == 1) {
        g_hs.purpose = DSP_SEC_ADD;
    } else {
        g_hs.session_id ^= 0x100u;
    }
}
static void h_enroll(int i) /* a = phone, b = identity index (0 = ADD: no new identity) */
{
    uint8_t p[32], ip[32], iu[32];
    k32(p, PHONE(sc.op[i].a));
    k32(ip, IDPRIV(sc.op[i].b));
    k32(iu, IDPUB(sc.op[i].b));
    char name[8];
    int n = snprintf(name, sizeof name, "P%d", sc.op[i].a);
    sc.res[i].r = dsp_app_sec_enroll(&app, &g_hs, p, name, (size_t)n, sc.op[i].b ? ip : NULL,
                                     sc.op[i].b ? iu : NULL, &g_auth[sc.op[i].a]);
}
static void h_claim(int i) /* admit + enroll */
{
    sc.res[i].r = dsp_app_sec_admit(&app, DSP_SEC_CLAIM, &g_hs);
    if (sc.res[i].r.status == DSP_OP_OK) {
        h_enroll(i);
    }
}
static void h_session(int i) /* a = phone */
{
    sc.res[i].r = dsp_app_sec_admit(&app, DSP_SEC_SESSION, &g_hs);
    if (sc.res[i].r.status == DSP_OP_OK) {
        uint8_t p[32];
        k32(p, PHONE(sc.op[i].a));
        sc.res[i].r = dsp_app_sec_lookup(&app, &g_hs, p, &g_auth[sc.op[i].a]);
    }
}
static void h_lookup_again(int i) /* replays the last handle (no new admission) */
{
    uint8_t p[32];
    k32(p, PHONE(sc.op[i].a));
    dsp_app_auth_t out;
    sc.res[i].r = dsp_app_sec_lookup(&app, &g_hs, p, &out);
}
static void h_add(int i) /* a = phone */
{
    sc.res[i].r = dsp_app_sec_admit(&app, DSP_SEC_ADD, &g_hs);
    if (sc.res[i].r.status == DSP_OP_OK) {
        uint8_t t[16];
        sc.res[i].r = dsp_app_sec_invite_token(&app, &g_hs, t);
        if (sc.res[i].r.status == DSP_OP_OK && memcmp(t, g_token, 16) != 0) {
            sc.res[i].r.status = DSP_OP_INVALID; /* not the token the owner was given */
        }
        if (sc.res[i].r.status == DSP_OP_OK) {
            int b = sc.op[i].b;
            sc.op[i].b = 0;
            h_enroll(i);
            sc.op[i].b = b;
        }
    }
}
static void h_invite(int i) /* a = owner phone */
{
    uint16_t exp = 0;
    sc.res[i].r = dsp_app_invite_create(&app, &g_auth[sc.op[i].a], g_token, &exp);
    sc.res[i].detail = exp;
}
static void h_invite_cancel(int i)
{
    sc.res[i].r = dsp_app_invite_cancel(&app, &g_auth[sc.op[i].a]);
}
static void h_recovery_cancel(int i)
{
    sc.res[i].r = dsp_app_recovery_cancel(&app, &g_auth[sc.op[i].a]);
}
static void h_remove(int i) /* a = owner phone, b = slot */
{
    bool ends = false;
    sc.res[i].r = dsp_app_phone_remove(&app, &g_auth[sc.op[i].a], (uint8_t)sc.op[i].b, &ends);
    sc.res[i].state = ends;
}
static void h_list(int i) /* a = owner phone */
{
    dsp_app_phone_t ph[DSP_APP_PHONE_SLOTS];
    uint8_t n = 0;
    sc.res[i].r = dsp_app_phones_list(&app, &g_auth[sc.op[i].a], ph, &n);
    sc.res[i].detail = n;
}
static dsp_app_auth_t g_forged;
static void h_list_forged(int i)
{
    dsp_app_phone_t ph[DSP_APP_PHONE_SLOTS];
    uint8_t n = 0;
    sc.res[i].r = dsp_app_phones_list(&app, &g_forged, ph, &n);
}
static void h_view(int i) /* state = cred, detail bit 0 armed, bit 1 invite, bit 2 slot free */
{
    dsp_app_sec_view_t v;
    dsp_app_sec_view(&app, &v);
    sc.res[i].state = v.cred_state;
    sc.res[i].detail = (v.recovery_armed ? 1u : 0u) | (v.invite_active ? 2u : 0u) |
                       (v.slot_free ? 4u : 0u);
}
static void h_recover_begin(int i) /* a = transcript */
{
    sc.res[i].r = dsp_app_sec_admit(&app, DSP_SEC_RECOVERY, &g_hs);
    if (sc.res[i].r.status == DSP_OP_OK) {
        sc.res[i].r = dsp_app_sec_recovery_begin(&app, &g_hs, TRANS[sc.op[i].a]);
    }
}
static void h_press(int i) /* a = presses, b: 0 normal, 1 timestamps before the operation */
{
    uint64_t base = sc.op[i].b == 1 ? 0 : mono_ms + 400;
    for (int k = 0; k < sc.op[i].a; k++) {
        dsp_button_event_t e = {DSP_BUTTON_SHORT, base + (uint64_t)k * 400, base + (uint64_t)k * 400 + 100};
        (void)dsp_app_post_button(&app, &e);
    }
}
static void h_state(int i)
{
    sc.res[i].state = (uint8_t)dsp_app_sec_recovery_state(&app);
}
static void h_apply(int i) /* a = transcript, b = phone (identity index 2 + b) */
{
    uint8_t p[32], ip[32], iu[32];
    k32(p, PHONE(sc.op[i].b));
    k32(ip, IDPRIV(2 + sc.op[i].b));
    k32(iu, IDPUB(2 + sc.op[i].b));
    sc.res[i].r = dsp_app_sec_recovery_apply(&app, TRANS[sc.op[i].a], p, "R", 1, ip, iu,
                                             &g_auth[sc.op[i].b]);
}
static void h_key_fault(int i)
{
    sc.res[i].r = dsp_app_sec_key_fault(&app);
}
static void h_save_rtc(int i)
{
    (void)i;
    memcpy(g_rtc_copy, rtc, sizeof g_rtc_copy);
    g_rtc_copy_len = rtc_len;
}
static void h_clock_step(int i) /* the platform clock is stepped (as an NTP correction would) */
{
    g_offset += sc.op[i].a;
}
static void h_identity(int i)
{
    uint8_t pub[32], priv[32];
    sc.res[i].r = dsp_app_sec_admit(&app, DSP_SEC_SESSION, &g_hs);
    if (sc.res[i].r.status == DSP_OP_OK) {
        sc.res[i].r = dsp_app_sec_identity(&app, &g_hs, pub, priv);
        uint8_t ep[32], eu[32];
        k32(ep, IDPRIV(sc.op[i].a));
        k32(eu, IDPUB(sc.op[i].a));
        sc.res[i].state = memcmp(pub, eu, 32) == 0 && memcmp(priv, ep, 32) == 0;
        dsp_secure_zero(priv, sizeof priv);
    }
}

/* ---- Helpers -------------------------------------------------------------------------------- */

static dsp_auth_t durable_auth(bool *ok)
{
    dsp_auth_t a;
    memset(&a, 0, sizeof a);
    const fake_kv_entry_t *e = fake_kv_find(&kv, "auth");
    bool good = e != NULL && dsp_codec_decode_auth(e->value, e->len, &a) == DSP_CODEC_OK;
    if (ok != NULL) {
        *ok = good;
    }
    return a;
}

static dsp_dosing_state_t durable_dosing(void)
{
    dsp_dosing_state_t d;
    memset(&d, 0, sizeof d);
    const fake_kv_entry_t *e = fake_kv_find(&kv, "dosing");
    DSP_CHECK(e != NULL && dsp_codec_decode_dosing(e->value, e->len, &d) == DSP_CODEC_OK);
    return d;
}

static void put_dosing(const dsp_dosing_state_t *d)
{
    uint8_t b[DSP_DOSING_RECORD_BYTES];
    DSP_CHECK_EQ_INT(DSP_DOSING_RECORD_BYTES, dsp_codec_encode_dosing(d, b, sizeof b));
    fake_kv_put_raw(&kv, "dosing", b, sizeof b);
}

/* Provisioned device (READY or not) with the fake security layer registered. */
static void setup(bool ready)
{
    provision(ready);
    CFG.security = &SEC;
    rng_state = 0x1234567u;
    rng_fail = false;
    memset(g_auth, 0, sizeof g_auth);
    memset(&g_hs, 0, sizeof g_hs);
    for (int i = 0; i < 4; i++) {
        memset(TRANS[i], 0x40 + i, 32);
    }
}

/* One button session claiming the device with phone 0 (identity 1). */
static void claim_device(void)
{
    clear_script();
    advance(g_true + MINUTE);
    int c = step(g_true, hk(h_claim, 0, 1));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[c].r.status);
    clear_script();
}

/* Owner `owner` invites (one session), phone `phone` enrolls (next session). */
static void add_phone(int owner, int phone)
{
    clear_script();
    advance(g_true + MINUTE);
    step(g_true, hk(h_session, owner, 0));
    int inv = step(g_true + 1, hk(h_invite, owner, 0));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[inv].r.status);
    clear_script();
    advance(g_true + MINUTE);
    int a = step(g_true, hk(h_add, phone, 0));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[a].r.status);
    clear_script();
}

/* Arms recovery: a power-on (or button) boot with a verified 10 s hold. */
static void arm_recovery(dsp_wake_cause_t cause)
{
    CFG.recovery_hold_ms = DSP_OWNER_ARM_HOLD_MS;
    if (cause == DSP_CAUSE_POWER_ON) {
        reboot_power_on();
    }
    clear_script();
    DSP_CHECK(cause == DSP_CAUSE_BUTTON ? session() : wake(cause));
    CFG.recovery_hold_ms = 0;
    clear_script();
}

/* Snapshot of every key except "auth" (and the dosing record, compared field-wise). */
typedef struct {
    int n;
    char key[48][8];
    size_t len[48];
    uint8_t v[48][DSP_RECORD_MAX_BYTES];
} snap_t;
static snap_t snap_before, snap_after;
static void snapshot(snap_t *sn)
{
    memset(sn, 0, sizeof *sn);
    for (int i = 0; i < FAKE_KV_MAX_KEYS && sn->n < 48; i++) {
        const fake_kv_entry_t *e = &kv.durable[i];
        if (!e->used || strcmp(e->key, "auth") == 0 || strcmp(e->key, "dosing") == 0) {
            continue;
        }
        strcpy(sn->key[sn->n], e->key);
        sn->len[sn->n] = e->len;
        memcpy(sn->v[sn->n], e->value, e->len);
        sn->n++;
    }
}
static bool snap_equal(const snap_t *x, const snap_t *y)
{
    if (x->n != y->n) {
        return false;
    }
    for (int i = 0; i < x->n; i++) {
        if (strcmp(x->key[i], y->key[i]) != 0 || x->len[i] != y->len[i] ||
            memcmp(x->v[i], y->v[i], x->len[i]) != 0) {
            return false;
        }
    }
    return true;
}

static dsp_power_retained_t rtc_block(void)
{
    dsp_power_retained_t r;
    memset(&r, 0, sizeof r);
    (void)dsp_power_retained_decode(rtc, rtc_len, &r);
    return r;
}

/* ==== 1. Without a security layer nothing can be authorized ================================ */

static void test_no_security_layer(void)
{
    provision(true); /* CFG.security == NULL, as in every build before M1.10c */
    clear_script();
    advance(g_true + MINUTE);
    int first = sc.len;
    for (int p = DSP_SEC_SESSION; p <= DSP_SEC_RECOVERY; p++) {
        step(g_true, hk(h_admit, p, 0));
    }
    memset(&g_hs, 0, sizeof g_hs);
    step(g_true, hk(h_enroll, 0, 1));
    step(g_true, hk(h_session, 0, 0));
    g_forged = (dsp_app_auth_t){DSP_APP_ROLE_OWNER, 0, 1, 1};
    int forged = step(g_true, hk(h_list_forged, 0, 0));
    g_auth[0] = g_forged;
    step(g_true, hk(h_invite, 0, 0));
    step(g_true, hk(h_remove, 0, 0));
    int kf = step(g_true, hk(h_key_fault, 0, 0));
    int last = sc.len;
    DSP_CHECK(session());
    for (int i = first; i < last; i++) {
        DSP_CHECK(sc.res[i].r.status == DSP_OP_FORBIDDEN);
        DSP_CHECK_EQ_INT(0, sc.res[i].writes);
    }
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[forged].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[kf].r.status);
    bool ok;
    dsp_auth_t a = durable_auth(&ok);
    DSP_CHECK(ok && dsp_auth_is_provisioning(&a));
    DSP_CHECK_EQ_INT(DSP_CRED_UNCLAIMED, rep.cred_state);
    DSP_CHECK(!rep.cred_inhibit);
    /* The security layer outside a button session: nothing. */
    CFG.security = &SEC;
    clear_script();
    advance(g_true + HOUR);
    DSP_CHECK(wake(DSP_CAUSE_TIMER));
    dsp_app_hs_t hs;
    DSP_CHECK_EQ_INT(DSP_OP_BUSY, dsp_app_sec_admit(&app, DSP_SEC_CLAIM, &hs).status);
    invariants();
}

/* ==== 2. Claim and the issued authorization ================================================= */

static void test_claim_and_issued_authorization(void)
{
    setup(true);
    clear_script();
    advance(g_true + MINUTE);
    snapshot(&snap_before);
    dsp_dosing_state_t d0 = durable_dosing();
    int c = step(g_true, hk(h_claim, 0, 1));
    int l = step(g_true + 1, hk(h_list, 0, 0));
    int f1 = step(g_true + 2, hk(h_list_forged, 0, 0));
    int again = step(g_true + 3, hk(h_admit, DSP_SEC_CLAIM, 0));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[c].r.status);
    DSP_CHECK_EQ_INT(1, sc.res[c].writes); /* one atomic "auth" write */
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[l].r.status);
    DSP_CHECK_EQ_INT(1, sc.res[l].detail);
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[f1].r.status); /* g_forged: no token */
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[again].r.status);
    bool ok;
    dsp_auth_t a = durable_auth(&ok);
    DSP_CHECK(ok && dsp_auth_usable(&a));
    DSP_CHECK_EQ_INT(1, a.owner_epoch);
    uint8_t p0[32];
    k32(p0, PHONE(0));
    DSP_CHECK_EQ_INT(0, dsp_auth_find(&a, p0));
    DSP_CHECK(a.id_priv[0] == IDPRIV(1) && a.id_pub[31] == IDPUB(1));
    DSP_CHECK_EQ_INT(DSP_CRED_CLAIMED, rep.cred_state);
    snapshot(&snap_after);
    DSP_CHECK(snap_equal(&snap_before, &snap_after)); /* only "auth" changed */
    dsp_dosing_state_t d1 = durable_dosing();
    DSP_CHECK(dsp_dosing_state_equal(&d0, &d1));
    /* Forged variations of the issued authorization in a later session. */
    dsp_app_auth_t old = g_auth[0];
    clear_script();
    advance(g_true + MINUTE);
    g_forged = old; /* replay of the previous session's authorization */
    int r1 = step(g_true, hk(h_list_forged, 0, 0));
    step(g_true, hk(h_session, 0, 0));
    int ok2 = step(g_true + 1, hk(h_list, 0, 0));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[r1].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[ok2].r.status);
    DSP_CHECK(g_auth[0].token != old.token && g_auth[0].session_id != old.session_id);
    /* Unknown phone key: no authorization. */
    clear_script();
    advance(g_true + MINUTE);
    int unknown = step(g_true, hk(h_session, 6, 0));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[unknown].r.status);
    DSP_CHECK_EQ_INT(0, g_auth[6].role);
    invariants();
}

/* Forged authorization fields, checked directly inside one session. */
static void h_forge_checks(int i)
{
    dsp_app_phone_t ph[DSP_APP_PHONE_SLOTS];
    uint8_t n;
    dsp_app_auth_t f = g_auth[0];
    int fails = 0;
    f.token ^= 1u;
    fails += dsp_app_phones_list(&app, &f, ph, &n).status == DSP_OP_FORBIDDEN;
    f = g_auth[0];
    f.slot = 1;
    fails += dsp_app_phones_list(&app, &f, ph, &n).status == DSP_OP_FORBIDDEN;
    f = g_auth[0];
    f.session_id ^= 1u;
    fails += dsp_app_phones_list(&app, &f, ph, &n).status == DSP_OP_FORBIDDEN;
    f = g_auth[0];
    f.role = DSP_APP_ROLE_NONE;
    fails += dsp_app_phones_list(&app, &f, ph, &n).status == DSP_OP_FORBIDDEN;
    fails += dsp_app_phones_list(&app, NULL, ph, &n).status == DSP_OP_FORBIDDEN;
    fails += dsp_app_phones_list(&app, &g_auth[0], ph, &n).status == DSP_OP_OK; /* genuine */
    sc.res[i].detail = (uint32_t)fails;
}

static void test_forged_authorization_fields(void)
{
    setup(true);
    claim_device();
    advance(g_true + MINUTE);
    step(g_true, hk(h_session, 0, 0));
    int f = step(g_true + 1, hk(h_forge_checks, 0, 0));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(6, sc.res[f].detail);
}

/* ==== 3. Handshake evidence: missing, mismatched, expired, replayed ========================= */

static void test_handshake_evidence(void)
{
    setup(true);
    clear_script();
    advance(g_true + MINUTE);
    int r[12];
    int n = 0;
    r[n++] = step(g_true, hk(h_admit, DSP_SEC_CLAIM, 0));
    step(g_true, hk(h_tamper, 0, 0)); /* nonce */
    r[n++] = step(g_true, hk(h_enroll, 0, 1));
    step(g_true, hk(h_tamper, 0, 0)); /* restore the nonce: the handle is consumed anyway */
    r[n++] = step(g_true, hk(h_enroll, 0, 1));
    r[n++] = step(g_true, hk(h_admit, DSP_SEC_CLAIM, 0));
    step(g_true, hk(h_tamper, 1, 0)); /* purpose */
    r[n++] = step(g_true, hk(h_enroll, 0, 1));
    r[n++] = step(g_true, hk(h_admit, DSP_SEC_CLAIM, 0));
    step(g_true, hk(h_tamper, 2, 0)); /* session */
    r[n++] = step(g_true, hk(h_enroll, 0, 1));
    r[n++] = step(g_true, hk(h_admit, DSP_SEC_CLAIM, 0));
    r[n++] = step(g_true + 31, hk(h_enroll, 0, 1)); /* expired (> 30 s) */
    r[n++] = step(g_true + 40, hk(h_admit, DSP_SEC_CLAIM, 0));
    r[n++] = step(g_true + 69, hk(h_enroll, 0, 1)); /* 29 s: in time */
    r[n++] = step(g_true + 70, hk(h_enroll, 0, 1)); /* replay of the consumed handle */
    DSP_CHECK(session());
    const int want[] = {DSP_OP_OK,        DSP_OP_FORBIDDEN, DSP_OP_FORBIDDEN, DSP_OP_OK,
                        DSP_OP_FORBIDDEN, DSP_OP_OK,        DSP_OP_FORBIDDEN, DSP_OP_OK,
                        DSP_OP_FORBIDDEN, DSP_OP_OK,        DSP_OP_OK,        DSP_OP_FORBIDDEN};
    for (int k = 0; k < n; k++) {
        DSP_CHECK_EQ_INT(want[k], sc.res[r[k]].r.status);
    }
    for (int k = 1; k < 9; k++) {
        DSP_CHECK_EQ_INT(0, sc.res[r[k]].writes);
    }
    bool ok;
    dsp_auth_t a = durable_auth(&ok);
    DSP_CHECK(ok && a.claim_state == DSP_AUTH_CLAIMED && a.owner_epoch == 1);
    /* A replayed SESSION commit (state would still allow it): refused, single use. */
    clear_script();
    advance(g_true + MINUTE);
    int s1 = step(g_true, hk(h_session, 0, 0));
    int rp = step(g_true + 1, hk(h_lookup_again, 0, 0));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[s1].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[rp].r.status);
    /* A handle from an earlier session is useless in the next one. */
    dsp_app_hs_t keep = g_hs;
    clear_script();
    advance(g_true + MINUTE);
    step(g_true, hk(h_admit, DSP_SEC_SESSION, 0));
    DSP_CHECK(session());
    keep = g_hs;
    clear_script();
    advance(g_true + MINUTE);
    g_hs = keep;
    int stale = step(g_true, hk(h_identity, 1, 0)); /* admits a new one: works */
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[stale].r.status);
    DSP_CHECK_EQ_INT(1, sc.res[stale].state); /* the stored identity key pair */
    /* RNG failure: no handle can be issued. */
    clear_script();
    advance(g_true + MINUTE);
    int nr = step(g_true, hk(h_admit, DSP_SEC_SESSION, 0));
    rng_fail = true;
    DSP_CHECK(session());
    rng_fail = false;
    DSP_CHECK(sc.res[nr].r.status == DSP_OP_BUSY || sc.res[nr].r.status == DSP_OP_DEGRADED ||
              sc.res[nr].r.status == DSP_OP_FORBIDDEN);
    invariants();
}

/* ==== 4. Invite: add, single use, expiry, resets, clock steps, epoch ======================== */

static void test_invite_add_and_single_use(void)
{
    setup(true);
    claim_device();
    /* invite in one session; save the RTC image right after it */
    advance(g_true + MINUTE);
    step(g_true, hk(h_session, 0, 0));
    int inv = step(g_true + 1, hk(h_invite, 0, 0));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[inv].r.status);
    DSP_CHECK_EQ_INT(DSP_OWNER_INVITE_S, sc.res[inv].detail);
    DSP_CHECK_EQ_INT(0, sc.res[inv].writes); /* RTC only: no flash write */
    memcpy(g_rtc_copy, rtc, sizeof g_rtc_copy);
    g_rtc_copy_len = rtc_len;
    /* the token never reaches flash */
    for (int i = 0; i < FAKE_KV_MAX_KEYS; i++) {
        const fake_kv_entry_t *e = &kv.durable[i];
        for (size_t j = 0; e->used && j + 16 <= e->len; j++) {
            DSP_CHECK(memcmp(e->value + j, g_token, 16) != 0);
        }
    }
    clear_script();
    advance(g_true + MINUTE);
    int a = step(g_true, hk(h_add, 1, 0));
    int a2 = step(g_true + 1, hk(h_add, 2, 0)); /* consumed: a second phone cannot use it */
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[a].r.status);
    DSP_CHECK_EQ_INT(1, g_auth[1].slot);
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[a2].r.status);
    /* consumed: the RTC no longer holds the invite or its token */
    DSP_CHECK_EQ_INT(0, rtc_block().invite.valid);
    for (size_t j = 0; j + 16 <= sizeof rtc; j++) {
        DSP_CHECK(memcmp(rtc + j, g_token, 16) != 0);
    }
    bool ok;
    dsp_auth_t au = durable_auth(&ok);
    DSP_CHECK(ok && au.owner_epoch == 2 && au.slot[1].used);
    /* The consumed invite restored from the old RTC image (stale copy): refused (epoch). */
    memcpy(rtc, g_rtc_copy, sizeof rtc);
    rtc_len = g_rtc_copy_len;
    clear_script();
    advance(g_true + MINUTE);
    int v = step(g_true, hk(h_view, 0, 0));
    int a3 = step(g_true, hk(h_add, 3, 0));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(0, sc.res[v].detail & 2u);
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[a3].r.status);
    au = durable_auth(&ok);
    DSP_CHECK(ok && !au.slot[2].used && !au.slot[3].used);
    invariants();
}

/* Expiry boundary (raw clock) across deep sleep, and every reset type. */
static void test_invite_expiry_and_resets(void)
{
    const dsp_wake_cause_t middle[] = {DSP_CAUSE_TIMER,    DSP_CAUSE_SOFTWARE, DSP_CAUSE_WATCHDOG,
                                       DSP_CAUSE_PANIC,    DSP_CAUSE_BROWNOUT, DSP_CAUSE_POWER_ON};
    for (int boundary = 0; boundary < 2; boundary++) {
        setup(true);
        claim_device();
        advance(g_true + MINUTE);
        step(g_true, hk(h_session, 0, 0));
        int inv = step(g_true + 1, hk(h_invite, 0, 0));
        DSP_CHECK(session());
        DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[inv].r.status);
        dsp_power_retained_t r = rtc_block();
        dsp_utc_t deadline = r.invite.deadline_raw;
        DSP_CHECK_EQ_INT(r.invite.created_raw + DSP_OWNER_INVITE_S, deadline);
        /* timer wakes in between never extend it */
        advance(g_true + 120);
        DSP_CHECK(wake(DSP_CAUSE_TIMER));
        DSP_CHECK_EQ_INT(deadline, rtc_block().invite.deadline_raw);
        /* ADD admitted at deadline - 1 (raw), refused at the deadline */
        clear_script();
        dsp_utc_t target = deadline - (boundary == 0 ? 1 : 0) - g_offset;
        advance(target - 1); /* the session starts one second before */
        int add = step(target, hk(h_admit, DSP_SEC_ADD, 0));
        DSP_CHECK(session());
        DSP_CHECK_EQ_INT(boundary == 0 ? DSP_OP_OK : DSP_OP_FORBIDDEN, sc.res[add].r.status);
    }
    for (size_t k = 0; k < sizeof middle / sizeof middle[0]; k++) {
        setup(true);
        claim_device();
        advance(g_true + MINUTE);
        step(g_true, hk(h_session, 0, 0));
        step(g_true + 1, hk(h_invite, 0, 0));
        DSP_CHECK(session());
        advance(g_true + 30);
        if (middle[k] == DSP_CAUSE_POWER_ON || middle[k] == DSP_CAUSE_BROWNOUT) {
            reboot_power_on(); /* RTC memory lost */
        }
        clear_script();
        DSP_CHECK(wake(middle[k]));
        /* soft resets keep the block but clear the overlays; power-on/brownout lose RTC memory */
        bool soft = middle[k] == DSP_CAUSE_SOFTWARE || middle[k] == DSP_CAUSE_WATCHDOG ||
                    middle[k] == DSP_CAUSE_PANIC;
        DSP_CHECK(rep.overlays_cleared == soft);
        DSP_CHECK_EQ_INT(0, middle[k] == DSP_CAUSE_TIMER ? 0 : rtc_block().invite.valid);
        clear_script();
        advance(g_true + 30);
        int add = step(g_true, hk(h_admit, DSP_SEC_ADD, 0));
        DSP_CHECK(session());
        DSP_CHECK_EQ_INT(middle[k] == DSP_CAUSE_TIMER ? DSP_OP_OK : DSP_OP_FORBIDDEN,
                         sc.res[add].r.status);
    }
    /* Repeated software resets: never back. */
    setup(true);
    claim_device();
    advance(g_true + MINUTE);
    step(g_true, hk(h_session, 0, 0));
    step(g_true + 1, hk(h_invite, 0, 0));
    DSP_CHECK(session());
    for (int i = 0; i < 3; i++) {
        advance(g_true + 5);
        DSP_CHECK(wake(DSP_CAUSE_SOFTWARE));
    }
    DSP_CHECK_EQ_INT(0, rtc_block().invite.valid);
    invariants();
}

/* A clock step (raw clock moved against the monotonic clock) clears invite and arm at once. */
static void test_clock_step_clears_overlays(void)
{
    for (int dir = 0; dir < 2; dir++) {
        setup(true);
        claim_device();
        arm_recovery(DSP_CAUSE_BUTTON);
        DSP_CHECK(rep.recovery_armed);
        clear_script();
        advance(g_true + MINUTE);
        step(g_true, hk(h_session, 0, 0));
        step(g_true + 1, hk(h_invite, 0, 0));
        int v1 = step(g_true + 2, hk(h_view, 0, 0));
        step(g_true + 3, hk(h_clock_step, dir == 0 ? 5 : -5, 0));
        int v2 = step(g_true + 4, hk(h_view, 0, 0));
        DSP_CHECK(session());
        DSP_CHECK_EQ_INT(3, sc.res[v1].detail & 3u); /* armed + invite */
        DSP_CHECK_EQ_INT(0, sc.res[v2].detail & 3u);
        DSP_CHECK(rep.overlays_cleared);
        dsp_power_retained_t r = rtc_block();
        DSP_CHECK(!r.invite.valid && !r.arm.armed);
    }
    /* A step within the 2 s tolerance (rounding) does not. */
    setup(true);
    claim_device();
    advance(g_true + MINUTE);
    step(g_true, hk(h_session, 0, 0));
    step(g_true + 1, hk(h_invite, 0, 0));
    step(g_true + 2, hk(h_clock_step, 1, 0));
    int v = step(g_true + 3, hk(h_view, 0, 0));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(2, sc.res[v].detail & 2u);
}

/* ==== 5. Revocation and slot reuse (manual floor) ========================================== */

static void test_revoke_and_slot_reuse(void)
{
    setup(true);
    claim_device();
    add_phone(0, 1);
    /* phone 1 decides three manual requests (slot 1) */
    clear_script();
    advance(g_true + MINUTE);
    step(g_true, hk(h_session, 1, 0));
    int m[3];
    for (int k = 0; k < 3; k++) {
        m[k] = step(g_true + 1 + k * 70, as(op_manual((uint64_t)k + 1, 9999999u), &g_auth[1]));
    }
    DSP_CHECK(session());
    for (int k = 0; k < 3; k++) {
        DSP_CHECK(sc.res[m[k]].r.status == DSP_OP_LIMIT || sc.res[m[k]].r.status == DSP_OP_OK ||
                  sc.res[m[k]].r.status == DSP_OP_REFUSED);
    }
    DSP_CHECK_EQ_INT(3, durable_dosing().manual_hw[1]);
    bool ok;
    dsp_auth_t before = durable_auth(&ok);
    /* phone 0 revokes phone 1: only slot 1 changes, epoch + 1, manual_hw untouched */
    clear_script();
    advance(g_true + MINUTE);
    step(g_true, hk(h_session, 0, 0));
    int rm = step(g_true + 1, hk(h_remove, 0, 1));
    int l = step(g_true + 2, hk(h_list, 0, 0)); /* the caller stays authorized */
    int last = step(g_true + 3, hk(h_remove, 0, 0)); /* the last phone: refused */
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[rm].r.status);
    DSP_CHECK_EQ_INT(0, sc.res[rm].state);
    DSP_CHECK_EQ_INT(1, sc.res[rm].writes);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[l].r.status);
    DSP_CHECK_EQ_INT(1, sc.res[l].detail);
    DSP_CHECK_EQ_INT(DSP_OP_REFUSED, sc.res[last].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_R_LAST_SLOT, sc.res[last].r.detail);
    dsp_auth_t after = durable_auth(&ok);
    DSP_CHECK(ok && !after.slot[1].used);
    DSP_CHECK(memcmp(&after.slot[0], &before.slot[0], sizeof after.slot[0]) == 0);
    DSP_CHECK(memcmp(after.id_priv, before.id_priv, 32) == 0);
    DSP_CHECK_EQ_INT(before.owner_epoch + 1, after.owner_epoch);
    DSP_CHECK_EQ_INT(3, durable_dosing().manual_hw[1]);
    /* the revoked phone cannot authenticate any more */
    clear_script();
    advance(g_true + MINUTE);
    int gone = step(g_true, hk(h_session, 1, 0));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[gone].r.status);
    /* a new phone reuses slot 1 and continues above its high-water mark */
    add_phone(0, 2);
    DSP_CHECK_EQ_INT(1, g_auth[2].slot);
    clear_script();
    advance(g_true + MINUTE);
    step(g_true, hk(h_session, 2, 0));
    int fl = step(g_true + 1, as(op_floor(), &g_auth[2]));
    int dup = step(g_true + 2, as(op_manual(2, 1000), &g_auth[2]));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(4, sc.res[fl].floor);
    DSP_CHECK_EQ_INT(DSP_OP_DUPLICATE, sc.res[dup].r.status); /* decided by the old phone */
    DSP_CHECK_EQ_INT(0, sc.res[dup].pumps);
    /* removing itself ends the session */
    clear_script();
    advance(g_true + MINUTE);
    step(g_true, hk(h_session, 2, 0));
    int self = step(g_true + 1, hk(h_remove, 2, 1));
    int after_self = step(g_true + 2, hk(h_list, 2, 0));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[self].r.status);
    DSP_CHECK_EQ_INT(1, sc.res[self].state);
    DSP_CHECK_EQ_INT(DSP_BLE_END_REVOKED, rep.ble_end);
    DSP_CHECK(!sc.res[after_self].ran || sc.res[after_self].r.status != DSP_OP_OK);
    invariants();
}

/* ==== 6. Recovery: full procedure and preservation ========================================= */

static void test_recovery_preserves_everything(void)
{
    setup(true);
    claim_device();
    add_phone(0, 1);
    /* manual requests (slot 0) and a safety hold before the recovery */
    clear_script();
    advance(g_true + MINUTE);
    step(g_true, hk(h_session, 0, 0));
    step(g_true + 1, as(op_manual(7, 9999999u), &g_auth[0]));
    DSP_CHECK(session());
    dsp_dosing_state_t d = durable_dosing();
    DSP_CHECK_EQ_INT(7, d.manual_hw[0]);
    d.flags |= DSP_DOSING_F_BLOCKED; /* reservoir BLOCKED latch: a safety hold */
    put_dosing(&d);
    arm_recovery(DSP_CAUSE_POWER_ON);
    DSP_CHECK(rep.recovery_armed);
    DSP_CHECK_EQ_INT(0, rep.owner_changes);
    snapshot(&snap_before);
    dsp_dosing_state_t d0 = durable_dosing();
    bool ok;
    dsp_auth_t a0 = durable_auth(&ok);
    clear_script();
    advance(g_true + MINUTE);
    int v = step(g_true, hk(h_view, 0, 0));
    int b = step(g_true + 1, hk(h_recover_begin, 1, 0));
    int busy = step(g_true + 2, as(op_floor(), &OWNER)); /* only END_SESSION while confirming */
    step(g_true + 3, hk(h_press, 3, 0));
    int st = step(g_true + 6, hk(h_state, 0, 0));
    int ap = step(g_true + 7, hk(h_apply, 1, 5));
    int l = step(g_true + 8, hk(h_list, 5, 0));
    int old = step(g_true + 9, hk(h_session, 0, 0));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(1, sc.res[v].detail & 1u);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[b].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_BUSY, sc.res[busy].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_B_CONFIRMING, sc.res[busy].r.detail);
    DSP_CHECK_EQ_INT(DSP_CONFIRM_CONFIRMED, sc.res[st].state);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[ap].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[l].r.status);
    DSP_CHECK_EQ_INT(1, sc.res[l].detail);
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[old].r.status); /* old phones are gone */
    dsp_auth_t a1 = durable_auth(&ok);
    uint8_t p5[32];
    k32(p5, PHONE(5));
    DSP_CHECK(ok && dsp_auth_usable(&a1));
    DSP_CHECK_EQ_INT(0, dsp_auth_find(&a1, p5));
    DSP_CHECK_EQ_INT(1, dsp_auth_used_count(&a1));
    DSP_CHECK(a1.id_priv[0] == IDPRIV(7) && a1.owner_epoch > a0.owner_epoch);
    DSP_CHECK_EQ_INT(a0.recoveries + 1, a1.recoveries);
    /* Preservation: every other record byte-identical; dosing identical except readiness. */
    snapshot(&snap_after);
    DSP_CHECK(snap_equal(&snap_before, &snap_after));
    dsp_dosing_state_t d1 = durable_dosing();
    DSP_CHECK_EQ_INT(DSP_READINESS_SERVICE_REQUIRED, d1.readiness);
    d1.readiness = d0.readiness;
    DSP_CHECK(dsp_dosing_state_equal(&d0, &d1)); /* manual_hw, BLOCKED, ledger, ring, admission */
    DSP_CHECK_EQ_INT(7, d1.manual_hw[0]);
    DSP_CHECK(d1.flags & DSP_DOSING_F_BLOCKED);
    /* the arm is consumed */
    DSP_CHECK_EQ_INT(0, rtc_block().arm.armed);
    /* the new phone continues above slot 0's high-water mark */
    clear_script();
    advance(g_true + MINUTE);
    step(g_true, hk(h_session, 5, 0));
    int fl = step(g_true + 1, as(op_floor(), &g_auth[5]));
    int dup = step(g_true + 2, as(op_manual(7, 1000), &g_auth[5]));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(8, sc.res[fl].floor);
    DSP_CHECK_EQ_INT(DSP_OP_DUPLICATE, sc.res[dup].r.status);
    invariants();
}

/* Recovery is also the way out of damaged credentials; arming rules. */
static void test_recovery_arming_rules(void)
{
    /* 9,999 ms: not armed; a TIMER wake: not armed; a valid unclaimed device: not armed. */
    setup(true);
    claim_device();
    CFG.recovery_hold_ms = DSP_OWNER_ARM_HOLD_MS - 1;
    reboot_power_on();
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    DSP_CHECK(!rep.recovery_armed);
    CFG.recovery_hold_ms = DSP_OWNER_ARM_HOLD_MS;
    advance(g_true + 60);
    DSP_CHECK(wake(DSP_CAUSE_TIMER));
    DSP_CHECK(!rep.recovery_armed);
    CFG.recovery_hold_ms = 0;
    setup(true); /* unclaimed */
    CFG.recovery_hold_ms = DSP_OWNER_ARM_HOLD_MS;
    reboot_power_on();
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    DSP_CHECK(!rep.recovery_armed);
    CFG.recovery_hold_ms = 0;
    /* Window: 30 min of raw time; an owner can cancel; recovery refused without an arm. */
    setup(true);
    claim_device();
    arm_recovery(DSP_CAUSE_POWER_ON);
    dsp_utc_t deadline = rtc_block().arm.deadline_raw;
    clear_script();
    advance(deadline - g_offset - 2);
    int in = step(g_true + 1, hk(h_admit, DSP_SEC_RECOVERY, 0));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[in].r.status);
    clear_script();
    advance(deadline - g_offset);
    int out = step(g_true, hk(h_admit, DSP_SEC_RECOVERY, 0));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[out].r.status);
    arm_recovery(DSP_CAUSE_BUTTON); /* a button wake held 10 s also arms (O4) */
    DSP_CHECK(rep.recovery_armed);
    clear_script();
    advance(g_true + MINUTE);
    step(g_true, hk(h_session, 0, 0));
    int cancel = step(g_true + 1, hk(h_recovery_cancel, 0, 0));
    int after = step(g_true + 2, hk(h_admit, DSP_SEC_RECOVERY, 0));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[cancel].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[after].r.status);
    /* An ownership change during the window invalidates the arm (epoch binding). */
    arm_recovery(DSP_CAUSE_BUTTON);
    add_phone(0, 1);
    clear_script();
    advance(g_true + MINUTE);
    int bound = step(g_true, hk(h_admit, DSP_SEC_RECOVERY, 0));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[bound].r.status);
    invariants();
}

static void test_recovery_confirmation_failures(void)
{
    setup(true);
    claim_device();
    arm_recovery(DSP_CAUSE_POWER_ON);
    /* 1: two presses -> aborted at the deadline (aborts 1) */
    clear_script();
    advance(g_true + MINUTE);
    step(g_true, hk(h_recover_begin, 0, 0));
    step(g_true + 1, hk(h_press, 2, 0));
    int s1 = step(g_true + 25, hk(h_state, 0, 0));
    int ap1 = step(g_true + 26, hk(h_apply, 0, 5));
    DSP_CHECK(session());
    DSP_CHECK(sc.res[s1].state == DSP_CONFIRM_NONE || sc.res[s1].state == DSP_CONFIRM_ABORTED);
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[ap1].r.status);
    DSP_CHECK_EQ_INT(1, rtc_block().arm.aborts);
    /* 2: presses queued BEFORE the operation and replayed old timestamps never count */
    clear_script();
    advance(g_true + MINUTE);
    step(g_true, hk(h_press, 3, 0)); /* queued before begin: flushed */
    step(g_true, hk(h_recover_begin, 1, 0));
    step(g_true + 1, hk(h_press, 3, 1)); /* timestamps before the operation */
    int s2 = step(g_true + 25, hk(h_state, 0, 0));
    DSP_CHECK(session());
    DSP_CHECK(sc.res[s2].state != DSP_CONFIRM_CONFIRMED);
    DSP_CHECK_EQ_INT(2, rtc_block().arm.aborts);
    /* 3: confirmed, but applied with another transcript -> refused and consumed; the session
     *    end then disarms (third failure) */
    clear_script();
    advance(g_true + MINUTE);
    step(g_true, hk(h_recover_begin, 2, 0));
    step(g_true + 1, hk(h_press, 3, 0));
    int s3 = step(g_true + 4, hk(h_state, 0, 0));
    int wrong = step(g_true + 5, hk(h_apply, 3, 5));
    int right = step(g_true + 6, hk(h_apply, 2, 5));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_CONFIRM_CONFIRMED, sc.res[s3].state);
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[wrong].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[right].r.status); /* single use */
    bool ok;
    dsp_auth_t a = durable_auth(&ok);
    DSP_CHECK(ok && a.id_priv[0] == IDPRIV(1)); /* unchanged */
    /* 4: a session ending while confirming counts as a failure: the third disarms */
    clear_script();
    advance(g_true + MINUTE);
    step(g_true, hk(h_recover_begin, 0, 0));
    DSP_CHECK(session()); /* END_SESSION right after */
    DSP_CHECK_EQ_INT(0, rtc_block().arm.armed);
    clear_script();
    advance(g_true + MINUTE);
    int no = step(g_true, hk(h_admit, DSP_SEC_RECOVERY, 0));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[no].r.status);
    /* 5: a scheduled dose that becomes due aborts a pending confirmation */
    setup(true);
    claim_device();
    clear_script();
    advance(g_true + MINUTE);
    step(g_true, op_create(sched("due", 8, 0, 1000)));
    DSP_CHECK(session());
    advance(T0 - 10 * MINUTE); /* the arm window (30 min) covers the due time */
    arm_recovery(DSP_CAUSE_BUTTON);
    DSP_CHECK(rep.recovery_armed);
    clear_script();
    advance(T0 - 5);
    int b5 = step(g_true, hk(h_recover_begin, 0, 0));
    int s5 = step(T0 + 10, hk(h_state, 0, 0));
    int before5 = pump.calls;
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[b5].r.status);
    DSP_CHECK(sc.res[s5].state != DSP_CONFIRM_CONFIRMED);
    DSP_CHECK_EQ_INT(before5 + 1, pump.calls); /* the scheduled dose ran (BLE suspended) */
    DSP_CHECK_EQ_INT(1, rtc_block().arm.aborts);
    invariants();
}

/* ==== 7. Power loss at every write of recovery and of each ownership operation ============== */

static void test_recovery_interrupted(void)
{
    int cases = 0, applied = 0, kept = 0;
    for (int landed = 0; landed < 2; landed++) {
        for (long k = 1; k <= 4; k++) {
            setup(true);
            claim_device();
            add_phone(0, 1);
            dsp_dosing_state_t d0 = durable_dosing();
            arm_recovery(DSP_CAUSE_POWER_ON);
            clear_script();
            advance(g_true + MINUTE);
            step(g_true, hk(h_recover_begin, 1, 0));
            step(g_true + 1, hk(h_press, 3, 0));
            step(g_true + 4, hk(h_state, 0, 0));
            step_armed(g_true + 5, hk(h_apply, 1, 5));
            cut_at = k;
            cut_after = landed == 1;
            bool finished = session();
            cut_at = 0;
            if (finished) {
                break; /* fewer writes than k */
            }
            cases++;
            reboot_power_on();
            clear_script();
            DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
            /* never unclaimed, never a damaged record, the arm is gone with the power */
            DSP_CHECK_EQ_INT(DSP_CRED_CLAIMED, rep.cred_state);
            bool ok;
            dsp_auth_t a = durable_auth(&ok);
            DSP_CHECK(ok && dsp_auth_usable(&a));
            DSP_CHECK_EQ_INT(0, rtc_block().arm.armed);
            dsp_dosing_state_t d1 = durable_dosing();
            bool recovered = a.id_priv[0] == IDPRIV(7);
            applied += recovered;
            kept += !recovered;
            if (recovered) {
                DSP_CHECK_EQ_INT(DSP_READINESS_SERVICE_REQUIRED, d1.readiness); /* never without */
                DSP_CHECK_EQ_INT(1, dsp_auth_used_count(&a));
            } else {
                DSP_CHECK_EQ_INT(2, dsp_auth_used_count(&a)); /* the old owners, untouched */
            }
            for (int s = 0; s < DSP_MANUAL_SLOTS; s++) {
                DSP_CHECK(d1.manual_hw[s] == d0.manual_hw[s]); /* never lowered */
            }
            DSP_CHECK_EQ_INT(d0.flags, d1.flags);
        }
    }
    DSP_CHECK(cases >= 3);
    DSP_CHECK(applied >= 1 && kept >= 1);
}

static void test_ownership_writes_power_loss(void)
{
    for (int opk = 0; opk < 3; opk++) { /* claim, add, remove */
        for (int landed = 0; landed < 2; landed++) {
            setup(true);
            if (opk >= 1) {
                claim_device();
            }
            if (opk == 2) {
                add_phone(0, 1);
            }
            if (opk == 1) { /* the invite first */
                clear_script();
                advance(g_true + MINUTE);
                step(g_true, hk(h_session, 0, 0));
                step(g_true + 1, hk(h_invite, 0, 0));
                DSP_CHECK(session());
            }
            bool ok;
            dsp_auth_t before = durable_auth(&ok);
            clear_script();
            advance(g_true + MINUTE);
            if (opk == 2) {
                step(g_true, hk(h_session, 0, 0));
            }
            step_armed(g_true + 1, opk == 0 ? hk(h_claim, 0, 1)
                                            : (opk == 1 ? hk(h_add, 1, 0) : hk(h_remove, 0, 1)));
            cut_at = 1;
            cut_after = landed == 1;
            bool finished = session();
            cut_at = 0;
            DSP_CHECK(!finished);
            reboot_power_on();
            clear_script();
            DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
            dsp_auth_t after = durable_auth(&ok);
            DSP_CHECK(ok); /* old or new, never damaged */
            if (landed) {
                DSP_CHECK_EQ_INT(before.owner_epoch + 1, after.owner_epoch);
            } else {
                DSP_CHECK(dsp_auth_equal(&before, &after));
            }
            DSP_CHECK(opk == 0 && !landed ? rep.cred_state == DSP_CRED_UNCLAIMED
                                          : rep.cred_state == DSP_CRED_CLAIMED);
        }
    }
}

/* ==== 8. Damaged credentials and KEY_FAULT block dosing ===================================== */

static void test_corrupt_credentials_block_dosing(void)
{
    for (int kind = 0; kind < 3; kind++) { /* missing, malformed, newer format */
        setup(true);
        claim_device();
        clear_script();
        advance(g_true + MINUTE);
        step(g_true, op_create(sched("due", 8, 0, 1000)));
        DSP_CHECK(session());
        const fake_kv_entry_t *e = fake_kv_find(&kv, "auth");
        uint8_t v[DSP_AUTH_RECORD_BYTES];
        memcpy(v, e->value, sizeof v);
        if (kind == 0) {
            fake_kv_remove(&kv, "auth");
        } else if (kind == 1) {
            v[96] = 0; /* slot 0 unused but its key bytes remain */
            fake_kv_put_raw(&kv, "auth", v, sizeof v);
        } else {
            v[1] = 9;
            fake_kv_put_raw(&kv, "auth", v, sizeof v);
        }
        int before = pump.calls;
        advance(T0 + 5); /* the schedule is due */
        clear_script();
        DSP_CHECK(wake(DSP_CAUSE_TIMER));
        DSP_CHECK(!rep.degraded); /* the other records stay usable ... */
        DSP_CHECK_EQ_INT(DSP_CRED_DEGRADED, rep.cred_state);
        DSP_CHECK(rep.cred_inhibit);
        DSP_CHECK_EQ_INT(before, pump.calls); /* ... but nothing is dosed */
        /* the storage DEVICE_ERROR condition is raised */
        dsp_notify_state_t ns;
        const fake_kv_entry_t *ne = fake_kv_find(&kv, "notify");
        DSP_CHECK(ne != NULL && dsp_codec_decode_notify(ne->value, ne->len, &ns) == DSP_CODEC_OK);
        DSP_CHECK(ns.cond[DSP_COND_STORAGE].flags & DSP_COND_F_ACTIVE);
        /* never "unclaimed"; only RECOVERY can be admitted; manual doses refused */
        clear_script();
        advance(g_true + MINUTE);
        int v1 = step(g_true, hk(h_view, 0, 0));
        int c1 = step(g_true, hk(h_admit, DSP_SEC_CLAIM, 0));
        int s1 = step(g_true, hk(h_admit, DSP_SEC_SESSION, 0));
        int r1 = step(g_true, hk(h_admit, DSP_SEC_RECOVERY, 0)); /* not armed yet */
        int m1 = step(g_true + 1, op_manual(50, 1000));
        DSP_CHECK(session());
        DSP_CHECK_EQ_INT(DSP_CRED_DEGRADED, sc.res[v1].state);
        DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[c1].r.status);
        DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[s1].r.status);
        DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[r1].r.status);
        DSP_CHECK_EQ_INT(DSP_OP_REFUSED, sc.res[m1].r.status);
        DSP_CHECK(sc.res[m1].r.detail & DSP_DOSING_G_CREDENTIALS);
        DSP_CHECK_EQ_INT(0, sc.res[m1].pumps);
        DSP_CHECK_EQ_INT(before, pump.calls);
        /* the damaged record is left as it is (never repaired or re-provisioned) */
        if (kind != 0) {
            DSP_CHECK(memcmp(fake_kv_find(&kv, "auth")->value, v, sizeof v) == 0);
        } else {
            DSP_CHECK(fake_kv_find(&kv, "auth") == NULL);
        }
        /* recovery (arm taken while damaged) heals it */
        arm_recovery(DSP_CAUSE_POWER_ON);
        DSP_CHECK(rep.recovery_armed);
        DSP_CHECK_EQ_INT(DSP_OWNER_EPOCH_DEGRADED, rtc_block().arm.owner_epoch);
        clear_script();
        advance(g_true + MINUTE);
        step(g_true, hk(h_recover_begin, 0, 0));
        step(g_true + 1, hk(h_press, 3, 0));
        step(g_true + 4, hk(h_state, 0, 0));
        int ap = step(g_true + 5, hk(h_apply, 0, 5));
        DSP_CHECK(session());
        DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[ap].r.status);
        DSP_CHECK_EQ_INT(DSP_CRED_CLAIMED, rep.cred_state);
        DSP_CHECK(!rep.cred_inhibit);
        bool ok;
        dsp_auth_t a = durable_auth(&ok);
        DSP_CHECK(ok && dsp_auth_usable(&a));
        DSP_CHECK_EQ_INT(DSP_READINESS_SERVICE_REQUIRED, durable_readiness());
        ne = fake_kv_find(&kv, "notify");
        DSP_CHECK(dsp_codec_decode_notify(ne->value, ne->len, &ns) == DSP_CODEC_OK);
        DSP_CHECK(!(ns.cond[DSP_COND_STORAGE].flags & DSP_COND_F_ACTIVE));
    }
    invariants();
}

static void test_key_fault_inhibits_immediately(void)
{
    for (int durable = 0; durable < 2; durable++) {
        setup(true);
        claim_device();
        clear_script();
        advance(g_true + MINUTE);
        step(g_true, op_create(sched("kf", 8, 0, 1000)));
        DSP_CHECK(session());
        /* the fault is reported in a session that runs across the due time */
        int before = pump.calls;
        clear_script();
        advance(T0 - 20);
        if (!durable) {
            snprintf(fail_set_key, sizeof fail_set_key, "auth"); /* persisting fails */
        }
        int kf = step(g_true, hk(h_key_fault, 0, 0));
        step(T0 + 30, op_floor());
        DSP_CHECK(session());
        fail_set_key[0] = '\0';
        DSP_CHECK(rep.key_fault);
        DSP_CHECK(rep.cred_inhibit);
        DSP_CHECK_EQ_INT(before, pump.calls); /* inhibited at once, whatever the write did */
        DSP_CHECK_EQ_INT(durable ? DSP_OP_OK : DSP_OP_STORAGE, sc.res[kf].r.status);
        bool ok;
        dsp_auth_t a = durable_auth(&ok);
        DSP_CHECK(ok);
        DSP_CHECK_EQ_INT(durable ? DSP_AUTH_F_KEY_FAULT : 0, a.flags);
        /* next wake: persisted -> still blocked; not persisted -> the security layer has to
         * detect it again (the flag is diagnostic, not the only enforcement) */
        advance(g_true + 5);
        clear_script();
        DSP_CHECK(wake(DSP_CAUSE_TIMER));
        DSP_CHECK_EQ_INT(durable ? DSP_CRED_DEGRADED : DSP_CRED_CLAIMED, rep.cred_state);
    }
    invariants();
}

/* ==== 9. Admission (durable) and session-local limits ======================================= */

static void test_manual_admission_durable(void)
{
    setup(true);
    CFG.manual_decisions_per_day = 3;
    clear_script();
    advance(g_true + MINUTE);
    int r[6];
    for (int k = 0; k < 3; k++) {
        r[k] = step(g_true + k * 70, op_manual((uint64_t)k + 1, 9999999u)); /* durable LIMIT */
    }
    r[3] = step(g_true + 300, op_manual(10, 1000));
    r[4] = step(g_true + 301, op_manual(1, 1000)); /* a decided request: always answered */
    r[5] = step(g_true + 302, op_result(2));
    DSP_CHECK(session());
    for (int k = 0; k < 3; k++) {
        DSP_CHECK_EQ_INT(DSP_OP_LIMIT, sc.res[r[k]].r.status);
    }
    DSP_CHECK_EQ_INT(DSP_OP_BUSY, sc.res[r[3]].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_B_RATE_LIMITED, sc.res[r[3]].r.detail);
    DSP_CHECK(sc.res[r[3]].r.aux > 0); /* seconds to the next local day */
    DSP_CHECK_EQ_INT(0, sc.res[r[3]].writes); /* no write for an admission check */
    DSP_CHECK_EQ_INT(DSP_MRES_NOT_DECIDED, sc.res[r[3]].manual.state);
    DSP_CHECK_EQ_INT(DSP_OP_DUPLICATE, sc.res[r[4]].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[r[5]].r.status);
    DSP_CHECK_EQ_INT(DSP_MRES_REFUSED, sc.res[r[5]].state);
    dsp_dosing_state_t d = durable_dosing();
    DSP_CHECK_EQ_INT(3, d.adm_count);
    DSP_CHECK_EQ_INT(3, d.manual_hw[0]); /* seq 10 stays undecided */
    /* power loss + untrusted clock: the count survives and never restarts */
    reboot_power_on();
    clear_script();
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    clear_script();
    advance(g_true + MINUTE);
    int u = step(g_true, op_manual(10, 1000));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_BUSY, sc.res[u].r.status);
    DSP_CHECK_EQ_INT(0, sc.res[u].writes);
    /* trusted again, same day: still limited; next local day: admitted (the same seq) */
    clear_script();
    advance(g_true + MINUTE);
    step(g_true, op_now());
    int same = step(g_true + 1, op_manual(10, 9999999u));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_BUSY, sc.res[same].r.status);
    clear_script();
    advance(T0 + DAY);
    int next = step(g_true, op_manual(10, 9999999u));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_LIMIT, sc.res[next].r.status);
    d = durable_dosing();
    DSP_CHECK_EQ_INT(1, d.adm_count);
    DSP_CHECK_EQ_INT(10, d.manual_hw[0]);
    /* rollback: a trusted date at or before adm_date never restarts the count */
    d.adm_count = 3;
    put_dosing(&d);
    clear_script();
    advance(g_true + MINUTE);
    int rb = step(g_true, op_manual(11, 1000));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_BUSY, sc.res[rb].r.status);
    /* boundaries of the configured limit: 1 and 1000 (clamped above) */
    setup(true);
    CFG.manual_decisions_per_day = 1;
    clear_script();
    advance(g_true + MINUTE);
    int b1 = step(g_true, op_manual(1, 9999999u));
    int b2 = step(g_true + 70, op_manual(2, 9999999u));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_LIMIT, sc.res[b1].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_BUSY, sc.res[b2].r.status);
    DSP_CHECK_EQ_INT(1, durable_dosing().adm_count);
    /* per-session limit (RAM, documented: a reconnect resets it) */
    setup(true);
    CFG.session_manual_max = 2;
    clear_script();
    advance(g_true + MINUTE);
    int q1 = step(g_true, op_manual(1, 9999999u));
    int q2 = step(g_true + 70, op_manual(2, 9999999u));
    int q3 = step(g_true + 140, op_manual(3, 9999999u));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_LIMIT, sc.res[q1].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_LIMIT, sc.res[q2].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_BUSY, sc.res[q3].r.status);
    clear_script();
    advance(g_true + MINUTE);
    int q4 = step(g_true, op_manual(3, 9999999u));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_LIMIT, sc.res[q4].r.status); /* new session: allowed again */
    invariants();
}

static void test_session_write_limits(void)
{
    setup(true);
    CFG.session_writes_max = 4;
    CFG.writes_per_minute = 2;
    clear_script();
    advance(g_true + MINUTE);
    int c[6];
    c[0] = step(g_true, op_create(sched("w0", 20, 0, 1000)));
    c[1] = step(g_true + 1, op_create(sched("w1", 20, 1, 1000)));
    c[2] = step(g_true + 2, op_create(sched("w2", 20, 2, 1000))); /* third in a minute */
    c[3] = step(g_true + 62, op_create(sched("w2", 20, 2, 1000)));
    c[4] = step(g_true + 63, op_create(sched("w3", 20, 3, 1000)));
    c[5] = step(g_true + 130, op_create(sched("w4", 20, 4, 1000))); /* fifth in the session */
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[c[0]].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[c[1]].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_BUSY, sc.res[c[2]].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_B_RATE_LIMITED, sc.res[c[2]].r.detail);
    DSP_CHECK(sc.res[c[2]].r.aux >= 57 && sc.res[c[2]].r.aux <= 60);
    DSP_CHECK_EQ_INT(0, sc.res[c[2]].writes);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[c[3]].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[c[4]].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_BUSY, sc.res[c[5]].r.status);
    DSP_CHECK(rep.rate_limited >= 2);
    clear_script();
    advance(g_true + MINUTE);
    int again = step(g_true, op_create(sched("w4", 20, 4, 1000)));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[again].r.status); /* documented v1 limitation */
    /* operations per session */
    setup(true);
    CFG.session_ops_max = 3;
    clear_script();
    advance(g_true + MINUTE);
    int o[4];
    for (int k = 0; k < 4; k++) {
        o[k] = step(g_true + k, op_floor());
    }
    DSP_CHECK(session());
    for (int k = 0; k < 3; k++) {
        DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[o[k]].r.status);
    }
    DSP_CHECK_EQ_INT(DSP_OP_BUSY, sc.res[o[3]].r.status);
    invariants();
}

/* ==== 10. Secret hygiene ======================================================================== */

static bool has_run(const void *p, size_t n, uint8_t v)
{
    const uint8_t *b = p;
    size_t run = 0;
    for (size_t i = 0; i < n; i++) {
        run = b[i] == v ? run + 1 : 0;
        if (run >= 8) {
            return true;
        }
    }
    return false;
}

static void h_hygiene(int i)
{
    int bad = 0;
    bad += has_run(app.own.cache.id_priv, sizeof app.own.cache.id_priv, IDPRIV(1));
    bad += has_run(app.st.buf, sizeof app.st.buf, IDPRIV(1));
    bad += has_run(app.st.readback, sizeof app.st.readback, IDPRIV(1));
    bad += has_run(&app.st.auth_scratch, sizeof app.st.auth_scratch, IDPRIV(1));
    sc.res[i].detail = (uint32_t)bad;
}

static void test_secret_hygiene(void)
{
    setup(true);
    claim_device();
    clear_script();
    advance(g_true + MINUTE);
    step(g_true, hk(h_session, 0, 0));
    int h1 = step(g_true + 1, hk(h_hygiene, 0, 0));
    int id = step(g_true + 2, hk(h_identity, 1, 0)); /* the caller wipes its copy */
    int h2 = step(g_true + 3, hk(h_hygiene, 0, 0));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(0, sc.res[h1].detail);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[id].r.status);
    DSP_CHECK_EQ_INT(1, sc.res[id].state);
    DSP_CHECK_EQ_INT(0, sc.res[h2].detail);
    DSP_CHECK(!has_run(&app, sizeof app, IDPRIV(1))); /* nowhere in the application state */
    DSP_CHECK(!has_run(rtc, sizeof rtc, IDPRIV(1)));
}


/* Two consecutive power cuts inside recovery (around the SERVICE_REQUIRED write and the new
 * "auth" write, landed or not), each followed by a power-on reboot (the arm is lost: the user
 * must re-arm, i.e. explicitly restart), then a clean retry. After every cut: the record is the
 * old or the new one (never damaged, never unclaimed); once any recovery write landed the
 * device stays SERVICE_REQUIRED (no automatic or manual dose); issued authorizations from before
 * never work again; manual-dose marks, the BLOCKED latch and every other record are unchanged. */
static int recovery_attempt(long cut, int landed)
{
    clear_script();
    advance(g_true + MINUTE);
    step(g_true, hk(h_recover_begin, 1, 0));
    step(g_true + 1, hk(h_press, 3, 0));
    step(g_true + 4, hk(h_state, 0, 0));
    int ap = step_armed(g_true + 5, hk(h_apply, 1, 5));
    cut_at = cut;
    cut_after = landed == 1;
    bool finished = session();
    cut_at = 0;
    if (finished) {
        return sc.res[ap].r.status == DSP_OP_OK ? 1 : 2; /* completed (or refused) */
    }
    return 0; /* cut */
}

static void check_after_cut(const dsp_dosing_state_t *d0, const dsp_auth_t *a0,
                            const dsp_app_auth_t *stale, int *n_new, int *n_old)
{
    reboot_power_on();
    clear_script();
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    DSP_CHECK_EQ_INT(DSP_CRED_CLAIMED, rep.cred_state);
    DSP_CHECK_EQ_INT(0, rtc_block().arm.armed); /* power loss: re-arming is required */
    bool ok;
    dsp_auth_t a = durable_auth(&ok);
    DSP_CHECK(ok && dsp_auth_usable(&a));
    bool recovered = a.id_priv[0] == IDPRIV(7);
    DSP_CHECK(recovered || dsp_auth_equal(&a, a0)); /* old (untouched) or new */
    *n_new += recovered;
    *n_old += !recovered;
    dsp_dosing_state_t d = durable_dosing();
    for (int s = 0; s < DSP_MANUAL_SLOTS; s++) {
        DSP_CHECK(d.manual_hw[s] == d0->manual_hw[s]);
    }
    DSP_CHECK_EQ_INT(d0->flags, d.flags); /* BLOCKED latch kept */
    DSP_CHECK_EQ_INT(d0->adm_count, d.adm_count);
    snapshot(&snap_after);
    DSP_CHECK(snap_equal(&snap_before, &snap_after));
    bool restricted = d.readiness == DSP_READINESS_SERVICE_REQUIRED;
    if (recovered) {
        DSP_CHECK(restricted); /* never a new owner without SERVICE_REQUIRED */
    }
    /* a stale authorization from before the cut: never valid again */
    clear_script();
    advance(g_true + MINUTE);
    g_forged = *stale;
    int f = step(g_true, hk(h_list_forged, 0, 0));
    step(g_true, op_now()); /* trusted clock for the dose checks */
    int m = step(g_true + 1, as(op_manual(900 + (uint64_t)(*n_new + *n_old), 1000), &OWNER));
    int before = pump.calls;
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[f].r.status);
    if (restricted) {
        /* manual: refused (durably, NOT_READY or BLOCKED), automatic: the gate stays closed */
        DSP_CHECK(sc.res[m].r.status == DSP_OP_REFUSED);
        DSP_CHECK(sc.res[m].r.detail & DSP_DOSING_G_NOT_READY);
        DSP_CHECK_EQ_INT(before, pump.calls);
        clear_script();
        DSP_CHECK(wake(DSP_CAUSE_TIMER));
        DSP_CHECK_EQ_INT(before, pump.calls);
    }
}

static void test_recovery_two_cuts(void)
{
    int combos = 0, n_new = 0, n_old = 0, retried = 0;
    for (int l1 = 0; l1 < 2; l1++) {
        for (long k1 = 1; k1 <= 2; k1++) {
            for (int l2 = 0; l2 < 2; l2++) {
                for (long k2 = 1; k2 <= 2; k2++) {
                    setup(true);
                    claim_device();
                    add_phone(0, 1);
                    dsp_dosing_state_t d = durable_dosing();
                    d.flags |= DSP_DOSING_F_BLOCKED; /* a safety hold */
                    d.manual_hw[1] = 77;
                    put_dosing(&d);
                    /* an authorization issued before the recovery */
                    clear_script();
                    advance(g_true + MINUTE);
                    step(g_true, hk(h_session, 0, 0));
                    DSP_CHECK(session());
                    dsp_app_auth_t stale = g_auth[0];
                    dsp_dosing_state_t d0 = durable_dosing();
                    bool ok;
                    dsp_auth_t a0 = durable_auth(&ok);
                    snapshot(&snap_before);
                    /* first attempt, cut */
                    arm_recovery(DSP_CAUSE_POWER_ON);
                    if (recovery_attempt(k1, l1) != 0) {
                        continue; /* fewer writes than k1 */
                    }
                    check_after_cut(&d0, &a0, &stale, &n_new, &n_old);
                    /* the snapshot excludes the dosing record; after a landed SERVICE_REQUIRED
                     * the reference for the next cut is the current dosing state */
                    d0 = durable_dosing();
                    a0 = durable_auth(&ok);
                    snapshot(&snap_before); /* the check above set the phone time (records) */
                    /* the retry without re-arming is refused */
                    clear_script();
                    advance(g_true + MINUTE);
                    int no = step(g_true, hk(h_admit, DSP_SEC_RECOVERY, 0));
                    DSP_CHECK(session());
                    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[no].r.status);
                    /* second attempt (explicit restart: re-armed), cut again */
                    arm_recovery(DSP_CAUSE_POWER_ON);
                    int r2 = recovery_attempt(k2, l2);
                    if (r2 == 0) {
                        combos++;
                        check_after_cut(&d0, &a0, &stale, &n_new, &n_old);
                    } else {
                        DSP_CHECK_EQ_INT(1, r2); /* fewer writes the second time: completed */
                    }
                    /* final clean retry always completes */
                    arm_recovery(DSP_CAUSE_POWER_ON);
                    int r3 = recovery_attempt(0, 0);
                    DSP_CHECK_EQ_INT(1, r3);
                    retried += r3 == 1;
                    dsp_auth_t fin = durable_auth(&ok);
                    DSP_CHECK(ok && fin.id_priv[0] == IDPRIV(7) && dsp_auth_used_count(&fin) == 1);
                    DSP_CHECK_EQ_INT(DSP_READINESS_SERVICE_REQUIRED, durable_readiness());
                    dsp_dosing_state_t df = durable_dosing();
                    DSP_CHECK_EQ_INT(77, df.manual_hw[1]);
                    DSP_CHECK(df.flags & DSP_DOSING_F_BLOCKED);
                }
            }
        }
    }
    printf("recovery two cuts: %d double-cut combinations, %d cut states with the new record, "
           "%d with the old, %d clean retries\n", combos, n_new, n_old, retried);
    DSP_CHECK(combos >= 4);
    DSP_CHECK(n_new >= 1 && n_old >= 1);
    invariants();
}

int main(void)
{
    DSP_RUN(test_no_security_layer);
    DSP_RUN(test_claim_and_issued_authorization);
    DSP_RUN(test_forged_authorization_fields);
    DSP_RUN(test_handshake_evidence);
    DSP_RUN(test_invite_add_and_single_use);
    DSP_RUN(test_invite_expiry_and_resets);
    DSP_RUN(test_clock_step_clears_overlays);
    DSP_RUN(test_revoke_and_slot_reuse);
    DSP_RUN(test_recovery_preserves_everything);
    DSP_RUN(test_recovery_arming_rules);
    DSP_RUN(test_recovery_confirmation_failures);
    DSP_RUN(test_recovery_interrupted);
    DSP_RUN(test_recovery_two_cuts);
    DSP_RUN(test_ownership_writes_power_loss);
    DSP_RUN(test_corrupt_credentials_block_dosing);
    DSP_RUN(test_key_fault_inhibits_immediately);
    DSP_RUN(test_manual_admission_durable);
    DSP_RUN(test_session_write_limits);
    DSP_RUN(test_secret_hygiene);
    return dsp_test_finish();
}
