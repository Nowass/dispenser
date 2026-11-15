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
| Gradle wrapper | 9.6.0 |
| Gradle daemon JVM | 25 (`gradle/gradle-daemon-jvm.properties`) |
| compileSdk / targetSdk / minSdk | 37 / 37 / 26 |
| Compose BOM | 2026.02.01 (Material 3 1.4.0) |
| navigation-compose | 2.9.8 |
| lifecycle-viewmodel/runtime-compose | 2.9.4 |
| Room (runtime, compiler, Gradle plugin) | 2.8.5 |
| KSP | 2.3.12 |
| Test: kotlinx-coroutines-test / espresso | 1.9.0 / 3.6.1 |
| Test: Robolectric / androidx.test core | 4.17 / 1.6.1 (Robolectric runs on SDK 35) |

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
- Instrumented Compose tests (`src/androidTest`): navigation smoke test, editor
  screen states (Save visibility/enablement, discard dialog), create/edit/discard flows.
  These compile in CI-style runs but need an emulator to execute.

## Known limitations

- The Material 3 date picker grid uses the device language for month/day names
  (M3 1.4.0 cannot force Czech there). Titles and buttons are Czech.
- Battery is a fixed demo value (no voltage measurement). The simulation uses the current
  schedule configuration for each processed interval; service mode is not implemented.
- Schedules are never sent to a device (no BLE yet); all stay `PENDING`.
- One occurrence per schedule per day by design (use multiple schedules for multiple daily doses).
