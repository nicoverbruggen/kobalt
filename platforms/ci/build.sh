#!/bin/bash
# Build one public release target. Publication is owned by the workflow.
set -euo pipefail
[[ $# == 1 ]] || { echo 'Expected qt5, qt6 or android' >&2; exit 2; }
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
version=$(bash platforms/ci/version.sh)
export KOBALT_RELEASE_VERSION=$version
image=$(sh platforms/sdk-image.sh "$1")
docker pull "$image"
mkdir -p build/release
if [[ $1 == android ]]; then
    export KOBALT_ANDROID_RELEASE=1
    bash platforms/android/build.sh
    arch=$(docker image inspect "$image" --format '{{.Architecture}}')
    package="build/android-$arch/Kobalt-android-arm64.apk"
    bash platforms/android/check-notices.sh "$package"
    docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp --volume "$root:/work" --workdir /work "$image" \
        bash platforms/android/test-signing.sh "build/android-$arch/android-build/build/outputs/apk/release/android-build-release-unsigned.apk"
    cp "$package" "$package.signing.txt" build/release/
else
    verbose=$(bash platforms/ci/diagnostics.sh "$version")
    # The adapter tests run first, with the package's logging, so the package
    # build reuses their objects.
    KOBALT_VERBOSE_DIAGNOSTICS=$verbose bash platforms/kobo/build.sh "$1" --check
    KOBALT_VERBOSE_DIAGNOSTICS=$verbose bash platforms/kobo/build.sh "$1"
    suffix=
    [[ $verbose != ON ]] || suffix=-diagnostic
    package="build/kobo-$1/Kobalt-$1$suffix.zip"
    cp "$package" "build/release/Kobalt-$1.zip"
fi
jq -n --arg version "$version" --arg target "$1" --arg commit "$(git rev-parse HEAD)" \
    --arg sdk "$image" --arg sha "$(shasum -a 256 "$package" | awk '{print $1}')" \
    '{version:$version,target:$target,commit:$commit,sdk:$sdk,sha256:$sha}' > "build/release/build-$1.json"
