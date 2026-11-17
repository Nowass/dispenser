# Dispenser V2 BLE protocol v1 (normative)

Status: **M1.10a, normative wire specification** for the binary protocol between the Android
app and the ESP32-C3 firmware. Design rationale, security model and decisions:
`docs/BLE_PROTOCOL_DESIGN.md` (revision 3). Shared test vectors:
`test-vectors/ble_protocol_v1.txt`. Implementations: firmware `dsp_bleproto` (C), Android
`cz.nowass.dispenser.ble.protocol` (Kotlin).

This document defines framing, encoding and validation only. The Noise session that encrypts
frames (M1.10c), the GATT transport (M1.10d) and the semantics behind each operation (domain,
M1.10b) are specified elsewhere. "MUST" and "MUST NOT" are normative.

## 1. Conventions

- Integers are unsigned little-endian unless stated (`i64` = two's-complement little-endian).
- `bool` is one byte, `0` or `1`; any other value is malformed.
- `str(a..b)` is `u8 length ‖ bytes`, length in `a..b`, bytes well-formed UTF-8 (no overlong
  forms, no surrogates, no code points above U+10FFFF). Kinds:
  - `text`: additionally no control characters (U+0000–U+001F, U+007F);
  - `id`: ASCII `[A-Za-z0-9._-]` only (schedule IDs);
  - `ascii`: printable ASCII 0x20–0x7E only (Wi-Fi passphrases);
  - `raw`: any bytes, no UTF-8 requirement (Wi-Fi SSIDs, which are arbitrary octets).
- `bytes(n)` is exactly `n` raw bytes; `bytes(*)` is all remaining bytes of the body.
- `date` is `u16 year ‖ u8 month ‖ u8 day` with year 2000–2099, month 1–12, day 1–31 (calendar
  validity is checked by the domain, not the codec).
- `reserved(n)` is `n` bytes that MUST be zero when sent and MUST be zero when received.
- Enumerations list their valid values; any other value is malformed.

## 2. GATT transport (summary; normative details in M1.10d)

One primary service `36b60001-3457-4e8e-9cba-8126de3f52e4` with the characteristics Info
(`36b60002-…`, read), Request (`36b60003-…`, write with response) and Response (`36b60004-…`,
notify). Advertising carries the service UUID and, in the scan response, Service Data for that
UUID (section 7); no Manufacturer Specific Data and no company identifier.

## 3. Fragmentation

A frame (section 4) is sent as one or more fragments, each one ATT write (client → device) or
notification (device → client).

| Byte | Meaning |
|---|---|
| 0 | bit 7 FIRST, bit 6 LAST, bits 0–5 index (0–63) |
| 1… | fragment data, 1 or more bytes |

- `data_max = ATT_MTU − 3 − 1`; ATT_MTU ≥ 23, so `data_max` ≥ 19. Senders fill every fragment
  except the last to `data_max`.
- The first fragment has FIRST set and index 0; indices increase by one; the last fragment has
  LAST set (a single-fragment frame has both). A frame needs at most 64 fragments.
- Receivers keep at most one partial frame per direction. They MUST discard the partial frame
  and report `MALFORMED` when: a fragment without FIRST arrives with no partial frame, the index
  is not the expected one, a fragment has no data, the frame would exceed 520 bytes, or more than
  2,000 ms pass between two fragments of one frame. A fragment with FIRST discards any partial
  frame and starts a new one.
- Flow control: the client sends the next fragment only after the write response; the device
  sends one notification at a time.

## 4. Frame

| Offset | Size | Field | Rules |
|---|---|---|---|
| 0 | 1 | `major` | 1 |
| 1 | 1 | `minor` | 0 for this document; any value is accepted (section 8) |
| 2 | 1 | `type` | section 5 |
| 3 | 1 | `flags` | bit 0 RESPONSE, bit 1 ERROR, bit 2 EVENT, bit 3 ENCRYPTED; bits 4–7 MUST be 0 |
| 4 | 2 | `txn` | client transaction ID, echoed in the response; 0 in events |
| 6 | 2 | `len` | number of bytes after the header; MUST equal the remaining frame length |
| 8 | len | body | plain body (section 6) or, with ENCRYPTED, `ciphertext ‖ tag(16)` |

- Maximum frame 520 bytes (8 header + 512 body); maximum plaintext body 496 bytes.
- Valid flag combinations: request `0`/`ENCRYPTED`; response `RESPONSE` (+`ENCRYPTED`); error
  `RESPONSE|ERROR` (+`ENCRYPTED`); event `EVENT` (+`ENCRYPTED`). ERROR without RESPONSE, EVENT
  with RESPONSE or ERROR, and EVENT on a request type are malformed.
- An encrypted body is at least 16 bytes (the tag). Its plaintext is the body of section 6 for
  the same `type` and flags. The 8-byte header is the AEAD associated data.
- Security (enforced by the session layer, M1.10c): types marked **plain** in section 5 travel
  without ENCRYPTED; all others MUST be ENCRYPTED. The codec reports the class of each type.

## 5. Message types

| Type | Name | Class | Request body | Response body |
|---|---|---|---|---|
| 0x01 | HELLO | plain | `proto_min u8, proto_max u8` | `proto u8, claim_state u8, flags u8, caps u32, session_id u32, device_id_hash bytes(8)` |
| 0x02 | HS | plain | `purpose u8, msg_index u8, noise bytes(*)` | `msg_index u8, noise bytes(*)` |
| 0x08 | ENROLL_RESULT | enc | — (no request) | `slot u8` |
| 0x10 | GET_STATUS | enc | empty | Status (7.2) |
| 0x11 | GET_DIAGNOSTICS | enc | empty | Diagnostics (7.3) |
| 0x12 | ACK_REJECTIONS | enc | empty | empty |
| 0x20 | GET_CONFIG | enc | empty | Config (7.4) |
| 0x21 | SET_CONFIG | enc | `expected_revision u32, field_mask u16, Config` | `revision u32` |
| 0x30 | LIST_SCHEDULES | enc | `page u8` | `set_revision u32, total u8, page u8, n u8 (0–4), n × {id str(1..64,id), revision u32}` |
| 0x31 | GET_SCHEDULE | enc | `id str(1..64,id)` | Schedule (7.5) |
| 0x32 | CREATE_SCHEDULE | enc | Schedule | `set_revision u32` |
| 0x33 | UPDATE_SCHEDULE | enc | `expected_revision u32, Schedule` | `revision u32, set_revision u32` |
| 0x34 | DELETE_SCHEDULE | enc | `id str(1..64,id), expected_revision u32` | `set_revision u32` |
| 0x40 | MANUAL_DOSE | enc | `seq u64, volume_ul u32` | `outcome u8, volume_ul u32, runtime_ms u32, Status` |
| 0x41 | GET_RESULT | enc | `seq u64` | `state u8, detail u32` |
| 0x42 | REFILL | enc | `capacity_ul u32, remaining_ul u32` | Status |
| 0x43 | SET_READINESS | enc | `readiness u8` | Status |
| 0x50 | SET_TIME | enc | `phone_utc_ms i64, uncertainty_ms u32` | `decision u8, offset_s i64` |
| 0x60 | WIFI_SET | enc | `ssid str(1..32,raw), passphrase str(0..63,ascii)` (length 0 = open network, else 8–63) | empty |
| 0x61 | WIFI_GET | enc | empty | `ssid str(0..32,raw), passphrase_set bool, last_result u8` |
| 0x62 | WIFI_CLEAR | enc | empty | empty |
| 0x70 | PHONES_LIST | enc | empty | `n u8 (0–4), n × {slot u8 (0–3), name str(0..24,text), enrolled_at i64, last_seen i64, is_self bool}` |
| 0x71 | PHONE_REMOVE | enc | `slot u8 (0–3)` | empty |
| 0x72 | INVITE_CREATE | enc | `name_hint str(0..24,text)` | `token bytes(16), expires_s u16` |
| 0x73 | INVITE_CANCEL | enc | empty | empty |
| 0x74 | RECOVERY_CANCEL | enc | empty | empty |
| 0x7D | FACTORY_RESET | enc | `confirm u32` = `0x54455352` ("RSET") | empty |
| 0x7E | END_SESSION | plain or enc | empty | empty |
| 0x80 | EV_SESSION_CLOSING | event, enc | `reason u8, seconds_left u16` | — |
| 0x81 | EV_SUSPENDING_FOR_DOSE | event, enc | `seconds_until u16` | — |
| 0x82 | EV_BUSY | event, plain | empty | — |
| 0x83 | EV_CONFIRM_ON_DEVICE | event, enc | `action u8, seconds u8` | — |

Types 0x90–0x9F (journal) and 0xA0–0xAF (priming/calibration) are reserved; all other
unlisted types are unknown.

Error body (any type, flags `RESPONSE|ERROR`): `code u8, detail u32, aux u32`.

### 5.1 Enumerations

| Field | Values |
|---|---|
| `claim_state` | 0 UNCLAIMED, 1 CLAIMED |
| HELLO response `flags` | bit 0 RECOVERY_ARMED, bit 1 CREDENTIALS_DAMAGED (the stored ownership record is unreadable, inconsistent or key-faulted; `claim_state` is then 1, never 0; decision O9, M1.10c.1); bits 2–7 MUST be 0 |
| `purpose` | 1 SESSION, 2 CLAIM, 3 ADD, 4 RECOVERY |
| `msg_index` | 0–2 |
| `readiness` | 1 READY, 2 SERVICE_REQUIRED |
| `outcome` (MANUAL_DOSE) | 0 DONE, 1 PUMP_FAILED |
| `state` (GET_RESULT) | 0 NOT_DECIDED, 1 PENDING, 2 DONE, 3 FAILED, 4 INTERRUPTED, 5 REFUSED, 6 ALREADY_DECIDED_RESULT_UNAVAILABLE |
| `decision` (SET_TIME) | 0 ACCEPTED, 1 REJECTED_IMPLAUSIBLE, 2 REJECTED_UNCERTAIN, 3 IGNORED_NTP_PREFERRED, 4 HOLD |
| `last_result` (WIFI_GET) | 0 UNKNOWN, 1 CONNECTED, 2 NO_AP, 3 AUTH, 4 TIMEOUT, 5 OTHER |
| `reason` (EV_SESSION_CLOSING) | 1 IDLE, 2 MAX, 3 DOSE, 4 COMMAND |
| `action` (EV_CONFIRM_ON_DEVICE) | 1 RECOVERY |
| error `code` | 1 MALFORMED, 2 UNSUPPORTED_VERSION, 3 UNKNOWN_TYPE, 4 UNAUTHENTICATED, 5 FORBIDDEN, 6 BUSY, 7 REFUSED, 8 LIMIT, 9 NOT_COVERED, 10 INVALID, 11 CONFLICT, 12 NOT_FOUND, 13 STORAGE, 14 DEGRADED, 15 PUMP_FAILED, 16 SESSION_EXPIRED, 17 DUPLICATE, 18 HANDSHAKE_FAILED, 19 SLOTS_FULL, 20 UNAVAILABLE (M1.10c.1; section 8.1) |
| Schedule `kind` | 0 ONCE, 1 DAILY, 2 WEEKLY |

Value ranges not listed (gates, limits, masks, revisions, volumes) are carried as-is; their
meaning and validation belong to the domain.

## 6. Bodies

A body is decoded by its `type` and flags (request, response, error or event). A body MUST have
exactly the length its layout requires, except for forward compatibility (section 8). A
response type that has no request (ENROLL_RESULT) and events never appear as requests.

## 7. Records

### 7.1 Info characteristic (public, 32 bytes)

`magic bytes(4) = "DSP2" | proto_min u8 | proto_max u8 | claim_state u8 | flags u8 (bit 0
RECOVERY_ARMED, bit 1 CREDENTIALS_DAMAGED; as the HELLO response) | caps u32 | fw_major u8 | fw_minor u8 | fw_patch u16 | device_tag u16 |
reserved(14)`.

Advertising Service Data payload (4 bytes): `proto_major u8 | flags u8 (bit 0 CLAIMED, bit 1
RECOVERY_ARMED, bit 2 CREDENTIALS_DAMAGED; bits 3–7 MUST be 0) | device_tag u16`. A device with
damaged credentials sets CLAIMED and CREDENTIALS_DAMAGED; it is never advertised as unclaimed.

### 7.2 Status (58 bytes)

`readiness u8 | dosing_flags u8 | gates u32 | pump_problems u32 | capacity_ul u32 |
remaining_ul u32 | low_percent u8 | estimate_uncertain bool | clock_trust u8 (0 UNKNOWN, 1
SYNCED, 2 HOLDOVER) | clock_reason u8 | device_utc i64 | next_due_known bool | next_due_at i64 |
pending_notes u8 | highest_severity u8 (0 none, 1 WARNING, 2 CRITICAL) | storage_ok bool |
session_seconds_left u16 | battery_percent u8 (0–100, 255 unknown) | manual_floor u64 |
reserved(5)`.

`manual_floor` is the next acceptable MANUAL_DOSE `seq` of the **authenticated** phone slot
(its durable high-water mark + 1; 2^63 when the slot is exhausted). It is specific to the
session's slot, travels only in encrypted responses (GET_STATUS, MANUAL_DOSE, REFILL,
SET_READINESS) and never in HELLO, the Info characteristic or advertising. A phone reads it at
session start and uses `seq = max(local + 1, manual_floor)`.

