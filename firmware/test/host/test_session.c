/*
 * M1.10c.1 tests: the authorization boundary and the opaque frame path through complete wakes
 * (harness of test_app_ops: power cuts, pump oracle, fake BLE that connects a phone).
 *
 *  - frames: every privileged request type (and HS, encrypted frames) reaches NO operation: no
 *    dsp_app_* call, no storage write, no pump, only plain errors;
 *  - authorization: every operation refuses a caller-built {OWNER, slot}, an authorization of an
 *    earlier connection and one from before an ownership-epoch change;
 *  - HELLO, Info and advertising report the credential state (CREDENTIALS_DAMAGED, never
 *    "unclaimed" for damaged credentials) and the recovery arm;
 *  - connection scope: disconnect, stale generations, pump suspension and reboot drop the
 *    authorization;
 *  - the new status / diagnostics / acknowledge / refill / readiness operations.
 */
#define DSP_APP_OPS_NO_MAIN
#include "test_app_ops.c"

/* ---- Captured Info / advertising ------------------------------------------------------- */

static uint8_t info_rec[DSP_BLE_INFO_LEN], adv_rec[DSP_BLE_ADVERT_LEN];
static int infos;
static void cap_info(void *ctx, const uint8_t i[DSP_BLE_INFO_LEN], const uint8_t a[DSP_BLE_ADVERT_LEN])
{
    (void)ctx;
    memcpy(info_rec, i, sizeof info_rec);
    memcpy(adv_rec, a, sizeof adv_rec);
    infos++;
}
static dsp_ble_port_t BLE_INFO;

static op_t hk(void (*h)(int), int a, int b)
{
    op_t o = mk(OP_HOOK);
    o.hook = h;
    o.a = a;
    o.b = b;
    return o;
}

static op_t op_frame(const uint8_t *f, size_t n)
{
    op_t o = mk(OP_FRAME);
    memcpy(o.frame, f, n);
    o.frame_len = n;
    return o;
}

static void put_auth(const dsp_auth_t *a)
{
    uint8_t b[DSP_AUTH_RECORD_BYTES];
    DSP_CHECK_EQ_INT(DSP_AUTH_RECORD_BYTES, dsp_codec_encode_auth(a, b, sizeof b));
    fake_kv_put_raw(&kv, "auth", b, sizeof b);
}

/* ==== 1. No frame reaches an operation ==================================================== */

/* Durable records snapshot: every key except "time" (timekeeper bookkeeping, written by a
 * button wake that does nothing else) must stay byte-identical across refused requests. */
static fake_kv_entry_t snap_kv[FAKE_KV_MAX_KEYS];
static void kv_snapshot(void)
{
    memcpy(snap_kv, kv.durable, sizeof snap_kv);
}
static bool kv_unchanged_except_time(void)
{
    for (int i = 0; i < FAKE_KV_MAX_KEYS; i++) {
        const fake_kv_entry_t *a = &snap_kv[i], *b = &kv.durable[i];
        if (a->used != b->used || strcmp(a->key, b->key) != 0) {
            return false;
        }
        if (a->used && strcmp(a->key, "time") != 0 &&
            (a->len != b->len || memcmp(a->value, b->value, a->len) != 0)) {
            fprintf(stderr, "record %s changed\n", a->key);
            return false;
        }
    }
    return true;
}

static void test_frames_reach_no_operation(void)
{
    provision(true);
    clear_script();
    advance(g_true + MINUTE);
    kv_snapshot();
    int pumps0 = pump.calls;
    int n = 0;
    for (int t = 0; t < 256; t++) {
        uint8_t probe[8] = {1, 0, (uint8_t)t, 0, (uint8_t)t, 0, 0, 0};
        dsp_ble_header_t h;
        const uint8_t *b;
        if (dsp_ble_parse_header(probe, sizeof probe, &h, &b) != DSP_BLE_OK || t == DSP_BLE_T_HELLO ||
            t == DSP_BLE_T_END_SESSION) {
            continue;
        }
        step(g_true + n, op_frame(probe, sizeof probe)); /* one per second */
        n++;
    }
    /* a full MANUAL_DOSE request body and an encrypted frame */
    const uint8_t md[20] = {1, 0, 0x40, 0, 0x99, 0, 12, 0, 5, 0, 0, 0, 0, 0, 0, 0, 0xE8, 3, 0, 0};
    step(g_true + n, op_frame(md, sizeof md));
    uint8_t enc[8 + 16] = {1, 0, 0x10, DSP_BLE_F_ENCRYPTED, 1, 0, 16, 0};
    step(g_true + n + 1, op_frame(enc, sizeof enc));
    step(g_true + n + 2, mk(OP_END_TIMEOUT)); /* no authorization of any kind in this wake */
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(32, n); /* 31 privileged types (GET_HISTORY since Phase C, GET_BATTERY since fw 0.2.0, GET_UPDATE_STATUS/START_UPDATE since 0.3.0, PRIME_START/STOP/STATUS since 0.3.3) + HS */
    DSP_CHECK_EQ_INT(0, (int)rep.ops);       /* no dsp_app_* operation was entered */
    DSP_CHECK(kv_unchanged_except_time());   /* nothing written by any frame */
    DSP_CHECK_EQ_INT(pumps0, pump.calls);    /* no pump */
    DSP_CHECK(rep.frames_in >= 20);
    DSP_CHECK(rep.link_closes >= 8);         /* every third error, HS and the encrypted frame */
    for (int i = 0; i < sc.nresp; i++) {
        DSP_CHECK(sc.resp[i][3] == (DSP_BLE_F_RESPONSE | DSP_BLE_F_ERROR) ||
                  sc.resp[i][2] == DSP_BLE_T_END_SESSION);
        if (sc.resp[i][2] != DSP_BLE_T_END_SESSION) {
            uint8_t code = sc.resp[i][8];
            DSP_CHECK(code == DSP_BLE_E_UNAUTHENTICATED || code == DSP_BLE_E_HANDSHAKE_FAILED ||
                      code == DSP_BLE_E_MALFORMED);
        }
    }
    invariants();
}

