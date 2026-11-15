# Dispenser

Battery-powered automatic aquarium liquid-carbon dosing device (ESP32-C3) with a native
Android companion app.

- [`android/`](android/README.md) – Android application (Kotlin, Jetpack Compose). Currently
  an offline **DEMO** build: schedules are stored on the phone, device values are simulated.
- `firmware/` – ESP32 firmware (planned).
- [`docs/`](docs/) – requirements, architecture, [release process](docs/RELEASING.md) and the
  [backup file format](docs/BACKUP_FORMAT.md).

## Privacy

The app has no analytics or telemetry and stores its data only on the phone. Android system
backup (Google cloud backup and device-to-device transfer) is disabled for the app; data is
moved only with the manual JSON export in *Nastavení → Záloha a obnovení*.

## License

Copyright 2026 Nowass

The Android application and the future ESP32 firmware are licensed under the
[Apache License, Version 2.0](LICENSE). See [NOTICE](NOTICE) for attributions, including
third-party material.

Hardware, PCB and mechanical design files are not covered by this license; their license
has not been decided yet.
