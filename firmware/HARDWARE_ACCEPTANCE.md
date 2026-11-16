# Dispenser V2 — on-device acceptance checklist (XIAO ESP32-C3)

What M1.7/M1.8 could **not** verify without the board. Run it once a Seeed Studio XIAO
ESP32-C3 is available, record the results (date, firmware commit, board, measured values) in
the "Result" column, and keep the safety rules below. Nothing here has been run yet.

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
| 1.1 | `python -m esptool --chip esp32c3 flash-id` | 4 MB (0x400000) detected | |
| 1.2 | First full flash (`idf.py flash`: bootloader, partition table at 0x10000, `ota_data_initial`, app) | boots; partition table printed matches `partitions.csv` | |
| 1.3 | Boot log | identification line, no reset loop | |
| 1.4 | `idf.py flash` again | `nvs_sec` content unchanged (only `erase-flash` wipes it) | |

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
