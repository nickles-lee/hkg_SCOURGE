#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TWEAKS_PAYLOAD_DIAG=1
# Read-only diagnostic for the Tweaks night-mode fixes.
# Copy this file to run_stuff.sh on the USB root, then plug it in.
set -u
PATH="${TWEAKS_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="${USB_DIR:-/media/usb}"
QML_ROOT="${TWEAKS_QML_ROOT:-/app/share/AppHomeScreen}"
QML_DIR="$QML_ROOT/qml_eco3.0"
STANDBY_ROOT="${TWEAKS_STANDBY_ROOT:-/app/share/AppStandbyClock}"
OFFLINE_STATUS="${TWEAKS_OFFLINE_STATUS:-/update/offline_status}"
RW_DIR="${TWEAKS_RW_DIR:-/rw_data/tweaks}"
OUT="$USB_DIR/tweaks_diag.txt"
TMP="/tmp/tweaks_diag.$$"
mkdir -p "$RW_DIR" 2>/dev/null
mount -o remount,rw "$USB_DIR" 2>/dev/null || true

{
echo "TWEAKS_DIAG"
echo "time=$(date -u 2>/dev/null)"
echo "kernel=$(uname -a 2>/dev/null)"
echo "uid=$(id -u 2>/dev/null)"
echo
echo "== wrandr =="
command -v wrandr 2>/dev/null || true
ls -l /usr/bin/wrandr /usr/bin/wrandr.real 2>/dev/null || true
wrandr 2>&1 | head -80 || true
echo
echo "== DRM connectors =="
for f in /sys/class/drm/card*-*/status; do
    [ -f "$f" ] || continue
    echo "$f: $(cat "$f" 2>/dev/null)"
done
ls -l /dev/dri 2>/dev/null || true
echo
echo "== daemon config =="
cat "$RW_DIR/config.conf" 2>/dev/null || echo "(no config)"
echo
echo "== daemon status =="
cat "$RW_DIR/status.json" 2>/dev/null || echo "(no status)"
echo
echo "== desired cluster gamma =="
cat "$RW_DIR/cluster_gamma" 2>/dev/null || echo "(none)"
echo
echo "== persist helper =="
if [ -x "$RW_DIR/tweaks_persist" ]; then
    "$RW_DIR/tweaks_persist" read 2>&1 || true
else
    echo "(helper not installed)"
fi
echo
echo "== disclaimer runtime files =="
for f in "$RW_DIR/disclaimer_auto" "$RW_DIR/disclaimer_timeout_ms"; do
    if [ -f "$f" ]; then echo "$f=$(cat "$f" 2>/dev/null)"; else echo "$f: missing"; fi
done
echo
echo "== tzdata =="
cat "$RW_DIR/tzdata_version" 2>/dev/null || echo "(no tzdata version)"
[ -f "$RW_DIR/zones.txt" ] && echo "zones=$(wc -l < "$RW_DIR/zones.txt" 2>/dev/null)" || echo "zones.txt: missing"
ls -l "$RW_DIR/zoneinfo/Europe/Amsterdam" 2>/dev/null || true
zone=$(sed -n 's/^dst_zone=//p' "$RW_DIR/config.conf" 2>/dev/null | tail -n 1)
[ -n "$zone" ] && TZ=":$RW_DIR/zoneinfo/$zone" date 2>&1 || true
echo
echo "== OTA state =="
cat "$OFFLINE_STATUS" 2>/dev/null || echo "(no offline_status file)"
[ -f "$RW_DIR/ota_block_owned" ] && echo "owned_by_tweaks=1" || true
ps -ef 2>/dev/null | grep -E 'AppDMClient|dmengine|AppUpgrade' | grep -v grep || true
if command -v dbus-send >/dev/null 2>&1 && [ -f /tmp/session_appmgr ]; then
    bus=$(cat /tmp/session_appmgr 2>/dev/null)
    if [ -n "$bus" ]; then
        DBUS_SESSION_BUS_ADDRESS="$bus" dbus-send --session --print-reply \
            --reply-timeout=4000 --dest=com.lge.hkmc.avn.upgrade.fota \
            /fotaInterface \
            com.lge.hkmc.avn.upgrade.fota.fotaInterface.GetOTAEnableStatus 2>&1 | head -20 || \
        dbus-send --system --print-reply --reply-timeout=4000 \
            --dest=com.lge.hkmc.avn.upgrade.fota /fotaInterface \
            com.lge.hkmc.avn.upgrade.fota.fotaInterface.GetOTAEnableStatus 2>&1 | head -20 || true
    fi
fi
echo
echo "== service =="
systemctl status tweaks-daemon.service 2>&1 | head -40 || true
echo
echo "== daemon log (last 80 lines) =="
tail -80 "$RW_DIR/daemon.log" 2>/dev/null || echo "(no daemon.log)"
echo
echo "== failure log (also auto-copied to a mounted USB stick) =="
cat "$RW_DIR/failures.log" 2>/dev/null || echo "(no failures logged)"
[ -f "$RW_DIR/pcl_write_app" ] && echo "pcl_write_app=$(cat "$RW_DIR/pcl_write_app")"
echo
echo "== persistence helper (stderr shows the registered app name) =="
"$RW_DIR/tweaks_persist" read 2>&1 || true
echo
echo "== SMACK labels =="
echo "diagnostic process label: $(cat /proc/self/attr/current 2>/dev/null || echo unknown)"
if command -v getfattr >/dev/null 2>&1; then
    for f in /usr/bin/wrandr /usr/bin/wrandr.real /usr/bin/dbus-send /usr/bin/pers_admin_svc \
             /app/bin/AppSetup /app/bin/TimeManager \
             "$RW_DIR/tweaks_daemon" "$RW_DIR/tweaks_persist" \
             "$RW_DIR/cluster_gamma" "$RW_DIR/config.conf" "$RW_DIR/status.json" \
             "$QML_DIR/HomeScreen/TweaksPage.qml"; do
        [ -e "$f" ] || continue
        acc=$(getfattr -n security.SMACK64 --only-values "$f" 2>/dev/null || true)
        ex=$(getfattr -n security.SMACK64EXEC --only-values "$f" 2>/dev/null || true)
        printf '%s: SMACK64=%s SMACK64EXEC=%s\n' "$f" "${acc:-none}" "${ex:-none}"
    done
    for s in /var/run/dbus/system_bus_socket /run/dbus/system_bus_socket \
             /var/run/user/root/wayland-0 /run/user/root/wayland-0; do
        [ -S "$s" ] || continue
        echo "$s: $(getfattr -n security.SMACK64 --only-values "$s" 2>/dev/null || echo none)"
    done
else
    echo "(getfattr not available)"
fi
pid=$(pidof tweaks_daemon 2>/dev/null | awk '{print $1}')
if [ -n "$pid" ]; then
    echo "running tweaks_daemon pid=$pid label: $(cat "/proc/$pid/attr/current" 2>/dev/null || echo unknown)"
else
    echo "tweaks_daemon not running"
fi
echo
echo "== micom D-Bus reachability (no CAN write; Introspect may activate) =="
if command -v dbus-send >/dev/null 2>&1; then
    names=$(dbus-send --system --print-reply --reply-timeout=4000 \
        --dest=org.freedesktop.DBus / org.freedesktop.DBus.ListNames 2>&1)
    case "$names" in
        *com.lge.car.micom*) echo "com.lge.car.micom registered" ;;
        *) echo "com.lge.car.micom NOT registered (or bus access denied)" ;;
    esac
    dbus-send --system --print-reply --reply-timeout=4000 \
        --dest=com.lge.car.micom /Can \
        org.freedesktop.DBus.Introspectable.Introspect 2>&1 | head -12 || true
