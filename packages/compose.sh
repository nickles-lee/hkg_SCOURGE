#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# compose.sh — assemble ONE combo USB stick with the selected SCOURGE packages.
#
# Proven pattern (combo v3): each package ships its own installer as
# combo/<pkg>/stage.sh (+ restore.sh); the master run_stuff.sh runs the
# selected installers in a fixed dependency order — battery -> wifi -> tweaks
# -> wifi1b -> migrate — because each installer detects the current QML base
# and picks a matching variant.  A Wifi1b selection without wifi/migrate is
# the standalone layout and is marked combo/standalone.
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
AVAILABLE="battery wifi tweaks wifi1b migrate button-gate applauncher dmclient-gate"
DEFAULT_ORDER="battery wifi tweaks"
WIP_STAGES="button-gate applauncher dmclient-gate"

if [ -z "$SELECTION" ]; then
    echo "Which packages should go on the stick?"
    echo "  battery — Battery tile + preconditioning/charge-door daemon"
    echo "  wifi    — WiFi client / CarPlay AP / SSH (dropbear)"
    echo "  tweaks  — night-mode fixes, auto-DST, OTA block, disclaimer, Reboot app"
    echo "  wifi1b  — separate CarPlay AP/client app beside WiFi (or standalone)"
    echo "  migrate — one-way: remove WiFi, keep Battery/Tweaks and rebase Wifi1b"
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

# A Wifi1b-only selection beside Battery/Tweaks is the standalone layout: it
# never reinstalls WiFi and has no one-way migration stage.  build_flat.sh
# requests this explicitly; an interactive selection gets the same treatment.
STANDALONE=0
case " $SELECTED " in
    *" wifi1b "*)
        case " $SELECTED " in
            *" wifi "*|*" migrate "*) ;;
            *) STANDALONE=1 ;;
        esac ;;
esac
if [ "${SCOURGE_STANDALONE:-0}" = 1 ]; then
    case " $SELECTED " in
        *" wifi "*|*" migrate "*)
            echo "standalone bundle must not include wifi or migrate" >&2; exit 1 ;;
    esac
    for required in battery tweaks wifi1b; do
        case " $SELECTED " in
            *" $required "*) ;;
            *) echo "standalone bundle requires $required" >&2; exit 1 ;;
        esac
    done
    STANDALONE=1
fi

# --- build each selected package -------------------------------------------------
if [ "${SCOURGE_SKIP_BUILD:-0}" != "1" ]; then
    for pkg in $SELECTED; do
        case "$pkg" in
        battery|wifi|tweaks|wifi1b|migrate)
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
# Independently manifest the preflight helpers; verify them before sourcing
# on-car.  runtime_cleanup.sh is shared by the wrapper and the master so the
# cleanup policy has one implementation and one integrity gate.
cp "$HERE/common/install_io.sh" "$STAGE/combo/install_io.sh"
cp "$HERE/common/runtime_cleanup.sh" "$STAGE/combo/runtime_cleanup.sh"
RUNTIME_CLEANUP_MD5=$(md5sum "$STAGE/combo/runtime_cleanup.sh" | cut -d' ' -f1)
if [ "$STANDALONE" = 1 ]; then
    printf 'standalone=1\nlayout=battery,tweaks,wifi1b\n' > "$STAGE/combo/standalone"
    STANDALONE_MD5=$(md5sum "$STAGE/combo/standalone" | cut -d' ' -f1)
else
    STANDALONE_MD5=""
fi
(
    cd "$STAGE" &&
    md5sum combo/install_io.sh combo/runtime_cleanup.sh > combo/preflight.md5 &&
    [ -z "$STANDALONE_MD5" ] || md5sum combo/standalone >> combo/preflight.md5
)
BUNDLE_LAYOUT=combo
[ "$STANDALONE" = 0 ] || BUNDLE_LAYOUT=standalone
# Only a bundle that installs or removes the WiFi radio owner may stop WiFi.
# A wifi1b-only standalone bundle must leave a working WiFi app alone.
STOPS_WIFI=0
case " $SELECTED " in
    *" wifi "*|*" migrate "*) STOPS_WIFI=1 ;;
esac

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
    # Unchanged-stage skip data (see the master's stage_skippable) allows a
    # rebuilt bundle to leave already-installed packages untouched.
    for extra in skip.map skip.services; do
        [ ! -f "$HERE/$name/$extra" ] || cp -f "$HERE/$name/$extra" "$dst/$extra"
    done
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
    battery|wifi|tweaks|wifi1b|migrate) stage_pkg "$pkg" ;;
    *) stage_wip "$pkg" ;;
    esac
done

