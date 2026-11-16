# Dispenser V2 — firmware

ESP-IDF firmware for the Dispenser V2 battery-powered aquarium liquid-carbon doser.
Product rules: [`../docs/REQUIREMENTS_DRAFT.md`](../docs/REQUIREMENTS_DRAFT.md),
architecture: [`../docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md).

**Status: M1.1–M1.5 (recurrence, execution history, persistent storage, time management,
notification engine) committed; M1.7 (HTTPS notification transport) implemented, awaiting
review.**
At run time the firmware still only logs its identification. The domain and platform
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
- Linux host-side test infrastructure (CMake + CTest, warnings as errors, ASan/UBSan).
- Firmware version identification: `version.txt` → ESP-IDF app descriptor → boot log.

### Not implemented

Dosing service task (wiring of recurrence, coordinator, storage, time, stock, limits and
pump) and the application service that calls the M1.7 connectivity session, power manager
and deep-sleep lifecycle, production credential provisioning (authenticated BLE,
per-device credentials), battery measurement, MONTHLY recurrence, event journal, BLE, GPIO
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
| Flash size in build | **4 MB** (M1.7; the manufacturer's documentation confirms 4 MB). Check the actual board with `esptool flash-id` before the first flash. |
| Partition table | custom [`partitions.csv`](partitions.csv): two 1.75 MiB OTA slots, `nvs` 80 KiB, `nvs_sec` 24 KiB, reserved `nvs_keys`, `coredump`, 256 KiB reserve (see [Partition table](#partition-table-4-mb-m17)) |
| GPIO assignments | **None defined.** Pump, button and indicator pins come from the hardware review. |

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
├── partitions.csv              4 MB partition table (M1.7)
├── tools/provision_dev_secrets.py  developer-only nvs_sec image builder (+ its test, M1.7)
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
│   │   └── src/                      matching .c files
│   └── dispenser_platform/     ESP-IDF adapters (never in the host build)
│       ├── Kconfig                   DSP_SNTP_SERVER (M1.4), DSP_NOTIFY_URL, DSP_NOTIFY_HEALTH_URL (M1.7)
│       ├── include/dsp_nvs_kv.h      NVS implementation of dsp_kv_t (M1.3)
│       ├── include/dsp_esp_time.h    clock port, Wi-Fi driver, SNTP source (M1.4)
│       ├── include/dsp_esp_notify.h  HTTP port, device ID, monotonic clock, nvs_sec credentials (M1.7)
│       └── src/                      dsp_nvs_kv.c, dsp_esp_clock.c, dsp_esp_wifi.c, dsp_esp_sntp.c,
│                                     dsp_esp_http.c, dsp_esp_device.c
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
│   └── test_https.c               HTTPS transport and connectivity session (M1.7)
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

### Storage layout v3 (one NVS namespace, `dispenser`, partition `nvs`)

| Key | Content | Size |
|---|---|---|
| `layout` | layout marker (version 3, 32 history slots, 16 schedules, 16 tombstones); written **last** at provisioning | 7 B |
| `h00` … `h31` | execution history slot: M1.2 record or the explicit EMPTY record | 92 B each |
| `sched` | the whole schedule set (schedules + deleted-ID tombstones + set revision) | 8 … 2,504 B |
| `config` | device configuration | 18 B |
| `time` | time metadata (M1.4): last sync, high-water time, rollback hold, last correction, dosing floor | 42 B |
| `notify` | notification state (M1.5): ID epoch and sequence, condition latches, 16-entry queue; since M1.7 (format 2) also the transport diagnostics and the 4-entry rejection log | 758 B (format 1: 682 B) |

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
format version (1; `notify` is at format 2 since M1.7 and still reads format 1). Decoders require the exact length, a known type and version, valid
fields and zero padding. Encoders never produce an invalid record.

```text
history slot (92 B):  0 type | 1 ver | 2 state (0=EMPTY,1=STARTED,2=COMPLETED,3=FAILED,
                      4=INTERRUPTED) | 3 id_len | 4..67 id (zero padded) | 68..69 year |
                      70 month | 71 day | 72..75 revision | 76..83 scheduled_at (i64) |
                      84..91 claimed_at (i64). EMPTY: state 0 and bytes 3..91 zero.
schedule set:         0 type | 1 ver | 2..5 set revision | 6 count (<=16) |
                      7 tombstones (<=16) | count x entry | tombstones x id
  entry (91 B):       0 id_len | 1..64 id | 65..68 revision | 69 enabled (0/1) |
                      70..73 amount_ul | 74..77 start date | 78 hour | 79 minute | 80 kind |
                      81..84 interval | 85 weekday mask | 86 has_end (0/1) |
                      87..90 end date (zero when absent)
  tombstone (65 B):   0 id_len | 1..64 id
config (18 B):        0 type | 1 ver | 2..5 revision | 6..9 reservoir capacity µl |
                      10..13 estimated remaining µl | 14 low-stock % | 15..16 NTP interval h |
                      17 notification mask
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

The defaults mean "reservoir not configured", so a future stock check blocks every dose
until the user configures and refills. **Not in v1** (proposed for their milestones,
with a new schema version):
- pump calibration (µl/s, timestamp, FR-011/012);
- battery thresholds and measurement constants;
- device identity (e.g. a device UUID for the notification relay);
- Wi-Fi SSID, the notification endpoint and alert policy.

**No secrets** are stored here: since M1.7 the Wi-Fi credentials and the device API token
live in the separate `nvs_sec` partition (developer-provisioned, read-only for the
firmware, see M1.7), never in `config`, this namespace or logs.

### Capacity, flash layout and wear

NVS entries are 32 B and blobs cost 2 entries plus 1 per 32 data bytes. History
32 × 5 = 160 entries, schedule set ≤ 81, config 3, time 4 (M1.4, 42 B), notify 26 (M1.7,
758 B), marker 3, namespace 1: **≤ about 281 entries (9 KB)**, plus up to 81 free entries
while the largest record is rewritten.
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

**Layout v1/v2 → v3 (M1.4 and M1.5, implemented and tested):** a v1 marker (M1.3: no
`time`, no `notify`) or a v2 marker (M1.4: no `notify`) with valid data is migrated. The
newer keys may be absent, or all-zero as left by an interrupted migration:
- the missing keys are provisioned with zeros (`time`, then `notify`);
- the marker is rewritten as v3 **last**;
- a power cut before the marker is resumed at the next boot.

Without write permission the open reports `ERR_MIGRATION_REQUIRED` and writes nothing. An old
marker next to a newer key with real data is `CORRUPT`. Older firmware reading a newer layout
reports `UNSUPPORTED` (intended).

- Each record carries its own version, and the layout marker carries the layout version.
- A newer firmware keeps decoders for older versions and writes records back in its own
  format, one key at a time (each atomic). The marker is bumped **last**, so an interrupted
  migration is detectable and resumable.
- An older firmware that finds a newer version reports `ERR_UNSUPPORTED`, disables
  automatic dosing and leaves the data untouched. Downgrades are therefore not supported
  without maintenance.
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

## Build (ESP32-C3, native)

```sh
. ~/.espressif/tools/activate_idf_v6.1.sh     # once per shell
cd firmware
idf.py build                                  # sdkconfig is generated from sdkconfig.defaults
idf.py size
```

`sdkconfig` is not committed. It is generated from `sdkconfig.defaults` (which sets
`CONFIG_IDF_TARGET="esp32c3"`) plus ESP-IDF v6.1 defaults, so `idf.py set-target` is not
needed. A build from defaults alone produces an `sdkconfig` identical to the local one
(checked 2026-11-15). Add an option to `sdkconfig.defaults` only together with a
reviewed reason; a different ESP-IDF version may generate different defaults.

`idf.py flash monitor` exists but is **not part of any verification so far** (no board).

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
| `test_storage` | persistent storage (including the v1→v3 and v2→v3 migrations): first boot and provisioning (incl. power loss after every write), configuration save/reload and validation, schedule create/edit/delete with revision and ID rules, capacity and tombstone ring, power loss before/after commit (NVS-like and staged modes), marker loss, invalid/truncated/oversized/foreign records, unsupported versions, duplicate IDs, M1.2 coordinator on persistent history (missing slot = corruption, never empty), read/write/commit/no-space/read-back failures, golden bytes, maximum record sizes, randomized operation sequences with faults (always old or new set) |
| `test_time` | clock trust (first boot, deep sleep vs power loss, anomalies), sync interval 24/48 h, Wi-Fi and NTP failures with bounded backoff, implausible NTP, long outage (60 days of dosing in holdover), small/large forward/backward corrections, rollback hold (persisted, lifted), jumped-over protection, DST, restarts around a dose, late wake, storage and clock-step failures, Wi-Fi session lifecycle, wake planning; two randomized multi-year runs (realistic RTC; RTC glitches) with end-to-end dosing: never the same occurrence twice, never twice on the same true day under the trust assumption |

| `test_notify` | notification engine (M1.5, extended in M1.7): reservoir threshold, hysteresis, refill, battery, dosing and device errors, deduplication without writes, payload freshness (latest value until the first attempt, frozen afterwards), freeze surviving power loss after the receiver accepted (same ID, same payload, nothing lost) and zero transport calls when the freeze write fails, Wi-Fi failure, ACK, uncertain delivery and retry with the identical ID, durable before sent, restarts, power loss during queue updates and after an ACK, weekly reminders, backward/forward clock corrections, unknown clock, capacity exhaustion and overflow report, priority and no CRITICAL eviction, 120-day outage, manual dose consumption, single-use dose permits, uncertainty set by dosing errors and cleared by refill, golden bytes and strict decoding of `notify`, and a randomized 4000-step run with faults and restarts (one content per ID); M1.7: permanent rejection logged and removed in one write (also with the write failing outright, landing without confirmation, and a power cut at every write position: never lost, never both queued and logged), at most 2 rejections per session, log overflow with `lost_unread` and acknowledgement, diagnostic flags surviving restarts and their clearing conditions, a rejection without a 4xx treated as FAILED, the session budget leaving unstarted entries untouched, `notify` format-2 golden bytes and strict decoding, format-1 records read and upgraded |
| `test_proto` | protocol v1 (M1.7): golden request bodies (incl. the PROTOCOL.md example, every code name, extreme values, 48-bit device ID), refusal of invalid notes and too-small buffers, every response rule (strict 200/201 acknowledgement with the exact production bodies, R1 rules for 400/401/404/405/409/413/415/3xx/429/5xx, connect/TLS/send/no-response phases), and 20,000 fuzzed bodies (no crash, no ACK/REJECTED without the confirming body) |
| `test_https` | HTTPS transport and connectivity session (M1.7) over fakes: exact body and token handed to the port, token wiped after every request, token validation (length, spaces, control characters); **systemic 401/404/405/413/415, ambiguous 400, mismatched 409, redirects, 429 and 5xx never drain the queue** (20 sessions each, then recovery); event-specific 409/400 removed and logged; mismatched acknowledgement UNCERTAIN, then duplicate ACK with an identical payload; timeout after the backend accepted plus power loss; connect/TLS failures; the 60 s budget with a monotonic clock; session gating: dosing/permit/BLE guard, next-dose distance, regular sync vs CRITICAL on an existing wake, the 1-hour limit, the Wi-Fi backoff, untrusted time (nothing sent) vs trusted HOLDOVER, missing token, Wi-Fi always shut down |

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
| Battery monitor | not started | — | — |
| Dosing service, journal, service mode | not started | — | — |
| Motor/pump GPIO, button, indicator LED | not started (no pin map) | — | — |
| Deep sleep and wake-up | not started | — | — |
| BLE, production credential provisioning, OTA client, deep sleep | not started | — | — |

## Hardware verification still pending

- First flash and boot log on a real XIAO ESP32-C3 (`idf.py flash monitor`).
- Actual flash size (4 MB per the manufacturer; the build assumes 4 MB): `esptool flash-id`
  before the first flash, which must write the complete image set.
- Reset-state level of every GPIO considered for the pump gate (strapping pins GPIO2,
  GPIO8, GPIO9 must be avoided or analysed), plus the external pull-down that keeps the
  pump OFF before firmware runs (FR-044).
- Button on a deep-sleep wake-capable GPIO (GPIO0–GPIO5) and indicator LED (the board has
  no user LED).
- Deep-sleep current and RTC drift; battery measurement hardware.
- NVS adapter on real flash: provisioning, power loss during writes, partition sizing and
  the final partition layout (see "Capacity, flash layout and wear").
- Time: RTC continuity per reset type, deep-sleep drift, Wi-Fi/SNTP on a real network,
  energy per sync (see "Hardware validation still required" in the M1.4 section).
- M1.7 (pending, not estimates): TLS heap peak, TLS handshake time (ECDSA P-256 in
  software on the C3), energy per notification session, Wi-Fi/BLE runtime memory, the
  `esp_http_client` keep-alive behavior, the HTTPS diagnostic against a staging backend.

## Next milestone

After the M1.7 review: M1.8 (integration and robustness: shared protocol vectors, local
end-to-end harness against a local Worker, fault matrix) is designed and approved in
principle; its implementation is not authorized yet.
