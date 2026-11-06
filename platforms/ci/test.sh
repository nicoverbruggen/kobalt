#!/bin/bash
# Offline release checks. Never modify the application's Git state.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
for script in platforms/ci/*.sh platforms/android/*.sh platforms/kobo/*.sh platforms/sdk-image.sh; do bash -n "$script"; done
mkdir -p build
scratch=$(mktemp -d "$root/build/release-check.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
cmake "-DTEST_TMP_ROOT=$scratch" -P platforms/strings-test.cmake
cmake "-DTEST_TMP_ROOT=$scratch" -P platforms/test-validation-record.cmake
cat > "$scratch/version.cmake" <<'CMAKE'
include("$ENV{KOBALT_TEST_ROOT}/platforms/release-version.cmake")
kobalt_release_version(actual v1.0-beta.4)
if(NOT actual STREQUAL "$ENV{KOBALT_TEST_EXPECTED}")
    message(FATAL_ERROR "Unexpected version: ${actual}")
endif()
CMAKE
export KOBALT_TEST_ROOT=$root
for version in v1.0-beta.4 v1.0 v1.2.3; do
    KOBALT_RELEASE_VERSION=$version KOBALT_TEST_EXPECTED=$version cmake -P "$scratch/version.cmake"
done
for version in '../bad' '1.0' 'v1.0;bad' 'v1.0/evil'; do
    if KOBALT_RELEASE_VERSION=$version cmake -P "$scratch/version.cmake" > "$scratch/error" 2>&1; then echo 'Accepted invalid version' >&2; exit 1; fi
done
# Build provenance selects logging, even when branch versions contain beta.
for ref in refs/heads/main refs/heads/develop refs/heads/nightly; do
    [[ $(GITHUB_REF=$ref bash platforms/ci/diagnostics.sh v1.0-beta.6) == ON ]]
done
for version in v1.0 v1.0-beta.6 v1.0-RC.1; do
    [[ $(GITHUB_REF=refs/tags/$version bash platforms/ci/diagnostics.sh "$version") == OFF ]]
done
for version in v1.0-nightly v1.0-nightly.12 v1.0-interim.3; do
    [[ $(GITHUB_REF=refs/tags/$version bash platforms/ci/diagnostics.sh "$version") == ON ]]
done
# Exercise the real CI wrapper with fixture SDK/package builders.
mkdir -p "$scratch/ci/platforms/ci" "$scratch/ci/platforms/kobo" "$scratch/ci-bin"
cp platforms/ci/{build,diagnostics}.sh "$scratch/ci/platforms/ci/"
printf '#!/bin/bash\nprintf "%%s\\n" "$KOBALT_RELEASE_VERSION"\n' > "$scratch/ci/platforms/ci/version.sh"
printf '#!/bin/bash\necho fixture-sdk\n' > "$scratch/ci/platforms/sdk-image.sh"
printf '#!/bin/bash\nexit 0\n' > "$scratch/ci-bin/docker"
chmod +x "$scratch/ci-bin/docker"
cat > "$scratch/ci/platforms/kobo/build.sh" <<'SH'
#!/bin/bash
set -eu
mkdir -p "build/kobo-$1"
suffix=
[[ $KOBALT_VERBOSE_DIAGNOSTICS != ON ]] || suffix=-diagnostic
printf '%s' "$KOBALT_VERBOSE_DIAGNOSTICS" > "build/kobo-$1/Kobalt-$1$suffix.zip"
SH
for target in qt5 qt6; do
    for channel in release interim; do
        if [[ $channel == release ]]; then ref=refs/tags/v1.0-beta.6; expected=OFF
        else ref=refs/heads/develop; expected=ON; fi
        GITHUB_REF=$ref KOBALT_RELEASE_VERSION=v1.0-beta.6 PATH="$scratch/ci-bin:$PATH" \
            bash "$scratch/ci/platforms/ci/build.sh" "$target"
        [[ $(cat "$scratch/ci/build/release/Kobalt-$target.zip") == "$expected" ]]
    done
done
# Run the native wrapper with fake tools, including its container command.
# Tags cannot request reduced coverage; branches keep the everyday mode.
mkdir -p "$scratch/native/platforms/ci" "$scratch/native/dependencies" "$scratch/native-bin"
cp platforms/ci/native-tests.sh "$scratch/native/platforms/ci/"
cp dependencies/cache.cmake "$scratch/native/dependencies/"
cp platforms/sdk-image.sh "$scratch/native/platforms/"
cat > "$scratch/native-bin/cmake" <<'SH'
#!/bin/bash
if [[ $1 == -P ]]; then printf '%s/cache\n' "$KOBALT_NATIVE_FIXTURE"; fi
SH
cat > "$scratch/native-bin/ctest" <<'SH'
#!/bin/bash
printf '%s\n' "$KOBALT_TEST_ALL_VARIANTS" > "$KOBALT_NATIVE_FIXTURE/variants"
exit "${KOBALT_NATIVE_FAILURE:-0}"
SH
cat > "$scratch/native-bin/docker" <<'SH'
#!/bin/bash
set -eu
[[ $1 != pull ]] || exit 0
[[ $1 == run ]]
shift
environment=()
while [[ $1 != bash ]]; do
    if [[ $1 == -e ]]; then environment+=("$2"); shift; fi
    shift
done
env "${environment[@]}" "$@"
SH
chmod +x "$scratch/native-bin/"*
export KOBALT_NATIVE_FIXTURE="$scratch/native"
for ref in refs/heads/develop refs/heads/main refs/tags/v1.0-beta.6 refs/tags/v1.0; do
    [[ $ref != refs/tags/* ]] && expected=0 || expected=1
    GITHUB_REF=$ref KOBALT_TEST_ALL_VARIANTS=$((1 - expected)) JOBS=2 \
        CONTAINER_ENGINE="$scratch/native-bin/docker" CMAKE_COMMAND="$scratch/native-bin/cmake" \
        PATH="$scratch/native-bin:$PATH" bash "$scratch/native/platforms/ci/native-tests.sh"
    [[ $(cat "$scratch/native/variants") == "$expected" ]]
done
if GITHUB_REF=refs/tags/v1.0 KOBALT_NATIVE_FAILURE=7 JOBS=2 \
    CONTAINER_ENGINE="$scratch/native-bin/docker" CMAKE_COMMAND="$scratch/native-bin/cmake" \
    PATH="$scratch/native-bin:$PATH" bash "$scratch/native/platforms/ci/native-tests.sh"; then
    echo 'Native CI ignored a failed test suite' >&2; exit 1
fi
echo 'PASS: native CI coverage selection and failure propagation'
# Source manifests must have disjoint names and complete checksums.
jq -es '([.[0]|keys[]] + [.[1]|keys[]]) as $keys | ($keys|length)==($keys|unique|length) and (all(.[]|to_entries[]; (.key|test("^[a-z0-9-]+$")) and (.value.url|startswith("https://")) and (.value.sha256|test("^[a-f0-9]{64}$"))))' dependencies/manifest.json dependencies/release-sources.json > /dev/null
# Fail closed before consulting tools or producing an APK when secrets are absent.
if env -u KOBALT_ANDROID_KEYSTORE bash platforms/android/sign-apk.sh missing.apk "$scratch/output.apk" > "$scratch/error" 2>&1; then echo 'Signing accepted a missing key' >&2; exit 1; fi
[[ ! -e "$scratch/output.apk" ]]
# The debug launcher must also work with macOS's Bash 3 and no signing mounts.
cat > "$scratch/container-engine" <<'SH'
#!/bin/bash
if [[ $1 == image && $2 == inspect ]]; then
    printf 'arm64 fixture-sdk\n'
elif [[ $1 != run ]]; then
    exit 1
elif [[ -n ${KOBALT_TEST_ENGINE_LOG:-} ]]; then
    printf '%s\n' "$KOBALT_RELEASE_VERSION" "$*" >> "$KOBALT_TEST_ENGINE_LOG"
fi
SH
chmod +x "$scratch/container-engine"
CONTAINER_ENGINE="$scratch/container-engine" KOBALT_ANDROID_IMAGE=fixture-sdk KOBALT_ANDROID_RELEASE=0 \
    bash platforms/android/build.sh
# A Git-free source export must not inherit its enclosing checkout's identity.
exported="$scratch/export/application"
mkdir -p "$exported/platforms/android" "$exported/platforms/kobo" "$exported/assets" "$exported/dependencies"
cp dependencies/cache.cmake "$exported/dependencies/"
cp platforms/source-info.cmake platforms/release-version.cmake platforms/sdk-image.sh platforms/load-lock.cmake "$exported/platforms/"
cp platforms/android/build.sh "$exported/platforms/android/"
cp platforms/kobo/build.sh "$exported/platforms/kobo/"
printf '{"version":"v1.0-beta.1"}\n' > "$exported/assets/manifest.json"
printf '{"commit":"ffffffffffffffffffffffffffffffffffffffff","version":"v9.8.7-beta.6"}\n' > "$exported/source-info.json"
[[ $(env -u KOBALT_RELEASE_VERSION cmake -P "$exported/platforms/source-info.cmake") == 'fffffff OFF v9.8.7-beta.6' ]]
[[ $(KOBALT_RELEASE_VERSION=v9.8.8 cmake -P "$exported/platforms/source-info.cmake") == 'fffffff OFF v9.8.8' ]]
env -u KOBALT_RELEASE_VERSION CONTAINER_ENGINE="$scratch/container-engine" \
    KOBALT_TEST_ENGINE_LOG="$scratch/export-engine.log" bash "$exported/platforms/android/build.sh"
env -u KOBALT_RELEASE_VERSION CONTAINER_ENGINE="$scratch/container-engine" \
    KOBALT_TEST_ENGINE_LOG="$scratch/export-engine.log" bash "$exported/platforms/kobo/build.sh" qt6
# A Git-free export uses its own dependency cache, created on the host and
# mounted at the fixed container path.
[[ -d $exported/build/dependency-cache ]]
[[ $(grep -cF -- "--volume $exported/build/dependency-cache:/dependency-cache" "$scratch/export-engine.log") == 2 ]]
[[ $(grep -cF -- "-DKOBALT_DEPENDENCY_CACHE=/dependency-cache" "$scratch/export-engine.log") == 2 ]]
grep -Fx 'v9.8.7-beta.6' "$scratch/export-engine.log" > /dev/null
grep -F 'fffffff OFF' "$scratch/export-engine.log" > /dev/null
# Direct builds enable diagnostics; an explicit release choice must survive
# container launch, and diagnostic targets must stay verbose in either mode.
for target in qt5 qt6; do
    for mode in default ON OFF; do
        log="$scratch/diagnostics-$target-$mode.log"
        if [[ $mode == default ]]; then
            env -u KOBALT_VERBOSE_DIAGNOSTICS CONTAINER_ENGINE="$scratch/container-engine" \
                KOBALT_TEST_ENGINE_LOG="$log" bash "$exported/platforms/kobo/build.sh" "$target"
            expected=ON
        else
            KOBALT_VERBOSE_DIAGNOSTICS=$mode CONTAINER_ENGINE="$scratch/container-engine" \
                KOBALT_TEST_ENGINE_LOG="$log" bash "$exported/platforms/kobo/build.sh" "$target"
            expected=$mode
        fi
        grep -F -- '-DKOBALT_VERBOSE_DIAGNOSTICS="$6"' "$log" > /dev/null
        grep -F "kobalt-build $target 2 fffffff OFF arm64 fixture-sdk $expected" "$log" > /dev/null
    done
    KOBALT_VERBOSE_DIAGNOSTICS=OFF CONTAINER_ENGINE="$scratch/container-engine" \
        KOBALT_TEST_ENGINE_LOG="$scratch/diagnostic-target.log" bash "$exported/platforms/kobo/build.sh" "$target-diagnostic"
    grep -F "kobalt-build $target-diagnostic 2 fffffff OFF arm64 fixture-sdk ON" "$scratch/diagnostic-target.log" > /dev/null
done
if KOBALT_VERBOSE_DIAGNOSTICS=invalid CONTAINER_ENGINE="$scratch/container-engine" \
    bash "$exported/platforms/kobo/build.sh" qt5 > "$scratch/error" 2>&1; then
    echo 'Accepted an invalid diagnostic mode' >&2; exit 1
fi
echo 'PASS: direct and release diagnostic build modes'
if env -u KOBALT_RELEASE_VERSION -u KOBALT_ANDROID_KEYSTORE KOBALT_ANDROID_RELEASE=1 \
    CONTAINER_ENGINE="$scratch/container-engine" bash platforms/android/build.sh > "$scratch/error" 2>&1; then
    echo 'Release checkout accepted a missing version' >&2; exit 1
fi
grep -F 'Missing KOBALT_RELEASE_VERSION' "$scratch/error" > /dev/null
if env -u KOBALT_RELEASE_VERSION -u KOBALT_ANDROID_KEYSTORE KOBALT_ANDROID_RELEASE=1 \
    CONTAINER_ENGINE="$scratch/container-engine" bash "$exported/platforms/android/build.sh" > "$scratch/error" 2>&1; then
    echo 'Release export accepted a missing keystore' >&2; exit 1
fi
grep -F 'Missing KOBALT_ANDROID_KEYSTORE' "$scratch/error" > /dev/null
# The same metadata reader is used by native CMake configuration.
cat > "$scratch/provenance.cmake" <<'CMAKE'
include("$ENV{KOBALT_TEST_EXPORT}/platforms/source-info.cmake")
kobalt_source_info("$ENV{KOBALT_TEST_EXPORT}")
if(NOT KOBALT_COMMIT STREQUAL "fffffff" OR KOBALT_COMMIT_MODIFIED)
    message(FATAL_ERROR "Native configuration lost exported provenance")
endif()
CMAKE
KOBALT_TEST_EXPORT="$exported" cmake -P "$scratch/provenance.cmake"
printf '{"commit":"invalid","version":"v9.8.7-beta.6"}\n' > "$exported/source-info.json"
if cmake -P "$exported/platforms/source-info.cmake" > "$scratch/error" 2>&1; then
    echo 'Accepted invalid source provenance' >&2; exit 1
fi
rm "$exported/source-info.json"
[[ $(env -u KOBALT_RELEASE_VERSION cmake -P "$exported/platforms/source-info.cmake") == 'unknown OFF v1.0-beta.1' ]]
echo 'PASS: Git-free source provenance, release versions and launchers'
# Release filenames stay fixed while the tag selects the release and version.
mkdir -p "$scratch/bin" "$scratch/draft/build/release"
cat > "$scratch/bin/gh" <<'SH'
#!/bin/bash
printf '%s\n' "$*" >> "$KOBALT_TEST_GH_LOG"
if [[ $1 == release && $2 == view ]]; then printf 'true\n'; fi
SH
chmod +x "$scratch/bin/gh"
for name in Kobalt-qt5.zip Kobalt-qt6.zip Kobalt-android-arm64.apk Kobalt-sources.tar.gz build-qt5.json; do
    printf 'fixture\n' > "$scratch/draft/build/release/$name"
done
for version in v1.0-beta.4 v1.2.3; do
    (
        cd "$scratch/draft"
        PATH="$scratch/bin:$PATH" KOBALT_TEST_GH_LOG="$scratch/gh.log" \
            KOBALT_RELEASE_VERSION="$version" GITHUB_REF="refs/tags/$version" \
            bash "$root/platforms/ci/draft-release.sh"
    )
    grep -F "release upload $version build/release/Kobalt-android-arm64.apk build/release/Kobalt-qt5.zip build/release/Kobalt-qt6.zip build/release/Kobalt-sources.tar.gz" "$scratch/gh.log" > /dev/null
done
echo 'PASS: release helper syntax, versions, source pins and missing signing key'
# Changes to bundled native libraries require a fresh notice audit.
mkdir -p "$scratch/apk/lib/arm64-v8a"
while IFS= read -r library; do
    printf 'fixture\n' > "$scratch/apk/lib/arm64-v8a/$library"
done < <(jq -r '.native_libraries[]' dependencies/android-runtime.json)
(cd "$scratch/apk" && cmake -E tar cf "$scratch/notices.apk" --format=zip lib)
bash platforms/android/check-notices.sh "$scratch/notices.apk"
rm "$scratch/apk/lib/arm64-v8a/libc++_shared.so"
(cd "$scratch/apk" && cmake -E tar cf "$scratch/missing-runtime.apk" --format=zip lib)
if bash platforms/android/check-notices.sh "$scratch/missing-runtime.apk" > "$scratch/error" 2>&1; then
    echo 'Notice check accepted a changed APK runtime' >&2; exit 1
fi
# Verify that the staged runtime manifest receives the tag version.
mkdir "$scratch/assets-project"
cat > "$scratch/assets-project/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.22)
project(ReleaseAssets NONE)
add_subdirectory("$ENV{KOBALT_TEST_ROOT}/assets" assets)
file(READ "${KOBALT_ASSETS}/manifest.json" manifest)
string(JSON actual GET "${manifest}" version)
if(NOT actual STREQUAL "v9.8.7-beta.6")
    message(FATAL_ERROR "Runtime manifest did not receive the release version")
endif()
CMAKE
KOBALT_RELEASE_VERSION=v9.8.7-beta.6 cmake -S "$scratch/assets-project" -B "$scratch/assets-build" > "$scratch/configure.log"
# CI runs on Linux with GNU tar, as do the package builders.
if tar --version 2>/dev/null | head -n 1 | grep -q 'GNU tar'; then
    for target in qt5 qt6; do
        if [[ $target == qt5 ]]; then member=usr/local/Kobo/imageformats/libkobalt.so; installer=KoboRoot.tgz
        else member=imageformats/libkobalt.so; installer=Kobo.tgz; fi
        mkdir -p "$scratch/package/$(dirname "$member")" "$scratch/zip/.kobo"
        printf 'fixture\n' > "$scratch/package/$member"
        for owner in 0 501; do
            tar --format=gnu --owner="$owner" --group="$owner" --mode=0755 -czf "$scratch/zip/.kobo/$installer" -C "$scratch/package" "$member"
            (cd "$scratch/zip" && cmake -E tar cf "$scratch/package-$target.zip" --format=zip .kobo)
            if bash platforms/kobo/check-package.sh "$scratch/package-$target.zip" "$target" > "$scratch/package.log" 2>&1; then
                [[ $owner == 0 ]] || { echo 'Accepted a non-root package' >&2; exit 1; }
            else
                [[ $owner != 0 ]] || { cat "$scratch/package.log"; exit 1; }
            fi
        done
        rm "$scratch/zip/.kobo/$installer"
    done
else
    echo 'SKIP: archive ownership regression needs GNU tar (runs in Linux CI)'
fi
echo 'PASS: runtime tag version and package ownership regression'
