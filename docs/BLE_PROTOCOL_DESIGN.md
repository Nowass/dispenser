# Dispenser V2 — BLE/GATT protocol design (M1.10, revision 3, DRAFT for review)

Status: **design proposal, not approved, not implemented.** Baseline: firmware M1.9c
(`abe88e6`), Android beta UI with demo data. Nothing here has run on hardware.

Revision 2 incorporates the review of 2026-11-16 (security refinements 1–10, data/protocol
items 11–16). Already approved and kept unchanged: one GATT service with Info, Request and
Response characteristics; a binary versioned protocol; the Application Service as the only
dispatcher; BLE callbacks never touch domain state or the pump; transaction IDs, configuration
revisions and durable manual-dose request IDs; domain work items M1–M9; the M1.10a–f split.

Revision 3 (final review of revision 2) adds the concrete Noise implementation selection with
feasibility evidence (section 4.8) and a durable per-phone anti-replay design for manual doses
(section 7.4). Approved in principle: Noise XXpsk3/IK, per-phone revocable identities, 4 slots,
owner invites, the physical recovery procedure, no BLE pairing/bonding (subject to security
verification), separate Wi-Fi storage, 32-byte UTF-8 schedule names, the binary protocol.

Decisions are marked **[Dn]** and collected in section 13.

---

## 1. Starting point

### 1.1 Firmware (implemented, host-tested)

| Capability | Where | BLE relevance |
|---|---|---|
| Service-mode command queue (bounded), lock contract, session lifetime (180 s idle / kept while connected / 15 min max, monotonic), BLE suspended before every pump run | `dsp_app` (M1.9b), `dsp_esp_app_sync` (M1.9c) | the only entry point from the BLE task |
| STATUS, MANUAL_DOSE, REFILL, SET_READINESS, END_SESSION with a status snapshot | `dsp_app` | reused as protocol operations |
| Dosing gates, limits, durable claim/marker, permits, refill roll-forward, readiness | `dsp_dosing` (M1.9a) | reached only through `dsp_app` |
| Schedules with revision and tombstones; validation bits named like the Kotlin errors | `dsp_schedule_repo` | storage API only, no application command |
| Configuration record incl. calibration and limits | `dsp_config`, `dsp_storage` | storage API only, no application command |
| Timekeeper (trust evidence, rollback hold, dosing floor) | `dsp_timekeeping` | phone time not wired |
| Wi-Fi credentials and API token in the read-only `nvs_sec` partition (developer tool) | M1.7 D5 | user Wi-Fi provisioning needs a separate writable store |
| Notification diagnostics, rejection log | `dsp_notify` | thin read/ack commands |
| 32 history watermark records, no event journal (Q10) | `dsp_history` | no device history to page yet |

### 1.2 Android (beta UI, demo data)

`DispenserRepository` is the single data boundary and already names a future
`BleDispenserRepository`. Schedule IDs are UUID strings (valid firmware IDs as they are), Room
stores µl integers and a Monday = bit 0 weekday mask like the firmware, and validation errors
map 1:1 to `DSP_SCHED_E_*`. Gaps: schedule **names** exist only on the phone (now stored on the
device, section 7.3); Android volumes reach 2^53 µl while the device stores `uint32` µl (the app
must refuse larger values); no device history yet. No Bluetooth code exists. `allowBackup=false`:
anything the app stores is lost on reinstall.

### 1.3 Domain work items (explicit interfaces; never inside the BLE layer)

| # | Interface | Purpose |
|---|---|---|
| M1 | Configuration service: read, validated revisioned update, field restrictions while READY **[D12]** | calibration, limits, reservoir, thresholds |
| M2 | Schedule commands through `dsp_app` (list/get/create/update/delete, names), replan | schedule management |
| M3 | Phone time through `dsp_timekeeper_on_sync` with source `BLE` **[D10]** | FR-032 fallback |
| M4 | Durable per-phone manual-dose anti-replay (dosing record format 2, section 7.4) | no re-execution, ever |
| M5 | Credential store: device identity key, phone slots, invites, recovery state, counters (layout v5, section 7.5) | ownership |
| M6 | Writable Wi-Fi credential store (namespace `dsp_wifi`, section 7.6) | Wi-Fi provisioning |
| M7 | Reset service: ownership recovery vs. factory reset **[D6]** | recovery |
| M8 | Event journal (Q10) | history paging (types reserved) |
| M9 | Service-mode priming / calibration run (FR-025/026) | reserved |
| M10 | Button event input during a session (press/long press), part of the button GPIO work | physical confirmation in recovery and add-phone |

---

## 2. Architecture and responsibilities

```text
Android (BleDispenserRepository)
   │  GATT client: Companion Device Manager scan, connect, MTU, fragments
   ▼
NimBLE host task (platform, M1.10d)              never touches domain state
   │  advertising, GATT server, fragmentation/reassembly of OPAQUE frames,
   │  dsp_app_post_frame() into the bounded queue (M1.9b/c lock contract),
   │  notifies frames handed back by the application task
   ▼
Application task (single owner, dsp_app)
   │  dsp_bleproto  frame codec (pure C, shared vectors)
   │  dsp_blesec    Noise handshake + transport, roles, idempotency cache
   │  dispatch →    existing handlers and the new M1–M7/M10 interfaces
   ▼
Dosing Service / storage / timekeeper / notify   (M1.9 rules unchanged)
```

- The BLE task reassembles fragments into one frame and posts it; it never parses, decrypts or
  authorizes. All protocol, security and domain logic runs in the application task.
- Queue reuse: the M1.9b queue with its lock (`dsp_app_sync_t`), session flag and "posting only
  during a session" rule stays; the element becomes an opaque frame (section 6). The parsed
  command struct remains the internal dispatch type. Queue full → the BLE task sends a locally
  built plain `BUSY` frame.
- BLE never activates the pump: MANUAL_DOSE goes to `dsp_dosing_manual` exactly as today, BLE is
  suspended before the pump, and the result is retrievable afterwards (section 8.3).

---

## 3. GATT design

### 3.1 Service and characteristics

UUID base (random, generated for this project; not registered anywhere and not needed to be):
`36b6XXXX-3457-4e8e-9cba-8126de3f52e4` **[D14]**.

| Characteristic | UUID | Properties | ATT security | Content |
|---|---|---|---|---|
| Dispenser service | `36b60001-…` | primary | — | — |
| Info | `36b60002-…` | read | none | public 32-byte record (section 9.4) |
| Request | `36b60003-…` | write with response | none (application-layer security) | request fragments, client → device |
| Response | `36b60004-…` | notify (+ CCCD) | none | response/event fragments, device → client |

One connection at a time (`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`), one outstanding request per
connection. No Device Information Service.

### 3.2 Advertising (no company identifier) **[D14]**

- Only during a service session (button wake, FR-003); never otherwise.
- Advertising data (21 B): Flags (3) + complete list of 128-bit service UUIDs (18).
- Scan response (28 B): shortened local name `DSP2` (6) + **Service Data – 128-bit UUID** (22:
  2 header + 16 UUID + 4 payload: protocol major, flags (bit 0 claimed, bit 1 recovery armed),
  device tag u16 = first bytes of SHA-256(device ID public key), a display hint only).
- **No Manufacturer Specific Data**, so no Bluetooth SIG company identifier is used or implied.
  (`0xFFFF` is reserved by the SIG for internal testing and must never ship; revision 1's use of
  it is withdrawn.) A company identifier would only be needed if the project later obtains one.
- Public address (factory MAC). Identity is proven by the handshake, never by the address.
- Android: Companion Device Manager association filtered by the service UUID.

### 3.3 MTU

Device preferred ATT MTU 247; Android requests 517; effective `mtu = min`. Fragment payload =
`mtu − 3 − 1`. The protocol works at the minimum MTU 23 (19 data bytes per fragment).

---

## 4. Security analysis

### 4.1 What the radio layer can guarantee

- ESP32-C3 + NimBLE support LE Secure Connections and bonding. With no display or keyboard the IO
  capability is `NoInputNoOutput`, so pairing is **Just Works**: encrypted but unauthenticated.
  An active attacker present during pairing can be a man in the middle.
