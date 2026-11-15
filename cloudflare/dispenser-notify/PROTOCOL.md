# Dispenser V2 notification protocol, version 1

Contract between the firmware notification engine (M1.5,
`firmware/components/dispenser_domain/include/dsp_notify.h`) and the Cloudflare backend
(`cloudflare/dispenser-notify`, M1.6a). The future ESP-IDF HTTPS transport implements the
`dsp_notify_transport_t.send()` callback with exactly this request and response handling.

**Status:** implemented and tested in the backend (M1.6a). The firmware HTTPS transport does
not exist yet.

## 1. Request

```http
POST /api/v1/events HTTP/1.1
Host: dispenser-notify.nowassek.workers.dev
Authorization: Bearer <DEVICE_API_TOKEN>
Content-Type: application/json
Content-Length: <n, at most 4096>

{"schema_version":1,"event_id":"dsp-0123456789ab-a5a5a5a5-42","device_id":"0123456789ab",
 "type":"RESERVOIR_LOW","severity":"WARNING","code":null,"reminder_no":0,
 "created_at":1795158000,"uncertain":false,"manual":false,"value":8000,"value2":100000,
 "attempt":1}
```

- One event per request.
- The body must be at most **4096 bytes**; a v1 event is about 300 bytes.
- `Content-Type` must be `application/json`, optionally with `; charset=utf-8`.
- Every field is **required** (`null` where allowed). Unknown fields are rejected.

### Fields and mapping from `dsp_note_t`

| Field            | JSON type       | Firmware source                            | Rules                                                                                                                                                                                     |
| ---------------- | --------------- | ------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `schema_version` | integer         | —                                          | exactly `1`                                                                                                                                                                               |
| `event_id`       | string          | `dsp_notify_format_id(device, epoch, seq)` | `dsp-<12 lowercase hex>-<8 lowercase hex>-<seq>`: epoch ≠ `00000000`; seq 1…4294967294 without leading zeros                                                                              |
| `device_id`      | string          | factory MAC, 48 bits                       | 12 lowercase hex characters, equal to the device part of `event_id`. It identifies the device; it is **not** a credential.                                                                |
| `type`           | string          | `note.type`                                | see table below                                                                                                                                                                           |
| `severity`       | string          | `note.severity`                            | `WARNING` or `CRITICAL`, and must match the type (`INFO` is reserved; no firmware type uses it)                                                                                           |
| `code`           | string or null  | `note.code`                                | name from the type's code list, or `null` for types without codes                                                                                                                         |
| `reminder_no`    | integer         | `note.reminder_no`                         | 0 = episode start, 1…255 = reminder. It must be 0 for one-shot events (`DOSING_ERROR`, `DEVICE_ERROR/QUEUE_OVERFLOW`). The firmware's `REMINDER` flag is equivalent to `reminder_no > 0`. |
| `created_at`     | integer or null | `note.created_at`                          | UTC seconds, 1735689600…4133980799, or `null` when the firmware clock was not trusted (flag `TIME_VALID` clear; firmware value 0)                                                         |
| `uncertain`      | boolean         | `note.flags & DSP_NOTE_F_UNCERTAIN`        | the reservoir estimate was uncertain                                                                                                                                                      |
| `manual`         | boolean         | `note.flags & DSP_NOTE_F_MANUAL`           | `true` only for `DOSING_ERROR`                                                                                                                                                            |
| `value`          | integer         | `note.value`                               | 0…4294967295                                                                                                                                                                              |
| `value2`         | integer         | `note.value2`                              | 0…4294967295                                                                                                                                                                              |
| `attempt`        | integer         | `note.attempts`                            | 1…65535, the number of started attempts including this one. **Transport metadata:** not part of the event's identity or payload.                                                          |

`note.condition` is not transmitted: it is fully determined by `type` and `code`, as in the
firmware validator. Nothing else is lost.

### Types, severities and codes

| `type`              | `severity` | `code` (firmware constant)                                                             | `value` / `value2`                                                                            |
| ------------------- | ---------- | -------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------- |
| `RESERVOIR_LOW`     | WARNING    | `null`                                                                                 | estimated remaining µl / capacity µl                                                          |
| `RESERVOIR_BLOCKED` | CRITICAL   | `null`                                                                                 | estimated remaining µl / capacity µl                                                          |
| `BATTERY_LOW`       | WARNING    | `null`                                                                                 | millivolts (0 = not measured) / 0                                                             |
| `DOSING_ERROR`      | CRITICAL   | `START_FAILED` (1), `PUMP_FAILED` (2), `INTERRUPTED` (3), `STORAGE` (4), `BLOCKED` (5) | requested µl / 0                                                                              |
| `DEVICE_ERROR`      | CRITICAL   | `STORAGE` (1), `TIME_SYNC` (2), `CLOCK` (3), `QUEUE_OVERFLOW` (4)                      | detail (failed sync attempts, storage detail) or number of lost events (`QUEUE_OVERFLOW`) / 0 |

## 2. Event identity and payload immutability

- `event_id` is the **idempotency key**. The firmware never reuses it for different content:
  the payload is frozen and saved to flash before the first transmission (M1.5).
- **Canonical payload:** after validation, the backend serializes these fields, in this
  order, as compact JSON: `event_id, device_id, type, severity, code, reminder_no,
created_at, uncertain, manual, value, value2`. Example:

  ```json
  {
    "event_id": "dsp-0123456789ab-a5a5a5a5-42",
    "device_id": "0123456789ab",
    "type": "RESERVOIR_LOW",
    "severity": "WARNING",
    "code": null,
    "reminder_no": 0,
    "created_at": 1795158000,
    "uncertain": false,
    "manual": false,
    "value": 8000,
    "value2": 100000
  }
  ```

  - Only integers, booleans, `null` and ASCII strings from closed sets occur, so the result
    is unique. Key order, whitespace and number spelling in the request (`8000` vs
    `8.0e3`) do not change it.
  - `schema_version` and `attempt` are excluded. A later protocol version must map an event
    to the **same** canonical form, so that a retry sent after a firmware upgrade is still
    recognized as the same event.

