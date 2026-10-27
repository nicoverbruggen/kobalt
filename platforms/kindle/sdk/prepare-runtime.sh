#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/../../.." && pwd)
if [[ $# == 0 ]]; then
    export DOCKER_CONFIG=${DOCKER_CONFIG:-"$root/build/kindle-sdk/docker-config"}
    mkdir -p "$DOCKER_CONFIG" "$root/build/kindle-sdk"
    image=$(bash "$root/platforms/kindle/sdk/prepare-runtime-image.sh")
    exec docker run --rm --pull=never --init --label kobalt.purpose=kindle-firmware-runtime \
        -v "$root:/work" -v "$root/build/kindle-sdk:/sdk" --workdir /work \
        "$image" bash /work/platforms/kindle/sdk/prepare-runtime.sh --inside
fi
if [[ $# != 1 || "$1" != --inside || "$root" != /work ]]; then
    echo 'Usage: bash platforms/kindle/sdk/prepare-runtime.sh' >&2
    exit 2
fi

exec 9>/sdk/firmware-runtime.lock
flock -n 9 || { echo 'The firmware runtime fixture is already being prepared.' >&2; exit 1; }
downloads=/sdk/downloads
runtime=/sdk/firmware-runtime
mkdir -p "$downloads"
firmware=update_kindle_all_new_paperwhite_11th_5.19.2.bin
firmware_sha=64ddcbae52f0009b5d3f9974d93610db7f26a8787c895c55f413127af474b987
tool_commit=95291295e039a1a3bc0bc9d6040b07f640dae74a
tool_archive=KindleTool-$tool_commit.tar.gz
tool_sha=011ae7463000bc3bb602321afa9bb13094dce25f43ca7621f6277b0c2a649e87

fetch() {
    local name=$1 url=$2 digest=$3
    if [[ ! -f "$downloads/$name" ]]; then
        curl --fail --location --retry 3 --output "$downloads/$name.partial" "$url"
        printf '%s  %s\n' "$digest" "$downloads/$name.partial" | sha256sum --check --status
        mv "$downloads/$name.partial" "$downloads/$name"
    fi
    printf '%s  %s\n' "$digest" "$downloads/$name" | sha256sum --check --status
}
fetch "$firmware" "https://s3.amazonaws.com/firmwaredownloads/$firmware" "$firmware_sha"
[[ $(stat -c %s "$downloads/$firmware") == 361949776 ]] || { echo 'Unexpected firmware size.' >&2; exit 1; }

verify_runtime() {
    local candidate=${1:-$runtime}
    [[ -f "$candidate/firmware.sha256" && $(cat "$candidate/firmware.sha256") == "$firmware_sha" &&
       -f "$candidate/files.sha256" && $(readlink "$candidate/lib/ld-linux-armhf.so.3") == ld-2.20.so ]] || return 1
    (
        cd "$candidate"
        printf '%s  lib/ld-2.20.so\n%s  lib/libc-2.20.so\n' \
            8b36a28d91b6fdb20849869b13befeb0cf1024001c1a4e439189bd73dddf8b4b \
            8357e99917260277a1c137383a4edbdc5bcb4318464cb1f5a2096533aff3b2d9 | sha256sum --check --status || exit 1
        sha256sum --check --status files.sha256
    )
}
if verify_runtime; then
    echo 'Verified cached PW5 firmware 5.19.2 runtime (stock glibc 2.20).'
    exit 0
fi

fetch "$tool_archive" "https://codeload.github.com/NiLuJe/KindleTool/tar.gz/$tool_commit" "$tool_sha"
sources=/sdk/src/firmware-tools/$(uname -m)
mkdir -p "$sources"
tool=$sources/KindleTool-$tool_commit/KindleTool/Release/kindletool
if [[ ! -x "$tool" ]]; then
    tar -xzf "$downloads/$tool_archive" -C "$sources"
    make -C "$sources/KindleTool-$tool_commit/KindleTool" -j2
fi

scratch=$(mktemp -d /sdk/firmware-runtime.prepare.XXXXXX)
cleanup() {
    if [[ -f "$scratch/extract.log" ]]; then
        cp "$scratch/extract.log" /sdk/firmware-runtime-extract.log
    fi
    rm -rf "$scratch"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$scratch/unpacked" "$scratch/runtime/usr"
"$tool" extract "$downloads/$firmware" "$scratch/unpacked" >"$scratch/extract.log" 2>&1
gzip -dc "$scratch/unpacked/rootfs.img.gz" >"$scratch/rootfs.img"
# debugfs opens the image read-only without -w. No loop device or mount is used.
debugfs -R "rdump /lib $scratch/runtime" "$scratch/rootfs.img"
debugfs -R "rdump /usr/lib $scratch/runtime/usr" "$scratch/rootfs.img"
debugfs -R 'cat /etc/version.txt' "$scratch/rootfs.img" >"$scratch/runtime/version.txt"
grep -q '040-juno_1902_malbec_bellatrix-467076' "$scratch/runtime/version.txt"
printf '%s\n' "$firmware_sha" >"$scratch/runtime/firmware.sha256"
(
    cd "$scratch/runtime"
    find lib usr/lib -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum >files.sha256
)
verify_runtime "$scratch/runtime"
rm -rf "$runtime"
mv "$scratch/runtime" "$runtime"
verify_runtime
echo 'Prepared PW5 firmware 5.19.2 runtime (stock glibc 2.20).'