### 7.3 Diagnostics (24 bytes)

`notify_storage_ok bool | tx_diag u8 | tx_diag_status u16 | rejections_unread u8 |
rejections_total u16 | clock_trust u8 | clock_reason u8 | last_sync_utc i64 | last_wake_reason u8
(dsp_wake_reason_t) | reserved(6)`.

### 7.4 Config (48 bytes)

`revision u32 | reservoir_capacity_ul u32 | reservoir_remaining_ul u32 | low_stock_percent u8 |
time_sync_interval_h u16 | notify_mask u8 | pump_flow_ul_per_s u32 | calibrated_at i64 |
max_dose_ul u32 | max_daily_ul u32 | max_runtime_ms u32 | min_interval_s u32 |
hw_max_runtime_ms u32`.

In SET_CONFIG, `field_mask` selects the fields to change (bit 0 reservoir_capacity_ul, bit 1
low_stock_percent, bit 2 time_sync_interval_h, bit 3 notify_mask, bit 4 pump_flow_ul_per_s,
bit 5 calibrated_at, bit 6 max_dose_ul, bit 7 max_daily_ul, bit 8 max_runtime_ms, bit 9
min_interval_s; bits 10–15 MUST be 0). `revision`, `reservoir_remaining_ul` (changed only by
REFILL) and `hw_max_runtime_ms` are read-only and ignored in SET_CONFIG.

