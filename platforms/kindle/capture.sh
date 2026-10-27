#!/usr/bin/env bash
set -euo pipefail
[[ $# == 0 ]] || { echo 'Usage: bash platforms/kindle/capture.sh' >&2; exit 2; }
root=$(cd "$(dirname "$0")/../.." && pwd)
app=$root/build/kindle-native/bin/kobalt
output=$root/build/screenshots/kindle
[[ -x "$app" ]] || { echo 'Build the kindle-native preset first. See doc/building.md.' >&2; exit 1; }
if [[ -e "$output" ]] && ! grep -q '"kind": "kobalt-kindle-screenshots"' "$output/manifest.json"; then
    echo "Refusing to replace an unrelated screenshot directory: $output" >&2
    exit 1
fi
mkdir -p "$root/build/kindle-native/test-tmp" "$root/build/screenshots"
profile=$(mktemp -d "$root/build/kindle-native/test-tmp/capture.XXXXXX")
trap 'rm -rf "$profile"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$profile/tmp" "$profile/screens"
QT_QPA_PLATFORM=minimal:enable_fonts:freetype TMPDIR="$profile/tmp" "$app" \
    --screenshots "$profile/screens" --profile "$profile/data" \
    --assets "$root/build/kindle-native/bin/assets" --library "$root/tests/fixtures" \
    --book "$root/tests/fixtures/trevelyans-type-tester.kepub.epub" >"$profile/screens/capture.log" 2>&1 || {
        cat "$profile/screens/capture.log" >&2
        exit 1
    }
if [[ -d "$output" ]]; then mv "$output" "$profile/previous"; fi
if ! mv "$profile/screens" "$output"; then
    if [[ -d "$profile/previous" ]]; then mv "$profile/previous" "$output"; fi
    exit 1
fi
echo "Kindle screenshots: $output"
