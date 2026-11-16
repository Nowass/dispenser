#!/usr/bin/env bash
# M1.10a Noise feasibility gate: host, Android NDK and ESP32-C3 builds of the PATCHED pinned
# noise-c, plus the regression check against the UNPATCHED library. Prints PASS/FAIL per check
# and exits non-zero on any failure. Downloads the pinned sources (GitHub, ESP-IDF registry).
#
#   ./run_gate.sh                     all steps
#   GATE_STEPS="2 3" ./run_gate.sh    selected steps (later steps reuse earlier build trees)
#
# Requirements: cmake >= 3.24, ninja, gcc, python3, rsync, git; Android NDK r30
# (ANDROID_NDK, default ~/Android/Sdk/ndk/30.0.16248370); ESP-IDF v6.1 (IDF_ACTIVATE, default
# ~/.espressif/tools/activate_idf_v6.1.sh). Work directory: ./build (git-ignored).
set -uo pipefail

GATE="$(cd "$(dirname "$0")" && pwd)"
OUT="$GATE/build"
NDK="${ANDROID_NDK:-$HOME/Android/Sdk/ndk/30.0.16248370}"
IDF_ACTIVATE="${IDF_ACTIVATE:-$HOME/.espressif/tools/activate_idf_v6.1.sh}"
NOISE_C_COMMIT=62c2fdca7a874ea898761147bb062ca1cde9f36d
SODIUM_COMMIT=95c5e60d991b3f735619da8a307951c0483ef567
SODIUM_UPSTREAM_COMMIT=d24faf56214469b354b01c8ba36257e04737101e
VECTORS=(cacophony.txt noise-c-basic.txt)
ABIS=(arm64-v8a armeabi-v7a x86_64 x86)

pass=0
fail=0
ok() { echo "PASS $*"; pass=$((pass + 1)); }
bad() { echo "FAIL $*"; fail=$((fail + 1)); }
step() { echo; echo "== $*"; }
want() { [[ " ${GATE_STEPS:-1 2 3 4} " == *" $1 "* ]]; }
H="$OUT/host"
A="$OUT/android-x86_64"
U="$OUT/android-unpatched"
HOST_DIGEST=""
check() { local what="$1"; shift; if "$@" >> "$OUT/checks.log" 2>&1; then ok "$what"; else bad "$what"; fi; }

mkdir -p "$OUT"
: > "$OUT/checks.log"

# configure+build one variant: build_variant DIR [cmake args...]
build_variant() {
    local dir="$1"; shift
    cmake --fresh -S "$GATE" -B "$OUT/$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "$@" > "$OUT/$dir.log" 2>&1 &&
        cmake --build "$OUT/$dir" >> "$OUT/$dir.log" 2>&1
}

android_variant() {
    local dir="$1" abi="$2"; shift 2
    local extra=()
    [ "$abi" = x86_64 ] && extra=(-DCMAKE_EXE_LINKER_FLAGS=-static) # runnable on the Linux host
    build_variant "$dir" -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$abi" -DANDROID_PLATFORM=android-26 "${extra[@]}" "$@"
}

vector_files() { local d="$1"; for v in "${VECTORS[@]}"; do echo "$d/_deps/noise_c-src/tests/vector/$v"; done; }
upstream_bin() { echo "$1/_deps/noise_c-build/$2"; } # noise-c's own test executables

# interop INIT_BIN RESP_BIN PATTERN: separate processes over FIFOs
interop() {
    local fifo="$OUT/fifo"
    mkdir -p "$fifo"
    rm -f "$fifo/a" "$fifo/b"
    mkfifo "$fifo/a" "$fifo/b"
    timeout 30 "$2" resp "$3" < "$fifo/a" > "$fifo/b" 2> "$fifo/resp.err" &
    local rp=$!
    timeout 30 "$1" init "$3" > "$fifo/a" < "$fifo/b" 2> "$fifo/init.err"
    local ri=$?
    wait "$rp"
    local rr=$?
    cat "$fifo/init.err" "$fifo/resp.err" >> "$OUT/checks.log"
    [ "$ri" -eq 0 ] && [ "$rr" -eq 0 ]
}

