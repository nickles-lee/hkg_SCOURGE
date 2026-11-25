#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Gen5W_L EU 260128
# Bounded, ownership-aware runtime cleanup shared by the flat wrapper, the
# composed master and the Wifi1b payload.  Read this as data: no function here
# follows a symlink, scans a foreign path, or deletes a directory on absence
# evidence alone.
#
# Removal of a removed WiFi app's /rw_data/wifi (credentials, keys, backups)
# additionally requires a verified private archive of that state, or a receipt
# this helper wrote after an earlier verified removal.  A corrupt or missing
# bundle therefore preserves credentials and backups.
#
# Callers set RUNTIME_CLEANUP_LOG or leave it empty and report
# RUNTIME_CLEANUP_NOTES after the first runtime write is allowed.

RUNTIME_CLEANUP_NOTES="${RUNTIME_CLEANUP_NOTES:-}"
RUNTIME_CLEANUP_SHA256="${RUNTIME_CLEANUP_SHA256:-sha256sum}"

runtime_cleanup_note() {
    case "$RUNTIME_CLEANUP_NOTES" in
        "") RUNTIME_CLEANUP_NOTES=$1 ;;
        *) RUNTIME_CLEANUP_NOTES="$RUNTIME_CLEANUP_NOTES; $1" ;;
    esac
    if [ -n "${RUNTIME_CLEANUP_LOG:-}" ] && [ -f "$RUNTIME_CLEANUP_LOG" ] &&
       [ ! -L "$RUNTIME_CLEANUP_LOG" ]; then
        printf 'cleanup: %s\n' "$1" >> "$RUNTIME_CLEANUP_LOG" 2>/dev/null || true
    fi
}

# Refuse relative, dot-containing and symlinked paths.  Every ancestor of the
# leaf is checked, so an attacker-owned symlink cannot redirect a write.
runtime_cleanup_safe_path() {
    _rc_path=$1
    case "$_rc_path" in
        /*) : ;;
        *) return 1 ;;
    esac
    case "$_rc_path" in
        *'/../'*|*/..|*'/./'*|*/.|*'//'*) return 1 ;;
    esac
    _rc_walk=$_rc_path
    while [ -n "$_rc_walk" ] && [ "$_rc_walk" != "/" ]; do
        [ ! -L "$_rc_walk" ] || return 1
        _rc_walk=$(dirname "$_rc_walk")
    done
    _rc_walk=$(dirname "$_rc_path")
    while [ -n "$_rc_walk" ] && [ "$_rc_walk" != "/" ]; do
        [ ! -e "$_rc_walk" ] || [ -d "$_rc_walk" ] || return 1
        _rc_walk=$(dirname "$_rc_walk")
    done
    return 0
}

# Directory-or-absent parent predicate: symlinks, regular files, FIFOs,
# sockets and devices at a directory position are all refused.
runtime_cleanup_safe_dir_path() {
    runtime_cleanup_safe_path "$1" || return 1
    [ ! -e "$1" ] || [ -d "$1" ] || return 1
    return 0
}

# Read-only regular-source predicate.  A source may legitimately be a
# hardlink; it is never opened here, so FIFOs cannot block the check.
runtime_cleanup_safe_regular_path() {
    runtime_cleanup_safe_path "$1" || return 1
    [ -f "$1" ] || return 1
    return 0
}

# Write-destination file predicate: regular file or absent.  Directories,
# symlinks, FIFOs, sockets, devices and multi-link aliases are refused because
# an in-place product write could otherwise alter another owner path.
runtime_cleanup_safe_file_path() {
    runtime_cleanup_safe_path "$1" || return 1
    if [ -e "$1" ]; then
        [ -f "$1" ] || return 1
        _rc_links=$(stat -c %h "$1" 2>/dev/null || true)
        case "$_rc_links" in
            ''|*[!0-9]*) return 1 ;;
            1) : ;;
            *) return 1 ;;
        esac
    fi
    return 0
}

# Only these product-owned files may have stale atomic-write temporaries.
runtime_cleanup_owned_name() {
    case "$1" in
        status.json|config.conf|wifi.txt|home.json|bt.state|apply.result|scan.json|ssh.bound|\
        wpa_supplicant.conf|wpa_supplicant.pid|dhcp.leases|dhcpd.pid|udhcpc.pid|udhcpc.sh|\
        udhcpc.log|authorized_keys|dropbear.pid|dropbear_ed25519_host_key|\
        dropbear_rsa_host_key|migration.done) return 0 ;;
    esac
    return 1
}

