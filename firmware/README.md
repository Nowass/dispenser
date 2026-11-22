# Dispenser V2 — firmware

ESP-IDF firmware for the Dispenser V2 battery-powered aquarium liquid-carbon doser.
Product rules: [`../docs/REQUIREMENTS_DRAFT.md`](../docs/REQUIREMENTS_DRAFT.md),
architecture: [`../docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md).

**Status: M1.1–M1.5, M1.7, M1.8 and M1.9a–c (recurrence, execution history, persistent
storage, time management, notification engine, HTTPS notification transport, integration
tests, Dosing Service with storage layout v4, Application and Power Services, ESP-IDF platform
adapters with an opt-in application runtime) and M1.10a (BLE protocol v1 codec, Noise
feasibility gate) committed.**
In its default configuration the firmware still only logs its identification (the M1.9c
runtime is opt-in). The domain and platform
components are compiled into the image's component libraries but not called by
`app_main` (optional diagnostics exist, off by default), Wi-Fi is never started, and
nothing is dosed. It
has **not been run on physical hardware**: the board has not arrived yet. All
verification so far is Linux host tests and cross-compilation for `esp32c3`.

### Implemented

- ESP-IDF project skeleton (`CMakeLists.txt`, `main/`, `sdkconfig.defaults`).
- Cross-compilation for ESP32-C3 (`esp32c3`) with ESP-IDF v6.1.
- Platform-independent C11 domain component `dispenser_domain`: firmware identification
  formatting and (M1.1) the **recurrence engine** with explicit Europe/Prague DST handling
  (see [Recurrence engine](#recurrence-engine-m11)), and (M1.2) the **execution
  coordinator and history** that claim an occurrence durably before the pump may run and
  prevent repeated automatic dosing (see
  [Execution history and deduplication](#execution-history-and-deduplication-m12)),
  tested against a fake storage with injected faults and power loss, and (M1.3)
  **persistent storage**: byte-exact versioned records, explicit-presence layout with
  corruption detection, schedule repository (16 schedules, ID rules), device configuration
  and the history store over a key-value interface (see
  [Persistent storage and configuration](#persistent-storage-and-configuration-m13)).
- ESP-IDF NVS key-value adapter in the platform component `dispenser_platform` (compiled;
  not run on flash).
- (M1.4) **Time management**: evidence-based clock trust, correction policy with rollback
  hold and jumped-over protection, sync scheduling with bounded retries, bounded Wi-Fi
  session manager, time-sync service, wake-up planner, `time` record (layout v2 with
  migration); ESP-IDF clock, Wi-Fi and SNTP adapters (compiled; not run on hardware). See
  [Time management, Wi-Fi and SNTP](#time-management-wi-fi-and-sntp-m14).
- (M1.5) **Notification engine** (domain only): reservoir LOW/BLOCKED, battery LOW, dosing
  and device errors; stable event IDs, deduplication, weekly reminders, a bounded
  persistent queue (`notify` record, layout v3 with migration), at-least-once delivery over
  a transport interface, and reservoir consumption accounting. No real transport. See
  [Notification engine](#notification-engine-m15).
- (M1.7) **HTTPS notification transport**: protocol-v1 encoder and strict response
  classifier, permanent-rejection log and transport diagnostics in the `notify` record
  (format 2), HTTPS transport over an injected HTTP port, connectivity session (D1-B: Wi-Fi
  only on existing wakes, never during dosing), 4 MB partition table with `nvs_sec`;
  ESP-IDF `esp_http_client` adapter (full certificate bundle), factory-MAC device ID,
  read-only credential providers, developer provisioning tool, off-by-default diagnostic.
  See [HTTPS notification transport](#https-notification-transport-and-connectivity-session-m17).
- (M1.8) **Integration and robustness:** shared protocol vectors with the backend, and a local
  end-to-end harness (real domain code, real TLS, the committed Worker under `wrangler dev`,
  local D1, fake Gmail, fault-injection proxy, 1,000 randomized sessions). See
  [Verification levels](#verification-levels-m18).
- (M1.9a) **Dosing Service** (domain only): the sole owner of the pump port; readiness
  (default SERVICE_REQUIRED), reservoir BLOCKED latch, restart-safe refill, durable manual
  in-flight marker, calibration and safety limits (per dose, daily per local date, runtime,
  minimum interval), idempotent recovery of interrupted doses; storage layout v4 (`dosing`
  record) with history format 2 (claimed volume), configuration format 2 and `notify`
  format 3. No real pump driver. See [Dosing Service](#dosing-service-m19a).
- (M1.9b) **Application and Power Services** (domain only): one single-owner, run-to-completion
  wake cycle (SAFE_OFF → LOAD → RECOVER → CLOCK → OBSERVE → DOSE → CONNECT → BLE → SYNC → PLAN
  → SHUTDOWN → SLEEP), the sleep plan (dose / sync / 6 h cap, no notification wakes, no dose
  wakes while dosing is impossible), a versioned CRC-checked RTC-retained block, the shutdown
  checklist with an explicit fail-safe, and an injected BLE service-mode port with a bounded
  command queue. Host-tested. See
  [Application and Power Services](#application-and-power-services-m19b).
- (M1.9c) **ESP-IDF platform adapters** (compiled only, not run on hardware): power port
  (reset/wake cause, RTC-retained block, RTC timer and provisional button wake, deep sleep,
  fail-safe reset), command-queue lock, **simulated pump (no GPIO)**, time-continuity port,
  and an opt-in `app_main` runtime (`CONFIG_DSP_APP_RUNTIME`, default off). See
  [Platform adapters and application runtime](#platform-adapters-and-application-runtime-m19c).
- (M1.10a) **BLE protocol v1 codec** and the Noise feasibility gate (see
  [BLE protocol codec and Noise gate](#ble-protocol-codec-and-noise-gate-m110a)).
- (M1.10c.2a, under review) **BLE security session** (`components/dispenser_security`): Noise
  XXpsk3 (claim, add, recovery) and IK (sessions) over the pinned, patched noise-c 0.1.31 and
  libsodium 1.10021.12 (`third_party/noise-c/noise_c_deps.cmake`), the PSK by HKDF-SHA256
  (mbedTLS/TF-PSA-Crypto on the device), the authorization-bound dispatcher, the eight-response
  cache and the ESP32-C3 RNG adapter (fail closed without an active RF entropy source). Build-only
  (`CONFIG_DSP_BLE_SECURITY`, default off) until the hardware acceptance (M1.10c.2b,
  `HARDWARE_ACCEPTANCE.md` 8a); reachable only through the NimBLE transport (M1.10d.1).
- (M1.10d.1, under review) **NimBLE BLE transport** (`dsp_ble_transport`, pure and host-tested;
  `dsp_esp_ble`, the NimBLE adapter): GATT service, advertising during service windows only,
  fragmentation at the negotiated MTU, asynchronous notification backpressure, one connection with
  strictly increasing generations, an authoritative connection snapshot in the Application Service
  (connection events never lost, no stale session or response survives a reconnect) and fragment
  errors decided by the security layer (no plaintext error after a handshake started). Build-only
  (`CONFIG_DSP_BLE_TRANSPORT` via `sdkconfig.ble.defaults`, refused with production) until
  M1.10c.2b and M1.10d.2 (`HARDWARE_ACCEPTANCE.md` 8b). See
  [NimBLE BLE transport](#nimble-ble-transport-m110d1).
- (M1.10c.1) **BLE authorization boundary**: the BLE queue carries opaque frames
  (capacity 4, connection generation) plus connect/disconnect/button events; the BLE port is
  byte-only (`send_frame`, `disconnect`, `publish_info`); a pre-authentication gate (`dsp_link`)
  answers HELLO (public state incl. CREDENTIALS_DAMAGED), refuses every privileged request
  (UNAUTHENTICATED, never decoded or dispatched) and closes after 3 errors or 30 s; an
  unauthenticated END_SESSION closes only its own connection (the service window ends only by
  the authorized `dsp_app_end_service_session` or an internal timeout); every
  `dsp_app_*` operation verifies an authorization issued for the current connection and epoch
  (a caller-built `{OWNER, slot}` is FORBIDDEN); status, diagnostics, acknowledge, refill and
  readiness became authorized operations (their queue commands are retired); protocol error 20
  UNAVAILABLE and the CREDENTIALS_DAMAGED flags. No Noise yet (M1.10c.2): no BLE request can
  reach an operation. Design: `docs/SECURITY_INTEGRATION_DESIGN.md`.
- (M1.10b.3) **Device ownership and credential persistence**: storage layout v5 with the `auth`
  credential record (identity key pair, four revocable phone slots, ownership epoch), resumable
  v4 → v5 migration, claim / add (RTC invite) / revoke / recovery (RTC arm + bound 3-press
  confirmation) through the Application Service, damaged credentials block all dosing (never
  "unclaimed"), KEY_FAULT inhibits at once, durable manual-dose admission (`dosing` format 3) and
  session-local limits, setup-code provisioning in the developer tool. No cryptography, no
  Noise, no BLE: the security layer (M1.10c) registers later; until then nothing can authorize
  an ownership change. See
  [Device ownership and credentials](#device-ownership-and-credentials-m110b3).
- (M1.10b.2) **Manual dose with durable anti-replay**: per-phone-slot request high-water marks
  and a ring of the last 8 results in the `dosing` record (format 2, layout v4); a request is
  decided exactly once (executed or durably refused) and never activates the pump again; the
  authenticated `dsp_app_manual_dose()` is the only manual path (the unauthenticated queue
  command is retired); the slot's counter floor travels in the protocol Status record. See
  [Manual dose with durable anti-replay](#manual-dose-with-durable-anti-replay-m110b2).
- (M1.10b.1) **Configuration, schedules and phone time** behind the Application Service, for the
  future BLE dispatcher: revisioned configuration read/update (calibration and limits only in
  SERVICE_REQUIRED), schedule list/get/create/update/delete with on-device names (≤ 32 bytes
  UTF-8) and a per-schedule edit floor (no catch-up after edits), owner phone time through the
  timekeeper (never creates or lifts a rollback hold); authorization is an explicit input and
  fails closed. `sched` record format 2, no layout change. Domain only, host-tested. See
  [Configuration, schedules and phone time](#configuration-schedules-and-phone-time-m110b1).
- (Phase C, 2026-11-20) **Event history:** a bounded persistent log of dose outcomes and reservoir
  changes, separate from the anti-replay records, read by the phone with GET_HISTORY. See
  [Event history (Phase C)](#event-history-phase-c).
- Linux host-side test infrastructure (CMake + CTest, warnings as errors, ASan/UBSan).
- Firmware version identification: `version.txt` → ESP-IDF app descriptor → boot log.

### Not implemented

BLE/NimBLE and GATT, the button and its pin, the real pump GPIO driver with hardware
cutoff, production credential provisioning (authenticated BLE,
per-device credentials), battery measurement, MONTHLY recurrence, BLE, GPIO
and motor control, service mode, OTA (update client and rollback; only the partitions
exist), a storage maintenance/repair tool. The domain code decides and records; nothing
drives hardware yet.

### Not verified

Physical ESP32-C3 execution (first boot log), GPIO boot/reset behavior, deep-sleep
current consumption, actual motor control. **Nothing in this firmware has been
hardware-validated.**

## Target hardware

| Item | Value |
|---|---|
| Board | Seeed Studio XIAO ESP32-C3 |
| SoC / IDF target | ESP32-C3 (RISC-V) / `esp32c3` |
| Flash size in build | **4 MB** (M1.7). Confirmed on the first board 2026-11-19 (`esptool flash-id`: ESP32-C3 rev v0.4, 4 MB XMC; [`HARDWARE_ACCEPTANCE.md`](HARDWARE_ACCEPTANCE.md) 1.1). |
| Partition table | custom [`partitions.csv`](partitions.csv): two 1.75 MiB OTA slots, `nvs` 80 KiB, `nvs_sec` 24 KiB, reserved `nvs_keys`, `coredump`, 256 KiB reserve (see [Partition table](#partition-table-4-mb-m17)) |
| GPIO assignments | **None in default builds.** Bench opt-in only: pump control signal on XIAO D10 = GPIO10, active high (`CONFIG_DSP_PUMP_GPIO`, see "GPIO pump control signal"). Button and indicator pins come from the hardware review. |

## Versioning

Three independently versioned artifacts:

| Artifact | Scheme | Source of truth | Current |
|---|---|---|---|
| Android app | SemVer, independent | `android/app/build.gradle.kts` ([RELEASING](../docs/RELEASING.md)) | `0.5.0-beta.1` |
| Firmware | SemVer, independent | [`version.txt`](version.txt) (this directory) | `0.1.0-dev` |
| BLE protocol | separate version, once designed (FR-034) | not defined yet | — |

Compatibility between app and firmware will be negotiated through the BLE protocol
version, not by matching app and firmware version numbers.

`version.txt` must contain exactly one SemVer line. ESP-IDF reads it into the app
descriptor (`esp_app_get_description()->version`); without the file ESP-IDF would fall
back to `git describe`, which reports the Android release tags of this monorepo. The host
test build reads the same file, rejects anything that is not a single SemVer line and
reconfigures when it changes. Check the compiled version:

```sh
python -m esptool --chip esp32c3 image-info build/dispenser.bin   # "App version: 0.1.0-dev"
```

## Toolchain

| Item | Version |
|---|---|
| ESP-IDF | **v6.1** (6.1.0, git tag `v6.1`, released 2026-08-27) |
| RISC-V GCC (`riscv32-esp-elf`) | `esp-15.2.0_20251204` (GCC 15.2.0, installed by ESP-IDF v6.1) |
| Installer | ESP-IDF Installation Manager (EIM) 0.21.1 / ESP-IDF VS Code extension |
| CMake | ≥ 3.22 (host: 3.28.3; ESP-IDF builds use its own CMake/Ninja) |
| Host tests | system GCC or Clang with C11 and ASan/UBSan runtime (verified: GCC 13.3.0, Ubuntu 24.04) |
| Devcontainer (optional) | `espressif/idf:v6.1`, pinned by digest (see below) |

**Why v6.1 rather than 5.5.x:** under Espressif's support policy each minor release
gets 30 months of support: 12 months of *service* (recommended for new projects) followed
by 18 months of *maintenance* (critical and security fixes only). v6.1 is the newest
stable release and is in its service period, which runs until about Aug 2027, with end
of life about Feb 2029. v5.5 dates from mid-2025 and is already in maintenance, so it
is not recommended for new projects. ESP32-C3 is fully supported by both. The project
uses only basic APIs, so moving to a later 6.x release should be cheap. Re-evaluate
when v6.2 ships.

The bugfix release **v6.1.1** appeared on 2026-11-13. The project stays on v6.1.0 for now
(no known issue affects this skeleton); moving to v6.1.1 is a separate, deliberate change
covering the native setup, this README and the devcontainer digest together.

## Native Linux setup

1. Install ESP-IDF **v6.1** with EIM or the VS Code ESP-IDF extension (Linux default
   location: `~/.espressif/v6.1/esp-idf`, tools in `~/.espressif/tools`).
2. Activate it in a shell, e.g. `. ~/.espressif/tools/activate_idf_v6.1.sh`
   (or `. $IDF_PATH/export.sh`). Check: `idf.py --version` → `ESP-IDF v6.1`,
   `riscv32-esp-elf-gcc --version` → `esp-15.2.0_20251204`.

The VS Code files that contain local absolute paths (`.vscode/settings.json` here and in the
repository root) are not committed. The committed `.vscode/c_cpp_properties.json`,
`.vscode/launch.json` and `.clangd` are the machine-independent ESP-IDF extension defaults.

## Project structure

```text
firmware/
├── CMakeLists.txt              ESP-IDF project (name "dispenser")
├── version.txt                 firmware version, single source for both builds
├── sdkconfig.defaults          committed Kconfig defaults (target, flash size, partition table, TLS bundle)
├── sdkconfig.ble.defaults      BUILD-ONLY fragment: runtime + security + NimBLE transport (M1.10d.1)
├── sdkconfig.wifitime.defaults BUILD-ONLY fragment: Wi-Fi/SNTP time sync without HTTPS (bench MVP)
├── sdkconfig.pumpgpio.defaults BUILD-ONLY fragment: GPIO10 pump control signal instead of the simulation (bench)
├── partitions.csv              4 MB partition table (M1.7)
├── tools/provision_dev_secrets.py  developer-only nvs_sec image builder (+ its test, M1.7)
├── HARDWARE_ACCEPTANCE.md      on-device acceptance checklist (M1.8; not run yet)
├── main/                       composition root: the only code calling ESP-IDF APIs
│   ├── CMakeLists.txt          REQUIRES dispenser_domain dispenser_platform esp_app_format log
│   ├── Kconfig.projbuild       DSP_STORAGE_SMOKE_TEST, DSP_TIME_SYNC_DIAGNOSTIC,
│   │                           DSP_NOTIFY_HTTPS_DIAGNOSTIC (all default off)
│   └── main.c                  app_main(): logs identification (+ optional read-only storage check)
├── components/
│   ├── dispenser_domain/       pure C11 business logic, no ESP-IDF dependency
│   │   ├── CMakeLists.txt      dual-mode: IDF component or plain host library
│   │   ├── include/dsp_identity.h    firmware identification
│   │   ├── include/dsp_time.h        dates, UTC instants, timezone rule (M1.1)
│   │   ├── include/dsp_recurrence.h  schedules and recurrence queries (M1.1)
│   │   ├── include/dsp_history.h     execution records, storage contract (M1.2)
│   │   ├── include/dsp_exec.h        execution coordinator (M1.2)
│   │   ├── include/dsp_kv.h          key-value storage interface (M1.3)
│   │   ├── include/dsp_codec.h       serialized record formats (M1.3)
│   │   ├── include/dsp_storage.h     layout, provisioning, history store, config (M1.3)
│   │   ├── include/dsp_schedule_set.h / dsp_schedule_repo.h  schedules (M1.3)
│   │   ├── include/dsp_config.h      configuration model (M1.3)
│   │   ├── include/dsp_time_meta.h / dsp_timekeeping.h  clock trust and corrections (M1.4)
│   │   ├── include/dsp_net.h         Wi-Fi session manager and connectivity interfaces (M1.4)
│   │   ├── include/dsp_time_service.h  one bounded sync session (M1.4)
│   │   ├── include/dsp_wakeup.h      next wake-up planning (M1.4)
│   │   ├── include/dsp_notify_state.h  notification state and queue model (M1.5)
│   │   ├── include/dsp_notify.h      notification engine, transport interface (M1.5)
│   │   ├── include/dsp_reservoir.h   reservoir consumption accounting (M1.5)
│   │   ├── include/dsp_notify_proto.h  protocol-v1 encoder and response classifier (M1.7)
│   │   ├── include/dsp_https_transport.h  HTTPS transport over an injected HTTP port (M1.7)
│   │   ├── include/dsp_connectivity.h  connectivity session: sync, then delivery (M1.7)
│   │   ├── include/dsp_dosing_state.h  persistent dosing record model (M1.9a)
│   │   ├── include/dsp_dosing.h      Dosing Service: sole pump owner (M1.9a)
│   │   ├── include/dsp_power.h       Power Service: plan, RTC-retained block, sleep port (M1.9b)
│   │   ├── include/dsp_app.h         Application Service: the wake cycle (M1.9b)
│   │   ├── include/dsp_bleproto.h    BLE protocol v1 codec: frames, fragments, records (M1.10a)
│   │   ├── include/dsp_auth.h        credential record model and transitions (M1.10b.3)
│   │   ├── include/dsp_owner.h       RTC invite/recovery-arm rules, 3-press confirmation (M1.10b.3)
│   │   ├── include/dsp_app_sec.h     RESTRICTED security-layer interface (M1.10b.3)
│   │   ├── include/dsp_link.h        pre-authentication frame gate per connection (M1.10c.1)
│   │   ├── sec_header_guard.cmake    configure-time check: who may include dsp_app_sec.h
│   │   └── src/                      matching .c files
│   └── dispenser_platform/     ESP-IDF adapters (never in the host build)
│       ├── Kconfig                   DSP_SNTP_SERVER (M1.4), DSP_NOTIFY_URL, DSP_NOTIFY_HEALTH_URL (M1.7)
│       ├── include/dsp_nvs_kv.h      NVS implementation of dsp_kv_t (M1.3)
│       ├── include/dsp_esp_time.h    clock port, Wi-Fi driver, SNTP source (M1.4)
│       ├── include/dsp_esp_notify.h  HTTP port, device ID, monotonic clock, nvs_sec credentials (M1.7)
│       ├── include/dsp_esp_app.h     power port, queue lock, simulated pump, time port (M1.9c)
│       ├── include/dsp_platform_logic.h  pure adapter rules, also host-tested (M1.9c)
│       ├── include/dsp_ble_transport.h   pure BLE byte transport, also host-tested (M1.10d.1)
│       ├── include/dsp_esp_ble.h     NimBLE adapter: GATT, advertising, radio ops (M1.10d.1)
│       └── src/                      dsp_nvs_kv.c, dsp_esp_clock.c, dsp_esp_wifi.c, dsp_esp_sntp.c,
│                                     dsp_esp_http.c, dsp_esp_device.c, dsp_esp_power.c,
│                                     dsp_esp_sync.c, dsp_esp_pump_sim.c, dsp_platform_logic.c
├── test/host/                  host unit tests (CMake + CTest, no ESP-IDF)
│   ├── CMakeLists.txt
│   ├── support/dsp_test.h      minimal header-only test harness
│   ├── support/fake_store.[ch] in-memory history store with fault/power-loss injection
│   ├── support/fake_kv.[ch]    in-memory key-value store (NVS-like and staged modes, faults)
│   ├── test_identity.c
│   ├── test_recurrence.c          engine unit tests (C only)
│   ├── test_recurrence_vectors.c  runs ../../test-vectors/*.txt
│   ├── test_exec.c                coordinator/history tests (M1.2)
│   ├── test_storage.c             persistent storage tests (M1.3, + v2/v3 migrations)
│   ├── test_time.c                time management tests (M1.4)
│   ├── test_notify.c              notification engine and reservoir tests (M1.5, + M1.7 rejections)
│   ├── test_proto.c               protocol encoder and response classifier (M1.7)
│   ├── test_https.c               HTTPS transport and connectivity session (M1.7)
│   ├── test_proto_vectors.c       runs ../../test-vectors/notification_protocol_v1.txt (M1.8)
│   ├── test_dosing.c              Dosing Service (M1.9a)
│   ├── test_app.c                 Application and Power Services (M1.9b)
│   ├── test_platform.c            platform adapter logic and queue threads (M1.9c)
│   ├── test_bleproto.c            BLE protocol v1 codec, shared vectors, fuzzing (M1.10a)
│   ├── test_app_ops.c             configuration, schedule, time operations (M1.10b.1)
│   ├── test_manual.c              manual-dose anti-replay through wakes (M1.10b.2)
│   ├── test_battery_app.c         battery telemetry through wakes (GET_BATTERY, fw 0.2.0)
│   ├── test_auth.c                credential record, RTC overlays, confirmation (M1.10b.3)
│   ├── test_owner.c               ownership through wakes, power cuts, admission (M1.10b.3)
│   ├── test_sec_header_guard.cmake  restricted-header check (M1.10b.3)
│   ├── test_link.c                pre-authentication gate, every privileged type, fuzzing (M1.10c.1)
│   ├── test_session.c             authorization boundary through wakes, HELLO/Info state (M1.10c.1)
│   ├── test_secderive.c           PSK / prologue / device hash against ble_session_v1 (M1.10c.2a)
│   ├── test_blesec.c              Noise sessions end to end: noise-c phone vs dsp_blesec (M1.10c.2a);
│   │                              also built as test_ble_e2e: the same suite through the transport
│   ├── test_handshake_vectors.c   M1.10e (E7): test-vectors/ble_handshake_v1.txt (independent Rust snow
│   │                              transcripts) replayed byte for byte through dsp_app + dsp_blesec
│   ├── test_ble_transport.c       BLE transport over a fault-injecting radio, two threads (M1.10d.1)
│   └── test_ble_transport_symbols.cmake  link-time boundary check of the transport (M1.10d.1)
├── test/fwsim/                 firmware-in-the-loop simulator library for the Android tests (test-only, M1.10e.2b)
│   └── tsan/                      ThreadSanitizer driver for its connection lifecycle (test-only, see its README)
├── test/psk_linux/             device HKDF provider (PSA) on the ESP-IDF linux target (M1.10c.2a)
├── test/integration/           local end-to-end harness (M1.8, see its README)
│   ├── notify_harness.c           real domain code driven over stdin
│   ├── posix_https_port.[ch]      host HTTP port: real TLS (OpenSSL), test CA, timeouts
│   └── notify_e2e.mjs             TLS fault proxy, wrangler dev + local D1, fake Gmail, scenarios
└── .devcontainer/              optional pinned ESP-IDF v6.1 build container

../test-vectors/                shared C/Kotlin test vectors (see its README)
```

### Dependency rules

```text
main (ESP-IDF APIs, wiring)  ──►  dispenser_domain (pure C)
          │
          └──►  future platform adapters (GPIO motor, NVS storage, RTC clock, BLE, Wi-Fi)
                implementing interfaces declared in dispenser_domain
```

- `dispenser_domain` never includes ESP-IDF, FreeRTOS or hardware headers. The host test
  build compiles it **without any ESP-IDF include path**, so a violation fails the build.
- Platform dependencies are passed in explicitly as structs of function pointers plus a
  context pointer, in the spirit of the `IClock` / `IActuator` / `IStorage` /
  `IBattery` / `INotifier` interfaces in ARCHITECTURE. No global mutable state.
- **Planned and not yet created:** dosing coordinator, reservoir estimator, event journal
  and service-mode state machine go into `dispenser_domain` next to the recurrence engine
  (split into a separate component only if size or build boundaries justify it).
  Hardware adapters go into a `dispenser_platform` component once the first adapter
  exists (M0 deep sleep / safe-off GPIO, after the hardware review).
- C11 only. No C++ until there is a concrete reason.

## Recurrence engine (M1.1)

Pure C11 in `components/dispenser_domain` (`dsp_time.h`, `dsp_recurrence.h`). Behavioral
reference: the Android Kotlin `RecurrenceEngine`, `ScheduleValidator`,
`NextDoseCalculator` and `OccurrenceCalendar`. Parity is enforced by the shared vectors in
[`../test-vectors/`](../test-vectors/README.md), which both test suites run.

**Safety boundary:** the engine only *calculates* planned occurrences. It never doses,
touches GPIO, NVS, radios, timers or the system clock, and it has no catch-up: queries
return only occurrences strictly after a given instant. Whether an occurrence may be
executed (already handled, missed, reservoir, service mode, clock validity) is the job of
the future dosing coordinator.

### Rules (implemented, matching Android)

- **ONCE:** on `start_date` only.
- **DAILY:** every `interval` calendar days counted from `start_date`.
- **WEEKLY:** selected weekdays (mask bit 0 = Monday … bit 6 = Sunday) in weeks whose
  distance from the Monday of the start week is divisible by `interval`; days before
  `start_date` never occur, even in the start week.
- Optional inclusive end date. Disabled schedules have no occurrences.
- **DST (FR-015, approved):** a local time that does not exist (spring forward) is skipped
  that day, never shifted; a repeated local time (fall back) yields one occurrence at the
  earlier UTC instant; a ONCE schedule at a nonexistent time fails validation
  (`DSP_SCHED_E_TIME_NONEXISTENT`).
- MONTHLY, hourly intervals and RRULE text are **not** supported (MONTHLY awaits a product
  decision on missing dates).

### Data model

`dsp_schedule_t` is fixed-size and pointer-free (about 96 bytes): `id[65]` (1–64 chars of
`[A-Za-z0-9._-]`, as in the Android backup format), `revision` ≥ 1, `enabled`,
`amount_ul` (integer microlitres, `uint32_t`, ≥ 1, no floating point), `start_date`,
`hour`/`minute`, `kind` (`uint8_t`), `interval` (1–2147483647), `weekday_mask`,
optional `end_date`. Canonical form as in the Android backup codec: ONCE has interval 1,
no weekdays and no end date; DAILY has no weekdays. The persistent/BLE encoding is not
defined yet (M1.2+/M3); fixed-width fields keep it straightforward.

### Public API (summary; contracts in the headers)

| Function | Result |
|---|---|
| `dsp_schedule_validate(s, tz)` | bitmask of **all** problems (`DSP_SCHED_E_*`), 0 = valid; names follow Kotlin `ValidationError` where one exists |
| `dsp_rec_next(s, tz, after, &occ)` | earliest occurrence strictly after `after`: `DSP_REC_OK` / `DSP_REC_NONE` |
| `dsp_rec_between(s, tz, after, until, buf, cap, &n)` | all occurrences in (after, until] into a caller buffer; more than `cap` → `DSP_REC_ERR_BUFFER_FULL` with the earliest `cap` written, resumable from `buf[cap-1].at`; never a silent truncation |
| `dsp_rec_occurrence_on(s, tz, date, &occ)` | occurrence on one local date, or `DSP_REC_NONE` |
| `dsp_rec_next_of_set(arr, n, tz, after, &i, &occ)` | earliest across schedules; ties → smaller id, then smaller index; an invalid schedule fails the call with its index |
| `dsp_occurrence_id_format(id, date, buf, len)` | `"<id>@YYYY-MM-DD"`; too-small buffer is an error, never a truncated id |

Every query validates its schedule first and returns `DSP_REC_ERR_SCHEDULE` instead of
evaluating an invalid one. No allocation, no global mutable state, no current-time input:
the caller passes every instant.

### Time representation and DST strategy

- Instants are `dsp_utc_t`: `int64_t` seconds since 1970-01-01T00:00:00Z (POSIX time,
  no leap seconds). Local times exist only as (date, hour, minute) inputs.
- Calendar math is integer-only (Hinnant's `days_from_civil` / `civil_from_days`).
- The timezone is an explicit `dsp_tz_t` value passed to every call:
  `DSP_TZ_EUROPE_PRAGUE` = CET +1 h / CEST +2 h with the EU rule (DST from the last
  Sunday of March 01:00 UTC to the last Sunday of October 01:00 UTC). Local → UTC tries
  both offsets and keeps those consistent with the rule: two → overlap (earlier wins),
  none → gap.
- **Why not libc `TZ`/`mktime`:** `TZ` is process-global state, `mktime` resolves gaps and
  overlaps differently in newlib (ESP-IDF) and glibc, and results would depend on the host
  `TZ`. **Why not a tz database:** a full IANA database is large and unnecessary for one
  zone. The EU rule matches IANA `Europe/Prague` exactly for every supported year
  (verified against all 100 transition pairs of 2000–2099 from tzdata 2026e and against
  Kotlin `java.time`). The engine references no libc time function (checked with `nm`).
- **Legislation changes:** if the EU changes or abolishes DST, the rule is updated by a
  firmware update (approved decision); there is no runtime timezone configuration.

### Supported ranges (checked; no undefined or implementation-defined behavior)

| Input | Supported range (inclusive) | Outside the range |
|---|---|---|
| Schedule dates (`start_date`, `end_date`) | 2000-01-01 … 2099-12-31 (approved) | `dsp_schedule_validate` error |
| Query instants (`after`, `until` of `dsp_rec_*`) | any `int64_t` | clamped to 1999-12-30T00:00Z … 2100-01-02T00:00Z before any conversion; the bounds are a day away from every schedule date, so clamping never changes a result |
| `dsp_tz_offset_at`, `dsp_utc_to_local_date` | `DSP_UTC_MIN` … `DSP_UTC_MAX` = 1999-01-01T00:00:00Z … 2100-12-31T23:59:59Z | return `false`, output untouched |
| `dsp_date_from_days` | `DSP_DAYS_MIN` … `DSP_DAYS_MAX` = 1900-01-01 … 2199-12-31 | return `false`, output untouched |
| `dsp_local_to_utc` | valid schedule date, hour 0–23, minute 0–59, valid timezone | `DSP_LOCAL_INVALID`, output untouched |
| `dsp_tz_transitions` | years 1999 … 2100 | return `false` |
| `dsp_date_to_days`, `dsp_weekday_from_days` | every input value (total, overflow-free) | — |

Every narrowing conversion inside the module is preceded by one of these checks. Inside
the engine an unexpected conversion failure is reported as `DSP_REC_ERR_INTERNAL`, never
as "no occurrence". The boundary values are covered by `test_recurrence`
(`test_time_api_range_boundaries`).

### Occurrence identity (approved 2026-11-15, FR-014)

Identity = `<scheduleId>@<localDate>`, e.g. `schedule-001@2026-11-16`, as in Android.

- Independent of the schedule revision. Changing a schedule's time or volume never
  creates a second identity for the same local date: an edit from 08:00 to 20:00 yields
  the *same* identity for a day already handled, so a durable "handled" record keyed by
  it (M1.2/M1.3) blocks a second automatic dose.
- At most one occurrence per schedule per local date; multiple doses per day need
  multiple schedules.
- **Mandatory invariant:** schedule IDs are unique within the device, and the ID of a
  deleted schedule is **never reused** for a new schedule. Otherwise a new schedule could
  inherit (and be blocked by) the handled-occurrence history of a deleted one. This is
  enforced by the future schedule-management/storage layer; M1.1 does not manage IDs.

M1.1 only formats and tests the identity; there is no durable deduplication and no
execution yet.

### Approved decisions (2026-11-15)

| Topic | Decision |
|---|---|
| Disabled schedules | All firmware recurrence queries return no occurrences |
| Supported dates | 2000-01-01 through 2099-12-31 |
| Volume representation | `uint32_t` microlitres for now |
| Maximum dispensing volume | To be defined in the BLE protocol specification |
| Timezone | Explicit Europe/Prague EU DST implementation |
| Future DST legislation changes | Handled through a future firmware update |
| Occurrence identity | `<scheduleId>@<localDate>`, IDs unique and never reused |
| Shared test vectors | `test-vectors/` at the repository root |

### Resource bounds

- `dsp_rec_next`: O(1) date arithmetic per candidate, at most 8 candidate dates per
  search (4 are provably enough: the search starts one day early, and a DST-gap day
  is never followed by another); exceeding it returns `DSP_REC_ERR_INTERNAL` instead of
  looping. Huge intervals jump directly, with no day-by-day scan (64-bit day numbers).
- `dsp_rec_between`: at most `capacity + 1` searches; `dsp_rec_next_of_set`: one search
  per schedule. No unbounded loop exists.
- No heap, no recursion, no VLAs, no floating point. ESP32-C3 (IDF default build):
  about 5.6 KB code for `dsp_time` + `dsp_recurrence`, no `.data`/`.bss`; largest
  stack frame 128 bytes (`-Os`), deepest call chain well under 1 KB.

### Known limitations and differences from the Android reference

Parity holds for all shared vectors. The remaining differences follow from the approved
decisions above and lie outside normal use:

1. **Date range:** firmware rejects dates before 2000 or after 2099 and never generates
   occurrences after 2099-12-31; Kotlin has no such bound (vectors with `scope=c`).
2. **Timezone data:** firmware hard-codes the EU rule; Android uses the device's IANA
   tzdata. A DST legislation change needs a firmware update, while Android follows its
   tzdata updates; until both are updated the two can disagree.
3. **Disabled schedules:** firmware queries return nothing for a disabled schedule (approved).
   Kotlin `RecurrenceEngine` ignores `enabled`, but every Android consumer filters it
   first (dashboard, calendar, simulation), so observable behavior is the same. Only the
   editor preview shows dates of a disabled schedule.
4. **Amount range:** `uint32_t` microlitres (max ≈ 4,295 l per dose) vs Kotlin's technical
   maximum of 2^53−1 µl. Neither is a dosing limit; the maximum dispensing volume will be
   defined in the BLE protocol specification (FR-016 still TBD).
5. **Canonical form:** firmware validation also rejects non-canonical fields (ONCE with
   interval ≠ 1, an end date or weekdays; DAILY with weekdays; bad id; revision 0),
   which Android enforces through its types and backup codec instead.
6. **Inverted range:** `dsp_rec_between` with until < after returns `DSP_REC_ERR_RANGE`;
   Kotlin returns an empty list.
7. **Android finding — silent truncation in Kotlin `occurrencesBetween`** (confirmed by the
   independent review; Android production code deliberately unchanged in M1.1):
   - *Cause:* the private `occurrencesAfter` sequence stops after `MAX_CANDIDATES` =
     10,000 candidate dates. `occurrencesBetween` returns `null` only when it sees an
     occurrence beyond `limit`; if the sequence ends first, the loop simply finishes and the
     partial list is returned as if complete.
   - *Conditions:* more than 10,000 rule-matching candidate dates lie in the range, and
     `limit` is at least the number of occurrences among the first 10,000 candidates. That
     number is 10,000 for most schedules, but lower when DST-gap days (which are candidates
     without an occurrence) are among them. Example: a daily schedule over a range longer
     than about 27 years with the default budget of 10,000 (`SimulationEngine` and
     `ReservoirPredictor` both pass `maxOccurrences = 10_000` minus what they already
     collected). Measured with a temporary probe on a 34-year range: daily 08:00 with
     limit 10,000 returned 10,000 occurrences (correct result: `null`), while limit 9,999
     returned `null`. Daily 02:30 with limit 9,990 returned 9,972 occurrences (correct
     result: `null`). `nextOccurrence` is not affected in practice, since it needs only a
     few candidates.
   - *Effect:* a simulation "advance" or reservoir prediction over such a range would
     silently use only the first 10,000 occurrences instead of reporting the budget as
     exceeded.
   - *Recommended fix (separate Android change):* make the candidate cap observable, e.g.
     let `occurrencesAfter` signal exhaustion of the cap and have `occurrencesBetween`
     return `null` in that case, or drop the cap from the range query and bound it by
     `limit + 1` occurrences as the C engine does. Add a regression test with a daily
     schedule, `limit = 10_000` and a 30-year range, expecting `null`.
   - The C engine has no such cap and always reports `DSP_REC_ERR_BUFFER_FULL`.

## Execution history and deduplication (M1.2)

Pure C11 in `components/dispenser_domain`: `dsp_history.h` (records, storage contract,
history operations) and `dsp_exec.h` (execution coordinator). It decides whether an
automatic occurrence may run and makes sure it runs **at most once**. It does not start
the pump, read the clock or write NVS itself: the caller supplies the clock snapshot and
an injected storage implementation.

### Approved semantics

- Identity `<scheduleId>@<localDate>` (M1.1). Once an automatic occurrence has been
  **claimed**, it is never executed automatically again, in any state (STARTED,
  COMPLETED, FAILED, INTERRUPTED). Editing a schedule (time, volume, revision) keeps the
  identity, so a claimed day is never repeated.
- **Due window (approved: 10 minutes):** an occurrence may run while
  `scheduled_at <= now <= scheduled_at + 600 s`. Before that: `DSP_EXEC_NOT_DUE`. After
  that: `DSP_EXEC_MISSED`, skipped forever. There is no catch-up, so accumulated occurrences
  are never executed after a restart. The window is a coordinator parameter (0 … 3600 s)
  so it can be retuned after RTC drift measurements.
- A **valid clock** (`dsp_clock_t.valid`, set by the M1.4 timekeeper (`dsp_timekeeper_clock`) after SNTP or
  Android time, FR-031/FR-033) is required; otherwise `DSP_EXEC_ERR_CLOCK`.
- **Disabled** schedules are never executed (`DSP_EXEC_DISABLED`).
- The occurrence must be exactly what the **current** schedule yields on that date
  (re-computed with the recurrence engine). Stale occurrences computed before an edit
  and fabricated ones are refused (`DSP_EXEC_ERR_OCCURRENCE`).
- **Manual dosing** (`dsp_exec_manual_begin`) is independent: it never reads, writes or
  clears automatic history, and it neither satisfies nor resets an automatic occurrence.
  Only mutual exclusion with automatic execution (one pump) is enforced.
- **Default is no dosing:** any error, unknown history (read failure, corrupted or
  duplicate record), full history or unconfirmed persistence means the pump is **not**
  authorized.

### Automatic execution protocol

1. `dsp_exec_check()`: optional read-only pre-check, e.g. before stock and battery checks.
2. `dsp_exec_claim()`: re-checks everything, then writes a `STARTED` record, commits it
   and reads it back. **Only `DSP_EXEC_OK` authorizes pump activation.**
3. The caller runs the pump (future milestone).
4. `dsp_exec_finish(token, COMPLETED | FAILED)`: stores the outcome. Nothing is ever
   retried automatically.

At boot, `dsp_exec_recover()` rewrites every record still `STARTED` as `INTERRUPTED`
(still blocking), so the outcome can be reported as uncertain.

### State transitions (per schedule record)

```text
             claim (durable STARTED)                     finish(COMPLETED)
 (no record) ─────────────────────────► STARTED ─────────────────────────► COMPLETED
 or older date                             │   └─────── finish(FAILED) ───► FAILED
                                           └── restart, recover() ────────► INTERRUPTED

 Every state blocks automatic execution of its local date and of all earlier dates.
 A claim for a LATER date overwrites the schedule's record (the watermark moves forward).
```

### Crash and restart behavior

| Event | Result |
|---|---|
| Restart before the scheduled time | Executes normally when due (within the window) |
| Restart or wake after the window | `MISSED`; skipped, never caught up; the next occurrence runs normally |
| Power loss before the claim is durable (write or commit lost) | Claim failed, pump never authorized. After restart, the occurrence may still run once if the window is still open. This is not a duplicate, because the pump never ran. |
| Power loss after the durable claim, before or during the pump run | Record `STARTED` → `INTERRUPTED` at recovery; never resumed or repeated. **The dose may be lost or partial (accepted).** |
| Commit reports failure but data persisted / read-back mismatch | Not authorized; the occurrence is blocked afterwards (lost, never doubled) |
| Storing the outcome fails after the pump ran | Record stays `STARTED` (still blocks); reported as interrupted after the next boot |

### Storage contract (for the future NVS adapter)

`dsp_history_store_t` is a fixed array of record slots with `read`, `write` (stage one
slot) and `commit` (make durable):

- **Per-slot atomicity:** after any power loss, each slot holds its old or its new
  record, never a mix. Suggested NVS mapping: one blob key per slot, a versioned
  encoding, `nvs_set_blob` + `nvs_commit`.
- **Durability:** `commit` returning OK means the record survives power loss. On a commit
  error, the slot may hold either value.
- **No cross-slot atomicity** is assumed: every coordinator transition stages and commits
  exactly one slot. The coordinator confirms each commit by reading the slot back.
- **Concurrency (approved: single owner):** neither the coordinator nor the store is
  thread-safe. The future **dosing service task** owns the coordinator and the store and
  serializes every automatic and manual request. Two execution paths can therefore never
  claim the same occurrence at the same time. Within that task, the coordinator allows only
  one active execution (`DSP_EXEC_BUSY`). BLE handlers and timers must post requests
  to that task instead of calling the coordinator.

### History representation and retention (approved)

- **Per-schedule watermark:** one record per schedule ID, holding the last claimed local
  date, state, revision, scheduled and claim instants (96 bytes in RAM). An occurrence
  is blocked if its schedule's record date is ≥ its local date. This is sufficient because a
  schedule has at most one occurrence per local date and dates are never caught up.
  While a schedule's record exists, it also blocks its dates after a backward clock
  correction of any size. Each claim updates the schedule's record in place: 2 flash writes
  per automatic dose (claim and outcome) and none otherwise.
- **Retention (approved: 7 days):** a record of another schedule may be overwritten when the
  history is full, but only once its local date is **more than 7 days** before today's local
  date, oldest first. Under a correct clock this cannot cause a duplicate: such a record
  only blocks occurrences dated a week or more in the past, which are refused as
  `MISSED` anyway.
- **Capacity (approved: 32 slots by default, `DSP_HISTORY_DEFAULT_SLOTS`; maximum 64):**
  32 slots take about 3 KB in NVS. They must cover the active schedules (FR-010 proposes
  up to 8) plus schedules whose last claim is within the last 7 days. When no empty or
  recyclable slot exists, a new schedule's claim fails with `DSP_EXEC_ERR_CAPACITY` and
  **storage is not modified**; the new schedule is not executed until a record becomes
  older than 7 days. Existing schedules keep working (in-place update). Tested with all
  32 slots occupied.
- **Whole-history validation:** every check, claim, lookup and recovery first validates all
  slots. Any corrupted record, or any two records sharing a schedule ID (whichever schedule
  they belong to), returns `DSP_EXEC_ERR_INTERNAL` and blocks automatic dosing (approved).
  Duplicates are found with one 32-bit FNV-1a hash per occupied slot (at most
  64 × 63 / 2 integer comparisons). Equal hashes trigger an exact re-read and compare of
  both slots, so different IDs that collide are not misreported.
- This history is for deduplication only. The user-visible audit trail (every attempt,
  skip and outcome, FR-040/FR-041) is the future event journal.

### Backward clock adjustments

Inside M1.2 alone, deduplication of **recycled** records relies on an assumption: the
*valid* clock never moves back by more than the 7-day retention window.

| Situation | M1.2 coordinator alone |
|---|---|
| Clock moved back, the schedule's record still exists (any distance) | Blocked (`ALREADY_CLAIMED`) |
| Clock moved back ≤ 7 days | Record cannot have been recycled: blocked |
| Clock moved back > 7 days to the date of a **recycled** record | Refused with `ERR_CAPACITY` (storage untouched): an emergent effect, not a designed guarantee |
| Same, but a slot is free (M1.2 never frees one) | Would be authorized again |

**Enforced since M1.4.** The timekeeper detects a synchronization that moves the clock back
more than 7 days below the highest trusted time. It then holds automatic dosing (the
coordinator receives an invalid clock) until the clock reaches high water − 7 days. Once
that point is reached, no recycled record can belong to a date that is still due. The
remaining unprotected case is an RTC that jumps by itself between synchronizations,
without any reference to detect it. That is a hardware fault outside the trust model; even
then, the M1.4 randomized test confirms that the same occurrence identity never repeats.
`test_limitation_backward_clock_after_recycling` still pins the M1.2-level behavior.

### Re-timing an executed schedule (analysis 2026-11-20)

Bench observation (2026-11-20, GPIO-only test): moving an already executed schedule to a later
time **of the same day** did not produce another pulse, while new schedules did. This is the
approved at-most-once rule, not a defect:

- The identity of an automatic occurrence is `<scheduleId>@<localDate>`; the durable record is a
  per-schedule watermark (last claimed local date). A record in any state (STARTED, COMPLETED,
  FAILED, INTERRUPTED) blocks every occurrence of that schedule on that date or earlier
  (`ALREADY_CLAIMED`). Edits never clear or lower the watermark, so **a schedule doses at most
  once per local date**, whatever its time, volume or enabled state is changed to.
- Independently, the edit floor (`not_before` = trusted edit time + 1 s) makes every occurrence
  scheduled before an edit of the timing `MISSED`: a time moved to the past or to "now" never
  creates a catch-up dose.
- What does dose again: the same schedule on a **later date** (a DAILY/WEEKLY schedule the next
  day at its new time; a ONCE schedule whose date is moved to a later day), or a **new** schedule
  (new ID) whose time is after its creation. Deleting and re-creating under a new ID on the same
  day is a new identity (approved, see "Schedule IDs").

Why it seemed unreliable on the bench: an edit within the same day is accepted (the schedule is
stored and listed with its new time) but has no visible effect until the next date. Regression
tests: `test_once_schedule_retimed_after_execution` (ONCE: same day blocked, later date doses
once, a new schedule doses), `test_schedule_edit_never_resurrects_claimed_occurrence` (DAILY),
`test_schedule_edits_near_due_window` (edit floor). The Android schedule editor explains the rule
when an executed schedule is edited (docs/ANDROID_BLE_DESIGN.md §16b). The anti-replay records
are never cleared to get another dose; a bench pulse needs a new schedule.

### Schedule IDs (invariant from M1.1, approved 2026-11-15)

Schedule IDs must be unique within the device and **never reused**. Preferred for the
schedule-management milestone (approved): **UUID-based IDs generated by Android**. The
device rejects a create whose ID already exists, and an edit keeps the ID with a higher
revision. With the watermark design, an ID that does reappear (for example via backup
restore) can only **suppress** a dose on a date the old schedule already claimed. It can
never cause a duplicate. **Approved:** deleting a schedule and creating a new one (new ID)
on the same day is a new identity and may dose on that day as well.

### Status codes

`DSP_EXEC_OK`, `NOT_FOUND` (lookup), `NOT_DUE`, `MISSED`, `ALREADY_CLAIMED`,
`DISABLED`, `BUSY`, `ERR_ARG`, `ERR_SCHEDULE`, `ERR_OCCURRENCE` (invalid identity),
`ERR_CLOCK`, `ERR_CAPACITY`, `ERR_STORAGE_READ`, `ERR_STORAGE_WRITE`,
`ERR_STORAGE_COMMIT`, `ERR_PERSIST_CONFIRM`, `ERR_INTERNAL` (corrupted or duplicate
record anywhere in the history, slot changed under an active execution).

### Resources

No heap, no globals, no recursion. Bounded work: at most 64 slot reads per scan, plus
2 re-reads per hash collision and at most 2,016 hash comparisons. ESP32-C3 code (`-Os`):
about 2.4 KB (`dsp_exec` 1.5 KB, `dsp_history` 1.0 KB), no `.data`/`.bss`. RAM:
`dsp_exec_t` about 150 B (caller-owned). Stack (`-Os`): `dsp_history_scan` 576 B (hash
table for 64 slots), `dsp_exec_finish` 320 B, `dsp_exec_claim` 272 B. The deepest chain
(claim → scan) is about 0.85 KB plus the storage adapter's own needs, which the dosing
service task's stack must cover.

### Remaining limitations

- Backward clock jumps over 7 days after recycling (see above): an assumption, enforced
  later by the clock service.
- Corrupted or duplicate history blocks all automatic dosing (approved, fail-safe) until a
  future maintenance/repair operation clears it. That operation does not exist yet.
- After heavy schedule churn, capacity can delay a brand-new schedule by up to 7 days.
- The record encoding, NVS key layout and wear figures belong to the NVS adapter
  milestone. Clock validity, stock/battery checks, limits, event journal and pump control
  are not part of M1.2.

## Persistent storage and configuration (M1.3)

Domain (pure C11, host-tested): `dsp_kv.h` (key-value interface), `dsp_codec.h`
(byte formats), `dsp_storage.h` (layout, provisioning, history store, configuration),
`dsp_schedule_repo.h` (schedules), `dsp_config.h` (configuration model). Platform
(ESP-IDF only): `components/dispenser_platform` with `dsp_nvs_kv.h`, a thin NVS
implementation of `dsp_kv_t`. **Implemented and host-tested; NVS runtime behavior on real
flash has not been verified** (no hardware).

```text
dosing service (future) ──► dsp_exec ──► dsp_history_store_t ─┐
                          ──► dsp_schedule_repo ───────────────┼─► dsp_storage ──► dsp_kv_t
                          ──► dsp_storage_*_config ────────────┘                    │
                                                         dispenser_platform: dsp_nvs_kv (NVS)
```

### What ESP-IDF v6.1 NVS actually guarantees (checked in its sources)

- **Per-key atomic replacement:** a new value is written completely (blobs under an
  alternate chunk version, then their index) before the old one is erased. After a power
  loss, init removes incomplete new versions and older duplicates, so a key holds the old
  or the new value.
- `nvs_set_blob()` is **durable when it returns OK**; `nvs_commit()` is a no-op. That is
  stricter than our contract requires and safe: a durable claim before a failed confirmation
  blocks the occurrence (lost dose), it never doubles it.
- **No atomicity across keys.**
- **Corrupted items silently disappear:** items failing the header CRC at load, or the data
  CRC on read, are erased and then read as `ESP_ERR_NVS_NOT_FOUND`. Therefore
  **"not found" cannot mean "empty"** (see explicit presence below).
- `ESP_ERR_NVS_NO_FREE_PAGES` / `ESP_ERR_NVS_NEW_VERSION_FOUND` at init: the ESP-IDF
  examples erase the partition. **This firmware never requests an erase**: the adapter
  reports the error and automatic dosing stays disabled.
- **NVS initialization itself may write:** `nvs_flash_init_partition()` initializes blank
  pages and, after an interrupted operation or a power loss, completes or reverts it. The
  ESP-IDF docs note that its recovery may erase and rewrite some pages. This is internal
  library behavior that the firmware neither performs nor can prevent.

### Storage layout v5 (one NVS namespace, `dispenser`, partition `nvs`)

| Key | Content | Size |
|---|---|---|
| `layout` | layout marker (version 5 since M1.10b.3, 32 history slots, 16 schedules, 16 tombstones); written **last** at provisioning | 7 B |
| `h00` … `h31` | execution history slot: M1.2 record (format 2 since M1.9a: + claimed volume) or the explicit EMPTY record | 96 B each (format 1: 92 B) |
| `sched` | the whole schedule set (schedules + deleted-ID tombstones + set revision); format 2 since M1.10b.1 (+ name, edit floor per schedule) | 8 … 3,160 B (format 1: 8 … 2,504 B) |
| `config` | device configuration (format 2 since M1.9a: + pump calibration and safety limits) | 46 B (format 1: 18 B) |
| `time` | time metadata (M1.4): last sync, high-water time, rollback hold, last correction, dosing floor | 42 B |
| `notify` | notification state (M1.5): ID epoch and sequence, condition latches, 16-entry queue; since M1.7 (format 2) also the transport diagnostics and the 4-entry rejection log; since M1.9a (format 3) the dose-bound report keys | 794 B (format 2: 758 B, format 1: 682 B) |
| `dosing` | dosing state (M1.9a, layout v4): readiness, BLOCKED latch, refill intent, manual in-flight marker, daily ledger, last pump start; format 2 since M1.10b.2: per-slot manual-dose high-water marks, the marker's request binding, ring of the last 8 request results ; format 3 since M1.10b.3: the durable manual-dose admission date and count | 212 B (format 2: 206 B, format 1: 52 B) |
| `auth` | credential record (layout v5, M1.10b.3): claim state, KEY_FAULT flag, ownership epoch, device identity key pair, 4 phone slots (public key, name, enrolled, last seen), recovery and diagnostic counters. **Not** the setup code (only in `nvs_sec`) | 416 B |
| `ev00` … `ev63` | **not part of the layout** (Phase C): user-visible event history, one event per slot (`dsp_evlog`); a missing key is an empty slot, never corruption; no marker change, ignored by older firmware. See "Event history (Phase C)" | 41 … 105 B each |

**Explicit presence (approved):** on first boot every key is written with its
provisioning value (EMPTY slots, empty set, default configuration), then the marker.
Afterwards keys are never deleted, only overwritten, so:

| Found at open | Result |
|---|---|
| Marker valid, every key present and valid | `DSP_STORAGE_OK` |
| Marker valid, any key missing or malformed | `DSP_STORAGE_ERR_CORRUPT`, nothing modified |
| Any record or the layout from a newer format | `DSP_STORAGE_ERR_UNSUPPORTED`, nothing modified |
| No marker, all other keys absent or still at provisioning values | first boot or interrupted provisioning: the missing keys are written, then the marker |
| No marker, any key holds real data | marker lost: `DSP_STORAGE_ERR_CORRUPT`, nothing modified |

Open also runs the M1.2 whole-history validation (corrupted records, duplicate IDs). After
a successful open, a missing history key reads as `DSP_STORE_ERR_READ` (the coordinator
then refuses with `ERR_STORAGE_READ`), never as an empty slot. Any storage error means
**no automatic dosing**; the firmware never erases, reformats or rewrites data to "repair" it
(ESP-IDF's internal NVS recovery during init is separate, see above). Data stays
in place for maintenance (no repair tool exists yet).

### Persistent data format (all little-endian, explicit sizes, no structs)

Every record: byte 0 = type (1 layout, 2 history, 3 schedule set, 4 config, 5 time,
6 notify), byte 1 =
format version (1; since M1.9a `notify` is at format 3, `h00`–`h31` and `config` at format 2;
since M1.10b.1 `sched` is at format 2; older formats are still read and upgraded by the next
write). Decoders require the exact length, a known type and version, valid
fields and zero padding. Encoders never produce an invalid record.

```text
history slot (96 B):  0 type | 1 ver (2) | 2 state (0=EMPTY,1=STARTED,2=COMPLETED,3=FAILED,
                      4=INTERRUPTED) | 3 id_len | 4..67 id (zero padded) | 68..69 year |
                      70 month | 71 day | 72..75 revision | 76..83 scheduled_at (i64) |
                      84..91 claimed_at (i64) | 92..95 claimed volume µl (format 2; format 1
                      = the first 92 B, volume unknown = 0). EMPTY: state 0, rest zero.
schedule set:         0 type | 1 ver (2) | 2..5 set revision | 6 count (<=16) |
                      7 tombstones (<=16) | count x entry | tombstones x id
  entry (132 B):      0 id_len | 1..64 id | 65..68 revision | 69 enabled (0/1) |
                      70..73 amount_ul | 74..77 start date | 78 hour | 79 minute | 80 kind |
                      81..84 interval | 85 weekday mask | 86 has_end (0/1) |
                      87..90 end date (zero when absent) | 91 name length (0..32) |
                      92..123 name, UTF-8 without control characters (zero padded, no NUL
                      inside) | 124..131 edit floor not_before (i64, 0 or a valid instant)
                      (format 2, M1.10b.1; format 1 entries are the first 91 B: empty name,
                      no floor)
  tombstone (65 B):   0 id_len | 1..64 id
config (46 B):        0 type | 1 ver (2) | 2..5 revision | 6..9 reservoir capacity µl |
                      10..13 estimated remaining µl | 14 low-stock % | 15..16 NTP interval h |
                      17 notification mask | 18..21 pump flow µl/s | 22..29 calibrated_at (i64) |
                      30..33 max dose µl | 34..37 max daily µl | 38..41 max runtime ms |
                      42..45 min interval s (format 2; format 1 = the first 18 B, these 0)
layout (7 B):         0 type | 1 ver | 2..3 layout version | 4 history slots |
                      5 schedule capacity | 6 tombstone capacity
time (42 B):          0 type | 1 ver | 2..9 last_sync | 10..17 high_water | 18..25 hold_until |
                      26..33 last_correction_s | 34..41 not_before (all i64; instants 0 or in range)
notify (758 B, format 2; format 1 = the first 682 B with version byte 1):
                      0 type | 1 ver | 2..5 epoch | 6..9 next_seq | 10..11 dropped |
                      12 flags (1 = estimate uncertain) | 13 zero | 6 x condition | 16 x entry |
                      diagnostics (4 B) | rejection log header (8 B) | 4 x rejection
  condition (26 B):   0 flags (1 active, 2 raise pending) | 1 reminders | 2..9 opened_at |
                      10..17 next_reminder_at (i64) | 18..21 value | 22..25 value2
  entry (32 B):       0..3 seq (0 = empty, then all zero) | 4 type | 5 severity |
                      6 condition (0xFF = one-shot) | 7 flags | 8 reminder_no | 9 zero |
                      10..11 attempts | 12..19 created_at (i64, 0 = clock unknown) | 20..21 code |
                      22..25 value | 26..29 value2 | 30..31 zero
  diagnostics (4 B):  0 tx_diag (1 AUTH, 2 ENDPOINT, 4 PROTOCOL) | 1 zero |
                      2..3 HTTP status that last set a flag (0 = none / local cause)
  log header (8 B):   0..3 total (saturating) | 4..5 lost_unread | 6 unread | 7 count (<= 4)
  rejection (16 B):   0..3 seq | 4 type | 5 entry flags | 6..7 code | 8..9 HTTP status (400..499) |
                      10 error | 11 field | 12..13 attempts | 14..15 zero (unused slots all zero)
  report keys (36 B): 0 count (<= 4) | 1..3 zero | 4 x u64 dose-bound report key (format 3)
dosing (52 B):        0 type (7) | 1 ver | 2 readiness (1 READY, 2 SERVICE_REQUIRED) | 3 flags
                      (1 BLOCKED, 2 REFILL_PENDING, 4 MANUAL_ACTIVE) | 4..7 refill capacity |
                      8..11 refill remaining | 12..15 refill seq | 16..19 manual volume |
                      20..27 manual started_at (i64) | 28..31 manual seq | 32..35 ledger date |
                      36..39 ledger sum µl | 40..47 last pump start (i64) | 48..51 next seq
                      (format 1 = these 52 B with version byte 1)
  format 2 (206 B, M1.10b.2): 52..83 manual_hw[4] (u64 each, <= 2^63 - 1) | 84 marker_slot |
                      85 ring_next (0..7) | 86..93 marker_req (u64; 0 = none) |
                      94..205 8 x result (0 slot | 1 state 1 PENDING..5 REFUSED | 2..9 seq u64 |
                      10..13 detail: volume or refusal code; an unused entry is all zero)
  format 3 (212 B, M1.10b.3): 206..209 adm_date (year u16, month, day; all zero = none) |
                      210..211 adm_count (u16): fresh manual-dose decisions counted for adm_date
                      (formats 1 and 2 decode with both zero)
auth (416 B, layout v5, M1.10b.3):
                      0 type (8) | 1 ver (1) | 2 claim_state (0 UNCLAIMED, 1 CLAIMED) | 3 flags
                      (1 KEY_FAULT) | 4..7 owner_epoch | 8..39 identity private key | 40..71
                      identity public key | 72..79 claimed_at (i64) | 80..87 recovered_at (i64) |
                      88..91 recoveries | 92..95 failed_sessions | 96.. 4 x slot (80 B)
  slot (80 B):        0 used (0/1) | 1 name_len (0..24) | 2..3 zero | 4..35 phone public key |
                      36..59 name (UTF-8, no control characters, zero padded) | 60..63 zero |
                      64..71 enrolled_at (i64) | 72..79 last_seen (i64); unused slot all zero
                      Strict validation: UNCLAIMED has no identity and no slot; CLAIMED has a
                      non-zero identity, epoch >= 1 and >= 1 used slot; used keys non-zero,
                      distinct, never the identity key; KEY_FAULT only with CLAIMED.
```

Golden byte vectors for history, configuration and schedule set (computed independently
with Python `struct`) are in `test_storage.c`.

**No application-level checksum.** NVS protects every entry and every blob chunk with
CRC32 and drops items that fail it. Explicit presence turns that into a detected
corruption, and the strict decoders catch truncated, mis-sized, foreign or newer data. An
extra CRC would only re-check what NVS already checks.

### Atomicity

| Operation | Keys written | Atomic? |
|---|---|---|
| History claim / outcome / recovery (M1.2) | one `hNN` | yes (one key) |
| Create, edit or delete a schedule | `sched` | yes: the whole set is one record (approved) |
| Future full schedule sync from Android | `sched` | yes (same record; acceptance test 8) |
| Configuration change (e.g. refill: capacity + remaining) | `config` | yes |
| Provisioning | all keys, marker last | resumable: a cut before the marker is completed at the next boot |
| Automatic dose: claim + reservoir consumption (M1.5 order) | `hNN` (claim), then `config`, then pump | **no**: two keys, ordered so that a cut is safe. The claim comes first (dedup safety). The full requested volume is subtracted **before** the pump runs, so a cut after the claim leaves the pump off and the estimate correct, and a cut after the consumption can only make the estimate too low, never too high. |
| Notification event: latch + enqueue + sequence (M1.5) | `notify` | yes: one record |
| Permanent rejection: log entry + queue removal (M1.7) | `notify` | yes: one record, written before anything else happens |
| Automatic dose (M1.9a order) | `hNN` (claim + volume), `dosing` (ledger, last start), `config` (consumption → permit), pump, `hNN` (outcome) | **no**: restricting writes first, the enabling write (permit) last; a cut can only cost a dose or make accounting stricter |
| Manual dose (M1.9a) | `dosing` (marker + ledger + last start), `config`, pump, `dosing` (marker cleared) | **no**: same rule; the marker makes the interruption recoverable |
| Interruption report (M1.9a recovery) | `notify` (event + report key), then `hNN` / `dosing` cleared | report first and keyed: a repeat is idempotent, never lost |
| Refill (M1.9a) | `dosing` (intent) → `notify` → `config` → `dosing` (latch + intent cleared) | **no**: roll-forward; dosing disabled until the last write |

**Known platform defect (found in M1.10b.1, `test/nvs_linux`):** ESP-IDF v6.1 NVS can lose a blob
key entirely on a power cut at one point of its replacement (after the last new data chunk,
before the new blob index), so "old or new value" does not hold at that instant; the storage
detects the missing key and fails closed (CORRUPT). The project's patched `nvs_flash`
([`third_party/nvs_flash`](third_party/nvs_flash/README.md)) closes that window
(0 losses in the emulator's single- and two-cut matrices). **Release and hardware-acceptance
blocker: open until bench power cuts on the ESP32-C3 pass (acceptance 9.4).**

Every write is: encode → `set` → `commit` → read back → compare bytes. Any failure
returns an error; the schedule repository then refuses changes until it re-reads the
durable set (old or new, never a mix). In the randomized test, 400 operations per mode
with injected faults and restarts always left exactly the old or the new set.

### Schedule repository

- Up to **16 schedules** (approved), each passing `dsp_schedule_validate` (M1.1 model,
  unchanged, all fields stored).
- **create:** revision 1. The ID must not be active (`ERR_DUPLICATE_ID`), in the
  tombstone ring, or present in the execution history (`ERR_ID_REUSED`).
- **update:** same ID, revision = stored + 1 (`ERR_REVISION` otherwise; optimistic
  concurrency as in the Android repository). The ID never changes.
- **delete:** expected revision must match; the ID moves to the **tombstone ring
  (16 newest deleted IDs)**. History is not touched.
- **Names (M1.10b.1, D9):** stored with each schedule (format 2), 0–32 bytes of well-formed
  UTF-8 without control characters, not normalized. Validated by `dsp_schedule_validate`
  (`DSP_SCHED_E_NAME`) with the same rule as the protocol's `text` strings (`dsp_text`).
- **Edit floor `not_before` (M1.10b.1):** maintained by the Application Service, never sent
  over BLE; occurrences scheduled before it are MISSED in the coordinator (see
  [Configuration, schedules and phone time](#configuration-schedules-and-phone-time-m110b1)).
- **Non-reuse (approved: "UUID + bounded tombstones"):** enforced against active IDs,
  the 16 most recently deleted IDs and every ID in the 32-slot history. Beyond that it rests
  on Android-generated UUIDv4 randomness. A reused ID can only suppress a dose its old
  schedule already claimed, never cause a duplicate.

### Device configuration (schema v1)

| Field | Meaning | Default | Valid |
|---|---|---|---|
| `revision` | +1 on every save (enforced) | 0 | — |
| `reservoir_capacity_ul` | configured reservoir volume | 0 (not configured) | — |
| `reservoir_remaining_ul` | **estimated** remaining volume (FR-020) | 0 | ≤ capacity |
| `low_stock_percent` | low-reservoir warning threshold (FR-021, PROPOSED 20 %) | 20 | 1–99 |
| `time_sync_interval_h` | NTP interval (confirmed: about 24–48 h) | 24 | 24–48 |
| `notify_mask` | low reservoir, low battery, device error notifications | all on | 3 bits |

Format 2 (M1.9a) adds the pump calibration and safety limits, all **0 = not configured**
(no production defaults are invented):

| Field | Meaning | Required for pump activation |
|---|---|---|
| `pump_flow_ul_per_s` | calibrated flow (FR-011) | > 0 |
| `calibrated_at` | trusted UTC of the calibration | ≠ 0, valid instant |
| `max_dose_ul` | per-dose limit (FR-016) | > 0 |
| `max_daily_ul` | per local day (Europe/Prague) | ≥ `max_dose_ul` |
| `max_runtime_ms` | per dose | > 0 and ≤ the platform's hardware cutoff |
| `min_interval_s` | between two pump starts | ≥ 1 |

The defaults mean "reservoir not configured" and "pump not configured", so no dose can run
until the user configures, calibrates, sets the limits and makes the device READY. **Not in
the configuration** (proposed for their milestones):
- battery thresholds and measurement constants;
- device identity (e.g. a device UUID for the notification relay);
- Wi-Fi SSID, the notification endpoint and alert policy.

**No secrets** are stored here: since M1.7 the Wi-Fi credentials and the device API token
live in the separate `nvs_sec` partition (developer-provisioned, read-only for the
firmware, see M1.7), never in `config`, this namespace or logs.

### Capacity, flash layout and wear

NVS entries are 32 B and blobs cost 2 entries plus 1 per 32 data bytes. History
32 × 5 = 160 entries, schedule set ≤ 101 (format 2, 3,160 B; format 1: 81), config 4 (46 B),
time 4 (M1.4, 42 B), notify 27 (M1.9a, 794 B), dosing 9 (M1.10b.3, 212 B), auth 15 (M1.10b.3,
416 B), marker 3, namespace 1: **≤ about 324 entries (measured with the real NVS:
`test/nvs_linux` check 1)**, plus up to 101 free entries while the largest record is rewritten.
The M1.7 partition table gives `nvs` 80 KiB (20 pages, about 2,500 usable entries): the
live data uses about 11 %, which leaves room for ESP-IDF's own Wi-Fi/PHY/BLE data and
spreads wear. Wear: about 10 entries per automatic dose (plus 3 for the reservoir estimate
since M1.5). Each notification change or delivery session costs 26 entries, plus 26 more for
every notification's first transmission (freeze write) and for every permanent rejection.
Even at 16 doses a day each NVS sector is erased only rarely, far inside flash endurance.

**Since M1.7** the flash size is 4 MB and the custom partition table described in
[Partition table (4 MB, M1.7)](#partition-table-4-mb-m17) applies. The `nvs` partition moved
from 0x9000 to 0x11000 and grew from 24 KiB to 80 KiB. Existing bench data is lost with the
first full flash; no field device exists.

### Schema migration strategy

**Layout v1/v2/v3 → v4 (M1.4, M1.5, M1.9a; implemented and tested):** a v1 marker (M1.3: no
`time`, `notify`, `dosing`), a v2 marker (M1.4: no `notify`, `dosing`) or a v3 marker (M1.5:
no `dosing`) with valid data is migrated. The newer keys may be absent, or hold their
provisioning value as left by an interrupted migration:
- the missing keys are provisioned (`time`, `notify`, then `dosing`);
- `dosing` is provisioned **SERVICE_REQUIRED** (never enabling dosing), with the BLOCKED
  latch set if the existing `notify` record has an active RESERVOIR_BLOCKED episode;
- the marker is rewritten as v4 **last**;
- a power cut before the marker is resumed at the next boot.

**Layout v4 → v5 (M1.10b.3; implemented, tested with the fake KV at every write and with the
real NVS cut at every flash cycle):** the v4 data is validated as before; the `auth` record is
provisioned UNCLAIMED (epoch 0) unless an interrupted migration already wrote exactly that
value; the marker is rewritten as v5 **last**. A v4 device was never claimable, so UNCLAIMED is
its true state. Any other `auth` content next to a v4 marker is CORRUPT (nothing written).
v1/v2/v3 migrate straight to v5 (`auth` provisioned before the marker).

Under a v5 marker a missing, malformed or newer `auth` does **not** fail the open: it is
classified (`st->auth_status`) and the Application Service enters CRED_DEGRADED (dosing
blocked, see the M1.10b.3 section), so recovery can still force SERVICE_REQUIRED durably; any
other damaged key is CORRUPT as before.

Record formats inside a layout: history 1 → 2, configuration 1 → 2, `notify` 1/2 → 3,
(M1.10b.1) `sched` 1 → 2, (M1.10b.2) `dosing` 1 → 2 and (M1.10b.3) `dosing` 2 → 3 are read as
before (new fields 0) and upgraded by the next write of that record; no migration step is
needed.

Without write permission the open reports `ERR_MIGRATION_REQUIRED` and writes nothing. An old
marker next to a newer key with real data is `CORRUPT`. Older firmware reading a newer layout
reports `UNSUPPORTED` (intended).

- Each record carries its own version, and the layout marker carries the layout version.
- A newer firmware keeps decoders for older versions and writes records back in its own
  format, one key at a time (each atomic). The marker is bumped **last**, so an interrupted
  migration is detectable and resumable.
- An older firmware that finds a newer version reports `ERR_UNSUPPORTED`, disables
  automatic dosing and leaves the data untouched. Downgrades are therefore not supported
  without maintenance. **Rollback limitation (R3):** firmware older than M1.9a cannot read
  layout v4 or the new record formats; a future OTA must never roll back across them.
- Changing slot counts or capacities is a layout change, which needs a migration.

### Optional boot smoke test

`CONFIG_DSP_STORAGE_SMOKE_TEST` (menu "Dispenser V2", **default off**) makes `app_main`
initialize NVS, open the `dispenser` namespace **read-only** and run `dsp_storage_open`
without provisioning, then log the status. The distinction matters:

- **The smoke test itself** is read-only at the application level. It writes no dispenser
  keys, provisions nothing and requests no erase.
- **ESP-IDF's NVS initialization** (`nvs_flash_init_partition()`, needed to open any
  namespace) is not guaranteed write-free. It may initialize blank pages, complete or
  revert an interrupted NVS operation, or erase and rewrite pages during its recovery.
  Enabling the option therefore does not guarantee zero flash writes.

The default image is unchanged. A build with the option enabled is checked, so the NVS
adapter links.

### Implemented vs not verified

- Implemented and host-tested: codec, layout and provisioning, explicit presence,
  configuration, schedule repository, history store over key-value, M1.2 coordinator on
  persistent history, fault and power-loss behavior against the fake (NVS-like and staged
  modes).
- Compiled for ESP32-C3: the NVS adapter (`dispenser_platform`), linked in the smoke-test
  build.
- **Not verified:** the NVS adapter on real flash, real power-loss behavior, NVS wear and
  GC timing, partition sizing on the actual board.

## Time management, Wi-Fi and SNTP (M1.4)

Domain (pure C11, host-tested): `dsp_timekeeping.h` (trust model, corrections, sync
schedule), `dsp_time_meta.h` (persistent metadata), `dsp_net.h` (Wi-Fi session manager,
credential provider, time source and clock port interfaces), `dsp_time_service.h` (one
bounded sync session), `dsp_wakeup.h` (next wake-up). Platform (ESP-IDF):
`dsp_esp_time.h` with clock port + continuity evidence, Wi-Fi station driver and SNTP
time source. **Implemented and host-tested; nothing has run on hardware or a real
network.** `app_main` does not start Wi-Fi (an optional diagnostic exists, off by default).

### Clock state model (approved: evidence-based, no expiry)

| State | When | Automatic dosing |
|---|---|---|
| `UNKNOWN` / never synced | no accepted sync yet | no |
| `UNKNOWN` / time base lost | power-on or brownout reset, or the RTC continuity marker is missing or does not match the last sync | no |
| `UNKNOWN` / out of range | clock before 2025-01-01 or after 2099 (e.g. ~1970 after power-on) | no |
| `UNKNOWN` / behind last sync | a "continuous" clock reads earlier than the last accepted sync | no |
| `SYNCED` | continuous since an accepted sync less than one interval ago | yes |
| `HOLDOVER` | continuous since the last accepted sync, sync overdue | **yes, no time limit** |
| rollback hold | after a backward correction > 7 days, until the clock reaches high water − 7 days | no |

- Evidence comes from the platform. The reset reason must be deep-sleep wake,
  software/panic or a watchdog reset (power-on, brownout and others lose the time base), and
  an RTC_NOINIT block (magic, version, CRC) must hold the last accepted sync time.
- **A stored timestamp is never proof of the current time.**
- A failed Wi-Fi or NTP attempt never changes trust and never stops dosing. "Sync overdue"
  (more than 2 × interval) is only a diagnostic flag for future notifications.
- `dsp_timekeeper_clock()` provides the `dsp_clock_t` for the M1.2 coordinator.

### Clock corrections (approved: hold only beyond 7 days)

An accepted NTP time is applied as an **immediate step** (`settimeofday`), not slewed:
`adjtime` slewing is interrupted by deep sleep and would leave time wrong for a long period.

| Correction (new − old clock) | Behavior |
|---|---|
| ≤ 10 min either way (`SMALL`) | accepted; dosing continues |
| forward > 10 min | accepted. **Every occurrence scheduled at or before the step instant is missed**, even inside the due window, so a slow RTC can never produce a second dose right after the correction. Implemented as a persisted, **monotonic** dosing floor (`time.not_before_utc` → `dsp_clock_t.not_before`): never lowered by later syncs, it survives restarts, deep sleep, power loss with re-synchronization and repeated corrections. |
| backward ≤ 7 days | accepted; the M1.2 watermark blocks every already claimed date |
| backward > 7 days | accepted, **automatic dosing held** until the clock reaches (highest previously trusted time − 7 days). The hold is persisted, and a later sync that jumps past it ends it. A schedule claimed under a too-early "future" date additionally stays blocked by its watermark until that date (missed, never doubled). |
| initial sync (previous clock untrusted) | accepted; floor raised to max(floor, synchronized time + 1 s): occurrences at or before re-establishing time never run |
| result before 2025-01-01 / after 2099 | rejected (`DSP_SYNC_ERR_IMPLAUSIBLE`), never applied |

Historical occurrence identities never move. Missed occurrences are never retried. Manual
dosing is unaffected by holds.

**Initial synchronization (approved 2026-11-15).** When time is re-established after the
clock was untrusted (first boot, power loss, lost continuity), the floor is raised to
`max(floor, synchronized time + 1 s)`. So nothing scheduled at or before that moment runs,
because the device cannot know what it did before under an unverifiable clock. The new
floor is persisted and confirmed before the time is applied; on a write failure nothing is
applied. Cost: up to one missed dose per schedule when time comes back shortly after a dose
time. This is intentional: a missed dose is preferable to a possible extra one.

### Synchronization policy

- Interval 24 h by default, configurable 24–48 h (configuration v1 `time_sync_interval_h`).
- One session at a time:
  1. decide whether a sync is due;
  2. Wi-Fi up only then, bounded;
  3. SNTP once, with a 10 s timeout by default;
  4. **durable before effective:** write the new metadata (last sync, high water, hold,
     floor) and confirm it, and only then step the clock and refresh the continuity marker.
     If the write fails, nothing is applied (`DSP_SYNC_ERR_PERSIST`, trust unchanged, retry
     scheduled), so a restart can never re-synchronize from stale metadata and lose the floor
     or the hold;
  5. persist any other changed metadata (e.g. an expired hold);
  6. Wi-Fi down, unless the caller keeps the session for future notifications.
- Retry after a failure, with the trust state unchanged:
  - clock untrusted: 15 min, doubling up to 6 h;
  - clock trusted: 1 h, doubling up to the sync interval.

  The retry state lives in RTC memory and is lost on power-on, which simply means "try
  now".
- SNTP: `esp_netif_sntp` with one server (Kconfig `DSP_SNTP_SERVER`, default
  `pool.ntp.org`). The weak `sntp_sync_time()` hook is overridden, so SNTP never sets the
  clock itself; the override was verified in the link map.
- Recommended dosing-service order at a wake (used in the tests): evaluate trust → execute
  due doses → sync if due → execute doses that just became possible. A routine sync then
  never delays a dose.

### Wi-Fi lifecycle

`DISABLED → STARTING → CONNECTING → CONNECTED → (shutdown) DISABLED`; any failure ends in
`FAILED` with the radio already stopped.
- **Attempts:** at most 1–5 per session (default 3), each bounded at 1–60 s (default 15 s). An
  authentication failure ends the session at once. There is no reconnect loop and no
  permanent connection.
- **Shared sessions:** a connected session can be reused (NTP, then future HTTPS
  notifications), and `dsp_wifi_shutdown()` ends it.
- **Credentials (approved: interface only):** `dsp_wifi_credentials_provider_t` with
  `DSP_CRED_UNAVAILABLE`. The only provider today is `DSP_WIFI_NO_CREDENTIALS`, so the radio
  never starts. Credentials live in RAM for one connection attempt and are then wiped. The
  driver uses `WIFI_STORAGE_RAM`: by default ESP-IDF would store the SSID and password in
  plain NVS. Credentials are never logged, and there are none in the repository.
- **Production credential storage** is designed with BLE provisioning (proposal: a separate
  namespace with NVS encryption, which needs an eFuse/HMAC key decision).

### Persistent time metadata (`time` record, since layout v2)

`last_sync_utc`, `high_water_utc`, `hold_until_utc`, `last_correction_s`. Written only
when they change: after an accepted sync (≤ once per interval) and when a hold starts or
ends; nothing per wake. A failed write keeps the metadata "dirty" and retries at the next
session, and dosing continues. The volatile retry state lives in RTC memory, not NVS.

### Wake-up planning (interface for the future power manager)

`dsp_wake_plan()` returns the earliest of next dose, next sync and "other" (there is no
notification reason: approved D1-B, M1.7 — a wake-up is never scheduled for a
notification), as a sleep duration relative to the raw clock, capped by `max_sleep_s`. Ties go
to the dose. `dsp_wake_next_dose()` uses the recurrence engine only while the clock is
valid. While it is unknown, only the sync is scheduled (the RTC still counts since
power-on). **No timing guarantee** is made: wake-up latency and RTC drift in deep sleep
must be measured on the board. The 10-minute due window absorbs moderate lateness; an
early wake simply sleeps again.

### Resources and power

- Domain M1.4 code: about 3.1 KB (`-Os`). RAM: timekeeper 80 B, Wi-Fi manager 28 B,
  service 24 B, RTC block 40 B. Largest stack frames 128 B.
- Platform adapters: about 2.3 KB. The ESP-IDF Wi-Fi/LwIP stack dominates. With the
  diagnostic enabled (Wi-Fi + SNTP linked) the image was 0xce010 bytes (about 824 KiB) in
  M1.4, which no longer fitted BLE and TLS in the old 1 MB partition; M1.7 introduced the
  4 MB table with 1.75 MiB slots (current sizes: see the M1.7 section).
- Wi-Fi heap is used only while a session is active (`esp_wifi_init` / `deinit` per session).
  Energy per sync (connect plus SNTP, typically a few seconds at Wi-Fi current) must be
  measured; at one sync per 24–48 h it should be small next to deep-sleep current. This
  is an assumption, not a measurement.

### Hardware validation still required

- RTC continuity per reset type: does the RTC time survive software/watchdog resets and get
  lost on power-on and brownout as assumed?
- RTC_NOINIT behavior.
- Deep-sleep RTC drift with the internal oscillator (the XIAO ESP32-C3 has no 32 kHz crystal,
  to be confirmed). Drift sets how large routine corrections are; corrections above 10 min
  forward skip a dose.
- Real Wi-Fi association and timing, SNTP over the network, failure reasons, energy per
  session, and the `sntp_sync_time` override at run time.

## Notification engine (M1.5)

Pure domain code (`dsp_notify`, `dsp_notify_state`, `dsp_reservoir`): it detects
conditions, creates deduplicated events, schedules reminders, keeps the bounded persistent
queue and hands pending notifications to an injected transport. Since M1.7 the HTTPS
transport and the connectivity session exist (see the M1.7 section); there is still no
battery measurement, and nothing calls the engine from `app_main`.

### Events

| Type | Severity | Kind | Payload |
|---|---|---|---|
| `RESERVOIR_LOW` | WARNING | condition: estimate below `low_stock_percent` | value = estimated µl, value2 = capacity µl |
| `RESERVOIR_BLOCKED` | CRITICAL | condition: FR-022 (the estimate does not cover a dose) | same |
| `BATTERY_LOW` | WARNING | condition from a future battery monitor (`OK`/`LOW`/`UNKNOWN`) | value = mV (0 = not provided) |
| `DOSING_ERROR` | CRITICAL | **one-shot** event | code = start failed / pump failed / interrupted / storage / blocked; value = requested µl; flag MANUAL |
| `DEVICE_ERROR` | CRITICAL | condition (storage, sync overdue, clock UNKNOWN with ≥ 4 failed syncs; a trusted HOLDOVER clock never raises it and stays usable for dosing) or one-shot `QUEUE_OVERFLOW` | code; value = detail or number of lost events |

Entry flags: `REMINDER`, `TIME_VALID` (created_at is trusted UTC; otherwise 0),
`UNCERTAIN` (reservoir estimate uncertain), `MANUAL`.

### Approved policy (2026-11-15, confirmed in the M1.5 review)

- **Conditions and deduplication:** a condition opens an episode once (one event). Repeated
  observations do not create events and do not write flash (approved: RAM-only payload
  updates). The episode closes on resolution, and its undelivered notifications are
  withdrawn (stale). A new episode later gets a new ID.
- **Payload freshness (M1.5 review):** a notification reports the **latest known value
  at its first transmission attempt**, not necessarily the value that opened the episode.
  - Until the first attempt, observations of the still active condition update the queued
    entry in RAM: LOW at 19.99 %, then an observation at 8 %, then delivery transmits 8 %.
  - **Freeze is durable before the first send.** Before an entry is transmitted for the
    first time, its final payload and the freeze marker (`attempts > 0`; `attempts`
    counts started attempts) are written to flash in one write. If that write fails,
    nothing is sent (`ERR_STORAGE`, zero transport calls).
  - From then on the payload is **frozen**: every retry with the same ID carries identical
    content, also after a power cut between the receiver accepting the event and the
    result being saved. Newer values reach the receiver with the next reminder, which is
    a new event with a new ID.
  - Cost: one extra `notify` write (758 B since M1.7) per notification, at its first
    transmission.
  - A restart before the first attempt loses the RAM-only refresh. The entry then carries
    its last persisted value until the caller's next observation refreshes it.
  - Flags (`UNCERTAIN`) are fixed when the entry is created.
- **Reservoir hysteresis (approved):** LOW opens when `remaining × 100 < capacity × threshold`
  and closes at `threshold + 5` points (capped at 100 %). Capacity 0 means unconfigured:
  unknown, never low.
- **Reminders:** one every **7 days** while a condition is unresolved (reservoir LOW and
  BLOCKED, battery LOW, persistent device errors). They are trusted-clock only, and the
  next one is always scheduled from now: a forward jump gives one reminder, not a burst,
  and a backward correction pulls the schedule in to at most 7 days. Dosing errors are
  never reminded.
- **Queue:** 16 entries in one atomic record. A new event goes, in order of preference:
  - into the same condition's undelivered entry (the newer event replaces it);
  - into a free slot;
  - in place of the oldest entry of lower severity.

  **CRITICAL entries are never evicted.** If nothing fits, a condition stays latched as
  "raise pending" (raised when space frees, with its reminder number). One-shot events are
  counted and reported later as one `DEVICE_ERROR/QUEUE_OVERFLOW` with the count. Nothing
  is lost silently.
- **Delivery (at-least-once):** `dsp_notify_deliver()` runs over an already open Wi-Fi
  session. It sends the highest severity first, then the oldest, and removes an entry
  **only on explicit ACK** (or, since M1.7, after a permanent rejection has been logged in
  the same write). `FAILED` or `UNCERTAIN` (for example a timeout after sending)
  keeps the entry with the **same ID** and `attempts + 1`, and ends the session. The
  receiver deduplicates by ID.
- **Durable before sent:** nothing is transmitted unless the queue state is in flash, so a
  sequence number can never be reused for different content after a restart. An ACK
  whose removal is lost (power cut) causes a re-send with the same ID.
- **No extra wake-ups:** `dsp_notify_delivery_requested()` never causes a wake-up. Since
  M1.7 (approved D1-B) a pending CRITICAL notification may open Wi-Fi during an existing
  wake (at most once per hour, respecting the Wi-Fi backoff); WARNING notifications wait
  for the regular sync session. The wake planner has no notification input any more.

### Stable event IDs

`dsp-<device 12 hex>-<epoch 8 hex>-<seq>`, for example `dsp-0123456789ab-a5a5a5a5-42`.
- `epoch`: a random value from the platform RNG, assigned with the first event and then
  persisted.
- `seq`: a persisted monotonic counter, never reused within the epoch.
- `device`: the factory MAC (48 bits, approved), supplied by the platform. It identifies,
  it **never authenticates**: it is not a secret and must not be used as one.

A storage reset gives a new epoch, so old and new IDs cannot collide at the receiver.

### Reservoir accounting (approved: conservative)

The remaining volume is an **estimate from dosing consumption**; there is no level sensor
(FR-006/FR-020).
- `dsp_reservoir_consume()` subtracts the **full requested volume** of every authorized
  dose, automatic or **manual**, whatever the outcome; it saturates at 0 and bumps the
  configuration revision.
- **Consume before pump is part of the API contract, not just documentation.**
  - A `dsp_dose_permit_t` is issued only by `dsp_reservoir_consume()`, and only after the
    reduced estimate is durable. On any error it is cleared.
  - `dsp_dose_permit_take()` redeems a permit exactly once.
  - The future actuator interface must accept nothing but a redeemed permit, so the pump
    cannot run for a volume that was not subtracted. The actuator does not exist yet; its
    milestone must keep this rule.
- **Uncertainty is set by the dosing error itself.** `dsp_notify_report_dosing_error()`
  with `START_FAILED`, `PUMP_FAILED` or `INTERRUPTED` (the volume was already consumed)
  sets the persistent uncertainty flag in the **same write** as the event, also when the
  queue is full. `STORAGE` and `BLOCKED` (nothing consumed) do not set it. Reservoir
  events then carry `UNCERTAIN`.
- **Refill clears it:** `dsp_reservoir_refill()` (capacity and remaining, FR-023), then
  `dsp_notify_on_refill()` clears the flag and the BLOCKED state and re-evaluates LOW, in
  one write. If power is lost between the two calls, the flag remains set (conservative)
  until the next refill.
- `dsp_reservoir_covers()` is the FR-022 check: configured, and the estimate covers the
  full dose.

### Persistence and failures

All engine state (epoch, next sequence, drop counter, uncertainty flag, 6 condition latches
with reminder schedules, 16 entries; since M1.7 also the transport diagnostics and the
rejection log) is the `notify` record (758 B), written only when something changes.
- **A failed write never leads to transmitting a non-durable event.**
  - The state stays in RAM, marked dirty.
  - `dsp_notify_deliver()` first writes any dirty state, then freezes each entry durably
    before its first transmission. If either write fails it sends **nothing more**.
  - Every later call retries the write.
  - The transport fake in the tests checks, at every send, that the entry is in flash,
    already frozen, with exactly the content being sent.
- **Observable by the caller:**
  - Every call that cannot persist returns `DSP_NOTIFY_ERR_STORAGE`; the delivery report
    carries the same status.
  - `dsp_notify_storage_degraded()` stays true until a write succeeds, or while the record
    could not be loaded.
  - If the record cannot be loaded at boot, the engine refuses to run (`ERR_STORAGE`); it
    never assumes an empty queue.
- **When the `notify` record itself is the failing storage** (approved for M1.5), the
  error cannot be queued durably and is only logged by the future integration.
  **Requirement for a later milestone:** the BLE diagnostics must expose this state
  (`dsp_notify_storage_degraded()`, together with the other storage statuses), so that the
  user can see a notification failure that could not be notified.

### Resources

- Code (`-Os`, RISC-V): `dsp_notify` 4.2 KB (M1.5), `dsp_notify_state` 0.9 KB,
  `dsp_reservoir` 0.4 KB. None of it is linked into the default image yet, because nothing
  calls it (current sizes: see the M1.7 section).
- RAM (32-bit): `dsp_notify_t` was 864 B in M1.5 and grows by the M1.7 diagnostics and
  rejection log (about +76 B); caller-owned. No heap, no globals; every loop is bounded by
  16 entries × 6 conditions (and 4 log entries).

## Dosing Service (M1.9a)

Pure domain code (`dsp_dosing`, `dsp_dosing_state`) over the M1.2 coordinator, M1.3 storage,
M1.5 reservoir accounting and notification engine. It is the **sole owner of the pump port**
(`dsp_pump_port_t`: `run(runtime_ms)` and `force_off()`); nothing else can reach the pump.
**There is no real pump driver yet** (GPIO, hardware cutoff and pin map are hardware-review
items); the host tests use a fake pump. Nothing calls the service from `app_main`.

### Gates (all must be open for any pump activation)

| Gate | Opened by |
|---|---|
| dosing record loaded, no storage failure earlier in this wake (DEGRADED) | a clean boot |
| readiness READY (a new or migrated record is **SERVICE_REQUIRED**) | `dsp_dosing_set_readiness(READY)`, which re-checks every gate below except the clock; the Application Service first stores the READY dosing floor (below) |
| BLOCKED latch clear (FR-007/022; also blocks manual doses, Q7) | a completed refill only |
| no refill pending, no manual marker (recovery not yet run) | `dsp_dosing_recover()` |
| trusted clock | M1.4 timekeeper |
| configuration format 2 with valid calibration and limits | the user (future BLE); missing values are a diagnostic, never BLOCKED |
| configured reservoir (capacity > 0) | the user; never BLOCKED |

### Limits (overflow-safe 64-bit arithmetic)

- **Per dose:** volume ≤ `max_dose_ul`; runtime = ⌈volume × 1000 / flow⌉ ms ≤ `max_runtime_ms`
  ≤ the platform's hardware cutoff (passed at init; 0 = unknown = never actuates).
- **Daily:** per Europe/Prague local date of the trusted clock. The ledger keeps the
  **highest** local date seen: midnight starts a new budget only when the local date is
  greater; a clock moved back to an earlier date keeps counting against the highest date
  (never a fresh budget). DST days (23/25 h) are simply one local date.
- **Interval:** `now ≥ last_start + min_interval_s`; a clock behind the last start waits.
  `last_start` is durable, so the cooldown survives restarts and power loss. It is measured
  **between pump starts**, not from the end of the previous run: with `min_interval_s` below the
  runtime, the next dose can start right after the previous one ends (bench 2026-11-20,
  `min_interval_s` = 1: an 8 s and a 5 s pulse 40 ms apart, starts 8.04 s apart; correct).
  **Design decision for the real pump driver (open, not implemented):** a real pump may need a
  cooldown measured from the end of the previous run (motor and supply recovery, flow settling);
  decide with the driver hardware review whether to add a separate `min_cooldown_s` from the run's
  end, or to validate `min_interval_s ≥ max_runtime + cooldown`. The current semantics are unchanged.

### READY dosing floor (incident 2026-11-20, F1)

Entering READY never releases occurrences that fell due while the device was NOT READY. On the
bench, "Nuli" (8 ml, 17:17) and "Bar" (5 ml, 17:20) were refused at their times (NOT READY), stayed
inside their 10-minute due window, and ran at 17:20:22 right after the owner switched to READY.
Now `dsp_app_set_readiness` handles NOT READY → READY as follows:

1. The gates are checked first (`dsp_dosing_ready_gates`); a refused READY writes nothing.
2. With a trusted time, the timekeeper's durable dosing floor (`time.not_before_utc`, the same
   floor a large forward clock step raises) becomes the transition instant + 1 s: written,
   committed and read back. If that write fails: `STORAGE`, READY is **not** entered.
3. Only then is READY written to the dosing record.

Boundary: an occurrence scheduled at or before the transition instant (also in the same second or
minute, e.g. READY at 08:20:00 → the 08:20 occurrence) is MISSED; at 08:19:59 the 08:20 occurrence
runs. Later occurrences of the same day and of every following day are unaffected. Power loss: after
step 2 the device is NOT READY with a raised floor (harmless); READY is never durable with an older
floor (tested by a cut at every write). Without a trusted time no floor is written: no automatic dose
runs before the first trusted time, which raises the floor to the sync instant + 1 s anyway. Other
consumers of the floor are unchanged (clock corrections only raise it; the per-schedule edit floors,
anti-replay records and manual doses are not involved). READY → READY and SERVICE_REQUIRED never touch
it. Tests: `test/host/test_ready_floor.c` (the incident fails without the fix: two doses right after
READY), Android firmware-in-the-loop `BenchPumpFirmwareTest`.
- **Untrusted clock:** no activation, ledger untouched.

### Write order (approved: restricting before enabling)

| Dose | Writes |
|---|---|
| automatic | claim `hNN` (STARTED, **claimed volume**) → `dosing` (ledger, last start) → `config` (consumption → single-use permit) → permit redeemed → pump → fault report (if any) → `hNN` outcome |
| manual | `dosing` (marker {volume, start, seq} + ledger + last start) → `config` (permit) → pump → fault report → `dosing` (marker cleared) |
| uncovered automatic dose | `dosing` (BLOCKED latch) → `notify` (RESERVOIR_BLOCKED); nothing claimed |

A power cut can only cost a dose or make the accounting stricter. The fake pump checks at
every activation that the claim or marker (with the volume), the ledger, the last start and
the consumption of exactly that volume are **durable**.

### Refill (restart-safe roll-forward)

`dosing` (intent: target capacity/remaining) → `notify` (`dsp_notify_on_refill`: uncertainty
cleared, BLOCKED closed, LOW re-evaluated) → `config` (estimate; skipped if already equal) →
`dosing` (latch and intent cleared together). Dosing is enabled only after the last write;
a cut anywhere leaves the intent pending (dosing disabled) and `dsp_dosing_recover()`
completes it (every step idempotent).

### Recovery (idempotent)

At boot, `dsp_dosing_recover()`: completes a pending refill; then for every interrupted
automatic dose (history STARTED) and an active manual marker, **reports** `DOSING_ERROR /
INTERRUPTED` with the **durable claimed volume** (format-1 records: 0 = unknown, never
reconstructed from the current schedule) and only then rewrites the record / clears the
marker. Reports are keyed per dose (`dsp_notify_report_dosing_error_once`, `notify` format 3
keeps the last 4 keys, written in the same atomic write as the event), so a recovery repeated
after a power loss never loses a report and never queues a second one. Recovery never calls
the pump and never changes the reservoir estimate.

### Storage failures and lost reports (limitation)

- **Dosing disabled for the wake:** any storage failure inside the service (including a
  `notify` write that fails while reporting a dose error) closes the `DEGRADED` gate until
  the next boot reloads the state. Neither automatic nor manual doses run.
- **Never repeated:** a claimed occurrence is never executed again (STARTED, FAILED and
  INTERRUPTED all block it), and a manual dose is never re-issued by the firmware.
- **Durable source kept:** if a dose-bound report (pump fault, storage error after the claim)
  cannot be persisted, the service **keeps** the dose's STARTED record or manual marker
  instead of finishing or clearing it. The next boot then reports the dose as
  `INTERRUPTED`, with its durable volume, exactly once. If the RAM copy of the report is
  flushed by a later `notify` write in the same wake, its key is durable and recovery adds
  nothing. Either way exactly one report results.
- **Not recoverable:** a report whose dose left **no** durable source is held only in RAM
  and is lost at a restart. This covers a claim or manual-marker write that failed before
  anything restricting was written: no pump run, no estimate change. Recovery does not
  invent a report in that case. The only remaining signal is the storage condition, once a
  `notify` write succeeds, plus the missing history entry. A persistent event journal is a
  later milestone (Q10).
- **Conservative over-report:** a dose whose final write failed (`hNN` outcome or marker
  clear), or whose fault report could not be persisted, is reported as `INTERRUPTED` at the
  next boot even if it completed or never pumped. The `INTERRUPTED` report sets the
  uncertainty flag.

### Hardware dependencies (not decidable on the host)

Limit values (pump characterization), the hardware cutoff mechanism and its maximum, the gate
pin and its reset/brownout/deep-sleep behavior, watchdog timeouts relative to the cutoff, and
the brownout threshold under motor inrush. Until then provisioning keeps every limit at 0, so
no pump can be activated.

### Same-time schedules

Two schedules due at the same time run one after another only if `min_interval_s` allows it;
otherwise the second is refused (LIMIT, **not claimed**) and the Application Service (M1.9b)
re-evaluates it after the interval, inside its due window (`dsp_dosing_next_due`). Once the window has passed, the
coordinator reports it as `MISSED` permanently: never dosed later, never caught up, and the
same after restarts (tested). M1.9a writes no record for a missed occurrence; MISSED is
derived from the occurrence time. A durable missed-occurrence event belongs to the event
journal (Q10).

## Event history (Phase C)

User-visible chronological history of what the device did (`dsp_evlog`, keys `ev00`…`ev63`), read
with GET_HISTORY (docs/BLE_PROTOCOL.md 7.6, 10.7). It is **informational** and kept apart from every
safety record: the anti-replay history `h00`…`h31` (one watermark per schedule, see "Execution
history and deduplication") is untouched, and nothing in the dosing path reads the log.

### Events

| Type | When (appended only after the transition is durable) | Data |
|---|---|---|
| DOSE_SCHEDULED | `dsp_exec_finish` stored COMPLETED/FAILED; or boot recovery rewrote a STARTED claim as INTERRUPTED | schedule ID, requested volume, claim time, result, estimate before/after (unknown for INTERRUPTED and a failed consumption) |
| DOSE_MANUAL | the completion write cleared the manual marker; or recovery cleared it as INTERRUPTED | volume, start time, result, estimate before/after |
| RESERVOIR_INIT / REFILL / ADJUST | a refill completed (R4 of the roll-forward), or SET_CONFIG saved a new capacity | estimate before/after, capacity; INIT when the capacity was 0, ADJUST when the capacity changed or the remaining amount did not increase, REFILL otherwise; before unknown when a power cut fell between R3 and R4 |

Results: OK (the pump port ran the calculated runtime: no flow sensing, never a measurement),
PUMP_FAILED, START_FAILED, INTERRUPTED (reset/power loss after the claim), ABORTED (a storage failure
after the claim; the pump never ran). Flags: trusted clock, reservoir unknown, and the build's pump
kind: `DSP_EVF_PUMP_SIMULATED` (default build, nothing actuated) or `DSP_EVF_PUMP_BENCH_GPIO`
(`CONFIG_DSP_PUMP_GPIO`: control signal only, no liquid verified), set by `main.c` through
`dsp_app_config_t.event_pump_flags`. Refused or skipped doses (gates, limits, missed, already
claimed) are not events (they never started); see "Secondary diagnostics" below for that gap.

### Storage and safety

- One event per NVS key, slot = `seq % 64`; an append is one `set` + `commit` + read-back compare
  (single-key atomicity: after a power loss the slot holds the old or the new event, never a mix).
  Wraparound overwrites the oldest event.
- **Event identity (`seq`), Phase C review 2026-11-20:** `seq` is the identity, the phone's
  de-duplication key and the paging cursor, and is never handed out twice. Revision 0f843ec derived
  the head from the readable slots only, so a newest event dropped by NVS (before or after a
  wraparound) or the loss of every slot let the next event take an old `seq`
  (`test_seq_never_reused_after_lost_slots` failed 3 of 3 cases there). Now the key `evhw` holds a
  durable upper bound of every `seq` used: before each append the bound is raised to the new `seq`
  (5-byte blob, set + commit + read-back) and only then is the event written; at boot the head is
  the bound. If the bound itself is missing, damaged or behind a readable event while events exist,
  the head is the highest readable `seq` + 64 + 1. A failed reservation writes no event and uses no
  `seq`; a failed event write after a confirmed reservation leaves a gap. Gaps are allowed, duplicates
  never. One `seq` per reservation (not blocks): with slot = `seq % 64`, block gaps would shrink the
  retention when wakes log one event each, and the first append of a wake writes the bound anyway.
  **Residual (not closable with this storage):** if all 64 event keys AND `evhw` vanish at once (an
  erased or destroyed namespace), that state is indistinguishable from a never-used log and the log
  restarts at 1. The phone keeps no history copy, merges rows by the whole event (two different
  events with an equal `seq` never hide each other) and resets its list when the device's newest
  `seq` drops below what it shows, so no event can disappear from the app; device-side paging stays
  consistent because the device never holds two events with the same `seq`.
  **Decision (owner, 2026-11-20): R1 is ACCEPTED FOR BENCH TESTING ONLY** and remains an open
  production-design consideration (options: a second copy of the bound, a log epoch in a later
  protocol minor). The history is a best-effort user-facing record, not a complete audit trail;
  the dosing anti-replay protection stays independent of it and unchanged.
- **Persistence order (as implemented):** the transition (automatic: `dsp_exec_finish`; manual: the
  completion write that clears the marker; recovery: `dsp_exec_mark_interrupted` / the marker clear;
  refill: R4; SET_CONFIG: the config save) is durable first, then the reservation, then the event.
  A power cut in between loses the event (a window of two small NVS commits) and never repeats it:
  the transition is finished, so recovery does not see it again. Each event also carries a
  deduplication key (dose-bound report key, `RF`|refill intent seq, `CF`|config revision); an append
  whose key is still in the log is a no-op. An event never claims a change that is not durable
  (tests: power cut at every write of a dose wake, a manual dose and a refill). **The history is
  therefore not a complete audit trail:** an event can be missing after a power loss or a storage
  failure, and refused, blocked or skipped doses are never events.
- **Failures never touch dosing:** an append failure (write, commit, read-back, NVS full) is counted
  (`faults`, RAM only, reported by GET_HISTORY as FAULTS in that wake; a failure in an earlier wake
  shows only as a missing event) and changes nothing else: no gate, no DEGRADED, no decision depends
  on the log, the anti-replay records are never read or written by it. Tests with every history write
  failing (write error and NO_SPACE): doses, refill and configuration still persist; ownership,
  schedules and the reservoir stay intact; no dose repeats. A corrupt or foreign slot is ignored at
  open (counted) and never lowers the head.
- **No migration:** the keys are outside the layout's explicit-presence rule (missing = empty), so
  the layout stays v5 and older firmware ignores them. Nothing existing is erased or repurposed.
  Doses from before this firmware are not reconstructed from the anti-replay records (those hold only
  the last claim per schedule, and inventing events is not allowed).

### Capacity and wear

An event is 41 + ID bytes (≤ 105 B with a 64-character ID; 77 B with a UUID): at most 6 NVS entries
(4 data + header + index); `evhw` is 3 entries; the full log ≤ 387 entries. Keys `ev00`…`ev63` and
`evhw` differ from every layout key (`layout`, `h00`…`h31`, `sched`, `config`, `time`, `notify`,
`dosing`, `auth`); `nvs_sec` is a separate partition.

Measured on the real ESP-IDF v6.1 NVS over the emulated flash (`test/nvs_linux`, 2026-11-20, every
layout record at its maximum incl. a full schedule set, ownership with 4 phones, calibration, PHY/Wi-Fi
co-tenants): namespace 711 entries (layout 324 + log with reservation 387); partition 2,520 entries,
used 849, available 1,545; steady-state reclamation needs 140 available entries (check 4): margin
1,405. 20,000 maximum-size appends (312 wraparounds) with a power cycle every 2,000: used entries
constant, `seq` strictly increasing across every reopen, 0.071 page erases per append, wear spread
(per page max 110). A full partition refuses only the write that does not fit; dosing safety does
not depend on the log (see above). Not yet measured on the device's flash.

Wear estimate. Measured: 0.071 page erases per history append (reservation + maximum-size event,
check 2b), spread with a worst/mean page ratio of 1.54 (110 vs 71.4 erases). Estimated, not measured:
the safety writes of one dose (claim, dosing state, configuration, finish: about 23 NVS entries,
≈ 740 B) cost about 0.18 erases by the same bytes-per-erase rate, so a dose with its event ≈ 0.25
erases, of which the history is ≈ 29 % (≈ +40 % flash bytes per dose; an earlier "under 10 %" was
wrong). The partition has 20 pages; ESP32-C3 flash is rated for 100,000 erase cycles per sector.

| Events per day (each a dose) | History only: erases/day (per page, worst page) | Dose + history: erases/day (per page, worst page) | Worst page per year (dose + history) |
|---|---|---|---|
| 1 | 0.071 (0.0035, 0.0055) | 0.25 (0.013, 0.019) | ≈ 7 |
| 3 | 0.21 (0.011, 0.016) | 0.76 (0.038, 0.058) | ≈ 21 |
| 10 | 0.71 (0.036, 0.055) | 2.5 (0.13, 0.19) | ≈ 71 |

The earlier single figure "≈ 0.05 erases per page per day" assumed 3 doses and 1 reservoir change per
day, counted dose and history writes together, and was the mean over the pages (3 × 0.25 + ≈ 0.15 ≈
0.9 erases/day ÷ 20 pages). Not counted: wake writes outside doses (time metadata, notification state
after a sync), which the history does not change. Even 10 doses a day leave the worst page at ≈ 71
cycles per year (> 1,000 years to the rating): no material endurance concern.
Retention: 64 events ≈ 3 weeks at three doses a day.

### Tests

`test/host/test_evlog.c` (codec golden bytes and malformed records, paging, wraparound, restart,
duplicates, reservation and event-write failures, power cut, corruption, `seq` never reused after a
lost newest slot / all slots / a lost or damaged reservation, repeated wraparound across boots,
arguments) and
`test/host/test_history_app.c` (through the Application Service: dose and reservoir events with the
estimate, pump fault, manual dose, adjustment, persistence across reboots; automatic and manual pump
power cuts logged once as INTERRUPTED across repeated reboots; a power cut at every write of a dose
wake never duplicates an event nor logs OK without a pump run; a failing history never blocks a dose
or makes the wake DEGRADED; history writes failing with write errors or NO_SPACE leave ownership,
schedules and reservoir intact; power cuts at every write of a manual dose and of a refill; GET_HISTORY
paging, empty page, FORBIDDEN, invalid `max`). Android: firmware
in the loop over the simulator (`DeviceRepositoryFirmwareTest`).

### Secondary diagnostics (proposal, not implemented)

The bench session showed that the owner could not tell why a dose did not happen. Already available
over BLE: device clock and trust (Status `device_utc`, `clock_trust`, `clock_reason`), last
synchronization (Diagnostics `last_sync_utc`), next due dose (Status `next_due_known/next_due_at`).
Proposal: (1) show these three in the app's device status screen (Android only, no protocol change);
(2) a "last skipped dose" record — schedule ID, occurrence, reason (gate bits, limit bits, MISSED,
ALREADY_CLAIMED, NOT_COVERED) — kept in RAM/RTC per wake and exposed as a new history event type
(DOSE_SKIPPED, one per occurrence, deduplicated by occurrence identity) in a later minor revision;
this needs a protocol enumeration extension (`type` 6) and care not to log every re-evaluation.

## Application and Power Services (M1.9b)

Pure domain code (`dsp_app`, `dsp_power`, small additions to `dsp_dosing` and `dsp_notify`):
**implemented and host-tested only.** Nothing calls it from `app_main` yet. There is no deep
sleep, no RTC memory adapter, no button, no BLE stack and no pump driver: the platform side
is injected ports, and the M1.9c adapters (and later the BLE milestone) supply them.

### Wake cycle (one owner, run to completion)

`dsp_app_run_wake()` is called once per wake (every deep-sleep wake is a reset on the
ESP32-C3). One task, static memory, no allocation; every domain service is called only from
it.

| Stage | What happens | On failure |
|---|---|---|
| SAFE_OFF | the pump port is passed to `dsp_dosing_port_safe_off()`: `force_off` plus the `is_off` confirmation | unconfirmed: nothing else runs; straight to SHUTDOWN, which fails safe |
| LOAD | storage open (provisions on first boot), schedules, configuration, time metadata, timekeeper (with the retained retry state), coordinator, notification engine, Dosing Service | **DEGRADED** (Q12): no recovery, dosing, Wi-Fi or notification transmission; read-only service session on a button wake; plan = cap; the retained block is preserved |
| RECOVER | `dsp_dosing_recover()` (refill roll-forward, interrupted automatic and manual doses, keyed reports) | continues; dosing gates stay closed where state is not durable |
| CLOCK | `dsp_timekeeper_evaluate()` (continuity, plausibility, rollback hold) | untrusted clock: no doses |
| OBSERVE | time conditions, reminders and deferred events, reservoir with the BLOCKED latch | — |
| DOSE | `dsp_dosing_process_due()` while `dsp_dosing_next_due()` says something is due now (bounded passes, stops without progress) | a pump fault or storage failure ends dosing for the wake |
| CONNECT | `dsp_connectivity_run()` with the guard {no dose running, no permit, BLE inactive, next dose incl. a same-time dose waiting for the interval}; DOSE runs again after a session that synchronized | Wi-Fi failure: backoff; never touches history or accounting |
| BLE | button wake only: service session (below); CONNECT once more if commands were processed | — |
| SYNC | retries a dirty `notify` record (`dsp_notify_flush`) and dirty time metadata | stays dirty; retried next wake |
| PLAN | `dsp_power_plan()`; a dose that became due meanwhile runs now (at most 2 rounds) instead of a zero-length sleep | minimum sleep 1 s |
| SHUTDOWN | checklist (pump OFF confirmed, HTTPS ended, Wi-Fi `DISABLED`, BLE confirmed stopped), retried 3 times; RTC block saved | **fail-safe** instead of sleep |
| SLEEP / FAIL_SAFE | `sleep(sleep_s)`, or `fail_safe(failures)` | — |

**Dosing priority.** Due doses run before CONNECT; CONNECT is skipped when the next dose is
within the session's worst case plus margin; a BLE session is suspended before every pump run
(scheduled or manual) and resumed if its window remains. The application never holds or calls
the pump: doses only through `dsp_dosing_*`, so every pump start still needs the durable
claim or marker and a redeemed single-use permit. Wi-Fi, BLE and the pump are never active at
the same time (stages are sequential; the host fakes assert it on every start).

### Sleep plan (Power Service)

- **Inputs:** next dose (`dsp_dosing_next_due`: an unclaimed occurrence still inside its
  window that only the minimum interval holds back, else the next future occurrence), next
  time sync (`dsp_timekeeper_next_sync`, incl. its backoff), and the re-evaluation cap
  (configurable, **default 6 h**, clamped to 60 s–24 h). Earliest wins (ties: dose, sync).
- **No notification input:** a wake is never planned for a notification (D1-B). A CRITICAL
  event created in a wake may start a session in that same wake (orchestrator: hourly limit,
  Wi-Fi backoff, trusted time).
- **No dose wakes** while dosing is impossible until a user action (Q13): not loaded,
  `SERVICE_REQUIRED`, BLOCKED, refill pending, manual marker, calibration/limits or
  reservoir not configured. A wake-local storage failure or pump fault does not suppress
  them (the next wake reloads everything). Readiness and refill commands change the gates;
  the plan is computed at the end of every wake and the service session re-checks due work
  before every wait.
- **Overdue sync** (could not run in this wake, e.g. a dose was too close): re-planned 5 min
  ahead instead of a busy wake loop. DEGRADED: cap only.

### RTC-retained block

48 bytes, little-endian: magic `DSPR`, version 1, length, reserved zeros, the M1.4 sync retry
state (failures, next attempt), the M1.7 hourly CRITICAL attempt limit, wake count,
consecutive fail-safe count, last planned reason and instant, CRC-32 (IEEE). It is used only
after a timer, button, software, watchdog or panic reset and only if magic, version, length,
reserved bytes, CRC and every field range validate; after a power-on or brownout reset, or
on any mismatch, the defaults apply (one sync attempt allowed, no backoff). The clock
continuity marker stays in the M1.4 clock adapter.

### Fail-safe (kept as designed; hardware integration risk)

If any checklist item cannot be confirmed after 3 attempts, `sleep()` is **not** called;
`fail_safe(failures)` is, after the retained block (with the incremented fail-safe count) was
saved. On hardware the port must not return: drive every actuator off by the strongest means
available and reset the SoC; the pump gate then falls OFF through its hardware pull-down (P1,
**hardware-unverified**). After `DSP_POWER_FAILSAFE_LIMIT` (3) consecutive fail-safes, dosing
is inhibited until a shutdown is confirmed again. **Hardware integration risk:** a pump whose
OFF state can never be confirmed (e.g. a broken readback) produces a reset loop with the pump
held OFF by hardware: safe, but it drains the battery. No hardware-specific recovery is
invented before the pump gate and reset behavior are verified
([`HARDWARE_ACCEPTANCE.md`](HARDWARE_ACCEPTANCE.md) 7.1–7.3). Saving the RTC block is
not a checklist item (losing it costs at most one extra sync attempt).

### Service mode (BLE port, no NimBLE/GATT)

On a button wake: `start` → command loop → `stop`. **Session lifetime** (approved), measured
on the injected **monotonic** clock, never wall time:

| State | Rule |
|---|---|
| advertising, no client | ends 180 s (configurable) after the start or after the last disconnect |
| client connected | stays open |
| any | ends at the maximum total duration, **15 min** (configurable) |
| scheduled dose due | BLE suspended (connection closed) before the pump; resumed only while the deadline permits; a connection closed by the suspension restarts the idle timeout |

The BLE port reports the connection state (`connected()`), so this is host-tested without
NimBLE. **Queue synchronization contract:** `dsp_app_post()` is the only function another
task may call; it touches only the bounded queue (8 entries; full or no session → the
platform answers busy), never domain state. Both sides of the queue run under the optional
`dsp_app_sync_t` lock (the FreeRTOS lock belongs to M1.9c/BLE); posting is accepted only while
a session is active. Host tests run the producer synchronously inside the BLE fake's
`wait()` in the application task, so the host implementation has no concurrent access (the
fake lock also checks balance and no nesting). The application task processes the commands: `STATUS`,
`MANUAL_DOSE`, `REFILL`, `SET_READINESS`, `END_SESSION`. Every response carries a status
snapshot (readiness, latch flags, estimate, gates, next due). A manual dose suspends BLE,
runs a due scheduled dose first, then the manual dose (same gates and limits; refused while
BLOCKED), then resumes. Unprocessed commands are dropped when the session ends. In DEGRADED
only `STATUS` is answered (`storage_available = false`).

### Verification and limitations

| Behavior | Status |
|---|---|
| Stage order, DEGRADED, dosing priority, BLE suspension, connectivity guard, plan rules, retained block, checklist and fail-safe | **host-verified** (`test_app`) |
| Power loss at every write, pump run, Wi-Fi connection, HTTP post, BLE wait and sleep entry (2 scenarios × 4 reboot variants) | **host-verified** |
| 365-day simulations (clean: every expected occurrence exactly once across both DST transitions; chaotic: cuts, resets, Wi-Fi faults, clock steps, pump faults, button sessions) | **host-verified** |
| Deep sleep, timer and button wake, RTC memory across resets, wake latency, fail-safe reset and the gate level after it, BLE stop/resume latency | **hardware-unverified** (no adapters yet) |

Limitations: a response to a
command that suspended BLE is delivered after resume, so the phone may have to read it with
`STATUS` after reconnecting; a refused same-time dose whose interval outlasts its window is
missed (by design, no catch-up); the real lock and BLE task are M1.9c/BLE work (single-task
host tests cannot show races); the fail-safe reset loop above is a hardware integration risk.

## Platform adapters and application runtime (M1.9c)

ESP-IDF adapters for the M1.9b ports, plus an opt-in runtime. **Compiled and cross-built
only; nothing has run on the board** (it has not arrived). No GPIO output is configured
anywhere, and nothing can energize a pump.

### Adapters

| Port | Adapter | Behavior (hardware-unverified unless noted) |
|---|---|---|
| `dsp_power_port_t` | `dsp_esp_power_port()` | reset reason (`esp_reset_reason`) and wake sources (`esp_sleep_get_wakeup_causes`) → `dsp_wake_cause_t`; the 48-byte block in `RTC_NOINIT` memory behind a length/inverted-length header; `esp_sleep_enable_timer_wakeup` (1 s–24 h) and, only if `CONFIG_DSP_BUTTON_WAKE_GPIO` passes the pin check, `esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown`; `esp_deep_sleep_start`. A timer that cannot be armed → fail-safe reset instead of an endless sleep |
| fail-safe | `dsp_esp_fail_safe()` | pump `force_off`, Wi-Fi driver stop, error log, `esp_restart()`; never deep sleep. The retained block (with the fail-safe count) is saved by the application before |
| `dsp_app_sync_t` | `dsp_esp_app_sync()` | `portMUX` critical section around the queue only (one command copied; no blocking call, log or allocation inside); task context only, never from an ISR |
| `dsp_pump_port_t` | `dsp_esp_pump_sim()` | **simulation**: no GPIO; `run()` waits for the runtime and counts, `is_off()` reports the simulated state |
| `dsp_time_port_t` | `dsp_esp_time_port()` | `dsp_esp_clock_reading()` (M1.4 continuity rule) |

The decision rules (cause mapping, which resets keep RTC memory, sleep clamping, button pin
check, RTC header) live in `dsp_platform_logic.c`: no ESP-IDF include, compiled unchanged into
the firmware and **host-tested** (`test_platform`). The retained block and the clock continuity
use the same reset rule (power-on, brownout, external and unknown resets invalidate both;
deep sleep, software, panic and watchdog resets keep both). The retained block holds no clock
trust: trust needs the continuity evidence (reset reason + RTC marker + persisted last sync).

### Application runtime (`CONFIG_DSP_APP_RUNTIME`, default off)

`app_main` → `dsp_dosing_port_safe_off(simulated pump)` (first statement) → NVS, clock, time,
monotonic, lock and power wiring → identification log → one static task (`dsp_app`, static
stack `CONFIG_DSP_APP_TASK_STACK`, default 12 KiB) runs `dsp_app_run_wake()` once; the wake ends in
deep sleep or the fail-safe reset. The main task only logs before the task starts, so the
application task is the single owner. No automatic dosing can happen:

- the pump is simulated (no GPIO) and `CONFIG_DSP_APP_SIM_HW_MAX_RUNTIME_MS` defaults to 0, which
  closes every dose in the Dosing Service;
- first boot provisions `SERVICE_REQUIRED` with all limits at 0, and without BLE readiness cannot
  change;
- connectivity is a second opt-in (`CONFIG_DSP_APP_CONNECTIVITY`): it uses the `nvs_sec`
  credentials and its own endpoint `CONFIG_DSP_APP_NOTIFY_URL`. **Build-time endpoint guard**
  (`main/dsp_endpoint_guard.cmake`, run at the configure step): the endpoint must be set
  explicitly (empty → build fails; no fallback to production), must be `https://` with a host,
  and the production host (any spelling: case, port, userinfo, trailing dot) additionally
  needs `CONFIG_DSP_APP_ALLOW_PRODUCTION`. The M1.7 `CONFIG_DSP_NOTIFY_URL` (HTTPS diagnostic) is
  unchanged and never used by the runtime. Regression: `test_endpoint_guard` (the six
  required cases plus spellings and malformed URLs, and the production host cross-checked
  against the `DSP_NOTIFY_URL` default). Automated tests never run firmware.

The runtime cannot be combined with the older diagnostics (Kconfig dependency). An NVS open
failure gives the application a failing KV, so it enters DEGRADED.

### Pinout (pump GPIO10, wake button GPIO3)

From Seeed's published XIAO ESP32-C3 pinout (to be confirmed against the board schematic in the
hardware review): D0 = GPIO2 (strapping), D1 = GPIO3, D2 = GPIO4, D3 = GPIO5, D4 = GPIO6,
D5 = GPIO7, D6 = GPIO21 (UART0 TX), D7 = GPIO20 (UART0 RX), D8 = GPIO8 (strapping), D9 = GPIO9
(strapping, BOOT button), D10 = GPIO10; GPIO18/19 are the USB-Serial-JTAG lines; the flash
uses GPIO12–17.

- **Button wake:** the ESP32-C3 wakes from deep sleep only on GPIO0–5. On the pads that leaves
  D1–D3 (GPIO3–5); D0 (GPIO2) is a strapping pin and is rejected; the BOOT button (GPIO9) cannot
  wake from deep sleep. **Assigned (MVP, 2026-11-21): D1 = GPIO3**, see "Wake button" below; the
  default image keeps `CONFIG_DSP_BUTTON_WAKE_GPIO=-1`.
- **Battery measurement:** ADC1 is GPIO0–4 (ADC2 is unreliable with Wi-Fi). With GPIO3 taken by
  the button, GPIO4 (D2) is the natural candidate (GPIO0/1 are not on the XIAO pads).
- **Pump gate:** avoid the strapping pins (GPIO2/8/9), USB (GPIO18/19) and the UART0 console
  (GPIO20/21); the remaining pads (GPIO6, GPIO7, GPIO10, or GPIO3–5 if not used for the button or
  ADC) need their reset and boot levels measured; an external pull-down is required either way.

### Requirements for the real pump driver (separate hardware review)

1. Gate pin from the review above; never a strapping pin.
2. Gate polarity and an external pull-down so that unpowered, reset, boot, brownout, panic and
   deep sleep all mean OFF without software (P1); measured with a scope at every reset type.
3. A hardware cutoff that ends the drive pulse without the CPU (peripheral one-shot or external
   timeout), its maximum = the driver's `hw_max_runtime_ms`.
4. Watchdog timeouts shorter than the cutoff; feeding the watchdog never extends a run.
5. `force_off()` drives the gate low, `is_off()` confirms it (readback), `run()` leaves it OFF on
   every return; GPIO hold or isolation in deep sleep as measured.
6. Brownout behavior under motor inrush; a brownout mid-dose is an interruption (reported, never
   re-run).

### Fail-safe: hardware integration risk

The platform fail-safe resets the SoC instead of sleeping. If the pump OFF confirmation can never
succeed (e.g. a broken readback), every wake ends in a reset: a **reset loop** with the pump held
OFF by hardware, safe but **draining the battery**. After 3 consecutive fail-safes the domain
inhibits dosing until a shutdown is confirmed. No hardware-specific recovery (backoff sleep,
power cut) is implemented before the gate and reset behavior are measured.

### Sizes and resources (ESP32-C3, measured on the cross-builds)

| Configuration | Image | Headroom in a 1.75 MiB slot | Static DRAM (`idf.py size`) |
|---|---|---|---|
| default | 0x24400 (148,480 B) | 91.9 % | 50,059 B |
| diagnostics (time + HTTPS) | 0xfe800 (1,042,432 B) | 43.2 % | 108,552 B |
| runtime (simulated pump, no connectivity) | 0x3a290 (238,224 B) | 87.0 % | 84,487 B |
| runtime + connectivity (+ test button GPIO 3, sim maximum 30 s) | 0x10db70 (1,104,752 B) | 39.8 % | 140,164 B |

- Static RAM of the runtime: application state `s_app` 17,472 B, task stack 12,288 B, retained
  block 136 B in RTC memory (120 B domain block since M1.10b.3); `s_https` adds 1,336 B with
  connectivity.
- **Stack:** worst domain call chain from `dsp_app_run_wake` = **3,152 B** (M1.10b.3; GCC
  `-fcallgraph-info=su`, RISC-V `-Os`: run_wake → stage_dose → process_due → run_automatic →
  claim → history scan; the same script gives 3,104 B for M1.10b.2 `aa0532f`, which this README
  listed as 2,992 B with an earlier method). Not included: the platform code behind the ports (NVS; with
  connectivity the TLS handshake inside `esp_http_client`), which needs measuring: the sleep log
  prints the task's stack high-water mark for that (hardware acceptance 7.6). The 12 KiB default
  is a provisional margin, not a measurement.
- New dependencies: ESP-IDF built-ins only (`esp_system`, `freertos`, `log`, `esp_hw_support` in
  `dispenser_platform` and `main`); no external component, no partition change.

## BLE protocol codec and Noise gate (M1.10a)

**Implemented and host-tested; not wired to BLE yet** (no NimBLE, no GATT, no session crypto:
M1.10c/d). Normative specification [`../docs/BLE_PROTOCOL.md`](../docs/BLE_PROTOCOL.md), design and
security model [`../docs/BLE_PROTOCOL_DESIGN.md`](../docs/BLE_PROTOCOL_DESIGN.md) (revision 3).

- **`dsp_bleproto`** (pure C11, no allocation): 8-byte frame header, plain bodies encoded and
  decoded from layout tables that carry every rule of the specification (lengths, bools,
  enumerations, masks, reserved bytes, dates, UTF-8/ID/ASCII/raw strings, list counts, the
  forward-compatible trailing-bytes rule), ENCRYPTED frames validated at header level only
  (opaque body for the session layer), fragmentation for any ATT MTU ≥ 23 and a bounded
  reassembler (one 520-byte partial frame, 2,000 ms timeout), version negotiation, the public
  Info record and the advertising Service Data payload. ESP32-C3 size (`-Os`, standalone):
  ≈ 10.9 KB flash (4.5 KB code, 6.3 KB layout tables), no static RAM; a message is 512 B and the
  reassembler 536 B (caller-owned).
- **Shared vectors** `test-vectors/ble_protocol_v1.txt`, generated by an independent Python
  script (`test-vectors/tools/gen_ble_protocol_v1.py`) from a schema transcribed from the
  specification; the Android codec runs the same file.
- **Noise feasibility gate** [`third_party/noise-c/`](third_party/noise-c/README.md): pinned
  `esphome/noise-c` 0.1.31 + `esphome/libsodium` 1.10021.12 with two temporary local patches
  (bounded pattern search for Android FORTIFY; default feature switches on ESP-IDF), verified on
  host, Android NDK r30 and ESP32-C3 by `gate/run_gate.sh` (34 checks). Not yet integrated into
  the firmware (M1.10c).

## Configuration, schedules and phone time (M1.10b.1)

**Implemented and host-tested in the domain; not reachable over BLE yet** (the dispatcher, the
security layer and NimBLE are M1.10c/d). New public operations of the Application Service
(`dsp_app.h`), called by the future dispatcher from the application task inside a service
session:

| Operation | Protocol message | Reuses |
|---|---|---|
| `dsp_app_get_config`, `dsp_app_set_config` | GET_CONFIG, SET_CONFIG | `dsp_config_apply_update`, `dsp_storage_*_config` |
| `dsp_app_list_schedules`, `dsp_app_get_schedule` | LIST_SCHEDULES, GET_SCHEDULE | `dsp_schedule_repo` |
| `dsp_app_create_schedule`, `dsp_app_update_schedule`, `dsp_app_delete_schedule` | CREATE/UPDATE/DELETE_SCHEDULE | `dsp_schedule_repo`, `dsp_schedule_validate` |
| `dsp_app_set_phone_time` | SET_TIME | `dsp_timekeeper_on_phone_time`, `dsp_time_apply` |

- **Ownership:** the Application Service stays the only owner of state; the operations read
  and write only through the existing modules, never drive the pump and never change
  readiness. Results (`dsp_app_op_result_t`: status, detail, aux) map 1:1 to the protocol's
  error codes (`docs/BLE_PROTOCOL.md` section 10).
- **Authorization fails closed:** every operation takes `const dsp_app_auth_t *` from the
  future security layer; NULL, a non-owner role or a slot ≥ 4 is FORBIDDEN before any storage
  access. Nothing produces an owner authorization yet. Outside a service session every
  operation is BUSY; without loaded storage DEGRADED.
- **Configuration:** `expected_revision` must equal the stored revision (CONFLICT with the
  current one otherwise; doses also advance it, because the estimate lives in this record);
  only the fields in `field_mask` change (bits = protocol), all others and the read-only ones
  (`revision`, `reservoir_remaining_ul`, the hardware cutoff) are preserved. Calibration and
  limits only while SERVICE_REQUIRED (D12); while READY the capacity may change but never
  from or to 0 (which would close or reopen a dosing gate). Refused while a refill is pending.
  Strict validation: thresholds, interval, mask bits, capacity ≥ estimate, calibration time 0
  or ≥ 2025-01-01, `max_daily ≥ max_dose`, `max_runtime ≤` the hardware cutoff. One atomic
  record write; a new sync interval applies to the running timekeeper; capacity/threshold
  changes re-observe the reservoir.
- **Schedules:** create (revision 1), update (record revision = `expected_revision` + 1, stored
  revision must match), delete (revision must match) through the repository: IDs never
  reused, tombstones, history untouched, one atomic write of the set. Names are stored on the
  device. `amount_ul` above a configured `max_dose_ul` is refused (LIMIT); volumes are `u32` µl
  by type (the app must refuse anything larger). Pages of 4.
- **Edit floor (D16, approved; no catch-up after edits):** create, an update of any timing
  field (start date, time, kind, interval, weekdays, end date) and re-enabling set the
  schedule's `not_before` to the trusted edit time + 1 s (during a rollback hold: the highest
  trusted time + 1 s if later); monotonic per schedule, never lowered. With an untrusted clock
  these three edits are **refused** (REFUSED, detail 3: send SET_TIME first), because an
  untrusted clock can regain trust without a sync (a continuous clock that read behind the
  last sync); name, volume and disable edits and deletion stay possible. The coordinator treats occurrences before it as MISSED.
  So moving a time into the current due window, re-enabling a schedule, creating a schedule
  for a time just passed, or deleting and recreating a schedule never doses for that past
  instant. Name- and volume-only edits keep the floor. Claimed occurrences stay blocked by
  the M1.2 watermark (identity `<id>@<date>`), so an edit never resurrects one, also across
  DST (one occurrence at the earlier instant on fall-back days, skipped on spring-forward days).
- **Phone time (D10):** accepted only while the clock is UNKNOWN or in HOLDOVER (SYNCED:
  IGNORED_NTP_PREFERRED; the NTP retry schedule is unchanged), uncertainty ≤ 2,000 ms, within
  2025-01-01..2100. A recorded rollback hold refuses it (HOLD), and so does a time more than 7
  days behind the highest trusted time: phone time never creates or lifts a hold. Otherwise
  the M1.4 rules apply unchanged (initial-sync floor, forward-step floor, high water), durable
  before effective (`dsp_time_apply`, shared with the NTP path). Missed doses are never run.
- **Failures:** a write failure answers STORAGE and ends dosing for the rest of the wake; the
  schedule repository is re-read at once so the wake plan keeps the durable set. Every write
  is a single NVS key.
- **Configuration bounds:** every representable value is accepted where no approved limit
  exists; the arithmetic (runtime from volume and flow, daily total, interval, reservoir
  percentages) is 64-bit and overflow-safe, host-tested with every field at its 32-bit extreme
  (a 4,294,967,295 µl dose at 4,294,967,295 µl/s runs exactly 1,000 ms; at 1 µl/s it does not fit
  32 bits and is refused, never truncated). Physical bounds wait for measurements
  (`HARDWARE_ACCEPTANCE.md` section 9).
- **Real ESP-IDF NVS (`test/nvs_linux`, linux target, committed partition table):** the
  maximum layout (`sched` 3,160 B, 304 entries) plus PHY/Wi-Fi co-tenants leaves 1,952 of 2,520
  entries available; repeated maximum-size writes need about 140 available entries in steady
  state (margin about 1,800); 20,000 rewrites: 0.82 page erases per write, no entry leak; a
  write that does not fit is refused cleanly (old set intact). **Defect found in ESP-IDF v6.1
  NVS:** a power cut after the new version's last data chunk and before the new blob index
  makes `nvs_flash_init_partition()` erase the old version too (the page loader's "duplicate
  last item" check ignores the chunk index), so the key is lost; reproduced with plain
  `nvs_set_blob()` at exactly one cut point per write. The storage then reports CORRUPT and
  fails closed (no dosing, never an extra dose). This affects every record since M1.3. **Fixed in software:**
  a project-scoped patched `nvs_flash` ([`third_party/nvs_flash`](third_party/nvs_flash/README.md),
  espressif/esp-idf#19212/#19213/#19214); the release and hardware-acceptance blocker stays open
  until bench power cuts verify it (`HARDWARE_ACCEPTANCE.md` 9.4).

Sizes (ESP32-C3, `-Os`): the operations add about 4.4 KB of code once the dispatcher links them
(`dsp_app` +2.9 KB, timekeeper +0.5 KB, config +0.4 KB, codec +0.25 KB, shared UTF-8 validator
0.2 KB, the BLE codec −0.2 KB because it now uses the shared validator); static RAM +2,744 B
(`dsp_app_t` 15,856 B: the larger schedule set in the repository and the storage buffers);
at most 208 B of stack per operation.

## Device ownership and credentials (M1.10b.3)

**Implemented and host-tested in the domain; nothing reachable over BLE yet** (security layer
and dispatcher: M1.10c/d). Design: [`docs/OWNERSHIP_DESIGN.md`](../docs/OWNERSHIP_DESIGN.md)
(revision 2, approved with O1-O11 and R1-R5). No cryptography in the domain: key generation,
X25519, HKDF and the Noise handshakes belong to the M1.10c security layer; the domain stores and
validates key bytes and decides ownership transitions.

- **Credential record `auth`** (`dsp_auth.h`, layout v5, 416 B): identity key pair, four
  independently revocable phone slots, `owner_epoch` (+1 at every ownership change, never
  decreases), KEY_FAULT, recovery and failed-session counters. The 128-bit setup code is stored
  only in `nvs_sec` (`setup_code`, 16-byte blob) and never in `auth`.
- **Three credential states:** UNCLAIMED (valid record, no owner), CLAIMED, CRED_DEGRADED
  (missing / corrupt / newer `auth` under a v5 marker, KEY_FAULT, or a key fault reported in this
  wake). CRED_DEGRADED is never reported or treated as unclaimed, closes the dosing gate
  `DSP_DOSING_G_CREDENTIALS` (automatic and manual), raises the storage DEVICE_ERROR condition and
  admits only RECOVERY. It ends by a successful recovery (the device is then SERVICE_REQUIRED) or
  maintenance.
- **Owner operations** (`dsp_app.h`): `dsp_app_phones_list`, `dsp_app_phone_remove` (never the
  last phone; removing itself ends the session), `dsp_app_invite_create` / `_cancel` (one invite),
  `dsp_app_recovery_cancel`. They require an authorization **issued by the Application Service**
  in the current session and epoch (`auth->session_id`, `auth->token`); a hand-made
  `{OWNER, slot}` is FORBIDDEN.
- **Integration blocker (must hold until M1.10c):** the configuration, schedule, phone-time and
  manual-dose operations (M1.10b.1/b.2) and the REFILL / SET_READINESS queue commands still
  accept a caller-provided authorization input. They must **not** become reachable through BLE
  until M1.10c provides security-layer-issued authorization and they verify it like the
  ownership operations (session ID, token, epoch). A caller-provided or hand-crafted owner
  authorization must never be sufficient for an externally reachable operation. Today nothing
  external can call them (no BLE task, no dispatcher).
- **Security-layer interface** (`dsp_app_sec.h`, restricted): admit (per purpose) → identity /
  invite token → commit (lookup, enroll, recovery begin/apply), key fault, failed handshake.
  Every call checks at runtime: a registered security layer (`cfg->security`; **NULL in every
  build before M1.10c, so no ownership authorization can exist**), an active button-wake session
  (physical presence), the single-use handshake handle issued for this session (purpose, random
  nonce, 30 s, epoch at admission), the ownership preconditions again at commit, and for
  recovery the RTC arm and the confirmation bound to (session, transcript hash). The header
  boundary is also enforced at configure time (`sec_header_guard.cmake`): defense in depth only.
- **RTC overlays** (`dsp_owner.h`, retained block format 2, 120 B): the invite (16-byte device
  RNG token, 10 min) and the recovery arm (30 min) are kept only across deep-sleep wakes (every
  reset clears them, R1), have fixed deadlines, are cleared by any clock step (raw clock vs.
  monotonic clock within the wake; raw clock below the last save across wakes) and are bound to
  `owner_epoch`. CRC-valid but inconsistent overlays are dropped without discarding the rest of
  the block. Overlays are preconditions, never authority (a commit still needs the setup-code
  handshake). Consumption is saved to RTC memory **before** the commit.
- **Recovery:** arm by a verified ≥ 10 s hold from the first sample after a power-on or button
  boot (`cfg->recovery_hold_ms`, measured by the button driver, M1.10d); RECOVERY handshake in a
  later session; exactly 3 short presses (guard 300 ms after start, ≤ 400 ms each, gaps ≤ 500 ms,
  700 ms sequence end, 20 s) delivered as `DSP_CMD_BUTTON` events, queued and replayed events
  ignored; the third failed confirmation disarms. Apply: arm cleared and saved, readiness
  SERVICE_REQUIRED (restricting write), then the new `auth` (new identity, new phone in slot 0).
  History, schedules, configuration, time, notifications, the BLOCKED latch, refill intent,
  manual high-water marks, the result ring and the admission counter are untouched (tested byte
  for byte).
- **KEY_FAULT:** `dsp_app_sec_key_fault` inhibits dosing in RAM first (for the rest of the wake,
  whatever the write does), then persists the flag (epoch + 1) for later wakes.
- **Admission and limits:** manual dose order: invalid → already decided (always answered, never
  counted) → session limit (8) → durable daily admission (`dosing` format 3, default 32 per
  local day, `cfg->manual_decisions_per_day`, counted only inside the decision write; restarts
  only for a trusted later date; without trusted time it keeps counting) → decision. Over a limit:
  `BUSY` detail `RATE_LIMITED` (aux: seconds), nothing written, the request stays undecided.
  Session-local: 256 operations, 48 durable writes, 16 writes per minute (configurable); **v1
  limitation: a reconnect or reset clears these** (the manual daily counter is durable).
- **Provisioning:** `tools/provision_dev_secrets.py` generates the setup code if absent (written
  back to the owner-only secrets file), stores it in `nvs_sec`, and writes the label text
  `DSP2:<device-id>:<26 Crockford base32 + 2 check characters>` to a 0600 file outside the
  repository; it never prints the code.
- **Resources** (runtime image, measured against `aa0532f`): +6,352 B image, +1,384 B static DRAM
  (`s_app` 17,472 B), +64 B RTC memory; worst wake chain 3,152 B (+48 B); ownership operations at
  most 688 B of stack (recovery apply). NVS: +15 entries.
- **Not done here:** Noise and key generation (M1.10c), button driver and hold measurement
  (M1.10d), factory reset (M1.10b.4, separate review), Wi-Fi store (M1.10b.4).
- **M1.10c deliverable (protocol):** the HELLO/Info credentials-damaged flag (bit 1, O9). The
  wire format today is unchanged (bit 0 RECOVERY_ARMED only; the codecs reject bit 1); it must be
  added in one change to the normative specification, the shared vector generator and vectors,
  the C codec and the Kotlin codec, never as an undocumented difference. The domain already
  exposes the state (`dsp_app_sec_view`).

## Manual dose with durable anti-replay (M1.10b.2)

**Implemented and host-tested in the domain; not reachable over BLE yet** (dispatcher and
security layer: M1.10c/d). Design: `docs/BLE_PROTOCOL_DESIGN.md` 7.4 (D8); wire semantics:
`docs/BLE_PROTOCOL.md` 10.4.

- **Entry point:** `dsp_app_manual_dose(app, auth, seq, volume)` is the only manual-dose path
  (application task, inside a service session, owner authorization, fail closed). The slot is
  `auth->slot`, the security layer's verified context; nothing in the request selects it. The
  former unauthenticated queue command `DSP_CMD_MANUAL_DOSE` is retired (`dsp_app_post()` refuses
  value 2). `dsp_app_get_result()` and `dsp_app_manual_floor()` serve GET_RESULT and the Status
  record's `manual_floor`. REFILL and SET_READINESS remain legacy queue commands and must not
  become externally reachable before the security layer authorizes them (M1.10c).
- **Rule:** `seq <= manual_hw[slot]` is already decided and never executes (the retained result,
  or UNAVAILABLE). A larger `seq` is decided exactly once: accepted - `manual_hw[slot] = seq`, the
  marker bound to `(slot, seq)` and a PENDING result in **write 1**, the restricting write, before
  the consumption/permit (write 2) and the pump; write 3 clears the marker and records DONE/FAILED;
  boot recovery records INTERRUPTED in the write that clears an interrupted marker - or refused
  (gates, limits, coverage, or Application Service refusals: a pump fault earlier in the wake, the
  fail-safe inhibit) with `manual_hw[slot] = seq` and the REFUSED result in **one write** before the
  answer. Invalid requests (`seq` 0, `seq` > 2^63 − 1, volume 0) consume nothing. If no decision can
  be made durable (storage unavailable or degraded, or the decision write fails) nothing executes:
  no consumption, no pump, and the request stays undecided unless the durable state says otherwise.
- **Counters:** four independent slots; `manual_hw` only grows (until a factory reset re-provisions
  the record; ownership recovery keeps it); a slot at 2^63 − 1 accepts nothing more (floor 2^63),
  never wraps. The ring of 8 results is a convenience: eviction never weakens the rule.
- **BLE:** a fresh request suspends BLE, runs a due scheduled dose first, decides, runs the pump,
  then resumes only while the session deadline permits (the session state lives in `dsp_app_t`).
  Already decided and invalid requests touch neither BLE nor the pump.
- **Storage:** `dosing` format 2 (206 B), decoded strictly (no high-water mark above 2^63 − 1, the
  marker request at or below its slot's mark and only with an active marker, a PENDING result only
  for the active marker, one result per request, zero padding). Format 1 decodes with zeros and is
  upgraded by its next write (one atomic key): **layout v4 is kept**; a corrupt or newer record is
  reported (CORRUPT / UNSUPPORTED, fail closed), never treated as fresh.
- **Sizes** (ESP32-C3, `-Os`, against the previous commit): default image unchanged; runtime
  +496 B image, +256 B static DRAM (`dsp_app_t` 16,112 B); worst stack of a manual request
  784 B (automatic dose 672 B, recovery 416 B).
- **Flash wear** (measured with the patched NVS on the real layout; the daily rates are
  assumptions): a dose now costs about 0.0125 erases of the most-erased page (52 B record:
  0.0095), a durable refusal 0.0048. At 4 automatic doses, about 2 notifications, 1 time write,
  1 refusal a day and 1 manual dose a week the most-erased page sees about 320 erase cycles in 10
  years (about 260 with format 1). A phone that keeps sending valid but refused requests adds
  about 4.8 cycles on that page per 1,000 refusals (1,000 a day: about 17,700 cycles in 10
  years): compare with the flash's rated endurance before release; no endurance figure is
  assumed here. **Bounded since M1.10b.3:** fresh manual-dose decisions are limited per local
  day by a durable counter (`dosing` format 3, default 32, written only in the decision write);
  configuration, schedule and ownership writes have session-local limits (v1: a reconnect or
  reset clears them; documented residual). See the M1.10b.3 section.

## NimBLE BLE transport (M1.10d.1)

Design: [`docs/BLE_TRANSPORT_DESIGN.md`](../docs/BLE_TRANSPORT_DESIGN.md) (revision 2, decisions
T1–T14); wire rules: `docs/BLE_PROTOCOL.md` sections 2–3. **Build-only and not run on hardware:**
M1.10d.2 (`HARDWARE_ACCEPTANCE.md` 8b) and M1.10c.2b (8a) are open; do not deploy
security-enabled firmware before the ESP32-C3 RNG gate passes.

- `dsp_ble_transport` (pure C, `dispenser_platform`, compiled unchanged into the host tests)
  implements the BLE port over a small radio interface: one connection with strictly increasing
  generations, reassembly with the shared codec, a 3-frame TX ring sent asynchronously (20 ms
  retry callout, ≤ 25 refusals, 1000 ms per frame, then close), advertising (re)started only from
  the application task, generation-bound `send_frame`/`disconnect`, a final reply sent before a
  requested close. It never sends a protocol message of its own.
- `dsp_esp_ble` (ESP-IDF): NimBLE port, the GATT service, advertising (100 ms for 30 s, then
  500 ms; public address), the retry callout on the host task's event queue, FreeRTOS lock and
  wake semaphore. The controller stays enabled while a window is suspended for a pump run and is
  disabled at the end of the window.
- Application Service: connection events go to an authoritative **connection snapshot** beside
  the frame queue (never refused for space); the application reconciles with it before every item,
  so no session, authorization or response survives a reconnect. Fragment errors are reported to
  it and decided by the security layer (plain MALFORMED only before a handshake started).
- `main.c` wires the transport only together with the security layer; the Info record carries the
  device tag and firmware version.

```sh
# BUILD-ONLY variant (refused together with CONFIG_DSP_APP_ALLOW_PRODUCTION)
idf.py -B build-ble -D SDKCONFIG=build-ble/sdkconfig \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ble.defaults" build
# hardware bring-up without the button driver: add CONFIG_DSP_BLE_BRINGUP_POWER_ON_WINDOW=y
```

| Configuration | Image | Slot | Static DRAM |
|---|---|---|---|
| runtime + security + BLE | 680,816 B | 37.1 % | 136,021 B |
| runtime + connectivity + security + BLE | 1,400,784 B | 76.3 % (gate 85 %) | 180,393 B |

NimBLE (controller, host, PHY/NVS) plus transport and adapter add 243,392 B image and 39,936 B
static DRAM to the security image (transport + adapter ≈ 6.1 KB code, 3.2 KB static); with Wi-Fi
already linked the increase is 197,536 B. Heap after NimBLE init, stack high-water marks,
throughput and current are hardware items (H5, H7, H8).

## RNG gate diagnostic (M1.10c.2b evidence)

A separate, off-by-default diagnostic image that produces the ESP32-C3 RNG evidence R1, R2, R3, R5
and R6 of [`HARDWARE_ACCEPTANCE.md`](HARDWARE_ACCEPTANCE.md) section 8a.1 (design:
`docs/ANDROID_BLE_DESIGN.md` Appendix B). `CONFIG_DSP_RNG_GATE_DIAGNOSTIC` (fragment
`sdkconfig.rnggate.defaults`) enables the Bluetooth controller only (`CONFIG_BT_CONTROLLER_ONLY`, no
host), registers the libsodium RNG adapter, checks it refuses with the controller off, streams
1 MiB (configurable up to 8 MiB) from `esp_fill_random` and the same through `randombytes_buf` as
framed Base64 lines with CRC-32, derives the public PSK vectors through `dsp_psk_derive` (counts
only), disables the controller and idles. Code: `main/main.c` (`rng_gate_task`); the R6 header is
generated at configure time from `test-vectors/ble_session_v1.txt` by `main/CMakeLists.txt`.

```sh
idf.py -B build-rnggate -D SDKCONFIG=build-rnggate/sdkconfig \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.rnggate.defaults" build
# after an approved flash (command in HARDWARE_ACCEPTANCE.md 8a.1):
python firmware/tools/rng_capture.py --port /dev/ttyACM0 --reset --out-dir rng-out --log rng-out/console.log
python3 firmware/tools/test_rng_capture.py      # host tests of the capture tool (synthetic logs)
```

Safety notes:

- Not a deployment image. No Noise service, `dsp_blesec`, GATT, NimBLE/Bluedroid host,
  advertising, scanning or connections; no application runtime, dosing or GPIO output; no
  credentials, setup code or `nvs_sec` access; NVS is never initialized and PHY calibration is
  never stored (`CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE=n`); no eFuse writes. Antenna
  attached before running (RF calibration).
- Kconfig excludes it with the runtime, BLE security/transport, connectivity and the other
  diagnostics; the configure step refuses it with `CONFIG_DSP_APP_ALLOW_PRODUCTION` and fails when
  `sdkconfig.rnggate.defaults` was requested but the result is not a valid RNG-gate image.
- The capture tool's chi-square/monobit figures are sanity checks only; run `ent`, dieharder or a
  NIST STS subset on the captured `.bin` files. Neither proves that the entropy source is enabled
  (that is R1–R3). R4 (live handshakes over BLE) is not covered by this image: it runs in the
  controlled security-enabled bring-up that may start only after R1–R3 and R5 pass, and
  security-enabled firmware is accepted for normal use only after R4 passes
  (`HARDWARE_ACCEPTANCE.md` 8a, gate sequence).
- Cross-built only; not run on hardware. Flashing needs explicit approval.

## HTTPS notification transport and connectivity session (M1.7)

Pure domain code (`dsp_notify_proto`, `dsp_https_transport`, `dsp_connectivity`, extensions
of `dsp_notify`) plus ESP-IDF adapters (`dsp_esp_http`, `dsp_esp_device`). Contract with the
backend: [`../cloudflare/dispenser-notify/PROTOCOL.md`](../cloudflare/dispenser-notify/PROTOCOL.md).
Compiled and cross-built; **not run on hardware and never against a real network yet.**
Nothing calls the session from `app_main` except the off-by-default diagnostic; the
dosing/connectivity application service is a later milestone.

### Connectivity session (approved D1-B, D4)

`dsp_connectivity_run()` runs at most one bounded session **on an existing wake**:
1. **Guard:** nothing while a dose runs, a dose permit is outstanding or BLE is active, or if
   the next dose is closer than the worst-case session duration (Wi-Fi attempts × timeout +
   SNTP timeout + HTTPS budget; 115 s by default) plus 2 minutes.
2. **Reason:** the regular time sync is due (M1.4 schedule, including its backoff), **or** a
   CRITICAL notification is pending, the last notification-initiated attempt was at least
   1 hour ago (RTC-retained; a backward clock step allows one attempt) and no failed sync
   is waiting for its backoff. WARNING notifications only travel with regular sessions.
   **No wake-up is ever scheduled for a notification** and no delivery latency is promised.
3. **Wi-Fi + time sync** (M1.4 time service; a CRITICAL session also synchronizes, and a
   failure feeds the normal backoff). Conditions are re-observed with the new time state.
4. **Trusted time (D4):** HTTPS only if the timekeeper is not UNKNOWN and the time is
   plausible. Otherwise nothing is sent and the queue is untouched. Certificate date
   validation is never disabled. Consequence (approved R2): a `DEVICE_ERROR/CLOCK` episode
   may never produce an e-mail, because regaining trusted time closes it and withdraws its
   undelivered notification; BLE diagnostics will expose clock problems.
5. **Delivery:** all pending notifications, CRITICAL first, within a **60 s budget**. An
   entry is started only if its worst case (20 s) still fits; entries that are not started
   stay untouched (no attempt counted, no freeze) for the next session. The budget does
   not guarantee that all 16 queued entries go in one session.
6. **Wi-Fi is always shut down** before returning.

### GPIO pump control signal (`CONFIG_DSP_PUMP_GPIO`, bench stage)

Bench opt-in (fragment `sdkconfig.pumpgpio.defaults`, default off; refused together with
`CONFIG_DSP_APP_ALLOW_PRODUCTION`): the Dosing Service drives a GPIO as the pump control signal
instead of the simulated pump. **No pump, MOSFET or other load is connected at this stage**; the
stage verifies the control signal and the real scheduling path.

| Item | Value |
|---|---|
| Pin | XIAO ESP32-C3 **D10 = GPIO10** (`CONFIG_DSP_PUMP_GPIO_NUM`). Not a strapping pin (GPIO2/8/9), not SPI flash (GPIO11–17), not USB (GPIO18/19), not UART0 (GPIO20/21); the build refuses those and the button-wake pin |
| Polarity | **active high** (`CONFIG_DSP_PUMP_GPIO_ACTIVE_HIGH`): intended for a low-side N-channel logic-level MOSFET, gate driven from GPIO10 through a series resistor (≈ 100–220 Ω) with a **gate-to-source pull-down (≈ 47–100 kΩ)**, a flyback diode across the motor |
| Maximum per run | `CONFIG_DSP_PUMP_GPIO_MAX_RUNTIME_MS` = **10,000 ms** (bench): the hardware runtime maximum of the build (pump configuration with a larger `max_runtime_ms` keeps the PUMP_CONFIG gate closed) and the driver's own limit |
| Independent cutoff | `esp_timer` one-shot at runtime + 100 ms with **ISR dispatch**: the callback runs in the timer interrupt from IRAM (one GPIO register write, one flag), independent of every task and of the flash cache. The build refuses the GPIO pump without ISR dispatch, the interrupt watchdog, a panicking task watchdog and panic = reboot without OCD awareness (all set by `sdkconfig.pumpgpio.defaults`) |

Bounds on the active time (software only; reviewed 2026-11-20):
- Normal path: the calculated runtime within ± one FreeRTOS tick (10 ms): `vTaskDelay(n)` waits n
  tick interrupts and the first may come at once, so the pulse is in (runtime − 10 ms,
  runtime + 10 ms) (bench: 7,991 ms for 8,000 ms; host test `test_tick_rounded_pulse_bounds`); never
  near the cutoff (one tick < the 100 ms margin, checked at build time); ≤ 10,010 ms.
- Application task stalls or the scheduler is starved: the interrupt cutoff ends the pulse at
  runtime + 100 ms (≤ 10.1 s). A scheduler starved for 5 s also panics through the task watchdog.
- Interrupts disabled (or the timer interrupt cannot run): the interrupt watchdog panics after
  300 ms; the panic handler prints the crash (≈ 0.1–0.5 s over the console) and resets; the pad is
  then unconfigured and the pull-down ends the pulse. Worst case ≈ runtime + 100 ms + 300 ms +
  panic output, i.e. **about 11 s for a 10 s run**.
- **Not covered by software**: a hung clock or watchdog hardware, or a CPU halted by a debugger
  (OpenOCD halts the CPU and disables the watchdogs while halted). Only a CPU-independent hardware
  cutoff in the driver circuit (e.g. a monostable or an RC-limited gate) bounds those; it is required
  before a real pump is connected. **Never attach a debugger while a load is connected.**
- Earlier statement "at most 10.1 s absolute" (task dispatch, task watchdog without panic, OCD-aware
  panic) was not justified and is withdrawn.

Watchdogs in the bench GPIO build: interrupt watchdog 300 ms (panic); task watchdog 5 s on the idle
task (panic, `CONFIG_ESP_TASK_WDT_PANIC`); panic = print and reboot, reboot delay 0, OCD awareness off
(an attached JTAG debugger cannot turn a panic into a halt); the bootloader's RTC watchdog (9 s) runs
only until the app starts. A pump run blocks the application task in `vTaskDelay`, so the idle task
keeps running and the task watchdog stays quiet during a normal 10 s run.

Hold across deep sleep and wake (ESP32-C3, ESP-IDF v6.1 `gpio_hold_en` documentation): a digital pad
keeps a held level through deep sleep only with `gpio_deep_sleep_hold_en()` (the driver calls both
after driving the pad inactive). On wake the chip resets: the hold is released and the pad returns to
its default unconfigured state until SAFE_OFF configures it again (the pull-down covers this interval).
The driver releases the hold (`gpio_hold_dis`) before every level change and holds again afterwards.

Activation path: a dose becomes due → every existing gate and limit (trusted clock, reservoir
configured and covering, calibration and limits, READY, max dose/runtime/daily, minimum interval,
durable claim) → `dsp_pump_gpio` run: refuses runtime 0 or above its maximum, configures the pad
and confirms by readback that it is inactive, arms the cutoff, drives it active, reads it back,
waits the calculated runtime, drives it inactive, disarms the cutoff, reads it back, holds the pad.
Any mismatch, interrupted wait or fired cutoff is `DSP_PUMP_FAILED` (the Dosing Service counts the
volume with uncertainty, as for a pump fault). Code: `dsp_pump_gpio.c` (pure, host-tested) and
`dsp_esp_pump_gpio.c` (ESP-IDF, compiled only with the opt-in).

Output level outside a run:
- **Reset, boot ROM, until SAFE_OFF**, and **after every wake from deep sleep** (on the ESP32-C3 a
  digital pad returns to its default unconfigured state): the firmware does not drive GPIO10, so
  only the **external gate pull-down** keeps the driver off. Software cannot guarantee this
  interval; the pull-down is mandatory for any real driver.
- **SAFE_OFF** (first action of every wake, before anything else) and after every run: driven
  inactive (low), internal pull-down enabled, readback confirmed, pad held.
- **Before deep sleep**: driven inactive and held (`gpio_hold_en` + `gpio_deep_sleep_hold_en`); the
  SHUTDOWN checklist sleeps only after a confirmed OFF readback, otherwise fail-safe reset.
- **Fail-safe reset / watchdog / brownout**: the pad reverts to its unconfigured state → pull-down.

What a bench run means: `DSP_PUMP_OK` / history `COMPLETED` mean "the control signal was active for
the commanded runtime". Nothing measures liquid in any configuration (there is no flow sensor; the
same holds for the future pump), so the reservoir value is the firmware's **estimate** of the
commanded volume. With no load connected, bench runs lower that estimate although no liquid moved:
keep a bench reservoir configuration and set the real capacity and amount again ("Změnit kapacitu
nebo stav") before connecting a pump. The firmware cannot detect whether a load is connected.

Log lines: `dsp_pump_gpio: BENCH pump control signal on GPIO10, active-high, max 10000 ms per run`,
`GPIO10 ACTIVE for N ms (bench: control signal only, no liquid measured)`, `GPIO10 inactive after N ms`.

### Wake button (`sdkconfig.button.defaults`, bench MVP)

A physical button wakes the device from deep sleep and opens the BLE service window, replacing
the RESET press of the bring-up workflow (the bring-up power-on window stays as a fallback).

- **Circuit:** XIAO D1 = GPIO3, normally-open momentary pushbutton to GND, external 10 kΩ pull-up
  GPIO3 → 3V3, active low; no connection to RESET or BOOT. GPIO3 is a deep-sleep wake pad, not a
  strapping, JTAG, USB, UART0 or flash pin, and differs from the pump control pin GPIO10
  (compile-time checks in `dsp_esp_power.c` and `dsp_esp_pump_gpio.c`; `test_button_config`).
- **Arming (every sleep):** the RTC timer is always armed with the planned sleep;
  `esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(BIT(3), ESP_GPIO_WAKEUP_GPIO_LOW)` is added
  when the button is released. ESP-IDF configures the pad inside `esp_deep_sleep_start` (internal
  pull-up, because `CONFIG_ESP_SLEEP_GPIO_ENABLE_INTERNAL_RESISTORS=y` is kept: same direction as
  the external 10 kΩ, ~0.4 mA only while pressed, and the pad stays defined if the external
  resistor is missing) and latches it with a pad hold, which the next sleep entry releases before
  it reads the level.
- **Held or stuck button:** the ESP32-C3 GPIO wake is level-only, so a button still low at sleep
  entry would wake the chip at once into a new service window. The level is sampled 5× over ~8 ms
  right before sleep; if any sample is low, only the timer is armed and the sleep is capped to
  60 s (`DSP_PLAT_BUTTON_HELD_RETRY_S`). Those short timer wakes open no window; the first sleep
  after the release arms the button again.
- **Wake handling:** `ESP_SLEEP_WAKEUP_GPIO` → `DSP_CAUSE_BUTTON` (wins over a simultaneous timer
  wake) → physical presence → the BLE service window, exactly as a power-on in the bring-up build.
  A timer wake never opens a window. The order is unchanged: recovery, clock, due dose, Wi-Fi/SNTP
  when a sync is due, then BLE, then plan and sleep with both sources. The window ends after 180 s
  without a client or 15 min in total.
- **Not a dosing trigger:** dosing runs only through the Dosing Service's gates (readiness,
  schedules, due window, anti-replay, limits, reservoir) exactly as on a timer wake; a button wake
  never changes readiness or the READY floor. GPIO10 behavior, NVS layout, BLE protocol and
  credentials are unchanged.
- **Log (once per boot, no device data):** `dsp_power: wake: cause=BUTTON (reset reason 8,
  timer=0, gpio=1, gpio mask 0x8)`; before sleep `dsp_power: button wake armed (GPIO3, active low)`
  or `dsp_power: button active at sleep entry: GPIO wake not armed, timer 60 s (planned N s)`.

```sh
# BUILD-ONLY bench variant (BLE bring-up + Wi-Fi time + GPIO pump signal + wake button)
idf.py -B build-button -D SDKCONFIG=build-button/sdkconfig \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ble.defaults;<bring-up fragment>;sdkconfig.wifitime.defaults;sdkconfig.pumpgpio.defaults;sdkconfig.button.defaults" build
```

- **Debounce (review 2026-11-21, no filter added):** the wake cause is read once at boot from the
  latched RTC wake register, and GPIO3 has no interrupt or polling while awake, so contact bounce
  after the first low level (during boot or the window) cannot create a second event; deep-sleep wake
  sources are re-armed only at the next sleep entry. A short press that already ended at boot is still
  a BUTTON wake (GPIO3 need not stay low). Bounce at release right before sleep entry is caught by the
  5-sample check (any low sample → timer only, ≤ 60 s). Residual race: a press or a release bounce
  between the last sample and `esp_deep_sleep_start` gives one immediate BUTTON wake and one extra
  window; the next sleep entry samples again, so it cannot repeat without further presses (accepted
  bench limitation). The 60 s cap is `min(planned, 60)`: an earlier dose wake is never postponed. The
  active-mode GPIO glitch filter is not relied on; in deep sleep any low level of ≥ 3 RTC slow-clock
  cycles (~22 µs) wakes the chip, so noise on long wires could wake it (an external pull-up or a small
  capacitor helps; not seen on the bench).

Bench verified 2026-11-21 (`HARDWARE_ACCEPTANCE.md` §8e, smoke test W0–W4, internal pull-up only);
the held-button path and a natural timer wake were not run on hardware.

### Battery voltage (`sdkconfig.battery.defaults`, firmware 0.2.0, BUILD-ONLY)

The measured battery voltage is reported to the authenticated app (GET_BATTERY, BLE_PROTOCOL 7.7).
No dosing decision depends on it in this milestone (no low-battery inhibition yet).

- **Circuit:** XIAO BAT+ → R1 1 MΩ → D2 / A2 = GPIO4 (ADC1_CH4) → R2 1 MΩ → GND; an optional
  100 nF capacitor from GPIO4 to GND. GPIO4 is an ADC1 pad (ADC2 is not usable with the radio),
  not a strapping pin, and differs from the wake button (GPIO3) and the pump pin (GPIO10)
  (compile-time checks in `dsp_esp_battery.c`). The divider draws ~2 µA permanently.
- **Configuration:** `CONFIG_DSP_BATTERY_ADC_GPIO` (-1 = none, the default image) and
  `CONFIG_DSP_BATTERY_DIVIDER_X1000` (2000 for equal resistors; may be trimmed after a multimeter
  comparison at BAT+). Build with `…;sdkconfig.button.defaults;sdkconfig.battery.defaults`.
- **Measurement:** ESP-IDF ADC oneshot driver, 12 dB attenuation (about 0–2.5 V at the pad; the
  midpoint of a 1S cell is 1.5–2.1 V), curve-fitting calibration. The divider is a ~500 kΩ source,
  so the sampling capacitor needs several conversions to settle: 4 conversions are discarded, then
  16 calibrated readings are taken back to back and the mean of the middle 8 is used (no delays,
  about 1 ms). States: VALID (2,500–4,600 mV), NO_CALIBRATION (no eFuse calibration: no voltage is
  guessed), ADC_FAILED, OUT_OF_RANGE (e.g. divider open, ADC saturated), UNSTABLE (kept readings
  spread more than 100 mV at the pad: fit the 100 nF capacitor). Pure logic and limits:
  `dsp_platform_logic.c` (`test_platform`).
- **Sampling policy:** once per wake (power-on, button, timer: scheduled dose or SNTP) right after
  SAFE_OFF, before LOAD, any dosing, Wi-Fi or BLE; re-measured on GET_BATTERY only when the value is
  at least 2 s old and no pump run ended within 2 s (the app's manual refresh is a new sample;
  `age_s` reports the real age of whatever is returned). Never a reason to wake, never continuous, runtime
  memory only (nothing in NVS). One log line per measurement:
  `dsp_battery: battery: VALID 3970 mV (pad 1985 mV, spread 4 mV, divider x2000/1000)`.
- **Clients:** GET_BATTERY is new in firmware 0.2.0; clients send it only when the Info record
  reports 0.2.0 or newer (BLE_PROTOCOL 8.2). Domain: `dsp_app_get_battery`, `test_battery_app`.
- **BATTERY_LOW notification:** in OBSERVE (before any pump run and before Wi-Fi) the latest
  measurement feeds the existing engine (`dsp_notify_observe_battery`: one event per episode,
  7-day reminder, queue, stable IDs, frozen payload, delivery with the next regular sync session).
  Hysteresis on the measured voltage: an episode opens below `CONFIG_DSP_BATTERY_LOW_MV`
  (default 3,900 mV) and closes at or above `CONFIG_DSP_BATTERY_RECOVER_MV` (default 4,050 mV);
  in between nothing changes. Opening or closing needs a second measurement in the same wake
  that agrees (else UNKNOWN: one disturbed reading changes nothing). Anything but VALID is
  UNKNOWN: never an event, never 0 mV. The engine writes the "notify" record only when the
  episode state changes. No dosing restriction. Defaults (BENCH PROPOSAL): the temporary 5 V
  relay module on the 1S cell is the limiting part — a 5 V coil typically needs ≥ 75 % (3.75 V)
  to pull in, the module drops a little more and the cell sags while the motor starts; the ADC
  read ~2 % low on the bench (4.07 V vs 4.15 V at BAT+, while charging), so 3,900 mV measured is
  about 3.95–4.0 V at the cell. Recovery at 4,050 mV practically needs a charge (no LOW/OK
  flapping from rest recovery). Validate the relay's pull-in voltage on a bench supply.
- **Delivery needs an HTTPS build:** the bench image (`CONFIG_DSP_APP_WIFI_TIME`) queues the event
  but has no HTTP client (time-only sessions); delivery needs `CONFIG_DSP_APP_CONNECTIVITY`, which
  the build refuses together with the production endpoint while BLE security/transport, the
  bring-up window or the GPIO pump are enabled (R5 not accepted, bench-only pump). Hardware
  end-to-end check: the diagnostic image `sdkconfig.batterynotify.defaults` (one BATTERY_LOW test
  event with the measured voltage through the real ESP32 TLS transport, then the identical
  retry; no application runtime).
- **Opt-out (`notify_mask`):** enforced by the engine since a27e640+: a category whose bit is clear
  queues nothing and sends nothing (RESERVOIR_LOW/BLOCKED = bit 0, BATTERY_LOW = bit 1,
  DEVICE_ERROR/DOSING_ERROR = bit 2); conditions are still tracked (no flood when re-enabled);
  already queued entries wait, are not pending and start no session. Default: all enabled.
- **Normal image with HTTPS:** `sdkconfig.notify-staging.defaults` (see HARDWARE_ACCEPTANCE 8f).
- **Hardware verification (pending):** compare the reported voltage with a multimeter **at BAT+**
  (a 10 MΩ meter at the 500 kΩ midpoint itself reads about 5 % low); confirm whether the 100 nF
  capacitor is fitted; check that the voltage is VALID on button and timer wakes.

### Wi-Fi time synchronization only (`CONFIG_DSP_APP_WIFI_TIME`, bench MVP)

A second, mutually exclusive opt-in beside `CONFIG_DSP_APP_CONNECTIVITY` (fragment
`sdkconfig.wifitime.defaults`, build-only): the application cycle synchronizes the clock over
Wi-Fi/SNTP **without** HTTPS. `main.c` wires the Wi-Fi manager and the SNTP source only
(`https = NULL`); no endpoint, API token or HTTP client is used, and the ESP-IDF HTTP client and
TLS are not linked into the image. `dsp_connectivity_run()` with `https == NULL`:
- runs only for a **due regular time sync** (M1.4 interval and backoff, unchanged); a pending
  CRITICAL notification never starts a session, and nothing is delivered (the queue is untouched);
- worst case without the HTTPS budget: 3 × 15 s Wi-Fi + 10 s SNTP = **55 s**, also used by the dose
  guard;
- trust, plausibility and persistence come from the unchanged timekeeper (`time` record saved in
  SYNC); a failed Wi-Fi connect or SNTP request never produces trust; Wi-Fi is shut down before
  returning, and SHUTDOWN verifies it is off before deep sleep.

Order in a wake: LOAD → CLOCK → DOSE → **CONNECT (Wi-Fi + SNTP, Wi-Fi off)** → DOSE (re-evaluated
with the new clock) → BLE service window (NimBLE starts only here) → SYNC (time metadata) → PLAN →
SHUTDOWN → SLEEP. On a presence wake with a due sync and an unreachable network, the BLE window
therefore opens up to ~55 s later. BLE `SET_TIME` stays available as a fallback. Log lines (no
SSID, passphrase or token): `dsp_wifi: connected (IP) in N ms` / `connect failed after N ms (reason R)`
/ `stopped (radio off)`, `dsp_sntp: time sample received in N ms`, `dsp_clock: clock stepped by N s`,
`dsp_esp_ble: radio up`.

```sh
# BUILD-ONLY bench variant (BLE bring-up + Wi-Fi time sync)
idf.py -B build-wifitime -D SDKCONFIG=build-wifitime/sdkconfig \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ble.defaults;<bring-up fragment>;sdkconfig.wifitime.defaults" build
```

Limitation: `nvs` and `nvs_sec` are **not encrypted** (no flash encryption, no NVS encryption
keys); the Wi-Fi passphrase, setup code and ownership state can be read from the flash by anyone
with physical access. Acceptable only on the bench with synthetic credentials and a dedicated
bench Wi-Fi network.

### Request and response handling (approved D8, R1)

The body is the exact protocol-v1 JSON (≤ 512 B; the token is never in it). The response
is classified strictly; the priority is that a **systemic** configuration or protocol
problem can never drain the queue:

| Response | Result | Persistent diagnostic |
|---|---|---|
| 201 + `{"status":"accepted","event_id":<sent ID>}`, 200 + `{"status":"duplicate",…}` | ACK | all flags cleared |
| 200/201 with a missing, malformed or mismatched body | UNCERTAIN (kept) | PROTOCOL |
| 409 + `{"status":"conflict","event_id":<sent ID>}` | REJECTED | — (rejection log) |
| 400 + `{"status":"invalid","error":"invalid_field","field":F}`, F an event-content field (`type`, `severity`, `code`, `reminder_no`, `created_at`, `uncertain`, `manual`, `value`, `value2`) | REJECTED | — (rejection log) |
| any other 400 (ambiguous: malformed JSON, schema, missing/unknown field, `event_id`/`device_id`/`attempt`/`body`) | FAILED (kept) | PROTOCOL |
| 401 | FAILED | AUTH |
| 404, 405, 3xx (redirects are never followed) | FAILED | ENDPOINT |
| 413, 415, 409 without the confirming body | FAILED | PROTOCOL |
| 429, 5xx, anything else | FAILED | — |
| DNS/TCP or TLS/certificate failure (nothing sent) | FAILED | — |
| request (partly) sent, then timeout or reset | UNCERTAIN | — |

FAILED and UNCERTAIN keep the entry and end the session (M1.5, unchanged). At most **2
REJECTED per session** (extra safeguard; the classification above is the real protection).

### Rejection log and diagnostics (approved D7)

Both live in the `notify` record (format 2), so they share its atomicity and durability.
- **Rejection:** the log entry and the queue removal are written in **one** write before
  anything else; if it fails, the entry stays queued exactly as it was and the session
  stops. The log keeps the **4 most recent** rejections (sequence number, type, code, HTTP
  status, closed `error`/`field` enums, attempts), a saturating `total`, an `unread` count
  and `lost_unread`: when the log is full, the oldest entry is dropped, and if it was unread
  `lost_unread` counts it. Acknowledging (`dsp_notify_ack_rejections`, future BLE) clears
  `unread` and `lost_unread`; entries and `total` stay.
- **Flags** AUTH / ENDPOINT / PROTOCOL (plus the HTTP status that last set one) survive
  restarts. **Clearing conditions:** the next acknowledged delivery (proof that credentials,
  endpoint and protocol work end to end) or `dsp_notify_clear_tx_diag()` (future BLE).
- Format 1 records (M1.5) are read as "no diagnostics" and upgraded by the next write.
  **Formats are forward-only** (approved R3): firmware that only knows format 1 reports
  format 2 as unsupported and fails closed (no automatic dosing). A future OTA milestone
  must therefore never roll back across a record-format change.

### Credentials (approved D5: development only)

- Wi-Fi SSID, passphrase and `DEVICE_API_TOKEN` live in the dedicated **`nvs_sec`**
  partition (namespace `dsp_sec`, keys `wifi_ssid`, `wifi_pass`, `api_token`), separate from
  the storage layout. The firmware opens it **read-only**, never writes or erases it, and
  never logs a value. The token is fetched per request into a buffer that is wiped right
  after; the platform deletes `esp_http_client`'s header copy after every request.
- **Never** in source code, `sdkconfig`, build artifacts, test fixtures, logs or Git. Tests
  use generated dummy values only.
- **Developer provisioning** (outside the repository, inside an activated ESP-IDF shell):

  ```sh
  umask 077
  mkdir -p ~/.config/dispenser-device
  # create ~/.config/dispenser-device/dev-secrets.json (mode 0600) with an editor:
  #   {"wifi_ssid": "...", "wifi_pass": "...", "api_token": "..."}
  python firmware/tools/provision_dev_secrets.py \
      --secrets ~/.config/dispenser-device/dev-secrets.json \
      --out ~/.config/dispenser-device/nvs_sec.bin
  python -m esptool --chip esp32c3 write-flash 0x28000 ~/.config/dispenser-device/nvs_sec.bin
  ```

  The tool refuses repository paths and a secrets file readable by group/others, never
  prints a value, deletes its temporary CSV and writes the image with mode 0600. **The
  image is not encrypted: treat it as sensitive and delete it after flashing.** `idf.py
  flash` does not touch `nvs_sec`; `idf.py erase-flash` wipes it.
- **Known limitations (documented, accepted for development):**
  - **Physical access:** `nvs_sec` is not encrypted (no flash or NVS encryption, no eFuse
    burned). Anyone with the device and a USB cable can read the Wi-Fi passphrase and the
    token.
  - **Shared token:** the backend has one `DEVICE_API_TOKEN` for all devices (v1). A token
    read from one device lets an attacker submit events (and so cause e-mails) for any
    device ID; there is no read API. Per-device credentials and authenticated BLE
    provisioning are a future milestone; the reserved `nvs_keys` partition allows NVS
    encryption later without changing the partition table.
  - ESP-IDF does not wipe freed heap: the header copy may remain in freed memory until it
    is reused.

### Partition table (4 MB, M1.7)

[`partitions.csv`](partitions.csv), partition table at **0x10000**
(`CONFIG_PARTITION_TABLE_OFFSET`, room for a larger future secure-boot bootloader). Verified
with ESP-IDF's `gen_esp32part.py` (fills exactly 4 MB; app slots 64 KiB aligned, data
partitions 4 KiB aligned, `otadata` exactly 2 sectors).

| Partition | Type / subtype | Offset | Size |
|---|---|---|---|
| (bootloader) | — | 0x000000 | ≤ 64 KiB (currently 21,152 B) |
| (partition table) | — | 0x010000 | 4 KiB |
| `nvs` | data / nvs | 0x011000 | 80 KiB (20 pages) |
| `otadata` | data / ota | 0x025000 | 8 KiB |
| `phy_init` | data / phy | 0x027000 | 4 KiB |
| `nvs_sec` | data / nvs | 0x028000 | 24 KiB (6 pages) |
| `nvs_keys` | data / nvs_keys, encrypted | 0x02E000 | 4 KiB (reserved, unused) |
| `ota_0` | app / ota_0 | 0x030000 | 1.75 MiB |
| `ota_1` | app / ota_1 | 0x1F0000 | 1.75 MiB |
| `coredump` | data / coredump | 0x3B0000 | 64 KiB (reserved; core dumps not enabled) |
| `reserve` | data / 0x40 | 0x3C0000 | 256 KiB (raw reserve, e.g. a future event log) |

- **Initial transition:** NVS moves from 0x9000 to 0x11000. The first flash with this table
  must write the complete image set (bootloader, partition table, `ota_data_initial`, app);
  bench data in the old layout is lost. Run `esptool flash-id` first.
- **OTA is not implemented** (no update client, no rollback, no signed images). The table
  and the bootloader cannot be changed over the air, so this layout is permanent for any
  device in the field; image size is bounded by the 1.75 MiB slot.

### Resources

Measured with ESP-IDF v6.1 for `esp32c3` (clean builds, the 4 MB table, the full certificate
bundle; zero compiler warnings). The slot is 1.75 MiB = 1,835,008 B.

| Configuration | App binary | Free in the slot |
|---|---|---|
| default (`sdkconfig.defaults`) | 148,480 B | 1,686,528 B (92 %) |
| HTTPS diagnostic only (Wi-Fi, SNTP, TLS, `esp_http_client` linked) | 1,041,472 B | 793,536 B (43 %) |
| all diagnostics (storage check, time sync, HTTPS) | 1,054,016 B | 781,000 B (43 %) |

- Sizing builds during the design review (not part of the repository): NimBLE with its
  default configuration (peripheral, one GATT service) added about **256 KB** of flash and
  27 KB of static DRAM on top of HTTPS and the full bundle, which would leave about 29 % of
  a 1.75 MiB slot. The full bundle costs about 51 KB more than the common subset.
- Code (`-Os`, RISC-V, `.text` per object): `dsp_notify_proto` 3.9 KB, `dsp_https_transport`
  0.9 KB, `dsp_connectivity` 1.3 KB, `dsp_notify` 5.2 KB (was 4.2 KB), `dsp_codec` 4.6 KB,
  `dsp_notify_state` 1.4 KB; platform `dsp_esp_http` 1.6 KB (+720 B static: URL and header
  buffer), `dsp_esp_device` 0.7 KB.
- Static DRAM with all diagnostics: 117,152 B used, 204,144 B free at build time. RAM
  (caller-owned): the `notify` state grows by 76 B; `dsp_https_transport_t` is about 1.3 KB
  (request body, token buffer, response prefix). The TLS handshake heap peak (mbedTLS with
  dynamic buffers) and its duration are **not measured** (no board); BLE and Wi-Fi/TLS are
  never active at the same time by design.
- Energy: no measurement. Estimates from the design review (about 1–2 s of extra radio and
  CPU time when HTTPS rides on a sync session) are not results.

## Verification levels (M1.8)

| Behavior | Host-verified (CTest, fakes, ASan/UBSan) | Locally integrated (`test/integration`) | Hardware |
|---|---|---|---|
| Notification engine, queue, freeze, rejection log, diagnostics | yes | yes (with the real Worker, power cuts at every write) | not run |
| Protocol encoder / response classifier | yes, plus shared vectors with the backend | yes (real Worker responses) | not run |
| HTTPS transport (domain) and session budget | yes | yes (real monotonic clock) | not run |
| TLS | — | OpenSSL with certificate-chain and host-name verification (untrusted CA, wrong host, TLS black hole) | **mbedTLS + certificate bundle unverified** |
| `esp_http_client` adapter, keep-alive, phase mapping | — | — (host port instead) | **unverified** |
| Backend contract (201/200/400/401/404/405/409/413/415, idempotency, D1, scheduled e-mail) | shared vectors | yes (committed Worker, local D1, fake Gmail) | staging (needs approval) |
| Connectivity session (D1-B, D4, guards) | yes | yes (fake Wi-Fi/NTP/clock) | **real Wi-Fi/SNTP unverified** |
| `nvs_sec` credentials, NVS on flash, partitions | provisioning tool tested with dummy values | — | **unverified** |
| Energy, TLS heap and timing | — | — | **not measured** |

Hardware behavior is listed, with pass criteria, in
[`HARDWARE_ACCEPTANCE.md`](HARDWARE_ACCEPTANCE.md).

## Build (ESP32-C3, native)

```sh
. ~/.espressif/tools/activate_idf_v6.1.sh     # once per shell
cd firmware
idf.py build                                  # sdkconfig is generated from sdkconfig.defaults
idf.py size
```

`sdkconfig` is not committed. It is generated from `sdkconfig.defaults` (which sets
`CONFIG_IDF_TARGET="esp32c3"`) plus ESP-IDF v6.1 defaults, so `idf.py set-target` is not
needed. ESP-IDF v6.1 marks every value it took from a default with `# default:` and
re-derives those values from `sdkconfig.defaults` at each reconfigure; a value set
explicitly (menuconfig, a hand edit, an older ESP-IDF without markers) is kept and wins over
`sdkconfig.defaults`. Add an option to `sdkconfig.defaults` only together with a reviewed
reason; a different ESP-IDF version may generate different defaults.

**Flash layout guard.** The configure step fails unless the effective configuration is the
approved layout: target `esp32c3`, 4 MB flash, custom `partitions.csv` at 0x10000
([`main/dsp_flash_layout_guard.cmake`](main/dsp_flash_layout_guard.cmake), host test
`test_flash_layout_guard`). An explicitly set wrong value therefore stops the build instead
of producing an image for another layout. The guard runs only when something builds.

**Flash only what you just built.** A flash command writes the files listed in the build
directory's `flasher_args.json`, whatever their age. `idf.py flash` runs the build (and so the
configure step with the flash layout guard) before writing, which reduces the risk of writing
stale artifacts, but it does not make flashing safe on its own: it still writes whatever that
build directory and its `sdkconfig` produce. Writing existing binaries directly bypasses the
build and therefore the guard entirely: `esptool write-flash` with files or `@flash_args`,
OpenOCD `program_esp`, and the VS Code ESP-IDF "Flash Device" command (UART or JTAG), which
does **not** build and writes the existing `build/` as it is. On 2026-11-19 "Flash Device"
wrote a `build/` left over from 2026-11-15 (the pre-M1.7 2 MB single-app layout) to the board
([`HARDWARE_ACCEPTANCE.md`](HARDWARE_ACCEPTANCE.md) section 1, incident record). Therefore:

- in VS Code use "Build, Flash and Monitor", or run "Build Project" immediately before
  "Flash Device", and check the build output (`flasher_args.json`: `--flash-size 4MB`,
  0x0 bootloader, 0x10000 partition table, 0x25000 `ota_data_initial`, 0x30000 app);
- on the command line prefer `idf.py -B <dir> -D SDKCONFIG=<dir>/sdkconfig ... flash` (it
  builds first) and still check that directory's `flasher_args.json`; when writing existing
  binaries directly, check `flasher_args.json` and the image files first, because no guard runs;
- for hardware acceptance images use a fresh, separate build directory and check it before
  flashing: `idf.py -B <dir> -D SDKCONFIG=<dir>/sdkconfig build` (`build-*/` is git-ignored);
- never flash with `erase-flash` (it also wipes `nvs_sec`).

First flash and boot log on the board: 2026-11-19 (`HARDWARE_ACCEPTANCE.md` 1.1–1.3).

The ESP-IDF v6.1 configure step prints five `component_validation` CMake warnings about
`wpa_supplicant` using private `esp_wifi` include directories. They come from ESP-IDF
itself, not from this project. The compile step itself is warning-free.

## Host tests (Linux, no hardware)

```sh
cd firmware
cmake -S test/host -B build-host
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

For a clean run, delete `build-host/` first (it is generated and ignored).

They need only CMake and a C11 compiler: no ESP-IDF, board, serial port, OpenOCD, flashing,
Wi-Fi or BLE. Flags: `-std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`, with
AddressSanitizer and UBSan on by default and `-fno-sanitize-recover=all`
(`-DDSP_HOST_SANITIZERS=OFF` disables them, e.g. on a compiler without sanitizer runtimes).

Harness (`support/dsp_test.h`, header-only, about 80 lines):

- `DSP_CHECK`, `DSP_CHECK_EQ_INT`, `DSP_CHECK_EQ_STR` record a failure with file, line,
  test name, expected and actual value, then continue, so one run reports all failures.
- Each test file is its own executable and CTest process (isolation between files).
  The exit status is non-zero if any check failed **or if no check ran at all**;
  crashes and sanitizer reports also fail the test. Each test has a 30 s CTest timeout.
- **Open stability item (recorded 2026-11-18, M1.10e.2b):** `test_app` takes 17–19 s alone
  (sanitizers on, `ctest -j4`), so its margin to the 30 s timeout is small. Run concurrently
  with the full Android Gradle build on the same machine it exceeded the timeout once; run alone
  it passed (32/32). No GitHub workflow runs the host suite today (the Android CI does not), so
  CI is not affected yet. The timeout is deliberately unchanged: before a host-test CI job is
  added, either split `test_app` or set a measured per-test timeout with the justification
  recorded here; never weaken the test.
- Every executable prints `[PASS]`/`[FAIL]` per test function and a
  `N checks, M failed` summary (`ctest -V` shows it).

The harness stays deliberately small. A test framework (e.g. vendored Unity, which would
need a NOTICE entry) is added only when a concrete need is demonstrated.

Current host tests (CTest names):

| Test | What it checks |
|---|---|
| `test_identity` | firmware identification formatting, version from `version.txt` |
| `test_recurrence_vectors` | all shared vectors in `../test-vectors/` (also run by the Android Kotlin tests), incl. 100 years of DST transitions |
| `test_recurrence_vectors_TZ_*` | the same vectors with host `TZ=America/New_York` and `TZ=Asia/Kathmandu` (host timezone has no effect) |
| `test_recurrence` | day arithmetic vs a naive calendar walk 1999–2101, jump search vs brute force (600 random schedules), buffer paging, argument errors, extreme instants and API range boundaries, identity across edits |
| `test_exec` | execution coordinator and history: normal run, duplicates, restarts, power loss before and after the durable claim, write/commit/read/read-back failures, edits, same-instant schedules, dates and clock rollback, invalid clock, due window, manual independence, capacity/retention, corruption, invalid input; a randomized 60-day run with injected faults and restarts (no duplicate or late authorization) and a fault-free 200-day run (every occurrence exactly once) |
| `test_storage` | persistent storage (including the v1/v2→v3 and v1/v2/v3→v4 migrations, history and configuration format 2 golden bytes): first boot and provisioning (incl. power loss after every write), configuration save/reload and validation, schedule create/edit/delete with revision and ID rules, capacity and tombstone ring, power loss before/after commit (NVS-like and staged modes), marker loss, invalid/truncated/oversized/foreign records, unsupported versions, duplicate IDs, M1.2 coordinator on persistent history (missing slot = corruption, never empty), read/write/commit/no-space/read-back failures, golden bytes, maximum record sizes, randomized operation sequences with faults (always old or new set) |
| `test_time` | clock trust (first boot, deep sleep vs power loss, anomalies), sync interval 24/48 h, Wi-Fi and NTP failures with bounded backoff, implausible NTP, long outage (60 days of dosing in holdover), small/large forward/backward corrections, rollback hold (persisted, lifted), jumped-over protection, DST, restarts around a dose, late wake, storage and clock-step failures, Wi-Fi session lifecycle, wake planning; two randomized multi-year runs (realistic RTC; RTC glitches) with end-to-end dosing: never the same occurrence twice, never twice on the same true day under the trust assumption |

| `test_notify` | notification engine (M1.5, extended in M1.7): reservoir threshold, hysteresis, refill, battery, dosing and device errors, deduplication without writes, payload freshness (latest value until the first attempt, frozen afterwards), freeze surviving power loss after the receiver accepted (same ID, same payload, nothing lost) and zero transport calls when the freeze write fails, Wi-Fi failure, ACK, uncertain delivery and retry with the identical ID, durable before sent, restarts, power loss during queue updates and after an ACK, weekly reminders, backward/forward clock corrections, unknown clock, capacity exhaustion and overflow report, priority and no CRITICAL eviction, 120-day outage, manual dose consumption, single-use dose permits, uncertainty set by dosing errors and cleared by refill, golden bytes and strict decoding of `notify`, and a randomized 4000-step run with faults and restarts (one content per ID); M1.7: permanent rejection logged and removed in one write (also with the write failing outright, landing without confirmation, and a power cut at every write position: never lost, never both queued and logged), at most 2 rejections per session, log overflow with `lost_unread` and acknowledgement, diagnostic flags surviving restarts and their clearing conditions, a rejection without a 4xx treated as FAILED, the session budget leaving unstarted entries untouched, `notify` format-2 golden bytes and strict decoding, format-1 records read and upgraded |
| `test_proto` | protocol v1 (M1.7): golden request bodies (incl. the PROTOCOL.md example, every code name, extreme values, 48-bit device ID), refusal of invalid notes and too-small buffers, every response rule (strict 200/201 acknowledgement with the exact production bodies, R1 rules for 400/401/404/405/409/413/415/3xx/429/5xx, connect/TLS/send/no-response phases), and 20,000 fuzzed bodies (no crash, no ACK/REJECTED without the confirming body) |
| `test_proto_vectors` | the shared protocol v1 vectors (M1.8, also run by the backend's Vitest): exact request bodies and canonical payloads, notes the encoder must refuse, the firmware classification of every backend response (byte-exact bodies) and of synthetic responses |
| `test_dosing` | Dosing Service (M1.9a) over the real coordinator, storage and engine with a fake pump that verifies complete durable state at every activation: default SERVICE_REQUIRED and the checked READY transition, every missing/invalid calibration or limit (no actuation, never BLOCKED), overflow-safe runtime, write order and durable claimed volume, same-time schedules and the due window, per-dose/runtime/daily/interval limits, midnight, both DST days, rollback and untrusted time, restart in cooldown, BLOCKED latch (automatic and manual refused until refill), refill with a power cut at every write (dosing disabled until complete), pump faults, storage failures at every write (DEGRADED), a dose report whose `notify` write fails (durable source kept, dosing disabled for the wake, exactly one report whether flushed in the wake or recovered at boot, never repeated) and reports with no durable source (not invented), a refused same-time dose that becomes MISSED without catch-up, **exhaustive power-loss matrices for automatic and manual doses with a second cut inside recovery** (never a pump run in recovery, interruption reported exactly once with the durable volume, estimate unchanged), format-1 history recovery, and a 3000-step randomized run with cuts, restarts, refills and clock steps |
| `test_app` | Application and Power Services (M1.9b): complete wake cycles over the real domain services with fakes for the pump, KV (power cuts that abort the wake via longjmp), clock and RTC continuity, NTP, Wi-Fi, HTTP, BLE service mode and the sleep port with RTC memory. Oracles on every pump start (durable claim/marker, last start and consumption; no radio active; inside the due window; no duplicate) and every sleep (pump OFF, radios off, no HTTPS session). Retained-block codec (every bit flip rejected) and plan rules; first boot and exact stage order; scheduled, same-time (interval retry inside the window, else missed) and missed doses; manual dose and bounded queue in service mode; BLE session lifetime (180 s idle, kept while connected, idle restart after disconnect, 15 min and configurable maximum, wall-clock steps ignored, no resume past the deadline, queue lock balance and no posting outside a session); SERVICE_REQUIRED, READY, BLOCKED and refill (dose wakes suppressed and restored); Wi-Fi unavailable (backoff, dosing unaffected); CRITICAL on an existing wake and the hourly limit; BLE suspended for a due dose and resumed; DEGRADED and storage failures; clock rollback (2 h, > 7 days) and DST; SAFE_OFF/shutdown/BLE confirmation failures and the fail-safe inhibit; **power loss at every stage (2 scenarios × 4 reboot variants)**; **two 365-day simulations** (clean: every expected occurrence exactly once; chaotic: cuts, resets, Wi-Fi faults, clock steps, pump faults, button sessions) |
| `test_app_ops` | M1.10b.1 operations through complete wakes (operations run from the BLE fake's `wait()`, i.e. the application task, like the future dispatcher), oracles on every pump start (no BLE, no operation in progress, inside the due window, never before the edit floor, no duplicate). Authorization fails closed (NULL, non-owner, bad slot: FORBIDDEN, no write; BUSY outside a session; DEGRADED storage); phone time: initial sync without catch-up, plan follows, SYNCED ignored, uncertainty and range boundaries (incl. INT64 extremes), HOLDOVER acceptance and a 2 h step back without re-running a claimed occurrence, > 7 days back and recorded holds refused, forward step skips jumped-over doses, write and clock-set failures, power cut at every write; configuration: field mask, read-only fields, stale revision, every calibration/limit field refused while READY, capacity rules, 25 field boundaries, SERVICE_REQUIRED never enables dosing, refill pending, write failure ends dosing for the wake, power cuts; schedules: CRUD across restarts with names, 30 validation and name cases, revisions, duplicate and reused IDs, capacity and paging, no resurrection after edits, edits near the due window, delete + recreate, Prague DST (fall back and spring forward), plan follows changes, write failure and power cuts in create/update/delete (old or new set, floor durable), format-1 upgrade |
| `test_manual` | M1.10b.2 anti-replay through complete wakes (harness of `test_app_ops`; oracle on every manual pump start: durable decision bound to the request, high-water mark raised, PENDING result, no request ever twice): accept / duplicate / invalid / floor, duplicates before, during (re-entrant from the pump) and after a dose, lost response and reboot, ring eviction (UNAVAILABLE, never executed), counter gaps, independent slots, exhaustion at 2^63 - 1, fail-closed authorization and the retired queue command, refusals followed by changed conditions (readiness, refill, pump fault in the wake), storage failures at the refusal, decision and consumption writes, power loss at every write of an accepted request (landed or not) and during the pump, power loss at the refusal write, format 1 -> 2 upgrade with a cut at the upgrade write (readiness, BLOCKED, refill and ledger preserved), a format-1 in-flight marker, corrupt and unsupported records (never treated as fresh), format-2 golden bytes, BLE suspend/resume and no resume after the 15-minute deadline |
| `test_battery_app` | Battery telemetry through complete wakes (harness of `test_app_ops`, scripted battery port): one measurement per wake of every cause, before the pump run of a scheduled dose, sleep plan unchanged (no added wake), nothing stored; GET_BATTERY cached value with its age, a new sample from 2 s on, no measurement within 2 s after a manual pump run; explicit unavailable states (never 0 mV), port normalization, window boundaries, no port = NOT_MEASURED, fail-closed authorization. Pure ADC logic (trimmed mean, divider, window, spread, pin checks) in `test_platform`; the encrypted end-to-end request in `test_blesec` |
| `test_auth` | M1.10b.3 pure units: `auth` golden bytes (built field by field), every validity rule at byte and model level, claim/add/remove/key fault/recover transitions and refusals (unchanged input), epoch increments and saturation, last_seen policy, zeroization; retained block format 2 (round trip, CRC, format 1 discarded, CRC-valid inconsistent overlays dropped while the fail-safe counter survives); overlay rules for every reset cause, expiry boundaries, deep sleeps never extending, raw clock regression, epoch binding and the degraded sentinel, abort counting; the 3-press confirmation (exact, 2, 4, long, slow, gaps, guard/queued/replayed events, out of order, deadline edges, clock back) |
| `test_owner` | M1.10b.3 ownership through complete wakes (harness of `test_app_ops`, fake security registration): no security layer = nothing authorized and nothing written; claim with one write and only `auth` changed; forged, replayed, other-slot, other-session and wrong-role authorizations; handshake handles mismatched, expired, consumed, replayed (CLAIM and SESSION), RNG failure; invite single use (incl. an old RTC image restored), expiry boundary, every reset type, repeated resets, clock steps; revocation (only one slot, epoch + 1, manual marks untouched), self-removal, last phone, slot reuse continuing above the old high-water mark; recovery (byte-identical preservation, BLOCKED and manual marks kept, old phones gone), **two consecutive power cuts inside recovery** (every combination around the SERVICE_REQUIRED and the new `auth` write, landed or not, each followed by a reboot: old or new record only, SERVICE_REQUIRED before any new owner, no automatic or manual dose, stale authorizations refused, re-arm required, clean retry completes), arming rules (9,999 ms, timer wake, unclaimed, window edge, cancel, epoch change), confirmation failures (deadline, queued/old events, wrong transcript, single use, session end, due dose); power loss at every write of recovery and of claim/add/remove (landed or not); missing/malformed/newer `auth` block dosing and only RECOVERY heals; KEY_FAULT inhibits at once even when not durable; durable admission (limit boundaries, decided retries answered, reboot, untrusted clock, next day, rollback) and session-local limits; secret hygiene (no private key in the application state, storage buffers or RTC) |
| `test_sec_header_guard` | M1.10b.3 configure-time boundary check for `dsp_app_sec.h` (`cmake -P`): allowed files pass, violating sources and headers are reported, the firmware tree is clean |
| `test_endpoint_guard` | M1.9c build-time endpoint guard (`cmake -P`): runtime disabled, runtime without connectivity, connectivity without an endpoint (fails), explicit staging endpoint, production without approval (fails), production with approval; production-host spellings, non-https and host-less URLs, a look-alike host, and the guard's production host equal to the `DSP_NOTIFY_URL` default |
| `test_bleproto` | M1.10a BLE protocol v1 codec: every record of `test-vectors/ble_protocol_v1.txt` (59 messages decoded field by field and re-encoded byte for byte, 63 rejections with their reason, encrypted headers, forward compatibility, negotiation, fragmentation for MTU 23–517, reassembly errors and timeout, Info and advertising records), canonical re-encoding of every accepted bit-flipped frame (12,968 mutations), rejection of every body truncation, 200,000 random frames and 200,000 random fragments, encoder refusals |
| `test_platform` | M1.9c pure adapter rules (`dsp_platform_logic.c`, compiled unchanged into the firmware): reset/wake mapping for every reset kind and wake-source combination, retained block and clock continuity agreeing on every reset, sleep clamping, button pin check (strapping and non-wake pins rejected), RTC header (power-on garbage, torn header, oversize, corrupt data rejected by the domain CRC), backoff and hourly limit preserved through the RTC buffer, no clock trust from retained data; and a **two-thread** queue test (a producer thread posting 400 commands, queue full and session-closed refusals, while the application task runs a service session under a mutex): every accepted command answered exactly once. Also run separately under ThreadSanitizer (0 races; a lock-free mutant reports races) |
| `test_ble_transport` | M1.10d.1 BLE byte transport (`dsp_ble_transport.c`, compiled unchanged into the firmware) over a fault-injecting radio with an error-checking lock (nesting aborts): fragmentation and reassembly at every ATT MTU 23–247 (+517 clamped) and boundary lengths, MTU change between frames, every fragment fault and the 2 s gap (±1 ms), the transport's own fault limit, 200,000 fuzzed fragments, queue refusal without any reply, asynchronous TX retries (never blocking), retry limit, absolute deadline, stack errors, ring capacity, unsubscribed clients, disconnect during RX/TX/inside a notification (also with a reconnect on the same handle), stale completions/callouts/generations, advertising only from the application task, fast/slow advertising, Info and payload restarts, refused second and suspended links, suspend with and without a GAP disconnect (synthesized report), close after the final reply, stop/is_off; and a **two-thread** test (simulated NimBLE host task vs the application task, one reused connection handle): no fragment ever on another link. Also run under ThreadSanitizer (0 reports) |
| `test_ble_transport_symbols` | link-time boundary: the transport object references only the three `dsp_app_post_*` producers, the fragmentation codec and `mem*` |
| `test_handshake_vectors` | the shared transcripts `test-vectors/ble_handshake_v1.txt` (CLAIM, SESSION + INVITE_CREATE, ADD, SESSION, RECOVERY, SESSION; generated by the independent Rust `snow` tool) through the real `dsp_app` + `dsp_blesec`: with the device's randomness scripted per transcript (host-only `test_rng_script`), all 17 device frames are byte-identical and every phone frame is accepted; the 7 device-side negative records answer HANDSHAKE_FAILED or close without any reply and store nothing |
| `test_ble_e2e`, `test_ble_e2e_mtu64`, `test_ble_e2e_mtu247` | the complete `test_blesec` suite (CLAIM, SESSION, ADD, RECOVERY, cache, manipulations, lifecycle, fault policy) with the noise-c phone talking **through the transport**: fragmented ATT writes in, reassembled notifications out, at MTU 23, 64 and 247 |
| `test_https` | HTTPS transport and connectivity session (M1.7) over fakes: exact body and token handed to the port, token wiped after every request, token validation (length, spaces, control characters); **systemic 401/404/405/413/415, ambiguous 400, mismatched 409, redirects, 429 and 5xx never drain the queue** (20 sessions each, then recovery); event-specific 409/400 removed and logged; mismatched acknowledgement UNCERTAIN, then duplicate ACK with an identical payload; timeout after the backend accepted plus power loss; connect/TLS failures; the 60 s budget with a monotonic clock; session gating: dosing/permit/BLE guard, next-dose distance, regular sync vs CRITICAL on an existing wake, the 1-hour limit, the Wi-Fi backoff, untrusted time (nothing sent) vs trusted HOLDOVER, missing token, Wi-Fi always shut down |

Python tool test (no ESP-IDF needed; the real-generator case runs inside an ESP-IDF shell):
`python3 tools/test_provision_dev_secrets.py`.

Local end-to-end integration (M1.8; local services and synthetic credentials only): one
command, `test/integration/run.sh` (prerequisites: Node.js ≥ 22.12, OpenSSL, CMake, `npm ci`
in `cloudflare/dispenser-notify`; non-zero exit on failure; cleans up all processes and
temporary data). See [`test/integration/README.md`](test/integration/README.md).

Planned host coverage (later milestones): dose limits, service-mode transitions, event journal, NVS adapter (on target). The Android domain
(`android/app/src/main/java/cz/nowass/dispenser/domain/`) is the behavioral reference.

## Devcontainer (optional)

`.devcontainer/` provides an isolated ESP-IDF v6.1 environment for VS Code *Dev
Containers* (open the `firmware/` folder, then *Reopen in Container*). It is an
alternative to the native setup, not a requirement.

- **Pinning:** the Dockerfile uses `espressif/idf:v6.1@sha256:81893c71…edb18c`, the
  multi-arch (amd64/arm64) index digest of the `v6.1` tag, verified against Docker Hub
  on 2026-11-15. The digest makes the image immutable. A tag alone would not: Docker tags
  can be re-pushed, and `release-v6.1` / `latest` move by design. The image installs
  the tool versions listed by ESP-IDF v6.1 (same as the native install); bit-identical
  output between the two has not been checked.
- **No interference with the native install:** the container has its own ESP-IDF in
  `/opt/esp/idf` and never touches `~/.espressif`. It mounts only the workspace. Use
  a separate build directory and sdkconfig so container and native builds never share
  a CMake cache:

  ```sh
  idf.py -B build-container -D SDKCONFIG=build-container/sdkconfig build
  ```

  With Docker the container runs as root, so files it creates on the mount are owned by
  root. Keep them inside `build-container/` (ignored by Git).
- The ignored local `.vscode/settings.json` (if present) is also visible inside the
  container; its absolute `idf.currentSetup` path does not exist there, so point the
  ESP-IDF extension at `/opt/esp/idf` or use the terminal.
- No USB/serial passthrough and no `--privileged`: flashing from the container is out of
  scope until hardware arrives. Add a specific `--device` when needed.
- Not yet exercised end-to-end on this machine (image not pulled); only the tag and
  digest were verified against the registry.

## Implementation status

| Area | Linux host | Compiled for ESP32-C3 | Physical hardware |
|---|---|---|---|
| Firmware identification formatting (`dsp_identity`) | implemented, unit-tested | yes | not tested |
| `app_main` identification logging | n/a | yes | **not run** |
| Firmware version from `version.txt` | validated at configure time | yes (app descriptor) | not run |
| Recurrence engine (`dsp_recurrence`, `dsp_time`) | implemented, unit- and vector-tested, Kotlin parity | compiled, not called | not run |
| Execution coordinator and history (`dsp_exec`, `dsp_history`) | implemented, tested with fake storage (faults, power loss) | compiled, not called | not run |
| Persistent storage: codec, layout, schedules, config (`dsp_storage`, `dsp_codec`, `dsp_schedule_repo`, `dsp_config`) | implemented, tested with fake key-value store (faults, power loss) | compiled, not called | not run |
| NVS key-value adapter (`dispenser_platform`) | n/a (ESP-IDF only) | compiled; linked with the optional smoke test | **not run on flash** |
| Time management: trust, corrections, sync service, Wi-Fi manager, wake planning | implemented, tested with fakes (clock, Wi-Fi, NTP, storage) | compiled, not called | not run |
| ESP-IDF clock / Wi-Fi / SNTP adapters | n/a (ESP-IDF only) | compiled; linked with the optional diagnostic | **not run** |
| Host test infrastructure (CMake/CTest, sanitizers) | working | n/a | n/a |
| Notification engine and reservoir accounting (`dsp_notify`, `dsp_reservoir`) | implemented, tested with fake key-value store and transport | compiled, not called | not run |
| Protocol encoder/classifier, HTTPS transport, connectivity session (M1.7) | implemented, tested with fakes (port, token, Wi-Fi, NTP, clock, storage) | compiled; linked with the optional diagnostic | not run |
| ESP-IDF HTTP port, device ID, `nvs_sec` credential providers (M1.7) | n/a (ESP-IDF only) | compiled; linked with the optional diagnostic | **not run** (no TLS on a real network yet) |
| Developer credential provisioning tool | tested with dummy values (stub and real `nvs_partition_gen`) | n/a | image never flashed |
| Integration: domain + real TLS + committed Worker + local D1 + fake Gmail (M1.8) | locally integrated (`test/integration`, 1,000 randomized sessions) | n/a | not run |
| Dosing Service, storage layout v4, formats (M1.9a) | implemented, tested with fake pump and fake KV (power cuts, failures, randomized) | compiled, not called | not run; **no real pump driver** |
| Application and Power Services (M1.9b) | implemented, tested with fakes (pump, KV with power cuts, clock, NTP, Wi-Fi, HTTP, BLE, sleep port, RTC memory); 365-day simulations | compiled, not called | not run; **no deep sleep, RTC, button or BLE adapters** |
| Ownership, credential record, storage layout v5, admission (M1.10b.3) | implemented, tested through complete wakes with power cuts (`test_owner`), unit tests (`test_auth`), storage migrations (`test_storage`) and the real NVS (`test/nvs_linux` check 11) | compiled, not linked (no security layer until M1.10c) | not run |
| Manual dose with durable anti-replay (M1.10b.2) | implemented, tested through complete wakes with power cuts (`test_manual`) and service-level tests (`test_dosing`) | compiled, not linked (no caller until the dispatcher) | not run |
| Configuration, schedules, phone time operations (M1.10b.1) | implemented, tested through complete wakes with power cuts (`test_app_ops`) and unit tests | compiled, not linked (no caller until the dispatcher) | not run |
| BLE protocol v1 codec (`dsp_bleproto`, M1.10a) | implemented, shared vectors + property/fuzz tests; Kotlin codec runs the same vectors | compiled, not linked (not called) | not run |
| Platform adapters: power/sleep, RTC block, queue lock, simulated pump, runtime (M1.9c) | pure rules host-tested (`test_platform`, incl. a two-thread queue test; TSan-clean separately) | compiled in four configurations; runtime opt-in | **not run**; no real pump driver, no button pin |
| NimBLE BLE transport and adapter (M1.10d.1) | transport host-tested (`test_ble_transport`, two threads + TSan, symbol boundary), end to end with the noise-c phone at three MTUs (`test_ble_e2e*`) | compiled (`sdkconfig.ble.defaults`, build-only) | **not run** (M1.10d.2 open) |
| Battery monitor | not started | — | — |
| Dosing service, journal, service mode | not started | — | — |
| Motor/pump GPIO, button, indicator LED | not started (no pin map) | — | — |
| Deep sleep and wake-up | domain plan and checklist (M1.9b) | adapter compiled (M1.9c) | **not run** |
| Production credential provisioning, OTA client, deep sleep | not started | — | — |

## Hardware verification still pending

- Reset-state level of every GPIO considered for the pump gate (strapping pins GPIO2,
  GPIO8, GPIO9 must be avoided or analysed), plus the external pull-down that keeps the
  pump OFF before firmware runs (FR-044).
- Button on a deep-sleep wake-capable GPIO (GPIO0–GPIO5) and indicator LED (the board has
  no user LED).
- Deep-sleep current and RTC drift; battery measurement hardware.
- M1.9b/M1.9c: deep-sleep entry only after the checklist, timer and button wake, the
  RTC-retained block across each reset type, the fail-safe reset and the pump gate level after
  it, the application task's stack high-water mark (with and without TLS), BLE suspend/resume
  latency around a dose, wake latency versus the 10-minute due window (see
  [`HARDWARE_ACCEPTANCE.md`](HARDWARE_ACCEPTANCE.md) section 7).
- NVS adapter on real flash: provisioning, power loss during writes, partition sizing and
  the final partition layout (see "Capacity, flash layout and wear").
- Time: RTC continuity per reset type, deep-sleep drift, Wi-Fi/SNTP on a real network,
  energy per sync (see "Hardware validation still required" in the M1.4 section).
- M1.7 (pending, not estimates): TLS heap peak, TLS handshake time (ECDSA P-256 in
  software on the C3), energy per notification session, Wi-Fi/BLE runtime memory, the
  `esp_http_client` keep-alive behavior, the HTTPS diagnostic against a staging backend.

## Next milestone

**Release blocker (open):** the ESP-IDF v6.1 NVS power-loss defect (`test/nvs_linux`,
`HARDWARE_ACCEPTANCE.md` section 9); fixed in software (`third_party/nvs_flash`), bench
verification (9.4) pending.
After the M1.10b.3 review: M1.10b.4 (Wi-Fi provisioning store; factory reset after its own
design review), only after explicit authorization;
then M1.10c–f (session security, NimBLE adapter, Android integration, on-device acceptance); the
hardware review (pin map, pump gate and cutoff, button, battery measurement) is not started.
The on-device acceptance
([`HARDWARE_ACCEPTANCE.md`](HARDWARE_ACCEPTANCE.md)) waits for the board and an approved
staging backend.
