# Dispenser v2 — Android app

Native Android companion app for the Dispenser v2 aquarium liquid-carbon doser
(`cz.nowass.dispenser`). The app works fully offline. **Schedules are real user data,
persisted locally on the phone (Room)**, while device-side values (battery, reservoir,
history) are still simulated and the persistent **DEMO** badge marks them. There is no
Bluetooth, Wi-Fi, cloud or hardware access, and nothing is ever reported as transferred
to the dispenser.

Product and architecture sources of truth live in [`../docs`](../docs).

## Toolchain

| Item | Version |
|---|---|
| Android Gradle Plugin | 9.5.1 (built-in Kotlin) |
| Kotlin / Compose compiler plugin | 2.2.10 |
| Gradle wrapper | 9.6.0 (JAR and distribution checksums verified against gradle.org) |
| Gradle daemon JVM | 25 (`gradle/gradle-daemon-jvm.properties`) |
| compileSdk / targetSdk / minSdk | 37 / 37 / 26 |
| Compose BOM | 2026.02.01 (Material 3 1.4.0) |
| navigation-compose | 2.9.8 |
| lifecycle-viewmodel/runtime-compose | 2.9.4 |
| Room (runtime, compiler, Gradle plugin) | 2.8.5 |
| KSP | 2.3.12 |
| DataStore Preferences | 1.2.1 |
| kotlinx-serialization-json | 1.7.3 (already resolved via Navigation; JSON tree API only) |
| Test: OkHttp MockWebServer | 4.12.0 (test only) |
| Test: kotlinx-coroutines-test / espresso | 1.9.0 / 3.6.1 |
| Test: Robolectric / androidx.test core | 4.17 / 1.6.1 (Robolectric runs on SDK 35) |
| Android NDK (M1.10e.1, native Noise provider) | r30 `30.0.16248370` (`android.ndkVersion`) |
| SDK CMake | 3.31.6 (`externalNativeBuild.cmake.version`) |
| Native sources | `esphome/noise-c` 0.1.31 (`62c2fdca…`) + `esphome/libsodium` 1.10021.12 (`95c5e60d…`), fetched by commit, verified and patched by `../firmware/third_party/noise-c/noise_c_deps.cmake` (same pins and patches as the firmware); ABIs arm64-v8a, armeabi-v7a, x86_64 |

All versions are pinned in [`gradle/libs.versions.toml`](gradle/libs.versions.toml).

## Native Noise provider (M1.10e.1)

`app/src/main/cpp` builds `libdspnoise.so`: the pinned noise-c + libsodium and a thin C core +
JNI glue (no cryptographic logic of its own), used by `cz.nowass.dispenser.crypto`
(`NativeNoiseProvider`, `SecretBytes`, `SetupLabel`, `Invitation`, `credentials/CredentialStore`
with Android Keystore wrapping). Not used by any screen yet: the app behaves exactly as before
(DEMO). Design: `../docs/ANDROID_BLE_DESIGN.md`.

- **First build needs network once:** noise-c and the libsodium fork are fetched by commit into
  `app/build/dsp_deps` (re-verified on every configure). Requires the NDK and SDK CMake above
  (`sdkmanager "ndk;30.0.16248370" "cmake;3.31.6"`), plus `git`, `patch`, a host C compiler and
  Ninja (bundled with SDK CMake) for the host test library.
- **Release safety:** the APK library offers only the initiator role with CSPRNG ephemerals
  (libsodium `randombytes_buf`). A post-build check (`src/main/cpp/check_symbols.cmake`) fails the
  build if a test-only symbol (fixed ephemerals, test hooks) appears or anything but the JNI entry
  points is exported. FORTIFY stays enabled (patch 0001 makes noise-c FORTIFY-clean).
