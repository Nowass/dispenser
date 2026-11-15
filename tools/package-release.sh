#!/usr/bin/env bash
# Verifies the locally signed release APK and prepares the GitHub Release assets in dist/.
#
#   cd android && ./gradlew clean assembleRelease      # with local signing configured
#   tools/package-release.sh [--expected-cert-sha256 <hex>]   # default: the release certificate
#
# Produces:
#   dist/dispenser-<version>.apk          signed APK (attach to the release)
#   dist/dispenser-<version>.apk.sha256   checksum in `sha256sum` format (attach)
#   dist/release-notes-<version>.md       notes extracted from CHANGELOG.md
#
# Refuses unsigned, debug or v1-only APKs and (if given) a certificate other than the
# expected release certificate. Nothing is uploaded or published.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
# Public fingerprint of the permanent release certificate (docs/RELEASING.md). Overridable for tests.
release_cert="32:C2:4A:F8:B4:13:8D:49:CF:E8:D8:C6:95:19:A2:6D:44:2D:F6:4C:AF:F5:BE:E7:C7:29:56:FA:C7:CE:64:29"
expected_cert="${DISPENSER_RELEASE_CERT_SHA256:-$release_cert}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --expected-cert-sha256) expected_cert="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

out="$root/android/app/build/outputs/apk/release"
meta="$out/output-metadata.json"
[[ -f "$meta" ]] || { echo "error: $meta not found; run ./gradlew assembleRelease first" >&2; exit 1; }

read -r version code apk_name < <(python3 -I -c '
import json, sys
e = json.load(open(sys.argv[1]))["elements"][0]
print(e["versionName"], e["versionCode"], e["outputFile"])' "$meta")
apk="$out/$apk_name"

case "$apk_name" in *unsigned*) echo "error: $apk_name is unsigned; configure local release signing (docs/RELEASING.md)" >&2; exit 1 ;; esac
case "$version" in *+*) echo "error: versionName '$version' carries build metadata (debug build?)" >&2; exit 1 ;; esac

# apksigner from ANDROID_HOME / ANDROID_SDK_ROOT / android/local.properties (newest build-tools).
sdk="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
if [[ -z "$sdk" && -f "$root/android/local.properties" ]]; then
    sdk="$(sed -n 's/^sdk\.dir=//p' "$root/android/local.properties")"
fi
apksigner="$(ls -d "$sdk"/build-tools/*/apksigner 2>/dev/null | sort -V | tail -n1 || true)"
[[ -x "$apksigner" ]] || { echo "error: apksigner not found (set ANDROID_HOME)" >&2; exit 1; }

report="$("$apksigner" verify --verbose --print-certs "$apk")" || { echo "error: signature verification failed" >&2; exit 1; }
grep -Eq 'Verified using v(2|3) scheme.*: true' <<<"$report" || { echo "error: APK is not signed with scheme v2/v3" >&2; exit 1; }
cert="$(sed -n 's/^Signer #1 certificate SHA-256 digest: //p' <<<"$report" | head -n1)"
[[ -n "$cert" ]] || { echo "error: could not read the signer certificate" >&2; exit 1; }
if grep -q 'CN=Android Debug' <<<"$report"; then echo "error: APK is signed with a debug certificate" >&2; exit 1; fi

if [[ -n "$expected_cert" ]]; then
    norm() { tr -d ': ' <<<"$1" | tr '[:upper:]' '[:lower:]'; }
    [[ "$(norm "$cert")" == "$(norm "$expected_cert")" ]] || { echo "error: signer certificate $cert does not match the expected release certificate" >&2; exit 1; }
else
    echo "warning: no expected certificate given; compare manually with docs/RELEASING.md" >&2
fi

notes="$("$root/tools/release-notes.sh" "$version" "$root/CHANGELOG.md")"

mkdir -p "$root/dist"
name="dispenser-$version.apk"
cp "$apk" "$root/dist/$name"
(cd "$root/dist" && sha256sum "$name" > "$name.sha256")
printf '%s\n' "$notes" > "$root/dist/release-notes-$version.md"

echo "version:            $version (versionCode $code)"
echo "certificate SHA-256: $cert"
echo "APK SHA-256:        $(cut -d' ' -f1 "$root/dist/$name.sha256")"
echo "assets:             dist/$name, dist/$name.sha256, dist/release-notes-$version.md"