/* ==== 2. Caller-built and stale authorizations are refused everywhere =================== */

static int stale_mode; /* 0: caller-built, 1: earlier connection, 2: before an epoch change */
static dsp_app_auth_t stale_auth;
static int refused, ran;
static void h_all_ops(int i)
{
    (void)i;
    const dsp_app_auth_t *a = &stale_auth;
    dsp_config_t c;
    memset(&c, 0, sizeof c);
    dsp_schedule_t s = sched("x", 9, 0, 1000);
    dsp_app_schedule_page_t pg;
    dsp_app_time_result_t tr;
    dsp_app_manual_result_t mr;
    uint8_t st;
    uint32_t det, rev, srev;
    uint64_t fl;
    dsp_ble_status_rec_t sr;
    dsp_ble_diag_rec_t dr;
    dsp_app_phone_t ph[4];
    uint8_t n;
    bool ends;
    uint8_t tok[16];
    uint16_t exp;
    dsp_app_op_result_t r[22];
    int k = 0;
    r[k++] = dsp_app_get_config(&app, a, &c, NULL);
    r[k++] = dsp_app_set_config(&app, a, 0, DSP_CONFIG_F_LOW_STOCK, &c, &rev);
    r[k++] = dsp_app_list_schedules(&app, a, 0, &pg);
    r[k++] = dsp_app_get_schedule(&app, a, "x", &s);
    r[k++] = dsp_app_create_schedule(&app, a, &s, &srev);
    r[k++] = dsp_app_update_schedule(&app, a, 1, &s, &rev, &srev);
    r[k++] = dsp_app_delete_schedule(&app, a, "x", 1, &srev);
    r[k++] = dsp_app_set_phone_time(&app, a, (int64_t)g_true * 1000, 500, &tr);
    r[k++] = dsp_app_manual_dose(&app, a, 77, 1000, &mr);
    r[k++] = dsp_app_get_result(&app, a, 1, &st, &det);
    r[k++] = dsp_app_manual_floor(&app, a, &fl);
    r[k++] = dsp_app_get_status(&app, a, &sr);
    r[k++] = dsp_app_get_diagnostics(&app, a, &dr);
    r[k++] = dsp_app_ack_rejections(&app, a);
    r[k++] = dsp_app_refill(&app, a, CAP, CAP, NULL);
    r[k++] = dsp_app_set_readiness(&app, a, DSP_READINESS_SERVICE_REQUIRED, NULL);
    r[k++] = dsp_app_phones_list(&app, a, ph, &n);
    r[k++] = dsp_app_phone_remove(&app, a, 1, &ends);
    r[k++] = dsp_app_invite_create(&app, a, tok, &exp);
    r[k++] = dsp_app_invite_cancel(&app, a);
    r[k++] = dsp_app_recovery_cancel(&app, a);
    for (int j = 0; j < k; j++) {
        ran++;
        refused += r[j].status == DSP_OP_FORBIDDEN;
    }
}
static void h_issue_then_reconnect(int i)
{
    (void)i;
    stale_auth = *issue_for(&OWNER);
    if (stale_mode == 1) {
        dsp_app_sec_connection_end(&app);
        dsp_app_sec_connection_begin(&app); /* a new connection: new session ID */
    }
}
static void h_change_epoch(int i)
{
    (void)i;
    bool ends;
    dsp_app_auth_t other = *issue_for(&OWNER3);
    (void)dsp_app_phone_remove(&app, &other, 2, &ends); /* epoch + 1 by another phone */
}

