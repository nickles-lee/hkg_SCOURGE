#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Host test of bootguard/hudnav-bootguard.sh against a fake root. No car, no network.
# Run through WSL: sh tests/test_bootguard.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
S="$HERE/../bootguard/hudnav-bootguard.sh"
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
fails=0; checks=0
ck() { checks=$((checks+1)); if ! eval "$1"; then echo "FAIL: $2"; fails=$((fails+1)); fi; }

mkfake() {  # mkfake ROOT : a fake car in patched state, plus fake pidof/dmesg
  r=$1; rm -rf "$r"
  mkdir -p "$r/etc" "$r/app/bin" "$r/lib/systemd/system" "$r/rw_data/hudnav/bin" "$r/rw_data/hudnav/backup" "$r/bin" \
           "$r/log_data/backtrace" "$r/Data/mnt-wt/shared/public"
  printf 'export LD_LIBRARY_PATH=/app/lib\nexport LD_PRELOAD=/usr/lib/libEGL.so.1\ncd /app/bin\n' > "$r/stock_al"
  printf '[Service]\nEnvironment=LD_LIBRARY_PATH=/app/lib\nExecStart=/app/bin/iAP2Service-2.0\n' > "$r/stock_cp"
  cp "$r/stock_al" "$r/rw_data/hudnav/backup/AppLauncher.sh.stock"
  cp "$r/stock_cp" "$r/rw_data/hudnav/backup/carplay_iAP2.service.stock"
  sed 's|^export LD_PRELOAD=.*$|export LD_PRELOAD=/usr/lib/libEGL.so.1:/rw_data/hudnav/bin/aa_shim.so|' "$r/stock_al" > "$r/app/bin/AppLauncher.sh"
  sed '/^Environment=LD_LIBRARY_PATH=/a Environment=LD_PRELOAD=/rw_data/hudnav/bin/cp_hook.so' "$r/stock_cp" > "$r/lib/systemd/system/carplay_iAP2.service"
  printf '/rw_data/hudnav/bin/aa_shim.so\n/rw_data/hudnav/bin/cp_hook.so\n' > "$r/etc/ld.so.preload"
  echo x > "$r/rw_data/hudnav/bin/aa_shim.so"
  printf '#!/bin/sh\ncase "$1" in\n  AppHomeScreen) cat "%s/pid_AppHomeScreen" 2>/dev/null;;\n  AppNavi) cat "%s/pid_AppNavi" 2>/dev/null;;\nesac\n' "$r" "$r" > "$r/bin/pidof"
  printf '#!/bin/sh\ncat "%s/dmesg.txt"\n' "$r" > "$r/bin/dmesg"
  chmod +x "$r/bin/"*
  : > "$r/dmesg.txt"; echo 4242 > "$r/pid_AppHomeScreen"; echo 5252 > "$r/pid_AppNavi"
  : > "$r/log_data/backtrace/AppManager_info.txt"
  : > "$r/log_data/backtrace/AppManager_deregister.txt"
  printf 'KdB-bin-data\n' > "$r/Data/mnt-wt/shared/public/resource-table-cfg.itz"
  PA=$(sha256sum "$r/stock_al" | cut -d' ' -f1); PC=$(sha256sum "$r/stock_cp" | cut -d' ' -f1)
}

arm() { mkdir -p "$1/rw_data/hudnav-bootguard"; : > "$1/rw_data/hudnav-bootguard/armed"; }
run() {  # run ROOT MODE OUTFILE [SCALE] - stdout goes outside the fake root
  r=$1; mode=$2; out=$3; sc=${4:-1}
  HUDNAV_ROOT="$r" HUDNAV_PATH_PREPEND="$r/bin" HUDNAV_GUARD_SCALE="$sc" HUDNAV_PIN_AL="$PA" HUDNAV_PIN_CP="$PC" sh "$S" "$mode" > "$out" 2>&1
}
snap() { (cd "$1" && { find . -type d | sort; find . -type f | sort | while IFS= read -r f; do sha256sum "$f"; done; }); }
ST="$W/stdout"; LOG() { echo "$1/rw_data/hudnav-bootguard/log"; }
INFO() { echo "$1/log_data/backtrace/AppManager_info.txt"; }
DEREG() { echo "$1/log_data/backtrace/AppManager_deregister.txt"; }
IDX() { echo "$1/Data/mnt-wt/shared/public/resource-table-cfg.itz"; }
bootui() { printf 'LWP[2913]: send_boot_type status[10] | (2026-11-26)10:40:03.457\nLWP[2913]: ign3[1] | (2026-11-26)10:40:14.333\n' >> "$(INFO "$1")"; }
bootbg() { printf 'LWP[2913]: send_boot_type status[43] | (2026-11-26)11:02:03.457\nLWP[2913]: ign3[0] | (2026-11-26)11:02:03.458\n' >> "$(INFO "$1")"; }
boot43ui() { printf 'LWP[2913]: send_boot_type status[43] | (2026-11-26)11:02:03.457\nLWP[2913]: ign3[1] | (2026-11-26)11:02:14.333\n' >> "$(INFO "$1")"; }

