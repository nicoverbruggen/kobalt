#!/bin/bash
set -euo pipefail
if [[ $# -ne 1 || ! "$1" =~ ^qt[56](-eval|-diagnostic)?$ ]]; then
    echo "Usage: bash platforms/kobo/build.sh qt5|qt6|qt5-eval|qt6-eval|qt5-diagnostic|qt6-diagnostic" >&2
    exit 2
fi
target=$1
root=$(cd "$(dirname "$0")/../.." && pwd)
jobs=${JOBS:-2}
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo "JOBS must be positive" >&2; exit 2; }
image=$(sh "$root/platforms/sdk-image.sh" "${target%%-*}")
engine=${CONTAINER_ENGINE:-docker}
sdk_id=$("$engine" image inspect "$image" --format '{{.Id}}')
info=$(cmake -P "$root/platforms/source-info.cmake")
read -r commit modified KOBALT_RELEASE_VERSION <<< "$info"
export KOBALT_RELEASE_VERSION
# Own exactly this container, including when the foreground client is interrupted.
mkdir -p "$root/build"
run_dir=$(mktemp -d "$root/build/kobo-run.XXXXXX")
cleanup() {
    result=$?
    trap - EXIT INT TERM
    if [[ -s "$run_dir/container.cid" ]]; then
        if ! "$engine" rm -f "$(cat "$run_dir/container.cid")" > /dev/null 2>&1; then
            echo "Cannot remove the build container; its ID is retained in $run_dir/container.cid" >&2
            exit 1
        fi
    fi
    rm -rf "$run_dir"
    exit "$result"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
"$engine" run --cidfile "$run_dir/container.cid" --pull=never --user "$(id -u):$(id -g)" -e HOME=/tmp -e KOBALT_RELEASE_VERSION \
    --volume "$root:/work" \
    --workdir /work/platforms/kobo "$image" bash -euc '
        started=$SECONDS
        cmake --preset "$1" \
            -DKOBALT_SDK_ID="$5" -DKOBALT_DEPENDENCY_JOBS="$2" -DKOBALT_COMMIT="$3" -DKOBALT_COMMIT_MODIFIED="$4"
        echo "Configure: $((SECONDS - started)) seconds"
        started=$SECONDS
        cmake --build --preset "$1" --parallel "$2"
        cmake --build --preset "$1" --target check-package
        echo "Build and package: $((SECONDS - started)) seconds"
    ' kobalt-build "$target" "$jobs" "$commit" "$modified" "$sdk_id"
