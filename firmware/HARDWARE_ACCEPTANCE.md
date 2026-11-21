# Dispenser V2 — on-device acceptance checklist (XIAO ESP32-C3)

What M1.7/M1.8 could **not** verify without the board. Run it once a Seeed Studio XIAO
ESP32-C3 is available, record the results (date, firmware commit, board, measured values) in
the "Result" column, and keep the safety rules below. Run so far (2026-11-19): checks 1.1–1.3 and
the RNG-gate items R1, R2, R3, R5, R6 of section 8a (R5 statistically NOT ACCEPTED; bench-only
deviation, see 8a.2) and a first bench BLE advertising/GATT bring-up (parts of 8b H1 and H2, see
8b.1); 2026-11-20: GPIO-only pump control signal G3 (owner-reported) and part of G7, see 8c.1.
Everything else is still OPEN.

## Verification levels

| Level | Meaning | Where |
|---|---|---|
| Host-verified | domain logic with fakes (ASan/UBSan) | `firmware/test/host` (CTest) |
| Locally integrated | real domain code + real TLS (OpenSSL) + the committed Worker under `wrangler dev` + local D1 + fake Gmail | `firmware/test/integration` |
| Hardware | ESP-IDF adapters, mbedTLS, `esp_http_client`, NVS on flash, Wi-Fi, energy | **this checklist — still unverified** |

## Safety rules

- Use a **staging** backend (separate Worker and D1, no Gmail secrets: approved in principle
  in M1.8, to be created only after explicit approval). Never point a test device at the
  production Worker for fault tests; production is for one final, approved end-to-end check.
- Synthetic test events only; credentials only via `tools/provision_dev_secrets.py` from an
  owner-only file outside the repository. Delete the generated `nvs_sec` image afterwards.
- Never enable flash encryption, secure boot or any other irreversible eFuse setting.
- The pump/motor stays disconnected for all notification tests.

## 1. Board and flash

| # | Check | Pass criterion | Result |
|---|---|---|---|
| 1.1 | `python -m esptool --chip esp32c3 flash-id` | 4 MB (0x400000) detected | **PASS** 2026-11-19: ESP32-C3 (QFN32) rev v0.4, embedded 4 MB flash (XMC), USB-Serial/JTAG, esptool v5.6.0 |
| 1.2 | First full flash (`idf.py flash`: bootloader, partition table at 0x10000, `ota_data_initial`, app) | boots; partition table printed matches `partitions.csv` | **PASS** 2026-11-19: default image of `af9677a`, ESP-IDF v6.1; all four regions written with verified hashes; the printed table matches all 9 entries of `partitions.csv` |
| 1.3 | Boot log | identification line, no reset loop | **PASS** 2026-11-19: `Dispenser V2 firmware 0.1.0-dev (esp32c3, v6.1)` and the M0.2 skeleton line; one boot per reset, no panic, watchdog, error or warning line |
| 1.4 | `idf.py flash` again | `nvs_sec` content unchanged (only `erase-flash` wipes it) | OPEN |

**Record of the first flash (2026-11-19).** Board: Seeed Studio XIAO ESP32-C3, chip revision
v0.4, 4 MB flash, connected by USB only (no pump, MOSFET or battery). Firmware: commit `af9677a`,
default configuration (`sdkconfig.defaults` only, no diagnostic or runtime option), app version
`0.1.0-dev`, ESP-IDF v6.1. Built in a fresh build directory with its own `sdkconfig`
(`idf.py -B <dir> -D SDKCONFIG=<dir>/sdkconfig build`), never from a pre-existing local
`sdkconfig`. Written: bootloader 0x0 (21,152 B), partition table 0x10000, `ota_data_initial`
0x25000, app 0x30000 (148,480 B); `nvs`, `nvs_sec` and the rest of the flash untouched; no
erase-flash, no eFuse change, no credentials. Evidence, kept outside the repository by the owner
(read-only):

- Full flash backup taken before the first flash (factory content, 4,194,304 bytes, verified by
  a second identical read): `xiao-c3-factory-backup-20261119-202840.bin`, SHA-256
  `f61f361dbb211ebf121d2ef8d3621760ff638bf7c2cf9ee28b581be4cd30449b`.
- Boot log captured over `/dev/ttyACM0` after a reset: `first-boot-20261119.log` (3,632 bytes).