# 1. not armed: early and ok change nothing (fake root byte-identical).
R1="$W/r1"; mkfake "$R1"
snap "$R1" > "$W/s1.before"
run "$R1" early "$W/o1a"
run "$R1" ok "$W/o1b"
snap "$R1" > "$W/s1.after"
ck 'cmp -s "$W/s1.before" "$W/s1.after"' "1: not armed: fake root byte-identical after early + ok"
ck 'grep -q "not armed" "$W/o1a"' "1: early reports not armed on stdout"

# 2. armed, early -> pending exists, fails=0, no rollback, baselines recorded.
R2="$W/r2"; mkfake "$R2"; arm "$R2"
run "$R2" early "$W/o2"
ck '[ -f "$R2/rw_data/hudnav-bootguard/pending" ]' "2: pending created"
ck '[ "$(cat "$R2/rw_data/hudnav-bootguard/fails")" = 0 ]' "2: fails=0"
ck '[ "$(cat "$R2/rw_data/hudnav-bootguard/boots_seen")" = 0 ]' "2: boots_seen recorded"
ck '[ "$(cat "$R2/rw_data/hudnav-bootguard/dereg_seen")" = 0 ]' "2: dereg_seen recorded"
ck '[ -f "$R2/rw_data/hudnav-bootguard/armed" ]' "2: still armed"
ck '[ ! -f "$R2/rw_data/hudnav-bootguard/rolled_back" ]' "2: no rollback marker"
ck '[ -e "$R2/etc/ld.so.preload" ] && [ -d "$R2/rw_data/hudnav" ]' "2: fake car untouched"
ck 'grep -q "pending created" "$(LOG "$R2")"' "2: log line written"

# 3. armed + pending + healthy UI boot -> pending removed, fails=0, confirmed.
R3="$W/r3"; mkfake "$R3"; arm "$R3"
: > "$R3/rw_data/hudnav-bootguard/pending"; echo 1 > "$R3/rw_data/hudnav-bootguard/fails"
bootui "$R3"; : > "$R3/dmesg.txt"
run "$R3" ok "$W/o3" 20
ck '[ ! -e "$R3/rw_data/hudnav-bootguard/pending" ]' "3: pending removed"
ck '[ "$(cat "$R3/rw_data/hudnav-bootguard/fails")" = 0 ]' "3: fails reset to 0"
ck 'grep -q "boot confirmed" "$(LOG "$R3")"' "3: log says boot confirmed"
ck '[ -f "$R3/rw_data/hudnav-bootguard/armed" ] && [ -d "$R3/rw_data/hudnav" ]' "3: no rollback on a good boot"

# 4. UI boot with an AppNavi segfault line -> NOT confirmed, pending stays, fails unchanged.
R4="$W/r4"; mkfake "$R4"; arm "$R4"
: > "$R4/rw_data/hudnav-bootguard/pending"; echo 1 > "$R4/rw_data/hudnav-bootguard/fails"
bootui "$R4"
printf '[   12.345] AppNavi[1234]: segfault at 8 ip 00000000deadbeef\n' > "$R4/dmesg.txt"
run "$R4" ok "$W/o4" 20
ck '[ -f "$R4/rw_data/hudnav-bootguard/pending" ]' "4: pending left in place"
ck '[ "$(cat "$R4/rw_data/hudnav-bootguard/fails")" = 1 ]' "4: fails unchanged"
ck 'grep -q "boot NOT confirmed" "$(LOG "$R4")"' "4: log says NOT confirmed"
ck 'grep -q "segfault" "$(LOG "$R4")"' "4: log names the segfault criterion"

