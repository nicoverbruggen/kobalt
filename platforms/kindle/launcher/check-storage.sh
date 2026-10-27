#!/bin/sh

# Check the most specific mount covering the installed app before stopping UI.
set -eu
path=${1:?application directory required}
mounts=${2:-/proc/mounts}
case "$path" in /*) ;; *) exit 1 ;; esac
best=
options=
while IFS=' ' read -r source mount type flags rest; do
    # Linux escapes whitespace and backslashes in /proc/mounts paths.
    mount=$(printf '%b' "$mount")
    case "$path" in
        "$mount"|"$mount"/*) ;;
        *) [ "$mount" = / ] || continue ;;
    esac
    if [ "${#mount}" -ge "${#best}" ]; then
        best=$mount
        options=$flags
    fi
done <"$mounts"
[ -n "$best" ] || { echo 'Cannot determine Kobalt storage mount options.' >&2; exit 1; }
case ",$options," in
    *,noexec,*)
        echo 'Kobalt storage does not permit execution (noexec). Stock UI was not stopped.' >&2
        exit 1
        ;;
esac
