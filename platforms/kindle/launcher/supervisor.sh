#!/bin/sh

set -u
APP_DIR=$1
RUN_DIR=$2
shift 2
framework_stopped=no
child=
exit_status=1
volume_held=no
volume_pid=
screensaver_loaded=no
cd / || exit 1

has_screensaver()
{
    modules=$(lipc-get-prop -s -q com.lab126.blanket load) || return 2
    for module in $modules; do
        [ "$module" != screensaver ] || return 0
    done
    return 1
}

restore_screensaver()
{
    [ "$screensaver_loaded" = yes ] || return 0
    lipc-set-prop -s -q com.lab126.blanket load screensaver || return 1
    attempts=0
    while [ "$attempts" -lt 50 ]; do
        if has_screensaver; then
            return 0
        else
            [ "$?" != 2 ] || return 1
        fi
        usleep 100000 || return 1
        attempts=$((attempts + 1))
    done
    return 1
}

stop_bridge()
{
    [ -f "$RUN_DIR/bridge.pid" ] || return 0
    bridge_pid=$(cat "$RUN_DIR/bridge.pid")
    case "$bridge_pid" in ''|*[!0-9]*) return 1 ;; esac
    # Do not signal a recycled PID after the helper has already exited.
    if [ "$(readlink "/proc/$bridge_pid/exe" 2>/dev/null)" = "$RUN_DIR/power-bridge" ]; then
        kill -TERM "$bridge_pid" 2>/dev/null || :
        attempts=0
        while kill -0 "$bridge_pid" 2>/dev/null && [ "$attempts" -lt 3 ]; do
            usleep 1000000 || :
            attempts=$((attempts + 1))
        done
        if [ "$(readlink "/proc/$bridge_pid/exe" 2>/dev/null)" = "$RUN_DIR/power-bridge" ]; then
            kill -KILL "$bridge_pid" 2>/dev/null || :
        fi
    fi
}

restore_waveform_mounts()
{
    waveform_restored=yes
    for probe in "$RUN_DIR"/waveform-*; do
        [ -e "$probe" ] || [ -L "$probe" ] || continue
        if [ ! -d "$probe" ] || [ -L "$probe" ]; then
            waveform_restored=no
            echo "Unexpected waveform recovery entry: $probe" >&2
            continue
        fi
        if [ -r /proc/mounts ] && grep -Fq " $probe " /proc/mounts; then
            if ! umount -i "$probe"; then
                waveform_restored=no
                echo "Could not unmount read-only waveform probe: $probe" >&2
                continue
            fi
        fi
        if ! rmdir "$probe"; then
            waveform_restored=no
            echo "Could not remove waveform probe directory: $probe" >&2
        fi
    done
}

cleanup()
{
    trap '' HUP INT TERM
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || :
        attempts=0
        while kill -0 "$child" 2>/dev/null && [ "$attempts" -lt 100 ]; do
            usleep 100000 || :
            attempts=$((attempts + 1))
        done
        if kill -0 "$child" 2>/dev/null; then
            echo 'Kobalt did not stop within ten seconds; forcing exit before recovery.' >&2
            kill -KILL "$child" 2>/dev/null || :
        fi
        wait "$child" 2>/dev/null || :
    fi
    stop_bridge || echo 'Could not inspect the power bridge PID.' >&2
    restore_waveform_mounts
    if [ -f "$RUN_DIR/launcher.log" ]; then
        cat "$RUN_DIR/launcher.log" >>"$APP_DIR/logs/launcher.log" 2>/dev/null || :
    fi
    if [ "$volume_held" = yes ]; then
        if kill -CONT "$volume_pid" 2>/dev/null; then
            volume_held=no
        else
            echo "Could not resume volumd PID $volume_pid. Recovery files remain in $RUN_DIR." >&2
        fi
    fi
    if [ "$framework_stopped" = yes ]; then
        screen_restored=yes
        if ! restore_screensaver; then
            screen_restored=no
            echo 'Could not reload stock screensaver. Recovery files remain.' >&2
        fi
        if start lab126_gui; then
            framework_stopped=no
        else
            echo "Could not restart lab126_gui. Run 'start lab126_gui' from SSH." >&2
            # Retain the lock and this recovery script after a restoration failure.
            return
        fi
        [ "$screen_restored" = yes ] || return
    fi
    [ "$volume_held" = no ] || return
    [ "$waveform_restored" = yes ] || return
    rm -f "$RUN_DIR/supervisor.sh" "$RUN_DIR/pid" "$RUN_DIR/bridge.pid" "$RUN_DIR/power-bridge" "$RUN_DIR/volume.pid" "$RUN_DIR/launcher.log" "$RUN_DIR/screensaver.loaded"
    rmdir "$RUN_DIR" || :
}
terminate()
{
    if [ -n "$child" ]; then kill -TERM "$child" 2>/dev/null || :; fi
    exit 143
}
trap cleanup EXIT
trap terminate INT TERM
trap '' HUP
echo "$$" >"$RUN_DIR/pid"

