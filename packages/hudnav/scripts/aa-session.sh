#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
#
# SESSION-ONLY Android Auto test for the 260319 head unit (run as root).
# This script NEVER writes to / : the read-only root filesystem is left alone and
# there is no /etc/ld.so.preload. The only kernel change is a --bind mount of a
# patched COPY over /app/bin/AppProjection; a reboot ALWAYS returns the car to
# stock. The copy (AppProjection.aa), aa_shim.so and SHA256SUMS live in
# /rw_data/hudnav/bin. Host-tested only; do not use on the car without the owner.
#
# Usage: sh aa-session.sh status
#        sh aa-session.sh start          (dry run: print, change nothing)
#        sh aa-session.sh start --yes    (bind mount + restart AppProjection)
#        sh aa-session.sh stop           (refuses)
#        sh aa-session.sh stop --yes     (unmount + restart stock AppProjection)
#        sh -s -- status < aa-session.sh
# Test-only hooks: HUDNAV_ROOT (fake root prefix), HUDNAV_PATH_PREPEND (fake tools),
# HUDNAV_PIN_AP (stock hash), HUDNAV_RESTART_TRIES / HUDNAV_RESTART_DELAY (poll),
# HUDNAV_KILL (absolute fake kill path; `kill` alone is a shell builtin).
set -u
PATH="${HUDNAV_PATH_PREPEND:+$HUDNAV_PATH_PREPEND:}/bin:/sbin:/usr/bin:/usr/sbin"
R="${HUDNAV_ROOT:-}"
P="$R/app/bin/AppProjection"
BIN="$R/rw_data/hudnav/bin"
COPY="$BIN/AppProjection.aa"
SHIM="$BIN/aa_shim.so"
SUMS="$BIN/SHA256SUMS"
PRE="$R/etc/ld.so.preload"
MOUNTS="$R/proc/mounts"
PIN="${HUDNAV_PIN_AP:-726c69ce627e9183771d11bdfac9acf9d92286a9b6c1a0d88bfbd6b5ac5a7d50}"
NAV_ENABLE="$R/rw_data/hudnav/aa-nav-enable"
NAV_TRACE="$R/rw_data/hudnav/aa-trace"
TRIES="${HUDNAV_RESTART_TRIES:-30}"
DELAY="${HUDNAV_RESTART_DELAY:-1}"
KILL="${HUDNAV_KILL:-kill}"

shaof() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1; }

# A line in proc/mounts whose SECOND field is exactly the AppProjection path.
bound() {
  [ -r "$MOUNTS" ] || return 1
  while read -r dev mp rest; do
    [ "$mp" = "$P" ] && return 0
  done < "$MOUNTS"
  return 1
}

appid() {
  ids=$(pidof AppProjection 2>/dev/null)
  set -- $ids
  echo "${1:-}"
}

shim_mapped() {
  p=$1
  [ -n "$p" ] || return 1
  n=$(grep -c aa_shim.so "$R/proc/$p/maps" 2>/dev/null)
  [ -n "$n" ] && [ "$n" -gt 0 ] 2>/dev/null
}

final_status() {
  b=no; bound && b=yes
  now=$(shaof "$P")
  s=no; [ "$now" = "$PIN" ] && s=yes
  pid=$(appid)
  m=n/a
  if [ -n "$pid" ]; then m=no; shim_mapped "$pid" && m=yes; fi
  pr=absent; [ -e "$PRE" ] && pr=present
  echo "STATUS bound=$b stock_hash=$s shim_mapped=$m preload=$pr"
}

refuse() {
  echo "REFUSED: $1" >&2
  final_status
  exit 1
}

# TERM the current AppProjection, then poll once per interval for a NEW pid.
restart_app() {
  old=$(appid)
  if [ -n "$old" ]; then "$KILL" -TERM "$old" 2>/dev/null; else echo "  (no AppProjection pid to TERM)"; fi
  i=0
  while [ "$i" -lt "$TRIES" ]; do
    [ "$DELAY" = 0 ] || sleep "$DELAY"
    i=$((i+1))
    cur=$(appid)
    if [ -n "$cur" ] && [ "$cur" != "$old" ]; then echo "$cur"; return 0; fi
  done
  return 1
}

# The stop path, used by `stop --yes` and by automatic rollback after a mount.
do_stop() {
  if bound; then
    # Car 2026-11-26: a plain umount fails with EBUSY while the running AppProjection executes the
    # bind-mounted copy; a lazy umount detaches it at once (the running process keeps its mapping until
    # it is restarted below, which then execs the stock binary). Verified on the car.
    if umount "$P" 2>/dev/null; then echo "umount $P: ok"
    elif umount -l "$P" 2>/dev/null; then echo "umount $P: busy, lazy umount ok"
    else echo "umount $P: FAILED"; fi
  else
    echo "not bind-mounted; nothing to umount"
  fi
  if bound; then echo "STILL bind-mounted after umount" >&2; else echo "bind mount gone"; fi
  now=$(shaof "$P")
  if [ "$now" = "$PIN" ]; then echo "stock hash restored: $now"; else echo "stock hash NOT restored: $now (pin $PIN)" >&2; fi
  pid=$(appid)
  if [ -n "$pid" ]; then "$KILL" -TERM "$pid" 2>/dev/null; else echo "(no AppProjection pid to TERM)"; fi
  i=0; new=""
  while [ "$i" -lt "$TRIES" ]; do
    [ "$DELAY" = 0 ] || sleep "$DELAY"
    i=$((i+1))
    cur=$(appid)
    if [ -n "$cur" ] && [ "$cur" != "$pid" ]; then new=$cur; break; fi
  done
  if [ -n "$new" ]; then echo "AppProjection restarted: $new"; else echo "AppProjection did not come back within $((TRIES * DELAY)) s" >&2; fi
}

