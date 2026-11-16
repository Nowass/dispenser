#include "dsp_bleproto.h"

#include "dsp_text.h"

#include <string.h>

/* ---- Layout tables (docs/BLE_PROTOCOL.md sections 5 and 7) ------------------------------- */

#define NONE16 0xFFFFu
#define NONE32 0xFFFFFFFFu
#define OFF(st, m) ((uint16_t)offsetof(st, m))
#define FLD(name, kind, sk, lo, hi, allowed, off, cond, sub, esz, ioff) \
    {name, kind, sk, lo, hi, allowed, off, cond, sub, esz, ioff}
#define F_U8(st, m) FLD(#m, DSP_BLEF_U8, 0, 0, 0, 0, OFF(st, m), NONE16, NULL, 0, 0)
#define F_U16(st, m) FLD(#m, DSP_BLEF_U16, 0, 0, 0, 0, OFF(st, m), NONE16, NULL, 0, 0)
#define F_U32(st, m) FLD(#m, DSP_BLEF_U32, 0, 0, 0, 0, OFF(st, m), NONE16, NULL, 0, 0)
#define F_U64(st, m) FLD(#m, DSP_BLEF_U64, 0, 0, 0, 0, OFF(st, m), NONE16, NULL, 0, 0)
#define F_I64(st, m) FLD(#m, DSP_BLEF_I64, 0, 0, 0, 0, OFF(st, m), NONE16, NULL, 0, 0)
#define F_BOOL(st, m) FLD(#m, DSP_BLEF_BOOL, 0, 0, 0, 0, OFF(st, m), NONE16, NULL, 0, 0)
#define F_ENUM(st, m, lo, hi) FLD(#m, DSP_BLEF_ENUM, 0, lo, hi, NONE32, OFF(st, m), NONE16, NULL, 0, 0)
#define F_ENUMX(st, m, lo, hi, x) FLD(#m, DSP_BLEF_ENUM, 0, lo, hi, x, OFF(st, m), NONE16, NULL, 0, 0)
#define F_MASK8(st, m, a) FLD(#m, DSP_BLEF_MASK8, 0, 0, 0, a, OFF(st, m), NONE16, NULL, 0, 0)
#define F_MASK16(st, m, a) FLD(#m, DSP_BLEF_MASK16, 0, 0, 0, a, OFF(st, m), NONE16, NULL, 0, 0)
#define F_MASK32(st, m, a) FLD(#m, DSP_BLEF_MASK32, 0, 0, 0, a, OFF(st, m), NONE16, NULL, 0, 0)
#define F_CONST32(st, m, v) FLD(#m, DSP_BLEF_CONST32, 0, 0, 0, v, OFF(st, m), NONE16, NULL, 0, 0)
#define F_DATE(st, m) FLD(#m, DSP_BLEF_DATE, 0, 0, 0, 0, OFF(st, m), NONE16, NULL, 0, 0)
#define F_DATE_IF(st, m, c) FLD(#m, DSP_BLEF_DATE, 0, 0, 0, 0, OFF(st, m), OFF(st, c), NULL, 0, 0)
#define F_RESERVED(n) FLD(NULL, DSP_BLEF_RESERVED, 0, n, n, 0, 0, NONE16, NULL, 0, 0)
#define F_BYTES(st, m, n) FLD(#m, DSP_BLEF_BYTES, 0, n, n, 0, OFF(st, m), NONE16, NULL, 0, 0)
#define F_BLOB(st, m) FLD(#m, DSP_BLEF_BLOB, 0, 0, 0, 0, OFF(st, m), NONE16, NULL, 0, 0)
#define F_STR(st, m, k, lo, hi) FLD(#m, DSP_BLEF_STR, k, lo, hi, 0, OFF(st, m), NONE16, NULL, 0, 0)
#define F_REC(st, m, l) FLD(#m, DSP_BLEF_RECORD, 0, 0, 0, 0, OFF(st, m), NONE16, &l, 0, 0)
#define F_LIST(st, m, lt, et, l) \
    FLD(#m, DSP_BLEF_LIST, 0, 0, DSP_BLE_LIST_MAX, 0, OFF(st, m), NONE16, &l, (uint16_t)sizeof(et), OFF(lt, items))
#define LAYOUT(name, arr) static const dsp_ble_layout_t name = {arr, (uint8_t)(sizeof arr / sizeof arr[0])}
#define EMPTY_LAYOUT(name) static const dsp_ble_layout_t name = {NULL, 0}

static const dsp_ble_field_t STATUS_F[] = {
    F_U8(dsp_ble_status_rec_t, readiness), F_U8(dsp_ble_status_rec_t, dosing_flags),
    F_U32(dsp_ble_status_rec_t, gates), F_U32(dsp_ble_status_rec_t, pump_problems),
    F_U32(dsp_ble_status_rec_t, capacity_ul), F_U32(dsp_ble_status_rec_t, remaining_ul),
    F_U8(dsp_ble_status_rec_t, low_percent), F_BOOL(dsp_ble_status_rec_t, estimate_uncertain),
    F_ENUM(dsp_ble_status_rec_t, clock_trust, 0, 2), F_U8(dsp_ble_status_rec_t, clock_reason),
    F_I64(dsp_ble_status_rec_t, device_utc), F_BOOL(dsp_ble_status_rec_t, next_due_known),
    F_I64(dsp_ble_status_rec_t, next_due_at), F_U8(dsp_ble_status_rec_t, pending_notes),
    F_ENUM(dsp_ble_status_rec_t, highest_severity, 0, 2), F_BOOL(dsp_ble_status_rec_t, storage_ok),
    F_U16(dsp_ble_status_rec_t, session_seconds_left),
    F_ENUMX(dsp_ble_status_rec_t, battery_percent, 0, 100, 255),
    F_U64(dsp_ble_status_rec_t, manual_floor), F_RESERVED(5),
};
LAYOUT(STATUS_L, STATUS_F);

static const dsp_ble_field_t DIAG_F[] = {
    F_BOOL(dsp_ble_diag_rec_t, notify_storage_ok), F_U8(dsp_ble_diag_rec_t, tx_diag),
    F_U16(dsp_ble_diag_rec_t, tx_diag_status), F_U8(dsp_ble_diag_rec_t, rejections_unread),
    F_U16(dsp_ble_diag_rec_t, rejections_total), F_ENUM(dsp_ble_diag_rec_t, clock_trust, 0, 2),
    F_U8(dsp_ble_diag_rec_t, clock_reason), F_I64(dsp_ble_diag_rec_t, last_sync_utc),
    F_U8(dsp_ble_diag_rec_t, last_wake_reason), F_RESERVED(6),
};
LAYOUT(DIAG_L, DIAG_F);

static const dsp_ble_field_t CONFIG_F[] = {
    F_U32(dsp_ble_config_rec_t, revision), F_U32(dsp_ble_config_rec_t, reservoir_capacity_ul),
    F_U32(dsp_ble_config_rec_t, reservoir_remaining_ul), F_U8(dsp_ble_config_rec_t, low_stock_percent),
    F_U16(dsp_ble_config_rec_t, time_sync_interval_h), F_U8(dsp_ble_config_rec_t, notify_mask),
    F_U32(dsp_ble_config_rec_t, pump_flow_ul_per_s), F_I64(dsp_ble_config_rec_t, calibrated_at),
    F_U32(dsp_ble_config_rec_t, max_dose_ul), F_U32(dsp_ble_config_rec_t, max_daily_ul),
    F_U32(dsp_ble_config_rec_t, max_runtime_ms), F_U32(dsp_ble_config_rec_t, min_interval_s),
    F_U32(dsp_ble_config_rec_t, hw_max_runtime_ms),
};
LAYOUT(CONFIG_L, CONFIG_F);

static const dsp_ble_field_t SCHEDULE_F[] = {
    F_STR(dsp_ble_schedule_rec_t, id, DSP_BLES_ID, 1, 64), F_U32(dsp_ble_schedule_rec_t, revision),
    F_BOOL(dsp_ble_schedule_rec_t, enabled), F_U32(dsp_ble_schedule_rec_t, amount_ul),
    F_DATE(dsp_ble_schedule_rec_t, start_date), F_ENUM(dsp_ble_schedule_rec_t, hour, 0, 23),
    F_ENUM(dsp_ble_schedule_rec_t, minute, 0, 59), F_ENUM(dsp_ble_schedule_rec_t, kind, 0, 2),
    F_U32(dsp_ble_schedule_rec_t, interval), F_MASK8(dsp_ble_schedule_rec_t, weekday_mask, 0x7Fu),
    F_BOOL(dsp_ble_schedule_rec_t, has_end), F_DATE_IF(dsp_ble_schedule_rec_t, end_date, has_end),
    F_STR(dsp_ble_schedule_rec_t, name, DSP_BLES_TEXT, 0, 32),
};
LAYOUT(SCHEDULE_L, SCHEDULE_F);

static const dsp_ble_field_t SCHED_ITEM_F[] = {
    F_STR(dsp_ble_sched_item_t, id, DSP_BLES_ID, 1, 64), F_U32(dsp_ble_sched_item_t, revision),
};
LAYOUT(SCHED_ITEM_L, SCHED_ITEM_F);

static const dsp_ble_field_t PHONE_ITEM_F[] = {
    F_ENUM(dsp_ble_phone_item_t, slot, 0, 3), F_STR(dsp_ble_phone_item_t, name, DSP_BLES_TEXT, 0, 24),
    F_I64(dsp_ble_phone_item_t, enrolled_at), F_I64(dsp_ble_phone_item_t, last_seen),
    F_BOOL(dsp_ble_phone_item_t, is_self),
};
LAYOUT(PHONE_ITEM_L, PHONE_ITEM_F);

static const dsp_ble_field_t ERROR_F[] = {
    F_ENUM(dsp_ble_error_body_t, code, 1, 19), F_U32(dsp_ble_error_body_t, detail),
    F_U32(dsp_ble_error_body_t, aux),
};
LAYOUT(ERROR_L, ERROR_F);

EMPTY_LAYOUT(EMPTY_L);

static const dsp_ble_field_t HELLO_REQ_F[] = {F_U8(dsp_ble_hello_req_t, proto_min), F_U8(dsp_ble_hello_req_t, proto_max)};
LAYOUT(HELLO_REQ_L, HELLO_REQ_F);
static const dsp_ble_field_t HELLO_RSP_F[] = {
    F_U8(dsp_ble_hello_rsp_t, proto), F_ENUM(dsp_ble_hello_rsp_t, claim_state, 0, 1),
    F_MASK8(dsp_ble_hello_rsp_t, flags, 0x01u), F_MASK32(dsp_ble_hello_rsp_t, caps, 0x1Fu),
    F_U32(dsp_ble_hello_rsp_t, session_id), F_BYTES(dsp_ble_hello_rsp_t, device_id_hash, 8),
};
LAYOUT(HELLO_RSP_L, HELLO_RSP_F);
static const dsp_ble_field_t HS_REQ_F[] = {
    F_ENUM(dsp_ble_hs_req_t, purpose, 1, 4), F_ENUM(dsp_ble_hs_req_t, msg_index, 0, 2), F_BLOB(dsp_ble_hs_req_t, noise),
};
LAYOUT(HS_REQ_L, HS_REQ_F);
static const dsp_ble_field_t HS_RSP_F[] = {F_ENUM(dsp_ble_hs_rsp_t, msg_index, 0, 2), F_BLOB(dsp_ble_hs_rsp_t, noise)};
LAYOUT(HS_RSP_L, HS_RSP_F);
static const dsp_ble_field_t SLOT_F[] = {F_ENUM(dsp_ble_slot_t, slot, 0, 3)};
LAYOUT(SLOT_L, SLOT_F);
static const dsp_ble_field_t STATUS_BODY_F[] = {F_REC(dsp_ble_status_body_t, status, STATUS_L)};
LAYOUT(STATUS_BODY_L, STATUS_BODY_F);
static const dsp_ble_field_t DIAG_BODY_F[] = {F_REC(dsp_ble_diag_body_t, diag, DIAG_L)};
LAYOUT(DIAG_BODY_L, DIAG_BODY_F);
static const dsp_ble_field_t CONFIG_BODY_F[] = {F_REC(dsp_ble_config_body_t, config, CONFIG_L)};
LAYOUT(CONFIG_BODY_L, CONFIG_BODY_F);
static const dsp_ble_field_t SET_CONFIG_F[] = {
    F_U32(dsp_ble_set_config_req_t, expected_revision), F_MASK16(dsp_ble_set_config_req_t, field_mask, 0x03FFu),
    F_REC(dsp_ble_set_config_req_t, config, CONFIG_L),
};
LAYOUT(SET_CONFIG_L, SET_CONFIG_F);
static const dsp_ble_field_t REVISION_F[] = {F_U32(dsp_ble_revision_t, revision)};
LAYOUT(REVISION_L, REVISION_F);
static const dsp_ble_field_t PAGE_F[] = {F_U8(dsp_ble_page_t, page)};
LAYOUT(PAGE_L, PAGE_F);
static const dsp_ble_field_t LIST_SCHED_RSP_F[] = {
    F_U32(dsp_ble_list_schedules_rsp_t, set_revision), F_U8(dsp_ble_list_schedules_rsp_t, total),
    F_U8(dsp_ble_list_schedules_rsp_t, page),
    F_LIST(dsp_ble_list_schedules_rsp_t, items, dsp_ble_sched_list_t, dsp_ble_sched_item_t, SCHED_ITEM_L),
};
LAYOUT(LIST_SCHED_RSP_L, LIST_SCHED_RSP_F);
static const dsp_ble_field_t ID_F[] = {F_STR(dsp_ble_id_t, id, DSP_BLES_ID, 1, 64)};
LAYOUT(ID_L, ID_F);
static const dsp_ble_field_t SCHEDULE_BODY_F[] = {F_REC(dsp_ble_schedule_body_t, schedule, SCHEDULE_L)};
LAYOUT(SCHEDULE_BODY_L, SCHEDULE_BODY_F);
static const dsp_ble_field_t SET_REVISION_F[] = {F_U32(dsp_ble_set_revision_t, set_revision)};
LAYOUT(SET_REVISION_L, SET_REVISION_F);
static const dsp_ble_field_t UPDATE_REQ_F[] = {
    F_U32(dsp_ble_update_schedule_req_t, expected_revision), F_REC(dsp_ble_update_schedule_req_t, schedule, SCHEDULE_L),
};
LAYOUT(UPDATE_REQ_L, UPDATE_REQ_F);
static const dsp_ble_field_t UPDATE_RSP_F[] = {
    F_U32(dsp_ble_update_schedule_rsp_t, revision), F_U32(dsp_ble_update_schedule_rsp_t, set_revision),
};
LAYOUT(UPDATE_RSP_L, UPDATE_RSP_F);
static const dsp_ble_field_t DELETE_REQ_F[] = {
    F_STR(dsp_ble_delete_schedule_req_t, id, DSP_BLES_ID, 1, 64), F_U32(dsp_ble_delete_schedule_req_t, expected_revision),
};
LAYOUT(DELETE_REQ_L, DELETE_REQ_F);
static const dsp_ble_field_t MANUAL_REQ_F[] = {F_U64(dsp_ble_manual_dose_req_t, seq), F_U32(dsp_ble_manual_dose_req_t, volume_ul)};
LAYOUT(MANUAL_REQ_L, MANUAL_REQ_F);
static const dsp_ble_field_t MANUAL_RSP_F[] = {
    F_ENUM(dsp_ble_manual_dose_rsp_t, outcome, 0, 1), F_U32(dsp_ble_manual_dose_rsp_t, volume_ul),
    F_U32(dsp_ble_manual_dose_rsp_t, runtime_ms), F_REC(dsp_ble_manual_dose_rsp_t, status, STATUS_L),
};
LAYOUT(MANUAL_RSP_L, MANUAL_RSP_F);
static const dsp_ble_field_t SEQ_F[] = {F_U64(dsp_ble_seq_t, seq)};
LAYOUT(SEQ_L, SEQ_F);
static const dsp_ble_field_t GET_RESULT_RSP_F[] = {
    F_ENUM(dsp_ble_get_result_rsp_t, state, 0, 6), F_U32(dsp_ble_get_result_rsp_t, detail),
};
LAYOUT(GET_RESULT_RSP_L, GET_RESULT_RSP_F);
static const dsp_ble_field_t REFILL_REQ_F[] = {F_U32(dsp_ble_refill_req_t, capacity_ul), F_U32(dsp_ble_refill_req_t, remaining_ul)};
LAYOUT(REFILL_REQ_L, REFILL_REQ_F);
static const dsp_ble_field_t READINESS_F[] = {F_ENUM(dsp_ble_readiness_t, readiness, 1, 2)};
LAYOUT(READINESS_L, READINESS_F);
static const dsp_ble_field_t SET_TIME_REQ_F[] = {
    F_I64(dsp_ble_set_time_req_t, phone_utc_ms), F_U32(dsp_ble_set_time_req_t, uncertainty_ms),
};
LAYOUT(SET_TIME_REQ_L, SET_TIME_REQ_F);
static const dsp_ble_field_t SET_TIME_RSP_F[] = {
    F_ENUM(dsp_ble_set_time_rsp_t, decision, 0, 4), F_I64(dsp_ble_set_time_rsp_t, offset_s),
};
LAYOUT(SET_TIME_RSP_L, SET_TIME_RSP_F);
static const dsp_ble_field_t WIFI_SET_F[] = {
    F_STR(dsp_ble_wifi_set_req_t, ssid, DSP_BLES_RAW, 1, 32), F_STR(dsp_ble_wifi_set_req_t, passphrase, DSP_BLES_WPA, 0, 63),
};
LAYOUT(WIFI_SET_L, WIFI_SET_F);
static const dsp_ble_field_t WIFI_GET_F[] = {
    F_STR(dsp_ble_wifi_get_rsp_t, ssid, DSP_BLES_RAW, 0, 32), F_BOOL(dsp_ble_wifi_get_rsp_t, passphrase_set),
    F_ENUM(dsp_ble_wifi_get_rsp_t, last_result, 0, 5),
};
LAYOUT(WIFI_GET_L, WIFI_GET_F);
static const dsp_ble_field_t PHONES_RSP_F[] = {
    F_LIST(dsp_ble_phones_list_rsp_t, items, dsp_ble_phone_list_t, dsp_ble_phone_item_t, PHONE_ITEM_L),
};
LAYOUT(PHONES_RSP_L, PHONES_RSP_F);
static const dsp_ble_field_t INVITE_REQ_F[] = {F_STR(dsp_ble_invite_create_req_t, name_hint, DSP_BLES_TEXT, 0, 24)};
LAYOUT(INVITE_REQ_L, INVITE_REQ_F);
static const dsp_ble_field_t INVITE_RSP_F[] = {
    F_BYTES(dsp_ble_invite_create_rsp_t, token, 16), F_U16(dsp_ble_invite_create_rsp_t, expires_s),
};
LAYOUT(INVITE_RSP_L, INVITE_RSP_F);
static const dsp_ble_field_t FACTORY_F[] = {F_CONST32(dsp_ble_factory_reset_req_t, confirm, DSP_BLE_FACTORY_RESET_CONFIRM)};
LAYOUT(FACTORY_L, FACTORY_F);
static const dsp_ble_field_t EV_CLOSING_F[] = {
    F_ENUM(dsp_ble_ev_session_closing_t, reason, 1, 4), F_U16(dsp_ble_ev_session_closing_t, seconds_left),
};
LAYOUT(EV_CLOSING_L, EV_CLOSING_F);
static const dsp_ble_field_t EV_SUSPEND_F[] = {F_U16(dsp_ble_ev_suspending_t, seconds_until)};
LAYOUT(EV_SUSPEND_L, EV_SUSPEND_F);
static const dsp_ble_field_t EV_CONFIRM_F[] = {F_ENUM(dsp_ble_ev_confirm_t, action, 1, 1), F_U8(dsp_ble_ev_confirm_t, seconds)};
LAYOUT(EV_CONFIRM_L, EV_CONFIRM_F);

typedef struct {
    uint8_t type;
    uint8_t cls;
    const dsp_ble_layout_t *req, *rsp, *evt;
} type_entry_t;

#define PL DSP_BLE_CLASS_PLAIN
#define EN DSP_BLE_CLASS_ENCRYPTED
static const type_entry_t TYPES[] = {
    {DSP_BLE_T_HELLO, PL, &HELLO_REQ_L, &HELLO_RSP_L, NULL},
    {DSP_BLE_T_HS, PL, &HS_REQ_L, &HS_RSP_L, NULL},
    {DSP_BLE_T_ENROLL_RESULT, EN, NULL, &SLOT_L, NULL},
    {DSP_BLE_T_GET_STATUS, EN, &EMPTY_L, &STATUS_BODY_L, NULL},
    {DSP_BLE_T_GET_DIAGNOSTICS, EN, &EMPTY_L, &DIAG_BODY_L, NULL},
    {DSP_BLE_T_ACK_REJECTIONS, EN, &EMPTY_L, &EMPTY_L, NULL},
    {DSP_BLE_T_GET_CONFIG, EN, &EMPTY_L, &CONFIG_BODY_L, NULL},
    {DSP_BLE_T_SET_CONFIG, EN, &SET_CONFIG_L, &REVISION_L, NULL},
    {DSP_BLE_T_LIST_SCHEDULES, EN, &PAGE_L, &LIST_SCHED_RSP_L, NULL},
    {DSP_BLE_T_GET_SCHEDULE, EN, &ID_L, &SCHEDULE_BODY_L, NULL},
    {DSP_BLE_T_CREATE_SCHEDULE, EN, &SCHEDULE_BODY_L, &SET_REVISION_L, NULL},
    {DSP_BLE_T_UPDATE_SCHEDULE, EN, &UPDATE_REQ_L, &UPDATE_RSP_L, NULL},
    {DSP_BLE_T_DELETE_SCHEDULE, EN, &DELETE_REQ_L, &SET_REVISION_L, NULL},
    {DSP_BLE_T_MANUAL_DOSE, EN, &MANUAL_REQ_L, &MANUAL_RSP_L, NULL},
    {DSP_BLE_T_GET_RESULT, EN, &SEQ_L, &GET_RESULT_RSP_L, NULL},
    {DSP_BLE_T_REFILL, EN, &REFILL_REQ_L, &STATUS_BODY_L, NULL},
    {DSP_BLE_T_SET_READINESS, EN, &READINESS_L, &STATUS_BODY_L, NULL},
    {DSP_BLE_T_SET_TIME, EN, &SET_TIME_REQ_L, &SET_TIME_RSP_L, NULL},
    {DSP_BLE_T_WIFI_SET, EN, &WIFI_SET_L, &EMPTY_L, NULL},
    {DSP_BLE_T_WIFI_GET, EN, &EMPTY_L, &WIFI_GET_L, NULL},
    {DSP_BLE_T_WIFI_CLEAR, EN, &EMPTY_L, &EMPTY_L, NULL},
    {DSP_BLE_T_PHONES_LIST, EN, &EMPTY_L, &PHONES_RSP_L, NULL},
    {DSP_BLE_T_PHONE_REMOVE, EN, &SLOT_L, &EMPTY_L, NULL},
    {DSP_BLE_T_INVITE_CREATE, EN, &INVITE_REQ_L, &INVITE_RSP_L, NULL},
    {DSP_BLE_T_INVITE_CANCEL, EN, &EMPTY_L, &EMPTY_L, NULL},
    {DSP_BLE_T_RECOVERY_CANCEL, EN, &EMPTY_L, &EMPTY_L, NULL},
    {DSP_BLE_T_FACTORY_RESET, EN, &FACTORY_L, &EMPTY_L, NULL},
    {DSP_BLE_T_END_SESSION, DSP_BLE_CLASS_EITHER, &EMPTY_L, &EMPTY_L, NULL},
    {DSP_BLE_T_EV_SESSION_CLOSING, EN, NULL, NULL, &EV_CLOSING_L},
    {DSP_BLE_T_EV_SUSPENDING_FOR_DOSE, EN, NULL, NULL, &EV_SUSPEND_L},
    {DSP_BLE_T_EV_BUSY, PL, NULL, NULL, &EMPTY_L},
    {DSP_BLE_T_EV_CONFIRM_ON_DEVICE, EN, NULL, NULL, &EV_CONFIRM_L},
};

static const type_entry_t *find_type(uint8_t type)
{
    for (size_t i = 0; i < sizeof TYPES / sizeof TYPES[0]; i++) {
        if (TYPES[i].type == type) {
            return &TYPES[i];
        }
    }
    return NULL;
}

const dsp_ble_layout_t *dsp_ble_layout_for(uint8_t type, uint8_t flags, dsp_ble_class_t *cls)
{
    const type_entry_t *t = find_type(type);
    if (t == NULL) {
        return NULL;
    }
    if (cls != NULL) {
        *cls = (dsp_ble_class_t)t->cls;
    }
    uint8_t f = (uint8_t)(flags & (DSP_BLE_F_RESPONSE | DSP_BLE_F_ERROR | DSP_BLE_F_EVENT));
    if (f == 0) {
        return t->req;
    }
    if (f == DSP_BLE_F_RESPONSE) {
        return t->rsp;
    }
    if (f == (DSP_BLE_F_RESPONSE | DSP_BLE_F_ERROR)) {
        return t->rsp != NULL ? &ERROR_L : NULL;
    }
    if (f == DSP_BLE_F_EVENT) {
        return t->evt;
    }
    return NULL;
}

const char *dsp_ble_result_name(dsp_ble_result_t r)
{
    static const char *const NAMES[] = {
        "ok", "arg", "truncated", "length_mismatch", "bad_major", "bad_flags", "unknown_type",
        "not_a_request", "trailing", "bad_bool", "bad_enum", "bad_reserved", "bad_date", "bad_string",
        "too_large", "bad_encrypted", "bad_magic", "frag_no_first", "frag_index", "frag_empty",
        "frag_timeout", "frag_overflow", "buffer",
    };
    return (size_t)r < sizeof NAMES / sizeof NAMES[0] ? NAMES[r] : "?";
}

/* ---- Value rules ----------------------------------------------------------------------------- */

static bool date_ok(const dsp_ble_date_t *d)
{
    return d->year >= 2000 && d->year <= 2099 && d->month >= 1 && d->month <= 12 && d->day >= 1 && d->day <= 31;
}

static bool date_zero(const dsp_ble_date_t *d)
{
    return d->year == 0 && d->month == 0 && d->day == 0;
}

static bool str_ok(const dsp_ble_field_t *f, const dsp_ble_str_t *s)
{
    if (s->len < f->min || s->len > f->max || s->len > DSP_BLE_STR_MAX) {
        return false;
    }
    switch (f->str_kind) {
    case DSP_BLES_TEXT:
        return dsp_utf8_valid(s->b, s->len, true);
    case DSP_BLES_ID:
        for (size_t i = 0; i < s->len; i++) {
            uint8_t c = s->b[i];
            bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '.' || c == '_' || c == '-';
            if (!ok) {
                return false;
            }
        }
        return true;
    case DSP_BLES_RAW:
        return true;
    case DSP_BLES_WPA:
        if (s->len != 0 && s->len < 8) {
            return false;
        }
        for (size_t i = 0; i < s->len; i++) {
            if (s->b[i] < 0x20 || s->b[i] > 0x7E) {
                return false;
            }
        }
        return true;
    default:
        return false;
    }
}

static bool enum_ok(const dsp_ble_field_t *f, uint8_t v)
{
    return (v >= f->min && v <= f->max) || (f->allowed != NONE32 && v == f->allowed);
}

/* ---- Little-endian access -------------------------------------------------------------------- */

static uint64_t get_le(const uint8_t *p, size_t n)
{
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        v |= (uint64_t)p[i] << (8 * i);
    }
    return v;
}

