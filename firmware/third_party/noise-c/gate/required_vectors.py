#!/usr/bin/env python3
"""Strict Noise vector gate (M1.10a): runs ONLY the required protocols from the official vector
files and fails if any of them is missing, skipped or failing.

Usage: required_vectors.py RUNNER VECTOR_FILE... (RUNNER = noise-c-test-vector)
"""
import json, re, subprocess, sys, tempfile, os
REQUIRED = ["Noise_XXpsk3_25519_ChaChaPoly_SHA256", "Noise_IK_25519_ChaChaPoly_SHA256"]
runner, *files = sys.argv[1:]
selected = []
for f in files:
    data = json.load(open(f))
    vecs = data["vectors"] if isinstance(data, dict) else data
    for v in vecs:
        name = v.get("protocol_name") or v.get("name")
        if name in REQUIRED:
            selected.append(v)
counts = {p: sum(1 for v in selected if (v.get("protocol_name") or v.get("name")) == p) for p in REQUIRED}
print("selected vectors per protocol:", counts)
if any(c == 0 for c in counts.values()):
    sys.exit("GATE FAIL: a required protocol has no official vector")
with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as t:
    json.dump({"vectors": selected}, t)
    path = t.name
out = subprocess.run([runner, path], capture_output=True, text=True).stdout
os.unlink(path)
print(out.strip().splitlines()[-1])
ok = {p: len(re.findall(re.escape(p) + r" \.\.\. ok", out)) for p in REQUIRED}
m = re.search(r"(\d+) vectors run, (\d+) skipped", out)
run, skipped = (int(m.group(1)), int(m.group(2))) if m else (0, -1)
fails = [l for l in out.splitlines() if "... " in l and not l.endswith("ok")]
print("executed ok per protocol:", ok, "| run", run, "skipped", skipped)
if skipped != 0 or run != len(selected) or fails or any(ok[p] != counts[p] for p in REQUIRED):
    print("\n".join(fails))
    sys.exit("GATE FAIL: required vectors skipped, missing or failed")
print("GATE PASS: all", run, "required vectors executed and passed, 0 skipped")
