/*
 * Phase C: the user-visible event history through the Application Service (complete wakes on the
 * shared harness of test_app_ops.c): scheduled and manual doses (completed, pump failure,
 * interrupted by a power loss), reservoir initialization / refill / adjustment, persistence across
 * reboots, no duplicates from repeated recovery, GET_HISTORY paging and authorization, and the
 * safety rule that a failing history never blocks or authorizes a dose.
 */
#define DSP_APP_OPS_NO_MAIN
#include "test_app_ops.c"

#include "dsp_evlog.h"

/* The durable history as a fresh boot would see it (independent reader over the same flash). */
static size_t durable_history(dsp_event_t *out, size_t cap)
{
    static dsp_evlog_t rd;
    DSP_CHECK(dsp_evlog_open(&rd, &kv.iface));
    size_t n = 0;
    uint32_t before = 0;
    while (n < cap) {
        size_t got = dsp_evlog_read(&rd, before, out + n, cap - n < 4 ? cap - n : 4);
        if (got == 0) {
            break;
        }
        before = out[n + got - 1].seq;
        n += got;
    }
    return n;
}

static int count_type(const dsp_event_t *e, size_t n, uint8_t type, uint8_t result)
{
    int c = 0;
    for (size_t i = 0; i < n; i++) {
        c += e[i].type == type && e[i].result == result;
    }
    return c;
}

static void reboot_and_trust(void)
{
    reboot_power_on();
    DSP_CHECK(wake(DSP_CAUSE_POWER_ON));
    DSP_CHECK(!rep.degraded);
    advance(g_true + 2 * MINUTE);
}

static dsp_schedule_t once_at(const char *id, dsp_utc_t at_local_day, int h, int m, uint32_t vol)
{
    dsp_schedule_t s = sched(id, h, m, vol);
    s.kind = DSP_REC_ONCE;
    s.start_date = date_of(at_local_day);
    return s;
}

static op_t op_readiness(uint8_t r)
{
    op_t o = mk(OP_READINESS);
    o.readiness = r;
    return o;
}
static op_t op_refill_cap(uint32_t capacity, uint32_t remaining)
{
    op_t o = mk(OP_REFILL);
    o.capacity = capacity;
    o.remaining = remaining;
    return o;
}

/* ---- GET_HISTORY through the operation (inside a session, via OP_HOOK) -------------------- */

static dsp_app_history_page_t hist_pages[8];
static dsp_app_op_result_t hist_res[8];
static void hook_history(int i)
{
    const op_t *o = &sc.op[i];
    int k = o->a;
    const dsp_app_auth_t *auth = o->b == 1 ? &NOBODY : o->b == 2 ? &BAD_SLOT : issue_for(&OWNER);
    uint32_t before = k > 0 && hist_res[k - 1].status == DSP_OP_OK && hist_pages[k - 1].n > 0
                          ? hist_pages[k - 1].items[hist_pages[k - 1].n - 1].seq
                          : 0;
    uint8_t max = o->b == 3 ? 0 : o->b == 4 ? 5 : 4;
    hist_res[k] = dsp_app_get_history(&app, auth, before, max, &hist_pages[k]);
}
static op_t op_history(int k, int variant)
{
    op_t o = mk(OP_HOOK);
    o.hook = hook_history;
    o.a = k;
    o.b = variant;
    return o;
}