digest_of() { "$1" selftest 2>&1 | sed -n 's/^transcript digest: //p'; }

# ---- 1. Host, patched -----------------------------------------------------------------------
if want 1; then
step "1. host build (gcc), patched"
check "host build" build_variant host
check "noise-c source is commit $NOISE_C_COMMIT" \
    test "$(git -C "$H/_deps/noise_c-src" rev-parse HEAD)" = "$NOISE_C_COMMIT"
check "local patches applied (patterns.c, CMakeLists.txt only)" \
    test "$(git -C "$H/_deps/noise_c-src" diff --name-only | sort | tr '\n' ' ')" = "CMakeLists.txt src/protocol/patterns.c "
SOD="$H/_deps/esphome_libsodium-1.10021.12"
check "libsodium fork is commit $SODIUM_COMMIT" test "$(git -C "$SOD" rev-parse HEAD)" = "$SODIUM_COMMIT"
check "libsodium upstream is commit $SODIUM_UPSTREAM_COMMIT (1.0.21-RELEASE)" \
    test "$(git -C "$SOD/libsodium" rev-parse HEAD)" = "$SODIUM_UPSTREAM_COMMIT"
check "upstream unit + vector tests (ctest, no-tests=error)" \
    ctest --test-dir "$H" --output-on-failure --no-tests=error
check "required vectors XXpsk3/IK executed, 0 skipped (host)" \
    python3 -I "$GATE/required_vectors.py" "$(upstream_bin "$H" noise-c-test-vector)" $(vector_files "$H")
check "pattern lookup regression test (host)" "$H/test_patterns"
check "probe self-test: handshakes, transport, wrong keys, tampering, truncation, replay (host)" \
    "$H/nprobe" selftest
HOST_DIGEST="$(digest_of "$H/nprobe")"
echo "host transcript digest: $HOST_DIGEST" | tee -a "$OUT/checks.log"
"$H/nprobe_heap" selftest > /dev/null 2> "$OUT/heap.log"
echo "heap (in-process, both sides): $(grep HEAP "$OUT/heap.log")"
for p in XX IK; do
    interop "$H/nprobe_heap" "$H/nprobe" "$p"
    echo "heap per side ($p initiator): $(grep HEAP "$OUT/fifo/init.err")"
done

fi

# ---- 2. Android NDK, patched, FORTIFY enabled ------------------------------------------------
if want 2; then
[ -n "$HOST_DIGEST" ] || HOST_DIGEST="$(digest_of "$H/nprobe")"
step "2. Android NDK ($(basename "$NDK")) builds, patched, FORTIFY enabled"
for abi in "${ABIS[@]}"; do
    check "android $abi build" android_variant "android-$abi" "$abi"
    check "android $abi compiled with _FORTIFY_SOURCE" \
        grep -q -- "-D_FORTIFY_SOURCE=" "$OUT/android-$abi/compile_commands.json"
    so="$OUT/android-$abi/libdspnoise.so"
    if [ -f "$so" ]; then
        cp "$so" "$OUT/android-$abi/libdspnoise.stripped.so"
        "$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-unneeded \
            "$OUT/android-$abi/libdspnoise.stripped.so"
        echo "android $abi stripped library: $(stat -c %s "$OUT/android-$abi/libdspnoise.stripped.so") B"
    fi
done
check "android-native upstream unit tests (x86_64, FORTIFY)" "$(upstream_bin "$A" noise-c-test-unit)"
check "android-native upstream vector tests (x86_64, FORTIFY)" \
    "$(upstream_bin "$A" noise-c-test-vector)" $(vector_files "$A")
