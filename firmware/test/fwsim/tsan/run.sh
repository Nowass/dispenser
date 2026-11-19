#!/usr/bin/env bash
# Build and run the firmware-simulator ThreadSanitizer driver (TEST-ONLY; see README.md).
#   test/fwsim/tsan/run.sh [rounds] [repeats]      (defaults: 200 rounds, 3 repeats)
# Environment: DSP_JDK_HOME=<jdk> to use a specific JDK's jni.h (otherwise CMake's FindJNI),
#              BUILD_DIR (default: build-fwsim-tsan/ in the firmware directory, git-ignored;
#              kept outside test/fwsim, which the Gradle buildFirmwareSim task hashes as input).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
rounds="${1:-200}"
repeats="${2:-3}"
fw="$(cd "$here/../../.." && pwd)"
build="${BUILD_DIR:-$fw/build-fwsim-tsan}"

cmake -S "$here" -B "$build" ${DSP_JDK_HOME:+-DDSP_JDK_HOME="$DSP_JDK_HOME"} >/dev/null
cmake --build "$build" -j"$(nproc)" >/dev/null

# The TSan runtime refuses high-entropy ASLR layouts on recent kernels ("unexpected memory
# mapping"); run without address randomization. Any report aborts the run with a non-zero status.
export TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1 ${TSAN_OPTIONS:-}"
for i in $(seq 1 "$repeats"); do
    printf 'run %d/%d: ' "$i" "$repeats"
    setarch "$(uname -m)" -R "$build/fwsim_tsan" "$rounds"
done
echo "fwsim_tsan: $repeats x $rounds rounds, 0 ThreadSanitizer reports, 0 failures"
