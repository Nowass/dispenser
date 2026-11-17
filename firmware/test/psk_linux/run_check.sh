#!/usr/bin/env bash
# Device HKDF provider check on ESP-IDF's linux target (M1.10c.2a). Requires ESP-IDF v6.1
# (IDF_ACTIVATE, default ~/.espressif/tools/activate_idf_v6.1.sh) and the host's libbsd headers
# (CPATH/LIBRARY_PATH, as for test/nvs_linux). BUILD_ROOT defaults to ${TMPDIR:-/tmp}/dsp-psk-linux.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IDF_ACTIVATE="${IDF_ACTIVATE:-$HOME/.espressif/tools/activate_idf_v6.1.sh}"
B="${BUILD_ROOT:-${TMPDIR:-/tmp}/dsp-psk-linux}"
mkdir -p "$B"
HERE="$HERE" B="$B" IDF_ACTIVATE="$IDF_ACTIVATE" bash -c '
    . "$IDF_ACTIVATE" >/dev/null 2>&1 || exit 3
    cd "$HERE" && idf.py -B "$B" -D SDKCONFIG="$B/sdkconfig" build' > "$B/build.log" 2>&1 || {
    echo "FAIL  build (see $B/build.log)"; exit 1; }
"$B/dsp_psk_linux.elf"
