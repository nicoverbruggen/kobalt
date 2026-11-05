#!/bin/bash
set -euo pipefail
if [[ $# -ne 0 ]]; then echo "Usage: bash platforms/android/build.sh" >&2; exit 2; fi
root=$(cd "$(dirname "$0")/../.." && pwd)
jobs=${JOBS:-2}
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo "JOBS must be positive" >&2; exit 2; }
# Prepared multi-architecture Android SDK; application dependencies build locally.
image=${KOBALT_ANDROID_IMAGE:-$(sh "$root/platforms/sdk-image.sh" android)}
engine=${CONTAINER_ENGINE:-docker}
read -r arch sdk_id < <("$engine" image inspect "$image" --format '{{.Architecture}} {{.Id}}')
mkdir -p "$root/build/android-$arch" "$root/build/gradle"
release=${KOBALT_ANDROID_RELEASE:-0}
[[ "$release" == 0 || "$release" == 1 ]] || { echo 'KOBALT_ANDROID_RELEASE must be 0 or 1' >&2; exit 2; }
if [[ $release == 1 && -z ${KOBALT_RELEASE_VERSION:-} && ! -f $root/source-info.json ]]; then
    echo 'Missing KOBALT_RELEASE_VERSION' >&2; exit 2
fi
info=$(cmake -P "$root/platforms/source-info.cmake")
# The shared dependency cache is created on the host, so the host user owns it.
# Containers mount it at one fixed path, the path stored in its CMake files.
cache=$(cmake -P "$root/dependencies/cache.cmake")
mkdir -p "$cache"
read -r commit modified KOBALT_RELEASE_VERSION <<< "$info"
export KOBALT_RELEASE_VERSION
extra=(--pull=never)
if [[ "$release" == 1 ]]; then
    for name in KOBALT_RELEASE_VERSION KOBALT_ANDROID_KEYSTORE KOBALT_ANDROID_KEY_ALIAS KOBALT_ANDROID_KEYSTORE_PASSWORD KOBALT_ANDROID_KEY_PASSWORD KOBALT_ANDROID_CERT_SHA256; do
        [[ -n ${!name:-} ]] || { echo "Missing $name" >&2; exit 2; }
    done
    [[ -f $KOBALT_ANDROID_KEYSTORE ]] || { echo 'Release keystore is missing' >&2; exit 2; }
    extra+=(--volume "$KOBALT_ANDROID_KEYSTORE:/signing/release.keystore:ro")
fi
"$engine" run --rm --user "$(id -u):$(id -g)" \
    "${extra[@]}" -e KOBALT_RELEASE_VERSION -e KOBALT_ANDROID_RELEASE="$release" \
    -e KOBALT_ANDROID_KEYSTORE=/signing/release.keystore -e KOBALT_ANDROID_KEY_ALIAS \
    -e KOBALT_ANDROID_KEYSTORE_PASSWORD -e KOBALT_ANDROID_KEY_PASSWORD -e KOBALT_ANDROID_CERT_SHA256 \
    -e KOBALT_ANDROID_VERSION_CODE="${KOBALT_ANDROID_VERSION_CODE:-1}" \
    -e HOME=/tmp -e GRADLE_USER_HOME=/gradle -e ANDROID_USER_HOME="/work/build/android-$arch/user-home" -e KOBALT_ANDROID_HOST_ARCH="$arch" \
    --volume "$root:/work" \
    --volume "$root/build/gradle:/gradle" \
    --volume "$cache:/dependency-cache" \
    --workdir /work/platforms/android "$image" bash -euc '
        exec 9>"/work/build/android-$KOBALT_ANDROID_HOST_ARCH/build.lock"
        flock -n 9 || { echo "Android output is in use by another build" >&2; exit 1; }
        started=$SECONDS
        options=(-DKOBALT_ANDROID_RELEASE=OFF)
        if [[ $KOBALT_ANDROID_RELEASE == 1 ]]; then options=(-DKOBALT_ANDROID_RELEASE=ON -DCMAKE_BUILD_TYPE=Release); fi
        cmake --preset android "${options[@]}" -DKOBALT_ANDROID_VERSION_CODE="$KOBALT_ANDROID_VERSION_CODE" \
            -DKOBALT_SDK_ID="$4" -DKOBALT_DEPENDENCY_JOBS="$1" -DKOBALT_DEPENDENCY_CACHE=/dependency-cache -DKOBALT_COMMIT="$2" -DKOBALT_COMMIT_MODIFIED="$3"
        echo "Configure: $((SECONDS - started)) seconds"
        started=$SECONDS
        cmake --build --preset android --parallel "$1"
        if [[ $KOBALT_ANDROID_RELEASE == 1 ]]; then
            apk="/work/build/android-$KOBALT_ANDROID_HOST_ARCH/android-build/build/outputs/apk/release/android-build-release-unsigned.apk"
            [[ -f $apk ]] || { echo "Missing unsigned release APK: $apk" >&2; exit 1; }
            bash /work/platforms/android/sign-apk.sh "$apk" "/work/build/android-$KOBALT_ANDROID_HOST_ARCH/Kobalt-android-arm64.apk"
        fi
        echo "Build and APK: $((SECONDS - started)) seconds"
    ' kobalt-android "$jobs" "$commit" "$modified" "$sdk_id"
