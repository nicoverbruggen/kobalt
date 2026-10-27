#!/bin/sh

# KUAL invokes this wrapper. The supervisor survives both its caller and an app crash.
set -eu
APP_DIR=$(CDPATH='' cd "$(dirname "$0")/.." && pwd -P)
if [ "$(uname -s)" != Linux ] || [ ! -e /lib/ld-linux-armhf.so.3 ]; then
    echo 'Kobalt Kindle requires a hard-float Kindle Linux system.' >&2
    exit 1
fi
if [ -e /var/local/system/userpasswdenabled ]; then
    echo 'Kobalt prototype cannot run with the Kindle passcode enabled.' >&2
    exit 1
fi
[ -x "$APP_DIR/bin/kobalt" ] || { echo 'Kobalt executable is missing.' >&2; exit 1; }
[ -x "$APP_DIR/bin/kobalt-power-bridge" ] || { echo 'Kindle power bridge is missing.' >&2; exit 1; }
[ -f "$APP_DIR/launcher/supervisor.sh" ] || exit 1
for tool in status start stop nohup usleep readlink ps grep umount lipc-get-prop lipc-set-prop; do
    command -v "$tool" >/dev/null || { echo "Required Kindle command missing: $tool" >&2; exit 1; }
done

# A stale lock is kept after an uncatchable supervisor failure. Do not start a
# second reader over a UI whose ownership needs to be checked first.
RUN_DIR=/var/tmp/kobalt-kindle-prototype
mkdir -p "$APP_DIR/logs"
if ! mkdir "$RUN_DIR" 2>/dev/null; then
    echo "Kobalt is running, or its previous supervisor failed. Inspect $RUN_DIR before removing it." >&2
    exit 1
fi
trap 'rm -f "$RUN_DIR/supervisor.sh" "$RUN_DIR/power-bridge"; rmdir "$RUN_DIR" 2>/dev/null || :' EXIT
chmod 700 "$RUN_DIR"
if ! cp "$APP_DIR/launcher/supervisor.sh" "$RUN_DIR/supervisor.sh"; then
    rmdir "$RUN_DIR"
    exit 1
fi
cp "$APP_DIR/bin/kobalt-power-bridge" "$RUN_DIR/power-bridge"
chmod 700 "$RUN_DIR/power-bridge"
# Keep the supervisor and its open files on system storage. USB export must
# not depend on a shell or log descriptor inside /mnt/us.
cd /
nohup sh "$RUN_DIR/supervisor.sh" "$APP_DIR" "$RUN_DIR" "$@" >>"$RUN_DIR/launcher.log" 2>&1 </dev/null &
trap - EXIT
echo "Kobalt supervisor started: $!"
