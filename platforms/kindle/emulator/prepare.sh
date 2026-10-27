#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/../../.." && pwd)
commit=81ae873f57ea5a8aba71a00774118dc3ad8462e7
source_dir=$root/build/kindle-emulator/eink-emulator-$commit
if [[ $# == 0 ]]; then
    export DOCKER_CONFIG=${DOCKER_CONFIG:-$root/build/kindle-sdk/docker-config}
    mkdir -p "$DOCKER_CONFIG" "$source_dir" "$root/build/kindle-sdk"
    case "$(docker info --format '{{.Architecture}}')" in
        aarch64|arm64) arch=arm64; base=arm64v8/ubuntu:24.04 ;;
        x86_64|amd64) arch=amd64; base=amd64/ubuntu:24.04 ;;
        *) echo 'The emulator requires an ARM64 or AMD64 Docker engine.' >&2; exit 1 ;;
    esac
    image=kobalt-kindle-emulator:81ae873-v1-$arch
    if [[ ${REBUILD_EMULATOR_IMAGE:-0} == 1 ]] || ! docker image inspect "$image" >/dev/null 2>&1; then
        docker build --platform "linux/$arch" --build-arg "KOBALT_EMULATOR_BASE=$base" \
            -f "$root/platforms/kindle/emulator/Dockerfile" -t "$image" \
            "$root/platforms/kindle/emulator"
    fi
    [[ $(docker image inspect "$image" --format '{{.Architecture}}') == "$arch" ]] || exit 1
    docker run --rm --pull=never --init --label kobalt.purpose=kindle-emulator-prepare \
        -v "$root:/work" -v "$root/build/kindle-sdk:/sdk" -v "$source_dir:/emu" \
        -e "JOBS=${JOBS:-4}" "$image" bash /work/platforms/kindle/emulator/prepare.sh --inside
    printf '%s\n' "$image" >"$root/build/kindle-emulator/image.tag"
    exit 0
