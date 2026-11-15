#!/usr/bin/env bash
# Prints the CHANGELOG.md section of one version (GitHub release notes).
#
#   tools/release-notes.sh 0.5.0-beta.1 [path/to/CHANGELOG.md]
#
# Fails if the section "## [<version>] – <date>" is missing or empty, so a release can never
# be published with the "Nevydáno" (unreleased) notes by mistake.
set -euo pipefail

version="${1:?usage: $0 <version> [CHANGELOG.md]}"
changelog="${2:-$(dirname "$0")/../CHANGELOG.md}"
version="${version#v}"

notes="$(awk -v v="$version" '
    index($0, "## [" v "]") == 1 { found = 1; next }
    found && /^## \[/ { exit }
    found { print }
' "$changelog")"

if ! grep -q '[^[:space:]]' <<<"$notes"; then
    echo "error: no non-empty section '## [$version]' in $changelog" >&2
    echo "Move the released items from '## [Nevydáno]' under '## [$version] – YYYY-MM-DD' first." >&2
    exit 1
fi

# Trim leading/trailing blank lines.
sed -e '/./,$!d' <<<"$notes" | sed -e ':a' -e '/^\n*$/{$d;N;ba' -e '}'
