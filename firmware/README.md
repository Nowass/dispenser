# Dispenser V2 — firmware

ESP-IDF firmware for the Dispenser V2 battery-powered aquarium liquid-carbon doser.
Product rules: [`../docs/REQUIREMENTS_DRAFT.md`](../docs/REQUIREMENTS_DRAFT.md),
architecture: [`../docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md).

**Status: milestone M1.1 (recurrence engine) approved and finalized, pending commit.** At run time
the firmware still only logs its identification: the recurrence engine is compiled into
the image's component library but not called by `app_main`, and nothing is dosed. It
has **not been run on physical hardware**: the board has not arrived yet. All
verification so far is Linux host tests and cross-compilation for `esp32c3`.

### Implemented

- ESP-IDF project skeleton (`CMakeLists.txt`, `main/`, `sdkconfig.defaults`).
- Cross-compilation for ESP32-C3 (`esp32c3`) with ESP-IDF v6.1.
- Platform-independent C11 domain component `dispenser_domain`: firmware identification
  formatting and (M1.1) the **recurrence engine** with explicit Europe/Prague DST handling
  (see [Recurrence engine](#recurrence-engine-m11)).
- Linux host-side test infrastructure (CMake + CTest, warnings as errors, ASan/UBSan).
- Firmware version identification: `version.txt` → ESP-IDF app descriptor → boot log.

### Not implemented

Dosing coordinator and durable occurrence deduplication (M1.2/M1.3), MONTHLY recurrence,
reservoir logic, event journal, NVS persistence, BLE and Wi-Fi, clock synchronization,
deep sleep and wake-up, GPIO and motor control, service mode, OTA. The recurrence engine
only calculates planned occurrences; nothing acts on them yet.

### Not verified

Physical ESP32-C3 execution (first boot log), GPIO boot/reset behavior, deep-sleep
current consumption, actual motor control. **Nothing in this firmware has been
hardware-validated.**

## Target hardware

| Item | Value |
|---|---|
| Board | Seeed Studio XIAO ESP32-C3 |
| SoC / IDF target | ESP32-C3 (RISC-V) / `esp32c3` |
| Flash size in build | 2 MB (ESP-IDF default, unchanged). The XIAO module is specified with 4 MB; switch only after checking a real board (`esptool flash-id`). |
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
├── sdkconfig.defaults          committed Kconfig defaults (target only); sdkconfig is generated
├── main/                       composition root: the only code calling ESP-IDF APIs
│   ├── CMakeLists.txt          REQUIRES dispenser_domain esp_app_format log
│   └── main.c                  app_main(): logs identification, nothing else
├── components/
│   └── dispenser_domain/       pure C11 business logic, no ESP-IDF dependency
│       ├── CMakeLists.txt      dual-mode: IDF component or plain host library
│       ├── include/dsp_identity.h    firmware identification
│       ├── include/dsp_time.h        dates, UTC instants, timezone rule (M1.1)
│       ├── include/dsp_recurrence.h  schedules and recurrence queries (M1.1)
│       └── src/                      matching .c files
├── test/host/                  host unit tests (CMake + CTest, no ESP-IDF)
│   ├── CMakeLists.txt
│   ├── support/dsp_test.h      minimal header-only test harness
│   ├── test_identity.c
│   ├── test_recurrence.c          engine unit tests (C only)
│   └── test_recurrence_vectors.c  runs ../../test-vectors/*.txt
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
| `test_recurrence` | day arithmetic vs a naive calendar walk 1999–2101, jump search vs brute force (600 random schedules), buffer paging, argument errors, extreme instants, identity across edits |

Planned host coverage (M1.2+): occurrence deduplication, dosing state transitions,
reservoir depletion/refill, service-mode transitions, interrupted operations, simulated
clock and storage failures. The Android domain
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
| Host test infrastructure (CMake/CTest, sanitizers) | working | n/a | n/a |
| Dosing coordinator, deduplication, reservoir, journal, service mode | not started | — | — |
| Motor/pump GPIO, button, indicator LED | not started (no pin map) | — | — |
| Deep sleep and wake-up | not started | — | — |
| NVS persistence, BLE, Wi-Fi/SNTP, OTA | not started | — | — |

## Hardware verification still pending

- First flash and boot log on a real XIAO ESP32-C3 (`idf.py flash monitor`).
- Actual flash size (expected 4 MB; the build assumes 2 MB).
- Reset-state level of every GPIO considered for the pump gate (strapping pins GPIO2,
  GPIO8, GPIO9 must be avoided or analysed), plus the external pull-down that keeps the
  pump OFF before firmware runs (FR-044).
- Button on a deep-sleep wake-capable GPIO (GPIO0–GPIO5) and indicator LED (the board has
  no user LED).
- Deep-sleep current and RTC drift; battery measurement hardware.

## Next milestone

M1.2/M1.3 — durable occurrence tracking and the dosing coordinator. Not started; scope to
be defined after the M1.1 review.