### 7.5 Schedule

`id str(1..64,id) | revision u32 | enabled bool | amount_ul u32 | start_date date | hour u8
(0–23) | minute u8 (0–59) | kind u8 | interval u32 | weekday_mask u8 (bit 0 Monday … bit 6
Sunday, bit 7 MUST be 0) | has_end bool | end_date date | name str(0..32,text)`.

When `has_end` is 0 the `end_date` bytes MUST be zero.

## 8. Versioning and negotiation

- `major` changes are incompatible. The client sends HELLO with the range of majors it supports;
  the device answers with the highest major in both ranges, or with error
  `UNSUPPORTED_VERSION` whose `detail` is `device_min | device_max << 8`. A frame with a `major`
  other than the session's is malformed.
- `minor` changes only add: new message types (older receivers answer `UNKNOWN_TYPE`), new
  enumeration values only where a field is documented as extensible (none in v1.0), and new
  trailing body fields. A receiver accepts trailing bytes beyond its known layout **only** when
  the frame's `minor` is greater than its own; with the same or a lower `minor` trailing bytes
  are malformed. A sender never omits fields of its own minor.
- `caps` (u32) announces optional features; v1.0 bits: 0 WIFI_PROVISIONING, 1 PHONE_TIME,
  2 HISTORY, 3 PRIMING, 4 VIEWER_ROLE (all others 0).

