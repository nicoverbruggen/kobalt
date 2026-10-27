#!/usr/bin/env bash
set -euo pipefail
if [[ $# -ne 0 ]]; then echo "Usage: bash platforms/kindle/build.sh" >&2; exit 2; fi
root=$(cd "$(dirname "$0")/../.." && pwd)
export DOCKER_CONFIG=${DOCKER_CONFIG:-"$root/build/kindle-sdk/docker-config"}
mkdir -p "$DOCKER_CONFIG"
image=${KOBALT_KINDLE_SDK_IMAGE:-localhost/kobuild:kindle}
jobs=${JOBS:-4}
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo 'JOBS must be positive' >&2; exit 2; }
docker image inspect "$image" >/dev/null 2>&1 || {
    echo "Prepare the Kindle SDK image in kobuild first: ./build.sh kindle ($image)" >&2; exit 1;
}
commit=$(git -C "$root" rev-parse --short HEAD)
modified=OFF
if [[ -n $(git -C "$root" status --porcelain) ]]; then modified=ON; fi
docker run --rm --pull=never --platform linux/amd64 --init \
    --label kobalt.purpose=kindle-app \
    -e JOBS="$jobs" -e KOBALT_KINDLE_QT=/sdk/qt -e KOBALT_KINDLE_QT_HOST=/sdk/qt-host \
    -e KOBALT_SOURCE_COMMIT="$commit" -e KOBALT_SOURCE_MODIFIED="$modified" \
    -v "$root:/work" \
    --workdir /work/platforms/kindle "$image" bash -euc '
        mkdir -p /work/build/kindle
        exec 9>/work/build/kindle/application.lock
        flock -n 9 || { echo "Kindle output is already in use" >&2; exit 1; }
        cmake --preset kindle -DKOBALT_DEPENDENCY_JOBS="$JOBS" \
            -DKOBALT_COMMIT="$KOBALT_SOURCE_COMMIT" -DKOBALT_COMMIT_MODIFIED="$KOBALT_SOURCE_MODIFIED"
        cmake --build --preset kindle --parallel "$JOBS"
        cmake --install /work/build/kindle
        cmake -DBUILD=/work/build/kindle -DSDK=/sdk -P package.cmake
        mkdir -p /work/build/kindle/sdk-platforms
        cp /sdk/qt/plugins/platforms/libqoffscreen.so /work/build/kindle/sdk-platforms/
    '