# 5. two consecutive early runs without ok, pending seeded from the first unconfirmed boot.
R5="$W/r5"; mkfake "$R5"; arm "$R5"
: > "$R5/rw_data/hudnav-bootguard/pending"   # a boot that never confirmed
run "$R5" early "$W/o5a"
ck '[ -e "$R5/etc/ld.so.preload" ] && [ -d "$R5/rw_data/hudnav" ]' "5: no rollback after the first early"
ck '[ "$(cat "$R5/rw_data/hudnav-bootguard/fails")" = 1 ]' "5: fails=1 after the first early"
run "$R5" early "$W/o5b"
ck '[ ! -e "$R5/etc/ld.so.preload" ]' "5: preload removed"
ck '[ "$(sha256sum "$R5/app/bin/AppLauncher.sh" | cut -d" " -f1)" = "$PA" ]' "5: AppLauncher.sh back to stock hash"
ck '[ "$(sha256sum "$R5/lib/systemd/system/carplay_iAP2.service" | cut -d" " -f1)" = "$PC" ]' "5: carplay unit back to stock hash"
ck '[ ! -e "$R5/rw_data/hudnav" ] && [ -f "$R5/rw_data/hudnav.off/bin/aa_shim.so" ]' "5: kit renamed, nothing deleted"
ck '[ -f "$R5/rw_data/hudnav-bootguard/rolled_back" ]' "5: rolled_back marker exists"
ck 'grep -q "consecutive boots" "$R5/rw_data/hudnav-bootguard/rolled_back"' "5: rolled_back has date and reason"
ck '[ ! -f "$R5/rw_data/hudnav-bootguard/armed" ] && [ ! -f "$R5/rw_data/hudnav-bootguard/pending" ]' "5: guard disarmed, pending cleared"
ck '[ "$(cat "$R5/rw_data/hudnav-bootguard/fails")" = 0 ]' "5: fails reset to 0"
ck 'grep -q "remount rw skipped" "$(LOG "$R5")"' "5: remount skipped in test mode"
ck 'grep -q "renamed" "$(LOG "$R5")"' "5: rename logged"

# 6. pid changes during the window (AppHomeScreen restarting) -> not confirmed.
R6="$W/r6"; mkfake "$R6"; arm "$R6"
: > "$R6/rw_data/hudnav-bootguard/pending"
bootui "$R6"
printf '#!/bin/sh\nn=$(cat "%s/pidcount" 2>/dev/null)\nn=$((n + 1))\necho "$n" > "%s/pidcount"\ncase "$1" in\n  AppNavi) echo 5252;;\n  *) echo $((2000 + n));;\nesac\n' "$R6" "$R6" > "$R6/bin/pidof"
chmod +x "$R6/bin/pidof"
: > "$R6/pidcount"; : > "$R6/dmesg.txt"
run "$R6" ok "$W/o6" 20
ck '[ -f "$R6/rw_data/hudnav-bootguard/pending" ]' "6: pending stays on pid churn"
ck 'grep -q "boot NOT confirmed" "$(LOG "$R6")"' "6: log says NOT confirmed"
ck '[ -f "$R6/rw_data/hudnav-bootguard/armed" ]' "6: no rollback on pid churn"

# 7a. background wake after early: status[43] + ign3[0] -> pending removed, fails unchanged.
R7="$W/r7"; mkfake "$R7"; arm "$R7"
echo 1 > "$R7/rw_data/hudnav-bootguard/fails"
run "$R7" early "$W/o7a" 100
bootbg "$R7"
run "$R7" ok "$W/o7b" 100
ck '[ ! -f "$R7/rw_data/hudnav-bootguard/pending" ]' "7a: background wake removes pending"
ck '[ "$(cat "$R7/rw_data/hudnav-bootguard/fails")" = 1 ]' "7a: background wake leaves fails unchanged"
ck 'grep -q "background wake: not counted" "$(LOG "$R7")"' "7a: log says background wake: not counted"
ck '[ -f "$R7/rw_data/hudnav-bootguard/armed" ] && [ -d "$R7/rw_data/hudnav" ]' "7a: no rollback on a background wake"

