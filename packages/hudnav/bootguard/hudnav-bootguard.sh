#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# HUD Nav boot guard. Two modes:
#   early - before the UI: count unconfirmed boots, roll back on two in a row
#   ok    - after the UI: confirm AppHomeScreen stayed up and AppNavi did not segfault
# Host tests only. HUDNAV_ROOT is a test-only fake-root prefix (empty on the car).
# Test hooks: HUDNAV_PATH_PREPEND (fake pidof/dmesg), HUDNAV_GUARD_SCALE (shrink the windows),
# HUDNAV_PIN_AL / HUDNAV_PIN_CP (stock sha256 pins).
PATH="${HUDNAV_PATH_PREPEND:+$HUDNAV_PATH_PREPEND:}/bin:/sbin:/usr/bin:/usr/sbin"
R="${HUDNAV_ROOT:-}"

S="$R/rw_data/hudnav-bootguard"
LOG="$S/log"
AL="$R/app/bin/AppLauncher.sh"
CPU="$R/lib/systemd/system/carplay_iAP2.service"
PRE="$R/etc/ld.so.preload"
HN="$R/rw_data/hudnav"
BK="$HN/backup"
INFO="$R/log_data/backtrace/AppManager_info.txt"
DEREG="$R/log_data/backtrace/AppManager_deregister.txt"
IDX="$R/Data/mnt-wt/shared/public/resource-table-cfg.itz"
STOCK_AL="${HUDNAV_PIN_AL:-dc9487eb2f18218f20715881ea80011e597a29b778fc37ba754d90a8c4d4e39d}"
STOCK_CP="${HUDNAV_PIN_CP:-f7cf915509005a4aaa18d09e3a6ccd0abdd72ef2b2816641ea9a592a1e3f4495}"

shaof() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1; }

# Dated line to stdout. Used when not armed: writes nothing at all (test 1 needs a byte-identical root).
note() { printf '%s %s\n' "$(date)" "$*"; }

# Dated line in the state log, then keep only the last 200 lines.
log() {
  [ -d "$S" ] || mkdir -p "$S" 2>/dev/null || return 0
  printf '%s %s\n' "$(date)" "$*" >> "$LOG" 2>/dev/null || return 0
  t="$S/log.tmp"
  if tail -n 200 "$LOG" > "$t" 2>/dev/null; then mv "$t" "$LOG" 2>/dev/null; else rm -f "$t"; fi
}

# Car logs can contain non-printable bytes; always read them through this filter.
cleanlog() { tr -cd '[:print:]\n' < "$1" 2>/dev/null; }

# Number of "send_boot_type status[" lines in AppManager_info.txt (0 if missing).
count_boots() {
  n=0
  [ -f "$INFO" ] && n=$(cleanlog "$INFO" | grep -c 'send_boot_type status\[' 2>/dev/null)
  case "$n" in ''|*[!0-9]*) n=0 ;; esac
  echo "$n"
}

# Number of lines in AppManager_deregister.txt (0 if missing).
count_dereg() {
  n=0
  [ -f "$DEREG" ] && n=$(wc -l < "$DEREG" 2>/dev/null | tr -d ' ')
  case "$n" in ''|*[!0-9]*) n=0 ;; esac
  echo "$n"
}

# fails file: missing, empty or non-numeric means 0.
readfails() {
  f=0
  [ -f "$S/fails" ] && f=$(cat "$S/fails" 2>/dev/null)
  case "$f" in ''|*[!0-9]*) f=0 ;; esac
  echo "$f"
}

