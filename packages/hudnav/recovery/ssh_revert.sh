#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# ssh_revert.sh - one-command revert of every persistent HUD-project change on the car.
# Delivered over SSH on stdin (nothing is copied to the car):
#   ssh -p 2222 car@HOST 'sh -s' < ssh_revert.sh              # REPORT ONLY, changes nothing
#   ssh -p 2222 car@HOST 'sh -s -- --yes' < ssh_revert.sh     # actually revert
# POSIX sh + car tools only: sed, grep, sha256sum, cut, mount, sync, mv, date, cat, ls, rm.
# R and the two pins are test hooks; on the car R="" and the pins are the real stock hashes.
# Never reboots; never deletes anything except /etc/ld.so.preload (and its own /tmp scratch file).
set -u
R="${HUDNAV_ROOT:-}"
STOCK_AL="${HUDNAV_PIN_AL:-dc9487eb2f18218f20715881ea80011e597a29b778fc37ba754d90a8c4d4e39d}"
STOCK_CP="${HUDNAV_PIN_CP:-f7cf915509005a4aaa18d09e3a6ccd0abdd72ef2b2816641ea9a592a1e3f4495}"
PRE="$R/etc/ld.so.preload"
AL="$R/app/bin/AppLauncher.sh"
CPU="$R/lib/systemd/system/carplay_iAP2.service"
HN="$R/rw_data/hudnav"
TMP="${TMPDIR:-/tmp}/hudnav_revert.$$"

APPLY=0
[ "${1:-}" = "--yes" ] && APPLY=1
shaof() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1; }

# state: fill st_pre / st_al / st_cp / st_kit from the current filesystem (read only)
state() {
  if [ -e "$PRE" ]; then st_pre=present; else st_pre=absent; fi
  if [ "$(shaof "$AL")" = "$STOCK_AL" ]; then st_al=stock; else st_al=MODIFIED; fi
  if [ "$(shaof "$CPU")" = "$STOCK_CP" ]; then st_cp=stock; else st_cp=MODIFIED; fi
  st_kit=none
  [ -d "$HN" ] && st_kit=active
  if [ "$st_kit" = none ]; then
    for d in "$HN".off "$HN".off.*; do
      [ -d "$d" ] && { st_kit=parked; break; }
    done
  fi
}

# backup_for BASENAME PIN: echo the first stock-verified backup path, if any
backup_for() {
  b="$1"; pin="$2"
  for f in "$HN/backup/$b" "$R/rw_data/hudnav.off/backup/$b"; do
    if [ -f "$f" ] && [ "$(shaof "$f")" = "$pin" ]; then printf '%s\n' "$f"; return 0; fi
  done
  return 1
}

# al_candidate_hash / cp_candidate_hash: sed/grep transform piped into sha256sum (no file write)
al_candidate_hash() { sed 's|^export LD_PRELOAD=.*$|export LD_PRELOAD=/usr/lib/libEGL.so.1|' "$AL" 2>/dev/null | sha256sum | cut -d' ' -f1; }
cp_candidate_hash() { grep -vxF 'Environment=LD_PRELOAD=/rw_data/hudnav/bin/cp_hook.so' "$CPU" 2>/dev/null | sha256sum | cut -d' ' -f1; }

if [ "$APPLY" = 0 ]; then
  echo "MODE report-only: no --yes given; NOTHING is changed (no remount, no rm, no mv)"
  state
  echo "current: preload=$st_pre launcher=$st_al carplay=$st_cp kit=$st_kit"
  echo "current sha256: $AL = $(shaof "$AL") (stock $STOCK_AL)"
  echo "current sha256: $CPU = $(shaof "$CPU") (stock $STOCK_CP)"
  if [ -n "$R" ]; then echo "WOULD: test mode: no remount"; else echo "WOULD: mount -o remount,rw /"; fi
  if [ -e "$PRE" ]; then echo "WOULD: remove $PRE"; else echo "WOULD: no $PRE to remove"; fi
  if [ "$st_al" = stock ]; then
    echo "WOULD: AppLauncher.sh already stock (no write)"
  elif [ "$(al_candidate_hash)" = "$STOCK_AL" ]; then
    echo "WOULD: AppLauncher.sh restored by sed line edit (inode kept)"
  elif b=$(backup_for AppLauncher.sh.stock "$STOCK_AL"); then
    echo "WOULD: AppLauncher.sh restored from backup $b"
  else
    echo "WOULD: AppLauncher.sh NOT RESTORED (left untouched)"
  fi
  if [ "$st_cp" = stock ]; then
    echo "WOULD: carplay_iAP2.service already stock (no write)"
  elif [ "$(cp_candidate_hash)" = "$STOCK_CP" ]; then
    echo "WOULD: carplay_iAP2.service restored by grep line removal (inode kept)"
  elif b=$(backup_for carplay_iAP2.service.stock "$STOCK_CP"); then
    echo "WOULD: carplay_iAP2.service restored from backup $b"
  else
    echo "WOULD: carplay_iAP2.service NOT RESTORED (left untouched)"
  fi
  echo "WOULD: sync"
  if [ -n "$R" ]; then echo "WOULD: test mode: no remount"; else echo "WOULD: mount -o remount,ro /"; fi
  if [ -d "$HN" ]; then
    echo "WOULD: park $HN -> $HN.off (or $HN.off.<seconds> if $HN.off exists)"
  else
    echo "WOULD: no $HN to park"
  fi
  echo "STATUS preload=$st_pre launcher=$st_al carplay=$st_cp kit=$st_kit"
  exit 0
