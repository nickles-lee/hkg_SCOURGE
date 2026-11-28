#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
#
# SESSION-ONLY CarPlay test for the 260319 head unit (run as root).
# This script NEVER writes to / : the read-only root filesystem is left alone and
# there is no /etc/ld.so.preload. The only kernel change is a --bind mount of a
# patched COPY over /app/bin/iAP2Service-2.0; a reboot ALWAYS returns the car to
# stock. The copy (iAP2Service-2.0.cp), cp_rg.so and SHA256SUMS.cp live in
# /rw_data/hudnav/bin. Host-tested only; do not use on the car without the owner.
#
# Usage: sh cp-session.sh status
#        sh cp-session.sh start                  (dry run: print, change nothing)
#        sh cp-session.sh start --yes [--trace]  (bind mount + restart the service)
#        sh cp-session.sh stop                   (refuses)
#        sh cp-session.sh stop --yes             (unmount + restart stock service)
#        sh -s -- status < cp-session.sh
# Test-only hooks: HUDNAV_ROOT (fake root prefix), HUDNAV_PATH_PREPEND (fake tools),
# HUDNAV_PIN_CP (stock hash), HUDNAV_RESTART_TRIES / HUDNAV_RESTART_DELAY (poll),
# systemctl is taken from PATH so tests can supply a fake (as with pidof/mount).
set -u
PATH="${HUDNAV_PATH_PREPEND:+$HUDNAV_PATH_PREPEND:}/bin:/sbin:/usr/bin:/usr/sbin"
R="${HUDNAV_ROOT:-}"
P="$R/app/bin/iAP2Service-2.0"
BIN="$R/rw_data/hudnav/bin"
COPY="$BIN/iAP2Service-2.0.cp"
LIB="$BIN/cp_rg.so"
SUMS="$BIN/SHA256SUMS.cp"
PRE="$R/etc/ld.so.preload"
MOUNTS="$R/proc/mounts"
GATE="$R/rw_data/hudnav/cp-rg-enable"
TRACE="$R/rw_data/hudnav/cp-trace"
UNIT=carplay_iAP2.service
PIN="${HUDNAV_PIN_CP:-e663d5129177a8bd241cb875e36ec5e507d39a2c8111216acafc272bdc3fa88b}"
TRIES="${HUDNAV_RESTART_TRIES:-30}"
DELAY="${HUDNAV_RESTART_DELAY:-1}"

shaof() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1; }

# A line in proc/mounts whose SECOND field is exactly the iAP2Service path.
bound() {
  [ -r "$MOUNTS" ] || return 1
  while read -r dev mp rest; do
    [ "$mp" = "$P" ] && return 0
  done < "$MOUNTS"
  return 1
}

appid() {
  ids=$(pidof iAP2Service-2.0 2>/dev/null)
  set -- $ids
  echo "${1:-}"
}

lib_mapped() {
  p=$1
  [ -n "$p" ] || return 1
  n=$(grep -c cp_rg.so "$R/proc/$p/maps" 2>/dev/null)
  [ -n "$n" ] && [ "$n" -gt 0 ] 2>/dev/null
}

gate_on() { [ -f "$GATE" ]; }

final_status() {
  b=no; bound && b=yes
  now=$(shaof "$P")
  s=no; [ "$now" = "$PIN" ] && s=yes
  pid=$(appid)
  m=n/a
  if [ -n "$pid" ]; then m=no; lib_mapped "$pid" && m=yes; fi
  pr=absent; [ -e "$PRE" ] && pr=present
  g=off; gate_on && g=on
  echo "STATUS bound=$b stock_hash=$s lib_mapped=$m preload=$pr gate=$g"
}

refuse() {
  echo "REFUSED: $1" >&2
  final_status
  exit 1
}

# Poll once per interval for a NEW service pid after systemctl restart/start.
wait_new_pid() {
  old=$1
  i=0
  while [ "$i" -lt "$TRIES" ]; do
    [ "$DELAY" = 0 ] || sleep "$DELAY"
    i=$((i+1))
    cur=$(appid)
    if [ -n "$cur" ] && [ "$cur" != "$old" ]; then echo "$cur"; return 0; fi
  done
  return 1
}

