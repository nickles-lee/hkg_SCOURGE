#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Daemon smoke test (no hardware, no root required).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
BIN="$ROOT/src/tweaks_daemon"
[ -x "$BIN" ] || make -C "$ROOT/src"
TMP=$(mktemp -d /tmp/tweaks-daemon-test.XXXXXX)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/rw"

cat > "$TMP/fake-wrandr" <<EOF
#!/bin/sh
echo "\$@" >> "$TMP/wrandr.log"
exit 0
EOF
chmod +x "$TMP/fake-wrandr"

cat > "$TMP/fake-dbus" <<EOF
#!/bin/sh
echo "\$*" >> "$TMP/dbus.log"
echo "method return"
exit 0
EOF
chmod +x "$TMP/fake-dbus"

cat > "$TMP/rw/config.conf" <<EOF
cluster_fix=1
infotainment_fix=1
cluster_crtc=2
main_crtc=1
warmth_override=6
force_filter_on=1
projection_always=1
wrandr_path=$TMP/fake-wrandr
persist_helper=$TMP/rw/missing-helper
dbus_send_path=$TMP/fake-dbus
EOF

out=$("$BIN" --rw-dir "$TMP/rw" --once --no-dbus)
echo "$out" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["ok"] is True; assert d["cluster_fix"] == 1; assert d["infotainment_fix"] == 1; assert d["filter_active"] == 1; assert d["warmth_level"] == 6; assert d["gamma"] == "128:115:102"; assert d["projection_active"] == 1; assert d["main_applied"] == 1'

# The front display is tinted through the brightness channel on CRTC 1.
grep -q -- '--crtc=1 --brightness=128:115:102' "$TMP/wrandr.log"
test "$(cat "$TMP/rw/cluster_gamma")" = "128:115:102"

# Infotainment fix off must disarm the wrapper state and stop applying.
"$BIN" --rw-dir "$TMP/rw" --set infotainment_fix=0 >/dev/null
out=$("$BIN" --rw-dir "$TMP/rw" --once --no-dbus)
echo "$out" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["infotainment_fix"] == 0; assert d["main_applied"] == 0'
test "$(cat "$TMP/rw/cluster_gamma")" = "off"

# Manual projection state overrides the automatic source.
cat > "$TMP/rw/config.conf" <<EOF
cluster_fix=1
infotainment_fix=1
cluster_crtc=2
main_crtc=1
warmth_override=3
force_filter_on=1
projection_always=0
wrandr_path=$TMP/fake-wrandr
persist_helper=$TMP/rw/missing-helper
dbus_send_path=$TMP/fake-dbus
EOF
echo 0 > "$TMP/rw/projection_state"
out=$("$BIN" --rw-dir "$TMP/rw" --status --no-dbus)
echo "$out" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["projection_active"] == 0; assert d["projection_source"] == "manual"'
echo 1 > "$TMP/rw/projection_state"
out=$("$BIN" --rw-dir "$TMP/rw" --status --no-dbus)
echo "$out" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["projection_active"] == 1'

# Night mode must stay on while projection is active even when the vendor
# flips the blue-light filter off (the CarPlay scenario): the cluster is
# re-asserted through the micom CAN blue-light signal and the front display
# keeps the last known warmth through wrandr --brightness.
cat > "$TMP/fake-persist-night" <<EOF
#!/bin/sh
case "\$1" in
read)
    echo "APP_SETTING_DISP_BLUE_LIGHT_STATUS=0"
    echo "APP_SETTING_DISP_BLUE_LIGHT_WARM_LEVEL=5"
    echo "APP_SETTING_DISP_GAMMA_R_G_B=128:128:128"
    echo "APP_SETTING_DISP_BRIGHTNESS_R_G_B=128:116:104"
    ;;
