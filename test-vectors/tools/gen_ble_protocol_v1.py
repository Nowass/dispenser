#!/usr/bin/env python3
"""Generator for test-vectors/ble_protocol_v1.txt (Dispenser V2 BLE protocol v1).

Independent of both implementations under test (firmware dsp_bleproto, Android
cz.nowass.dispenser.ble.protocol): the schema below is transcribed from docs/BLE_PROTOCOL.md and
this script encodes the vectors from it. A vector that an implementation fails is a contract
bug to investigate, never a value to regenerate from that implementation.

Usage: python3 -I gen_ble_protocol_v1.py > ../ble_protocol_v1.txt
Deterministic output (no randomness, no time).
"""
import struct
import sys

# ---- Schema (docs/BLE_PROTOCOL.md sections 5 and 7) --------------------------------------
# Field: (name, kind, arg). Kinds: u8 u16 u32 u64 i64 bool date reserved:n bytes:n blob
# str:(min,max,strkind) enum:(values) mask8/mask16/mask32:(allowed bits) const:value(u32) list:(max, element_fields)
# record:fields

STATUS = [("readiness", "u8", None), ("dosing_flags", "u8", None), ("gates", "u32", None),
          ("pump_problems", "u32", None), ("capacity_ul", "u32", None), ("remaining_ul", "u32", None),
          ("low_percent", "u8", None), ("estimate_uncertain", "bool", None),
          ("clock_trust", "enum", (0, 1, 2)), ("clock_reason", "u8", None), ("device_utc", "i64", None),
          ("next_due_known", "bool", None), ("next_due_at", "i64", None), ("pending_notes", "u8", None),
          ("highest_severity", "enum", (0, 1, 2)), ("storage_ok", "bool", None),
          ("session_seconds_left", "u16", None), ("battery_percent", "enum", tuple(range(101)) + (255,)),
          ("manual_floor", "u64", None), ("reserved", "reserved", 5)]
DIAGNOSTICS = [("notify_storage_ok", "bool", None), ("tx_diag", "u8", None), ("tx_diag_status", "u16", None),
               ("rejections_unread", "u8", None), ("rejections_total", "u16", None),
               ("clock_trust", "enum", (0, 1, 2)), ("clock_reason", "u8", None), ("last_sync_utc", "i64", None),
               ("last_wake_reason", "u8", None), ("reserved", "reserved", 6)]
CONFIG = [("revision", "u32", None), ("reservoir_capacity_ul", "u32", None),
          ("reservoir_remaining_ul", "u32", None), ("low_stock_percent", "u8", None),
          ("time_sync_interval_h", "u16", None), ("notify_mask", "u8", None),
          ("pump_flow_ul_per_s", "u32", None), ("calibrated_at", "i64", None), ("max_dose_ul", "u32", None),
          ("max_daily_ul", "u32", None), ("max_runtime_ms", "u32", None), ("min_interval_s", "u32", None),
          ("hw_max_runtime_ms", "u32", None)]
SCHEDULE = [("id", "str", (1, 64, "id")), ("revision", "u32", None), ("enabled", "bool", None),
            ("amount_ul", "u32", None), ("start_date", "date", None), ("hour", "enum", tuple(range(24))),
            ("minute", "enum", tuple(range(60))), ("kind", "enum", (0, 1, 2)), ("interval", "u32", None),
            ("weekday_mask", "mask8", 0x7F), ("has_end", "bool", None), ("end_date", "date", None),
            ("name", "str", (0, 32, "text"))]
# GET_BATTERY (section 7.7, firmware >= 0.2.0)
BATTERY = [("state", "enum", (0, 1, 2, 3, 4, 5)), ("battery_mv", "u16", None), ("age_s", "u32", None)]
ERROR = [("code", "enum", tuple(range(1, 21))), ("detail", "u32", None), ("aux", "u32", None)]
# GET_HISTORY event (section 7.6, Phase C)
HIST_EVENT = [("seq", "u32", None), ("type", "enum", (1, 2, 3, 4, 5)), ("result", "enum", (0, 1, 2, 3, 4)),
              ("flags", "mask8", 0x0F), ("at", "i64", None), ("volume_ul", "u32", None),
              ("before_ul", "u32", None), ("after_ul", "u32", None), ("capacity_ul", "u32", None),
              ("schedule_id", "str", (0, 64, "id"))]

