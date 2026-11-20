# Bench checklist: first Android CLAIM on the ESP32-C3 (owner-assisted)

**BENCH ONLY.** This runs under the project decision "R5 DEVIATION ACCEPTED FOR BENCH MVP ONLY"
(`firmware/HARDWARE_ACCEPTANCE.md` 8a.2). R5 stays NOT ACCEPTED. Synthetic credentials only; no
production credentials, no normal deployment, no pump. Passing this checklist does **not** complete
the security acceptance (8a R4 needs SESSION, ADD and RECOVERY and two runs; see the end).

Every step is done by the owner, one at a time. Steps are marked **[READ-ONLY]** (reads the board,
the phone or files) or **[WRITE]** (changes the phone, the board or creates secret files). Stop at
any **STOP** condition and keep the evidence. Never print, paste, photograph or commit the setup
label, `bench-secrets.json` or the `nvs_sec` images.

## 0. State before starting [READ-ONLY]

| Item | Expected |
|---|---|
| Board | Seeed XIAO ESP32-C3 rev v0.4, 4 MB, external antenna, USB power only, no pump/MOSFET/battery |
| Firmware on the board | BLE bring-up image `~/dispenser-hw-builds/ble-bringup-db7e593` (`IMAGES.sha256`), flashed 2026-11-19 |
| `nvs` (0x11000, 80 KiB) | runtime storage written by the BLE image (provisioned on first boot: ownership record UNCLAIMED, PHY calibration) |
| `nvs_sec` (0x28000, 24 KiB = 0x6000) | no credentials; erased on 2026-11-19, probably initialized as an empty NVS by the BLE image at boot (`nvs_flash_init_partition`) |
| Device ID (for the label) | **`e072a11e1f34`** = the eFuse **base MAC**. It is **not** the Bluetooth address `E0:72:A1:1E:1F:36` (base + 2), which the app uses only to connect |
| Debug APK | `android/app/build/outputs/apk/debug/app-debug.apk` from branch `dev/ble-claim-prep` (contains the association fix `b3aa9ea`; without it every real CLAIM stops at "other device"), SHA-256 `54b8e7bc239022506ce1ca82722a8e653173b2d31799e7853d4695b66843ebc5` |
| PC | `. ~/.espressif/tools/activate_idf_v6.1.sh`; no VS Code serial monitor, debug session or "Flash Device" (`pgrep -a openocd` and `fuser /dev/ttyACM0` print nothing) |

The board sleeps after each service window (USB disappears). **RESET** starts a new boot and a new
window: 180 s without a connection; any single connection is closed by the device after **30 s**
unless a handshake has authenticated it, so the whole CLAIM (connect, HELLO, three handshake
messages, enrollment result) must finish within 30 s of connecting; the app does it automatically.
For esptool the board must be awake: press RESET and run the command right away, or hold BOOT, tap
RESET, release BOOT (download mode).

```sh
cd ~/GIT/dispenser && . ~/.espressif/tools/activate_idf_v6.1.sh
sha256sum android/app/build/outputs/apk/debug/app-debug.apk         # [READ-ONLY] the hash above
python -m esptool --chip esp32c3 -p /dev/ttyACM0 read-mac           # [READ-ONLY] MAC: e0:72:a1:1e:1f:34
```

## 1. Phone: Keystore check first, then the debug app

1. On the S23: Developer options → USB debugging on; connect by USB; allow the PC. Keep the phone
   **unlocked** during step 1.2 (the credential keys require an unlocked device).
2. **[WRITE, phone, temporary]** First real run of the Keystore code (no CI run executes it). The
   Gradle connected-test task installs the debug app and its test APK, runs the test and normally
   uninstalls both afterwards, so run it **before** step 1.3:
   ```sh
   cd ~/GIT/dispenser/android
   ./gradlew :app:connectedDebugAndroidTest \
     -Pandroid.testInstrumentationRunnerArguments.class=cz.nowass.dispenser.crypto.KeystoreKeyWrapperTest
   ```
   Expected: `BUILD SUCCESSFUL`, all 3 tests passed on the S23. The test uses its own key alias and
   deletes it. The release app `cz.nowass.dispenser` is not touched. **STOP** if it fails.
3. **[WRITE, phone]** Install the debug app next to beta.2:
   ```sh
   ADB=~/Android/Sdk/platform-tools/adb
   $ADB devices                                                     # [READ-ONLY] S23 listed as "device"
   $ADB install -r ~/GIT/dispenser/android/app/build/outputs/apk/debug/app-debug.apk
   $ADB shell pm list packages | grep cz.nowass.dispenser           # [READ-ONLY]
   ```
   Expected: `Success`; both `package:cz.nowass.dispenser` (beta.2, untouched) and
   `package:cz.nowass.dispenser.debug` listed; launcher name "Dispenser DEBUG".

## 2. Synthetic credentials [WRITE, PC files only]