- Android applications cannot use LE out-of-band pairing (`createBondOutOfBand` is a system
  API), and bond state can desynchronize between phone and device.
- Therefore **ownership must be established and verified at the application layer**, with a
  secret the attacker does not have. BLE link encryption, if used at all, is defense in depth.

### 4.2 Link-layer options compared **[D3]**

| Aspect | A. No pairing/bonding + application security (recommended) | B. LESC Just Works bonding + application security |
|---|---|---|
| Ownership / MITM resistance | application layer (section 5) | identical (application layer); Just Works adds none |
| Confidentiality and integrity of commands | application AEAD | application AEAD + link encryption (defense in depth) |
| Exposure without the application layer | GATT reads of the public Info record; frame sizes and timing visible to a sniffer | only after (unauthenticated) pairing; sizes/timing still inferable |
| Privacy | public address, advertising only during a pressed session | RPA possible with IRK exchange; still advertising only during sessions |
| Android UX | no system pairing dialog, no bond to manage | pairing consent dialog on first connection (vendor-dependent); "forget device" when bonds desync |
| Reinstall / phone change | nothing to clean up at the Bluetooth level | stale bonds on both sides; device bond table must be pruned |
| Firmware cost | no Security Manager use, no bond storage | SM code paths enabled; bonds persisted in NVS (`CONFIG_BT_NIMBLE_NVS_PERSIST`), `MAX_BONDS` slots |
| Attack surface | GATT server only | plus SM pairing implementation (historical pairing-layer flaws exist across stacks) |
| Failure modes | application handshake failures only | plus pairing/encryption failures independent of ownership |

Recommendation **A**. Residual risk: a passive observer sees that a session happens, message
sizes and timing (for example that a short command followed by a disconnect was likely a manual
dose). Content and authority stay protected by the application layer. Option B can be added
later without changing the protocol (it is transparent to the frames).

### 4.3 Trust anchor: per-device setup code **[D1]**

- 128 bits from the device's hardware RNG at provisioning, encoded as 26 Crockford base32
  characters plus a 2-character checksum, printed with the device ID as a QR code
  (`DSP2:<device-id-hex>:<code>`) and as text for manual entry. No short or human-chosen codes.
- Development: generated by the developer provisioning tool into `nvs_sec` (read-only for the
  firmware), shown once to the developer, never committed. Production: written at manufacturing
  (process outside this milestone).
- The device stores the setup code itself (it needs it as a Noise PSK input), only in `nvs_sec`.
- Entropy consequence: with 128 random bits, offline guessing from a recorded handshake and
  online guessing are both infeasible, so **no password-authenticated key exchange is needed**
  (a PAKE protects low-entropy secrets). This is why section 4.4 no longer recommends EC J-PAKE.

### 4.4 PAKE evaluation (review item 2)

Verified against the exact library sources on this machine:

| Library | Version | Finding |
|---|---|---|
| mbedTLS in ESP-IDF v6.1 | 4.1.0 (TF-PSA-Crypto) | J-PAKE available through the **PSA PAKE API** (`PSA_ALG_JPAKE(PSA_ALG_SHA_256)`), P-256 only; the legacy `ecjpake.h` is now a private header. `CONFIG_MBEDTLS_ECJPAKE_C` defaults to **off** in ESP-IDF. Built-in identities are fixed to `"client"`/`"server"`. ZKP hash (Thread / TLS-ECJPAKE encoding): `SHA-256(len‖G‖len‖V‖len‖X‖len‖id)` with 4-byte big-endian lengths and **uncompressed** points, digest read unsigned and reduced mod n. Shared key exported as the **uncompressed point K** (65 B). Only implicit authentication: key confirmation must be added by the protocol. PSA also defines SPAKE2+ algorithm IDs, but the built-in driver implements only J-PAKE. |
| Bouncy Castle | 1.85 (`bcprov-jdk18on`, sources in the local Gradle cache) | `ECJPAKEParticipant` (RFC 8236 style): ZKP hash over **compressed** points (`getEncoded(true)`, 33 B) and the digest converted to a **signed** `BigInteger`; arbitrary participant IDs; keying material = **x-coordinate** of K; optional round-3 key confirmation (NIST SP 800-56A MacTag). |

**Conclusion: the two implementations are not interoperable as shipped** (different ZKP hash
encodings make every proof fail; different shared-key exports). Using EC J-PAKE would require
re-implementing the participant logic on the Android side on Bouncy Castle EC primitives to match
the mbedTLS encoding, plus our own key confirmation: custom cryptographic code. If a short
manually typed code ever becomes a requirement, J-PAKE (or SPAKE2+ once available in TF-PSA-Crypto)
returns as an option with that cost; requirements recorded for that case: P-256, SHA-256,
uncompressed points, Thread encoding, identities fixed to `client` (phone) / `server` (device),
transcript-bound explicit key confirmation, `CONFIG_MBEDTLS_ECJPAKE_C=y`, cross-implementation
vectors.

### 4.5 Recommended protocol: Noise, not a custom design **[D2]**

Review item 3 asks not to implement a custom cryptographic protocol. The recommendation is the
**Noise Protocol Framework** (revision 34), a published, widely analyzed specification with
official test vectors, used unchanged:

| Purpose | Pattern | Why |
|---|---|---|
| Enrollment (initial claim, add phone, recovery) | `Noise_XXpsk3_25519_ChaChaPoly_SHA256` | mutual static-key exchange; PSK mixed in so the handshake succeeds only with the setup secret (and the owner invite for add-phone) |
| Every later session | `Noise_IK_25519_ChaChaPoly_SHA256` | phone knows the pinned device key; the phone's static key travels encrypted (identity hiding); one round trip |

- Cipher suite changed in revision 3 from AESGCM to **ChaChaPoly** **[D2]**: the selected library
  (section 4.8) builds AES out by default and its libsodium backend implements AES-GCM with
  libsodium's `crypto_aead_aes256gcm`, which libsodium provides only on CPUs with AES instructions
  (x86 AES-NI, ARMv8 crypto); the ESP32-C3's RISC-V core has none. ChaCha20-Poly1305 is constant
  time in software and is the suite ESPHome runs on ESP32 devices in production.
- **No Noise or primitive code of our own**: both sides use the same maintained library
  (section 4.8); our code is framing, storage and glue (RNG hook, key handling, JNI binding).
- Prologue (bound into every handshake hash): `"DSP2 BLE v1" ‖ protocol major ‖ minor ‖ purpose ‖
  device_id_pub_hash ‖ session_id` (purpose = SESSION/CLAIM/ADD/RECOVERY), so a handshake cannot
  be replayed across purposes, devices, protocol versions or sessions.
- PSK (32 B) = `HKDF-SHA256(ikm = setup_code_bytes(16), salt = "DSP2 psk", info = purpose ‖
  invite_token_or_empty)`: claim and recovery use the setup code only; add-phone also needs the
  owner-issued invite token (section 5.3).
- Alternative considered (not recommended): a SIGMA-style handshake with P-256 ECDSA keys held
  non-exportable in the Android Keystore. Stronger phone-key protection, but a custom composition
  that would need its own specification review and vectors. Noise trades that for a software
  X25519 key on the phone, wrapped by a Keystore AES key (section 5.1).

### 4.6 Cipher, nonces, key separation, replay, transaction binding (review item 8)

All by the Noise specification, with our framing rules:

- **Unique keys per session:** every handshake uses fresh ephemeral X25519 keys on both sides;
  `Split()` yields two independent ChaCha20-Poly1305 keys (initiator → responder, responder →
  initiator). Keys are never reused across sessions, directions or purposes.
- **Nonces:** Noise CipherState nonce: 64-bit counter starting at 0, encoded for ChaChaPoly as 4
  zero bytes ‖ 8-byte **little-endian** counter (Noise ChaChaPoly rule, RFC 8439 96-bit nonce). Strictly incremented per message; never sent on
  the wire. Counter exhaustion is impossible in a 15-minute session; the implementation still
  closes the session at 2^32 messages.
