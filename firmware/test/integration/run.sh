#!/usr/bin/env bash
# M1.8 notification integration suite: one command, local services only.
#
#   firmware/test/integration/run.sh [--sessions N]     (default: 1000 stress sessions)
#
# Builds notify_harness (ASan/UBSan) in a temporary directory, then runs notify_e2e.mjs:
# throwaway certificates, D1 database and synthetic token in another temporary directory,
# wrangler dev --local, fault proxy and fake Gmail on 127.0.0.1. Never contacts Cloudflare,
# production D1 or Google. Every temporary directory and child process is removed at the
# end (also on failure or Ctrl-C). Exit code: 0 = all checks passed, non-zero otherwise.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE="$(cd "$HERE/../.." && pwd)"
BACKEND="$(cd "$FIRMWARE/../cloudflare/dispenser-notify" && pwd)"

fail() { echo "run.sh: $*" >&2; exit 2; }

command -v node >/dev/null || fail "Node.js >= 22.12 is required"
node -e 'const [a,b]=process.versions.node.split(".").map(Number); process.exit(a>22||(a===22&&b>=12)?0:1)' \
  || fail "Node.js >= 22.12 is required (found $(node --version))"
command -v openssl >/dev/null || fail "the openssl command is required"
command -v cmake >/dev/null || fail "CMake >= 3.22 is required"
[ -x "$BACKEND/node_modules/.bin/wrangler" ] || fail "run 'npm ci' in cloudflare/dispenser-notify first"

BUILD="$(mktemp -d "${TMPDIR:-/tmp}/dispenser-m18-build-XXXXXX")"
trap 'rm -rf "$BUILD"' EXIT

echo "building notify_harness (ASan/UBSan) ..."
cmake -S "$FIRMWARE/test/host" -B "$BUILD" >/dev/null
cmake --build "$BUILD" --target notify_harness -j"$(nproc 2>/dev/null || echo 2)" >/dev/null
[ -x "$BUILD/notify_harness" ] || fail "notify_harness was not built (OpenSSL development files missing?)"

status=0
node --no-warnings "$HERE/notify_e2e.mjs" "$BUILD/notify_harness" "$@" || status=$?
exit "$status"
