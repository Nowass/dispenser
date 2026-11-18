# Dispenser V2 — BLE security integration (M1.10c, revision 2)

Status: **revision 4 (2026-11-17).** C1–C10 decided; M1.10c.1 committed (`613f146`).

Because no ESP32-C3 hardware is available yet, M1.10c.2 is split (approved 2026-11-17):

- **M1.10c.2a, software integration:** implemented, under review, not committed (section 14).
  Verified on the host and by ESP32-C3 cross-builds only.
- **M1.10c.2b, hardware acceptance:** OPEN. It covers the physical RNG validation (6.1), live BLE
  handshakes, and target stack, heap and timing (`firmware/HARDWARE_ACCEPTANCE.md` 8a). Neither
  cross-build success nor host interoperability is evidence for these items.
- Until 2b passes, the security layer is build-only (`CONFIG_DSP_BLE_SECURITY`, default off; the
  build refuses it together with the production endpoint), and nothing is reachable without the
  M1.10d transport. Deterministic randomness exists only in isolated host-test sources.

Baseline: `docs/BLE_PROTOCOL_DESIGN.md`
revision 3 (D2 Noise XXpsk3/IK 25519/ChaChaPoly/SHA256, D3 no pairing, D15 pinned
`esphome/noise-c` 0.1.31 + `esphome/libsodium` 1.10021.12 with local patches 0001/0002),
`docs/BLE_PROTOCOL.md` v1.0, `docs/OWNERSHIP_DESIGN.md` revision 2 (implemented, `3ae2592`), and the
M1.10a feasibility gate (section 4.9 of the protocol design: 34/34). Decisions needing approval are
marked **[Cn]** and collected in section 11.

Scope: the security layer (`dsp_blesec`) and the dispatcher that turn opaque frames into exactly one
authorized Application Service call, inside the existing application task and service-session
rules. Not in scope: NimBLE/GATT (M1.10d), the Android JNI binding and app behavior (M1.10e),
hardware (M1.10f), eFuses, partitions, production services.

---

## 1. Findings of the feasibility review

| # | Finding | Consequence |
|---|---|---|
| F1 | **No entropy source for libsodium on ESP-IDF.** noise-c on ESP-IDF draws randomness through libsodium (`noise_rand_bytes` → `randombytes_buf`; its CMake forces `NOISE_USE_CUSTOM_RAND=0`). The ESPHome libsodium build defines only `CONFIGURED=1`, so `randombytes_internal` probes `getentropy`/`getrandom` only when `HAVE_*` macros are set (they are not) and then opens `/dev/urandom`, which ESP-IDF v6.1 does not provide (no VFS device; verified in the v6.1 tree). The M1.10a target gate only **built** the self-test, it never ran it, so this was not visible. On the device the first `randombytes_buf` would end in `sodium_misuse()` (abort). | The firmware must install a libsodium RNG implementation backed by `esp_fill_random()` with the public `randombytes_set_implementation()` **before** `sodium_init()` (no library change, no own primitive). Section 6. |
| F2 | **True randomness needs RF on.** `esp_random()` returns true random numbers only while Wi-Fi or Bluetooth runs, or after `bootloader_random_enable()` (which uses the SAR ADC). | Every key, nonce, token and session ID is generated only inside an active BLE session (RF on), which is where all handshakes happen anyway; the security layer refuses to start a handshake when the transport is not active. Hardware acceptance item (RNG quality with RF on). |
| F3 | **No HKDF in the ESP build of libsodium.** The component compiles a source subset without `crypto_kdf/hkdf` and `crypto_auth` (HMAC). noise-c exposes its own HKDF (`noise_hashstate_hkdf`, the Noise-specification HKDF = RFC 5869 with empty `info`). The approved PSK formula puts context into HKDF `info`. | **[C1, revised]**: keep the approved `info` formula and compute it with the PSA HKDF of the mbedTLS/TF-PSA-Crypto that ESP-IDF v6.1 already ships and links (no library patch). Section 7.1. |
| F4 | **noise-c sources are not in the repository.** The gate fetched them into `third_party/noise-c/gate/build` (ignored). M1.10c needs them in the firmware build (ESP-IDF component manager) and in the host tests (CTest). | **[C3]**: delivery of the pinned, patched sources to both builds. |
| F5 | **Codec structures are large.** The decoded body union is ≈ 500 B; two of them (request, response) do not belong on the stack. | Static buffers in the security-session state (section 9). |
| F6 | **Measured cost so far.** noise-c 12,256 B + libsodium 38,981 B flash (gate, ESP32-C3), static RAM 199 B, stack ≈ 1.9 KB for the Noise path incl. X25519 (static estimate); `dsp_bleproto` 10,728 B (RISC-V, compiled but not linked today); handshake heap 1,761 B per side on x86-64 (not a target bound). | Section 9 budget. |
| F7 | **The existing BLE port carries parsed command structs** (`dsp_app_cmd_t`, `respond(dsp_app_response_t)`), and REFILL, SET_READINESS and STATUS are unauthenticated queue commands. | Queue and port change to opaque frames; those three become authorized operations (section 4). |
| F8 | **The idempotency cache of 8 responses (design 8.1)** costs ≈ 4 KB and protects nothing the domain does not already protect: every mutation is revisioned, absolute or sequence-guarded, and a lost notification within a live GATT connection means the connection is gone (and with it the session). | **[C4, not approved]**: keep the bounded cache provisionally. The analysis (section 8.1) shows that INVITE_CREATE is not equivalent without it, REFILL repeats flash writes, and five operations change their visible outcome. |

---

## 2. Security architecture

```text
BLE task (M1.10d, NimBLE)            bytes only: reassembly per connection, generation counter,
   │ dsp_app_post_frame(gen, bytes)   connect/disconnect events, serves the 32-byte Info record and
   │ dsp_app_post_conn_event(...)     the advertising payload from a buffer the app task publishes;
   │ (lock: dsp_app_sync_t)           never parses, decrypts, authorizes or touches domain state
   ▼
Application task (single owner)
   stage_ble loop ─► dsp_blesec_on_frame(frame)
                       1. dsp_ble_parse_header (codec)            ── malformed ─► plain MALFORMED (pre-auth)
                       2. state machine gate (section 3)          ── not allowed ─► UNAUTHENTICATED / close
                       3. plain: HELLO / HS / END_SESSION
                          enc:   noise_cipherstate_decrypt (AD = header) ── failure ─► CLOSE
                       4. dsp_ble_decode_body (codec)             ── malformed ─► MALFORMED, CLOSE
                       5. dsp_dispatch(session auth, type, body)  ── exactly one dsp_app_* call
                       6. encode response, encrypt, port->send_frame
   ▼
Application Service (dsp_app) ─► domain (dosing, storage, ...)   authorization re-verified per call
```

Rules:

1. **The BLE task is a byte pipe.** It reassembles fragments (codec `dsp_ble_reasm_*`), tags each
   complete frame with the connection generation, and posts it to the bounded queue under the
   existing lock. Responses go out through `port->send_frame()`; fragmentation happens in the BLE
   task. Info reads and advertising use a buffer published by the application task.
2. **Decode, authenticate, authorize and dispatch only in the application task.**
3. **No privileged path without a session.** The dispatcher has exactly two inputs it trusts: the
   security-session state (owned by `dsp_blesec`) and the decrypted body. The authorization passed to
   `dsp_app_*` is the one the Application Service issued to `dsp_blesec` for this session; the
   dispatcher never builds one, and no frame field can select a slot or a role.
4. **Every `dsp_app_*` operation verifies the issued authorization** (session ID, token, slot,
   ownership epoch), as the M1.10b.3 ownership operations already do. A caller-built
   `{OWNER, slot}` becomes FORBIDDEN everywhere (configuration, schedules, time, manual dose,
   GET_RESULT, floor, REFILL, SET_READINESS, status, diagnostics, ownership).
5. **Session scope = one BLE connection.** A security session starts with HELLO on a new connection
   and ends with the connection. The issued authorization, the handshake handle and the transport
   keys are bound to it; a reconnect needs a new IK handshake. Nothing survives a reboot or deep
   sleep (RAM only; `dsp_app_t` is re-initialized every wake).
6. **Close on any security error:** decryption failure (modified, truncated, replayed, reordered,
   wrong key), a nonce counter reaching the limit, a plain frame where encryption is required, an
   encrypted frame before the handshake completed, a malformed decrypted body, a handshake message
   out of order. Closing = zeroize, drop the connection (port `disconnect`), invalidate the
   authorization.

### 2.1 Module layout

| Module | Location | Depends on | Role |
|---|---|---|---|
| `dsp_bleproto` | domain (exists) | — | codec, fragments, records |
| `dsp_app` / `dsp_app_sec.h` | domain (exists) | — | operations, ownership state, issued authorization |
| `dsp_blesec` | **new component `dispenser_security`** | domain, noise-c, libsodium | per-connection security state machine, Noise glue, prologue/PSK, frame crypto |
| `dsp_dispatch` | `dispenser_security` | domain | type → operation table, body ↔ domain struct mapping, error mapping |
| `dsp_esp_rng` | platform | ESP-IDF, libsodium | libsodium RNG implementation over `esp_fill_random` (F1) |

`dispenser_domain` stays dependency-free (host build guarantees it). `dsp_app_sec.h` stays
restricted (the configure-time check gains `dsp_blesec.c` as the allowed includer, already listed).