esac
EOF
chmod +x "$TMP/fake-persist-night"
cat > "$TMP/rw/config.conf" <<EOF
cluster_fix=1
infotainment_fix=1
cluster_crtc=2
main_crtc=1
poll_ms=1000
wrandr_path=$TMP/fake-wrandr
persist_helper=$TMP/fake-persist-night
tzdata_dir=/usr/share/zoneinfo
tzdata_version_file=$TMP/rw/tzdata_version
offline_status_path=$TMP/offline-night
dbus_send_path=$TMP/fake-dbus
EOF
echo 2026f > "$TMP/rw/tzdata_version"
: > "$TMP/wrandr.log"
: > "$TMP/dbus.log"
echo 1 > "$TMP/rw/projection_state"
out=$("$BIN" --rw-dir "$TMP/rw" --once --no-dbus)
echo "$out" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["filter_active"] == 0, d; assert d["night_forced"] == 1, d; assert d["blu_forced"] == 1, d; assert d["blu_status_sent"] == 2, d; assert d["blu_level_sent"] == 5, d; assert d["main_applied"] == 1, d'
# The cluster amber is re-asserted via micom CAN 0x063F (status bit 26, level bit 12).
grep -q -- '--system --print-reply --reply-timeout=8000 --dest=com.lge.car.micom /Can com.lge.car.micom.can.SetCANFixedMicomRecv array:byte:0x06,0x3f int32:26 int32:2 int32:2' "$TMP/dbus.log"
grep -q -- 'array:byte:0x06,0x3f int32:12 int32:4 int32:5' "$TMP/dbus.log"
# The front display keeps the vehicle brightness, not neutral.
grep -q -- '--crtc=1 --brightness=128:116:104' "$TMP/wrandr.log"
test "$(cat "$TMP/rw/cluster_gamma")" = "128:116:104"

# Projection off + filter off -> force ends, cluster restored to off,
# front display neutral, wrapper disarmed.
: > "$TMP/wrandr.log"
: > "$TMP/dbus.log"
echo 0 > "$TMP/rw/projection_state"
out=$("$BIN" --rw-dir "$TMP/rw" --once --no-dbus)
echo "$out" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["night_forced"] == 0, d; assert d["blu_forced"] == 0, d; assert d["blu_status_sent"] == 1, d; assert d["main_applied"] == 0, d'
grep -q -- 'array:byte:0x06,0x3f int32:26 int32:2 int32:1' "$TMP/dbus.log"
test "$(cat "$TMP/rw/cluster_gamma")" = "off"

# HTTP server: /status, /set, /restore.
"$BIN" --rw-dir "$TMP/rw" --port 18091 --no-dbus >/dev/null 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null || true; rm -rf "$TMP"' EXIT
i=0
while [ $i -lt 30 ]; do
    if python3 - "$TMP/rw/status.json" <<'PY' 2>/dev/null
import json,sys
json.load(open(sys.argv[1]))
PY
    then break; fi
    i=$((i+1)); sleep 0.2
done
python3 - <<'PY'
import json,urllib.request
d=json.load(urllib.request.urlopen("http://127.0.0.1:18091/status", timeout=3))
assert d["ok"] is True and d["cluster_fix"] == 1
req=urllib.request.Request("http://127.0.0.1:18091/set?cluster_fix=0", method="POST")
d=json.load(urllib.request.urlopen(req, timeout=3))
assert d["cluster_fix"] == 0
req=urllib.request.Request("http://127.0.0.1:18091/set?disclaimer_auto=0&disclaimer_timeout_ms=3000&auto_dst=0&dst_zone=Europe%2FDublin&ota_block=0", method="POST")
d=json.load(urllib.request.urlopen(req, timeout=3))
assert d["disclaimer_auto"] == 0 and d["disclaimer_timeout_ms"] == 3000
assert d["auto_dst"] == 0 and d["dst_zone"] == "Europe/Dublin" and d["ota_block"] == 0
req=urllib.request.Request("http://127.0.0.1:18091/restore", method="POST")
d=json.load(urllib.request.urlopen(req, timeout=3))
assert d["cluster_fix"] == 0 and d["infotainment_fix"] == 0
PY
kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true
trap 'rm -rf "$TMP"' EXIT

