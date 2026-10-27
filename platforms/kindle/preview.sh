#!/usr/bin/env bash
set -euo pipefail
[[ $# -le 1 ]] || { echo 'Usage: bash platforms/kindle/preview.sh [EPUB-folder]' >&2; exit 2; }
root=$(cd "$(dirname "$0")/../.." && pwd)
app=$root/build/kindle-native/bin/kobalt
[[ -x "$app" ]] || { echo 'Build the kindle-native preset first. See doc/building.md.' >&2; exit 1; }
library=${1:-$root/tests/fixtures}
[[ -d "$library" ]] || { echo "Library folder does not exist: $library" >&2; exit 1; }
mkdir -p "$root/build/kindle-preview/profile" "$root/build/kindle-preview/tmp"
export TMPDIR="$root/build/kindle-preview/tmp"
export QT_QPA_PLATFORM=cocoa
exec "$app" --preview --assets "$root/build/kindle-native/bin/assets" \
    --profile "$root/build/kindle-preview/profile" --library "$library"