### 8.1 Unknown versus unavailable operations (M1.10c.1, decision C5)

- `UNKNOWN_TYPE` (3): the `type` byte is not defined by any protocol version this receiver
  implements (an unlisted or reserved type). The error frame echoes the request's `type`; the
  body is not decoded. Typical cause: a newer client talking to an older device.
- `UNAVAILABLE` (20): the `type` is a recognized v1 operation, but this firmware does not provide
  it (its optional feature is absent; the matching `caps` bit is clear), e.g. WIFI_SET/GET/CLEAR
  without WIFI_PROVISIONING, or FACTORY_RESET. `detail` and `aux` are 0. It is permanent for this
  firmware: retrying does not help, and nothing was executed. It is sent only inside an
  authenticated session (before authentication every privileged type is `UNAUTHENTICATED`,
  section 10.6), so it reveals nothing beyond the public `caps`.
- `REFUSED` (7) stays reserved for a changeable state precondition of a provided operation
  (section 10.1); `UNAVAILABLE` never depends on device state.

## 9. Examples

HELLO request, `txn = 7`, majors 1..1:

```text
01 00 01 00 07 00 02 00 01 01
```

Unauthenticated GET_STATUS answered with an error, `txn = 8`:

```text
01 00 10 03 08 00 09 00 04 00 00 00 00 00 00 00 00
```

