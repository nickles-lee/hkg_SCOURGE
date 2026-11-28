#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
#
# TARGET: AU Linux Gen5W 260319 head unit, run as root:  sh install-persist.sh [--yes]
# Run from the kit folder /rw_data/hudnav/bin (it expects hudnav-aa-boot.sh, hudnav-bootguard.sh,
# hudnav-storeguard.sh and the four .service files next to it). Without --yes: report only.
#
# Installs the persistent Android Auto -> HUD feature with its fallbacks:
#   1. settings-store guard: save a good copy of the shared settings index NOW (store must be healthy), arm it
#   2. boot guard v2: arm it (two failed UI boots in a row -> automatic rollback = kit renamed)
#   3. five units into /etc/systemd/system + links in multi-user.target.wants (the ONLY change to /).
#      hudnav-premount.service is new: it bind-mounts the patched AA/CP copies BEFORE the
#      vendor services start, so the boot script never has to restart them.
#   4. master switch /rw_data/hudnav/enabled + nav gate /rw_data/hudnav/aa-nav-enable
# It does not start anything now: the next boot is the first real test.
# Undo: uninstall-persist.sh --yes (or delete /rw_data/hudnav/enabled to disable without uninstalling).
#
# Test hooks: HUDNAV_ROOT (fake root; skips remount/systemctl), HUDNAV_PATH_PREPEND.
set -u
PATH="${HUDNAV_PATH_PREPEND:+$HUDNAV_PATH_PREPEND:}/bin:/sbin:/usr/bin:/usr/sbin"
R="${HUDNAV_ROOT:-}"
KIT="$R/rw_data/hudnav"
BIN="$KIT/bin"
SD="$R/etc/systemd/system"
WANTS="$SD/multi-user.target.wants"
BG="$R/rw_data/hudnav-bootguard"
SG="$R/rw_data/hudnav-storeguard"
UNITS="hudnav-storeguard.service hudnav-bootguard.service hudnav-bootok.service hudnav-premount.service hudnav-aa.service"
YES=0; [ "${1:-}" = "--yes" ] && YES=1

fail() { echo "REFUSE: $*" >&2; exit 1; }

# ---- preflight (always, read-only) ----
[ ! -e "$R/etc/ld.so.preload" ] || fail "/etc/ld.so.preload exists"
for f in hudnav-aa-boot.sh hudnav-premount.sh hudnav-bootguard.sh hudnav-storeguard.sh aa-session.sh hudnav-run.sh hudnavd \
         micom-send micom-send-tp CANService AppProjection.aa aa_shim.so SHA256SUMS $UNITS; do
    [ -f "$BIN/$f" ] || fail "missing $BIN/$f"
done
(cd "$BIN" && sha256sum -c SHA256SUMS >/dev/null 2>&1) || fail "kit does not match $BIN/SHA256SUMS"
sh "$BIN/hudnav-storeguard.sh" status 2>/dev/null | grep -q 'live resource-table-cfg.itz: valid' || fail "settings index not healthy; not installing"
for u in $UNITS; do [ -e "$SD/$u" ] && fail "$SD/$u already exists (run uninstall-persist.sh first)"; done
echo "preflight OK: no preload, kit verified, settings store healthy, units not yet installed"

if [ "$YES" != 1 ]; then
    echo "REPORT ONLY (add --yes): would save+arm the settings guard, arm the boot guard,"
    echo "  install $UNITS into $SD (+ links in $WANTS), create $KIT/enabled and $KIT/aa-nav-enable"
    echo "  hudnav-premount.service runs before carplay_iAP2.service and ApplicationLauncher.service"
    exit 0
fi

# ---- 1. settings-store guard ----
mkdir -p "$SG" && cp "$BIN/hudnav-storeguard.sh" "$SG/hudnav-storeguard.sh" && chmod 755 "$SG/hudnav-storeguard.sh" || fail "storeguard copy"
HUDNAV_ROOT="$R" sh "$SG/hudnav-storeguard.sh" save || fail "storeguard save failed"
: > "$SG/armed"; echo "storeguard: good copy saved, armed"

# ---- 2. boot guard ----
mkdir -p "$BG" && cp "$BIN/hudnav-bootguard.sh" "$BG/hudnav-bootguard.sh" && chmod 755 "$BG/hudnav-bootguard.sh" || fail "bootguard copy"
rm -f "$BG/pending" "$BG/rolled_back"; echo 0 > "$BG/fails"; : > "$BG/armed"; echo "bootguard: armed"

# ---- 3. units (the only change to /) ----
if [ -z "$R" ]; then mount -o remount,rw / || fail "remount rw failed"; fi
ok=1
for u in $UNITS; do
    cp "$BIN/$u" "$SD/$u" && chmod 644 "$SD/$u" && ln -sf "/etc/systemd/system/$u" "$WANTS/$u" || ok=0
done
sync
if [ -z "$R" ]; then mount -o remount,ro / || echo "WARNING: remount ro failed"; fi
[ "$ok" = 1 ] || fail "unit install incomplete (run uninstall-persist.sh --yes)"
if [ -z "$R" ]; then systemctl daemon-reload; fi
echo "units installed: $UNITS"

# ---- 4. switches ----
chmod 755 "$BIN/hudnav-aa-boot.sh" "$BIN/hudnav-premount.sh" "$BIN/aa-session.sh" "$BIN/hudnav-run.sh" "$BIN/hudnavd" \
          "$BIN/micom-send" "$BIN/micom-send-tp" "$BIN/CANService" "$BIN/AppProjection.aa"
: > "$KIT/aa-nav-enable"; : > "$KIT/enabled"; sync
echo "INSTALLED. Reboot (car off/on) for the first persistent boot; check $KIT/boot.log and $BG/log afterwards."
