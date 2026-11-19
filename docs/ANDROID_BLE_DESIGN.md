# Dispenser V2 — Android BLE client integration (M1.10e)

Status: **revision 10 (current implementation status).**

| Milestone | Status |
|---|---|
| M1.10e.1 native Noise provider, vectors, credentials | committed `6a73441`, accepted |
| M1.10e.2a GATT transport and frame link | committed `a7bce87`, accepted |
| M1.10e.2b secure session, firmware-in-the-loop | committed `4b126b2`, `582ad3c`, accepted |
| M1.10e.3 device repository and operations (section 15) | committed `f7b8c22`, accepted; review fixes `4d7e3b2`, `2a9d20a` (15.5), accepted |
| M1.10e.4a app mode and data plumbing (section 16) | committed `e7564da`, accepted |
| M1.10e.4b onboarding / CDM (section 17) | implemented and tested, uncommitted, awaiting the onboarding/security review |
| M1.10e.4c scanner and invitations, M1.10e.4d device operations | not started; each stops for its review |
| M1.10e.5 real-device interop and acceptance | OPEN (hardware) |

Sections 1–11 are the design baseline written before implementation (revision 2–8); where they
describe the app "today" they describe the state before M1.10e.1. The implemented state is in
sections 12–17.
M1.10d.2, M1.10c.2b and M1.10e.5 stay OPEN; nothing here is evidence for radio behavior.

Baseline: `docs/BLE_PROTOCOL.md` v1.0 as amended by M1.10d.1 (sections 2, 3, 10.4a, 11.3),
`docs/BLE_PROTOCOL_DESIGN.md` rev. 3 (approved D1 setup code, D2 Noise, D5 four owner slots, D15
noise-c via NDK/JNI, Companion Device Manager, Keystore wrapping), `docs/OWNERSHIP_DESIGN.md`
rev. 2, `docs/SECURITY_INTEGRATION_DESIGN.md` rev. 4 (C1 PSK through libsodium on Android, C4
response cache, C7 session per connection, C10), `docs/BLE_TRANSPORT_DESIGN.md` rev. 3 (T1–T14),
`docs/ANDROID_APP.md` (sync semantics, DEMO rules). Firmware head `2b9e5d5`.

Decisions proposed here are marked **[Ex]** and collected in section 11. Nothing in this document
changes the wire protocol, the Noise patterns, the PSK derivation or any firmware behavior.

---

## 1. Starting point (historical baseline, inspected before M1.10e.1)

*Historical: this is the app as it was before M1.10e (e.g. `isDemo = true` hard-coded, no native
code). The current state is described in sections 12–16.*

| Area | Current state |
|---|---|
| App | Kotlin 2.2.10, AGP 9.5.1, Compose BOM 2026.02.01, compile/target SDK 37, **minSdk 26**, Java 11 bytecode, JDK 25 Gradle daemon. Manual DI in `AppContainer`; `isDemo = true` hard-coded. |
| Data boundary | `DispenserRepository` (device, schedules, reservoir, history, refill, schedule sync map, CRUD with revisions). Single implementation `LocalDispenserRepository`: schedules in Room (`dispenser.db`, PENDING only), device values from the simulation (`simulation.db`), DataStore settings. `SyncState.DEVICE_APPLIED` reserved for BLE read-back, never set today. |
| BLE code | `ble/protocol/BleProtocol.kt` (619 lines): pure codec (frames, bodies as schema-ordered maps, fragmentation, reassembly, Info, advert, negotiation), run against `test-vectors/ble_protocol_v1.txt`. **Not used by the app.** No GATT, no permissions, no native code, no NDK. |
| Vectors | `ble_protocol_v1.txt` (codec; Kotlin + C). `ble_session_v1.txt` (PSK, negative PSK, prologue, `device_id_hash`; Python stdlib generator) — consumed by C only; **Android is the open consumer**. There are **no handshake transcript vectors**: the firmware tests drive the device with a host phone built on the same noise-c. |
| Noise | Pinned `esphome/noise-c` 0.1.31 (`62c2fdca…`) + `esphome/libsodium` 1.10021.12 (`95c5e60d…`), patches 0001 (FORTIFY `memchr` bound — **required on Android**, the unpatched library aborts under bionic FORTIFY) and 0002 (ESP-IDF only); `firmware/third_party/noise-c/noise_c_deps.cmake` fetches by SHA, re-verifies, patches with zero fuzz, checks SHA-256. The M1.10a gate built the patched library with NDK r30 (`30.0.16248370`, API 26, FORTIFY on) for 4 ABIs (stripped probe 54–79 KB) and ran **statically linked Android x86_64 test binaries on the Linux host**, plus host ↔ Android interop over FIFOs. |
| Tests | JVM unit tests (JUnit 4, Robolectric 4.17 offline on SDK 35, coroutines-test, MockWebServer); instrumented Compose tests compiled in CI, **never executed in CI** (no emulator). CI: `assembleDebug test lintDebug assembleDebugAndroidTest assembleRelease checkThirdPartyLicenses`. |
| Backup | `allowBackup=false`, extraction rules exclude every domain; explicit JSON export (`BackupService`) of schedules, simulation and settings. |

Firmware facts the client must respect (from the inspected specs and code): the device advertises
**only during a service window** (button wake; until M10 there is no button, only the
off-by-default bring-up power-on window T7); one connection; 30 s unauthenticated limit; 10 s per
handshake step; 3 failed handshakes end the service window; 180 s idle without client; 15 min
maximum; `EV_SESSION_CLOSING` 30 s before; a manual dose (and any scheduled dose) **suspends BLE
and closes the connection** before the pump — the MANUAL_DOSE response normally never arrives,
the result is read with `GET_RESULT` on the next connection; final replies before a device close
are best effort (T13); a frame the device cannot queue is dropped silently (T3b).

## 2. Architecture

### 2.1 Layers

```text
 ui/ (Compose screens, ViewModels)            ── unchanged visual design; no android.bluetooth,
        │  StateFlow<UiState>, suspend intents     no crypto, no ble.* imports (enforced, 2.4)
        ▼
 data/  DispenserRepository  ◄── DemoMode: LocalDispenserRepository (unchanged)
        │                    ◄── DeviceMode: DeviceDispenserRepository (new, M1.10e.3)
        │                          ├─ DeviceCache (Room, device-confirmed snapshot)
        │                          ├─ PendingOps journal (manual dose, unresolved writes)
        │                          └─ DeviceOperations (typed ops, retry policy, read-back)
        ▼
 device/ DeviceSessionManager (one process-wide actor; owns at most one live session)
        │   state machine 4.4; connection generations; timeouts; cancellation
        ├─ session/  SecureSession: HELLO → handshake (CLAIM/ADD/SESSION/RECOVERY) → encrypted
        │            request/response, txn allocation, event dispatch      (pure Kotlin + JNI)
        ├─ protocol/ BleProtocol codec (existing) + typed messages (new, pure Kotlin)
        ├─ link/     FrameLink: fragmentation/reassembly over a ByteTransport (pure Kotlin)
        └─ transport/ GattTransport (android.bluetooth only here) | FakeGattTransport (tests)
                     DeviceDiscovery: Companion Device Manager association (onboarding)
 crypto/  NoiseProvider (JNI facade) → libdspnoise.so (noise-c + libsodium, pinned, patched)
          CredentialStore (Keystore AES-GCM wrapping) · SetupLabel parser · Secret types
```

Package rule: `transport/` is the only package that imports `android.bluetooth.*`;
`crypto/` is the only package with native methods or key material types; `ui/` sees neither.

### 2.2 Interfaces (Kotlin, illustrative signatures)

```kotlin
// transport/ — byte pipe of one GATT connection; implemented by GattTransport and FakeGattTransport
interface ByteTransport {
    val generation: Long                              // strictly increasing per connect attempt
    val events: Flow<TransportEvent>                  // Connected(mtu) | Notification(bytes) | Closed(reason)
    suspend fun writeFragment(fragment: ByteArray)   // write-with-response; returns after onCharacteristicWrite
    suspend fun readInfo(): ByteArray                 // 32-byte Info characteristic
    fun close()                                       // idempotent; gatt.disconnect()+close()
}
interface TransportFactory { suspend fun connect(device: DeviceAddress, timeout: Duration): ByteTransport }

// link/ — frames over fragments (shared codec rules, section 3 of the protocol)
class FrameLink(transport: ByteTransport, mtu: Int) {
    suspend fun send(frame: ByteArray)                // fragments at the negotiated MTU, one write at a time
    val frames: Flow<ByteArray>                       // reassembled notifications (2 s gap rule)
}

// crypto/ — no cryptographic logic in Kotlin; the native library is noise-c + libsodium
interface NoiseProvider {
    fun derivePsk(purpose: Purpose, setupCode: SecretBytes, inviteToken: SecretBytes?): SecretBytes
    fun newInitiator(pattern: Pattern, prologue: ByteArray, localStatic: SecretBytes,
                     remoteStatic: ByteArray?, psk: SecretBytes?): HandshakeInitiator
    fun generateStaticKeyPair(): StaticKeyPair       // libsodium CSPRNG (never the test RNG in the APK)
}
interface HandshakeInitiator : AutoCloseable {       // wraps a native NoiseHandshakeState
    fun writeMessage(payload: ByteArray): ByteArray
    fun readMessage(message: ByteArray): ByteArray    // returns payload
    fun remoteStatic(): ByteArray
    fun split(): TransportCiphers                     // consumes the handshake state
}
interface TransportCiphers : AutoCloseable {          // two native CipherStates; zeroized on close
    fun encrypt(header: ByteArray, plaintext: ByteArray): ByteArray   // header = AD
    fun decrypt(header: ByteArray, ciphertext: ByteArray): ByteArray  // throws on tag failure
}
interface CredentialStore {
    suspend fun load(deviceId: DeviceId): DeviceCredential?           // null = none; Unreadable = lost key
    suspend fun saveEnrollment(e: Enrollment)                          // atomic, after ENROLL_RESULT
    suspend fun delete(deviceId: DeviceId)
    suspend fun nextManualSeq(deviceId: DeviceId, floor: ULong): ULong // durable BEFORE returning
}

// session/ — protocol semantics of one authenticated connection
interface SecureSession : AutoCloseable {
    val role: Role                                    // OWNER (v1)
    val hello: HelloInfo                              // claim state, flags, caps, session id
    val events: Flow<DeviceEvent>                     // EV_SESSION_CLOSING, EV_SUSPENDING_FOR_DOSE, EV_CONFIRM_ON_DEVICE
    suspend fun <R> request(op: DeviceRequest<R>, policy: RetryPolicy = op.defaultPolicy): OpResult<R>
}

// device/ — process-wide owner of the single live session
interface DeviceSessionManager {
    val state: StateFlow<SessionState>                // section 4.4
    suspend fun open(purpose: OpenPurpose): SessionHandle   // SESSION, or CLAIM/ADD/RECOVERY with inputs
    suspend fun end(graceful: Boolean)                // encrypted END_SESSION when graceful
}
```

`DeviceRequest<R>` is a typed message (e.g. `GetStatus`, `UpdateSchedule(expected, schedule)`,
`ManualDose(seq, volumeUl)`), encoded through the existing `BleProtocol` maps; `OpResult` is
`Ok(value) | DeviceError(code, detail, aux) | NotSent(reason) | Uncertain(reason)` (no exception
crosses into the repository for protocol outcomes; `Uncertain` is never mapped to a failure —
section 14.3).

### 2.3 Demo mode coexistence

- `AppMode { DEMO, DEVICE }` persisted in DataStore; `AppContainer.repository` selects
  `LocalDispenserRepository` (DEMO, unchanged code path) or `DeviceDispenserRepository`.
  `isDemo` becomes derived from the mode. Fresh installs stay in DEMO.
- DEVICE mode becomes reachable only after a successful claim/enroll (M1.10e.4); until then the
  device stack is built and tested but **not wired into the app** (M1.10e.1–.3 add no visible
  behavior). The simulation, its database and the DEMO badge are untouched and remain
  available by switching back to DEMO **[E1]**.
- Phone-local schedules (DEMO) are never uploaded automatically. After a claim the user may
  explicitly copy them to the device (each becomes CREATE_SCHEDULE with a fresh ID, read back,
  then DEVICE_APPLIED) **[E2]**.

### 2.4 Enforcement of the separation

- A JVM test (`ArchitectureRulesTest`) scans `src/main` sources: `ui/**` must not import
  `android.bluetooth`, `cz.nowass.dispenser.device.transport`, `…crypto`, `…session`; only
  `transport/**` imports `android.bluetooth`; only `crypto/**` declares `external fun`.
- The native library exports only the JNI symbols (`-Wl,--exclude-libs,ALL`, version script);
  a CTest-style symbol check (like `test_ble_transport_symbols`) asserts that the release `.so`
  contains no deterministic-RNG or fixed-ephemeral symbol (section 3.4).

## 3. Noise, cryptography and credentials

### 3.1 Native library `libdspnoise` (M1.10e.1)

- **Sources:** the same pinned noise-c and libsodium commits, patched by the same
  `noise_c_deps.cmake` logic (fetch by SHA, verify, patch 0001 with `--fuzz=0`, SHA-256 check),
  invoked from the app's CMake (`externalNativeBuild`). One source of truth for pins: the app
  CMake `include()`s `firmware/third_party/noise-c/noise_c_deps.cmake` (refactored to add a
  `dsp_noise_c_android()` function; the firmware functions are unchanged) **[E3]**. libsodium is
  built from the fork with its own curated sources plus `kdf_hkdf_sha256.c` and
  `auth_hmacsha256.c` (as the host build already does) for the C1 PSK derivation.
- **NDK r30 `30.0.16248370`, `ANDROID_PLATFORM=android-26`, FORTIFY on** (default for API ≥ 21;
  checked in `compile_commands.json` like gate step 2), `-fstack-protector-strong`,
  `-Wl,-z,relro,-z,now`, 16 KB page alignment (`-Wl,-z,max-page-size=16384`, required for
  targetSdk ≥ 35 on 16 KB devices).
- **JNI binding (`dspnoise_jni.c`, ~400 lines, no cryptographic logic):** handshake
  new/write/read/split/remoteStatic/free; cipher encrypt/decrypt/free; `derivePsk`
  (`crypto_kdf_hkdf_sha256_extract/expand`); `generateKeypair` (`crypto_box_keypair`-equivalent
  via noise-c `DHState`); `randomBytes`. Native objects are opaque `long` handles held by Kotlin
  `AutoCloseable` wrappers with a use-after-close guard; every handle is freed with
  `noise_*_free` (noise-c zeroizes before freeing).
- **Derivations:** PSK in native code (approved C1). Prologue (26 bytes) and `device_id_hash`
  (SHA-256 via `MessageDigest`, public data) in Kotlin; both checked against
  `ble_session_v1.txt`.
- **Randomness:** `noise_rand_bytes()` → libsodium `randombytes_buf()` (default sysrandom:
  `getrandom()`/`/dev/urandom`). `sodium_init()` once at library load; failure makes
  `NoiseProvider` unavailable (fail closed, DEVICE mode disabled with a clear error).
- **Supported patterns:** `Noise_XXpsk3_25519_ChaChaPoly_SHA256` (CLAIM, ADD, RECOVERY) and
  `Noise_IK_25519_ChaChaPoly_SHA256` (SESSION), phone = initiator, as section 11 of the protocol.
  The binding refuses any other protocol name.

### 3.2 Handshakes on the phone

