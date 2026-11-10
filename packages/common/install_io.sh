# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Gen5W_L 260128 / BusyBox ash. Sourced only after manifest verification.
# Call io_init before writes, io_restore_root from the installer's EXIT trap.
# Helpers use subshells so their variables cannot clobber callers' src/dst/f.

io_root_options() {
    awk -v root="$IO_ROOT" '$2 == root { print $4; exit }' "$IO_MOUNTS"
}

io_init() {
    IO_ROOT=${INSTALL_IO_ROOT:-/}
    IO_MOUNTS=${INSTALL_IO_MOUNTS:-/proc/mounts}
    IO_ORIGINAL_OPTIONS=$(io_root_options)
    [ -n "$IO_ORIGINAL_OPTIONS" ] || { log "ERROR: root mount not found: $IO_ROOT"; return 1; }
    IO_ACTIVE=1
    ASSET_WARNINGS=0
    log "root mount before install: $IO_ORIGINAL_OPTIONS"
}

io_ensure_rw() {
    case ",$(io_root_options)," in
        *,rw,*) return 0 ;;
    esac
    log "root is not writable; remounting $IO_ROOT rw"
    mount -o remount,rw "$IO_ROOT" >> "$LOG" 2>&1 || return 1
    case ",$(io_root_options)," in *,rw,*) return 0 ;; esac
    log "ERROR: root still not rw after remount"
    return 1
}

io_settle_root() (
    # Two successive writable probes reduce the boot-time remount race.
    # They cannot prevent a later remount: every copy also retries/verifies.
    tries=0
    stable=0
    probe=""
    trap '[ -z "$probe" ] || { io_ensure_rw && rm -f "$probe" 2>/dev/null; } || true' EXIT
    while [ "$tries" -lt 30 ]; do
        tries=$((tries + 1))
        if io_ensure_rw; then
            if [ -n "$probe" ]; then
                rm -f "$probe" 2>>"$LOG" || { stable=0; sleep 1; continue; }
            fi
            probe=$(mktemp "$IO_ROOT/.scourge-write.XXXXXX" 2>>"$LOG") || probe=""
            if [ -n "$probe" ] && printf 'probe\n' > "$probe" 2>>"$LOG"; then
                stable=$((stable + 1))
                rm -f "$probe" 2>>"$LOG" || { stable=0; sleep 1; continue; }
                probe=""
                if [ "$stable" -ge 2 ]; then
                    log "root writable and settled (attempt=$tries)"
                    return 0
                fi
            else
                stable=0
            fi
        else
            stable=0
        fi
        sleep 1
    done
    log "ERROR: root did not settle writable within 30 attempts"
    return 1
)

io_restore_root() {
    [ "${IO_ACTIVE:-0}" = 1 ] || return 0
    case ",$IO_ORIGINAL_OPTIONS," in
        *,ro,*) mount -o remount,ro "$IO_ROOT" >> "$LOG" 2>&1 || {
            log "WARNING: could not restore original read-only root state"; return 1;
        } ;;
    esac
}

io_mkdir() (
    for attempt in 1 2 3; do
        io_ensure_rw && mkdir -p "$1" >> "$LOG" 2>&1 && return 0
        log "mkdir retry=$attempt path=$1 root=$(io_root_options)"
        sleep 1
    done
    return 1
)

io_copy() (
    source=$1
    target=$2
    [ ! -d "$target" ] || target="$target/$(basename "$source")"
    # The firmware may ship md5sum only as md5sum.coreutils; installers that
    # resolved MD5SUM_BIN for their manifest check must get the same binary
    # here or every copy fails on such a target.
    md5=${MD5SUM_BIN:-md5sum}
    expected=$($md5 "$source" 2>>"$LOG" | cut -d' ' -f1)
    [ -n "$expected" ] || return 1
    for attempt in 1 2 3; do
        actual=$($md5 "$target" 2>/dev/null | cut -d' ' -f1)
        [ "$actual" != "$expected" ] || return 0
        # Never explicitly unlink the destination: keeping its inode keeps
        # its SMACK attributes (cp -f may still replace a destination it
        # cannot open).  On EROFS retry after a remount, not after deletion.
        if io_ensure_rw && cp -f "$source" "$target" >> "$LOG" 2>&1; then
            actual=$($md5 "$target" 2>/dev/null | cut -d' ' -f1)
            [ "$actual" != "$expected" ] || return 0
        fi
        log "copy retry=$attempt src=$source dst=$target root=$(io_root_options)"
        sleep 1
    done
    ls -ld "$source" "$target" "$(dirname "$target")" >> "$LOG" 2>&1 || true
    log "ERROR: verified copy failed: $source -> $target"
    return 1
)

io_asset() {
    if ! io_mkdir "$(dirname "$2")" || ! io_copy "$1" "$2"; then
        log "WARNING: optional icon not installed: $1 -> $2"
        ASSET_WARNINGS=$((ASSET_WARNINGS + 1))
    fi
    return 0
}

io_remove() (
    for attempt in 1 2 3; do
        io_ensure_rw && rm -f "$1" >> "$LOG" 2>&1 && return 0
        sleep 1
    done
    log "ERROR: remove failed: $1"
    return 1
)

io_append_line() (
    temporary=$(mktemp "$RW_DIR/.append.XXXXXX") || return 1
    trap 'rm -f "$temporary" 2>/dev/null || true' EXIT
    cat "$1" > "$temporary" || return 1
    [ -z "$(tail -c1 "$temporary")" ] || printf '\n' >> "$temporary"
    printf '%s\n' "$2" >> "$temporary" || return 1
    io_copy "$temporary" "$1"
)

io_restore_entry() (
    source=$1
    target=$2
    if [ -L "$source" ]; then
        link=$(readlink "$source") || return 1
        [ "$(readlink "$target" 2>/dev/null)" != "$link" ] || return 0
        temporary=""
        trap '[ -z "$temporary" ] || rm -f "$temporary" 2>/dev/null || true' EXIT
        for attempt in 1 2 3; do
            if io_ensure_rw; then
                temporary=$(mktemp "$(dirname "$target")/.scourge-link.XXXXXX") || return 1
                rm -f "$temporary" || return 1
                if cp -pP "$source" "$temporary" >> "$LOG" 2>&1 &&
                   mv -f "$temporary" "$target" >> "$LOG" 2>&1 &&
                   [ "$(readlink "$target")" = "$link" ]; then
                    temporary=""
                    return 0
                fi
            fi
            sleep 1
        done
        return 1
    fi
    io_copy "$source" "$target" || return 1
    mode=$(stat -c%a "$source") || return 1
    owner=$(stat -c%u:%g "$source") || return 1
    chmod "$mode" "$target" >> "$LOG" 2>&1 || return 1
    chown "$owner" "$target" >> "$LOG" 2>&1 || return 1
)