static void test_doses_and_reservoir_are_logged(void)
{
    provision(true); /* calibration (capacity 0 -> CAP) and REFILL (0 -> CAP) */
    dsp_event_t ev[DSP_EVLOG_SLOTS];
    size_t n = durable_history(ev, DSP_EVLOG_SLOTS);
    /* Newest first: REFILL (same capacity, more remaining), then INIT (capacity from 0). */
    DSP_CHECK_EQ_INT(2, n);
    DSP_CHECK_EQ_INT(DSP_EV_RESERVOIR_REFILL, ev[0].type);
    DSP_CHECK_EQ_INT(0, ev[0].before_ul);
    DSP_CHECK_EQ_INT(CAP, ev[0].after_ul);
    DSP_CHECK_EQ_INT(CAP, ev[0].capacity_ul);
    DSP_CHECK(ev[0].flags & DSP_EVF_CLOCK_TRUSTED);
    DSP_CHECK_EQ_INT(DSP_EV_RESERVOIR_INIT, ev[1].type);
    DSP_CHECK_EQ_INT(CAP, ev[1].capacity_ul);
    DSP_CHECK(ev[0].seq > ev[1].seq);

    /* A scheduled dose: COMPLETED with the estimate before/after and the schedule. */
    clear_script();
    step(g_true, op_create(once_at("dose-a", T0, 8, 0, 5000)));
    DSP_CHECK(session());
    run_until(T0 + 5 * MINUTE);
    DSP_CHECK_EQ_INT(1, pump.calls);
    n = durable_history(ev, DSP_EVLOG_SLOTS);
    DSP_CHECK_EQ_INT(3, n);
    DSP_CHECK_EQ_INT(DSP_EV_DOSE_SCHEDULED, ev[0].type);
    DSP_CHECK_EQ_INT(DSP_EVR_OK, ev[0].result);
    DSP_CHECK_EQ_STR("dose-a", ev[0].schedule_id);
    DSP_CHECK_EQ_INT(5000, ev[0].volume_ul);
    DSP_CHECK_EQ_INT(CAP, ev[0].before_ul);
    DSP_CHECK_EQ_INT(CAP - 5000, ev[0].after_ul);
    DSP_CHECK_EQ_INT(T0, ev[0].at);
    DSP_CHECK(ev[0].flags & DSP_EVF_CLOCK_TRUSTED);
    DSP_CHECK(!(ev[0].flags & DSP_EVF_RESERVOIR_UNKNOWN));
    DSP_CHECK_EQ_INT(0, ev[0].flags & (DSP_EVF_PUMP_SIMULATED | DSP_EVF_PUMP_BENCH_GPIO));

    /* A pump fault: FAILED (volume counted, conservative accounting). */
    clear_script();
    step(g_true, op_create(once_at("dose-b", T0, 9, 0, 2000)));
    DSP_CHECK(session());
    pump_fail_next = true;
    run_until(T0 + HOUR + 5 * MINUTE);
    n = durable_history(ev, DSP_EVLOG_SLOTS);
    DSP_CHECK_EQ_INT(DSP_EV_DOSE_SCHEDULED, ev[0].type);
    DSP_CHECK_EQ_INT(DSP_EVR_PUMP_FAILED, ev[0].result);
    DSP_CHECK_EQ_STR("dose-b", ev[0].schedule_id);
    DSP_CHECK_EQ_INT(CAP - 5000, ev[0].before_ul);
    DSP_CHECK_EQ_INT(CAP - 7000, ev[0].after_ul);

    /* A manual dose (next wake: a pump fault ends dosing for its wake) and a refill. */
    advance(T0 + 2 * HOUR);
    clear_script();
    int md = step(g_true, op_manual(1, 1000));
    int rf = step(g_true + 1, op_refill(CAP));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[md].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[rf].r.status);
    n = durable_history(ev, DSP_EVLOG_SLOTS);
    DSP_CHECK_EQ_INT(6, n);
    DSP_CHECK_EQ_INT(DSP_EV_RESERVOIR_REFILL, ev[0].type);
    DSP_CHECK_EQ_INT(CAP - 8000, ev[0].before_ul);
    DSP_CHECK_EQ_INT(CAP, ev[0].after_ul);
    DSP_CHECK_EQ_INT(DSP_EV_DOSE_MANUAL, ev[1].type);
    DSP_CHECK_EQ_INT(DSP_EVR_OK, ev[1].result);
    DSP_CHECK_EQ_INT(1000, ev[1].volume_ul);
    DSP_CHECK_EQ_INT(CAP - 7000, ev[1].before_ul);
    DSP_CHECK_EQ_INT(CAP - 8000, ev[1].after_ul);
    DSP_CHECK_EQ_STR("", ev[1].schedule_id);

    /* Manual adjustment: capacity changed through SET_CONFIG (NOT READY for the capacity rule). */
    clear_script();
    step(g_true, op_readiness(DSP_READINESS_SERVICE_REQUIRED));
    dsp_config_t c = calibration();
    c.reservoir_capacity_ul = 2 * CAP;
    int sc1 = step(g_true, op_set_cfg_cur(DSP_CONFIG_F_RESERVOIR_CAPACITY, c));
    int rf2 = step(g_true + 1, op_refill_cap(2 * CAP, 50000));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[sc1].r.status);
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[rf2].r.status);
    n = durable_history(ev, DSP_EVLOG_SLOTS);
    DSP_CHECK_EQ_INT(8, n);
    DSP_CHECK_EQ_INT(DSP_EV_RESERVOIR_ADJUST, ev[0].type); /* remaining set lower: adjustment */
    DSP_CHECK_EQ_INT(CAP, ev[0].before_ul);
    DSP_CHECK_EQ_INT(50000, ev[0].after_ul);
    DSP_CHECK_EQ_INT(DSP_EV_RESERVOIR_ADJUST, ev[1].type); /* capacity change */
    DSP_CHECK_EQ_INT(2 * CAP, ev[1].capacity_ul);

    /* Persistence: the same history after reboots; seqs strictly decreasing, no duplicates. */
    dsp_event_t before_reboot[DSP_EVLOG_SLOTS];
    memcpy(before_reboot, ev, sizeof ev);
    reboot_and_trust();
    reboot_and_trust();
    size_t n2 = durable_history(ev, DSP_EVLOG_SLOTS);
    DSP_CHECK_EQ_INT(n, n2);
    for (size_t i = 0; i < n; i++) {
        DSP_CHECK_EQ_INT(before_reboot[i].seq, ev[i].seq);
        DSP_CHECK_EQ_INT(before_reboot[i].type, ev[i].type);
        if (i > 0) {
            DSP_CHECK(ev[i].seq < ev[i - 1].seq);
        }
    }
    invariants();
}

