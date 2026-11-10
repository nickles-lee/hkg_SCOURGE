#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Reboot endpoint and micom WDT fallback test (no real reboot, no root).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
BIN="$ROOT/src/tweaks_daemon"
[ -x "$BIN" ] || make -C "$ROOT/src"
TMP=$(mktemp -d /tmp/tweaks-reboot-test.XXXXXX)

cleanup() {
    for pid in ${REBOOT_PIDS:-}; do kill "$pid" 2>/dev/null || true; done
    [ -n "${BUS_PID:-}" ] && kill "$BUS_PID" 2>/dev/null || true
    [ -n "${APPUP_PID:-}" ] && kill "$APPUP_PID" 2>/dev/null || true
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

mkdir -p "$TMP/rw" "$TMP/rw-bus" "$TMP/bus"
printf '_\n' > "$TMP/rw/smack_expected"
printf '_\n' > "$TMP/rw-bus/smack_expected"

cat > "$TMP/fake-systemctl-ok" <<EOF
#!/bin/sh
echo "\$*" >> "$TMP/systemctl.log"
exit 0
EOF
cat > "$TMP/fake-systemctl-fail" <<EOF
#!/bin/sh
echo "\$*" >> "$TMP/systemctl-fail.log"
exit 1
EOF
cat > "$TMP/fake-dbus" <<EOF
#!/bin/sh
echo "\$*" >> "$TMP/dbus.log"
exit 0
EOF
chmod +x "$TMP/fake-systemctl-ok" "$TMP/fake-systemctl-fail" "$TMP/fake-dbus"

cat > "$TMP/rw/config.conf" <<EOF
dbus_send_path=$TMP/fake-dbus
poll_ms=200
EOF

"$BIN" --rw-dir "$TMP/rw" --port 28095 --no-dbus \
    --reboot-command "$TMP/fake-systemctl-ok" --reboot-wdt-ms 300 \
    >/dev/null 2>&1 &
REBOOT_PIDS="$!"
sleep 0.5

python3 - <<'PY'
import json, urllib.error, urllib.request, time
base = "http://127.0.0.1:28095"

def post(headers=None):
    req = urllib.request.Request(base + "/reboot", method="POST",
                                 headers=headers or {})
    return urllib.request.urlopen(req, timeout=3)

try:
    post()
    raise SystemExit("missing X-Reboot-App was accepted")
except urllib.error.HTTPError as e:
    assert e.code == 403, e.code

try:
    post({"X-Reboot-App": "1", "Origin": "http://evil.example"})
    raise SystemExit("cross-origin request was accepted")
except urllib.error.HTTPError as e:
    assert e.code == 403, e.code

d = json.load(post({"X-Reboot-App": "1", "Origin": "file://"}))
assert d["ok"] is True and d["state"] == "rebooting", d
time.sleep(0.8)
st = json.load(urllib.request.urlopen(base + "/status", timeout=3))
assert st["reboot_state"] == "rebooting", st
assert st["reboot_wdt_sent"] == 1, st
assert st["reboot_wdt_ok"] == 1, st
assert "hard reset" in st["reboot_message"], st
assert st["smack_expected"] == "_", st
assert "smack_ok" in st and "smack_dbus" in st, st
try:
    post({"X-Reboot-App": "1"})
    raise SystemExit("second reboot was accepted")
except urllib.error.HTTPError as e:
    assert e.code == 409, e.code
    body = json.loads(e.read().decode())
    assert body["reason"] == "already_rebooting", body
print("CSRF, clean reboot and WDT fallback ok")
PY

grep -q 'reboot --no-block' "$TMP/systemctl.log"
grep -q -- 'SetETCCPUWDTReset array:byte:0x00' "$TMP/dbus.log"
grep -q 'reboot_state' "$TMP/rw/status.json"

kill "$REBOOT_PIDS" 2>/dev/null || true
wait "$REBOOT_PIDS" 2>/dev/null || true
REBOOT_PIDS=""

# ---- update guard ------------------------------------------------------
cat > "$TMP/rw-bus/config.conf" <<EOF
dbus_send_path=$TMP/fake-dbus
poll_ms=200
EOF
"$BIN" --rw-dir "$TMP/rw-bus" --port 28096 --no-dbus \
    --reboot-command "$TMP/fake-systemctl-ok" --reboot-wdt-ms 300 \
    >/dev/null 2>&1 &
REBOOT_PIDS="$!"
sleep 0.5
cp "$(command -v sleep)" "$TMP/AppUpgrade"
"$TMP/AppUpgrade" 15 &
APPUP_PID=$!
sleep 0.2
python3 - <<'PY'
import json, urllib.error, urllib.request
req = urllib.request.Request("http://127.0.0.1:28096/reboot", method="POST",
                             headers={"X-Reboot-App": "1"})
try:
    urllib.request.urlopen(req, timeout=3)
    raise SystemExit("reboot during update was accepted")
except urllib.error.HTTPError as e:
    assert e.code == 409, e.code
    d = json.loads(e.read().decode())
    assert d["reason"] == "update_in_progress", d
print("update guard ok")
PY
kill "$APPUP_PID" 2>/dev/null || true
wait "$APPUP_PID" 2>/dev/null || true
APPUP_PID=""
kill "$REBOOT_PIDS" 2>/dev/null || true
wait "$REBOOT_PIDS" 2>/dev/null || true
REBOOT_PIDS=""

# ---- failed clean command triggers the fallback immediately ------------
cat > "$TMP/rw-bus/config.conf" <<EOF
dbus_send_path=$TMP/fake-dbus
poll_ms=200
EOF
"$BIN" --rw-dir "$TMP/rw-bus" --port 28097 --no-dbus \
    --reboot-command "$TMP/fake-systemctl-fail" --reboot-wdt-ms 5000 \
    >/dev/null 2>&1 &
REBOOT_PIDS="$!"
sleep 0.5
python3 - <<'PY'
import json, urllib.request, time
req = urllib.request.Request("http://127.0.0.1:28097/reboot", method="POST",
                             headers={"X-Reboot-App": "1"})
assert json.load(urllib.request.urlopen(req, timeout=3))["ok"] is True
time.sleep(0.8)
d = json.load(urllib.request.urlopen("http://127.0.0.1:28097/status", timeout=3))
assert d["reboot_wdt_sent"] == 1, d
assert "failed" in d["reboot_error"], d
print("failed clean command fallback ok")
PY
grep -q -- 'SetETCCPUWDTReset array:byte:0x00' "$TMP/dbus.log"
kill "$REBOOT_PIDS" 2>/dev/null || true
wait "$REBOOT_PIDS" 2>/dev/null || true
REBOOT_PIDS=""

# ---- Home/Star session-bus counter ------------------------------------
BUS_SOCK="$TMP/bus/bus"
dbus-daemon --session --address="unix:path=$BUS_SOCK" --print-address \
    > "$TMP/bus/address" 2>/dev/null &
BUS_PID=$!
i=0
while [ ! -s "$TMP/bus/address" ] && [ $i -lt 50 ]; do
    i=$((i + 1)); sleep 0.1
done
BUS_ADDR=$(head -n 1 "$TMP/bus/address")
[ -n "$BUS_ADDR" ] || { echo "private session bus failed to start" >&2; exit 1; }
printf '%s\n' "$BUS_ADDR" > "$TMP/rw-bus/session_appmgr"
cat > "$TMP/rw-bus/config.conf" <<EOF
dbus_send_path=$TMP/fake-dbus
session_bus_file=$TMP/rw-bus/session_appmgr
poll_ms=200
EOF
: > "$TMP/systemctl.log"
"$BIN" --rw-dir "$TMP/rw-bus" --port 28098 \
    --reboot-command "$TMP/fake-systemctl-ok" --reboot-wdt-ms 300 \
    >/dev/null 2>&1 &
REBOOT_PIDS="$!"
sleep 0.8
DBUS_SESSION_BUS_ADDRESS="$BUS_ADDR" dbus-send --session --type=signal / \
    com.lge.avn.appmanager.info.custom_key_value int32:7 >/dev/null
DBUS_SESSION_BUS_ADDRESS="$BUS_ADDR" dbus-send --session --type=signal / \
    com.lge.hkmc.avn.appfunc.appInterface.NotifyRequestHomeScreen >/dev/null
python3 - <<'PY'
import json, urllib.request, time
base = "http://127.0.0.1:28098"
end = time.time() + 5
st = None
while time.time() < end:
    st = json.load(urllib.request.urlopen(base + "/status", timeout=3))
    if st["home_seq"] >= 2:
        break
    time.sleep(0.1)
assert st["home_seq"] >= 2, st
assert st["home_source"] in ("star", "home"), st
h = json.load(urllib.request.urlopen(base + "/health", timeout=3))
assert "home_seq" in h and "smack_expected" in h, h
print("home/star counter ok:", st["home_seq"], st["home_source"])
PY

echo "PASS: reboot daemon tests"