# ---- identify must not leave the display red after the window ends -----
# Daytime scenario: filter off, no projection -> night_on=0 -> the restore
# path (restore_main) must rewrite the CRTC to neutral once identify expires.
mkdir -p "$TMP/rw3"
cat > "$TMP/fake-persist-day" <<EOF
#!/bin/sh
case "\$1" in
read)
    echo "APP_SETTING_DISP_BLUE_LIGHT_STATUS=0"
    echo "APP_SETTING_DISP_BLUE_LIGHT_WARM_LEVEL=5"
    echo "APP_SETTING_DISP_GAMMA_R_G_B=128:128:128"
    echo "APP_SETTING_DISP_BRIGHTNESS_R_G_B=128:128:128"
    echo "APP_SETTING_CLOCK_DAYLIGHTSAVETIME=0"
    ;;
esac
EOF
chmod +x "$TMP/fake-persist-day"
cat > "$TMP/rw3/config.conf" <<EOF
cluster_fix=0
infotainment_fix=1
main_crtc=1
poll_ms=300
wrandr_path=$TMP/fake-wrandr
persist_helper=$TMP/fake-persist-day
tzdata_dir=/usr/share/zoneinfo
tzdata_version_file=$TMP/rw3/tzdata_version
offline_status_path=$TMP/offline-day
dbus_send_path=$TMP/fake-dbus
EOF
echo 2026f > "$TMP/rw3/tzdata_version"
: > "$TMP/wrandr.log"
"$BIN" --rw-dir "$TMP/rw3" --port 18092 --no-dbus >/dev/null 2>&1 &
pid3=$!
i=0
while [ $i -lt 30 ]; do
    if python3 - "$TMP/rw3/status.json" <<'PY' 2>/dev/null
import json,sys
json.load(open(sys.argv[1]))
PY
    then break; fi
    i=$((i+1)); sleep 0.2
done
python3 - <<'PY'
import json,time,urllib.request
base="http://127.0.0.1:18092"
req=urllib.request.Request(base+"/identify?crtc=1&seconds=1", method="POST")
d=json.load(urllib.request.urlopen(req, timeout=3))
assert d["ok"] is True and d["crtc"] == 1, d
# wait for the identify window to expire and at least one more tick
time.sleep(2.0)
d=json.load(urllib.request.urlopen(base+"/status", timeout=3))
assert d["main_applied"] == 0, d
PY
kill "$pid3" 2>/dev/null || true
wait "$pid3" 2>/dev/null || true
# the wrandr log must show red first and end with the neutral restore
grep -q -- '--crtc=1 --brightness=255:0:0' "$TMP/wrandr.log"
last=$(grep -- '--crtc=1' "$TMP/wrandr.log" | tail -1)
test "$last" = "--crtc=1 --brightness=128:128:128"

# ---- new controls: disclaimer, auto-DST, OTA block ---------------------
mkdir -p "$TMP/rw2" "$TMP/update2"
cat > "$TMP/rw2/persist" <<EOF
#!/bin/sh
case "\$1" in
read)
    echo "APP_SETTING_DISP_BLUE_LIGHT_STATUS=1"
    echo "APP_SETTING_DISP_BLUE_LIGHT_WARM_LEVEL=5"
    echo "APP_SETTING_DISP_GAMMA_R_G_B=128:116:104"
    if [ ! -f "$TMP/dst_missing" ] || [ -f "$TMP/dst" ]; then
        echo "APP_SETTING_CLOCK_DAYLIGHTSAVETIME=\$(cat "$TMP/dst" 2>/dev/null || echo 0)"
    fi
    echo "APP_SETTING_CLOCK_TIMEZONE=60"
    ;;
write)
    echo "\$2=\$3" >> "$TMP/persist-writes"
    [ "\$2" = "APP_SETTING_CLOCK_DAYLIGHTSAVETIME" ] && echo "\$3" > "$TMP/dst"
    echo "\$2=\$3"
    echo "verify=\$3"
    ;;
esac
EOF
chmod +x "$TMP/rw2/persist"
cat > "$TMP/fake-dbus" <<EOF
#!/bin/sh
echo "\$*" >> "$TMP/dbus.log"
echo "method return"
echo "   int32 0"
exit 0
EOF
chmod +x "$TMP/fake-dbus"
echo 'unix:path=/tmp/tweaks-test-bus' > "$TMP/session_bus"
echo 0 > "$TMP/dst"

