# Dispenser v2 — Functional requirements

**Draft 0.3, 14 Nov 2026.** Scheduling decisions confirmed by Petr on 14 Nov 2026. `PROPOSED` values elsewhere remain defaults to approve. Scope: aquarium liquid-carbon dosing via ESP32-C3 with Android BLE configuration.

## Confirmed decisions

| ID | Requirement |
| --- | --- |
| FR-001 | Autonomous scheduled dosing of roughly 10–20 ml daily; exact volumetric accuracy is not critical. |
| FR-002 | ESP32-C3 normally in deep sleep, waking for planned dosing and physical button press. |
| FR-003 | Button on suitable deep-sleep wake GPIO activates BLE maintenance session; default 3 min advertising timeout, maintain session while connected. |
| FR-004 | Android app provides schedule/configuration, calibration, manual dose, event logs, battery and estimated reservoir level. |
| FR-005 | Wi-Fi used only on important events/alerts or infrequent time sync. No routine telemetry or daily emails. |
| FR-006 | Stock estimated in firmware; user confirms/refills volume from the Android app. No level sensor in prototype. |
| FR-007 | One low-stock alert per low-stock episode. When estimated remaining stock is insufficient for the next dose, stop automatic dosing until refill is confirmed. |
| FR-008 | Configuration persists through reboot. Device remains safe and pump off by default on startup, crashes and deep sleep. |
| FR-009 | Missed scheduled doses are **skipped, never caught up**. Multiple independent recurring schedules with different periods are supported. Before a dose, if estimated stock is insufficient for that dose, **block it** until refill confirmation. |

## Proposed functional behavior

### Schedules and doses

| ID | Requirement / default |
| --- | --- |
| FR-010 | **CONFIRMED:** independent recurring dosing events, each with its own volume, local wall-clock time, recurrence rule, start date, enable flag; support 1× weekly, multiple events daily and user-defined intervals. Maximum number of events is `PROPOSED` 8. |
| FR-011 | `PROPOSED`: calibration by running pump for selected duration into a measuring vessel, then entering measured volume. Calculate ml/s, record calibration timestamp. |
| FR-012 | `PROPOSED`: use runtime-based dosing at nominal direct battery voltage. Detect/compensate voltage-dependent flow later only after physical measurements. |
| FR-013 | **CONFIRMED:** skip missed doses (do not catch up after restart or loss of time); record SKIPPED reason. |
| FR-013a | **CONFIRMED:** multiple independent schedules; frequency can differ between schedules. `PROPOSED` v1 rule subset: daily every N days, weekly every N weeks with selected weekdays, monthly on day-of-month, and one-off. Optional end date. Full arbitrary iCalendar RRULE syntax is out of scope unless explicitly approved. |
| FR-013b | **CONFIRMED:** editing recurrence must not re-run old occurrences or catch up missed ones. Schedule identity and revision must be reflected in occurrence deduplication and history. |
| FR-014 | **CONFIRMED (occurrence identity, approved 2026-11-15):** each schedule occurrence has the stable ID `<scheduleId>@<localDate>` (e.g. `schedule-001@2026-11-16`), independent of the schedule revision; changing a schedule's time or volume never creates a second identity for the same local date. Schedule IDs are unique within the device and a deleted schedule's ID is never reused (mandatory invariant for the schedule-management/storage layer). Multiple doses per day require multiple schedules. `PROPOSED` (unchanged): mark the attempt durably before motor start, and never retry an uncertain/incomplete occurrence automatically after reset. |
| FR-015 | **CONFIRMED (A1.2b brief, 2026-11-15):** a scheduled local time that does not exist because of the spring-forward transition is skipped that day (not shifted). A local time repeated by the autumn fall-back produces exactly one occurrence, at the earlier instant, and is never administered twice. A one-off (ONCE) schedule at a nonexistent local time is rejected at entry (approved 2026-11-15). Implemented and tested in the Android recurrence engine and validator; firmware must follow the same rule. |
| FR-016 | `PROPOSED`: limits: maximum amount per individual action, maximum sum per day, maximum continuous pump runtime, cooldown between starts. Exact values TBD after pump tests. |

### Stock and notifications