# --- master run_stuff.sh (MODE= pattern) -------------------------------------------
ORDER=$(echo "$SELECTED" | tr ' ' ',')
ORDER_SPACES="$SELECTED"

# Digest of every staged installer/manifest/helper. The loader re-runs the
# master script when its bytes change, so a same-selection rebuild with
# different package payloads still triggers an install on the next plug-in.
PAYLOAD_ID=$(cd "$STAGE" && find combo -type f \( -name manifest.md5 -o -name stage.sh -o -name restore.sh \
    -o -name runtime_cleanup.sh -o -name standalone \) -print0 \
    | LC_ALL=C sort -z | xargs -0 md5sum | md5sum | cut -c1-16)

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
# The bundle shape and the helper digest are baked in: a missing/unreadable
# standalone marker or cleanup helper fails closed before any stage runs.
BUNDLE_LAYOUT="$BUNDLE_LAYOUT"
STOPS_WIFI="$STOPS_WIFI"
RUNTIME_CLEANUP_MD5="$RUNTIME_CLEANUP_MD5"
STANDALONE_MARKER_MD5="$STANDALONE_MD5"
STAGE_ORDER="$ORDER_SPACES"
#
# MODE (edit the line below; the md5 change makes the loader re-run it):
#   all       every selected package, in dependency-safe order (default)
#   restore   restore every selected package in reverse order
#   battery|wifi|tweaks   just that package
MODE="\${SCOURGE_MODE:-all}"

set -u
PATH="\${COMBO_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="\${USB_DIR:-\$(CDPATH= cd -- "\$(dirname "\$0")" && pwd)}"
STICK_DIR="\${SCOURGE_STICK_DIR:-\$USB_DIR}"
COMBO="\$USB_DIR/combo"
RW_DIR="\${COMBO_RW_DIR:-/rw_data/combo}"
RUNTIME_ROOT="\${COMBO_RUNTIME_ROOT:-/rw_data}"
QML_BASE="\${COMBO_QML_DIR:-/app/share/AppHomeScreen/qml_eco3.0}"
RUN_DIR="\${COMBO_RUN_DIR:-/run}"
WIFI1B_SERVICE="\${WIFI1B_SERVICE:-/etc/systemd/system/wifi1b-daemon.service}"
WIFI_SERVICE="\${WIFI_SERVICE:-/etc/systemd/system/wifi-daemon.service}"
MD5SUM_BIN=md5sum
command -v "\$MD5SUM_BIN" >/dev/null 2>&1 || MD5SUM_BIN=md5sum.coreutils
SHA256SUM_BIN=sha256sum
command -v "\$SHA256SUM_BIN" >/dev/null 2>&1 || SHA256SUM_BIN=sha256sum.coreutils
RUNTIME_CLEANUP_SHA256="\$SHA256SUM_BIN"

# Minimal inline path guard: the report path itself must never be a symlink,
# even for the helper-missing failure below (the full helper is not loaded yet).
_m_safe_path() {
    _mp=\$1
    case "\$_mp" in /*) : ;; *) return 1 ;; esac
    case "\$_mp" in *'/../'*|*/..|*'/./'*|*/.|*'//'*) return 1 ;; esac
    while [ -n "\$_mp" ] && [ "\$_mp" != "/" ]; do
        [ ! -L "\$_mp" ] || return 1
        _mp=\$(dirname "\$_mp")
    done
    _mp=\$(dirname "\$1")
    while [ -n "\$_mp" ] && [ "\$_mp" != "/" ]; do
        [ ! -e "\$_mp" ] || [ -d "\$_mp" ] || return 1
        _mp=\$(dirname "\$_mp")
    done
    return 0
}
_m_dir_path() {
    _m_safe_path "\$1" || return 1
    [ ! -e "\$1" ] || [ -d "\$1" ] || return 1
    return 0
}
_m_file_path() {
    _m_safe_path "\$1" || return 1
    if [ -e "\$1" ]; then
        [ -f "\$1" ] || return 1
        _mf_links=\$(stat -c %h "\$1" 2>/dev/null || true)
        case "\$_mf_links" in
            ''|*[!0-9]*) return 1 ;;
            1) : ;;
            *) return 1 ;;
        esac
    fi
    return 0
}
_m_usb_ok=1
{ _m_file_path "\$USB_DIR/run_stuff_result.txt"; } || _m_usb_ok=0
for _mp in "\$USB_DIR" "\$RW_DIR" "\$RW_DIR/bin" "\$RUN_DIR" \
           "\$RUN_DIR/wifi1b" "\$RUN_DIR/wifi" \
           "\${COMBO_LEDGER_DIR:-/rw_data/combo/installed}"; do
    _m_dir_path "\$_mp" || {
        if [ "\$_m_usb_ok" = 1 ]; then
            printf 'SCOURGE_COMBO\nstatus=FAIL\nerror=unsafe_symlink_destination\n' \
                > "\$USB_DIR/run_stuff_result.txt" 2>/dev/null || true
        else
            printf 'SCOURGE_COMBO\nstatus=FAIL\nerror=unsafe_symlink_destination\n' >&2
        fi
        exit 1; }
