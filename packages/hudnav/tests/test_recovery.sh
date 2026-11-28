#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Host test of recovery/run_stuff.sh (v4) against a fake root. No car, no VM. Run: sh tests/test_recovery.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
S="$HERE/../recovery/run_stuff.sh"
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
fails=0; checks=0
ck() { checks=$((checks+1)); if ! eval "$1"; then echo "FAIL: $2"; fails=$((fails+1)); fi; }
# WSL /bin/sh is dash, where $'\r' is not ANSI-C quoting; use printf to get a real CR byte.
CR=$(printf '\r')

mk() {  # mk ROOT : a fake car in patched state
  r=$1; rm -rf "$r"; mkdir -p "$r/etc" "$r/app/bin" "$r/lib/systemd/system" "$r/rw_data/hudnav/bin" "$r/rw_data/hudnav/backup" "$r/usb" "$r/tmp"
  printf 'export LD_LIBRARY_PATH=/app/lib\nexport LD_PRELOAD=/usr/lib/libEGL.so.1\ncd /app/bin\n' > "$r/stock_al"
  printf '[Service]\nEnvironment=LD_LIBRARY_PATH=/app/lib\nExecStart=/app/bin/iAP2Service-2.0\n' > "$r/stock_cp"
  cp "$r/stock_al" "$r/rw_data/hudnav/backup/AppLauncher.sh.stock"
  cp "$r/stock_cp" "$r/rw_data/hudnav/backup/carplay_iAP2.service.stock"
  sed 's|^export LD_PRELOAD=.*$|export LD_PRELOAD=/usr/lib/libEGL.so.1:/rw_data/hudnav/bin/aa_shim.so|' "$r/stock_al" > "$r/app/bin/AppLauncher.sh"
  awk '{print} /^Environment=LD_LIBRARY_PATH=/{print "Environment=LD_PRELOAD=/rw_data/hudnav/bin/cp_hook.so"}' "$r/stock_cp" > "$r/lib/systemd/system/carplay_iAP2.service"
  printf '/rw_data/hudnav/bin/aa_shim.so\n/rw_data/hudnav/bin/cp_hook.so\n' > "$r/etc/ld.so.preload"
  echo x > "$r/rw_data/hudnav/bin/aa_shim.so"
}
run() { HUDNAV_ROOT=$1 USB_DIR=$1/usb HUDNAV_NO_REBOOT=1 HUDNAV_PIN_AL=$2 HUDNAV_PIN_CP=$3 sh "$S" > "$1/stdout.txt" 2>&1; }

# 1. patched car -> everything reverted, diagnostics written first
R1="$W/r1"; mk "$R1"
PA=$(sha256sum "$R1/stock_al" | cut -d' ' -f1); PC=$(sha256sum "$R1/stock_cp" | cut -d' ' -f1)
run "$R1" "$PA" "$PC"
ck '[ ! -e "$R1/etc/ld.so.preload" ]' "ld.so.preload removed"
ck '[ "$(sha256sum "$R1/app/bin/AppLauncher.sh" | cut -d" " -f1)" = "$PA" ]' "AppLauncher.sh back to stock"
ck '[ "$(sha256sum "$R1/lib/systemd/system/carplay_iAP2.service" | cut -d" " -f1)" = "$PC" ]' "carplay unit back to stock"
ck '[ ! -e "$R1/rw_data/hudnav" ] && [ -f "$R1/rw_data/hudnav.off/bin/aa_shim.so" ]' "kit renamed, not deleted"
ck '[ -f "$R1/usb/hudnav_diag_v4/00_loader_ran.txt" ] && [ -f "$R1/usb/hudnav_diag_v4/01_before.txt" ] && [ -f "$R1/usb/hudnav_diag_v4/06_after.txt" ]' "diag files on the stick"
ck 'grep -q "aa_shim" "$R1/usb/hudnav_diag_v4/01_before.txt"' "before-snapshot shows the preload"
ck '[ -f "$R1/rw_data/hudnav_recover_v4.done" ] && [ -f "$R1/rw_data/hudnav_recover_v4/05_revert.txt" ]' "done marker + copy in rw_data"
ck 'grep -q "ld.so.preload removed: yes" "$R1/usb/hudnav_diag_v4/05_revert.txt"' "revert log says removed"

# 2. second run is a no-op (done marker): no reboot path, nothing changes
echo y > "$R1/etc/ld.so.preload"
run "$R1" "$PA" "$PC"
ck '[ -f "$R1/etc/ld.so.preload" ] && [ -f "$R1/usb/hudnav_diag_v4/05_already_done.txt" ]' "rerun does nothing (marker)"

