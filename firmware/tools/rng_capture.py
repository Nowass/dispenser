#!/usr/bin/env python3
"""Capture and verify the RNG-gate diagnostic stream (M1.10c.2b R5).

The RNG-gate image (sdkconfig.rnggate.defaults, CONFIG_DSP_RNG_GATE_DIAGNOSTIC) prints, per
source, framed lines on the console:

    RNGBEGIN source=<name> bytes=<N> block=4096
    RNG <name> <index> <crc32 hex8> <Base64 of the raw block>
    RNGEND source=<name> blocks=<count> bytes=<N> crc32=<crc32 hex8 of the whole stream>

and "RNGGATE ..." result lines (R1, R2, R3, R5, R6 and a SUMMARY line). Other console lines are
ignored (a frame line split by other output is an error). This tool verifies the framing (order, block size, counts), every block CRC-32
(zlib.crc32) and the whole-stream CRC, writes the raw bytes of each source to
<out-dir>/<prefix>_<source>.bin (only when the whole capture is error-free) and exits non-zero on any framing or CRC error.

    # from a serial port (pyserial from the ESP-IDF Python environment); reads only, --reset
    # pulses RTS once for a normal reboot so that the capture starts at boot:
    python firmware/tools/rng_capture.py --port /dev/ttyACM0 --reset --out-dir rng-out --log rng.log
    # from a saved console log:
    python3 firmware/tools/rng_capture.py --input rng.log --out-dir rng-out

The printed byte histogram chi-square and monobit figures are only a sanity check for gross
failures (stuck or biased output, broken transport). They are NOT evidence of entropy quality:
run ent, dieharder and/or a NIST STS subset on the written .bin files. They are not evidence
that the entropy source was enabled either; that evidence is R1-R3 in the device log.
"""

import argparse
import base64
import binascii
import math
import os
import re
import sys
import time
import zlib

BLOCK = 4096
SOURCE_RE = re.compile(r"^[A-Za-z0-9_]+$")
BEGIN_RE = re.compile(r"^RNGBEGIN source=(\S+) bytes=(\d+) block=(\d+)$")
LINE_RE = re.compile(r"^RNG (\S+) (\d+) ([0-9a-f]{8}) ([A-Za-z0-9+/=]+)$")
END_RE = re.compile(r"^RNGEND source=(\S+) blocks=(\d+) bytes=(\d+) crc32=([0-9a-f]{8})$")
# ESP-IDF log prefix, e.g. "I (1234) dispenser: RNGGATE ..." (colour codes stripped first)
ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
GATE_RE = re.compile(r"(RNGGATE .*)$")

DISCLAIMER = ("NOTE: the statistics above are a sanity check for gross failures only; they are NOT "
              "evidence of entropy quality (run ent / dieharder / NIST STS on the .bin files) and "
              "NOT evidence that the entropy source was enabled (that is R1-R3 in the device log).")


class Stream:
    def __init__(self, source, total, block):
        self.source = source
        self.total = total
        self.block = block
        self.data = bytearray()
        self.blocks = 0
        self.crc = 0
        self.ended = False