cat > "$TMP/rw2/config.conf" <<EOF
cluster_fix=0
infotainment_fix=0
poll_ms=1000
disclaimer_auto=1
disclaimer_timeout_ms=4000
auto_dst=1
dst_zone=Europe/Amsterdam
dst_check_ms=60000
ota_block=0
wrandr_path=$TMP/fake-wrandr
persist_helper=$TMP/rw2/persist
tzdata_dir=/usr/share/zoneinfo
zones_file=$TMP/rw2/zones.txt
tzdata_version_file=$TMP/rw2/tzdata_version
offline_status_path=$TMP/update2/offline_status
session_bus_file=$TMP/session_bus
dbus_send_path=$TMP/fake-dbus
EOF
echo "2026f" > "$TMP/rw2/tzdata_version"
echo "Europe/Amsterdam" > "$TMP/rw2/zones.txt"

winter=$(python3 -c 'import datetime; print(int(datetime.datetime(2026,1,15,tzinfo=datetime.timezone.utc).timestamp()))')
summer=$(python3 -c 'import datetime; print(int(datetime.datetime(2026,7,15,tzinfo=datetime.timezone.utc).timestamp()))')
out=$("$BIN" --rw-dir "$TMP/rw2" --once --no-dbus --dst-at "$winter")
echo "$out" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["dst_active"] == 0, d'
test "$(cat "$TMP/dst")" = "0"
out=$("$BIN" --rw-dir "$TMP/rw2" --once --no-dbus --dst-at "$summer")
echo "$out" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["dst_active"] == 1, d'
test "$(cat "$TMP/dst")" = "1"
grep -q 'APP_SETTING_CLOCK_DAYLIGHTSAVETIME=1' "$TMP/persist-writes"

# Custom mode (2) must be replaced with the expected flag, not rejected.
echo 2 > "$TMP/dst"
out=$("$BIN" --rw-dir "$TMP/rw2" --once --no-dbus --dst-at "$winter")
echo "$out" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["dst_state"] == "inactive", d; assert d["dst_raw"] == 0, d'
test "$(cat "$TMP/dst")" = "0"
echo 2 > "$TMP/dst"
out=$("$BIN" --rw-dir "$TMP/rw2" --once --no-dbus --dst-at "$summer")
echo "$out" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["dst_state"] == "active", d; assert d["dst_raw"] == 1, d'
test "$(cat "$TMP/dst")" = "1"

# A missing key must be created, not treated as a fatal error.
rm -f "$TMP/dst"
touch "$TMP/dst_missing"
out=$("$BIN" --rw-dir "$TMP/rw2" --once --no-dbus --dst-at "$winter")
echo "$out" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["dst_state"] == "inactive", d'
test "$(cat "$TMP/dst")" = "0"
rm -f "$TMP/dst_missing"

# A helper that rejects the write must surface the PCL return code.
cat > "$TMP/rw2/persist-reject" <<EOF
#!/bin/sh
case "\$1" in
read)
    echo "APP_SETTING_DISP_BLUE_LIGHT_STATUS=1"
    echo "APP_SETTING_DISP_BLUE_LIGHT_WARM_LEVEL=5"
    echo "APP_SETTING_DISP_GAMMA_R_G_B=128:116:104"
    echo "APP_SETTING_CLOCK_DAYLIGHTSAVETIME=1"
    echo "APP_SETTING_CLOCK_TIMEZONE=60"
    ;;
write)
    echo "write_failed rc=-44 app=AppSetup"
    exit 4
    ;;
esac
EOF
chmod +x "$TMP/rw2/persist-reject"
sed -i "s|^persist_helper=.*|persist_helper=$TMP/rw2/persist-reject|" "$TMP/rw2/config.conf"
out=$("$BIN" --rw-dir "$TMP/rw2" --once --no-dbus --dst-at "$winter")
echo "$out" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["dst_state"] == "error", d; assert "rc=-44" in d["dst_error"], d'
grep -q "write FAILED" "$TMP/rw2/daemon.log"

