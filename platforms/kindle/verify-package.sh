#!/usr/bin/env bash
set -euo pipefail

glibc_compatible() {
    [[ "$1" =~ ^[0-9]+(\.[0-9]+)+$ ]] || return 1
    awk -v version="$1" 'BEGIN {
        n = split(version, parts, ".");
        if (parts[1] < 2) exit 0;
        if (parts[1] > 2 || parts[2] > 20) exit 1;
        if (parts[2] < 20) exit 0;
        for (i = 3; i <= n; ++i) if (parts[i] > 0) exit 1;
        exit 0;
    }'
}

glibc_library() {
    case "$1" in
        libc.so.6|libm.so.6|libdl.so.2|libpthread.so.0|librt.so.1|libutil.so.1|libresolv.so.2|libnsl.so.1|libanl.so.1|libcrypt.so.1|ld-linux-armhf.so.3) return 0 ;;
        *) return 1 ;;
    esac
}

fail() { echo "Kindle package verification failed: $*" >&2; exit 1; }

if [[ "${1:-}" == --self-test && $# == 1 ]]; then
    for version in 2.0 2.4 2.19 2.20 2.20.0; do
        glibc_compatible "$version" || fail "baseline parser rejected $version"
    done
    for version in 2.20.1 2.21 2.34 3.0 PRIVATE ABI_DT_RELR 2.invalid; do
        if glibc_compatible "$version"; then fail "baseline parser accepted $version"; fi
    done
    glibc_library libc.so.6 || fail 'libc whitelist'
    if glibc_library libatomic.so.1; then fail 'libatomic must be packaged'; fi
    echo 'Kindle package audit parser checks passed.'
    exit 0
fi

root=$(cd "$(dirname "$0")/../.." && pwd)
export DOCKER_CONFIG=${DOCKER_CONFIG:-"$root/build/kindle-sdk/docker-config"}
mkdir -p "$DOCKER_CONFIG"
if [[ $# == 0 ]]; then
    [[ -f "$root/build/kindle/package/kobalt/bin/kobalt" ]] || fail 'build/package the ARM app first'
    image=$(bash "$root/platforms/kindle/sdk/prepare-runtime-image.sh")
    exec docker run --rm --pull=never --init \
        --label kobalt.purpose=kindle-userspace-check \
        -v "$root:/work" -v "$root/build/kindle-sdk:/fixture" \
        --workdir /work "$image" bash /work/platforms/kindle/verify-package.sh --inside
fi
[[ $# == 1 && "$1" == --inside && "$root" == /work ]] || {
    echo 'Usage: bash platforms/kindle/verify-package.sh [--self-test]' >&2
    exit 2
}

exec 9>/work/build/kindle/application.lock
flock -n 9 || fail 'the Kindle output is already in use'
exec 8>/fixture/firmware-runtime.lock
flock -n 8 || fail 'the stock runtime fixture is being prepared'
app=/work/build/kindle/package/kobalt
sysroot=/fixture/firmware-runtime
readelf=/usr/bin/readelf
[[ -x "$readelf" ]] || fail 'SDK readelf missing'
[[ -f "$sysroot/firmware.sha256" && $(cat "$sysroot/firmware.sha256") == \
    64ddcbae52f0009b5d3f9974d93610db7f26a8787c895c55f413127af474b987 ]] \
    || fail 'prepare the stock runtime first: bash platforms/kindle/sdk/prepare-runtime.sh'
(cd "$sysroot" && sha256sum --check --status files.sha256) || fail 'stock runtime checksum mismatch'
for binary in "$app/bin/kobalt" "$app/plugins/platforms/libqkindle.so"; do
    "$readelf" --file-header "$binary" >/dev/null || fail "missing or invalid ELF: $binary"
done
[[ -z "$(find "$app" -type l -print -quit)" ]] || fail 'package contains symlinks unsupported by USB storage'

count=0
while IFS= read -r -d '' binary; do
    if ! header=$("$readelf" --file-header "$binary" 2>/dev/null); then continue; fi
    count=$((count + 1))
    grep -Eq 'Class:[[:space:]]+ELF32' <<<"$header" || fail "not ELF32: $binary"
    grep -Eq 'Machine:[[:space:]]+ARM[[:space:]]*$' <<<"$header" || fail "not ARM: $binary"
    grep -Eq 'Data:.*little endian' <<<"$header" || fail "not little endian: $binary"
    grep -q 'hard-float ABI' <<<"$header" || fail "not hard-float ABI: $binary"
    dynamic=$("$readelf" --dynamic --wide "$binary")
    while IFS= read -r needed; do
        [[ -n "$needed" ]] || continue
        if [[ -f "$app/lib/$needed" ]]; then continue; fi
        glibc_library "$needed" || fail "unpackaged dependency $needed required by $binary"
        if [[ ! -f "$sysroot/lib/$needed" && ! -f "$sysroot/usr/lib/$needed" ]]; then
            fail "glibc dependency $needed absent from target sysroot"
        fi
    done < <(sed -n 's/.*(NEEDED).*\[\([^]]*\)\].*/\1/p' <<<"$dynamic")
    versions=$("$readelf" --version-info --wide "$binary")
    while IFS= read -r version; do
        glibc_compatible "$version" || fail "GLIBC_$version exceeds target baseline 2.20: $binary"
    done < <(awk '{ for (i = 1; i < NF; ++i) if ($i == "Name:" && $(i + 1) ~ /^GLIBC_/) {
        version = $(i + 1); sub(/^GLIBC_/, "", version); print version;
    }}' <<<"$versions" | sort -u)
done < <(find "$app/bin" "$app/lib" "$app/plugins" -type f -print0)

program_headers=$("$readelf" --program-headers --wide "$app/bin/kobalt")
grep -Fq '[Requesting program interpreter: /lib/ld-linux-armhf.so.3]' <<<"$program_headers" \
    || fail 'unexpected executable interpreter'
echo "ARM ELF audit passed: $count objects, dependency closure, hard-float ABI, GLIBC <= 2.20."
bash /work/platforms/kindle/power-bridge-test.sh /work/build/kindle/test-tmp

# The SDK offscreen plugin uses the same FreeType database as the device QPA.
# qemu-user executes ARM userspace; it does not emulate a Kindle kernel or EPDC.
[[ -f /work/build/kindle/sdk-platforms/libqoffscreen.so ]] || fail 'matching SDK offscreen plugin missing; rebuild the application'
mkdir -p /work/build/kindle/test-tmp
profile=$(mktemp -d /work/build/kindle/test-tmp/arm-smoke.XXXXXX)
cleanup() { rm -rf "$profile"; }
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$profile/tmp" "$profile/runtime"
chmod 700 "$profile/runtime"
log=/work/build/kindle/arm-smoke.log
result=0
ulimit -c 0
timeout --signal=TERM --kill-after=10s 180s qemu-arm -cpu cortex-a7 -L "$sysroot" \
    -E "LD_LIBRARY_PATH=$app/lib" \
    -E QT_QPA_PLATFORM=offscreen \
    -E QT_QPA_PLATFORM_PLUGIN_PATH=/work/build/kindle/sdk-platforms \
    -E "QT_PLUGIN_PATH=$app/plugins" \
    -E "QT_QPA_FONTDIR=$app/assets/fonts" \
    -E "TMPDIR=$profile/tmp" \
    -E "XDG_RUNTIME_DIR=$profile/runtime" \
    "$app/bin/kobalt" --assets "$app/assets" \
    --smoke-test --profile "$profile/data" --library /work/tests/fixtures \
    --book /work/tests/fixtures/trevelyans-type-tester.kepub.epub >"$log" 2>&1 || result=$?
cat "$log"
[[ "$result" == 0 ]] || fail "ARM userspace smoke exited $result; see build/kindle/arm-smoke.log"
grep -q 'Kindle smoke passed:' "$log" || fail 'ARM app exited without the smoke completion marker'
[[ -s "$profile/data/config/kobalt.sqlite" ]] || fail 'ARM smoke did not create its SQLite profile'
echo 'ARM userspace smoke passed. This check does not exercise framebuffer, input, suspend or stock UI.'
