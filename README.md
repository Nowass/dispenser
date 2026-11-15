# Dispenser

Battery-powered automatic aquarium liquid-carbon dosing device (ESP32-C3) with a native
Android companion app.

- [`android/`](android/README.md) – Android application (Kotlin, Jetpack Compose). Currently
  an offline **DEMO** build: schedules are stored on the phone, device values are simulated.
- `firmware/` – ESP32 firmware (planned).
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

Build instructions and architecture notes: [android/README.md](android/README.md). CI
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