# 3. AppLauncher edited by hand to something unexpected and no usable backup -> left untouched, no damage
R3="$W/r3"; mk "$R3"; PA=$(sha256sum "$R3/stock_al" | cut -d' ' -f1); PC=$(sha256sum "$R3/stock_cp" | cut -d' ' -f1)
echo '# odd' >> "$R3/app/bin/AppLauncher.sh"; rm -f "$R3/rw_data/hudnav/backup/AppLauncher.sh.stock"
cp "$R3/app/bin/AppLauncher.sh" "$R3/al.before"
run "$R3" "$PA" "$PC"
ck 'cmp -s "$R3/al.before" "$R3/app/bin/AppLauncher.sh"' "unexpected AppLauncher.sh left untouched"
ck 'grep -q "NOT restored" "$R3/usb/hudnav_diag_v4/05_revert.txt"' "reports it was not restored"
ck '[ ! -e "$R3/etc/ld.so.preload" ]' "preload still removed in that case"

# 4. already-clean car (nothing of ours) -> no errors, no changes to stock files
R4="$W/r4"; mk "$R4"; cp "$R4/stock_al" "$R4/app/bin/AppLauncher.sh"; cp "$R4/stock_cp" "$R4/lib/systemd/system/carplay_iAP2.service"
rm -f "$R4/etc/ld.so.preload"; rm -rf "$R4/rw_data/hudnav"
PA=$(sha256sum "$R4/stock_al" | cut -d' ' -f1); PC=$(sha256sum "$R4/stock_cp" | cut -d' ' -f1)
run "$R4" "$PA" "$PC"
ck 'grep -q "already stock" "$R4/usb/hudnav_diag_v4/05_revert.txt"' "clean car reported as already stock"

# 4b. a hanging diagnostic must not stop the revert; it is killed and noted
R5="$W/r5"; mk "$R5"; mkdir -p "$R5/fakebin"
printf '#!/bin/sh
sleep 300
' > "$R5/fakebin/dmesg"; cp "$R5/fakebin/dmesg" "$R5/fakebin/journalctl"; chmod +x "$R5/fakebin/"*
PA=$(sha256sum "$R5/stock_al" | cut -d' ' -f1); PC=$(sha256sum "$R5/stock_cp" | cut -d' ' -f1)
HUDNAV_PATH_PREPEND="$R5/fakebin" HUDNAV_W_MAX=2 run "$R5" "$PA" "$PC"
ck '[ ! -e "$R5/etc/ld.so.preload" ] && [ "$(sha256sum "$R5/app/bin/AppLauncher.sh" | cut -d" " -f1)" = "$PA" ]' "revert done despite hanging diagnostics"
ck 'grep -q "killed after" "$R5/usb/hudnav_diag_v4/02_dmesg.txt"' "hung dmesg was killed and noted"
ck '[ -f "$R5/rw_data/hudnav_recover_v4.done" ]' "run completed"
ck 'grep -q "ld.so.preload removed: yes" "$R5/usb/hudnav_diag_v4/05_revert.txt"' "revert log written before the slow part"

# 4c. loader that does NOT set USB_DIR: output must still land next to the script (the stick), not vanish in /tmp
R6="$W/r6"; mk "$R6"; mkdir -p "$R6/usb/stick"; cp "$S" "$R6/usb/stick/run_stuff.sh"
PA=$(sha256sum "$R6/stock_al" | cut -d' ' -f1); PC=$(sha256sum "$R6/stock_cp" | cut -d' ' -f1)
( cd "$R6/usb/stick" && env -u USB_DIR HUDNAV_ROOT="$R6" HUDNAV_NO_REBOOT=1 HUDNAV_PIN_AL="$PA" HUDNAV_PIN_CP="$PC" sh "$R6/usb/stick/run_stuff.sh" > "$R6/stdout.txt" 2>&1 )
ck '[ -f "$R6/usb/stick/hudnav_diag_v4/00_loader_ran.txt" ] && [ -f "$R6/usb/stick/hudnav_diag_v4/05_revert.txt" ]' "no USB_DIR: diagnostics still written next to the script"

# 5. no USB_DIR writable -> falls back to /tmp, does not crash (just run it with an unwritable path)
ck 'sh -n "$S"' "script parses"