- **Tests:** `./gradlew test` also builds `libdspnoise_hosttest` (the same sources + test-only
  hooks for the responder role and fixed ephemerals) and runs the native vector runner on the
  host; `./gradlew nativeAndroidTest` builds the runner with the NDK as a static x86_64 executable
  (bionic, FORTIFY) and runs it on a Linux x86_64 host. Vectors: official Noise (conformance),
  `../test-vectors/ble_session_v1.txt`, `ble_handshake_v1.txt` (Dispenser framing, Rust `snow`),
  `qr_formats_v1.txt`. Instrumented (emulator or test phone): Keystore wrapping and the APK
  library on ART (`KeystoreKeyWrapperTest`, `NativeNoiseInstrumentedTest`).

## BLE transport (M1.10e.2a)

`cz.nowass.dispenser.device.transport` (GATT transport: `GattTransport` over the thin
`AndroidGattPort`, generations, MTU, timeouts, bounded notifications, permission and Bluetooth-off
handling) and `device.link.FrameLink` (fragmentation/reassembly). Tested on the JVM with the
deterministic `FakeGatt`; not wired into the app and no manifest permission is declared yet (the
app behaves exactly as before). Only `device/transport/AndroidGattPort.kt` uses `android.bluetooth`.

## Secure session (M1.10e.2b)

`cz.nowass.dispenser.device.session`: `SecureSession` (HELLO, IK SESSION, XXpsk3 CLAIM/ADD/RECOVERY,
staged-enrollment confirmation, encrypted requests and events) and the process-wide
`DeviceSessionManager`. Integration-tested against the real firmware application/security/transport
logic compiled for the host (`../firmware/test/fwsim`, built by `./gradlew buildFirmwareSim`, run by
`./gradlew test`). Not wired into the app. A request without a response is
`RequestResult.Uncertain` (it may have been executed; reconcile in a new session, never retry a dose
with a new `seq` or repeat REFILL automatically — `docs/BLE_PROTOCOL.md` 10.4a,
`docs/ANDROID_BLE_DESIGN.md` 14.3).

## Build, test, run

From this `android/` directory:

```sh
./gradlew assembleDebug            # build the debug APK
./gradlew test                     # JVM unit tests (domain, mock repository, ViewModels)
./gradlew lintDebug                # Android lint
./gradlew assembleDebugAndroidTest # compile instrumented UI tests
./gradlew connectedDebugAndroidTest   # run UI tests (emulator only, see warning below)
./gradlew installDebug             # install "Dispenser DEBUG" on a connected emulator/device
./gradlew assembleRelease          # release APK: unsigned unless local signing is configured
./gradlew checkThirdPartyLicenses  # bundled license data matches the dependencies
```

**Build variants (A1.5):** debug builds are a separate app, `cz.nowass.dispenser.debug`
("Dispenser DEBUG", versionName `…+debug`), next to the release app `cz.nowass.dispenser`
("Dispenser"). Release signing is optional and configured only outside the repository (see
`../docs/RELEASING.md`); without it `assembleRelease` produces `app-release-unsigned.apk`.

**Warning:** run instrumented tests only on an emulator or a test device. A
`connectedAndroidTest` run uninstalls the app under test afterwards, deleting its data.

CI: `../.github/workflows/android.yml` (JDK 25, no secrets) runs the commands above except the
connected tests and reports instrumented tests as compiled, not executed.

Emulator: open the project in Android Studio and run on any API 26+ AVD, or start one
headless, e.g. `emulator -avd <name> -no-window`, then `./gradlew installDebug`.
Running the emulator alongside Android Studio needs several GB of free RAM.

`local.properties` (SDK path) is machine-specific and not committed.

## Architecture

```
Compose screens -> ViewModels (StateFlow, immutable UI state)
  -> DispenserRepository (interface)
       -> LocalDispenserRepository (now: schedules via ScheduleStore, device values simulated)
            -> ScheduleStore -> RoomScheduleStore (app) | InMemoryScheduleStore (tests/previews)
       -> BleDispenserRepository  (future, NOT implemented; can reuse ScheduleStore as cache)
  -> pure domain: RecurrenceEngine, OccurrenceCalendar, NextDoseCalculator, ScheduleValidator, Volume
```

