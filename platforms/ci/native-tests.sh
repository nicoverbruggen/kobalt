#!/bin/bash
# Builds and runs the full native test suite on Linux, in the prepared kobuild
# Linux image. CI runs it before the package builds. Set KOBALT_LINUX_IMAGE to
# use another prepared image, such as a local kobuild build; it is not pulled.
set -euo pipefail
mode=${1:-all}
case "$mode" in all|build|test) ;; *) echo 'Expected all, build or test' >&2; exit 2 ;; esac
shard=${KOBALT_TEST_SHARD:-1}
shards=${KOBALT_TEST_SHARDS:-1}
[[ "$shard" =~ ^[1-9][0-9]*$ && "$shards" =~ ^[1-9][0-9]*$ && "$shard" -le "$shards" ]] || { echo 'Invalid test shard' >&2; exit 2; }
compiler_cache=${KOBALT_COMPILER_CACHE:-0}
[[ "$compiler_cache" == 0 || "$compiler_cache" == 1 ]] || exit 2
linker=${KOBALT_NATIVE_LINKER:-default}
[[ "$linker" == default || "$linker" == lld ]] || exit 2
autogen=${KOBALT_NATIVE_AUTOGEN_DEPENDS:-ON}
[[ "$autogen" == ON || "$autogen" == OFF ]] || exit 2
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
# Tags require every variant, including manual runs on a tag. Branch CI
# keeps representative variants; local runs may select either mode.
case "${GITHUB_REF:-}" in
    refs/tags/*) variants=1 ;;
    refs/heads/*) variants=0 ;;
    *) variants=${KOBALT_TEST_ALL_VARIANTS:-0} ;;
esac
[[ "$variants" == 0 || "$variants" == 1 ]] || { echo 'KOBALT_TEST_ALL_VARIANTS must be 0 or 1' >&2; exit 2; }
echo "Native test variants: $([[ $variants == 1 ]] && echo all || echo representative)"
engine=${CONTAINER_ENGINE:-docker}
image=${KOBALT_LINUX_IMAGE:-}
if [[ -z "$image" ]]; then
    image=$(sh platforms/sdk-image.sh linux)
    "$engine" pull "$image"
fi
sdk_id=$("$engine" image inspect "$image" --format '{{.Id}}')
cache=$("${CMAKE_COMMAND:-cmake}" -P dependencies/cache.cmake)
mkdir -p "$cache"
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo 'JOBS must be positive' >&2; exit 2; }
"$engine" run --rm --pull=never --user "$(id -u):$(id -g)" -e HOME=/tmp -e KOBALT_TEST_ALL_VARIANTS="$variants" \
    -e KOBALT_BENCHMARK_LINKERS="${KOBALT_BENCHMARK_LINKERS:-0}" \
    -e CCACHE_DIR=/work/build/ccache -e CCACHE_MAXSIZE=500M -e CCACHE_COMPILERCHECK=content \
    --volume "$root:/work" --volume "$cache:/dependency-cache" \
    --workdir /work/platforms/desktop "$image" bash -euc '
        jobs=$1; sdk_id=$2; mode=$3; compiler_cache=$4; linker=$5; autogen=$6; shard=$7; shards=$8
        if [[ "$mode" != test ]]; then
            options=(-DCMAKE_AUTOGEN_ORIGIN_DEPENDS="$autogen")
            if [[ "$compiler_cache" == 1 ]]; then
                command -v ccache
                options+=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
                ccache --zero-stats
            else
                options+=(-DCMAKE_C_COMPILER_LAUNCHER= -DCMAKE_CXX_COMPILER_LAUNCHER=)
            fi
            flags=
            [[ "$linker" != lld ]] || flags=-fuse-ld=lld
            options+=(-DCMAKE_EXE_LINKER_FLAGS="$flags" -DCMAKE_SHARED_LINKER_FLAGS="$flags")
            started=$SECONDS
            cmake --preset linux -DKOBALT_DEPENDENCY_CACHE=/dependency-cache -DKOBALT_DEPENDENCY_JOBS="$jobs" -DKOBALT_SDK_ID="$sdk_id" "${options[@]}"
            echo "Native configure: $((SECONDS - started)) seconds"
            started=$SECONDS
            cmake --build --preset linux-tests --parallel "$jobs"
            echo "Native compile/link: $((SECONDS - started)) seconds"
            [[ "$compiler_cache" != 1 ]] || ccache --show-stats
            if [[ "$KOBALT_BENCHMARK_LINKERS" == 1 ]]; then
                # Alternate linkers on the same runner and compiled objects.
                # No timings from these extra links belong to the normal build.
                for probe in default lld default lld; do
                    probe_flags=
                    [[ "$probe" != lld ]] || probe_flags=-fuse-ld=lld
                    cmake --preset linux -DCMAKE_EXE_LINKER_FLAGS="$probe_flags" -DCMAKE_SHARED_LINKER_FLAGS="$probe_flags"
                    started=$SECONDS
                    cmake --build --preset linux-tests --parallel "$jobs"
                    echo "Native relink ($probe): $((SECONDS - started)) seconds"
                done
                readelf --string-dump=.comment /work/build/desktop-linux/kobalt-desktop-tests
                cmake --preset linux -DCMAKE_EXE_LINKER_FLAGS="$flags" -DCMAKE_SHARED_LINKER_FLAGS="$flags"
                cmake --build --preset linux-tests --parallel "$jobs"
            fi
        fi
        if [[ "$mode" != build ]]; then
            # CTest indices partition every test exactly once, including tag variants.
            ctest --preset linux --parallel "$jobs" -I "$shard,,$shards"
        fi
    ' native-tests "$jobs" "$sdk_id" "$mode" "$compiler_cache" "$linker" "$autogen" "$shard" "$shards"
