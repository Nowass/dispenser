# Dispenser v2 — project instructions for Claude Code

## Project

Dispenser v2 is a battery-powered automatic aquarium liquid-carbon dosing device, with a native Android companion application. Development takes place on Linux in a Git monorepo.

Repository layout:

- `android/`: existing Android Studio Kotlin + Jetpack Compose project (`cz.nowass.dispenser`), minSdk 26.
- `firmware/`: ESP-IDF v6.1 project for Seeed XIAO ESP32-C3 (target `esp32c3`). Milestone M0.2 skeleton with Linux host tests; never run on hardware. See `firmware/README.md`.
- `cloudflare/dispenser-notify/`: Cloudflare Worker + D1 notification backend (M1.6a ingestion, M1.6b Gmail delivery; TypeScript). Protocol in its `PROTOCOL.md`, Google setup in `GMAIL_SETUP.md`. Needs Node.js ≥ 22.12.
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
5. Run relevant Gradle unit tests, compilation and lint when available (firmware: host CTest tests and `idf.py build`, see below). Report exact commands/results; never claim verification that did not run.
6. Do not commit, push, install new system packages, change API credentials, or perform destructive Git operations without explicit user approval.
7. Flag unresolved requirements (monthly missing dates, numeric safety limits, BLE wire format) explicitly; do not silently turn proposals into product decisions.

## Firmware conventions

- ESP-IDF **v6.1** (6.1.0), target `esp32c3`, RISC-V GCC `esp-15.2.0_20251204`. Do not change the ESP-IDF version, reinstall or modify the local toolchain (`~/.espressif`) without approval. The optional devcontainer pins `espressif/idf:v6.1` by digest.
- C11 only. `firmware/components/dispenser_domain` is pure business logic: no ESP-IDF, FreeRTOS or hardware headers; platform services are injected as function-pointer interfaces. `firmware/main/` is the only composition root calling ESP-IDF APIs.
- Host tests: `cmake -S test/host -B build-host && cmake --build build-host && ctest --test-dir build-host --output-on-failure` (from `firmware/`; warnings are errors, ASan/UBSan on). Keep the small harness in `test/host/support/dsp_test.h`; do not add a test framework without a demonstrated need.
- Target build: `. ~/.espressif/tools/activate_idf_v6.1.sh && idf.py build`. `sdkconfig` is generated from `sdkconfig.defaults` and not committed.
- Versioning: Android app, firmware and (future) BLE protocol are versioned independently with SemVer. Firmware version source of truth is `firmware/version.txt` (currently `0.1.0-dev`); do not touch Android versioning for firmware changes.
- Never claim hardware validation. No GPIO, motor, radio, deep-sleep or NVS behavior without the corresponding milestone and hardware review.

## Current milestone

- **Android:** A1 offline demo application implemented through A1.5 (open items listed in `docs/ANDROID_APP.md`); release `0.5.0-beta.1` prepared (`docs/RELEASING.md`). Task briefs: `docs/TASK_A1.md`.
- **Firmware:** M0.2 (skeleton, `078b1b8`), M1.1 (recurrence engine, `f9292c2`) and M1.2 (execution history and at-most-once coordinator, `d498813`) approved and committed. M1.2 essentials: durable `STARTED` claim before the pump may be authorized, any record state blocks, no retry or catch-up, 10-minute due window, valid clock required, per-schedule watermark history (32 slots, 7-day retention; recycled-record dedup assumes no clock rollback over 7 days, to be enforced in M1.4), corrupted/duplicate history blocks dosing, manual doses independent.
- **M1.3 (persistent storage and configuration, `9cb4c10`)** approved and committed: key-value interface, byte-exact versioned records, explicit-presence layout (missing key = corruption, never erased automatically), schedule set as one atomic record (16 schedules, revision rules, bounded ID-reuse checks), configuration v1, NVS adapter in `components/dispenser_platform`.
- **M1.4 — time management, Wi-Fi and SNTP (`fbc64f8`)**, approved and committed:
  - **Clock trust:** evidence-based, no expiry (UNKNOWN / SYNCED / HOLDOVER). Failed Wi-Fi/NTP never stops dosing. After a power-on reset, time is unknown until a sync.
  - **Corrections:** applied as an immediate step, durable before effective (metadata confirmed in NVS first; a failed write applies nothing). A forward step over 10 min raises a persisted, monotonic dosing floor (`time.not_before_utc` → `dsp_clock_t.not_before`), so jumped-over occurrences never run, also after a restart or power loss. Every initial sync (previous clock untrusted) also raises the floor to max(floor, synchronized time + 1 s): nothing scheduled before time was re-established runs. A backward step over 7 days holds automatic dosing until high water − 7 days (persisted).
  - **Sync:** every 24–48 h (default 24). Bounded Wi-Fi session manager (no credentials until BLE provisioning; `WIFI_STORAGE_RAM`). Time-sync service, wake-up planner.
  - **Storage:** layout v2 with a `time` record and a tested v1 migration (v3 since M1.5).
  - **Platform:** ESP-IDF clock/Wi-Fi/SNTP adapters (compiled; SNTP never sets the clock itself). Optional diagnostic `CONFIG_DSP_TIME_SYNC_DIAGNOSTIC`, default off; it never starts Wi-Fi without credentials.
  - **Not verified:** anything on hardware or a real network (RTC continuity, drift, Wi-Fi, SNTP, energy).
  - **Flash:** with Wi-Fi linked the image uses about 80 % of the 1 MB app partition. The partition recommendation must be applied before BLE/TLS.
