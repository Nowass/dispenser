# Dispenser v2 — Android app

Native Android companion app for the Dispenser v2 aquarium liquid-carbon doser
(`cz.nowass.dispenser`). The current build is a fully offline **DEMO**: every value
comes from an in-memory mock backend and is clearly marked as simulated. There is
no Bluetooth, Wi-Fi, cloud or hardware access.

Product and architecture sources of truth live in [`../docs`](../docs).

## Toolchain

| Item | Version |
|---|---|
| Android Gradle Plugin | 9.5.1 (built-in Kotlin) |
| Kotlin / Compose compiler plugin | 2.2.10 |
| Gradle wrapper | 9.6.0 |
| Gradle daemon JVM | 25 (`gradle/gradle-daemon-jvm.properties`) |
| compileSdk / targetSdk / minSdk | 37 / 37 / 26 |
| Compose BOM | 2026.02.01 (Material 3 1.4.0) |
| navigation-compose | 2.9.8 |
| lifecycle-viewmodel/runtime-compose | 2.9.4 |
| Test: kotlinx-coroutines-test / espresso | 1.9.0 / 3.6.1 |

All versions are pinned in [`gradle/libs.versions.toml`](gradle/libs.versions.toml).

## Build, test, run

From this `android/` directory:

```sh
./gradlew assembleDebug            # build the debug APK
./gradlew test                     # JVM unit tests (domain, mock repository, ViewModels)
./gradlew lintDebug                # Android lint
./gradlew assembleDebugAndroidTest # compile instrumented UI tests
./gradlew connectedDebugAndroidTest   # run UI tests (needs a running emulator/device)
./gradlew installDebug             # install on a connected emulator/device
```

Emulator: open the project in Android Studio and run on any API 26+ AVD, or start one
headless, e.g. `emulator -avd <name> -no-window`, then `./gradlew installDebug`.
Running the emulator alongside Android Studio needs several GB of free RAM.

`local.properties` (SDK path) is machine-specific and not committed.

## Architecture

```
Compose screens -> ViewModels (StateFlow, immutable UI state)
  -> DispenserRepository (interface)
       -> MockDispenserRepository (DEMO, now)
       -> BleDispenserRepository  (future, NOT implemented)
  -> pure domain: RecurrenceEngine, OccurrenceCalendar, NextDoseCalculator, ScheduleValidator
```

| Package | Contents |
|---|---|
| `domain/` | Platform-independent model and pure rules (no Android imports). |
| `data/` | `DispenserRepository` contract and the seeded `MockDispenserRepository`. |
| `ui/dashboard` | Next dose, reservoir estimate, simulated battery, 7-day strip. |
| `ui/calendar` | Month/week calendar, selected-day doses, schedule list with enable switches. |
| `ui/schedule` | Recurring schedule editor and Czech recurrence texts. |
| `ui/components` | Reusable design-system components (cards, DEMO badge, navigation dock, …). |
| `ui/theme` | Dark Material 3 theme, semantic color roles, shapes, spacing. |

### Mock vs. physical device

- The UI talks only to `DispenserRepository`; it never touches BLE APIs.
- Every mock value is tagged `DataSource.DEMO`, and the persistent **DEMO** badge is shown
  on every screen, including the editor.
- Schedule writes go `PENDING` → `SIMULATED_ACK` after a short simulated delay.
  `DEVICE_APPLIED` is reserved for a future BLE read-back. DEMO never claims a device sync.
- "Today" is the mock's simulated clock (seeded at 14 Nov 2026 07:30 Europe/Prague),
  not the phone's clock.

## Implemented functionality (A1.1 – A1.2b)

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
  Unsaved changes are confirmed before leaving.
- **Navigation:** floating icon-only dock (Přehled, Kalendář, Historie, Nastavení).
  Historie is a placeholder (milestone A1.3), and Nastavení is static.

### Scheduling rules (pure domain, unit-tested)

- Timezone `Europe/Prague`, shown explicitly in the editor.
- DST: a local time that does not exist (spring forward) is **skipped** that day. A
  repeated local time (autumn) produces **one** occurrence at the earlier instant.
- Missed doses are never caught up; only occurrences strictly after "now" are offered.
- Revision-checked writes: a stale edit is rejected as a conflict instead of overwriting.
- Occurrence ID = `scheduleId@localDate` (revision-independent), so at most one
  occurrence per schedule per day and an edit cannot duplicate a handled dose.
  *Proposed; pending product review.*
- Validation: amount finite and > 0, interval ≥ 1, at least one weekday for WEEKLY,
  end date ≥ start date. No physical pump limits are enforced (FR-016 is TBD).
- MONTHLY recurrence is intentionally not offered until missing-date semantics are decided.

## Tests

- JVM unit tests: recurrence (intervals, weekdays, end date, ONCE, DST spring/autumn,
  ID stability), validation, mock CRUD (revisions, conflicts, delete, sync states),
  calendar/dashboard/editor ViewModels, Czech formatting.
- Instrumented Compose tests (`src/androidTest`): navigation smoke test, editor
  screen states (Save visibility/enablement, discard dialog), create/edit/discard flows.
  These compile in CI-style runs but need an emulator to execute.

## Known limitations

- The Material 3 date picker grid uses the device language for month/day names
  (M3 1.4.0 cannot force Czech there). Titles and buttons are Czech.
- No simulation of time/dispensing yet: history, refill and stock blocking are A1.3.
- In-memory mock: data resets when the app process is restarted.
- One occurrence per schedule per day by design (use multiple schedules for multiple daily doses).