| Purpose | Inputs (phone) | Steps | Phone stores on success |
|---|---|---|---|
| CLAIM (2) | setup code from the device label; phone static key (new) | HELLO (UNCLAIMED) → XX1 → XX2 (device static `rs`) → XX3 + name → `ENROLL_RESULT{slot}` | device ID, device static key (pinned), slot, name, phone key |
| ADD (3) | setup code (label) + invite token + expected device static key (both from the owner's invite QR) | as CLAIM; abort if `rs` ≠ the invite's identity key **before** sending XX3 | as CLAIM |
| SESSION (1) | phone static key, pinned device static key | HELLO (CLAIMED, `device_id_hash` must equal SHA-256("DSP2 id" ‖ pinned key)[0..8)) → IK1 → IK2 | `last_seen`, manual floor |
| RECOVERY (4) | setup code (label); new phone static key | XX1–XX3 → `EV_CONFIRM_ON_DEVICE{1,20}` → user presses 3× → `ENROLL_RESULT{0}` or encrypted `REFUSED` | replaces the old credential of this device only after `ENROLL_RESULT` |

- The **setup code is never persisted** on the phone; it is held in a `SecretBytes` only for the
  PSK derivation and zeroized immediately after **[E4]**.
- A phone key per device association (not one per phone) **[E5]**: limits linkability across
  dispensers and the blast radius of a lost credential; cost is one 32-byte key per device.
- HELLO checks before any handshake: `proto` negotiated = 1; `claim_state`/flags consistent with
  the requested purpose (e.g. CLAIM only when UNCLAIMED and not CREDENTIALS_DAMAGED); for
  SESSION the `device_id_hash` must match the pinned key, else no handshake ("wrong device").
- Every failure path (timeout, `HANDSHAKE_FAILED`, decrypt failure, disconnect) closes the
  native handshake/cipher objects and zeroizes Kotlin-side buffers before the state machine
  leaves the state.

### 3.3 Credential storage

| Item | Secret | Where | Protection |
|---|---|---|---|
| Phone static private key (32 B) per device | yes | app-private file `files/credentials/<device-id>.bin` | AES-256-GCM under a non-exportable Android Keystore key (`setIsStrongBoxBacked(true)` when available on API 28+, else TEE); random 12-byte IV per write; AAD = format ‖ device ID |
| Device static public key (pinned), device ID, slot, enrolled name, enrollment time | integrity-critical, not secret | same encrypted record | AEAD integrity (a tampered pin fails decryption) |
| Manual-dose `seq` high water (per device) | no | same record (separate atomic file `seq.bin` acceptable) | written durably **before** each MANUAL_DOSE is sent |
| Setup code, invite token | yes | **never stored** (memory only, zeroized) | — |
| Session keys, ephemerals, handshake hash | yes | native memory only | noise-c zeroize on free; freed at every close/error/disconnect |

- Keystore key: alias `dsp_cred_v1`, AES/GCM/NoPadding 256, `setRandomizedEncryptionRequired`,
  no user authentication in v1 (BLE sessions must not prompt for biometrics on every
  connection) and `setUnlockedDeviceRequired(true)` on API 28+ **[E6]**.
- Atomic writes: write temp file + `fsync` + rename (`AtomicFile`). Format byte for forward
  migration; unknown format → treated as unreadable, never overwritten silently.
- **Backup:** system backup already off (`allowBackup=false`, extraction rules); Keystore keys
  are never backed up. The JSON export (`BackupService`) must exclude credentials and the device
  cache; a unit test asserts the export contains no credential file content and no key bytes.
- **Key loss** (reinstall, "clear storage", Keystore reset, `AEADBadTagException`/
  `KeyPermanentlyInvalidatedException`): the credential is marked LOST, never replaced silently;
  the UI explains re-enrollment (invite from another phone, or recovery) per BLE_PROTOCOL_DESIGN
  5.5. A new key pair is only generated inside a new CLAIM/ADD/RECOVERY flow.
- **Migration:** no prior credential format exists (fresh feature). Format v1 is defined now;
  any change is forward-only with a tested migration (same rule as the firmware records).
- **Logging:** secrets travel only as `SecretBytes` (`toString()` = `"SecretBytes(**)"`,
  `close()` zeroizes, no `equals`/`hashCode` on content). No `Log` call in `crypto/`,
  `session/`, `link/` prints payload bytes; a Robolectric test captures all `ShadowLog` output
  of the full handshake suite and asserts no fixture secret (setup code, token, private keys,
  PSK, session keys) appears in hex or Base64 — the Android analogue of the firmware's output spy.
- Residual risk (as approved in BLE_PROTOCOL_DESIGN T6/T8): the plaintext private key exists in
  a JVM `ByteArray` during a handshake; copies made by the GC cannot be guaranteed erased; root
  malware can use the key while the app runs.

### 3.4 Deterministic randomness — tests only

- The APK's native library contains only the CSPRNG path. Deterministic RNG and fixed
  ephemerals (`noise_handshakestate_get_fixed_ephemeral_dh`) exist only in a **separate host test
  library** (`libdspnoise_hosttest.so`, built for the JVM tests, section 6.1) compiled with
  `DSP_TEST_RNG=1`.
- Gradle wiring makes the test library unreachable from `main`; the symbol check (2.4) fails the
  build if the release `.so` exports or contains the test symbols.

### 3.5 Handshake transcript vectors **[E7, approved: Rust `snow`]**

No cryptographic primitive is written for the vectors. Three separate artifacts with separate
jobs, never mixed in one file or one test:

1. **Noise conformance (official vectors, unchanged).** noise-c's bundled cacophony and noise-c
   vector files (`Noise_XXpsk3_25519_ChaChaPoly_SHA256` 1/1, `Noise_IK_25519_ChaChaPoly_SHA256`
   2/2, **0 skipped**), used as shipped upstream, run by their own test target against the Android
   build (static NDK x86_64 binaries on the CI host; the JNI library on the JVM and in
   instrumented tests), as the M1.10a gate did. They say nothing about Dispenser framing.
2. **Dispenser BLE framing vectors — `test-vectors/ble_handshake_v1.txt`.** Produced by the Rust
   crate **`snow`** (independent of noise-c), **exact version pinned** in `Cargo.toml`
   (`=x.y.z`) with every transitive dependency pinned by the committed `Cargo.lock`
   (`cargo build --locked`), plus RustCrypto `hkdf`/`sha2` for the PSK, prologue and
   `device_id_hash`. Tool: `test-vectors/tools/gen_ble_handshake_v1/`. Before writing any record
   the tool must (a) reproduce the official XXpsk3/IK vectors through `snow` and (b) reproduce
   every PSK, prologue and `device_id_hash` record of `ble_session_v1.txt`, and stops otherwise.
   CI consumes the committed file; regenerating it is a reviewed change.
3. **Application-level negative tests (kept as tests, not only as vector records).** The existing
   firmware negative suites (wrong setup code/token, revoked or unknown key, prologue swaps,
   `msg_index` order, step timeouts, three failures, replayed/reordered/truncated frames,
   plaintext after authentication, T3 fragment faults) stay, and the same classes are added on the
   Android side against the simulated peer and the firmware-in-the-loop library (6.3, 6.4). The
   negative records in the vector file complement them; they do not replace them.

**Deterministic ephemeral keys are strictly test-only.** The vector tool sets them through
`snow`'s test-only fixed-ephemeral builder option; the firmware replays them through the
host-only scripted RNG; Android through `noise_handshakestate_get_fixed_ephemeral_dh` in the host
test library only. None of these paths exists in the firmware target build or the APK; the
native symbol check (2.4) and the firmware's existing rule (deterministic RNG only in host tests)
enforce it.

Content of each transcript record (CLAIM, ADD, SESSION, RECOVERY), all inputs visibly
synthetic and pinned:

| Group | Fields |
|---|---|
| Inputs | device ID, device static key pair, phone static key pair, phone and device **ephemeral private keys**, setup code, invite token (ADD), HELLO `session_id`, protocol minor, purpose, enrolled name payload |
| Derived | `device_id_hash` (claimed/unclaimed form as applicable), 26-byte prologue, PSK (XXpsk3 only) |
| Wire | every HS request/response frame (8-byte header + body) byte for byte; ENROLL_RESULT; RECOVERY's encrypted `EV_CONFIRM_ON_DEVICE{1,20}` then `ENROLL_RESULT{0}`; SESSION's first encrypted request and response |
| State | handshake hash after the last handshake message; for each encrypted frame the plaintext, header (AD), nonce index and ciphertext ‖ tag |
| Negative | same inputs with one change each: wrong setup code, wrong token, wrong pinned device key, prologue with another session ID / purpose / hash, flipped ciphertext bit, truncated tag — expected: which side fails at which message |

Encrypted transport frames in the transcripts are chosen so that the **device's** output is
deterministic in any state (ENROLL_RESULT, EV_CONFIRM_ON_DEVICE, an encrypted END_SESSION
response, an `UNAVAILABLE` error for WIFI_GET on a firmware without `caps` bit 0) — so the
firmware can reproduce them exactly.

**Verified by both implementations:**

- **Firmware (host CTest):** a new test replays each transcript through the real `dsp_blesec` /
  `dsp_app` with a *scripted* host RNG that returns the vector's bytes in the order the device
  draws them (HELLO `session_id`, ephemeral key, identity key at claim); the device's responses
  must equal the vector byte for byte, and the phone frames of the vector must be accepted.
  The scripted RNG is test-only, like the existing deterministic RNG.
- **Android:** JVM tests through the host JNI library with fixed ephemerals
  (`noise_handshakestate_get_fixed_ephemeral_dh`, test library only) must produce the phone
  frames and accept the device frames byte for byte; the Android-native (NDK, FORTIFY) static
  test binary does the same on the CI host.

## 4. BLE lifecycle

### 4.1 Discovery, permissions and addresses **[E8, approved]**

**Identity rule.** A Bluetooth address (and the CDM association that stores it) is only a
*locator*. It is never an authenticated identity: the device is authenticated only by the Noise
handshake (IK with the pinned device static key; XX with the setup-code PSK and, for ADD, the
identity key from the invite). The pre-handshake checks of the Info record and HELLO
`device_id_hash` only detect "wrong device" early; a match grants nothing.

**Permissions (minSdk 26 … targetSdk 37):**

| API level | Manifest | Runtime | Needed for |
|---|---|---|---|
| 31+ | `BLUETOOTH_CONNECT` | `BLUETOOTH_CONNECT` | GATT connect, CDM-associated device access |
| 31+ | `BLUETOOTH_SCAN` with `usesPermissionFlags="neverForLocation"` | `BLUETOOTH_SCAN` | **fallback scan only** (below); requested only when the fallback runs |
| 26–30 | `BLUETOOTH`, `BLUETOOTH_ADMIN` (`maxSdkVersion="30"`) | none (install-time) | GATT connect |
| 26–30 | `ACCESS_FINE_LOCATION` (`maxSdkVersion="30"`) | `ACCESS_FINE_LOCATION` | **fallback scan only** (BLE scan results require it on these levels) |
| all | `uses-feature android.hardware.bluetooth_le required="false"`, `android.software.companion_device_setup required="false"` | — | demo installs everywhere; device features hidden when absent |

CDM association itself needs no location or scan permission on any supported level.

**Discovery and reconnection paths:**

| Situation | Behavior |
|---|---|
| Onboarding | CDM `AssociationRequest` (single device) with `BluetoothLeDeviceFilter` on service UUID `36b60001-…`; the user selects the device in the system dialog; result: address + association. API 33+ `associate(request, executor, callback)`; 26–32 `IntentSender` result. The device must be advertising (service window). |
| Normal reconnect | `connectGatt(autoConnect = false, TRANSPORT_LE)` to the associated public address (`getRemoteLeDevice(addr, ADDRESS_TYPE_PUBLIC)` on 33+), user has pressed the button; 30 s attempt, then "press the button and retry". |
| Association missing (removed by the user in system settings, app data cleared, `getMyAssociations()` no longer lists it) | credentials are kept; the app asks to re-associate through CDM (same flow as onboarding, filtered additionally by the stored `device_tag`); the new association is accepted only if the following IK handshake succeeds with the pinned key. |
| Address changed (board replaced, or the device ever uses another address) | direct connect fails or Info/HELLO show another device: the app offers CDM re-association; nothing is trusted until IK succeeds. A *different* pinned key never authenticates — a replaced device needs a new claim. |
| Direct connect unreliable on a phone model (observed in M1.10e.5) | **fallback discovery:** a foreground, user-initiated 30 s scan filtered by service UUID (`BLUETOOTH_SCAN` neverForLocation on 31+; `ACCESS_FINE_LOCATION` and Location Services on 26–30), match by `device_tag` and address, then connect. Never a background scan. |
| Location Services disabled | CDM and direct connect are unaffected by design; on 26–30 the fallback scan returns nothing, so the app checks `LocationManager.isLocationEnabled` and asks the user to enable it only for the fallback. Whether some vendor CDM dialogs on 26–30 still require Location Services is an explicit item of the M1.10e.5 phone matrix. |
| Bluetooth disabled | `ACTION_REQUEST_ENABLE` intent (needs `BLUETOOTH_CONNECT` on 31+); state `Blocked(BluetoothOff)` until `ACTION_STATE_CHANGED` reports ON. |
| Permission denied or revoked at runtime | every GATT call guarded for `SecurityException`; the live session closes (zeroized), state `Blocked(PermissionRequired)`; permanent denial shows the system settings path. Demo mode never asks for any Bluetooth or location permission. |

API differences are hidden in `GattTransport`: API 33+ `writeCharacteristic(c, value, type)` /
`onCharacteristicChanged(gatt, c, value)`, older deprecated variants; newer Android versions may
request MTU 517 themselves regardless of the app's request (the app always uses the value from
`onMtuChanged`, 23 if none arrives).

### 4.2 Connection setup sequence

```text
connectGatt ─(15 s)─► CONNECTED ─► requestConnectionPriority(HIGH) ─► discoverServices (10 s)
  ─► verify service + 3 characteristics + CCCD (else close: "not a Dispenser")
  ─► requestMtu(517) (5 s; on timeout keep 23) ─► mtu = onMtuChanged value (device caps at 247)
  ─► read Info (32 B: magic, proto range, claim state, flags, caps, fw version, device_tag)
  ─► write CCCD = notifications (5 s)  ─► FrameLink ready  ─► HELLO (5 s) ─► handshake (10 s/step)
```

GATT operations are strictly serialized (one outstanding operation; Android requirement): a
single `GattOpQueue` coroutine issues the next operation only after the previous callback. Each
Request fragment is one write-with-response; the next fragment only after `onCharacteristicWrite`
(protocol flow control). Status 133/8/19/22 and any unexpected callback close the connection;
`gatt.close()` is always called on teardown.

### 4.3 Connection generations and stale callbacks

- Every `connect` creates a new `GattTransport` with `generation = ++counter` (process-wide,
  `Long`). The `BluetoothGattCallback` instance is per generation and forwards every callback
  as `TransportEvent(generation, …)` into the manager's single actor `Channel`.
- The actor drops any event whose generation ≠ the live one (counted, logged without payload).
  After `close()` a transport refuses writes. Session objects (`SecureSession`, ciphers, txn
  counter, pending requests) are bound to one generation and closed with it — a reconnect always
  needs a new HELLO + IK handshake (C7); nothing of an old session can encrypt or answer on a
  new one.
- Pending request continuations of a closed generation resume with `Uncertain` (the request may
  have been executed); requests issued after the close return `NotSent`.

### 4.4 Session state machine (`DeviceSessionManager`)

```text
 Idle ──open()──► CheckingPrereqs ──(BT off / perm)──► Blocked(reason) ──retry──► CheckingPrereqs
                       │ ok
                       ▼
                   Connecting ──timeout/err──► Failed(Unreachable) ──► Idle
                       │ connected(gen)
                       ▼
                   Discovering ─► MtuNegotiation ─► Subscribing ─► Hello ─► Handshaking(purpose)
                       │ any error/disconnect: close gen, zeroize ─► Failed(kind) ─► Idle
                       ▼ IK done / ENROLL_RESULT
                   Authenticated(gen, role) ◄──► Busy(request) ; events: SessionClosing(sec)
                       │        └─ RECOVERY: ConfirmingOnDevice(seconds) ─► Authenticated | Failed(Refused)
                       │ EV_SUSPENDING_FOR_DOSE ─► SuspendedForDose (expect disconnect; reconnect offered)
                       ▼ end()/disconnect/EV_SESSION_CLOSING expiry
                   Closing ─► Idle
```

- One live session per process; `open()` while a session exists returns the existing handle if
  compatible, else fails `AlreadyActive`.
- **Timeouts:** connect 15 s; discovery 10 s; MTU 5 s; CCCD 5 s; HELLO 5 s; each handshake step
  10 s (device limit); request 5 s default (8 s for writes that touch flash); confirmation on
  device: the `seconds` from `EV_CONFIRM_ON_DEVICE` + 5 s. All expressed as constants in one
  `Timeouts` object, tuned in M1.10e.5.
- **Cancellation (D3, M1.10e.3):** the manager owns every submitted operation. A cancelled caller
  stops waiting; the request still completes and its outcome and journal row are still processed
  (`DeviceOperations` runs requests in the manager scope). A cancellation that hits a handshake
  closes the connection (a half-done handshake is never resumed); if the manager scope itself is
  cancelled after a request may have been sent, the session closes (fail closed, 14.3).
- **Synchronous invalidation (D2, M1.10e.3):** when any `SecureSession.close()` returns (the first,
  a concurrent or a later call; `end()` included), the manager no longer exposes that session:
  `close()` runs the manager's close listener before returning, outside every session lock. The
  listener only compare-and-sets — `current` from that session to none, `state` from that
  session's own `Authenticated(generation)` value to `Idle` — so it is idempotent, never blocks,
  and a late close of an older session can never clear a newer session or a newer `open()`'s
  `Connecting`/`Handshaking` state. A session that closes during adoption is invalidated at once
  (the listener runs on registration if the session is already closed).