remove_gates() {
  rm -f "$GATE" "$TRACE" || return 1
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

# The stop path, used by `stop --yes` and by automatic rollback after a mount.
do_stop() {
  remove_gates || echo "could not remove the gate files" >&2
  if systemctl stop "$UNIT"; then echo "systemctl stop $UNIT: ok"
  else echo "systemctl stop $UNIT: FAILED" >&2; fi
  if bound; then
    # Car 2026-11-26: a plain umount fails with EBUSY while the running service
    # executes the bind-mounted copy; a lazy umount detaches it at once (the
    # running process keeps its mapping until systemctl restarts it below).
    if umount "$P" 2>/dev/null; then echo "umount $P: ok"
    elif umount -l "$P" 2>/dev/null; then echo "umount $P: busy, lazy umount ok"
    else echo "umount $P: FAILED" >&2; fi
  else
    echo "not bind-mounted; nothing to umount"
  fi
  if bound; then echo "STILL bind-mounted after umount" >&2; else echo "bind mount gone"; fi
  now=$(shaof "$P")
  if [ "$now" = "$PIN" ]; then echo "stock hash restored: $now"
  else echo "stock hash NOT restored: $now (pin $PIN)" >&2; fi
  old=$(appid)
  if systemctl start "$UNIT"; then echo "systemctl start $UNIT: ok"
  else echo "systemctl start $UNIT: FAILED" >&2; fi
  if new=$(wait_new_pid "$old"); then echo "service restarted: pid=$new"
  else echo "service did not come back within $((TRIES * DELAY)) s" >&2; fi
}

do_status() {
  echo "== cp-session status =="
  echo "iAP2Service:           $P"
  echo "systemd unit:          $UNIT"
  if bound; then echo "bind-mounted:          yes"; else echo "bind-mounted:          no"; fi
  now=$(shaof "$P")
  echo "sha256(iAP2Service):   $now"
  echo "pin:                   $PIN (match: $([ "$now" = "$PIN" ] && echo yes || echo no))"
  echo "copy:                  $COPY"
  echo "sha256(copy):          $(shaof "$COPY")"
  echo "library:               $LIB"
  echo "sha256(library):       $(shaof "$LIB")"
  pid=$(appid)
  echo "iAP2Service pid:       ${pid:-none}"
  if [ -z "$pid" ]; then echo "cp_rg.so mapped:       n/a"
  elif lib_mapped "$pid"; then echo "cp_rg.so mapped:       yes (pid $pid)"
  else echo "cp_rg.so mapped:       no (pid $pid)"; fi
  echo "gate cp-rg-enable:     $(gate_on && echo present || echo absent)"
  echo "gate cp-trace:         $([ -f "$TRACE" ] && echo present || echo absent)"
  echo "checksums:             $SUMS"
  echo "/etc/ld.so.preload:    $([ -e "$PRE" ] && echo present || echo absent) (must be absent)"
}

start_checks() {
  [ ! -e "$PRE" ] || refuse "/etc/ld.so.preload exists (system-wide preload is banned)"
  ! bound || refuse "already bind-mounted (run 'stop --yes' first)"
  now=$(shaof "$P")
  [ "$now" = "$PIN" ] || refuse "/app/bin/iAP2Service-2.0 does not match the stock pin ($now)"
  [ -f "$LIB" ] || refuse "cp_rg.so is missing ($LIB)"
  [ -f "$COPY" ] || refuse "patched copy is missing ($COPY)"
  [ -f "$SUMS" ] || refuse "SHA256SUMS.cp is missing ($SUMS)"
  grep -q ' iAP2Service-2.0.cp$' "$SUMS" || refuse "SHA256SUMS.cp has no iAP2Service-2.0.cp entry"
  grep -q ' cp_rg.so$' "$SUMS" || refuse "SHA256SUMS.cp has no cp_rg.so entry"
  chk=$( (cd "$BIN" && sha256sum -c SHA256SUMS.cp 2>&1) ) || refuse "kit files do not match SHA256SUMS.cp: $chk"
  echo "  checks OK: no preload, not bound, stock hash, copy + cp_rg.so verified"
}

case "${1:-status}" in
  status)
    do_status
    final_status
    ;;
  start)
    yes=${2:-}
    trace=${3:-}
    if [ "$yes" != "--yes" ]; then
      echo "DRY RUN (nothing will be changed):"
      echo "  1. refuse if /etc/ld.so.preload exists, already bound, stock hash wrong,"
      echo "     kit files != SHA256SUMS.cp, or cp_rg.so missing"
      echo "  2. create gate $GATE (and $TRACE only with 'start --yes --trace')"
      echo "  3. label $COPY with the real binary's SMACK64EXEC exec label"
      echo "  4. mount --bind $COPY $P  (no write to /, no system-wide preload)"
      echo "  5. verify proc/mounts, systemctl restart $UNIT, poll up to $TRIES s for a new pid"
      echo "  6. if anything after the mount fails, run the stop path automatically"
      final_status
      exit 0
    fi
    start_checks
    touch "$GATE" || { echo "could not create $GATE" >&2; final_status; exit 1; }
    if [ "$trace" = "--trace" ]; then
      touch "$TRACE" || { echo "could not create $TRACE" >&2; remove_gates; final_status; exit 1; }
    fi
    if ! label_copy; then
      remove_gates
      refuse "could not set security.SMACK64EXEC on the copy"
    fi
    if ! mount --bind "$COPY" "$P" 2>/dev/null; then
      echo "mount --bind failed" >&2
      remove_gates
      final_status; exit 1
    fi
    if ! bound; then
      echo "bind mount not visible in $MOUNTS; rolling back" >&2
      do_stop; final_status; exit 1
    fi
    echo "bind mount verified in proc/mounts"
    old=$(appid)
    if ! systemctl restart "$UNIT"; then
      echo "systemctl restart $UNIT failed; rolling back" >&2
      do_stop; final_status; exit 1
    fi
    if ! new=$(wait_new_pid "$old"); then
      echo "iAP2Service did not come back within $((TRIES * DELAY)) s; rolling back" >&2
      do_stop; final_status; exit 1
    fi
    echo "new pid=$new"
    if ! bound; then
      echo "bind mount disappeared after restart; rolling back" >&2
      do_stop; final_status; exit 1
    fi
    if ! lib_mapped "$new"; then
      echo "cp_rg.so NOT mapped in pid $new; rolling back" >&2
      do_stop; final_status; exit 1
    fi
    echo "cp_rg.so mapped in pid $new"
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
    echo "usage: sh cp-session.sh status | start [--yes [--trace]] | stop --yes" >&2
    final_status
    exit 2
    ;;
esac