---

## 3. State machine (per connection)

```text
            connected (gen g)
  IDLE ─────────────────────► OPEN ── HELLO ok ──► NEGOTIATED
                                │                     │ HS(p, msg 0): dsp_app_sec_admit(p) ok
                                │ 30 s / 3 errors      ▼
                                │                HANDSHAKING(p) ── error / 10 s per step ──► CLOSING
                                │                     │ complete
                                ▼                     ├─ SESSION ── lookup ok ─────────────► AUTHENTICATED(slot)
                             CLOSING                  ├─ CLAIM/ADD ─ enroll ok ─ ENROLL_RESULT ► AUTHENTICATED(slot)
                                ▲                     └─ RECOVERY ─ begin ─► CONFIRMING ── confirmed ─ apply ─ ENROLL_RESULT ─► AUTHENTICATED(0)
                                │                                              │ abort / 20 s / session end
                                └──────────────────────────────────────────────┴──────────────────────────
  AUTHENTICATED ── decrypt/protocol error, END_SESSION, 15-min cap, own slot removed, epoch lost,
                   pump suspension (connection closed), reboot/sleep ─────────────────────────► CLOSING
  CLOSING: zeroize keys, handshake state, pending identity, PSK, setup-code copy; drop the issued
           authorization (dsp_app_sec_connection_end); disconnect ─► IDLE
```

| State | Accepted | Anything else |
|---|---|---|
| OPEN | HELLO (plain), END_SESSION (plain) | plain error UNAUTHENTICATED (privileged types) or MALFORMED; 3 errors or 30 s → CLOSING |
| NEGOTIATED | HS msg 0 with a purpose `dsp_app_sec_admit` allows, END_SESSION | HANDSHAKE_FAILED → CLOSING |
| HANDSHAKING | the next HS message of the same purpose (`msg_index` exact) | HANDSHAKE_FAILED → CLOSING; counts as a failed handshake |
| CONFIRMING | END_SESSION (encrypted or plain; closes the connection and aborts the confirmation, the window continues); device sends `EV_CONFIRM_ON_DEVICE {RECOVERY, 20}` | encrypted BUSY detail 2 (CONFIRMING); decrypt failure → CLOSING |
| AUTHENTICATED | encrypted requests (section 5), END_SESSION | plain frames other than END_SESSION → CLOSING; decrypt failure → CLOSING |

Session-level limits (RAM): a failed handshake closes **its connection only**. It never ends the
service window and never carries a lockout into a later window (decision C10, revised from
"3 failed handshakes end the service session"; `DSP_BLE_END_AUTH_FAILURES` is retired).
`dsp_app_sec_handshake_failed` increments the durable `failed_sessions` counter at most once per
service window; it is a saturating diagnostic that gates nothing. Brute force is not a concern:
XXpsk3 needs the 128-bit setup code and IK an enrolled key, and each attempt costs a connection,
with 3 protocol errors or 30 s unauthenticated per connection. Other limits: 10 s per handshake
step; nonce limit 2^32 messages per direction (then CLOSING).

**Ending the service window (C10).** Only an authorized owner (`dsp_app_end_service_session`
with the authorization issued for this connection, called by the dispatcher for an encrypted
END_SESSION) or an internal transition ends it: idle timeout, 15-minute maximum, resume refused,
clock fault, own slot revoked. A plain END_SESSION in any state, an END_SESSION from a
CONFIRMING (recovering, not yet authorized) phone, a failed handshake and an expired or
erroneous unauthenticated connection close only that connection. The idle timer restarts and
advertising continues, so the owner can still connect in the same window.

### 3.1 Interaction with the service session and the pump

- The service session (button wake; 180 s idle without a client, kept while connected, **15 min
  maximum**) is unchanged; connections and security sessions live inside it. `EV_SESSION_CLOSING`
  (encrypted, AUTHENTICATED only) 30 s before the maximum; at the maximum every security session is
  closed and BLE stopped.
- **Pump:** before any pump run (scheduled or manual) BLE is suspended: the connection is closed,
  so the security session ends (zeroized). `EV_SUSPENDING_FOR_DOSE` is sent first when AUTHENTICATED
  (best effort). Resume happens only while the 15-minute deadline permits (existing
  `ble_resume`); a new connection needs a new IK handshake. A pending recovery confirmation is
  aborted (existing). The MANUAL_DOSE response is therefore normally not delivered (connection
  closed before the pump); the phone reconnects and uses GET_RESULT (protocol 10.4, design 8.3).
- Reboot or deep sleep: no state survives; the next wake starts at IDLE.

---

## 4. Domain changes required (missing interfaces)

Status: rows 1–6 and 8 implemented in M1.10c.1 (section 13). Row 7 (`device_id_hash` computation)
is an optional security-layer callback now; it is filled in M1.10c.2. Row 9 (secret port) is
M1.10c.2.

| Change | Why |
|---|---|
| **Issued-authorization check in `guard()` for every operation** (configuration, schedules, time, manual dose, GET_RESULT, floor) | requirement; today only the ownership operations check it |
| New operations with authorization: `dsp_app_get_status` (Status record incl. `manual_floor`, `session_seconds_left`, battery 255), `dsp_app_get_diagnostics`, `dsp_app_ack_rejections`, `dsp_app_refill`, `dsp_app_set_readiness` | REFILL, SET_READINESS and STATUS are unauthenticated queue commands today (F7) |
| **Retire** queue kinds STATUS, REFILL, SET_READINESS (like MANUAL_DOSE in M1.10b.2) | no unauthenticated path remains |
| Queue element = opaque frame `{gen, len, bytes[520]}` + transport events (CONNECTED gen/MTU, DISCONNECTED gen) + button events; `dsp_app_post_frame`, `dsp_app_post_conn_event` | design section 6; stale frames of an old connection are discarded by `gen` |
| BLE port: `send_frame(ctx, bytes, len)`, `disconnect(ctx)`, `publish_info(ctx, info[32], advert[4])` replace `respond(struct)` | byte transport only |
| Security-session scope: `dsp_app_sec_connection_begin/end` (per-connection session ID; drops handle, authorization, confirmation) | M1.10b.3 binds evidence to the service session; it must bind to the connection |
| `dsp_app_sec_view` adds `device_id_hash` inputs (device ID from the platform: factory MAC, read-only) | HELLO / prologue (section 7.2) |
| `cfg->security` registration provided by the platform runtime when BLE is built in (M1.10d); M1.10c registers it in host tests and in a build-only target configuration | ownership authorization possible only with the security layer |
| Secret port `dsp_secret_port_t.read_setup_code` (platform: `nvs_sec` blob `setup_code`), used only by `dsp_blesec` | PSK input |

Recognized operations whose optional feature this firmware does not provide (Wi-Fi provisioning,
factory reset; caps bits 0, 2, 3 clear) answer the new error **`UNAVAILABLE` (20)** inside an
authenticated session. Undefined type bytes answer `UNKNOWN_TYPE` **[C5, revised]** (section 8;
`docs/BLE_PROTOCOL.md` 8.1).

---

## 5. Dispatcher and exact authorization rules

### 5.1 Identity mapping

| Authenticated identity | How | Authorization |
|---|---|---|
| none (OPEN, NEGOTIATED, HANDSHAKING) | — | only HELLO, HS, END_SESSION |
| enrolled phone key `K` (IK SESSION) | `dsp_app_sec_lookup(hs, K)`: `K` equal to a used slot's key in the durable record, device CLAIMED (not degraded) | `OWNER` of that slot, bound to (connection session ID, random token, ownership epoch) |
| new phone (XX CLAIM / ADD) | handshake with the setup-code PSK (+ invite token for ADD) completed; `dsp_app_sec_enroll` stored the key | `OWNER` of the new slot (same binding) |
| new phone (XX RECOVERY) | PSK + arm + 3-press confirmation bound to the transcript hash; `dsp_app_sec_recovery_apply` | `OWNER` of slot 0 (same binding) |

The authorization becomes invalid when the connection ends, when the ownership epoch changes in a way
that removes the slot (own removal, recovery), and at every reboot. A removal of another slot keeps
the caller authorized (epoch re-bound, M1.10b.3).

### 5.2 Message table