static void cut_during_pump(void)
{
    power_lost();
}

static void test_interrupted_doses_are_logged_once(void)
{
    provision(true);
    clear_script();
    step(g_true, op_create(once_at("cut", T0, 8, 0, 3000)));
    DSP_CHECK(session());
    /* The power goes during the automatic pump run. */
    advance(T0);
    pump_hook = cut_during_pump;
    DSP_CHECK(!wake(DSP_CAUSE_TIMER));
    DSP_CHECK_EQ_INT(1, pump.calls);
    dsp_event_t ev[DSP_EVLOG_SLOTS];
    size_t n = durable_history(ev, DSP_EVLOG_SLOTS);
    DSP_CHECK_EQ_INT(0, count_type(ev, n, DSP_EV_DOSE_SCHEDULED, DSP_EVR_INTERRUPTED));
    /* Boot recovery logs it as INTERRUPTED (claimed volume, estimate unknown), exactly once even
     * after repeated reboots; the occurrence never runs again. */
    reboot_and_trust();
    reboot_and_trust();
    reboot_and_trust();
    n = durable_history(ev, DSP_EVLOG_SLOTS);
    DSP_CHECK_EQ_INT(1, count_type(ev, n, DSP_EV_DOSE_SCHEDULED, DSP_EVR_INTERRUPTED));
    DSP_CHECK_EQ_INT(0, count_type(ev, n, DSP_EV_DOSE_SCHEDULED, DSP_EVR_OK));
    DSP_CHECK_EQ_INT(DSP_EV_DOSE_SCHEDULED, ev[0].type);
    DSP_CHECK_EQ_STR("cut", ev[0].schedule_id);
    DSP_CHECK_EQ_INT(3000, ev[0].volume_ul);
    DSP_CHECK_EQ_INT(T0, ev[0].at);
    DSP_CHECK(ev[0].flags & DSP_EVF_RESERVOIR_UNKNOWN);
    DSP_CHECK_EQ_INT(0, ev[0].before_ul);
    run_until(T0 + 2 * HOUR);
    DSP_CHECK_EQ_INT(1, pump.calls); /* anti-replay unchanged */

    /* A manual dose interrupted by a power loss during its pump run: INTERRUPTED once. */
    advance(g_true + 2 * MINUTE);
    clear_script();
    step(g_true, op_now()); /* trusted time again after the power-on reboots */
    step_armed(g_true, op_manual(7, 1000));
    pump_cut = true;
    DSP_CHECK(!session());
    pump_cut = false;
    reboot_and_trust();
    reboot_and_trust();
    n = durable_history(ev, DSP_EVLOG_SLOTS);
    DSP_CHECK_EQ_INT(1, count_type(ev, n, DSP_EV_DOSE_MANUAL, DSP_EVR_INTERRUPTED));
    DSP_CHECK_EQ_INT(DSP_EV_DOSE_MANUAL, ev[0].type);
    DSP_CHECK_EQ_INT(1000, ev[0].volume_ul);
    invariants();
}

