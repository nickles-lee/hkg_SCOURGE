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
    echo "  tweaks  — night-mode fixes, auto-DST, OTA block, disclaimer auto-accept"
    echo "  [WIP, signatureless-update research — docs/08; VM-validated, car-pending]"
    echo "  button-gate   — AppScomoDM stubs + drive wrapper + watcher (tools/update-debug)"
    echo "  applauncher   — AppLauncher .sign stubs"
    echo "  dmclient-gate  — the REAL button gate (AppDMClient) + launcher prerequisite"
    printf 'comma-separated list (default: the three apps): '
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

# --- build each selected package -------------------------------------------------
if [ "${SCOURGE_SKIP_BUILD:-0}" != "1" ]; then
    for pkg in $SELECTED; do
        case "$pkg" in
        battery|wifi|tweaks)
            echo "== building package: $pkg"
            sh "$HERE/$pkg/scripts/build_payload.sh" || {
                echo "package build failed: $pkg" >&2; exit 1; } ;;
        *)  : ;;   # WIP gate stages stage from tools/update-debug below
        esac
    done
else
    echo "== SCOURGE_SKIP_BUILD=1 — using existing out/usb trees"
fi

# --- stage ----------------------------------------------------------------------
STAGE=$(mktemp -d /tmp/scourge_compose.XXXXXX)
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/combo"

stage_pkg() {
    name="$1"
    src="$HERE/$name/out/usb"
    [ -d "$src" ] || src="$HERE/$name/out/battery_usb"
    dst="$STAGE/combo/$name"
    mkdir -p "$dst"
    cp -R "$src/payload" "$dst/"
    cp -R "$src/backup" "$dst/" 2>/dev/null || true
    mkdir -p "$dst/backup_current"
    cp -f "$src/manifest.md5" "$dst/"
    cp -f "$src/run_stuff.sh" "$dst/stage.sh"
    cp -f "$src/run_stuff_restore.sh" "$dst/restore.sh" 2>/dev/null || true
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
PAYLOAD_ID=$(find "$STAGE/combo" -type f \( -name manifest.md5 -o -name stage.sh -o -name restore.sh \) -print0 \
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
PATH=/bin:/sbin:/usr/bin:/usr/sbin
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

run_stage(){
    name="\$1"; script="\$2"
    log "=== stage \$name ==="
    if [ ! -f "\$script" ]; then
        echo "stage_\$name=status=missing" >> "\$STAGES_LOG"
        FAILED="\$FAILED \$name"
        return 1
    fi
    # Per-package installers honor USB_DIR; point them at their own subtree.
    if USB_DIR="\$(dirname "\$script")" /bin/sh "\$script" >> "\$LOG" 2>&1; then
        echo "stage_\$name=status=OK rc=0" >> "\$STAGES_LOG"
        return 0
    else
        echo "stage_\$name=status=FAIL" >> "\$STAGES_LOG"
        FAILED="\$FAILED \$name"
        return 1
    fi
}

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
  *) echo "unknown MODE: $MODE" >> "$STAGES_LOG" ;;
esac

if [ -n "$FAILED" ]; then
    result "FAIL"
    exit 1
fi
result "OK"
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
sync
echo "DONE. Stick contents:"
ls -la "$TARGET"