do_status() {
  echo "== aa-session status =="
  echo "AppProjection: $P"
  if bound; then echo "bind-mounted: yes"; else echo "bind-mounted: no"; fi
  now=$(shaof "$P")
  echo "sha256(AppProjection): $now"
  echo "pin:                   $PIN (match: $([ "$now" = "$PIN" ] && echo yes || echo no))"
  echo "copy:                  $COPY"
  echo "sha256(copy):          $(shaof "$COPY")"
  pid=$(appid)
  echo "AppProjection pid:     ${pid:-none}"
  if [ -z "$pid" ]; then echo "aa_shim.so mapped:     n/a"
  elif shim_mapped "$pid"; then echo "aa_shim.so mapped:     yes (pid $pid)"
  else echo "aa_shim.so mapped:     no (pid $pid)"; fi
  echo "gate aa-nav-enable:    $([ -f "$NAV_ENABLE" ] && echo present || echo absent)"
  echo "gate aa-trace:         $([ -f "$NAV_TRACE" ] && echo present || echo absent)"
  echo "/etc/ld.so.preload:    $([ -e "$PRE" ] && echo present || echo absent) (must be absent)"
}

start_checks() {
  [ ! -e "$PRE" ] || refuse "/etc/ld.so.preload exists (system-wide preload is banned)"
  ! bound || refuse "already bind-mounted (run 'stop --yes' first)"
  now=$(shaof "$P")
  [ "$now" = "$PIN" ] || refuse "/app/bin/AppProjection does not match the stock pin ($now)"
  [ -f "$SHIM" ] || refuse "aa_shim.so is missing ($SHIM)"
  [ -f "$SUMS" ] || refuse "SHA256SUMS is missing ($SUMS)"
  grep -q ' AppProjection.aa$' "$SUMS" || refuse "SHA256SUMS has no AppProjection.aa entry"
  grep -q ' aa_shim.so$' "$SUMS" || refuse "SHA256SUMS has no aa_shim.so entry"
  chk=$( (cd "$BIN" && sha256sum -c SHA256SUMS 2>&1) ) || refuse "kit files do not match SHA256SUMS: $chk"
  echo "  checks OK: no preload, not bound, stock hash, copy + shim verified"
}

label_copy() {
  label=""
  if command -v chsmack >/dev/null 2>&1; then
    label=$(chsmack "$P" 2>/dev/null | sed -n 's/.*execute="\([^"]*\)".*/\1/p' | head -1)
  fi
  if [ -z "$label" ] && command -v getfattr >/dev/null 2>&1; then
    label=$(getfattr -n security.SMACK64EXEC --only-values "$P" 2>/dev/null | head -1)
  fi
  if [ -z "$label" ]; then
    echo "  no exec label readable on the real binary; defaulting to App::projection"
    label=App::projection
  fi
  echo "  exec label for the copy: $label"
  if command -v chsmack >/dev/null 2>&1; then
    chsmack -e "$label" "$COPY" || return 1
  elif command -v setfattr >/dev/null 2>&1; then
    setfattr -n security.SMACK64EXEC -v "$label" "$COPY" || return 1
  else
    echo "  no chsmack or setfattr on PATH; cannot label the copy" >&2
    return 1
  fi
}

case "${1:-status}" in
  status)
    do_status
    final_status
    ;;
  start)
    yes=${2:-}
    if [ "$yes" != "--yes" ]; then
      echo "DRY RUN (nothing will be changed):"
      echo "  1. refuse if /etc/ld.so.preload exists, already bound, stock hash wrong,"
      echo "     patched copy != SHA256SUMS, or aa_shim.so missing"
      echo "  2. label $COPY with the real binary's SMACK64EXEC exec label"
      echo "  3. mount --bind $COPY $P  (no write to /, no system-wide preload)"
      echo "  4. verify proc/mounts, TERM AppProjection, poll up to $TRIES s for a new pid"
      echo "  5. if anything after the mount fails, umount and restart the stock binary"
      final_status
      exit 0
    fi
    start_checks
    label_copy || refuse "could not set security.SMACK64EXEC on the copy"
    if ! mount --bind "$COPY" "$P" 2>/dev/null; then
      echo "mount --bind failed" >&2
      if bound; then do_stop; fi
      final_status; exit 1
    fi
    if ! bound; then
      echo "bind mount not visible in $MOUNTS; rolling back" >&2
      do_stop; final_status; exit 1
    fi
    echo "bind mount verified in proc/mounts"
    if ! new=$(restart_app); then
      echo "AppProjection did not come back within $((TRIES * DELAY)) s; rolling back" >&2
      do_stop; final_status; exit 1
    fi
    echo "new pid=$new"
    if ! shim_mapped "$new"; then
      echo "aa_shim.so NOT mapped in pid $new; rolling back" >&2
      do_stop; final_status; exit 1
    fi
    echo "aa_shim.so mapped in pid $new"
    if [ -e "$PRE" ]; then
      echo "/etc/ld.so.preload appeared; rolling back" >&2
      do_stop; final_status; exit 1
    fi
    final_status
    ;;
  stop)
    [ "${2:-}" = "--yes" ] || refuse "stop needs --yes (nothing was changed)"
    do_stop
    final_status
    ;;
  *)
    echo "usage: sh aa-session.sh status | start [--yes] | stop --yes" >&2
    final_status
    exit 2
    ;;
esac
