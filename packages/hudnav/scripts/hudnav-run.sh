#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
#
# TARGET: AU Linux Gen5W 260319 head unit, run as root.
#
# Runs hudnavd the only way it works: --appmanager-stdin must be FED the session-bus
# AppManager ownership signals (which app owns guidance: native 30, CarPlay 110,
# AA 111). If stdin is /dev/null or EOF -- plain `hudnavd --appmanager-stdin &`, or a
# systemd unit -- hudnavd flips to "native nav owns guidance" and stays silent forever,
# which looks exactly like "the shim never fired". This script does the plumbing
# (dbus-monitor -> fifo -> hudnavd stdin), the same plumbing run-aa-diagnostic.sh uses.
#
# Modes (the only difference is what hudnavd is allowed to do):
#   diag  hudnavd-diag: log backend only, CAN TX is not even compiled in. Logs every
#         line the shim sends ("RX ...") and every ownership change ("APPMANAGER owner=").
#         Use this first: it is the only mode that can tell "shim never fired" apart from
#         "event arrived but was gated".
#   dry   hudnavd --can WITHOUT --micom-send: prints the exact MICOM frames it WOULD send
#         ("MICOM SetCANMicomRecv array:..."). Still transmits nothing.
#   tx    dry + --micom-send. THIS TRANSMITS CAN. Needs --yes, and a kill switch file:
#         `pause` removes it (daemon emits a clear and goes quiet), `resume` restores it.
#
# Usage: sh hudnav-run.sh start diag|dry|tx [--yes]   background, survives ssh drop
#        sh hudnav-run.sh run   diag|dry|tx [--yes]   foreground (debugging)
#        sh hudnav-run.sh status | log [N] | pause | resume | stop
set -u
PATH="${HUDNAV_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
DIR="${HUDNAV_RW_DIR:-/rw_data/hudnav}"
BIN="$DIR/bin"
RUN="${HUDNAV_RUN_DIR:-/tmp/hudnav-run}"
SOCK="${HUDNAV_SOCK:-/tmp/hudnav.sock}"
STATUS="${HUDNAV_STATUS:-/tmp/hudnav-status.json}"   # Projection+ status (tmpfs)
SESSION_FILE="${HUDNAV_SESSION_FILE:-/tmp/session_appmgr}"
SELF="$0"

die() { echo "FAIL: $*" >&2; exit 1; }

runner_pid() { [ -f "$RUN/runner.pid" ] && cat "$RUN/runner.pid" 2>/dev/null; }
alive() { [ -n "${1:-}" ] && kill -0 "$1" 2>/dev/null; }

do_run() {
    mode=$1; yes=${2:-}
    case "$mode" in
        diag) [ -x "$BIN/hudnavd-diag" ] || die "missing $BIN/hudnavd-diag (run install-on-hu.sh)" ;;
        dry)  [ -x "$BIN/hudnavd" ] || die "missing $BIN/hudnavd" ;;
        tx)   [ "$yes" = "--yes" ] || die "tx transmits CAN; refusing without --yes"
              [ -x "$BIN/hudnavd" ] && [ -x "$BIN/micom-send" ] || die "missing hudnavd/micom-send" ;;
        *)    die "mode must be diag, dry or tx" ;;
    esac
    old=$(runner_pid)
    if alive "$old"; then die "already running (pid $old); run 'stop' first"; fi
    sb=$(sed 's/,guid=.*//' "$SESSION_FILE" 2>/dev/null)
    [ -n "$sb" ] || die "no session bus address in $SESSION_FILE (is the unit fully booted?)"

    umask 077
    mkdir -p "$RUN" || die "cannot create $RUN"
    echo $$ > "$RUN/runner.pid"
    echo "$mode" > "$RUN/mode"
    rm -f "$RUN/owners" "$SOCK"
    mkfifo "$RUN/owners" || die "mkfifo failed"
    mon=""; dae=""
    cleanup() {
        for p in $dae $mon; do kill "$p" 2>/dev/null; done
        rm -f "$RUN/owners" "$SOCK" "$RUN/runner.pid" "$RUN/mode"
    }
    trap cleanup EXIT
    trap 'exit 143' TERM HUP
    trap 'exit 130' INT

    dbus-monitor --address "$sb" \
        "type='signal',sender='com.lge.hyundai.avn.appmanager',path='/Information',interface='com.lge.avn.appmanager.info',member='navi_route_guide_application'" \
        "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='com.lge.hyundai.avn.appmanager'" \
        > "$RUN/owners" 2> "$RUN/monitor.log" &
    mon=$!

    case "$mode" in
        diag) set -- "$BIN/hudnavd-diag" --appmanager-stdin --socket "$SOCK" ;;
        dry)  set -- "$BIN/hudnavd" --can --appmanager-stdin --socket "$SOCK" \
                     --kit "$DIR" --status "$STATUS" ;;
        tx)   : > "$RUN/tx.enable"
              set -- "$BIN/hudnavd" --can --appmanager-stdin --socket "$SOCK" \
                     --enabled "$RUN/tx.enable" --micom-send "$BIN/micom-send" \
                     --kit "$DIR" --status "$STATUS" ;;
    esac
    echo "== $(date -u 2>/dev/null) start mode=$mode: $*" >> "$RUN/navigation.log"
    "$@" < "$RUN/owners" >> "$RUN/navigation.log" 2>&1 &
    dae=$!
    wait "$dae"
    rc=$?
    echo "== $(date -u 2>/dev/null) hudnavd exited rc=$rc" >> "$RUN/navigation.log"
    exit "$rc"
}

