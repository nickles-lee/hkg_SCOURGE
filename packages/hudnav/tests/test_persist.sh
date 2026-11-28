#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Host test of persist/hudnav-aa-boot.sh, install-persist.sh, uninstall-persist.sh against a fake root.
# Run: sh tests/test_persist.sh   (no car, no VM)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
P="$HERE/../persist"
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
fails=0; checks=0
ck() { checks=$((checks+1)); if ! eval "$1"; then echo "FAIL: $2"; fails=$((fails+1)); fi; }

FB="$W/fakebin"; mkdir -p "$FB"
# fake pidof: prints the content of $FAKE_DIR/pid_<name> if present
cat > "$FB/pidof" <<'EOF'
#!/bin/sh
[ -f "$FAKE_DIR/pid_$1" ] && cat "$FAKE_DIR/pid_$1"
exit 0
EOF
# fake mount/umount: record the call and simulate /proc/mounts + the file change.
cat > "$FB/mount" <<'EOF'
#!/bin/sh
echo "mount $*" >> "$TLOG"
[ "$1" = "--bind" ] || exit 0
src=$2; dst=$3
[ -f "$dst.stock" ] || cp "$dst" "$dst.stock"
cp "$src" "$dst"
printf '%s %s none rw 0 0\n' "$src" "$dst" >> "$HUDNAV_ROOT/proc/mounts"
EOF
cat > "$FB/umount" <<'EOF'
#!/bin/sh
echo "umount $*" >> "$TLOG"
d=$1; [ "$d" = "-l" ] && d=$2
[ -f "$d.stock" ] && cp "$d.stock" "$d"
grep -v -F " $d none rw 0 0" "$HUDNAV_ROOT/proc/mounts" > "$HUDNAV_ROOT/proc/mounts.new" || true
mv "$HUDNAV_ROOT/proc/mounts.new" "$HUDNAV_ROOT/proc/mounts"
EOF
cat > "$FB/chsmack" <<'EOF'
#!/bin/sh
echo "chsmack $*" >> "$TLOG"
if [ "$1" = "-e" ]; then printf '%s\n' "$2" > "$HUDNAV_ROOT/label.txt"; else printf '%s access="_" execute="App::projection"\n' "$1"; fi
EOF
# Nothing at boot may restart a service or kill a process; record any attempt.
cat > "$FB/systemctl" <<'EOF'
#!/bin/sh
echo "systemctl $*" >> "$TLOG"
EOF
cat > "$FB/kill" <<'EOF'
#!/bin/sh
echo "kill $*" >> "$TLOG"
EOF
chmod 755 "$FB"/*

mkkit() {   # mkkit ROOT [aa_rc] : fake car with a verified kit whose scripts record their calls
  r=$1; rc=${2:-0}; rm -rf "$r"; mkdir -p "$r/rw_data/hudnav/bin" "$r/etc/systemd/system/multi-user.target.wants" "$r/tmp" "$r/fake" \
     "$r/Data/mnt-wt/shared/public" "$r/app/bin" "$r/proc"
  b="$r/rw_data/hudnav/bin"
  printf '#!/bin/sh\necho "aa-session $*" >> "%s/fake/calls"\nexit %s\n' "$r" "$rc" > "$b/aa-session.sh"
  printf '#!/bin/sh\necho "hudnav-run $*" >> "%s/fake/calls"\necho "started"\n' "$r" > "$b/hudnav-run.sh"
  printf '#!/bin/sh\necho "cp-session $*" >> "%s/fake/calls"\nexit 0\n' "$r" > "$b/cp-session.sh"
  for f in hudnavd micom-send micom-send-tp CANService AppProjection.aa aa_shim.so iAP2Service-2.0.cp cp_rg.so; do echo "$f" > "$b/$f"; done
  cp "$P/hudnav-aa-boot.sh" "$P/hudnav-premount.sh" "$P/install-persist.sh" "$P/uninstall-persist.sh" "$P"/*.service "$b/"
  cp "$HERE/../bootguard/hudnav-bootguard.sh" "$HERE/../storeguard/hudnav-storeguard.sh" "$b/"
  cp "$HERE/../bootguard/hudnav-bootguard.service" "$HERE/../bootguard/hudnav-bootok.service" "$HERE/../storeguard/hudnav-storeguard.service" "$b/"
  (cd "$b" && sha256sum aa-session.sh hudnav-run.sh hudnavd micom-send micom-send-tp CANService AppProjection.aa aa_shim.so > SHA256SUMS)
  (cd "$b" && sha256sum iAP2Service-2.0.cp cp_rg.so > SHA256SUMS.cp)
  printf 'stock AppProjection\n' > "$r/app/bin/AppProjection"
  printf 'stock iAP2Service\n' > "$r/app/bin/iAP2Service-2.0"
  sha256sum "$r/app/bin/AppProjection" | cut -d' ' -f1 > "$r/pin_ap"
  sha256sum "$r/app/bin/iAP2Service-2.0" | cut -d' ' -f1 > "$r/pin_cp"
  printf 'rootfs / ext4 ro 0 0\n' > "$r/proc/mounts"
  for n in resource-table-cfg.itz default-data.itz configurable-default-data.itz; do printf 'KdB%s' "$n" > "$r/Data/mnt-wt/shared/public/$n"; done
  echo "unix:abstract=/tmp/dbus-x,guid=1" > "$r/tmp/session_appmgr"
  echo 4242 > "$r/fake/pid_AppProjection"
}
boot() { ( export HUDNAV_ROOT=$1 HUDNAV_PATH_PREPEND=$FB FAKE_DIR=$1/fake TLOG=$1/fake/calls \
    HUDNAV_PIN_AP="$(cat "$1/pin_ap")" HUDNAV_PIN_CP="$(cat "$1/pin_cp")" \
    HUDNAV_WAIT_S=3 HUDNAV_SLEEP=0; sh "$1/rw_data/hudnav/bin/hudnav-aa-boot.sh" start ) > "$1/fake/out" 2>&1; }
prebind() { # prebind ROOT PATH COPY : simulate hudnav-premount's already-active bind mount
  printf '%s %s none rw 0 0\n' "$3" "$2" >> "$1/proc/mounts"; cp "$3" "$2"; }

# --- boot script ---
R="$W/b1"; mkkit "$R"; boot "$R"
ck '[ ! -f "$R/fake/calls" ] && grep -q "not enabled" "$R/rw_data/hudnav/boot.log"' "boot: not enabled -> nothing called"
: > "$R/rw_data/hudnav/enabled"; boot "$R"
ck 'grep -q "^hudnav-run start tx --yes$" "$R/fake/calls" && ! grep -q "^aa-session" "$R/fake/calls"' "boot: enabled -> hudnavd tx, NO session restart"
ck 'grep -q "AA overlay not active.*no restart" "$R/rw_data/hudnav/boot.log"' "boot: no premount overlay -> stock binary, no restart"
R="$W/b2b"; mkkit "$R"; : > "$R/rw_data/hudnav/enabled"
prebind "$R" "$R/app/bin/AppProjection" "$R/rw_data/hudnav/bin/AppProjection.aa"; boot "$R"
ck '! grep -qE "^(aa-session|systemctl|kill) " "$R/fake/calls" && grep -q "AA overlay already active.*no restart" "$R/rw_data/hudnav/boot.log"' "boot: premount bind already active -> detected, no restart"
R="$W/b2"; mkkit "$R"; : > "$R/rw_data/hudnav/enabled"; echo x > "$R/etc/ld.so.preload"; boot "$R"
ck '[ ! -f "$R/fake/calls" ] && grep -q "REFUSE: /etc/ld.so.preload" "$R/rw_data/hudnav/boot.log"' "boot: preload present -> refuse"
R="$W/b3"; mkkit "$R"; : > "$R/rw_data/hudnav/enabled"; echo tampered >> "$R/rw_data/hudnav/bin/aa_shim.so"; boot "$R"
ck '[ ! -f "$R/fake/calls" ] && grep -q "hash check failed" "$R/rw_data/hudnav/boot.log"' "boot: tampered kit -> refuse"
R="$W/b4"; mkkit "$R"; : > "$R/rw_data/hudnav/enabled"; rm "$R/rw_data/hudnav/bin/micom-send"; boot "$R"
ck '[ ! -f "$R/fake/calls" ] && grep -q "missing" "$R/rw_data/hudnav/boot.log"' "boot: missing helper -> refuse"
R="$W/b5"; mkkit "$R"; : > "$R/rw_data/hudnav/enabled"; rm "$R/fake/pid_AppProjection"; boot "$R"
ck 'grep -q "^hudnav-run stop$" "$R/fake/calls" && ! grep -q "^aa-session" "$R/fake/calls"' "boot: no AppProjection -> hudnavd stopped, no overlay"
R="$W/b6"; mkkit "$R" 1; : > "$R/rw_data/hudnav/enabled"; boot "$R"
ck '! grep -q "^aa-session" "$R/fake/calls" && ! grep -q "^hudnav-run stop$" "$R/fake/calls"' "boot: session script failure cannot affect boot path (never called)"
R="$W/b7"; mkkit "$R"; : > "$R/rw_data/hudnav/enabled"; rm "$R/tmp/session_appmgr"; boot "$R"
ck '[ ! -f "$R/fake/calls" ] && grep -q "no session bus" "$R/rw_data/hudnav/boot.log"' "boot: no session bus -> nothing started"
R="$W/b8"; mv "$W/b1" "$R"; mv "$R/rw_data/hudnav" "$R/rw_data/hudnav.off"; rm -f "$R/fake/calls"
( export HUDNAV_ROOT=$R HUDNAV_PATH_PREPEND=$FB FAKE_DIR=$R/fake HUDNAV_WAIT_S=3 HUDNAV_SLEEP=0; sh "$R/rw_data/hudnav.off/bin/hudnav-aa-boot.sh" start ) > /dev/null 2>&1
ck '[ ! -f "$R/fake/calls" ]' "boot: kit renamed by a rollback -> nothing happens"

# --- CarPlay step (2026-11-27) ---
mkcp() { printf '#!/bin/sh\necho "cp-session $*" >> "%s/fake/calls"\nexit %s\n' "$1" "${2:-0}" > "$1/rw_data/hudnav/bin/cp-session.sh"; echo 777 > "$1/fake/pid_iAP2Service-2.0"; }
R="$W/c1"; mkkit "$R"; mkcp "$R"; : > "$R/rw_data/hudnav/enabled"; boot "$R"
ck '! grep -q "^cp-session" "$R/fake/calls"' "cp: no cp-enabled -> CarPlay untouched"
R="$W/c2"; mkkit "$R"; mkcp "$R"; : > "$R/rw_data/hudnav/enabled"; : > "$R/rw_data/hudnav/cp-enabled"; boot "$R"
ck '! grep -q "^cp-session" "$R/fake/calls" && grep -q "CarPlay overlay not active.*no restart" "$R/rw_data/hudnav/boot.log"' "cp: enabled -> premount overlay expected, no restart"
R="$W/c3"; mkkit "$R"; mkcp "$R" 1; : > "$R/rw_data/hudnav/enabled"; : > "$R/rw_data/hudnav/cp-enabled"
prebind "$R" "$R/app/bin/iAP2Service-2.0" "$R/rw_data/hudnav/bin/iAP2Service-2.0.cp"; boot "$R"
ck '! grep -q "^cp-session" "$R/fake/calls" && grep -q "CarPlay overlay already active.*no restart" "$R/rw_data/hudnav/boot.log"' "cp: premount bind already active -> detected, no restart"
R="$W/c4"; mkkit "$R"; mkcp "$R"; rm "$R/fake/pid_iAP2Service-2.0"; : > "$R/rw_data/hudnav/enabled"; : > "$R/rw_data/hudnav/cp-enabled"; boot "$R"
ck '! grep -q "^cp-session" "$R/fake/calls" && grep -q "CarPlay left stock" "$R/rw_data/hudnav/boot.log"' "cp: no iAP2Service -> CarPlay left stock"
R="$W/c5"; mkkit "$R"; mkcp "$R"; : > "$R/rw_data/hudnav/cp-enabled"; boot "$R"
ck '[ ! -f "$R/fake/calls" ]' "cp: master switch off -> CarPlay untouched too"

# --- install / uninstall ---
inst() { ( export HUDNAV_ROOT=$1 HUDNAV_PATH_PREPEND=$FB; sh "$1/rw_data/hudnav/bin/install-persist.sh" $2 ) > "$1/fake/inst.out" 2>&1; echo $? > "$1/fake/inst.rc"; }
unin() { ( export HUDNAV_ROOT=$1 HUDNAV_PATH_PREPEND=$FB FAKE_DIR=$1/fake; sh "$1/rw_data/hudnav/bin/uninstall-persist.sh" $2 ) > "$1/fake/unin.out" 2>&1; echo $? > "$1/fake/unin.rc"; }
snapshot() { (cd "$1" && find . -path ./fake -prune -o -type f -print | sort | xargs sha256sum) > "$2"; }
UNITS="hudnav-storeguard.service hudnav-bootguard.service hudnav-bootok.service hudnav-premount.service hudnav-aa.service"
R="$W/i1"; mkkit "$R"; snapshot "$R" "$W/i1.before"; inst "$R" ""; snapshot "$R" "$W/i1.after"
ck '[ "$(cat "$R/fake/inst.rc")" = 0 ] && cmp -s "$W/i1.before" "$W/i1.after" && grep -q "REPORT ONLY" "$R/fake/inst.out"' "install: report-only changes nothing"
inst "$R" --yes
ck '[ "$(cat "$R/fake/inst.rc")" = 0 ] && grep -q INSTALLED "$R/fake/inst.out"' "install --yes succeeds"
ok=1; for u in $UNITS; do [ -f "$R/etc/systemd/system/$u" ] && [ -L "$R/etc/systemd/system/multi-user.target.wants/$u" ] || ok=0; done
ck '[ $ok = 1 ]' "install: 5 units + links (includes hudnav-premount.service)"
ck '[ -f "$R/rw_data/hudnav/enabled" ] && [ -f "$R/rw_data/hudnav/aa-nav-enable" ]' "install: switches on"
ck '[ -f "$R/rw_data/hudnav-bootguard/armed" ] && [ -f "$R/rw_data/hudnav-storeguard/armed" ] && [ -f "$R/rw_data/hudnav-storeguard/good/SHA256SUMS" ]' "install: both guards armed, good copy saved"
inst "$R" --yes
ck '[ "$(cat "$R/fake/inst.rc")" = 1 ] && grep -q "already exists" "$R/fake/inst.out"' "install twice -> refuse"
unin "$R" ""
ck '[ -f "$R/rw_data/hudnav/enabled" ] && grep -q "REPORT ONLY" "$R/fake/unin.out"' "uninstall: report-only changes nothing"
unin "$R" --yes
left=0; for u in $UNITS; do [ -e "$R/etc/systemd/system/$u" ] || [ -L "$R/etc/systemd/system/multi-user.target.wants/$u" ] && left=1; done
ck '[ $left = 0 ] && [ "$(cat "$R/fake/unin.rc")" = 0 ]' "uninstall --yes: units + links removed"
ck '[ ! -f "$R/rw_data/hudnav/enabled" ] && [ ! -f "$R/rw_data/hudnav-bootguard/armed" ] && [ ! -f "$R/rw_data/hudnav-storeguard/armed" ]' "uninstall: switches off, guards disarmed"
R="$W/i4"; mkkit "$R"; : > "$R/rw_data/hudnav/cp-rg-enable"; unin "$R" --yes
ck '[ ! -f "$R/rw_data/hudnav/cp-rg-enable" ]' "uninstall: premount CP library gate removed"
ck '[ -f "$R/rw_data/hudnav/bin/AppProjection.aa" ]' "uninstall: kit kept"
R="$W/i2"; mkkit "$R"; printf 'XXX' > "$R/Data/mnt-wt/shared/public/resource-table-cfg.itz"; inst "$R" --yes
ck '[ "$(cat "$R/fake/inst.rc")" = 1 ] && [ ! -f "$R/rw_data/hudnav/enabled" ] && [ ! -f "$R/etc/systemd/system/hudnav-aa.service" ]' "install refuses on an unhealthy settings store"
R="$W/i3"; mkkit "$R"; echo x > "$R/etc/ld.so.preload"; inst "$R" --yes
ck '[ "$(cat "$R/fake/inst.rc")" = 1 ] && [ ! -f "$R/rw_data/hudnav/enabled" ]' "install refuses with ld.so.preload present"

for s in "$P/hudnav-aa-boot.sh" "$P/hudnav-premount.sh" "$P/install-persist.sh" "$P/uninstall-persist.sh"; do ck "sh -n '$s'" "parses: $s"; done
echo "persist: $checks checks, $fails failures"
[ "$fails" = 0 ]
