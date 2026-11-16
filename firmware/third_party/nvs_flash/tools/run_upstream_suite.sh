#!/usr/bin/env bash
# Runs ESP-IDF's own NVS host test suite (components/nvs_flash/host_test/nvs_host_test, linux
# target, Catch2) against the project's patched nvs_flash copy and against the unpatched ESP-IDF
# component, in a work directory outside the ESP-IDF tree, and compares the failing test cases.
#
#   firmware/third_party/nvs_flash/tools/run_upstream_suite.sh [work-dir]
#
# The ESP-IDF installation is never modified: the suite is built from a copy, and test cases
# that write relative to the ESP-IDF root run from a mirror root in the work directory.
# Needs ESP-IDF v6.1 (IDF_ACTIVATE), network access to the ESP Component Registry for
# espressif/catch2 (downloaded into the work directory by the component manager) and the host's
# libbsd headers (CPATH/LIBRARY_PATH, see firmware/test/nvs_linux/README.md).
# Exit status 0 only if the patched build has no failing test case that the unpatched build
# does not also have.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MOD="$HERE/../nvs_flash_override.cmake"
IDF_ACTIVATE="${IDF_ACTIVATE:-$HOME/.espressif/tools/activate_idf_v6.1.sh}"
WORK="${1:-${TMPDIR:-/tmp}/dsp-nvs-upstream-suite}"
mkdir -p "$WORK"

prepare() { # variant -> app dir
    local v="$1" app="$WORK/$1/app" nvs
    rm -rf "$WORK/$v"
    mkdir -p "$WORK/$v"
    WORK="$WORK" V="$v" MOD="$MOD" IDF_ACTIVATE="$IDF_ACTIVATE" bash -c '
        . "$IDF_ACTIVATE" >/dev/null 2>&1 || exit 3
        cp -r "$IDF_PATH/components/nvs_flash/host_test/nvs_host_test" "$WORK/$V/app"
        if [ "$V" = patched ]; then
            cmake -DDST="$WORK/$V/nvs" -P "$MOD" > "$WORK/$V/nvs.txt" 2>&1 || exit 4
            echo "$WORK/$V/nvs/nvs_flash" > "$WORK/$V/nvs_dir"
        else
            echo "$IDF_PATH/components/nvs_flash" > "$WORK/$V/nvs_dir"
        fi
        echo "$IDF_PATH" > "$WORK/$V/idf_path"' || return 1
    nvs="$(cat "$WORK/$v/nvs_dir")"
    local idf
    idf="$(cat "$WORK/$v/idf_path")"
    # The test sources include the component's private headers through relative paths.
    sed -i "s|\"../../../src\"|\"$nvs/src\"|; s|\"../../../private_include\"|\"$nvs/private_include\"|; \
            s|\"../../../../mbedtls/mbedtls/include\"|\"$idf/components/mbedtls/mbedtls/include\"|" \
        "$app/main/CMakeLists.txt"
    # Known upstream TEST bug (reported in espressif/esp-idf#19212): "nvs multiple write with same
    # key blob and string involved" passes an uninitialized size_t as the in/out buffer length,
    # so it passes or fails depending on stack contents. Initialized in this work copy only.
    grep -q '^    size_t blob_read_size;$' "$app/main/test_nvs.cpp" || { echo "known test bug not found"; return 1; }
    sed -i 's/^    size_t blob_read_size;$/    size_t blob_read_size = blob_data_len;/' "$app/main/test_nvs.cpp"
    if [ "$v" = patched ]; then
        sed -i "s|^set(COMPONENTS main)|set(COMPONENTS main)\nlist(APPEND EXTRA_COMPONENT_DIRS \"$nvs\")|" \
            "$app/CMakeLists.txt"
    fi
}

run() { # variant
    local v="$1" app="$WORK/$1/app"
    APP="$app" IDF_ACTIVATE="$IDF_ACTIVATE" bash -c '
        . "$IDF_ACTIVATE" >/dev/null 2>&1 || exit 3
        cd "$APP" && idf.py -B build build' > "$WORK/$v/build.log" 2>&1 || { echo "build failed: $v"; return 2; }
    grep -o '[^ "]*nvs_flash/src/nvs_page.cpp' "$app/build/compile_commands.json" | head -1 > "$WORK/$v/compiled_from"
    # Some test cases run shell commands relative to the ESP-IDF root and WRITE there
    # (tools/mass_mfg, nvs_partition_generator). They run from a mirror root in the work
    # directory instead, so the ESP-IDF installation is never modified.
    local root="$WORK/$v/root"
    rm -rf "$root"
    mkdir -p "$root/components" "$root/tools"
    cp -r "$(cat "$WORK/$v/nvs_dir")" "$root/components/nvs_flash"
    cp -r "$(cat "$WORK/$v/idf_path")/tools/mass_mfg" "$root/tools/mass_mfg"
    ROOT="$root" ELF="$app/build/nvs_host_test.elf" OUT="$WORK/$v" IDF_ACTIVATE="$IDF_ACTIVATE" bash -c '
        . "$IDF_ACTIVATE" >/dev/null 2>&1 || exit 3
        cd "$ROOT" && "$ELF" > "$OUT/console.log" 2>&1'
    # failing test cases: the name between the two dashed lines preceding a "FAILED:" line
    awk '/^-{20,}$/{d++; next} d==1{name=$0} /FAILED:/{print name} /^\.{20,}$/{d=0}' \
        "$WORK/$v/console.log" | sort -u > "$WORK/$v/failed.txt"
    # Catch2 v3 runs test cases in random order (seed printed): upstream's test cases share
    # the emulated partitions, so a failure must be replayed with the same seed
    # (--rng-seed <seed>) before it is attributed to the code under test.
    echo "$(grep -a -E "^test cases:|All tests passed" "$WORK/$v/console.log" | tail -1)" \
         "[$(grep -a -o "Randomness seeded to: [0-9]*" "$WORK/$v/console.log" | head -1)]" > "$WORK/$v/summary.txt"
    echo "[$v] compiled $(cat "$WORK/$v/compiled_from")"
    echo "[$v] $(cat "$WORK/$v/summary.txt")"
    echo "[$v] failing test cases: $(wc -l < "$WORK/$v/failed.txt")"
    sed 's/^/        /' "$WORK/$v/failed.txt"
}

status=0
for v in patched unpatched; do
    prepare "$v" || { echo "prepare failed: $v"; exit 2; }
    run "$v" || status=2
done
new="$(comm -23 "$WORK/patched/failed.txt" "$WORK/unpatched/failed.txt")"
if [ -n "$new" ]; then
    echo "FAIL  test cases failing only with the patches:"; echo "$new" | sed 's/^/        /'
    status=1
elif ! grep -q "All tests passed" "$WORK/patched/summary.txt"; then
    echo "FAIL  the patched build does not pass the whole suite"
    status=1
else
    echo "PASS  ESP-IDF's NVS suite passes completely with the patches (unpatched: $(cat "$WORK/unpatched/summary.txt"))"
fi
exit $status