class Capture:
    """Line-by-line parser; collects streams, RNGGATE lines and errors."""

    def __init__(self):
        self.streams = {}
        self.order = []
        self.current = None
        self.errors = []
        self.crc_failures = 0
        self.gate_lines = []
        self.done = False  # SUMMARY seen

    def error(self, lineno, msg):
        self.errors.append("line %d: %s" % (lineno, msg))

    def feed(self, lineno, raw):
        line = ANSI_RE.sub("", raw).strip("\r\n").strip()
        if not line:
            return
        g = GATE_RE.search(line)
        if g and not line.startswith("RNG "):
            self.gate_lines.append(g.group(1))
            if g.group(1).startswith("RNGGATE SUMMARY"):
                self.done = True
            return
        if line.startswith("RNGBEGIN"):
            m = BEGIN_RE.match(line)
            if not m:
                self.error(lineno, "malformed RNGBEGIN")
                return
            src, total, block = m.group(1), int(m.group(2)), int(m.group(3))
            if self.current is not None:
                self.error(lineno, "RNGBEGIN %s inside unfinished stream %s" % (src, self.current.source))
            if not SOURCE_RE.match(src) or src in self.streams:
                self.error(lineno, "invalid or repeated source %r" % src)
                self.current = None
                return
            if block != BLOCK or total <= 0:
                self.error(lineno, "unsupported block size %d or byte count %d" % (block, total))
                self.current = None
                return
            self.current = Stream(src, total, block)
            self.streams[src] = self.current
            self.order.append(src)
            return
        if line.startswith("RNGEND"):
            m = END_RE.match(line)
            cur = self.current
            if not m:
                self.error(lineno, "malformed RNGEND")
                return
            if cur is None or m.group(1) != cur.source:
                self.error(lineno, "RNGEND for %s without matching RNGBEGIN" % m.group(1))
                return
            blocks, total, crc = int(m.group(2)), int(m.group(3)), int(m.group(4), 16)
            if blocks != cur.blocks:
                self.error(lineno, "%s: RNGEND blocks=%d but %d received" % (cur.source, blocks, cur.blocks))
            if total != cur.total or len(cur.data) != cur.total:
                self.error(lineno, "%s: expected %d bytes, received %d (RNGEND says %d)"
                           % (cur.source, cur.total, len(cur.data), total))
            if crc != cur.crc:
                self.error(lineno, "%s: whole-stream CRC mismatch" % cur.source)
                self.crc_failures += 1
            cur.ended = True
            self.current = None
            return
        if line.startswith("RNG "):
            m = LINE_RE.match(line)
            cur = self.current
            if not m:
                self.error(lineno, "malformed RNG block line")
                return
            src, idx, crc, b64 = m.group(1), int(m.group(2)), int(m.group(3), 16), m.group(4)
            if cur is None or src != cur.source:
                self.error(lineno, "block of %s outside its stream" % src)
                return
            if idx != cur.blocks:
                self.error(lineno, "%s: block %d out of order (expected %d)" % (src, idx, cur.blocks))
                # keep counting from the received index so that one gap is reported once
                cur.blocks = idx
            try:
                raw_block = base64.b64decode(b64, validate=True)
            except (binascii.Error, ValueError):
                self.error(lineno, "%s: block %d: invalid Base64" % (src, idx))
                cur.blocks += 1
                return
            if zlib.crc32(raw_block) & 0xFFFFFFFF != crc:
                self.error(lineno, "%s: block %d: CRC mismatch" % (src, idx))
                self.crc_failures += 1
                cur.blocks += 1
                return
            remaining = cur.total - len(cur.data)
            expect = min(cur.block, remaining)
            if len(raw_block) != expect:
                self.error(lineno, "%s: block %d has %d bytes, expected %d" % (src, idx, len(raw_block), expect))
                cur.blocks += 1
                return
            cur.data += raw_block
            cur.crc = zlib.crc32(raw_block, cur.crc) & 0xFFFFFFFF
            cur.blocks += 1
            return

    def finish(self):
        if self.current is not None:
            self.errors.append("end of input: stream %s truncated (no RNGEND)" % self.current.source)
        if not self.streams:
            self.errors.append("no RNG stream found")


def stats(data):
    """Byte-histogram chi-square (255 degrees of freedom) and monobit fraction. Sanity only."""
    n = len(data)
    if n == 0:
        return None
    counts = [0] * 256
    for b in data:
        counts[b] += 1
    exp = n / 256.0
    chi2 = sum((c - exp) ** 2 / exp for c in counts)
    ones = sum(counts[v] * bin(v).count("1") for v in range(256))
    bits = 8 * n
    # monobit z-score: (ones - bits/2) / sqrt(bits/4)
    z = (ones - bits / 2.0) / math.sqrt(bits / 4.0)
    return {"chi2": chi2, "ones": ones / bits, "z": z, "min": min(counts), "max": max(counts)}