done
for _mp in "\$USB_DIR/run_stuff_result.txt" "\$RW_DIR/combo.log" \
           "\$RW_DIR/result.txt" "\$RW_DIR/stages.log" \
           "\$RW_DIR/bin/dbus-send" \
           "\$RUN_DIR/wifi1b/active" "\$RUN_DIR/wifi/active"; do
    _m_file_path "\$_mp" || {
        if [ "\$_m_usb_ok" = 1 ]; then
            printf 'SCOURGE_COMBO\nstatus=FAIL\nerror=unsafe_symlink_destination\n' \
                > "\$USB_DIR/run_stuff_result.txt" 2>/dev/null || true
        else
            printf 'SCOURGE_COMBO\nstatus=FAIL\nerror=unsafe_symlink_destination\n' >&2
        fi
        exit 1; }
done

# FIRST: verify and load the cleanup helper without writing anything, then
# reclaim bounded runtime log space before any other runtime write.  A full
# /rw_data makes every later write fail.  Stale temps are handled later, only
# after owned radios are verified stopped.
CLEANUP_LIB="\$COMBO/runtime_cleanup.sh"
if [ ! -f "\$CLEANUP_LIB" ] || [ -L "\$CLEANUP_LIB" ] || [ ! -r "\$CLEANUP_LIB" ]; then
    printf 'SCOURGE_COMBO\nstatus=FAIL\nerror=cleanup_helper_missing\n' \
        > "\$USB_DIR/run_stuff_result.txt" 2>/dev/null || true
    exit 1
fi
_cleanup_md5=\$("\$MD5SUM_BIN" "\$CLEANUP_LIB" 2>/dev/null | cut -d' ' -f1)
if [ -z "\$_cleanup_md5" ] || [ "\$_cleanup_md5" != "\$RUNTIME_CLEANUP_MD5" ]; then
    printf 'SCOURGE_COMBO\nstatus=FAIL\nerror=cleanup_helper_integrity\n' \
        > "\$USB_DIR/run_stuff_result.txt" 2>/dev/null || true
    exit 1
fi
. "\$CLEANUP_LIB"

# Refuse symlink ancestors for the master's own RW/output paths before the
# first runtime write or report.
_usb_report_ok=1
{ runtime_cleanup_safe_path "\$USB_DIR" &&
  runtime_cleanup_safe_path "\$USB_DIR/run_stuff_result.txt"; } || _usb_report_ok=0
for _dst in "\$USB_DIR" "\$RW_DIR" "\$RW_DIR/bin" "\$RUN_DIR" \
            "\$RUN_DIR/wifi1b" "\$RUN_DIR/wifi" \
            "\${COMBO_LEDGER_DIR:-/rw_data/combo/installed}"; do
    runtime_cleanup_safe_dir_path "\$_dst" || {
        if [ "\$_usb_report_ok" = 1 ]; then
            printf 'SCOURGE_COMBO\nstatus=FAIL\nerror=unsafe_symlink_destination\n' \
                > "\$USB_DIR/run_stuff_result.txt" 2>/dev/null || true
        else
            printf 'SCOURGE_COMBO\nstatus=FAIL\nerror=unsafe_symlink_destination\n' >&2
        fi
        exit 1; }
done
for _dst in "\$USB_DIR/run_stuff_result.txt" "\$RW_DIR/combo.log" \
            "\$RW_DIR/result.txt" "\$RW_DIR/stages.log" \
            "\$RW_DIR/bin/dbus-send" \
            "\$RUN_DIR/wifi1b/active" "\$RUN_DIR/wifi/active"; do
    runtime_cleanup_safe_file_path "\$_dst" || {
        if [ "\$_usb_report_ok" = 1 ]; then
            printf 'SCOURGE_COMBO\nstatus=FAIL\nerror=unsafe_symlink_destination\n' \
                > "\$USB_DIR/run_stuff_result.txt" 2>/dev/null || true
        else
            printf 'SCOURGE_COMBO\nstatus=FAIL\nerror=unsafe_symlink_destination\n' >&2
        fi
        exit 1; }
