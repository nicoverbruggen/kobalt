#!/bin/bash
set -euo pipefail
if [[ $# -lt 1 || $# -gt 2 || ! "$1" =~ ^qt[56](-eval|-diagnostic)?$ || ( $# -eq 2 && "$2" != --check ) ]]; then
    echo "Usage: bash platforms/kobo/build.sh qt5|qt6|qt5-eval|qt6-eval|qt5-diagnostic|qt6-diagnostic [--check]" >&2
    exit 2
fi
target=$1
mode=${2:-package}
diagnostics=${KOBALT_VERBOSE_DIAGNOSTICS:-ON}
[[ "$target" != *-diagnostic ]] || diagnostics=ON
[[ "$diagnostics" == ON || "$diagnostics" == OFF ]] || { echo 'KOBALT_VERBOSE_DIAGNOSTICS must be ON or OFF' >&2; exit 2; }
root=$(cd "$(dirname "$0")/../.." && pwd)
# Register the build, so a performance evaluation waits for it; see platforms/load-lock.cmake.
if [[ -z "${KOBALT_LOAD_LOCK_HELD:-}" ]]; then
    exec 3< <("${CMAKE_COMMAND:-cmake}" "-DROOT=$root" "-DOWNER=$$" -P "$root/platforms/load-lock.cmake")
    read -r registered <&3 && [[ "$registered" == registered ]] || { echo 'Cannot register the build' >&2; exit 1; }
fi
if [[ "$mode" == --check && -n "${KOBALT_CHECK_SOURCE_ROOT:-}" ]]; then
    [[ -f "$KOBALT_CHECK_SOURCE_ROOT/platforms/kobo/CMakeLists.txt" ]] || { echo 'Invalid check source root' >&2; exit 2; }
    root=$(cd "$KOBALT_CHECK_SOURCE_ROOT" && pwd)
fi
jobs=${JOBS:-2}
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo "JOBS must be positive" >&2; exit 2; }
[[ "${KOBALT_DOCKER_CPUS:-1}" =~ ^[0-9]+(\.[0-9]+)?$ ]] || { echo "KOBALT_DOCKER_CPUS must be a number of CPUs" >&2; exit 2; }
image=$(sh "$root/platforms/sdk-image.sh" "${target%%-*}")
engine=${CONTAINER_ENGINE:-docker}
sdk_id=$("$engine" image inspect "$image" --format '{{.Id}}')
# A package that completes is recorded for the validation guard.
record() {
    [[ "$mode" == --check ]] || "${CMAKE_COMMAND:-cmake}" "-DMODE=$1" "-DRUN=kobo:$target" "-DDIAGNOSTICS=$diagnostics" \
        "-DVERSION=$requested_version" -P "$root/platforms/validation-record.cmake" || true
}
requested_version=${KOBALT_RELEASE_VERSION:-}
record begin
info=$("${CMAKE_COMMAND:-cmake}" -P "$root/platforms/source-info.cmake")
# The shared dependency cache is created on the host, so the host user owns it.
# Containers mount it at one fixed path, the path stored in its CMake files.
cache=$("${CMAKE_COMMAND:-cmake}" -P "$root/dependencies/cache.cmake")
mkdir -p "$cache"
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
# Leave CPUs for the rest of the machine: half of the host's, at least two and
# at most what the engine has.
cpus=${KOBALT_DOCKER_CPUS:-}
if [[ -z "$cpus" ]]; then
    host_cpus=$(sysctl -n hw.ncpu 2> /dev/null || getconf _NPROCESSORS_ONLN)
    cpus=$((host_cpus / 2 > 2 ? host_cpus / 2 : 2))
    engine_cpus=$("$engine" info --format '{{.NCPU}}' 2> /dev/null || true)
    [[ ! "$engine_cpus" =~ ^[1-9][0-9]*$ || "$engine_cpus" -ge "$cpus" ]] || cpus=$engine_cpus
fi
"$engine" run --cpus "$cpus" --cidfile "$run_dir/container.cid" --pull=never --user "$(id -u):$(id -g)" -e HOME=/tmp -e KOBALT_RELEASE_VERSION -e KOBALT_ENGINE_SOURCE \
    --volume "$root:/work" \
    --volume "$cache:/dependency-cache" \
    --workdir /work/platforms/kobo "$image" bash -euc '
        started=$SECONDS
        tests=()
        [[ "$7" != --check ]] || tests=(-DKOBALT_BUILD_TESTS=ON)
        cmake --preset "$1" ${tests[@]+"${tests[@]}"} \
            -DKOBALT_SDK_ID="$5" -DKOBALT_DEPENDENCY_JOBS="$2" -DKOBALT_COMMIT="$3" -DKOBALT_COMMIT_MODIFIED="$4" \
            -DKOBALT_VERBOSE_DIAGNOSTICS="$6" -DKOBALT_DEPENDENCY_CACHE=/dependency-cache
        echo "Configure: $((SECONDS - started)) seconds"
        started=$SECONDS
        if [[ "$7" == --check ]]; then
            cmake --build --preset "$1" --target kobalt kobo-tests --parallel "$2"
            echo "Compile and link check: $((SECONDS - started)) seconds"
            started=$SECONDS
            ctest --test-dir "/work/build/kobo-$1" --output-on-failure
            echo "Adapter tests: $((SECONDS - started)) seconds"
        else
            cmake --build --preset "$1" --parallel "$2"
            cmake --build --preset "$1" --target check-package
            echo "Build and package: $((SECONDS - started)) seconds"
        fi
    ' kobalt-build "$target" "$jobs" "$commit" "$modified" "$sdk_id" "$diagnostics" "$mode"
record record