ViewModels and domain models have no database dependencies; Room types live only in
`data/local/`.

| Package | Contents |
|---|---|
| `domain/` | Platform-independent model and pure rules (no Android imports). |
| `data/` | `DispenserRepository` contract, `LocalDispenserRepository`, `ScheduleStore`, demo fixtures (`MockSeed`, `MockDispenserRepository(...)` for previews/tests). |
| `data/local/` | Room database, entity, DAO, validated entity mapper, `RoomScheduleStore`. |
| `ui/dashboard` | Next dose, reservoir estimate, simulated battery, 7-day strip. |
| `ui/calendar` | Month/week calendar, selected-day doses, schedule list with enable switches. |
| `ui/schedule` | Recurring schedule editor and Czech recurrence texts. |
| `ui/components` | Reusable design-system components (cards, DEMO badge, navigation dock, …). |
| `ui/theme` | Dark Material 3 theme, semantic color roles, shapes, spacing. |

### Mock vs. physical device

- The UI talks only to `DispenserRepository`; it never touches BLE APIs.
- Device values (battery, reservoir, history) are simulated and tagged `DataSource.DEMO`;
  the persistent **DEMO** badge is shown on every screen, including the editor.
- **Sync semantics:** every locally created/edited schedule is stored as `PENDING` and
  shown as **"Uloženo v telefonu"** (also as a confirmation after saving). It has NOT been
  transferred to or accepted by the dispenser. No device acknowledgement is simulated.
  `DEVICE_APPLIED` is reserved for a future BLE read-back.
- **Clock:** the running app uses the phone's real date and time (`java.time.Clock`,
  zone `Europe/Prague`), refreshed every minute. Tests and Compose previews inject a fixed
  `Clock`; the fixed demo date (14 Nov 2026 07:30) is used only together with fixture data.

### Simulation (A1.3)

- Pure domain in `domain/simulation/` (`SimulationEngine`, `ReservoirState`, `LowStockPolicy`,
  `ReservoirPredictor`, `HistoryEvent`); no Android dependencies, intended as a firmware
  reference. Rules: chronological, at most once per occurrence, time never moves backwards
  (reset instead), advancing = device running, outage = skipped without catch-up, no partial
  doses, never negative, insufficient stock blocks until refill, low-stock warning (PROPOSED
  20 %, configurable) once per episode, bounded commands (366 days / 10 000 occurrences).
- Persisted in a **separate `simulation.db`** (state, processed occurrences, immutable
  events). It never touches `dispenser.db`; "Resetovat simulaci" clears only simulation data.
- Events carry an origin (`SIMULATION` now, `DEVICE` reserved) and snapshot the schedule name
  and dose volume, so later schedule edits never change history.
- Service mode / tubing priming (FR-025) is documented and its event types and the
  `SERVICE_REQUIRED` readiness state are reserved, but nothing of it is implemented yet.

### Local persistence

- Room database `dispenser.db`, table `schedules` (schema exported to `app/schemas/`).
- A fresh installation starts **empty**; demo schedules are never inserted into it.
  Demo fixtures exist only for previews, unit tests and explicit simulation scenarios.
- Amounts are stored as **integer microlitres** (`amount_ul`, 1 µl precision); the domain
  and UI keep millilitres. Conversion (`domain/Volume.kt`) rejects values finer than 1 µl
  and non-positive / non-finite / out-of-technical-range values instead of rounding.
- Dates are ISO strings, time `HH:mm`, weekdays a bit mask (bit 0 = Monday).
- Writes use optimistic concurrency: the stored revision is checked and incremented
  atomically in a DB transaction; a stale write is a conflict.
- Rows that fail validation on read never crash the app: they are excluded from the list,
  left untouched (never deleted or repaired), reported to the user as a non-blocking
  warning in the Rozvrhy section ("… nelze načíst"), and logged with id and failing check
  only (no stored values).