# type: (name, request_fields or None, response_fields or None, event_fields or None)
TYPES = {
    0x01: ("HELLO", [("proto_min", "u8", None), ("proto_max", "u8", None)],
           [("proto", "u8", None), ("claim_state", "enum", (0, 1)), ("flags", "mask8", 0x03),
            ("caps", "mask32", 0x1F), ("session_id", "u32", None), ("device_id_hash", "bytes", 8)], None),
    0x02: ("HS", [("purpose", "enum", (1, 2, 3, 4)), ("msg_index", "enum", (0, 1, 2)), ("noise", "blob", None)],
           [("msg_index", "enum", (0, 1, 2)), ("noise", "blob", None)], None),
    0x08: ("ENROLL_RESULT", None, [("slot", "enum", (0, 1, 2, 3))], None),
    0x10: ("GET_STATUS", [], [("status", "record", STATUS)], None),
    0x11: ("GET_DIAGNOSTICS", [], [("diag", "record", DIAGNOSTICS)], None),
    0x12: ("ACK_REJECTIONS", [], [], None),
    0x13: ("GET_BATTERY", [], [("battery", "record", BATTERY)], None),
    0x20: ("GET_CONFIG", [], [("config", "record", CONFIG)], None),
    0x21: ("SET_CONFIG", [("expected_revision", "u32", None), ("field_mask", "mask16", 0x03FF),
                          ("config", "record", CONFIG)], [("revision", "u32", None)], None),
    0x30: ("LIST_SCHEDULES", [("page", "u8", None)],
           [("set_revision", "u32", None), ("total", "u8", None), ("page", "u8", None),
            ("items", "list", (4, [("id", "str", (1, 64, "id")), ("revision", "u32", None)]))], None),
    0x31: ("GET_SCHEDULE", [("id", "str", (1, 64, "id"))], [("schedule", "record", SCHEDULE)], None),
    0x32: ("CREATE_SCHEDULE", [("schedule", "record", SCHEDULE)], [("set_revision", "u32", None)], None),
    0x33: ("UPDATE_SCHEDULE", [("expected_revision", "u32", None), ("schedule", "record", SCHEDULE)],
           [("revision", "u32", None), ("set_revision", "u32", None)], None),
    0x34: ("DELETE_SCHEDULE", [("id", "str", (1, 64, "id")), ("expected_revision", "u32", None)],
           [("set_revision", "u32", None)], None),
    0x40: ("MANUAL_DOSE", [("seq", "u64", None), ("volume_ul", "u32", None)],
           [("outcome", "enum", (0, 1)), ("volume_ul", "u32", None), ("runtime_ms", "u32", None),
            ("status", "record", STATUS)], None),
    0x41: ("GET_RESULT", [("seq", "u64", None)], [("state", "enum", tuple(range(7))), ("detail", "u32", None)], None),
    0x42: ("REFILL", [("capacity_ul", "u32", None), ("remaining_ul", "u32", None)], [("status", "record", STATUS)], None),
    0x43: ("SET_READINESS", [("readiness", "enum", (1, 2))], [("status", "record", STATUS)], None),
    0x50: ("SET_TIME", [("phone_utc_ms", "i64", None), ("uncertainty_ms", "u32", None)],
           [("decision", "enum", (0, 1, 2, 3, 4)), ("offset_s", "i64", None)], None),
    0x60: ("WIFI_SET", [("ssid", "str", (1, 32, "raw")), ("passphrase", "str", (0, 63, "wpa"))], [], None),
    0x61: ("WIFI_GET", [], [("ssid", "str", (0, 32, "raw")), ("passphrase_set", "bool", None),
                            ("last_result", "enum", (0, 1, 2, 3, 4, 5))], None),
    0x62: ("WIFI_CLEAR", [], [], None),
    0x70: ("PHONES_LIST", [], [("items", "list", (4, [("slot", "enum", (0, 1, 2, 3)), ("name", "str", (0, 24, "text")),
                                                       ("enrolled_at", "i64", None), ("last_seen", "i64", None),
                                                       ("is_self", "bool", None)]))], None),
    0x71: ("PHONE_REMOVE", [("slot", "enum", (0, 1, 2, 3))], [], None),
    0x72: ("INVITE_CREATE", [("name_hint", "str", (0, 24, "text"))], [("token", "bytes", 16), ("expires_s", "u16", None)], None),
    0x73: ("INVITE_CANCEL", [], [], None),
    0x74: ("RECOVERY_CANCEL", [], [], None),
    0x7D: ("FACTORY_RESET", [("confirm", "const", 0x54455352)], [], None),
    0x7E: ("END_SESSION", [], [], None),
    0x90: ("GET_HISTORY", [("before_seq", "u32", None), ("max", "enum", (1, 2, 3, 4))],
           [("newest_seq", "u32", None), ("oldest_seq", "u32", None), ("flags", "mask8", 0x01),
            ("items", "list", (4, HIST_EVENT))], None),
    # Firmware update (sections 7.8, 10.8, firmware >= 0.3.0)
    0xB0: ("GET_UPDATE_STATUS", [], [("running_version", "str", (1, 31, "id")), ("capable", "bool", None),
                                     ("last_result", "u8", None), ("last_version", "str", (0, 17, "id"))], None),
    0xB1: ("START_UPDATE", [("version", "str", (5, 17, "id")), ("tag", "str", (1, 40, "id")), ("size", "u32", None),
                            ("sha256", "bytes", 32)], [("verdict", "u8", None)], None),
    0x80: ("EV_SESSION_CLOSING", None, None, [("reason", "enum", (1, 2, 3, 4)), ("seconds_left", "u16", None)]),
    0x81: ("EV_SUSPENDING_FOR_DOSE", None, None, [("seconds_until", "u16", None)]),
    0x82: ("EV_BUSY", None, None, []),
    0x83: ("EV_CONFIRM_ON_DEVICE", None, None, [("action", "enum", (1,)), ("seconds", "u8", None)]),
}

RESPONSE, ERRORF, EVENT, ENCRYPTED = 1, 2, 4, 8
FMT = {"u8": "<B", "u16": "<H", "u32": "<I", "u64": "<Q", "i64": "<q"}


# ---- Encoder ------------------------------------------------------------------------------
def enc_field(kind, arg, v):
    if kind in FMT:
        return struct.pack(FMT[kind], v)
    if kind in ("enum", "bool", "mask8"):
        return struct.pack("<B", v)
    if kind == "mask16":
        return struct.pack("<H", v)
    if kind == "mask32":
        return struct.pack("<I", v)
    if kind == "const":
        return struct.pack("<I", v)
    if kind == "date":
        return struct.pack("<HBB", *v)
    if kind == "reserved":
        return bytes(arg)
    if kind == "bytes":
        assert len(v) == arg
        return bytes(v)
    if kind == "blob":
        return bytes(v)
    if kind == "str":
        return bytes([len(v)]) + bytes(v)
    if kind == "record":
        return enc_fields(arg, v)
    if kind == "list":
        out = bytes([len(v)])
        for item in v:
            out += enc_fields(arg[1], item)
        return out
    raise ValueError(kind)


def enc_fields(fields, values):
    out = b""
    for name, kind, arg in fields:
        if kind == "reserved":
            out += bytes(arg)
            continue
        out += enc_field(kind, arg, values[name])
    return out


def frame(t, flags, txn, body, major=1, minor=0):
    return struct.pack("<BBBBHH", major, minor, t, flags, txn, len(body)) + body


# ---- Field text (canonical, shared by both consumers) -----------------------------------
def fmt_value(kind, v):
    if kind in ("bytes", "blob", "str"):
        return "x:" + bytes(v).hex() if len(v) else "x:"
    if kind == "date":
        return "%04d-%02d-%02d" % v
    return str(v)


