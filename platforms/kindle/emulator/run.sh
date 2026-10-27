#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/../../.." && pwd)
if [[ $# == 1 && $1 =~ ^[a-z][a-z0-9-]{0,39}$ ]]; then
    name=$1
    cache=$root/build/kindle-emulator
    source_dir=$cache/eink-emulator-81ae873f57ea5a8aba71a00774118dc3ad8462e7
    [[ -f $cache/image.tag ]] || { echo 'Run emulator/prepare.sh first.' >&2; exit 1; }
    image=$(cat "$cache/image.tag")
    [[ $image =~ ^kobalt-kindle-emulator:81ae873-v1-(arm64|amd64)$ ]] || exit 1
    export DOCKER_CONFIG=${DOCKER_CONFIG:-$root/build/kindle-sdk/docker-config}
    container=kobalt-kindle-emulator-$name
    if docker container inspect "$container" >/dev/null 2>&1; then
        echo "Container already exists: $container" >&2; exit 1
    fi
    if [[ -e $source_dir/machines/$name || -L $source_dir/machines/$name ]]; then
        echo "Machine already exists: $name" >&2; exit 1
    fi
    run_dir=$(mktemp -d "$cache/run-$name.XXXXXX")
    docker_pid=
    cleanup() {
        if [[ -s $run_dir/container.id ]]; then
            docker stop --time 20 "$(cat "$run_dir/container.id")" >/dev/null 2>&1 || true
            docker rm -f "$(cat "$run_dir/container.id")" >/dev/null 2>&1 || true
        fi
        if [[ -n $docker_pid ]]; then
            wait "$docker_pid" 2>/dev/null || true
        fi
        rm -rf "$run_dir"
        if [[ -e $source_dir/machines/$name ]]; then
            echo "Guest cleanup did not finish: $source_dir/machines/$name" >&2
            return 1
        fi
    }
    trap cleanup EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    printf 'Container: %s\n' "$container"
    printf 'SSH: docker exec -it %s ssh -F /dev/null -i /emu/build/ssh/id_ed25519 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p 2222 root@127.0.0.1\n' "$container"
    docker run --rm --pull=never --init --stop-timeout 20 --name "$container" \
        --cidfile "$run_dir/container.id" --label kobalt.purpose=kindle-emulator-run \
        -v "$root:/work" -v "$root/build/kindle-sdk:/sdk" \
        -v "$source_dir:/emu" "$image" bash /work/platforms/kindle/emulator/run.sh --inside "$name" &
    docker_pid=$!
    status=0
    wait "$docker_pid" || status=$?
    exit "$status"
fi
if [[ $# != 2 || $1 != --inside || $root != /work || ! $2 =~ ^[a-z][a-z0-9-]{0,39}$ ]]; then
    echo 'Usage: bash platforms/kindle/emulator/run.sh NAME' >&2
    exit 2
fi
name=$2
exec 9>"/work/build/kindle-emulator/run-$name.lock"
flock -n 9 || { echo "Machine is already owned by another run: $name" >&2; exit 1; }
machine=/emu/machines/$name
[[ ! -e $machine && ! -L $machine ]] || { echo "Machine already exists: $name" >&2; exit 1; }
created=0
qemu_pid=
cleanup() {
    if [[ -n $qemu_pid ]]; then
        kill -TERM "$qemu_pid" 2>/dev/null || true
        wait "$qemu_pid" 2>/dev/null || true
    fi
    if [[ $created == 1 ]]; then
        # Upstream delete also prunes shared bases. Remove only this run's guest.
        rm -rf "$machine"
        [[ ! -e $machine ]] || { echo "Failed to remove $machine" >&2; return 1; }
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
cd /emu
created=1
./eink create "$name" --model kindle-paperwhite-5 --profile production --firmware 5.19.2 --jbsh
./eink run "$name" --headless --qmp-socket --serial-socket &
qemu_pid=$!
status=0
wait "$qemu_pid" || status=$?
qemu_pid=
exit "$status"
