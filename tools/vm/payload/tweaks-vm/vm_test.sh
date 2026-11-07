#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Tweaks daemon VM validation. Runs inside the vendor-rootfs guest.
# Verifies the micom CAN blue-light calls against a real D-Bus daemon with
# signature-checked method dispatch, plus the session-bus projection monitor.
#
# Note: the daemon's status JSON uses numbers (1/0), not true/false.
set -u
export PATH=/bin:/sbin:/usr/bin:/usr/sbin

PASS=0
FAIL=0
ok()   { echo "TWEAKS_VM_CHECK $1 PASS"; PASS=$((PASS+1)); }
bad()  { echo "TWEAKS_VM_CHECK $1 FAIL"; FAIL=$((FAIL+1)); }
check(){ if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (expected [$3] got [$2])"; fi; }
has()  { case "$2" in *"$3"*) ok "$1" ;; *) bad "$1 (missing [$3])" ;; esac; }
simcalls() { grep -c "TWEAKS_CAN_SIM_CALL.*SetCANFixedMicomRecv" "$T/can-sim.log" 2>/dev/null; }

# Run the daemon once with file output and a watchdog.  A command
# substitution can hang forever if any child inherits the pipe, so the
# daemon writes to a file and this waits for process exit instead.
run_once() {  # extra args passed through; output in $T/once.out
    "$T/tweaks_daemon" --rw-dir "$T/rw" "$@" > "$T/once.out" 2>/dev/null &
    local dp=$! i=0
    while [ "$i" -lt 60 ] && kill -0 "$dp" 2>/dev/null; do
        sleep 0.25; i=$((i+1))
    done
    if kill -0 "$dp" 2>/dev/null; then
        echo "TWEAKS_VM_DEBUG daemon --once HUNG (pid $dp); processes:"
        /bin/busybox ps 2>/dev/null | grep -vE "\[|ps " | head -14
        for p in /proc/[0-9]*; do
            cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null | head -c 50)
            case "$cmd" in
                *tweaks*|*dbus*|*persist*|*wrandr*)
                    echo "TWEAKS_VM_DEBUG pid=${p#/proc/} [${cmd}] fds=$(ls "$p/fd" 2>/dev/null | tr '\n' ' ')"
                    ;;
            esac
        done
        kill -9 "$dp" 2>/dev/null; wait "$dp" 2>/dev/null
        return 1
    fi
    wait "$dp" 2>/dev/null
    return 0
}

# ---- payload stick (may already be mounted by the outer command) ----
modprobe ehci_pci 2>/dev/null
modprobe usb_storage 2>/dev/null
modprobe sd_mod 2>/dev/null
sleep 4
mkdir -p /media/payload
PAY=
if [ -f /media/payload/tweaks_daemon ]; then
    PAY=/media/payload
else
    for dev in /dev/sdb /dev/sdb1 /dev/sdc /dev/sdc1 /dev/sdd /dev/sdd1; do
        [ -b "$dev" ] || continue
        mount -t ext4 -o ro "$dev" /media/payload 2>/dev/null || continue
        if [ -f /media/payload/tweaks_daemon ]; then PAY=/media/payload; break; fi
        umount /media/payload
    done
fi
[ -n "$PAY" ] || { echo "TWEAKS_VM_FAIL payload stick not found"; ls /dev/sd* 2>/dev/null; exit 1; }
ok payload-mount

T=/tmp/tweaks-vm
rm -rf "$T"; mkdir -p "$T/rw"
cp "$PAY/tweaks_daemon" "$T/tweaks_daemon"
cp "$PAY/tweaks-micom-can-sim" "$T/can-sim"
chmod +x "$T/tweaks_daemon" "$T/can-sim"

# ---- system bus + micom CAN sim ------------------------------------
# The vendor rootfs symlinks /etc/passwd -> /tmp/passwd; create it first
# (same as the dbus-ready scenario) so dbus-daemon can resolve "root".
cp /etc/passwd.legacy /tmp/passwd
ln -sf /etc/shadow.Global /tmp/shadow
# The VM boots without loopback configured; the daemon's status server
# binds 127.0.0.1 and (correctly) fail-fasts when it cannot. Raise lo with
# the vendor busybox (no ip/ifconfig binaries in the vendor rootfs).
/bin/busybox ifconfig lo 127.0.0.1 up 2>/dev/null
/bin/busybox ifconfig lo 2>/dev/null | head -1
# Vendor dbus listens on /var/run/dbus (-> rw_data/var/run/dbus); the
# statically linked sim's libdbus looks in /run/dbus. Bridge with a symlink.
mkdir -p /rw_data/var/run/dbus
ln -sfn /rw_data/var/run/dbus /run/dbus 2>/dev/null
rm -f /rw_data/var/run/dbus/system_bus_socket /rw_data/var/run/dbus/pid
dbus-daemon --system --fork 2>/dev/null
sleep 0.5
"$T/can-sim" > "$T/can-sim.log" 2>&1 &
SIM_PID=$!
sleep 1
has can-sim-ready "$(cat "$T/can-sim.log")" "TWEAKS_CAN_SIM_READY"