- **`payload_hash`** = lowercase hex SHA-256 of the UTF-8 bytes of the canonical payload.
  The canonical payload is stored in `notifications.payload`.
- **The same ID with the same canonical payload** is a duplicate: acknowledged, nothing
  changes.
- **The same ID with a different payload** is a conflict: rejected, and the stored event is
  never modified.

## 3. Responses

Every response is JSON with `Cache-Control: no-store`.

| Status    | Body                                                            | Meaning                                                                                                            | Firmware transport result (required for the HTTPS milestone)         |
| --------- | --------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------- |
| **201**   | `{"status":"accepted","event_id":…}`                            | Durably stored as `PENDING`. The backend is now responsible for delivery.                                          | `DSP_TX_ACK`                                                         |
| **200**   | `{"status":"duplicate","event_id":…}`                           | Already stored with an identical payload (an earlier attempt succeeded)                                            | `DSP_TX_ACK`                                                         |
| 400       | `{"status":"invalid","error":…,"field"?:…}`                     | Malformed JSON, unsupported `schema_version`, missing, unknown or invalid field                                    | `DSP_TX_REJECTED` (see below)                                        |
| 401       | `{"status":"unauthorized"}` + `WWW-Authenticate: Bearer`        | Missing or wrong token                                                                                             | `DSP_TX_FAILED` (keep pending; credentials may be fixed)             |
| 404 / 405 | `{"status":"invalid","error":"not_found"/"method_not_allowed"}` | Wrong path or method                                                                                               | `DSP_TX_REJECTED`                                                    |
| 409       | `{"status":"conflict","event_id":…}`                            | Same ID, different payload                                                                                         | `DSP_TX_REJECTED`                                                    |
| 413       | `{"status":"invalid","error":"payload_too_large"}`              | Body over 4096 bytes                                                                                               | `DSP_TX_REJECTED`                                                    |
| 415       | `{"status":"invalid","error":"unsupported_media_type"}`         | Wrong `Content-Type`                                                                                               | `DSP_TX_REJECTED`                                                    |
| **503**   | `{"status":"retry"}` + `Retry-After: 60`                        | Storage unavailable, server misconfigured (secret missing) or unexpected error. **Nothing may be assumed stored.** | `DSP_TX_FAILED`                                                      |
| —         | timeout or connection loss after the request was sent           | the event may have been stored                                                                                     | `DSP_TX_UNCERTAIN` (retry with the same ID; the backend answers 200) |
| —         | connection failure before sending                               | not stored                                                                                                         | `DSP_TX_FAILED`                                                      |

- `error` values: `malformed_json`, `unsupported_schema_version`, `missing_field`,
  `unknown_field`, `invalid_field`, `payload_too_large`, `unsupported_media_type`,
  `not_found`, `method_not_allowed`.
- Validation details are returned only after successful authentication.
- **A 2xx response means durable acceptance, not e-mail delivery.** E-mail sending is a
  later milestone. Its progress lives in `delivery_status`, `delivery_attempts`,
  `delivered_at` and `last_error`, which the device never sees.
- Any other status, including unexpected 5xx: treat as `DSP_TX_FAILED` (keep pending).

### Integration requirement: permanent rejection (approved 2026-11-15)

The firmware HTTPS transport milestone adds the transport result **`DSP_TX_REJECTED`**. The
firmware is **not** changed in M1.6a; this is a requirement for that milestone.

- **Which statuses:** HTTP **400, 404, 405, 409, 413 and 415** mean the backend permanently
  rejected this submission.
- **Never blocking:** M1.5 delivery stops at the first non-acknowledged entry. A rejected
  entry therefore must not stay at the head of the queue, or it would block every later
  notification forever.
- **Never silent:** the entry leaves the queue only after a **persistent local diagnostic**
  recording the rejected event (at least its event ID, type and code, the HTTP status and
  the backend `error`/`field` if present) has been written to flash.
- **BLE diagnostics:** the diagnostic stays available until it has been read, and the
  future BLE diagnostics expose it.
- **Kept pending (unchanged):** HTTP **401**, **503**, any other 5xx and connection failures
  before sending are `DSP_TX_FAILED`. The event keeps its ID and payload and is retried in a
  later regular session.
- **Timeouts:** a timeout or connection loss after sending stays **`DSP_TX_UNCERTAIN`**. The
  event keeps its original ID and frozen payload, and the retry is answered with 200
  (duplicate) if the first attempt was stored.

These statuses can only result from a firmware bug, a protocol mismatch, or a storage-epoch
collision after a storage reset.

## 4. Authentication

- `Authorization: Bearer <token>`: exactly one space, token of printable ASCII.
- The token is the Worker secret `DEVICE_API_TOKEN` (one shared token in v1).
- It is compared in constant time (SHA-256 of both values, `crypto.subtle.timingSafeEqual`).
- The secret must be 32–512 characters. If it is missing or shorter, the backend **fails
  closed** with 503 for every ingestion request.
- On the device the token belongs in the future credential store (provisioned over BLE,
  never logged, never in the `config` record).

## 5. Other endpoints

- `GET /health` (or `HEAD`): `{"status":"ok","service":"dispenser-notify","protocol":1}`.
  No authentication, no configuration, no database access.
- There is **no** endpoint that lists or reads stored events.