def fields_text(fields, values, prefix=""):
    out = []
    for name, kind, arg in fields:
        key = prefix + name
        if kind == "reserved":
            continue
        if kind == "record":
            out += fields_text(arg, values[name], key + ".")
        elif kind == "list":
            out.append("%s.n=%d" % (key, len(values[name])))
            for i, item in enumerate(values[name]):
                out += fields_text(arg[1], item, "%s.%d." % (key, i))
        else:
            out.append("%s=%s" % (key, fmt_value(kind, values[name])))
    return out


# ---- Vector values -----------------------------------------------------------------------
def b(s):
    return list(s.encode("utf-8"))


STATUS_V = dict(readiness=1, dosing_flags=0, gates=0, pump_problems=0, capacity_ul=100000,
                remaining_ul=73500, low_percent=20, estimate_uncertain=0, clock_trust=1, clock_reason=0,
                device_utc=1795158000, next_due_known=1, next_due_at=1795161600, pending_notes=2,
                highest_severity=1, storage_ok=1, session_seconds_left=171, battery_percent=255,
                manual_floor=43)
STATUS_EXTREME = dict(readiness=2, dosing_flags=7, gates=0x1FF, pump_problems=0x7F, capacity_ul=4294967295,
                      remaining_ul=0, low_percent=99, estimate_uncertain=1, clock_trust=0, clock_reason=4,
                      device_utc=-1, next_due_known=0, next_due_at=0, pending_notes=16, highest_severity=2,
                      storage_ok=0, session_seconds_left=65535, battery_percent=100,
                      manual_floor=0xFFFFFFFFFFFFFFFF)
CONFIG_V = dict(revision=7, reservoir_capacity_ul=100000, reservoir_remaining_ul=73500, low_stock_percent=20,
                time_sync_interval_h=24, notify_mask=0x1F, pump_flow_ul_per_s=1200, calibrated_at=1795071600,
                max_dose_ul=20000, max_daily_ul=60000, max_runtime_ms=30000, min_interval_s=60,
                hw_max_runtime_ms=60000)
SCHED_V = dict(id=b("3f2b8c1e-9d4a-4f6b-8e21-7a5c0d9e1f42"), revision=3, enabled=1, amount_ul=4000,
               start_date=(2026, 11, 2), hour=8, minute=0, kind=2, interval=1, weekday_mask=0x15, has_end=1,
               end_date=(2027, 3, 31), name=b("Ranní dávka CO₂"))
SCHED_MIN = dict(id=b("a"), revision=1, enabled=0, amount_ul=1, start_date=(2000, 1, 1), hour=0, minute=0,
                 kind=0, interval=1, weekday_mask=0, has_end=0, end_date=(0, 0, 0), name=[])
DIAG_V = dict(notify_storage_ok=1, tx_diag=0x05, tx_diag_status=503, rejections_unread=1, rejections_total=3,
              clock_trust=2, clock_reason=0, last_sync_utc=1795100000, last_wake_reason=1)
NOISE_MSG = list(range(0x20, 0x20 + 48))

