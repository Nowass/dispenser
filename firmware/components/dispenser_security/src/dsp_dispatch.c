#include "dsp_dispatch.h"

#include <string.h>

#include "dsp_auth.h" /* dsp_secure_zero */

uint8_t dsp_dispatch_error_code(dsp_app_op_status_t status)
{
    switch (status) {
    case DSP_OP_OK:
        return 0;
    case DSP_OP_FORBIDDEN:
        return DSP_BLE_E_FORBIDDEN;
    case DSP_OP_BUSY:
        return DSP_BLE_E_BUSY;
    case DSP_OP_REFUSED:
        return DSP_BLE_E_REFUSED;
    case DSP_OP_LIMIT:
    case DSP_OP_CAPACITY:
        return DSP_BLE_E_LIMIT;
    case DSP_OP_INVALID:
        return DSP_BLE_E_INVALID;
    case DSP_OP_CONFLICT:
        return DSP_BLE_E_CONFLICT;
    case DSP_OP_NOT_FOUND:
        return DSP_BLE_E_NOT_FOUND;
    case DSP_OP_STORAGE:
        return DSP_BLE_E_STORAGE;
    case DSP_OP_DEGRADED:
        return DSP_BLE_E_DEGRADED;
    case DSP_OP_DUPLICATE:
        return DSP_BLE_E_DUPLICATE;
    case DSP_OP_NOT_COVERED:
        return DSP_BLE_E_NOT_COVERED;
    case DSP_OP_SLOTS_FULL:
        return DSP_BLE_E_SLOTS_FULL;
    }
    return DSP_BLE_E_DEGRADED; /* unknown result: fail closed, never "OK" */
}

static void set_error(dsp_ble_msg_t *rsp, uint8_t code, uint32_t detail, uint32_t aux)
{
    rsp->h.flags = DSP_BLE_F_RESPONSE | DSP_BLE_F_ERROR;
    memset(&rsp->b, 0, sizeof rsp->b);
    rsp->b.error.code = code;
    rsp->b.error.detail = detail;
    rsp->b.error.aux = aux;
}

/* true when the result is OK; otherwise rsp carries the mapped error */
static bool ok_or_error(dsp_ble_msg_t *rsp, dsp_app_op_result_t r)
{
    if (r.status == DSP_OP_OK) {
        return true;
    }
    uint32_t aux = r.status == DSP_OP_CAPACITY && r.aux == 0 ? 16u : r.aux;
    set_error(rsp, dsp_dispatch_error_code(r.status), r.detail, aux);
    return false;
}

static void str_out(dsp_ble_str_t *dst, const char *src, size_t max)
{
    size_t n = 0;
    while (n < max && src[n] != '\0') {
        n++;
    }
    dst->len = (uint8_t)n;
    memcpy(dst->b, src, n);
}

static bool str_in(char *dst, size_t cap, const dsp_ble_str_t *src)
{
    if (src->len >= cap) {
        return false;
    }
    memcpy(dst, src->b, src->len);
    dst[src->len] = '\0';
    return true;
}

static void config_out(dsp_ble_config_rec_t *o, const dsp_config_t *c, uint32_t hw)
{
    o->revision = c->revision;
    o->reservoir_capacity_ul = c->reservoir_capacity_ul;
    o->reservoir_remaining_ul = c->reservoir_remaining_ul;
    o->low_stock_percent = c->low_stock_percent;
    o->time_sync_interval_h = c->time_sync_interval_h;
    o->notify_mask = c->notify_mask;
    o->pump_flow_ul_per_s = c->pump_flow_ul_per_s;
    o->calibrated_at = c->calibrated_at;
    o->max_dose_ul = c->max_dose_ul;
    o->max_daily_ul = c->max_daily_ul;
    o->max_runtime_ms = c->max_runtime_ms;
    o->min_interval_s = c->min_interval_s;
    o->hw_max_runtime_ms = hw;
}

