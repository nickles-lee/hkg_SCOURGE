#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
[ -z "${HUDNAV_NOCR:-}" ] && [ -f "$0" ] && { HUDNAV_SELF_DIR=$(dirname "$0"); export HUDNAV_SELF_DIR; tr -d '\r' < "$0" > "${TMPDIR:-/tmp}/hudnav_rs_nocr.sh" && HUDNAV_NOCR=1 exec sh "${TMPDIR:-/tmp}/hudnav_rs_nocr.sh" "$@"; } #
# HUD Nav recovery stick v4: save diagnostics to the stick, then undo EVERYTHING the HUD Nav project changed
# on the head unit, then reboot. Put this file at the ROOT of a FAT/exFAT USB stick, plug it in, wait ~1-2 min.
# Runs as root via the SCOURGE run_stuff failsafe (a boot-time service; md5-gated: this file differs from v1).
#
# v4 vs v3: survives CRLF line endings (line 2 re-execs a CR-stripped copy of itself).
# v3 vs v2: the REVERT runs first (cheap read-only snapshot, then revert), a background watchdog forces the reboot
# whatever happens, and every slow diagnostic runs under a time limit (the car has no `timeout` command).
# What it reverts (all four are the only persistent things the project ever wrote):
#   1. /etc/ld.so.preload                    removed
#   2. /app/bin/AppLauncher.sh               LD_PRELOAD line put back to stock (sha256-checked)
#   3. /lib/systemd/system/carplay_iAP2.service   our Environment=LD_PRELOAD line removed (sha256-checked)
#   4. /rw_data/hudnav                       renamed to /rw_data/hudnav.off (nothing deleted)
# It never touches any other file. Results: hudnav_diag_v4/ on the stick (00_loader_ran.txt appears FIRST),
# copy in /rw_data/hudnav_recover_v4/.
MARK=4
PATH="${HUDNAV_PATH_PREPEND:+$HUDNAV_PATH_PREPEND:}/bin:/sbin:/usr/bin:/usr/sbin"   # prepend = test hook only
R="${HUDNAV_ROOT:-}"            # test hook only: fake root prefix (empty on the car)
USB="${USB_DIR:-}"
# If the loader did not set USB_DIR (or it is not writable), find the stick ourselves: our own directory, then any /media mount.
if [ -z "$USB" ] || [ ! -w "$USB" ]; then d=${HUDNAV_SELF_DIR:-$(dirname "$0")} 2>/dev/null; [ -n "$d" ] && [ -w "$d" ] && USB="$d"; fi
if [ -z "$USB" ] || [ ! -w "$USB" ]; then
  for m in $(grep '^[^ ]* /media/' "$R/proc/mounts" 2>/dev/null | cut -d' ' -f2); do [ -w "$m" ] && { USB="$m"; break; }; done
fi
[ -n "$USB" ] || USB=/tmp
D="$USB/hudnav_diag_v4"
mkdir -p "$D" 2>/dev/null || { D=/tmp/hudnav_diag_v4; mkdir -p "$D"; }
echo "loader ran: $(date) mark=$MARK uid=$(id -u) USB_DIR=${USB_DIR:-unset}" > "$D/00_loader_ran.txt"
sync

STOCK_AL="${HUDNAV_PIN_AL:-dc9487eb2f18218f20715881ea80011e597a29b778fc37ba754d90a8c4d4e39d}"
STOCK_CP="${HUDNAV_PIN_CP:-f7cf915509005a4aaa18d09e3a6ccd0abdd72ef2b2816641ea9a592a1e3f4495}"
AL="$R/app/bin/AppLauncher.sh"
CPU="$R/lib/systemd/system/carplay_iAP2.service"
PRE="$R/etc/ld.so.preload"
HN="$R/rw_data/hudnav"
BK="$HN/backup"
DONE="$R/rw_data/hudnav_recover_v4.done"

# W SECS OUTFILE CMD...: run CMD with output to OUTFILE, kill it after SECS (no `timeout` on the car).
W() { s=$1; o=$2; shift 2; [ -n "${HUDNAV_W_MAX:-}" ] && [ "$s" -gt "$HUDNAV_W_MAX" ] && s=$HUDNAV_W_MAX; "$@" > "$o" 2>&1 & p=$!; i=0
  while kill -0 $p 2>/dev/null && [ $i -lt $s ]; do sleep 1; i=$((i+1)); done
  if kill -0 $p 2>/dev/null; then kill -9 $p 2>/dev/null; echo "[killed after ${s}s]" >> "$o"; fi; }
