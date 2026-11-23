# Firmware update over the air (OTA)

Minimal, owner-initiated update of the ESP32-C3 firmware from the Android application. GitHub
Releases of `Nowass/dispenser` are the only source of firmware updates. BLE messages: 
`docs/BLE_PROTOCOL.md` 7.8, 8.3, 10.8.

## 1. Flow

1. The app reads `GET_UPDATE_STATUS` (firmware ≥ 0.3.0): running version, `capable`, last result.
2. On the owner's request the app lists the GitHub releases (the existing unauthenticated
   release check) and selects the newest compatible firmware release (section 3).
3. It shows the version and the release notes; the owner confirms explicitly.
4. The app sends `START_UPDATE(version, tag, size, sha256)` in the authenticated session.
5. The device checks the request and the conditions (verdict), answers, ends the session and,
   with the BLE radio down, downloads the asset over verified HTTPS into the inactive OTA slot.
6. Before activation: project `dispenser`, chip, version, size, SHA-256 and the RSA-3072
   application signature (key of the running firmware). Then reboot.
7. The new firmware confirms itself at the end of its first complete wake; otherwise the
   bootloader returns to the previous firmware.
8. After the next connection the app shows the result (`last_result`, `running_version`).

## 2. Release format

A firmware release is any **published** GitHub release (never a draft) that carries both assets:

| Asset | Content |
|---|---|
| `dispenser-firmware-X.Y.Z.bin` | the signed application image (Secure Boot V2 RSA signature block appended) |
| `dispenser-firmware-X.Y.Z.json` | the manifest below |

```json
{
  "schema": 1,
  "project": "dispenser",
  "hardware": "xiao-esp32c3",
  "version": "0.3.1",
  "asset": "dispenser-firmware-0.3.1.bin",
  "size": 1430528,
  "sha256": "<64 lowercase hex of the .bin file>",
  "min_installed": "0.3.0",
  "notes": "Short Czech description for the app (optional; the release body is used otherwise)."
}
```

- Firmware versions are plain `X.Y.Z` and independent of the APK version. A release may carry an
  APK, firmware, or both. A firmware-only release uses a tag that is not a semantic version
  (`firmware-v0.3.1`), so the APK update check never treats it as an app release.
- Beta firmware = a GitHub **pre-release**; offered only when the owner enabled the beta firmware
  channel in the app.

## 3. App selection rules

- Releases: the existing `GET /repos/Nowass/dispenser/releases`; drafts ignored; pre-releases only
  with the beta firmware channel.
- A release qualifies only with both assets of the same version and a manifest that parses with
  `schema` 1, `project` `dispenser`, `hardware` `xiao-esp32c3`, `version` = the asset names'
  version, `asset` = the `.bin` name, `size` = the `.bin` asset size, `sha256` 64 lowercase hex and,
  when GitHub reports an asset `digest`, `digest` = `sha256:<sha256>`.
