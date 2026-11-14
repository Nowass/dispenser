# Dispenser v2 — project instructions for Claude Code

## Project

Dispenser v2 is a battery-powered automatic aquarium liquid-carbon dosing device, with a native Android companion application. Development takes place on Linux in a Git monorepo.

Repository layout:

- `android/`: existing Android Studio Kotlin + Jetpack Compose project (`cz.nowass.dispenser`), minSdk 26.
- `firmware/`: future ESP-IDF project for Seeed XIAO ESP32-C3.
- `docs/`: source-of-truth product and architecture documents.

Read these documents before making product decisions:

1. `docs/REQUIREMENTS_DRAFT.md`
2. `docs/ARCHITECTURE.md`
3. `docs/ANDROID_APP.md`

If any document is missing, **stop and ask** for it; do not guess missing business requirements.

## Confirmed behavior

- User configures multiple independent calendar-like dosing schedules: single, daily/every N days, weekly/every N weeks on selected weekdays, and eventually monthly.
- A schedule has a local date/time, timezone (`Europe/Prague` by default), amount in millilitres, enabled state, and stable identity/revision.
- Missed doses are **skipped**. There is never catch-up dosing.
- Reservoir volume is **estimated**, not measured. If estimated remaining volume is below the requested next dose, block dosing until user confirms refill.
- The physical ESP32 device normally sleeps. Button-triggered BLE maintenance, occasional Wi-Fi clock synchronization and event-driven notifications belong to later milestones.
- Firmware eventually performs dosing autonomously; Android must never be required to be connected for scheduled dispensing.
- Safety: no accidental duplicate dose on restart, uncertain prior action or wall-clock changes.

## Android development conventions

- Prefer Kotlin, Jetpack Compose/Material 3, ViewModel, `StateFlow`, unidirectional data flow, and small testable domain classes.
- Separate UI/domain from data access via `DispenserRepository` and a mock implementation now, later BLE implementation.
- Mock mode must be conspicuous (`DEMO` badge); simulated data must not be presented as readings from real hardware.
- UI strings in Czech, placed in Android string resources where practical.
- Use stable dependency versions compatible with the already-generated Gradle project. Avoid gratuitous toolchain upgrades.
- Do not implement Bluetooth permissions, production BLE transport, Wi-Fi provisioning, cloud services, billing or real actuator control during A1.
- Avoid secrets, credentials or private keys in source code or Git.

## How to work

1. Inspect Git status and existing source/project configuration before editing. Preserve user changes.
2. For nontrivial tasks, give a short plan and intended file/module changes first.
3. Make small, reviewable changes. Avoid unrelated refactors and generated file churn.
4. Prefer robust deterministic domain tests for recurrence, timezone/DST and reservoir rules.
5. Run relevant Gradle unit tests, compilation and lint when available. Report exact commands/results; never claim verification that did not run.
6. Do not commit, push, install new system packages, change API credentials, or perform destructive Git operations without explicit user approval.
7. Flag unresolved requirements (monthly missing dates, numeric safety limits, BLE wire format) explicitly; do not silently turn proposals into product decisions.

## Current milestone

Android **A1 — offline, calendar-first mock application**. Follow `docs/TASK_A1.md` and stop for review at the A1 boundary.