static void put_le(uint8_t *p, uint64_t v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

/* ---- Generic body decoding -------------------------------------------------------------------- */

typedef struct {
    const uint8_t *p;
    size_t len;
    size_t pos;
} rd_t;

static bool take(rd_t *r, size_t n, const uint8_t **out)
{
    if (r->len - r->pos < n) {
        return false;
    }
    *out = r->p + r->pos;
    r->pos += n;
    return true;
}

static dsp_ble_result_t dec_layout(const dsp_ble_layout_t *l, rd_t *r, uint8_t *base);

static dsp_ble_result_t dec_field(const dsp_ble_field_t *f, rd_t *r, uint8_t *base)
{
    const uint8_t *p = NULL;
    uint8_t *dst = base + f->offset;
    switch (f->kind) {
    case DSP_BLEF_U8:
    case DSP_BLEF_ENUM:
    case DSP_BLEF_BOOL:
    case DSP_BLEF_MASK8:
        if (!take(r, 1, &p)) {
            return DSP_BLE_ERR_TRUNCATED;
        }
        if (f->kind == DSP_BLEF_BOOL) {
            if (p[0] > 1) {
                return DSP_BLE_ERR_BAD_BOOL;
            }
            *(bool *)dst = p[0] == 1;
            return DSP_BLE_OK;
        }
        if ((f->kind == DSP_BLEF_ENUM && !enum_ok(f, p[0])) ||
            (f->kind == DSP_BLEF_MASK8 && (p[0] & ~f->allowed) != 0)) {
            return DSP_BLE_ERR_BAD_ENUM;
        }
        *dst = p[0];
        return DSP_BLE_OK;
    case DSP_BLEF_U16:
    case DSP_BLEF_MASK16: {
        if (!take(r, 2, &p)) {
            return DSP_BLE_ERR_TRUNCATED;
        }
        uint16_t v = (uint16_t)get_le(p, 2);
        if (f->kind == DSP_BLEF_MASK16 && (v & ~f->allowed) != 0) {
            return DSP_BLE_ERR_BAD_ENUM;
        }
        memcpy(dst, &v, sizeof v);
        return DSP_BLE_OK;
    }
    case DSP_BLEF_U32:
    case DSP_BLEF_MASK32:
    case DSP_BLEF_CONST32: {
        if (!take(r, 4, &p)) {
            return DSP_BLE_ERR_TRUNCATED;
        }
        uint32_t v = (uint32_t)get_le(p, 4);
        if ((f->kind == DSP_BLEF_MASK32 && (v & ~f->allowed) != 0) ||
            (f->kind == DSP_BLEF_CONST32 && v != f->allowed)) {
            return DSP_BLE_ERR_BAD_ENUM;
        }
        memcpy(dst, &v, sizeof v);
        return DSP_BLE_OK;
    }
    case DSP_BLEF_U64:
    case DSP_BLEF_I64: {
        if (!take(r, 8, &p)) {
            return DSP_BLE_ERR_TRUNCATED;
        }
        uint64_t v = get_le(p, 8);
        memcpy(dst, &v, sizeof v); /* int64_t: two's complement */
        return DSP_BLE_OK;
    }
    case DSP_BLEF_DATE: {
        if (!take(r, 4, &p)) {
            return DSP_BLE_ERR_TRUNCATED;
        }
        dsp_ble_date_t d = {(uint16_t)get_le(p, 2), p[2], p[3]};
        bool must_be_zero = f->cond_offset != NONE16 && !*(const bool *)(base + f->cond_offset);
        if (must_be_zero ? !date_zero(&d) : !date_ok(&d)) {
            return DSP_BLE_ERR_BAD_DATE;
        }
        memcpy(dst, &d, sizeof d);
        return DSP_BLE_OK;
    }
    case DSP_BLEF_RESERVED:
        if (!take(r, f->min, &p)) {
            return DSP_BLE_ERR_TRUNCATED;
        }
        for (size_t i = 0; i < f->min; i++) {
            if (p[i] != 0) {
                return DSP_BLE_ERR_BAD_RESERVED;
            }
        }
        return DSP_BLE_OK;
    case DSP_BLEF_BYTES:
        if (!take(r, f->min, &p)) {
            return DSP_BLE_ERR_TRUNCATED;
        }
        memcpy(dst, p, f->min);
        return DSP_BLE_OK;
    case DSP_BLEF_BLOB: {
        dsp_ble_blob_t *b = (dsp_ble_blob_t *)(void *)dst;
        size_t n = r->len - r->pos;
        if (n > sizeof b->b) {
            return DSP_BLE_ERR_TOO_LARGE;
        }
        (void)take(r, n, &p);
        b->len = (uint16_t)n;
        if (n > 0) {
            memcpy(b->b, p, n);
        }
        return DSP_BLE_OK;
    }
    case DSP_BLEF_STR: {
        dsp_ble_str_t *s = (dsp_ble_str_t *)(void *)dst;
        if (!take(r, 1, &p)) {
            return DSP_BLE_ERR_TRUNCATED;
        }
        uint8_t n = p[0];
        if (n > DSP_BLE_STR_MAX) {
            /* cannot be valid for any field; still report truncation first if it is short */
            if (r->len - r->pos < n) {
                return DSP_BLE_ERR_TRUNCATED;
            }
            return DSP_BLE_ERR_BAD_STRING;
        }
        if (!take(r, n, &p)) {
            return DSP_BLE_ERR_TRUNCATED;
        }
        s->len = n;
        memcpy(s->b, p, n);
        return str_ok(f, s) ? DSP_BLE_OK : DSP_BLE_ERR_BAD_STRING;
    }
    case DSP_BLEF_RECORD:
        return dec_layout(f->sub, r, dst);
    case DSP_BLEF_LIST: {
        if (!take(r, 1, &p)) {
            return DSP_BLE_ERR_TRUNCATED;
        }
        if (p[0] > f->max) {
            return DSP_BLE_ERR_BAD_ENUM;
        }
        *dst = p[0];
        for (size_t i = 0; i < p[0]; i++) {
            dsp_ble_result_t e = dec_layout(f->sub, r, dst + f->items_offset + i * f->elem_size);
            if (e != DSP_BLE_OK) {
                return e;
            }
        }
        return DSP_BLE_OK;
    }
    default:
        return DSP_BLE_ERR_ARG;
    }
}

static dsp_ble_result_t dec_layout(const dsp_ble_layout_t *l, rd_t *r, uint8_t *base)
{
    for (size_t i = 0; i < l->count; i++) {
        dsp_ble_result_t e = dec_field(&l->fields[i], r, base);
        if (e != DSP_BLE_OK) {
            return e;
        }
    }
    return DSP_BLE_OK;
}

/* ---- Generic body encoding -------------------------------------------------------------------- */

typedef struct {
    uint8_t *p;
    size_t cap;
    size_t pos;
} wr_t;

static bool put(wr_t *w, const void *src, size_t n)
{
    if (w->cap - w->pos < n) {
        return false;
    }
    if (n > 0) {
        memcpy(w->p + w->pos, src, n);
    }
    w->pos += n;
    return true;
}

static bool put_int(wr_t *w, uint64_t v, size_t n)
{
    uint8_t tmp[8];
    put_le(tmp, v, n);
    return put(w, tmp, n);
}

static dsp_ble_result_t enc_layout(const dsp_ble_layout_t *l, wr_t *w, const uint8_t *base);

static dsp_ble_result_t enc_field(const dsp_ble_field_t *f, wr_t *w, const uint8_t *base)
{
    const uint8_t *src = base + f->offset;
    bool ok = true;
    switch (f->kind) {
    case DSP_BLEF_U8:
        ok = put(w, src, 1);
        break;
    case DSP_BLEF_ENUM:
        if (!enum_ok(f, *src)) {
            return DSP_BLE_ERR_BAD_ENUM;
        }
        ok = put(w, src, 1);
        break;
    case DSP_BLEF_MASK8:
        if ((*src & ~f->allowed) != 0) {
            return DSP_BLE_ERR_BAD_ENUM;
        }
        ok = put(w, src, 1);
        break;
    case DSP_BLEF_BOOL: {
        uint8_t v = *(const bool *)src ? 1 : 0;
        ok = put(w, &v, 1);
        break;
    }
    case DSP_BLEF_U16:
    case DSP_BLEF_MASK16: {
        uint16_t v;
        memcpy(&v, src, sizeof v);
        if (f->kind == DSP_BLEF_MASK16 && (v & ~f->allowed) != 0) {
            return DSP_BLE_ERR_BAD_ENUM;
        }
        ok = put_int(w, v, 2);
        break;
    }
    case DSP_BLEF_U32:
    case DSP_BLEF_MASK32:
    case DSP_BLEF_CONST32: {
        uint32_t v;
        memcpy(&v, src, sizeof v);
        if ((f->kind == DSP_BLEF_MASK32 && (v & ~f->allowed) != 0) ||
            (f->kind == DSP_BLEF_CONST32 && v != f->allowed)) {
            return DSP_BLE_ERR_BAD_ENUM;
        }
        ok = put_int(w, v, 4);
        break;
    }
    case DSP_BLEF_U64:
    case DSP_BLEF_I64: {
        uint64_t v;
        memcpy(&v, src, sizeof v);
        ok = put_int(w, v, 8);
        break;
    }
    case DSP_BLEF_DATE: {
        dsp_ble_date_t d;
        memcpy(&d, src, sizeof d);
        bool must_be_zero = f->cond_offset != NONE16 && !*(const bool *)(base + f->cond_offset);
        if (must_be_zero ? !date_zero(&d) : !date_ok(&d)) {
            return DSP_BLE_ERR_BAD_DATE;
        }
        ok = put_int(w, d.year, 2) && put(w, &d.month, 1) && put(w, &d.day, 1);
        break;
    }
    case DSP_BLEF_RESERVED: {
        static const uint8_t zeros[16];
        ok = put(w, zeros, f->min);
        break;
    }
    case DSP_BLEF_BYTES:
        ok = put(w, src, f->min);
        break;
    case DSP_BLEF_BLOB: {
        const dsp_ble_blob_t *b = (const dsp_ble_blob_t *)(const void *)src;
        if (b->len > sizeof b->b) {
            return DSP_BLE_ERR_TOO_LARGE;
        }
        ok = put(w, b->b, b->len);
        break;
    }
    case DSP_BLEF_STR: {
        const dsp_ble_str_t *s = (const dsp_ble_str_t *)(const void *)src;
        if (!str_ok(f, s)) {
            return DSP_BLE_ERR_BAD_STRING;
        }
        ok = put(w, &s->len, 1) && put(w, s->b, s->len);
        break;
    }
    case DSP_BLEF_RECORD:
        return enc_layout(f->sub, w, src);
    case DSP_BLEF_LIST: {
        uint8_t n = *src;
        if (n > f->max) {
            return DSP_BLE_ERR_BAD_ENUM;
        }
        if (!put(w, &n, 1)) {
            return DSP_BLE_ERR_BUFFER;
        }
        for (size_t i = 0; i < n; i++) {
            dsp_ble_result_t e = enc_layout(f->sub, w, src + f->items_offset + i * f->elem_size);
            if (e != DSP_BLE_OK) {
                return e;
            }
        }
        break;
    }
    default:
        return DSP_BLE_ERR_ARG;
    }
    return ok ? DSP_BLE_OK : DSP_BLE_ERR_BUFFER;
}

static dsp_ble_result_t enc_layout(const dsp_ble_layout_t *l, wr_t *w, const uint8_t *base)
{
    for (size_t i = 0; i < l->count; i++) {
        dsp_ble_result_t e = enc_field(&l->fields[i], w, base);
        if (e != DSP_BLE_OK) {
            return e;
        }
    }
    return DSP_BLE_OK;
}

/* ---- Frames ----------------------------------------------------------------------------------- */

static dsp_ble_result_t check_flags(uint8_t type, uint8_t flags, const type_entry_t **entry)
{
    if ((flags & 0xF0u) != 0) {
        return DSP_BLE_ERR_BAD_FLAGS;
    }
    uint8_t f = (uint8_t)(flags & (DSP_BLE_F_RESPONSE | DSP_BLE_F_ERROR | DSP_BLE_F_EVENT));
    if (f != 0 && f != DSP_BLE_F_RESPONSE && f != (DSP_BLE_F_RESPONSE | DSP_BLE_F_ERROR) &&
        f != DSP_BLE_F_EVENT) {
        return DSP_BLE_ERR_BAD_FLAGS;
    }
    const type_entry_t *t = find_type(type);
    if (t == NULL) {
        return DSP_BLE_ERR_UNKNOWN_TYPE;
    }
    if (f == 0 && t->req == NULL) {
        return DSP_BLE_ERR_NOT_A_REQUEST;
    }
    if ((f == DSP_BLE_F_RESPONSE || f == (DSP_BLE_F_RESPONSE | DSP_BLE_F_ERROR)) && t->rsp == NULL) {
        return DSP_BLE_ERR_BAD_FLAGS;
    }
    if (f == DSP_BLE_F_EVENT && t->evt == NULL) {
        return DSP_BLE_ERR_BAD_FLAGS;
    }
    *entry = t;
    return DSP_BLE_OK;
}

dsp_ble_result_t dsp_ble_parse_header(const uint8_t *frame, size_t len, dsp_ble_header_t *h,
                                      const uint8_t **body)
{
    if (frame == NULL || h == NULL || body == NULL) {
        return DSP_BLE_ERR_ARG;
    }
    if (len < DSP_BLE_HEADER_LEN) {
        return DSP_BLE_ERR_TRUNCATED;
    }
    h->major = frame[0];
    h->minor = frame[1];
    h->type = frame[2];
    h->flags = frame[3];
    h->txn = (uint16_t)get_le(frame + 4, 2);
    h->len = (uint16_t)get_le(frame + 6, 2);
    if (h->major != DSP_BLE_MAJOR) {
        return DSP_BLE_ERR_BAD_MAJOR;
    }
    if ((size_t)h->len != len - DSP_BLE_HEADER_LEN) {
        return DSP_BLE_ERR_LENGTH_MISMATCH;
    }
    if (len > DSP_BLE_FRAME_MAX) {
        return DSP_BLE_ERR_TOO_LARGE;
    }
    const type_entry_t *t = NULL;
    dsp_ble_result_t e = check_flags(h->type, h->flags, &t);
    if (e != DSP_BLE_OK) {
        return e;
    }
    if (h->flags & DSP_BLE_F_ENCRYPTED) {
        if (h->len < DSP_BLE_TAG_LEN) {
            return DSP_BLE_ERR_BAD_ENCRYPTED;
        }
    } else if (h->len > DSP_BLE_PLAIN_BODY_MAX) {
        return DSP_BLE_ERR_TOO_LARGE;
    }
    *body = frame + DSP_BLE_HEADER_LEN;
    return DSP_BLE_OK;
}

dsp_ble_result_t dsp_ble_decode_body(const dsp_ble_header_t *h, const uint8_t *body, size_t len,
                                     uint8_t my_minor, dsp_ble_body_t *out)
{
    if (h == NULL || (body == NULL && len > 0) || out == NULL) {
        return DSP_BLE_ERR_ARG;
    }
    if (len > DSP_BLE_PLAIN_BODY_MAX) {
        return DSP_BLE_ERR_TOO_LARGE;
    }
    const dsp_ble_layout_t *l = dsp_ble_layout_for(h->type, h->flags, NULL);
    if (l == NULL) {
        return DSP_BLE_ERR_BAD_FLAGS;
    }
    memset(out, 0, sizeof *out);
    rd_t r = {body, len, 0};
    dsp_ble_result_t e = dec_layout(l, &r, (uint8_t *)out);
    if (e != DSP_BLE_OK) {
        return e;
    }
    if (r.pos < len && h->minor <= my_minor) {
        return DSP_BLE_ERR_TRAILING;
    }
    return DSP_BLE_OK;
}

dsp_ble_result_t dsp_ble_decode(const uint8_t *frame, size_t len, uint8_t my_minor, dsp_ble_msg_t *out)
{
    if (out == NULL) {
        return DSP_BLE_ERR_ARG;
    }
    const uint8_t *body = NULL;
    dsp_ble_result_t e = dsp_ble_parse_header(frame, len, &out->h, &body);
    if (e != DSP_BLE_OK || (out->h.flags & DSP_BLE_F_ENCRYPTED)) {
        return e;
    }
    return dsp_ble_decode_body(&out->h, body, out->h.len, my_minor, &out->b);
}

dsp_ble_result_t dsp_ble_encode_body(const dsp_ble_msg_t *msg, uint8_t *out, size_t cap, size_t *len)
{
    if (msg == NULL || out == NULL || len == NULL) {
        return DSP_BLE_ERR_ARG;
    }
    const type_entry_t *t = NULL;
    dsp_ble_result_t e = check_flags(msg->h.type, (uint8_t)(msg->h.flags & ~DSP_BLE_F_ENCRYPTED), &t);
    if (e != DSP_BLE_OK) {
        return e;
    }
    const dsp_ble_layout_t *l = dsp_ble_layout_for(msg->h.type, msg->h.flags, NULL);
    if (l == NULL) {
        return DSP_BLE_ERR_BAD_FLAGS;
    }
    wr_t w = {out, cap < DSP_BLE_PLAIN_BODY_MAX ? cap : DSP_BLE_PLAIN_BODY_MAX + 1, 0};
    e = enc_layout(l, &w, (const uint8_t *)&msg->b);
    if (e == DSP_BLE_ERR_BUFFER && cap > DSP_BLE_PLAIN_BODY_MAX) {
        return DSP_BLE_ERR_TOO_LARGE;
    }
    if (e != DSP_BLE_OK) {
        return e;
    }
    if (w.pos > DSP_BLE_PLAIN_BODY_MAX) {
        return DSP_BLE_ERR_TOO_LARGE;
    }
    *len = w.pos;
    return DSP_BLE_OK;
}

dsp_ble_result_t dsp_ble_encode(const dsp_ble_msg_t *msg, uint8_t *out, size_t cap, size_t *len)
{
    if (msg == NULL || out == NULL || len == NULL || cap < DSP_BLE_HEADER_LEN ||
        (msg->h.flags & DSP_BLE_F_ENCRYPTED)) {
        return DSP_BLE_ERR_ARG;
    }
    size_t body_len = 0;
    dsp_ble_result_t e = dsp_ble_encode_body(msg, out + DSP_BLE_HEADER_LEN, cap - DSP_BLE_HEADER_LEN, &body_len);
    if (e != DSP_BLE_OK) {
        return e;
    }
    out[0] = msg->h.major;
    out[1] = msg->h.minor;
    out[2] = msg->h.type;
    out[3] = msg->h.flags;
    put_le(out + 4, msg->h.txn, 2);
    put_le(out + 6, body_len, 2);
    if (msg->h.major != DSP_BLE_MAJOR) {
        return DSP_BLE_ERR_BAD_MAJOR;
    }
    *len = DSP_BLE_HEADER_LEN + body_len;
    return DSP_BLE_OK;
}

/* ---- Negotiation, Info, advertising ------------------------------------------------------------- */

bool dsp_ble_negotiate(uint8_t dev_min, uint8_t dev_max, uint8_t cli_min, uint8_t cli_max, uint8_t *major)
{
    if (major == NULL || dev_min == 0 || cli_min == 0 || dev_min > dev_max || cli_min > cli_max) {
        return false;
    }
    uint8_t lo = dev_min > cli_min ? dev_min : cli_min;
    uint8_t hi = dev_max < cli_max ? dev_max : cli_max;
    if (hi < lo) {
        return false;
    }
    *major = hi;
    return true;
}

static const uint8_t INFO_MAGIC[4] = {'D', 'S', 'P', '2'};

dsp_ble_result_t dsp_ble_info_encode(const dsp_ble_info_t *in, uint8_t out[DSP_BLE_INFO_LEN])
{
    if (in == NULL || out == NULL) {
        return DSP_BLE_ERR_ARG;
    }
    if (in->claim_state > 1 || (in->flags & ~0x01u) != 0 || (in->caps & ~0x1Fu) != 0) {
        return DSP_BLE_ERR_BAD_ENUM;
    }
    memset(out, 0, DSP_BLE_INFO_LEN);
    memcpy(out, INFO_MAGIC, 4);
    out[4] = in->proto_min;
    out[5] = in->proto_max;
    out[6] = in->claim_state;
    out[7] = in->flags;
    put_le(out + 8, in->caps, 4);
    out[12] = in->fw_major;
    out[13] = in->fw_minor;
    put_le(out + 14, in->fw_patch, 2);
    put_le(out + 16, in->device_tag, 2);
    return DSP_BLE_OK;
}

dsp_ble_result_t dsp_ble_info_decode(const uint8_t *buf, size_t len, dsp_ble_info_t *out)
{
    if (buf == NULL || out == NULL) {
        return DSP_BLE_ERR_ARG;
    }
    if (len < DSP_BLE_INFO_LEN) {
        return DSP_BLE_ERR_TRUNCATED;
    }
    if (len > DSP_BLE_INFO_LEN) {
        return DSP_BLE_ERR_LENGTH_MISMATCH;
    }
    if (memcmp(buf, INFO_MAGIC, 4) != 0) {
        return DSP_BLE_ERR_BAD_MAGIC;
    }
    for (size_t i = 18; i < DSP_BLE_INFO_LEN; i++) {
        if (buf[i] != 0) {
            return DSP_BLE_ERR_BAD_RESERVED;
        }
    }
    dsp_ble_info_t v = {buf[4], buf[5], buf[6], buf[7], (uint32_t)get_le(buf + 8, 4), buf[12], buf[13],
                        (uint16_t)get_le(buf + 14, 2), (uint16_t)get_le(buf + 16, 2)};
    if (v.claim_state > 1 || (v.flags & ~0x01u) != 0 || (v.caps & ~0x1Fu) != 0) {
        return DSP_BLE_ERR_BAD_ENUM;
    }
    *out = v;
    return DSP_BLE_OK;
}

dsp_ble_result_t dsp_ble_advert_encode(const dsp_ble_advert_t *in, uint8_t out[DSP_BLE_ADVERT_LEN])
{
    if (in == NULL || out == NULL) {
        return DSP_BLE_ERR_ARG;
    }
    if ((in->flags & ~0x03u) != 0) {
        return DSP_BLE_ERR_BAD_ENUM;
    }
    out[0] = in->proto_major;
    out[1] = in->flags;
    put_le(out + 2, in->device_tag, 2);
    return DSP_BLE_OK;
}

dsp_ble_result_t dsp_ble_advert_decode(const uint8_t *buf, size_t len, dsp_ble_advert_t *out)
{
    if (buf == NULL || out == NULL) {
        return DSP_BLE_ERR_ARG;
    }
    if (len != DSP_BLE_ADVERT_LEN) {
        return len < DSP_BLE_ADVERT_LEN ? DSP_BLE_ERR_TRUNCATED : DSP_BLE_ERR_LENGTH_MISMATCH;
    }
    if ((buf[1] & ~0x03u) != 0) {
        return DSP_BLE_ERR_BAD_ENUM;
    }
    out->proto_major = buf[0];
    out->flags = buf[1];
    out->device_tag = (uint16_t)get_le(buf + 2, 2);
    return DSP_BLE_OK;
}

/* ---- Fragmentation ------------------------------------------------------------------------------ */

size_t dsp_ble_frag_count(size_t frame_len, uint16_t mtu)
{
    if (mtu < DSP_BLE_ATT_MTU_MIN || frame_len == 0) {
        return 0;
    }
    size_t data = (size_t)mtu - 4u;
    size_t n = (frame_len + data - 1) / data;
    return n <= DSP_BLE_FRAG_MAX ? n : 0;
}

size_t dsp_ble_frag_get(const uint8_t *frame, size_t frame_len, uint16_t mtu, size_t index, uint8_t *out,
                        size_t cap)
{
    size_t n = dsp_ble_frag_count(frame_len, mtu);
    if (frame == NULL || out == NULL || index >= n) {
        return 0;
    }
    size_t data = (size_t)mtu - 4u;
    size_t start = index * data;
    size_t chunk = frame_len - start < data ? frame_len - start : data;
    if (cap < chunk + 1) {
        return 0;
    }
    out[0] = (uint8_t)(index | (index == 0 ? 0x80u : 0u) | (index == n - 1 ? 0x40u : 0u));
    memcpy(out + 1, frame + start, chunk);
    return chunk + 1;
}

void dsp_ble_reasm_init(dsp_ble_reasm_t *r)
{
    if (r != NULL) {
        memset(r, 0, sizeof *r);
    }
}

static dsp_ble_result_t reasm_fail(dsp_ble_reasm_t *r, dsp_ble_result_t e)
{
    r->active = false;
    r->len = 0;
    r->next_index = 0;
    return e;
}

dsp_ble_result_t dsp_ble_reasm_push(dsp_ble_reasm_t *r, const uint8_t *frag, size_t len, uint32_t now_ms,
                                    bool *complete)
{
    if (r == NULL || complete == NULL || (frag == NULL && len > 0)) {
        return DSP_BLE_ERR_ARG;
    }
    *complete = false;
    if (len == 0) {
        return reasm_fail(r, DSP_BLE_ERR_FRAG_EMPTY);
    }
    bool timed_out = false;
    if (r->active && (uint32_t)(now_ms - r->last_ms) > DSP_BLE_REASM_TIMEOUT_MS) {
        (void)reasm_fail(r, DSP_BLE_OK);
        timed_out = true;
    }
    uint8_t hdr = frag[0];
    bool first = (hdr & 0x80u) != 0;
    bool last = (hdr & 0x40u) != 0;
    uint8_t index = (uint8_t)(hdr & 0x3Fu);
    if (first) {
        r->active = true;
        r->len = 0;
        r->next_index = 0;
    } else if (!r->active) {
        return reasm_fail(r, timed_out ? DSP_BLE_ERR_FRAG_TIMEOUT : DSP_BLE_ERR_FRAG_NO_FIRST);
    }
    if (index != r->next_index) {
        return reasm_fail(r, DSP_BLE_ERR_FRAG_INDEX);
    }
    size_t data = len - 1;
    if (data == 0) {
        return reasm_fail(r, DSP_BLE_ERR_FRAG_EMPTY);
    }
    if (r->len + data > DSP_BLE_FRAME_MAX) {
        return reasm_fail(r, DSP_BLE_ERR_FRAG_OVERFLOW);
    }
    memcpy(r->buf + r->len, frag + 1, data);
    r->len += data;
    r->next_index++;
    r->last_ms = now_ms;
    if (last) {
        r->active = false;
        r->next_index = 0;
        *complete = true;
    } else if (r->next_index >= DSP_BLE_FRAG_MAX) {
        return reasm_fail(r, DSP_BLE_ERR_FRAG_INDEX);
    }
    return DSP_BLE_OK;
}