def write_outputs(cap, out_dir, prefix):
    """Writes the raw files only for a capture without any error (never a partial sample)."""
    paths = {}
    if cap.errors:
        return paths
    os.makedirs(out_dir, exist_ok=True)
    for src in cap.order:
        s = cap.streams[src]
        p = os.path.join(out_dir, "%s_%s.bin" % (prefix, src))
        with open(p, "wb") as f:
            f.write(bytes(s.data))
        paths[src] = p
    return paths


def report(cap, paths, out):
    for line in cap.gate_lines:
        print("device: %s" % line, file=out)
    for src in cap.order:
        s = cap.streams[src]
        st = stats(s.data)
        print("source %s: %d/%d bytes, %d blocks, ended=%s, file=%s"
              % (src, len(s.data), s.total, s.blocks, s.ended, paths.get(src, "-")), file=out)
        if st:
            print("  sanity: chi2(255 df)=%.1f (expected about 255, roughly 190-330), byte count "
                  "min/max=%d/%d, ones=%.6f, monobit z=%.2f (|z| < 3 expected)"
                  % (st["chi2"], st["min"], st["max"], st["ones"], st["z"]), file=out)
    if cap.errors:
        print("no .bin written: the capture has errors (re-run the capture)", file=out)
    print("CRC failures: %d, framing/CRC errors: %d" % (cap.crc_failures, len(cap.errors)), file=out)
    for e in cap.errors[:50]:
        print("ERROR %s" % e, file=out)
    if len(cap.errors) > 50:
        print("ERROR ... %d more" % (len(cap.errors) - 50), file=out)
    print(DISCLAIMER, file=out)


def read_serial(port, baud, timeout_s, log_path, cap, reset):
    try:
        import serial  # pyserial (ESP-IDF Python environment)
    except ImportError:
        sys.exit("pyserial is not available: run inside the ESP-IDF Python environment or use --input")
    # DTR stays low (never the download-mode strap); RTS is pulsed only with --reset
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 1
    ser.dtr = False
    ser.rts = False
    ser.open()
    if reset:
        # hard reset into the normal boot (same RTS pulse as esptool's hard reset; DTR low, so the
        # chip never enters the ROM download mode). Nothing is written to the device.
        ser.rts = True
        time.sleep(0.2)
        ser.rts = False
    logf = open(log_path, "w", encoding="utf-8", errors="replace") if log_path else None
    deadline = time.monotonic() + timeout_s
    lineno = 0
    buf = b""
    try:
        while time.monotonic() < deadline and not cap.done:
            chunk = ser.read(8192)
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                text = raw.decode("ascii", errors="replace")
                lineno += 1
                if logf:
                    logf.write(text + "\n")
                cap.feed(lineno, text)
    finally:
        ser.close()
        if logf:
            logf.close()
    if not cap.done:
        cap.errors.append("timeout after %d s without the RNGGATE SUMMARY line" % timeout_s)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--port", help="serial port of the device console (nothing is written to the device)")
    src.add_argument("--input", help="saved console log to parse instead of a port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--timeout", type=int, default=3600, help="serial capture limit in seconds")
    ap.add_argument("--log", help="also save the raw console text (serial mode)")
    ap.add_argument("--reset", action="store_true",
                    help="serial mode: pulse RTS once after opening the port so that the capture "
                         "starts at boot (normal boot, never download mode); without it, reset the "
                         "board by hand after the capture has started")
    ap.add_argument("--out-dir", default=".", help="directory for <prefix>_<source>.bin")
    ap.add_argument("--prefix", default="rng")
    args = ap.parse_args(argv)

    cap = Capture()
    if args.input:
        with open(args.input, "r", encoding="ascii", errors="replace") as f:
            for i, line in enumerate(f, 1):
                cap.feed(i, line)
    else:
        read_serial(args.port, args.baud, args.timeout, args.log, cap, args.reset)
    cap.finish()
    paths = write_outputs(cap, args.out_dir, args.prefix)
    report(cap, paths, sys.stdout)
    return 1 if cap.errors else 0


if __name__ == "__main__":
    sys.exit(main())