# Failures are logged locally, flushed to a mounted stick, and closed out
# with a success line once the write works again.
grep -q "write FAILED" "$TMP/rw2/failures.log"
grep -q "rc=-44" "$TMP/rw2/failures.log"
test -f "$TMP/rw2/dst_write_failed"
mkdir -p "$TMP/usb"
TWEAKS_USB_DIR="$TMP/usb" "$BIN" --rw-dir "$TMP/rw2" --once --no-dbus --dst-at "$winter" >/dev/null
test -f "$TMP/usb/tweaks_failures.log"
grep -q "write FAILED" "$TMP/usb/tweaks_failures.log"
sed -i "s|^persist_helper=.*|persist_helper=$TMP/rw2/persist|" "$TMP/rw2/config.conf"
echo 1 > "$TMP/dst"   # force one more real write through the good helper
TWEAKS_USB_DIR="$TMP/usb" "$BIN" --rw-dir "$TMP/rw2" --once --no-dbus --dst-at "$winter" >/dev/null
grep -q "write ok after failures" "$TMP/rw2/failures.log"
grep -q "write ok after failures" "$TMP/usb/tweaks_failures.log"
test ! -f "$TMP/rw2/dst_write_failed"

# A same-session verify that still shows the old value must be reported as
# "did not persist" and land in the failure log, even when the write helper
# itself exited 0.
cat > "$TMP/rw2/persist-stale" <<EOF
#!/bin/sh
case "\$1" in
read)
    echo "APP_SETTING_DISP_BLUE_LIGHT_STATUS=1"
    echo "APP_SETTING_DISP_BLUE_LIGHT_WARM_LEVEL=5"
    echo "APP_SETTING_DISP_GAMMA_R_G_B=128:116:104"
    echo "APP_SETTING_CLOCK_DAYLIGHTSAVETIME=1"
    echo "APP_SETTING_CLOCK_TIMEZONE=60"
    ;;
write)
    echo "APP_SETTING_CLOCK_DAYLIGHTSAVETIME=\$3"
    echo "verify=1"
    ;;
esac
EOF
chmod +x "$TMP/rw2/persist-stale"
sed -i "s|^persist_helper=.*|persist_helper=$TMP/rw2/persist-stale|" "$TMP/rw2/config.conf"
out=$("$BIN" --rw-dir "$TMP/rw2" --once --no-dbus --dst-at "$winter")
echo "$out" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["dst_state"] == "error", d; assert "did not persist" in d["dst_error"], d; assert "wrote 0" in d["dst_error"], d'
grep -q "did not persist" "$TMP/rw2/failures.log"
grep -q "did not persist" "$TMP/usb/tweaks_failures.log" 2>/dev/null || true
test -f "$TMP/rw2/dst_write_failed"
sed -i "s|^persist_helper=.*|persist_helper=$TMP/rw2/persist|" "$TMP/rw2/config.conf"

"$BIN" --rw-dir "$TMP/rw2" --set disclaimer_timeout_ms=900 >/dev/null
test "$(cat "$TMP/rw2/disclaimer_timeout_ms")" = "1000"
"$BIN" --rw-dir "$TMP/rw2" --set disclaimer_auto=0 >/dev/null
test "$(cat "$TMP/rw2/disclaimer_auto")" = "0"

"$BIN" --rw-dir "$TMP/rw2" --set ota_block=1 >/dev/null
test "$(cat "$TMP/update2/offline_status")" = "1"
test -f "$TMP/rw2/ota_block_owned"
grep -q 'setOFFlineMode int32:1' "$TMP/dbus.log"
out=$("$BIN" --rw-dir "$TMP/rw2" --status --no-dbus)
echo "$out" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["ota_block"] == 1'
"$BIN" --rw-dir "$TMP/rw2" --set ota_block=0 >/dev/null
test "$(cat "$TMP/update2/offline_status")" = "0"
test ! -f "$TMP/rw2/ota_block_owned"
grep -q 'setOFFlineMode int32:0' "$TMP/dbus.log"

echo "PASS: daemon smoke test"