done
for _stage in \$STAGE_ORDER; do
    for _name in "\${_stage}_result.txt" "restore_\${_stage}_result.txt" \
                 "\${_stage}_install.log" "\${_stage}_restore.log" \
                 "\${_stage}_status.json" \
                 "runtime_snapshot_\${_stage}_status.json"; do
        runtime_cleanup_safe_file_path "\$USB_DIR/\$_name" || {
            if [ "\$_usb_report_ok" = 1 ]; then
                printf 'SCOURGE_COMBO\nstatus=FAIL\nerror=unsafe_symlink_destination\n' \
                    > "\$USB_DIR/run_stuff_result.txt" 2>/dev/null || true
            fi
            exit 1; }
    done
    runtime_cleanup_safe_file_path \
        "\${COMBO_LEDGER_DIR:-/rw_data/combo/installed}/\${_stage}.digest" || {
        if [ "\$_usb_report_ok" = 1 ]; then
            printf 'SCOURGE_COMBO\nstatus=FAIL\nerror=unsafe_symlink_destination\n' \
                > "\$USB_DIR/run_stuff_result.txt" 2>/dev/null || true
        fi
        exit 1; }
done
runtime_cleanup_safe_file_path \
    "\${COMBO_LEDGER_DIR:-/rw_data/combo/installed}/final-qml.digest" || {
    if [ "\$_usb_report_ok" = 1 ]; then
        printf 'SCOURGE_COMBO\nstatus=FAIL\nerror=unsafe_symlink_destination\n' \
            > "\$USB_DIR/run_stuff_result.txt" 2>/dev/null || true
    fi
    exit 1; }

RUNTIME_CLEANUP_LOG=""
[ "\$MODE" = restore ] || cleanup_bounded_logs "\$RUNTIME_ROOT" || true

mkdir -p "\$RW_DIR" 2>/dev/null || RW_DIR="\$USB_DIR"
LOG="\$RW_DIR/combo.log"
: > "\$LOG" 2>/dev/null
RUNTIME_CLEANUP_LOG="\$LOG"
[ -z "\$RUNTIME_CLEANUP_NOTES" ] || printf 'cleanup: %s\n' "\$RUNTIME_CLEANUP_NOTES" >> "\$LOG" 2>/dev/null
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

