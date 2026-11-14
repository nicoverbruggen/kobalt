#!/bin/bash
# Configure an extracted release using only its bundled sources and prepared SDK.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
archive=${1:-$root/build/release/Kobalt-sources.tar.gz}
[[ $# -le 1 && -f $archive ]] || { echo 'Expected a Kobalt source archive' >&2; exit 2; }
mkdir -p "$root/build"
scratch=$(mktemp -d "$root/build/source-archive-check.XXXXXX")
trap 'cmake -DPATHS="$scratch" -P "$root/platforms/cleanup.cmake"' EXIT
tar -xzf "$archive" -C "$scratch"
applications=("$scratch"/*/application)
[[ ${#applications[@]} == 1 && -d ${applications[0]} ]] || { echo 'Invalid source archive layout' >&2; exit 1; }
engine=${CONTAINER_ENGINE:-docker}
image=$(sh "$root/platforms/sdk-image.sh" qt6)
"$engine" image inspect "$image" > /dev/null
"$engine" run --rm --pull=never --network none --user "$(id -u):$(id -g)" -e HOME=/tmp \
    --volume "${applications[0]}:/work" --workdir /work/platforms/kobo "$image" bash -euc '
        [[ ! -e /work/.git && -f /work/source-info.json ]]
        [[ -f /work/src/engine/blaze/crengine/include/lvdocview.h ]]
        [[ ! -e /work/dependencies/crengine-patches.txt && ! -e /work/upstream/crengine.archive ]]
        info=$(cmake -P /work/platforms/source-info.cmake)
        read -r commit modified version <<< "$info"
        cmake --preset qt6 -DKOBALT_COMMIT="$commit" -DKOBALT_COMMIT_MODIFIED="$modified"
        grep -F "$commit" /work/build/kobo-qt6/build_info.h
        grep -F "$version" /work/build/kobo-qt6/assets/runtime/manifest.json > /dev/null
        mv /work/upstream/harfbuzz.archive /work/upstream/harfbuzz.saved
        if cmake --preset qt6 > /work/missing-source.log 2>&1; then
            echo "Accepted a missing bundled archive" >&2; exit 1
        fi
        grep -F "Missing bundled source archive: harfbuzz" /work/missing-source.log
        cp /work/upstream/harfbuzz.saved /work/upstream/harfbuzz.archive
        printf damage >> /work/upstream/harfbuzz.archive
        if cmake --preset qt6 > /work/changed-source.log 2>&1; then
            echo "Accepted a changed bundled archive" >&2; exit 1
        fi
        grep -F "Bundled source checksum mismatch: harfbuzz" /work/changed-source.log
    '
echo 'PASS: Git-free release source configuration with networking disabled'
