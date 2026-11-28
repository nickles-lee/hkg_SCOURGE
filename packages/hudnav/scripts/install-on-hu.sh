#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
#
# TARGET: AU Linux Gen5W 260319 head unit, run as root from the kit directory.
#
# Installs the HUD Nav runtime into /rw_data/hudnav:
#   bin/hudnavd            static daemon (transmits only in hudnav-run.sh `tx` mode)
#   bin/hudnavd-diag       static diagnostic daemon (log backend; CAN TX not compiled in)
#   bin/hudnav-run.sh      runner that feeds the daemon the AppManager ownership signals
#   bin/aa_shim.so         gated AppProjection interposer
#   bin/cp_hook.so         gated iAP2Service-2.0 interposer
#   bin/CANService         verbatim copy of the car's /usr/bin/dbus-send
#   bin/micom-send         wrapper that execs CANService with the proven flags
#   bin/micom-send-tp      same for CAN-TP text messages (0x6E7 only): the HUD turn arrow comes from these
#   hudnavd.service        unit file, written but NOT enabled or started
#
# This script never sends CAN, never starts the daemon, and never patches a
# signed vendor binary. See docs/hudnav/HUD_NAV_REPORT.md for the gate analysis.
#
# Usage: sh install-on-hu.sh [--check]
set -eu
PATH="${HUDNAV_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
SRC=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="${HUDNAV_RW_DIR:-/rw_data/hudnav}"
BIN="$DEST/bin"
LOG="$DEST/install.log"

say() { echo "== $* =="; }
die() { echo "FAIL: $*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "must run as root"

if [ "${1:-}" = "--check" ]; then
    say "installed files"
    for f in "$BIN/hudnavd" "$BIN/hudnavd-diag" "$BIN/hudnav-run.sh" "$BIN/aa_shim.so" "$BIN/cp_hook.so" "$BIN/CANService" "$BIN/micom-send" "$BIN/micom-send-tp" "$DEST/hudnavd.service"; do
        [ -e "$f" ] && ls -l "$f" || echo "missing: $f"
    done
    [ -f "$BIN/hudnavd" ] && sha256sum "$BIN/hudnavd" "$BIN/hudnavd-diag" "$BIN/hudnav-run.sh" "$BIN/aa_shim.so" "$BIN/cp_hook.so" 2>/dev/null || true
    exit 0
fi

for f in hudnavd hudnavd-diag hudnav-run.sh aa_shim.so cp_hook.so; do
    [ -f "$SRC/$f" ] || die "kit is incomplete: $SRC/$f missing"
done
[ -x /usr/bin/dbus-send ] || die "/usr/bin/dbus-send missing"

mkdir -p "$BIN" "$DEST/backup"
: > "$LOG"
log() { echo "$(date -u 2>/dev/null) $*" >> "$LOG"; }

say "1. copy runtime"
# Remove before copying so a previous inode's xattrs cannot leak onto the new file.
rm -f "$BIN/hudnavd" "$BIN/hudnavd-diag" "$BIN/hudnav-run.sh" "$BIN/aa_shim.so" "$BIN/cp_hook.so" "$BIN/noop.so"
cp "$SRC/hudnavd" "$BIN/hudnavd"
cp "$SRC/hudnavd-diag" "$BIN/hudnavd-diag"
cp "$SRC/hudnav-run.sh" "$BIN/hudnav-run.sh"
cp "$SRC/aa_shim.so" "$BIN/aa_shim.so"
cp "$SRC/cp_hook.so" "$BIN/cp_hook.so"
[ -f "$SRC/noop.so" ] && cp "$SRC/noop.so" "$BIN/noop.so"
chmod 755 "$BIN/hudnavd" "$BIN/hudnavd-diag" "$BIN/hudnav-run.sh"
chmod 644 "$BIN/aa_shim.so" "$BIN/cp_hook.so"
[ -f "$BIN/noop.so" ] && chmod 644 "$BIN/noop.so"
log "copied hudnavd/hudnavd-diag/hudnav-run.sh/aa_shim.so/cp_hook.so"

say "2. CANService helper (verbatim dbus-send, proven basename)"
rm -f "$BIN/CANService"
cp /usr/bin/dbus-send "$BIN/CANService"
chmod 755 "$BIN/CANService"
sha256sum /usr/bin/dbus-send "$BIN/CANService" | tee -a "$LOG"
cmp -s /usr/bin/dbus-send "$BIN/CANService" || die "CANService copy differs from dbus-send"

say "3. micom-send wrapper"
cat > "$BIN/micom-send" <<'EOF'
#!/bin/sh
# hudnavd -> renamed dbus-send -> com.lge.car.micom /Can SetCANMicomRecv
# --print-reply keeps the process alive for the micom /proc/<pid>/exe lookup.
out=$("$(dirname "$0")/CANService" --system --type=method_call --print-reply \
    --reply-timeout=500 --dest=com.lge.car.micom /Can \
    com.lge.car.micom.can.SetCANMicomRecv "$1" 2>&1)
rc=$?
[ $rc -eq 0 ] && exit 0
case "$out" in
    *NoReply*|*"Did not receive a reply"*) exit 0 ;;
