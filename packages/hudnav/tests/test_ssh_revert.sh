#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Host test of recovery/ssh_revert.sh against a fake root. No car, no VM, no network.
# Run: sh tests/test_ssh_revert.sh   (the script is fed through stdin exactly as over SSH)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
S="$HERE/../recovery/ssh_revert.sh"
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
fails=0; checks=0
ck() { checks=$((checks+1)); if ! eval "$1"; then echo "FAIL: $2"; fails=$((fails+1)); fi; }

shaof() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1; }
hash_tree() { ( cd "$1" && find . -type f -print | sort | while read -r f; do printf '%s  %s\n' "$(sha256sum "$f" | cut -d' ' -f1)" "$f"; done ); }

mk() {  # mk ROOT : fake car in the patched state; also sets PA and PC (stock pins)
  r=$1; rm -rf "$r"; mkdir -p "$r/etc" "$r/app/bin" "$r/lib/systemd/system" "$r/rw_data/hudnav/bin" "$r/rw_data/hudnav/backup"
  printf 'export LD_LIBRARY_PATH=/app/lib\nexport LD_PRELOAD=/usr/lib/libEGL.so.1\ncd /app/bin\n' > "$r/stock_al"
  printf '[Service]\nEnvironment=LD_LIBRARY_PATH=/app/lib\nExecStart=/app/bin/iAP2Service-2.0\n' > "$r/stock_cp"
  cp "$r/stock_al" "$r/rw_data/hudnav/backup/AppLauncher.sh.stock"
  cp "$r/stock_cp" "$r/rw_data/hudnav/backup/carplay_iAP2.service.stock"
  sed 's|^export LD_PRELOAD=.*$|export LD_PRELOAD=/usr/lib/libEGL.so.1:/rw_data/hudnav/bin/aa_shim.so|' "$r/stock_al" > "$r/app/bin/AppLauncher.sh"
  sed '/^Environment=LD_LIBRARY_PATH=/a Environment=LD_PRELOAD=/rw_data/hudnav/bin/cp_hook.so' "$r/stock_cp" > "$r/lib/systemd/system/carplay_iAP2.service"
  printf '/rw_data/hudnav/bin/aa_shim.so\n/rw_data/hudnav/bin/cp_hook.so\n' > "$r/etc/ld.so.preload"
  echo x > "$r/rw_data/hudnav/bin/aa_shim.so"
  PA=$(shaof "$r/stock_al"); PC=$(shaof "$r/stock_cp")
}

runx() { # runx ROOT report|yes OUT : exports the hooks, feeds the script through stdin
  r=$1; mode=$2; out=$3
  export HUDNAV_ROOT="$r" HUDNAV_PIN_AL="$PA" HUDNAV_PIN_CP="$PC"
  if [ "$mode" = yes ]; then sh -s -- --yes < "$S" > "$out" 2>&1; else sh -s < "$S" > "$out" 2>&1; fi
  echo $? > "$out.rc"
}

# 1. report only: not a single byte in the fake root may change, and STATUS must show preload=present
R1="$W/r1"; mk "$R1"
hash_tree "$R1" > "$W/r1.before"
runx "$R1" report "$W/r1.out"
hash_tree "$R1" > "$W/r1.after"
ck 'cmp -s "$W/r1.before" "$W/r1.after"' "report-only: every file in fake root byte-identical"
ck 'grep -q "^STATUS preload=present " "$W/r1.out"' "report-only: STATUS shows preload=present"
ck 'grep -q "MODE report-only" "$W/r1.out"' "report-only: mode announced"
ck 'grep -q "WOULD: remove" "$W/r1.out"' "report-only: prints what it would do"
ck 'grep -q "WOULD: park" "$W/r1.out"' "report-only: prints the kit action"
ck '[ -d "$R1/rw_data/hudnav" ]' "report-only: kit still active (nothing parked)"

