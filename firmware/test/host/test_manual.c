/*
 * M1.10b.2 tests: manual dose with durable per-slot anti-replay, through the Application Service
 * (dsp_app_manual_dose / dsp_app_get_result / dsp_app_manual_floor) and the Dosing Service, on
 * the shared harness of test_app_ops.c (complete wakes, KV with power cuts that abort the wake,
 * clock, pump, power port, BLE fake that runs operations in the application task).
 *
 * Invariant under test: once a manual-dose request (slot, seq) has been durably accepted or
 * refused, it never activates the pump again - across duplicates, lost responses, reboots,
 * power loss at every write and during the pump, result-ring eviction and changed conditions.
 * The harness pump oracle checks on EVERY manual pump start that the decision was durable
 * before it (marker bound to the request, high-water mark raised, ring entry PENDING) and that
 * no request (or automatic occurrence) ever starts the pump twice.
 */
#define DSP_APP_OPS_NO_MAIN
#include "test_app_ops.c"

#define VOL 1000u /* 0.84 s at 1200 ul/s */

static const dsp_app_auth_t OWNER1 = {DSP_APP_ROLE_OWNER, 1, 0, 0};

static dsp_dosing_state_t durable_dosing_state(void)
{
    dsp_dosing_state_t d;
    memset(&d, 0, sizeof d);
    const fake_kv_entry_t *e = fake_kv_find(&kv, "dosing");
    DSP_CHECK(e != NULL && dsp_codec_decode_dosing(e->value, e->len, &d) == DSP_CODEC_OK);
    return d;
}
static void put_dosing_state(const dsp_dosing_state_t *d)
{
    uint8_t b[DSP_DOSING_RECORD_BYTES];
    DSP_CHECK_EQ_INT(DSP_DOSING_RECORD_BYTES, dsp_codec_encode_dosing(d, b, sizeof b));
    fake_kv_put_raw(&kv, "dosing", b, sizeof b);
}
static int manual_runs(uint8_t slot, uint64_t seq)
{
    char id[64];
    snprintf(id, sizeof id, "m%u:%llu", slot, (unsigned long long)seq);
    return runs_of(id);
}
static op_t op_service_required(void)
{
    op_t o = mk(OP_READINESS);
    o.readiness = DSP_READINESS_SERVICE_REQUIRED;
    return o;
}
/* After a power loss: power-on boot (recovery), then a session that re-establishes the time. */
static void reboot_and_trust(void)
{
    reboot_power_on();
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    DSP_CHECK(!rep.degraded);
    advance(g_true + 2 * MINUTE); /* past the minimum interval of any earlier start */
}
/* No in-flight marker and no PENDING result outside a manual dose call. */
static void check_quiescent(void)
{
    dsp_dosing_state_t d = durable_dosing_state();
    DSP_CHECK(!(d.flags & DSP_DOSING_F_MANUAL_ACTIVE));
    DSP_CHECK_EQ_INT(0, d.marker_req);
    for (int i = 0; i < DSP_MANUAL_RING; i++) {
        DSP_CHECK(d.ring[i].seq == 0 || d.ring[i].state != DSP_MRES_PENDING);
    }
}

/* ==== Basic decisions, duplicates, invalid requests, floor ================================== */

static void test_accept_duplicate_invalid_floor(void)
{
    provision(true);
    advance(g_true + 2 * MINUTE);
    clear_script();
    int f0 = step(g_true, op_floor());
    int a = step(g_true, op_manual(1, VOL));
    int dup = step(g_true, op_manual(1, VOL));
    int dup_other_volume = step(g_true, op_manual(1, 2 * VOL));
    int r1 = step(g_true, op_result(1));
    int r2 = step(g_true, op_result(2));
    int f1 = step(g_true, op_floor());
    int z = step(g_true, op_manual(0, VOL));
    (void)step(g_true, op_manual((uint64_t)INT64_MAX + 1u, VOL)); /* checked in the z..v0 loop */
    int v0 = step(g_true, op_manual(2, 0));
    int rz = step(g_true, op_result(0));
    int lim = step(g_true, op_manual(2, VOL));               /* within the 60 s interval */
    int lim_retry = step(g_true + 2 * MINUTE, op_manual(2, VOL)); /* conditions changed */
    int ok3 = step(g_true + 2 * MINUTE, op_manual(3, VOL));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[f0].r.status);
    DSP_CHECK_EQ_INT(1, sc.res[f0].floor);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[a].r.status);
    DSP_CHECK_EQ_INT(0, sc.res[a].manual.outcome);
    DSP_CHECK_EQ_INT(VOL, sc.res[a].manual.volume_ul);
    DSP_CHECK_EQ_INT(DSP_MRES_DONE, sc.res[a].manual.state);
    DSP_CHECK_EQ_INT(2, sc.res[a].manual.floor);
    DSP_CHECK_EQ_INT(1, sc.res[a].pumps);
    DSP_CHECK_EQ_INT(DSP_OP_DUPLICATE, sc.res[dup].r.status);
    DSP_CHECK_EQ_INT(DSP_MRES_DONE, sc.res[dup].r.detail);
    DSP_CHECK_EQ_INT(0, sc.res[dup].pumps);
    DSP_CHECK_EQ_INT(0, sc.res[dup].writes);
    DSP_CHECK_EQ_INT(DSP_OP_DUPLICATE, sc.res[dup_other_volume].r.status);
    DSP_CHECK_EQ_INT(DSP_MRES_DONE, sc.res[r1].state);
    DSP_CHECK_EQ_INT(VOL, sc.res[r1].detail);
    DSP_CHECK_EQ_INT(DSP_MRES_NOT_DECIDED, sc.res[r2].state);
    DSP_CHECK_EQ_INT(2, sc.res[f1].floor);
    for (int i = z; i <= v0; i++) {
        DSP_CHECK_EQ_INT(DSP_OP_INVALID, sc.res[i].r.status);
        DSP_CHECK_EQ_INT(0, sc.res[i].writes);
        DSP_CHECK_EQ_INT(0, sc.res[i].pumps);
    }
    DSP_CHECK_EQ_INT(DSP_OP_INVALID, sc.res[rz].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_LIMIT, sc.res[lim].r.status);
    DSP_CHECK(sc.res[lim].r.detail & DSP_DOSING_L_INTERVAL);
    DSP_CHECK_EQ_INT(1, sc.res[lim].writes); /* durable refusal: one dosing write */
    DSP_CHECK_EQ_INT(DSP_OP_DUPLICATE, sc.res[lim_retry].r.status);
    DSP_CHECK_EQ_INT(DSP_MRES_REFUSED, sc.res[lim_retry].r.detail);
    DSP_CHECK_EQ_INT(DSP_MREFUSE_LIMIT, sc.res[lim_retry].r.aux);
    DSP_CHECK_EQ_INT(0, sc.res[lim_retry].pumps);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[ok3].r.status);
    DSP_CHECK_EQ_INT(1, manual_runs(0, 1));
    DSP_CHECK_EQ_INT(0, manual_runs(0, 2));
    DSP_CHECK_EQ_INT(1, manual_runs(0, 3));
    DSP_CHECK_EQ_INT(3, durable_dosing_state().manual_hw[0]);
    check_quiescent();
    invariants();
}

