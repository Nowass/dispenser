# Bench checklist: first Android CLAIM on the ESP32-C3 (owner-assisted)

**BENCH ONLY.** This runs under the project decision "R5 DEVIATION ACCEPTED FOR BENCH MVP ONLY"
(`firmware/HARDWARE_ACCEPTANCE.md` 8a.2). R5 stays NOT ACCEPTED. Synthetic credentials only; no
production credentials, no normal deployment, no pump. Passing this checklist does **not** complete
the security acceptance (8a R4 needs SESSION, ADD and RECOVERY and two runs; see the end).

Every step that writes to the board or the phone is done by the owner, one at a time. Stop at any
**STOP** condition and keep the evidence.

## 0. State before starting (verify, do not change)

| Item | Expected |
|---|---|
| Board | Seeed XIAO ESP32-C3 rev v0.4, 4 MB, external antenna, USB power only, no pump/MOSFET/battery |
| Firmware on the board | BLE bring-up image `~/dispenser-hw-builds/ble-bringup-db7e593` (`IMAGES.sha256`), flashed 2026-11-19 |
| `nvs_sec` | erased (no credentials) |
| Device ID (for the label) | **`e072a11e1f34`** = the eFuse **base MAC** (`esptool read-mac`: `MAC: e0:72:a1:1e:1f:34`). It is **not** the Bluetooth address `E0:72:A1:1E:1F:36` (base + 2). |
| Debug APK | `android/app/build/outputs/apk/debug/app-debug.apk` built from branch `dev/ble-claim-prep` (contains the association fix `b3aa9ea`; without it every real CLAIM stops at "other device") |
| PC tools | `. ~/.espressif/tools/activate_idf_v6.1.sh`; no VS Code serial monitor, debug session or "Flash Device" running (`pgrep -a openocd` prints nothing; `fuser /dev/ttyACM0` prints nothing) |

The board sleeps after each service window (USB disappears). **RESET** starts a new boot and a new
window: 180 s without a connection; any single connection is closed by the device after **30 s**
unless a handshake has authenticated it. The whole CLAIM (connect, HELLO, three handshake messages,
enrollment result) must finish inside that 30 s; the app does it automatically.

## 1. Install the debug app next to beta.2

1. On the S23: enable Developer options → USB debugging; connect it by USB; allow the PC.
2. On the PC:
   ```sh
   ADB=~/Android/Sdk/platform-tools/adb
   $ADB devices                          # the S23 listed as "device"
   $ADB install -r android/app/build/outputs/apk/debug/app-debug.apk
   $ADB shell pm list packages | grep cz.nowass.dispenser
   ```
   Expected: `cz.nowass.dispenser.debug` installed **next to** `cz.nowass.dispenser` (beta.2,
   untouched); launcher name "Dispenser DEBUG".
3. Optional, recommended (first real run of the Keystore code, which no CI run executes):
   ```sh
   cd android && ./gradlew :app:connectedDebugAndroidTest \
     -Pandroid.testInstrumentationRunnerArguments.class=cz.nowass.dispenser.crypto.KeystoreKeyWrapperTest
   ```
   Expected: all tests pass on the S23. **STOP** if they fail (credential storage would be broken).

## 2. Synthetic credentials (PC, owner only)

```sh
install -d -m 700 ~/.config/dispenser-device/bench
cd ~/.config/dispenser-device/bench
python3 - <<'EOF'
import json, os, secrets
d = {"wifi_ssid": "DSP-BENCH-NONE",                       # placeholder, not a real network
     "wifi_pass": "",                                     # open network = key absent
     "api_token": "bench-dummy-" + secrets.token_hex(24)} # placeholder, no backend accepts it
fd = os.open("bench-secrets.json", os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
os.write(fd, json.dumps(d).encode()); os.close(fd)
EOF
cd ~/GIT/dispenser
. ~/.espressif/tools/activate_idf_v6.1.sh
python firmware/tools/provision_dev_secrets.py \
  --secrets ~/.config/dispenser-device/bench/bench-secrets.json \
  --out ~/.config/dispenser-device/bench/nvs_sec.bin \
  --label-out ~/.config/dispenser-device/bench/label.txt --device-id e072a11e1f34
```
Expected: `wrote …/nvs_sec.bin (mode 0600, 24576 bytes, partition nvs_sec at 0x28000)` and
`wrote the setup label … the code is not shown`. The tool generates the setup code, stores it in
`bench-secrets.json` (owner-only) and never prints it. Wi-Fi is never started by the BLE image
(connectivity off), so the placeholders are never used. Do not paste any of these files anywhere.

## 3. Provision `nvs_sec` (one region write, owner-approved)

The board must be awake: press **RESET**, then run the command right away (or hold BOOT, tap RESET,
release BOOT for download mode).
```sh
python -m esptool --chip esp32c3 -p /dev/ttyACM0 write-flash 0x28000 ~/.config/dispenser-device/bench/nvs_sec.bin
```
Expected: one region `0x00028000`–`0x0002dfff` written, "Hash of data verified". Nothing else.
**STOP** if any other offset is written. Never use `erase-flash` (it wipes everything).

## 4. Verify the provisioning (read-only)