shaof() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1; }

snap() {   # snap TAG : state of everything we touched plus why a boot might hang
  f="$D/$1.txt"
  {
    echo "== $1 $(date) =="
    echo "-- uptime/load"; cat /proc/uptime /proc/loadavg 2>&1
    echo "-- cmdline"; cat /proc/cmdline 2>&1
    echo "-- ld.so.preload"; ls -l "$PRE" 2>&1; cat "$PRE" 2>&1
    echo "-- AppLauncher.sh"; ls -l "$AL" 2>&1; shaof "$AL"; grep -n 'LD_PRELOAD' "$AL" 2>&1
    echo "-- carplay_iAP2.service"; ls -l "$CPU" 2>&1; shaof "$CPU"; grep -n -E 'LD_PRELOAD|^Environment' "$CPU" 2>&1
    echo "-- /rw_data"; ls -la "$R/rw_data" 2>&1 | head -40
    echo "-- hudnav tree"; ls -laR "$HN" 2>&1 | head -80
    echo "-- mounts"; cat /proc/mounts 2>&1
    echo "-- df"; df 2>&1
    echo "-- mem"; head -5 /proc/meminfo 2>&1
    echo "-- processes mapping a hudnav shim"; grep -l -E 'aa_shim|cp_hook|noop' /proc/[0-9]*/maps 2>/dev/null
  } > "$f" 2>&1
}

snap 01_before
sync

if [ -f "$DONE" ]; then
  echo "recovery already ran ($DONE exists); not repeating, not rebooting. Change MARK to force." > "$D/05_already_done.txt"
  sync
  exit 0
fi

LOGR="$D/05_revert.txt"
{
  echo "== revert $(date) =="
  RW=0
  if mount -o remount,rw / 2>&1; then RW=1; echo "remount rw: ok"; else echo "remount rw: FAILED (continuing)"; fi

  # 1. ld.so.preload
  rm -f "$PRE"; echo "ld.so.preload removed: $([ -e "$PRE" ] && echo NO || echo yes)"

  # 2. AppLauncher.sh
  if [ -f "$AL" ]; then
    now=$(shaof "$AL")
    if [ "$now" = "$STOCK_AL" ]; then echo "AppLauncher.sh already stock"
    else
      tmp="$D/AppLauncher.new"
      sed 's|^export LD_PRELOAD=.*$|export LD_PRELOAD=/usr/lib/libEGL.so.1|' "$AL" > "$tmp"
      if [ "$(shaof "$tmp")" = "$STOCK_AL" ]; then cat "$tmp" > "$AL"; echo "AppLauncher.sh restored by line edit"
      elif [ -f "$BK/AppLauncher.sh.stock" ] && [ "$(shaof "$BK/AppLauncher.sh.stock")" = "$STOCK_AL" ]; then
        cat "$BK/AppLauncher.sh.stock" > "$AL"; echo "AppLauncher.sh restored from backup"
      else echo "AppLauncher.sh NOT restored (neither edit nor backup gives the stock hash) - left untouched"; fi
      rm -f "$tmp"
    fi
    echo "AppLauncher.sh sha256 now: $(shaof "$AL") (stock $STOCK_AL)"
  else echo "AppLauncher.sh MISSING"; fi

  # 3. carplay_iAP2.service
  if [ -f "$CPU" ]; then
    now=$(shaof "$CPU")
    if [ "$now" = "$STOCK_CP" ]; then echo "carplay_iAP2.service already stock"
    else
      tmp="$D/carplay.new"
      grep -vxF 'Environment=LD_PRELOAD=/rw_data/hudnav/bin/cp_hook.so' "$CPU" > "$tmp"
      if [ "$(shaof "$tmp")" = "$STOCK_CP" ]; then cat "$tmp" > "$CPU"; echo "carplay_iAP2.service restored by line removal"
      elif [ -f "$BK/carplay_iAP2.service.stock" ] && [ "$(shaof "$BK/carplay_iAP2.service.stock")" = "$STOCK_CP" ]; then
        cat "$BK/carplay_iAP2.service.stock" > "$CPU"; echo "carplay_iAP2.service restored from backup"
      else echo "carplay_iAP2.service NOT restored (no stock match) - left untouched"; fi
      rm -f "$tmp"
    fi
    echo "carplay_iAP2.service sha256 now: $(shaof "$CPU") (stock $STOCK_CP)"
  else echo "carplay_iAP2.service MISSING"; fi

  # 4. the kit: rename, never delete
  if [ -d "$HN" ]; then
    tgt="$HN.off"
    [ -e "$tgt" ] && tgt="$HN.off.$(date +%s)"
    mv "$HN" "$tgt" && echo "renamed $HN -> $tgt" || echo "rename of $HN FAILED"
  else echo "no $HN"; fi

  sync
  [ "$RW" = 1 ] && { mount -o remount,ro / 2>&1 && echo "remount ro: ok" || echo "remount ro: failed"; }
  echo "ld.so.preload now: $(ls -l "$PRE" 2>&1)"
} > "$LOGR" 2>&1
sync
# Safety net: whatever the slow diagnostics below do, the unit is rebooted. (Not armed in the already-done case.)
if [ -z "${HUDNAV_NO_REBOOT:-}" ]; then ( sleep 150; sync; reboot -f ) > /dev/null 2>&1 & fi