static void test_unissued_authorizations_refused(void)
{
    for (stale_mode = 0; stale_mode < 3; stale_mode++) {
        provision(true);
        clear_script();
        advance(g_true + MINUTE);
        refused = ran = 0;
        kv_snapshot();
        if (stale_mode == 0) {
            for (uint8_t slot = 0; slot < 4; slot++) {
                stale_auth = (dsp_app_auth_t){DSP_APP_ROLE_OWNER, slot, 0, 0};
                step(g_true, hk(h_all_ops, 0, 0));
            }
        } else {
            step(g_true, hk(h_issue_then_reconnect, 0, 0));
            if (stale_mode == 2) {
                step(g_true, hk(h_change_epoch, 0, 0));
            }
            step(g_true, hk(h_all_ops, 0, 0));
        }
        step(g_true, mk(OP_END_TIMEOUT));
        DSP_CHECK(session());
        DSP_CHECK(ran >= 21);
        DSP_CHECK_EQ_INT(ran, refused);
        if (stale_mode == 0) {
            DSP_CHECK(kv_unchanged_except_time()); /* no write by any refused operation */
        }
    }
    /* the same operations with the authorization issued for this connection work */
    provision(true);
    clear_script();
    advance(g_true + MINUTE);
    int g = step(g_true, op_floor());
    int s = step(g_true, mk(OP_GET_CONFIG));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[g].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[s].r.status);
    /* no security layer registered: nothing can be issued, every operation refused */
    provision(true);
    CFG.security = NULL;
    clear_script();
    advance(g_true + MINUTE);
    int f = step(g_true, op_floor());
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[f].r.status);
    invariants();
}

/* Tampering with an issued authorization: each field alone, and (white box) an ownership-epoch
 * change that no API path leaves bound (every real path drops, re-binds or degrades), so the
 * defense-in-depth epoch comparison is exercised directly. */
static dsp_app_op_result_t tamper_res[6];
static void h_tamper(int i)
{
    (void)i;
    const dsp_app_auth_t good = *issue_for(&OWNER);
    uint64_t fl;
    dsp_app_auth_t t[4] = {good, good, good, good};
    t[0].slot = (uint8_t)((good.slot + 1) % 4);
    t[1].token ^= 1;
    t[2].session_id ^= 1;
    t[3].role = 0;
    for (int k = 0; k < 4; k++) {
        tamper_res[k] = dsp_app_manual_floor(&app, &t[k], &fl);
    }
    app.own.epoch++; /* RAM only, restored below */
    tamper_res[4] = dsp_app_manual_floor(&app, &good, &fl);
    app.own.epoch--;
    tamper_res[5] = dsp_app_manual_floor(&app, &good, &fl);
}

static void test_tampered_authorization_refused(void)
{
    provision(true);
    clear_script();
    advance(g_true + MINUTE);
    step(g_true, hk(h_tamper, 0, 0));
    DSP_CHECK(session());
    for (int k = 0; k < 5; k++) {
        DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, tamper_res[k].status);
    }
    DSP_CHECK_EQ_INT(DSP_OP_OK, tamper_res[5].status);
}

/* ==== 3. HELLO, Info and advertising =========================================================== */

static uint8_t hello_flags(int idx, uint8_t *claim)
{
    dsp_ble_msg_t m;
    DSP_CHECK_EQ_INT(DSP_BLE_OK, dsp_ble_decode(sc.resp[idx], sc.resp_len[idx], 0, &m));
    DSP_CHECK_EQ_INT(DSP_BLE_T_HELLO, m.h.type);
    *claim = m.b.hello_rsp.claim_state;
    DSP_CHECK(m.b.hello_rsp.session_id != 0);
    return m.b.hello_rsp.flags;
}

static void test_hello_info_report_state(void)
{
    const uint8_t hello[10] = {1, 0, 1, 0, 3, 0, 2, 0, 1, 1};
    for (int kind = 0; kind < 4; kind++) { /* claimed, unclaimed, damaged, armed */
        provision(true);
        BLE_INFO = BLE;
        BLE_INFO.publish_info = cap_info;
        CFG.ble = &BLE_INFO;
        infos = 0;
        if (kind == 1) {
            dsp_auth_t p;
            dsp_auth_provisioned(&p);
            put_auth(&p);
        } else if (kind == 2) {
            fake_kv_remove(&kv, "auth");
        } else if (kind == 3) {
            CFG.recovery_hold_ms = DSP_OWNER_ARM_HOLD_MS;
        }
        clear_script();
        advance(g_true + MINUTE);
        step(g_true, op_frame(hello, sizeof hello));
        DSP_CHECK(session());
        CFG.recovery_hold_ms = 0;
        DSP_CHECK(sc.nresp >= 1);
        uint8_t claim = 9;
        uint8_t flags = hello_flags(0, &claim);
        const uint8_t want_claim = kind == 1 ? 0 : 1;
        const uint8_t want_flags = kind == 2 ? DSP_BLE_HELLO_F_CREDENTIALS_DAMAGED
                                             : (kind == 3 ? DSP_BLE_HELLO_F_RECOVERY_ARMED : 0);
        DSP_CHECK_EQ_INT(want_claim, claim);
        DSP_CHECK_EQ_INT(want_flags, flags);
        DSP_CHECK(infos >= 1);
        dsp_ble_info_t info;
        dsp_ble_advert_t adv;
        DSP_CHECK_EQ_INT(DSP_BLE_OK, dsp_ble_info_decode(info_rec, sizeof info_rec, &info));
        DSP_CHECK_EQ_INT(DSP_BLE_OK, dsp_ble_advert_decode(adv_rec, sizeof adv_rec, &adv));
        DSP_CHECK_EQ_INT(want_claim, info.claim_state);
        DSP_CHECK_EQ_INT(want_flags, info.flags);
        uint8_t want_adv = (uint8_t)((want_claim ? DSP_BLE_ADV_F_CLAIMED : 0) |
                                     (kind == 3 ? DSP_BLE_ADV_F_RECOVERY_ARMED : 0) |
                                     (kind == 2 ? DSP_BLE_ADV_F_CREDENTIALS_DAMAGED : 0));
        DSP_CHECK_EQ_INT(want_adv, adv.flags);
    }
}