# (label, type, flags, txn, values, minor)
MSGS = [
    ("hello_request", 0x01, 0, 7, dict(proto_min=1, proto_max=1)),
    ("hello_request_future_range", 0x01, 0, 1, dict(proto_min=1, proto_max=3)),
    ("hello_response", 0x01, RESPONSE, 7, dict(proto=1, claim_state=1, flags=0, caps=0x03,
                                               session_id=0xA1B2C3D4, device_id_hash=list(range(1, 9)))),
    ("hello_response_unclaimed_recovery", 0x01, RESPONSE, 2, dict(proto=1, claim_state=0, flags=1, caps=0,
                                                                  session_id=0, device_id_hash=[0xFF] * 8)),
    ("hello_response_credentials_damaged", 0x01, RESPONSE, 5, dict(proto=1, claim_state=1, flags=3, caps=0x03,
                                                                   session_id=7, device_id_hash=[0x5A] * 8)),
    ("hs_request_claim", 0x02, 0, 3, dict(purpose=2, msg_index=0, noise=NOISE_MSG)),
    ("hs_request_empty_noise", 0x02, 0, 4, dict(purpose=1, msg_index=2, noise=[])),
    ("hs_response", 0x02, RESPONSE, 3, dict(msg_index=1, noise=list(range(0x80, 0x80 + 96)))),
    ("enroll_result", 0x08, RESPONSE, 5, dict(slot=3)),
    ("get_status_request", 0x10, 0, 9, dict()),
    ("get_status_response", 0x10, RESPONSE, 9, dict(status=STATUS_V)),
    ("get_status_response_extremes", 0x10, RESPONSE, 0xFFFF, dict(status=STATUS_EXTREME)),
    ("get_diagnostics_response", 0x11, RESPONSE, 10, dict(diag=DIAG_V)),
    ("ack_rejections_request", 0x12, 0, 11, dict()),
    ("ack_rejections_response", 0x12, RESPONSE, 11, dict()),
    ("get_battery_request", 0x13, 0, 41, dict()),
    ("get_battery_response", 0x13, RESPONSE, 41, dict(battery=dict(state=0, battery_mv=3970, age_s=12))),
    ("get_battery_response_unavailable", 0x13, RESPONSE, 42, dict(battery=dict(state=3, battery_mv=0, age_s=0))),
    ("get_battery_response_extremes", 0x13, RESPONSE, 0xFFFF, dict(battery=dict(state=5, battery_mv=65535, age_s=4294967295))),
    ("get_update_status_request", 0xB0, 0, 43, dict()),
    ("get_update_status_response", 0xB0, RESPONSE, 43, dict(running_version=b("0.3.0"), capable=1, last_result=0, last_version=b(""))),
    ("get_update_status_response_rolled_back", 0xB0, RESPONSE, 44, dict(running_version=b("0.3.0"), capable=1, last_result=2, last_version=b("0.3.1"))),
    ("get_update_status_response_dev", 0xB0, RESPONSE, 0xFFFF, dict(running_version=b("0.3.0-dev"), capable=0, last_result=255, last_version=b("65535.65535.65535"))),
    ("start_update_request", 0xB1, 0, 45, dict(version=b("0.3.1"), tag=b("firmware-v0.3.1"), size=1430528, sha256=bytes(range(32)))),
    ("start_update_response_accepted", 0xB1, RESPONSE, 45, dict(verdict=0)),
    ("start_update_response_dose_soon", 0xB1, RESPONSE, 46, dict(verdict=4)),
    ("get_config_response", 0x20, RESPONSE, 12, dict(config=CONFIG_V)),
    ("set_config_request", 0x21, 0, 13, dict(expected_revision=7, field_mask=0x03F0, config=CONFIG_V)),
    ("set_config_response", 0x21, RESPONSE, 13, dict(revision=8)),
    ("list_schedules_request", 0x30, 0, 14, dict(page=0)),
    ("list_schedules_response", 0x30, RESPONSE, 14, dict(set_revision=21, total=6, page=0, items=[
        dict(id=b("3f2b8c1e-9d4a-4f6b-8e21-7a5c0d9e1f42"), revision=3), dict(id=b("evening"), revision=1),
        dict(id=b("a.b_c-9"), revision=4294967295), dict(id=b("x" * 64), revision=2)])),
    ("list_schedules_response_empty", 0x30, RESPONSE, 15, dict(set_revision=0, total=0, page=0, items=[])),
    ("get_schedule_request", 0x31, 0, 16, dict(id=b("evening"))),
    ("get_schedule_response", 0x31, RESPONSE, 16, dict(schedule=SCHED_V)),
    ("create_schedule_request_minimal", 0x32, 0, 17, dict(schedule=SCHED_MIN)),
    ("create_schedule_response", 0x32, RESPONSE, 17, dict(set_revision=22)),
    ("update_schedule_request", 0x33, 0, 18, dict(expected_revision=3, schedule=SCHED_V)),
    ("update_schedule_response", 0x33, RESPONSE, 18, dict(revision=4, set_revision=23)),
    ("delete_schedule_request", 0x34, 0, 19, dict(id=b("evening"), expected_revision=1)),
    ("delete_schedule_response", 0x34, RESPONSE, 19, dict(set_revision=24)),
    ("manual_dose_request", 0x40, 0, 12, dict(seq=0x1122334455667788, volume_ul=2000)),
    ("manual_dose_response", 0x40, RESPONSE, 12, dict(outcome=0, volume_ul=2000, runtime_ms=1667, status=STATUS_V)),
    ("get_result_request", 0x41, 0, 20, dict(seq=18446744073709551615)),
    ("get_result_response", 0x41, RESPONSE, 20, dict(state=6, detail=0)),
    ("refill_request", 0x42, 0, 21, dict(capacity_ul=100000, remaining_ul=100000)),
    ("set_readiness_request", 0x43, 0, 22, dict(readiness=1)),
    ("set_readiness_response", 0x43, RESPONSE, 22, dict(status=STATUS_V)),
    ("set_time_request", 0x50, 0, 23, dict(phone_utc_ms=1795158000123, uncertainty_ms=850)),
    ("set_time_response", 0x50, RESPONSE, 23, dict(decision=0, offset_s=-37)),
    ("wifi_set_request", 0x60, 0, 24, dict(ssid=b("Aquarium Wi-Fi"), passphrase=b("correct horse battery"))),
    ("wifi_set_request_open_binary_ssid", 0x60, 0, 25, dict(ssid=[0x00, 0xFF, 0xC3, 0x28], passphrase=[])),
    ("wifi_set_response", 0x60, RESPONSE, 24, dict()),
    ("wifi_get_response", 0x61, RESPONSE, 26, dict(ssid=b("Aquarium Wi-Fi"), passphrase_set=1, last_result=1)),
    ("wifi_clear_request", 0x62, 0, 27, dict()),
    ("phones_list_response", 0x70, RESPONSE, 28, dict(items=[
        dict(slot=0, name=b("Pixel 8"), enrolled_at=1795000000, last_seen=1795158000, is_self=1),
        dict(slot=2, name=[], enrolled_at=1795100000, last_seen=0, is_self=0)])),
    ("phone_remove_request", 0x71, 0, 29, dict(slot=2)),
    ("invite_create_request", 0x72, 0, 30, dict(name_hint=b("Tablet"))),
    ("invite_create_response", 0x72, RESPONSE, 30, dict(token=list(range(0xA0, 0xB0)), expires_s=600)),
    ("invite_cancel_request", 0x73, 0, 31, dict()),
    ("recovery_cancel_request", 0x74, 0, 32, dict()),
    ("factory_reset_request", 0x7D, 0, 33, dict(confirm=0x54455352)),
    ("end_session_request", 0x7E, 0, 34, dict()),
    ("end_session_response", 0x7E, RESPONSE, 34, dict()),
    ("get_history_request_newest", 0x90, 0, 35, dict(before_seq=0, max=4)),
    ("get_history_request_page", 0x90, 0, 36, dict(before_seq=4294967295, max=1)),
    ("get_history_response", 0x90, RESPONSE, 35, dict(newest_seq=137, oldest_seq=74, flags=0, items=[
        dict(seq=137, type=1, result=0, flags=0x09, at=1795158000, volume_ul=5000, before_ul=100000,
             after_ul=95000, capacity_ul=100000, schedule_id=b("3f2b8c1e-9d4a-4f6b-8e21-7a5c0d9e1f42")),
        dict(seq=136, type=4, result=0, flags=0x00, at=1795100000, volume_ul=0, before_ul=20000,
             after_ul=100000, capacity_ul=100000, schedule_id=[]),
        dict(seq=135, type=2, result=3, flags=0x07, at=-1, volume_ul=4294967295, before_ul=0,
             after_ul=0, capacity_ul=4294967295, schedule_id=[]),
        dict(seq=4294967295, type=1, result=4, flags=0x05, at=0, volume_ul=1, before_ul=4294967295,
             after_ul=4294967295, capacity_ul=0, schedule_id=b("x" * 64))])),
    ("get_history_response_empty_faults", 0x90, RESPONSE, 37, dict(newest_seq=0, oldest_seq=0, flags=1, items=[])),
    ("error_unavailable_history", 0x90, RESPONSE | ERRORF, 38, dict(code=20, detail=0, aux=0)),
    ("error_unauthenticated", 0x10, RESPONSE | ERRORF, 8, dict(code=4, detail=0, aux=0)),
    ("error_refused_gates", 0x40, RESPONSE | ERRORF, 12, dict(code=7, detail=0x0C, aux=0x10)),
    ("error_unsupported_version", 0x01, RESPONSE | ERRORF, 1, dict(code=2, detail=0x0101, aux=0)),
    ("error_unavailable_wifi", 0x60, RESPONSE | ERRORF, 40, dict(code=20, detail=0, aux=1)),
    ("error_conflict", 0x33, RESPONSE | ERRORF, 18, dict(code=11, detail=4, aux=0)),
    ("ev_session_closing", 0x80, EVENT, 0, dict(reason=2, seconds_left=30)),
    ("ev_suspending_for_dose", 0x81, EVENT, 0, dict(seconds_until=2)),
    ("ev_busy", 0x82, EVENT, 0, dict()),
    ("ev_confirm_on_device", 0x83, EVENT, 0, dict(action=1, seconds=20)),
]


