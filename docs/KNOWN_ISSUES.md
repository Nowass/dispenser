# Known issues and deferred items (bench MVP)

Single personal-use aquarium dispenser, developed in small MVP increments. The items below are
known, deliberately deferred, and recorded once here; accepted functionality is not reopened unless
a defect shows up. Details live in `firmware/HARDWARE_ACCEPTANCE.md` and `firmware/README.md`.

Status 2026-11-21. Phase C (event history, GET_HISTORY, Android history screen) is considered
**good enough for the bench MVP** by the owner.

| Item | Status | Where |
|---|---|---|
| Remaining Phase C hardware acceptance | E1 partly verified (history persisted across the 0cbb830 update); E2–E6 and GPIO G1, G2, G4–G6 (G7 partly) not run. Not blocking further increments. | HARDWARE_ACCEPTANCE §8c, §8d |
| R5 RNG statistical classification | NOT ACCEPTED; bench-only deviation (synthetic credentials, bench network). Must pass before any production use. The bring-up power-on BLE window belongs to the same bench-only setup. | HARDWARE_ACCEPTANCE §8a.2 |
| F1/F2 hardware verification (READY dosing floor; Android readiness/reload status) | Implemented and host-tested in 0cbb830; not verified on hardware: APK identity and F2 UI, readiness read from the device after the 2026-11-21 13:09 dose, reservoir on the status screen. | HARDWARE_ACCEPTANCE §8d, E1 row |
| Real pump electrical safety and calibration | Only the GPIO10 control signal exists (no load). The MOSFET/driver circuit, gate pull-down, flyback, supply, brownout behavior, cooldown between runs and ml/s calibration need their own review. | README "Requirements for the real pump driver", §8c |
| BLE availability | BLE exists only inside service windows (button wake, or power-on in the bring-up build): 180 s without a client, 15 min at most. A sleeping device cannot be woken over BLE; the phone must ask for a button press. | README "Wake button", BLE_PROTOCOL |
| Wake button GPIO3 | Implemented, host-tested and cross-built; not verified on hardware (wake current, pad hold, held-button path). | HARDWARE_ACCEPTANCE §8e |
| Storage at rest | `nvs` and `nvs_sec` are not encrypted (Wi-Fi passphrase, setup code readable with physical access). | README "Wi-Fi time synchronization only" |
| Event history | Best-effort user record, not an audit trail (R1 accepted for bench only); dosing anti-replay is independent. | HARDWARE_ACCEPTANCE §8d |