static void config_in(dsp_config_t *c, const dsp_ble_config_rec_t *i)
{
    memset(c, 0, sizeof *c);
    c->revision = i->revision;
    c->reservoir_capacity_ul = i->reservoir_capacity_ul;
    c->reservoir_remaining_ul = i->reservoir_remaining_ul;
    c->low_stock_percent = i->low_stock_percent;
    c->time_sync_interval_h = i->time_sync_interval_h;
    c->notify_mask = i->notify_mask;
    c->pump_flow_ul_per_s = i->pump_flow_ul_per_s;
    c->calibrated_at = i->calibrated_at;
    c->max_dose_ul = i->max_dose_ul;
    c->max_daily_ul = i->max_daily_ul;
    c->max_runtime_ms = i->max_runtime_ms;
    c->min_interval_s = i->min_interval_s;
    /* hw_max_runtime_ms is read-only (platform cutoff): ignored on input */
}

static void schedule_out(dsp_ble_schedule_rec_t *o, const dsp_schedule_t *s)
{
    memset(o, 0, sizeof *o);
    str_out(&o->id, s->id, DSP_SCHEDULE_ID_MAX_LEN);
    o->revision = s->revision;
    o->enabled = s->enabled;
    o->amount_ul = s->amount_ul;
    o->start_date.year = (uint16_t)s->start_date.year;
    o->start_date.month = s->start_date.month;
    o->start_date.day = s->start_date.day;
    o->hour = s->hour;
    o->minute = s->minute;
    o->kind = s->kind;
    o->interval = s->interval;
    o->weekday_mask = s->weekday_mask;
    o->has_end = s->has_end_date;
    if (s->has_end_date) {
        o->end_date.year = (uint16_t)s->end_date.year;
        o->end_date.month = s->end_date.month;
        o->end_date.day = s->end_date.day;
    }
    str_out(&o->name, s->name, DSP_SCHEDULE_NAME_MAX_BYTES);
}

static bool schedule_in(dsp_schedule_t *s, const dsp_ble_schedule_rec_t *i)
{
    memset(s, 0, sizeof *s);
    if (!str_in(s->id, sizeof s->id, &i->id) || !str_in(s->name, sizeof s->name, &i->name) ||
        i->start_date.year > 32767 || i->end_date.year > 32767) {
        return false;
    }
    s->revision = i->revision;
    s->enabled = i->enabled;
    s->amount_ul = i->amount_ul;
    s->start_date.year = (int16_t)i->start_date.year;
    s->start_date.month = i->start_date.month;
    s->start_date.day = i->start_date.day;
    s->hour = i->hour;
    s->minute = i->minute;
    s->kind = i->kind;
    s->interval = i->interval;
    s->weekday_mask = i->weekday_mask;
    s->has_end_date = i->has_end;
    if (i->has_end) {
        s->end_date.year = (int16_t)i->end_date.year;
        s->end_date.month = i->end_date.month;
        s->end_date.day = i->end_date.day;
    }
    return true; /* not_before is device-maintained: zero on input, ignored by the operation */
}