/* Power cut at EVERY write of a scheduled dose wake: never a duplicate event, never an event for a
 * dose that did not happen, at most one event per occurrence. */
static void test_power_cuts_never_duplicate_events(void)
{
    int cases = 0;
    for (int variant = 0; variant < 2; variant++) {
        for (long k = 1; k <= 40; k++) {
            provision(true);
            clear_script();
            step(g_true, op_create(once_at("pc", T0, 8, 0, 3000)));
            DSP_CHECK(session());
            advance(T0);
            /* cut at the k-th KV write of the dose wake (armed for the whole wake) */
            armed = true;
            op_writes = 0;
            cut_at = k;
            cut_after = variant == 1;
            bool finished = wake(DSP_CAUSE_TIMER);
            cut_at = 0;
            armed = false;
            if (finished) {
                break;
            }
            cases++;
            reboot_and_trust();
            reboot_and_trust();
            run_until(T0 + HOUR);
            dsp_event_t ev[DSP_EVLOG_SLOTS];
            size_t n = durable_history(ev, DSP_EVLOG_SLOTS);
            int doses = 0;
            for (size_t i = 0; i < n; i++) {
                doses += ev[i].type == DSP_EV_DOSE_SCHEDULED;
                if (i > 0) {
                    DSP_CHECK(ev[i].seq < ev[i - 1].seq);
                }
            }
            DSP_CHECK(doses <= 1);
            DSP_CHECK(pump.calls <= 1);
            /* A claim that landed without a pump run is logged as INTERRUPTED/ABORTED (never as
             * completed): an OK dose event exists only for a pump run that happened. */
            for (size_t i = 0; i < n; i++) {
                if (ev[i].type == DSP_EV_DOSE_SCHEDULED && ev[i].result == DSP_EVR_OK) {
                    DSP_CHECK_EQ_INT(1, pump.calls);
                }
            }
            invariants();
        }
    }
    printf("  history: %d power cuts in a dose wake, no duplicate events\n", cases);
    DSP_CHECK(cases >= 6);
}