MANUAL_DOSE plaintext body (`seq = 0x1122334455667788`, 2,000 µl), sent encrypted with
`txn = 12`: header `01 00 40 08 0c 00 1c 00`, body = 12-byte ciphertext ‖ 16-byte tag of
`88 77 66 55 44 33 22 11 d0 07 00 00`.

The complete byte-level cases are the shared vectors.

## 10. Operation semantics: configuration, schedules, time (M1.10b.1)

Implemented in the firmware domain (`dsp_app_*`, M1.10b.1); reachable once the session layer and
dispatcher exist (M1.10c/d). Every operation requires an **owner** session; without one the
device answers `FORBIDDEN` and touches nothing. A response carries the body of section 5; a
refusal the error body `code, detail, aux` below.

### 10.1 Error mapping

| Situation | `code` | `detail` | `aux` |
|---|---|---|---|
| no owner authorization | 5 FORBIDDEN | 0 | 0 |
| not inside a service session | 6 BUSY | 0 | 0 |
| rate limited (M1.10b.3: durable manual-dose admission per day, session-local operation and write limits); the request is NOT decided | 6 BUSY | 1 | seconds until a retry can succeed (0 = unknown) |
| a recovery confirmation is pending on the device (only END_SESSION) | 6 BUSY | 2 | 0 |
| state precondition | 7 REFUSED | 1 = field changeable only in SERVICE_REQUIRED, 2 = refill pending, 3 = device time not trusted (send SET_TIME first) | refused `field_mask` bits (detail 1) |
| schedule `amount_ul` above `max_dose_ul` | 8 LIMIT | 1 (per-dose limit) | `max_dose_ul` |
| 16 schedules stored | 8 LIMIT | 0 | 16 |
| validation | 10 INVALID | configuration or schedule problem bits (below); 0 = malformed argument | 0 |
| stale revision | 11 CONFLICT | current revision | 0 |
| unknown schedule ID | 12 NOT_FOUND | 0 | 0 |
| write failed (the old or the new value is durable; read back to decide) | 13 STORAGE | 0 | 0 |
| storage unreadable / device degraded | 14 DEGRADED | 0 | 0 |
| schedule ID active (`aux` 0) or deleted / executed before (`aux` 1) | 17 DUPLICATE | 0 | 0 or 1 |

