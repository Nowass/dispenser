# Dispenser

Battery-powered automatic aquarium liquid-carbon dosing device (ESP32-C3) with a native
Android companion app.

- [`android/`](android/README.md) – Android application (Kotlin, Jetpack Compose). Currently
  an offline **DEMO** build: schedules are stored on the phone, device values are simulated.
- [`firmware/`](firmware/README.md) – ESP32-C3 firmware (ESP-IDF v6.1). Currently a skeleton
  plus host-tested domain logic: recurrence engine (M1.1), execution history and
  deduplication (M1.2), persistent storage and configuration with an NVS adapter (M1.3),
  time management with Wi-Fi/SNTP adapters (M1.4), notification engine with a persistent
  queue (M1.5) and its HTTPS transport with a connectivity session (M1.7), verified end to
  end against a local Worker (M1.8), the Dosing Service with storage layout v4 (M1.9a), and
  the Application and Power Services wake cycle (M1.9b), and opt-in ESP-IDF platform adapters
  with a simulated pump (M1.9c), the BLE protocol v1 codec with its Noise feasibility gate
  (M1.10a), and the configuration, schedule and phone-time operations for the future BLE
  dispatcher (M1.10b.1), and the manual dose with durable per-phone anti-replay (M1.10b.2).
  Nothing drives hardware yet; not yet run on hardware. **Open firmware
  release and hardware-acceptance blocker:** an ESP-IDF v6.1 NVS power-loss defect
  ([`firmware/test/nvs_linux`](firmware/test/nvs_linux/README.md)); fix implemented, bench verification pending
  ([`firmware/third_party/nvs_flash`](firmware/third_party/nvs_flash/README.md)).
- [`cloudflare/dispenser-notify/`](cloudflare/dispenser-notify/README.md) – Cloudflare Worker
  and D1 backend that durably accepts device notifications (M1.6a, deployed; protocol in
  its [`PROTOCOL.md`](cloudflare/dispenser-notify/PROTOCOL.md)) and e-mails them through the
  Gmail API (M1.6b, deployed and verified end to end; a 5-minute cron delivers e-mails).
- [`test-vectors/`](test-vectors/README.md) – shared test cases run by both the Android and
  the firmware tests (scheduling parity).
- [`docs/`](docs/) – requirements, architecture, [release process](docs/RELEASING.md) and the
  [backup file format](docs/BACKUP_FORMAT.md).

## Installation (Android)

No release has been published yet. The first release, `0.5.0-beta.1`, is in preparation; it
will be an APK attached to a GitHub Release together with its SHA-256 checksum:

1. Download `dispenser-<version>.apk` and `dispenser-<version>.apk.sha256` from the release.
2. Verify the download: `sha256sum -c dispenser-<version>.apk.sha256`.
   Optionally verify the signer with `apksigner verify --print-certs dispenser-<version>.apk`;
   official APKs are signed only with this release certificate (SHA-256):
   `32:C2:4A:F8:B4:13:8D:49:CF:E8:D8:C6:95:19:A2:6D:44:2D:F6:4C:AF:F5:BE:E7:C7:29:56:FA:C7:CE:64:29`
3. Install the APK (Android 8.0+, "install unknown apps" must be allowed for the browser or file
   manager).

Before replacing or reinstalling the app, export a backup (*Nastavení → Záloha a obnovení*):
Android system backup is disabled for the app. Release process, signing and the migration from
development builds: [docs/RELEASING.md](docs/RELEASING.md).

## Development

Firmware build, host tests and toolchain (ESP-IDF v6.1, target `esp32c3`):
[firmware/README.md](firmware/README.md). The firmware is not built in CI yet.

Android build instructions and architecture notes: [android/README.md](android/README.md). CI
(`.github/workflows/android.yml`) builds, runs the JVM unit tests and lint, compiles the
instrumented tests (not executed in CI) and builds an unsigned release APK.

## Privacy

The app has no analytics or telemetry and stores its data only on the phone. Android system
backup (Google cloud backup and device-to-device transfer) is disabled for the app; data is
moved only with the manual JSON export in *Nastavení → Záloha a obnovení*.

## License

Copyright 2026 Nowass

The Android application and the future ESP32 firmware are licensed under the
[Apache License, Version 2.0](LICENSE). See [NOTICE](NOTICE) for attributions, including
third-party material, and [docs/assets/README.md](docs/assets/README.md) for asset provenance.

Hardware, PCB and mechanical design files are not covered by this license; their license
has not been decided yet.
