#!/bin/sh

# Mock only command boundaries. This exercises the real supervisor and its traps.
set -eu
SUPERVISOR=$(CDPATH='' cd "$(dirname "$0")" && pwd -P)/supervisor.sh
TEST_ROOT=${1:?pass an ignored workspace scratch directory}
mkdir -p "$TEST_ROOT"
TEST_ROOT=$(CDPATH='' cd "$TEST_ROOT" && pwd -P)
TEST_DIR=$(mktemp -d "$TEST_ROOT/supervisor-test.XXXXXX")
sleep 120 &
KINDLE_TEST_VOLUME_PID=$!
export KINDLE_TEST_VOLUME_PID
trap 'kill -CONT "$KINDLE_TEST_VOLUME_PID" 2>/dev/null || :; kill -TERM "$KINDLE_TEST_VOLUME_PID" 2>/dev/null || :; wait "$KINDLE_TEST_VOLUME_PID" 2>/dev/null || :; rm -rf "$TEST_DIR"' EXIT HUP INT TERM
mkdir -p "$TEST_DIR/tools" "$TEST_DIR/app/bin" "$TEST_DIR/app/logs"
export KINDLE_TEST_ROOT="$TEST_DIR"
cat >"$TEST_DIR/tools/status" <<'EOF'
#!/bin/sh
if [ "$1" = volumd ]; then echo "volumd start/running, process $KINDLE_TEST_VOLUME_PID"; exit 0; fi
echo 'lab126_gui start/running, process 123'
EOF
cat >"$TEST_DIR/tools/stop" <<'EOF'
#!/bin/sh
echo stop >>"$KINDLE_TEST_ROOT/actions"
[ "${KINDLE_TEST_STOP_FAIL:-0}" = 0 ]
EOF
cat >"$TEST_DIR/tools/start" <<'EOF'
#!/bin/sh
echo start >>"$KINDLE_TEST_ROOT/actions"
[ "${KINDLE_TEST_START_FAIL:-0}" = 0 ]
EOF
cat >"$TEST_DIR/tools/usleep" <<'EOF'
#!/bin/sh
case "$1" in
1250000) echo delay >>"$KINDLE_TEST_ROOT/delays" ;;
100000) sleep 0.001 ;;
*) exit 1 ;;
esac
EOF
cat >"$TEST_DIR/app/bin/kobalt" <<'EOF'
#!/bin/sh
case "$(ps -o stat= -p "$KINDLE_TEST_VOLUME_PID")" in *T*) ;; *) exit 99 ;; esac
[ "$(pwd)" = / ] || exit 98
echo app >>"$KINDLE_TEST_ROOT/actions"
mkdir "$KOBALT_KINDLE_RUN_DIR/waveform-abandoned"
exit "${KINDLE_TEST_APP_EXIT:-0}"
EOF
chmod +x "$TEST_DIR/tools/status" "$TEST_DIR/tools/start" "$TEST_DIR/tools/stop" "$TEST_DIR/tools/usleep" "$TEST_DIR/app/bin/kobalt"
printf '#!/bin/sh\ncase "$4" in load) echo "${KINDLE_TEST_MODULES-screensaver langpicker blankwindow}" ;; *) echo 1 ;; esac\n' >"$TEST_DIR/tools/lipc-get-prop"
chmod +x "$TEST_DIR/tools/lipc-get-prop"
printf '#!/bin/sh\nexit "${KINDLE_TEST_SCREEN_RESTORE_FAIL:-0}"\n' >"$TEST_DIR/tools/lipc-set-prop"
chmod +x "$TEST_DIR/tools/lipc-set-prop"
export PATH="$TEST_DIR/tools:$PATH"