# --- unchanged-stage skip -----------------------------------------------
# A stage is skipped only when ALL hold:
#   1. the packaged digest (manifest + installer) equals the digest recorded
#      after that stage's last successful install;
#   2. every curated installed file in its skip.map still md5-matches the
#      packaged file (and required paths/services still exist);
#   3. the shared home-screen QML is byte-identical to the last successful
#      run's recorded output.
# The ledger lives in /rw_data, so a firmware update or a restore invalidates
# it. MODE=<package> always reinstalls; skipping applies to MODE=all only.
LEDGER="\${COMBO_LEDGER_DIR:-/rw_data/combo/installed}"
QML_BASE="\${COMBO_QML_DIR:-/app/share/AppHomeScreen/qml_eco3.0}"
QML_FILES="HomeScreen/QHMI_HomeScreenMain_Advanced.qml
HomeScreen/QHMI_HomeScreenMain_Wide.qml
HomeScreen/Screen_HomeScreen_AllMenus.qml
HomeScreen/Screen_HomeScreen_Submenu_Setup.qml
component/QCustomizedMenuListAllowEmpty.qml
component/common/Q_ThemeImage.qml"
md5f(){ "\$MD5SUM_BIN" "\$1" 2>/dev/null | cut -d' ' -f1; }
qml_digest(){
    for _q in \$QML_FILES; do
        [ -f "\$QML_BASE/\$_q" ] || return 1
        md5f "\$QML_BASE/\$_q"
    done | "\$MD5SUM_BIN" | cut -d' ' -f1
}
stage_digest(){
    (
        cat "\$COMBO/\$1/manifest.md5" "\$COMBO/\$1/stage.sh" \
            "\$COMBO/\$1/skip.map" "\$COMBO/\$1/skip.services" 2>/dev/null
        # Owner-supplied SSH keys / WiFi profiles are part of the installed
        # state: a changed key must re-run the stage, while an identical one
        # must still skip.
        case "\$1" in
        wifi|wifi1b)
            for _in in authorized_keys wifi.txt iphone_wifi.txt; do
                [ ! -f "\$USB_DIR/\$_in" ] || cat "\$USB_DIR/\$_in"
            done ;;
        esac
    ) | "\$MD5SUM_BIN" | cut -d' ' -f1
}
stage_owns_destination(){
    # True when another selected stage ships this exact destination.  A
    # "final stage owns it" entry in skip.map is then left to that stage.
    _own_from=\$1; _own_dst=\$2; _own_seen=0
    for _own_s in \$STAGE_ORDER; do
        if [ "\$_own_s" = "\$_own_from" ]; then _own_seen=1; continue; fi
        [ "\$_own_seen" = 1 ] || continue
        [ -f "\$COMBO/\$_own_s/skip.map" ] || continue
        while IFS= read -r _own_line; do
            [ -n "\$_own_line" ] || continue
            case "\$_own_line" in \#*|'!'*) continue ;; esac
            _own_line=\${_own_line#\?}
            _own_line=\${_own_line#\*}
            [ "\${_own_line#*|}" = "\$_own_dst" ] || continue
            [ -f "\$COMBO/\$_own_s/\${_own_line%%|*}" ] || continue
            return 0
        done < "\$COMBO/\$_own_s/skip.map"
    done
    return 1
}
stage_skippable(){
    _s=\$1; _d="\$COMBO/\$_s"
    [ -f "\$_d/skip.map" ] || return 1
    [ -f "\$LEDGER/\$_s.digest" ] || return 1
    _want=\$(cat "\$LEDGER/\$_s.digest" 2>/dev/null)
    [ -n "\$_want" ] && [ "\$_want" = "\$(stage_digest "\$_s")" ] || return 1
    _qrec=\$(cat "\$LEDGER/final-qml.digest" 2>/dev/null)
    _qnow=\$(qml_digest)
    [ -n "\$_qrec" ] && [ -n "\$_qnow" ] && [ "\$_qrec" = "\$_qnow" ] || return 1
    while IFS='|' read -r _left _dst; do
        [ -n "\$_left" ] || continue
        case "\$_left" in \#*) continue ;; esac
        case "\$_left" in
        \!*) [ -e "\${_left#!}" ] || return 1; continue ;;
        esac
        _rel=\$_left; _cond=0; _final=0
        case "\$_left" in
            \?*) _rel=\${_left#\?}; _cond=1 ;;
            \**) _rel=\${_left#\*}; _final=1 ;;
        esac
        # "*" = the final selected stage that ships this destination owns the
        # installed-state check.  Standalone builds have no WiFi stage, so the
        # Battery map must still verify its BatteryTile itself.
        [ "\$_final" = 0 ] || ! stage_owns_destination "\$_s" "\$_dst" || continue
        [ "\$_cond" = 0 ] || [ -d "\$(dirname "\$_dst")" ] || continue
        [ -f "\$_d/\$_rel" ] || continue
        [ -f "\$_dst" ] || return 1
        [ "\$(md5f "\$_dst")" = "\$(md5f "\$_d/\$_rel")" ] || return 1
    done < "\$_d/skip.map"
    if [ -f "\$_d/skip.services" ]; then
        while IFS= read -r _unit; do
            [ -n "\$_unit" ] || continue
            case "\$_unit" in \#*) continue ;; esac
            command -v systemctl >/dev/null 2>&1 || return 1
            systemctl is-active --quiet "\$_unit" 2>/dev/null || return 1
        done < "\$_d/skip.services"
    fi
    return 0
}
record_stage_digest(){
    mkdir -p "\$LEDGER" 2>/dev/null || return 0
    stage_digest "\$1" > "\$LEDGER/\$1.digest" 2>/dev/null || true
}

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
    # Unchanged stage: same packaged bytes as its last successful install and
    # every installed file/service/UI still matches. MODE=all only; an
    # explicit MODE=<package> (or restore) always runs the real installer.
    if [ "\$MODE" = all ] && stage_skippable "\$name"; then
        { echo "SCOURGE_COMBO_STAGE"; echo "stage=\$name"; echo "status=SKIPPED";
          echo "reason=unchanged_payload_and_installed_state"; } > "\$stage_result"
        cp -f "\$stage_result" "\$USB_DIR/\${name}_result.txt" 2>/dev/null || true
        echo "stage_\$name=status=SKIPPED" >> "\$STAGES_LOG"
        log "stage \$name skipped: payload and installed state unchanged"
        return 0
    fi
    # Per-package installers honor USB_DIR; point them at their own subtree.
    # Forward explicit owner inputs into the WiFi subtree. Do not package
    # credentials in source or leave stale copies after the stage finishes.
    case "\$name" in wifi|wifi1b)
        for input in wifi.txt iphone_wifi.txt authorized_keys MDNSD_ROOT_ARM; do
            [ ! -f "\$USB_DIR/\$input" ] || cp -f "\$USB_DIR/\$input" "\$stage_dir/\$input"
        done ;;
    esac
    # A WiFi-removal bundle rebuilds the shared QML in its migrate stage, so
    # Wifi1b must not fail on unknown current bytes or patch them first.
    _defer=""
    [ "\$name" = wifi1b ] && [ -f "\$COMBO/migrate/stage.sh" ] && _defer=1
    USB_DIR="\$stage_dir" SCOURGE_STICK_DIR="\$STICK_DIR" WIFI1B_DEFER_QML="\$_defer" \
        /bin/sh "\$script" >> "\$LOG" 2>&1
    stage_rc=\$?
    status=\$(sed -n 's/^status=\([^ ]*\).*/\1/p' "\$stage_result" 2>/dev/null | tail -n 1)
    [ ! -f "\$stage_result" ] || cp -f "\$stage_result" "\$USB_DIR/\${name}_result.txt"
    case "\$name" in wifi|wifi1b) rm -f "\$stage_dir/wifi.txt" "\$stage_dir/iphone_wifi.txt" "\$stage_dir/authorized_keys" "\$stage_dir/MDNSD_ROOT_ARM" ;; esac
    # A stage that exits 0 with status=REBOOT_REQUIRED succeeded: the WiFi
    # installer reports it when the UI restart could not be verified on the
    # spot.  Only rc=0 plus an accepted status counts; rc=0 with a failure
    # status is still a failure.
    case "\$stage_rc:\$status" in
        0:OK|0:REBOOT_REQUIRED)
            echo "stage_\$name=status=\$status rc=0" >> "\$STAGES_LOG"
            [ "\$MODE" = restore ] || record_stage_digest "\$name"
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