/* A duplicate arriving while the pump of the original runs (re-entrant call from the pump):
 * already decided (PENDING), never a second run. */
static res_t during, during_stale;
static void probe_during(void)
{
    op_allows_pump = true;
    /* M1.10c.1: the pump suspended BLE, which ended the connection and its authorization */
    during_stale.r = dsp_app_manual_dose(&app, &issued, 7, VOL, &during_stale.manual);
    /* even a freshly authenticated duplicate (new connection) is already decided: PENDING */
    during.r = dsp_app_manual_dose(&app, issue_for(&OWNER), 7, VOL, &during.manual);
}
static void test_duplicate_before_during_after(void)
{
    provision(true);
    advance(g_true + 2 * MINUTE);
    memset(&during, 0, sizeof during);
    pump_hook = probe_during;
    clear_script();
    int a = step(g_true, op_manual(7, VOL));
    int after = step(g_true, op_manual(7, VOL));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[a].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, during_stale.r.status);
    DSP_CHECK_EQ_INT(DSP_OP_DUPLICATE, during.r.status);
    DSP_CHECK_EQ_INT(DSP_MRES_PENDING, during.r.detail);
    DSP_CHECK_EQ_INT(DSP_OP_DUPLICATE, sc.res[after].r.status);
    DSP_CHECK_EQ_INT(DSP_MRES_DONE, sc.res[after].r.detail);
    DSP_CHECK_EQ_INT(1, manual_runs(0, 7));
    /* "before": duplicates queued in one burst are processed one after another */
    clear_script();
    advance(g_true + 2 * MINUTE);
    int b[4];
    for (int i = 0; i < 4; i++) {
        b[i] = step(g_true, op_manual(8, VOL));
    }
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[b[0]].r.status);
    for (int i = 1; i < 4; i++) {
        DSP_CHECK_EQ_INT(DSP_OP_DUPLICATE, sc.res[b[i]].r.status);
    }
    DSP_CHECK_EQ_INT(1, manual_runs(0, 8));
    invariants();
}

/* ==== Lost response, reboot, ring eviction, gaps ========================================== */

static void test_lost_response_reboot_and_eviction(void)
{
    provision(true);
    advance(g_true + 2 * MINUTE);
    clear_script();
    step(g_true, op_manual(10, VOL)); /* the phone never sees this response */
    DSP_CHECK(session());
    reboot_and_trust();
    clear_script();
    step(g_true, op_now());
    int r = step(g_true, op_result(10));
    int retry = step(g_true, op_manual(10, VOL));
    int f = step(g_true, op_floor());
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_MRES_DONE, sc.res[r].state);
    DSP_CHECK_EQ_INT(DSP_OP_DUPLICATE, sc.res[retry].r.status);
    DSP_CHECK_EQ_INT(11, sc.res[f].floor);
    DSP_CHECK_EQ_INT(1, manual_runs(0, 10));
    /* Eight later refusals evict the result of 10: still never executed, UNAVAILABLE. */
    clear_script();
    step(g_true, op_service_required());
    for (uint64_t q = 11; q <= 18; q++) {
        step(g_true + 1, op_manual(q, VOL));
    }
    int ev = step(g_true + 2, op_manual(10, VOL));
    int evr = step(g_true + 2, op_result(10));
    int last = step(g_true + 2, op_result(18));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_DUPLICATE, sc.res[ev].r.status);
    DSP_CHECK_EQ_INT(DSP_MRES_UNAVAILABLE, sc.res[ev].r.detail);
    DSP_CHECK_EQ_INT(DSP_MRES_UNAVAILABLE, sc.res[evr].state);
    DSP_CHECK_EQ_INT(DSP_MRES_REFUSED, sc.res[last].state);
    DSP_CHECK_EQ_INT(DSP_MREFUSE_GATES, sc.res[last].detail);
    /* ... also after a reboot and once dosing is possible again. */
    reboot_and_trust();
    clear_script();
    step(g_true, op_now());
    step(g_true, op_ready());
    int ev2 = step(g_true + 1, op_manual(10, VOL));
    int ev3 = step(g_true + 1, op_manual(15, VOL));
    /* A gap: 100 is new, 50 was never seen but is below the floor now: never executed. */
    int g100 = step(g_true + 1, op_manual(100, VOL));
    int g50 = step(g_true + 2 * MINUTE, op_manual(50, VOL));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_DUPLICATE, sc.res[ev2].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_DUPLICATE, sc.res[ev3].r.status);
    DSP_CHECK_EQ_INT(DSP_MRES_REFUSED, sc.res[ev3].r.detail);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[g100].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_DUPLICATE, sc.res[g50].r.status);
    DSP_CHECK_EQ_INT(DSP_MRES_UNAVAILABLE, sc.res[g50].r.detail);
    DSP_CHECK_EQ_INT(1, manual_runs(0, 10));
    DSP_CHECK_EQ_INT(0, manual_runs(0, 15));
    DSP_CHECK_EQ_INT(0, manual_runs(0, 50));
    check_quiescent();
    invariants();
}

