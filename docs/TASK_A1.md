# Dispenser v2 — A1 implementation task

Status: ready for first Claude Code implementation pass. All UI and simulation work is **offline**; no ESP32 hardware needed.

## First action

Read root `CLAUDE.md`, `docs/REQUIREMENTS_DRAFT.md`, `docs/ARCHITECTURE.md` and `docs/ANDROID_APP.md`. Inspect the existing Android Studio project in `android/` and its Git status. Briefly report a staged plan and the Gradle/Compose dependencies you intend to use, then implement A1. If the referenced docs are missing, stop and request them.

## Outcome

A buildable Android app (`cz.nowass.dispenser`, minSdk 26), in Czech, launched on the emulator, demonstrating the dosing experience against a deterministic mock backend.

## Deliver in stages

**A1.1 — working shell**

- Preserve the existing Android Gradle project; do not create a nested replacement project.
- Material 3 navigation with screens: Dashboard, Calendar, History, Settings.
- Visible persistent `DEMO` badge and seeded example data; no Bluetooth/device permissions.
- `DispenserRepository` interface and `MockDispenserRepository`; use ViewModel + immutable UI state.
- Build/test and show how to launch on the Android emulator.

**A1.2 — calendar + schedule editor**

- Calendar-first UI with week view, selected-day timeline and agenda view.
- Create/edit/enable/disable/delete independent dose schedules. Request name, amount in ml, start date, time and recurrence.
- Include ONCE, DAILY every N days, WEEKLY every N weeks with weekdays, and preview of next five occurrences.
- MONTHLY recurrence may be added only with explicitly documented/tested short-month behavior (proposal: skip missing dates).
- Multiple schedules can occur on the same day. Missed occurrences are skipped, not made up later.

**A1.2c — local schedule persistence** (added 2026-11-15; numbering of later stages unchanged)

- Persist schedules on the phone with Room behind a `ScheduleStore` port; ViewModels and domain stay database-free.
- Fresh installation starts with an empty database; demo fixtures only for previews, tests and explicit simulations.
- Saved schedules are `PENDING` and shown as "Uloženo v telefonu"; no simulated device acknowledgement.
- Running app uses the phone's real clock; an injectable `Clock` keeps tests/previews deterministic.
- Quantities stored as integer microlitres with validated ml↔µl conversion; optimistic revisions; no destructive migrations.

**A1.3 — reservoir and simulation** (button-driven priming, FR-026, is documented and its event types reserved; no pump/button/BLE implementation in A1) (delivered in two parts: **A1.3a** pure simulation domain, reservoir state, prediction, safety rules and tests; **A1.3b** separate `simulation.db` persistence, History UI, reservoir detail, Settings simulation controls, dashboard integration)

- Dashboard shows next dose, upcoming total, estimated remaining fluid and mock battery percentage.
- Mock controls let user advance simulation time by hours/days; they affect only demo state.
- History records completed, skipped, blocked, refill, interrupted/uncertain events as applicable.
- Prevent simulated execution if estimated reservoir stock is insufficient for a dose. Remain blocked until simulated refill confirmation.
- Low-volume warning appears only once per low-stock episode. All stock values clearly marked estimates.

**A1.4 — quality and handoff**

- Unit tests for recurrence intervals, selected weekdays, multiple daily events, missed-dose skipping, DST Europe/Prague, stock blocking and refill reset.
- Compose UI smoke tests for core navigation and schedule editing where the local emulator/toolchain permits.
- Update `android/README.md` with build/run instructions and explain mock/BLE separation.
- Run `./gradlew test` and `./gradlew assembleDebug` from `android/` (and lint if feasible), reporting actual results and blockers.
- Summarize changed files, verification and unresolved product questions. **Do not commit or push without asking.**

## Architecture boundary

UI -> ViewModel -> `DispenserRepository` -> `MockDispenserRepository` -> pure recurrence/simulation domain.

The future `BleDispenserRepository` will use the same interface but is NOT part of this task. All UI writes must distinguish local pending changes from configuration confirmed by a physical device; DEMO may simulate confirmation.

## Explicit non-goals

- No ESP-IDF firmware or pump actuation.
- No Android BLE discovery, pairing or permissions.
- No notification/email backend or Wi-Fi configuration.
- No background Android scheduling or Google Calendar integration.
- No unapproved pump rate, dosage safety-limit or RTC assumptions.

## Acceptance criteria

1. Fresh project builds without connected hardware and opens on an emulator.
2. Demo data is clearly separated from physical-device state.
3. At least two independent recurrence rules produce correct occurrences and visible calendar entries.
4. User can create/edit/delete and inspect next occurrences.
5. Simulation never catches up missed doses and never dispenses an insufficient-stock dose.
6. Tests verify recurrence, time and reservoir business rules; results are reported accurately.

## Recommended first prompt

> Read CLAUDE.md and docs/TASK_A1.md, plus the three linked product/architecture docs. Inspect the existing android/ Gradle project and Git status. First present your short implementation plan and proposed dependencies; then implement only A1.1 (working shell, navigation, DEMO badge, repository interface, seeded mock dashboard) and run assembleDebug and relevant tests. Stop after A1.1 for my review; do not commit or push.