# 0 = exact owned executable would be alive, 1 = confirmed absent,
# 2 = process table unreadable (callers must fail closed, never treat as dead).
runtime_cleanup_process_alive() {
    _rc_root=$1
    _rc_app=$2
    _rc_exe=$3
    _rc_proc=${CLEANUP_PROC_ROOT:-/proc}
    if [ ! -d "$_rc_proc" ] || [ ! -r "$_rc_proc" ]; then
        return 2
    fi
    _rc_scan=0
    for _rc_p in "$_rc_proc"/[0-9]*; do
        [ -d "$_rc_p" ] || continue
        _rc_scan=1
        _rc_link=$(readlink "$_rc_p/exe" 2>/dev/null)
        _rc_readlink=$?
        if [ "$_rc_readlink" -ne 0 ]; then
            # A PID that vanished during the scan is legitimately absent.
            if [ ! -d "$_rc_p" ]; then continue; fi
            # An extant PID whose exe cannot be read is unproven evidence.
            if [ ! -r "$_rc_p" ] || [ ! -x "$_rc_p" ]; then return 2; fi
            if [ -L "$_rc_p/exe" ]; then return 2; fi
            # Kernel threads/zombies have no executable link; not our daemon.
            continue
        fi
        [ -n "$_rc_link" ] || continue
        case "$_rc_link" in
            "$_rc_root/$_rc_app/$_rc_exe"|"$_rc_root/$_rc_app/$_rc_exe (deleted)") return 0 ;;
        esac
    done
    [ "$_rc_scan" = 1 ] || return 2
    return 1
}

# Kill only exact owned executables: TERM first, then KILL.  Used by the
# installer preflight so no manual WiFi Off step is needed.
runtime_cleanup_kill_owned() {
    _rc_root=$1
    _rc_app=$2
    _rc_exe=$3
    _rc_proc=${CLEANUP_PROC_ROOT:-/proc}
    if [ ! -d "$_rc_proc" ] || [ ! -r "$_rc_proc" ]; then
        return 1
    fi
    for _rc_sig in TERM KILL; do
        _rc_found=0
        for _rc_p in "$_rc_proc"/[0-9]*; do
            [ -d "$_rc_p" ] || continue
            _rc_link=$(readlink "$_rc_p/exe" 2>/dev/null)
            _rc_readlink=$?
            if [ "$_rc_readlink" -ne 0 ]; then
                if [ ! -d "$_rc_p" ]; then continue; fi
                if [ ! -r "$_rc_p" ] || [ ! -x "$_rc_p" ] || [ -L "$_rc_p/exe" ]; then
                    return 1
                fi
                continue
            fi
            [ -n "$_rc_link" ] || continue
            case "$_rc_link" in
                "$_rc_root/$_rc_app/$_rc_exe"|"$_rc_root/$_rc_app/$_rc_exe (deleted)")
                    kill -"$_rc_sig" "${_rc_p##*/}" 2>/dev/null || true
                    _rc_found=1 ;;
            esac
        done
        [ "$_rc_found" = 1 ] || return 0
        sleep 1 2>/dev/null || sleep 1
    done
    return 1
}

# Fixed, append-only daemon logs only.  This is deliberately the first runtime
# write: a full /rw_data otherwise breaks the installer's own state.  Unknown
# files, symlinks and non-regular files are left alone.
cleanup_bounded_logs() {
    _rc_root=$1
    if ! runtime_cleanup_safe_path "$_rc_root" || [ "$_rc_root" = "/" ]; then
        runtime_cleanup_note "refused unsafe runtime root: $_rc_root"
        return 1
    fi
    for _rc_app in wifi wifi1b battery tweaks; do
        _rc_dir="$_rc_root/$_rc_app"
        if [ -L "$_rc_dir" ]; then
            runtime_cleanup_note "refused symlink runtime dir: $_rc_dir"
            continue
        fi
        [ -d "$_rc_dir" ] || continue
        for _rc_name in daemon.log daemon.log.1 wpa_supplicant.log \
                        wpa_supplicant.log.1 ssh.log ssh.log.1 dhcpd.log \
                        dhcpd.log.1 tweaks_failures.log; do
            _rc_file="$_rc_dir/$_rc_name"
            if [ -L "$_rc_file" ]; then
                runtime_cleanup_note "refused symlink log: $_rc_file"
                continue
            fi
            [ -f "$_rc_file" ] || continue
            : > "$_rc_file" 2>/dev/null || \
                runtime_cleanup_note "cannot truncate log: $_rc_file"
        done
    done
    return 0
}