/* C2 fail-closed: without randomness no connection gets a session ID, so no handshake can be
 * admitted and no authorization can be issued; the public HELLO still answers. */
static bool rng_down(void *ctx, uint8_t *out, size_t len)
{
    (void)ctx;
    memset(out, 0, len);
    return false;
}
static const dsp_app_security_t SEC_RNG_DOWN = {.ctx = NULL, .random = rng_down};
static dsp_app_op_result_t admit_res[4];
static void h_admit_all(int i)
{
    (void)i;
    for (int p = 0; p < 4; p++) {
        dsp_app_hs_t hs;
        admit_res[p] = dsp_app_sec_admit(&app, (dsp_sec_purpose_t)(DSP_SEC_SESSION + p), &hs);
    }
}

static void test_rng_failure_fails_closed(void)
{
    const uint8_t hello[10] = {1, 0, 1, 0, 4, 0, 2, 0, 1, 1};
    provision(true);
    CFG.security = &SEC_RNG_DOWN;
    clear_script();
    advance(g_true + MINUTE);
    step(g_true, op_frame(hello, sizeof hello));
    step(g_true, hk(h_admit_all, 0, 0));
    step(g_true, mk(OP_END_TIMEOUT));
    kv_snapshot();
    DSP_CHECK(session());
    DSP_CHECK(sc.nresp >= 1);
    dsp_ble_msg_t m;
    DSP_CHECK_EQ_INT(DSP_BLE_OK, dsp_ble_decode(sc.resp[0], sc.resp_len[0], 0, &m));
    DSP_CHECK_EQ_INT(DSP_BLE_T_HELLO, m.h.type);
    DSP_CHECK_EQ_INT(0, (int)m.b.hello_rsp.session_id);
    for (int p = 0; p < 4; p++) {
        DSP_CHECK(admit_res[p].status != DSP_OP_OK);
    }
    DSP_CHECK(kv_unchanged_except_time());
    CFG.security = &HARNESS_SEC;
}

/* ==== M1.10d (T7): build-only power-on service window ============================================
 * Without the option a power-on wake never opens BLE and the security precondition refuses; with
 * it, the power-on wake is a presence wake, and authentication is still required for everything
 * (the owner operation below goes through the harness's issued authorization; a raw one fails). */
static dsp_app_op_result_t t7_raw;
static void h_t7_raw(int i)
{
    (void)i;
    uint64_t fl;
    dsp_app_auth_t forged = OWNER;
    forged.token = 0x1234;
    t7_raw = dsp_app_manual_floor(&app, &forged, &fl);
}
static void test_power_on_window_option(void)
{
    provision(true);
    clear_script();
    advance(g_true + MINUTE);
    int st = sc.starts;
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    DSP_CHECK(!rep.ble_session);
    DSP_CHECK_EQ_INT(st, sc.starts);
    CFG.service_on_power_on = true;
    clear_script();
    advance(g_true + MINUTE);
    step(g_true, hk(h_admit_all, 0, 0));
    step(g_true, hk(h_t7_raw, 0, 0));
    int fl = step(g_true + 1, op_floor());
    step(g_true + 2, mk(OP_END));
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    CFG.service_on_power_on = false;
    DSP_CHECK(rep.ble_session);
    DSP_CHECK_EQ_INT(DSP_OP_OK, admit_res[0].status);      /* the presence precondition holds */
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, t7_raw.status);     /* never without authentication */
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[fl].r.status);       /* issued authorization: allowed */
    DSP_CHECK_EQ_INT(DSP_BLE_END_COMMAND, rep.ble_end);
    /* a timer wake never opens the window, with or without the option */
    CFG.service_on_power_on = true;
    clear_script();
    sleep_and_wake();
    CFG.service_on_power_on = false;
    DSP_CHECK(!rep.ble_session);
    invariants();
}