/* A failing history never blocks dosing, never makes the wake DEGRADED, and never repeats a dose. */
static void test_history_failure_never_affects_dosing(void)
{
    provision(true);
    snprintf(fail_set_prefix, sizeof fail_set_prefix, "ev");
    clear_script();
    step(g_true, op_create(once_at("f1", T0, 8, 0, 1000)));
    step(g_true, op_create(once_at("f2", T0, 8, 30, 1000)));
    DSP_CHECK(session());
    run_until(T0 + HOUR);
    DSP_CHECK_EQ_INT(2, pump.calls); /* both doses ran */
    DSP_CHECK(!rep.degraded);
    DSP_CHECK_EQ_INT(CAP - 2000, durable_cfg().reservoir_remaining_ul);
    dsp_event_t ev[DSP_EVLOG_SLOTS];
    size_t n = durable_history(ev, DSP_EVLOG_SLOTS);
    DSP_CHECK_EQ_INT(0, count_type(ev, n, DSP_EV_DOSE_SCHEDULED, DSP_EVR_OK)); /* lost, not invented */
    /* Within one wake GET_HISTORY reports the failed append (FAULTS): a manual dose whose event
     * cannot be stored, then the history read in the same session. */
    advance(g_true + 2 * MINUTE);
    clear_script();
    step(g_true, op_now());
    int md = step(g_true, op_manual(1, 500));
    step(g_true, op_history(0, 0));
    DSP_CHECK(session());
    DSP_CHECK_EQ_INT(DSP_OP_OK, sc.res[md].r.status);
    DSP_CHECK_EQ_INT(3, pump.calls);
    DSP_CHECK_EQ_INT(DSP_OP_OK, hist_res[0].status);
    DSP_CHECK(hist_pages[0].faults);
    /* Recovered storage: logging resumes, no gap-filling with invented events. */
    fail_set_prefix[0] = '\0';
    clear_script();
    step(g_true, op_create(once_at("f3", T0 + DAY, 8, 0, 1000)));
    DSP_CHECK(session());
    run_until(T0 + DAY + HOUR);
    DSP_CHECK_EQ_INT(4, pump.calls);
    n = durable_history(ev, DSP_EVLOG_SLOTS);
    DSP_CHECK_EQ_INT(1, count_type(ev, n, DSP_EV_DOSE_SCHEDULED, DSP_EVR_OK));
    DSP_CHECK_EQ_STR("f3", ev[0].schedule_id);
    invariants();
}

static void test_get_history_paging_and_authorization(void)
{
    provision(true);
    bulk_limits();
    /* 12 manual doses -> 2 provisioning events + 12 = 14 events. */
    for (int i = 0; i < 12; i++) {
        advance(g_true + 2 * MINUTE);
        clear_script();
        step(g_true, op_manual((uint64_t)i + 1, 100));
        DSP_CHECK(session());
    }
    memset(hist_pages, 0, sizeof hist_pages);
    memset(hist_res, 0, sizeof hist_res);
    clear_script();
    step(g_true, op_history(0, 0));
    step(g_true, op_history(1, 0));
    step(g_true, op_history(2, 0));
    step(g_true, op_history(3, 0));
    step(g_true, op_history(4, 0)); /* past the oldest: empty, still OK */
    step(g_true, op_history(5, 1)); /* no authorization */
    step(g_true, op_history(6, 3)); /* max 0 */
    step(g_true, op_history(7, 4)); /* max 5 */
    DSP_CHECK(session());
    uint32_t expect = hist_pages[0].newest_seq;
    DSP_CHECK_EQ_INT(14, expect);
    size_t total = 0;
    for (int k = 0; k < 4; k++) {
        DSP_CHECK_EQ_INT(DSP_OP_OK, hist_res[k].status);
        DSP_CHECK_EQ_INT(14, hist_pages[k].newest_seq);
        DSP_CHECK_EQ_INT(1, hist_pages[k].oldest_seq);
        DSP_CHECK(!hist_pages[k].faults);
        for (uint8_t j = 0; j < hist_pages[k].n; j++) {
            DSP_CHECK_EQ_INT(expect, hist_pages[k].items[j].seq);
            expect--;
        }
        total += hist_pages[k].n;
    }
    DSP_CHECK_EQ_INT(14, total);
    DSP_CHECK_EQ_INT(DSP_OP_OK, hist_res[4].status);
    DSP_CHECK_EQ_INT(0, hist_pages[4].n);
    DSP_CHECK_EQ_INT(DSP_OP_FORBIDDEN, hist_res[5].status);
    DSP_CHECK_EQ_INT(0, hist_pages[5].n);
    DSP_CHECK_EQ_INT(DSP_OP_INVALID, hist_res[6].status);
    DSP_CHECK_EQ_INT(DSP_OP_INVALID, hist_res[7].status);
    invariants();
}

int main(void)
{
    DSP_RUN(test_doses_and_reservoir_are_logged);
    DSP_RUN(test_interrupted_doses_are_logged_once);
    DSP_RUN(test_power_cuts_never_duplicate_events);
    DSP_RUN(test_history_failure_never_affects_dosing);
    DSP_RUN(test_get_history_paging_and_authorization);
    return dsp_test_finish();
}
