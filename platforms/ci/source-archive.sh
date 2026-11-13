#!/bin/bash
# Bundle tracked application sources and checksum-verified upstream inputs.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
version=$(bash platforms/ci/version.sh)
mkdir -p build/release build/release-source-cache
stage=$(mktemp -d "$root/build/release-source.XXXXXX")
trap 'cmake -DPATHS="$stage" -P "$root/platforms/cleanup.cmake"' EXIT
bundle="$stage/Kobalt-$version-sources"
mkdir -p "$bundle/application"
git archive HEAD | tar -xf - -C "$bundle/application"
upstream="$bundle/application/upstream"
mkdir -p "$upstream"
jq -n --arg commit "$(git rev-parse HEAD)" --arg version "$version" \
    '{commit:$commit,version:$version}' > "$bundle/application/source-info.json"
jq -s '.[0] + .[1]' dependencies/manifest.json dependencies/release-sources.json > "$upstream/manifest.json"
while IFS=$'\t' read -r name url checksum; do
    archive="$root/build/release-source-cache/$checksum.archive"
    if [[ ! -f $archive ]]; then
        curl --fail --location --retry 3 --proto '=https' --tlsv1.2 "$url" -o "$stage/download"
        actual=$(shasum -a 256 "$stage/download" | awk '{print $1}')
        [[ $actual == "$checksum" ]] || { echo "Checksum mismatch: $name" >&2; exit 1; }
        mv "$stage/download" "$archive"
    fi
    actual=$(shasum -a 256 "$archive" | awk '{print $1}')
    [[ $actual == "$checksum" ]] || { echo "Invalid cached archive: $name" >&2; exit 1; }
    cp "$archive" "$upstream/$name.archive"
done < <(jq -r 'to_entries[] | [.key,.value.url,.value.sha256] | @tsv' "$upstream/manifest.json")
cp platforms/sdk-image.sh "$bundle/SDK.txt"
printf '%s\n' "Application commit: $(git rev-parse HEAD)" "Version: $version" > "$bundle/SOURCE.txt"
(cd "$stage" && tar -czf "$root/build/release/Kobalt-sources.tar.gz" "Kobalt-$version-sources")