void dsp_dispatch(dsp_app_t *app, const dsp_app_auth_t *auth, const dsp_ble_msg_t *req,
                  dsp_ble_msg_t *rsp, dsp_dispatch_fx_t *fx)
{
    memset(rsp, 0, sizeof *rsp);
    memset(fx, 0, sizeof *fx);
    rsp->h.major = DSP_BLE_MAJOR;
    rsp->h.minor = DSP_BLE_MINOR;
    rsp->h.type = req->h.type;
    rsp->h.flags = DSP_BLE_F_RESPONSE;
    rsp->h.txn = req->h.txn;
    const dsp_ble_body_t *in = &req->b;
    dsp_ble_body_t *out = &rsp->b;
    dsp_app_op_result_t r;
    switch (req->h.type) {
    case DSP_BLE_T_GET_STATUS:
        (void)ok_or_error(rsp, dsp_app_get_status(app, auth, &out->status.status));
        break;
    case DSP_BLE_T_GET_DIAGNOSTICS:
        (void)ok_or_error(rsp, dsp_app_get_diagnostics(app, auth, &out->diag.diag));
        break;
    case DSP_BLE_T_ACK_REJECTIONS:
        (void)ok_or_error(rsp, dsp_app_ack_rejections(app, auth));
        break;
    case DSP_BLE_T_GET_CONFIG: {
        dsp_config_t c;
        uint32_t hw = 0;
        if (ok_or_error(rsp, dsp_app_get_config(app, auth, &c, &hw))) {
            config_out(&out->config.config, &c, hw);
        }
        break;
    }
    case DSP_BLE_T_SET_CONFIG: {
        dsp_config_t c;
        uint32_t rev = 0;
        config_in(&c, &in->set_config_req.config);
        if (ok_or_error(rsp, dsp_app_set_config(app, auth, in->set_config_req.expected_revision,
                                                 in->set_config_req.field_mask, &c, &rev))) {
            out->revision.revision = rev;
        }
        break;
    }
    case DSP_BLE_T_LIST_SCHEDULES: {
        dsp_app_schedule_page_t pg;
        memset(&pg, 0, sizeof pg);
        if (ok_or_error(rsp, dsp_app_list_schedules(app, auth, in->page.page, &pg))) {
            dsp_ble_list_schedules_rsp_t *l = &out->list_schedules_rsp;
            l->set_revision = pg.set_revision;
            l->total = pg.total;
            l->page = pg.page;
            l->items.n = pg.n <= DSP_BLE_LIST_MAX ? pg.n : DSP_BLE_LIST_MAX;
            for (uint8_t k = 0; k < l->items.n; k++) {
                str_out(&l->items.items[k].id, pg.items[k].id, DSP_SCHEDULE_ID_MAX_LEN);
                l->items.items[k].revision = pg.items[k].revision;
            }
        }
        break;
    }
    case DSP_BLE_T_GET_SCHEDULE: {
        char id[DSP_SCHEDULE_ID_MAX_LEN + 1];
        dsp_schedule_t s;
        if (!str_in(id, sizeof id, &in->id.id)) {
            set_error(rsp, DSP_BLE_E_INVALID, 0, 0);
        } else if (ok_or_error(rsp, dsp_app_get_schedule(app, auth, id, &s))) {
            schedule_out(&out->schedule.schedule, &s);
        }
        break;
    }
    case DSP_BLE_T_CREATE_SCHEDULE: {
        dsp_schedule_t s;
        uint32_t set_rev = 0;
        if (!schedule_in(&s, &in->schedule.schedule)) {
            set_error(rsp, DSP_BLE_E_INVALID, 0, 0);
        } else if (ok_or_error(rsp, dsp_app_create_schedule(app, auth, &s, &set_rev))) {
            out->set_revision.set_revision = set_rev;
        }
        break;
    }
    case DSP_BLE_T_UPDATE_SCHEDULE: {
        dsp_schedule_t s;
        uint32_t rev = 0, set_rev = 0;
        if (!schedule_in(&s, &in->update_schedule_req.schedule)) {
            set_error(rsp, DSP_BLE_E_INVALID, 0, 0);
        } else if (ok_or_error(rsp, dsp_app_update_schedule(app, auth,
                                                             in->update_schedule_req.expected_revision,
                                                             &s, &rev, &set_rev))) {
            out->update_schedule_rsp.revision = rev;
            out->update_schedule_rsp.set_revision = set_rev;
        }
        break;
    }
    case DSP_BLE_T_DELETE_SCHEDULE: {
        char id[DSP_SCHEDULE_ID_MAX_LEN + 1];
        uint32_t set_rev = 0;
        if (!str_in(id, sizeof id, &in->delete_schedule_req.id)) {
            set_error(rsp, DSP_BLE_E_INVALID, 0, 0);
        } else if (ok_or_error(rsp, dsp_app_delete_schedule(app, auth, id,
                                                             in->delete_schedule_req.expected_revision,
                                                             &set_rev))) {
            out->set_revision.set_revision = set_rev;
        }
        break;
    }
    case DSP_BLE_T_MANUAL_DOSE: {
        dsp_app_manual_result_t m;
        memset(&m, 0, sizeof m);
        r = dsp_app_manual_dose(app, auth, in->manual_dose_req.seq, in->manual_dose_req.volume_ul, &m);
        if (r.status == DSP_OP_DUPLICATE) {
            set_error(rsp, DSP_BLE_E_DUPLICATE, m.state, m.detail); /* already decided */
        } else if (ok_or_error(rsp, r)) {
            out->manual_dose_rsp.outcome = m.outcome;
            out->manual_dose_rsp.volume_ul = m.volume_ul;
            out->manual_dose_rsp.runtime_ms = m.runtime_ms;
            (void)dsp_app_get_status(app, auth, &out->manual_dose_rsp.status);
        }
        break;
    }
    case DSP_BLE_T_GET_RESULT: {
        uint8_t state = 0;
        uint32_t detail = 0;
        if (ok_or_error(rsp, dsp_app_get_result(app, auth, in->seq.seq, &state, &detail))) {
            out->get_result_rsp.state = state;
            out->get_result_rsp.detail = detail;
        }
        break;
    }
    case DSP_BLE_T_REFILL: {
        dsp_ble_status_rec_t st;
        memset(&st, 0, sizeof st);
        if (ok_or_error(rsp, dsp_app_refill(app, auth, in->refill_req.capacity_ul,
                                            in->refill_req.remaining_ul, &st))) {
            out->status.status = st;
        }
        break;
    }
    case DSP_BLE_T_SET_READINESS: {
        dsp_ble_status_rec_t st;
        memset(&st, 0, sizeof st);
        if (ok_or_error(rsp, dsp_app_set_readiness(app, auth, in->readiness.readiness, &st))) {
            out->status.status = st;
        }
        break;
    }
    case DSP_BLE_T_SET_TIME: {
        dsp_app_time_result_t t;
        memset(&t, 0, sizeof t);
        if (ok_or_error(rsp, dsp_app_set_phone_time(app, auth, in->set_time_req.phone_utc_ms,
                                                    in->set_time_req.uncertainty_ms, &t))) {
            out->set_time_rsp.decision = t.decision;
            out->set_time_rsp.offset_s = t.offset_s;
        }
        break;
    }
    case DSP_BLE_T_PHONES_LIST: {
        dsp_app_phone_t ph[DSP_APP_PHONE_SLOTS];
        uint8_t n = 0;
        memset(ph, 0, sizeof ph);
        if (ok_or_error(rsp, dsp_app_phones_list(app, auth, ph, &n))) {
            dsp_ble_phone_list_t *l = &out->phones_list_rsp.items;
            l->n = n <= DSP_BLE_LIST_MAX ? n : DSP_BLE_LIST_MAX;
            for (uint8_t k = 0; k < l->n; k++) {
                l->items[k].slot = ph[k].slot;
                l->items[k].name.len = ph[k].name_len <= DSP_AUTH_NAME_MAX ? ph[k].name_len : 0;
                memcpy(l->items[k].name.b, ph[k].name, l->items[k].name.len);
                l->items[k].enrolled_at = ph[k].enrolled_at;
                l->items[k].last_seen = ph[k].last_seen;
                l->items[k].is_self = ph[k].is_self;
            }
        }
        break;
    }
    case DSP_BLE_T_PHONE_REMOVE: {
        bool ends = false;
        if (ok_or_error(rsp, dsp_app_phone_remove(app, auth, in->slot.slot, &ends))) {
            fx->close_after = ends;
        }
        break;
    }
    case DSP_BLE_T_INVITE_CREATE: {
        uint8_t token[16];
        uint16_t expires = 0;
        /* name_hint is accepted and ignored (design 5.2) */
        if (ok_or_error(rsp, dsp_app_invite_create(app, auth, token, &expires))) {
            memcpy(out->invite_create_rsp.token, token, sizeof token);
            out->invite_create_rsp.expires_s = expires;
        }
        dsp_secure_zero(token, sizeof token);
        break;
    }
    case DSP_BLE_T_INVITE_CANCEL:
        (void)ok_or_error(rsp, dsp_app_invite_cancel(app, auth));
        break;
    case DSP_BLE_T_RECOVERY_CANCEL:
        (void)ok_or_error(rsp, dsp_app_recovery_cancel(app, auth));
        break;
    case DSP_BLE_T_END_SESSION:
        /* C10: the authorized owner ends the whole service window */
        (void)ok_or_error(rsp, dsp_app_end_service_session(app, auth));
        break;
    case DSP_BLE_T_WIFI_SET:
    case DSP_BLE_T_WIFI_GET:
    case DSP_BLE_T_WIFI_CLEAR:
    case DSP_BLE_T_FACTORY_RESET:
        set_error(rsp, DSP_BLE_E_UNAVAILABLE, 0, 0); /* C5: recognized, not provided */
        break;
    default:
        set_error(rsp, DSP_BLE_E_MALFORMED, 0, 0); /* not a request of an authenticated session */
        break;
    }
}