fi
if [[ $# != 1 || $1 != --inside || $root != /work ]]; then
    echo 'Usage: bash platforms/kindle/emulator/prepare.sh' >&2
    exit 2
fi

cache=/work/build/kindle-emulator
exec 9>"$cache/prepare.lock"
flock -n 9 || { echo 'The emulator is already being prepared.' >&2; exit 1; }
mkdir -p "$cache/downloads" /sdk/downloads
fetch() {
    local destination=$1 url=$2 digest=$3
    if [[ ! -f $destination ]]; then
        curl --fail --location --retry 3 --output "$destination.partial" "$url"
        printf '%s  %s\n' "$digest" "$destination.partial" | sha256sum --check --status
        mv "$destination.partial" "$destination"
    fi
    printf '%s  %s\n' "$digest" "$destination" | sha256sum --check --status
}
archive=$cache/downloads/eink-emulator-81ae873.tar.gz
fetch "$archive" "https://codeload.github.com/katadelos/eink-emulator/tar.gz/$commit" \
    7227c5b53b74e4a028c1f47b7e90abf11bfc6a89365e535f14b8b35c929191ac
[[ -f /emu/eink ]] || tar -xzf "$archive" --strip-components=1 -C /emu
qemu_commit=00e9b8598ee0926f93fb6e3c69b2d88cf09d1e0a
qemu_archive=$cache/downloads/qemu-00e9b85.tar.gz
fetch "$qemu_archive" "https://codeload.github.com/katadelos/qemu/tar.gz/$qemu_commit" \
    0c0deaa99140cf752a0936419bd5ff4054ac1f423bd93be1382ef9df14dab477
mkdir -p /emu/qemu
[[ -f /emu/qemu/configure ]] || tar -xzf "$qemu_archive" --strip-components=1 -C /emu/qemu
lut_patch=/work/dependencies/emulator/qemu-lut-frames.patch
if patch --dry-run --batch --forward -p1 -d /emu/qemu <"$lut_patch" >/dev/null 2>&1; then
    patch --batch --forward -p1 -d /emu/qemu <"$lut_patch"
elif ! patch --dry-run --batch --reverse -p1 -d /emu/qemu <"$lut_patch" >/dev/null 2>&1; then
    echo 'QEMU display source does not match the pinned source and LUT patch.' >&2
    exit 1
fi
jobs=${JOBS:-4}
[[ $jobs =~ ^[1-9][0-9]*$ ]] || { echo 'JOBS must be a positive integer.' >&2; exit 2; }
if [[ ! -x /emu/qemu/build/qemu-system-aarch64 || ! -x /emu/qemu/build/qemu-img ]] || \
        ! grep -q '^#define CONFIG_PNG\([[:space:]]\|$\)' /emu/qemu/build/config-host.h; then
    mkdir -p "$source_dir/qemu/build"
    (
        cd "$source_dir/qemu/build"
        ../configure --target-list=aarch64-softmmu --disable-docs --disable-werror \
            --enable-slirp --disable-pvg --disable-gtk --disable-sdl --disable-opengl --enable-png
    )
fi
# Always let Ninja check the patched sources, including an existing binary.
ninja -C /emu/qemu/build -j "$jobs" qemu-system-aarch64 qemu-img
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I/emu/qemu/include $(pkg-config --cflags glib-2.0) \
    /work/platforms/kindle/emulator/frames-test.c $(pkg-config --libs glib-2.0) \
    -o "$cache/hwtcon-frames-test"
"$cache/hwtcon-frames-test"
# Executing these also rejects a cached build for a different host architecture.
/emu/qemu/build/qemu-system-aarch64 --version
/emu/qemu/build/qemu-img --version

firmware=update_kindle_all_new_paperwhite_11th_5.19.2.bin
fetch "/sdk/downloads/$firmware" "https://s3.amazonaws.com/firmwaredownloads/$firmware" \
    64ddcbae52f0009b5d3f9974d93610db7f26a8787c895c55f413127af474b987
[[ $(stat -c %s "/sdk/downloads/$firmware") == 361949776 ]]
tool_commit=95291295e039a1a3bc0bc9d6040b07f640dae74a
tool_archive=/sdk/downloads/KindleTool-$tool_commit.tar.gz
fetch "$tool_archive" "https://codeload.github.com/NiLuJe/KindleTool/tar.gz/$tool_commit" \
    011ae7463000bc3bb602321afa9bb13094dce25f43ca7621f6277b0c2a649e87
tools=/sdk/src/firmware-tools/$(uname -m)
tool=$tools/KindleTool-$tool_commit/KindleTool/Release
if [[ ! -x $tool/kindletool ]]; then
    mkdir -p "$tools"
    tar -xzf "$tool_archive" -C "$tools"
    make -C "$tools/KindleTool-$tool_commit/KindleTool" -j "$jobs"
fi
export PATH=$tool:$PATH
version_dir=/emu/firmware/versions/kindle-paperwhite-5/5.19.2
if [[ ! -d $version_dir ]]; then
    [[ ! -e /emu/firmware/kindle-paperwhite-5 ]] || {
        echo 'An unversioned PW5 firmware import already exists. Preserve or move it before preparing.' >&2
        exit 1
    }
    /emu/eink import "/sdk/downloads/$firmware" --model kindle-paperwhite-5
    mkdir -p "$(dirname "$version_dir")"
    mv /emu/firmware/kindle-paperwhite-5 "$version_dir"
fi
(
    cd "$version_dir"
    sha256sum --check --status <<'CHECKSUMS'
b68451fad1e374275f1eb6d3e0ddb2e81b9dcf348ef86d981053461be1ec8d73  bl2.img
5e2b28b11ddb307bcdb8e9c3f93ff1279dbcc24d68822c73e43c2d81d4f1d4e4  boot.img
ebb7413336d9e8d097c43cc71cbf2c4cf8d04fbbf5bbccdbac0c2409d783db99  quickboot.img
e7870c835d175a4ebc5c16d5f19147d46afa545da7ea0b6fac10df8706db0fda  rootfs.img
661d2ce83a5d579ebf039f7b7aab1dca2ac5a34e4aa4893fda07ce8ba21e68c2  tee.img
f95368abc2b799d925510e23e13fd5cfde29a251cd3042e2d6fe8189641184ab  u-boot.bin
CHECKSUMS
)
mkdir -p /emu/guest-additions/kindle/downloads/2.2.1
fetch /emu/guest-additions/kindle/downloads/2.2.1/jb.sh \
    https://github.com/KindleModding/jb.sh/releases/download/v2.2.1/jb.sh \
    cf60f9f95db3f0a25bea80bbe0c85f4de86d2f900be872f8d22d118163b708ba
echo 'Prepared the PW5 5.19.2 headless emulator. No guest was created.'
