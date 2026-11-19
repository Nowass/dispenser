# Dispenser V2 — on-device acceptance checklist (XIAO ESP32-C3)

What M1.7/M1.8 could **not** verify without the board. Run it once a Seeed Studio XIAO
ESP32-C3 is available, record the results (date, firmware commit, board, measured values) in
the "Result" column, and keep the safety rules below. Only checks 1.1–1.3 have been run so far
(2026-11-19); everything else is still OPEN.

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
| R1 | RNG registration order | `dsp_esp_rng_name()` = `dsp_esp32c3_hw` after the first ready call; `dsp_esp_rng_registered_before_init()` true; no libsodium use before `dsp_esp_rng_register()` | |
| R2 | RNG routing | with the BT controller enabled, a handshake raises `dsp_esp_rng_calls()`; libsodium's internal source never used | |
| R3 | Refusal without RF | controller disabled: `dsp_esp_rng_ready()` false, HELLO session ID 0, HS message 0 answered HANDSHAKE_FAILED; the `abort()` path not reached | |
| R4 | Live handshakes over BLE | CLAIM (XXpsk3), SESSION (IK), ADD and RECOVERY with a phone (or a host peer over BLE) succeed; two runs give different ephemeral keys and session IDs | |
| R5 | RNG quality sample | ≥ 1 MiB from `esp_fill_random` with RF on, analyzed off-device (`ent`, dieharder or a NIST STS subset); recorded | |
| R6 | PSK on target | the `test-vectors/ble_session_v1.txt` PSK vectors reproduce on the device through `dsp_psk_derive` (mbedTLS/TF-PSA-Crypto on the ESP32-C3) | |
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
| H1 | Advertising | visible only during a service window; advertising data = Flags + service UUID; scan response = `DSP2` + Service Data (7.1 payload, updated after ownership changes); 100 ms for 30 s after window start/disconnect, then 500 ms; discovery latency recorded | |
| H2 | GATT and MTU | service and three characteristics as `BLE_PROTOCOL.md` section 2; CCCD; MTU 247 with Android and a 23-byte client both work; a long/prepared write refused | |
| H3 | Handshakes over BLE | CLAIM, SESSION, ADD, RECOVERY with a phone (together with 8a R4) | |
| H4 | Lifecycle | disconnect/reconnect (new generation, new IK handshake), out of range and supervision timeout, pump suspension (link closed, EV_SUSPENDING_FOR_DOSE best effort, controller stays enabled, no RF activity) and resume; a reply sent before a close (END_SESSION, encrypted MALFORMED) is best effort (T13): record how often it arrives with the 100 ms grace (not a delivery pass condition); the link is terminated ≤ 1.1 s after the close request; suspension completes ≤ 1.5 s and the pump never starts before it returns | |
| H5 | Notifications | throughput with Android; buffer-pool refusals (`tx_busy`) recorded; no frame abandoned (`tx_failures` 0) with a well-behaved client | |
| H6 | RNG with the radio | 8a R1–R5 with this transport (controller enabled at window start) | |
| H7 | Target resources | free heap after NimBLE init and its minimum during a session (gate ≥ 64 KB), NimBLE host task and application task stack high-water marks, image sizes on the device | |
| H8 | Power and teardown | current while advertising / connected / suspended; controller disabled (`is_off`) before Wi-Fi and before deep sleep | |
| H9 | Contention | a second phone repeatedly connecting during the owner's window: per-window counters (`dsp_esp_ble_stats`: connections, refused, fragment errors) recorded; owner still served | |
| H10 | Stack thread-safety | GAP/GATT calls from the application task (advertising, notify, terminate) under load without assertion or deadlock | |

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