esac
printf '%s\n' "$out" >&2
exit $rc
EOF
chmod 755 "$BIN/micom-send"
# CAN-TP (multi-frame text) sender. The HUD draws its turn arrow from the cluster text channel 0x6E7
# (SID F2; car-proven 2026-11-25). One message per argument, sent in order. It refuses anything that
# is not a 0x6E7 message, so it can never be pointed at another ID. hudnavd finds it next to
# micom-send; if it is absent the TP part is simply not transmitted.
cat > "$BIN/micom-send-tp" <<'EOF'
#!/bin/sh
d=$(dirname "$0"); first=1
for arr in "$@"; do
    case "$arr" in
        array:byte:0x06,0xe7,*) ;;
        *) echo "micom-send-tp: refusing non-0x6E7 array" >&2; exit 2 ;;
    esac
    [ -n "$first" ] || sleep 0.15
    first=
    out=$("$d/CANService" --system --type=method_call --print-reply \
        --reply-timeout=500 --dest=com.lge.car.micom /Can \
        com.lge.car.micom.can.SetCANTPMicomRecv "$arr" 2>&1)
    rc=$?
    [ $rc -eq 0 ] && continue
    case "$out" in
        *NoReply*|*"Did not receive a reply"*) continue ;;
    esac
    printf '%s\n' "$out" >&2
    exit $rc
done
exit 0
EOF
chmod 755 "$BIN/micom-send-tp"

say "4. SMACK labels (exec label = this installer's own label)"
grep -q ' smackfs ' /proc/mounts 2>/dev/null || die "smackfs not mounted"
LABEL=$(cat /proc/self/attr/current 2>/dev/null | tr -d '\r\n')
[ -n "$LABEL" ] || die "cannot read /proc/self/attr/current"
command -v chsmack >/dev/null 2>&1 || die "chsmack missing"
for f in "$BIN/hudnavd" "$BIN/hudnavd-diag" "$BIN/CANService" "$BIN/micom-send" "$BIN/micom-send-tp"; do
    chsmack -e "$LABEL" "$f" >> "$LOG" 2>&1 || die "chsmack failed: $f"
    got=$(getfattr -n security.SMACK64EXEC --only-values "$f" 2>/dev/null || true)
    [ -z "$got" ] || [ "$got" = "$LABEL" ] || die "label readback mismatch: $f"
    log "label $f = ${got:-unreadable} (expected $LABEL)"
done

say "5. systemd unit (written, NOT enabled)"
# hudnav-run.sh, not hudnavd directly: --appmanager-stdin needs the AppManager feed, and
# systemd gives a service /dev/null as stdin (the daemon would go permanently mute).
# `run diag` cannot transmit CAN. Enabling CAN TX is a deliberate edit of this line
# (`run tx --yes`), never a default.
cat > "$DEST/hudnavd.service" <<'EOF'
[Unit]
Description=HUD Nav projection navigation bridge (log-only diag mode)
After=dbus.service Dbuslaunch.service
Wants=dbus.service

[Service]
Type=simple
ExecStart=/bin/sh /rw_data/hudnav/bin/hudnav-run.sh run diag
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

say "done (nothing started, no CAN sent)"
cat <<EOF
Installed under: $DEST
Log:            $LOG

Next, read-only:
  $BIN/hudnavd --help
  /proc/<pid>/attr/current                 # verify the live daemon label
  dbus-send --system --print-reply --dest=com.lge.car.micom /Can \\
      org.freedesktop.DBus.Introspectable.Introspect

Run the receiver ONLY through hudnav-run.sh (log-only first):
  sh $BIN/hudnav-run.sh start diag         # logs every shim line + ownership change
  sh $BIN/hudnav-run.sh status
  sh $BIN/hudnav-run.sh log 40
  sh $BIN/hudnav-run.sh stop

Android Auto activation (reversible, one line):
  sh $SRC/aa-preload.sh status
  sh $SRC/aa-preload.sh enable --yes
  reboot
  sh $SRC/aa-preload.sh verify

CarPlay activation (reversible, one added line in the systemd unit):
  sh $SRC/cp-preload.sh status
  sh $SRC/cp-preload.sh enable --yes
  sh $SRC/cp-preload.sh verify

Do NOT use 'hudnav-run.sh start tx --yes' until the owner signs off on CAN TX,
the car is parked, and the log-only pass has shown real TURN/DIST events.
EOF