def layout(t, flags):
    name, req, rsp, evt = TYPES[t]
    if flags & ERRORF:
        return ERROR
    if flags & EVENT:
        return evt
    if flags & RESPONSE:
        return rsp
    return req


def kind_of(flags):
    if flags & ERRORF:
        return "err"
    if flags & EVENT:
        return "evt"
    if flags & RESPONSE:
        return "rsp"
    return "req"


def main():
    w = sys.stdout.write
    w(HEADER)
    for label, t, flags, txn, values in MSGS:
        fields = layout(t, flags)
        body = enc_fields(fields, values)
        assert len(body) <= 496, label
        fr = frame(t, flags, txn, body)
        toks = fields_text(fields, values)
        w("msg name=%s kind=%s type=0x%02x txn=%d %s frame=%s\n" % (
            label, kind_of(flags), t, txn, " ".join(toks) if toks else "-", fr.hex()))
    w("\n")
    # Encrypted frames: header validation only (body is ciphertext||tag, opaque to the codec).
    w("enc name=encrypted_request_minimum frame=%s\n" % frame(0x40, ENCRYPTED, 12, bytes(range(16))).hex())
    w("enc name=encrypted_manual_dose_shape frame=%s\n" % frame(0x40, ENCRYPTED, 12, bytes(28)).hex())
    w("enc name=encrypted_event frame=%s\n" % frame(0x80, EVENT | ENCRYPTED, 0, bytes(range(20))).hex())
    w("enc name=encrypted_error frame=%s\n" % frame(0x40, RESPONSE | ERRORF | ENCRYPTED, 12, bytes(25)).hex())
    w("\n")
    for label, hexframe, reason in bad_vectors():
        w("bad name=%s frame=%s expect=%s\n" % (label, hexframe, reason))
    w("\n")
    # Forward compatibility: a newer minor may append fields.
    st = frame(0x10, RESPONSE, 9, enc_fields(STATUS, STATUS_V) + b"\x01\x02\x03", minor=1)
    w("compat name=newer_minor_trailing_ignored my_minor=0 frame=%s accept=1\n" % st.hex())
    st0 = frame(0x10, RESPONSE, 9, enc_fields(STATUS, STATUS_V) + b"\x01", minor=0)
    w("compat name=same_minor_trailing_rejected my_minor=0 frame=%s accept=0\n" % st0.hex())
    old = frame(0x10, RESPONSE, 9, enc_fields(STATUS, STATUS_V) + b"\x01", minor=1)
    w("compat name=receiver_newer_minor_rejects_trailing my_minor=1 frame=%s accept=0\n" % old.hex())
    w("\n")
    for dmin, dmax, cmin, cmax, exp in [(1, 1, 1, 1, "1"), (1, 1, 1, 3, "1"), (1, 2, 1, 3, "2"), (2, 3, 1, 1, "none"),
                                        (1, 1, 2, 4, "none"), (1, 4, 2, 3, "3"), (1, 1, 3, 1, "none")]:
        w("negotiate dev_min=%d dev_max=%d cli_min=%d cli_max=%d expect=%s\n" % (dmin, dmax, cmin, cmax, exp))
    w("\n")
    info_frags()
    w("\n")
    info_records()