# Delete only <owned-name>.tmp.<pid> where <pid> is numeric, no longer exists,
# and the in-scope owned radios are verified stopped.  WiFi temps are scanned
# only when RUNTIME_CLEANUP_SCAN_WIFI=1 and both radio owners are dead.
cleanup_stale_temps() {
    _rc_root=$1
    if [ "${RUNTIME_CLEANUP_RADIOS_STOPPED:-0}" != 1 ]; then
        runtime_cleanup_note "deferred stale temp cleanup: radios not verified stopped"
        return 1
    fi
    if ! runtime_cleanup_safe_path "$_rc_root"; then
        runtime_cleanup_note "refused unsafe runtime root: $_rc_root"
        return 1
    fi
    runtime_cleanup_process_alive "$_rc_root" wifi1b wifi1b_daemon
    _rc_w1b=$?
    if [ "$_rc_w1b" != 1 ]; then
        runtime_cleanup_note "deferred stale temp cleanup: wifi1b process not verifiably stopped"
        return 1
    fi
    _rc_scan_wifi=${RUNTIME_CLEANUP_SCAN_WIFI:-0}
    if [ "$_rc_scan_wifi" = 1 ]; then
        runtime_cleanup_process_alive "$_rc_root" wifi wifi_daemon
        _rc_wf=$?
        if [ "$_rc_wf" != 1 ]; then
            runtime_cleanup_note "deferred stale temp cleanup: WiFi process not verifiably stopped"
            return 1
        fi
    fi
    _rc_proc=${CLEANUP_PROC_ROOT:-/proc}
    for _rc_dir in "$_rc_root/wifi1b" \
                    "$_rc_root/wifi1b/ssh" "$_rc_root/wifi1b/home/.ssh" \
                    "$_rc_root/battery" "$_rc_root/tweaks"; do
        if ! runtime_cleanup_safe_path "$_rc_dir"; then
            runtime_cleanup_note "refused unsafe temp dir: $_rc_dir"
            continue
        fi
        if [ -L "$_rc_dir" ]; then
            runtime_cleanup_note "refused symlink temp dir: $_rc_dir"
            continue
        fi
        [ -d "$_rc_dir" ] || continue
        for _rc_file in "$_rc_dir"/*.tmp.*; do
            [ -e "$_rc_file" ] || [ -L "$_rc_file" ] || continue
            if [ -L "$_rc_file" ]; then
                runtime_cleanup_note "refused symlink temp: $_rc_file"
                continue
            fi
            if [ ! -f "$_rc_file" ]; then
                runtime_cleanup_note "kept unproven temp: $_rc_file"
                continue
            fi
            _rc_base=${_rc_file##*/}
            _rc_pid=${_rc_base##*.}
            case "$_rc_pid" in
                ''|*[!0-9]*)
                    runtime_cleanup_note "kept unproven temp: $_rc_file"
                    continue ;;
            esac
            _rc_owned=${_rc_base%.tmp."$_rc_pid"}
            if ! runtime_cleanup_owned_name "$_rc_owned"; then
                runtime_cleanup_note "kept unknown temp: $_rc_file"
                continue
            fi
            [ "$_rc_pid" -gt 1 ] 2>/dev/null || {
                runtime_cleanup_note "kept invalid-pid temp: $_rc_file"
                continue
            }
            if [ -d "$_rc_proc/$_rc_pid" ]; then
                runtime_cleanup_note "kept live-pid temp: $_rc_file"
                continue
            fi
            if rm -f "$_rc_file" 2>/dev/null &&
               [ ! -e "$_rc_file" ] && [ ! -L "$_rc_file" ]; then
                runtime_cleanup_note "removed stale owned temp: $_rc_file"
            else
                runtime_cleanup_note "cannot remove stale temp: $_rc_file"
            fi
        done
    done
    if [ "$_rc_scan_wifi" = 1 ]; then
        if runtime_cleanup_safe_path "$_rc_root/wifi" &&
           [ ! -L "$_rc_root/wifi" ] && [ -d "$_rc_root/wifi" ]; then
            for _rc_file in "$_rc_root/wifi"/*.tmp.*; do
                [ -e "$_rc_file" ] || [ -L "$_rc_file" ] || continue
                if [ -L "$_rc_file" ]; then
                    runtime_cleanup_note "refused symlink temp: $_rc_file"
                    continue
                fi
                [ -f "$_rc_file" ] || { runtime_cleanup_note "kept unproven temp: $_rc_file"; continue; }
                _rc_base=${_rc_file##*/}
                _rc_pid=${_rc_base##*.}
                case "$_rc_pid" in
                    ''|*[!0-9]*) runtime_cleanup_note "kept unproven temp: $_rc_file"; continue ;;
                esac
                _rc_owned=${_rc_base%.tmp."$_rc_pid"}
                runtime_cleanup_owned_name "$_rc_owned" || {
                    runtime_cleanup_note "kept unknown temp: $_rc_file"; continue; }
                [ "$_rc_pid" -gt 1 ] 2>/dev/null || {
                    runtime_cleanup_note "kept invalid-pid temp: $_rc_file"; continue; }
                if [ -d "$_rc_proc/$_rc_pid" ]; then
                    runtime_cleanup_note "kept live-pid temp: $_rc_file"
                    continue
                fi
                if rm -f "$_rc_file" 2>/dev/null &&
                   [ ! -e "$_rc_file" ] && [ ! -L "$_rc_file" ]; then
                    runtime_cleanup_note "removed stale owned temp: $_rc_file"
                else
                    runtime_cleanup_note "cannot remove stale temp: $_rc_file"
                fi
            done
        fi
    fi
    return 0
}