# Same revert as recovery/run_stuff.sh. Never deletes anything, never reboots.
rollback() {
  RW=0
  if [ -z "$R" ]; then
    if mount -o remount,rw / 2>&1; then RW=1; log "remount rw: ok"; else log "remount rw: FAILED (continuing)"; fi
  else
    log "test mode: remount rw skipped"
  fi

  rm -f "$PRE"
  if [ -e "$PRE" ]; then log "ld.so.preload STILL PRESENT (remove failed)"; else log "ld.so.preload removed"; fi

  if [ -f "$AL" ]; then
    now=$(shaof "$AL")
    if [ "$now" = "$STOCK_AL" ]; then
      log "AppLauncher.sh already stock"
    else
      t="$S/AL.new"
      sed 's|^export LD_PRELOAD=.*$|export LD_PRELOAD=/usr/lib/libEGL.so.1|' "$AL" > "$t"
      if [ "$(shaof "$t")" = "$STOCK_AL" ]; then
        cat "$t" > "$AL"; log "AppLauncher.sh restored by line edit"
      elif [ -f "$BK/AppLauncher.sh.stock" ] && [ "$(shaof "$BK/AppLauncher.sh.stock")" = "$STOCK_AL" ]; then
        cat "$BK/AppLauncher.sh.stock" > "$AL"; log "AppLauncher.sh restored from backup"
      else
        log "AppLauncher.sh NOT restored (neither edit nor backup gives the stock hash) - left untouched"
      fi
      rm -f "$t"
    fi
    log "AppLauncher.sh sha256 now: $(shaof "$AL") (stock $STOCK_AL)"
  else
    log "AppLauncher.sh MISSING"
  fi

  if [ -f "$CPU" ]; then
    now=$(shaof "$CPU")
    if [ "$now" = "$STOCK_CP" ]; then
      log "carplay_iAP2.service already stock"
    else
      t="$S/CP.new"
      grep -vxF 'Environment=LD_PRELOAD=/rw_data/hudnav/bin/cp_hook.so' "$CPU" > "$t"
      if [ "$(shaof "$t")" = "$STOCK_CP" ]; then
        cat "$t" > "$CPU"; log "carplay_iAP2.service restored by line removal"
      elif [ -f "$BK/carplay_iAP2.service.stock" ] && [ "$(shaof "$BK/carplay_iAP2.service.stock")" = "$STOCK_CP" ]; then
        cat "$BK/carplay_iAP2.service.stock" > "$CPU"; log "carplay_iAP2.service restored from backup"
      else
        log "carplay_iAP2.service NOT restored (no stock match) - left untouched"
      fi
      rm -f "$t"
    fi
    log "carplay_iAP2.service sha256 now: $(shaof "$CPU") (stock $STOCK_CP)"
  else
    log "carplay_iAP2.service MISSING"
  fi

  sync
  if [ "$RW" = 1 ]; then
    if mount -o remount,ro / 2>&1; then log "remount ro: ok"; else log "remount ro: failed"; fi
  else
    log "test mode: remount ro skipped"
  fi

  if [ -d "$HN" ]; then
    dest="$HN.off"
    [ -e "$dest" ] && dest="$HN.off.$(date +%s)"
    if mv "$HN" "$dest" 2>&1; then log "renamed $HN -> $dest"; else log "rename of $HN FAILED"; fi
  else
    log "no $HN"
  fi
}

early() {
  if [ ! -f "$S/armed" ]; then
    note "not armed"
    exit 0
  fi
  f=$(readfails)
  if [ -f "$S/pending" ]; then f=$((f + 1)); fi
  if [ "$f" -ge 2 ]; then
    log "early: rollback (fails=$f, pending present)"
    rollback
    printf '%s two consecutive boots without home-screen confirmation (fails=%s); rollback performed\n' "$(date)" "$f" > "$S/rolled_back"
    log "rolled_back written; guard disarmed"
    rm -f "$S/armed" "$S/pending"
    echo 0 > "$S/fails"
    exit 0
  fi
  # Baseline for this boot's ok run. Only when armed and NOT rolling back.
  echo "$(count_boots)" > "$S/boots_seen"
  echo "$(count_dereg)" > "$S/dereg_seen"
  echo "$f" > "$S/fails"
  : > "$S/pending"
  log "early: pending created, fails=$f"
  exit 0
}

# Look for a NEW "send_boot_type status[" line since early. Sets btype (N of the newest
# such line) and ignafter (number of ign3[1] lines after it). A shrunken file resets the
# baseline to 0. Called at every poll so a late ign3[1] can still upgrade the class to UI.
bootclass() {
  btype=""; ignafter=0; txt=""
  [ -f "$INFO" ] && txt=$(cleanlog "$INFO")
  blines=$(printf '%s\n' "$txt" | grep -c 'send_boot_type status\[' 2>/dev/null)
  case "$blines" in ''|*[!0-9]*) blines=0 ;; esac
  eff=$base
  [ "$blines" -lt "$eff" ] && eff=0
  [ "$blines" -gt "$eff" ] || return 0
  newest=$(printf '%s\n' "$txt" | grep -n 'send_boot_type status\[' | tail -n 1)
  bline=${newest%%:*}
  btype=$(printf '%s\n' "$newest" | sed -n 's/.*send_boot_type status\[\([0-9][0-9]*\)\].*/\1/p')
  if [ -n "$bline" ]; then
    ignafter=$(printf '%s\n' "$txt" | tail -n "+$((bline + 1))" | grep -c 'ign3\[1\]' 2>/dev/null)
    case "$ignafter" in ''|*[!0-9]*) ignafter=0 ;; esac
  fi
}