snap 06_after
W 20 "$D/02_dmesg.txt" sh -c 'dmesg | head -120; echo ......; dmesg | tail -300'
W 20 "$D/03_journal.txt" sh -c 'journalctl -b --no-pager -n 400'
W 10 "$D/04_loader_log.txt" sh -c "tail -150 '$R/rw_data/initial_root.log'; echo ---; tail -50 '$R/tmp/initial_root.log'"
# The car's own trace (DLT) carries AppNavi/AppLauncher messages: the last ~4 MB of text, filtered.
W 25 "$D/04b_dlt_filtered.txt" sh -c "ls -la '$R/log_data' | head -30; for f in '$R'/log_data/dltlog*; do [ -f \"\$f\" ] || continue; echo \"== \$f\"; tail -c 4000000 \"\$f\" | { if command -v strings >/dev/null 2>&1; then strings -n 8; else tr -cd '[:print:]\n'; fi; } | grep -a -i -E 'navi|launcher|crash|segv|fatal|abort|respawn|restart|terminate|homescreen' | tail -400; done"
# What changed on the writable partitions recently, and an unfiltered DLT tail (the filter above could miss the cause).
W 30 "$D/09_recent_files.txt" sh -c "find '$R/rw_data' '$R/Data' -xdev -type f -mtime -3 2>/dev/null | head -300; echo ---; ls -la '$R/rw_data' '$R/rw_data/hudnav.off' '$R/rw_data/app_data' 2>&1 | head -80"
W 25 "$D/10_dlt_tail.txt" sh -c "for f in '$R'/log_data/dltlog*; do [ -f \"\$f\" ] || continue; echo \"== \$f\"; tail -c 600000 \"\$f\" | { if command -v strings >/dev/null 2>&1; then strings -n 8; else tr -cd '[:print:]\n'; fi; } | tail -300; done"
W 15 "$D/07_ps.txt" sh -c 'ps -eo pid,ppid,stat,wchan:22,args || ps'
W 15 "$D/08_systemd.txt" sh -c 'systemctl is-system-running; systemctl --no-pager --failed; systemctl --no-pager list-jobs'
sync
mkdir -p "$R/rw_data/hudnav_recover_v4" 2>/dev/null && cp "$D"/*.txt "$R/rw_data/hudnav_recover_v4/" 2>/dev/null
echo "mark=$MARK $(date)" > "$DONE" 2>/dev/null
echo "rebooting in 5 s (remove the stick once the screen goes dark)" >> "$LOGR"
sync
[ -n "${HUDNAV_NO_REBOOT:-}" ] && exit 0
sleep 5
reboot
# v1 logged a clean run but the unit did NOT reboot (still up 30 s later); the watchdog above is the backstop,
# this is the quicker forced reboot.
sleep 25
sync
reboot -f
