#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
#
# TARGET: AU Linux Gen5W 260319 head unit, run as root.
#
# Replays VERBATIM stock nav frames (captured from AppNavi's own DLT log, see
# capture/make_replay.py and docs/hudnav/HUD_NAV_REPORT.md) through the proven
# micom path, with no navigation daemon and no converter involved. It answers one question:
# does the cluster/HUD draw a frame the car itself would send? If not, the problem is
# delivery or context, not our payload.
#
# THIS TRANSMITS CAN when run with --yes. Without --yes it only validates and prints.
#   - only these IDs are accepted: 4cc 63e 640 641 642 4e8 4df 646 4a3 (anything else aborts BEFORE sending)
#   - `tp <hex bytes>` lines send a CAN-TP text message (id_hi id_lo len sid payload.., len excludes the sid); the id must be 06e7
#     (the cluster text channel). `PAUSE <seconds>` lines wait inside a step.
#   - kill switch: `touch /tmp/hudnav-replay.stop` (or Ctrl-C / kill): stops after the current
#     frame and sends the stock clear frames
#   - the stock clear frames are always sent last (twice)
#
# Usage: sh hudnav-replay.sh FILE [--yes] [--step SECONDS] [--no-clear]
#   FILE   replay-right.txt etc: `STEP` lines separate timed groups, then `<id> <8 hex bytes>`
set -u
PATH="${HUDNAV_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
DIR="${HUDNAV_RW_DIR:-/rw_data/hudnav}"
SEND="${HUDNAV_MICOM_SEND:-$DIR/bin/micom-send}"
STOP="${HUDNAV_REPLAY_STOP:-/tmp/hudnav-replay.stop}"
STEP_S="${HUDNAV_REPLAY_STEP_S:-1}"
ALLOWED=" 4cc 63e 640 641 642 4e8 4df 646 4a3 "
CANSVC="${HUDNAV_CANSERVICE:-$DIR/bin/CANService}"

die() { echo "FAIL: $*" >&2; exit 1; }

FILE=""; YES=""; GUIDE=""; NOCLEAR=""
while [ $# -gt 0 ]; do
    case "$1" in
        --yes) YES=1 ;;
        # EXPERIMENT: also send CANService's 0x4A3 with byte 6 = 01 (the stock "guiding" flag, set
        # when the first maneuver appears) before the replay, and restore the stock idle frame
        # after. 0x4A3 is never accepted from a replay FILE, only via this explicit option.
        --guide-flag) GUIDE=1 ;;
        # EXPERIMENT: leave the last frames on the display instead of sending the clear frames after a
        # normal finish (an interrupt / kill switch / send failure still sends the clear).
        --no-clear) NOCLEAR=1 ;;
        --step) shift; STEP_S="${1:?--step needs seconds}" ;;
        -*) die "unknown option $1" ;;
        *) [ -z "$FILE" ] || die "one file only"; FILE="$1" ;;
    esac
    shift
done
[ -n "$FILE" ] && [ -f "$FILE" ] || die "usage: sh hudnav-replay.sh FILE [--yes] [--step SECONDS] [--no-clear]"

# --- validate everything before sending anything -------------------------------------
n_steps=0; n_frames=0; ln=0
while IFS= read -r line; do
    ln=$((ln + 1))
    case "$line" in ''|'#'*) continue ;; STEP) n_steps=$((n_steps + 1)); continue ;; esac
    case "$line" in
        PAUSE\ *) set -- $line; [ $# -eq 2 ] || die "line $ln: want 'PAUSE <seconds>'"
                  case "$2" in ''|*[!0-9.]*) die "line $ln: bad pause '$2'" ;; esac; continue ;;
        tp\ *) set -- $line; shift
               [ $# -ge 4 ] && [ $# -le 135 ] || die "line $ln: tp needs 4..135 bytes"
               [ "$1$2" = 06e7 ] || die "line $ln: tp id $1$2 is not 06e7"
               for b in "$@"; do case "$b" in [0-9a-f][0-9a-f]) ;; *) die "line $ln: bad tp byte '$b'" ;; esac; done
               [ "$((0x$3 + 4))" -eq $# ] || die "line $ln: tp length byte 0x$3 does not match $# bytes"
               n_frames=$((n_frames + 1)); continue ;;
    esac
    set -- $line
    [ $# -eq 9 ] || die "line $ln: want '<id> <8 bytes>', got: $line"
    id=$1; shift
    case "$ALLOWED" in *" $id "*) ;; *) die "line $ln: id $id is not in the allowed set$ALLOWED" ;; esac
    for b in "$@"; do
        case "$b" in [0-9a-f][0-9a-f]) ;; *) die "line $ln: bad byte '$b'" ;; esac
    done
    n_frames=$((n_frames + 1))
done < "$FILE"
[ "$n_frames" -gt 0 ] || die "no frames in $FILE"