case "${1:-status}" in
    start)
        mode=${2:-}; yes=${3:-}
        # Validate synchronously so a bad mode/missing file is reported on the terminal,
        # not buried in the background runner's log.
        case "$mode" in
            diag|dry) ;;
            tx) [ "$yes" = "--yes" ] || die "tx transmits CAN; refusing without --yes" ;;
            *) die "usage: $SELF start diag|dry|tx [--yes]" ;;
        esac
        mkdir -p "$RUN"
        setsid sh "$SELF" run "$mode" "$yes" < /dev/null > "$RUN/runner.out" 2>&1 &
        sleep 2
        pid=$(runner_pid)
        if alive "$pid"; then
            echo "started: mode=$mode runner=$pid log=$RUN/navigation.log sock=$SOCK"
        else
            echo "runner did not stay up:" >&2
            cat "$RUN/runner.out" >&2 2>/dev/null
            exit 1
        fi
        ;;
    run)
        do_run "${2:-}" "${3:-}"
        ;;
    status)
        pid=$(runner_pid)
        if alive "$pid"; then
            echo "running: pid=$pid mode=$(cat "$RUN/mode" 2>/dev/null)"
            [ -e "$SOCK" ] && ls -l "$SOCK" || echo "socket $SOCK: missing"
            [ -f "$RUN/tx.enable" ] && echo "tx.enable: PRESENT (transmitting enabled)" || true
            echo "--- last log lines ---"
            tail -n 8 "$RUN/navigation.log" 2>/dev/null
        else
            echo "not running"
            tail -n 5 "$RUN/runner.out" 2>/dev/null
        fi
        ;;
    log)
        tail -n "${2:-40}" "$RUN/navigation.log" 2>/dev/null || echo "(no log yet)"
        ;;
    pause)
        [ -f "$RUN/mode" ] && [ "$(cat "$RUN/mode")" = tx ] || die "pause only applies to tx mode"
        rm -f "$RUN/tx.enable"
        echo "tx paused (daemon clears the cluster and goes quiet within ~1s)"
        ;;
    resume)
        [ -f "$RUN/mode" ] && [ "$(cat "$RUN/mode")" = tx ] || die "resume only applies to tx mode"
        : > "$RUN/tx.enable"
        echo "tx resumed"
        ;;
    stop)
        pid=$(runner_pid)
        alive "$pid" || { echo "not running"; exit 0; }
        # If transmitting, let the daemon send its clear frames before the kill.
        if [ "$(cat "$RUN/mode" 2>/dev/null)" = tx ]; then
            rm -f "$RUN/tx.enable"
            sleep 1
        fi
        kill "$pid" 2>/dev/null
        for _ in 1 2 3 4 5 6 7 8 9 10; do alive "$pid" || break; sleep 1; done
        alive "$pid" && kill -9 "$pid" 2>/dev/null
        echo "stopped"
        ;;
    *)
        echo "usage: sh hudnav-run.sh start diag|dry|tx [--yes] | run ... | status | log [N] | pause | resume | stop" >&2
        exit 2
        ;;
esac