Configuration problem bits: 0 remaining > capacity, 1 low-stock percent outside 1–99, 2 sync
interval outside 24–48 h, 3 unknown notification bits, 4 calibration time neither 0 nor
2025-01-01..2100, 5 field mask empty or bits 10–15 set, 6 `max_daily_ul` < `max_dose_ul`,
7 `max_runtime_ms` above `hw_max_runtime_ms`. Schedule problem bits: as the firmware's
`DSP_SCHED_E_*` (named like the Android validation errors), plus bit 16 name.

### 10.2 Configuration

- `GET_CONFIG` returns the stored record and the read-only hardware cutoff.
- `SET_CONFIG`: `expected_revision` must equal the stored revision. The revision also advances
  with every dose (the reservoir estimate is part of the record), so read before writing and
  retry on `CONFLICT`. Only fields selected in `field_mask` change; `revision`,
  `reservoir_remaining_ul` (REFILL only) and `hw_max_runtime_ms` are ignored. Response:
  the new revision.
- Calibration and limits (bits 4–9) are accepted only while the device is SERVICE_REQUIRED;
  while READY they are `REFUSED` (detail 1). The reservoir capacity may change while READY, but
  neither from 0 nor to 0. A configuration change never changes readiness.

### 10.3 Schedules

- `LIST_SCHEDULES`: 4 entries per page in stored order; `total` = schedules stored; a page
  beyond the last is `INVALID` (page 0 of an empty set is valid and empty).
- `CREATE_SCHEDULE`: `revision` 1, a new ID (never reused, also after deletion). Response:
  the new set revision.
- `UPDATE_SCHEDULE`: `expected_revision` = the stored revision (else `CONFLICT`), the record's
  `revision` = `expected_revision` + 1 (else `INVALID`). Response: the new schedule revision
  and set revision.
- `DELETE_SCHEDULE`: `expected_revision` must match. Execution history is kept.
- Names: 0–32 bytes of UTF-8 without control characters, stored as sent.
- Volumes are `u32` µl; the app must refuse larger values before sending.
- **No catch-up after edits:** creating a schedule, changing its time or recurrence, or
  re-enabling it never doses for an instant at or before the change. These three need a trusted
  device time; otherwise they are `REFUSED` with detail 3 (send `SET_TIME` first). Name, volume
  and disable edits and deletion work without it. An occurrence already
  executed on a date is never executed again on that date, whatever the edit (identity
  `<id>@<date>`). Name and volume edits do not affect pending occurrences.

### 10.4 Manual dose (M1.10b.2)

- The phone slot is the authenticated session's; nothing in the request selects it. `seq` is
  1..2^63 − 1, per slot, strictly increasing from the phone's side.
- `seq` ≥ the slot's floor is decided **exactly once**: executed (response: `outcome` 0 DONE or
  1 PUMP_FAILED) or refused (`REFUSED` detail = gates, `LIMIT` detail = limits, `NOT_COVERED`).
  Refusals are durable: the same `seq` never executes later, also after the conditions change.