/* ==== C10: only an authorized owner ends the service window ================================ */

static bool win_open[8];
static int win_checks;
static void h_window_open(int i)
{
    (void)i;
    if (win_checks < 8) {
        win_open[win_checks++] = app.accepting && app.ss.end == DSP_BLE_END_NONE;
    }
}
static void h_hs_failed(int i)
{
    (void)i;
    dsp_app_sec_handshake_failed(&app); /* what the M1.10c.2 security layer reports */
}
static dsp_app_op_result_t end_forged, end_unissued;
static void h_end_forged(int i)
{
    (void)i;
    const dsp_app_auth_t forged = {DSP_APP_ROLE_OWNER, 0, app.ss.session_id, 0x1234};
    end_forged = dsp_app_end_service_session(&app, &forged);
    end_unissued = dsp_app_end_service_session(&app, &OWNER);
}
static uint32_t stored_failed_sessions(void)
{
    const fake_kv_entry_t *e = fake_kv_find(&kv, "auth");
    dsp_auth_t a;
    if (e == NULL || dsp_codec_decode_auth(e->value, e->len, &a) != DSP_CODEC_OK) {
        return UINT32_MAX;
    }
    uint32_t n = a.failed_sessions;
    dsp_auth_wipe(&a);
    return n;
}

static void test_unauthenticated_end_keeps_window(void)
{
    const uint8_t hello[10] = {1, 0, 1, 0, 5, 0, 2, 0, 1, 1};
    const uint8_t end[8] = {1, 0, DSP_BLE_T_END_SESSION, 0, 0x7E, 0, 0, 0};
    provision(true);
    clear_script();
    advance(g_true + MINUTE);
    win_checks = 0;
    /* a nearby client: HELLO + END_SESSION, then 20 more connections that only send END */
    step(g_true, op_frame(hello, sizeof hello));
    step(g_true + 1, op_frame(end, sizeof end));
    for (int k = 0; k < 20; k++) {
        step(g_true + 2 + k, op_frame(end, sizeof end));
    }
    step(g_true + 25, hk(h_window_open, 0, 0));
    /* forged and unissued authorizations cannot end it either */
    step(g_true + 26, hk(h_end_forged, 0, 0));
    step(g_true + 27, hk(h_window_open, 0, 0));
    /* the owner, connecting later in the same window, is served and ends it */
    int fl = step(g_true + 120, op_floor());
    DSP_CHECK(session());
    DSP_CHECK(sc.nresp >= 2);
    DSP_CHECK_EQ_INT(DSP_BLE_T_END_SESSION, sc.resp[1][2]);
    DSP_CHECK_EQ_INT(DSP_BLE_F_RESPONSE, sc.resp[1][3]); /* the END is answered... */
    DSP_CHECK(sc.disconnects >= 21);                      /* ...and only that connection closes */
    DSP_CHECK_EQ_INT(2, win_checks);
    DSP_CHECK(win_open[0]);
    DSP_CHECK(win_open[1]);
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, end_forged.status);
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, end_unissued.status);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[fl].r.status);
    DSP_CHECK_EQ_INT(DSP_BLE_END_COMMAND, rep.ble_end); /* ended by the owner's authorized END */
    /* without the owner the window lasts to its own timeout, whatever the client sends */
    clear_script();
    advance(g_true + MINUTE);
    for (int k = 0; k < 10; k++) {
        step(g_true + 3 * k, op_frame(end, sizeof end));
    }
    step(g_true + 40, mk(OP_END_TIMEOUT));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_BLE_END_MAX, rep.ble_end);
    invariants();
}

/* Failed handshakes close only their connection: the owner can still authenticate in the same
 * window, the durable diagnostic counter rises at most once per window, and nothing carries a
 * lockout into the next button wake. */
static void test_handshake_failures_no_lockout(void)
{
    const uint8_t hello[10] = {1, 0, 1, 0, 6, 0, 2, 0, 1, 1};
    const uint8_t hs[12] = {1, 0, DSP_BLE_T_HS, 0, 7, 0, 4, 0, 1, 0, 0xAA, 0xBB};
    provision(true);
    uint32_t f0 = stored_failed_sessions();
    DSP_CHECK(f0 != UINT32_MAX);
    for (int w = 0; w < 3; w++) { /* three consecutive button windows */
        clear_script();
        advance(g_true + MINUTE);
        win_checks = 0;
        for (int k = 0; k < 10; k++) { /* 10 failed handshakes, each on its own connection */
            step(g_true + 2 * k, op_frame(hello, sizeof hello));
            step(g_true + 2 * k, op_frame(hs, sizeof hs));
            step(g_true + 2 * k + 1, hk(h_hs_failed, 0, 0));
        }
        step(g_true + 30, hk(h_window_open, 0, 0));
        int fl = step(g_true + 31, op_floor()); /* owner: admit + IK lookup + operation */
        DSP_CHECK(session());
        DSP_CHECK_EQ_INT(1, win_checks);
        DSP_CHECK(win_open[0]);
        DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[fl].r.status);
        DSP_CHECK_EQ_INT(DSP_BLE_END_COMMAND, rep.ble_end);
        DSP_CHECK_EQ_INT((int)(f0 + (uint32_t)w + 1), (int)stored_failed_sessions());
    }
    invariants();
}