state=$(status lab126_gui 2>&1) || { echo "$state" >&2; exit 1; }
case "$state" in
    *'start/running'*) ;;
    *) echo "Refusing to launch: lab126_gui is not running: $state" >&2; exit 1 ;;
esac

if has_screensaver; then
    screensaver_loaded=yes
else
    module_status=$?
    [ "$module_status" != 2 ] || { echo 'Cannot inspect stock screensaver modules.' >&2; exit 1; }
fi
echo "$screensaver_loaded" >"$RUN_DIR/screensaver.loaded"

# Freeze only the current stock volume daemon. It must not unmount USB
# storage while Kobalt still maps its executable, Qt libraries and profile.
volume_state=$(status volumd 2>&1) || { echo "$volume_state" >&2; exit 1; }
case "$volume_state" in
    *'start/running, process '*) volume_pid=${volume_state##*process } ;;
    *) echo "Refusing to launch: volumd is not running: $volume_state" >&2; exit 1 ;;
esac
case "$volume_pid" in ''|*[!0-9]*) echo 'Invalid volumd PID.' >&2; exit 1 ;; esac
# Upstart still reports running when another reader has paused the process.
# Refuse that state rather than taking ownership and later resuming its guard.
volume_process_state=$(ps -o stat= -p "$volume_pid") || exit 1
case "$volume_process_state" in
    ''|*T*|*t*|*Z*|*X*) echo 'Refusing to launch: volumd is stopped or unavailable.' >&2; exit 1 ;;
esac
echo "$volume_pid" >"$RUN_DIR/volume.pid"
KOBALT_KINDLE_USB_NETWORK=$(lipc-get-prop -i -q com.lab126.volumd useUsbForNetwork) || KOBALT_KINDLE_USB_NETWORK=0
export KOBALT_KINDLE_USB_NETWORK
# Record the cleanup obligation before STOP. TERM can arrive immediately
# after the signal, before the shell executes its next assignment.
volume_held=yes
kill -STOP "$volume_pid" || exit 1

# Stopping the framework may signal the KUAL process tree. Ignore TERM until
# ownership is recorded, as in the upstream Kindle framework-stop launch flow.
trap '' INT TERM
if stop lab126_gui; then
    framework_stopped=yes
    # Upstream KOReader waits for the final framework black frame and delayed
    # TERM before restoring its handler and starting a framebuffer client.
    usleep 1250000 || exit 1
else
    trap terminate INT TERM
    echo 'Could not stop lab126_gui; Kobalt was not started.' >&2
    exit 1
fi
trap terminate INT TERM

export QT_QPA_PLATFORM=kindle
export QT_QPA_PLATFORM_PLUGIN_PATH="$APP_DIR/plugins/platforms"
export QT_PLUGIN_PATH="$APP_DIR/plugins"
export QT_QPA_FONTDIR="$APP_DIR/assets/fonts"
export LD_LIBRARY_PATH="$APP_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export KOBALT_KINDLE_HOME="$APP_DIR"
export KOBALT_KINDLE_RUN_DIR="$RUN_DIR"
export KOBALT_KINDLE_POWER_BRIDGE="$RUN_DIR/power-bridge"
"$APP_DIR/bin/kobalt" --assets "$APP_DIR/assets" "$@" &
child=$!
wait "$child"
exit_status=$?
child=
exit "$exit_status"