def bad_vectors():
    out = []
    sv = enc_fields(STATUS, STATUS_V)
    hello = frame(0x01, 0, 7, b"\x01\x01")
    out.append(("too_short_header", hello[:7].hex(), "truncated"))
    out.append(("len_larger_than_frame", (hello[:6] + struct.pack("<H", 3) + hello[8:]).hex(), "length_mismatch"))
    out.append(("len_smaller_than_frame", (hello[:6] + struct.pack("<H", 1) + hello[8:]).hex(), "length_mismatch"))
    out.append(("major_2", (b"\x02" + hello[1:]).hex(), "bad_major"))
    out.append(("major_0", (b"\x00" + hello[1:]).hex(), "bad_major"))
    out.append(("reserved_flag_bit", frame(0x01, 0x10, 7, b"\x01\x01").hex(), "bad_flags"))
    out.append(("error_without_response", frame(0x10, ERRORF, 8, struct.pack("<BII", 4, 0, 0)).hex(), "bad_flags"))
    out.append(("event_with_response", frame(0x80, EVENT | RESPONSE, 0, b"\x02\x1e\x00").hex(), "bad_flags"))
    out.append(("event_flag_on_request_type", frame(0x10, EVENT, 0, b"").hex(), "bad_flags"))
    out.append(("unknown_type", frame(0x05, 0, 1, b"").hex(), "unknown_type"))
    out.append(("reserved_journal_type", frame(0x91, 0, 1, b"").hex(), "unknown_type"))
    out.append(("history_max_0", frame(0x90, 0, 35, struct.pack("<IB", 0, 0)).hex(), "bad_enum"))
    out.append(("history_max_5", frame(0x90, 0, 35, struct.pack("<IB", 0, 5)).hex(), "bad_enum"))
    out.append(("history_request_truncated", frame(0x90, 0, 35, struct.pack("<I", 0)).hex(), "truncated"))
    hist_ev = struct.pack("<IBBBqIIIIB", 1, 1, 0, 1, 1795158000, 5000, 1, 1, 1, 1) + b"a"
    hist_head = struct.pack("<IIB", 1, 1, 0)
    out.append(("history_type_0", frame(0x90, RESPONSE, 35, hist_head + b"\x01" + hist_ev[:4] + b"\x00" + hist_ev[5:]).hex(), "bad_enum"))
    out.append(("history_type_6", frame(0x90, RESPONSE, 35, hist_head + b"\x01" + hist_ev[:4] + b"\x06" + hist_ev[5:]).hex(), "bad_enum"))
    out.append(("history_result_5", frame(0x90, RESPONSE, 35, hist_head + b"\x01" + hist_ev[:5] + b"\x05" + hist_ev[6:]).hex(), "bad_enum"))
    out.append(("history_event_flag_bit4", frame(0x90, RESPONSE, 35, hist_head + b"\x01" + hist_ev[:6] + b"\x10" + hist_ev[7:]).hex(), "bad_enum"))
    out.append(("history_page_flag_bit1", frame(0x90, RESPONSE, 35, struct.pack("<IIB", 1, 1, 2) + b"\x01" + hist_ev).hex(), "bad_enum"))
    out.append(("history_five_items", frame(0x90, RESPONSE, 35, hist_head + b"\x05" + hist_ev * 5).hex(), "bad_enum"))
    out.append(("history_bad_schedule_id", frame(0x90, RESPONSE, 35, hist_head + b"\x01" + hist_ev[:-1] + b"/").hex(), "bad_string"))
    out.append(("event_type_as_request", frame(0x80, 0, 1, b"\x02\x1e\x00").hex(), "not_a_request"))
    out.append(("enroll_result_as_request", frame(0x08, 0, 1, b"\x00").hex(), "not_a_request"))
    out.append(("request_type_as_event", frame(0x10, EVENT, 0, sv).hex(), "bad_flags"))
    out.append(("body_truncated", frame(0x40, 0, 12, struct.pack("<Q", 1) + b"\xd0\x07").hex(), "truncated"))
    out.append(("body_trailing_same_minor", frame(0x01, 0, 7, b"\x01\x01\x00").hex(), "trailing"))
    out.append(("empty_body_with_byte", frame(0x10, 0, 9, b"\x00").hex(), "trailing"))
    out.append(("bool_value_2", frame(0x10, RESPONSE, 9, sv[:19] + b"\x02" + sv[20:]).hex(), "bad_bool"))
    out.append(("status_reserved_nonzero", frame(0x10, RESPONSE, 9, sv[:-1] + b"\x01").hex(), "bad_reserved"))
    out.append(("readiness_0", frame(0x43, 0, 22, b"\x00").hex(), "bad_enum"))
    out.append(("readiness_3", frame(0x43, 0, 22, b"\x03").hex(), "bad_enum"))
    out.append(("battery_state_6", frame(0x13, RESPONSE, 41, struct.pack("<BHI", 6, 0, 0)).hex(), "bad_enum"))
    out.append(("battery_truncated", frame(0x13, RESPONSE, 41, struct.pack("<BH", 0, 3970)).hex(), "truncated"))
    out.append(("battery_trailing", frame(0x13, RESPONSE, 41, struct.pack("<BHIB", 0, 3970, 0, 0)).hex(), "trailing"))
    upd = b"\x050.3.1" + b"\x0ffirmware-v0.3.1" + struct.pack("<I", 1430528) + bytes(32)
    out.append(("start_update_version_short", frame(0xB1, 0, 45, b"\x040.31" + upd[6:]).hex(), "bad_string"))
    out.append(("start_update_tag_slash", frame(0xB1, 0, 45, upd[:6] + b"\x0ffirmware/v0.3.1" + upd[22:]).hex(), "bad_string"))
    out.append(("start_update_sha_truncated", frame(0xB1, 0, 45, upd[:-1]).hex(), "truncated"))
    out.append(("update_status_capable_2", frame(0xB0, RESPONSE, 43, b"\x050.3.0\x02\x00\x00").hex(), "bad_bool"))
    out.append(("battery_101", frame(0x10, RESPONSE, 9, sv[:44] + b"\x65" + sv[45:]).hex(), "bad_enum"))
    out.append(("error_code_0", frame(0x10, RESPONSE | ERRORF, 8, struct.pack("<BII", 0, 0, 0)).hex(), "bad_enum"))
    out.append(("error_code_21", frame(0x10, RESPONSE | ERRORF, 8, struct.pack("<BII", 21, 0, 0)).hex(), "bad_enum"))
    out.append(("hello_flags_bit2", frame(0x01, RESPONSE, 7, struct.pack("<BBBII", 1, 1, 4, 0, 0) + bytes(8)).hex(), "bad_enum"))
    out.append(("caps_unknown_bit", frame(0x01, RESPONSE, 7, struct.pack("<BBBII", 1, 1, 0, 0x20, 0) + bytes(8)).hex(), "bad_enum"))
    out.append(("set_config_mask_bit10", frame(0x21, 0, 13, struct.pack("<IH", 7, 0x0400) + enc_fields(CONFIG, CONFIG_V)).hex(), "bad_enum"))
    out.append(("factory_reset_wrong_confirm", frame(0x7D, 0, 33, struct.pack("<I", 0x54455353)).hex(), "bad_enum"))
    out.append(("list_n_5", frame(0x30, RESPONSE, 14, struct.pack("<IBBB", 1, 5, 0, 5)).hex(), "bad_enum"))
    out.append(("hs_purpose_0", frame(0x02, 0, 3, b"\x00\x00").hex(), "bad_enum"))
    out.append(("phone_slot_4", frame(0x71, 0, 29, b"\x04").hex(), "bad_enum"))
    sched = enc_fields(SCHEDULE, SCHED_V)
    idlen = len(SCHED_V["id"]) + 1
    out.append(("schedule_hour_24", frame(0x32, 0, 17, sched[:idlen + 13] + b"\x18" + sched[idlen + 14:]).hex(), "bad_enum"))
    out.append(("schedule_weekday_bit7", frame(0x32, 0, 17, sched[:idlen + 20] + b"\x95" + sched[idlen + 21:]).hex(), "bad_enum"))
    m = dict(SCHED_MIN)
    smin = enc_fields(SCHEDULE, m)
    out.append(("schedule_end_date_without_has_end", frame(0x32, 0, 17, smin[:-5] + b"\xe7\x07\x01\x01" + smin[-1:]).hex(), "bad_date"))
    # minimal schedule: id "a" (2 bytes), start_date at 11..14 (year 11-12, month 13)
    out.append(("schedule_date_month_13", frame(0x32, 0, 17, smin[:13] + b"\x0d" + smin[14:]).hex(), "bad_date"))
    out.append(("schedule_date_year_1999", frame(0x32, 0, 17, smin[:11] + struct.pack("<H", 1999) + smin[13:]).hex(), "bad_date"))
    out.append(("schedule_date_day_0", frame(0x32, 0, 17, smin[:14] + b"\x00" + smin[15:]).hex(), "bad_date"))
    out.append(("schedule_date_day_32", frame(0x32, 0, 17, smin[:14] + b"\x20" + smin[15:]).hex(), "bad_date"))
    out.append(("schedule_date_month_0", frame(0x32, 0, 17, smin[:13] + b"\x00" + smin[14:]).hex(), "bad_date"))
    out.append(("schedule_date_year_2100", frame(0x32, 0, 17, smin[:11] + struct.pack("<H", 2100) + smin[13:]).hex(), "bad_date"))
    out.append(("id_empty", frame(0x31, 0, 16, b"\x00").hex(), "bad_string"))
    out.append(("id_65_chars", frame(0x31, 0, 16, b"\x41" + b"a" * 65).hex(), "bad_string"))
    out.append(("id_bad_char_space", frame(0x31, 0, 16, b"\x03a b").hex(), "bad_string"))
    out.append(("id_bad_char_slash", frame(0x31, 0, 16, b"\x03a/b").hex(), "bad_string"))
    out.append(("string_len_past_body", frame(0x31, 0, 16, b"\x05abc").hex(), "truncated"))
    nm = b"\x02\xc3\x28"  # invalid UTF-8 continuation
    out.append(("name_invalid_utf8", frame(0x72, 0, 30, nm).hex(), "bad_string"))
    out.append(("name_overlong_utf8", frame(0x72, 0, 30, b"\x02\xc0\xaf").hex(), "bad_string"))
    out.append(("name_surrogate_utf8", frame(0x72, 0, 30, b"\x03\xed\xa0\x80").hex(), "bad_string"))
    out.append(("name_overlong_3byte_utf8", frame(0x72, 0, 30, b"\x03\xe0\x80\xaf").hex(), "bad_string"))
    out.append(("name_overlong_4byte_utf8", frame(0x72, 0, 30, b"\x04\xf0\x80\x80\xaf").hex(), "bad_string"))
    out.append(("name_truncated_utf8_sequence", frame(0x72, 0, 30, b"\x02\xe2\x82").hex(), "bad_string"))
    out.append(("name_above_u10ffff", frame(0x72, 0, 30, b"\x04\xf4\x90\x80\x80").hex(), "bad_string"))
    out.append(("name_control_char", frame(0x72, 0, 30, b"\x03a\x0ab").hex(), "bad_string"))
    out.append(("name_del_char", frame(0x72, 0, 30, b"\x01\x7f").hex(), "bad_string"))
    out.append(("name_25_bytes", frame(0x72, 0, 30, b"\x19" + b"n" * 25).hex(), "bad_string"))
    out.append(("ssid_empty", frame(0x60, 0, 24, b"\x00\x00").hex(), "bad_string"))
    out.append(("ssid_33_bytes", frame(0x60, 0, 24, b"\x21" + b"s" * 33 + b"\x00").hex(), "bad_string"))
    out.append(("passphrase_7_chars", frame(0x60, 0, 24, b"\x01s\x07" + b"p" * 7).hex(), "bad_string"))
    out.append(("passphrase_non_ascii", frame(0x60, 0, 24, b"\x01s\x08" + b"p" * 7 + b"\xc3").hex(), "bad_string"))
    out.append(("passphrase_64_chars", frame(0x60, 0, 24, b"\x01s\x40" + b"p" * 64).hex(), "bad_string"))
    out.append(("plain_body_too_large", frame(0x02, 0, 3, b"\x01\x00" + bytes(495)).hex(), "too_large"))
    big = struct.pack("<BBBBHH", 1, 0, 0x40, ENCRYPTED, 12, 513) + bytes(513)
    out.append(("frame_too_large", big.hex(), "too_large"))
    out.append(("encrypted_body_shorter_than_tag", frame(0x40, ENCRYPTED, 12, bytes(15)).hex(), "bad_encrypted"))
    return out