- **Replay and reordering:** the receiver's counter must match exactly. GATT delivers in order
  over one connection, so any authentication failure (replayed, reordered, modified or dropped
  frame) **closes the session** (no resynchronization). A replayed IK first message from an
  earlier session cannot complete (the device's fresh ephemeral changes every key).
- **Associated data:** the 8-byte frame header (section 9.1) is the AEAD associated data, binding
  type, flags and `txn` to the ciphertext.
- **Transaction authentication:** requests and responses are AEAD messages in their direction;
  the response echoes `txn` in the authenticated header and carries the request type; a response
  can only be produced by the session key holder.
- **Key separation:** handshake keys (Noise chaining key), transport keys (Split), the PSK
  derivation and device-at-rest data use different derivations and labels; transport keys never
  encrypt stored data.
- **Session expiry:** ends with the BLE connection, END_SESSION, the 15-minute maximum, a
  restart or deep sleep. Keys and handshake state are zeroized; no session resumption; every
  reconnect runs a full IK handshake.
- **Reboot:** all session state is RAM only; durable effects use revisions and per-slot manual-dose sequence numbers
  (section 8).
- **RNG:** device ephemerals come from the ESP32-C3 hardware RNG, which is a true RNG only while
  the RF subsystem runs (it does during a BLE session) or with the bootloader entropy source;
  the setup code is generated in a provisioning session. Hardware acceptance item.

### 4.7 Rate limiting and denial of service (review item 9)

- With a 128-bit setup code and per-phone keys, guessing is infeasible; limits exist against
  **resource exhaustion and nuisance**, not guessing.
- Per connection: an unauthenticated connection is dropped after 30 s; a failed handshake
  closes the connection; three failed handshakes in one service session stop advertising for the
  rest of that session (the user can press the button again).
- **No persistent lockout** for high-entropy secrets: a persistent lockout would let an attacker
  near the device lock the owner out. A durable failure counter is kept for diagnostics only
  (written at most once per failed session to bound flash wear).
- Denial-of-service residuals: an attacker in range during the owner's 180-second window can
  occupy the single connection or jam the radio; the owner retries with a new press. BLE is
  never needed for dosing (autonomous operation continues).
- If J-PAKE with a short code were ever chosen, persistent exponential cool-down would become
  mandatory, with the lock-out DoS trade-off documented.

---

### 4.8 Noise implementation selection and feasibility evidence (revision 3)

Requirement: maintained implementations of exactly these patterns and suite on both platforms,
no own primitives, official vectors, byte-exact interoperability, pinned versions.

**Survey (2026-11-16).**

| Candidate | Platform | Findings | Verdict |
|---|---|---|---|
| **`esphome/noise-c` v0.1.31** (MIT; fork of Rhys Weatherley's Noise-C maintained by the ESPHome project; ESP-IDF registry component; tags v0.1.27–v0.1.31, last release 2026-10-08, commit `62c2fdca7a874ea898761147bb062ca1cde9f36d`) | ESP-IDF, any C target | backends libsodium (default), reference, OpenSSL; defaults: Curve25519, ChaChaPoly, SHA-256 on, AES off; depends on `esphome/libsodium` **1.10021.12** (pinned in its `idf_component.yml`); ships the official **cacophony** vectors (contain `Noise_XXpsk3_25519_ChaChaPoly_SHA256` and `Noise_IK_25519_ChaChaPoly_SHA256`) plus noise-c vectors; zeroizes every state object before freeing (`noise_free` → `noise_clean`, volatile loop); RNG hook supplied by the integrator on embedded builds (`NOISE_USE_CUSTOM_RAND`) | **selected for the device** |
| `jchambers/java-noise` (MIT) | JVM | revision 34 incl. modifiers, runs cacophony vectors; **not published to any artifact repository, no releases**; X25519 and ChaCha20-Poly1305 only through installed JCA providers | not selected (no pinnable release) |
| `corda/noise-java` (fork of rweather/noise-java) | JVM | AES-GCM only through the JDK; ChaChaPoly and PSK modifiers not documented | not selected |
| `sander/noise-kotlin` (`nl.sanderdijkhuis:noise-kotlin`, MIT) | Kotlin | Maven Central; 25519/ChaChaPoly/SHA-256 behind a user-implemented `Cryptography` interface; snow vectors; PSK patterns not documented; not audited | not selected (PSK coverage unproven) |
| `rweather/noise-java` | JVM | reference implementation, no releases, unmaintained | not selected |

**Recommendation [D15]: the same library on both sides.** Device: `esphome/noise-c` v0.1.31 +
`esphome/libsodium` 1.10021.12 as ESP-IDF components. Android: the same two sources at the same
pins, built with the Android NDK (CMake `externalNativeBuild`) into a small native library with a
thin JNI binding (handshake start/write/read/split, encrypt/decrypt; no cryptographic logic in the
binding). Byte-exact interoperability then holds by construction and is still checked with the
official vectors on host, device (vector runner on the target) and Android (instrumented test).

**Blockers and required changes found:**

1. **Single-pattern ESP-IDF build.** The component's ESP-IDF registration sets
   `NOISE_USE_PROTOCOL_NAME_TABLE=0 NOISE_USE_FALLBACK=0 NOISE_USE_HFS=0` as PRIVATE compile
   definitions; with the name table off the library accepts only
   `Noise_NNpsk0_25519_ChaChaPoly_SHA256` (ESPHome's protocol). XXpsk3 and IK need the name table
   on. Options **[D15]**: (a) propose an upstream option that keeps the table for ESP-IDF builds
   (preferred; ESPHome's own builds unchanged), (b) meanwhile vendor the exact v0.1.31 sources with
   only that build definition changed and a recorded diff, (c) neither: stop. No source change to
   protocol or crypto code in any option.
2. **AESGCM unavailable** on the device with this library (see 4.5): suite moved to ChaChaPoly.
3. **No maintained JVM implementation** with documented PSK coverage and a pinnable release:
   hence the NDK route, which adds a native build (CMake, NDK version pin) and per-ABI `.so` files
   to the Android app (decision **[D15]**). Alternative if the NDK is rejected: vendor
   `jchambers/java-noise` at a pinned commit with Bouncy Castle 1.85 as JCA provider for X25519 and
   ChaCha20-Poly1305 (Android 8.0–8.1 lack both in the platform) and prove interoperability with
   cross vectors; weaker (unpublished, single maintainer).
4. **Vectors not yet executed in this review.** Building the noise-c test suite fetches the pinned
   libsodium fork from GitHub and runs third-party code; that step was not run here. It is the
   first gate of M1.10a (section 14), together with a target build that measures flash, RAM and
   stack.

**Pinning.** ESP-IDF: `idf_component.yml` with exact versions (`esphome/noise-c: "==0.1.31"`,
`esphome/libsodium: "==1.10021.12"`) and the committed `dependencies.lock`; Android: the same
sources by commit hash (git submodule or a checked archive with SHA-256), NDK version pinned in
Gradle. Any update is a reviewed change that re-runs all vectors.

**Long-term keys and zeroization.**

- Device: static X25519 private key in the `auth` record (plain NVS; residual risk T10); loaded
  into a noise-c `DHState` only for a handshake; noise-c zeroizes its objects on free; our buffers
  are cleared with `sodium_memzero`/`noise_clean`; RNG hook = ESP-IDF hardware RNG (`esp_fill_random`).
- Android: the static private key is stored encrypted under a non-exportable Keystore AES-GCM key
  (StrongBox or TEE where available), decrypted into a byte array only for the handshake, passed
  to the native library and cleared on both sides (`Arrays.fill`, native `sodium_memzero`).
  Residual risk: copies the JVM may have made (garbage collector) cannot be guaranteed erased.

**Resource estimates (to be measured in the M1.10a gate; not measurements):** flash 30–60 KB for
noise-c (name table on, ChaChaPoly/25519/SHA-256 only) plus the libsodium subset; heap 2–4 KB per
handshake (noise-c allocates its state objects), released after `Split()` except two CipherStates
(< 200 B); stack 2–3 KB peak for X25519 in software; Android: one native library per ABI
(estimate 150–300 KB each).

### 4.9 M1.10a Phase 1 feasibility gate results (2026-11-16)

**Status: PASSED (34/34 checks) with the temporary local patches** in
`firmware/third_party/noise-c/patches` (provenance, rationale and removal procedure in
`firmware/third_party/noise-c/README.md`; reproducible with `gate/run_gate.sh`). A first run
without patch 0001 was blocked: the Android build aborted under bionic FORTIFY
(`memchr: prevented 62-byte read from 10-byte buffer` in `noise_pattern_expand()`, an invalid
`memchr` bound on 5–12-byte pattern tables in upstream noise-c 0.1.31). The upstream report is
drafted, not submitted.

| Item | Result |
|---|---|
| Sources | `esphome/noise-c` 0.1.31, git `62c2fdca7a874ea898761147bb062ca1cde9f36d`; `esphome/libsodium` 1.10021.12, git `95c5e60d991b3f735619da8a307951c0483ef567` (libsodium 1.0.21-RELEASE `d24faf56…` + 21 ESPHome patches); registry hashes in the README; registry packages byte-identical to git except metadata |
| Local patches | 0001: bound the base-pattern search by the table size (pattern lookup only); 0002: ESP-IDF build keeps the upstream default feature switches (needed for XXpsk3/IK). No cryptographic, state-machine or wire-format change |
| Upstream tests | host: unit + vectors pass (`ctest --no-tests=error`); Android-native x86_64 (FORTIFY on): unit + vectors pass |
| Required vectors (strict gate) | `Noise_XXpsk3_25519_ChaChaPoly_SHA256` 1/1, `Noise_IK_25519_ChaChaPoly_SHA256` 2/2, **0 skipped**, on host and Android-native |
| Patch regression | pattern semantics test (15 base patterns vs. sequential reference and the specification, modifiers, unknown ids, malformed names) passes on host and Android; the **unpatched** library aborts under FORTIFY (probe and upstream unit test, exit 134) |
| Interoperability | transcript digest identical for GCC/glibc and NDK clang/bionic builds (`6d263574…b120`); separate processes host ↔ Android, both roles, XX and IK, handshake + 3 transport rounds each way |
| Failure behavior | wrong PSK, wrong responder static key (MAC failure), modified, truncated and replayed transport frames rejected (host and Android) |
| Android NDK r30 (API 26), FORTIFY enabled | arm64-v8a 69,200 B, armeabi-v7a 54,188 B, x86_64 74,984 B, x86 78,920 B (stripped probe library) |
| ESP32-C3 (ESP-IDF v6.1, patched tree via `override_path`) | 0 warnings; noise-c 12,256 B + libsodium 38,981 B flash; static RAM 199 B; stack ≈ 1.9 KB for the Noise path incl. X25519 (static RISC-V call-graph estimate, not a target measurement) |
| Peak handshake heap | 1,761 B per side, 11 allocations, 0 leaked: a **host (x86-64) measurement** of noise-c's allocations, not a bound for the ESP32-C3 (allocator overhead, alignment and pointer sizes differ). Target heap, stack and handshake timing are hardware acceptance items (`firmware/HARDWARE_ACCEPTANCE.md` 8.1–8.3) |

Toolchain added for the gate: Android SDK command-line tools 23.0 and NDK r30 `30.0.16248370`
(`~/Android/Sdk`).

## 5. Ownership model

### 5.0 Protocol-security state machine (device side, per connection)

```text
            connect
  IDLE ───────────────► OPEN ──HELLO ok──► NEGOTIATED
                         │                    │ HS(purpose, msg 1) accepted
                         │ 30 s / error       ▼
                         ▼              HANDSHAKING(purpose) ──fail / 10 s per step──► CLOSING
                      CLOSING              │ handshake complete
                         ▲                 ▼
                         │           ┌─ purpose SESSION ──────────────► AUTHENTICATED(slot)
                         │           ├─ purpose CLAIM / ADD ──store──► AUTHENTICATED(new slot)
                         │           └─ purpose RECOVERY ─► CONFIRMING ─3 presses─► AUTHENTICATED
                         │                                     │ 20 s / no presses
                         └─────────────────────────────────────┴───────────────────────────────
  AUTHENTICATED ── decrypt failure, END_SESSION, 15-min cap, slot removed, reboot/sleep ──► CLOSING
  CLOSING: zeroize keys and handshake state, drop the connection ─► IDLE
```

| State | Accepted frames | Everything else |
|---|---|---|
| OPEN | Info read, HELLO | `MALFORMED`/`UNAUTHENTICATED`, close after 3 errors or 30 s |
| NEGOTIATED | HS msg 1 with an allowed purpose: SESSION (claimed), CLAIM (unclaimed), ADD (claimed, a valid invite exists), RECOVERY (recovery armed) | `HANDSHAKE_FAILED` → CLOSING |
| HANDSHAKING | the next HS message of the same pattern | `HANDSHAKE_FAILED` → CLOSING (counts toward the 3-per-session limit) |
| CONFIRMING | none except END_SESSION; the device sends `EV_CONFIRM_ON_DEVICE` and waits for button events (M10) | abort → CLOSING, nothing stored |
| AUTHENTICATED | encrypted requests for the role; plain HELLO/HS are protocol errors | decrypt failure → CLOSING immediately |

Purpose rules are evaluated at msg 1 and again at completion (state may change in between, e.g.
an invite expiring); every storing step is a single durable write, performed only after the
handshake (and, for recovery, the confirmation) has completed.

### 5.1 Independent per-phone credentials (review item 4)

- Each phone has **its own static X25519 key pair**, generated on the phone at enrollment. The
  private key is stored encrypted by a non-exportable Android Keystore AES-GCM key (StrongBox or
  TEE where available) and is decrypted in memory only during a handshake.
- The device stores per phone slot: public key (32 B), display name (≤ 24 B UTF-8), enrollment
  time, last-seen time, flags. **Up to 4 slots** **[D5]**. No shared owner secret exists.
- The device has its own static X25519 identity key, generated on the device at its first claim
  (not at the factory), stored in the credential record (M5). Phones pin its public key at
  enrollment and verify it in every IK handshake.
- Revocation is individual: deleting a slot invalidates exactly that phone at the next handshake.
- Roles: all enrolled phones are OWNER in v1 (no viewer role) **[D5]**.

### 5.2 Initial claim (setup secret + physical presence)

```text
precondition: device unclaimed (no slots), user pressed the button (service session open)
1. phone scans the QR (device ID + setup code); connects; HELLO (plain): device reports UNCLAIMED
2. phone → device  HS(purpose CLAIM, XXpsk3 msg 1)
   device → phone  HS(XXpsk3 msg 2: device static key)
   phone → device  HS(XXpsk3 msg 3: phone static key; payload: phone name)
   (succeeds only if both used the same setup code: the psk3 token)
3. device stores slot 0 = phone key (+ creates its identity key on first claim) in ONE durable
   write; replies ENROLLED(slot) inside the new transport session
4. phone pins the device public key, stores its own key; session continues as OWNER
```

Physical presence is the button press that opened the session (BLE exists only then). Claiming
is refused when any slot exists (use add-phone or recovery).

### 5.3 Adding a phone (setup secret + physical presence + existing owner) (review item 5)

```text
1. existing owner phone A (session) → INVITE_CREATE {name hint}
   device → A: invite_token (16 B random), expires in 10 min, single use, stored durably
2. A shows the invite as a QR on its screen (or shares it); new phone B scans it
3. user presses the device button (new service session); B connects
4. B → device HS(purpose ADD, XXpsk3) with PSK = HKDF(setup code, invite token)
   (needs the label's setup code AND the owner's invite AND physical presence)
5. device consumes the invite and stores B's slot in one durable write; replies ENROLLED
```

Without an invite, adding is impossible outside the recovery procedure (5.5). Invites are
listed and revocable by owners (`INVITE_CANCEL`).

### 5.4 Removing a phone

`PHONE_REMOVE(slot)` from any owner session; a phone may remove itself. Removing the last slot is
refused (that would make the device unclaimed outside the recovery procedure). Removal is one
durable write and takes effect at the next handshake of that phone; if it is the current
session's phone, the session ends.

### 5.5 Secure ownership recovery (review items 6, 7)

| Situation | Path |
|---|---|
| Lost phone, another phone still enrolled | the other phone removes the lost phone's slot |
| App reinstalled (Keystore key lost) | the phone is new: invite from another enrolled phone (5.3); old slot removed by an owner; if no other phone → recovery |
| Phone replacement, old phone available | old phone creates an invite, new phone enrolls, then removes the old slot |
| Phone replacement, old phone not available | as "lost phone" (another phone) or recovery |
| Loss of all authorized phones | recovery procedure below |

**Recovery procedure** (designed so an accidental long press can never silently remove ownership):

```text
1. ARM:     button held continuously from power-on for >= 10 s (FR-026 already guarantees a
            button held at boot never runs the pump). Persists RECOVERY_ARMED {armed_at}.
            Nothing is removed. Dosing continues unchanged.
2. WINDOW:  armed state expires after 30 min (or immediately when any enrolled phone completes a
            session and cancels it; STATUS shows "recovery armed" to every owner).
3. CLAIM:   in a NEW service session (separate short press) within the window, the new phone runs
            HS(purpose RECOVERY, XXpsk3) with the setup code.
4. CONFIRM: the device asks for a physical confirmation inside that session: 3 short presses
            within 20 s (button events, M10). Without it the recovery is aborted.
5. APPLY:   one durable write: all phone slots and invites cleared, new device identity key,
            the new phone in slot 0, RECOVERY_ARMED cleared, readiness set to SERVICE_REQUIRED
            [D6]; a recovery note is kept for the journal (M8) and STATUS.
```

- Physical access plus the label is the root of trust (as for the initial claim); the two-step
  procedure (boot-time arm, later session with confirmation) prevents accidental resets.
- Optional additional delay (recovery takes effect only after N hours unless an owner cancels)
  protects against brief physical access by a third party; it cannot help when the attacker
  keeps the device **[D6]**.
- **Dosing safety:** recovery never changes schedules, limits or calibration; it only forces
  SERVICE_REQUIRED so the new owner reviews the configuration before READY. It never runs
  between a claim and a pump run (single owner; BLE is suspended for doses anyway).

### 5.6 Factory reset (separate from ownership)

`FACTORY_RESET {confirm = "RESET"}` from an owner session (also available to the new owner right
after a recovery). Clears schedules, configuration (back to provisioning values: no limits,
SERVICE_REQUIRED), user Wi-Fi credentials, phone slots, invites and the device identity key;
keeps the setup code (it belongs to the label) and history watermarks until the journal defines
otherwise. Never reachable by a gesture alone.

---

## 6. Framing, memory and flow control (review item 12)

| Item | Value |
|---|---|
| Maximum frame | 520 B = 8 header + 496 payload + 16 tag (handshake messages ≤ 160 B) |
| Fragment | 1 header byte (bit 7 FIRST, bit 6 LAST, bits 0–5 index 0..63) + data |
| Reassembly buffers (BLE task) | one RX frame (520 B) + one TX frame being sent (520 B) |
| Queue | 4 opaque frames × 524 B (frame + length) = 2,096 B, static |
| Application task buffers | one decrypt/encode buffer pair, 2 × 520 B |
| Total static protocol RAM | ≈ 4.2 KiB (plus NimBLE host memory, measured in M1.10d) |
| Concurrency | one connection; one outstanding request; the client must wait for a response, `BUSY` or its timeout before the next request |
| Fragment timeout | 2 s between fragments of one frame, else the partial frame is discarded |
| Malformed fragments | index gap, FIRST missing, overflow, LAST without data → discard, send plain `MALFORMED` (no secrets), count; 3 per session → disconnect |
| Flow control client → device | ATT write with response: the client sends the next fragment only after the write response |
| Flow control device → client | notifications sent one fragment at a time; on NimBLE mbuf exhaustion retry after 20 ms, at most 10 times, then end the session |
| Queue full | `EV_BUSY`; the request was not taken; the client retries the same `txn` |

---

## 7. Data and storage

### 7.1 Storage layout v5 (review item 14)

| Change | Record | Format |
|---|---|---|
| new key `auth` | credential record (7.5) | format 1 |
| schedule names | `sched` set | format 3 (format 2 decodes with empty names) |
| manual-dose anti-replay | `dosing` | format 2: per-slot high-water marks, marker slot/seq, result ring (format 1 decodes with zeros) |
| Wi-Fi | namespace `dsp_wifi` (not part of the layout marker) | separate |

**Migration v4 → v5** reuses the resumable M1.3 scheme: (1) write the provisioned `auth` record
(unclaimed, no identity key) if absent; (2) rewrite the layout marker last. Records with new
formats are upgraded lazily on their next write (decoders accept the older format), so no bulk
rewrite is needed. An interrupted migration leaves the v4 marker and re-runs step 1 idempotently
at the next boot. v1/v2/v3 migrate straight to v5 as before. **Rollback limitation** (as R3): a
firmware older than v5 reports the layout or the new formats as unsupported and fails closed (no
automatic dosing); a future OTA must not roll back across v5. Existing devices: none in the field
(no hardware yet); host test devices migrate in place.

### 7.2 Compatibility

New fields are appended to records; enum values never change meaning; decoders reject unknown
newer formats (fail closed), as in M1.3–M1.9a.

### 7.3 Schedule names (review item 11)

- Stored on the device: UTF-8, **at most 32 bytes**, well-formed, no control characters (U+0000–
  U+001F, U+007F), not normalized by the device (stored as sent). Empty allowed.
- The schedule set grows by 16 × 33 B to 3,032 B (fits the NVS blob limits).
- The device is the source of truth for every Android-visible schedule field (name, amount,
  time, recurrence, start/end, enabled, revision), so a replaced phone sees everything after
  enrollment. Phone-only data: display preferences only.

### 7.4 Durable manual-dose anti-replay (revision 3)

**Invariant:** a manual-dose request that was ever accepted never activates the pump again,
regardless of later requests, reconnects or restarts.

Two separate mechanisms:

| Purpose | Mechanism | Lifetime |
|---|---|---|
| **Permanent prevention** of re-execution | per-phone-slot monotonic **request counter high-water mark** `manual_hw[slot]` (u64) in the `dosing` record | until factory reset |
| **Convenience** for retries after a lost response | ring of the last **8** results `{slot, seq, outcome, volume_ul}` in the `dosing` record | recent only; may be evicted |

**Request.** `MANUAL_DOSE {seq u64, volume_ul u32}`; the phone slot comes from the authenticated
session (never from the request). The phone keeps `seq` per slot and uses `max(local + 1,
floor)` where `floor = manual_hw[slot] + 1` is reported by the device in the session-start status
(so a phone that lost its counter state never collides).

**Decision rule (application task, single owner):**

1. `seq <= manual_hw[slot]` → **never executed**. If the ring holds `(slot, seq)` its outcome is
   returned (`DONE`, `FAILED`, `INTERRUPTED`, `REFUSED`, `PENDING`); otherwise
   `ALREADY_DECIDED_RESULT_UNAVAILABLE`.
2. `seq > manual_hw[slot]` → the request is evaluated exactly once:
   - **accepted:** `manual_hw[slot] = seq` and the ring entry `(slot, seq, PENDING)` are written
     **in the same atomic `dosing` write as the manual in-flight marker, ledger and last start**
     (M1.9a write 1, restrict-before-enable). Only then the consumption/permit (write 2) and the
     pump; write 3 clears the marker and sets the ring outcome (`DONE`/`FAILED`).
   - **refused** (gates, limits, coverage): `manual_hw[slot] = seq` and `(slot, seq, REFUSED,
     reason)` are written durably (one `dosing` write) before the refusal is answered, so a retry of
     a refused request can never execute later when the conditions have changed. If that write
     fails, the answer is `STORAGE` and nothing durable happened (no pump), so a retry is evaluated
     again — still at most once executed.
   - **invalid** (syntax, volume 0, too large): rejected without consuming `seq` (no state change,
     cannot execute).

**Power loss at every write:**

| Cut | Durable state | Retry with the same `seq` |
|---|---|---|
| before write 1 | nothing | evaluated once (may execute once) |
| after write 1, before the pump | marker + `hw = seq` + ring PENDING | recovery reports INTERRUPTED and sets the ring entry; retry → rule 1, never executes |
| during the pump / before write 3 | as above (+ consumption) | same: rule 1 |
| after write 3 | `hw = seq`, ring DONE/FAILED | rule 1, returns the outcome |
| refusal write cut | nothing or `hw = seq` + REFUSED | evaluated again or REFUSED; never a pump for a refused decision |

Recovery (M1.9a) clears an interrupted marker and, in the same write, sets the ring entry of that
`seq` to INTERRUPTED (the marker carries `slot` and `seq`).

**Concurrency and duplicates.** Requests are processed one at a time by the application task; BLE
is suspended during the pump, so a duplicate can only be processed afterwards and hits rule 1.
Duplicates already queued are processed after the original and also hit rule 1.

**Lost responses.** `GET_RESULT {seq}` (slot from the session) applies rule 1 without any side
effect. Evicted results return `ALREADY_DECIDED_RESULT_UNAVAILABLE`; the app shows "check the
device history" and never issues the same dose with a new `seq` automatically.

**Multiple phones.** Independent counters per slot; one phone can never block or replay another.

**Counter rollback, overflow, unavailable results.**

- A phone counter that goes backwards (bug, reinstall) produces only rule-1 answers, never a pump;
  the session-start `floor` lets the phone resynchronize.
- `seq` must be ≤ 2^63 − 1; a slot whose `hw` reaches that value refuses further manual doses
  (unreachable in practice, defined for completeness).
- The device never decreases `manual_hw` except at factory reset.

**App reinstall / lost phone-side state.** The Keystore key is gone, so the phone must enroll
again and gets a slot (possibly a previously used index). `manual_hw[slot]` is kept per slot index
and is **not** reset on enrollment; the new phone starts from the reported `floor`. Old requests of
a removed phone cannot even authenticate (its key is revoked; Noise replay protection), and the
kept high-water mark adds defense in depth.

**Ownership recovery and factory reset.** Recovery keeps `manual_hw` and the ring (slots are
cleared, counters stay monotonic). Factory reset re-provisions the `dosing` record (hw = 0, ring
empty) together with all slots and the device identity key; requests from before the reset
cannot authenticate afterwards.

**Format and migration.** `dosing` format 2 = format 1 + `manual_hw[4]` (32 B) + `marker_slot u8`
+ `marker_seq u64` + ring of 8 × 14 B (≈ 152 B in total, record ≈ 205 B). Format 1 decodes with
zeros and an empty ring; there are no phones before layout v5, so no request can predate the
counters. Written lazily at the first format-2 write (part of the v5 migration, section 7.1).

### 7.5 Credential record `auth`

`format | claimed | device identity private key (32) | identity public key (32) | 4 slots ×
{used, public key (32), name (≤ 24 + len), enrolled_at, last_seen} | 2 invites × {token (16),
expires_at, used} | recovery {armed, armed_at} | failure counter | reserved`. About 400 B, one
atomic NVS record. Written only by enrollment, removal, invites, recovery and factory reset.

### 7.6 Wi-Fi credentials at rest (review item 15)

- Namespace `dsp_wifi` in the `nvs` partition: SSID and passphrase. `nvs_sec` stays read-only for
  the developer/factory secrets (setup code, API token); the notification-token partition is
  never written by BLE.
- Requirements: written only from an owner session; never logged, never returned over BLE (only
  "set/not set" and the SSID); erased by factory reset and `WIFI_CLEAR`.
- At-rest limitation: NVS is **not encrypted** (NVS encryption needs flash encryption, which
  needs eFuse changes: out of scope). Anyone with physical flash access can read the passphrase,
  the device identity key and the phone public keys; NVS erase marks entries invalid but old
  pages may still hold data until garbage collection. Documented residual risk.

---

## 8. Lifecycle and error handling

### 8.1 Transactions and retries

- Timeouts: 5 s for normal commands, 60 s for MANUAL_DOSE (includes the BLE suspension).
- Idempotency cache per session: last 8 `(txn → response)`; a duplicate `txn` returns the cached
  response. A new session starts a new cache.
- Durable identities for effects that must survive reconnects and reboots: `expected_revision`
  (schedules, configuration), per-slot manual-dose `seq` with the durable high-water mark (7.4),
  invite tokens (single use).

### 8.2 Disconnection during a command

| Moment | Effect | Recovery |
|---|---|---|
| before the frame was complete | nothing executed | resend in a new session |
| after execution, before the response | executed once | read-back (revision) or `GET_RESULT(seq)` |
| during a configuration write | atomic per record (M1.3) | read-back decides |
| during a manual dose | BLE suspended before the pump; dose completes or is interrupted (recovery report, uncertainty) | new session, `GET_RESULT(seq)`; never a new `seq` for the same dose automatically |
| during enrollment | the slot is stored in one write: either enrolled or not | phone retries enrollment; a stored but unconfirmed slot is visible to owners and removable |

### 8.3 BLE suspension before pumping (M1.9b, unchanged)

Scheduled dose due: `EV_SUSPENDING_FOR_DOSE` (best effort), suspend, dose, resume if the session
deadline permits. Manual dose: the result is kept durably (7.4) and fetched after reconnecting
(new IK handshake).

### 8.4 Sessions and subscriptions

Session state is RAM only and ends with the connection, a restart or deep sleep. The client
re-enables Response notifications after every connection before HELLO. Lifetime: 180 s without a
client, kept while connected, 15 min maximum, `EV_SESSION_CLOSING` 30 s before the maximum;
unauthenticated connections dropped after 30 s. Wi-Fi never runs during a session.

---

## 9. Wire format

### 9.1 Frame (binary, little-endian; approved)

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `major` (1) |
| 1 | 1 | `minor` (0) |
| 2 | 1 | `type` |
| 3 | 1 | `flags`: bit 0 RESPONSE, bit 1 ERROR, bit 2 EVENT, bit 3 ENCRYPTED |
| 4 | 2 | `txn` (u16) |
| 6 | 2 | `len` (u16): bytes that follow |

Encrypted frames: `ciphertext ‖ tag(16)`, header as associated data, Noise transport nonce
(implicit counter). Encoding rules: unsigned little-endian integers unless stated; strings `u8
length ‖ UTF-8`; booleans 0/1 only; instants `i64` UTC seconds; volumes `u32` µl; dates `u16 year
‖ u8 month ‖ u8 day`. Noise handshake payloads use Noise's own byte encoding inside `HS` frames.

### 9.2 Messages (v1.0)

| Type | Name | Security | Request | Response | Domain |
|---|---|---|---|---|---|
| 0x01 | HELLO | plain | `proto_min, proto_max` | `proto, claim_state, flags (recovery armed), caps u32, session_id u32, device_id_pub_hash[8]` | session |
| 0x02 | HS | plain (Noise handshake) | `purpose u8, msg_index u8, noise_msg` | `msg_index u8, noise_msg` | M5 |
| 0x08 | ENROLL_RESULT | enc | — | `slot u8` | M5 |
| 0x10 | GET_STATUS | owner | — | status (9.4) | existing |
| 0x11 | GET_DIAGNOSTICS | owner | — | notify/clock/sync diagnostics | existing (thin) |
| 0x12 | ACK_REJECTIONS | owner | — | — | existing |
| 0x20 | GET_CONFIG | owner | — | config (9.4) | M1 |
| 0x21 | SET_CONFIG | owner | `expected_revision u32, field_mask u16, values` | `revision u32` | M1 |
| 0x30 | LIST_SCHEDULES | owner | `page u8` | `set_revision u32, total, page, n × {id, revision}` | M2 |
| 0x31 | GET_SCHEDULE | owner | `id` | schedule (9.4) | M2 |
| 0x32 | CREATE_SCHEDULE | owner | schedule (revision 1) | `set_revision u32` | M2 |
| 0x33 | UPDATE_SCHEDULE | owner | `expected_revision u32` + schedule | `revision, set_revision` | M2 |
| 0x34 | DELETE_SCHEDULE | owner | `id, expected_revision u32` | `set_revision` | M2 |
| 0x40 | MANUAL_DOSE | owner | `seq u64, volume_ul u32` | `outcome u8, volume_ul, runtime_ms` + status | existing + M4 |
| 0x41 | GET_RESULT | owner | `seq u64` | `state u8 (PENDING/DONE/FAILED/INTERRUPTED/REFUSED/ALREADY_DECIDED_RESULT_UNAVAILABLE/NOT_DECIDED), detail` | M4 |
| 0x42 | REFILL | owner | `capacity_ul, remaining_ul` | status | existing |
| 0x43 | SET_READINESS | owner | `readiness u8` | status (gates if refused) | existing |
| 0x50 | SET_TIME | owner | `phone_utc_ms i64, uncertainty_ms u32` | `decision u8, offset_s i64` | M3 |
| 0x60 | WIFI_SET | owner | `ssid (1..32 B), passphrase (0 or 8..63 B)` | — | M6 |
| 0x61 | WIFI_GET | owner | — | `ssid, passphrase_set u8, last_result u8` | M6 |
| 0x62 | WIFI_CLEAR | owner | — | — | M6 |
| 0x70 | PHONES_LIST | owner | — | `n × {slot, name, enrolled_at, last_seen, is_self}` | M5 |
| 0x71 | PHONE_REMOVE | owner | `slot u8` | — | M5 |
| 0x72 | INVITE_CREATE | owner | `name_hint` | `invite_token[16], expires_s u16` | M5 |
| 0x73 | INVITE_CANCEL | owner | — | — | M5 |
| 0x74 | RECOVERY_CANCEL | owner | — | — | M7 |
| 0x7D | FACTORY_RESET | owner | `confirm u32 = 0x54455352 ("RSET")` | — | M7 |
| 0x7E | END_SESSION | any | — | — | existing |
| 0x80 | EV_SESSION_CLOSING | event | `reason u8, seconds_left u16` | | existing |
| 0x81 | EV_SUSPENDING_FOR_DOSE | event | `seconds_until u16` | | existing |
| 0x82 | EV_BUSY | event (plain) | queue full | | platform |
| 0x83 | EV_CONFIRM_ON_DEVICE | event | `action u8, seconds u8` (press the button now) | | M10 |
| 0x90–0x9F | reserved: journal (M8) | | | | |
| 0xA0–0xAF | reserved: priming/calibration (M9) | | | | |

Error payload: `code u8, detail u32, aux u32` with the codes of revision 1 (MALFORMED,
UNSUPPORTED_VERSION, UNKNOWN_TYPE, UNAUTHENTICATED, FORBIDDEN, BUSY, REFUSED (gates), LIMIT,
NOT_COVERED, INVALID (validation bits), CONFLICT (current revision), NOT_FOUND, STORAGE, DEGRADED,
PUMP_FAILED, SESSION_EXPIRED, DUPLICATE) plus HANDSHAKE_FAILED and SLOTS_FULL.

### 9.3 Size limits

Largest payloads: schedule with name ≈ 125 B, config ≈ 60 B, Wi-Fi set ≤ 97 B, phones list
≈ 170 B, Noise XX message 2 ≈ 96 B + payload. Lists are paged (≤ 4 entries per page).

### 9.4 Records

- Info (public, 32 B): `"DSP2" | proto_min | proto_max | claim_state | flags | caps u32 |
  fw_major | fw_minor | fw_patch u16 | device_tag u16 | reserved[14]` (no configuration, no
  schedule data, no keys).
- Status (50 B), schedule (with `name`), config: as in revision 1, schedule extended by
  `name (0..32 B)`.

### 9.5 Examples

HELLO request (plain, `txn = 7`): `01 00 01 00 07 00 02 00 01 01`.

Error response to an unauthenticated GET_STATUS (`txn = 8`):
`01 00 10 03 08 00 09 00 04 00 00 00 00 00 00 00 00`.

MANUAL_DOSE (`txn = 12`, `seq = 0x1122334455667788`, 2,000 µl): plaintext payload
`88 77 66 55 44 33 22 11 d0 07 00 00`; frame `01 00 40 08 0c 00 1c 00 ‖ ciphertext(12) ‖ tag(16)`
(`len = 28`). Encrypted example bytes are not written by hand: the shared vectors will contain
complete frames generated from fixed test keys by an independent generator.

---

## 10. Threat model

| # | Threat | Mitigation | Residual risk |
|---|---|---|---|
| T1 | Passive eavesdropper in range | Noise AEAD for everything after the handshake; Wi-Fi credentials only inside sessions | session existence, sizes, timing |
| T2 | Active MITM during the initial claim | XXpsk3: needs the 128-bit setup code | attacker who also has the label (physical access) |
| T3 | Active MITM during later sessions | IK with pinned device key and enrolled phone keys | none known beyond implementation flaws |
| T4 | Nearby attacker without credentials sends commands | everything except HELLO/HS is rejected before dispatch; no pump path without an owner session | DoS of the 180 s window (owner retries) |
| T5 | Replay of recorded frames | fresh ephemerals per session, strict nonce counter, session closed on failure | none |
| T6 | Lost or stolen phone | per-phone slot removal; Keystore-wrapped key | window until the owner removes the slot |
| T7 | Former owner keeps access | slot removal; recovery clears all slots | if no other phone and no recovery performed |
| T8 | Malware on an owner phone | Keystore AES wrapping, key decrypted only during handshakes | root malware can use or extract the key while the app runs |
| T9 | Physical attacker with brief access | ownership changes need the label + boot-time arm + later session + on-device confirmation; optional delay | label readable by anyone holding the device |
| T10 | Physical attacker keeping the device | none at the protocol level (physical possession = ownership) | flash readable (no flash/NVS encryption): Wi-Fi passphrase, keys |
| T11 | Accidental ownership loss (long press) | arm only; expires; needs a later session + 3-press confirmation; owners can cancel | none |
| T12 | Unauthorized or accidental manual dose | owner session (physical button), explicit app confirmation, all dosing gates and limits, durable per-slot anti-replay (7.4) | owner mistakes within the configured limits |
| T13 | Duplicate dose on retry or replay | per-slot high-water mark in the same write as the manual marker; refusals decided durably (7.4) | an evicted result is reported as "decided, result unavailable" (never executed) |
| T14 | Resource exhaustion (frames, handshakes) | static buffers, fragment timeouts, failure limits per session | radio jamming |
| T15 | Downgrade | version in the prologue (handshake hash); major mismatch refused | none |
| T16 | Weak device randomness | RNG used with RF on; acceptance test | hardware-dependent (to be verified) |

---

## 11. Memory and flash budget (estimates unless marked measured)

| Item | Flash | RAM | Basis |
|---|---|---|---|
| TF-PSA-Crypto in the M1.9c runtime **with** connectivity | 135 KB (118 KB code + 16.5 KB rodata) | 0.65 KB static | **measured** (`idf.py size-components`) |
| TF-PSA-Crypto in the runtime **without** connectivity | 1.4 KB | 0.2 KB | **measured** |
| `esphome/noise-c` (name table on, 25519/ChaChaPoly/SHA-256) + `esphome/libsodium` subset | 30–60 KB (estimate; independent of the mbedTLS footprint, which TLS keeps) | heap 2–4 KB per handshake, < 200 B afterwards; stack 2–3 KB peak | estimate, **measured in the M1.10a gate** |
| `dsp_bleproto` + `dsp_blesec` code | 10–20 KB (estimate) | 4.2 KiB static buffers (section 6) | estimate |
| Android native library (noise-c + libsodium subset + JNI binding) | 150–300 KB per ABI (estimate) | — | estimate, measured in M1.10e |
| NimBLE host + controller | not estimated here: measured in M1.10d (configuration dependent) | heap measured in M1.10d | — |
| Headroom today | runtime + connectivity image 1,096,784 B in a 1,835,008 B slot: **738 KB free (40.2 %)** | static DRAM 135,780 B of 321,296 B | **measured** (M1.9c) |

Budget gate (M1.10d): image ≤ 85 % of the slot and ≥ 64 KB free heap after NimBLE init; otherwise
the configuration is revisited before any feature work.

---

## 12. Verification plan

### Host-testable (before hardware)

| Area | Firmware (C, ASan/UBSan) | Android (JVM) |
|---|---|---|
| Codec | every message, golden bytes, length/enum/bool/string rules | same vectors |
| Noise | official Noise test vectors for both patterns (25519/AESGCM/SHA256), then C ↔ Kotlin cross vectors from a third independent implementation (Python generator with fixed test keys, never real secrets) | same |
| Crypto primitives | RFC 7748 (X25519), NIST GCM, RFC 4231 (HMAC), RFC 5869 (HKDF) | same |
| Security behavior | wrong setup code, wrong invite, expired/used invite, revoked slot, replayed/reordered/truncated frames, purpose/version swap in the prologue, counter limits | Keystore wrapping tests (instrumented) |
| Authorization | every type × {no session, handshake incomplete, owner}; unauthenticated requests never reach a domain handler (spy) | repository refuses without a session |
| Transactions | duplicate/delayed `txn`, manual-dose `seq` anti-replay across power cuts at every write, duplicates, rollback, eviction, multiple slots (M1.9b longjmp harness), enrollment interrupted at every write | retry logic on a fake transport |
| Lifecycle | BLE port fake: suspend before a due dose, resume, CCCD resubscription, MTU 23/247/517, 15-min cap, queue full, fragment timeout and malformed fragments | fake GATT transport |
| Migration | v1–v4 → v5 at every write, format 2/3 decoding, older-firmware fail-closed | — |
| Fuzzing | frame decoder, reassembly, Noise handshake state machine (bounded CI run, corpus kept) | property tests |
| Cross-check | requests through the real `dsp_app` and domain | — |
| Budget | cross-builds with the BLE stack, size and DRAM gate | APK size |

### Needs hardware

NimBLE timing and throughput with real phones; vendor differences (no pairing dialog appears);
handshake duration on the C3 (X25519 in software); heap peak with NimBLE and later TLS; RNG
quality with RF on; suspend/resume latency around a dose; advertising power; button gestures.

---

## 13. Decisions

### 13.1 Revised decision table

| # | Decision | Revision 2 proposal | Status |
|---|---|---|---|
| D1 | Trust anchor | per-device 128-bit setup code (QR + text), dev: provisioning tool into `nvs_sec` | **approved** in principle |
| D2 | Handshake | Noise `XXpsk3` (enrollment) and `IK` (sessions), unmodified spec; EC J-PAKE rejected (mbedTLS 4.1 ↔ Bouncy Castle 1.85 not interoperable; not needed with a 128-bit code) | **approved**; revision 3 changes the suite to **25519/ChaChaPoly/SHA256** (needs approval) |
| D3 | BLE link | no pairing/bonding; application security (comparison 4.2) | **approved**, subject to the security verification in M1.10c/f |
| D4 | Credential storage and limits | `auth` record (layout v5); no persistent lockout; per-session limits; diagnostic failure counter | **needs approval** |
| D5 | Phones and roles | independent per-phone keys, 4 slots, all OWNER; add = setup code + button + owner invite | **approved** |
| D6 | Recovery and reset | boot-time arm (10 s) + later session + setup code + 3-press confirmation; forces SERVICE_REQUIRED; optional delay (none vs. N hours); factory reset only from an owner session | **approved**; delay value open |
| D7 | Wi-Fi provisioning | `dsp_wifi` namespace in `nvs`; `nvs_sec` read-only; API token not over BLE in v1 | **approved** (separate storage) |
| D8 | Manual-dose anti-replay | per-slot monotonic `seq` with a durable high-water mark in the same write as the manual marker; refusals decided durably; result ring of 8 for convenience only (7.4) | **needs approval** (revision 3) |
| D9 | Schedule names | on the device, ≤ 32 bytes UTF-8 | **approved** |
| D10 | Phone time | owner session, through the M1.4 rules, source BLE, uncertainty ≤ 2 s, NTP preferred, never lifts a rollback hold | **needs approval** |
| D11 | Wire format | fixed-layout little-endian binary | **approved** |
| D12 | Config while READY | calibration and limits only in SERVICE_REQUIRED | **needs approval** |
| D13 | Device history | deferred to the journal milestone | **needs approval** |
| D14 | Identifiers | UUID base `36b6XXXX-3457-4e8e-9cba-8126de3f52e4`; no manufacturer data, no company identifier | **needs approval** |
| D15 | Noise implementation | `esphome/noise-c` v0.1.31 + `esphome/libsodium` 1.10021.12 on both sides (Android via NDK + JNI); ESP-IDF name table enabled (upstream option preferred, else pinned vendored copy with a recorded build-definition diff); suite ChaChaPoly | **needs approval** (revision 3) |

### 13.2 Still requiring owner approval (revision 3)

1. **D15:** `esphome/noise-c` v0.1.31 + `esphome/libsodium` 1.10021.12 on both platforms, Android
   through the NDK with a JNI binding; how to enable the name table on ESP-IDF (upstream option
   vs. pinned vendored copy).
2. **D2 suite change:** `25519_ChaChaPoly_SHA256` instead of AESGCM.
3. **D8:** per-slot durable high-water-mark anti-replay, durable refusals, ring of 8.
4. **D6:** optional recovery delay (none vs. N hours).
5. D4, D10, D12, D13, D14 as proposed.
6. Residual risks T8 and T10 (no flash/NVS encryption without eFuse changes) and the JVM-copy
   residual for the phone's static key (4.8).
7. Permission for the M1.10a gate to fetch the pinned third-party sources (noise-c, libsodium
   fork) and run their vector tests on the host and a target build.

## 14. Implementation plan (revised)

| Milestone | Scope | Hardware | Exit criteria |
|---|---|---|---|
| **M1.10a** (done: gate 34/34, codecs + shared vectors, reproduced from a clean worktree) | **Gate first:** fetch the pinned noise-c/libsodium sources, run their unit and official vector tests on the host (ChaChaPoly/25519/SHA-256 incl. XXpsk3 and IK), cross-build them for the ESP32-C3 with the name table enabled and measure flash/heap/stack, and build them with the Android NDK (no app integration yet). Then: normative `docs/BLE_PROTOCOL.md` and the normative security specification; shared codec vectors with an independent Python generator; C codec `dsp_bleproto` and Kotlin codec; fuzz harness. No security code of our own. | no | gate results reported and accepted; codec vectors pass on both sides; security spec approved |
| **M1.10b** | Domain work items M1–M4, M6, M7 behind `dsp_app`; storage layout v5 with migration; schedule names; per-slot manual-dose anti-replay in the dosing record (7.4) with the extended power-loss matrix | no | host suites incl. migrations and power cuts |
| **M1.10c** | `dsp_blesec`: integration of the pinned noise-c (RNG hook, key handling, framing); credential record (M5); enrollment, sessions, invites, removal, recovery state machine; authorization tests; cross-check through the real application; JNI binding with the same vectors on Android | no | official and cross vectors pass on host, target and Android; authorization tests pass |
| **M1.10d** | NimBLE adapter (GATT, advertising with service data, fragmentation, flow control, suspend/resume, `connected()`), button events (M10) behind the reviewed pin, off by default; size/heap gate | compile only | builds pass the budget gate |
| **M1.10e** | Android: Companion Device Manager, native Noise (JNI) with Keystore wrapping, claim via QR, sessions, schedules (`DEVICE_APPLIED`), refill, manual dose with confirmation and per-slot `seq`, phones/invites/recovery UI, Wi-Fi provisioning | no (fake transport) | JVM + instrumented tests |
| **M1.10f** | On-device acceptance with real phones (pairing dialogs absent, timing, heap, RNG, power) | yes | `HARDWARE_ACCEPTANCE.md` BLE section |