/* ==== Slots, exhaustion, authorization ======================================================= */

static void test_slots_exhaustion_and_authorization(void)
{
    provision(true);
    advance(g_true + 2 * MINUTE);
    dsp_dosing_state_t d = durable_dosing_state();
    d.manual_hw[2] = DSP_MANUAL_SEQ_MAX - 1u; /* slot 2 one request away from exhaustion */
    put_dosing_state(&d);
    clear_script();
    int s0 = step(g_true, op_manual(5, VOL));
    int s1 = step(g_true + 2 * MINUTE, as(op_manual(5, VOL), &OWNER1)); /* independent counter */
    int f0 = step(g_true + 2 * MINUTE, op_floor());
    int f1 = step(g_true + 2 * MINUTE, as(op_floor(), &OWNER1));
    int f3 = step(g_true + 2 * MINUTE, as(op_floor(), &OWNER3));
    static const dsp_app_auth_t OWNER2 = {DSP_APP_ROLE_OWNER, 2, 0, 0};
    int last = step(g_true + 4 * MINUTE, as(op_manual(DSP_MANUAL_SEQ_MAX, VOL), &OWNER2));
    int f2 = step(g_true + 4 * MINUTE, as(op_floor(), &OWNER2));
    int exh = step(g_true + 6 * MINUTE, as(op_manual(DSP_MANUAL_SEQ_MAX, VOL), &OWNER2));
    int exh2 = step(g_true + 6 * MINUTE, as(op_manual(1, VOL), &OWNER2));
    int over = step(g_true + 6 * MINUTE, as(op_manual(DSP_MANUAL_SEQ_MAX + 1u, VOL), &OWNER2));
    int bad[9];
    int nb = 0;
    const dsp_app_auth_t *who[] = {NULL, &NOBODY, &BAD_SLOT};
    for (int i = 0; i < 3; i++) {
        bad[nb++] = step(g_true + 6 * MINUTE, as(op_manual(900, VOL), who[i]));
        bad[nb++] = step(g_true + 6 * MINUTE, as(op_result(5), who[i]));
        bad[nb++] = step(g_true + 6 * MINUTE, as(op_floor(), who[i]));
    }
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[s0].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[s1].r.status);
    DSP_CHECK_EQ_INT(6, sc.res[f0].floor);
    DSP_CHECK_EQ_INT(6, sc.res[f1].floor);
    DSP_CHECK_EQ_INT(1, sc.res[f3].floor);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[last].r.status);
    DSP_CHECK(sc.res[f2].floor == DSP_MANUAL_SEQ_MAX + 1u); /* 2^63: nothing acceptable any more */
    DSP_CHECK_EQ_INT(DSP_OP_DUPLICATE, sc.res[exh].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_DUPLICATE, sc.res[exh2].r.status);
    DSP_CHECK_EQ_INT(DSP_MRES_UNAVAILABLE, sc.res[exh2].r.detail);
    DSP_CHECK_EQ_INT(DSP_OP_INVALID, sc.res[over].r.status);
    for (int i = 0; i < nb; i++) {
        DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[bad[i]].r.status);
        DSP_CHECK_EQ_INT(0, sc.res[bad[i]].writes);
        DSP_CHECK_EQ_INT(0, sc.res[bad[i]].pumps);
    }
    DSP_CHECK_EQ_INT(1, manual_runs(0, 5));
    DSP_CHECK_EQ_INT(1, manual_runs(1, 5));
    DSP_CHECK_EQ_INT(0, manual_runs(0, 900));
    d = durable_dosing_state();
    DSP_CHECK(d.manual_hw[2] == DSP_MANUAL_SEQ_MAX); /* never wraps */
    /* Outside a session: BUSY. The retired queue command is refused. */
    dsp_app_manual_result_t mr;
    uint64_t fl = 0;
    DSP_CHECK_EQ_INT(DSP_OP_BUSY, dsp_app_manual_dose(&app, &OWNER, 1000, VOL, &mr).status);
    DSP_CHECK_EQ_INT(DSP_OP_BUSY, dsp_app_manual_floor(&app, &OWNER, &fl).status);
    /* M1.10c.1: the queue command is gone; a MANUAL_DOSE protocol frame without a security
     * session is refused by the gate (UNAUTHENTICATED) and never reaches the Dosing Service. */
    clear_script();
    op_t fr = mk(OP_FRAME);
    const uint8_t md[8 + 12] = {1, 0, 0x40, 0, 0x34, 0x12, 12, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0xE8, 3, 0, 0};
    memcpy(fr.frame, md, sizeof md);
    fr.frame_len = sizeof md;
    long sets0 = kv.sets;
    int pumps0 = pump.calls;
    int rp = step(g_true, fr);
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[rp].r.status); /* posted */
    DSP_CHECK(sc.nresp >= 1);
    DSP_CHECK(sc.resp[0][2] == 0x40 && sc.resp[0][3] == (DSP_BLE_F_RESPONSE | DSP_BLE_F_ERROR) &&
              sc.resp[0][8] == DSP_BLE_E_UNAUTHENTICATED);
    DSP_CHECK_EQ_INT(pumps0, pump.calls);
    DSP_CHECK_EQ_INT(sets0, kv.sets); /* no decision, nothing written */
    invariants();
}