if [ -z "$YES" ]; then
    echo "DRY RUN (no --yes): $FILE is valid: $n_steps steps, $n_frames frames. Nothing sent."
    echo "first frames:"; grep -v '^#' "$FILE" | head -n 4 | sed 's/^/  /'
    exit 0
fi
[ -x "$SEND" ] || die "missing $SEND (run install-on-hu.sh first)"

# --- send --------------------------------------------------------------------------------
send_frame() {   # send_frame ID b0 .. b7
    id=$1; shift
    case ${#id} in 3) id4="0$id" ;; *) id4=$id ;; esac
    hi=${id4%??}; lo=${id4#??}
    arr="array:byte:0x$hi,0x$lo,0x08"
    for b in "$@"; do arr="$arr,0x$b"; done
    "$SEND" "$arr" || { fails=$((fails + 1)); echo "  send failed ($fails): $id" >&2; return 1; }
    fails=0
}
send_tp() {   # send_tp b0 .. bn  (id_hi id_lo len sid payload.., len excludes the sid)
    arr="array:byte:0x$1"; shift
    for b in "$@"; do arr="$arr,0x$b"; done
    out=$("$CANSVC" --system --type=method_call --print-reply --reply-timeout=500         --dest=com.lge.car.micom /Can com.lge.car.micom.can.SetCANTPMicomRecv "$arr" 2>&1); rc=$?
    case "$out" in *NoReply*|*"Did not receive a reply"*) rc=0 ;; esac
    [ "$rc" -eq 0 ] || { fails=$((fails + 1)); echo "  tp send failed ($fails)" >&2; return 1; }
    fails=0
}
send_clear() {
    echo "-- sending stock clear frames"
    for pass in 1 2; do
        # Stock END-OF-ROUTE frames (AppNavi, 2026-11-25 10:36:11). NOT 0x4CC type 09: that is the
        # stock "Calculating" state (sent at route START) and leaves the HUD showing a spinner.
        send_frame 641 ff ff ff ff 0f ff ff ff
        send_frame 642 ff ff ff ff ff ff 00 00
        send_frame 63e ff ff ff ff ff ff ff ff
        send_frame 640 ff ff ff 00 ff ff ff 00
        send_frame 4cc 03 ff ff ff ff ff ff f0
        send_frame 4e8 ff ff ff ff ff ff ff 00
        # 03 alone = "arrived" (HUD draws the checkered flag; seen on the car). The stock nav
        # follows it at once with idle compass frames, which return the display to normal.
        send_frame 4cc 08 00 00 21 00 00 00 00
        send_frame 4e8 ff ff ff 21 ff ff ff 00
        [ "$pass" = 1 ] && sleep 0.3
    done
    # restore CANService's stock idle 0x4A3 (observed: 40 00 24 08 00 00 00 00)
    [ -n "$GUIDE" ] && send_frame 4a3 40 00 24 08 00 00 00 00
}
fails=0; cleaned=""
finish() { [ -n "$cleaned" ] && return; cleaned=1; send_clear; rm -f "$STOP"; }
trap 'echo "-- interrupted"; finish; exit 130' INT TERM HUP
rm -f "$STOP"

echo "REPLAY $FILE: $n_steps steps, $n_frames frames, ${STEP_S}s/step. Stop: touch $STOP"
step=0; stopped=""
if [ -n "$GUIDE" ]; then
    echo "-- guidance flag: 0x4A3 byte 6 = 01 (restored to idle at the end)"
    send_frame 4a3 40 00 24 08 00 00 01 00
fi
while IFS= read -r line; do
    case "$line" in ''|'#'*) continue ;; esac
    if [ -e "$STOP" ]; then stopped=1; break; fi
    if [ "$fails" -ge 3 ]; then echo "-- 3 consecutive send failures, aborting" >&2; stopped=1; break; fi
    if [ "$line" = STEP ]; then
        [ "$step" -gt 0 ] && sleep "$STEP_S"
        step=$((step + 1)); continue
    fi
    case "$line" in
        PAUSE\ *) set -- $line; sleep "$2"; continue ;;
        tp\ *) set -- $line; shift; echo "step $step/$n_steps  TP sid=0x$4 len=0x$3"; send_tp "$@"; continue ;;
    esac
    set -- $line
    id=$1; shift
    if [ "$id" = 4cc ]; then
        echo "step $step/$n_steps  0x4CC type=0x$1 dir=0x$4 dist=$((0x$6$5)) m"
    fi
    send_frame "$id" "$@"
done < "$FILE"
[ -n "$stopped" ] && echo "-- stopped early"
if [ -n "$NOCLEAR" ] && [ -z "$stopped" ]; then
    echo "-- --no-clear: frames left on the display; clear with: sh $0 replay-clear.txt --yes"
    rm -f "$STOP"; cleaned=1
else
    finish
fi
echo "done"
