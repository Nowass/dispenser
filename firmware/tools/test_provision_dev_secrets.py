#!/usr/bin/env python3
"""Tests for provision_dev_secrets.py with DUMMY values only (no real credentials).

    python3 firmware/tools/test_provision_dev_secrets.py

Uses a stub generator by default; if an activated ESP-IDF environment provides the real
nvs_partition_gen, one test also builds a real image from dummy values.
"""

import base64
import contextlib
import hashlib
import io
import json
import os
import re
import stat
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))
import provision_dev_secrets as pds  # noqa: E402

DUMMY = {
    "wifi_ssid": "dummy-ssid",
    "wifi_pass": "dummy-passphrase-1",
    "api_token": "dummy-token-" + "0" * 40,
}

STUB = r'''
import sys
# Stub of nvs_partition_gen: "generate <csv> <out> <size>" -> a zero image of <size>.
args = sys.argv[1:]
assert args[0] == "generate"
with open(args[2], "wb") as f:
    f.write(b"\0" * int(args[3], 0))
'''


class ProvisionTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="dsp-prov-test-")
        self.stub = os.path.join(self.dir, "stub_gen.py")
        with open(self.stub, "w") as f:
            f.write(STUB)

    def tearDown(self):
        for name in os.listdir(self.dir):
            os.remove(os.path.join(self.dir, name))
        os.rmdir(self.dir)

    def secrets(self, data=None, mode=0o600, name="dev-secrets.json"):
        path = os.path.join(self.dir, name)
        with open(path, "w") as f:
            json.dump(DUMMY if data is None else data, f)
        os.chmod(path, mode)
        return path

    def read_code(self, path):
        with open(path) as f:
            return json.load(f)[pds.SETUP_KEY]

    def run_main(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = pds.main(list(argv))
        text = out.getvalue() + err.getvalue()
        for v in DUMMY.values():
            self.assertNotIn(v, text)  # never echoes a value
        # nor the setup code, in any spelling (hex, label characters)
        for name in os.listdir(self.dir):
            path = os.path.join(self.dir, name)
            if name.endswith(".json") and os.path.isfile(path):
                try:
                    with open(path) as f:
                        sc = json.load(f).get(pds.SETUP_KEY)
                except (ValueError, AttributeError):
                    sc = None
                if isinstance(sc, str) and re.fullmatch(r"[0-9a-f]{32}", sc):
                    self.assertNotIn(sc, text.lower())
                    self.assertNotIn(pds.setup_label_code(bytes.fromhex(sc)), text)
        return rc, text

    def test_success_with_stub(self):
        s = self.secrets()
        out = os.path.join(self.dir, "nvs_sec.bin")
        rc, text = self.run_main("--secrets", s, "--out", out, "--generator", self.stub)
        self.assertEqual(rc, 0, text)
        self.assertEqual(os.path.getsize(out), 0x6000)
        self.assertEqual(stat.S_IMODE(os.stat(out).st_mode), 0o600)
        self.assertIn("write-flash 0x28000", text)
        # Existing image: refused without --force, replaced with it.
        rc, text = self.run_main("--secrets", s, "--out", out, "--generator", self.stub)
        self.assertEqual(rc, 2)
        rc, _ = self.run_main("--secrets", s, "--out", out, "--force", "--generator", self.stub)
        self.assertEqual(rc, 0)
        # No temporary CSV is left behind.
        leftovers = [d for d in os.listdir(tempfile.gettempdir()) if d.startswith("dsp-nvs-sec-")]
        self.assertEqual(leftovers, [])

    def test_refuses_insecure_or_invalid_input(self):
        out = os.path.join(self.dir, "x.bin")
        cases = [
            self.secrets(mode=0o644, name="a.json"),  # readable by others
            self.secrets(mode=0o640, name="b.json"),  # readable by group
            self.secrets({**DUMMY, "extra": "x"}, name="c.json"),
            self.secrets({k: v for k, v in DUMMY.items() if k != "api_token"}, name="d.json"),
            self.secrets({**DUMMY, "api_token": "short"}, name="e.json"),
            self.secrets({**DUMMY, "api_token": "has space " + "0" * 40}, name="f.json"),
            self.secrets({**DUMMY, "api_token": "x" * 513}, name="g.json"),
            self.secrets({**DUMMY, "wifi_ssid": ""}, name="h.json"),
            self.secrets({**DUMMY, "wifi_ssid": "s" * 33}, name="i.json"),
            self.secrets({**DUMMY, "wifi_pass": "short"}, name="j.json"),
            self.secrets({**DUMMY, "wifi_pass": "p" * 64}, name="k.json"),
            self.secrets({**DUMMY, "wifi_pass": 12345678}, name="l.json"),
        ]
        for path in cases:
            rc, text = self.run_main("--secrets", path, "--out", out, "--generator", self.stub)
            self.assertEqual(rc, 2, path)
            self.assertFalse(os.path.exists(out), path)
        broken = os.path.join(self.dir, "broken.json")
        with open(broken, "w") as f:
            f.write('{"api_token": "' + DUMMY["api_token"] + '",')  # truncated JSON
        os.chmod(broken, 0o600)
        rc, text = self.run_main("--secrets", broken, "--out", out, "--generator", self.stub)
        self.assertEqual(rc, 2)
        rc, _ = self.run_main("--secrets", os.path.join(self.dir, "none.json"), "--out", out,
                              "--generator", self.stub)
        self.assertEqual(rc, 2)

    def test_open_network_allowed(self):
        s = self.secrets({**DUMMY, "wifi_pass": ""})
        out = os.path.join(self.dir, "open.bin")
        rc, text = self.run_main("--secrets", s, "--out", out, "--generator", self.stub)
        self.assertEqual(rc, 0, text)

    def test_refuses_paths_inside_repository(self):
        s = self.secrets()
        inside = os.path.join(pds.FIRMWARE, "nvs_sec_test_should_not_exist.bin")
        rc, _ = self.run_main("--secrets", s, "--out", inside, "--generator", self.stub)
        self.assertEqual(rc, 2)
        self.assertFalse(os.path.exists(inside))
        rc, _ = self.run_main("--secrets", os.path.join(pds.FIRMWARE, "partitions.csv"), "--out",
                              os.path.join(self.dir, "y.bin"), "--generator", self.stub)
        self.assertEqual(rc, 2)
        rc, _ = self.run_main("--secrets", s, "--out", os.path.join(self.dir, "img.txt"),
                              "--generator", self.stub)
        self.assertEqual(rc, 2)

    def test_generator_failure_reports_without_output(self):
        bad = os.path.join(self.dir, "bad_gen.py")
        with open(bad, "w") as f:
            f.write("import sys\nprint(open(sys.argv[2]).read())\nsys.exit(3)\n")  # would leak
        s = self.secrets()
        out = os.path.join(self.dir, "z.bin")
        rc, text = self.run_main("--secrets", s, "--out", out, "--generator", bad)
        self.assertEqual(rc, 2)
        self.assertIn("exit code 3", text)
        self.assertFalse(os.path.exists(out))

    # ---- M1.10b.3: setup code ---------------------------------------------------------------

    def test_setup_code_generated_kept_and_rotated(self):
        s = self.secrets()
        out = os.path.join(self.dir, "sc.bin")
        rc, text = self.run_main("--secrets", s, "--out", out, "--generator", self.stub)
        self.assertEqual(rc, 0, text)
        with open(s) as f:
            data = json.load(f)
        sc = data[pds.SETUP_KEY]
        self.assertRegex(sc, r"^[0-9a-f]{32}$")
        self.assertEqual(stat.S_IMODE(os.stat(s).st_mode), 0o600)  # still owner-only
        self.assertEqual({k: data[k] for k in DUMMY}, DUMMY)       # the rest untouched
        # kept on the next run
        rc, _ = self.run_main("--secrets", s, "--out", out, "--force", "--generator", self.stub)
        self.assertEqual(rc, 0)
        self.assertEqual(self.read_code(s), sc)
        # replaced only on request
        rc, _ = self.run_main("--secrets", s, "--out", out, "--force", "--new-setup-code",
                              "--generator", self.stub)
        self.assertEqual(rc, 0)
        self.assertNotEqual(self.read_code(s), sc)
        leftovers = [n for n in os.listdir(self.dir) if n.startswith(".dsp-secrets-")]
        self.assertEqual(leftovers, [])

    def test_setup_code_reaches_the_csv_as_a_blob(self):
        check = os.path.join(self.dir, "check_gen.py")
        with open(check, "w") as f:
            f.write(STUB + r"""
import csv, re
rows = list(csv.reader(open(args[1])))
ok = [r for r in rows if r[0] == "setup_code"]
assert len(ok) == 1 and ok[0][1:3] == ["data", "hex2bin"] and re.fullmatch("[0-9a-f]{32}", ok[0][3])
""")
        s = self.secrets()
        rc, text = self.run_main("--secrets", s, "--out", os.path.join(self.dir, "b.bin"),
                                 "--generator", check)
        self.assertEqual(rc, 0, text)

    def test_setup_code_validation(self):
        out = os.path.join(self.dir, "v.bin")
        for i, bad in enumerate(["00" * 16, "AB" * 16, "ab" * 15, "zz" * 16, 12345, ""]):
            s = self.secrets({**DUMMY, pds.SETUP_KEY: bad}, name=f"v{i}.json")
            rc, _ = self.run_main("--secrets", s, "--out", out, "--generator", self.stub)
            self.assertEqual(rc, 2, bad)
            self.assertFalse(os.path.exists(out))

    def test_setup_label_matches_shared_vectors(self):
        """The label the tool writes is the one the Android parser accepts: every label_ok record of
        test-vectors/qr_formats_v1.txt (also run by the app's QrFormatsVectorTest)."""
        path = os.path.join(pds.REPO, "test-vectors", "qr_formats_v1.txt")
        records = 0
        with open(path, encoding="ascii") as f:
            for line in f:
                if not line.startswith("label_ok "):
                    continue
                fields = dict(kv.split("=", 1) for kv in line.split()[1:])
                code = bytes.fromhex(fields["code"][2:])  # "x:<hex>"
                self.assertEqual(pds.setup_label(fields["device_id"], code), fields["label"], fields["name"])
                records += 1
        self.assertGreaterEqual(records, 4)

    def test_setup_label(self):
        code = bytes(range(16))
        # independent reference: RFC 4648 base32 mapped onto the Crockford alphabet
        rfc = base64.b32encode(code).decode().rstrip("=")
        ref = "".join(pds.CROCKFORD["ABCDEFGHIJKLMNOPQRSTUVWXYZ234567".index(c)] for c in rfc)
        chk = int.from_bytes(hashlib.sha256(b"DSP2 setup" + code).digest()[:2], "big") >> 6
        ref += pds.CROCKFORD[chk >> 5] + pds.CROCKFORD[chk & 31]
        self.assertEqual(len(ref), 28)
        self.assertEqual(pds.setup_label_code(code), ref)
        self.assertEqual(pds.setup_label("a0b1c2d3e4f5", code), "DSP2:a0b1c2d3e4f5:" + ref)
        # one changed bit changes the body or the check characters
        other = bytes([1]) + code[1:]
        self.assertNotEqual(pds.setup_label_code(other), ref)
        # the label file: owner-only, outside the repository, with the device ID
        s = self.secrets({**DUMMY, pds.SETUP_KEY: code.hex()})
        out = os.path.join(self.dir, "l.bin")
        label = os.path.join(self.dir, "label.txt")
        rc, text = self.run_main("--secrets", s, "--out", out, "--generator", self.stub,
                                 "--label-out", label, "--device-id", "A0B1C2D3E4F5")
        self.assertEqual(rc, 0, text)
        self.assertEqual(open(label).read(), "DSP2:a0b1c2d3e4f5:" + ref + "\n")
        self.assertEqual(stat.S_IMODE(os.stat(label).st_mode), 0o600)
        self.assertIn("the code is not shown", text)
        # refused: existing label without --force, missing device ID, bad ID, inside the repo
        for i, extra in enumerate((["--label-out", label, "--device-id", "a0b1c2d3e4f5"],
                                   ["--label-out", os.path.join(self.dir, "l2.txt")],
                                   ["--label-out", os.path.join(self.dir, "l3.txt"), "--device-id", "xyz"],
                                   ["--label-out", os.path.join(pds.FIRMWARE, "label_should_not_exist.txt"),
                                    "--device-id", "a0b1c2d3e4f5"])):
            fresh_out = os.path.join(self.dir, f"r{i}.bin")
            rc, _ = self.run_main("--secrets", s, "--out", fresh_out, "--generator", self.stub, *extra)
            self.assertEqual(rc, 2, extra)
            self.assertFalse(os.path.exists(fresh_out), extra)  # refused before anything is written
        self.assertFalse(os.path.exists(os.path.join(pds.FIRMWARE, "label_should_not_exist.txt")))

    def test_partition_geometry_matches_firmware(self):
        self.assertEqual(pds.partition_geometry(), (0x28000, 0x6000))

    @unittest.skipUnless(os.environ.get("IDF_PATH"), "needs an activated ESP-IDF environment")
    def test_real_generator_with_dummy_values(self):
        s = self.secrets()
        out = os.path.join(self.dir, "real.bin")
        rc, text = self.run_main("--secrets", s, "--out", out)
        self.assertEqual(rc, 0, text)
        data = open(out, "rb").read()
        self.assertEqual(len(data), 0x6000)
        for k in ("dsp_sec", "wifi_ssid", "api_token", "setup_code"):
            self.assertIn(k.encode(), data)
        sc = bytes.fromhex(self.read_code(s))
        self.assertIn(sc, data)  # the 16-byte blob, plaintext (nvs_sec is not encrypted)
        self.assertIn(DUMMY["api_token"].encode(), data)  # plaintext: why it is sensitive


if __name__ == "__main__":
    unittest.main(verbosity=2)
