# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Gen5W_L AU 260319 / BusyBox ash. Projection+ install transaction.
# Sourced by run_stuff.sh and run_stuff_restore.sh after packages/common/install_io.sh.
# Callers define log(), LOG and RW_DIR, and have run io_init.
#
# A backup directory ($RW_DIR/backup/<stamp>/) holds:
#   state       pending | complete | restored
#   index       one line per destination, written BEFORE that destination is
#               touched:  rel|existed|dest|sha256|SMACK64|SMACK64EXEC|payloads
#               (payloads = space-separated sha256 of every payload this
#               installer has written there, added before each write)
#   dirs        directories the installer created
#   files/<rel> byte copy (cp -p) of the destination as it was before install
# $RW_DIR/backup/latest is a symlink to the active backup. Because the index
# line exists before the write, restore is correct after an interruption at
# any point: entries with existed=1 are copied back, existed=0 are removed.

TXN_SHA=${SHA256SUM_BIN:-sha256sum}

txn_sha() { "$TXN_SHA" "$1" 2>/dev/null | cut -d' ' -f1; }

txn_state() { cat "$1/state" 2>/dev/null | head -n 1; }

txn_set_state() {
    printf '%s\n' "$2" > "$1/state.tmp" && mv -f "$1/state.tmp" "$1/state" && sync
}

# Print the index line for rel (empty when the backup does not cover it).
txn_lookup() {
    [ -f "$1/index" ] || return 0
    while IFS= read -r _line; do
        case "$_line" in "$2|"*) printf '%s\n' "$_line"; return 0 ;; esac
    done < "$1/index"
}

# Add payload sha $3 to rel $2's payload list (atomic index rewrite).
txn_add_payload() {
    _bk=$1; _rel=$2; _pay=$3
    while IFS= read -r _line; do
        case "$_line" in
            "$2|"*)
                case " ${_line##*|} " in
                    *" $_pay "*) : ;;
                    "  ") _line="$_line$_pay" ;;
                    *) _line="$_line $_pay" ;;
                esac ;;
        esac
        printf '%s\n' "$_line"
    done < "$_bk/index" > "$_bk/index.tmp" || return 1
    mv -f "$_bk/index.tmp" "$_bk/index" || return 1
    sync
}

# Back up dest (if present) into backup dir $1 as rel $2 and record it, with
# the payload sha $4 about to be written. Must be called before dest is
# modified. An already covered rel only gains $4 in its payload list.
txn_remember() {
    _bk=$1; _rel=$2; _dest=$3; _pay=$4
    if [ -n "$(txn_lookup "$_bk" "$_rel")" ]; then
        txn_add_payload "$_bk" "$_rel" "$_pay"
        return
    fi
    _existed=0; _sha=""; _l64=""; _lexec=""
    if [ -e "$_dest" ] || [ -L "$_dest" ]; then
        mkdir -p "$_bk/files/$(dirname "$_rel")" || return 1
        cp -pP "$_dest" "$_bk/files/$_rel" 2>>"$LOG" || return 1
        _sha=$(txn_sha "$_bk/files/$_rel")
        [ -n "$_sha" ] && [ "$_sha" = "$(txn_sha "$_dest")" ] || return 1
        _l64=$(getfattr -n security.SMACK64 --only-values "$_dest" 2>/dev/null | tr -d '|\r\n' || true)
        _lexec=$(getfattr -n security.SMACK64EXEC --only-values "$_dest" 2>/dev/null | tr -d '|\r\n' || true)
        _existed=1
    fi
    sync
    printf '%s|%s|%s|%s|%s|%s|%s\n' "$_rel" "$_existed" "$_dest" "$_sha" "$_l64" "$_lexec" "$_pay" >> "$_bk/index" || return 1
    sync
    log "backup: $_rel existed=$_existed sha=${_sha:-none}"
}

# Create directory $2 (and parents), recording each one that did not exist in
# $1/dirs first, so restore can remove exactly the directories we added.
txn_mkdir() {
    _bk=$1; _d=$2; _todo=""
    while [ -n "$_d" ] && [ "$_d" != "/" ] && [ "$_d" != "." ] && [ ! -d "$_d" ]; do
        _todo="$_d
$_todo"
        _d=$(dirname "$_d")
    done
    printf '%s' "$_todo" | while IFS= read -r _d; do
        [ -n "$_d" ] || continue
        grep -qxF "$_d" "$_bk/dirs" 2>/dev/null || printf '%s\n' "$_d" >> "$_bk/dirs" || exit 1
    done || return 1
    sync
    io_mkdir "$2"
}

# List entries of backup dir $1 whose destination is in a state this
# installer did not produce (neither the original, nor absent, nor one of the
# recorded payloads): something else changed it after install. Prints one
# line per conflict; returns 1 when there is any.
txn_conflicts() {
    _bk=$1; _rc=0
    [ -f "$_bk/index" ] || return 0
    while IFS='|' read -r _rel _existed _dest _sha _l64 _lexec _pays; do
        [ -n "$_dest" ] || continue
        [ -e "$_dest" ] && [ ! -L "$_dest" ] || continue
        [ -n "$_pays" ] || continue
        _cur=$(txn_sha "$_dest")
        [ "$_existed" = 1 ] && [ "$_cur" = "$_sha" ] && continue
        case " $_pays " in *" $_cur "*) continue ;; esac
        echo "$_dest (sha256 $_cur)"
        _rc=1
    done < "$_bk/index"
    return $_rc
}