/* ==== 4. Connection scope ========================================================================= */

static dsp_app_auth_t kept;
static dsp_app_op_result_t after_disc, after_stale;
static void h_keep(int i)
{
    (void)i;
    kept = *issue_for(&OWNER);
}
static void h_disconnect_event(int i)
{
    (void)i;
    (void)dsp_app_post_conn_event(&app, false, sc.gen, 0);
}
static void h_use_kept(int i)
{
    uint64_t fl;
    sc.res[i].r = dsp_app_manual_floor(&app, &kept, &fl);
}

static void test_connection_scope(void)
{
    provision(true);
    clear_script();
    advance(g_true + MINUTE);
    step(g_true, hk(h_keep, 0, 0));
    int ok = step(g_true, hk(h_use_kept, 0, 0));
    step(g_true + 1, hk(h_disconnect_event, 0, 0));
    int gone = step(g_true + 2, hk(h_use_kept, 0, 0));
    /* a frame of the old connection generation is dropped */
    const uint8_t hello[10] = {1, 0, 1, 0, 3, 0, 2, 0, 1, 1};
    int fr = step(g_true + 3, op_frame(hello, sizeof hello));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[ok].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[gone].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[fr].r.status); /* posted ... */
    DSP_CHECK(rep.frames_stale >= 1);                 /* ... and dropped (no connection) */
    /* the pump suspends BLE: the connection and its authorization end */
    provision(true);
    clear_script();
    advance(g_true + MINUTE);
    step(g_true, hk(h_keep, 0, 0));
    int m = step(g_true, as(op_manual(5, 1000), &OWNER));
    int after = step(g_true + 5, hk(h_use_kept, 0, 0));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[m].r.status);
    DSP_CHECK_EQ_INT(1, sc.res[m].pumps);
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[after].r.status);
    /* reboot: nothing survives */
    reboot_power_on();
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    DSP_CHECK(!app.own.authz_valid);
    DSP_CHECK_EQ_INT(0, app.ss.session_id);
    clear_script();
    advance(g_true + MINUTE);
    int rb = step(g_true, hk(h_use_kept, 0, 0));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[rb].r.status);
    (void)after_disc;
    (void)after_stale;
    invariants();
}

/* ==== 5. Status, diagnostics, acknowledge, refill, readiness ===================================== */

static dsp_ble_status_rec_t st0, st2;
static dsp_ble_diag_rec_t dg;
static dsp_app_op_result_t ack_r, refill_bad, ready_r;
static long ack_writes;
static void h_status(int i)
{
    (void)i;
    (void)dsp_app_get_status(&app, issue_for(&OWNER), &st0);
    static const dsp_app_auth_t S2 = {DSP_APP_ROLE_OWNER, 2, 0, 0};
    (void)dsp_app_get_status(&app, issue_for(&S2), &st2);
    (void)dsp_app_get_diagnostics(&app, issue_for(&OWNER), &dg);
    long s = kv.sets;
    ack_r = dsp_app_ack_rejections(&app, issue_for(&OWNER));
    ack_writes = kv.sets - s;
    refill_bad = dsp_app_refill(&app, issue_for(&OWNER), 1000, 2000, NULL); /* remaining > cap */
}
static void h_ready(int i)
{
    (void)i;
    dsp_ble_status_rec_t st;
    ready_r = dsp_app_set_readiness(&app, issue_for(&OWNER), DSP_READINESS_READY, &st);
}

/* ==== 4a. Connection-state reconciliation (M1.10d, decision T2) ==================================
 * The transport reports connection events into a snapshot that is never refused for lack of
 * space. These hooks run inside the application task, so every sequence below is posted with
 * the application NOT running between the callbacks (the worst interleaving of a radio task). */

static size_t opened_before, stale_before;
static bool posted[8];
static const uint8_t T2_HELLO[10] = {1, 0, 1, 0, 0x21, 0, 2, 0, 1, 1}; /* txn 0x21 */

static void h_t2_before(int i)
{
    (void)i;
    kept = *issue_for(&OWNER);
    opened_before = app.rep.links_opened;
    stale_before = app.rep.frames_stale;
    sc.nresp = 0;
}
/* a: disconnect then connect at once; b: a further disconnect of the new generation;
 * c: a frame of the old generation posted after its disconnect; d: a 1000-cycle storm */