# Read-only preflight, before any stage creates backups or changes services.
if ! (cd "\$USB_DIR" && "\$MD5SUM_BIN" -c combo/preflight.md5) >> "\$LOG" 2>&1; then
    echo 'preflight=status=FAIL error=helper_manifest' >> "\$STAGES_LOG"
    result FAIL; beep_n 2; exit 1
fi
_layout_error=""
case "\$BUNDLE_LAYOUT" in
    standalone)
        if [ ! -f "\$COMBO/standalone" ] || [ -L "\$COMBO/standalone" ] || \
           [ ! -r "\$COMBO/standalone" ]; then
            _layout_error=standalone_marker_missing
        elif [ "\$("\$MD5SUM_BIN" "\$COMBO/standalone" 2>/dev/null | cut -d' ' -f1)" != \
               "\$STANDALONE_MARKER_MD5" ]; then
            _layout_error=standalone_marker_integrity
        elif ! grep -qx 'standalone=1' "\$COMBO/standalone" 2>/dev/null; then
            _layout_error=standalone_marker_content
        fi
        [ ! -d "\$COMBO/wifi" ] || _layout_error=standalone_has_wifi_stage
        [ ! -d "\$COMBO/migrate" ] || _layout_error=standalone_has_migrate_stage
        ;;
    combo) : ;;
    *) _layout_error=unknown_bundle_layout ;;
esac
if [ -n "\$_layout_error" ]; then
    echo "preflight=status=FAIL error=\$_layout_error" >> "\$STAGES_LOG"
    result FAIL; beep_n 2; exit 1
fi
. "\$COMBO/install_io.sh"
if ! io_init; then
    echo 'preflight=status=FAIL error=root_mount_detection' >> "\$STAGES_LOG"
    result FAIL; beep_n 2; exit 1
fi
echo 'preflight=status=OK' >> "\$STAGES_LOG"

