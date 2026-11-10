#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# compose.sh — assemble ONE combo USB stick with the selected SCOURGE packages.
#
# Proven pattern (combo v3): each package ships its own installer as
# combo/<pkg>/stage.sh (+ restore.sh); the master run_stuff.sh runs the
# selected installers in a fixed order — battery -> wifi -> tweaks — because
# each installer detects the current QML base and picks a matching variant.
# Restore runs in reverse. The failsafe's md5 gate re-runs the master script
# whenever its content (the MODE= line) changes.
#
# Usage:
#   compose.sh /path/to/mounted/stick [battery,wifi,tweaks]
#   (no package list = interactive prompt)
set -eu
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
TARGET="${1:?usage: compose.sh /path/to/stick [pkg,pkg,...]}"
SELECTION="${2:-}"

# Battery builds into a private tree; clean it up no matter where a later
# step aborts.  Re-installed below once the staging tree exists.
trap '[ -z "${BATTERY_BUILD:-}" ] || rm -rf "$BATTERY_BUILD"' EXIT

[ -d "$TARGET" ] || { echo "not a directory: $TARGET" >&2; exit 1; }
[ "$TARGET" != "/" ] || { echo "refusing to deploy to /" >&2; exit 1; }

# --- package selection -----------------------------------------------------------
AVAILABLE="battery wifi tweaks button-gate applauncher dmclient-gate"
DEFAULT_ORDER="battery wifi tweaks"
WIP_STAGES="button-gate applauncher dmclient-gate"

if [ -z "$SELECTION" ]; then
    echo "Which packages should go on the stick?"
    echo "  battery — Battery tile + preconditioning/charge-door daemon"
    echo "  wifi    — WiFi client / CarPlay AP / SSH (dropbear)"
    echo "  tweaks  — night-mode fixes, auto-DST, OTA block, disclaimer, Reboot app"
    echo "  [WIP, signatureless-update research — docs/08; VM-validated, car-pending]"
    echo "  button-gate   — AppScomoDM stubs + drive wrapper + watcher (tools/update-debug)"
    echo "  applauncher   — AppLauncher .sign stubs"
    echo "  dmclient-gate  — the REAL button gate (AppDMClient) + launcher prerequisite"
    printf 'comma-separated list (default: the three packages; Tweaks includes Reboot): '
    read -r SELECTION
    [ -n "$SELECTION" ] || SELECTION="$DEFAULT_ORDER"
fi

SELECTED=""
for pkg in $(echo "$SELECTION" | tr ',' ' '); do
    echo "$AVAILABLE" | grep -qw "$pkg" || {
        echo "unknown package: $pkg (available: $AVAILABLE)" >&2; exit 1; }
    [ -n "$SELECTED" ] && SELECTED="$SELECTED "
    SELECTED="$SELECTED$pkg"
done
# Always preserve dependency order, regardless of the user's selection order.
REQUESTED="$SELECTED"
SELECTED=""
for pkg in $AVAILABLE; do
    case " $REQUESTED " in *" $pkg "*) SELECTED="${SELECTED:+$SELECTED }$pkg" ;; esac
done

# --- build each selected package -------------------------------------------------
if [ "${SCOURGE_SKIP_BUILD:-0}" != "1" ]; then
    for pkg in $SELECTED; do
        case "$pkg" in
        battery|wifi|tweaks)
            echo "== building package: $pkg"
            if [ "$pkg" = battery ]; then
                mkdir -p "$HERE/battery/out"
                BATTERY_BUILD=$(mktemp -d "$HERE/battery/out/compose.XXXXXX")
                sh "$HERE/battery/scripts/build_payload.sh" "$BATTERY_BUILD"
            else
                sh "$HERE/$pkg/scripts/build_payload.sh"
            fi || {
                echo "package build failed: $pkg" >&2; exit 1; } ;;
        *)  : ;;   # WIP gate stages stage from tools/update-debug below
        esac
    done
else
    echo "== SCOURGE_SKIP_BUILD=1 — using existing out/usb trees"
fi

# --- stage ----------------------------------------------------------------------
# Product path: never depend on a harness-specific temp directory.
STAGE=$(mktemp -d "${TMPDIR:-/tmp}/scourge_compose.XXXXXX")
trap 'rm -rf "$STAGE"; [ -z "${BATTERY_BUILD:-}" ] || rm -rf "$BATTERY_BUILD"' EXIT
mkdir -p "$STAGE/combo"