| Type | State required | Class | Domain call | Response |
|---|---|---|---|---|
| HELLO 0x01 | OPEN | plain | `dsp_app_sec_view` | claim state, flags (bit 0 RECOVERY_ARMED, **bit 1 CREDENTIALS_DAMAGED**), caps, session_id, device_id_hash |
| HS 0x02 | NEGOTIATED / HANDSHAKING | plain | admit / identity / invite token / lookup / enroll / recovery begin | HS response or ENROLL_RESULT (section 7) |
| GET_STATUS 0x10 | AUTHENTICATED | enc | `dsp_app_get_status(auth)` | Status |
| GET_DIAGNOSTICS 0x11 | AUTHENTICATED | enc | `dsp_app_get_diagnostics(auth)` | Diagnostics |
| ACK_REJECTIONS 0x12 | AUTHENTICATED | enc | `dsp_app_ack_rejections(auth)` | empty |
| GET_CONFIG / SET_CONFIG 0x20/0x21 | AUTHENTICATED | enc | `dsp_app_get_config` / `dsp_app_set_config` | Config / revision |
| LIST / GET / CREATE / UPDATE / DELETE schedule 0x30–0x34 | AUTHENTICATED | enc | `dsp_app_*_schedule` | as specified |
| MANUAL_DOSE 0x40 | AUTHENTICATED | enc | `dsp_app_manual_dose(auth, seq, volume)` | outcome + Status (normally not delivered: BLE suspended) |
| GET_RESULT 0x41 | AUTHENTICATED | enc | `dsp_app_get_result` | state, detail |
| REFILL 0x42 | AUTHENTICATED | enc | `dsp_app_refill(auth, …)` | Status |
| SET_READINESS 0x43 | AUTHENTICATED | enc | `dsp_app_set_readiness(auth, …)` | Status (gates if refused) |
| SET_TIME 0x50 | AUTHENTICATED | enc | `dsp_app_set_phone_time` | decision, offset |
| WIFI_* 0x60–0x62, FACTORY_RESET 0x7D | AUTHENTICATED | enc | none (not provided by this firmware) | `UNAVAILABLE` (20) **[C5, revised]**; before authentication `UNAUTHENTICATED` like every privileged type |
| PHONES_LIST 0x70 | AUTHENTICATED | enc | `dsp_app_phones_list` | list (page 0, ≤ 4) |
| PHONE_REMOVE 0x71 | AUTHENTICATED | enc | `dsp_app_phone_remove` | empty; own slot → response, then CLOSING |
| INVITE_CREATE 0x72 | AUTHENTICATED | enc | `dsp_app_invite_create` (`name_hint` accepted, ignored) | token, expires_s |
| INVITE_CANCEL / RECOVERY_CANCEL 0x73/0x74 | AUTHENTICATED | enc | `dsp_app_invite_cancel` / `dsp_app_recovery_cancel` | empty |
| END_SESSION 0x7E | any | either | **C10:** plain or unauthorized → closes only this connection (`dsp_link`, implemented). AUTHENTICATED (encrypted) → `dsp_app_end_service_session(auth)` ends the whole service window | empty |

Error mapping: `dsp_app_op_result_t` maps 1:1 to protocol errors (existing table, BUSY details 1/2);
before authentication only plain MALFORMED, UNSUPPORTED_VERSION, UNKNOWN_TYPE, UNAUTHENTICATED and
HANDSHAKE_FAILED are sent, with no detail that depends on secret state. `UNKNOWN_TYPE` = undefined
type byte (body not decoded); `UNAVAILABLE` = a recognized operation this firmware does not
provide (permanent, detail 0); `REFUSED` = a changeable state precondition of a provided
operation. The three are never interchanged.

---

## 6. Randomness (F1, F2)

- `dsp_esp_rng.c` (platform) registers `randombytes_implementation {name, random → esp_random,
  stir → no-op, uniform → NULL (libsodium then uses its own rejection sampling over `random`),
  buf → esp_fill_random, close → 0}` with
  `randombytes_set_implementation()` and then calls `sodium_init()`, once at runtime start, before any
  Noise object exists. This is libsodium's documented extension point; no primitive is written.
- Uses: Noise ephemeral keys (each handshake), the device identity key pair (claim, recovery), the
  HELLO session ID, the M1.10b.3 handshake nonce, authorization token and invite token
  (`cfg->security->random` → `esp_fill_random`).
- **RF on:** `dsp_blesec` starts handshakes and the Application Service generates tokens only while
  the BLE transport is active (always true inside a session). No key material is generated at boot.
- Host tests: a deterministic RNG implementation (fixed seeds) for vectors and reproducibility; a
  test asserts that the target build registers the hardware implementation before `sodium_init`.
- Hardware acceptance (M1.10f): RNG statistics with RF on (e.g. a large `esp_fill_random` sample
  through a standard test battery off-device), boot-to-first-handshake ordering.

**C2 approved** with three binding additions:

1. **Registration before initialization.** `dsp_esp_rng_init()` calls
   `randombytes_set_implementation(&dsp_esp_rng)` and only then `sodium_init()`, once, before any
   Noise object exists. If either step fails, the security layer is **not registered**
   (`cfg->security == NULL`). Then no session ID, handshake or authorization can exist; HELLO still
   reports the public state, and every privileged request stays `UNAUTHENTICATED`/`FORBIDDEN`.
2. **Randomness only with an active hardware entropy source.** `dsp_esp_rng_ready()` is true only
   while the Bluetooth controller is enabled (`esp_bt_controller_get_status() ==
   ESP_BT_CONTROLLER_STATUS_ENABLED`) or Wi-Fi is started. Each consumer checks it:
   - `cfg->security->random` returns false when not ready. The Application Service then issues no
     session ID (HELLO carries 0), admits no handshake and creates no token or invite. Implemented
     and tested in M1.10c.1 with a failing fake (`test_session`, `test_rng_failure_fails_closed`).
   - `dsp_blesec` refuses HS message 0 when not ready (`HANDSHAKE_FAILED`, no Noise object).
   - The libsodium implementation's `random`/`buf` callbacks call `abort()` if they are ever
     reached while not ready. This path is unreachable by construction. Restarting is fail-closed
     (no key material is generated, nothing persists); returning bytes from an inactive source is
     not.
   `bootloader_random_enable()` is not used.
3. **Explicit ESP32-C3 runtime acceptance test** (section 6.1); successful compilation is not
   sufficient.

**Refinements found in M1.10c.2a (implemented in `dispenser_platform/src/dsp_esp_rng.c`):**