check "android-native required vectors XXpsk3/IK, 0 skipped" \
    python3 -I "$GATE/required_vectors.py" "$(upstream_bin "$A" noise-c-test-vector)" $(vector_files "$A")
check "android-native pattern lookup regression test" "$A/test_patterns"
check "android-native probe self-test (FORTIFY enabled)" "$A/nprobe" selftest
ANDROID_DIGEST="$(digest_of "$A/nprobe")"
echo "android transcript digest: $ANDROID_DIGEST" | tee -a "$OUT/checks.log"
check "byte-exact transcripts host (gcc) == android (clang/bionic)" \
    test -n "$HOST_DIGEST" -a "$HOST_DIGEST" = "$ANDROID_DIGEST"
for p in XX IK; do
    check "interop $p: host initiator, android responder" interop "$H/nprobe" "$A/nprobe" "$p"
    check "interop $p: android initiator, host responder" interop "$A/nprobe" "$H/nprobe" "$p"
done

fi

# ---- 3. Regression: the unpatched library aborts under Android FORTIFY ------------------------
if want 3; then
step "3. regression: unpatched noise-c under Android FORTIFY"
check "android x86_64 build, UNPATCHED" android_variant android-unpatched x86_64 -DNOISE_GATE_PATCHES=OFF
"$U/nprobe" selftest > "$OUT/unpatched.log" 2>&1
urc=$?
check "unpatched build aborts (exit $urc)" test "$urc" -ne 0
check "abort is the FORTIFY memchr check" grep -q "FORTIFY: memchr" "$OUT/unpatched.log"

fi

# ---- 4. ESP32-C3, patched --------------------------------------------------------------------
if want 4; then
step "4. ESP32-C3 build (ESP-IDF v6.1), patched"
mkdir -p "$OUT/esp-deps/esphome__noise-c"
rsync -a --delete --exclude .git "$H/_deps/noise_c-src/" "$OUT/esp-deps/esphome__noise-c/"
# The ESP-IDF activation script ends a sourcing subshell of a script file, so the build runs
# in its own bash process.
GATE="$GATE" OUT="$OUT" IDF_ACTIVATE="$IDF_ACTIVATE" bash -c '
    . "$IDF_ACTIVATE" > /dev/null 2>&1 || { echo "ESP-IDF activation failed" > "$OUT/esp32c3.log"; exit 1; }
    cd "$GATE/esp32c3" &&
        idf.py -B "$OUT/esp32c3" -D SDKCONFIG="$OUT/esp32c3/sdkconfig" build > "$OUT/esp32c3.log" 2>&1 &&
        idf.py -B "$OUT/esp32c3" -D SDKCONFIG="$OUT/esp32c3/sdkconfig" size-components > "$OUT/esp32c3-size.log" 2>&1'
check "esp32c3 build" test -f "$OUT/esp32c3/noise_gate_c3.bin"
check "esp32c3 uses the patched tree" grep -q "esp-deps/esphome__noise-c/src/protocol/patterns.c" \
    "$OUT/esp32c3/compile_commands.json"
check "esp32c3 keeps the upstream default feature switches" \
    bash -c "! grep -q NOISE_USE_PROTOCOL_NAME_TABLE=0 '$OUT/esp32c3/compile_commands.json'"
check "esp32c3 build without warnings" bash -c "! grep -q 'warning:' '$OUT/esp32c3.log'"
grep -E "esphome__(noise-c|libsodium)" "$OUT/esp32c3-size.log" | sed 's/│/|/g' |
    awk -F'|' '{gsub(/ /,"",$2); printf "esp32c3 %s: total %d B, DRAM %d B, code %d B, rodata %d B\n",$2,$3,$4,$8,$10}'
echo "esp32c3 image: $(stat -c %s "$OUT/esp32c3/noise_gate_c3.bin" 2>/dev/null) B"
fi

echo
echo "== gate: $pass passed, $fail failed (details: $OUT/checks.log)"
[ "$fail" -eq 0 ]