/* ==== Refusals followed by changed conditions ================================================ */

static void test_refusals_then_changed_conditions(void)
{
    provision(false); /* SERVICE_REQUIRED */
    clear_script();
    int r1 = step(g_true, op_manual(1, VOL));
    step(g_true, op_ready());
    int r1b = step(g_true + 1, op_manual(1, VOL));
    int ok = step(g_true + 1, op_manual(2, VOL));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_REFUSED, sc.res[r1].r.status);
    DSP_CHECK(sc.res[r1].r.detail & DSP_DOSING_G_NOT_READY);
    DSP_CHECK_EQ_INT(DSP_OP_DUPLICATE, sc.res[r1b].r.status);
    DSP_CHECK_EQ_INT(DSP_MRES_REFUSED, sc.res[r1b].r.detail);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[ok].r.status);
    /* Not covered, then refilled. */
    advance(g_true + 2 * MINUTE);
    clear_script();
    step(g_true, op_refill(VOL / 2));
    int nc = step(g_true + 1, op_manual(3, VOL));
    step(g_true + 2, op_refill(CAP));
    int nc2 = step(g_true + 3, op_manual(3, VOL));
    int ok4 = step(g_true + 3, op_manual(4, VOL));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_NOT_COVERED, sc.res[nc].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_DUPLICATE, sc.res[nc2].r.status);
    DSP_CHECK_EQ_INT(DSP_MREFUSE_NOT_COVERED, sc.res[nc2].r.aux);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[ok4].r.status);
    /* A pump fault ends dosing for the wake: the next request is refused DURABLY by the
     * Application Service (code DEGRADED) and never runs in a later wake either. */
    advance(g_true + 2 * MINUTE);
    pump_fail_next = true;
    clear_script();
    int pf = step(g_true, op_manual(5, VOL));
    int ar = step(g_true + 2 * MINUTE, op_manual(6, VOL));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[pf].r.status);
    DSP_CHECK_EQ_INT(1, sc.res[pf].manual.outcome); /* PUMP_FAILED */
    DSP_CHECK_EQ_INT(DSP_MRES_FAILED, sc.res[pf].manual.state);
    DSP_CHECK_EQ_INT(DSP_OP_REFUSED, sc.res[ar].r.status);
    DSP_CHECK_EQ_INT(DSP_MREFUSE_DEGRADED, sc.res[ar].r.aux);
    advance(g_true + 2 * MINUTE);
    clear_script();
    int ar2 = step(g_true, op_manual(6, VOL));
    int pf2 = step(g_true, op_manual(5, VOL));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_DUPLICATE, sc.res[ar2].r.status);
    DSP_CHECK_EQ_INT(DSP_MRES_REFUSED, sc.res[ar2].r.detail);
    DSP_CHECK_EQ_INT(DSP_OP_DUPLICATE, sc.res[pf2].r.status);
    DSP_CHECK_EQ_INT(DSP_MRES_FAILED, sc.res[pf2].r.detail);
    DSP_CHECK_EQ_INT(0, manual_runs(0, 6));
    DSP_CHECK_EQ_INT(1, manual_runs(0, 5));
    invariants();
}

/* ==== Storage failures ========================================================================== */