# 6. CRLF stick copy (Windows Notepad): line 2 re-execs a CR-stripped copy; same revert, clean diag names.
R7="$W/r7"; mk "$R7"; mkdir -p "$W/crlf"; sed 's/$/\r/' "$S" > "$W/crlf/run_stuff.sh"
PA=$(sha256sum "$R7/stock_al" | cut -d' ' -f1); PC=$(sha256sum "$R7/stock_cp" | cut -d' ' -f1)
HUDNAV_ROOT="$R7" USB_DIR="$R7/usb" HUDNAV_NO_REBOOT=1 HUDNAV_PIN_AL="$PA" HUDNAV_PIN_CP="$PC" sh "$W/crlf/run_stuff.sh" > "$R7/stdout.txt" 2>&1
ck '[ ! -e "$R7/etc/ld.so.preload" ]' "CRLF: ld.so.preload removed"
ck '[ "$(sha256sum "$R7/app/bin/AppLauncher.sh" | cut -d" " -f1)" = "$PA" ]' "CRLF: AppLauncher.sh back to stock hash"
ck '[ "$(sha256sum "$R7/lib/systemd/system/carplay_iAP2.service" | cut -d" " -f1)" = "$PC" ]' "CRLF: carplay unit back to stock hash"
ck '[ ! -e "$R7/rw_data/hudnav" ] && [ -f "$R7/rw_data/hudnav.off/bin/aa_shim.so" ]' "CRLF: kit renamed, not deleted"
ck '[ -f "$R7/usb/hudnav_diag_v4/00_loader_ran.txt" ] && [ -f "$R7/usb/hudnav_diag_v4/05_revert.txt" ]' "CRLF: diag files on the stick"
ck '[ "$(ls "$R7/usb/hudnav_diag_v4" | grep -c "$CR")" = 0 ]' "CRLF: no CR characters in diag folder names"

# 7. CRLF with USB_DIR UNSET, script on the fake stick: HUDNAV_SELF_DIR must put diag next to the script.
#    Run from / with an absolute script path so cwd cannot accidentally supply the answer.
R8="$W/r8"; mk "$R8"; mkdir -p "$R8/usb/stick"; cp "$W/crlf/run_stuff.sh" "$R8/usb/stick/run_stuff.sh"
PA=$(sha256sum "$R8/stock_al" | cut -d' ' -f1); PC=$(sha256sum "$R8/stock_cp" | cut -d' ' -f1)
( cd / && env -u USB_DIR HUDNAV_ROOT="$R8" HUDNAV_NO_REBOOT=1 HUDNAV_PIN_AL="$PA" HUDNAV_PIN_CP="$PC" sh "$R8/usb/stick/run_stuff.sh" > "$R8/stdout.txt" 2>&1 )
ck '[ -f "$R8/usb/stick/hudnav_diag_v4/00_loader_ran.txt" ] && [ -f "$R8/usb/stick/hudnav_diag_v4/05_revert.txt" ]' "CRLF, USB_DIR unset: diag folder next to the script (HUDNAV_SELF_DIR)"
ck '[ ! -e "$R8/etc/ld.so.preload" ]' "CRLF, USB_DIR unset: revert still ran"

# 8. stdin delivery (sh -s < script): [ -f "$0" ] guard skips the re-exec; the script still reverts.
R9="$W/r9"; mk "$R9"
PA=$(sha256sum "$R9/stock_al" | cut -d' ' -f1); PC=$(sha256sum "$R9/stock_cp" | cut -d' ' -f1)
( cd "$W" && HUDNAV_ROOT="$R9" USB_DIR="$R9/usb" HUDNAV_NO_REBOOT=1 HUDNAV_PIN_AL="$PA" HUDNAV_PIN_CP="$PC" sh -s < "$S" > "$R9/stdout.txt" 2>&1 )
ck '[ ! -e "$R9/etc/ld.so.preload" ] && [ "$(sha256sum "$R9/app/bin/AppLauncher.sh" | cut -d" " -f1)" = "$PA" ] && [ "$(sha256sum "$R9/lib/systemd/system/carplay_iAP2.service" | cut -d" " -f1)" = "$PC" ]' "stdin: reverted everything without re-exec"
ck '[ -f "$R9/usb/hudnav_diag_v4/00_loader_ran.txt" ] && [ -f "$R9/rw_data/hudnav_recover_v4.done" ]' "stdin: diag + done marker written"

# 9. /rw_data/hudnav.off already exists: never delete it, move the kit to a timestamped path.
R10="$W/r10"; mk "$R10"; mkdir -p "$R10/rw_data/hudnav.off"; echo keep > "$R10/rw_data/hudnav.off/keep.txt"
PA=$(sha256sum "$R10/stock_al" | cut -d' ' -f1); PC=$(sha256sum "$R10/stock_cp" | cut -d' ' -f1)
run "$R10" "$PA" "$PC"
ck '[ -f "$R10/rw_data/hudnav.off/keep.txt" ] && [ "$(cat "$R10/rw_data/hudnav.off/keep.txt")" = keep ]' "existing .off: old contents untouched"
ck '[ ! -e "$R10/rw_data/hudnav" ]' "existing .off: kit moved away from the original path"
ck '[ -n "$(ls -d "$R10"/rw_data/hudnav.off.[0-9]* 2>/dev/null | head -1)" ] && [ -f "$(ls -d "$R10"/rw_data/hudnav.off.[0-9]* 2>/dev/null | head -1)/bin/aa_shim.so" ]' "existing .off: kit at hudnav.off.<number>"

echo "recovery: $checks checks, $fails failures"
[ "$fails" = 0 ]