else
    echo "(dbus-send not available)"
fi
echo
echo "== persistence RCT hunt =="
rct=$(find /Data /rw_data /space /opt /etc -name 'resource-table*' -o -name '*.itz' 2>/dev/null | head -5)
[ -n "$rct" ] && echo "$rct" || echo "(no RCT found in the usual places)"
for f in $rct; do
    [ -f "$f" ] && cp -f "$f" "$USB_DIR/" 2>/dev/null && echo "copied $f to the stick" || true
done
echo
echo "== processes =="
ps -ef 2>/dev/null | grep -E 'tweaks_daemon|AppProjection|AppHomeScreen|ApplicationManager|wrandr' | grep -v grep || true
echo
echo "== QML markers =="
for f in HomeScreen/QHMI_HomeScreenMain_Advanced.qml \
         HomeScreen/QHMI_HomeScreenMain_Wide.qml \
         HomeScreen/Screen_HomeScreen_AllMenus.qml \
         HomeScreen/Screen_HomeScreen_Submenu_Setup.qml \
         component/QCustomizedMenuListAllowEmpty.qml \
         HomeScreen/TweaksPage.qml; do
    p="$QML_DIR/$f"
    if [ -f "$p" ]; then
        echo "$f: $(md5sum "$p" 2>/dev/null | cut -d' ' -f1) $(grep -o 'TWEAKS_PAYLOAD_260128\|WIFI_PAYLOAD_260128\|HELLO_PAYLOAD_260128\|BATTERY_PAYLOAD_260128' "$p" 2>/dev/null | sort -u | tr '\n' ' ')"
    else
        echo "$f: missing"
    fi
done
echo
echo "== AppStandbyClock QML markers =="
for base in qml_eco3.0 new_qml qml_genesis; do
    for f in main.qml Screen_AppStandbyClock_Confirm.qml Screen_AppStandbyClock_Confirm_Profile.qml; do
        p="$STANDBY_ROOT/$base/$f"
        if [ -f "$p" ]; then
            echo "$base/$f: $(grep -q 'TWEAKS_DISCLAIMER_260128' "$p" && echo patched || echo stock)"
        else
            echo "$base/$f: missing"
        fi
    done
done
echo
echo "== 8s dbus-monitor: appmanager.info (system + session bus) =="
if command -v dbus-monitor >/dev/null 2>&1; then
    dbus-monitor --system "type='signal',interface='com.lge.avn.appmanager.info'" > "$TMP" 2>&1 &
    pid=$!
    spid=""
    if [ -f /tmp/session_appmgr ]; then
        addr=$(cut -d, -f1 /tmp/session_appmgr 2>/dev/null)
        if [ -n "$addr" ]; then
            dbus-monitor --address "$addr" \
                "type='signal',interface='com.lge.avn.appmanager.info'" > "$TMP.sess" 2>&1 &
            spid=$!
        fi
    fi
    sleep 8
    echo "--- system bus:"
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    cat "$TMP" 2>/dev/null | head -200
    rm -f "$TMP"
    if [ -n "$spid" ]; then
        echo "--- session bus ($addr):"
        kill "$spid" 2>/dev/null || true
        wait "$spid" 2>/dev/null || true
        cat "$TMP.sess" 2>/dev/null | head -200
        rm -f "$TMP.sess"
    else
        echo "--- session bus: no address in /tmp/session_appmgr"
    fi
else
    echo "(dbus-monitor missing)"
fi
} > "$OUT" 2>&1

cp -f "$OUT" "$RW_DIR/diag.txt" 2>/dev/null || true
echo "wrote $OUT"
exit 0
