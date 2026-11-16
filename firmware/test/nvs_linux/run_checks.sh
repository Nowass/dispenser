#!/usr/bin/env bash
# NVS reliability checks (M1.10b.1 follow-up): the domain storage over the real ESP-IDF v6.1
# nvs_flash on the linux target, with the project's patched component AND the unpatched
# ESP-IDF component as negative control.
#
#   firmware/test/nvs_linux/run_checks.sh [--matrix]
#
# Requires: ESP-IDF v6.1 (activation script: IDF_ACTIVATE, default
# ~/.espressif/tools/activate_idf_v6.1.sh), the host's libbsd headers (CPATH/LIBRARY_PATH, see
# README.md). BUILD_ROOT (default ${TMPDIR:-/tmp}/dsp-nvs-checks) holds the builds and logs.
#
# Required outcomes (exit status 0 only if all hold):
#   patched   checks 1-5 pass                       (and with --matrix: 6-8 pass)
#   unpatched checks 3 and 5 fail with the known defect: 14 of 14 plain-NVS scenarios lose the
#             key and the format 1 -> 2 upgrade leaves the storage CORRUPT
#                                                   (with --matrix: keys lost after 1 and 2 cuts)
#   both      byte-identical flash images for the same workload; each reads the other's images
#             and recovers the other's unrecovered cut state
# --matrix runs the two-cut matrices (several million recovery cuts, about 30-40 minutes).
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IDF_ACTIVATE="${IDF_ACTIVATE:-$HOME/.espressif/tools/activate_idf_v6.1.sh}"
ROOT="${BUILD_ROOT:-${TMPDIR:-/tmp}/dsp-nvs-checks}"
MATRIX=0
[ "${1:-}" = "--matrix" ] && MATRIX=1
mkdir -p "$ROOT"
status=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; status=1; }

build() { # variant
    local unpatched=0
    [ "$1" = unpatched ] && unpatched=1
    NVS_UNPATCHED=$unpatched HERE="$HERE" B="$ROOT/$1" IDF_ACTIVATE="$IDF_ACTIVATE" bash -c '
        . "$IDF_ACTIVATE" >/dev/null 2>&1 || exit 3
        cd "$HERE" && idf.py -B "$B" -D SDKCONFIG="$B/sdkconfig" build' > "$ROOT/$1-build.log" 2>&1
}
run() { # variant mode [IMG]
    find "$ROOT/$1" -name '*.gcda' -delete 2>/dev/null
    (cd "$ROOT" && MODE="$2" IMG="${3:-}" "$ROOT/$1/dsp_nvs_linux.elf" > "$ROOT/$1-$2.log" 2>&1)
}
compiled_from() { grep -o '[^ "]*nvs_flash/src/nvs_page.cpp' "$ROOT/$1/compile_commands.json" | head -1; }

for v in patched unpatched; do
    if build $v; then pass "$v build"; else fail "$v build (see $ROOT/$v-build.log)"; exit 1; fi
done
case "$(compiled_from patched)" in
    "$ROOT/patched/dsp_nvs_flash/nvs_flash/src/nvs_page.cpp") pass "patched build compiles the patched copy";;
    *) fail "patched build compiles $(compiled_from patched)";;
esac
case "$(compiled_from unpatched)" in
    */components/nvs_flash/src/nvs_page.cpp) pass "unpatched build compiles ESP-IDF's component";;
    *) fail "unpatched build compiles $(compiled_from unpatched)";;
esac

# Checks 1-5
if run patched ""; then pass "patched: checks 1-5 ($(tail -1 "$ROOT/patched-.log"))"
else fail "patched: checks 1-5 ($(tail -1 "$ROOT/patched-.log"))"; fi
run unpatched ""; rc=$?
if [ $rc -ne 0 ] && grep -aq "5. raw NVS total lost/torn: 14$" "$ROOT/unpatched-.log" &&
   grep -aq "3. format 1 -> 2 upgrade: cut at cycle .*: storage open failed" "$ROOT/unpatched-.log"; then
    pass "unpatched: the known defect is reproduced (14/14 plain-NVS scenarios, CORRUPT after the upgrade cut)"
else
    fail "unpatched: the known defect was NOT reproduced (negative control broken)"
fi

# Compatibility: identical images, cross-loading both ways
run patched dump "$ROOT/img-patched.bin" && run unpatched dump "$ROOT/img-unpatched.bin" ||
    fail "image dump"
if cmp -s "$ROOT/img-patched.bin" "$ROOT/img-unpatched.bin" &&
   cmp -s "$ROOT/img-patched.bin.cut" "$ROOT/img-unpatched.bin.cut"; then
    pass "identical flash images (workload and unrecovered cut state)"
else
    fail "flash images differ between patched and unpatched"
fi
if run patched load "$ROOT/img-unpatched.bin" && grep -aq "0 differ" "$ROOT/patched-load.log"; then
    pass "patched reads and recovers images written by the unpatched component"
else fail "patched cannot read the unpatched images"; fi
if run unpatched load "$ROOT/img-patched.bin" && grep -aq "0 differ" "$ROOT/unpatched-load.log"; then
    pass "unpatched reads and recovers images written by the patched component"
else fail "unpatched cannot read the patched images"; fi

if [ $MATRIX -eq 1 ]; then
    run patched matrix & p1=$!
    run unpatched matrix & p2=$!
    wait $p1; rc_p=$?
    wait $p2; rc_u=$?
    if [ $rc_p -eq 0 ] &&
       grep -aq "6. TOTAL: .* lost after 1 cut 0, after 2 cuts 0, unstable 0, follow-up writes lost 0, boot failures 0" "$ROOT/patched-matrix.log" &&
       grep -aq "7. TOTAL: .* lost after 1 cut 0, after 2 cuts 0, unstable 0, follow-up writes lost 0, boot failures 0" "$ROOT/patched-matrix.log" &&
       grep -aq "8. #19213 hash check: .* 0 colliding pairs" "$ROOT/patched-matrix.log"; then
        pass "patched: two-cut matrices (both tear models) and hash check"
        grep -a "TOTAL" "$ROOT/patched-matrix.log" | sed 's/^/        /'
    else fail "patched: two-cut matrices ($(tail -1 "$ROOT/patched-matrix.log"))"; fi
    if [ $rc_u -ne 0 ] && grep -aq "6. TOTAL: .* lost after 1 cut [1-9]" "$ROOT/unpatched-matrix.log" &&
       grep -aq "7. TOTAL: .* follow-up writes lost [1-9]" "$ROOT/unpatched-matrix.log"; then
        pass "unpatched: the matrix reproduces the defect"
        grep -a "TOTAL" "$ROOT/unpatched-matrix.log" | sed 's/^/        /'
    else fail "unpatched: the matrix did NOT reproduce the defect"; fi
fi

echo "logs: $ROOT"
exit $status