**Incident record — second flash with a stale build directory (2026-11-19).** After the first
flash above, the VS Code ESP-IDF "Flash Device" command (JTAG) was used. It does not build: it
wrote the existing `firmware/build/` from 2026-11-15, generated before the M1.7 layout. Its
`flasher_args.json` was `--flash-size 2MB`: 0x0 bootloader (expects its table at 0x8000),
0x8000 single-app partition table (`nvs` 0x9000, `phy_init` 0xF000, `factory` app 0x10000),
0x10000 app (148,480 B, an M0.2 build of 2026-11-15). The owner's boot log showed the app
loaded from 0x10000 and a 2048 KB image header on the 4096 KB flash. The 0x10000 write
(sectors 0x10000–0x34FFF) covered the approved partition table and the approved `nvs`,
`otadata`, `phy_init`, `nvs_sec`, `nvs_keys` areas and the first 20 KiB of `ota_0`; `nvs_sec`
held no credentials (never provisioned). The results of 1.1–1.3 above are unaffected: they
were recorded from the approved image before the incident. Confirmed by a read-only full
readback (`post-vscode-flash-readback-20261119-211000.bin`, SHA-256
`030c521bfd919b03f5ce1e3363fcfadbcd0361a15ef1886da9f4c27a1e360738`): it equals, byte for
byte, the factory content + the approved first flash + the three stale files written into
erased sectors (0x0–0x5FFF, 0x8000–0x8FFF, 0x10000–0x34FFF); nothing else was written. Not
caused by the configuration itself: the stale `sdkconfig` values were all marked
`# default:`, and any build would have re-derived the approved layout; the stale artifacts had
simply never been rebuilt. Fixes: the stale `sdkconfig` and `build/` were moved unchanged out
of the repository (owner's evidence folder `stale-config-20261115/`), a default build now
produces the approved layout (4 MB, `partitions.csv` at 0x10000, app at 0x30000), a
configure-time flash layout guard rejects explicitly set wrong layout values, and README
"Build" documents that a flash command writes the existing build directory.

**Recovery record (2026-11-19, approved by the owner).** A fresh build of `af9677a` (with the
uncommitted flash layout guard, which does not change the image) in a separate build directory,
checked before writing (target `esp32c3`, 4 MB, `partitions.csv` at 0x10000, app at 0x30000,
runtime/BLE/diagnostics off), was written with
`idf.py -B <dir> -D SDKCONFIG=<dir>/sdkconfig -p /dev/ttyACM0 flash`: bootloader 0x0, partition
table 0x10000, `ota_data_initial` 0x25000, app 0x30000, every hash verified. Then exactly two
approved region erases: `esptool erase-region 0x11000 0x14000` (`nvs`) and
`esptool erase-region 0x28000 0x6000` (`nvs_sec`); no `erase-flash`, no eFuse or security
change. Read-only full readback afterwards: `post-restore-readback-20261119-211545.bin`,
SHA-256 `954fe7cfc8adb63ffe05d597738a12a120eb607ef334e28dd07e8adafdc86266`. The bootloader,
partition table (all 9 entries of `partitions.csv`) and app are present byte for byte; `nvs`
and `nvs_sec` are all 0xFF. Compared with the incident readback, 38 sectors have different
content, all inside the 72 written or erased sectors; the other 34 (3 bootloader sectors and
the app's 0x35000–0x53FFF, already the approved app) were rewritten with identical bytes, and
nothing outside the approved regions changed. `otadata[0]` differs from `ota_data_initial`
(all 0xFF) by its 32-byte entry `ota_seq=1`, state VALID, CRC 0x4743989A: written by the
ESP-IDF bootloader itself at the first boot (no factory partition, initial otadata), selecting
`ota_0`; expected. Left untouched (unused by the approved bootloader and partitions): the old
single-app table at 0x8000, stale bytes in `phy_init`, `nvs_keys` and the gap at 0x2F000,
factory leftovers at 0x9000–0xFFFF and in `ota_0` after the app. Boot log
(`post-restore-boot-20261119.log`): chip v0.4, flash 4 MB, the approved 9-entry table, app
loaded from 0x30000, `Dispenser V2 firmware 0.1.0-dev (esp32c3, v6.1)` and the M0.2 skeleton
line; one boot per reset, no panic, watchdog, error, warning or 2 MB line. The board is back
in the approved layout; check 1.4 remains OPEN.

## 2. Credentials

| # | Check | Pass criterion | Result |
|---|---|---|---|
| 2.1 | Provision `nvs_sec` (dummy Wi-Fi of a test AP, staging token), flash at 0x28000 | `DSP_NOTIFY_HTTPS_DIAGNOSTIC` logs `nvs_sec=1 token_provisioned=1` | |
| 2.2 | Serial log of a full diagnostic run | no SSID, passphrase or token appears anywhere | |
| 2.3 | Erase `nvs_sec` | diagnostic reports "no credentials", radio never started | |

## 3. Wi-Fi, time and TLS

| # | Check | Pass criterion | Result |
|---|---|---|---|
| 3.1 | Diagnostic with a reachable AP | Wi-Fi connects, SNTP sync accepted, trust SYNCED | |
| 3.2 | TLS probe of the staging `/health` | phase `response`, status 200 (full certificate bundle, date checks on) | |
| 3.3 | Probe of a host with an invalid or self-signed certificate | TLS failure, nothing sent | |
| 3.4 | Probe before any sync (clock 1970, NTP blocked) | no TLS attempt at all (D4) | |
| 3.5 | AP with wrong passphrase / out of range | bounded attempts, Wi-Fi off afterwards, backoff recorded | |

## 4. HTTPS delivery against staging (requires the approved staging backend)

| # | Check | Pass criterion | Result |
|---|---|---|---|
| 4.1 | One synthetic CRITICAL event | 201, D1 row with the canonical payload, entry removed | |
| 4.2 | Pull the network after the request (or block the response) | `UNCERTAIN`; retry answered 200 duplicate; one D1 row | |
| 4.3 | Wrong token / wrong path | `FAILED` with AUTH / ENDPOINT flag, queue kept, cleared after the fix | |
| 4.4 | Power cut (USB unplug) at random points during a delivery session, 20 times | no lost event, no duplicate D1 row, same IDs and payloads | |
| 4.5 | `esp_http_client` keep-alive with several queued events | one TLS handshake per session (log or packet capture) | |

## 5. Measurements (replace the estimates; record the numbers)

| # | Measure | How | Result |
|---|---|---|---|
| 5.1 | TLS handshake time (ECDSA P-256, software on the C3) | timestamps around the first request | |
| 5.2 | Heap: free before Wi-Fi, minimum during TLS | `heap_caps_get_minimum_free_size` | |
| 5.3 | Energy of a sync-only session and of a sync + HTTPS session | current measurement (power analyzer) | |
| 5.4 | Wi-Fi association time distribution (10 runs) | log timestamps | |
| 5.5 | Deep-sleep current and RTC drift over 24 h | power analyzer, NTP offset at the next sync | |

## 6. Coexistence and limits (later milestones)

| # | Check | Pass criterion | Result |
|---|---|---|---|
| 6.1 | BLE session followed by a Wi-Fi/TLS session (never concurrent) | both work, heap recovers | |
| 6.2 | Image with NimBLE + HTTPS fits the 1.75 MiB slot | `idf.py size` | |

## 7. Application cycle, adapters and fail-safe (M1.9b domain, M1.9c adapters)

Build with `CONFIG_DSP_APP_RUNTIME=y` (simulated pump, no GPIO). Keep
`CONFIG_DSP_APP_CONNECTIVITY` off unless an approved staging URL is set in
`CONFIG_DSP_APP_NOTIFY_URL` (the build refuses an empty endpoint, and the production endpoint
without `CONFIG_DSP_APP_ALLOW_PRODUCTION`; leave that off during bring-up).

| # | Check | Pass criterion | Result |
|---|---|---|---|
| 7.1 | Pump gate level during reset, boot, brownout, panic and deep sleep | OFF in every state without firmware (pull-down, P1) | |
| 7.2 | Fail-safe path: force an unconfirmed pump-off reading | no deep sleep; SoC reset; gate OFF throughout | |
| 7.3 | **Hardware integration risk:** a pump-off confirmation that never succeeds (broken readback) | the fail-safe resets repeatedly with the pump held OFF by hardware: measure the reset period and current draw (battery drain), decide the hardware recovery mechanism only after 7.1/7.2 | |
| 7.4 | RTC-retained block per reset type (timer, button, software, watchdog, panic, brownout, power-on) | kept / discarded as specified, CRC valid | |
| 7.5 | BLE suspend before a pump run and resume latency; session lifetime (180 s idle, kept while connected, 15 min maximum) on the monotonic clock | no overlap; deadlines met | |
| 7.6 | Application task stack high-water mark (logged before deep sleep), without and with a TLS session | margin to `CONFIG_DSP_APP_TASK_STACK` recorded; adjust the stack | |
| 7.7 | Runtime first boot: provisioning, SERVICE_REQUIRED, no simulated pump run, plan = cap, deep sleep entered | log shows the stages and `deep sleep for 21600 s` | |
| 7.8 | Deep-sleep timer wake: retained block kept (wake count increments), clock continuity kept | logs over 3 wakes | |
| 7.9 | Power cycle and brownout: retained block discarded, clock untrusted until a sync | logs | |
| 7.10 | Deep-sleep current with the runtime image | power analyzer | |
| 7.11 | Pin map from the board schematic: button on GPIO3–5 (not GPIO2), battery ADC on ADC1, pump gate not on strapping/USB/UART0 pins | hardware review record | |

## 8. BLE security (M1.10; needs the M1.10c/d integration)

The M1.10a gate measured noise-c on the host and estimated the stack from the RISC-V call graph;
these figures are **not** target measurements.

| # | Measure | How | Result |
|---|---|---|---|
| 8.1 | Peak heap of one Noise `XXpsk3` and one `IK` handshake (device side) | `heap_caps_get_minimum_free_size` around the handshake (the gate's `esp32c3` probe prints it) | |
| 8.2 | Stack high-water mark of the task running the handshake | `uxTaskGetStackHighWaterMark` | |
| 8.3 | Handshake duration (X25519 in software) for XXpsk3 and IK, and per-frame encrypt/decrypt time | timestamps (`esp_timer_get_time`) | |
| 8.4 | Hardware RNG quality with the radio on during a BLE session (ephemeral keys) | ESP-IDF RNG notes; entropy check | |

### 8a. M1.10c.2b — hardware acceptance of the security integration (OPEN)

M1.10c.2a (software integration) is verified on the host and by ESP32-C3 cross-builds only. None of
that demonstrates the target RNG path, BLE runtime behavior, timing, stack or heap on the device:
cross-build success and host interoperability are **not** evidence for these items. Design
reference: `docs/SECURITY_INTEGRATION_DESIGN.md` section 6.1. No security-enabled firmware
(`CONFIG_DSP_BLE_SECURITY`) is accepted for normal use before R1–R4 pass; the build refuses that
option together with the production endpoint.

**Gate sequence (approved 2026-11-18):**

1. **RNG gate** — the diagnostic image of 8a.1 (no Noise service, no GATT, no privileged BLE
   operation; not a deployment of security-enabled firmware): **R1, R2, R3 and R5 must pass**
   (R6 is run where the image includes it). Only then may stage 2 start.
2. **Controlled security-enabled BLE bring-up** — on the bench only: `sdkconfig.ble.defaults` +
   `CONFIG_DSP_BLE_BRINGUP_POWER_ON_WINDOW`, never the production endpoint, development setup code
   (its provisioning into `nvs_sec` needs a separate approval), no eFuse changes. **R4 is executed
   here** (live CLAIM/SESSION/ADD/RECOVERY with different ephemerals and session IDs across runs),
   together with R7, R8 and 8b H1–H10.
3. **Acceptance for normal use** — security-enabled firmware is accepted for normal use only after
   **R4 has passed** in stage 2 (and the remaining 8a items are recorded). Until then every
   security-enabled image is a bench bring-up image.

| # | Check | Pass criterion | Result |
|---|---|---|---|
| R1 | RNG registration order | `dsp_esp_rng_name()` = `dsp_esp32c3_hw` after the first ready call; `dsp_esp_rng_registered_before_init()` true; no libsodium use before `dsp_esp_rng_register()` | **PASS** 2026-11-19 (RNG-gate image, two runs; 8a.2) |
| R2 | RNG routing | with the BT controller enabled, a handshake raises `dsp_esp_rng_calls()`; libsodium's internal source never used | **PASS** for the RNG-gate part 2026-11-19 (libsodium requests raise the adapter counter: 1 → 6 for 5 requests; one adapter call per 4 KiB `randombytes_buf` block); the handshake part is checked in stage 2 |
| R3 | Refusal without RF | controller disabled: `dsp_esp_rng_ready()` false, HELLO session ID 0, HS message 0 answered HANDSHAKE_FAILED; the `abort()` path not reached | **PASS** for the RNG-gate part 2026-11-19 (ready=0, bytes=0 before the controller starts and after it stops; no abort); the HELLO / HANDSHAKE_FAILED part needs the security service (stage 2) |
| R4 | Live handshakes over BLE | CLAIM (XXpsk3), SESSION (IK), ADD and RECOVERY with a phone (or a host peer over BLE) succeed; two runs give different ephemeral keys and session IDs | |
| R5 | RNG quality sample | ≥ 1 MiB from `esp_fill_random` with RF on, analyzed off-device (`ent`, dieharder or a NIST STS subset); recorded | Capture integrity **PASS** (both runs). Statistical classification **NOT ACCEPTED** under the pre-registered protocol v2 (8a.2). **R5 DEVIATION ACCEPTED FOR BENCH MVP ONLY** (8a.2) |
| R6 | PSK on target | the `test-vectors/ble_session_v1.txt` PSK vectors reproduce on the device through `dsp_psk_derive` (mbedTLS/TF-PSA-Crypto on the ESP32-C3) | **PASS** 2026-11-19 (psk 24/24, psk_not 12/12, both runs) |
| R7 | Target resources | 8.1–8.3 with the real stack: peak heap per handshake, task stack high-water mark during a handshake and during dispatch, handshake and per-frame timing; the static estimates in the design's resource table replaced | |
| R8 | Lifecycle on the device | pump suspension closes the security session (EV_SUSPENDING_FOR_DOSE best effort), reconnect needs a new IK handshake; EV_SESSION_CLOSING before the 15-minute maximum; a plain END_SESSION closes only its connection | |

#### 8a.1 RNG-gate diagnostic image (R1, R2, R3, R5, R6)

Design: `docs/ANDROID_BLE_DESIGN.md` Appendix B. A separate, off-by-default build
(`CONFIG_DSP_RNG_GATE_DIAGNOSTIC`, fragment `sdkconfig.rnggate.defaults`) that enables the BT
controller **only** (`CONFIG_BT_CONTROLLER_ONLY=y`, no NimBLE/Bluedroid host: no GATT, no
advertising, scanning or connections) to activate the RF entropy source. It contains no Noise
service, no `dsp_blesec`, no dispatcher, no application runtime, no GPIO output, no credentials
or setup code, and never initializes NVS (`CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE=n`, so PHY
calibration is never stored; with R6 the PSA persistent-key storage code, which contains `nvs_set_blob`/`nvs_commit`, is linked but never reached, since no PSA key object is created, and `nvs_flash_init` is not in the image, so any NVS open would fail). It writes no eFuse (ESP-IDF startup and the BT controller read the
MAC/chip eFuses as every image does). The configure step refuses it together with
`CONFIG_DSP_APP_ALLOW_PRODUCTION` (also when that flag is only requested in a defaults file).
Cross-built only; **not run on hardware yet**.

Build (no flashing):

```sh
. ~/.espressif/tools/activate_idf_v6.1.sh
cd firmware
idf.py -B build-rnggate -D SDKCONFIG=build-rnggate/sdkconfig \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.rnggate.defaults" build
```

Flash (**proposal only; requires explicit approval before it is run**; the board must have its
antenna attached; writes bootloader, partition table, OTA data and the app, no NVS image, no
eFuse):

```sh
idf.py -B build-rnggate -D SDKCONFIG=build-rnggate/sdkconfig -p /dev/ttyACM0 flash
```

Capture (reads the console; `--reset` pulses RTS once for a normal reboot, DTR stays low, so
the ROM download mode is never entered; without `--reset` start the capture first and then
reset the board by hand):

```sh
python firmware/tools/rng_capture.py --port /dev/ttyACM0 --reset \
       --out-dir rng-out --log rng-out/console.log
# offline re-check of a saved log:
python3 firmware/tools/rng_capture.py --input rng-out/console.log --out-dir rng-out
```

The tool verifies block order, block CRC-32, byte counts and the whole-stream CRC, writes
`rng-out/rng_esp_fill_random.bin` and `rng-out/rng_randombytes_buf.bin` only for an error-free
capture and exits non-zero on any framing/CRC error (then re-run the capture). With the default
115200-baud UART console (also mirrored to the USB-Serial-JTAG port) 1 MiB per source takes
about 2 minutes, 8 MiB about 16 minutes.

How each item is produced (one run per boot, device log lines `RNGGATE ...`):

| # | Produced by | Pass evidence in the log |
|---|---|---|
| R1 | `dsp_esp_rng_register()` before any libsodium use; name and order read after the first ready call | `RNGGATE R1 register=1`, `RNGGATE R1 name=dsp_esp32c3_hw registered_before_init=1` |
| R3 (pre) | controller not initialized: `dsp_esp_rng_ready()` and `dsp_esp_rng_bytes()`; no libsodium call in this state | `RNGGATE R3pre controller_status=0 ready=0 bytes=0` (no abort) |
| — | `esp_bt_controller_init` + `esp_bt_controller_enable(ESP_BT_MODE_BLE)`; first ready call runs `sodium_init` | `RNGGATE controller init=0x0 enable=0x0 status=2 ready=1` |
| R2 | 4 × `randombytes_buf` + 1 × `randombytes_random` through libsodium | `RNGGATE R2 calls_before=a calls_after=b` with b ≥ a + 5; the `randombytes_buf` R5 pass adds one adapter call per block (`adapter_calls_during_sodium_pass`) |
| R5 | `CONFIG_DSP_RNG_GATE_SAMPLE_BYTES` (default 1 MiB, up to 8 MiB) from `esp_fill_random`, then the same amount through libsodium `randombytes_buf` (adapter path), sequentially, controller enabled and re-checked before every block | `RNGBEGIN`/`RNG`/`RNGEND` frames; `RNGGATE R5 esp_fill_random=1 randombytes_buf=1`; capture tool exit 0; then `ent`, dieharder and/or a NIST STS subset on both `.bin` files, results recorded here |
| R6 | `CONFIG_DSP_RNG_GATE_PSK_VECTORS=y` (set by the fragment): every `psk` and `psk_not` record of `test-vectors/ble_session_v1.txt` (CMake-generated header, public synthetic inputs) through `dsp_psk_derive` (PSA HKDF); counts only | `RNGGATE R6 psk pass=24 fail=0 psk_not pass=12 fail=0 (expected 24/12)` |
| R3 (post) | `esp_bt_controller_disable` + `deinit`, then ready/bytes again | `RNGGATE R3post disable=0x0 deinit=0x0 controller_status=0 ready=0 bytes=0` |
| — | summary | `RNGGATE SUMMARY R1=PASS R2=PASS R3pre=PASS R3post=PASS R5=STREAMED R6=PASS ...` |

The R3 criteria about HELLO session ID 0 and HANDSHAKE_FAILED need the security service and are
not part of this image; they are checked with the security+BLE bring-up image (below).

**What this evidence does and does not show:**

- (a) Statistical tests on the samples (`ent`, dieharder, NIST STS, and the tool's histogram
  chi-square/monobit sanity figures) do **not** prove that the entropy source is correctly
  enabled; they only detect gross failures (stuck, biased or repeating output, a broken
  transport). Good-looking statistics are also produced by a pseudo-random generator. The
  evidence that the source is enabled is R1–R3 (registration before use, routing through the
  adapter, refusal without RF, controller status ENABLED while sampling) together with the
  ESP-IDF documented condition that `esp_fill_random` returns true random numbers while the RF
  subsystem (here the BT controller) is enabled.
- (b) **R4 is not covered by this image.** Live CLAIM/SESSION/ADD/RECOVERY handshakes over BLE,
  with different ephemeral keys and session IDs across runs, remain a mandatory hardware
  acceptance item. They are verified in stage 2 of the gate sequence above (controlled
  security-enabled BLE bring-up with `sdkconfig.ble.defaults` +
  `CONFIG_DSP_BLE_BRINGUP_POWER_ON_WINDOW`), which may start only after R1–R3 and R5 have passed
  with this image, using either an Android phone running the M1.10e client (M1.10e.2 or later) or a
  host BLE peer; record which peer was used. Security-enabled firmware is accepted for normal use
  only after R4 passes.

#### 8a.2 RNG-gate results and the bench-only R5 deviation (2026-11-19)

Board: XIAO ESP32-C3 rev v0.4, 4 MB, external antenna, USB power only (no pump, MOSFET or
battery). Firmware: commit `db7e593`, `sdkconfig.defaults` + `sdkconfig.rnggate.defaults`, written
with an explicit build directory (no erase, no credentials, no eFuse change). Evidence is kept by
the owner outside the repository (`~/dispenser-hw-evidence/`), sealed with SHA-256 lists.

**Run 1** (1 MiB per source, `rnggate-20261119/`): capture exit 0, 0 framing/CRC errors, both
files 1,048,576 bytes; device log `RNGGATE SUMMARY R1=PASS R2=PASS R3pre=PASS R3post=PASS
R5=STREAMED R6=PASS`. Offline sanity statistics (own SP 800-22-style implementation, validated
against the SP 800-22 Rev. 1a worked examples) all passed. Post hoc observation: the long-range
bit distribution of both streams looked "too balanced" (not significant after accounting for post
hoc selection).

**Run 2 — confirmatory** (8 MiB per source, `rnggate-confirm-8m/`, image
`rnggate8m-db7e593` with `CONFIG_DSP_RNG_GATE_SAMPLE_BYTES=8388608`), evaluated by protocol v2,
sealed before the capture (`protocol-v2/PROTOCOL.sha256`
`bef89ccc2a61d68215020dd134b95e5d5fcb0765c0b30ada65aafefc415eaeba`; v1 kept as `protocol/`):
- Integrity: VALID. Capture 32 min 48 s, `rng_capture.py` exit 0, 0 framing/CRC errors, both
  files 8,388,608 bytes (SHA-256 `12a9ce8d…a294` `esp_fill_random`, `e08af1b2…83a8`
  `randombytes_buf`), independent reconstruction from the console log byte-identical; same
  boot: `RNGGATE SUMMARY R1=PASS R2=PASS R3pre=PASS R3post=PASS R5=STREAMED R6=PASS
  stack_free_min=6456`.
- Primary confirmatory tests (lower tail, Bonferroni α' = 0.01/6): the "too balanced"
  observation is **not confirmed** (all p ≥ 0.0419): `esp_fill_random` C1 0.783, C2 0.0419,
  C3 0.0484; `randombytes_buf` C1 0.998, C2 0.702, C3 0.534.
- Sanity battery 5a: **FAIL**. `randombytes_buf` frequency p = 0.0016, cumulative sums
  forward p = 0.0021 and backward p = 0.0024 (3 of 14 p-values < 0.01, the rule allows 1; none
  < 0.001). The three reflect one effect: a slight excess of ones over the whole 67 Mbit stream
  (S = +25,840, Z = +3.15, ones fraction 0.500193). All other 5a tests and the deterministic checks
  (entropy, serial correlation, repeated chunks) passed; `esp_fill_random` passed every 5a test.
- NIST STS 2.1.2 (64 × 1 Mbit per source): no flagged row in the independently validated tests
  (Frequency, BlockFrequency, CumulativeSums, Runs, LongestRun, ApproximateEntropy) and none in
  the other core tests; `esp_fill_random` 0 of 188 rows flagged; `randombytes_buf` 2 of the 174
  template/excursion rows flagged (NonOverlappingTemplate #3 proportion 59/64, #7 uniformity
  p = 0.00006), within the allowed 2, no investigation trigger.
- Classification (protocol v2, section 6): **R5 NOT ACCEPTED** — investigate the failing test, no
  repeat capture for statistical reasons.
- Investigation so far: a read-only review of `randombytes_buf` (libsodium 1.0.20) → `dsp_esp_rng`
  adapter → `esp_fill_random` (ESP-IDF v6.1) found **no defect**: no transformation, offset or
  length change on the path, one adapter call per 4 KiB block, identical framing code for both
  sources. The finding (excess of ones in the `randombytes_buf` stream only) is **unresolved**;
  whether it is chance or has a cause is not established.

**Project decision (2026-11-19): R5 DEVIATION ACCEPTED FOR BENCH MVP ONLY.** This is a limited
exception to the gate sequence above (which requires R1–R3 and R5 to pass before stage 2). R5 is
not a security PASS and remains NOT ACCEPTED.
- Allowed: bench-only BLE advertising and GATT testing; controlled handshakes with synthetic
  credentials (development setup code and test identities only); development and debugging of
  the Android ↔ ESP32 path.
- Not allowed: production credentials; normal deployment; real pump operation; declaring the
  security acceptance (8a) complete.
- Full R5 acceptance remains required before any production use. R4, R7, R8 and the stage-2 parts
  of R2 and R3 remain OPEN.

### 8b. M1.10d.2 — hardware acceptance of the NimBLE transport (OPEN)

M1.10d.1 (transport, NimBLE adapter, build option) is verified on the host (unit, fault, fuzz,
two-thread/ThreadSanitizer and end-to-end tests with the noise-c phone through the transport at
MTU 23/64/247) and by ESP32-C3 cross-builds only. None of that is evidence for radio behavior,
Android interoperability, timing or target resources. Design reference:
`docs/BLE_TRANSPORT_DESIGN.md` section 15.2. Build: `sdkconfig.ble.defaults` (build-only, refused
with the production endpoint). Gate sequence: 8a (RNG gate R1–R3 + R5 first, then the controlled
bring-up where R4 runs; acceptance for normal use only after R4 passes).
Without the button driver (M10) the bring-up option `CONFIG_DSP_BLE_BRINGUP_POWER_ON_WINDOW` makes a
power cycle open the service window (never in a production build).

| # | Check | Pass criterion | Result |
|---|---|---|---|
| H1 | Advertising | visible only during a service window; advertising data = Flags + service UUID; scan response = `DSP2` + Service Data (7.1 payload, updated after ownership changes); 100 ms for 30 s after window start/disconnect, then 500 ms; discovery latency recorded | **PARTIAL** 2026-11-19/20: `DSP2` advertised and discovered by a Samsung S23 (8b.1); the rest OPEN |
| H2 | GATT and MTU | service and three characteristics as `BLE_PROTOCOL.md` section 2; CCCD; MTU 247 with Android and a 23-byte client both work; a long/prepared write refused | **PARTIAL** 2026-11-20: service, three characteristics, Info read, notifications on Response, MTU 247 with Android (8b.1); the rest OPEN |
| H3 | Handshakes over BLE | CLAIM, SESSION, ADD, RECOVERY with a phone (together with 8a R4) | |
| H4 | Lifecycle | disconnect/reconnect (new generation, new IK handshake), out of range and supervision timeout, pump suspension (link closed, EV_SUSPENDING_FOR_DOSE best effort, controller stays enabled, no RF activity) and resume; a reply sent before a close (END_SESSION, encrypted MALFORMED) is best effort (T13): record how often it arrives with the 100 ms grace (not a delivery pass condition); the link is terminated ≤ 1.1 s after the close request; suspension completes ≤ 1.5 s and the pump never starts before it returns | |
| H5 | Notifications | throughput with Android; buffer-pool refusals (`tx_busy`) recorded; no frame abandoned (`tx_failures` 0) with a well-behaved client | |
| H6 | RNG with the radio | 8a R1–R5 with this transport (controller enabled at window start) | |
| H7 | Target resources | free heap after NimBLE init and its minimum during a session (gate ≥ 64 KB), NimBLE host task and application task stack high-water marks, image sizes on the device | |
| H8 | Power and teardown | current while advertising / connected / suspended; controller disabled (`is_off`) before Wi-Fi and before deep sleep | |
| H9 | Contention | a second phone repeatedly connecting during the owner's window: per-window counters (`dsp_esp_ble_stats`: connections, refused, fragment errors) recorded; owner still served | |
| H10 | Stack thread-safety | GAP/GATT calls from the application task (advertising, notify, terminate) under load without assertion or deadlock | |

#### 8b.1 First bench BLE advertising and GATT bring-up (2026-11-19/20)

Scope: advertising, discovery, GATT connection and service discovery only, under the bench-only R5
deviation (8a.2). No handshake, no write to Request, no credentials (`nvs_sec` empty), no
provisioning. Board: XIAO ESP32-C3 rev v0.4, 4 MB, external antenna, USB power only (no pump,
MOSFET or battery). Firmware: commit `db7e593`, `sdkconfig.defaults` + `sdkconfig.ble.defaults` +
`CONFIG_DSP_BLE_BRINGUP_POWER_ON_WINDOW=y` (runtime with simulated pump, simulated hardware maximum
run time 0, no GPIO output; BLE security and NimBLE transport; connectivity off; production
endpoint refused), written with an explicit build directory (bootloader, table, otadata, app; no
erase, no eFuse change). Phone: Samsung S23 with nRF Connect for Mobile (a generic scanner; the
Dispenser app was not used). Evidence: owner's folder `ble-bringup-20261119/`.

Completed observations:
- Boot (serial log): 4 MB flash, the approved partition table, app loaded from 0x30000, the
  bring-up warning, BT controller started (`BLE_INIT: Bluetooth MAC: e0:72:a1:1e:1f:36`); no panic,
  watchdog, NVS or NimBLE error.
- Advertising and discovery: `DSP2` advertised and discovered by the S23 at `E0:72:A1:1E:1F:36`.
- GATT: connection established; service `36b60001-3457-4e8e-9cba-8126de3f52e4` with all three
  characteristics discovered (Info `36b60002-…` read, Request `36b60003-…` write, Response
  `36b60004-…` notify); Info READ successful; notifications on Response enabled; ATT MTU 247
  negotiated.
- Disconnect: phone log 00:13:49.245 connected, 00:13:49.805 services discovered, 00:14:04.372
  MTU 247, 00:14:19.327 "Connection terminated by peer" (the device), HCI reason 0x13
  (status 19), 30.082 s after the connection. This is the intended unauthenticated-connection limit (`BLE_PROTOCOL.md`: an
  unauthenticated connection is closed after 30 s; `DSP_LINK_UNAUTH_MS` = 30,000 ms in the
  security layer, closed with `BLE_ERR_REM_USER_CONN_TERM` = 0x13). The service window itself
  continues. No unexpected BLE defect was established.

Remaining for H1: advertising only inside the service window, the exact advertising and scan
response contents (Flags, UUID list, Service Data values), 100 ms / 500 ms intervals, Service
Data update after ownership changes, discovery latency. Remaining for H2: CCCD behaviour in
detail, a 23-byte-MTU client, refusal of a long/prepared write. H3–H10 and 8a R4 remain OPEN.

Test-environment observations:
- An OpenOCD process (`openocd -f board/esp32c3-builtin.cfg`, started by VS Code at 20:41 on
  2026-11-19, the time of the stale-build "Flash Device (JTAG)" incident in section 1) stayed
  attached to the chip's built-in JTAG interface during the restore, both RNG-gate runs and this
  BLE flash. All esptool writes were hash-verified and the readbacks and captures were consistent;
  no effect on flash or data was found. It was no longer running at 00:18 on 2026-11-20 (exit
  time unknown; not terminated by this work).
- During the 23:37–23:57 serial capture the board rebooted four times (`rst:0x15
  USB_UART_CHIP_RESET`) instead of staying in deep sleep, and no `deep sleep for … s` line was
  logged. Hypothesis, **not confirmed**: the attached OpenOCD reset the chip when USB dropped at
  sleep entry. After OpenOCD had gone, the board left USB as expected for deep sleep. The serial
  capture did not cover the phone test (00:13–00:14), and this build logs neither connections nor
  disconnects.

### 8c. Bench GPIO pump control signal (G3 passed, G7 partly; the rest OPEN)

Firmware with `sdkconfig.pumpgpio.defaults` (README "GPIO pump control signal"). **No pump, MOSFET
or other load connected.** Before every run: no debugger (`pgrep -a openocd` empty, no VS Code debug
session) and no other process on the serial port (`fuser /dev/ttyACM0`); a halted CPU would keep the
output at its level. Probe GPIO10 (XIAO D10) against GND with a logic analyzer or scope
(≥ 1 kHz sampling), or a current-limited LED (≥ 1 kΩ in series, ≤ 3 mA) to GND.

| # | Check | Expected | Result |
|---|---|---|---|
| G1 | Level through power-on, reset and boot until the app runs (record the first 2 s after RESET) | low or high-impedance only; never high | |
| G2 | Level during the BLE window and Wi-Fi sync without a due dose | low | |
| G3 | Scheduled dose with every gate open (READY, calibration, limits, reservoir, trusted clock) | one high pulse at the due minute, width = calculated runtime (± 10 ms tick) ≤ 10,000 ms; log `GPIO10 ACTIVE for N ms` | |
| G4 | Same schedule with one gate closed (e.g. SERVICE_REQUIRED) | no pulse | |
| G5 | Through deep sleep after the dose | low (held) | |
| G6 | RESET during a pulse | the pulse ends at the reset; low afterwards; the occurrence is not repeated (history INTERRUPTED) | |
| G7 | Boot log of the GPIO image | `dsp_pump_gpio: BENCH pump control signal on GPIO10, active-high, max 10000 ms per run`; no task-watchdog or panic lines during a 10 s pulse | |

#### 8c.1 First GPIO bench run (2026-11-20, owner-reported)

Firmware `54a7925` (`ble-wifitime-gpio-54a7925`, application at 0x30000 only; bootloader, partition
table, otadata, `nvs` and `nvs_sec` verified unchanged before the first boot). **GPIO only: no pump,
MOSFET or other load was connected; no liquid was moved or measured.** On this firmware a dose
result `COMPLETED` means: the control signal was driven active for the runtime calculated from the
configured flow, and nothing more.

Owner-reported observations (logic analyzer on GPIO10; the captures stay with the owner and are not
in the repository):

- Bench configuration through the debug-only bench screen: pump flow **1,000 µl/s**; readiness set
  to READY by the owner.
- Three newly created schedules of **5 ml** each produced **three separate GPIO10 pulses**, each
  **5 s** wide (5,000 µl at 1,000 µl/s = 5,000 ms: the calculated runtime).
- The reservoir estimate in the app decreased after the doses as expected. (A suspected
  non-update was reported first and then explicitly retracted by the owner; no defect.)
- Changing the time of an already executed schedule did not reliably produce another pulse, new
  schedules did. This is the intended at-most-once rule per schedule and local date, not a GPIO
  fault: see firmware/README.md "Re-timing an executed schedule" (analysis 2026-11-20).

Serial capture of the same firmware before the pulse test (12:17–13:17, redacted copy outside the
repository): 5 boots, each with the driver line of G7, no `GPIO10 ACTIVE` line (READY was off), no
panic, task-watchdog or error line.

| # | Result (2026-11-20) |
|---|---|
| G1 | OPEN (not recorded) |
| G2 | OPEN (not recorded) |
| G3 | **PASSED (owner-reported):** 3 of 3 scheduled 5 ml doses → one pulse each, width 5 s = calculated runtime. Pulse-width precision (± 10 ms) not reported; serial log of the pulses not captured. |
| G4 | OPEN (not recorded) |
| G5 | OPEN (not recorded) |
| G6 | OPEN (not run) |
| G7 | **PARTLY:** driver boot line present in 5 of 5 boots, no panic/watchdog lines in 60 min idle; a 10 s pulse was not run (pulses were 5 s), so "no watchdog during a 10 s pulse" is OPEN. |

Not covered by this run and still required before any real pump: G1/G2/G4–G6, a CPU-independent
hardware cutoff, the external pull-down check, and every measurement of real flow (calibration).

### 8d. Event history (Phase C; OPEN, needs a new firmware image and app)

Offline only so far (host tests, firmware-in-the-loop, NVS emulation; see firmware README "Event
history (Phase C)"). On the bench, with the GPIO image built from the Phase C commit (application at
0x30000 only, after the usual backups and verification; **separate approval required**) and the
matching debug APK:

| # | Check | Expected | Result |
|---|---|---|---|
| E1 | First boot of the new image on the existing NVS | storage opens as layout v5 (no migration), schedules, ownership, reservoir and readiness unchanged; History tab shows exactly the events already in NVS (none invented) | **PARTLY VERIFIED — checklist not complete (firmware 0cbb830).** Screenshot verification by the owner, 2026-11-21: authenticated BLE session ("Připojeno") with the device history displayed — PASS; the three events of 2026-11-20 preserved with matching names, volumes and reservoir transitions (seq 1 refill 17:14:55 417 → 500 ml; seq 2 Nuli 8 ml 17:20:21 OK BENCH 500 → 492; seq 3 Bar 5 ml 17:20:29 OK BENCH 492 → 487; as decoded from the pre-flash `nvs` backup) — **PASS: history persisted across the firmware update.** NOT YET VERIFIED: the installed APK (expected SHA-256 `a3d5f7908ea243bfd6dbf38db8c03bae08d8ee08a5f4214f34d412c6bdcafead`, 0.5.0-beta.2+debug) and the updated F2 debug UI; the current readiness (a dose ran on 2026-11-21, see below, so SERVICE_REQUIRED must be read from the device status before any further test); the reservoir on the status screen (inferred from the history: 495 / 500 ml) |
| E2 | Refill / set capacity in the app (NOT READY) | one RESERVOIR_REFILL / INIT / ADJUST event with before → after | |
| E3 | One new scheduled 5 ml dose (GPIO only, no load) | one 5 s pulse; one DOSE_SCHEDULED OK event tagged "BENCH: jen signál GPIO", estimate before → after − 5 ml | |
| E4 | Reconnect, then a RESET of the board | the same events with the same order after both | |
| E5 | RESET during a pulse (same as G6) | after the reboot exactly one DOSE INTERRUPTED event, reservoir unknown; never repeated by further resets | |
| E6 | "Načíst starší" after more than 12 events | older events appended, no duplicates, the end marker after the oldest | |

**Decisions recorded 2026-11-20 (owner):** residual R1 of the event identity (all 64 event keys and
`evhw` lost together → the log restarts at seq 1; firmware README "Event history (Phase C)") is
ACCEPTED FOR BENCH TESTING ONLY and stays open for production. The history is a best-effort record,
not an audit trail. The dosing anti-replay protection stays independent and unchanged. R5 stays NOT
ACCEPTED (bench-only security deviation, 8a.2). This is not an authorization to flash or install.

Observations 2026-11-21 (owner screenshots, history tab; not a formal E2/E3 run): two further events
after the 0cbb830 update — 13:05 reservoir refill 487 → 500 ml; 13:09 scheduled dose "Test" 5 ml, OK,
tagged BENCH (GPIO signal only), 500 → 495 ml. They show new events appended after the update with the
expected labels and estimates. The 13:09 dose implies the device was READY and the schedule enabled at
13:09; this was not part of an operator-run test (no pulse measurement or serial log recorded), and the
readiness since then is unverified. E2/E3 stay OPEN until run under the procedure.

#### 8d.1 Proposed deployment procedure (requires separate approval; nothing done yet)

Artifacts for the NEXT deployment (incident fixes; source commit
`0cbb8302562251df7bea5b5437a57386d9286e11`, see 8d.2; the board runs ca282f1 since 2026-11-20 17:11):
`~/dispenser-hw-builds/ble-wifitime-gpio-incident-0cbb830/dispenser.bin` 1,211,872 B, SHA-256
`bec8eaac7a4912fc005b457ddb38ad147ddd0cef5751fbdceec06fcd51aa10fb` (erase range again
0x00030000..0x00157fff);
`~/dispenser-hw-builds/apk-debug-incident-0cbb830/app-debug.apk` 18,145,895 B, SHA-256
`a3d5f7908ea243bfd6dbf38db8c03bae08d8ee08a5f4214f34d412c6bdcafead`. Superseded: the ca282f1 APK
`4a2891b9…` (never installed) and, for future writes, the ca282f1 image `84d79e04…`.

1. Preconditions: no pump, MOSFET or load on GPIO10 (analyzer optional); `pgrep -a openocd` empty;
   no VS Code flash/debug session; `fuser /dev/ttyACM0` empty; the board currently runs 54a7925.
2. Offline checks: `sha256sum -c IMAGES.sha256` in the build directory; `FIRMWARE-commit.txt` =
   ca282f1; sdkconfig has GPIO10, 10,000 ms, ISR timer dispatch, task-watchdog panic;
   `cmp` of the partition table with 54a7925's (identical).
3. The owner presses RESET; the script waits for a stable port. Identity with `--after no-reset
   flash-id`: ESP32-C3 rev v0.4, MAC e0:72:a1:1e:1f:34, 4 MB; stop on any difference.
4. `verify-flash` that the board holds the db7e593 bootloader at 0x0, the partition table at 0x10000
   and the 54a7925 application at 0x30000; stop otherwise.
5. Private backups (owner-only directory `~/.config/dispenser-device/bench/backup-<date>-phasec/`,
   mode 0400, SHA256SUMS): `nvs` 0x11000+0x14000, `nvs_sec` 0x28000+0x6000, `otadata` 0x25000+0x2000;
   `nvs_sec` must equal the provisioned image. Never copied into the repository.
6. The only write: `write-flash 0x30000 dispenser.bin` (`--after no-reset`); expect the erase range
   0x00030000..0x00157fff and "Hash of data verified".
7. Before the firmware runs: `verify-flash` of 0x30000; bootloader and table unchanged; `otadata`,
   `nvs`, `nvs_sec` read back and byte-identical to the backups.
8. Boot with a console capture (RTS pulse, 60 min); raw log private (contains the SSID), redacted
   copy to `~/dispenser-hw-evidence/phasec-<date>/`. Expect the driver line of G7, layout v5 opened
   without migration, no panic or watchdog line.
9. APK: verify the SHA-256, then `adb install -r` (keeps the app data and the pairing; never
   uninstall or clear data). If Android refuses for a signature mismatch: stop and report.
10. Run E1–E6 (E3/E5: one new 5 ml schedule, READY set by the owner in the bench screen; E5 also is
    G6). After the tests: NOT READY again in the bench screen.

Lessons from 2026-11-20 (8d.2): the readiness check from the `nvs` backup is essential (it stopped two
runs while the app showed NOT READY); in the script every variable used after a `{ … } | tee` block
must be defined outside it (run 3 stopped falsely after a verified write); the owner's confirmation
of NOT READY must be read back from the device, not from the app's last message.

Rollback (also separately approved): write the 54a7925 application back at 0x30000 only, with the
same verification. The new keys `ev00`…`ev63`/`evhw` stay in `nvs` and are ignored by 54a7925; no
NVS write or erase is part of the rollback.

#### 8d.2 Phase C firmware deployment and the 17:20 incident (2026-11-20)

Approved: firmware update only (ca282f1), first-boot verification. Runs (logs in
`~/dispenser-hw-evidence/phasec-20261120/`, backups owner-only in
`~/.config/dispenser-device/bench/backup-20261120-phasec{,2,3}/`):

| Run | Result |
|---|---|
| 1 (17:05) | identity, bootloader/table/app 54a7925 verified, backups; **stopped**: `nvs` dosing record READY although the owner believed NOT READY. Nothing written |
| 2 (17:08) | same, still READY; **stopped**. Nothing written |
| 3 (17:11) | readiness SERVICE_REQUIRED (2) read back; **only 0x30000 written** (0x30000–0x157fff erased, 1,211,456 B, hash verified); the script then stopped falsely (own variable-scope bug). Separate read-only verification before the first run: app, bootloader, table match; `otadata`, `nvs`, `nvs_sec` byte-identical to the backups |

First boot of ca282f1 (redacted log `serial-phasec-boot.redacted.log`): one boot, GPIO10 driver line,
no panic/watchdog/error line, Wi-Fi connected in 1.8 s, SNTP sample, clock accepted, Wi-Fi stopped
before BLE started, BLE service window up. **17:20:22: two GPIO10 pulses (8,000 ms, 5,000 ms) —
stop condition.** No load connected.

Analysis (offline; evidence: code, redacted log, non-secret records of the 17:11 backup, the owner's
screenshot): no safety-gate bypass. Between 17:12 and 17:20 the owner switched READY in the bench
screen; the firmware accepted it and, in the same loop, found "Nuli" (8 ml, 17:17) and "Bar" (5 ml,
17:20) inside their 10-minute due window, suspended BLE and ran both. The bench screen's reload after
the confirmed answer failed (BLE suspended) and showed "not connected" with the stale NOT READY. The
second pulse respected `min_interval_s` = 1 (starts 8.04 s apart; measured between starts). Fixes:
F1 READY dosing floor (firmware README "READY dosing floor"), F2 Android readiness feedback
(docs/ANDROID_BLE_DESIGN.md §16b), F3 reviewed (the bounded TX drain before a suspend already exists;
now tested), F4 interval semantics labelled; pulse-timing comment corrected. All offline; the board
was not touched after the first boot.

**Deployment of 0cbb830 (2026-11-20, approved firmware-only):** pre-flash read-only check in ROM
download mode (BOOT held at RESET, application never started; `--before/--after no-reset`): 19:23
READY (stop; owner disarmed via the app), 19:50 SERVICE_REQUIRED (2), no schedule eligible before
2026-11-21 10:17. 19:58: identity, db7e593/table/ca282f1 verified; fresh backups (`nvs` identical to
the 19:50 read, `nvs_sec` = provisioned image, readiness 2); only 0x30000 written (0x30000–0x157fff,
1,211,872 B, hash verified); before the first run app, bootloader, table verified and `otadata`,
`nvs`, `nvs_sec` byte-identical. The logger's RTS pulse left the chip in ROM download mode
(`boot:0x0 USB_BOOT`, entered by the BOOT button), so the controlled boot was one manual RESET at
20:18:47. First boot (redacted `serial-0cbb830-boot.redacted.log`): one application boot, driver line,
Wi-Fi 1.6 s, SNTP sample, clock accepted (RTC time had been lost by the USB resets: first trust),
Wi-Fi stopped before BLE, BLE window up; no GPIO10 activation, no panic/watchdog/error line; deep sleep
at 20:21:52 after the idle window. Storage initialization is not logged at INFO level: no error, but a
positive check (Status `storage_ok`, history) needs the app session or a read, both not yet approved.

Still open for this firmware: E1–E6 (need the new APK; with the incident fixes the next deployment is
the 0cbb830 image, 8d.1), confirmation that ownership, schedules and reservoir are unchanged (the
pulses imply storage, credentials and gates were valid; a post-boot NVS read was not authorized).

### 8e. Wake button GPIO3 (bench MVP; **BENCH VERIFIED** 2026-11-21, smoke test W0–W4)

Image: the 0cbb830 bench configuration plus `sdkconfig.button.defaults` (README "Wake button").
Candidate (built 2026-11-21 from 2ab4212 in a fresh directory,
`~/dispenser-hw-builds/ble-wifitime-gpio-button-2ab4212`): `dispenser.bin` 1,214,160 B, SHA-256
`e22474d916413ed748ec27bd3be8b75646f7a1a57a1992b3076d614cd441c6bd`; erase range
`0x00030000..0x00158fff` (one sector more than 0cbb830); partition table byte-identical to
0cbb830/54a7925; the only `CONFIG_` difference from the 0cbb830 build is
`CONFIG_DSP_BUTTON_WAKE_GPIO=3`. Only `0x30000` would be written (the board keeps bootloader db7e593).
Circuit: normally-open button D1/GPIO3 → GND, 10 kΩ GPIO3 → 3V3, nothing on RESET/BOOT; GPIO10
unchanged with no load. Before any flash: approval, then the read-only readiness check in ROM
download mode (the device ran a BENCH dose on 2026-11-21, so NOT READY must not be assumed), stop if
READY; private backups of `nvs`, `nvs_sec`, `otadata`; write only `0x30000`; verify that the
bootloader, partition table, `otadata`, `nvs` and `nvs_sec` are byte-identical before the first boot.

| # | Step | Expected | Result |
|---|---|---|---|
| W0 | Board unpowered: wire the button and pull-up; then powered, multimeter on D1 | ~3.3 V released, ~0 V pressed | NOT MEASURED. Deviation accepted by the owner: button D1 → GND only, **no external 10 kΩ** (internal sleep pull-up, kept by `CONFIG_ESP_SLEEP_GPIO_ENABLE_INTERNAL_RESISTORS=y`, is the only pull-up). The released level is shown indirectly: four sleeps without a spurious wake, every wake caused by a press |
| W1 | First boot (one RESET), console capture running, no connection; wait out the 180 s window | `wake: cause=POWER_ON`; window; `button wake armed (GPIO3, active low)`; `deep sleep for N s` | **PASS** (16:07:31): `wake: cause=POWER_ON (reset reason 1 …)`, Wi-Fi/SNTP, Wi-Fi off, BLE up at 6.06 s, deep sleep at 16:10:37 (180 s idle). The pre-sleep lines (`button wake armed`, `deep sleep for`) are never captured: the USB-Serial-JTAG console drops at sleep entry (same for earlier images); arming is shown by W2 |
| W2 | Short press (~0.2 s) | `wake: cause=BUTTON (reset reason 8, timer=0, gpio=1, gpio mask 0x8)`; `dsp_ble: radio up`; the app connects authenticated; record press → advertising time | **PASS** (16:30:30): exactly that line; `dsp_ble: radio up` at 641 ms after boot (no sync due); no pump line |
| W3 | End the window from the app (or wait 180 s) | both sources armed again; deep sleep | **PASS**: authenticated connection in the app (owner-reported); after ending, deep sleep at 16:34:46 (the app's end closed the connection, the window then ended after 180 s idle) |
| W4 | Natural timer wake (next sync/dose plan, up to 6 h; no schedule change) | `wake: cause=TIMER`; no `dsp_ble: radio up`; deep sleep again | DEFERRED (approved smoke-test scope). Replaced by the debounce check: two more ordinary presses in separate sleep cycles (16:37:25, 16:41:30) → one BUTTON wake and one BLE window each, deep sleep at 16:40:27 and 16:44:30, no further wake until the capture ended at 16:49:10: **PASS** |
| W5 | Hold the button through a sleep entry for ≥ 2 min, then release, then press | `button active at sleep entry: GPIO wake not armed, timer 60 s`; TIMER wakes ~60 s apart without BLE; after release `button wake armed`; the press gives a BUTTON wake | DEFERRED (optional long press not performed; host-tested) |
| W6 | GPIO10 throughout W1–W5 (logic probe), device NOT READY | always low, no pump log line; readiness still SERVICE_REQUIRED in the app afterwards | **PASS (log level)**: no pump run, cutoff or error line in any of the 4 boots; GPIO10 not probed electrically. Readiness read from the backup before the flash: SERVICE_REQUIRED; no operation changed it afterwards |

Deployment 2026-11-21 (owner approval of the same day; `deploy-2ab4212.log`, redacted
`serial-2ab4212.redacted.log` in `~/dispenser-hw-evidence/wakebutton-20261121`): ROM download mode,
identity ESP32-C3 v0.4 / e0:72:a1:1e:1f:34 / 4 MB; flash held bootloader db7e593, the partition
table and app 0cbb830; private backups of `nvs`, `nvs_sec`, `otadata` (each read twice, identical;
`nvs_sec` = provisioned image); readiness **SERVICE_REQUIRED**; only `0x30000` written (erase
`0x00030000..0x00158fff`, 1,214,160 B, hash verified); app, bootloader and table verified,
`otadata`, `nvs`, `nvs_sec` byte-identical before the first boot. No erase, provisioning, schedule,
readiness or reservoir change, no dose.

Logger finding: the earlier `ble_logger.py` reopened the port after every deep sleep with DTR set low
first, passing through DTR=0/RTS=1, the USB-JTAG chip-reset state (`rst:0x15 USB_UART_CHIP_RESET`
right after each "port reopened" in the 2026-11-20 GPIO logs): those wakes were turned into resets
by the capture. The new capture leaves the control lines untouched on reopen; the wake causes above
are therefore genuine deep-sleep wakes (`reset reason 8`).

## 9. Configuration limits and storage (M1.10b.1)

The firmware accepts every representable value of the calibration and safety fields (only
logical rules are enforced: `max_daily_ul ≥ max_dose_ul`, `max_runtime_ms ≤` the hardware
cutoff, calibration time 0 or ≥ 2025-01-01); all arithmetic is overflow-safe and host-tested at
the 32-bit extremes. **Physical bounds are not invented and remain open until measured:**

| # | Item | How | Result |
|---|---|---|---|
| 9.1 | Plausible range of the calibrated flow `pump_flow_ul_per_s` for the chosen pump at nominal battery voltage (FR-011/012), and whether to refuse values outside it | calibration runs into a measuring vessel | |
| 9.2 | Hardware cutoff of the real pump driver (`hw_max_runtime_ms`, simulation default 0, Kconfig 0–60,000 ms) and a sensible `max_runtime_ms` | driver design + bench run | |
| 9.3 | Sensible ranges for `max_dose_ul`, `max_daily_ul`, `min_interval_s` (FR-016) | pump tests, product decision | |
| 9.4 | NVS on real flash: provisioning, maximum-size `sched` (3,160 B) rewrites, power loss during writes | bench power cuts with a relay | |

**BLOCKER (open): ESP-IDF v6.1 NVS power-loss defect.** A power cut after the new version's last
data chunk and before its blob index makes NVS recovery erase the old version too, so the record
is lost (reproduced in `firmware/test/nvs_linux`, 14 of 14 scenarios; espressif/esp-idf#19212).
The storage then fails closed (CORRUPT, no automatic dosing). **Fix implemented and approved in software:**
project-scoped patched `nvs_flash` (`firmware/third_party/nvs_flash`, patches for #19212, #19213,
#19214), passing the emulator matrices and ESP-IDF's NVS suite. The blocker stays open until the
bench power cuts (9.4) pass on real ESP32-C3 flash; 9.4 is NOT complete.