# 0 = readable and marker absent, 1 = marker present, 2 = unreadable/unsafe.
runtime_cleanup_marker_state() {
    _rc_file=$1
    [ -f "$_rc_file" ] && [ ! -L "$_rc_file" ] && [ -r "$_rc_file" ] || return 2
    grep -q 'WIFI_PAYLOAD_260128' "$_rc_file" 2>/dev/null
    case $? in
        0) return 1 ;;
        1) return 0 ;;
        *) return 2 ;;
    esac
}

# Abort a fresh WiFi archive attempt without leaving a partial private file.
runtime_cleanup_archive_abort() {
    [ -z "${RUNTIME_CLEANUP_TMP_ARCHIVE:-}" ] ||
        rm -f "$RUNTIME_CLEANUP_TMP_ARCHIVE" 2>/dev/null || true
    RUNTIME_CLEANUP_TMP_ARCHIVE=""
    runtime_cleanup_note "$1"
    return 1
}

# chmod a private file/dir and verify the exact mode.  Failure is never ignored:
# an archive that is not private must not authorise credential deletion.
runtime_cleanup_private_mode() {
    _rpm_path=$1
    _rpm_want=$2
    [ -n "$_rpm_path" ] && [ -e "$_rpm_path" ] && [ ! -L "$_rpm_path" ] || return 1
    chmod "$_rpm_want" "$_rpm_path" 2>/dev/null || return 1
    _rpm_got=$(stat -c %a "$_rpm_path" 2>/dev/null) || return 1
    [ "$_rpm_got" = "$_rpm_want" ]
}

# Return '-' for an archived regular file, 'd' for a directory, and fail for
# links, devices, hardlinks, unsafe modes or unreadable archives.  The tar
# command's exit status is checked independently of any output processing.
runtime_cleanup_archive_member_type() {
    _ram_archive=$1
    _ram_member=$2
    _ram_out=$(tar -tvzf "$_ram_archive" "$_ram_member" 2>/dev/null) || return 1
    [ -n "$_ram_out" ] || return 1
    _ram_line=$(printf '%s\n' "$_ram_out" | sed -n '1p')
    _ram_modes=$(printf '%s\n' "$_ram_line" | cut -c1-10)
    case "$_ram_modes" in
        ?????w*|????????w*|???[sS]*|??????[sS]*|?????????[tT]*) return 1 ;;
    esac
    case "$_ram_modes" in
        -?*) printf '%s\n' '-' ;;
        d?*) printf '%s\n' 'd' ;;
        *) return 1 ;;
    esac
}

# Extract one archived member to a private temporary file, checking tar's exit
# status before hashing.  This cannot let a failed/partial extraction hash as
# an empty original file.
runtime_cleanup_extract_hash() {
    _reh_archive=$1
    _reh_member=$2
    _reh_dir=${3:-${RUNTIME_CLEANUP_TMP_DIR:-}}
    [ -n "$_reh_dir" ] && [ -d "$_reh_dir" ] || return 1
    _reh_tmp=$(mktemp "$_reh_dir/.member.XXXXXX" 2>/dev/null) || return 1
    if ! runtime_cleanup_private_mode "$_reh_tmp" 600; then
        rm -f "$_reh_tmp" 2>/dev/null || true
        return 1
    fi
    if ! tar -xOzf "$_reh_archive" "$_reh_member" > "$_reh_tmp" 2>/dev/null; then
        rm -f "$_reh_tmp" 2>/dev/null || true
        return 1
    fi
    _reh_sha=$("$RUNTIME_CLEANUP_SHA256" "$_reh_tmp" 2>/dev/null | cut -d' ' -f1)
    rm -f "$_reh_tmp" 2>/dev/null || true
    [ -n "$_reh_sha" ] || return 1
    printf '%s\n' "$_reh_sha"
}