run_case()
{
    mkdir "$TEST_DIR/run"
    cp "$SUPERVISOR" "$TEST_DIR/run/supervisor.sh"
    : >"$TEST_DIR/actions"
    : >"$TEST_DIR/delays"
    result=0
    sh "$TEST_DIR/run/supervisor.sh" "$TEST_DIR/app" "$TEST_DIR/run" || result=$?
    kill -0 "$KINDLE_TEST_VOLUME_PID"
    case "$(ps -o stat= -p "$KINDLE_TEST_VOLUME_PID")" in *T*) echo 'Volume daemon was not resumed.' >&2; exit 1 ;; esac
}

# A volume daemon already paused by another reader is not ours to resume.
kill -STOP "$KINDLE_TEST_VOLUME_PID"
mkdir "$TEST_DIR/run"
cp "$SUPERVISOR" "$TEST_DIR/run/supervisor.sh"
: >"$TEST_DIR/actions"
result=0
sh "$TEST_DIR/run/supervisor.sh" "$TEST_DIR/app" "$TEST_DIR/run" || result=$?
[ "$result" = 1 ] && [ ! -e "$TEST_DIR/run" ] && [ ! -s "$TEST_DIR/actions" ]
case "$(ps -o stat= -p "$KINDLE_TEST_VOLUME_PID")" in *T*) ;; *) echo 'Another reader lost its volume guard.' >&2; exit 1 ;; esac
kill -CONT "$KINDLE_TEST_VOLUME_PID"

run_case
[ "$result" = 0 ] && [ ! -e "$TEST_DIR/run" ]
[ "$(cat "$TEST_DIR/delays")" = delay ]
[ "$(cat "$TEST_DIR/actions")" = "$(printf 'stop\napp\nstart')" ]
export KINDLE_TEST_APP_EXIT=23
run_case
[ "$result" = 23 ] && [ ! -e "$TEST_DIR/run" ]
[ "$(cat "$TEST_DIR/actions")" = "$(printf 'stop\napp\nstart')" ]
export KINDLE_TEST_STOP_FAIL=1
run_case
[ "$result" = 1 ] && [ ! -e "$TEST_DIR/run" ]
[ ! -s "$TEST_DIR/delays" ]
[ "$(cat "$TEST_DIR/actions")" = stop ]
export KINDLE_TEST_STOP_FAIL=0 KINDLE_TEST_START_FAIL=1
run_case
[ "$result" = 23 ] && [ -f "$TEST_DIR/run/supervisor.sh" ]
[ "$(cat "$TEST_DIR/actions")" = "$(printf 'stop\napp\nstart')" ]
rm -rf "$TEST_DIR/run"

export KINDLE_TEST_START_FAIL=0 KINDLE_TEST_SCREEN_RESTORE_FAIL=1
run_case
[ "$result" = 23 ] && [ -f "$TEST_DIR/run/supervisor.sh" ]
[ "$(cat "$TEST_DIR/actions")" = "$(printf 'stop\napp\nstart')" ]
rm -rf "$TEST_DIR/run"
# A screensaver module that was absent before launch is not ours to reload.
export KINDLE_TEST_MODULES=
run_case
[ "$result" = 23 ] && [ ! -e "$TEST_DIR/run" ]
unset KINDLE_TEST_MODULES KINDLE_TEST_SCREEN_RESTORE_FAIL

# Interrupt exactly after STOP, before its shell command returns. Recording
# the cleanup obligation afterward would leave the mock daemon frozen.
cat >"$TEST_DIR/interrupt-after-stop" <<'EOF'
#!/bin/sh
source=$1
shift
kill() {
    if command kill "$@"; then
        if [ "$1" = -STOP ]; then command kill -TERM "$$"; fi
        return 0
    fi
    return 1
}
. "$source"
EOF
mkdir "$TEST_DIR/run"
cp "$SUPERVISOR" "$TEST_DIR/run/supervisor.sh"
: >"$TEST_DIR/actions"
result=0
sh "$TEST_DIR/interrupt-after-stop" "$TEST_DIR/run/supervisor.sh" "$TEST_DIR/app" "$TEST_DIR/run" || result=$?
[ "$result" = 143 ] && [ ! -e "$TEST_DIR/run" ] && [ ! -s "$TEST_DIR/actions" ]
case "$(ps -o stat= -p "$KINDLE_TEST_VOLUME_PID")" in *T*) echo 'Interrupted STOP leaked volume ownership.' >&2; exit 1 ;; esac