stage_pkg() {
    name="$1"
    src="$HERE/$name/out/usb"
    [ -d "$src" ] || src="$HERE/$name/out/battery_usb"
    if [ "$name" = battery ] && [ -n "${BATTERY_BUILD:-}" ]; then src=$BATTERY_BUILD; fi
    dst="$STAGE/combo/$name"
    mkdir -p "$dst"
    cp -R "$src/payload" "$dst/"
    cp -R "$src/backup" "$dst/" 2>/dev/null || true
    mkdir -p "$dst/backup_current"
    cp -f "$src/manifest.md5" "$dst/"
    cp -f "$src/run_stuff.sh" "$dst/stage.sh"
    cp -f "$src/run_stuff_restore.sh" "$dst/restore.sh" 2>/dev/null || true
    (cd "$dst" && md5sum -c manifest.md5 >/dev/null) || exit 1
    if [ "$name" = tweaks ]; then
        python3 "$HERE/common/check_layout.py" \
            "$dst/payload/qml/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.battery_tile_wifi.qml"
    fi
}

stage_wip() {
    # WIP signatureless-update stages (docs/08): staged from tools/update-debug.
    name="$1"
    src="$HERE/../tools/update-debug"
    dst="$STAGE/combo/$name"
    mkdir -p "$dst"
    case "$name" in
    button-gate)   deploy="$src/button_gate.sh" ;;
    applauncher)   deploy="$src/applauncher_sigstub.sh"
                   cp -f "$src/applauncher_sigstub_restore.sh" "$dst/restore.sh"
                   cp -f "$src/applauncher_sigstub_diag.sh" "$dst/diag.sh" ;;
    dmclient-gate) deploy="$src/dmclient_gate.sh" ;;
    esac
    cp -f "$deploy" "$dst/stage.sh"
    # The gate deployers need the patched wrapper container next to them.
    wrapper="$HERE/../out/AppUpgrade.patched.te2"
    if [ -f "$wrapper" ]; then
        cp -f "$wrapper" "$dst/AppUpgrade.patched.te2"
    else
        echo "WARNING: $dst is missing AppUpgrade.patched.te2 —" >&2
        echo "  run scripts/07_build_patched_usb_appupgrade.sh first" >&2
    fi
}

for pkg in $SELECTED; do
    case "$pkg" in
    battery|wifi|tweaks) stage_pkg "$pkg" ;;
    *) stage_wip "$pkg" ;;
    esac
done

# --- master run_stuff.sh (MODE= pattern) -------------------------------------------
ORDER=$(echo "$SELECTED" | tr ' ' ',')
ORDER_SPACES="$SELECTED"

# Digest of every staged installer/manifest. The loader re-runs the master
# script when its bytes change, so a same-selection rebuild with different
# package payloads still triggers an install on the next plug-in.
PAYLOAD_ID=$(cd "$STAGE" && find combo -type f \( -name manifest.md5 -o -name stage.sh -o -name restore.sh \) -print0 \
    | sort -z | xargs -0 md5sum | md5sum | cut -c1-16)