# 7b. UI boot, healthy -> confirmed.
R8="$W/r8"; mkfake "$R8"; arm "$R8"
run "$R8" early "$W/o8a" 100
bootui "$R8"
run "$R8" ok "$W/o8b" 100
ck '[ ! -f "$R8/rw_data/hudnav-bootguard/pending" ]' "7b: healthy UI boot removes pending"
ck '[ "$(cat "$R8/rw_data/hudnav-bootguard/fails")" = 0 ]' "7b: healthy UI boot resets fails"
ck 'grep -q "boot confirmed" "$(LOG "$R8")"' "7b: log says boot confirmed"

# 7c. UI boot, settings index missing -> NOT confirmed.
R9="$W/r9"; mkfake "$R9"; arm "$R9"
run "$R9" early "$W/o9a" 100
bootui "$R9"; rm -f "$(IDX "$R9")"
run "$R9" ok "$W/o9b" 100
ck '[ -f "$R9/rw_data/hudnav-bootguard/pending" ]' "7c: missing index leaves pending"
ck 'grep -q "boot NOT confirmed" "$(LOG "$R9")"' "7c: log says NOT confirmed"
ck 'grep -q "index" "$(LOG "$R9")"' "7c: log names the index criterion"

# 7d. UI boot, new AppNavi crash deregistration -> NOT confirmed.
R10="$W/r10"; mkfake "$R10"; arm "$R10"
run "$R10" early "$W/o10a" 100
bootui "$R10"
printf 'APPM_LWP[3428]: XXXXXXXXXX Registration::deregistration() a_nSigNum[11] a_strName[AppNavi] XXXXXXXXXX | time (2026-11-26 10:40:20)\n' >> "$(DEREG "$R10")"
run "$R10" ok "$W/o10b" 100
ck '[ -f "$R10/rw_data/hudnav-bootguard/pending" ]' "7d: new crash dereg leaves pending"
ck 'grep -q "boot NOT confirmed" "$(LOG "$R10")"' "7d: log says NOT confirmed"
ck 'grep -q "dereg" "$(LOG "$R10")"' "7d: log names the dereg criterion"

# 7e. UI boot, AppNavi pid changes -> NOT confirmed.
R11="$W/r11"; mkfake "$R11"; arm "$R11"
run "$R11" early "$W/o11a" 100
bootui "$R11"
printf '#!/bin/sh\ncase "$1" in\n  AppHomeScreen) echo 4242;;\n  AppNavi) n=$(cat "%s/navicount" 2>/dev/null); n=$((n + 1)); echo "$n" > "%s/navicount"; echo $((6000 + n));;\nesac\n' "$R11" "$R11" > "$R11/bin/pidof"
chmod +x "$R11/bin/pidof"
run "$R11" ok "$W/o11b" 30   # need=2 polls: one observation alone cannot be "stable"
ck '[ -f "$R11/rw_data/hudnav-bootguard/pending" ]' "7e: AppNavi pid churn leaves pending"
ck 'grep -q "boot NOT confirmed" "$(LOG "$R11")"' "7e: log says NOT confirmed"
ck 'grep -q "navi_pid" "$(LOG "$R11")"' "7e: log names the navi_pid criterion"

# 7f. no new send_boot_type line -> UNKNOWN, not confirmed, pending kept.
R12="$W/r12"; mkfake "$R12"; arm "$R12"
run "$R12" early "$W/o12a" 100
run "$R12" ok "$W/o12b" 100
ck '[ -f "$R12/rw_data/hudnav-bootguard/pending" ]' "7f: unknown class leaves pending"
ck 'grep -q "boot class unknown: NOT confirmed" "$(LOG "$R12")"' "7f: log says boot class unknown"
ck '[ "$(cat "$R12/rw_data/hudnav-bootguard/fails")" = 0 ]' "7f: fails unchanged on unknown class"

# 7g. status[43] followed by ign3[1] -> treated as UI and confirmed.
R13="$W/r13"; mkfake "$R13"; arm "$R13"
run "$R13" early "$W/o13a" 100
boot43ui "$R13"
run "$R13" ok "$W/o13b" 100
ck '[ ! -f "$R13/rw_data/hudnav-bootguard/pending" ]' "7g: status43+ign3[1] removes pending"
ck 'grep -q "boot confirmed" "$(LOG "$R13")"' "7g: status43+ign3[1] is treated as UI"

ck 'sh -n "$S"' "script parses"
echo "bootguard: $checks checks, $fails failures"
[ "$fails" = 0 ]