def info_frags():
    w = sys.stdout.write
    body = enc_fields(STATUS, STATUS_V)
    fr = frame(0x40, RESPONSE, 12, struct.pack("<BII", 0, 2000, 1667) + body)
    for mtu in (23, 27, 64, 247, 517):
        data = mtu - 4
        frags = []
        chunks = [fr[i:i + data] for i in range(0, len(fr), data)]
        for i, c in enumerate(chunks):
            hdr = i | (0x80 if i == 0 else 0) | (0x40 if i == len(chunks) - 1 else 0)
            frags.append(bytes([hdr]) + c)
        w("frag name=manual_dose_response_mtu_%d mtu=%d frame=%s fragments=%s\n" % (
            mtu, mtu, fr.hex(), ",".join(f.hex() for f in frags)))
    big = frame(0x02, 0, 3, b"\x01\x00" + bytes(494))  # 504-byte frame (plain body maximum 496)
    frags = []
    chunks = [big[i:i + 19] for i in range(0, len(big), 19)]
    for i, c in enumerate(chunks):
        hdr = i | (0x80 if i == 0 else 0) | (0x40 if i == len(chunks) - 1 else 0)
        frags.append(bytes([hdr]) + c)
    w("frag name=largest_plain_frame_mtu_23 mtu=23 frame=%s fragments=%s\n" % (big.hex(), ",".join(f.hex() for f in frags)))
    # Reassembly errors (fragments received in this order; ms = arrival time).
    h = frame(0x01, 0, 7, b"\x01\x01")
    a, c2 = h[:5], h[5:]
    w("reasm_bad name=no_first fragments=%s ms=0 expect=frag_no_first\n" % (bytes([0x41]) + c2).hex())
    w("reasm_bad name=index_gap fragments=%s,%s ms=0,10 expect=frag_index\n" % ((bytes([0x80]) + a).hex(), (bytes([0x42]) + c2).hex()))
    w("reasm_bad name=empty_fragment fragments=%s,%s ms=0,10 expect=frag_empty\n" % ((bytes([0x80]) + a).hex(), "41"))
    w("reasm_bad name=timeout fragments=%s,%s ms=0,2001 expect=frag_timeout\n" % ((bytes([0x80]) + a).hex(), (bytes([0x41]) + c2).hex()))
    w("reasm_ok name=just_within_timeout fragments=%s,%s ms=0,2000 frame=%s\n" % ((bytes([0x80]) + a).hex(), (bytes([0x41]) + c2).hex(), h.hex()))
    w("reasm_ok name=new_first_restarts fragments=%s,%s ms=0,10 frame=%s\n" % ((bytes([0x80]) + a).hex(), (bytes([0xC0]) + h).hex(), h.hex()))
    over = [bytes([0x80]) + bytes(519)] + [bytes([0x41]) + bytes(2)]
    w("reasm_bad name=overflow_521 fragments=%s ms=0,10 expect=frag_overflow\n" % ",".join(f.hex() for f in over))