cat > "$STAGE/run_stuff.sh" <<EOF
#!/bin/sh
# SCOURGE_COMBO_REV=1 — master installer for: $ORDER
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only (https://gnu.org/licenses/agpl-3.0)
#
# Put this file and combo/ at the ROOT of a FAT/exFAT USB stick. The
# initial-root failsafe runs it as root within ~8 s of a content change.
#
# PAYLOAD_ID is a digest of the selected stage installers + manifests. It is
# part of this file, so a rebuilt package changes the loader's md5 gate.
PAYLOAD_ID="$PAYLOAD_ID"
#
# MODE (edit the line below; the md5 change makes the loader re-run it):
#   all       every selected package, in dependency-safe order (default)
#   restore   restore every selected package in reverse order
#   battery|wifi|tweaks   just that package
MODE="\${SCOURGE_MODE:-all}"

set -u
PATH="\${COMBO_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="\${USB_DIR:-\$(CDPATH= cd -- "\$(dirname "\$0")" && pwd)}"
COMBO="\$USB_DIR/combo"
RW_DIR="\${COMBO_RW_DIR:-/rw_data/combo}"
mkdir -p "\$RW_DIR" 2>/dev/null || RW_DIR="\$USB_DIR"
LOG="\$RW_DIR/combo.log"
: > "\$LOG" 2>/dev/null
log(){ echo "\$(date -u 2>/dev/null) \$*" >> "\$LOG" 2>/dev/null; }

result(){
    {
        echo "SCOURGE_COMBO"
        echo "time=\$(date -u 2>/dev/null)"
        echo "usb=\$USB_DIR"
        echo "uid=\$(id -u 2>/dev/null)"
        echo "mode=\$MODE"
        echo "payload_id=\$PAYLOAD_ID"
        cat "\$STAGES_LOG" 2>/dev/null
        echo "status=\$1"
    } > "\$USB_DIR/run_stuff_result.txt" 2>&1
    cp -f "\$USB_DIR/run_stuff_result.txt" "\$RW_DIR/result.txt" 2>/dev/null
    sync
}

STAGES_LOG="\$RW_DIR/stages.log"
: > "\$STAGES_LOG" 2>/dev/null
FAILED=""

# Audible installer status through the factory micom audio service.  The
# installer copies its own dbus-send so the call runs in the same SMACK
# context as the working micom client (the floor rule allows
# "_ App::micommanager rwx---").  Beeps are strictly non-fatal.
BEEP_DBUS="\$RW_DIR/bin/dbus-send"
beep_init(){
    [ -x "\$BEEP_DBUS" ] && return 0
    _src=/usr/bin/dbus-send
    [ -x "\$_src" ] || _src=/bin/dbus-send
    [ -x "\$_src" ] || return 1
    mkdir -p "\$RW_DIR/bin" 2>/dev/null || return 1
    rm -f "\$BEEP_DBUS" 2>/dev/null || true
    cp -f "\$_src" "\$BEEP_DBUS" 2>/dev/null || return 1
    chmod 755 "\$BEEP_DBUS" 2>/dev/null || true
    chown root:root "\$BEEP_DBUS" 2>/dev/null || true
    _label=\$(cat /proc/self/attr/current 2>/dev/null | tr -d '\r\n')
    [ -n "\$_label" ] && chsmack -e "\$_label" "\$BEEP_DBUS" 2>/dev/null || true
    [ -x "\$BEEP_DBUS" ]
}
beep_once(){
    _b=\$BEEP_DBUS
    [ -x "\$_b" ] || _b=/usr/bin/dbus-send
    [ -x "\$_b" ] || return 0
    "\$_b" --system --print-reply --reply-timeout=1000 --dest=com.lge.car.micom \
        /com/lge/car/micom com.lge.car.micom.audio.SetBeepPlay \
        array:byte:0x00 >>"\$LOG" 2>&1 && return 0
    # The system bus may still be coming up when the loader starts us.
    sleep 0.3 2>/dev/null || sleep 1
    "\$_b" --system --print-reply --reply-timeout=1000 --dest=com.lge.car.micom \
        /com/lge/car/micom com.lge.car.micom.audio.SetBeepPlay \
        array:byte:0x00 >>"\$LOG" 2>&1 || true
}
beep_n(){
    _n=\$1
    beep_init 2>/dev/null || true
    _i=0
    while [ "\$_i" -lt "\$_n" ]; do
        beep_once
        _i=\$((_i + 1))
        if [ "\$_i" -lt "\$_n" ]; then
            sleep 0.5 2>/dev/null || sleep 1
        fi
    done
    return 0
}

run_stage(){
    name="\$1"; script="\$2"
    stage_dir=\$(dirname "\$script")
    stage_result="\$stage_dir/run_stuff_result.txt"
    # Clear both copies: a stage that dies before writing its result must
    # not leave the previous run's result looking current on the stick.
    rm -f "\$stage_result" "\$USB_DIR/\${name}_result.txt"
    log "=== stage \$name ==="
    if [ ! -f "\$script" ]; then
        echo "stage_\$name=status=missing" >> "\$STAGES_LOG"
        FAILED="\$FAILED \$name"
        beep_n 2
        return 1
    fi
    # Per-package installers honor USB_DIR; point them at their own subtree.
    # Forward explicit owner inputs into the WiFi subtree. Do not package
    # credentials in source or leave stale copies after the stage finishes.
    case "\$name" in wifi)
        for input in wifi.txt authorized_keys MDNSD_ROOT_ARM; do
            [ ! -f "\$USB_DIR/\$input" ] || cp -f "\$USB_DIR/\$input" "\$stage_dir/\$input"
        done ;;
    esac
    USB_DIR="\$stage_dir" /bin/sh "\$script" >> "\$LOG" 2>&1
    stage_rc=\$?
    status=\$(sed -n 's/^status=\([^ ]*\).*/\1/p' "\$stage_result" 2>/dev/null | tail -n 1)
    [ ! -f "\$stage_result" ] || cp -f "\$stage_result" "\$USB_DIR/\${name}_result.txt"
    case "\$name" in wifi) rm -f "\$stage_dir/wifi.txt" "\$stage_dir/authorized_keys" "\$stage_dir/MDNSD_ROOT_ARM" ;; esac
    # A stage that exits 0 with status=REBOOT_REQUIRED succeeded: the WiFi
    # installer reports it when the UI restart could not be verified on the
    # spot.  Only rc=0 plus an accepted status counts; rc=0 with a failure
    # status is still a failure.
    case "\$stage_rc:\$status" in
        0:OK|0:REBOOT_REQUIRED)
            echo "stage_\$name=status=\$status rc=0" >> "\$STAGES_LOG"
            return 0
            ;;
    esac
    echo "stage_\$name=status=\${status:-missing} rc=\$stage_rc" >> "\$STAGES_LOG"
    FAILED="\$FAILED \$name"
    beep_n 2
    return 1
}

