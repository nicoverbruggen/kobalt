#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$root/build"
scratch=$(mktemp -d "$root/build/launcher-cleanup-test.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
cat > "$scratch/engine" <<'ENGINE'
#!/bin/bash
set -euo pipefail
case $1 in
    image)
        if [[ ${KOBALT_FIXTURE_ANDROID:-0} == 1 ]]; then echo 'arm64 fixture-sdk'; else echo fixture-sdk; fi
        ;;
    run)
        # A successful simulated package run would record a real validation pass.
        [[ ${KOBALT_FIXTURE_ANDROID:-0} == 1 || ${!#} == --check ]] || { echo "Fixture cannot validate a Kobo package" >&2; exit 1; }
        shift
        while [[ $# -gt 0 ]]; do
            if [[ $1 == --cidfile ]]; then printf '%s\n' owned-launcher-container > "$2"; break; fi
            shift
        done
        if [[ ${KOBALT_FIXTURE_ANDROID:-0} == 1 ]]; then
            # The client disconnects while the container is still running.
            kill -TERM "$PPID"
            exit 143
        fi
        exit "${KOBALT_FIXTURE_EXIT:-0}"
        ;;
    rm)
        [[ $2 == -f && $3 == owned-launcher-container ]]
        echo "$3" >> "$KOBALT_CLEANUP_LOG"
        ;;
    *) exit 99 ;;
esac
ENGINE
chmod +x "$scratch/engine"
# A zero CPU quota would run the container without the build's CPU limit.
for quota in 0 0.00; do
    actual=0
    KOBALT_LOAD_LOCK_HELD=1 KOBALT_CHECK_SOURCE_ROOT= KOBALT_DOCKER_CPUS=$quota \
        CONTAINER_ENGINE="$scratch/engine" KOBALT_CLEANUP_LOG="$scratch/removed" \
        bash "$root/platforms/kobo/build.sh" qt5 --check > "$scratch/output" 2>&1 || actual=$?
    [[ $actual == 2 && ! -e "$scratch/removed" ]] || {
        echo "Expected zero CPU quota $quota to be rejected before the build; got status $actual"
        exit 1
    }
done
# Cleanup is shared with SDK checks. A fixture engine must never record a package pass.
for result in 0 17; do
    actual=0
    KOBALT_LOAD_LOCK_HELD=1 KOBALT_CHECK_SOURCE_ROOT= CONTAINER_ENGINE="$scratch/engine" \
        KOBALT_FIXTURE_EXIT=$result KOBALT_CLEANUP_LOG="$scratch/removed" \
        bash "$root/platforms/kobo/build.sh" qt5 --check > "$scratch/output" 2>&1 || actual=$?
    [[ $actual == "$result" ]] || { cat "$scratch/output"; exit 1; }
    [[ $(cat "$scratch/removed") == owned-launcher-container ]] || exit 1
    rm "$scratch/removed"
done
echo 'PASS: successful and failed Kobo builds remove only their owned container and preserve exit status'
actual=0
KOBALT_FIXTURE_ANDROID=1 KOBALT_ANDROID_RELEASE=0 CONTAINER_ENGINE="$scratch/engine" \
    KOBALT_CLEANUP_LOG="$scratch/removed" bash "$root/platforms/android/build.sh" \
    > "$scratch/output" 2>&1 || actual=$?
[[ $actual == 143 && -s "$scratch/removed" && $(cat "$scratch/removed") == owned-launcher-container ]] || {
    cat "$scratch/output"
    echo "Interrupted Android build did not remove its container (status $actual)"
    exit 1
}
echo 'PASS: interrupted Android builds remove their owned container'