```sh
install -d -m 700 ~/.config/dispenser-device/bench
cd ~/.config/dispenser-device/bench
python3 - <<'EOF'
import json, os, secrets
d = {"wifi_ssid": "DSP-BENCH-NONE",                       # placeholder, not a real network
     "wifi_pass": "",                                     # open network = key absent in nvs_sec
     "api_token": "bench-dummy-" + secrets.token_hex(24)} # 60 chars placeholder, no backend accepts it
fd = os.open("bench-secrets.json", os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
os.write(fd, json.dumps(d).encode()); os.close(fd)
EOF
cd ~/GIT/dispenser && . ~/.espressif/tools/activate_idf_v6.1.sh
python firmware/tools/provision_dev_secrets.py \
  --secrets ~/.config/dispenser-device/bench/bench-secrets.json \
  --out ~/.config/dispenser-device/bench/nvs_sec.bin \
  --label-out ~/.config/dispenser-device/bench/label.txt --device-id e072a11e1f34
```
Expected output (nothing secret is printed):
`wrote …/nvs_sec.bin (mode 0600, 24576 bytes, partition nvs_sec at 0x28000).`,
`wrote the setup label to …/label.txt (mode 0600); the code is not shown.` and the suggested
`write-flash 0x28000` command. The tool accepts exactly `wifi_ssid` (1–32 bytes), `wifi_pass` (empty
or 8–63 printable), `api_token` (32–512 printable, no spaces) and an optional `setup_code`; it
generates the 16-byte setup code, stores it in `bench-secrets.json` (owner-only) and writes the
label `DSP2:e072a11e1f34:<26 Crockford characters + 2 check characters>`. The BLE image never starts
Wi-Fi (connectivity off), so the placeholders are never used.

## 3. Back up the current storage [READ-ONLY]

Recovery information before anything is written (no credentials are in these regions yet):
```sh
E=~/dispenser-hw-evidence/claim-$(date +%Y%m%d); install -d -m 700 "$E"
python -m esptool --chip esp32c3 -p /dev/ttyACM0 read-flash 0x11000 0x14000 "$E/nvs-before-claim.bin"
python -m esptool --chip esp32c3 -p /dev/ttyACM0 read-flash 0x28000 0x6000  "$E/nvs_sec-before-provisioning.bin"
(cd "$E" && sha256sum *.bin > SHA256SUMS-before.txt && chmod 400 *.bin SHA256SUMS-before.txt)
```
Expected: two files of 81,920 and 24,576 bytes. `nvs-before-claim.bin` holds the UNCLAIMED
storage and allows restoring exactly this state later (section 10).

## 4. Write only `nvs_sec` [WRITE, board]

Press RESET (or use download mode), then:
```sh
python -m esptool --chip esp32c3 -p /dev/ttyACM0 --after no-reset write-flash 0x28000 ~/.config/dispenser-device/bench/nvs_sec.bin
```
Expected: `Flash will be erased from 0x00028000 to 0x0002dfff...`, `Wrote 24576 bytes`,
`Hash of data verified`, and **no** reset (the chip stays in the ROM bootloader, so the firmware
cannot touch the partition before step 5). **STOP** if any other offset appears. Never use
`erase-flash`.

## 5. Verify `nvs_sec` before the firmware runs [READ-ONLY]

```sh
B=~/.config/dispenser-device/bench
python -m esptool --chip esp32c3 -p /dev/ttyACM0 --after hard-reset read-flash 0x28000 0x6000 $B/nvs_sec.readback.bin
cmp $B/nvs_sec.bin $B/nvs_sec.readback.bin && echo "readback identical"
sha256sum $B/nvs_sec.bin $B/nvs_sec.readback.bin      # record the hashes only (in $E/notes.txt)
```
Expected: `readback identical` and two equal hashes; the hard reset at the end boots the firmware.
**STOP** on a mismatch. **Keep** `nvs_sec.bin`, the readback, `bench-secrets.json` and `label.txt`
(owner-only, never in the evidence folder) until the CLAIM and a SESSION have succeeded (section 9).

## 6. Start a service window [READ-ONLY]

Press **RESET**, or start the optional console capture, which resets the board once itself (one RTS
pulse, normal boot) and then only reads; the firmware prints no secrets:
`~/.espressif/tools/python/v6.1/venv/bin/python ~/dispenser-hw-evidence/ble-bringup-20261119/ble_logger.py "$E/serial.log" 900`.
Expected boot: `app: BRING-UP build: a power-on wake opens a BLE service window`, the BT controller
lines, no panic. Do step 7 within 180 s (press RESET again if the window has closed).

## 7. First CLAIM [WRITE, phone and board ownership]

On the S23 in "Dispenser DEBUG":
1. Settings → **Zařízení** → **Připojit zařízení**; allow "Zařízení v okolí" (Nearby devices).
2. **Kód zařízení**: type the text of `label.txt` exactly (`DSP2:e072a11e1f34:` + 28 characters;
   spaces and hyphens are ignored). Show the file only on your own screen (`less …/label.txt`).
3. In the system dialog pick `DSP2` (`E0:72:A1:1E:1F:36`). The address differs from the label's
   device ID; that is expected.
