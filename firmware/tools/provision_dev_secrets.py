#!/usr/bin/env python3
"""Developer-only credential provisioning for Dispenser V2 (milestone M1.7, approved D5).

Builds the NVS image of the dedicated "nvs_sec" partition (Wi-Fi credentials and
DEVICE_API_TOKEN) from an owner-readable JSON file OUTSIDE the repository, using ESP-IDF's
nvs_partition_gen. You run it yourself, inside an activated ESP-IDF environment:

    python firmware/tools/provision_dev_secrets.py \\
        --secrets ~/.config/dispenser-device/dev-secrets.json \\
        --out ~/.config/dispenser-device/nvs_sec.bin

secrets JSON (mode 0600):  {"wifi_ssid": "...", "wifi_pass": "...", "api_token": "..."}
("wifi_pass" may be "" for an open network.)

Setup code (M1.10b.3, docs/OWNERSHIP_DESIGN.md section 9): the device's 128-bit ownership trust
anchor. If the secrets JSON has no "setup_code" (32 hex digits), the tool generates one with
secrets.token_bytes(16) and writes it back to the same owner-only file (atomic replace, mode
0600), so later rebuilds keep the label valid; --new-setup-code replaces it. It is stored in
nvs_sec as the 16-byte blob "setup_code". --label-out FILE --device-id HEX12 writes the label
text "DSP2:<device-id>:<code>" (26 Crockford base32 characters + 2 check characters = the first
10 bits of SHA-256("DSP2 setup" || code)) to a new owner-only file outside the repository, for
printing the QR code. The code is never printed.

Guarantees:
  - never prints, logs or echoes a value; the generator's own output is captured, not shown;
  - refuses input or output paths inside the repository, and a secrets file readable by
    group/others;
  - the temporary CSV lives in a private temporary directory outside the repository and is
    overwritten and deleted afterwards; the image is written with mode 0600;
  - offset and size come from firmware/partitions.csv (partition "nvs_sec").

The image is NOT encrypted: treat it like the secrets file and delete it after flashing.
Production provisioning (authenticated BLE, per-device credentials) is a future milestone.
"""

import argparse
import csv
import hashlib
import json
import os
import re
import secrets as secrets_rng
import stat
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.realpath(__file__))
FIRMWARE = os.path.dirname(HERE)
REPO = os.path.dirname(FIRMWARE)
PARTITIONS = os.path.join(FIRMWARE, "partitions.csv")

PARTITION = "nvs_sec"
NAMESPACE = "dsp_sec"  # DSP_NVS_SEC_NAMESPACE
KEYS = ("wifi_ssid", "wifi_pass", "api_token")  # DSP_NVS_SEC_KEY_*
SETUP_KEY = "setup_code"  # DSP_NVS_SEC_KEY_SETUP_CODE (optional in the JSON; generated)
SETUP_BYTES = 16  # DSP_SETUP_CODE_BYTES
TOKEN_MIN, TOKEN_MAX = 32, 512  # DSP_API_TOKEN_MIN_LEN / _MAX_LEN
CROCKFORD = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"


class Refused(Exception):
    """A check failed; the message never contains a secret value."""


def inside_repo(path):
    real = os.path.realpath(path)
    return real == REPO or real.startswith(REPO + os.sep)


def partition_geometry(path=PARTITIONS):
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            cols = [c.strip() for c in line.split(",")]
            if cols[0] == PARTITION:
                offset, size = int(cols[3], 0), int(cols[4], 0)
                if cols[1] != "data" or cols[2] != "nvs" or size % 4096 or size < 0x3000:
                    raise Refused("partitions.csv: unexpected nvs_sec definition")
                return offset, size
    raise Refused("partitions.csv: no nvs_sec partition")


def printable(s, lo, hi):
    return all(lo <= ord(ch) <= hi for ch in s)


