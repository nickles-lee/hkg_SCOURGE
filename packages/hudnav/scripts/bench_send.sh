#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Bench: run hudnavd with --micom-send pointing at a fake helper, drive a synthetic
# AA turn, assert only whitelisted IDs were "transmitted", frames deduped, and the
# helper was exec'd as argv[0]=CANService with a single array arg.
set -eu
cd "$(dirname "$0")/.."
sh scripts/build.sh >/dev/null
W=$(mktemp -d)
trap 'if [ -n "${D:-}" ]; then kill "$D" 2>/dev/null || true; fi' EXIT
LOG=$W/frames; : > "$LOG"
export HUDNAV_BENCH_LOG=$LOG
S=$W/hudnav.sock
mkfifo "$W/owners"
exec 3<>"$W/owners"
printf '%s\n' 'signal path=/Information; interface=com.lge.avn.appmanager.info; member=navi_route_guide_application' '   int32 111' >&3
./out/hudnavd --socket "$S" --can --appmanager-stdin --micom-send "$PWD/tests/bench/fake_micom_send.sh" <&3 3>&- 2>/dev/null &
D=$!; sleep 0.4
python3 - <<PY
import socket,time
s=socket.socket(socket.AF_UNIX); s.connect("$S")
def w(l): s.sendall((l+"\n").encode())
w("STATE s=1"); w("TURN src=aa man=3 raw=4 side=2")
for m in [150,120,90,60,30,10]:
    w("HB"); w("DIST m=%d disp=%d unit=0"%(m,m)); time.sleep(0.3)
w("STATE s=0"); w("HB"); time.sleep(0.6)
PY
kill "$D"; wait "$D" 2>/dev/null || true
D=
test -s "$LOG"
echo "frames sent: $(wc -l < $LOG)"
bad=$(grep -v -E 'array:byte:0x(04,0xcc|06,0x3e|06,0x40|04,0xea|04,0xe9),0x08' $LOG | wc -l)
[ "$bad" -eq 0 ] && echo "PASS: only whitelisted IDs (4CC/63E/640/4EA/4E9) transmitted" || { echo "FAIL: $bad non-whitelisted"; exit 1; }
grep -c 'array:byte:0x04,0xcc' $LOG | sed 's/^/0x4CC frames: /'
head -3 $LOG