static void h_t2_storm(int i)
{
    int mode = sc.op[i].a;
    uint32_t g = sc.gen;
    memset(posted, 0, sizeof posted);
    posted[0] = dsp_app_post_frame(&app, g, T2_HELLO, sizeof T2_HELLO); /* queued on g */
    posted[1] = dsp_app_post_conn_event(&app, false, g, 0);
    if (mode == 'd') {
        for (int k = 0; k < 1000; k++) {
            (void)dsp_app_post_conn_event(&app, true, ++sc.gen, 247);
            (void)dsp_app_post_conn_event(&app, false, sc.gen, 0);
        }
    }
    posted[2] = dsp_app_post_conn_event(&app, true, ++sc.gen, 247);
    posted[3] = dsp_app_post_frame(&app, g, T2_HELLO, sizeof T2_HELLO); /* stale producer */
    if (mode == 'b') {
        posted[4] = dsp_app_post_conn_event(&app, false, sc.gen, 0);
    }
    posted[5] = !dsp_app_post_conn_event(&app, true, g, 247);      /* older: refused */
    posted[6] = !dsp_app_post_conn_event(&app, true, sc.gen, 247); /* same: refused */
}
static void h_t2_saturate(int i)
{
    (void)i;
    memset(posted, 0, sizeof posted);
    int n = 0;
    for (int k = 0; k < DSP_APP_CMDQ_CAPACITY; k++) {
        n += dsp_app_post_frame(&app, sc.gen, T2_HELLO, sizeof T2_HELLO);
    }
    posted[0] = n == DSP_APP_CMDQ_CAPACITY;
    posted[1] = !dsp_app_post_frame(&app, sc.gen, T2_HELLO, sizeof T2_HELLO); /* queue full */
    posted[2] = dsp_app_post_conn_event(&app, false, sc.gen, 0); /* never hidden by the queue */
}
static void h_t2_fault(int i)
{
    for (int k = 0; k < sc.op[i].a; k++) {
        (void)dsp_app_post_rx_fault(&app, sc.gen);
    }
    (void)dsp_app_post_rx_fault(&app, sc.gen - 1); /* another generation: ignored */
}
static void h_t2_reconnect(int i)
{
    (void)i;
    (void)dsp_app_post_conn_event(&app, true, ++sc.gen, 247);
}

static void h_t2_noop(int i)
{
    (void)i;
}

static int t2_hello_responses(void)
{
    int n = 0;
    for (int k = 0; k < sc.nresp; k++) {
        n += sc.resp_len[k] >= 8 && sc.resp[k][2] == 1 && sc.resp[k][4] == 0x21;
    }
    return n;
}

static void test_link_reconcile(void)
{
    const int modes[] = {'a', 'b', 'c', 'd'};
    for (size_t m = 0; m < sizeof modes / sizeof modes[0]; m++) {
        provision(true);
        clear_script();
        advance(g_true + MINUTE);
        sc.stale_sends = 0;
        step(g_true, hk(h_t2_before, 0, 0));
        step(g_true + 1, hk(h_t2_storm, modes[m], 0));
        int gone = step(g_true + 2, hk(h_use_kept, 0, 0));
        DSP_CHECK(session());
        DSP_CHECK(posted[0] && posted[1] && posted[2] && posted[5] && posted[6]);
        DSP_CHECK(posted[3]); /* the queue accepts it; the application drops it as stale */
        /* the old session never survives: its authorization, session ID and frames are gone */
        DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[gone].r.status);
        DSP_CHECK(app.rep.frames_stale >= stale_before + 2);
        DSP_CHECK_EQ_INT(0, t2_hello_responses()); /* nothing answered for generation g ... */
        DSP_CHECK_EQ_INT(0, sc.stale_sends);       /* ... and nothing sent to another one */
        if (modes[m] == 'b') {
            DSP_CHECK_EQ_INT((int)opened_before, (int)app.rep.links_opened); /* none opened */
        } else {
            DSP_CHECK_EQ_INT((int)opened_before + 1, (int)app.rep.links_opened); /* only the last */
        }
    }
    /* Queue saturation cannot hide a disconnect: four queued frames, a refused fifth, then the
     * disconnect; every queued frame is stale and the authorization ended. */
    provision(true);
    clear_script();
    advance(g_true + MINUTE);
    sc.stale_sends = 0;
    step(g_true, hk(h_t2_before, 0, 0));
    step(g_true + 1, hk(h_t2_saturate, 0, 0));
    int gone = step(g_true + 2, hk(h_use_kept, 0, 0));
    step(g_true + 3, hk(h_t2_reconnect, 0, 0));
    DSP_CHECK(session());
    DSP_CHECK(posted[0] && posted[1] && posted[2]);
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[gone].r.status);
    DSP_CHECK(app.rep.frames_stale >= stale_before + DSP_APP_CMDQ_CAPACITY);
    DSP_CHECK_EQ_INT(0, t2_hello_responses());
    DSP_CHECK_EQ_INT(0, sc.stale_sends);
    /* Fragment errors before authentication (no security session: the M1.10c.1 gate): a plain
     * MALFORMED (type 0, txn 0) each, the third closes the connection; another generation's
     * report is ignored. */
    provision(true);
    clear_script();
    advance(g_true + MINUTE);
    step(g_true, hk(h_t2_before, 0, 0));
    step(g_true + 1, hk(h_t2_fault, 2, 0));
    step(g_true + 2, hk(h_t2_fault, 1, 0));
    step(g_true + 3, hk(h_t2_noop, 0, 0)); /* the application handles the reports first */
    DSP_CHECK(session());
    int malformed = 0;
    for (int k = 0; k < sc.nresp; k++) {
        malformed += sc.resp_len[k] == 17 && sc.resp[k][2] == 0 && (sc.resp[k][3] & 0x02) != 0 &&
                     sc.resp[k][4] == 0 && sc.resp[k][8] == DSP_BLE_E_MALFORMED;
    }
    DSP_CHECK_EQ_INT(3, malformed);
    DSP_CHECK_EQ_INT(3, (int)app.rep.rx_faults);
    DSP_CHECK(app.rep.link_closes >= 1);
    invariants();
}