fi

# ---- APPLY MODE: the verified steps, in order, one line per step ----

# 1. make / writable (skipped in test mode)
if [ -n "$R" ]; then
  echo "test mode: no remount"
else
  mount -o remount,rw / && echo "remount rw: ok" || echo "remount rw: FAILED (continuing)"
fi

# 2. remove the system-wide preload (the only file we delete)
rm -f "$PRE" && echo "removed $PRE" || echo "remove $PRE FAILED"

# 3. AppLauncher.sh: sed candidate -> stock-verified backup -> untouched
if [ "$(shaof "$AL")" = "$STOCK_AL" ]; then
  echo "AppLauncher.sh already stock"
else
  sed 's|^export LD_PRELOAD=.*$|export LD_PRELOAD=/usr/lib/libEGL.so.1|' "$AL" > "$TMP" 2>/dev/null
  if [ "$(shaof "$TMP")" = "$STOCK_AL" ]; then
    cat "$TMP" > "$AL" && echo "AppLauncher.sh restored by sed line edit (inode kept)"
  elif b=$(backup_for AppLauncher.sh.stock "$STOCK_AL"); then
    cat "$b" > "$AL" && echo "AppLauncher.sh restored from backup $b"
  else
    echo "AppLauncher.sh NOT RESTORED (no edit/backup matches stock) - left untouched"
  fi
  rm -f "$TMP"
fi

# 4. carplay_iAP2.service: grep candidate -> stock-verified backup -> untouched
if [ "$(shaof "$CPU")" = "$STOCK_CP" ]; then
  echo "carplay_iAP2.service already stock"
else
  grep -vxF 'Environment=LD_PRELOAD=/rw_data/hudnav/bin/cp_hook.so' "$CPU" > "$TMP" 2>/dev/null
  if [ "$(shaof "$TMP")" = "$STOCK_CP" ]; then
    cat "$TMP" > "$CPU" && echo "carplay_iAP2.service restored by grep line removal (inode kept)"
  elif b=$(backup_for carplay_iAP2.service.stock "$STOCK_CP"); then
    cat "$b" > "$CPU" && echo "carplay_iAP2.service restored from backup $b"
  else
    echo "carplay_iAP2.service NOT RESTORED (no edit/backup matches stock) - left untouched"
  fi
  rm -f "$TMP"
fi

# 5. flush, then make / read-only again (skipped in test mode)
sync; echo "sync: done"
if [ -n "$R" ]; then
  echo "test mode: no remount"
else
  mount -o remount,ro / && echo "remount ro: ok" || echo "remount ro: FAILED"
fi

# 6. park the kit, never delete it
if [ -d "$HN" ]; then
  if [ -e "$HN.off" ]; then
    dest="$HN.off.$(date +%s)"
    mv "$HN" "$dest" && echo "kit parked: $HN -> $dest" || echo "kit park FAILED"
  else
    mv "$HN" "$HN.off" && echo "kit parked: $HN -> $HN.off" || echo "kit park FAILED"
  fi
else
  echo "kit: no $HN to park"
fi

# 7. exact final status line
state
echo "STATUS preload=$st_pre launcher=$st_al carplay=$st_cp kit=$st_kit"
if [ "$st_pre" = absent ] && [ "$st_al" = stock ] && [ "$st_cp" = stock ] && [ "$st_kit" != active ]; then
  exit 0
fi
exit 1