- `sodium_init()` itself draws randomness (libsodium's allocation canary). The implementation is
  therefore registered at runtime start (`dsp_esp_rng_register`, no randomness drawn), and
  `sodium_init()` runs only at the first moment the hardware entropy source is active
  (`dsp_esp_rng_ready`), never at boot with RF off.
- The hardware entropy source counts as active only while the Bluetooth controller is enabled
  (`esp_bt_controller_get_status`). This is stricter than "BT or Wi-Fi": every handshake happens
  over BLE. Without `CONFIG_BT_ENABLED` in the build, nothing is ever ready (fail closed).
- noise-c's default `NOISE_USE_CUSTOM_RAND` applies in every build, so the firmware supplies
  `noise_rand_bytes()` as `randombytes_buf()`. There is exactly one randomness path, through the
  registered implementation.

### 6.1 Target RNG acceptance test (M1.10c.2b; requires the board)

A build-only check does not count. A dedicated test configuration runs on an ESP32-C3 and records:

| # | Check | Pass criterion |
|---|---|---|
| R1 | registration order | `randombytes_implementation_name()` is the firmware's implementation, and the registration happened before `sodium_init()` (an init-order flag set by the implementation) |
| R2 | routing | with the BT controller enabled, `randombytes_buf` / `noise_rand_bytes` go through `esp_fill_random` (call counter rises; libsodium's internal source is never used) |
| R3 | refusal without RF | with the controller disabled, `dsp_esp_rng_ready()` is false: `cfg->security->random` fails, the session ID is 0 and HS message 0 gets HANDSHAKE_FAILED (the `abort()` path is not reached) |
| R4 | live handshakes | XXpsk3 and IK handshakes complete on the device against the host reference "phone" (or a loopback of the device and an on-device peer). Two runs produce different ephemeral keys and session IDs |
| R5 | quality sample | ≥ 1 MiB from `esp_fill_random` with RF on, analyzed off-device (e.g. `ent`, dieharder or the NIST STS subset): no failure beyond the expected rate. Recorded, not a gate on its own |
| R6 | PSK on target | the C1 reference vectors (section 7.1) reproduce on the device through the production derivation code |

Status: **open (M1.10c.2b)**. Checklist items R1–R8 are in `firmware/HARDWARE_ACCEPTANCE.md` 8a.
No firmware with the security layer enabled is deployed before R1–R4 pass. M1.10c.2a provides the
code these checks exercise (`dsp_esp_rng_name`, `dsp_esp_rng_registered_before_init`,
`dsp_esp_rng_calls`) and the PSK vectors for R6, but it has run none of them on a device. The host
check of the device HKDF provider (`firmware/test/psk_linux`, ESP-IDF linux target) runs the same
mbedTLS sources on the host CPU, so it is not R6. Hardware acceptance 9.4 is not affected and stays
open.

---

## 7. Handshakes and protocol details

### 7.1 Prologue and PSK

- Prologue (bound into the handshake hash): `"DSP2 BLE v1" ‖ major u8 ‖ minor u8 ‖ purpose u8 ‖
  device_id_hash(8) ‖ session_id u32 LE` — exactly the HELLO response values of this connection.
- `device_id_hash` (O6, to be made normative): first 8 bytes of SHA-256(`"DSP2 id"` ‖ identity public
  key) when CLAIMED; of SHA-256(`"DSP2 dev"` ‖ device ID (6-byte MAC)) when UNCLAIMED or
  CREDENTIALS_DAMAGED. SHA-256 from libsodium (`crypto_hash_sha256`, in the ESP build).
- PSK (32 B) **[C1, revised: approved formula preserved]**:
  `PSK = HKDF-SHA256(salt = "DSP2 psk" (8 ASCII bytes), ikm = setup_code (16 bytes),
  info = purpose u8 ‖ invite_token (16 bytes, ADD only; empty for CLAIM and RECOVERY), L = 32)`.
  This is RFC 5869 Extract-then-Expand, exactly the D2 formula (`docs/BLE_PROTOCOL_DESIGN.md` 4.5).
  The earlier proposal to move the context into the salt is **withdrawn**. It is not equivalent:
  a different salt gives an unrelated PRK, and RFC 5869 assigns context binding to `info`, not to
  the extractor salt. Reference vectors include a salt/info-swapped case that must differ.
- **Provider investigation (C1):**

  | Provider | Device (ESP-IDF v6.1) | Host CTest | Android (M1.10e) | Verdict |
  |---|---|---|---|---|
  | noise-c `noise_hashstate_hkdf` | built | built | built | **unusable**: Noise-specification HKDF without `info` |
  | libsodium `crypto_kdf_hkdf_sha256_*` | source present in the pinned tree but **not compiled** (the component's source list omits `crypto_kdf/hkdf`); enabling it = local patch 0003 | not compiled in the gate build | not compiled in the gate build; the app's own CMake could add the file without changing the library | device: rejected (patch) |
  | mbedTLS / TF-PSA-Crypto PSA `psa_key_derivation_*` with `PSA_ALG_HKDF(PSA_ALG_SHA_256)` | **shipped and enabled**: `components/mbedtls` of ESP-IDF v6.1 (TF-PSA-Crypto, `PSA_WANT_ALG_HKDF 1` by default, not removed by ESP's `esp_config.h`); `mbedtls` is already a dependency of `dispenser_platform` (HTTPS) | buildable from the same pinned ESP-IDF sources | not available | **device: recommended** (full RFC 5869 with `info`, no patch, vetted, maintained by the ESP-IDF release) |
  | JCA / Android platform | — | — | no public HKDF at minSdk 26; HMAC only | Android: use the libsodium function through the JNI library (already planned for noise-c), compiled from the same pinned source by the app's CMake (no library change). Decided in M1.10e |

  **C1 approved (2026-11-17):** device → mbedTLS / TF-PSA-Crypto PSA HKDF behind
  `dsp_psk_derive()` in `dispenser_security`; Android → libsodium `crypto_kdf_hkdf_sha256_*`
  through JNI; reference vectors → Python standard library. Host tests link the same
  `dsp_psk_derive()` against TF-PSA-Crypto built from the pinned ESP-IDF tree, with a libsodium
  cross-check. Requirements:
  - **byte-identical** PSKs on firmware (host and target), Android and the Python reference for
    every vector;
  - **boundary vectors**: setup code all-zero, all-0xFF and alternating; each purpose; ADD
    token all-zero, all-0xFF and random; the empty-`info`-suffix purposes (CLAIM, RECOVERY);
  - **negative vectors**: context moved from `info` into the salt (must differ), wrong purpose,
    and token or setup code off by one bit (all must differ);
  - **secure clearing** of every intermediate secret: the setup-code copy, the PRK/OKM buffers
    and the PSK after `noise_handshakestate_set_pre_shared_key`. On the device: the PSA key
    handles are destroyed (`psa_destroy_key`) and the derivation aborted
    (`psa_key_derivation_abort`) on every path, including errors, and stack buffers are cleared
    with `mbedtls_platform_zeroize`. On Android: `sodium_memzero` in JNI, with no secret ever in a
    Java `String`. A host test scans the derivation's buffers after success and after each
    injected failure;
  - a build check that `PSA_WANT_ALG_HKDF` is enabled and linked in every firmware configuration
    with the security layer; R6 on the board.
- **Independent reference vectors** (generated in M1.10c.2 by `test-vectors/tools/gen_ble_session_v1.py`):
  - The generator uses only the Python standard library `hmac`/`hashlib` (RFC 5869 written out in
    a few lines; independent of noise-c, libsodium and mbedTLS). It first self-checks against
    RFC 5869 Appendix A test cases 1–3.
  - Cases: CLAIM and RECOVERY (empty token); ADD with a token; setup codes all-zero, all-0xFF and
    random; and the negative cases context-in-salt (must differ), wrong purpose and token off by
    one bit (must differ).
  - Consumed by the firmware host CTest (PSA backend), the on-target test R6 and the Android unit
    tests (M1.10e).
- The setup code is read into a stack buffer, used for the PSK and wiped (`sodium_memzero`); the PSK
  is wiped after `noise_handshakestate_set_pre_shared_key`.

### 7.2 Sequences

```text
CLAIM (device UNCLAIMED)                      ADD (CLAIMED, invite valid)
 P→D HELLO                                     P→D HELLO               (P checks device_id_hash
 D→P HELLO rsp (UNCLAIMED, sid, hash_dev)      D→P HELLO rsp            against the invite's
 P→D HS{CLAIM, 0, XX msg1}                                               identity key)
     D: admit(CLAIM); new identity (RAM)       P→D HS{ADD, 0, msg1}     D: admit(ADD); invite token
 D→P HS{1, msg2: D static (new)}                                         → PSK; stored identity
 P→D HS{CLAIM, 2, msg3: P static, psk,         D→P HS{1, msg2}
        payload name str(0..24)}               P→D HS{ADD, 2, msg3 + name}
     D: split; enroll(CLAIM, P, name, id)          D: split; enroll(ADD) (invite consumed first)
 D→P ENROLL_RESULT{0} (enc, txn of msg3)       D→P ENROLL_RESULT{slot} (enc)
     → AUTHENTICATED(0)                            → AUTHENTICATED(slot)

SESSION (CLAIMED)                              RECOVERY (armed)
 P→D HELLO; D→P HELLO rsp (hash_id)             P→D HELLO; D→P HELLO rsp (RECOVERY_ARMED)
 P→D HS{SESSION, 0, IK msg1: e, es, s, ss}      P→D HS{RECOVERY, 0, XX msg1}
     D: admit(SESSION); stored identity             D: admit(RECOVERY); new identity (RAM)
        read msg1 → remote static K             D→P HS{1, msg2}
        lookup(K) → authorization or FAIL       P→D HS{RECOVERY, 2, msg3 + name}
 D→P HS{1, IK msg2}; split                          D: split; begin(transcript hash)
     → AUTHENTICATED(slot)                      D→P EV_CONFIRM_ON_DEVICE{1, 20} (enc)
                                                    user: 3 presses (button events)
                                                    D: state CONFIRMED → apply(hash, P, name, id)
                                                D→P ENROLL_RESULT{0} (enc) → AUTHENTICATED(0)
                                                    abort/timeout → encrypted error REFUSED, CLOSING
```

- `msg_index` must be 0, then 2 (XX) / 0 (IK) from the phone; the device answers 1. Any other order
  is HANDSHAKE_FAILED.
- Noise payloads: XX msg 1 and msg 2 empty, msg 3 = `name str(0..24,text)`; IK msg 1 and msg 2 empty.
  A non-conforming payload is HANDSHAKE_FAILED.
- ENROLL_RESULT is the encrypted RESPONSE to the HS msg-3 request (same `txn`), the first message of
  the device's transport CipherState.
- The device's static key consistency (public = X25519(private)) is checked with noise-c's DHState
  once per boot before the first handshake that uses it; a mismatch calls `dsp_app_sec_key_fault`
  (dosing inhibited at once, M1.10b.3) and fails the handshake.
- Unknown phone key, revoked slot, wrong PSK, wrong invite: the handshake fails (MAC failure or
  lookup refusal) with a plain HANDSHAKE_FAILED; no hint which input was wrong.

---

## 8. Protocol changes (one coordinated change)

| Change | Spec | Generator + vectors | C codec | Kotlin codec |
|---|---|---|---|---|
| HELLO `flags` bit 1 CREDENTIALS_DAMAGED; Info `flags` bit 1; advertising flags bit 2 CREDENTIALS_DAMAGED (bit 0 CLAIMED, bit 1 RECOVERY_ARMED unchanged); degraded devices report `claim_state` 1 | section 5.1, 7.1 | masks 0x03 / 0x07, new accept and reject vectors (bit 2 / bit 3 still rejected) | `F_MASK8` 0x03, Info/advert checks | same masks |
| `device_id_hash` definition | 5 + new section 11 | hash vectors (Python `hashlib`) | — (value carried as-is) | — |
| Session security (normative): prologue, PSK, sequences, payloads, ENROLL_RESULT placement, close rules, state table | new section 11 "Session security" | prologue/PSK vectors (Python `hashlib`/`hmac`, independent of noise-c) | — | — |
| **Error 20 UNAVAILABLE** (recognized operation not provided) vs UNKNOWN_TYPE (undefined type) **[C5, revised]** — done in M1.10c.1 | 5.1, new 8.1 | `error_unavailable_wifi`, reject `error_code_21` | enum 1..20 | range 1..20 |
| Pre-authentication rules (gate) — done in M1.10c.1 | new 10.6 | — | `dsp_link` | — |
| Retry rule: duplicate `txn` answered from the bounded per-session cache; first response wins **[C4, cache kept]** | new 10.4a | — | M1.10c.2 | M1.10e |

No other wire change: the minor version stays 0 (protocol v1.0 not released; the flag and the error
code are added before release, as `manual_floor` was). Status: the flag, advertising and error
rows are implemented in M1.10c.1 in spec, generator, vectors and both codecs (also an
`advert_bad` reject record kind). `device_id_hash` and session security follow in M1.10c.2.

### 8.1 Retry and idempotency analysis (C4)

**Two mechanisms, two purposes.** The *transport response cache* (design 8.1) answers a duplicate
`txn` within one security session with the stored response bytes, without executing again.
It lives in RAM only and is lost at disconnect, which always ends the session (C7). The *durable
manual-dose anti-replay* (M1.10b.2) decides a `seq` once per slot, across connections, reboots
and power cuts. The cache never replaces the durable mechanism, and the durable mechanism
covers only MANUAL_DOSE.

**When can a duplicate arrive?** Noise transport makes a byte-identical replay impossible: the
nonce advances, so a replayed or reordered ciphertext fails to decrypt and closes the connection.
GATT delivery is reliable inside a connection, so a response is "lost" only when the connection
drops, and then the session ends and no cache exists. A duplicate inside one session therefore
arises only from an **application retransmission**: the phone's response timeout (5 s; 60 s for
MANUAL_DOSE) fires while the device is still working or the response is still queued, and the
phone re-sends the request with the same `txn` as a new ciphertext. A **late original response**
can then also arrive after the retry was sent.

Behavior per operation without a cache (re-execution) vs with it:

| Operation | Re-executed in the same session | Across connections (no cache anyway) | Equivalent without cache? |
|---|---|---|---|
| GET_STATUS / GET_DIAGNOSTICS / GET_CONFIG / LIST / GET_SCHEDULE / GET_RESULT / PHONES_LIST | read-only, fresh data | read-only | yes |
| ACK_REJECTIONS | the second call finds nothing unread: OK, no write | same | yes |
| REFILL `{capacity, remaining}` | absolute values: the same state again. But one more two-phase write (R1–R4, ≈ 2–3 records), one more write-budget unit, `refill_seq` advances; no dose can interleave (any pump run suspends BLE and ends the session) | user re-confirms after reading Status | **effect yes, cost no** (extra flash writes, budget) |
| SET_READINESS | absolute target; already at the target → no write; READY re-checks gates | same | yes |
| SET_CONFIG `expected_revision` | `CONFLICT{current revision}`; nothing written twice | read-back decides (8.2) | **safe, but the outcome differs**: the phone sees CONFLICT instead of OK unless it takes the first response or reads back |
| CREATE_SCHEDULE (`revision` 1, phone ID) | `DUPLICATE` (ID active), never created twice | same | safe, outcome differs |
| UPDATE / DELETE_SCHEDULE `expected_revision` | `CONFLICT` / `NOT_FOUND` or `DUPLICATE` | read-back | safe, outcome differs |
| SET_TIME `{phone_utc_ms}` | an accepted phone time makes the clock trusted (`DSP_TRUST_SYNCED`), so the repeat answers `IGNORED_NTP_PREFERRED` with offset 0 and applies nothing. A first attempt that was *not* accepted (HOLD, rejected) is decided the same way again | phone re-samples the time | safe, **outcome differs** (the phone may lose the original decision and offset) |
| MANUAL_DOSE `seq` | `DUPLICATE{state}` from the durable record; never pumps twice. The session normally ends at BLE suspension anyway | `GET_RESULT(seq)` | yes, by the **durable** mechanism (the cache is irrelevant here) |
| PHONE_REMOVE `slot` | `NOT_FOUND` (already free) or `REFUSED LAST_SLOT`; a slot cannot be re-filled within the session (enrollment needs another connection). Own slot: the session ends after the first response, so no retry | PHONES_LIST decides | safe, outcome differs |
| INVITE_CREATE | **a second, different token replaces the first** (one active invite). If the late first response arrives and the phone uses "first response wins", it shows a **token the device has already discarded**: ADD then fails (fail-safe, but broken UX), and the extra RTC write repeats | user creates a new invite | **no** |
| INVITE_CANCEL / RECOVERY_CANCEL | clears an already clear RTC overlay: OK (one extra RTC save) | same | yes |
| HS / ENROLL_RESULT | not cacheable: handshake messages are a strict sequence (`msg_index`). A duplicate message 3 is HANDSHAKE_FAILED. A lost ENROLL_RESULT ends the connection; the phone then tries IK SESSION with its new key (success = it was enrolled) or repeats CLAIM/ADD (refused if already enrolled: the device is CLAIMED or the invite was consumed) | IK decides | n/a (own rule) |
| END_SESSION | authorized: ends the window, a retry finds no session; unauthenticated: closes the connection, a retry needs a new connection | — | yes |

**Result:** removing the cache is **not** equivalent for INVITE_CREATE: the device state differs,
because the delivered token is not the active one. REFILL costs extra flash writes and write
budget. For SET_CONFIG, the schedule writes, SET_TIME and PHONE_REMOVE it is safe but changes
the visible outcome (CONFLICT, DUPLICATE, NOT_FOUND, IGNORED instead of the original result),
unless every client applies "first response for a `txn` wins".

**C4 approved (2026-11-17): keep the eight-response cache.** Specification for M1.10c.2:

- **Binding.** Each entry stores the authenticated connection (session ID and transport
  generation), the ownership epoch and the phone slot of the issued authorization at execution
  time, and the request identity: `txn`, `type`, plus the length and SHA-256 of the decrypted
  request (header + body), computed with libsodium's `crypto_hash_sha256`. No custom primitive.
  A lookup matches only if every bound field equals the current session's.
- **Duplicate, identical payload:** the stored encoded response is re-encrypted with the next
  transport nonce and sent; the operation is **not** called again. This applies to every request
  type answered in AUTHENTICATED state, read-only ones included, so the "original result"
  holds without exceptions. HS, ENROLL_RESULT and END_SESSION are not cached.
- **Same `txn`, different content** (type, length or digest differs): rejected with
  `MALFORMED`, not executed. The entry is kept, so a later identical retry still gets the
  original.
- **Clearing (secure, `sodium_memzero`; entries may hold an invite token):**
  - all entries on disconnect, security-session end (including CLOSING after a security error),
    the authorized end of the window, BLE suspension for the pump, and reboot or deep sleep
    (RAM only);
  - on any ownership change (epoch change: phone removal, recovery, claim, KEY_FAULT), every
    entry created before the change. The response of the ownership-changing request itself is
    inserted after the clear, bound to the new epoch, so its own retry is answered exactly once.
  - An own-slot removal ends the session, which clears everything.
- **Eviction:** oldest first (8 entries).
- **Not exactly-once across reconnections.** The cache lives and dies with one authenticated
  connection. After a reconnect the durable mechanisms decide: revisions, invite single use, and
  the **durable manual-dose anti-replay, which stays authoritative** for MANUAL_DOSE (the cache
  never replaces it; a MANUAL_DOSE retry inside the session is answered from the cache, and
  across sessions `DUPLICATE`/`GET_RESULT` from the durable record).
- **Measured RAM** (prototype of exactly this entry layout, RISC-V `riscv32-esp-elf-gcc`,
  ESP-IDF v6.1): entry 576 B (bindings 56 B + response slot 520 B = `DSP_BLE_FRAME_MAX`). Cache
  of 8 entries + cursor = **4,612 B** static DRAM in `dispenser_security`, versus 87,463 B used
  of 321,296 B DRAM in the rt configuration. A byte ring could save ≈ 3 KB but is not adopted
  (simplicity, fixed worst case). The figure is re-measured on the final M1.10c.2 implementation.
- Client rule (spec 10.4a): retransmit with the same `txn` and an identical body; take the
  first response; a new SET_TIME sample uses a new `txn`.

---

## 9. Resources (estimates of revision 1; measured values: section 14.3)

| Item | Estimate | Basis |
|---|---|---|
| Flash (runtime image with security linked) | +65–75 KB: noise-c 12.3 KB + libsodium 39.0 KB (measured, gate) + `dsp_bleproto` 10.7 KB (measured, now linked) + `dsp_blesec`/`dsp_dispatch` 6–10 KB; ≈ 305–315 KB total, > 82 % headroom in the 1.75 MiB slot | M1.10a gate, RISC-V objects |
| Static DRAM | +4.5–5 KB: frame queue 4 × 524 B = 2,096 B; decrypt + encode buffers 2 × 520 B; request/response bodies 2 × ≈ 460 B (32-bit); session state ≈ 300 B; Info/advert 36 B; noise-c 199 B | design section 6, F5 |
| Heap | peak ≤ 3 KB per handshake (host 1,761 B, 11 allocations; target to be measured), ≈ 2 × 100 B CipherStates while AUTHENTICATED; 0 after close | gate |
| Stack (application task, 12 KiB) | handshake chain ≈ 2.5 KB (Noise 1.9 KB + glue); worst operation chain via the dispatcher ≈ 3.6 KB (manual dose 3,008 B + dispatcher/session frames) | M1.10b.3 measurement + estimate |
| NVS | none new (credentials exist; failed-session counter exists) | — |
| BLE task (M1.10d) | reassembly 520 B + TX 520 B | design section 6 |

Budget gate (unchanged from the design): image ≤ 85 % of the slot and ≥ 64 KB free heap after NimBLE
init (checked in M1.10d with the real stack).

---

## 10. Test strategy and acceptance criteria

### 10.1 Tests (host, ASan/UBSan, real noise-c and libsodium built for the host)

| Area | Tests |
|---|---|
| Library | official cacophony vectors for `Noise_XXpsk3_25519_ChaChaPoly_SHA256` and `Noise_IK_25519_ChaChaPoly_SHA256` in firmware CTest (strict: executed, 0 skipped) |
| Derivations | prologue, `device_id_hash` and PSK vectors generated with Python `hashlib`/`hmac` (independent of noise-c), checked by C now and by Kotlin/JNI in M1.10e |
| Transcripts | full CLAIM, ADD, SESSION and RECOVERY transcripts with a deterministic RNG (fixed ephemerals and identity), stored as vectors; a host "phone" built from the same noise-c drives the device side through the real `dsp_app` (fake transport); Android re-runs them in M1.10e |
| Negative (handshake) | wrong setup code, wrong invite token, expired / consumed invite, revoked or unknown phone key, wrong device identity expected by the phone, prologue mismatch (purpose, version, session ID from an earlier connection, device hash), `msg_index` out of order, payload too long or malformed, handshake timeout per step, 3 failures end the service session, RNG failure |
| Negative (transport) | every privileged type before authentication → UNAUTHENTICATED and **no domain call** (spy on every `dsp_app_*` entry); plain request after authentication → close; bit flips in ciphertext, tag and header (AD) → close; truncated body; replayed, reordered and duplicated frames → close; nonce limit → close; frames of an old connection generation discarded |
| Authorization | every operation × {no session, handshake incomplete, CONFIRMING, AUTHENTICATED slot k, authorization of another connection, after own removal, after recovery, caller-built `{OWNER, slot}`}: FORBIDDEN unless issued for this connection and epoch; ownership effects visible only to the right slot |
| Lifecycle | END_SESSION, disconnect, 15-minute cap (EV_SESSION_CLOSING at T−30 s), pump suspension closes the security session and resume requires a new handshake, no resume after the deadline, reboot/sleep (no session or authorization in the next wake), zeroization (memory scan of session state after close) |
| Recovery over BLE | full sequence with button events; abort paths (timeout, wrong count, session end, due dose) close the session |
| Fuzz | random frames and fragments in every state (no crash, no domain call before authentication), bounded CI run with a kept corpus |
| Codec | HELLO/Info/advert flag vectors in C and Kotlin (existing shared-vector runners) |
| Target | build-only configuration with the security layer linked (0 warnings, sizes, static stack); the RNG registration test |

### 10.2 Acceptance criteria (M1.10c)

**Software (M1.10c.2a) vs hardware (M1.10c.2b) verification.** Criteria 1–3, 5 (host part), 6, 8 and
10–13 are verified in software: host tests under ASan/UBSan, the independent vectors, the
linux-target provider check, mutation tests, cross-builds and static analysis. Criterion 4 is
verified by host tests (close rules, zeroization scan of the session state). Criterion 7 is split:
the cross-build part (0 warnings, measured flash and DRAM, static stack) is software; the target
measurements are hardware (R7). Criterion 9 is software for the host behavior (failing source,
session ID 0, no handshake, no token) and hardware for the target RNG path (R1–R4). The hardware
items are in `firmware/HARDWARE_ACCEPTANCE.md` 8a. Status per criterion: section 14.2.

1. All existing suites pass; new suites pass with 0 skipped vectors.
2. No unauthenticated frame reaches any privileged `dsp_app_*` call (spy proves 0 calls).
3. Every `dsp_app_*` operation refuses a caller-built authorization (FORBIDDEN, no storage access).
4. Every security error closes the session and zeroizes it; nothing survives a reboot.
5. 15-minute cap and pump suspension rules unchanged (existing tests) and extended to sessions.
6. Protocol flag implemented identically in spec, generator, vectors, C and Kotlin codecs (Kotlin
   codec unit tests pass; no Android app behavior change).
7. Target build with the security layer: 0 warnings, measured flash/DRAM/stack within section 9.
8. Mutation pass on the security checks (state gate, authorization check, close-on-error, nonce
   handling glue, prologue binding) caught by the tests.

**Acceptance-criteria changes from the 2026-11-17 decisions:**

- 7 is extended: a successful target build is **not** sufficient for the RNG. The ESP32-C3
  runtime test of section 6.1 (R1–R4, R6) must pass **before** M1.10c.2 starts. R5 is recorded.
- New 9 (C2): the RNG fails closed. No randomness consumer works without the registered
  implementation and an active entropy source (host test with a failing source, done in
  M1.10c.1; target R3).
- New 10 (C1): the PSK uses the approved `info` formula. Independent Python vectors (boundary,
  salt/info-swap and bit-flip negatives) give byte-identical results in the firmware host tests,
  on the target (R6) and on Android. Intermediate secrets are cleared on success and on every
  error path (host buffer scan).
- New 11 (C4): the eight-response cache with the bindings of 8.1. For each request type, a
  same-session duplicate with an identical payload returns the original response without a
  second execution (no extra storage write, no second invite token, the original SET_TIME
  decision). A reused `txn` with different content is rejected unexecuted. No hit after a
  disconnect, reconnect, epoch change (except the changing request's own entry), session end
  or reboot. Entries are zeroized when cleared. RAM is re-measured (prototype 4,612 B).
- New 14 (M1.10e, **open**): Android JNI interoperability, verified independently of the
  host tests (section 14.4). It is not met by M1.10c.2a host interoperation.
- New 13 (C10): an unauthenticated END_SESSION, failed handshakes, protocol errors and
  unauthenticated timeouts close only their connection. The service window continues and the
  owner is served afterwards. A forged or unissued authorization cannot end the window. Repeated
  handshake failures leave no lockout within the window or into the next one (`failed_sessions`
  at most +1 per window, diagnostic only). Done in M1.10c.1 (`test_session`).
- New 12 (C5): `UNKNOWN_TYPE` for undefined type bytes and `UNAVAILABLE` for recognized
  unprovided operations are distinguished in spec, vectors, both codecs and dispatcher tests
  (codec part done in M1.10c.1).
- 2 and 3 are already met by M1.10c.1 for the transport gate and the Application Service (section
  13). M1.10c.2 re-proves them through the dispatcher.

---

## 11. Decisions (status 2026-11-17)

| # | Decision | Status |
|---|---|---|
| C1 | PSK derivation | **Approved:** approved formula with context in HKDF `info`; mbedTLS/TF-PSA-Crypto on the ESP32-C3, libsodium on Android, Python standard library vectors; byte-identical results, boundary and negative (salt/info-swap) vectors, secure clearing of intermediates (section 7.1) |
| C2 | RNG over `esp_fill_random`, registered before `sodium_init`, only with an active entropy source, fail closed | **Approved** with section 6 and the mandatory target runtime test 6.1 (gate for M1.10c.2) |
| C3 | Source delivery: component manager with exact pins, committed lock, project-scoped patch step; host copy prepared by a hash-checked script | **Approved** |
| C4 | Idempotency cache | **Approved: keep 8 entries**, bound to connection, epoch, slot and request identity; identical duplicate → original result, reused `txn` with different content → rejected; cleared on disconnect, ownership change, session end, reboot; not exactly-once across reconnections (the durable manual-dose anti-replay stays authoritative); RAM 4,612 B measured on a prototype (section 8.1) |
| C5 | Unsupported operations | **Approved as revised:** error 20 `UNAVAILABLE` for recognized operations not provided by this firmware; `UNKNOWN_TYPE` only for undefined types (implemented in M1.10c.1) |
| C6 | Malformed decrypted body: encrypted MALFORMED, then close | **Approved** |
| C7 | Security-session scope = connection | **Approved** (implemented) |
| C8 | New component `dispenser_security` | **Approved** |
| C9 | Split into M1.10c.1 / M1.10c.2 with a review between | **Approved** |
| C10 | END_SESSION scope | **Decided and implemented in M1.10c.1:** unauthenticated END_SESSION closes only its connection; the service window ends only by an authorized owner (`dsp_app_end_service_session`) or an internal timeout/safety transition; failed handshakes never end the window or lock out later windows (section 3) |

---

## 12. Implementation plan

| Step | Content | Exit |
|---|---|---|
| M1.10c.1 (**implemented, section 13**) | Protocol: flag bit 1/advert bit 2, error 20, pre-authentication rules (`device_id_hash` and the session-security section moved to M1.10c.2: they depend on C1); generator, vectors, C and Kotlin codecs. Domain: issued-authorization check in every operation, new status/diagnostics/ack/refill/readiness operations, retire unauthenticated queue kinds, frame queue + transport events + port change, connection-scoped security session. Tests migrated to issued authorizations (fake security layer). | host suites, Kotlin codec tests, vectors |
| M1.10c.2a (**implemented, section 14**) | Source delivery (C3), `dispenser_security` with `dsp_blesec` and `dsp_dispatch`, RNG registration (platform), prologue/PSK, handshakes and transport, close rules, lifetime integration; vectors, negative and fuzz tests; target build-only configuration and measurements | section 10.2 (software part) |
| M1.10c.2b (**open**) | ESP32-C3: RNG R1–R6, live BLE handshakes (with M1.10d), target stack, heap and timing, lifecycle on the device | `HARDWARE_ACCEPTANCE.md` 8a |
| M1.10d | NimBLE adapter (byte transport, Info/advert publication, fragmentation), button driver, budget gate | as planned |

---

## 13. M1.10c.1 implementation record (for review; not committed)

### 13.1 What changed

> **Superseded in part by M1.10d.1** (`docs/BLE_TRANSPORT_DESIGN.md` T2/T3): connection events
> are no longer queue items but an authoritative connection snapshot beside the frame queue
> (`dsp_app_post_conn_event` never refused for space, strictly increasing generations;
> `dsp_app_post_rx_fault` added); the BLE port's `send_frame` and `disconnect` take the connection
> generation; the security layer gained `on_rx_fault` (plain MALFORMED only before a handshake
> started). The table below records the M1.10c.1 state.

| Area | Change |
|---|---|
| Queue | Opaque items `dsp_app_qitem_t {kind, gen, mtu, len, button, bytes[520]}`, capacity 4: FRAME, CONNECTED, DISCONNECTED, BUTTON. Producers `dsp_app_post_frame`, `dsp_app_post_conn_event`, `dsp_app_post_button` fill the tail slot under the lock (no frame copy on the producer's stack). The command/response structs and `dsp_app_post` are removed. The queue kinds STATUS, REFILL, SET_READINESS and END_SESSION are retired (MANUAL_DOSE already was) |
| BLE port | Byte-only: `send_frame`, `disconnect`, optional `publish_info(info[32], advert[4])`. A configuration without `send_frame`/`disconnect` is rejected |
| Gate | `dsp_link` (pure domain): IDLE/OPEN/NEGOTIATED per connection generation; the rules of `docs/BLE_PROTOCOL.md` 10.6. Privileged types are never decoded. 3 errors or 30 s close the connection. **C10:** a plain END_SESSION is answered and closes only its connection (the gate has no action that could end the window) |
| End of the window (C10) | `dsp_app_end_service_session(app, auth)`: only with an authorization issued for this connection (FORBIDDEN otherwise, the window continues); not rate limited, no storage precondition. `DSP_BLE_END_COMMAND` now means "ended by the authorized owner". Failed handshakes never end the window (`DSP_BLE_END_AUTH_FAILURES` removed) |
| Session scope (C7) | `dsp_app_sec_connection_begin/end`: a new random session ID per connection (0 when no security layer or no randomness). End drops the handshake handle, the issued authorization and a pending recovery confirmation (it counts as failed). Called on CONNECTED/DISCONNECTED, stale generations, BLE suspension for the pump and link close; nothing survives a reboot |
| Authorization | `guard()` now requires `issued_ok()`: a registered security layer, a valid issued authorization, matching connection session ID, non-zero matching token, slot, current ownership epoch and CLAIMED credentials. It applies to **every** `dsp_app_*` operation (configuration, schedules, time, manual dose, GET_RESULT, floor, the new operations, ownership) |
| New operations | `dsp_app_get_status` (Status record incl. the slot's `manual_floor`, `session_seconds_left`, battery 255), `dsp_app_get_diagnostics`, `dsp_app_ack_rejections` (no write when nothing is unread), `dsp_app_refill`, `dsp_app_set_readiness` (READY refused with gates/pump problems). Writes count against the session write budget |
| HELLO / Info / advert | `claim_state` 0 only for a valid UNCLAIMED record. CREDENTIALS_DAMAGED for a degraded or unreadable record (never "unclaimed"). RECOVERY_ARMED from the RTC arm. `caps` = PHONE_TIME. `device_id_hash` from an optional security-layer callback (8 zero bytes until M1.10c.2). Info and advertising are republished at session start |
| Protocol | HELLO/Info flag bit 1, advert bit 2, error 20 UNAVAILABLE. Spec, generator (`advert_bad` record kind added), vectors (reproducible), C codec, Kotlin codec and runner |

Deliberate c.1 semantics:

- With unreadable storage no authorization can be issued, so operations answer FORBIDDEN
  (formerly DEGRADED for some); HELLO still reports CREDENTIALS_DAMAGED.
- Every connection is unauthenticated and dropped after 30 s; HS always fails.

### 13.2 Verification

| Check | Result |
|---|---|
| Host suites (ASan + UBSan, `-Werror`) | 24/24 pass, 1,519,626 checks, 0 failed (after C10). New: `test_link` 250,991 checks (HELLO/negotiation, HS refusal, all 24 privileged types UNAUTHENTICATED by raw-byte check, encrypted close, unknown type, END_SESSION, 30 s expiry, 100k-frame fuzz); `test_session` 379 checks (C10: 21 unauthenticated END_SESSIONs, forged/unissued authorized end, owner served afterwards, window to its own timeout; 3 consecutive windows × 10 failed handshakes with the owner authenticating each time and `failed_sessions` +1 per window; no frame reaches an operation or a storage write; 21 operations × {caller-built, earlier connection, epoch change} refused; field-by-field tampering; RNG failure fails closed; HELLO/Info/advert for claimed, unclaimed, damaged, armed; disconnect / stale generation / pump suspension / reboot scope; new operations) |
| Migrated suites | `test_app` (chaotic year: 762 automatic + 32 manual runs, 66 power cuts), `test_app_ops`, `test_manual`, `test_owner`, `test_platform`. The harness now obtains real issued authorizations through `dsp_app_sec.h` with a deterministic fake RNG; caller-built authorizations are exercised separately |
| ThreadSanitizer (`test_platform`, 2 producer threads) | 3 runs, 0 races; 400 frames posted, 401 sent, 0 stale; the final plain END_SESSION closes only the connection and the window ends at its maximum |
| Android codec | `:app:testDebugUnitTest` 284 tests, 0 failures (no app behavior change) |
| Vectors | `gen_ble_protocol_v1.py` output byte-identical to the committed file |
| Mutation (12 mutants on `issued_ok`, `guard`, `dsp_link`, session-ID RNG path, and C10: plain END ending the window, a handshake failure ending it, a forged authorized end ending it) | all caught (the epoch comparison is caught only by the white-box tamper test: no API path can leave a stale epoch bound) |
| ESP32-C3 cross-builds (default, diag, rt, rtc) | exit 0, 0 warnings. Images: default 148,480 B and diag 1,042,432 B unchanged; rt 250,192 B (+11,968 B vs `3ae2592`: `dsp_bleproto` and `dsp_link` now linked); rtc 1,116,672 B (+11,920 B). DRAM used: rt 87,463 B, rtc 143,140 B of 321,296 B. `dsp_app_t` 20,448 B on RISC-V (+2,976 B: frame queue, rx item, tx buffer) |
| Static stack (RISC-V, `-fcallgraph-info`) | frame path `handle_item` → `dsp_link_on_frame` → codec 1,424 B. The worst application chain is still the dose path (`dsp_app_manual_dose` 3,472 B) within the 12 KiB task stack |

| C4 cache RAM (prototype, RISC-V) | 4,612 B for 8 entries (576 B each); planned in `dispenser_security`, not in this build |

### 13.3 Not done in M1.10c.1 (by scope)

Noise, `dsp_blesec`, the dispatcher, the PSK, `device_id_hash` computation, the secret port, the
response cache, the RNG registration and its **target runtime test (6.1, needs the board)**,
NimBLE/GATT (M1.10d), Android behavior (M1.10e). Hardware acceptance 9.4 stays open.

### 13.4 Risks

- **Connection occupancy (remaining finding, M1.10d):** with a single BLE connection, a nearby
  client can keep reconnecting and occupy the only connection, each time for up to 30 s
  unauthenticated (or until its third error). It cannot end the window (C10), but it can delay
  the owner within it. Mitigation for the NimBLE adapter (M1.10d): allow a second concurrent
  connection reserved for a new client, or a short per-address reconnect back-off while the
  window is open. Both are bounded by the window lifetime and RAM-only. **M1.10d decision T1
  (approved):** single connection with immediate re-advertising and per-window contention
  counters; measured on hardware (`HARDWARE_ACCEPTANCE.md` 8b H9).
- The 30 s unauthenticated limit also bounds the M1.10c.2 handshake (HELLO + 3 HS messages).
  It is measured on the board.
- Nothing in the runtime configurations registers a BLE port or security layer yet (M1.10d), so
  M1.10c.1 has no field-visible effect beyond code size.

---

## 14. M1.10c.2a implementation record (software integration; for review, not committed)

### 14.1 What was built

| Area | Implementation |
|---|---|
| Source delivery (C3) | `firmware/third_party/noise-c/noise_c_deps.cmake`: noise-c `62c2fdca…` and the libsodium fork `95c5e60d…` (upstream `d24faf56…`) fetched by full SHA, commits re-verified, patches 0001/0002 at zero fuzz, patched-file SHA-256 pinned. ESP-IDF: `EXTRA_COMPONENT_DIRS` (`esphome__noise-c`) + registry `esphome/libsodium ==1.10021.12` (`firmware/dependencies.lock`). Host: the same trees as CMake targets. Any mismatch stops the configuration |
| HKDF (C1) | `dsp_secderive.c` (PSK, prologue) over `dsp_hkdf_sha256()`: `dsp_hkdf_psa.c` (mbedTLS / TF-PSA-Crypto, device) and `dsp_hkdf_sodium.c` (libsodium, host and Android provider). `dsp_idhash.c` (SHA-256, libsodium). Intermediates cleared on every path; PSA derivation aborted on every path; the secret is passed as bytes, so there is no key object to destroy |
| Session (`dsp_blesec`) | Per connection: the M1.10c.1 gate for HELLO, END_SESSION and errors; XXpsk3 (CLAIM, ADD, RECOVERY) and IK (SESSION) as the Noise responder; prologue binding; the identity-key consistency check (KEY_FAULT on mismatch); the evidence handle kept from admission to commit; IK authorizes the phone key before answering; ENROLL_RESULT as the encrypted response to message 3; CONFIRMING with encrypted BUSY 2 and the three-press confirmation; encrypted frames with the header as AD; per-direction message limit; close on every security error; encrypted MALFORMED then close (C6); class check of decrypted types; plain END_SESSION closes only the connection (C10); events EV_SESSION_CLOSING, EV_SUSPENDING_FOR_DOSE, EV_CONFIRM_ON_DEVICE; zeroization and freeing of every Noise object at each close |
| Dispatcher | `dsp_dispatch.c`: one `dsp_app_*` call per request with the issued authorization; 1:1 error mapping; UNAVAILABLE for WIFI_* and FACTORY_RESET (C5); the encrypted END_SESSION calls `dsp_app_end_service_session` (C10) |
| Response cache (C4) | `dsp_rescache.c`: 8 entries bound to (session ID, connection generation, epoch, slot, txn, type, SHA-256 of the plaintext); an identical duplicate gets the original response re-encrypted; a reused txn with different content gets MALFORMED, not executed; purged on an epoch change before lookup and after dispatch; cleared and zeroized at every close |
| Domain hooks | `dsp_app_security_t` gains `on_connection`, `on_frame`, `on_tick`, `on_suspend`. The restricted `dsp_app_sec.h` gains `connected`, `hello`, `send`, `drop`, `now_ms`, `window_seconds_left`, `epoch`. Without hooks the M1.10c.1 behavior is unchanged |
| Target RNG adapter (C2) | `dispenser_platform/src/dsp_esp_rng.c` (section 6 refinements); setup-code reader `dsp_esp_setup_code_read` (`nvs_sec`); runtime registration under `CONFIG_DSP_BLE_SECURITY` (default off, build-only; refused with `CONFIG_DSP_APP_ALLOW_PRODUCTION`) |
| Deterministic randomness | only `firmware/test/host/support/test_rng.c`, registered through the same libsodium interface; `dsp_blesec.c` and `dsp_esp_rng.c` refuse `DSP_TEST_DETERMINISTIC_RNG` on ESP-IDF (`#error`) |
| Protocol | `docs/BLE_PROTOCOL.md` section 11 (normative session security); vectors `test-vectors/ble_session_v1.txt` + generator |

### 14.2 Verification (software only)

| Check | Result |
|---|---|
| Independent vectors | the Python standard library generator self-checks RFC 5869 test cases 1–3; OpenSSL reproduces all 24 PSK records; libsodium (host, `test_secderive`: 24 psk, 12 psk_not, 24 prologue, 6 idhash) and mbedTLS/TF-PSA-Crypto PSA (`test/psk_linux`, ESP-IDF linux target: 24 + 12) are byte-identical. The prologue-length constant was wrong (28 instead of 26) and was caught by these vectors |
| Host suites (ASan + UBSan, `-Werror`) | 26/26, 1,520,845 checks, 0 failed. `test_blesec` 953 checks |
| `test_blesec` scenarios | CLAIM → SESSION → dispatch of 7 operation types; ADD in the same window after A's plain END; RECOVERY with confirmation; handshake negatives (wrong setup code, prologue session ID, msg_index 0/2, message-3 index, no entropy, unknown key, wrong device identity, CLAIM on a claimed device, RECOVERY not armed, step timeout, inconsistent identity key → KEY_FAULT, name too long, consumed invite); transport manipulations (ciphertext, tag and AD bit flips, truncation, replay, plain request after auth, encrypted HELLO); cache (identical duplicate, conflicting txn of a different type and of the same type, epoch purge); C10 (plain END, forged authorized end, failed handshakes, END during confirmation); lifecycle (EV_SESSION_CLOSING, manual dose → EV_SUSPENDING_FOR_DOSE and session gone); pre-auth fuzz (240 frames, 0 operations); zeroization scan after every close |
| Mutation (blesec, rescache, dispatch) | 15 mutants caught (prologue binding, decrypt failure, cache hit and conflict, plain request after auth, tokens without entropy, key consistency, name length, type class, IK lookup, message-3 index and purpose, END during confirmation, cache digest, UNAVAILABLE, both epoch defenses removed). Equivalent by layered design: removing either epoch defense alone (entry binding or purge); an authorized end attempted in CONFIRMING (no authorization exists, the domain refuses it). Removing both epoch defenses is caught |
| ThreadSanitizer (`test_platform`) | 3 runs, 0 races |
| Static analysis | `gcc -fanalyzer` on all security sources and `dsp_link.c`: 0 findings (cppcheck and clang-tidy not installed) |
| ESP32-C3 cross-builds (7 configurations) | all exit 0, 0 warnings (section 14.3) |
| Not verified (M1.10c.2b) | target RNG path (R1–R5), PSK on the device (R6), BLE runtime and live handshakes, target stack, heap and timing (R7), device lifecycle (R8) |
| Not verified (M1.10e) | Android JNI interoperability (vectors, handshakes, negatives from the Android side) |
| Manual-dose oracle with BLE dispatch | `manual_via_ble` replaces only the "scripted OP_MANUAL step running" clause. The durable-decision checks (marker, high-water mark, ring entry PENDING before the pump) are still evaluated on every manual pump run. Evidence: the encrypted MANUAL_DOSE runs the pump with 0 violations (asserted). A domain mutant writing the decision as DONE instead of PENDING is caught with the flag set. With the flag off the same oracle fires (the flag only marks the BLE path). Only `test_blesec` sets it, around one window |

### 14.3 Measured resources (ESP32-C3, ESP-IDF v6.1)

Baseline: the committed `origin/main` `c650153`. Its firmware tree is identical to `613f146`
(M1.10c.1); `c650153` changed only Android build files. Each configuration was built from the
committed sources and from the working tree with the same scratch `sdkconfig` defaults.

| Configuration | Image | DRAM | vs committed baseline (`c650153` = `613f146` firmware) |
|---|---|---|---|
| default / diag | 148,480 / 1,042,432 B | 50,059 / 108,552 B | identical |
| rt / rtc (no security) | 250,480 / 1,116,960 B | 87,463 / 143,140 B | +288 B image (domain hooks), DRAM unchanged |
| sec (rt + `CONFIG_DSP_BLE_SECURITY`) | 436,768 B (23.8 % of the 1.75 MiB slot) | 96,069 B | +186,288 B vs rt: TF-PSA-Crypto 81.2 KB (the PSA core with ESP-IDF's enabled algorithms, absent from rt), libsodium 39.0 KB, noise-c 12.5 KB, `dispenser_security` 9.8 KB, domain +23.3 KB (dispatcher reaches every operation and the codec), main +7.1 KB; DRAM +8,606 B |
| rtcsec (rtc + security; TLS already present) | 1,202,592 B (65.5 % of the slot) | 150,220 B | +85,632 B vs rtc; DRAM +7,080 B |
| secbt (sec + BT/NimBLE enabled, compile check of the adapter's BT path) | 440,464 B | 98,439 B | not representative before M1.10d |

- `dsp_blesec_t` is 6,808 B of static state, including the response cache at 4,420 B (measured,
  RISC-V). The C4 prototype figure was 4,612 B.
- Static stack (RISC-V call graph): the worst chain from `on_frame` is the encrypted MANUAL_DOSE
  dispatch, 5,344 B (dose path 3,472 B + security frames). The handshake path is `on_frame` 576 B
  + `hs_first` 640 B + noise-c and X25519 behind function pointers, which the static graph can't
  see (gate estimate ≈ 1.9 KB), ≈ 3.5 KB. All within the 12 KiB application task stack. These are
  estimates; target high-water marks are R7.
- The flash estimate of revision 1 (+65–75 KB) is exceeded in images without TLS, because of the
  PSA core. With TLS present the increment is 85.6 KB. Both are well inside the 85 % budget gate.
  **Decision (2026-11-17): the PSA configuration is not optimized just to meet the old estimate.**
  These figures are the recorded baseline for the security-enabled images.
- Stack and heap on the target stay **open hardware items** (`HARDWARE_ACCEPTANCE.md` 8.1–8.3, R7).
  The figures above are static estimates and host measurements only.

### 14.4 Findings and risks

- **Host Noise interoperability is not independent Noise conformance.** The host "phone" uses the
  same pinned noise-c. Noise conformance rests on the upstream cacophony vectors (M1.10a gate:
  XXpsk3 and IK executed, 0 skipped). Our derivations have independent vectors. Stored transcript
  vectors for the Android re-run are M1.10e.
- **Android interoperability: OPEN acceptance item for M1.10e.** Host interoperability with the
  same noise-c implementation does not substitute for independent Android verification. M1.10e
  must show all of the following:
  - the Android JNI build (noise-c + libsodium, FORTIFY on) reproduces `ble_session_v1.txt`
    byte for byte with its libsodium HKDF;
  - CLAIM, ADD, SESSION and RECOVERY interoperate with the firmware's `dsp_blesec` (stored
    transcript vectors, then the device in M1.10c.2b/M1.10d);
  - the negative and transport-manipulation cases run from the Android side.
  The host test phone uses the provider function libsodium exposes to Android, but it is not the
  Android build.
- **Single BLE connection** (M1.10c.1 residual risk) unchanged; M1.10d.
- **Window-ending clock discontinuities clear RTC overlays** (invites, arm). This is correct domain
  behavior; recorded because host tests must not end a window by a monotonic jump before checking
  overlays.
- Deterministic randomness, production firmware: none. The security layer is not linked unless
  `CONFIG_DSP_BLE_SECURITY` is set; default and diagnostic images are byte-identical to
  `613f146`.