- **M1.5 — notification engine (`f02d64d`)**, approved and committed (domain only, no transport):
  - **Events:** reservoir LOW (warning) / BLOCKED, battery LOW (warning), dosing errors (one-shot), device errors (storage, sync overdue, clock unknown, queue overflow).
  - **Approved policy:** one event per episode plus a reminder every 7 days while unresolved; dosing errors one-shot. A 16-entry queue in one `notify` record (layout v3, v1/v2 migration); CRITICAL is never evicted; overflow is latched or counted, never silent.
  - **Delivery:** at-least-once, stable IDs `dsp-<device>-<epoch>-<seq>`, removed only on ACK, durable before sent. **No extra Wi-Fi wake-ups**: delivery waits for the regular session.
  - **Reservoir:** estimate from consumption (no sensor); every authorized dose (automatic or manual) subtracts the full requested volume before the pump. This is enforced by a single-use dose permit that the future actuator must require. Start/pump failures and interruptions set the persistent uncertainty flag in the same write; refill clears it.
  - **Review (2026-11-15):** a payload follows the latest value until the first transmission attempt, then is frozen; the freeze is persisted before the first send (survives power loss). A notify persistence failure returns `ERR_STORAGE` / `dsp_notify_storage_degraded()`, to be exposed via BLE diagnostics later. The device ID is the factory MAC, never a secret.
- **M1.6a — Cloudflare notification backend (`543af75`, `6d61393`, `2f7aa48`)**, approved and **deployed** (version `61c7dba5`; `DEVICE_API_TOKEN` rotated to a Cloudflare Secret; migration 0001 recorded; observability on, preview URLs off):
  - **Where:** `cloudflare/dispenser-notify` (Worker `dispenser-notify`, D1 `dispenser-notifications`, binding `DB`, secret `DEVICE_API_TOKEN`).
  - **Protocol v1:** `POST /api/v1/events`, Bearer token, one JSON event, strict validation that mirrors the firmware validator.
  - **Responses:** 201 accepted / 200 duplicate (identical canonical payload) / 409 conflict / 400, 401, 413, 415 / 503 retry.
  - **Idempotency:** `event_id` is the primary key; `INSERT … ON CONFLICT DO NOTHING`, then a payload-hash comparison. Existing rows are never modified.
  - **Delivery boundary:** an acknowledgement means durable acceptance (`PENDING`), not e-mail delivery; e-mail is a later milestone.
  - **Safety rules:** never deploy, run remote D1 commands that write, or touch production secrets without explicit approval.
  - **Approved for the firmware HTTPS milestone:** `DSP_TX_REJECTED` for 400/404/405/409/413/415. Record a persistent diagnostic (exposed via BLE), never block the queue, never discard silently. 401 and 5xx stay pending; timeouts stay UNCERTAIN with the same ID.
  - **Open:** per-device credentials, rate limiting.
- **M1.6b — Gmail notification delivery**, implemented and tested locally with a fake Gmail API, awaiting review; **not deployed**, no Google credentials yet:
  - **Mechanism:** a Cron Trigger in the same Worker; a dedicated sender account sends to a configurable recipient (both addresses are secrets). OAuth with the `gmail.send` scope only; refresh token in Cloudflare Secrets; one-time interactive helper `scripts/gmail-authorize.mjs`.
  - **Leases and retries:** atomic D1 leases (5 min). **Approved retry policy:** temporary errors (429, 5xx, timeouts, network) retry without an attempt limit, with backoff 1 min to 6 h. Only clearly non-recoverable payload rejections end in FAILED (413, a 400 whose message names the raw payload or its size, unreadable payload); configuration 400s never count, ambiguous 400s stay PENDING with a warning, and message-level outcomes never block other events. Credential/account errors never count; events wait, and Google is retried every 30 min. At-least-once with a stable Message-ID (duplicates possible).
  - **Diagnostics:** a `delivery_health` structured log line per run (`level: warn` when attention is needed, including any FAILED event and any unclear-400 PENDING event), plus `npm run diagnostics:remote` (read-only SQL). No public endpoint.
  - **Schema:** additive migration 0002 (not applied in production).
  - **Safety rules:** never run OAuth authorization, create Google resources, deploy or touch production D1 without explicit approval.
- **Next (after M1.6b approval):** scope to be agreed. Do not start it without approval; stop for review at each milestone boundary.
- **Shared test vectors:** when a scheduling rule changes, update `test-vectors/` first and make both the Kotlin and the C tests pass; never regenerate expected values from either implementation.
