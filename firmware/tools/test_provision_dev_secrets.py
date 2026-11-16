#!/usr/bin/env python3
"""Tests for provision_dev_secrets.py with DUMMY values only (no real credentials).

    python3 firmware/tools/test_provision_dev_secrets.py

Uses a stub generator by default; if an activated ESP-IDF environment provides the real
nvs_partition_gen, one test also builds a real image from dummy values.
"""

import contextlib
import io
import json
import os
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

    def run_main(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = pds.main(list(argv))
        text = out.getvalue() + err.getvalue()
        for v in DUMMY.values():
            self.assertNotIn(v, text)  # never echoes a value
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
        for k in ("dsp_sec", "wifi_ssid", "api_token"):
            self.assertIn(k.encode(), data)
        self.assertIn(DUMMY["api_token"].encode(), data)  # plaintext: why it is sensitive


if __name__ == "__main__":
    unittest.main(verbosity=2)