| ID | Requirement / default |
| --- | --- |
| FR-020 | Track configured reservoir volume and estimated remaining volume based on commanded completed doses; do not claim liquid flow was physically sensed. |
| FR-021 | `PROPOSED`: low-stock at 20% remaining. Send event-triggered notification at most once per threshold episode; no automatic repeated reminder. |
| FR-022 | **CONFIRMED:** block the next automatic dose if estimated remaining stock is less than its requested volume. Send `EMPTY/BLOCKED` once, then sleep; do not keep retrying Wi-Fi while blocked. |
| FR-023 | BLE refill operation sets user-supplied new remaining amount and clears blocked state and per-episode notification latches. Validate volume and user authorization. |
| FR-024 | `PROPOSED`: unexpected motor fault and critically low battery may cause alerts, with bounded retry policy; repeated events deduplicated. |
| FR-025 | **CONFIRMED requirement, future implementation (added 2026-11-15): container replacement / tubing priming service mode.** After a container is replaced or refilled, the tubing may contain air. Workflow: (1) user starts container replacement/service; (2) normal dispensing is suspended (device state `SERVICE_REQUIRED`); (3) user confirms the tube outlet is directed into a waste container; (4) user manually controls the pump to prime the tubing; (5) pump operation has strict duration limits, an emergency stop, and fail-safe pump-OFF when communication is lost; (6) user visually confirms the tubing is filled; (7) device returns to `READY`; (8) the service operation is recorded in history. Priming volume is deducted from the reservoir estimate. History distinguishes *reservoir refilled*, *tubing primed* and *device ready for normal operation*. Not implemented in A1: no pump control, no BLE priming, no UI controls. Primary interaction: physical button, see FR-026. |
| FR-026 | **CONFIRMED requirement, future firmware (added 2026-11-15): button-driven tubing priming.** The multifunction button (FR-003) drives the service workflow of FR-025 as an explicit firmware state machine, separate from scheduled dispensing (see ARCHITECTURE "Service mode state machine"). **Gestures:** 4 short presses in READY → `SERVICE_REQUIRED` (automatic dispensing disabled). In `SERVICE_REQUIRED`, holding the button runs the pump; releasing stops it immediately. 4 short presses in `SERVICE_REQUIRED` confirm the tubing is primed → `READY`. Readiness may alternatively be confirmed via BLE. A single short press keeps its FR-003 meaning (BLE maintenance session). **Safety:** pump output fail-safe OFF (FR-044); hard maximum continuous priming runtime (pump stops even while the button is held, and restarts only after a release); a reboot never leaves `SERVICE_REQUIRED` automatically (state persisted); a button held at boot never starts the pump (the button is armed only after a release is observed); priming consumption deducted from the reservoir estimate (runtime × calibrated flow, FR-011; marked uncertain when uncalibrated); every service action recorded in history; visible feedback for each recognized gesture. **Timing:** debounce and multi-press windows defined in ARCHITECTURE; all values `PROPOSED` until hardware tests. Not implemented in A1. |

### Sleep, time, communication

| ID | Requirement / default |
| --- | --- |
| FR-030 | Timer wake for scheduled doses. GPIO wake for manual BLE maintenance. Radio remains off in deep sleep. |
| FR-031 | Maintain UTC internal time with Europe/Prague timezone conversion for schedules/UI. Record clock-validity and last sync source/time. |
| FR-032 | SNTP over Wi-Fi at configurable infrequent intervals; synchronize from Android time over BLE as a fallback. |
| FR-033 | After total power loss, do not assume clock is valid. `PROPOSED`: suspend time-dependent dosing until time can be recovered from SNTP or Android. |
| FR-034 | BLE communication with authenticated access to all mutation commands. UUIDs, versioned schema, log paging and timeout defined in separate protocol document. |
| FR-035 | Event notification: ESP sends signed/authenticated HTTPS event to a minimal endpoint that sends an email. Event IDs must be deduplicated at source and receiver. Credentials never logged. |
| FR-036 | Failed Wi-Fi connection never blocks core dosing; short connection timeout and bounded retries. No infinite retry loops. |

### Logging and safety

| ID | Requirement / default |
| --- | --- |
| FR-040 | Bounded durable event log: scheduled, attempted, completed, skipped, manually requested, refilled, error, notification status. |
| FR-041 | Each record carries monotonically increasing sequence, action ID, timestamp (with validity flag), requested amount, runtime, outcome and reason. |
| FR-042 | Flash-write strategy must avoid excessive writes; no high-frequency pump-loop logging to NVS. |
| FR-043 | Manual dose obeys maximum run and safety limits, requires explicit action/confirmation in app, and is logged. |
| FR-044 | The pump is forced OFF on all resets/failures/boot/deep sleep transitions; GPIO cannot accidentally drive it during boot. |
| FR-045 | Test device using water only until chemical compatibility and safety are confirmed. |

## Acceptance test examples

1. Given a valid clock and an enabled schedule due now, pump starts once, runs within the configured maximum, then stops and records the event.
2. Given a reboot during dispensing, pump stays off after reboot and the same planned occurrence is not repeated automatically; event is marked interrupted/uncertain.
3. Given a due dose while stock is insufficient, pump never starts, blocking and one alert event are recorded, system sleeps.
4. Given a refill via authenticated BLE, stock updates and future scheduled doses are enabled; prior episodes remain in history.
5. Given an unavailable Wi-Fi network, alert delivery attempts are bounded and battery is not drained by retries; dosing subsystem is unaffected.
6. Given an invalid clock after power loss, scheduled pump operation remains inhibited until time is restored.
7. Given a button wake, BLE becomes available, then returns to sleep after timeout/disconnect without affecting upcoming schedules.
8. Given Android disconnect during configuration write, configuration is either fully valid/committed or remains the previous valid configuration.

## Confirmed product decisions

1. **Missed dose**: skip (no catch-up).
2. **Scheduling**: multiple independent recurring calendar-style dose events, including weekly or other custom recurrence, and multiple daily events.
3. **Empty-reservoir semantics**: block before dose if estimated remaining volume cannot cover its requested amount; user must explicitly confirm refill to resume.

## Other open items before coding

- Battery and pump electrical characterization, valid calibration under battery voltage changes.
- Confirm recurrence subset: daily interval, weekly selected weekdays, monthly, one-off, optional expiration; clarify whether arbitrary hourly/minute intervals are required and how monthly 29–31st behaves.
- Exact alert transport/service and retry policy; drift measurements and Wi-Fi resync interval.
- BLE authorization details; Android UX and notification delivery expectations.
- Battery measurement hardware; pin mapping and protections; firmware/Android version strategy.

## Implementation note for coding agent

The three primary product decisions above are accepted. Build pure deterministic host-side tests for schedule and dose state machines first. Keep the motor control behind a fakeable driver interface and preserve an audit trail of interrupted events. Review remaining open decisions before hardware actuation or cloud notifications.
