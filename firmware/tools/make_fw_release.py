#!/usr/bin/env python3
"""Prepare the two GitHub release assets of a firmware update (docs/FIRMWARE_UPDATE.md 2, 7).

    make_fw_release.py --signed signed.bin --pubkey dispenser-fw-signing.pub.pem \
        --version 0.3.1 --min-installed 0.3.0 --notes "..." --out-dir DIR

Checks before writing anything:
  - the image is an ESP32-C3 application image whose app descriptor says project "dispenser"
    and version == --version (strict X.Y.Z);
  - it fits the OTA slot (0x1C0000);
  - it carries a valid Secure Boot V2 RSA signature by --pubkey (espsecure verify-signature).
Writes DIR/dispenser-firmware-X.Y.Z.bin (a copy) and DIR/dispenser-firmware-X.Y.Z.json.
Never reads a private key; publishing is a separate, manual step.
"""
import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
import sys

SLOT = 0x1C0000
VERSION = re.compile(r"(0|[1-9]\d{0,4})\.(0|[1-9]\d{0,4})\.(0|[1-9]\d{0,4})")
APP_DESC_MAGIC = 0xABCD5432
ESP32C3_CHIP_ID = 5


def fail(msg):
    sys.exit("make_fw_release: " + msg)


def strict_version(v):
    m = VERSION.fullmatch(v)
    if not m or any(int(x) > 65535 for x in m.groups()):
        fail(f"version {v!r} is not X.Y.Z")
    return v


def app_desc(img):
    # esp_image_header_t (24 B) + first segment header (8 B) + esp_app_desc_t
    if len(img) < 0x20 + 256 or img[0] != 0xE9:
        fail("not an ESP application image")
    chip_id = struct.unpack_from("<H", img, 12)[0]
    if chip_id != ESP32C3_CHIP_ID:
        fail(f"chip id {chip_id}, expected ESP32-C3 ({ESP32C3_CHIP_ID})")
    magic, _sv, _r1, _r2 = struct.unpack_from("<IIII", img, 0x20)
    if magic != APP_DESC_MAGIC:
        fail("no esp_app_desc_t in the first segment")
    version = img[0x30:0x50].split(b"\0", 1)[0].decode()
    project = img[0x50:0x70].split(b"\0", 1)[0].decode()
    return project, version


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--signed", required=True)
    ap.add_argument("--pubkey", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--min-installed", required=True)
    ap.add_argument("--notes", default="")
    ap.add_argument("--out-dir", required=True)
    a = ap.parse_args()
    version = strict_version(a.version)
    min_installed = strict_version(a.min_installed)
    img = open(a.signed, "rb").read()
    project, desc_version = app_desc(img)
    if project != "dispenser" or desc_version != version:
        fail(f"image says project {project!r} version {desc_version!r}")
    if len(img) > SLOT:
        fail(f"{len(img)} B does not fit the OTA slot ({SLOT} B)")
    r = subprocess.run([sys.executable, "-m", "espsecure", "verify-signature", "--version", "2",
                        "--keyfile", a.pubkey, a.signed], capture_output=True, text=True)
    if r.returncode != 0 or "verification successful" not in (r.stdout + r.stderr).lower():
        fail("signature verification failed:\n" + r.stdout + r.stderr)
    name = f"dispenser-firmware-{version}.bin"
    os.makedirs(a.out_dir, exist_ok=True)
    out_bin = os.path.join(a.out_dir, name)
    out_json = os.path.join(a.out_dir, f"dispenser-firmware-{version}.json")
    for p in (out_bin, out_json):
        if os.path.exists(p):
            fail(f"{p} exists")
    with open(out_bin, "wb") as f:
        f.write(img)
    manifest = {
        "schema": 1,
        "project": "dispenser",
        "hardware": "xiao-esp32c3",
        "version": version,
        "asset": name,
        "size": len(img),
        "sha256": hashlib.sha256(img).hexdigest(),
        "min_installed": min_installed,
    }
    if a.notes:
        manifest["notes"] = a.notes
    with open(out_json, "w") as f:
        json.dump(manifest, f, indent=2, ensure_ascii=False)
        f.write("\n")
    print(f"{out_bin}\n{out_json}\nsize {len(img)} sha256 {manifest['sha256']}")


if __name__ == "__main__":
    main()
