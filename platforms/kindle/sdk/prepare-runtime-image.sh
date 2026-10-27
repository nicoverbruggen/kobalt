#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/../../.." && pwd)
export DOCKER_CONFIG=${DOCKER_CONFIG:-$root/build/kindle-sdk/docker-config}
mkdir -p "$DOCKER_CONFIG"
case "$(docker info --format '{{.Architecture}}')" in
    aarch64|arm64) arch=arm64; base=arm64v8/ubuntu:24.04 ;;
    x86_64|amd64) arch=amd64; base=amd64/ubuntu:24.04 ;;
    *) printf 'Kindle verification requires an ARM64 or AMD64 Docker engine.\n' >&2; exit 1 ;;
esac
image=kobalt-kindle-runtime:pw5-5.19.2-v1-$arch
if [ "${REBUILD_RUNTIME_IMAGE:-0}" = 1 ] || ! docker image inspect "$image" >/dev/null 2>&1; then
    docker build --platform "linux/$arch" --build-arg "KOBALT_RUNTIME_BASE=$base" \
        -f "$root/platforms/kindle/sdk/Dockerfile.runtime" -t "$image" \
        "$root/platforms/kindle/sdk" >&2
fi
if [ "$(docker image inspect "$image" --format '{{.Architecture}}')" != "$arch" ]; then
    printf 'The Kindle runtime image must match the Docker engine architecture.\n' >&2
    exit 1
fi
printf '%s\n' "$image"