# Put every destination in backup dir $1 back to its recorded pre-install
# state, newest entry first. Continues past individual failures; returns 1
# if any entry could not be restored byte-for-byte. Callers gate on
# txn_conflicts first (the installer's own rollback only touches its writes).
txn_restore() {
    _bk=$1
    _rc=0
    [ -f "$_bk/index" ] || return 0
    sed -n '1!G;h;$p' "$_bk/index" > "$_bk/index.rev" || return 1
    while IFS='|' read -r _rel _existed _dest _sha _l64 _lexec _pays; do
        [ -n "$_dest" ] || continue
        if [ "$_existed" = 1 ]; then
            if [ ! -L "$_dest" ] && [ -e "$_dest" ] && [ "$(txn_sha "$_dest")" = "$_sha" ]; then
                log "already original: $_rel"
                continue
            fi
            if ! io_restore_entry "$_bk/files/$_rel" "$_dest"; then
                log "RESTORE FAILED: $_dest (backup kept at $_bk/files/$_rel)"
                _rc=1; continue
            fi
            if [ ! -L "$_dest" ] && [ "$(txn_sha "$_dest")" != "$_sha" ]; then
                log "RESTORE FAILED: $_dest sha mismatch after copy"
                _rc=1; continue
            fi
            [ -z "$_l64" ] || setfattr -n security.SMACK64 -v "$_l64" "$_dest" 2>/dev/null || true
            [ -z "$_lexec" ] || setfattr -n security.SMACK64EXEC -v "$_lexec" "$_dest" 2>/dev/null || true
            log "restored: $_rel"
        else
            if [ -e "$_dest" ] || [ -L "$_dest" ]; then
                io_remove "$_dest" || { _rc=1; continue; }
            fi
            log "removed: $_rel"
        fi
    done < "$_bk/index.rev"
    rm -f "$_bk/index.rev"
    # Directories we created, deepest first; only removed when empty.
    if [ -f "$_bk/dirs" ]; then
        sed -n '1!G;h;$p' "$_bk/dirs" | while IFS= read -r _d; do
            [ -d "$_d" ] || continue
            io_ensure_rw >/dev/null 2>&1 || true
            rmdir "$_d" 2>/dev/null && log "removed dir: $_d" || log "kept non-empty dir: $_d"
        done
    fi
    sync
    return $_rc
}

# AppHomeScreen restart through AppLauncher (app id 11), exactly as Tweaks/Wi-Fi.
# Sets $restart. SIGTERM fallback only on the car (R empty), never kill -9.
restart_homescreen() {
    restart="not-attempted"
    # Car 2026-11-27: restarting AppHomeScreen live left CarPlay unable to start until a head-unit
    # reboot. Default: do NOT restart it; the operator reboots the head unit (the new home screen
    # loads at boot). PROJECTION_RESTART_UI=1 (or run_stuff.sh --restart-ui) keeps the old behaviour.
    if [ "${PROJECTION_RESTART_UI:-0}" != 1 ]; then
        restart="reboot-required"
        return 0
    fi
    launcher_pid=""
    for p in /proc/[0-9]*; do
        [ -r "$p/comm" ] || continue
        if [ "$(cat "$p/comm" 2>/dev/null)" = "AppLauncher" ]; then
            launcher_pid="${p#/proc/}"
            break
        fi
    done
    if [ -n "$launcher_pid" ] && [ -r "/proc/$launcher_pid/environ" ]; then
        for var in DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR WAYLAND_DISPLAY QT_QPA_PLATFORM; do
            val=$(tr '\0' '\n' < "/proc/$launcher_pid/environ" 2>/dev/null | grep "^$var=" | head -n 1 | cut -d= -f2-)
            [ -n "$val" ] && export "$var=$val"
        done
    fi
    if command -v dbus-send >/dev/null 2>&1; then
        dbus-send --session --type=method_call --dest=com.lge.avn.applauncher /Request \
            com.lge.avn.applauncher.request.terminate int32:11 >> "$LOG" 2>&1 || true
        sleep "${PROJECTION_RESTART_SLEEP:-2}"
        restart="dbus-launch-failed"
        for i in 1 2 3; do
            if dbus-send --session --type=method_call --dest=com.lge.avn.applauncher /Request \
                com.lge.avn.applauncher.request.launch int32:11 \
                string:AppHomeScreen string:/app/bin/AppHomeScreen >> "$LOG" 2>&1; then
                restart="dbus-ok"
                break
            fi
            sleep "${PROJECTION_RESTART_SLEEP:-2}"
        done
    else
        restart="dbus-send-missing"
    fi
    if [ "$restart" != "dbus-ok" ] && [ -z "$R" ]; then
        for p in /proc/[0-9]*; do
            [ -r "$p/comm" ] || continue
            [ "$(cat "$p/comm" 2>/dev/null)" = "AppHomeScreen" ] && kill -TERM "${p#/proc/}" 2>/dev/null || true
        done
        restart="$restart (reboot required if UI does not return)"
    fi
}