# Stop owned radios ourselves; never ask the owner to turn WiFi off manually.
# Ownership requires byte-identical staged units.  SPDX text alone is not
# proof: an owner/foreign edit that keeps the header must not be stopped.
unit_owned(){
    _uo_file=\$1
    _uo_stage=\$2
    _uo_name=\${_uo_file##*/}
    _uo_expected="\$COMBO/\$_uo_stage/payload/systemd/\$_uo_name"
    [ -f "\$_uo_file" ] && [ ! -L "\$_uo_file" ] && [ -r "\$_uo_file" ] || return 1
    [ -f "\$_uo_expected" ] && [ ! -L "\$_uo_expected" ] && [ -r "\$_uo_expected" ] || return 1
    cmp -s "\$_uo_file" "\$_uo_expected"
}
if [ "\$MODE" != restore ]; then
    _stop_wifi=\$STOPS_WIFI
    _w1b_unit=0
    unit_owned "\$WIFI1B_SERVICE" wifi1b && _w1b_unit=1
    _wifi_unit=0
    if [ "\$_stop_wifi" = 1 ]; then
        unit_owned "\$WIFI_SERVICE" wifi && _wifi_unit=1
    fi
    if command -v systemctl >/dev/null 2>&1; then
        [ "\$_w1b_unit" = 0 ] || \
            systemctl stop wifi1b-daemon.service >> "\$LOG" 2>&1 || true
        [ "\$_wifi_unit" = 0 ] || \
            systemctl stop wifi-daemon.service >> "\$LOG" 2>&1 || true
    fi
    if [ "\$_w1b_unit" = 1 ] ||
       { [ ! -e "\$WIFI1B_SERVICE" ] && [ ! -L "\$WIFI1B_SERVICE" ]; }; then
        runtime_cleanup_kill_owned "\$RUNTIME_ROOT" wifi1b wifi1b_daemon || true
    else
        log "preflight: Wifi1b unit is not byte-identical to the staged unit; not stopping it"
    fi
    if [ "\$_stop_wifi" = 1 ]; then
        if [ "\$_wifi_unit" = 1 ] ||
           { [ ! -e "\$WIFI_SERVICE" ] && [ ! -L "\$WIFI_SERVICE" ]; }; then
            runtime_cleanup_kill_owned "\$RUNTIME_ROOT" wifi wifi_daemon || true
        else
            log "preflight: WiFi unit is not byte-identical to the staged unit; not stopping it"
        fi
    fi
    rm -f "\$RUN_DIR/wifi1b/active" 2>/dev/null || true
    [ "\$_stop_wifi" = 0 ] || rm -f "\$RUN_DIR/wifi/active" 2>/dev/null || true
    runtime_cleanup_process_alive "\$RUNTIME_ROOT" wifi1b wifi1b_daemon
    _radio_w1b=\$?
    if [ "\$_stop_wifi" = 1 ]; then
        runtime_cleanup_process_alive "\$RUNTIME_ROOT" wifi wifi_daemon
        _radio_wifi=\$?
    else
        _radio_wifi=1
    fi
    if [ "\$_radio_w1b" = 1 ] && [ "\$_radio_wifi" = 1 ]; then
        RUNTIME_CLEANUP_RADIOS_STOPPED=1
        RUNTIME_CLEANUP_SCAN_WIFI=\$_stop_wifi
        echo 'preflight=status=OK radio_stopped=1' >> "\$STAGES_LOG"
    else
        log "preflight: an owned radio is not verifiably stopped; stale temp cleanup deferred"
    fi
    cleanup_stale_temps "\$RUNTIME_ROOT" || true
    if [ "\$MODE" = all ]; then
        # Full removed-WiFi cleanup is evidence-gated inside the helper: a
        # corrupt/missing bundle, unreadable marker, page/service or live
        # process preserves credentials and backups and is reported instead.
        cleanup_removed_wifi "\$RUNTIME_ROOT" "\$QML_BASE" "\$STICK_DIR" \
            "\${SCOURGE_ARCHIVE:-}" "\${SCOURGE_TARBALL_SHA256:-}" \
            "\$WIFI_SERVICE" || true
    fi
    [ -z "\$RUNTIME_CLEANUP_NOTES" ] || log "cleanup: \$RUNTIME_CLEANUP_NOTES"
fi

case "\$MODE" in
  all)     for s in $ORDER_SPACES; do run_stage "\$s" "\$COMBO/\$s/stage.sh" || break; done ;;
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
if [ "$MODE" = restore ]; then
    # Remove only known product ledger digests after validating the whole
    # ledger.  Any unknown file/subdirectory/symlink/special entry preserves
    # every unknown entry and FAILs; never recursively delete the directory.
    _ledger_bad=""
    if [ -L "$LEDGER" ] || { [ -e "$LEDGER" ] && [ ! -d "$LEDGER" ]; }; then
        _ledger_bad=1
    elif [ -d "$LEDGER" ]; then
        [ -r "$LEDGER" ] && [ -w "$LEDGER" ] && [ -x "$LEDGER" ] || _ledger_bad=1
        for _le in "$LEDGER"/* "$LEDGER"/.[!.]* "$LEDGER"/..?*; do
            [ -e "$_le" ] || [ -L "$_le" ] || continue
            _lname=$(basename "$_le")
            case "$_lname" in
                final-qml.digest) : ;;
                *.digest)
                    _lstage=${_lname%.digest}
                    case " $STAGE_ORDER " in
                        *" $_lstage "*) : ;;
                        *) _ledger_bad=1; continue ;;
                    esac ;;
                *) _ledger_bad=1; continue ;;
            esac
            [ -f "$_le" ] && [ ! -L "$_le" ] && [ -r "$_le" ] && [ -w "$_le" ] || {
                _ledger_bad=1; continue; }
            _lval=$(cat "$_le" 2>/dev/null) || { _ledger_bad=1; continue; }
            case "$_lval" in
                ''|*[!0-9a-f]*) _ledger_bad=1 ;;
                *) [ "${#_lval}" -eq 32 ] || _ledger_bad=1 ;;
            esac
        done
    fi
    if [ -n "$_ledger_bad" ]; then
        FAILED="ledger_unknown_state"
    else
        for _stage in $STAGE_ORDER; do
            rm -f "$LEDGER/$_stage.digest" 2>/dev/null || FAILED="ledger_cleanup_failed"
        done
        rm -f "$LEDGER/final-qml.digest" 2>/dev/null || FAILED="ledger_cleanup_failed"
        if [ -d "$LEDGER" ]; then
            rmdir "$LEDGER" 2>/dev/null || FAILED="ledger_cleanup_failed"
        fi
    fi
elif [ "$MODE" = all ]; then
    # Only a full-stack run defines the shared QML the skip check compares.
    mkdir -p "$LEDGER" 2>/dev/null || true
    qml_now=$(qml_digest 2>/dev/null) || qml_now=""
    [ -z "$qml_now" ] || printf '%s\n' "$qml_now" > "$LEDGER/final-qml.digest" 2>/dev/null || true
fi
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
{
cat <<EOF
SCOURGE combo stick
===================
Packages: $ORDER
Bundle layout: $BUNDLE_LAYOUT

SCOURGE — The SCOURGE Project contributors (AGPL-3.0-or-later, with the
attribution terms in the project LICENSE). Firmware: Ioniq 5 Gen5W_L 260128.

1. Copy the CONTENTS of this directory to the root of a FAT/exFAT stick.
2. Plug it into the head unit; the failsafe runs run_stuff.sh as root
   within ~8 s. The preflight stops this bundle's own radio daemons itself;
   do not turn WiFi off manually.
3. Read run_stuff_result.txt on the stick; every stage reports status=OK.
4. To restore: edit MODE=restore in run_stuff.sh, replug, wait ~20 s.
5. To re-run a single package: edit MODE=<package>, replug.

EOF
if [ "$STANDALONE" = 1 ]; then
cat <<'EOF'
Standalone layout: this bundle installs Battery, Tweaks/Reboot and Wifi1b.
It does NOT install or reinstall the WiFi app and has NO migration stage.
Wifi1b installs beside an existing WiFi app, or on the no-WiFi
Battery/Tweaks base (the canonical standalone layout).
EOF
cat <<'EOF'
Optional inputs: iphone_wifi.txt (or wifi.txt) and authorized_keys at the
stick ROOT. They are imported without activating WLAN, a mode or SSH.
Wifi1b SSH: user wifi1b, UID 0, key-only, configured port (default 2224),
bind-all. This is a deliberate product default account and is NOT
car-validated. Owner keys already present in /rw_data/wifi1b are preserved
unless replaced here.

V24 BlueLink/Pi networking:
- While rmnet_usb0/rmnet_usb1 has an IPv4 address, Wifi1b installs no WiFi
  default route. WiFi remains an SSH-only link; the modem default and the
  vendor per-server host routes stay authoritative.
- Unverified legacy wlan0 defaults/records are preserved and route changes
  refused rather than treating a familiar format as ownership proof.
  A 30 s audit restores/removes the bounded proto-66 fallback as modem data
  appears/disappears. Routes on other interfaces are never touched.
- The Orange Pi ECM link is usb0 at 10.55.0.2/24 with no default route. SSH
  follows the configured port and stays available over usb0 when WiFi is
  switched Off. ConnMan is kept off usb0/rmnet_usb0..7.
EOF
else
cat <<'EOF'
WiFi note: put your wifi.txt and authorized_keys at the stick ROOT before
the first install; the WiFi installer imports them. Remove them afterwards.
Default SSH: car@<head-unit-ip> -p 2222, password 'electric' — CHANGE IT.
EOF
fi
cat <<'EOF'

WIP gate stages (if selected — button-gate / applauncher / dmclient-gate):
these are the signatureless-update research deployers (docs/08 of the
repo). They patch the updater's signature gates in place, need
AppUpgrade.patched.te2 next to them, require a REBOOT afterwards, and are
VM-validated but not yet car-validated. REVERT markers restore stock bytes.

This whole branch is work in progress: offline-tested, not car-validated.
Keep the stick private: it can contain owner credentials and private
WiFi/QML snapshots.
EOF
} > "$STAGE/README.txt"

echo "== deploying to $TARGET"
cp -R "$STAGE"/. "$TARGET"/
[ -z "${BATTERY_BUILD:-}" ] || rm -rf "$BATTERY_BUILD"
sync
echo "DONE. Stick contents:"
ls -la "$TARGET"