4. **Název telefonu**: e.g. `S23` (at most 24 bytes). Start the pairing.

Expected: the enrollment finishes and the app switches to DEVICE mode.

| App result | Meaning | Action |
|---|---|---|
| DEVICE mode, no error | CLAIM done | step 8 |
| "Odpovědělo jiné zařízení, než je na štítku." (WRONG_DEVICE) | the label's device ID is not this board (HELLO `device_id_hash` check, before any key exists) | **STOP**: check `--device-id` = `e072a11e1f34` |
| "Ověření selhalo – zkontrolujte kód ze štítku." (HANDSHAKE) | PSK mismatch: typo, or label and `nvs_sec` differ | retype once after RESET; then **STOP** and check sections 2–5 |
| "Zařízení už patří jinému telefonu…" (ALREADY_CLAIMED) | the board is already claimed | **STOP** (section 10) |
| "Nepodařilo se připojit…" (CONNECTION) / no device in the dialog | window closed or out of range | RESET, retry |
| "Zařízení neodpovědělo včas…" (TIMEOUT) | a step timed out (e.g. the 30 s limit) | RESET, retry once; then **STOP** |
| "Probíhá jiné spojení se zařízením…" (BUSY) | another connection (e.g. nRF Connect) | close it, RESET, retry |
| "Zařízení spárování odmítlo…" (DEVICE_REFUSED) | refused by the device | **STOP**, keep logs |
| "Neočekávaná odpověď zařízení…" (PROTOCOL) | protocol error | **STOP**, keep logs |
| Unconfirmed / staged pairing reported | the device may have stored the phone, but its answer was lost | RESET, then confirm in the app (IK with the staged key); **do not discard** unless the confirmation reports that nothing was stored |
| Crash, ANR, reset loop on the board | defect | **STOP**, keep logs |

## 8. Ownership and SESSION [READ-ONLY on the board; normal app use]

- nRF Connect (optional): in the 4-byte advertising Service Data the second byte (flags) has bit 0
  (CLAIMED) set; Info byte 7 (`claim_state`) = 1. **Do not write to Request**, and disconnect it
  before using the app (one connection at a time).
- RESET; the device still advertises CLAIMED (ownership is stored in `nvs`).
- RESET; in the app (DEVICE mode) connect/refresh: the status is read over an IK SESSION. Repeat
  once after another RESET.

## 9. Evidence and clean-up

Into `$E` (owner-only): app screenshots of the steps and results, `serial.log`, `notes.txt` with the
hashes from step 5, nRF Connect screenshots of the CLAIMED advertising, optionally
`adb logcat -d --pid=$(adb shell pidof cz.nowass.dispenser.debug)` after checking it contains no
label text. **Never** the label, `bench-secrets.json` or the `nvs_sec` images.

Only after the CLAIM **and** a SESSION have succeeded: `shred -u ~/.config/dispenser-device/bench/nvs_sec.bin ~/.config/dispenser-device/bench/nvs_sec.readback.bin`
(the unencrypted images). Keep `bench-secrets.json` and `label.txt` owner-only until the bench work
is finished: they are needed to CLAIM again after a recovery (section 10).

## 10. Recovery

- **Failure before handshake message 3:** nothing was stored on the device. The phone either holds
  nothing (the key is created only after the HELLO checks) or reports a staged key; retry after
  RESET or confirm the staged key.
- **Lost enrollment result (staged key on the phone):** RESET, confirm in the app. The phone stores
  the key durably **before** sending message 3, so a device that accepted message 3 can always be
  confirmed. Discard only if the confirmation reports NOTHING_STAGED.
- **Board CLAIMED, phone without a credential** (Keystore failure after the CLAIM, app data cleared,
  or "Zapomenout zařízení", which is local only): RECOVERY is **not available on this board** (it
  needs the button driver, milestone M10; `recovery_hold_ms` is never set). Owner-approved bench
  options, each a single-region write, then CLAIM again with the same label (the setup code in
  `nvs_sec` stays valid):
  - restore the pre-CLAIM storage from step 3 (rolls back **all** application storage, including
    ownership epoch and anti-replay state, to that moment: acceptable on the bench only):
    `python -m esptool --chip esp32c3 -p /dev/ttyACM0 write-flash 0x11000 "$E/nvs-before-claim.bin"`, or
  - erase the application storage (re-provisioned UNCLAIMED at the next boot):
    `python -m esptool --chip esp32c3 -p /dev/ttyACM0 erase-region 0x11000 0x14000`.
- **Back to the M0.2 skeleton:** `idf.py -B ~/dispenser-hw-builds/restore-20261119 -D
  SDKCONFIG=$HOME/dispenser-hw-builds/restore-20261119/sdkconfig -p /dev/ttyACM0 flash`.

## What this checklist does not complete

8a R4 also needs ADD (a second phone identity with an invitation), RECOVERY (button driver) and
evidence that two runs used different ephemeral keys and session IDs; R7 (target resources) and R8
(lifecycle) are separate. H3 is partially covered by sections 7 and 8 only.
