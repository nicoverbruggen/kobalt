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
    image) echo fixture-sdk ;;
    run)
        # A successful simulated package run would record a real validation pass.
        [[ ${!#} == --check ]] || { echo "Fixture cannot validate a Kobo package" >&2; exit 1; }
        shift
        while [[ $# -gt 0 ]]; do
            if [[ $1 == --cidfile ]]; then printf '%s\n' owned-launcher-container > "$2"; break; fi
            shift
        done
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