def validate(secrets):
    if not isinstance(secrets, dict) or not (set(KEYS) <= set(secrets) <= set(KEYS) | {SETUP_KEY}):
        raise Refused("secrets JSON must contain exactly the keys: " + ", ".join(KEYS) +
                      " (and optionally " + SETUP_KEY + ")")
    if SETUP_KEY in secrets:
        sc = secrets[SETUP_KEY]
        if not isinstance(sc, str) or not re.fullmatch(r"[0-9a-f]{32}", sc) or sc == "0" * 32:
            raise Refused(SETUP_KEY + ": 32 lowercase hex digits (16 random bytes) required")
    for k in KEYS:
        if not isinstance(secrets[k], str):
            raise Refused(f"{k}: must be a string")
    ssid = secrets["wifi_ssid"].encode("utf-8")
    if not 1 <= len(ssid) <= 32 or b"\0" in ssid:
        raise Refused("wifi_ssid: 1..32 bytes required")
    pw = secrets["wifi_pass"]
    if pw and (not 8 <= len(pw) <= 63 or not printable(pw, 0x20, 0x7E)):
        raise Refused("wifi_pass: empty (open network) or 8..63 printable ASCII characters")
    tok = secrets["api_token"]
    if not TOKEN_MIN <= len(tok) <= TOKEN_MAX or not printable(tok, 0x21, 0x7E):
        raise Refused(f"api_token: {TOKEN_MIN}..{TOKEN_MAX} printable ASCII characters, no spaces")


def load_secrets(path):
    if inside_repo(path):
        raise Refused("the secrets file must be outside the repository")
    try:
        st = os.stat(path)
    except OSError:
        raise Refused("secrets file not found") from None
    if not stat.S_ISREG(st.st_mode):
        raise Refused("secrets file is not a regular file")
    if st.st_mode & 0o077:
        raise Refused("secrets file must be owner-only (chmod 600)")
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError):
        raise Refused("secrets file is not valid JSON") from None  # no parser echo
    validate(data)
    return data


def setup_label_code(code):
    """26 Crockford base32 characters (128 bits, MSB first, 2 zero bits) + 2 check characters."""
    if len(code) != SETUP_BYTES:
        raise Refused("setup code must be 16 bytes")
    n = int.from_bytes(code, "big") << 2  # 130 bits
    body = "".join(CROCKFORD[(n >> (5 * (25 - i))) & 31] for i in range(26))
    check = int.from_bytes(hashlib.sha256(b"DSP2 setup" + code).digest()[:2], "big") >> 6  # 10 bits
    return body + CROCKFORD[check >> 5] + CROCKFORD[check & 31]


def setup_label(device_id, code):
    return "DSP2:" + device_id + ":" + setup_label_code(code)


def write_back_secrets(path, data):
    """Atomically replaces the owner-only secrets file (the value is never printed)."""
    d = os.path.dirname(os.path.abspath(path))
    old = os.umask(0o077)
    try:
        fd, tmp = tempfile.mkstemp(prefix=".dsp-secrets-", dir=d)
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                json.dump(data, f, indent=2)
                f.write("\n")
                f.flush()
                os.fsync(f.fileno())
            os.chmod(tmp, 0o600)
            os.replace(tmp, path)
        except BaseException:
            wipe_and_remove(tmp)
            raise
    finally:
        os.umask(old)


def write_label(path, text, force):
    path = os.path.abspath(path)
    if inside_repo(path):
        raise Refused("the label file must be outside the repository")
    if not os.path.isdir(os.path.dirname(path)):
        raise Refused("the label directory does not exist")
    if os.path.lexists(path):
        if not force:
            raise Refused("the label file exists (use --force to replace it)")
        wipe_and_remove(path)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w", encoding="ascii") as f:
        f.write(text + "\n")
    os.chmod(path, 0o600)


def default_generator():
    idf = os.environ.get("IDF_PATH")
    if not idf:
        raise Refused("IDF_PATH is not set: activate the ESP-IDF environment first")
    gen = os.path.join(idf, "components", "nvs_flash", "nvs_partition_generator", "nvs_partition_gen.py")
    if not os.path.isfile(gen):
        raise Refused("nvs_partition_gen.py not found in IDF_PATH")
    return gen


def wipe_and_remove(path):
    try:
        size = os.path.getsize(path)
        with open(path, "r+b") as f:
            f.write(b"\0" * size)
            f.flush()
            os.fsync(f.fileno())
    except OSError:
        pass
    try:
        os.remove(path)
    except OSError:
        pass