- The manifest is downloaded only from `https://github.com/Nowass/dispenser/releases/download/…`
  (redirects to GitHub's asset host are followed).
- Offered: the highest version newer than `running_version` whose `min_installed` ≤
  `running_version`. Tag and asset must match `[A-Za-z0-9._-]`.
- The manifest is **not** a proof of origin. Origin is proven on the device by the signature;
  the SHA-256 binds the confirmed asset to what is installed.

## 4. Device rules

- No URL from the phone: `https://github.com/Nowass/dispenser/releases/download/<tag>/dispenser-firmware-<version>.bin`
  (base compiled in, `CONFIG_DSP_OTA_RELEASE_BASE`).
- Refused while: a dose is due within 15 min or the clock is untrusted; battery not VALID ≥
  3,800 mV in this wake or a BATTERY_LOW episode is open (the ADC is uncalibrated: on the bench it
  read 0.10 V below a multimeter, so the threshold is conservative); no Wi-Fi credentials; degraded,
  inhibited or an update already pending; version not newer.
- TLS: certificate bundle, no insecure options, `https` only, redirects followed with the same
  verification. Timeout 4 min for the whole download.
- Signature: `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` + `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`
  (RSA-3072, Secure Boot V2 format). No eFuse is written; the trusted key is the one in the running
  firmware's signature block, so the running firmware must itself be signed. **Limit:** someone
  with physical (USB) access can still flash anything; this protects the network path.
- Rollback: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` (bootloader). The new image stays
  PENDING_VERIFY until the first complete wake ends (loaded, not degraded, pump confirmed off);
  a reset, crash or failed check before that boots the previous slot. No anti-rollback eFuse.
- Preserved: NVS, `nvs_sec`, ownership, pairing, schedules and the notification queue (only the
  inactive app slot and `otadata` are written).

## 5. Signing key

- RSA-3072 PEM, created once by the owner, kept **outside** the repository:
  `~/.config/dispenser-device/signing/dispenser-fw-signing.pem` (0600) plus an offline backup.
  Losing it means the next update needs USB again; leaking it lets others sign updates.
- Builds are unsigned (`CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n`); signing is a separate step:
  `espsecure sign-data --version 2 --keyfile <key> --output signed.bin dispenser.bin`, then
  `espsecure signature-info-v2 signed.bin` to check.

## 6. One-time USB bootstrap (0.3.0)

The deployed bootloader (db7e593) has no rollback support, so the bootstrap writes the new
bootloader **and** the signed 0.3.0 application:

- 0x0 bootloader (rollback enabled), 0x30000 signed app (`ota_0`, currently running slot).
- Partition table, `otadata` (ota_0 VALID), NVS and `nvs_sec` are not written and are compared
  before and after. Recovery is always possible over USB (ROM download mode).

## 7. Publishing procedure (owner approval required)

1. Set `firmware/version.txt` to `X.Y.Z`, build the pilot production image, sign it.
2. `tools/make_fw_release.py --signed signed.bin --version X.Y.Z --min-installed 0.3.0 --notes …`
   writes `dispenser-firmware-X.Y.Z.bin` and `.json` (size and SHA-256 computed, signature and app
   descriptor checked).
3. Create the GitHub release (stable or pre-release), attach both files, publish.
4. In the app: Settings → Firmware zařízení → Zkontrolovat aktualizace → Aktualizovat.

A firmware-only release uses a tag like `firmware-v0.3.1`; test or first releases go out as
pre-releases (beta firmware channel) and are promoted to stable after a successful update.

## 8. Short verification procedure (first OTA test; owner approvals, motor disconnected)

1. **Bootstrap (USB):** flash bootloader + signed 0.3.0 (section 6). Boot log: `App version 0.3.0`,
   no `dsp_ota` error. App: Firmware screen shows 0.3.0, update possible; schedules, threshold,
   pairing and battery unchanged.
2. **Publish 0.3.1** (pre-release `firmware-v0.3.1`, both assets). App: enable the beta firmware
   channel, check: 0.3.1 with its notes is offered.
3. **Update:** Aktualizovat → confirm → verdict "accepted"; the device disconnects. Optional USB
   log: Wi-Fi, `update 0.3.1 verified and activated: restart`, `App version 0.3.1`,
   `boot of 0.3.1 confirmed`.
4. **After 2–3 min:** wake button, connect: running 0.3.1, last result INSTALLED 0.3.1; data intact.
5. **Simulated failure (rollback):** publish pre-release `firmware-v0.3.2-rollbacktest` with the
   TEST image 0.3.2 (`sdkconfig.otatest.defaults`), update as above. The 0.3.2 image boots, fails its
   boot check on purpose and the bootloader returns to 0.3.1. Reconnect: running 0.3.1, last result
   ROLLED_BACK 0.3.2. Delete the test pre-release afterwards.
6. **Refusal check (optional, no flash write):** with a dose due within 15 min the verdict is
   "dose soon" and nothing is downloaded.