# Build and read back a new private archive of the CURRENT /rw_data/wifi tree.
# An old, empty or stale archive on the stick is never trusted on its own.
# Globals on success: RUNTIME_CLEANUP_ARCHIVE, RUNTIME_CLEANUP_ARCHIVE_SHA,
# RUNTIME_CLEANUP_STATE_SHA.
runtime_cleanup_archive_current() {
    _rca_root=$1
    _rca_stick=$2
    RUNTIME_CLEANUP_ARCHIVE=""
    RUNTIME_CLEANUP_ARCHIVE_SHA=""
    RUNTIME_CLEANUP_STATE_SHA=""
    RUNTIME_CLEANUP_TMP_ARCHIVE=""

    _rca_wifi="$_rca_root/wifi"
    if ! runtime_cleanup_safe_path "$_rca_wifi" || [ -L "$_rca_wifi" ] ||
       [ ! -d "$_rca_wifi" ]; then
        runtime_cleanup_archive_abort "preserved WiFi state: unsafe WiFi runtime path"
        return 1
    fi
    if find "$_rca_wifi" \( ! -type f ! -type d \) -print 2>/dev/null | grep -q .; then
        runtime_cleanup_archive_abort "preserved WiFi state: symlink or special file in WiFi runtime"
        return 1
    fi
    if find "$_rca_wifi" \( -perm -0002 -o -perm -4000 -o -perm -2000 \) \
         -print 2>/dev/null | grep -q .; then
        runtime_cleanup_archive_abort "preserved WiFi state: unsafe permissions in WiFi runtime"
        return 1
    fi
    if ! find "$_rca_wifi" -type f -print 2>/dev/null | grep -q .; then
        runtime_cleanup_archive_abort "preserved WiFi state: empty WiFi runtime (nothing to archive)"
        return 1
    fi
    if [ -z "$_rca_stick" ] || ! runtime_cleanup_safe_path "$_rca_stick/backup" ||
       [ -L "$_rca_stick/backup" ]; then
        runtime_cleanup_archive_abort "preserved WiFi state: unsafe stick backup directory"
        return 1
    fi
    mkdir -p "$_rca_stick/backup" 2>/dev/null ||
        { runtime_cleanup_archive_abort "preserved WiFi state: cannot create stick backup directory"; return 1; }
    if ! runtime_cleanup_private_mode "$_rca_stick/backup" 700; then
        runtime_cleanup_archive_abort "preserved WiFi state: cannot make stick backup directory private (mode 700)"
        return 1
    fi
    RUNTIME_CLEANUP_TMP_DIR="$_rca_stick/backup"

    RUNTIME_CLEANUP_TMP_ARCHIVE=$(mktemp "$_rca_stick/backup/.wifi-verify.XXXXXX" 2>/dev/null) ||
        { runtime_cleanup_archive_abort "preserved WiFi state: cannot create private archive"; return 1; }
    if ! runtime_cleanup_private_mode "$RUNTIME_CLEANUP_TMP_ARCHIVE" 600; then
        runtime_cleanup_archive_abort "preserved WiFi state: cannot make private archive mode 600"
        return 1
    fi
    if ! tar -czf "$RUNTIME_CLEANUP_TMP_ARCHIVE" -C "$_rca_root" wifi 2>/dev/null; then
        runtime_cleanup_archive_abort "preserved WiFi state: cannot archive current WiFi runtime"
        return 1
    fi
    if ! runtime_cleanup_private_mode "$RUNTIME_CLEANUP_TMP_ARCHIVE" 600; then
        runtime_cleanup_archive_abort "preserved WiFi state: private archive mode changed"
        return 1
    fi
    if [ ! -f "$RUNTIME_CLEANUP_TMP_ARCHIVE" ] ||
       [ -L "$RUNTIME_CLEANUP_TMP_ARCHIVE" ] ||
       [ ! -r "$RUNTIME_CLEANUP_TMP_ARCHIVE" ]; then
        runtime_cleanup_archive_abort "preserved WiFi state: private archive unreadable"
        return 1
    fi
    _rca_sha=$("$RUNTIME_CLEANUP_SHA256" "$RUNTIME_CLEANUP_TMP_ARCHIVE" 2>/dev/null | cut -d' ' -f1)
    if [ -z "$_rca_sha" ]; then
        runtime_cleanup_archive_abort "preserved WiFi state: cannot hash private archive"
        return 1
    fi
    _rca_list=$(tar -tzf "$RUNTIME_CLEANUP_TMP_ARCHIVE" 2>/dev/null) ||
        { runtime_cleanup_archive_abort "preserved WiFi state: cannot read private archive back"; return 1; }
    [ -n "$_rca_list" ] ||
        { runtime_cleanup_archive_abort "preserved WiFi state: private archive is empty"; return 1; }

    _rca_old_ifs=$IFS
    IFS='
'
    _rca_bad=0
    _rca_reason="member"
    for _rca_member in $_rca_list; do
        case "$_rca_member" in
            wifi|wifi/|wifi/*) : ;;
            *) _rca_bad=1; _rca_reason="member"; break ;;
        esac
        _rca_member_path=${_rca_member%/}
        case "/$_rca_member_path/" in
            *'/../'*|*'/./'*|*'//'*) _rca_bad=1; _rca_reason="member"; break ;;
        esac
        _rca_member_type=$(runtime_cleanup_archive_member_type \
            "$RUNTIME_CLEANUP_TMP_ARCHIVE" "$_rca_member_path") || {
            _rca_bad=1; _rca_reason="type"; break; }
        if [ "$_rca_member_path" = "wifi" ]; then
            [ "$_rca_member_type" = d ] && [ -d "$_rca_root/wifi" ] || {
                _rca_bad=1; _rca_reason="type"; break; }
        elif [ "$_rca_member_type" = d ]; then
            [ -d "$_rca_root/$_rca_member_path" ] &&
                [ ! -L "$_rca_root/$_rca_member_path" ] || {
                _rca_bad=1; _rca_reason="type"; break; }
        elif [ "$_rca_member_type" = - ]; then
            [ -f "$_rca_root/$_rca_member_path" ] &&
                [ ! -L "$_rca_root/$_rca_member_path" ] || {
                _rca_bad=1; _rca_reason="type"; break; }
            _rca_current=$("$RUNTIME_CLEANUP_SHA256" "$_rca_root/$_rca_member_path" 2>/dev/null | cut -d' ' -f1)
            _rca_archived=$(runtime_cleanup_extract_hash \
                "$RUNTIME_CLEANUP_TMP_ARCHIVE" "$_rca_member_path" \
                "$RUNTIME_CLEANUP_TMP_DIR") || {
                _rca_bad=1; _rca_reason="extract"; break; }
            if [ -z "$_rca_current" ] || [ -z "$_rca_archived" ] ||
               [ "$_rca_current" != "$_rca_archived" ]; then
                _rca_bad=1; _rca_reason="hash"; break
            fi
        else
            _rca_bad=1; _rca_reason="type"; break
        fi
        IFS=$_rca_old_ifs
    done
    IFS=$_rca_old_ifs
    if [ "$_rca_bad" = 1 ]; then
        case "$_rca_reason" in
            type) runtime_cleanup_archive_abort "preserved WiFi state: private archive member type rejected" ;;
            extract) runtime_cleanup_archive_abort "preserved WiFi state: private archive extraction failed" ;;
            hash) runtime_cleanup_archive_abort "preserved WiFi state: current file/hash readback mismatch" ;;
            *) runtime_cleanup_archive_abort "preserved WiFi state: private archive member rejected" ;;
        esac
        return 1
    fi

    _rca_files=$(find "$_rca_wifi" -type f -print 2>/dev/null)
    [ -n "$_rca_files" ] ||
        { runtime_cleanup_archive_abort "preserved WiFi state: WiFi runtime changed during archive"; return 1; }
    _rca_state=""
    _rca_reason="current"
    IFS='
'
    for _rca_file in $_rca_files; do
        if [ -z "$_rca_file" ] || [ -L "$_rca_file" ] || [ ! -f "$_rca_file" ]; then
            _rca_bad=1; _rca_reason="current"; break
        fi
        _rca_rel=${_rca_file#"$_rca_root/"}
        case "/$_rca_rel/" in
            *'/../'*|*'/./'*|*'//'*) _rca_bad=1; _rca_reason="current"; break ;;
        esac
        if ! printf '%s\n' "$_rca_list" | grep -Fqx "$_rca_rel"; then
            _rca_bad=1; _rca_reason="current"; break
        fi
        _rca_member_type=$(runtime_cleanup_archive_member_type \
            "$RUNTIME_CLEANUP_TMP_ARCHIVE" "$_rca_rel") || {
            _rca_bad=1; _rca_reason="type"; break; }
        [ "$_rca_member_type" = - ] || { _rca_bad=1; _rca_reason="type"; break; }
        _rca_current=$("$RUNTIME_CLEANUP_SHA256" "$_rca_file" 2>/dev/null | cut -d' ' -f1)
        _rca_archived=$(runtime_cleanup_extract_hash \
            "$RUNTIME_CLEANUP_TMP_ARCHIVE" "$_rca_rel" \
            "$RUNTIME_CLEANUP_TMP_DIR") || {
            _rca_bad=1; _rca_reason="extract"; break; }
        if [ -z "$_rca_current" ] || [ -z "$_rca_archived" ] ||
           [ "$_rca_current" != "$_rca_archived" ]; then
            _rca_bad=1; _rca_reason="hash"; break
        fi
        _rca_state="$_rca_state$_rca_rel $("$RUNTIME_CLEANUP_SHA256" "$_rca_file" 2>/dev/null | cut -d' ' -f1)
"
        IFS=$_rca_old_ifs
    done
    IFS=$_rca_old_ifs
    if [ "$_rca_bad" = 1 ]; then
        case "$_rca_reason" in
            type) runtime_cleanup_archive_abort "preserved WiFi state: private archive member type rejected" ;;
            extract) runtime_cleanup_archive_abort "preserved WiFi state: private archive extraction failed" ;;
            *) runtime_cleanup_archive_abort "preserved WiFi state: current file/hash readback mismatch" ;;
        esac
        return 1
    fi

    _rca_final="$_rca_stick/backup/wifi-pre-migration-current-$_rca_sha.tar.gz"
    if [ -e "$_rca_final" ] || [ -L "$_rca_final" ]; then
        if [ ! -f "$_rca_final" ] || [ -L "$_rca_final" ] ||
           [ "$("$RUNTIME_CLEANUP_SHA256" "$_rca_final" 2>/dev/null | cut -d' ' -f1)" != "$_rca_sha" ]; then
            runtime_cleanup_archive_abort "preserved WiFi state: content-addressed archive collision"
            return 1
        fi
        if ! runtime_cleanup_private_mode "$_rca_final" 600; then
            runtime_cleanup_archive_abort "preserved WiFi state: cannot secure existing private archive"
            return 1
        fi
        rm -f "$RUNTIME_CLEANUP_TMP_ARCHIVE" 2>/dev/null || true
        RUNTIME_CLEANUP_TMP_ARCHIVE=""
    else
        mv -f "$RUNTIME_CLEANUP_TMP_ARCHIVE" "$_rca_final" 2>/dev/null ||
            { runtime_cleanup_archive_abort "preserved WiFi state: cannot seal private archive"; return 1; }
        RUNTIME_CLEANUP_TMP_ARCHIVE=""
        if ! runtime_cleanup_private_mode "$_rca_final" 600; then
            runtime_cleanup_archive_abort "preserved WiFi state: cannot secure sealed private archive (mode 600)"
            return 1
        fi
    fi
    _rca_readback=$("$RUNTIME_CLEANUP_SHA256" "$_rca_final" 2>/dev/null | cut -d' ' -f1)
    if [ -z "$_rca_readback" ] || [ "$_rca_readback" != "$_rca_sha" ]; then
        runtime_cleanup_archive_abort "preserved WiFi state: sealed archive hash readback failed"
        return 1
    fi
    RUNTIME_CLEANUP_ARCHIVE=$_rca_final
    RUNTIME_CLEANUP_ARCHIVE_SHA=$_rca_sha
    RUNTIME_CLEANUP_STATE_SHA=$(printf '%s' "$_rca_state" |
        "$RUNTIME_CLEANUP_SHA256" | cut -d' ' -f1)
    runtime_cleanup_note "verified current WiFi archive: $_rca_final"
    return 0
}

# Remove /rw_data/wifi only with all positive evidence below:
#   * readable shared QML and assets, no WiFi markers/page/tile/HelloTile;
#   * no wifi-daemon service file or dangling link;
#   * no live exact owned wifi_daemon or WiFi dropbear process, and /proc readable;
#   * caller bundle archive/hash proof;
#   * a fresh private archive of the CURRENT /rw_data/wifi tree, verified by
#     member list and per-file hash readback before deletion.
# Old/stale archives and receipts are never authorization.  Any read or
# verification failure preserves the runtime tree and reports the reason.
cleanup_removed_wifi() {
    _rc_root=$1
    _rc_qml=$2
    _rc_stick=$3
    _rc_archive=$4
    _rc_expected=$5
    _rc_service=${6:-${CLEANUP_WIFI_SERVICE:-/etc/systemd/system/wifi-daemon.service}}

    if ! runtime_cleanup_safe_path "$_rc_root" || [ "$_rc_root" = "/" ]; then
        runtime_cleanup_note "refused unsafe runtime root: $_rc_root"
        return 1
    fi
    _rc_wifi="$_rc_root/wifi"
    if [ ! -e "$_rc_wifi" ] && [ ! -L "$_rc_wifi" ]; then
        return 0
    fi
    if [ -L "$_rc_wifi" ]; then
        runtime_cleanup_note "refused symlink WiFi runtime dir: $_rc_wifi"
        return 1
    fi
    if [ ! -d "$_rc_wifi" ]; then
        runtime_cleanup_note "refused non-directory WiFi runtime path: $_rc_wifi"
        return 1
    fi

    # Direct payload calls pass no bundle proof; they must never recursively
    # delete WiFi credentials.  The flat wrapper passes its verified bundle.
    if [ -z "$_rc_expected" ] || [ -z "$_rc_archive" ]; then
        runtime_cleanup_note "preserved WiFi state: caller bundle proof missing (non-destructive)"
        return 1
    fi

    # Re-verify the bundle hash without extracting or writing anything.
    if [ ! -f "$_rc_archive" ] || [ -L "$_rc_archive" ] || [ ! -r "$_rc_archive" ]; then
        runtime_cleanup_note "preserved WiFi state: bundle archive unreadable"
        return 1
    fi
    _rc_actual=$("$RUNTIME_CLEANUP_SHA256" "$_rc_archive" 2>/dev/null | cut -d' ' -f1)
    if [ -z "$_rc_actual" ] || [ "$_rc_actual" != "$_rc_expected" ]; then
        runtime_cleanup_note "preserved WiFi state: bundle archive hash mismatch"
        return 1
    fi
    if ! tar -tzf "$_rc_archive" >/dev/null 2>&1; then
        runtime_cleanup_note "preserved WiFi state: bundle archive unreadable"
        return 1
    fi

    if ! runtime_cleanup_safe_path "$_rc_qml" || [ ! -d "$_rc_qml" ] ||
       [ -L "$_rc_qml" ] || [ ! -r "$_rc_qml" ]; then
        runtime_cleanup_note "preserved WiFi state: unreadable QML directory"
        return 1
    fi
    for _rc_rel in HomeScreen/QHMI_HomeScreenMain_Advanced.qml \
                    HomeScreen/QHMI_HomeScreenMain_Wide.qml \
                    HomeScreen/Screen_HomeScreen_AllMenus.qml \
                    HomeScreen/Screen_HomeScreen_Submenu_Setup.qml \
                    component/QCustomizedMenuListAllowEmpty.qml; do
        _rc_file="$_rc_qml/$_rc_rel"
        runtime_cleanup_marker_state "$_rc_file"
        _rc_state=$?
        case "$_rc_state" in
            0) : ;;
            1) runtime_cleanup_note "preserved WiFi state: WiFi marker in $_rc_rel"
               return 1 ;;
            *) runtime_cleanup_note "preserved WiFi state: unreadable marker $_rc_rel"
               return 1 ;;
        esac
    done
    for _rc_rel in HomeScreen/WifiPage.qml component/WifiTile.qml \
                    component/WifiSwitch.qml component/WifiButton.qml \
                    component/HelloTile.qml; do
        if [ -e "$_rc_qml/$_rc_rel" ] || [ -L "$_rc_qml/$_rc_rel" ]; then
            runtime_cleanup_note "preserved WiFi state: WiFi UI still present ($_rc_rel)"
            return 1
        fi
    done
    _rc_apphome=${_rc_qml%/qml_eco3.0}
    for _rc_rel in wifi hello; do
        if [ -e "$_rc_apphome/$_rc_rel" ] || [ -L "$_rc_apphome/$_rc_rel" ]; then
            runtime_cleanup_note "preserved WiFi state: WiFi asset dir still present ($_rc_rel)"
            return 1
        fi
    done
    if ! runtime_cleanup_safe_path "$_rc_service"; then
        runtime_cleanup_note "preserved WiFi state: unsafe WiFi service path"
        return 1
    fi
    if [ -e "$_rc_service" ] || [ -L "$_rc_service" ]; then
        runtime_cleanup_note "preserved WiFi state: WiFi service still present"
        return 1
    fi
    runtime_cleanup_process_alive "$_rc_root" wifi wifi_daemon
    _rc_proc_state=$?
    if [ "$_rc_proc_state" != 1 ]; then
        runtime_cleanup_note "preserved WiFi state: wifi_daemon not verifiably stopped"
        return 1
    fi
    runtime_cleanup_process_alive "$_rc_root" wifi bin/dropbearmulti
    _rc_proc_state=$?
    if [ "$_rc_proc_state" != 1 ]; then
        runtime_cleanup_note "preserved WiFi state: WiFi dropbear not verifiably stopped"
        return 1
    fi

    if ! runtime_cleanup_archive_current "$_rc_root" "$_rc_stick"; then
        return 1
    fi
    _rc_sha=$RUNTIME_CLEANUP_ARCHIVE_SHA
    _rc_source=$RUNTIME_CLEANUP_ARCHIVE

    if ! rm -rf "$_rc_wifi" 2>/dev/null ||
       [ -e "$_rc_wifi" ] || [ -L "$_rc_wifi" ]; then
        runtime_cleanup_note "failed to remove owned WiFi runtime: $_rc_wifi"
        return 1
    fi
    runtime_cleanup_note "removed owned WiFi runtime after verified current-state archive: $_rc_wifi"

    # Persist provenance only; receipts never authorise later deletion.
    _rc_combo="$_rc_root/combo"
    if [ ! -L "$_rc_combo" ] && { [ ! -e "$_rc_combo" ] || [ -d "$_rc_combo" ]; }; then
        if mkdir -p "$_rc_combo" 2>/dev/null; then
            {
                echo "SCOURGE_WIFI_REMOVAL_RECEIPT"
                echo "archive_sha256=$_rc_sha"
                echo "archive_source=${_rc_source:-current-state}"
                echo "state_sha256=$RUNTIME_CLEANUP_STATE_SHA"
                echo "runtime=$_rc_wifi"
            } > "$_rc_combo/wifi-removed.receipt" 2>/dev/null &&
                chmod 600 "$_rc_combo/wifi-removed.receipt" 2>/dev/null || \
                runtime_cleanup_note "could not record WiFi removal receipt"
        fi
    fi
    return 0
}
