# noise-c for Dispenser V2 BLE security (M1.10)

The BLE protocol uses Noise `XXpsk3` (enrollment) and `IK` (sessions) with
`25519_ChaChaPoly_SHA256` (see `docs/BLE_PROTOCOL_DESIGN.md`, revision 3). Both the ESP32-C3
firmware and the Android app (through a minimal JNI binding, M1.10c) use the **same** pinned
sources, so interoperability holds by construction. Nothing in this directory implements a
cryptographic primitive or a Noise state machine.

## Pinned upstream sources

| Component | Version | Source identity |
|---|---|---|
| `esphome/noise-c` (MIT) | 0.1.31 | git `https://github.com/esphome/noise-c.git` commit `62c2fdca7a874ea898761147bb062ca1cde9f36d` (tag `v0.1.31`, 2026-10-08); ESP-IDF registry `component_hash 12dcc72a8ca1ba82f703aaba07dcddeab6037135a7fa7e73e99af80efd6e565f` |
| `esphome/libsodium` (ISC) | 1.10021.12 | git `https://github.com/esphome-libs/libsodium` commit `95c5e60d991b3f735619da8a307951c0483ef567` (libsodium `d24faf56214469b354b01c8ba36257e04737101e` = 1.0.21-RELEASE with 21 ESPHome patches); ESP-IDF registry `component_hash 39bee6f58d2d7fb8f2d30e04133afe60d0b642792196d36cde3a6dd07738fb08` |

The registry packages are byte-identical to the git sources apart from registry metadata
(verified 2026-11-16). `gate/esp32c3/dependencies.lock` records the registry pins.

## Local patches (temporary)

Applied identically to every build (host tests, Android NDK, ESP32-C3) by
`gate/run_gate.sh`; M1.10c integrates the same patched tree into the firmware and app builds.

### `patches/0001-patterns-bound-base-pattern-search-by-table-size.patch`

- **Problem:** `noise_pattern_expand()` (`src/protocol/patterns.c`) searched each base pattern
  for its end token with `memchr(base_pattern + 2, NOISE_TOKEN_END, NOISE_MAX_TOKENS - 2)`, a
  62-byte bound, while the pattern tables are 5–12-byte static arrays. Every table ends with the
  end token, so a sequential `memchr` stops inside the array (ASan/UBSan report nothing on glibc),
  but the length argument exceeds the object. Android's bionic FORTIFY checks that bound and
  **aborts** the process (`FORTIFY: memchr: prevented 62-byte read from 10-byte buffer`) at the
  first handshake. ESP-IDF's newlib does not enforce FORTIFY, which hides the defect on ESP32.
- **Fix:** an internal `noise_pattern_lookup_sized()` returns each table together with its
  `sizeof`; `noise_pattern_expand()` searches only within the table (and never beyond
  `NOISE_MAX_TOKENS`). `noise_pattern_lookup()` keeps its signature and behavior. No cryptographic
  code, handshake state machine or wire format is touched.
- **Semantics preserved:** `gate/test_patterns.c` compares every one of the 15 base patterns with
  an independent sequential reference and with the Noise specification sequences for XX, IK and
  XXpsk3; unknown pattern ids, unknown modifiers and malformed protocol names are rejected.
- **Regression:** `gate/run_gate.sh` step 3 builds the unpatched library for Android x86_64 and
  requires it to abort with the FORTIFY `memchr` message; the patched build must pass with
  FORTIFY enabled (step 2).

### `patches/0002-esp-idf-keep-default-feature-switches.patch`