static void test_storage_failures_never_execute(void)
{
    /* Refusal write fails: STORAGE, nothing durable; the next wake decides it again. */
    provision(false);
    clear_script();
    strcpy(fail_set_key, "dosing");
    int a = step(g_true, op_manual(1, VOL));
    DSP_CHECK(session());
    fail_set_key[0] = '\0';
    DSP_CHECK_EQ_INT(DSP_OP_STORAGE, sc.res[a].r.status);
    DSP_CHECK_EQ_INT(0, durable_dosing_state().manual_hw[0]);
    clear_script();
    int a2 = step(g_true, op_manual(1, VOL));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_REFUSED, sc.res[a2].r.status); /* still SERVICE_REQUIRED */
    /* Decision write (write 1) fails: no consumption, no pump; the next wake executes it once. */
    provision(true);
    advance(g_true + 2 * MINUTE);
    uint32_t remaining = durable_cfg().reservoir_remaining_ul;
    clear_script();
    strcpy(fail_set_key, "dosing");
    int b = step(g_true, op_manual(2, VOL));
    int b_same_wake = step(g_true + 1, op_manual(3, VOL));
    DSP_CHECK(session());
    fail_set_key[0] = '\0';
    DSP_CHECK_EQ_INT(DSP_OP_STORAGE, sc.res[b].r.status);
    DSP_CHECK_EQ_INT(0, sc.res[b].pumps);
    DSP_CHECK_EQ_INT(DSP_OP_DEGRADED, sc.res[b_same_wake].r.status); /* nothing durable now */
    DSP_CHECK_EQ_INT(remaining, durable_cfg().reservoir_remaining_ul);
    clear_script();
    int b2 = step(g_true, op_manual(2, VOL));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[b2].r.status);
    DSP_CHECK_EQ_INT(1, manual_runs(0, 2));
    /* Consumption write (write 2) fails after the decision: no pump; recovery -> INTERRUPTED. */
    advance(g_true + 2 * MINUTE);
    clear_script();
    strcpy(fail_set_key, "config");
    int c = step(g_true, op_manual(4, VOL));
    DSP_CHECK(session());
    fail_set_key[0] = '\0';
    DSP_CHECK_EQ_INT(DSP_OP_STORAGE, sc.res[c].r.status);
    DSP_CHECK_EQ_INT(0, sc.res[c].pumps);
    DSP_CHECK_EQ_INT(DSP_MRES_PENDING, sc.res[c].manual.state);
    reboot_and_trust();
    check_quiescent();
    clear_script();
    step(g_true, op_now());
    int c2 = step(g_true, op_manual(4, VOL));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_DUPLICATE, sc.res[c2].r.status);
    DSP_CHECK_EQ_INT(DSP_MRES_INTERRUPTED, sc.res[c2].r.detail);
    DSP_CHECK_EQ_INT(0, manual_runs(0, 4));
    invariants();
}

/* ==== Power-loss matrix ========================================================================== */

/* Cuts at every write of an accepted manual dose (landed or not) and during the pump. */
static void test_power_loss_accepted_request(void)
{
    /* Reference run: the number of writes of an uninterrupted accepted request. */
    provision(true);
    advance(g_true + 2 * MINUTE);
    clear_script();
    int ref = step_armed(g_true, op_manual(41, VOL));
    DSP_CHECK(session());
    long writes = sc.res[ref].writes;
    DSP_CHECK(writes >= 3); /* decision, consumption, completion */
    int cases = 0, executed_before = 0, executed_on_retry = 0, decided = 0;
    for (int variant = 0; variant < 3; variant++) {       /* 0: write not landed, 1: landed, 2: pump */
        for (long k = 1; k <= (variant == 2 ? 1 : 12); k++) {
            provision(true);
            advance(g_true + 2 * MINUTE);
            clear_script();
            step_armed(g_true, op_manual(42, VOL));
            cut_at = variant == 2 ? 0 : k;
            cut_after = variant == 1;
            pump_cut = variant == 2;
            bool finished = session();
            cut_at = 0;
            pump_cut = false;
            if (finished) {
                break; /* fewer writes than k */
            }
            cases++;
            int before = manual_runs(0, 42);
            executed_before += before;
            dsp_dosing_state_t d = durable_dosing_state();
            bool was_decided = d.manual_hw[0] == 42;
            decided += was_decided;
            reboot_and_trust();
            check_quiescent(); /* recovery cleared any marker and finished the ring entry */
            clear_script();
            step(g_true, op_now());
            int r = step(g_true, op_result(42));
            int retry = step(g_true, op_manual(42, VOL));
            int retry2 = step(g_true + 1, op_manual(42, VOL));
            DSP_CHECK(session());
            int after = manual_runs(0, 42) - before;
            executed_on_retry += after;
            DSP_CHECK(manual_runs(0, 42) <= 1); /* at most once, whatever happened */
            if (was_decided) {
                DSP_CHECK_EQ_INT(DSP_OP_DUPLICATE, sc.res[retry].r.status);
                DSP_CHECK_EQ_INT(0, after);
                uint8_t st = sc.res[r].state;
                DSP_CHECK(st == DSP_MRES_INTERRUPTED || st == DSP_MRES_DONE);
                /* interrupted before the marker was cleared -> INTERRUPTED; never PENDING */
            } else {
                DSP_CHECK_EQ_INT(0, before); /* nothing was decided, so nothing ran */
                DSP_CHECK_EQ_INT(DSP_MRES_NOT_DECIDED, sc.res[r].state);
                DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[retry].r.status);
                DSP_CHECK_EQ_INT(1, after);
            }
            DSP_CHECK_EQ_INT(DSP_OP_DUPLICATE, sc.res[retry2].r.status);
            invariants();
        }
    }
    printf("  power loss, accepted request: %ld writes, %d cuts (%d with the decision durable; "
           "pump runs before the cut %d, on retry %d)\n", writes, cases, decided, executed_before,
           executed_on_retry);
    DSP_CHECK_EQ_INT(2 * writes + 1, cases); /* every write, landed or not, plus the pump */
}

