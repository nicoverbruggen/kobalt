#!/bin/bash
# Builds and runs the full native test suite on Linux, in the prepared kobuild
# Linux image. CI runs it before the package builds. Set KOBALT_LINUX_IMAGE to
# use another prepared image, such as a local kobuild build; it is not pulled.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
engine=${CONTAINER_ENGINE:-docker}
image=${KOBALT_LINUX_IMAGE:-}
if [[ -z "$image" ]]; then
    image=$(sh platforms/sdk-image.sh linux)
    "$engine" pull "$image"
fi
cache=$("${CMAKE_COMMAND:-cmake}" -P dependencies/cache.cmake)
mkdir -p "$cache"
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo 'JOBS must be positive' >&2; exit 2; }
"$engine" run --rm --pull=never --user "$(id -u):$(id -g)" -e HOME=/tmp \
    --volume "$root:/work" --volume "$cache:/dependency-cache" \
    --workdir /work/platforms/desktop "$image" bash -euc '
        cmake --preset linux -DKOBALT_DEPENDENCY_CACHE=/dependency-cache -DKOBALT_DEPENDENCY_JOBS="$1"
        cmake --build --preset linux-tests --parallel "$1"
        ctest --preset linux --parallel "$1"
    ' native-tests "$jobs"