# ---- probe: the exact micom call the daemon makes, by hand ----------
# Isolates tool/policy/destination problems from daemon-context problems.
/usr/bin/dbus-send --system --print-reply --reply-timeout=8000 \
    --dest=com.lge.car.micom /Can com.lge.car.micom.can.SetCANFixedMicomRecv \
    array:byte:0x06,0x3f int32:26 int32:2 int32:2 > "$T/probe.out" 2>&1
PRC=$?
echo "TWEAKS_VM_DEBUG dbus-send probe rc=$PRC out=$(head -c 300 "$T/probe.out" | tr '\n' ' ')"
if [ "$PRC" = 0 ] && grep -q "method return" "$T/probe.out"; then
    ok dbus-probe
else
    bad dbus-probe "(rc=$PRC)"
fi
PROBE_CALLS=$(simcalls); : "${PROBE_CALLS:=0}"

# ---- fake vendor helpers -------------------------------------------
cat > "$T/persist" <<'PEOF'
#!/bin/sh
case "$1" in
read)
    echo "APP_SETTING_DISP_BLUE_LIGHT_STATUS=$(cat /tmp/tweaks-vm/filter 2>/dev/null || echo 1)"
    echo "APP_SETTING_DISP_BLUE_LIGHT_WARM_LEVEL=$(cat /tmp/tweaks-vm/level 2>/dev/null || echo 4)"
    echo "APP_SETTING_DISP_GAMMA_R_G_B=128:118:108"
    echo "APP_SETTING_DISP_BRIGHTNESS_R_G_B=128:118:108"
    echo "APP_SETTING_CLOCK_DAYLIGHTSAVETIME=0"
    echo "APP_SETTING_CLOCK_TIMEZONE=60"
    ;;
write)
    echo "$2=$3"
    echo "verify=$3"
    ;;
esac
PEOF
chmod +x "$T/persist"

cat > "$T/wrandr" <<'WEOF'
#!/bin/sh
echo "$@" >> /tmp/tweaks-vm/wrandr.log
exit 0
WEOF
chmod +x "$T/wrandr"
: > "$T/wrandr.log"

echo 1 > "$T/filter"
echo 4 > "$T/level"

cat > "$T/rw/config.conf" <<EOF
cluster_fix=1
infotainment_fix=1
main_crtc=1
poll_ms=500
wrandr_path=$T/wrandr
persist_helper=$T/persist
tzdata_dir=/usr/share/zoneinfo
dst_zone=Europe/Paris
tzdata_version_file=$T/tzdata_version
offline_status_path=$T/offline
dbus_send_path=/usr/bin/dbus-send
session_bus_file=/tmp/session_appmgr
EOF
echo 2026f > "$T/tzdata_version"

# ---- TEST 1: filter ON, no projection ------------------------------
echo "== TEST 1: filter on, projection idle"
rm -f "$T/rw/projection_state" "$T/rw/blu_forced"
run_once --once --no-dbus
out=$(cat "$T/once.out" 2>/dev/null)
echo "TWEAKS_VM_DEBUG t1 daemon: $(echo "$out" | head -c 400)"
echo "TWEAKS_VM_DEBUG t1 wrandr: $(cat "$T/wrandr.log" 2>/dev/null | tr '\n' ';')"
has t1-filter-on "$out" '"filter_active":1'
has t1-wrandr-applied "$out" '"main_applied":1'
NOW=$(simcalls); : "${NOW:=0}"
check t1-no-can-call "$NOW" "$PROBE_CALLS"

# ---- TEST 2: CarPlay connects, vendor flips filter off --------------
echo "== TEST 2: projection active + filter off (CarPlay scenario)"
echo 0 > "$T/filter"
echo 1 > "$T/rw/projection_state"
run_once --once --no-dbus
out=$(cat "$T/once.out" 2>/dev/null)
echo "TWEAKS_VM_DEBUG t2 daemon: $(echo "$out" | head -c 400)"
echo "TWEAKS_VM_DEBUG t2 blu_call.out: $(head -c 300 "$T/rw/blu_call.out" 2>/dev/null | tr '\n' ' ')"
has t2-night-forced "$out" '"night_forced":1'
has t2-blu-forced "$out" '"blu_forced":1'
has t2-blu-status "$out" '"blu_status_sent":2'
has t2-blu-level "$out" '"blu_level_sent":4'
sleep 0.3
SIMLOG=$(cat "$T/can-sim.log")
# The daemon must have added exactly the status + level calls (the probe
# already satisfies the substring checks below, so the count is authoritative).
AFTER2=$(simcalls); : "${AFTER2:=0}"
check t2-can-call-delta "$AFTER2" "$((PROBE_CALLS + 2))"
has t2-can-status-call "$SIMLOG" "member=SetCANFixedMicomRecv signature=ayiii"
has t2-can-id "$SIMLOG" "hex=063f"
has t2-can-status-args "$SIMLOG" "value=26"
has t2-can-data-on "$SIMLOG" "value=2"
[ -f "$T/rw/blu_forced" ] && ok t2-marker-file || bad t2-marker-file "(missing)"