# 2. patched fake car -> all stock, kit parked, exact STATUS, exit 0
R2="$W/r2"; mk "$R2"
runx "$R2" yes "$W/r2.out"; rc2=$(cat "$W/r2.out.rc")
ck '[ "$rc2" = 0 ]' "--yes: exit 0"
ck '[ ! -e "$R2/etc/ld.so.preload" ]' "--yes: ld.so.preload removed"
ck '[ "$(shaof "$R2/app/bin/AppLauncher.sh")" = "$PA" ]' "--yes: AppLauncher.sh back to stock"
ck '[ "$(shaof "$R2/lib/systemd/system/carplay_iAP2.service")" = "$PC" ]' "--yes: carplay unit back to stock"
ck '[ ! -d "$R2/rw_data/hudnav" ] && [ -f "$R2/rw_data/hudnav.off/bin/aa_shim.so" ]' "--yes: kit parked, not deleted"
ck 'grep -q "^STATUS preload=absent launcher=stock carplay=stock kit=parked$" "$W/r2.out"' "--yes: exact STATUS line"
ck 'grep -q "test mode: no remount" "$W/r2.out"' "--yes: no remount in test mode"

# 3. second --yes is harmless: exit 0 and nothing lost
hash_tree "$R2" > "$W/r2.after1"
runx "$R2" yes "$W/r2b.out"; rc3=$(cat "$W/r2b.out.rc")
hash_tree "$R2" > "$W/r2.after2"
ck '[ "$rc3" = 0 ]' "second --yes: exit 0"
ck 'cmp -s "$W/r2.after1" "$W/r2.after2"' "second --yes: nothing changed"
ck '[ -f "$R2/rw_data/hudnav.off/bin/aa_shim.so" ]' "second --yes: parked kit intact"
ck 'grep -q "AppLauncher.sh already stock" "$W/r2b.out"' "second --yes: reports already stock"

# 4. pre-existing hudnav.off is preserved; kit goes to hudnav.off.<seconds>
R4="$W/r4"; mk "$R4"; mkdir -p "$R4/rw_data/hudnav.off"
printf 'keep me\n' > "$R4/rw_data/hudnav.off/keep.txt"
runx "$R4" yes "$W/r4.out"; rc4=$(cat "$W/r4.out.rc")
found=0
for d in "$R4"/rw_data/hudnav.off.*; do [ -d "$d" ] && [ -f "$d/bin/aa_shim.so" ] && found=1; done
ck '[ "$found" = 1 ]' "kit moved to hudnav.off.<seconds>"
ck '[ "$(cat "$R4/rw_data/hudnav.off/keep.txt" 2>/dev/null)" = "keep me" ]' "pre-existing hudnav.off content kept"
ck '[ "$rc4" = 0 ]' "existing off-dir: exit 0"
ck 'grep -q "^STATUS preload=absent launcher=stock carplay=stock kit=parked$" "$W/r4.out"' "existing off-dir: exact STATUS line"

# 5. unexpected AppLauncher.sh (neither sed edit nor backup matches) is left byte-identical
R5="$W/r5"; mk "$R5"
printf '# totally unexpected content\n' > "$R5/app/bin/AppLauncher.sh"
printf 'not the stock file\n' > "$R5/rw_data/hudnav/backup/AppLauncher.sh.stock"
cp "$R5/app/bin/AppLauncher.sh" "$W/r5.al.before"
runx "$R5" yes "$W/r5.out"; rc5=$(cat "$W/r5.out.rc")
ck '[ "$rc5" = 1 ]' "unexpected launcher: exit 1"
ck 'cmp -s "$W/r5.al.before" "$R5/app/bin/AppLauncher.sh"' "unexpected launcher: byte-identical"
ck 'grep -q "AppLauncher.sh NOT RESTORED" "$W/r5.out"' "unexpected launcher: NOT RESTORED reported"
ck 'grep -q "^STATUS preload=absent launcher=MODIFIED carplay=stock kit=parked$" "$W/r5.out"' "unexpected launcher: exact STATUS line"

# 6. car-like run: HUDNAV_ROOT UNSET (as on the car), report-only: must reach the STATUS line
#    (regression: `set -u` + a bare $HUDNAV_ROOT aborted the script before it did anything).
#    Report-only mode only READS this host's /etc, /app, /lib, /rw_data.
( unset HUDNAV_ROOT; sh -s < "$S" ) > "$W/r6.out" 2>&1
ck 'grep -q "^STATUS preload=" "$W/r6.out" && ! grep -q "unbound variable" "$W/r6.out"' "HUDNAV_ROOT unset: report-only runs to STATUS"

ck 'sh -n "$S"' "script parses"
echo "ssh_revert: $checks checks, $fails failures"
[ "$fails" = 0 ]