```sh
python -m esptool --chip esp32c3 -p /dev/ttyACM0 read-flash 0x28000 0x6000 ~/.config/dispenser-device/bench/nvs_sec.readback.bin
cmp ~/.config/dispenser-device/bench/nvs_sec.bin ~/.config/dispenser-device/bench/nvs_sec.readback.bin && echo "readback identical"
sha256sum ~/.config/dispenser-device/bench/nvs_sec.bin    # record the hash only
shred -u ~/.config/dispenser-device/bench/nvs_sec.bin ~/.config/dispenser-device/bench/nvs_sec.readback.bin
```
Expected: `readback identical`. The images are unencrypted secrets: shred them after the check (the
code stays in `bench-secrets.json`, which keeps `label.txt` valid). **STOP** on a mismatch.

## 5. Start a service window

Press **RESET**, or start the optional console capture, which resets the board once itself (one RTS
pulse, normal boot) and then only reads; the firmware prints no secrets:
`~/.espressif/tools/python/v6.1/venv/bin/python ~/dispenser-hw-evidence/ble-bringup-20261119/ble_logger.py <new log path> 600`.
Expected boot: `app: BRING-UP build: a power-on wake opens a BLE service window`, the BT controller
lines, no panic. Do steps 6 within 180 s (press RESET again if the window has closed).

## 6. First CLAIM (Samsung S23, "Dispenser DEBUG")

1. Settings → **Zařízení** → **Připojit zařízení**. Allow "Zařízení v okolí" (Nearby devices).
2. **Kód zařízení**: type the text of `~/.config/dispenser-device/bench/label.txt` exactly
   (`DSP2:e072a11e1f34:` + 28 characters; spaces and hyphens are ignored). Show the file only on
   your own screen (`less …/label.txt`); do not photograph it or send it anywhere.
3. In the system dialog pick the dispenser (`DSP2`, `E0:72:A1:1E:1F:36`). The address differs from
   the label's device ID; that is expected.
4. **Název telefonu**: e.g. `S23`. Start the pairing.

Expected: the enrollment finishes, the app switches to DEVICE mode and shows the device; no error.

| Outcome | Meaning | Action |
|---|---|---|
| Success, DEVICE mode | CLAIM done | continue with 7 |
| "Odpovědělo jiné zařízení, než je na štítku." (WRONG_DEVICE) | the label's device ID is not this board | **STOP**: check `--device-id` = base MAC `e072a11e1f34` |
| "Ověření selhalo – zkontrolujte kód ze štítku." | PSK mismatch: label and `nvs_sec` differ, or a typo | retype the label once after RESET; then **STOP** and check steps 2–4 |
| "Nepodařilo se připojit…" / no device in the dialog | window closed or out of range | press RESET, retry |
| "Zařízení už patří jinému telefonu." | the board is already claimed | **STOP** (see 10) |
| An unconfirmed pairing is reported (staged key) | the enrollment result was lost | press RESET, then confirm it in the app (IK); **do not discard** unless confirming keeps failing |
| Crash, ANR, reset loop on the board | defect | **STOP**, keep logs |

## 7. Verify ownership on the board

- nRF Connect (optional): scan `DSP2`; in the 4-byte advertising Service Data the second byte
  (flags) now has bit 0 (CLAIMED) set; reading Info gives `claim_state` = 1 (7th byte).
  **Do not write to Request.**
- Press RESET; after the reboot the device still advertises CLAIMED (ownership is stored in the
  `nvs` partition).

## 8. Subsequent SESSION (IK)

Press RESET to open a window; in the app (DEVICE mode) refresh/connect. Expected: the app connects
and reads the device status (an IK handshake with the pinned key). Repeat once after another RESET.

## 9. Evidence (security-safe)

Into `~/dispenser-hw-evidence/claim-<date>/`: screenshots of the app steps and results, the serial
log, the `nvs_sec` image SHA-256 from step 4, nRF Connect screenshots of the CLAIMED advertising.
**Never** collect the label, `bench-secrets.json`, the images or screenshots that show the code.
If logcat is wanted: `adb logcat -d --pid=$(adb shell pidof cz.nowass.dispenser.debug)`; check it
contains no label text before keeping it.

## 10. Recovering from a partial or failed enrollment

- Failure before the third handshake message: nothing was stored on either side; retry after RESET.
- Unconfirmed (staged) key on the phone: confirm it after RESET (the app offers it); discard only if
  the device reports it never stored the enrollment.
- The board is CLAIMED but the phone has no credential (app data cleared, "Zapomenout zařízení"
  on the phone is local only): RECOVERY is **not available on this board** (it needs the button
  driver, milestone M10; `recovery_hold_ms` is never set). The bench way back is an
  owner-approved region erase of the application storage, which removes the ownership record:
  `python -m esptool --chip esp32c3 -p /dev/ttyACM0 erase-region 0x11000 0x14000`, then CLAIM again
  (the setup code in `nvs_sec` stays valid).
- To return the board to the M0.2 skeleton: `idf.py -B ~/dispenser-hw-builds/restore-20261119 -D
  SDKCONFIG=$HOME/dispenser-hw-builds/restore-20261119/sdkconfig -p /dev/ttyACM0 flash`.

## What this checklist does not complete

8a R4 also needs ADD (a second phone identity with an invitation), RECOVERY (button driver) and
evidence that two runs used different ephemeral keys and session IDs; R7 (target resources) and R8
(lifecycle) are separate. H3 is partially covered by steps 6 and 8 only.