/* Cuts at the refusal write: either nothing durable (decided again later) or REFUSED for good. */
static void test_power_loss_refusal(void)
{
    for (int landed = 0; landed < 2; landed++) {
        provision(false);
        clear_script();
        step_armed(g_true, op_manual(9, VOL));
        cut_at = 1;
        cut_after = landed != 0;
        DSP_CHECK(!session());
        cut_at = 0;
        reboot_and_trust();
        DSP_CHECK_EQ_INT(landed ? 9 : 0, durable_dosing_state().manual_hw[0]);
        clear_script();
        step(g_true, op_now());
        step(g_true, op_ready()); /* conditions change: dosing possible now */
        int retry = step(g_true + 1, op_manual(9, VOL));
        DSP_CHECK(session());
        if (landed) {
            DSP_CHECK_EQ_INT(DSP_OP_DUPLICATE, sc.res[retry].r.status);
            DSP_CHECK_EQ_INT(DSP_MRES_REFUSED, sc.res[retry].r.detail);
            DSP_CHECK_EQ_INT(0, manual_runs(0, 9));
        } else {
            DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[retry].r.status); /* never decided before */
            DSP_CHECK_EQ_INT(1, manual_runs(0, 9));
        }
        invariants();
    }
}

/* ==== Format 1 -> 2 upgrade, corrupt and unsupported records ==================================== */

static void put_format1(const dsp_dosing_state_t *d)
{
    uint8_t b[DSP_DOSING_RECORD_BYTES];
    DSP_CHECK_EQ_INT(DSP_DOSING_RECORD_BYTES, dsp_codec_encode_dosing(d, b, sizeof b));
    b[1] = 1;
    fake_kv_put_raw(&kv, "dosing", b, DSP_DOSING_RECORD_V1_BYTES);
}

static void test_format1_upgrade_with_interruptions(void)
{
    for (int accepted = 0; accepted < 2; accepted++) {
        for (int landed = 0; landed < 2; landed++) {
            provision(true);
            clear_script();
            step(g_true, op_create(sched("up", 7, 10, 1000)));
            DSP_CHECK(session());
            run_until(local(date_of(T0), 7, 12)); /* ledger and last start are set */
            dsp_dosing_state_t before = durable_dosing_state();
            DSP_CHECK(before.ledger_sum_ul > 0 && before.last_start_utc != 0);
            if (!accepted) {
                before.flags |= DSP_DOSING_F_BLOCKED; /* manual doses refused while BLOCKED */
            }
            put_format1(&before);
            reboot_and_trust();
            dsp_dosing_state_t f1 = durable_dosing_state();
            DSP_CHECK(dsp_dosing_state_equal(&before, &f1)); /* format 1 decoded, zeros */
            DSP_CHECK_EQ_INT(DSP_DOSING_RECORD_V1_BYTES, fake_kv_find(&kv, "dosing")->len);
            clear_script();
            step(g_true, op_now());
            step_armed(g_true, op_manual(3, VOL));
            cut_at = 1; /* the first write of the request: the format upgrade */
            cut_after = landed != 0;
            DSP_CHECK(!session());
            cut_at = 0;
            const fake_kv_entry_t *e = fake_kv_find(&kv, "dosing");
            DSP_CHECK(e->len == (landed ? DSP_DOSING_RECORD_BYTES : DSP_DOSING_RECORD_V1_BYTES));
            reboot_and_trust();
            dsp_dosing_state_t now = durable_dosing_state();
            /* never lost or reinterpreted: readiness, BLOCKED latch, refill intent */
            DSP_CHECK_EQ_INT(before.readiness, now.readiness);
            DSP_CHECK_EQ_INT(before.flags & DSP_DOSING_F_BLOCKED, now.flags & DSP_DOSING_F_BLOCKED);
            DSP_CHECK_EQ_INT(before.refill_seq, now.refill_seq);
            if (!accepted || !landed) {
                DSP_CHECK_EQ_INT(before.ledger_sum_ul, now.ledger_sum_ul);
                DSP_CHECK_EQ_INT(before.last_start_utc, now.last_start_utc);
            }
            DSP_CHECK_EQ_INT(landed ? 3 : 0, now.manual_hw[0]);
            check_quiescent();
            clear_script();
            step(g_true, op_now());
            int retry = step(g_true + 2 * MINUTE, op_manual(3, VOL));
            DSP_CHECK(session());
            DSP_CHECK_EQ_INT(landed ? DSP_OP_DUPLICATE : (accepted ? DSP_OP_OK : DSP_OP_REFUSED),
                             sc.res[retry].r.status);
            DSP_CHECK(manual_runs(0, 3) <= 1);
            DSP_CHECK_EQ_INT(DSP_DOSING_RECORD_BYTES, fake_kv_find(&kv, "dosing")->len);
            invariants();
        }
    }
    /* A format-1 record with an active manual marker (no request binding) recovers, upgrades. */
    provision(true);
    dsp_dosing_state_t m = durable_dosing_state();
    m.flags |= DSP_DOSING_F_MANUAL_ACTIVE;
    m.manual_volume_ul = 700;
    m.manual_started_at = raw();
    m.manual_seq = m.next_seq++;
    put_format1(&m);
    reboot_and_trust();
    check_quiescent();
    DSP_CHECK_EQ_INT(DSP_DOSING_RECORD_BYTES, fake_kv_find(&kv, "dosing")->len);
    for (int i = 0; i < DSP_MANUAL_RING; i++) {
        DSP_CHECK_EQ_INT(0, durable_dosing_state().ring[i].seq); /* no invented result */
    }
    clear_script();
    step(g_true, op_now());
    int ok = step(g_true, op_manual(1, VOL));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[ok].r.status);
    invariants();
}

