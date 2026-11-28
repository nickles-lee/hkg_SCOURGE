#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Stage 2 (host-level): hudnavd -> helper named CANService -> private system bus -> lab micom sim.
# Needs libdbus-1-dev + dbus-daemon. Uses a private bus; touches nothing system-wide.
set -eu
cd "$(dirname "$0")/.."
SIM_SRC="$PWD/../../tools/vm/guest/tweaks_micom_can_sim.c"
W=/tmp/vmt; mkdir -p $W/bin
gcc -o $W/micom_sim "$SIM_SRC" $(pkg-config --cflags --libs dbus-1)
cp "$(command -v dbus-send)" $W/bin/CANService
cat > $W/micom-send <<'W2'
#!/bin/sh
exec "$(dirname "$0")/bin/CANService" --system --type=method_call --print-reply --reply-timeout=500 \
  --dest=com.lge.car.micom /Can com.lge.car.micom.can.SetCANMicomRecv "$1" >/dev/null
W2
chmod +x $W/micom-send
sh scripts/build.sh >/dev/null
eval "$(dbus-launch --sh-syntax)"
export DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS"
$W/micom_sim >$W/sim.log 2>&1 & SIM=$!
sleep 0.7
S=$W/h.sock
owners=$(mktemp -d)/owners
mkfifo "$owners"
exec 3<>"$owners"
printf '%s\n' 'signal path=/Information; interface=com.lge.avn.appmanager.info; member=navi_route_guide_application' '   int32 111' >&3
./out/hudnavd --socket "$S" --can --appmanager-stdin --micom-send "$W/micom-send" <&3 3>&- 2>/dev/null & D=$!
sleep 0.4
python3 - <<PY
import socket,time
s=socket.socket(socket.AF_UNIX); s.connect("$S")
w=lambda l:s.sendall((l+"\n").encode())
w("STATE s=1"); w("TURN src=aa man=3 raw=4 side=2")
for m in [150,100,60,20]:
    w("HB"); w("DIST m=%d disp=%d unit=0"%(m,m)); time.sleep(0.4)
w("STATE s=0"); w("HB"); time.sleep(1.2)
PY
kill $D $SIM 2>/dev/null; kill $DBUS_SESSION_BUS_PID 2>/dev/null || true
echo "sim CALL lines: $(grep -c TWEAKS_CAN_SIM_CALL $W/sim.log)"
grep -c 'member=SetCANMicomRecv' $W/sim.log | sed 's/^/SetCANMicomRecv: /'
grep 'hex=04cc08' $W/sim.log | head -3
grep -i -c "not listed\|UnknownMethod" $W/sim.log | sed 's/^/rejections: /'
