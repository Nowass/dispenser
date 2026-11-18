#!/usr/bin/env python3
"""Tests for rng_capture.py with synthetic console logs (no hardware, no serial port).

    python3 firmware/tools/test_rng_capture.py
"""

import base64
import contextlib
import io
import os
import random
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))
import rng_capture as rc  # noqa: E402

BLOCK = 4096


def frames(source, data, block=BLOCK):
    """The device's framing (main.c rng_gate_stream) for one source."""
    lines = ["RNGBEGIN source=%s bytes=%d block=%d" % (source, len(data), block)]
    crc_all = 0
    idx = 0
    for off in range(0, len(data), block):
        b = data[off:off + block]
        crc_all = zlib.crc32(b, crc_all) & 0xFFFFFFFF
        lines.append("RNG %s %d %08x %s" % (source, idx, zlib.crc32(b) & 0xFFFFFFFF,
                                            base64.b64encode(b).decode()))
        idx += 1
    lines.append("RNGEND source=%s blocks=%d bytes=%d crc32=%08x" % (source, idx, len(data), crc_all))
    return lines


def sample(n, seed):
    r = random.Random(seed)  # synthetic test data only
    return bytes(r.getrandbits(8) for _ in range(n))


def valid_log(n=3 * BLOCK + 100):
    a = sample(n, 1)
    b = sample(n, 2)
    lines = ["ESP-ROM:esp32c3-api1-20210207", "I (300) dispenser: RNGGATE R1 register=1",
             "\x1b[0;32mI (310) dispenser: RNGGATE R3pre controller_status=0 ready=0 bytes=0\x1b[0m"]
    lines += frames("esp_fill_random", a)
    lines += ["I (900) BLE_INIT: unrelated log line between streams"]
    lines += frames("randombytes_buf", b)
    lines += ["I (999) dispenser: RNGGATE SUMMARY R1=PASS R2=PASS R3pre=PASS R3post=PASS R5=STREAMED R6=PASS"]
    return lines, a, b


class CaptureTest(unittest.TestCase):
    def run_tool(self, lines):
        with tempfile.TemporaryDirectory() as d:
            log = os.path.join(d, "console.log")
            with open(log, "w", encoding="ascii") as f:
                f.write("\r\n".join(lines) + "\r\n")
            out_dir = os.path.join(d, "out")
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rcode = rc.main(["--input", log, "--out-dir", out_dir, "--prefix", "t"])
            files = {}
            if os.path.isdir(out_dir):
                for name in os.listdir(out_dir):
                    with open(os.path.join(out_dir, name), "rb") as f:
                        files[name] = f.read()
            return rcode, buf.getvalue(), files

    def test_valid(self):
        lines, a, b = valid_log()
        rcode, out, files = self.run_tool(lines)
        self.assertEqual(rcode, 0, out)
        self.assertEqual(files["t_esp_fill_random.bin"], a)
        self.assertEqual(files["t_randombytes_buf.bin"], b)
        self.assertIn("CRC failures: 0, framing/CRC errors: 0", out)
        self.assertIn("device: RNGGATE SUMMARY", out)
        self.assertIn("NOT evidence of entropy quality", out)

    def test_exact_multiple_of_block(self):
        a = sample(2 * BLOCK, 3)
        rcode, out, files = self.run_tool(frames("esp_fill_random", a))
        self.assertEqual(rcode, 0, out)
        self.assertEqual(files["t_esp_fill_random.bin"], a)

    def test_corrupted_crc(self):
        lines, _, _ = valid_log()
        i = next(k for k, l in enumerate(lines) if l.startswith("RNG esp_fill_random 1 "))
        parts = lines[i].split(" ")
        parts[3] = "%08x" % (int(parts[3], 16) ^ 1)
        lines[i] = " ".join(parts)
        rcode, out, files = self.run_tool(lines)
        self.assertNotEqual(rcode, 0)
        self.assertIn("block 1: CRC mismatch", out)
        self.assertEqual(files, {})

    def test_corrupted_payload(self):
        lines, _, _ = valid_log()
        i = next(k for k, l in enumerate(lines) if l.startswith("RNG randombytes_buf 0 "))
        head, b64 = lines[i].rsplit(" ", 1)
        flipped = "B" if b64[10] == "A" else "A"
        lines[i] = head + " " + b64[:10] + flipped + b64[11:]
        rcode, out, _ = self.run_tool(lines)
        self.assertNotEqual(rcode, 0)
        self.assertIn("CRC mismatch", out)

    def test_missing_block(self):
        lines, _, _ = valid_log()
        i = next(k for k, l in enumerate(lines) if l.startswith("RNG esp_fill_random 2 "))
        del lines[i]
        rcode, out, files = self.run_tool(lines)
        self.assertNotEqual(rcode, 0)
        self.assertIn("out of order", out)
        self.assertEqual(files, {})

    def test_reordered_blocks(self):
        lines, _, _ = valid_log()
        i = next(k for k, l in enumerate(lines) if l.startswith("RNG esp_fill_random 1 "))
        lines[i], lines[i + 1] = lines[i + 1], lines[i]
        rcode, out, _ = self.run_tool(lines)
        self.assertNotEqual(rcode, 0)
        self.assertIn("out of order", out)

    def test_truncated_stream(self):
        lines, _, _ = valid_log()
        i = next(k for k, l in enumerate(lines) if l.startswith("RNG randombytes_buf 2 "))
        rcode, out, files = self.run_tool(lines[:i])
        self.assertNotEqual(rcode, 0)
        self.assertIn("randombytes_buf truncated", out)
        self.assertEqual(files, {})

    def test_truncated_line(self):
        lines, _, _ = valid_log()
        i = next(k for k, l in enumerate(lines) if l.startswith("RNG esp_fill_random 0 "))
        lines[i] = lines[i][:-40]
        rcode, out, _ = self.run_tool(lines)
        self.assertNotEqual(rcode, 0)

    def test_interleaved_output_in_frame(self):
        lines, _, _ = valid_log()
        i = next(k for k, l in enumerate(lines) if l.startswith("RNG esp_fill_random 1 "))
        lines[i] = lines[i][:100] + "W (5) wdt: task watchdog" + lines[i][100:]
        rcode, out, _ = self.run_tool(lines)
        self.assertNotEqual(rcode, 0)

    def test_byte_count_mismatch(self):
        a = sample(BLOCK + 10, 4)
        lines = frames("esp_fill_random", a)
        lines[0] = lines[0].replace("bytes=%d" % len(a), "bytes=%d" % (len(a) + BLOCK))
        rcode, out, _ = self.run_tool(lines)
        self.assertNotEqual(rcode, 0)
        self.assertIn("expected", out)

    def test_no_stream(self):
        rcode, out, _ = self.run_tool(["I (1) dispenser: nothing here"])
        self.assertNotEqual(rcode, 0)
        self.assertIn("no RNG stream found", out)

    def test_stats_sanity(self):
        st = rc.stats(bytes(range(256)) * 16)
        self.assertAlmostEqual(st["chi2"], 0.0)
        self.assertAlmostEqual(st["ones"], 0.5)
        stuck = rc.stats(b"\x00" * 4096)
        self.assertGreater(stuck["chi2"], 1e5)
        self.assertLess(stuck["z"], -50)


if __name__ == "__main__":
    unittest.main()