# ---- TEST 3: CarPlay disconnects, force restores --------------------
echo "== TEST 3: projection ends, restore"
echo 0 > "$T/rw/projection_state"
run_once --once --no-dbus
out=$(cat "$T/once.out" 2>/dev/null)
echo "TWEAKS_VM_DEBUG t3 daemon: $(echo "$out" | head -c 400)"
echo "TWEAKS_VM_DEBUG t3 blu_call.out: $(head -c 300 "$T/rw/blu_call.out" 2>/dev/null | tr '\n' ' ')"
has t3-blu-restored "$out" '"blu_status_sent":1'
has t3-not-forced "$out" '"blu_forced":0'
sleep 0.3
# The off write is CAN data=1 at arg index 3 (exact line so value=12 never matches).
off_seen=$(grep -c "index=3 type=i value=1$" "$T/can-sim.log" 2>/dev/null); : "${off_seen:=0}"
[ "$off_seen" -ge 1 ] && ok t3-can-off-sent || bad t3-can-off-sent "(no data=1 CAN write)"
[ ! -f "$T/rw/blu_forced" ] && ok t3-marker-cleared || bad t3-marker-cleared "(still present)"

# ---- TEST 4: session-bus projection monitor -------------------------
echo "== TEST 4: session-bus monitor with real dbus signals"
dbus-daemon --session --fork --print-address > /tmp/harness-session.addr 2>/dev/null
addr=$(cat /tmp/harness-session.addr)
echo "$addr" > /tmp/session_appmgr
echo "TWEAKS_VM_DEBUG session bus: $addr"
"$T/tweaks_daemon" --rw-dir "$T/rw" --port 18091 >/dev/null 2>&1 &
DPID=$!
sleep 2
if ! kill -0 "$DPID" 2>/dev/null; then
    echo "TWEAKS_VM_DEBUG t4 daemon DIED; daemon.log: $(tail -c 400 "$T/rw/daemon.log" 2>/dev/null | tr '\n' ';')"
    bad t4-daemon-alive "(exited)"
else
    ok t4-daemon-alive
fi
st=$(cat "$T/rw/status.json" 2>/dev/null)
echo "TWEAKS_VM_DEBUG t4 status: $(echo "$st" | head -c 400)"
has t4-mon-session "$st" '"mon_session":1'
# Emit the CarPlay play signal on the session bus (capture the result).
DBUS_SESSION_BUS_ADDRESS="$addr" dbus-send --session --type=signal \
    / com.lge.avn.appmanager.info.carplay_play_enable_status boolean:true \
    > "$T/sig1.out" 2>&1
echo "TWEAKS_VM_DEBUG t4 carplay signal rc=$? out=$(head -c 200 "$T/sig1.out" | tr '\n' ' ')"
DBUS_SESSION_BUS_ADDRESS="$addr" dbus-send --session --type=signal \
    / com.lge.avn.appmanager.info.front_drawing_application int32:7 \
    > "$T/sig2.out" 2>&1
echo "TWEAKS_VM_DEBUG t4 front signal rc=$? out=$(head -c 200 "$T/sig2.out" | tr '\n' ' ')"
# The daemon's counters must have seen both signals by now.
sleep 2
st=$(cat "$T/rw/status.json" 2>/dev/null)
echo "TWEAKS_VM_DEBUG t4 status after signals: $(echo "$st" | head -c 400)"
has t4-sig-carplay "$st" '"sig_carplay":1'
has t4-front-seen "$st" '"sig_front":1'
# Manual check: does the vendor dbus-monitor see the signal with the same
# guid-stripped address the daemon passes? (diagnostics only)
addr_noguid=$(echo "$addr" | sed 's/,guid=.*//')
dbus-monitor --address "$addr_noguid" "type='signal',interface='com.lge.avn.appmanager.info'" \
    > "$T/mon-manual.out" 2>&1 &
MONPID=$!
sleep 1
DBUS_SESSION_BUS_ADDRESS="$addr" dbus-send --session --type=signal \
    / com.lge.avn.appmanager.info.carplay_play_enable_status boolean:true >/dev/null 2>&1
sleep 1
kill "$MONPID" 2>/dev/null
echo "TWEAKS_VM_DEBUG t4 manual monitor: $(head -c 400 "$T/mon-manual.out" | tr '\n' ' ')"
echo "TWEAKS_VM_DEBUG t4 daemon.log: $(tail -c 600 "$T/rw/daemon.log" 2>/dev/null | tr '\n' ';')"
kill "$DPID" 2>/dev/null
wait "$DPID" 2>/dev/null

# ---- cleanup --------------------------------------------------------
kill "$SIM_PID" 2>/dev/null
wait "$SIM_PID" 2>/dev/null
umount "$PAY" 2>/dev/null

echo "TWEAKS_VM_SUMMARY pass=$PASS fail=$FAIL"
[ "$FAIL" -eq 0 ]