# An already stopped framework belongs to somebody else.
cat >"$TEST_DIR/tools/status" <<'EOF'
#!/bin/sh
if [ "$1" = volumd ]; then echo "volumd start/running, process $KINDLE_TEST_VOLUME_PID"; exit 0; fi
echo 'lab126_gui stop/waiting'
EOF
run_case
[ "$result" = 1 ] && [ ! -e "$TEST_DIR/run" ]
[ ! -s "$TEST_DIR/actions" ]

# TERM must let the app checkpoint before restoring the framework.
cat >"$TEST_DIR/tools/status" <<'EOF'
#!/bin/sh
if [ "$1" = volumd ]; then echo "volumd start/running, process $KINDLE_TEST_VOLUME_PID"; exit 0; fi
echo 'lab126_gui start/running, process 123'
EOF
cat >"$TEST_DIR/app/bin/kobalt" <<'EOF'
#!/bin/sh
trap 'echo saved >>"$KINDLE_TEST_ROOT/actions"; exit 0' TERM
echo app >>"$KINDLE_TEST_ROOT/actions"
while :; do :; done
EOF
export KINDLE_TEST_START_FAIL=0
mkdir "$TEST_DIR/run"
cp "$SUPERVISOR" "$TEST_DIR/run/supervisor.sh"
: >"$TEST_DIR/actions"
sh "$TEST_DIR/run/supervisor.sh" "$TEST_DIR/app" "$TEST_DIR/run" &
supervisor_pid=$!
attempt=0
until [ "$(tail -n 1 "$TEST_DIR/actions")" = app ]; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 100 ]; then
        kill -TERM "$supervisor_pid" 2>/dev/null || :
        wait "$supervisor_pid" || :
        echo 'Timed out waiting for test app.' >&2
        exit 1
    fi
    sleep 0.01
done
kill -TERM "$supervisor_pid"
result=0
wait "$supervisor_pid" || result=$?
[ "$result" = 143 ] && [ ! -e "$TEST_DIR/run" ]
[ "$(cat "$TEST_DIR/actions")" = "$(printf 'stop\napp\nsaved\nstart')" ]
# A hung application cannot hold the stock UI and volume daemon forever.
cat >"$TEST_DIR/app/bin/kobalt" <<'EOF'
#!/bin/sh
trap '' TERM
echo app >>"$KINDLE_TEST_ROOT/actions"
while :; do :; done
EOF
mkdir "$TEST_DIR/run"
cp "$SUPERVISOR" "$TEST_DIR/run/supervisor.sh"
: >"$TEST_DIR/actions"
sh "$TEST_DIR/run/supervisor.sh" "$TEST_DIR/app" "$TEST_DIR/run" &
supervisor_pid=$!
attempt=0
until [ "$(tail -n 1 "$TEST_DIR/actions")" = app ]; do
    attempt=$((attempt + 1))
    [ "$attempt" -lt 100 ] || { kill -TERM "$supervisor_pid"; wait "$supervisor_pid" || :; exit 1; }
    sleep 0.01
done
kill -TERM "$supervisor_pid"
result=0
wait "$supervisor_pid" || result=$?
[ "$result" = 143 ] && [ ! -e "$TEST_DIR/run" ]
[ "$(cat "$TEST_DIR/actions")" = "$(printf 'stop\napp\nstart')" ]
case "$(ps -o stat= -p "$KINDLE_TEST_VOLUME_PID")" in *T*) exit 1 ;; esac
echo 'Kindle supervisor checks passed.'