- **Backgrounding:** the session lives in the process-wide manager, not in a ViewModel. On
  `ON_STOP` a 30 s grace starts; when it expires the app disconnects (plain GATT close, the
  device's window continues until its idle limit). An explicit "Done" sends the encrypted
  END_SESSION, which ends the device's window. No foreground service in v1 **[E9]**.
- **Process death:** live session lost (by design); durable state that survives: credentials,
  device cache, pending-ops journal. On restart the repository shows the cached snapshot as
  cached (with its age) and pending operations as "result unknown — connect to check".

### 4.5 Connection loss, reconnect, lost final responses

- Any disconnect ends the session. Reconnect is user-initiated (or automatic once if the app is
  in the foreground and the disconnect followed `EV_SUSPENDING_FOR_DOSE`: the device resumes
  advertising after the pump while the window lasts).
- **Lost final responses (BLE_PROTOCOL 11.3):** the client never infers execution from a missing
  or a received final reply. END_SESSION + disconnect = done, never repeated. A missing response
  to any mutation is resolved only by reading durable state in the next session (table 5.1);
  MANUAL_DOSE by `GET_RESULT{seq}`. A timeout or a connection failure never proves that the
  operation was not executed (section 14.3).

## 5. Error handling and retry policy

### 5.1 Per operation

Retransmission rule (protocol 10.4a): within one session, after a timeout, the **same `txn` and
identical body** may be resent; the device answers from its 8-entry cache. A new session never
reuses an old `txn` or assumes the cache. Because the transport nonces are implicit, this recovers
a *delayed* response only; a request dropped before decryption or a lost encrypted response puts
the nonces out of step and the session closes. Every request that ends without a response is
reported as **uncertain** and reconciled in a new session by the column on the right (section
14.3); nothing is retransmitted across sessions automatically.

| Operation | In-session retransmit (same txn) | After disconnect / new session |
|---|---|---|
| GET_STATUS, GET_DIAGNOSTICS, GET_CONFIG, LIST_SCHEDULES, GET_SCHEDULE, GET_RESULT, PHONES_LIST, WIFI_GET | up to 2 | reissue freely |
| SET_CONFIG, UPDATE_SCHEDULE, DELETE_SCHEDULE | up to 2 | read back (GET_CONFIG / GET_SCHEDULE / LIST); `CONFLICT` → show both versions, never auto-merge; `STORAGE` → read back decides |
| CREATE_SCHEDULE | up to 2 | resend with the **same phone-generated ID**: `DUPLICATE aux 0` = it exists → read back; `aux 1` = ID burnt → new ID only after read-back shows it absent |
| MANUAL_DOSE | up to 1 (only while the connection is still up) | **never a new `seq` automatically**; `GET_RESULT{seq}` after reconnect; `ALREADY_DECIDED_RESULT_UNAVAILABLE` → "check device history" |
| REFILL | up to 2 | **no automatic resend** (absolute values: a resend after an intervening dose would overstate the reservoir) — read Status, ask the user |
| SET_READINESS, PHONE_REMOVE, INVITE_CANCEL, RECOVERY_CANCEL | up to 2 | read back (Status / PHONES_LIST) |
| INVITE_CREATE | up to 2 (cache returns the same token) | new invite only after the user asks; old one cancelled first |
| SET_TIME | up to 2 | re-evaluate and send fresh time |
| END_SESSION | none | never repeated after a disconnect |
| FACTORY_RESET, WIFI_* | not exposed in v1 (`UNAVAILABLE` on current firmware) | — |

- `txn`: per session, starts at 1, increments, never 0, never reused for a different body.
- One request in flight per session (the device processes one at a time; simpler retransmit
  semantics). Events are delivered on a separate flow.

### 5.2 Error mapping (device `code` → app outcome)

| Code | App behavior |
|---|---|
| 1 MALFORMED / 3 UNKNOWN_TYPE | bug class: log type/txn (no payload), close session, report "communication error" |
| 2 UNSUPPORTED_VERSION | "app/firmware incompatible" with the device range from `detail` |
| 4 UNAUTHENTICATED / 5 FORBIDDEN | unexpected inside an owner session: close and report. (A removed phone does not get FORBIDDEN: its next IK handshake fails with HANDSHAKE_FAILED; after repeated IK failures the UI offers re-enrollment, never automatic key replacement) |
| 6 BUSY | detail 0 no service window → "press the button"; 1 → retry after `aux` s (shown); 2 → recovery confirmation pending |
| 7 REFUSED | detail 1 field needs SERVICE_REQUIRED; 2 refill pending; 3 time untrusted → offer SET_TIME then retry once |
| 8 LIMIT / 9 NOT_COVERED / 10 INVALID | shown with the limit (`aux`) / problem bits mapped to existing `ValidationError` names |
| 11 CONFLICT | read back and show conflict UI (existing `ScheduleWriteResult.Conflict`) |
| 12 NOT_FOUND | refresh list |
| 13 STORAGE / 14 DEGRADED | read back; DEGRADED → device needs service, writes disabled in UI |
| 15 PUMP_FAILED | manual-dose result FAILED; show device problem state |
| 16 SESSION_EXPIRED | reconnect offered |
| 17 DUPLICATE | CREATE rules above |
| 18 HANDSHAKE_FAILED | generic "pairing failed" (never which input); counts locally — after the 3rd the device ends the window: tell the user to press the button again |
| 19 SLOTS_FULL | "4 phones enrolled — remove one" |
| 20 UNAVAILABLE | feature hidden for this firmware (`caps`) |
| decrypt failure, unexpected plaintext after auth | close immediately, no retry in that session |

## 6. Testing strategy

### 6.1 Layers and what each proves

| Layer | Runs | Proves | Does **not** prove |
|---|---|---|---|
| Shared vectors | JVM (`test`) | codec (existing), PSK/prologue/`device_id_hash` (`ble_session_v1.txt`), handshake transcripts (`ble_handshake_v1.txt`, E7), QR formats (`qr_formats_v1.txt`, E11) | radio, Android BLE stack |
| Native (Android build) | Linux CI host: static NDK x86_64 test binaries (as the M1.10a gate) | the **Android** compilation of noise-c + libsodium + binding (bionic, FORTIFY on) reproduces cacophony + session vectors | JNI on a real ART runtime |
| JNI on the JVM | JVM tests with `libdspnoise_hosttest.so` (host build of the same binding) | Kotlin wrappers, handle lifetime, zeroization calls, error mapping, four handshakes against a simulated peer | bionic/ART behavior |
| Simulated firmware peer | JVM | fault injection the real firmware cannot produce on demand (reordering, duplicates, malformed device frames) over `FakeGattTransport` | firmware correctness; **not sufficient for integration acceptance** |
| **Firmware-in-the-loop (required, [E10])** | JVM + host build of the real `dsp_app`/`dsp_blesec`/`dsp_ble_transport` via JNI, simulated GATT | the Android client against the actual firmware application and security logic — **the integration acceptance level** | radio, Android BLE stack, target RNG, timing |
| Instrumented | emulator / test phone (manual, not CI) | JNI on ART, Keystore wrapping/StrongBox fallback, CDM dialog, permissions, Compose UI regression | real device interop |
| Real interop (M1.10e.5) | Android phone ↔ ESP32-C3 | everything above on real radios | — |

Simulated or host results are never reported as Android ↔ ESP32-C3 interoperability, as target
RNG correctness, or as hardware acceptance.

### 6.2 Fake GATT transport and simulated peer

- `FakeGattTransport : ByteTransport` with a virtual clock: configurable MTU (23…517, effective
  min with the peer's 247), write latency, notification scheduling, and fault injection —
  drop/duplicate/reorder/delay a fragment or a frame, corrupt a byte, disconnect at any step
  (before/after a write callback, mid-frame, mid-handshake, between request and response),
  stale callbacks from an old generation, GATT status 133, permission revoked, Bluetooth off.
- `SimulatedDispenser` (test source set): an independent device model written from
  BLE_PROTOCOL.md — HELLO/gate rules (10.6, three errors, 30 s), Noise **responder** through the
  host test library, ENROLL_RESULT, RECOVERY confirmation driven by a test "button", the 8-entry
  response cache, schedule store with revisions/DUPLICATE rules, manual-dose rule 1 with
  `manual_hw`/ring and **suspend-and-close** behavior, T3b silent drops, T13 best-effort final
  replies, EV_SESSION_CLOSING. It shares only the codec and Noise library with the client, not
  client code.

### 6.3 Firmware-in-the-loop **[E10, approved and required]**

Mocks alone are not accepted for integration. The Android client is tested against the real
firmware application and security logic compiled for the host:

- **Firmware side (`firmware/test/sim/`, new, test-only C library `libdsp_fwsim`):** links the
  unmodified `dispenser_domain`, `dispenser_security` (noise-c + libsodium, same pins/patches) and
  `dsp_ble_transport.c`, with host platform fakes moved out of the CTest harness into a reusable
  module: fake KV storage (with power-cut injection), simulated pump, monotonic and UTC clocks
  under test control, power port, setup-code secret port, and a **fake radio**
  (`dsp_ble_radio_t`) whose `notify` hands fragments to the JVM and whose `on_write` entry
  receives the client's fragments. RNG: the existing host deterministic RNG or the scripted RNG of
  E7, never in any shipped artifact.
- **Threads:** the firmware application task (`dsp_app` wake cycle with `stage_ble`) runs on its
  own native thread with the real queue/sync contract (as `test_platform`); the "NimBLE host
  task" role is the JNI caller thread that delivers connect/MTU/subscribe/write/disconnect
  callbacks. Time is virtual: the test advances the monotonic clock (service-window idle 180 s,
  15 min maximum, 30 s unauthenticated, 10 s handshake step) without real waiting.
- **C API (stable, small):** `fwsim_create(config)` (claimed/unclaimed, setup code, limits,
  schedules), `fwsim_button_wake()` / `fwsim_press()` (presence, recovery presses),
  `fwsim_connect(mtu)`, `fwsim_write(fragment)`, `fwsim_poll_notify()`, `fwsim_disconnect()`,
  `fwsim_advance_ms()`, `fwsim_power_cut()` + `fwsim_reboot()`, `fwsim_inspect()` (durable state for
  assertions: slots, `manual_hw`, ring, schedules, config revision), `fwsim_destroy()`.
- **Android side:** `FirmwareGattTransport : ByteTransport` (test source set) bridges the client
  to `libdsp_fwsim` through a small JNI shim, preserving generations, MTU and write-with-response
  ordering; fault hooks (drop/duplicate/delay a fragment, disconnect at a step) sit in this
  bridge, so faults hit the *real* firmware.
- **Build:** a Gradle task builds `libdsp_fwsim` + shim for the host with the system toolchain
  (CMake + gcc on the CI runner; JNI headers from the JDK), before `testDebugUnitTest`. ASan/UBSan
  builds are a local option; CI runs the plain build.
- **Acceptance suite (runs in CI):** CLAIM, ADD (invite created by a first client session),
  SESSION, RECOVERY (arm + presses via `fwsim_press`); every operation of section 7 with
  read-back; manual dose through the real suspend-and-close path and `GET_RESULT` after reconnect,
  including a power cut at each dosing write; revoked phone; three failed handshakes end the
  window; session limits (EV_SESSION_CLOSING, 15 min); a request dropped before decryption (T3b)
  and a lost encrypted response: the same-`txn` retransmission meets the nonce desynchronization,
  the session closes and the outcome is reconciled in a new session (14.3); a delayed response
  answered by retransmission from the real response cache; T13 lost final replies; transcript
  vectors (E7) replayed through the same library.
- **Distinction:** these results are reported as *firmware-in-the-loop (host)*, never as
  Android ↔ ESP32-C3 interoperability, radio behavior or target-RNG evidence.

### 6.4 Required cases (mapped to milestones)

| Area | Cases |
|---|---|
| Vectors | all `ble_protocol_v1` (existing), all `ble_session_v1` PSK/psk_not/prologue/idhash on JVM-JNI and native; cacophony XXpsk3 1/1, IK 2/2, 0 skipped, Android build; `ble_handshake_v1` on Android and firmware; `qr_formats_v1` (valid, invalid, check-character, expiry cases) |
| Handshakes | CLAIM, ADD, SESSION, RECOVERY success; negatives: wrong setup code, wrong/expired/used invite, wrong device key in ADD (abort before XX3), unknown/revoked phone key (IK), prologue mismatch (session ID of another connection, purpose, hash), `msg_index` order, step timeout, HANDSHAKE_FAILED ×3, decrypt failure after auth, plaintext after auth |
| Link | fragmentation at every MTU 23…247 and 517 (clamped) × boundary lengths; reassembly faults; 2 s gap |
| Faults | duplicate, lost, reordered fragments/frames; disconnect during each handshake step and during each command; stale callbacks of generation n−1 after n connected; response after timeout (discarded by txn); delayed response → same-txn retransmit served from cache; device silent drop (T3b) or lost encrypted response → nonce desync, session closed, outcome uncertain, reconciled in a new session without duplicate execution |
| Authorization | requests before auth not sent; FORBIDDEN after revocation; role checks |
| Manual dose | seq durable before send; process death between persist and send; suspend-close → reconnect → GET_RESULT for every state 0–6; never a new seq automatically; floor resync after reinstall (`manual_floor`) |
| Persistence | credential save/load, Keystore key loss → LOST (never regenerated), tamper → unreadable, export excludes credentials, process restart with pending ops |
| Logging | ShadowLog spy: no secret in any log across the full suite |
| UI regression | existing JVM ViewModel tests and instrumented Compose tests unchanged and green in DEMO mode; DEVICE-mode UI tests on a fake repository |
| Architecture | `ArchitectureRulesTest`; native symbol check |

## 7. Functional integration (design plan, written before M1.10e.4; implemented state: section 16)

| Screen / feature | Device source (protocol) | Notes |
|---|---|---|
| Onboarding & ownership | CDM association → CLAIM (label QR or manual code entry) / ADD (device label + owner's invite QR) / RECOVERY; PHONES_LIST, PHONE_REMOVE, INVITE_CREATE/CANCEL, RECOVERY_CANCEL | QR/label formats in 7.1 **[E11]**, scanning 7.2 **[E15]**; recovery needs the device button (M10) for the 3 presses |
| Dashboard / status | GET_STATUS: readiness, gates, pump problems, reservoir estimate, clock trust, next due, battery, severity, session seconds left | badge "DEVICE" vs "DEMO"; cached values shown with their read time |
| Diagnostics | GET_DIAGNOSTICS, ACK_REJECTIONS | under Settings → device |
| Configuration | GET_CONFIG / SET_CONFIG (revisioned, field mask; limits only in SERVICE_REQUIRED) | read-before-write; conflict UI |
| Schedules | LIST (paged, 4/page) + GET each; CREATE/UPDATE/DELETE with revisions; read-back → `DEVICE_APPLIED` | device is the source of truth in DEVICE mode; Android IDs (UUID) fit `str(1..64,id)`; µl `u32` limit enforced before sending; device timezone is Europe/Prague (non-Prague schedules refused in DEVICE mode); edits are online-only in v1 **[E12]** |
| Manual dose | MANUAL_DOSE{seq, µl} → EV_SUSPENDING_FOR_DOSE → disconnect → reconnect → GET_RESULT | confirmation dialog; journal entry before send; no auto-reissue |
| Refill / readiness | REFILL (absolute capacity/remaining), SET_READINESS | existing reservoir screen; no automatic resend |
| Time | SET_TIME when Status `clock_trust` ≠ SYNCED or after REFUSED detail 3 | uncertainty must be ≤ 2,000 ms; source of phone time and its uncertainty **[E13]** |
| Notifications / errors | Status `pending_notes`, `highest_severity`; error table 5.2; EV_SESSION_CLOSING countdown | no Android push notifications (e-mail delivery stays in Cloudflare) |
| History | not in protocol v1 (types 0x90–0x9F reserved) | DEVICE mode shows no device history in v1; simulated history stays DEMO-only |
| Wi-Fi provisioning | WIFI_* (`caps` bit 0; current firmware: UNAVAILABLE) | out of scope for M1.10e **[E14]** |

### 7.1 QR and code formats **[E11: flows approved; invitation layout for final approval]**

Three kinds of information, kept in separate formats because they have different holders,
lifetimes and security purposes:

| | Setup label | Invitation | Recovery |
|---|---|---|---|
| Purpose | proves possession of the device (PSK input for CLAIM, ADD, RECOVERY) | proves an owner's consent to one additional phone (ADD) | — |
| Holder | printed on the device / its packaging | shown on the inviting owner phone's screen | the physical owner |
| Secret | yes (128-bit setup code) | yes (128-bit invite token; useless without the setup code and a button press) | no new secret |
| Lifetime | device lifetime (rotation only by re-provisioning) | ≤ 10 min, single use (enforced by the device) | — |
| Format | `DSP2:` label (existing, O7) | new `DSP2I1:` format | **no QR**: setup label + physical procedure |

**Why not one format.** Merging the setup code into the invitation would let any owner phone
re-share the device-possession secret (and require phones to store it, against E4); the
two-factor property of ADD (label **and** owner invite **and** presence, BLE_PROTOCOL_DESIGN 5.3)
depends on the two travelling separately. Recovery deliberately has no transferable token: its
authority is the setup code plus the boot hold and three presses on the device.

**A. Setup label (existing, unchanged; `firmware/tools/provision_dev_secrets.py`).**

```text
DSP2:<device-id>:<code>
  device-id  12 hex digits, the factory MAC (canonical lowercase; parser accepts either case)
  code       28 Crockford base32 characters = 26 (128-bit setup code, MSB first, 2 zero pad bits)
             + 2 check characters (first 10 bits of SHA-256("DSP2 setup" || code))
total 46 characters (QR byte mode; also typed manually)
```

- Versioning: the prefix `DSP2:` with exactly three `:`-separated fields is v1; any future label
  version uses a different prefix (`DSP2L2:` …), never a reinterpretation.
- Validation: exact length and charset; Crockford decoding for manual entry (case-insensitive,
  `I`/`L` → 1, `O` → 0, hyphens and spaces ignored when typed, never in QR); pad bits zero;
  check characters must match (detects typing errors, not attacks).
- Identity binding: the device ID selects the device for CDM filtering and the unclaimed
  `device_id_hash` check; authentication is the XXpsk3 handshake with the derived PSK.
- Sensitive handling: decoded into `SecretBytes`, used for one PSK derivation, zeroized; never
  stored, logged, put in an Intent extra, clipboard or screenshot (scanner/entry screens use
  `FLAG_SECURE`).

**B. Invitation `DSP2I1` (new, layout final for review).** Created by owner phone A from the
encrypted `INVITE_CREATE` response `{token bytes(16), expires_s u16}`; scanned by new phone B.

Payload: exactly **79 bytes**, all integers little-endian.

| Offset | Size | Field | Rule |
|---|---|---|---|
| 0 | 1 | `version` | `0x01`; any other value → unsupported (not "invalid", so a later app can say "update the app") |
| 1 | 6 | `device_id` | factory MAC as on the setup label, byte order as printed (`a0b1c2d3e4f5` → `a0 b1 c2 d3 e4 f5`) |
| 7 | 32 | `device_static_pub` | the device identity public key A has pinned (X25519); not all zero |
| 39 | 16 | `invite_token` | exactly the 16 bytes from INVITE_CREATE, unmodified |
| 55 | 4 | `expires_utc` | u32 seconds since 1970-01-01 UTC: A's UTC time when the response arrived, truncated to seconds, + `expires_s` |
| 59 | 4 | `created_utc` | u32 seconds: A's UTC time when the response arrived, truncated to seconds |
| 63 | 16 | `check` | first 16 bytes of SHA-256(ASCII `"DSP2 invite v1"` (14 bytes, no terminator) ‖ bytes 0..62) |

- **Token entropy:** 128 bits generated by the **device** at INVITE_CREATE from its hardware RNG
  (the same adapter as all security randomness; if no randomness is available or the token is all
  zero, INVITE_CREATE answers `DEGRADED` and creates nothing — `dsp_app_invite_create`). The
  device keeps one pending invitation in its RTC-retained state, bound to the creating slot and the
  ownership epoch, and refuses INVITE_CREATE with `SLOTS_FULL` when no slot is free. The phone never generates or alters the token. Its security comes from the
  setup code it is combined with (PSK = HKDF(setup code, ADD ‖ token)), the button press and the
  device-side expiry and single use.
- **Encoding:** `DSP2I1:` followed by the payload in **Crockford base32**: alphabet
  `0123456789ABCDEFGHJKMNPQRSTVWXYZ`, MSB first, the 632 payload bits followed by **3 zero pad
  bits** = exactly **127 characters**, uppercase, no padding characters, no separators; total
  134 characters, all in the QR alphanumeric set (encoded in alphanumeric mode, error correction
  level M).
- **Canonicalization (strict):** the decoder accepts only the canonical form: exact prefix, exact
  length 127, uppercase alphabet characters only (no lowercase, no `I`/`L`/`O`/`U`, no hyphens or
  whitespace), pad bits zero, and re-encoding the decoded bytes must reproduce the input. Manual
  typing of invitations is not supported (QR only), so the lenient Crockford rules of the setup
  label do not apply here.
- **Timestamps and expiry validation (on B):** `created_utc` ≤ `expires_utc` and
  `expires_utc − created_utc` ≤ 600 (the device's 10-minute maximum), else invalid. With B's
  current UTC time `now`: `now > expires_utc + 120` → shown as expired and not used;
  `created_utc > now + 300` → "phone clocks differ" warning, still usable. These checks are
  convenience only: phone clocks are untrusted, and the **device alone decides** validity
  (its own expiry, single use, consumption on success).
- **Integrity digest, not authentication:** `check` is an **unkeyed** hash. It detects accidental
  corruption (misread QR, truncation, wrong code type); anyone can recompute it, so it provides
  **no protection against malicious modification and no authenticity**. A forged or altered
  invitation can at most make B attempt a handshake that fails.
- **Authentication comes only from the device:** B accepts the device only if (1) HELLO reports
  CLAIMED with `device_id_hash` = SHA-256(`"DSP2 id"` ‖ `device_static_pub`)[0..8), (2) the
  device static key received in XX message 2 **equals** `device_static_pub` — otherwise B aborts
  before sending message 3 — and (3) the XXpsk3 handshake completes (proving the device knows the
  setup code PSK). The QR alone never authenticates the device, and the device never accepts B
  because of the QR alone (it needs the PSK from the setup code **and** the token, plus presence).
- **Token lifetime and handling:** on A the token exists only in a `SecretBytes` and in the QR
  bitmap while the invitation screen is shown; both are cleared when the screen closes, the
  invitation expires, succeeds or is cancelled (`INVITE_CANCEL` offered). On B the decoded payload
  lives in a `SecretBytes` only until the ADD attempt ends (success, failure, cancel, expiry,
  process death). Never persisted (no Room, DataStore, files, `SavedStateHandle`, Intent extras),
  never logged (the log spy of 3.3 covers the token), never on the clipboard or in a share
  intent; the scanner and display screens use `FLAG_SECURE`; ML Kit runs bundled and offline, so
  camera frames never leave the process.

Reference vector (all values synthetic; `device_static_pub` is a byte pattern, not a real key;
the token is the "rand" token of `ble_session_v1.txt`):

```text
version            01
device_id          a0b1c2d3e4f5
device_static_pub  202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f
invite_token       bacd0fa8879ea4dd104c412f93e1a7cb
created_utc        1795000000 (2026-11-18T11:06:40Z)  -> c086fd6a
expires_utc        1795000600 (2026-11-18T11:16:40Z)  -> 1889fd6a
check              1c75d6518b193fe01e456cf277b660a1
payload (79 B)     01a0b1c2d3e4f5202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f
                   bacd0fa8879ea4dd104c412f93e1a7cb1889fd6ac086fd6a1c75d6518b193fe01e456cf277b660a1
QR text (134 ch)   DSP2I1:06GB3GPKWKTJ08924CJ2A9H750MJMASC5MQ2YC1H68SK8D9P6WW3JEHV7GYKWFXTSM7TH1WYMKEH0K215Y9Y39YB324ZTTP0GVYPM73NTS8RP69ZW0F4AV7JEYV6188
device_id_hash      f7f4b2cd5985e1e1   (SHA-256("DSP2 id" || device_static_pub)[0..8), expected in HELLO)
```

(Computed with Python `hashlib` and integer base32 conversion; decoding the QR text reproduces
the payload with zero pad bits. `qr_formats_v1.txt` will carry this record and the invalid
variants.)

**C. Recovery.** No QR and no stored recovery secret. The flow uses the setup label (A) and the
device procedure (boot hold to arm, button session, three presses during `EV_CONFIRM_ON_DEVICE`).
The app shows the procedure and requires the label scan/entry; it stores nothing until
`ENROLL_RESULT{0}`.

**Vectors:** `test-vectors/qr_formats_v1.txt` with a Python generator (`hashlib` only, no
cryptographic primitive implemented): valid labels and invitations, every invalid case (prefix,
length, charset, pad bits, check, version, digest, zero key), manual-entry normalizations, and the
binding expectations (`device_id_hash` values) — consumed by Kotlin; the label cases also by the
provisioning tool's tests.

### 7.2 QR scanning **[E15, approved: ML Kit, bundled]**

- **ML Kit barcode scanning, bundled model** (`com.google.mlkit:barcode-scanning`, offline, no
  Google Play services or model download at first use; supports the app's minSdk 26) with
  **CameraX** for the preview, restricted to QR format.
- **Scoped to onboarding:** a separate Gradle library module `:onboarding-scanner` that only the
  onboarding/invite screens depend on; `CAMERA` permission requested only when the user opens the
  scanner; manual code entry always available (no camera needed).
- Implications to confirm in M1.10e.4: APK size (bundled model, measured per ABI), ML Kit's
  license terms (Google APIs terms, not open source) recorded in the bundled third-party list,
  R8 rules; versions pinned in the catalog.

## 8. Implementation milestones

*The plan as approved; the current status of each milestone is the table at the top of this document.*

| Milestone | Content | Exit criteria | App behavior change |
|---|---|---|---|
| **M1.10e.1** Native Noise provider, vectors, credentials | NDK/CMake build reusing the pinned fetch/patch/verify logic; JNI binding; host test library; `NoiseProvider`, `SecretBytes`, `CredentialStore` (Keystore); label + invitation parsers; `ble_handshake_v1` generator (`snow`) and `qr_formats_v1` generator; firmware host test replaying the transcripts; symbol check; licenses (noise-c MIT, libsodium ISC); CI installs NDK + CMake | official vectors (0 skipped), session, transcript and QR vectors pass on JVM-JNI and Android-native x86_64 (FORTIFY); firmware replays the transcripts byte for byte; Keystore instrumented tests on an emulator (reported as manual); CI green | none |
| **M1.10e.2** Transport, session, firmware-in-the-loop | `GattTransport`, `GattOpQueue`, `FrameLink`, `SecureSession`, `DeviceSessionManager`, `DeviceDiscovery` (CDM, fallback scan); `FakeGattTransport`, `SimulatedDispenser`; **`libdsp_fwsim` + `FirmwareGattTransport`**; manifest permissions (inert until used) | handshake, link, fault and authorization cases (6.4) against the simulated peer **and** the real firmware (FIL, in CI); architecture test | none (not wired) |
| **M1.10e.3** Repository + operations | typed messages; `DeviceOperations` with the retry table; `DeviceDispenserRepository`; device cache (`device.db`, schema exported); pending-ops journal; manual dose + GET_RESULT | repository contract tests; **FIL acceptance suite** (every operation with read-back, manual dose with power cuts, limits, lost replies) | none (DEVICE mode not selectable) |
| **M1.10e.4** UI + onboarding | AppMode switch; onboarding (CDM, label scan/entry, invitations), connect flow, device status/config/schedules/manual dose/refill/time, phones & invites, recovery screens; `:onboarding-scanner` (ML Kit + CameraX); Czech strings; existing design | UI tests in both modes (DEVICE mode on the FIL-backed repository where practical); DEMO regression suite unchanged | yes (reviewed) |
| **M1.10e.5** Real-device interop & acceptance | Android phone matrix (≥ one 12+, one 8–11) ↔ ESP32-C3 with the bring-up window; together with M1.10d.2 H1–H10 and M1.10c.2b R1–R8; E8 open points (direct connect reliability, CDM and Location Services) | acceptance checklist signed off on hardware | — |

M1.10e.1–.4 need no hardware; .5 depends on the hardware gates (RNG gate first). If the review prefers smaller steps, M1.10e.2 splits into .2a
(GATT transport + link + fake) and .2b (session + simulated peer).

## 9. Resources and dependencies

| Item | Impact |
|---|---|
| NDK | r30 `30.0.16248370` pinned via `android.ndkVersion` (already installed locally; CI installs it with `sdkmanager "ndk;30.0.16248370"`) |
| CMake | SDK CMake ≥ 3.24 (gate requirement), pinned via `externalNativeBuild.cmake.version` (e.g. 3.31.x); CI installs `cmake;<version>` |
| Build-time network | first configure fetches noise-c and libsodium by commit (same as the firmware); CI caches the fetched trees (`DSP_DEPS_CACHE` under the Gradle cache) |
| APK size | estimate +60–100 KB per ABI (gate probe 54–79 KB stripped); ABIs: arm64-v8a, armeabi-v7a, x86_64 (emulators) — x86 dropped **[E16]**; universal APK ≈ +250 KB |
| CI time | + NDK/CMake install and 3 ABI builds, host JNI test library and `libdsp_fwsim` builds (estimate +5–10 min, measured in M1.10e.1/.2) |
| Gradle libraries | none required for e.1–e.3 (coroutines already present via lifecycle; `kotlinx-coroutines-android` pinned explicitly if needed). e.4: ML Kit barcode scanning (bundled) + CameraX in `:onboarding-scanner` only (E15) |
| Vector tooling | Rust toolchain (present locally) + pinned `snow`, `hkdf`, `sha2` crates with committed `Cargo.lock` for `ble_handshake_v1` (generator only; not built in CI unless approved — CI consumes the committed vector file) |
| FIL build | host CMake + gcc + JDK JNI headers on the CI runner; reuses the firmware's pinned noise-c/libsodium fetch (cached) |
| R8 | keep rules for JNI classes/`external` methods |
| Licenses | noise-c (MIT) and libsodium (ISC) added to `third_party_licenses.json` (native sources are not Gradle dependencies, so manual entries + `checkThirdPartyLicenses` coverage) |
| Runtime memory | handshake 2–4 KB native heap (noise-c), two CipherStates afterwards |

## 10. Risks

- **Direct connect without scanning** may be unreliable on some phones/firmware windows — fallback
  [E8], measured in M1.10e.5.
- **CDM UX varies by vendor** (dialog, association limits); association needs the device to be
  advertising, i.e. a service window (button, or the T7 bring-up option before M10).
- **Recovery and ADD presence** depend on the device button (M10); until then only the
  bring-up power-on window exists, and RECOVERY's three-press confirmation cannot be exercised on
  hardware.
- **Keystore variability** (StrongBox absent, vendor bugs, key invalidation): handled as LOST
  credentials; re-enrollment needs another phone or recovery.
- **No emulator in CI**: instrumented tests remain manual unless [E17] adds an emulator job.
- **FIL fidelity:** the host build shares all domain/security code with the target but not the
  NimBLE adapter, FreeRTOS scheduling or radio timing; those stay M1.10d.2/M1.10e.5 items.
- **ML Kit** is proprietary (Google terms) and adds a bundled model; confined to one module and
  measured in M1.10e.4.

## 11. Decisions (E1–E17)

Status: **approved** (reviews 2026-11-18) or **for final approval**. The focus topics of the
review (Keystore lifecycle, backup/restore, phone replacement, interrupted manual doses,
process death, concurrent requests) are specified in 11.1–11.6 and referenced from the rows that
decide them. E9 and E12 are restated more precisely than in revision 2 (scope, not direction).

| # | Decision | Recommendation | Tradeoffs | Status |
|---|---|---|---|---|
| E1 | App modes DEMO and DEVICE, persisted; DEMO is the default; DEVICE selectable only after a successful enrollment; simulation, `simulation.db` and the DEMO badge untouched; switching back to DEMO keeps the device credential | approve | + demo always available and visibly separate; − two repository implementations; the mode switch must never mix sources (tested) | for final approval |
| E2 | DEMO (phone-local) schedules never uploaded automatically; after a claim the user may copy them explicitly (new device IDs, read back, then DEVICE_APPLIED) | approve | + no silent change to physical dosing; − one extra user step | for final approval |
| E3 | The app's CMake reuses `firmware/third_party/noise-c/noise_c_deps.cmake` (new Android function; firmware paths unchanged) | approve | + one source of pins and patch checks; − Android build depends on a file under `firmware/`; edits there re-run Android vectors | for final approval |
| E4 | Setup code and invitation token never persisted on any phone; held in `SecretBytes` only for their flow (7.1) | approve | + a stolen or backed-up phone leaks no device-possession secret; − the label is needed again for ADD and RECOVERY | for final approval |
| E5 | One phone static key pair per device association; never shared between devices or phones; never exported | approve | + no cross-device linkability; replacement/revocation per device; − one credential per device (small) | for final approval |
| E6 | Keystore lifecycle and backup policy as 11.1–11.2: AES-256-GCM wrapping key per device association, StrongBox preferred, TEE fallback, no per-use user authentication, `UnlockedDeviceRequired` (API 28+); credentials and device cache excluded from every backup and export; loss handled as LOST, never regenerated silently | approve | + no biometric prompt per BLE session; hardware-bound keys where available; no secret ever leaves the phone; − key loss (reinstall, clear data, Keystore reset) forces re-enrollment (11.3); malware in the unlocked app context can use the key (approved residual T8) | for final approval |
| E7 | Official Noise vectors (conformance) separate from `ble_handshake_v1.txt` (Dispenser framing, Rust `snow`, exact pins + `Cargo.lock`), application negative tests kept; deterministic ephemerals test-only (3.5) | — | + no home-made primitives, independent implementation; − Rust tool to maintain (generator only) | **approved** |
| E8 | CDM for initial discovery; direct GATT reconnection to the associated address; re-association for missing associations/changed addresses; foreground fallback scan; address never an identity (4.1) | — | + no location permission in the normal path; − vendor behavior to confirm on phones (M1.10e.5) | **approved** |
| E9 | Session lifecycle and concurrency as 11.5–11.6: one process-wide session actor; no foreground service; 30 s background grace then disconnect; explicit "Done" = encrypted END_SESSION; one request in flight, bounded FIFO, manual dose exclusive; process death recovers from durable state only | approve | + simple, predictable lifecycle matching the device's one-connection, one-request model; no persistent notification; − leaving the app ends the session after 30 s; queued requests are serialized (a slow write delays reads) | for final approval |
| E10 | Firmware-in-the-loop with the real firmware application/security logic compiled for the host (`libdsp_fwsim`), simulated GATT; required for integration acceptance, in CI (6.3) | — | + spec divergence caught before hardware; − host native build and reusable firmware harness | **approved, required** |
| E11 | Separate formats: setup label (existing `DSP2:`), invitation `DSP2I1:` (79-byte layout, reference vector, strict canonical base32, unkeyed check digest = corruption detection only), recovery without QR (7.1) | approve the final layout | + secrets with different purposes never travel together; ADD keeps both factors; − ADD needs two scans (label + invitation) | flows **approved**; layout for final approval |
| E12 | DEVICE-mode mutations are online only: no offline queue of device edits; every mutation is journaled when its outcome can be lost (11.4) and resolved by read-back; a manual dose is journaled before sending and **never re-issued with a new `seq` automatically** | approve | + no stale offline edits, no duplicate dose; − every change needs a connection (button press); unresolved items block a new manual dose until resolved | for final approval |
| E13 | SET_TIME only with Android automatic network time on and an uncertainty bound ≤ 2 s; sent when the device clock is not trusted or after REFUSED detail 3 | approve | + never feeds an unreliable phone clock; − phones with a manual clock cannot set device time | for final approval |
| E14 | Wi-Fi provisioning out of M1.10e (firmware answers UNAVAILABLE) | approve | + smaller scope; − time stays BLE/phone-based until later | for final approval |
| E15 | Bundled offline ML Kit barcode scanning + CameraX in `:onboarding-scanner`; manual setup-code entry always available (7.2) | — | + reliable offline scanning on minSdk 26; − proprietary dependency and size, confined to one module | **approved** |
| E16 | ABIs arm64-v8a, armeabi-v7a, x86_64 (drop x86) | approve | + smaller APK, x86_64 covers emulators; − no 32-bit x86 | for final approval |
| E17 | No CI emulator job now; instrumented tests (Keystore, JNI on ART, CDM, UI) run manually and are reported as such | defer | + no CI cost/flakiness; − those tests are not automated | for final approval |

### 11.1 Android Keystore lifecycle (E6)

| Phase | Behavior |
|---|---|
| Creation | at the start of an enrollment flow (CLAIM, ADD, RECOVERY): new AES-256-GCM key, alias `dsp_cred_v1_<device-id>`, `PURPOSE_ENCRYPT \| PURPOSE_DECRYPT`, `setRandomizedEncryptionRequired(true)`, `setIsStrongBoxBacked(true)` on API 28+ (on `StrongBoxUnavailableException` retried without), `setUnlockedDeviceRequired(true)` on API 28+, no user authentication. The new phone static key pair is generated in native code at the same time. |
| Stage | **before** sending XX message 3 (the device commits when it receives message 3, before it answers): the wrapped key, the device key from message 2 and the name are written atomically as a staged record (`<device>.pending`, slot unknown). Failures before message 3 was sent delete the new key and zeroize the key pair; a staged enrollment can only be confirmed or explicitly discarded (11.7). |
| Commit | on `ENROLL_RESULT`: the staged record becomes the credential (same key), atomically. |
| Use | per handshake: decrypt into `SecretBytes`, pass to native, zeroize both sides after the handshake (success or failure). The Keystore key never leaves secure hardware. |
| StrongBox fallback | API 28+: one StrongBox attempt; `StrongBoxUnavailableException` → the key is created in the TEE (or the platform's software keystore); other Keystore errors are not caught (no silent weaker key); below API 28 TEE directly. Tests: `KeystoreFallbackTest` (decision logic, injected generator), instrumented `strongbox_or_fallback_matches_the_device`. |
| Security level | `KeyInfo` records StrongBox / TEE / software for the device-details screen (no secret); software-only keystores are accepted but shown. |
| Replacement | RECOVERY or re-enrollment creates a new key and credential; the old ones are deleted only after the new `ENROLL_RESULT` commit. |
| Deletion | "Forget device" (after an optional `PHONE_REMOVE` of the own slot while connected), detected revocation acknowledged by the user, or uninstall: credential file and Keystore key deleted together. |
| Invalidation / loss | `KeyPermanentlyInvalidatedException`, `AEADBadTagException`, missing key with an existing file (reinstall with leftover data is impossible, but Keystore resets happen): credential state LOST; nothing is regenerated automatically; the UI offers re-enrollment (11.3). `UnlockedDeviceRequired` makes the key unusable while the phone is locked; sessions only run in the foreground, so this only fails closed. |
| Tests | Keystore behavior in instrumented tests (emulator, reported as manual); JVM tests use a `KeyWrapper` fake behind the same interface. |

### 11.2 Backup and restore (E6)

- System backup stays off (`allowBackup=false`, extraction rules exclude all domains): no cloud
  backup and no device-to-device transfer of any app file. Keystore keys are never backed up by
  Android in any case.
- The explicit JSON export (`BackupService`) contains phone-local data only (DEMO schedules,
  simulation, settings). It never contains credentials, the device cache, the pending-ops journal
  or any device data; a unit test asserts this. The backup format version is unchanged.
- Restoring a JSON backup never touches credentials, the journal, the device cache or the app
  mode; in DEVICE mode the device remains the source of truth for schedules.
- Consequence: device ownership never moves with a backup. A new or reset phone enrolls through an
  invitation or recovery (11.3).

### 11.3 Phone replacement and credential loss (E4–E6)

| Situation | Path (BLE_PROTOCOL_DESIGN 5.5) |
|---|---|
| New phone, old phone available | old phone: INVITE_CREATE → `DSP2I1` QR; new phone: scans the invitation and the device label, button press, ADD; old phone (or new) removes the old slot (`PHONE_REMOVE`) |
| Old phone lost, another enrolled phone exists | the other phone removes the lost slot; the new phone is added by invitation |
| App reinstalled / data cleared / Keystore lost on the same phone | the phone is new to the device: invitation from another enrolled phone, else recovery; an owner removes the stale slot (the app shows the old slot as "this phone (previous installation)" only if the name matches, never automatically) |
| No enrolled phone left | RECOVERY with the setup label, boot hold and three presses (needs the M10 button) |
| Slots full (4) | `SLOTS_FULL`: an owner removes a slot first |

Credentials are never exported, transferred or cloned between phones. Manual-dose counters
survive replacement on the device side (`manual_hw` per slot index, `manual_floor` in Status), so
a new phone resynchronizes `seq` from the floor.

### 11.4 Interrupted manual-dose operations (E12)

Journal: table `pending_ops` in `device.db` (not exported), one row per unresolved operation;
manual dose rows `{device_id, seq, volume_ul, created_at, state, last_result}`.

1. **Before sending (D4):** `seq = max(local_high + 1, manual_floor)` with `manual_floor` from a
   Status read **live in the same session immediately before**. Fixed order of two durable writes
   (the high-water mark lives with the credential, the journal in `device.db`, so they cannot share
   one transaction): (a) the new local high, (b) the journal row (`state = PREPARED`); only after
   both succeed is MANUAL_DOSE sent. Any persistence failure prevents transmission. A crash after (a)
   only skips the number (seq need only increase); a crash after (b) is resolved by `GET_RESULT`
   (PREPARED and SENT are treated alike: "may have reached the device"). An allocated number is
   never reused; a NOT_DECIDED dose is re-sent only on the user's request and only with its
   original `seq`.
2. **Normal path:** the device sends `EV_SUSPENDING_FOR_DOSE` and closes the link (BLE suspended
   for the pump); the response normally never arrives. If it does arrive (DONE / PUMP_FAILED or an
   error), the row is resolved.
3. **Resolution** (automatically at the start of the next authenticated session, before any new
   manual dose): `GET_RESULT{seq}`:

| `state` | Meaning | App action |
|---|---|---|
| 0 NOT_DECIDED | the request never reached a decision | offer "send again" **with the same `seq`** (safe: decided at most once); never a new `seq` automatically; or discard on user request |
| 1 PENDING | in progress, or interrupted until the next boot reports it | keep unresolved, show "in progress / check later" |
| 2 DONE / 3 FAILED / 5 REFUSED | decided | resolve, show outcome (detail mapped) |
| 4 INTERRUPTED | power loss or reset during the pump | resolve as interrupted: "the dose may be partial — check the aquarium"; no retry offered automatically |
| 6 ALREADY_DECIDED_RESULT_UNAVAILABLE | decided, result evicted | resolve as "decided, result unknown — check the device"; never re-issued |

4. While any manual-dose row is unresolved, the UI offers no new manual dose (prevents stacking
   and confusion); reads and other operations stay available.
5. Other mutations with a lost response (SET_CONFIG, schedule writes, REFILL, SET_READINESS,
   PHONE_REMOVE) are journaled `{op, target, expected revision, request summary}` before sending
   and resolved by read-back per table 5.1 at the next session; REFILL is never resent
   automatically.

### 11.5 Process death (E9)

- **Lost by design:** the live GATT connection, the Noise session, in-flight continuations.
  Nothing secret survives process death (native memory is gone; no secret is ever persisted).
- **Survives:** credentials (11.1), the device cache (last confirmed snapshot with its read time),
  the `pending_ops` journal, the app mode, local `seq` high water.
- **On restart:** the repository serves the cached snapshot, marked as cached with its age (never
  as live, never DEVICE_APPLIED for edits not read back); unresolved journal rows appear as
  "result unknown — connect to check". At the next session the resolution of 11.4 runs before new
  mutations. ViewModels hold no session state, so configuration changes and process recreation
  need no special handling beyond reading the repository flows.
- Enrollment interrupted by process death: before XX message 3 nothing is stored; after it the
  staged record survives and is resolved as in 11.7 (IK confirmation, or explicit discard); the
  app never assumes success and never silently drops a key the device may have enrolled.

### 11.6 Concurrent requests (E9)

- **Device constraints:** one connection (T1); requests processed one at a time; response cache
  per session; BLE suspended during a pump run.
- **One session actor** (`DeviceSessionManager`) owns the connection; all callers go through
  `DeviceOperations`, which submits to a bounded FIFO (capacity 8; a full queue fails fast with
  `Busy`). Exactly **one request in flight**; the next is sent only after the response, an error,
  or the retry policy giving up. Events are delivered on a separate flow and never block requests.
- **Cancellation:** a queued request is removed when its caller is cancelled; a sent request
  cannot be recalled — its outcome is still processed and journaled.
- **Exclusive operations:** a manual dose drains the queue first, is sent alone and ends the
  session (suspend-and-close); handshakes are exclusive by construction.
- **Duplicate intents:** repeated taps on the same mutation while one is queued or unresolved are
  rejected in the repository (same target + operation), so the user cannot queue two manual doses
  or two conflicting edits; revision checks on the device catch anything else (`CONFLICT`).
- **Two phones:** the device serves one connection; the second phone's connect fails or waits —
  shown as "the device is busy with another phone"; their schedule edits are serialized by
  device revisions.

### 11.7 Lost final ENROLL_RESULT

The device stores the enrollment durably when it accepts XX message 3 and only then sends the
encrypted ENROLL_RESULT (RECOVERY: after the three presses). If the connection or that frame is
lost, the device may hold an enrollment the phone never saw confirmed. The phone therefore never
discards the key it sent in message 3 silently:

1. **Stage before message 3** (`CredentialStore.stage`): key, pinned device key (message 2), name.
2. **ENROLL_RESULT received** → `commit` (slot from ENROLL_RESULT). Done.
3. **ENROLL_RESULT not received** (timeout, disconnect, process death) → the staged record stays;
   the UI shows "enrollment not confirmed — press the device button to check", never "enrolled".
4. **Resolution at the next connection:** HELLO must report CLAIMED with `device_id_hash` of the
   staged device key; then an IK SESSION with the staged key and staged device key. Success proves
   the device stored it → `PHONES_LIST` (`is_self`) gives the slot → `confirmPending(slot)`.
5. **IK fails** (HANDSHAKE_FAILED, or the device is still UNCLAIMED / reports another identity) →
   the device did not store it (or it was removed): the app offers "discard and enroll again";
   `discardPending` runs only on that explicit user decision. An existing confirmed credential of
   the same device is untouched by a staged one until confirmation.

Evidence: firmware `test_lost_enroll_result` (message 3 delivered, ENROLL_RESULT lost → the device
holds slot and identity, and the next IK SESSION transcript is accepted byte for byte, for CLAIM and
ADD; message 3 never delivered → nothing stored); Android `CredentialStoreTest` staged cases
(survives restart, cannot be aborted silently, confirm, explicit discard keeps the old credential,
key loss reported). The resolution flow itself is wired in M1.10e.2/.3 (session manager).

## 12. Implementation notes (M1.10e.1)

| Item | Implemented |
|---|---|
| Native library | `android/app/src/main/cpp`: `dspnoise_core.c` (C over noise-c/libsodium, initiator only, CSPRNG), `dspnoise_jni.c` (glue), `CMakeLists.txt` (reuses `noise_c_deps.cmake`), version script, post-build symbol check. Release sizes: arm64-v8a 73,400 B, armeabi-v7a 56,320 B, x86_64 78,264 B (estimate was 60–100 KB). FORTIFY in all 76 units per ABI. |
| Kotlin | `crypto/`: `SecretBytes`, `NoiseException`, `NativeNoise` (internal), `NoiseProvider`/`NativeNoiseProvider`/`HandshakeInitiator`/`TransportCiphers`, `SessionDerivation`, `SetupLabel`, `Invitation`, `Crockford`; `crypto/credentials/`: `KeyWrapper`, `AndroidKeystoreKeyWrapper`, `CredentialStore`. Not wired into the app (no behavior change). |
| Test-only native | `android/app/src/test/native`: `dspnoise_test_hooks.c` (responder, fixed ephemerals) in `libdspnoise_hosttest`; `dspnoise_vectors_test.c` (official, session, handshake vectors) built for the host and as an NDK x86_64 static executable. |
| Vectors | `test-vectors/ble_handshake_v1.txt` (Rust `snow` 0.10.0, `Cargo.lock`), `test-vectors/qr_formats_v1.txt` (Python stdlib); firmware replay `firmware/test/host/test_handshake_vectors.c` with a host-only scripted RNG (`test_rng_script`). |
| CI | NDK + CMake installed, pinned sources cached, `nativeAndroidTest` added; instrumented tests still compiled only. |

Deviations from the design text (none changes a decision):

- **E3:** the app CMake calls the existing, toolchain-neutral `dsp_noise_c_host()` of
  `noise_c_deps.cmake` instead of adding a `dsp_noise_c_android()` wrapper, so the Android change
  set does not modify the firmware tree. Pins, fetch-by-SHA, patching and checks are the same code.
- **Log spy (3.3):** instead of Robolectric `ShadowLog`, a static rule forbids any logging or
  printing in `crypto/` (`ArchitectureRulesTest`) and `SecretLeakSpyTest` captures stdout/stderr
  and `toString`/exception texts across the full transcript, parser and credential suites. Reason:
  a JNI library can be loaded by only one class loader per JVM, and Robolectric tests use their
  own; the crypto package has no Android log calls to capture.
- **Keystore alias (11.1):** each enrollment gets its own alias `dsp_cred_v1_<device-id>_<random>`
  (recorded in the credential file) instead of a fixed per-device alias, so a replacement
  (RECOVERY/re-enrollment) keeps the old credential usable until the new record is durable.
- **Storage location:** `CredentialStore` takes its directory; the app will pass
  `noBackupFilesDir/credentials` when it is wired in (M1.10e.3); excluded from every backup domain
  in any case.
- **Residual (7.1 B):** the scanned invitation text is a Java `String` that cannot be zeroized;
  the decoded token is held in `SecretBytes`.
- **Maintenance item (not a blocker, separate from M1.10e):** Espresso 3.6.1 is incompatible
  with API 37 (`InputManager.getInstance` removed); upgrade the androidx.test/Espresso versions in a
  dedicated maintenance change and re-run the Compose UI suite on API 37. On API 35 (local
  `system-images;android-35;google_apis;x86_64`, Pixel 6 profile) 34 of 40 instrumented tests pass,
  including all five new crypto tests (the emulator has no StrongBox: the fallback path ran,
  security level SOFTWARE); six existing Compose UI tests (`AboutNavigationTest` 1,
  `CalendarScreenTest` 1, `NavigationSmokeTest` 1, `ScheduleEditorFlowTest` 3) fail identically on
  the unchanged HEAD (`2b9e5d5`) — duplicate pager nodes / not displayed — and belong to the same
  maintenance item.
- **Instrumented regression:** on the local API 37 emulator the four new instrumented tests pass;
  the 34 existing Compose UI tests fail before reaching app code because Espresso 3.6.1 calls the
  removed `InputManager.getInstance` on API 37 — a test-dependency issue unrelated to this change,
  not fixed here (no toolchain upgrade in this milestone).

## 13. Implementation notes (M1.10e.2a — Android BLE transport)

| Part | Implemented |
|---|---|
| `device/transport/ByteTransport.kt` | the transport contract: `generation`, `mtu`, `info`, `events` (notifications, then one `Closed`), `closeReason`, `writeFragment`, `close`; `CloseReason` (Local, Disconnected(status), BluetoothOff, PermissionRevoked, Timeout(step), GattFailure(step, status), NotADispenser, NotificationOverflow); `ConnectResult`; `TransportTimeouts` (connect 15 s, discovery 10 s, MTU 5 s, Info 5 s, subscribe 5 s, write 5 s) |
| `GattPort` + `DispenserGatt` | the thin port interface (non-blocking calls, callbacks into a per-port sink) and the normative UUIDs |
| `GattTransport` (pure Kotlin) | setup connect → high priority → discovery (service + Info read / Request write / Response notify + CCCD) → MTU 517 requested, reported value clamped to 23..517, 23 on refusal, failure or timeout → Info read and decoded (`NotADispenser` otherwise) → notifications enabled; exactly one GATT operation in flight; every step with a timeout; callbacks of any thread through one channel and one coroutine; per-port sinks, so callbacks of an earlier connection can never reach a newer one, and callbacks after close are dropped; unsolicited MTU changes used for the next frame; bounded notification buffer (128 unconsumed fragments, then `NotificationOverflow`); SecurityException → `PermissionRevoked`; link drop while Bluetooth is off → `BluetoothOff`; cancellation of a connect closes the attempt |
| `GattTransportFactory` | prerequisite check (`NotReady` without any attempt), process-wide generations, address format check (the address is a locator only) |
| `device/link/FrameLink` | fragmentation at the MTU in effect when a frame starts (`BleProtocol.fragment`), one fragment per write response, frames serialized; reassembly with the shared `Reassembler` (2 s gap); fragment errors reported as `FragmentError` (the session decides); one `Closed` |
| `AndroidGattPort`, `AndroidGattPortFactory`, `AndroidBluetoothPrerequisites` | the only `android.bluetooth` code: API 33+ and pre-33 signatures of read/write/notify/descriptor calls, `getRemoteLeDevice(…, ADDRESS_TYPE_PUBLIC)` on 33+, `TRANSPORT_LE`, `autoConnect = false`; readiness = BLE feature, adapter, `BLUETOOTH_CONNECT` on 31+, adapter enabled |

**Tests (JVM, deterministic `FakeGatt` + virtual time):** `GattTransportTest` (13: setup order and
values, MTU cases incl. refusal/failure/timeout/clamping/unsolicited change, every setup failure
releases the GATT client, not-ready attempts nothing, SecurityException at every step and during a
write, link drop vs Bluetooth off, stale callbacks of an older generation after reconnect, one write
in flight, write failure and timeout, disconnect during a write, notification overflow, duplicate
and unexpected callbacks, cancelled connect, 4 real threads × 2,000 notifications + concurrent
writes with no loss or duplication and one `Closed` — 5/5 repeated runs); `FrameLinkTest` (7: echo at
MTU 23/24/64/185/247/517 × boundary lengths up to 520, flow control fragment by fragment, concurrent
sends never interleave, every fragment error class + recovery, MTU change mid-frame, single
`Closed`, size limits); `AndroidBluetoothPrerequisitesTest` (Robolectric: feature, permission,
adapter state; the API 26–30 path via injected SDK level); `ArchitectureRulesTest` now also covers
the transport package. Mutation checks: no one-operation lock, no notification bound, no MTU clamp,
no Bluetooth-off mapping, unserialized frame sends, unhandled SecurityException — all caught.

**Review checks (2026-11-18):**
1. *Write start vs completion (API 33+):* `writeCharacteristic(…)` must return
   `BluetoothStatusCodes.SUCCESS` to start; the write completes only with `onCharacteristicWrite`
   status 0. A refused start → `GattFailure(WRITE, -1)`; a failing completion →
   `GattFailure(WRITE, status)`; both close. Tests: `write_start_failure_and_completion_failure_are_both_handled`
   (fake), `AndroidGattPortTest` (Robolectric: refused start returns false; completion reported only
   from the callback, including a failing status).
2. *Subscription = CCCD write:* `setCharacteristicNotification` (refusal → not started) **and** the
   CCCD write of `0x0001`, completed only by `onDescriptorWrite` status 0; no callback → timeout,
   failing status → `GattFailure(SUBSCRIBE, status)`. Tests: `subscription_requires_the_cccd_write_callback`,
   `AndroidGattPortTest.subscription_is_the_cccd_write_and_its_callback`.
3. *Negotiated MTU:* the value reported by `onMtuChanged` is used (517 requested, smaller agreed →
   that value); an unsolicited report (Android 14+ negotiating by itself) is kept even when the app's
   own request is refused or unanswered; fragments are cut at that MTU. Tests:
   `actually_negotiated_mtu_is_used`, `FrameLinkTest.fragments_follow_the_negotiated_mtu` (MTU 185,
   23, and an unsolicited 100).

**Not in M1.10e.2a / deviations:**
- `AndroidGattPort` is compiled and lint-checked but not run against a radio (no BLE peer on the
  emulator); its runtime behavior is part of M1.10e.5.
- CDM discovery (`DeviceDiscovery`) moves to M1.10e.2b/M1.10e.4 together with the session manager
  and onboarding, where an association is first needed.
- Manifest permissions (`BLUETOOTH_CONNECT`; `BLUETOOTH` with `maxSdkVersion 30`) and the
  `bluetooth_le` feature are added when BLE is wired into the app (M1.10e.4), so the installed app
  stays unchanged until then. `BLUETOOTH_ADMIN` is not needed for GATT connections; it would only be
  needed by the fallback scan (E8).

## 14. Implementation notes (M1.10e.2b — secure session and firmware-in-the-loop)

### 14.1 Implemented

| Part | Implemented |
|---|---|
| `device/session/SecureSession` | one session per connection: HELLO, then IK SESSION, XXpsk3 CLAIM / ADD / RECOVERY, or the confirmation of a staged enrollment (11.7) with IK + `PHONES_LIST` (`is_self`) → `confirmPending`; frames routed by `txn`; plain responses accepted only before authentication (HELLO, HS, the HANDSHAKE_FAILED answer to message 3); plaintext after authentication, an undecryptable or malformed encrypted frame, a fragment error or a device-originated request closes the session; encrypted requests one in flight, ≤ 8 waiting (`Busy`), retransmitted with the same `txn` and plaintext after a timeout, first response wins; results `Ok`, `DeviceError`, `NotSent`, `Busy` or `Uncertain` (14.3); a request whose every attempt timed out closes the session; events delivered (EV_SESSION_CLOSING, EV_SUSPENDING_FOR_DOSE, EV_CONFIRM_ON_DEVICE — the latter captured even if it arrives before the recovery step listens); `end()` = encrypted END_SESSION (best effort) + close; every close frees the native Noise state |
| Enrollment rules | HELLO checks before any handshake (CLAIM: unclaimed hash of the label's device ID; ADD: invitation's identity hash and device ID; RECOVERY: armed; SESSION: pinned identity hash); ADD aborts before message 3 when the message-2 key differs from the invitation's; the enrollment is **staged before message 3** and committed on ENROLL_RESULT; ENROLL_RESULT lost → `EnrollmentUnconfirmed` (staged kept); HANDSHAKE_FAILED to message 3 (unauthenticated) → staged kept until the user discards it |
| `DeviceSessionManager` | process-wide owner of the single live session (`AlreadyActive` otherwise), `StateFlow<SessionState>` (Idle, Connecting, Handshaking, ConfirmingOnDevice, Authenticated(generation, slot)), events of the live session only, `request`, `end(graceful)`; a closed session is never reused |
| Firmware-in-the-loop (E10) | `firmware/test/fwsim/` (test-only JNI library `libdsp_fwsim`): the unmodified `dsp_app`, `dsp_blesec`, `dsp_ble_transport`, domain and libsodium/noise-c, in the host harness world (fake storage, pump, clocks), the application task on its own thread, a simulated radio with per-connection event queues and virtual time; built by the Gradle task `buildFirmwareSim`; Android side `FirmwareSim` + `FirmwareGattPortFactory` (test source set) drive the app's real `GattTransport` → `FrameLink` → `SecureSession` |

### 14.2 Tests

`FirmwareInTheLoopTest` (11, against the real firmware logic): CLAIM → SESSION + GET_STATUS +
INVITE_CREATE → ADD with the invitation → RECOVERY with three presses (identity replaced, other phones
removed, the old phone's credential then fails at HELLO); authentication failures (no credential,
wrong setup code → HANDSHAKE_FAILED with the staged key kept and explicitly discarded, foreign
invitation, forged credential → HANDSHAKE_FAILED; nothing enrolled); lost ENROLL_RESULT + process
death → `ConfirmStaged` in a new manager over the same files; XX message 3 delivered but its write
reported failed → `EnrollmentUnconfirmed` (never a plain transport failure: the device may have
stored it), then `ConfirmStaged`; disconnect during a request
(`Uncertain`, old session refuses with `NotSent`, new generation and session ID); a delayed response
retransmitted and answered from the device's response cache (late duplicate discarded); concurrent
requests serialized; EV_SESSION_CLOSING and cleanup at the window maximum; and the 14.3 regressions:
- MANUAL_DOSE (never answered: BLE is suspended for the pump) → `Uncertain`; new session:
  `GET_RESULT{seq}` = DONE; re-sending the same `seq` → `DUPLICATE` (state DONE); one pump run;
- a MANUAL_DOSE request dropped before decryption (the frame is discarded before the firmware, the
  GATT write acknowledged — the device's receive nonce does not advance, as with a T3b drop) → the
  same-`txn` retransmission fails the device's decryption (nonce desynchronization), the device
  closes → `Uncertain` (2 attempts); new session: `GET_RESULT{seq}` = NOT_DECIDED, floor unchanged,
  no pump run; the **same `seq`** then runs exactly once;
- a REFILL response lost on an open link → the retransmission is answered from the cache under the
  device's next nonce, the phone fails to decrypt it and closes → `Uncertain`; never repeated; the
  new session reads the applied values back.
The harness pump oracle (a pump run without a durable decision, or a duplicate run of one manual
request) is asserted zero after every test.

`SecureSessionTest` (17, scripted IK/XX peer over the fake GATT): plaintext after authentication,
tampered ciphertext (no retry), fragment error, device-originated request, duplicate response, all
attempts unanswered → `Uncertain` + session closed, with one `txn` and byte-identical plaintext
(the original manual-dose `seq`) on every attempt and nothing sent afterwards; a single unanswered
attempt closes; an undecryptable response while waiting → `Uncertain`, never failure; the
`NotSent`/`Uncertain` boundary (failed write and refused write start → `Uncertain`; a request queued
when the session closes → `NotSent` while the sent one is `Uncertain`; cancellation after sending
closes the session); bounded
waiting (`Busy`), events, HELLO error / wrong device, ADD impostor with the invitation's hash but
another static key (abort before message 3, nothing staged). Mutation checks: the 7 security rules
of revision 6 plus 5 retry rules (no close after a timeout, an uncertain outcome reported as
`NotSent`, a retransmission with a new `seq`, a failed send reported as `NotSent`, a failed
message-3 write reported as a transport failure) — all caught.

### 14.3 Retries and uncertain outcomes (decision: option 1, approved)

Implicit sequential Noise nonces and the wire protocol stay unchanged (no explicit nonces). The
client rules, matching BLE_PROTOCOL.md 10.4a as clarified:

1. Same-`txn` retransmission within one session (identical plaintext, so the same manual-dose
   `seq`) may recover a delayed response; it cannot guarantee recovery from a lost encrypted
   message — a request the device dropped before decrypting it (T3b) or a response lost on an open
   link puts the nonces out of step.
2. A request timeout or connection failure never proves that the operation was not executed.
3. Nonce state inconsistent (a decryption failure on either side) or unknown (every attempt timed
   out): the session is closed; work continues only after a new handshake.
4. A request without a response is `RequestResult.Uncertain` — reported as "result unknown", never as
   failure or success — and reconciled against durable device state in a new session
   (`GET_RESULT{seq}`, revisions, Status). `NotSent` / `Busy` only while no attempt has entered
   `FrameLink.send` (no live authenticated session, queued behind a request when the session
   closed, or no transport keys / encryption failure on the first attempt). Once a send has
   started, every outcome without a response is `Uncertain` — including a refused or failed write
   (the session is closed) — and a caller cancelled after that point gets no result, so the
   session is closed ("request cancelled: outcome unknown"). When the session closed first (e.g. a
   decryption failure that also aborts a pending write), the result carries the session's close
   reason. The same rule holds for enrollment: once XX message 3 may have been sent, a transport
   failure is `EnrollmentUnconfirmed`.
5. Never automatically allocate a new manual-dose `seq` to retry an uncertain dose; re-sending the
   same `seq` is safe (decided exactly once).
6. Never automatically repeat REFILL after an uncertain outcome.
7. The device's response cache and the durable anti-replay rules (manual-dose `seq` floor,
   revisions, single-use invite tokens) are unchanged.

Nothing in the session layer retries across sessions; the typed operations, journal and repository
(M1.10e.3) apply rules 4–6 at the operation level (table 5.1).

### 14.4 Not in M1.10e.2b

CDM discovery and onboarding UI (M1.10e.4); typed operations, journal and
repository (M1.10e.3); manifest permissions (M1.10e.4). The firmware simulator is test code: it is
not built by the firmware CTest and never enters a firmware image or the APK.

---

## 15. Implementation notes (M1.10e.3 — device repository and operations)

Committed `f7b8c22` and accepted; post-acceptance review fixes in 15.5.

Decisions D1–D8 (review of M1.10e.3): D1, D2, D3, D4, D5, D7 and D8 approved as recommended with
the additional rules below; D6 modified.

### 15.1 Implemented

| Part | Implemented |
|---|---|
| `device/ops/DeviceRecords` | typed records for every v1 body (Status, Diagnostics, Config with field-mask bits, Schedule, list pages, phones, invite, manual-dose response, GET_RESULT state, SET_TIME decision) and the device error codes, converted from/to the existing `BleProtocol` field maps |
| `device/ops/DeviceOperations` | `SessionPort` / `LiveSession` (the repository's view of the M1.10e.2b manager; production `ManagerSessionPort`); `OpResult` = `Ok`, `Rejected(DeviceError)`, `Refused(LocalRefusal)`, `NotSent`, `Uncertain(journalId)`; every request runs in the manager scope (D3) |
| `data/device/DeviceRepository` + `DeviceDispenserRepository` (D1) | device-only contract with typed results for every v1 operation except WIFI_* (E14) and FACTORY_RESET (5.1): status, diagnostics, ACK_REJECTIONS, config get/set, schedules list/get/create/update/delete with read-back, manual dose + GET_RESULT, REFILL, readiness, SET_TIME, phones, phone removal, invites, recovery cancel; `connect()` = IK session, journal resolution before any new mutation, live status; not wired into the UI (DEMO and the screens unchanged; About / Check for Updates untouched) |
| Freshness (D2) | `Fresh.Live(value, readAt, generation)` only while the authenticated session of that generation is the live one; `Cached(value, readAt)` otherwise (also after a process death); `Unknown`; connection state is a separate flow; cached values never authorize a manual dose or REFILL (both re-read the status live) |
| Device validation (D2) | domain rules plus wire limits before sending: name ≤ 32 UTF-8 bytes without control characters, dates 2000–2099, µl ≤ u32, interval ≤ 2³¹−1, Europe/Prague only; records the domain model cannot represent are counted as unreadable, never guessed |
| `data/device/local/DeviceDatabase` | Room `device.db` (schema exported): `pending_ops` journal and `device_cache` (the device's own response bodies with their read time); excluded from backup/transfer by the existing catch-all rules; no destructive migration |
| Journal (11.4, table 5.1) | written before every journaled mutation (manual dose, REFILL, SET_CONFIG, schedule writes, SET_READINESS, PHONE_REMOVE); removed on a definitive answer; kept on `Uncertain` and on STORAGE; resolved by read-back at the next session: GET_RESULT (manual), Status (REFILL → `READ_BACK`, readiness), GET_CONFIG (masked fields), GET_SCHEDULE (`APPLIED` / `NOT_APPLIED` / `CHANGED_ELSEWHERE`), PHONES_LIST; duplicate intents and stacked doses refused while a row is unresolved |
| Manual dose (D4, E12) | live floor → durable `seq` → durable PREPARED row → send; NOT_DECIDED → `AWAITING_USER` (re-send with the same `seq` or discard, user actions only); PENDING stays unresolved; DONE / FAILED / REFUSED / INTERRUPTED / result-unavailable resolved and kept until acknowledged |
| REFILL (D5) | capacity from GET_STATUS in the same session immediately before; aborted (`SessionChanged`) if the session changed or closed; never re-sent automatically |
| SET_TIME (D6) | behind `PhoneTimeSource` (UTC + uncertainty); no source or an uncertainty outside 0..2,000 ms → `Refused(TimeUntrusted)`, nothing sent; never retransmitted (a resend would carry a stale time) |

### 15.2 Findings

- **A refused manual dose also arrives as an unknown outcome.** The firmware suspends BLE *before*
  it decides a fresh MANUAL_DOSE (10.4, Q8), so a LIMIT / REFUSED / NOT_COVERED decision on a new
  request is visible only through GET_RESULT at the next session (verified against the firmware).
  The app therefore reports every fresh dose as `Uncertain` and shows the decision after
  reconnecting; this matches the approved flow (7, 11.4) and needs no change.
- **Firmware simulator: a stale poller could take a later connection's events (test harness bug,
  fixed).** `libdsp_fwsim` tagged queued events only with the BLE connection handle (cycled 1–8),
  and a poller could drop another poller's disconnect event as stale, so the port of an ended
  connection kept polling; once the handle came round again it took the new connection's
  notifications (seen under CPU load as a HELLO or handshake timeout after a reconnect). The device
  and the client behaved correctly (device-side and phone-side traces). Real controllers **reuse**
  connection handles, so the fix does not depend on unique handles: every connection gets the same
  handle by default (0x0001; `setConnectionHandle` picks another, still reused), and the harness
  binds queued events, writes and phone-side disconnects to a separate, never-reused **link id**.
  A poller takes only its own link's events and gets `GONE` at once when its link is over, so a
  left-over poller always stops; unread events of an ended link are discarded, never handed to a
  later link; late writes and disconnects of an ended link's client are no-ops. Events are tagged in
  the same critical section as the connection check, and phone-side connect, writes and disconnects
  are serialized like the NimBLE host task, so a link check and its delivery are one step. The firmware sees only the reused handle, so
  its own stale-event guard (`test_ble_transport`, one reused handle) stays exercised: every FIL
  test now runs with handle reuse. `FirmwareSimLinkTest` (7) covers repeated cycles on one handle,
  undelivered events of the previous link, late GATT calls, and reconnection while the previous
  poller is still polling (raw pollers and the real `FirmwareGattPort`) — nothing crosses links,
  nothing is lost or duplicated. Not an Android or firmware issue: real GATT callbacks are bound to
  their own `BluetoothGatt` object and `GattTransport` already filters by generation.
- **The manager published `Idle` asynchronously after a close (fixed, D2).** The manager moved to
  `Idle` and dropped the session in a coroutine dispatched after `SecureSession.closed` changed, so
  for a short time after `close()`/`end()` returned (longer under CPU load) `state` still said
  `Authenticated` and the repository could label a value of the closed session `Live`. Mutations
  were never affected (`live()` and `request()` check `isOpen`). Found by the FIL stress run
  (`all_four_handshakes_over_one_device_lifetime`, 3 of 15 runs under 12 CPU hogs). Now
  synchronous (4.4).
- **Firmware simulator: data race on the virtual clock (test harness, fixed).** ThreadSanitizer
  (`firmware/test/fwsim/tsan/`, a test-only native driver linking `fwsim.c` directly with a minimal
  `JNIEnv`) reported the
  transport's `now_ms` read on a phone-write thread racing the application task's clock update.
  Host-task-role calls now read the clock copy the application task publishes under its lock;
  on the device the clock is the thread-safe hardware timer (no firmware change).
- **Submitted requests are not removed from the queue on caller cancellation** (11.6 said queued
  requests are removed): with D3 the manager owns a request once it is submitted, so its outcome and
  journal are always processed. Requests not yet submitted (still in the repository) are not sent.

### 15.3 Tests

- `DeviceRepositoryTest` (25, scripted device): freshness across disconnect, generations and process
  death; cached values never authorize; D4 order (seq, then row, then send) and every crash boundary
  (seq persistence failure → nothing sent; journal failure → nothing sent, seq burned and never
  reused; crash after seq → number skipped; crash after the row → NOT_DECIDED → re-send with the
  original seq, no new allocation; drop before decryption + process death → exactly one pump run);
  dose answers (response, LIMIT, STORAGE); D3 cancellation (outcome and journal processed after the
  caller gave up); D5 (live capacity, session change aborts, uncertain REFILL read back and never
  re-sent, second REFILL refused while unresolved); schedule writes with read-back, device limits,
  resolution APPLIED / NOT_APPLIED / CHANGED_ELSEWHERE; config, readiness, phones and their
  resolution; D6; every operation without a session `NotSent`; result mapping.
- `DeviceDatabaseTest` (3, Robolectric): journal and cache rows survive reopening `device.db`;
  a missing-row update fails loudly; a dose resolved by a new repository over the reopened file.
- `DeviceRepositoryFirmwareTest` (10, real firmware logic): every operation with read-back and device
  refusals (CONFLICT, LIMIT, REFUSED detail 1 while READY, local refusal above the live capacity);
  SET_TIME untrusted never sent; dose → Uncertain → DONE at the next session; dose dropped before
  decryption → NOT_DECIDED → re-send with the same seq → exactly one pump run; process death
  between dose and resolution; **power cut during the pump → INTERRUPTED** (fwsim `armPumpPowerCut`,
  the harness cut point plus a simulated reboot); lost REFILL response → READ_BACK, never re-sent;
  lost schedule-create response → APPLIED; dose above the per-dose limit → REFUSED at the next
  session, no pump run. The harness pump oracle is asserted zero after every test.
- `SecureSessionTest` D2 (6): the manager is `Idle` with no session the moment `end()` / `close()`
  / a violation close returns (no scheduler step); 8 concurrent closers × 20 rounds each see the
  session invalidated when their own `close()` returns; a late close of an older session leaves a
  newer session `Authenticated` and usable; a listener on an already-closed session runs at once.
  `DeviceRepositoryFirmwareTest.values_are_never_live_after_the_session_closes` (real manager):
  after a session close, a manager disconnect and a repository disconnect, a new reader gets
  `Cached` at once and an existing reader receives `Cached`. Mutations (no synchronous
  invalidation, only the first close invalidates, unconditional invalidation, no run on
  registration) are each caught.
- Simulator lifecycle: `FirmwareSimLinkTest` (7, see 15.2); ThreadSanitizer driver
  `firmware/test/fwsim/tsan/` (`run.sh`, see its README) over the same races (one reused handle, left-over poller, late writer/disconnector, injector, malformed
  writes into the firmware transport racing the phone disconnect): 3 × 200 rounds, 0 reports;
  `test_ble_transport` and `test_platform` under TSan: 0 reports.
- Mutation checks: 9 safety rules removed one at a time (journal failure ignored, seq not persisted,
  re-send with a new seq, cached capacity for REFILL, no session-change check, Live without the
  generation, caller-owned lifecycle, unbounded time uncertainty, REFILL re-sent at resolution) —
  all caught (three needed the redundant guard removed too, which is defense in depth).

### 15.4 Open items

- **Production `PhoneTimeSource`** (D6, tracked separately): Android gives no accuracy guarantee for
  network time or AUTO_TIME, so the app ships `UnavailablePhoneTimeSource` (SET_TIME is refused
  locally) until a source with a defensible uncertainty estimate is designed and reviewed. The
  device keeps its own time rules (NTP preferred, holds); dosing gates that need trusted time stay
  in force.
- UI adaptation to `DispenserRepository`, CDM address, onboarding: M1.10e.4.

### 15.5 Post-acceptance review fixes (independent review of M1.10e.3)

**Finding 2 — concurrent mutations could bypass the pending-operation guard (genuine; fixed).**
`unresolved(kind, target)` and the insert of the caller's own journal row were separate suspending
steps; the session layer serializes single requests (`sendLock`) but does not make the guard and the
insert atomic. Demonstrated deterministically before the fix (`DeviceRepositoryConcurrencyTest`, a
journal that parks the first caller between its guard read and its insert): two concurrent manual
doses both reached the scripted device (**2 pump runs**, on the path where the device answers DONE and
keeps the link), two REFILLs were both applied, two SET_CONFIG and two writes of the same schedule were
both sent (the device's revision check refused the second). On the real firmware the normal dose path
closes the link after the first dose, which hides — but does not remove — the gap.
Fix (operation-level, no journal-semantics change): `DeviceDispenserRepository.reserved(key, guard)`
performs the journal guard and reserves the operation key atomically under one mutex and holds the
reservation until the operation has settled; a concurrent caller of the same operation is refused at
once with the new `LocalRefusal.InProgress` (11.6: duplicate intents are rejected, not queued). Keys:
`MANUAL_DOSE` (new dose, re-send with the original seq, discard — so a discard can never race a
re-send), `REFILL`, `SET_CONFIG`, `SET_READINESS`, `PHONE_REMOVE:<slot>`, `SCHEDULE:<id>` (create,
update, delete). Other operations and all reads are not blocked. A cancelled caller releases the
reservation, but by then its row is already inserted (or nothing was sent), so the journal guard still
holds. The shared screens map `InProgress` like `Unresolved` ("still being processed or awaiting
verification; nothing was sent").

**Finding 1 — incomplete journal resolution (no unsafe mutation; reporting fixed).** Verified:
unfinished rows stay PREPARED/AWAITING_USER and keep blocking their own operation (every journaled
mutation checks the journal under its reservation; a journal that cannot be read refuses every
mutation with `PersistenceFailed`); no row is ever dropped or re-sent by resolution; reads stay
available in the authenticated session. Gap: `connect()` returned `Connected` without saying the
resolution was unfinished and never retried it within the same session. Fix:
`ConnectOutcome.Connected.resolutionComplete` (true only when no PREPARED row is left), and the
session is marked resolved only then, so the next `connect()` retries. Cross-kind mutations stay
allowed while another kind is unresolved, as approved (11.4.5, per operation and target).

Tests: `DeviceRepositoryConcurrencyTest` (10: concurrent manual doses, refills, SET_CONFIG and writes
of the same schedule each send exactly one request; other kinds and reads are not blocked; the
reservation is released after Rejected and NotSent; a resolution that cannot write, an unreadable
journal, a lost GET_RESULT and a refused read-back each report `resolutionComplete = false`, keep the
guard, allow reads, and are retried by the next `connect()`; nothing re-issued);
`DeviceRepositoryConcurrencyFirmwareTest` (1, real firmware: a second REFILL and a second manual dose
arriving while the first is between guard and insert are refused; one REFILL frame, one MANUAL_DOSE
frame, exactly one pump run, pump oracle clean); `DeviceUiRepositoryTest` (+InProgress mapping).
7 of the first 9 concurrency tests failed before the fix (the 10th was added for a surviving mutant). Mutation checks: no in-flight check (2 failures),

**Review of `4d7e3b2` — cancellation (verified, no change needed).** When the caller is cancelled
after its operation inserted the journal row and started sending, the reservation is released, the
manager-owned request continues (`DeviceOperations` runs it in the manager scope) and settles the row
by its own outcome; meanwhile the journal guard refuses the same operation without sending anything
(not even the status read or a seq allocation for a dose). Tests (`DeviceRepositoryConcurrencyTest`,
+4): REFILL answered (row removed, then a new REFILL allowed), REFILL with a lost answer (row kept
PREPARED, the next session's read-back resolves it, then allowed; never re-sent), MANUAL_DOSE with the
device closing the link (one pump run, PREPARED until GET_RESULT says DONE at the next session, then a
new dose with a fresh seq), MANUAL_DOSE answered in the same session (row removed, then allowed).
Removing the REFILL or MANUAL_DOSE journal guard makes these tests fail.

**Review of `4d7e3b2` — repository instances (invariant demonstrated).** The reservations are
instance-local; they suffice because there is exactly one `DeviceDispenserRepository` per device in
the app: one process (no component declares another), one `AppContainer` per process
(`DispenserApplication`), one repository per device inside it (`AppContainer` builds it only through `PerKeyRegistry`, created
at most once per device and reused across mode changes and session replacement — the repository
follows the manager's sessions), and no other production code constructs it.
`DeviceRepositoryInstanceTest` (5) pins all of this (including 8 threads racing the first access, 20
rounds). It does not build the device stack: the native provider must not be loaded inside
Robolectric's sandbox class loader, which would make the plain JVM native tests in the same worker fail. Journal checks alone would **not** be enough across
instances (the same check-then-insert gap); if a second instance per device is ever introduced, the
reservation registry must become process-wide (keyed by device and operation) or the journal needs an
atomic constraint — to be reviewed then.
no reservation (2), no retry of an unfinished resolution (1), "complete" despite PREPARED rows (1).

## 16. Implementation notes (M1.10e.4a — app mode and data plumbing)

Committed `e7564da` and accepted. Plan approved with conditions (review of the M1.10e.4 plan,
decisions 1–8): one review gate per step (a: architecture, b: onboarding/security, c: QR/secret
handling, d: final acceptance); nothing is committed before its review. Step a adds **no user-reachable behavior**: DEVICE mode can only be
entered with a confirmed enrollment, and enrollment arrives in step b. DEMO, the visual design, About,
Check for Updates, the changelog and the licences are unchanged.

### 16.1 Implemented

| Part | Implemented |
|---|---|
| `data/mode/AppMode` | `AppMode {DEMO, DEVICE}` + selected device ID in their own DataStore file `app_mode` (not in the JSON backup, not touched by a restore, excluded from system backup like every file); unknown or malformed stored values read as DEMO. DEVICE can only be **selected** with a CONFIRMED credential record (`AppModeController.enterDevice`, `CredentialStore.hasConfirmedRecord`; a staged enrollment, `.pending` 11.7, is not an enrollment). `AppModeResolver` (review of 4a): once selected, DEVICE is restored after every restart — also while disconnected and also when the credential has since become unusable; DEMO is never substituted silently (that would hide cached values and unresolved operations). `enterDemo` keeps the device selection. Mode changes never delete credentials, staged enrollments, the device cache or the journal. |
| `AppContainer` / `MainActivity` | `UiSources(mode, repository, simulation?, lowStockPolicy)` built for exactly one resolved mode; the UI renders nothing before the mode is resolved and is rebuilt (`key(mode)`: navigation, ViewModels, saved state) on a mode change, so no screen can mix DEMO and DEVICE data. The device stack (native Noise, Keystore credential store, `device.db`, session manager, `AndroidGattPortFactory`) is created lazily, only when DEVICE mode is shown. CDM address: `null` until step b (`connect()` → `NoAddress`). `PhoneTimeSource`: `UnavailablePhoneTimeSource` (SET_TIME disabled, D6). |
| Recovery-required state (review of 4a) | `DeviceUiRepository.credentialAccess` (container: once per device, off the main thread) loads and at once zeroizes the credential: `OK`, `KEY_MISSING` (Keystore key gone / invalidated), `RECORD_UNREADABLE` (tampered, truncated, unknown format; also any unexpected Keystore error — fail closed), `RECORD_MISSING` (confirmed record gone). Recovery required → connection `RECOVERY_REQUIRED` (chip "Vyžaduje obnovu"), every mutation refused locally (`NotSent`, nothing journaled or sent), cached values with their time and the unresolved-operation count stay visible (Settings explains the cause; "Nevyřešené operace: N … nic se automaticky neopakuje"). No session can authenticate anyway (`NoCredential` / `CredentialLost` / `CredentialUnreadable` after HELLO), so journal resolution never runs and nothing is re-issued; the journal is never touched by a mode change or by the credential state. After re-enrollment (step b) the rows are resolved by read-back at the next session (11.4). |
| `data/device/DeviceUiRepository` (decision 1) | adapter `DispenserRepository` ← `DeviceRepository` for the shared screens: `DeviceStatus` with `source = DEVICE`, connection per device (CONNECTED only for an authenticated session of the selected device; CONNECTING while connecting/handshaking/confirming), battery and the device's own reservoir estimate with `Freshness` (Live only while that session is live; Cached with the read time; Unknown); schedules = the device's set as read back (`DEVICE_APPLIED`), with their freshness; no history (`historyAvailable = false`); `writesRequireConnection = true`. Outcomes stay explicit (below). |
| Contract additions (`DispenserRepository`, `CommandResult`) | default members (DEMO implementation unchanged): `observeScheduleFreshness()`, `historyAvailable`, `writesRequireConnection`; DEVICE-only result variants `ScheduleWriteResult.{NotSent, Uncertain, AwaitingVerification, DeviceRejected, DeviceLimit}` and `CommandResult.Device.{NotSent, Uncertain, AwaitingVerification, DeviceRejected, NotAccepted}`; `DeviceStatus.freshness`, `ConnectionState.CONNECTING`, domain `Freshness`. |
| Shared screens in DEVICE mode | header: connection chip (same shape and place as the DEMO badge) instead of the badge; dashboard/reservoir/settings: no "simulace" tag, "živě" or "uloženo HH:MM" (with the date when not today) on device values; unknown estimate says "not read from the device"; calendar: "Rozvrhy ze zařízení · <freshness>", create/toggle disabled while disconnected; editor: Save/Delete disabled while disconnected ("Uložení vyžaduje připojené zařízení"); reservoir: refill disabled while disconnected; history: "not available from the device in protocol v1"; settings: mode "Zařízení (Bluetooth)", no Simulace section, device connection row. |

### 16.2 Outcome mapping (condition 1)

| Device outcome | Shared-screen result | UI |
|---|---|---|
| `Ok` | `Success` (schedule = the device's read-back record) / `Done` | editor closes "Uloženo a potvrzeno zařízením"; refill "Doplnění potvrzeno zařízením" |
| `Uncertain` | `Uncertain` | **not a failure**: editor closes with "verified at the next connection"; refill/toggle show the same in warning colour, "neopakujte"; never mapped to Conflict, NotFound or NotSent, so nothing invites a retry |
| `NotSent`; `Refused(SessionChanged / PersistenceFailed / …)` | `NotSent` | nothing changed, the edit stays open (a retry is safe: provably not sent) |
| `Refused(Unresolved)` | `AwaitingVerification` | earlier change awaits read-back; nothing sent |
| `Refused(Invalid)` | `DeviceLimit` / `NotAccepted` | device limits; nothing sent |
| `Rejected(CONFLICT / NOT_FOUND)` | `Conflict` / `NotFound` | existing conflict / deleted-meanwhile banners |
| `Rejected(other)` | `DeviceRejected(code)` | device refused; nothing changed |

### 16.3 Tests

- `DeviceUiRepositoryTest` (10, scripted device): recovery required for each unusable credential
  state (even with a stale session: never CONNECTED; unresolved rows counted, resolved rows not; every
  mutation NotSent without reaching the repository); Unknown → nothing invented; Live only while
  connected, Cached keeps its read time; connection per device; unusable capacity → no estimate;
  schedules device-confirmed with freshness; no history; every write and refill outcome above.
- `AppModeTest` (3): selection needs a confirmed record (staged ≠ confirmed); DEVICE restored after a
  restart without a session; enter/leave DEVICE never deletes the confirmed or the staged record; back
  to DEVICE without a new enrollment; a vanished record keeps DEVICE (recovery required), but DEVICE
  cannot be selected again without a confirmed record; malformed values.
- `DeviceModeViewModelsTest` (7): dashboard, history, settings, editor (online-only; NotSent and
  AwaitingVerification keep the editor open; Uncertain closes it, no conflict), confirmed save,
  reservoir (online-only; Uncertain is neither success nor failure), calendar toggle outcomes.
- `DeviceUiRepositoryFirmwareTest` (2, real firmware): (new) with an unresolved manual dose journaled,
  each credential loss (Keystore key deleted, record corrupted, record deleted) after a process restart
  → recovery required, unresolved count 1, cached values, writes NotSent, `connect()` fails with the
  matching failure, zero GET_RESULT / MANUAL_DOSE / REFILL / CREATE_SCHEDULE frames, the row kept;
  credential restored → GET_RESULT at the next session, MANUAL_DOSE never re-issued, row kept. And: live values and CONNECTED after connect;
  refill and schedule creation confirmed by the device and shown as `DEVICE_APPLIED`; after
  disconnect the same values Cached, writes `NotSent` with zero frames sent; after a process restart
  cached values only.
- `ModeShellTest` (3, Robolectric Compose; recovery-required shell: chip, cached values, disabled
  schedule creation, Settings cause and unresolved count; test-only dependency `ui-test-junit4` added to
  `testImplementation`): DEMO keeps the badge and the Simulace section; DEVICE shows the chip, no
  badge, no "simulace" anywhere, "uloženo 10:30" on the cached device values, disabled schedule
  creation, history unavailable, no Simulace section.
- `AppContainerTest`: a fresh install resolves to DEMO with the simulation.
- `SystemBackupPolicyTest` (+1): the mode DataStore (files domain), `device.db` (databases) and the
  credential directory (no-backup, under root) are in domains excluded from cloud Auto Backup, device
  transfer and the legacy rules (`allowBackup=false` as before); the JSON backup code references none
  of them.
- Mutation checks (each must fail a test): Uncertain write → Conflict (3 failures), Uncertain refill
  → NotSent (2), Cached shown as Live (8), DEVICE without a confirmed record (2), editor treating
  Uncertain as retryable (1), edits offered offline (5), another device's session counted as
  connected (1); review of 4a: missing record falls back to DEMO (2), mutations not blocked in recovery
  (1), recovery not shown (3), unresolved count hidden (3), lost key reported as OK (1).
- DEMO regression: the existing unit and instrumented tests are unchanged.

### 16.4 Notes for the review

- Backup / restore in DEVICE mode: the Backup screen stays available and still covers the phone-local
  DEMO data only (11.2); a restore never touches the mode, credentials, journal or device cache.
- The dashboard's low-stock tone uses the default `LowStockPolicy` in both modes (unchanged); the
  device's own thresholds, gates and readiness are shown by the device status screen (step d).
- `CredentialStore.hasConfirmedRecord` (file check, also as a static for startup) is the only change
  in `crypto/`.

## 17. Implementation notes (M1.10e.4b — onboarding and device administration)

Implemented and tested, uncommitted, awaiting the onboarding/security review. QR scanning and
invitations (ADD) are M1.10e.4c; connect/disconnect, device operations and the background grace period
are M1.10e.4d.

### 17.1 Implemented

| Part | Implemented |
|---|---|
| Manifest (4.1) | `BLUETOOTH_CONNECT` (runtime, API 31+); `BLUETOOTH`, `BLUETOOTH_ADMIN` with `maxSdkVersion="30"`; `uses-feature` `bluetooth_le` and `companion_device_setup`, both `required="false"` (DEMO installs everywhere). **Not added (decision for review):** `BLUETOOTH_SCAN neverForLocation` and `ACCESS_FINE_LOCATION maxSdk 30` belong to the fallback scan of 4.1, which is not implemented; they are added with it (if M1.10e.5 shows direct connect is unreliable), so no unused location-class permission ships. |
| `device/transport/DeviceAssociator` | Companion Device Manager association, single device, `BluetoothLeDeviceFilter` on the Dispenser service UUID; API 33+ `associate(request, executor, callback)` → `onAssociationPending`; API 26–32 `onDeviceFound`; the result Intent → upper-case public address (`EXTRA_ASSOCIATION` / `EXTRA_DEVICE` ScanResult or BluetoothDevice); `disassociate` for "Forget device"; `Unsupported` without the CDM feature. Untested on a real phone until M1.10e.5 (E17). |
| Address storage | `DeviceAddressStore` in the mode DataStore file (`address_<device-id>`; excluded from every backup); the repository's locator reads it (`ConnectOutcome.NoAddress` before). A locator only. |
| `data/onboarding/OnboardingController` | readiness (Bluetooth off / permission / unsupported); setup-label entry (lenient manual parsing of 7.1, typed problems: format, device ID, code, check); association with an early "another device" check (the address is the factory MAC = device ID; a match grants nothing); CLAIM and RECOVERY (`OpenRequest.Claim/Recovery`) with UI phases (connecting, handshake, "press three times, n s"); staged enrollments: listed from file names (`CredentialStore.stagedDevices`), confirmed with IK (`ConfirmStaged`, using the stored address) or discarded **only** on an explicit user decision; a new CLAIM/RECOVERY is refused (`STAGED_PENDING`) while a staged key of that device exists, so a staged key is never overwritten; failures report whether a staged key was left (a plain HANDSHAKE_FAILED to message 3 is unauthenticated, so the key stays: 11.7); after a confirmed enrollment: journal resolution and a live status read on that session, then the switch to DEVICE mode (`enterDevice`, refused without a confirmed record). |
| Secret handling (E4, 7.1) | the setup code exists only in the parsed `SetupLabel` (SecretBytes) inside the controller for the flow; zeroized after a confirmed enrollment, on a new label, on cancel and when the onboarding ViewModel is cleared; the typed text lives only in non-saveable Compose state (never in the saved instance state) and is cleared when handed over; password-type keyboard without autocorrect (no IME learning); the onboarding window is `FLAG_SECURE` while shown; nothing is logged, persisted, put in an Intent or on the clipboard. The ViewModel holds no secret. |
| `data/onboarding/ForgetDevice` (condition 4) | local only: ends a live session, deletes this phone's confirmed and staged credential records with their Keystore keys, the stored address and the CDM association, returns to DEMO with no device selected. **Does not revoke** the phone on the device (it keeps the slot until another phone removes it or a recovery replaces all phones) — the confirmation dialog says so. The journal and the device cache are kept: unresolved operations are never dropped silently and resolve again if this phone is enrolled with the device again; nothing is re-sent. |
| UI | Settings → Zařízení: DEMO: "Připojit zařízení" (onboarding), "Obnovit přístup k zařízení" (recovery), "Přepnout na zařízení" when a confirmed device is selected, "not supported" without Bluetooth LE; DEVICE: "Obnovit přístup" in the recovery-required state, "Přepnout do ukázkového režimu", "Zapomenout zařízení" with the explanation. Onboarding route (full screen, existing design): intro, readiness (permission request / enable Bluetooth), label, association (system dialog), phone name (1–24 UTF-8 bytes), progress, done (→ DEVICE mode), failure with retry, "Spárování nebylo potvrzeno" (verify / discard with a confirmation). Czech strings. DEMO never asks for a permission (only the onboarding screen can). |

### 17.2 Tests

- `OnboardingFirmwareTest` (9, real firmware): CLAIM → enrolled, address stored, journal/status hook,
  label zeroized (a second enroll has nothing to use), DEVICE mode; tolerant but checked label entry;
  another device picked → reported before connecting, nothing stored; wrong code → HANDSHAKE with the
  staged key kept, a new CLAIM refused (STAGED_PENDING), confirmation reports NOTHING_STAGED, explicit
  discard, then the right code works; a claimed device refuses another phone (ALREADY_CLAIMED); lost
  ENROLL_RESULT → Unconfirmed, DEVICE mode refused, confirmed after a process restart with the stored
  address; explicit discard of a staged key; RECOVERY refused without the boot hold, then with the hold
  the UI phase asks for the presses and the phone owns the device alone; "Forget device" deletes the
  credential, address and association, returns to DEMO, keeps the journal, and the device still holds
  the slot.
- `OnboardingViewModelTest` (2, real firmware): the whole flow from label to DEVICE mode; a staged
  enrollment is shown first on a new screen; clearing the ViewModel zeroizes the label.
- `OnboardingUiTest` (10, Robolectric): FLAG_SECURE only while shown; the typed code is not restored
  from saved instance state; the staged-key failure text; Settings pairing, recovery, switch back,
  no pairing without BLE, "Forget device" explains non-revocation and unresolved operations; merged
  manifest: BLUETOOTH_CONNECT, no location, scan or camera permission, features not required; legacy
  permissions end at API 30; only the onboarding screen requests runtime permissions.
- `FullScreenRouteTest`: the onboarding route. Architecture rules unchanged (the UI imports no
  crypto, session or transport types).
- Mutation checks (each fails a test): staged key overwritable, another device accepted, label kept
  after the enrollment, label not zeroized on close, label in saved state, no FLAG_SECURE.

### 17.3 Security review fixes (F1–F3) and the orphan-key finding

**F1 — session termination.** "Forget device" runs its whole cleanup `NonCancellable` and blocks new
sessions for the device first (`DeviceSessionManager.block`: `open()` refuses with
`SessionFailure.Blocked`; an open already in progress is closed instead of adopted). The graceful
END_SESSION is bounded (3 s) and may throw, time out or be cancelled; the hard close
(`DeviceSessionManager.closeNow`, synchronous, non-throwing) always follows in a `finally`; nothing is
deleted unless the manager then exposes no authenticated session.

**F2 — partial failures.** `ForgetDevice` returns `Forgotten(associationRemoved)` or
`Failed(SESSION | CREDENTIALS | ADDRESS | MODE)` and never throws. `CredentialStore.removeVerified`
deletes and verifies per record the Keystore key FIRST (`hasKey` afterwards), then the file (deleted and
`exists()` false): a key that cannot be removed leaves the record file (so a retry still knows the key's
random alias); a file that cannot be removed has no key any more (the record is unusable, shown as
recovery required). DEMO is selected only after the session is closed, both records and keys are
verified gone, the address is removed and a final check finds no record again; otherwise DEVICE mode
stays (recovery required if the credential is already gone) and Settings shows the failure ("…zkuste to
znovu"). The CDM association is a locator, not a secret: its removal failure does not block DEMO but is
reported ("odeberte ho v nastavení systému"). Every step is idempotent, so a retry after any partial
failure completes. The journal and the cache are kept.

**F4 — an enrollment in flight vs Forget (confirmed, fixed).** Before the fix, an `open()` already in
progress (e.g. a CLAIM still waiting for HELLO, before its key existed) ran independently of Forget:
Forget could finish its deletion, final check and the switch to DEMO, and the enrollment then created
a new key, staged and committed a usable credential and — Forget having unblocked — was adopted.
Fix (smallest change, in `DeviceSessionManager`): Forget runs its whole cleanup inside
`forgetting(deviceId) { … }`, which blocks the device, increments a per-device forget epoch and holds the
manager's `openLock` — the lock every `open()` holds for its whole duration. So an open in progress is
waited for (it is bounded by its own timeouts) and its credential, if it committed one, is deleted by
the cleanup that follows; such an open is never adopted (blocked, or its start epoch changed); a new
open of the device during Forget is refused at once (`Blocked`, never queued behind Forget); an open
that started just before Forget but reached the lock after it is refused by the epoch check. No
credential can be created or restored for the device while the cleanup runs; the final record check
is now defense in depth only. Cancellation-safe: the cleanup is NonCancellable; waiting for the lock
cannot be interrupted. No deadlock: the cleanup never calls `open()`; ending and closing sessions do not
take the lock. Session calls are device-scoped (`endFor`, `closeNow(deviceId)`,
`hasLiveSession(deviceId)`): Forget never closes another device's session. Overlapping Forget runs for the same device (review of `78f5b97`): the block is a per-device count of
runs in progress, removed only when the last run ends (a plain set let the first run unblock the device
while the second still waited for or held the lock, so a new open queued behind it instead of being
refused); `OnboardingFirmwareTest.overlapping_forgets_keep_the_device_blocked_until_the_last_one_ends`
failed with the set and passes with the count.

**F3 — credential health.** `CredentialHealth` (one per device) is refreshed after a successful
enrollment or recovery and after every "Forget device" attempt; the same DEVICE UI source leaves the
recovery-required state without a restart.

**Orphan Keystore keys (finding, not changed).** `CredentialStore.beginEnrollment` creates a Keystore key
with a random alias (`dsp_cred_v1_<id>_<random>`) before the handshake; the key is referenced on disk
only once `stage` writes the pending record (before XX message 3). A process death in between leaves a
key that no record names. No cleanup exists: `delete` / `removeVerified` / `discardPending` remove only
keys named by a record, and `KeyWrapper` cannot enumerate aliases. Impact: none on secrecy — the key
is non-exportable and has wrapped nothing persisted (the phone key pair existed only in memory); it is
Keystore clutter only. A safe cleanup would need alias enumeration (`KeyStore.aliases()` filtered by the
`dsp_cred_v1_` prefix, minus every alias named by a confirmed or staged record, never during an
enrollment in progress) — a credential-storage change left for review.

Tests: `ForgetDeviceTest` (14: graceful end throwing, cancelled, hanging — always a hard close, blocked
meanwhile; a cancelled caller does not interrupt the cleanup; a session that cannot be closed stops
before anything is deleted; Keystore deletion throwing or silently keeping the key; file deletion
failing; address-store and mode-store failures; CDM disassociation returning false or throwing; retries
after every partial failure; forgetting twice; verified removal reports an undeletable file);
`OnboardingFirmwareTest` (+2, real firmware: no session can be opened for the device while it is
forgotten; recovery in the same process leaves the recovery-required state); `OnboardingUiTest` (+1:
the failure notice in Settings). F4: `OnboardingFirmwareTest` (+3, real firmware, the interleaving
forced by holding the device's HELLO answer: Forget waits for the in-flight CLAIM, which is refused and
never adopted; after a successful Forget no record, no key, no session, DEMO; after a failed Forget
(Keystore failure) DEVICE mode, no session, and a retry completes; Forget never closes another device's
session); mutation checks: cleanup without the open lock (2 failures), a new open queued behind Forget
(1), an in-flight open adopted (2), closing any device's session (1), Forget not exclusive (4). The
epoch check for an open that reached the lock after Forget is not forced by a test (it needs the
open to pass its pre-lock check and lose the lock race to Forget); it is defense in depth over the
blocked checks. Mutation checks (each fails a test): no hard close (13 failures),
cancellable cleanup (1), no live-session check (1), file deleted before the key (3), key removal
unchecked (1), file removal unchecked (1, with the new direct test; the final record check also catches
it), no session block (1), no health refresh after an enrollment (1).

### 17.4 Manual / M1.10e.5 items

The CDM system dialog and its result on real phones (API 26–32 and 33+), permission denial and
"don't ask again", Bluetooth enable, vendor CDM behaviour with Location Services off, and
`neverForLocation` (with the fallback scan, if added) are verified on hardware in M1.10e.5; the
instrumented tests compile in CI and are not run there (E17).

---

## Appendix A — prerequisites for the ESP32-C3 bring-up (do not flash or configure yet)

Hardware and host:

1. Seeed XIAO ESP32-C3 with its **external U.FL antenna attached** before any RF test; USB-C
   **data** cable; no pump, motor or GPIO wiring (simulated pump only, button GPIO −1).
2. Linux serial access to `/dev/ttyACM*` (user in `dialout` or a udev rule) — verify without
   writing anything.
3. ESP-IDF v6.1 environment as used for the cross-builds (activation via the EIM script; the
   install stays untouched).
4. Android test phone(s): one Android 12+ (and ideally one Android 8–11) with developer options
   and USB debugging; a generic BLE inspector app (e.g. nRF Connect) for H1/H2 checks — the
   Dispenser app has no BLE yet, but HELLO is plain and can be written by hand.
5. Optional: USB power meter or a current profiler for H8.

Approved for preparation (review 2026-11-18) — no image is flashed until its configuration and
safety implications are reviewed:

6. **USB identification and serial diagnostics:** `lsusb`, kernel log, `/dev/ttyACM*` permissions;
   confirm the board is an ESP32-C3 (XIAO) before any esptool command.
7. **Read-only chip and eFuse inspection** after the target is confirmed: `esptool chip-id`,
   `read-mac`, `flash-id`, `espefuse summary` (read-only). Record secure boot / flash encryption
   status and flash size (expected 4 MB for the committed partition table). **No eFuse write is
   authorized.**
8. **Minimal RNG-gate image (prepare, do not flash; proposal in Appendix B):** a dedicated diagnostic image that does
   **not** enable the Noise service or any privileged BLE operation (`CONFIG_DSP_BLE_SECURITY`,
   `CONFIG_DSP_BLE_TRANSPORT`, the runtime and the bring-up window all off). It enables the BT
   controller (RF on) only to make the hardware RNG entropy source active and provides:
   R1/R2-style evidence (entropy source state and RNG adapter routing via a diagnostic log), the R5
   sample export (≥ 1 MiB from `esp_fill_random` with RF on, streamed over serial for off-device
   analysis with `ent`/dieharder/NIST STS) and, optionally, the R6 PSK vectors through
   `dsp_psk_derive` (derivation only, no handshake). This tooling does not exist yet and is
   firmware work outside M1.10e; its configuration is submitted for review before any flashing.
9. **Gate sequence** (`firmware/HARDWARE_ACCEPTANCE.md` 8a): (1) RNG-gate image — R1, R2, R3 and
   R5 must pass; (2) only then a controlled, bench-only security-enabled BLE bring-up (bring-up
   window, development setup code), in which R4 is executed; (3) security-enabled firmware is
   accepted for normal use only after R4 passes. Setup-code provisioning into `nvs_sec` needs a
   separate approval; no eFuse change is authorized.

10. Record sheet: `firmware/HARDWARE_ACCEPTANCE.md` sections 1, 8a (R1–R8) and 8b (H1–H10);
    logs must not contain secrets (setup code, keys).
11. Known limits for tomorrow: no button (no presence wake except the T7 power-on window), so
    ADD/RECOVERY presence and the three-press confirmation cannot be tested; the Android app
    cannot run CLAIM/SESSION until M1.10e.2 exists (a host peer over BLE would be needed for R4).

## Appendix B — RNG-gate diagnostic image (implemented and cross-built as proposed; not flashed — see firmware/HARDWARE_ACCEPTANCE.md 8a.1)

Purpose: obtain the M1.10c.2b RNG evidence (R1, R2, R3, R5; optionally R6) on the ESP32-C3 without
enabling the Noise service, any privileged BLE operation or any advertising. Per the review, such an
image is not a deployment of security-enabled firmware.

**Configuration (new, off by default):** `CONFIG_DSP_RNG_GATE_DIAGNOSTIC` in
`main/Kconfig.projbuild`, `depends on BT_ENABLED && !DSP_APP_RUNTIME && !DSP_BLE_SECURITY &&
!DSP_BLE_TRANSPORT && !DSP_APP_CONNECTIVITY`; refused with `CONFIG_DSP_APP_ALLOW_PRODUCTION` by the
existing CMake guard. Fragment `sdkconfig.rnggate.defaults`: BT controller only
(`CONFIG_BT_CONTROLLER_ONLY=y`, no NimBLE host, so no GATT and no advertising can exist),
`CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE=n` (no PHY calibration data written to NVS), Wi-Fi not
started, all other diagnostics off.

**Behavior (`app_main` diagnostic path, one run per boot, then idle):**

1. R1: `dsp_esp_rng_register()`; print `dsp_esp_rng_name()` and
   `dsp_esp_rng_registered_before_init()`.
2. R3 (pre): controller disabled → print `dsp_esp_rng_ready()` (must be false); no libsodium call
   is made in this state.
3. `esp_bt_controller_init` + `esp_bt_controller_enable(ESP_BT_MODE_BLE)` (RF on, no host, no
   advertising, no connection possible); print `dsp_esp_rng_ready()` (must be true, `sodium_init`
   performed there).
4. R2: record `dsp_esp_rng_calls()`, call libsodium `randombytes_buf` for a few requests, print the
   new count (must increase).
5. R5: stream 1 MiB (configurable, default 1 MiB, optional 8 MiB) from `esp_fill_random` with the
   controller enabled, over the USB serial console as framed blocks (block index, 4096 bytes in
   Base64, CRC-32 per block). A host script `firmware/tools/rng_capture.py` (pyserial from the
   ESP-IDF environment) verifies framing and CRCs and writes the raw file for `ent`, dieharder or a
   NIST STS subset. Optionally the same amount through `randombytes_buf` (the libsodium path).
6. R6 (optional, separate option `CONFIG_DSP_RNG_GATE_PSK_VECTORS`): the `psk` and `psk_not`
   records of `ble_session_v1.txt` embedded at build time (CMake-generated header from the vector
   file) are derived through `dsp_psk_derive` (mbedTLS/TF-PSA-Crypto); print pass/fail counts
   only — never key material. The inputs are the public synthetic vectors, not device secrets.
7. R3 (post): `esp_bt_controller_disable` + `deinit`; print `dsp_esp_rng_ready()` (must be false).

**Safety implications:**

| Aspect | Effect |
|---|---|
| Actuation | none: no pump, no GPIO output (application runtime off) |
| Flash writes after flashing | none intended: NVS not opened by the diagnostic, PHY calibration storage off, `nvs_sec` untouched, no setup code |
| eFuses | none read or written by the image |
| RF | BT controller enabled without host: no advertising, scanning or connections; RF calibration only. Antenna must be attached |
| Secrets | none: no keys, no setup code, the sample is public test data; logs carry no key material |
| Security services | Noise, `dsp_blesec`, GATT and the dispatcher are not linked |
| Rollback | the image is replaced by the next reviewed image; nothing persistent is created |

**Deliverables for the review (before any flashing):** the Kconfig/fragment, the diagnostic
source, the capture script, a cross-build with size and the configuration diff (`sdkconfig` key
list showing BT host off, NimBLE absent, security off), and the exact flash command proposed.
Implementation of this image is firmware work outside M1.10e and starts only after approval.