def provision(secrets_path, out_path, force=False, generator=None, new_setup_code=False,
              label_out=None, device_id=None):
    secrets = load_secrets(secrets_path)
    if (label_out is None) != (device_id is None):
        raise Refused("--label-out and --device-id must be given together")
    if device_id is not None:
        device_id = device_id.lower()
        if not re.fullmatch(r"[0-9a-f]{12}", device_id):
            raise Refused("--device-id: 12 hex digits (the factory MAC, e.g. from esptool read-mac)")
    out_path = os.path.abspath(out_path)
    if inside_repo(out_path):
        raise Refused("the output image must be outside the repository")
    if not out_path.endswith(".bin"):
        raise Refused("the output image must end in .bin")
    if not os.path.isdir(os.path.dirname(out_path)):
        raise Refused("the output directory does not exist")
    if os.path.lexists(out_path):
        if not force:
            raise Refused("the output image exists (use --force to replace it)")
        os.remove(out_path)
    if label_out is not None:
        lp = os.path.abspath(label_out)
        if inside_repo(lp):
            raise Refused("the label file must be outside the repository")
        if os.path.lexists(lp) and not force:
            raise Refused("the label file exists (use --force to replace it)")
    offset, size = partition_geometry()
    gen = generator or default_generator()
    # Setup code: generated (never printed) when absent or on request; kept in the secrets file.
    if new_setup_code or SETUP_KEY not in secrets:
        code = secrets_rng.token_bytes(SETUP_BYTES)
        while code == bytes(SETUP_BYTES):
            code = secrets_rng.token_bytes(SETUP_BYTES)
        secrets[SETUP_KEY] = code.hex()
        write_back_secrets(secrets_path, secrets)
    setup_hex = secrets[SETUP_KEY]

    old_umask = os.umask(0o077)
    tmpdir = tempfile.mkdtemp(prefix="dsp-nvs-sec-")  # mode 0700
    csv_path = os.path.join(tmpdir, "nvs_sec.csv")
    try:
        if inside_repo(tmpdir):
            raise Refused("temporary directory inside the repository")
        fd = os.open(csv_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            w.writerow(["key", "type", "encoding", "value"])
            w.writerow([NAMESPACE, "namespace", "", ""])
            for k in KEYS:
                if k == "wifi_pass" and secrets[k] == "":
                    continue  # absent key = open network
                w.writerow([k, "data", "string", secrets[k]])
            w.writerow([SETUP_KEY, "data", "hex2bin", setup_hex])  # 16-byte blob
        proc = subprocess.run(
            [sys.executable, gen, "generate", csv_path, out_path, hex(size)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        if proc.returncode != 0 or not os.path.isfile(out_path):
            if os.path.lexists(out_path):
                wipe_and_remove(out_path)
            raise Refused(f"nvs_partition_gen failed (exit code {proc.returncode}); output suppressed")
        os.chmod(out_path, 0o600)
        if os.path.getsize(out_path) != size:
            wipe_and_remove(out_path)
            raise Refused("generated image has an unexpected size")
    finally:
        wipe_and_remove(csv_path)
        try:
            os.rmdir(tmpdir)
        except OSError:
            pass
        os.umask(old_umask)
    if label_out is not None:
        write_label(label_out, setup_label(device_id, bytes.fromhex(setup_hex)), force)
    return offset, size


def main(argv=None):
    p = argparse.ArgumentParser(description="Build the nvs_sec image (developer use only).")
    p.add_argument("--secrets", required=True, help="owner-only JSON file outside the repository")
    p.add_argument("--out", required=True, help="output .bin outside the repository")
    p.add_argument("--force", action="store_true", help="replace an existing output image/label")
    p.add_argument("--new-setup-code", action="store_true",
                   help="generate a new setup code (the old label becomes invalid)")
    p.add_argument("--label-out", help="write the setup label text to this new file (outside the repo)")
    p.add_argument("--device-id", help="12 hex digits: the device ID printed on the label")
    p.add_argument("--generator", help=argparse.SUPPRESS)  # tests only
    a = p.parse_args(argv)
    try:
        offset, size = provision(a.secrets, a.out, a.force, a.generator, a.new_setup_code,
                                 a.label_out, a.device_id)
    except Refused as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    out = os.path.abspath(a.out)
    print(f"wrote {out} (mode 0600, {size} bytes, partition {PARTITION} at {offset:#x}).")
    if a.label_out:
        print(f"wrote the setup label to {os.path.abspath(a.label_out)} (mode 0600); the code is not shown.")
    print("The image is NOT encrypted. Flash it, then delete it:")
    print(f"  python -m esptool --chip esp32c3 write-flash {offset:#x} {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