static void test_corrupt_and_unsupported_dosing_records(void)
{
    provision(true);
    dsp_dosing_state_t base = durable_dosing_state();
    base.manual_hw[0] = 5;
    base.ring[0] = (dsp_manual_result_t){0, DSP_MRES_DONE, 5, VOL};
    base.ring_next = 1;
    uint8_t good[DSP_DOSING_RECORD_BYTES];
    DSP_CHECK_EQ_INT(DSP_DOSING_RECORD_BYTES, dsp_codec_encode_dosing(&base, good, sizeof good));
    dsp_dosing_state_t back;
    DSP_CHECK_EQ_INT(DSP_CODEC_OK, dsp_codec_decode_dosing(good, sizeof good, &back));
    DSP_CHECK(dsp_dosing_state_equal(&base, &back));
    /* Golden layout of format 3 (= format 2 + admission; docs: dsp_codec.h, README). */
    {
        dsp_dosing_state_t g = base;
        g.manual_hw[1] = 0x0102030405060708ull;
        g.manual_hw[3] = DSP_MANUAL_SEQ_MAX;
        g.ring[7] = (dsp_manual_result_t){3, DSP_MRES_REFUSED, 0x0E0F1011ull, 9};
        uint8_t b[DSP_DOSING_RECORD_BYTES];
        g.adm_date = (dsp_date_t){2026, 11, 20};
        g.adm_count = 0x0102;
        DSP_CHECK_EQ_INT(212, dsp_codec_encode_dosing(&g, b, sizeof b));
        static const uint8_t hw1[8] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
        static const uint8_t hw3[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F};
        DSP_CHECK_EQ_INT(DSP_REC_TYPE_DOSING, b[0]);
        DSP_CHECK_EQ_INT(3, b[1]);
        DSP_CHECK(b[206] == 0xEA && b[207] == 0x07 && b[208] == 11 && b[209] == 20); /* 2026-11-20 */
        DSP_CHECK(b[210] == 0x02 && b[211] == 0x01);                                 /* count */
        DSP_CHECK_EQ_INT(5, b[52]); /* manual_hw[0] = 5 */
        DSP_CHECK(memcmp(b + 60, hw1, 8) == 0);
        DSP_CHECK(memcmp(b + 76, hw3, 8) == 0);
        DSP_CHECK_EQ_INT(1, b[85]); /* ring_next */
        const uint8_t *e0 = b + 94, *e7 = b + 94 + 7 * 14;
        DSP_CHECK(e0[0] == 0 && e0[1] == DSP_MRES_DONE && e0[2] == 5 && e0[10] == 0xE8 && e0[11] == 0x03);
        DSP_CHECK(e7[0] == 3 && e7[1] == DSP_MRES_REFUSED && e7[2] == 0x11 && e7[3] == 0x10 &&
                  e7[4] == 0x0F && e7[5] == 0x0E && e7[10] == 9);
        DSP_CHECK_EQ_INT(DSP_CODEC_OK, dsp_codec_decode_dosing(b, sizeof b, &back));
        DSP_CHECK(dsp_dosing_state_equal(&g, &back));
        /* format 2 = the first 206 bytes with version 2: admission counter 0 (upgrade path) */
        b[1] = 2;
        DSP_CHECK_EQ_INT(DSP_CODEC_OK, dsp_codec_decode_dosing(b, DSP_DOSING_RECORD_V2_BYTES, &back));
        DSP_CHECK_EQ_INT(0, back.adm_count);
        DSP_CHECK_EQ_INT(0, back.adm_date.year);
        DSP_CHECK_EQ_INT(0x0102030405060708ull == back.manual_hw[1], 1);
        DSP_CHECK_EQ_INT(DSP_CODEC_ERR_SIZE, dsp_codec_decode_dosing(b, sizeof b, &back));
        b[1] = 3;
        DSP_CHECK_EQ_INT(DSP_CODEC_ERR_SIZE, dsp_codec_decode_dosing(b, DSP_DOSING_RECORD_V2_BYTES, &back));
        /* an invalid admission date is refused */
        b[208] = 13;
        DSP_CHECK_EQ_INT(DSP_CODEC_ERR_FIELD, dsp_codec_decode_dosing(b, sizeof b, &back));
        b[208] = 11;
        /* format 1 = the first 52 bytes with version 1, valid only without anti-replay state */
        b[1] = 1;
        DSP_CHECK_EQ_INT(DSP_CODEC_OK, dsp_codec_decode_dosing(b, DSP_DOSING_RECORD_V1_BYTES, &back));
        DSP_CHECK_EQ_INT(0, back.manual_hw[1]);
        DSP_CHECK_EQ_INT(0, back.ring[7].seq);
        DSP_CHECK_EQ_INT(DSP_CODEC_ERR_SIZE, dsp_codec_decode_dosing(b, sizeof b, &back));
    }
    /* Invalid states the encoder refuses (the decoder applies the same validation). */
    dsp_dosing_state_t x;
    x = base; x.manual_hw[1] = DSP_MANUAL_SEQ_MAX + 1u;
    DSP_CHECK_EQ_INT(0, dsp_codec_encode_dosing(&x, good, sizeof good));
    x = base; x.ring[0].seq = 6; /* result above the high-water mark */
    DSP_CHECK_EQ_INT(0, dsp_codec_encode_dosing(&x, good, sizeof good));
    x = base; x.ring[0].state = DSP_MRES_PENDING; /* PENDING without the marker */
    DSP_CHECK_EQ_INT(0, dsp_codec_encode_dosing(&x, good, sizeof good));
    x = base; x.ring[1] = x.ring[0]; /* duplicate result */
    DSP_CHECK_EQ_INT(0, dsp_codec_encode_dosing(&x, good, sizeof good));
    x = base; x.marker_req = 5; x.marker_slot = 0; /* marker request without an active marker */
    DSP_CHECK_EQ_INT(0, dsp_codec_encode_dosing(&x, good, sizeof good));
    x = base; x.ring_next = DSP_MANUAL_RING;
    DSP_CHECK_EQ_INT(0, dsp_codec_encode_dosing(&x, good, sizeof good));
    x = base; x.ring[2].slot = 1; /* bytes in an unused entry */
    DSP_CHECK_EQ_INT(0, dsp_codec_encode_dosing(&x, good, sizeof good));
    /* Byte-level corruption of a stored record: storage reports it, never "fresh". */
    DSP_CHECK_EQ_INT(DSP_DOSING_RECORD_BYTES, dsp_codec_encode_dosing(&base, good, sizeof good));
    const size_t offs[] = {52 + 7 /* hw[0] top byte -> > 2^63 */, 84 /* marker_slot */,
                           85 /* ring_next */, 94 + 1 /* ring state */, 94 + 14 /* unused entry */};
    for (size_t i = 0; i < sizeof offs / sizeof offs[0]; i++) {
        uint8_t bad[DSP_DOSING_RECORD_BYTES];
        memcpy(bad, good, sizeof bad);
        bad[offs[i]] = 0x80;
        DSP_CHECK_EQ_INT(DSP_CODEC_ERR_FIELD, dsp_codec_decode_dosing(bad, sizeof bad, &back));
        fake_kv_put_raw(&kv, "dosing", bad, sizeof bad);
        reboot_power_on();
        DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
        DSP_CHECK(rep.degraded);
        DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_CORRUPT, rep.open_status);
        DSP_CHECK(memcmp(fake_kv_find(&kv, "dosing")->value, bad, sizeof bad) == 0); /* untouched */
        clear_script();
        int r = step(g_true, op_manual(77, VOL));
        DSP_CHECK(session());
        /* storage unreadable: no credentials, no authorization (M1.10c.1) */
        DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[r].r.status);
        DSP_CHECK_EQ_INT(0, sc.res[r].pumps);
    }
    /* A newer format: unsupported, fail closed, untouched. */
    uint8_t v3[DSP_DOSING_RECORD_BYTES];
    memcpy(v3, good, sizeof v3);
    v3[1] = 4; /* format 4: newer than this firmware (3, M1.10b.3) */
    DSP_CHECK_EQ_INT(DSP_CODEC_ERR_VERSION, dsp_codec_decode_dosing(v3, sizeof v3, &back));
    fake_kv_put_raw(&kv, "dosing", v3, sizeof v3);
    reboot_power_on();
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    DSP_CHECK_EQ_INT(DSP_STORAGE_ERR_UNSUPPORTED, rep.open_status);
    clear_script();
    int r3 = step(g_true, op_manual(78, VOL));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, sc.res[r3].r.status);
    DSP_CHECK_EQ_INT(0, sc.res[r3].pumps);
    invariants();
}