ok() {
  if [ ! -f "$S/armed" ]; then exit 0; fi
  n="${HUDNAV_GUARD_SCALE:-1}"
  case "$n" in ''|*[!0-9]*) n=1 ;; esac
  [ "$n" -lt 1 ] && n=1
  max=$((300 / n)); need=$((60 / n))
  [ "$max" -lt 1 ] && max=1
  [ "$need" -lt 1 ] && need=1
  log "ok: start (max=${max}s, need=${need}s)"
  base=$(cat "$S/boots_seen" 2>/dev/null)
  case "$base" in ''|*[!0-9]*) base=0 ;; esac
  dbase=$(cat "$S/dereg_seen" 2>/dev/null)
  case "$dbase" in ''|*[!0-9]*) dbase=0 ;; esac
  i=0; stableh=0; lasth=""; stablen=0; lastn=""; confirmed=0; cls="UNKNOWN"; why=""
  while [ "$i" -lt "$max" ]; do
    # First: classify this boot from the ApplicationManager info log.
    bootclass
    case "$btype" in
      10|32) cls=UI ;;
      *)
        if [ "$ignafter" -ge 1 ]; then cls=UI
        elif [ "$btype" = 43 ]; then cls=BG
        else cls=UNKNOWN
        fi ;;
    esac

    # Then: all health criteria must hold at this same poll.
    curh=$(pidof AppHomeScreen 2>/dev/null)
    curn=$(pidof AppNavi 2>/dev/null)
    seg=$(dmesg 2>/dev/null | grep -c 'AppNavi.*segfault')
    case "$seg" in ''|*[!0-9]*) seg=0 ;; esac
    if [ -n "$curh" ] && [ "$curh" = "$lasth" ]; then stableh=$((stableh + 1)); else lasth="$curh"; stableh=1; fi
    if [ -n "$curn" ] && [ "$curn" = "$lastn" ]; then stablen=$((stablen + 1)); else lastn="$curn"; stablen=1; fi
    idx3=$(head -c 3 "$IDX" 2>/dev/null)
    crash=0
    if [ -f "$DEREG" ]; then
      dn=$(wc -l < "$DEREG" 2>/dev/null | tr -d ' ')
      case "$dn" in ''|*[!0-9]*) dn=0 ;; esac
      deff=$dbase
      [ "$dn" -lt "$deff" ] && deff=0
      if [ "$dn" -gt "$deff" ]; then
        crash=$(tail -n "+$((deff + 1))" "$DEREG" | tr -cd '[:print:]\n' | grep -c -E 'a_nSigNum\[(11|6|5)\]' 2>/dev/null)
        case "$crash" in ''|*[!0-9]*) crash=0 ;; esac
      fi
    fi
    why=""
    [ -n "$curh" ] && [ "$stableh" -ge "$need" ] || why="$why home_pid"
    [ -n "$curn" ] && [ "$stablen" -ge "$need" ] || why="$why navi_pid"
    [ "$seg" -eq 0 ] || why="$why segfault"
    [ "$idx3" = "KdB" ] || why="$why index"
    [ "$crash" -eq 0 ] || why="$why dereg"
    if [ "$cls" = UI ] && [ -z "$why" ]; then confirmed=1; break; fi
    # Reviewer fix 2026-11-26: ignition-off wakes last only ~3 min (11-25: 13:11:56 -> sleep 13:14:54),
    # shorter than the 300 s window. Decide BACKGROUND after `need` s without ign3[1], before power-off.
    if [ "$cls" = BG ] && [ "$i" -ge "$need" ]; then break; fi
    i=$((i + 1))
    [ "$i" -lt "$max" ] && sleep 1
  done
  if [ "$confirmed" = 1 ]; then
    rm -f "$S/pending"
    echo 0 > "$S/fails"
    log "boot confirmed"
  elif [ "$cls" = BG ]; then
    rm -f "$S/pending"
    log "background wake: not counted"
  elif [ "$cls" = UNKNOWN ]; then
    log "boot class unknown: NOT confirmed"
  else
    log "boot NOT confirmed (timeout after ${max}s): failed:${why}"
  fi
  exit 0
}

case "${1:-}" in
  early) early ;;
  ok) ok ;;
  *) echo "usage: $0 {early|ok}" >&2; exit 2 ;;
esac