- Schema changes must ship explicit migrations; destructive migration is never enabled.

## Implemented functionality (A1.1 – A1.2c)

- **Dashboard:** next dose, estimated reservoir (normal/low/blocked styling), simulated
  battery, 7-day preview; tapping a day opens it in the calendar.
- **Calendar:** month and week views, Monday first, today/selected highlighting,
  multiple-dose indicator, selected-day details with time, name, ml and daily total.
  Selection and mode survive rotation and process death.
- **Schedules:** create (prefilled with the selected day), edit the whole recurring
  definition by tapping an occurrence or a schedule, enable/disable, delete with
  confirmation. Disabled schedules are hidden from the calendar and listed (muted,
  "Vypnuto") in the Rozvrhy list.
- **Editor:** name (optional), amount (ml, accepts `12,5`), start date, 24-hour time,
  ONCE / DAILY every N days / WEEKLY every N weeks on selected weekdays, optional
  inclusive end date, enabled flag, live preview of the next five occurrences, and a
  floating Save button that stays disabled while the form is invalid (or unchanged).
  "Modified" is a semantic comparison with the original values (restoring them clears it).
  Unsaved changes are confirmed before leaving; while a save is running the editor cannot
  be left and the form is frozen. After saving, "Uloženo v telefonu" is shown.
- **Persistence (A1.2c):** schedules survive app restarts; calendar and dashboard update
  immediately after every change.
- **Dispensing simulation (A1.3):** started from Nastavení ("Simulace dávkování") with a
  start date/time and reservoir; +1 hour, +1 day (calendar day), advance to a date/time,
  explicit device outage, reset. While it runs, the whole app uses the visible simulation
  clock ("Simulovaný čas: …" in the header); otherwise the phone clock. Reservoir detail
  (tap the dashboard card) shows remaining/capacity, low/blocked state, prediction (covered
  doses, "refill by" date with day precision) and the refill action. Historie lists simulated
  events newest first by day, each marked SIMULACE.
