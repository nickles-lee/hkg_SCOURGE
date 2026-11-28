#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# HUD Nav settings-store guard. Three modes:
#   save   - store a known-good copy of the three shared-public .itz index files
#   check  - boot hook: restore any missing/corrupt index BEFORE pas-daemon starts
#   status - read-only report
# Host tests only. HUDNAV_ROOT is a test-only fake-root prefix (empty on the car).
# The guard never contacts the car, never touches wt.itz/cached.itz and never
# touches any *-lock file (the persistence library recreates those itself).
PATH="${HUDNAV_PATH_PREPEND:+$HUDNAV_PATH_PREPEND:}/bin:/sbin:/usr/bin:/usr/sbin"
R="${HUDNAV_ROOT:-}"

P="$R/Data/mnt-wt/shared/public"
S="$R/rw_data/hudnav-storeguard"
GOOD="$S/good"
LOG="$S/log"
SUMS="$GOOD/SHA256SUMS"
NAMES='resource-table-cfg.itz default-data.itz configurable-default-data.itz'

# A valid .itz: exists, is non-empty, and its first three bytes are KdB.
valid() {
  [ -f "$1" ] && [ -s "$1" ] && [ "$(head -c 3 "$1" 2>/dev/null)" = 'KdB' ]
}

# Dated line to stdout ONLY - no filesystem change (all no-op exits use this).
note() { printf '%s %s\n' "$(date)" "$*"; }

# Dated line in the state log, then keep only the last 200 lines.
log() {
  [ -d "$S" ] || mkdir -p "$S" 2>/dev/null || return 0
  printf '%s %s\n' "$(date)" "$*" >> "$LOG" 2>/dev/null || return 0
  t="$S/log.tmp"
  if tail -n 200 "$LOG" > "$t" 2>/dev/null; then mv "$t" "$LOG" 2>/dev/null; else rm -f "$t"; fi
}

# A good copy is usable only if SHA256SUMS exists and every listed file matches.
sums_ok() {
  [ -f "$SUMS" ] || return 1
  (cd "$GOOD" && sha256sum -c SHA256SUMS) >/dev/null 2>&1
}

save() {
  # Validate ALL three before touching anything. On failure change nothing.
  for n in $NAMES; do
    if ! valid "$P/$n"; then
      note "store not healthy, not saving ($n is missing, empty or has no KdB header)"
      exit 1
    fi
  done
  if [ ! -d "$GOOD" ] && ! mkdir -p "$GOOD" 2>/dev/null; then
    note "cannot create $GOOD, not saving"
    exit 1
  fi
  for n in $NAMES; do
    if ! cp "$P/$n" "$GOOD/$n.tmp" 2>/dev/null; then
      note "copy of $n failed, not saving"
      exit 1
    fi
    if ! mv "$GOOD/$n.tmp" "$GOOD/$n" 2>/dev/null; then
      note "replace of $n failed"
      exit 1
    fi
    printf '%s saved %s from %s\n' "$(date)" "$n" "$P/$n"
    log "saved $n from $P/$n"
  done
  if ! (cd "$GOOD" && sha256sum $NAMES > SHA256SUMS.tmp) 2>/dev/null; then
    note "sha256sum failed, not saving"
    exit 1
  fi
  if ! mv "$GOOD/SHA256SUMS.tmp" "$SUMS" 2>/dev/null; then
    note "could not rewrite SHA256SUMS"
    exit 1
  fi
  log "SHA256SUMS rewritten"
  exit 0
}

check() {
  # A guard must never block boot: every exit here is 0 and every no-op writes
  # only to stdout (no state files are created or changed).
  if [ ! -f "$S/armed" ]; then
    note "not armed"
    exit 0
  fi
  if ! sums_ok; then
    note "no valid good copy (SHA256SUMS missing or mismatch)"
    exit 0
  fi
  for n in $NAMES; do
    if valid "$P/$n"; then
      continue
    fi
    if ! cp "$GOOD/$n" "$P/$n.storeguard.tmp" 2>/dev/null; then
      note "restore of $n FAILED (cp)"
      continue
    fi
    chmod 666 "$P/$n.storeguard.tmp" 2>/dev/null
    if ! mv "$P/$n.storeguard.tmp" "$P/$n" 2>/dev/null; then
      note "restore of $n FAILED (mv)"
      rm -f "$P/$n.storeguard.tmp" 2>/dev/null
      continue
    fi
    log "restored $n"
  done
  exit 0
}

status() {
  if [ -f "$S/armed" ]; then echo "armed: yes"; else echo "armed: no"; fi
  for n in $NAMES; do
    if valid "$P/$n"; then echo "live $n: valid"; else echo "live $n: MISSING OR INVALID"; fi
  done
  if sums_ok; then
    echo "good copy: present and SHA256-valid ($GOOD)"
  else
    echo "good copy: MISSING OR INVALID ($GOOD)"
  fi
  if [ -f "$LOG" ]; then
    echo "last 5 log lines:"
    tail -n 5 "$LOG"
  else
    echo "last 5 log lines: (none)"
  fi
  exit 0
}

case "${1:-}" in
  save) save ;;
  check) check ;;
  status) status ;;
  *) echo "usage: $0 {save|check|status}" >&2; exit 2 ;;
esac