def info_records():
    w = sys.stdout.write
    rec = b"DSP2" + struct.pack("<BBBBIBBHH", 1, 1, 1, 0, 0x03, 0, 1, 7, 0xBEEF) + bytes(14)
    assert len(rec) == 32
    w("info name=claimed proto_min=1 proto_max=1 claim_state=1 flags=0 caps=3 fw_major=0 fw_minor=1 fw_patch=7 device_tag=48879 record=%s\n" % rec.hex())
    rec2 = b"DSP2" + struct.pack("<BBBBIBBHH", 1, 2, 0, 1, 0, 1, 2, 65535, 0) + bytes(14)
    w("info name=unclaimed_recovery proto_min=1 proto_max=2 claim_state=0 flags=1 caps=0 fw_major=1 fw_minor=2 fw_patch=65535 device_tag=0 record=%s\n" % rec2.hex())
    w("info_bad name=bad_magic record=%s expect=bad_magic\n" % (b"DSP3" + rec[4:]).hex())
    w("info_bad name=short record=%s expect=truncated\n" % rec[:31].hex())
    w("info_bad name=reserved_nonzero record=%s expect=bad_reserved\n" % (rec[:31] + b"\x01").hex())
    w("info_bad name=flags_bit2 record=%s expect=bad_enum\n" % (rec[:7] + b"\x04" + rec[8:]).hex())
    # M1.10c.1: credentials damaged (bit 1); a damaged device reports claim_state 1
    rec3 = b"DSP2" + struct.pack("<BBBBIBBHH", 1, 1, 1, 3, 0x03, 0, 1, 7, 0xBEEF) + bytes(14)
    w("info name=credentials_damaged_armed proto_min=1 proto_max=1 claim_state=1 flags=3 caps=3 fw_major=0 fw_minor=1 fw_patch=7 device_tag=48879 record=%s\n" % rec3.hex())
    adv = struct.pack("<BBH", 1, 0x01, 0xBEEF)
    w("advert name=claimed proto_major=1 flags=1 device_tag=48879 data=%s\n" % adv.hex())
    adv2 = struct.pack("<BBH", 1, 0x07, 0x0102)
    w("advert name=claimed_armed_damaged proto_major=1 flags=7 device_tag=258 data=%s\n" % adv2.hex())
    w("advert_bad name=flags_bit3 data=%s expect=bad_enum\n" % struct.pack("<BBH", 1, 0x08, 0xBEEF).hex())
    w("advert_bad name=short data=%s expect=truncated\n" % adv[:3].hex())
    w("advert_bad name=long data=%s expect=length_mismatch\n" % (adv + b"\x00").hex())


HEADER = """# BLE protocol v1 vectors (docs/BLE_PROTOCOL.md). Generated by tools/gen_ble_protocol_v1.py from a
# schema transcribed from the specification, never by the implementations under test
# (firmware dsp_bleproto, Android cz.nowass.dispenser.ble.protocol). A vector one side fails
# is a contract bug to investigate, not a value to regenerate.
#
# Format: one record per line, "<record> key=value ...", values never contain spaces.
# Strings and byte fields are written "x:<hex>" (UTF-8 bytes for strings); dates YYYY-MM-DD;
# integers decimal; nested fields "status.readiness", list entries "items.n" and
# "items.<i>.<field>"; "-" = no fields. Reserved bytes are not listed (always zero).
#
#   msg       frame <-> fields: decoding "frame" yields exactly these fields (in this order),
#             encoding them yields exactly "frame". kind req|rsp|err|evt.
#   enc       an ENCRYPTED frame: the header decodes, the body is opaque (ciphertext||tag).
#   bad       the decoder rejects "frame" with reason "expect".
#   compat    trailing bytes after the known layout, decoded by a receiver of minor "my_minor":
#             accept=1 decodes (trailing ignored), accept=0 rejects (reason trailing).
#   negotiate HELLO version negotiation: highest common major, or none.
#   frag      fragmentation of "frame" for ATT MTU "mtu" yields exactly "fragments"; reassembly
#             of those fragments yields "frame".
#   reasm_bad fragments (arrival times "ms") the reassembler rejects with reason "expect".
#   reasm_ok  fragments that reassemble to "frame".
#   info      32-byte Info characteristic record <-> fields.
#   info_bad  Info records rejected with reason "expect".
#   advert    4-byte advertising Service Data payload <-> fields.
#   advert_bad the advertising payload decoder rejects "data" with reason "expect".
#
# Reasons: truncated length_mismatch bad_major bad_flags unknown_type not_a_request trailing
# bad_bool bad_enum bad_reserved bad_date bad_string too_large bad_encrypted bad_magic
# frag_no_first frag_index frag_empty frag_timeout frag_overflow.

"""

if __name__ == "__main__":
    main()