# One beep: installer started.
beep_n 1

case "\$MODE" in
  all)     for s in $ORDER_SPACES; do run_stage "\$s" "\$COMBO/\$s/stage.sh" || true; done ;;
EOF

# restore runs in reverse
RESTORE_ORDER=$(echo "$SELECTED" | awk '{for(i=NF;i>0;i--) printf "%s%s", $i, (i>1?" ":"")}')
cat >> "$STAGE/run_stuff.sh" <<EOF
  restore) for s in $RESTORE_ORDER; do run_stage "restore_\$s" "\$COMBO/\$s/restore.sh" || true; done ;;
EOF

for pkg in $SELECTED; do
    cat >> "$STAGE/run_stuff.sh" <<EOF
  $pkg)    run_stage "$pkg" "\$COMBO/$pkg/stage.sh" || true ;;
EOF
done

cat >> "$STAGE/run_stuff.sh" <<'EOF'
  *) echo "unknown MODE: $MODE" >> "$STAGES_LOG"; FAILED="invalid-mode" ;;
esac

if [ -n "$FAILED" ]; then
    result "FAIL"
    beep_n 3
    exit 1
fi
result "OK"
# Five beeps: all requested stages completed successfully.  A short pause
# separates this from the last stage's two-beep signal.
sleep 0.8 2>/dev/null || sleep 1
beep_n 5
exit 0
EOF

cp -f "$HERE/../payload/run_stuff.sample.sh" "$STAGE/run_stuff.sample.sh" 2>/dev/null || true
cat > "$STAGE/README.txt" <<EOF
SCOURGE combo stick
===================
Packages: $ORDER

SCOURGE — The SCOURGE Project contributors (AGPL-3.0-or-later, with the
attribution terms in the project LICENSE). Firmware: Ioniq 5 Gen5W_L 260128.

1. Copy the CONTENTS of this directory to the root of a FAT/exFAT stick.
2. Plug it into the head unit; the failsafe runs run_stuff.sh as root
   within ~8 s.
3. Read run_stuff_result.txt on the stick; every stage reports status=OK.
4. To restore: edit MODE=restore in run_stuff.sh, replug, wait ~20 s.
5. To re-run a single package: edit MODE=<package>, replug.

WiFi note: put your wifi.txt and authorized_keys at the stick ROOT before
the first install; the WiFi installer imports them. Remove them afterwards.
Default SSH: car@<head-unit-ip> -p 2222, password 'electric' — CHANGE IT.

WIP gate stages (if selected — button-gate / applauncher / dmclient-gate):
these are the signatureless-update research deployers (docs/08 of the
repo). They patch the updater's signature gates in place, need
AppUpgrade.patched.te2 next to them, require a REBOOT afterwards, and are
VM-validated but not yet car-validated. REVERT markers restore stock bytes.
EOF

echo "== deploying to $TARGET"
cp -R "$STAGE"/. "$TARGET"/
[ -z "${BATTERY_BUILD:-}" ] || rm -rf "$BATTERY_BUILD"
sync
echo "DONE. Stick contents:"
ls -la "$TARGET"