static void test_status_and_service_operations(void)
{
    provision(true);
    /* manual requests from slot 0 (floor 4) */
    clear_script();
    advance(g_true + MINUTE);
    for (int k = 1; k <= 3; k++) {
        step(g_true + (dsp_utc_t)(k * 70), op_manual((uint64_t)k, 9999999u));
    }
    DSP_CHECK(session());
    clear_script();
    advance(g_true + MINUTE);
    step(g_true, hk(h_status, 0, 0));
    DSP_CHECK(session());
    DSP_CHECK(st0.storage_ok);
    DSP_CHECK_EQ_INT(DSP_READINESS_READY, st0.readiness);
    DSP_CHECK_EQ_INT(CAP, st0.capacity_ul);
    DSP_CHECK_EQ_INT(255, st0.battery_percent);
    DSP_CHECK(st0.session_seconds_left > 800 && st0.session_seconds_left <= 900);
    DSP_CHECK_EQ_INT(4, st0.manual_floor); /* slot 0: three decided requests */
    DSP_CHECK_EQ_INT(1, st2.manual_floor); /* slot 2: none */
    DSP_CHECK(st0.next_due_known == false || st0.next_due_at > 0);
    DSP_CHECK(dg.notify_storage_ok);
    DSP_CHECK_EQ_INT(DSP_OP_OK, ack_r.status);
    DSP_CHECK_EQ_INT(0, ack_writes); /* nothing unread: no write */
    DSP_CHECK_EQ_INT(DSP_OP_INVALID, refill_bad.status);
    /* the codec accepts the produced records */
    uint8_t buf[DSP_BLE_FRAME_MAX];
    size_t len;
    dsp_ble_msg_t m;
    memset(&m, 0, sizeof m);
    m.h.major = 1;
    m.h.type = DSP_BLE_T_GET_STATUS;
    m.h.flags = DSP_BLE_F_RESPONSE;
    m.b.status.status = st0;
    DSP_CHECK_EQ_INT(DSP_BLE_OK, dsp_ble_encode(&m, buf, sizeof buf, &len));
    m.h.type = DSP_BLE_T_GET_DIAGNOSTICS;
    m.b.diag.diag = dg;
    DSP_CHECK_EQ_INT(DSP_BLE_OK, dsp_ble_encode(&m, buf, sizeof buf, &len));
    /* READY refused with the gates while not configured (fresh device) */
    provision(false);
    clear_script();
    dsp_config_t z = calibration();
    z.max_dose_ul = 0;
    step(g_true, op_set_cfg_cur(DSP_CONFIG_F_MAX_DOSE, z));
    step(g_true, hk(h_ready, 0, 0));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_REFUSED, ready_r.status);
    DSP_CHECK(ready_r.detail & DSP_DOSING_G_PUMP_CONFIG);
    DSP_CHECK_EQ_INT(DSP_READINESS_SERVICE_REQUIRED, durable_readiness());
    /* refill and readiness count against the session write budget */
    provision(false);
    CFG.session_writes_max = 1;
    clear_script();
    int r1 = step(g_true, op_refill(CAP / 2));
    int r2 = step(g_true, op_ready());
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[r1].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_BUSY, sc.res[r2].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_B_RATE_LIMITED, sc.res[r2].r.detail);
    invariants();
}

int main(void)
{
    DSP_RUN(test_frames_reach_no_operation);
    DSP_RUN(test_unissued_authorizations_refused);
    DSP_RUN(test_tampered_authorization_refused);
    DSP_RUN(test_hello_info_report_state);
    DSP_RUN(test_rng_failure_fails_closed);
    DSP_RUN(test_unauthenticated_end_keeps_window);
    DSP_RUN(test_handshake_failures_no_lockout);
    DSP_RUN(test_connection_scope);
    DSP_RUN(test_link_reconcile);
    DSP_RUN(test_power_on_window_option);
    DSP_RUN(test_status_and_service_operations);
    return dsp_test_finish();
}