/* ==== BLE suspension around the manual pump and the session deadline ========================== */

static void test_manual_dose_suspends_ble_and_respects_deadline(void)
{
    provision(true);
    advance(g_true + 2 * MINUTE);
    clear_script();
    int a = step(g_true + 10, op_manual(1, VOL));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[a].r.status);
    DSP_CHECK_EQ_INT(1, sc.suspends);
    DSP_CHECK_EQ_INT(1, sc.resumes);
    DSP_CHECK_EQ_INT(DSP_BLE_END_COMMAND, rep.ble_end);
    /* A dose that ends after the 15-minute maximum: no resume, the session ends (MAX). */
    advance(g_true + 2 * MINUTE);
    sc.suspends = sc.resumes = 0;
    clear_script();
    uint64_t start = mono_ms;
    int b = step(g_true + 899, op_manual(2, 3000)); /* 2.5 s pump run, crosses 900 s */
    step(g_true + 950, op_list(0));                 /* never reached: the session is over */
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[b].r.status);
    DSP_CHECK_EQ_INT(1, sc.suspends);
    DSP_CHECK_EQ_INT(0, sc.resumes);
    DSP_CHECK_EQ_INT(DSP_BLE_END_MAX, rep.ble_end);
    DSP_CHECK(mono_ms - start < 960000u);
    DSP_CHECK(!sc.res[sc.len - 2].ran); /* the later step never ran */
    /* Already decided and invalid requests never touch BLE. */
    sc.suspends = sc.resumes = 0;
    clear_script();
    step(g_true, op_manual(2, 3000));
    step(g_true, op_manual(0, 3000));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(0, sc.suspends);
    invariants();
}

int main(void)
{
    DSP_RUN(test_accept_duplicate_invalid_floor);
    DSP_RUN(test_duplicate_before_during_after);
    DSP_RUN(test_lost_response_reboot_and_eviction);
    DSP_RUN(test_slots_exhaustion_and_authorization);
    DSP_RUN(test_refusals_then_changed_conditions);
    DSP_RUN(test_storage_failures_never_execute);
    DSP_RUN(test_power_loss_accepted_request);
    DSP_RUN(test_power_loss_refusal);
    DSP_RUN(test_format1_upgrade_with_interruptions);
    DSP_RUN(test_corrupt_and_unsupported_dosing_records);
    DSP_RUN(test_manual_dose_suspends_ble_and_respects_deadline);
    return dsp_test_finish();
}