- **Settings (A1.4a):** sections Aplikace (aquarium name shown in the dashboard header, demo
  mode, timezone, version, "O aplikaci"), Zásobník (read-only capacity and estimate + shortcut
  to reservoir management), Simulace (existing controls, "Aktivní" tag) and Zařízení ("Zařízení
  není připojeno"; no connection is ever simulated). The name is stored with DataStore
  (`app_settings.preferences_pb`), separate from both Room databases.
- **Branding:** adaptive launcher icon (navy background, turquoise droplet with a dosing-nozzle
  cut-out, monochrome themed variant; vector, no text) based on the approved reference image
  `docs/assets/app-icon-reference.png`; Android 12+
  splash in the same colors. Launcher name "Dispenser" (debug: "Dispenser DEBUG"); version
  0.5.0-beta.1 (5). Asset provenance: `../docs/assets/README.md`.
- **About (A1.4b):** Settings → O aplikaci opens a dedicated screen: icon, version and build
  from package metadata, update channel, "Co je nového" (bundled from the repository's
  `CHANGELOG.md`), "Licence a knihovny" (generated from the resolved runtime dependencies),
  user-initiated GitHub Releases update check (public metadata only; opens the release page in
  the browser, never downloads or installs), "Licence aplikace: Apache License 2.0" (opens the
  bundled license text; see `../LICENSE` and `../NOTICE`). The source-code
  link is hidden while the repository is private (`dispenser.github.sourceLinkEnabled`).
  The app's only permission is `INTERNET`, used solely for that check. See `docs/RELEASING.md`.
- **System backup (A1.5):** `allowBackup="false"`; `backup_rules.xml` and
  `data_extraction_rules.xml` exclude all app domains from cloud backup and device transfer.
  Data moves only via the manual JSON export (Nastavení → Záloha a obnovení,
  `docs/BACKUP_FORMAT.md`).
- **Navigation:** floating icon-only dock (Přehled, Kalendář, Historie, Nastavení) plus
  horizontal swipe between these four screens (one `HorizontalPager` in the `main`
  destination; the pager state is the single source of truth for dock, header and swipes).
  All four pages stay composed, so state and scroll positions survive tab switches. Back
  from another tab returns to Přehled. The schedule editor is a separate route (no swiping).
  Historie is a placeholder (milestone A1.3), and Nastavení is static.

### Scheduling rules (pure domain, unit-tested)

- Timezone `Europe/Prague`, shown explicitly in the editor.
- DST: for recurring schedules a local time that does not exist (spring forward) is
  **skipped** that day; a repeated local time (autumn) produces **one** occurrence at the
  earlier instant. A **one-off** schedule at a nonexistent local time cannot be saved
  ("Tento čas neexistuje kvůli přechodu na letní čas. Zvolte jiný čas.").
- Weekly "every N weeks" counts from the Monday of the start date's week; selected days
  before the start date in that first week are skipped. Daily "every N days" counts
  calendar days from the start date. Any interval length is supported.
- Missed doses are never caught up; only occurrences strictly after "now" are offered.
- Revision-checked writes: a stale edit is rejected as a conflict instead of overwriting.
- Occurrence ID = `scheduleId@localDate` (revision-independent), so at most one
  occurrence per schedule per day and an edit cannot duplicate a handled dose.
  *Proposed; pending product review.*
- Validation: amount finite and > 0 with at most 3 decimal places (1 µl), interval ≥ 1,
  at least one weekday for WEEKLY, end date ≥ start date. No physical pump limits are
  enforced (FR-016 is TBD); the only upper bound is the technical exact-storage range.
- MONTHLY recurrence is intentionally not offered until missing-date semantics are decided.

## Tests

- JVM unit tests: recurrence (intervals, weekdays, end date, ONCE, DST spring/autumn,
  ID stability), validation, ml/µl conversion, entity mapping and corrupted rows,
  a shared repository contract run against both the in-memory and the Room store,
  Room persistence across database reopen ("restart"), fresh-install emptiness,
  real vs. simulated clock, calendar/dashboard updates from Room, editor ViewModel.
  Room tests run on the JVM via Robolectric (no emulator needed).
  Robolectric runs **offline**: its Android framework jar
  (`org.robolectric:android-all-instrumented`, version `robolectricAndroidAll` in
  `gradle/libs.versions.toml`, the SDK 35 build Robolectric 4.17 expects) is resolved by Gradle
  and synced to `app/build/robolectric-deps` by `prepareRobolectricRuntime`. Tests never download
  at run time (a transient Maven Central failure in that download broke CI run 44070876851).
  When upgrading Robolectric or the tests' SDK level, update `robolectricAndroidAll` too; a
  mismatch fails with "Unable to locate dependency".
- Instrumented Compose tests (`src/androidTest`): navigation smoke test, editor
  screen states (Save visibility/enablement, discard dialog), create/edit/discard flows.
  These compile in CI-style runs but need an emulator to execute.

## Maintenance backlog

- **Low priority:** `checkThirdPartyLicenses` is declared incompatible with the configuration
  cache (it resolves POMs and parent POMs at execution time), so runs that include it discard
  the configuration-cache entry. This is a warning, not a failure. Proposed refactor (deferred,
  must not weaken the verification): `generateThirdPartyLicenses` also writes a non-shipped lock
  file (every resolved module, including BOMs, plus the SHA-256 of `third_party_licenses.json`),
  and the check compares the lazily resolved `releaseRuntimeClasspath` graph against it.

## Known limitations

- The Material 3 date picker grid uses the device language for month/day names
  (M3 1.4.0 cannot force Czech there). Titles and buttons are Czech.
- Battery is a fixed demo value (no voltage measurement). The simulation uses the current
  schedule configuration for each processed interval; service mode is not implemented.
- Schedules are never sent to a device (no BLE yet); all stay `PENDING`.
- One occurrence per schedule per day by design (use multiple schedules for multiple daily doses).