- **Problem:** the component's ESP-IDF registration forces `NOISE_USE_PROTOCOL_NAME_TABLE=0
  NOISE_USE_FALLBACK=0 NOISE_USE_HFS=0` (PRIVATE), which limits ESP-IDF builds to the single
  protocol ESPHome uses (`Noise_NNpsk0_25519_ChaChaPoly_SHA256`).
- **Fix:** remove that definition so ESP-IDF builds use the upstream defaults (the configuration
  the full upstream test suite validates and the host/Android builds use). Cost: +2,713 B flash
  compared with the packaged single-pattern build. A narrower variant (name table only) passes our
  gate but not the upstream suite, so it is not used.
- **Integration:** the firmware uses the patched tree through ESP-IDF's `override_path` for the
  `esphome/noise-c` dependency (as `gate/esp32c3/main/idf_component.yml` does).

## Firmware and host integration (M1.10c.2a)

`noise_c_deps.cmake` delivers the same patched tree to both builds (decision C3):

- It fetches noise-c by its full commit SHA, re-verifies the checked-out commit, applies 0001 and
  0002 with GNU `patch --fuzz=0` and checks the SHA-256 of the two patched files
  (`src/protocol/patterns.c`, `CMakeLists.txt`). Any mismatch stops the configuration.
- **ESP-IDF:** `dsp_noise_c_prepare()` puts the tree into the build directory as the component
  `esphome__noise-c` (`firmware/CMakeLists.txt`, `EXTRA_COMPONENT_DIRS`). libsodium comes from the
  ESP-IDF registry at exactly `==1.10021.12` (`components/dispenser_security/idf_component.yml`,
  hash in `firmware/dependencies.lock`).
- **Host tests:** `dsp_noise_c_host()` also fetches the libsodium fork at commit
  `95c5e60d…`, verifies its upstream submodule (`d24faf56…`) and applies the fork's own patches.
  For the host cross-check only, it compiles libsodium's `kdf_hkdf_sha256.c` and
  `auth_hmacsha256.c` from that tree as `dsp_sodium_hkdf` (not in the fork's curated list; the
  library is not modified).
- Downloads are cached in `DSP_DEPS_CACHE` (default `<build>/_dsp_deps`); a fresh cache needs
  network access once.
- Randomness: noise-c's default `NOISE_USE_CUSTOM_RAND` applies in every build. The firmware
  supplies `noise_rand_bytes()` (`dispenser_security/src/dsp_noise_rand.c`) as libsodium's
  `randombytes_buf()`, i.e. the single implementation registered before `sodium_init()`:
  the ESP32-C3 hardware adapter, or the deterministic test implementation in host tests.

## Removing the patches after an upstream release

1. When `esphome/noise-c` releases a version that bounds the pattern search (0001) and, if
   accepted, offers an option to keep the default feature switches on ESP-IDF (0002):
   update `NOISE_C_COMMIT` in `gate/CMakeLists.txt` and `run_gate.sh`, the libsodium pins if the
   new release changes them, and this README.
2. Delete the patch that upstream made unnecessary from `patches/` and from `gate/CMakeLists.txt`
   (`_patches`); for 0002 replace the `override_path` with the registry version plus the upstream
   option.
3. Run `gate/run_gate.sh`. Step 3 (regression) must then be changed to expect the **pinned
   upstream** library to pass under FORTIFY; all other steps must pass unchanged.
4. Record the new hashes here and in the design document (section 4.9).

An upstream report (reproducer, affected version, cause, proposed fix) has been drafted outside
the repository and is **not submitted**; submitting it needs the project owner's approval.

## Feasibility gate

`gate/run_gate.sh` (needs network access for the pinned sources, CMake ≥ 3.24, Ninja, GCC,
Python 3, rsync, the Android NDK r30 and ESP-IDF v6.1) runs:

1. host build: source identities, upstream unit and vector tests, the strict vector gate
   (`required_vectors.py`: XXpsk3 and IK executed, zero skipped), the pattern regression test,
   the probe self-test (handshakes, transport both ways, wrong PSK, wrong static key, modified,
   truncated and replayed frames), peak heap per side (a host measurement only; target heap,
   stack and timing are hardware acceptance items);
2. Android NDK builds for arm64-v8a, armeabi-v7a, x86_64 and x86 with FORTIFY enabled;
   Android-native (static x86_64) upstream tests, strict vector gate, regression test, self-test;
   byte-exact transcript comparison with the host build; cross-process interoperability host ↔
   Android in both roles for XX and IK; stripped library sizes;
3. regression: the unpatched library aborts under FORTIFY;
4. ESP32-C3 build with the patched tree (ESP-IDF v6.1), sizes.

`GATE_STEPS="2 3" ./run_gate.sh` runs selected steps. Work directory `gate/build/` (ignored).

## Reproducing the M1.10a verification (CI)

All inputs are files in this repository plus the pinned external sources (fetched by commit
hash or exact registry version; the gate verifies the hashes). Nothing is vendored: noise-c (MIT)
and libsodium (ISC) are downloaded at build time with their license files intact; vendoring for
the firmware/app integration (M1.10c) must keep those notices.

| Step | Command (from the repository root) | Needs |
|---|---|---|
| 1. Shared vectors are current | `python3 -I test-vectors/tools/gen_ble_protocol_v1.py \| diff - test-vectors/ble_protocol_v1.txt` | Python 3 |
| 2. Firmware host tests (incl. `test_bleproto`) | `cmake -S firmware/test/host -B firmware/build-host && cmake --build firmware/build-host && ctest --test-dir firmware/build-host --no-tests=error` | CMake, GCC (ASan/UBSan on by default) |
| 3. Noise gate (host, Android NDK, regression, ESP32-C3) | `firmware/third_party/noise-c/gate/run_gate.sh` | network (GitHub, ESP-IDF registry), CMake ≥ 3.24, Ninja, rsync, Android NDK r30 (`ANDROID_NDK`), ESP-IDF v6.1 (`IDF_ACTIVATE`) |
| 4. Kotlin codec and app unit tests | `cd android && ./gradlew :app:testDebugUnitTest` | JDK 17+, Android SDK (`ANDROID_HOME` or `local.properties`) |
| 5. Firmware image | `cd firmware && idf.py build` | ESP-IDF v6.1 |

Each step exits non-zero on failure; step 3 prints one PASS/FAIL line per check and fails on any
skipped required Noise vector.