- `seq` below the floor never executes: error `DUPLICATE`, `detail` = its result state
  (GET_RESULT values: 1 PENDING … 5 REFUSED, or 6 ALREADY_DECIDED_RESULT_UNAVAILABLE when the
  result is no longer retained), `aux` = the result detail (volume, or the refusal code 7/8/9/14).
- `seq` 0, `seq` ≥ 2^63 or `volume_ul` 0: `INVALID`, not consumed.
- `STORAGE`: the decision could not be made durable; nothing was executed and the request is
  still undecided unless a later GET_RESULT says otherwise. `DEGRADED`: the device cannot record
  decisions in this wake (storage failure, read-only session); nothing executed or consumed.
- `GET_RESULT {seq}` (no side effect): `state` 0 NOT_DECIDED (above the floor), 1 PENDING (in
  progress, or interrupted until the next boot reports it), 2 DONE, 3 FAILED, 4 INTERRUPTED,
  5 REFUSED, 6 ALREADY_DECIDED_RESULT_UNAVAILABLE; `detail` as above. The device retains the
  last 8 results; older decided requests report 6 and still never execute. The app must never
  re-issue a dose with a new `seq` automatically after a lost response.

### 10.4a Retries (summary; details in `docs/SECURITY_INTEGRATION_DESIGN.md` C4)

Within one session a request MAY be retransmitted with the same `txn` and an identical body after
a response timeout. The device answers it from its bounded response cache (the last 8 responses,
bound to the authenticated connection, ownership epoch, phone slot and request identity) without
executing the operation again. A `txn` reused with a different type or body is rejected with
`MALFORMED` and not executed. The cache is cleared at disconnect, session end, any ownership
change and reboot. It does **not** provide durable exactly-once semantics across reconnections:
there the durable mechanisms decide (revisions, the manual-dose `seq`, which stays authoritative,
and single-use invite tokens). A client uses the first response it receives for a `txn` and
discards later ones.

### 10.5 Time

`SET_TIME`: `phone_utc_ms` is truncated to seconds. `decision`:

| Value | Meaning | Applied |
|---|---|---|
| 0 ACCEPTED | the device had no trusted time or its last synchronization is older than the sync interval; the M1.4 correction rules apply (missed doses are never run) | yes |
| 1 REJECTED_IMPLAUSIBLE | before 2025-01-01 or after 2100 | no |
| 2 REJECTED_UNCERTAIN | `uncertainty_ms` > 2,000 | no |
| 3 IGNORED_NTP_PREFERRED | the device synchronized within the sync interval | no |
| 4 HOLD | a rollback hold is recorded, or the time is more than 7 days behind the highest trusted time; phone time never starts or ends a hold | no |

`offset_s` = new minus previous device time when accepted from a trusted clock, else 0.

### 10.6 Before authentication (M1.10c.1)

Until a security session is established on a connection, the device accepts only:

| Frame | State | Device action |
|---|---|---|
| HELLO (plain request) | connection open, no HELLO yet | version negotiation (section 8); HELLO response with the public state |
| HS (plain request) | after HELLO | the handshake (M1.10c.2); until then `HANDSHAKE_FAILED` and the connection is closed |
| END_SESSION (plain request) | any | empty response; **only this connection** is closed. The device's service window (button session) continues and accepts new connections (decision C10). Only an authenticated owner's END_SESSION ends the service window |

Everything else is refused without decoding the body: any other request type → `UNAUTHENTICATED`;
HS before HELLO or a second HELLO → `MALFORMED`; an undefined `type` → `UNKNOWN_TYPE`; a foreign
`major` → `UNSUPPORTED_VERSION`; a frame with RESPONSE, ERROR or EVENT set, or an unparsable header
→ `MALFORMED`; any frame with ENCRYPTED set → the connection is closed without a reply. The third
error on a connection closes it, and an unauthenticated connection is closed after 30 s. These
errors carry `detail` 0 (except `UNSUPPORTED_VERSION`, section 8) and never depend on secret state.
No request in this phase reaches a device operation.

