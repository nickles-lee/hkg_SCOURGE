#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Host test of scripts/cp-session.sh against a fake root with fake mount/umount/
# pidof/systemctl/touch/rm/chsmack. No car, no VM, no real mount. Run:
# sh tests/test_cp_session.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
S="$HERE/../scripts/cp-session.sh"
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
fails=0; checks=0
ck() { checks=$((checks+1)); if ! eval "$1"; then echo "FAIL: $2"; fails=$((fails+1)); fi; }
shaof() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1; }
lineof() { grep -n -F "$1" "$2" 2>/dev/null | head -1 | cut -d: -f1; }
# The old pid is read BEFORE the restart; the new-pid poll must come AFTER it.
order_ok() {
  r=$1
  g=$(lineof "touch $r/rw_data/hudnav/cp-rg-enable" "$r/calls.log")
  m=$(lineof "mount --bind" "$r/calls.log")
  s=$(lineof "systemctl restart" "$r/calls.log")
  [ -n "$g" ] && [ -n "$m" ] && [ -n "$s" ] && [ "$g" -lt "$m" ] && [ "$m" -lt "$s" ] &&
    tail -n "+$((s + 1))" "$r/calls.log" | grep -q '^pidof '
}

# ---- fake tools (they record arguments in $TLOG and simulate state via files) ----
mktools() {
  d=$1
  cat > "$d/mount" <<'EOF'
#!/bin/sh
echo "mount $*" >> "$TLOG"
[ "$1" = "--bind" ] || exit 0
src=$2; dst=$3
[ -f "$dst.stock" ] || cp "$dst" "$dst.stock"
cp "$src" "$dst"
printf '%s %s none rw 0 0\n' "$src" "$dst" >> "$HUDNAV_ROOT/proc/mounts"
echo 1 > "$HUDNAV_ROOT/mounted"
EOF
  cat > "$d/umount" <<'EOF'
#!/bin/sh
echo "umount $*" >> "$TLOG"
if [ "$1" != "-l" ] && [ "${HUDNAV_TEST_EBUSY:-}" = 1 ]; then
  echo "umount: target is busy" >&2
  exit 1
fi
dst=$1; [ "$dst" = "-l" ] && dst=$2
[ -f "$dst.stock" ] && cp "$dst.stock" "$dst"
grep -v -F " $dst none rw 0 0" "$HUDNAV_ROOT/proc/mounts" > "$HUDNAV_ROOT/proc/mounts.new" || true
mv "$HUDNAV_ROOT/proc/mounts.new" "$HUDNAV_ROOT/proc/mounts"
echo 0 > "$HUDNAV_ROOT/mounted"
EOF
  cat > "$d/pidof" <<'EOF'
#!/bin/sh
echo "pidof $*" >> "$TLOG"
cat "$PIDFILE" 2>/dev/null
EOF
  # Fake systemd: restart/start make a NEW pid unless HUDNAV_TEST_NO_RESTART=1;
  # the new process maps cp_rg.so only while the bind mount is in place.
  cat > "$d/systemctl" <<'EOF'
#!/bin/sh
echo "systemctl $*" >> "$TLOG"
case "$1" in
  restart|start)
    [ "${HUDNAV_TEST_NO_RESTART:-}" = 1 ] && exit 0
    old=$(cat "$PIDFILE" 2>/dev/null)
    [ -n "$old" ] || old=0
    new=$((old + 1))
    echo "$new" > "$PIDFILE"
    mkdir -p "$HUDNAV_ROOT/proc/$new"
    if [ "$(cat "$HUDNAV_ROOT/mounted" 2>/dev/null)" = 1 ]; then
      printf '/x /rw_data/hudnav/bin/cp_rg.so\n' > "$HUDNAV_ROOT/proc/$new/maps"
    else
      printf '/x /app/bin/iAP2Service-2.0\n' > "$HUDNAV_ROOT/proc/$new/maps"
    fi
    ;;
esac
EOF
  # Present for parity with test_aa_session.sh; cp-session.sh must not use kill
  # directly (systemd owns the restart), so any recorded call is a failure.
  cat > "$d/kill" <<'EOF'
#!/bin/sh
echo "kill $*" >> "$TLOG"
EOF
  cat > "$d/chsmack" <<'EOF'
#!/bin/sh
echo "chsmack $*" >> "$TLOG"
if [ "$1" = "-e" ]; then
  printf '%s\n' "$2" > "$HUDNAV_ROOT/label.txt"
  echo "chsmack: exec label $2 set on $3"
else
  printf '%s access="_" execute="App::projection"\n' "$1"
fi
EOF
  cat > "$d/touch" <<'EOF'
#!/bin/sh
echo "touch $*" >> "$TLOG"
exec /usr/bin/touch "$@"
EOF
  cat > "$d/rm" <<'EOF'
#!/bin/sh
echo "rm $*" >> "$TLOG"
exec /usr/bin/rm "$@"
EOF
  chmod +x "$d"/*
}

mk() {
  r=$1
  rm -rf "$r"
  mkdir -p "$r/app/bin" "$r/rw_data/hudnav/bin" "$r/proc/900" "$r/etc" "$r/fakebin"
  printf 'stock binary\n' > "$r/app/bin/iAP2Service-2.0"
  printf 'patched copy\n' > "$r/rw_data/hudnav/bin/iAP2Service-2.0.cp"
  printf 'library\n' > "$r/rw_data/hudnav/bin/cp_rg.so"
  ( cd "$r/rw_data/hudnav/bin" && sha256sum iAP2Service-2.0.cp cp_rg.so > SHA256SUMS.cp )
  printf 'rootfs / ext4 ro 0 0\nproc /proc proc rw 0 0\n' > "$r/proc/mounts"
  printf '/x /app/bin/iAP2Service-2.0\n' > "$r/proc/900/maps"
  echo 900 > "$r/pid"
  echo 0 > "$r/mounted"
  shaof "$r/app/bin/iAP2Service-2.0" > "$r/pin"
  mktools "$r/fakebin"
  : > "$r/calls.log"
}

run() {
  r=$1; shift
  ( export HUDNAV_ROOT="$r" HUDNAV_PATH_PREPEND="$r/fakebin" HUDNAV_PIN_CP="$(cat "$r/pin")" \
      TLOG="$r/calls.log" PIDFILE="$r/pid" \
      HUDNAV_RESTART_TRIES=2 HUDNAV_RESTART_DELAY=0
    sh "$S" "$@" > "$r/stdout.txt" 2>&1; echo $? > "$r/rc" )
}

NO_CHANGES='! grep -qE "^(mount|umount|systemctl|touch|rm|chsmack|kill) "'

# ---- status: read-only ----
R1=$W/r1; mk "$R1"; run "$R1" status
ck '[ "$(cat "$R1/rc")" = 0 ]' "status exits 0"
ck 'grep -q "bind-mounted:          no" "$R1/stdout.txt"' "status reports not bound"
ck 'grep -q "gate cp-rg-enable:     absent" "$R1/stdout.txt"' "status shows cp-rg-enable gate"
ck 'grep -q "gate cp-trace:         absent" "$R1/stdout.txt"' "status shows cp-trace gate"
ck 'grep -q "/etc/ld.so.preload:    absent" "$R1/stdout.txt"' "status shows preload absent"
ck '[ "$(tail -1 "$R1/stdout.txt")" = "STATUS bound=no stock_hash=yes lib_mapped=no preload=absent gate=off" ]' "status STATUS is final line"
ck "$NO_CHANGES \"$R1/calls.log\"" "status is read-only (no mount/umount/systemctl/touch/rm/chsmack/kill)"
ck '! grep -q "touch " "$R1/calls.log"' "status created no gate"
ck '[ "$(cat "$R1/mounted")" = 0 ] && [ ! -e "$R1/app/bin/iAP2Service-2.0.stock" ]' "status created no state files"

R1b=$W/r1b; mk "$R1b"; : > "$R1b/rw_data/hudnav/cp-rg-enable"; run "$R1b" status
ck '[ "$(cat "$R1b/rc")" = 0 ]' "status with gate on exits 0"
ck '[ "$(tail -1 "$R1b/stdout.txt")" = "STATUS bound=no stock_hash=yes lib_mapped=no preload=absent gate=on" ]' "status reports gate=on"

# ---- dry start changes nothing ----
R2=$W/r2; mk "$R2"; run "$R2" start
ck '[ "$(cat "$R2/rc")" = 0 ]' "dry start exits 0"
ck 'grep -q "DRY RUN" "$R2/stdout.txt"' "dry start says DRY RUN"
ck "$NO_CHANGES \"$R2/calls.log\"" "dry start changed nothing"
ck '! grep -q "none rw 0 0" "$R2/proc/mounts"' "dry start added no bind line"
ck '[ ! -e "$R2/rw_data/hudnav/cp-rg-enable" ]' "dry start created no gate"
ck '[ "$(tail -1 "$R2/stdout.txt")" = "STATUS bound=no stock_hash=yes lib_mapped=no preload=absent gate=off" ]' "dry start STATUS is final line"

# ---- refusal gates ----
R3a=$W/r3a; mk "$R3a"; echo /rw_data/hudnav/bin/cp_rg.so > "$R3a/etc/ld.so.preload"
run "$R3a" start --yes
ck '[ "$(cat "$R3a/rc")" = 1 ]' "refuse 1: preload exits 1"
ck 'grep -q "REFUSED: /etc/ld.so.preload exists" "$R3a/stdout.txt"' "refuse 1: reason printed"
ck "$NO_CHANGES \"$R3a/calls.log\"" "refuse 1: changed nothing"
ck '[ "$(tail -1 "$R3a/stdout.txt")" = "STATUS bound=no stock_hash=yes lib_mapped=no preload=present gate=off" ]' "refuse 1: STATUS final line"

R3b=$W/r3b; mk "$R3b"
( export HUDNAV_ROOT="$R3b" TLOG="$R3b/calls.log" PIDFILE="$R3b/pid"
  "$R3b/fakebin/mount" --bind "$R3b/rw_data/hudnav/bin/iAP2Service-2.0.cp" "$R3b/app/bin/iAP2Service-2.0" )
: > "$R3b/calls.log"
run "$R3b" start --yes
ck '[ "$(cat "$R3b/rc")" = 1 ]' "refuse 2: already bound exits 1"
ck 'grep -q "REFUSED: already bind-mounted" "$R3b/stdout.txt"' "refuse 2: reason printed"
ck "$NO_CHANGES \"$R3b/calls.log\"" "refuse 2: changed nothing"
ck 'grep -q "STATUS bound=yes" "$R3b/stdout.txt"' "refuse 2: STATUS shows bound"

R3c=$W/r3c; mk "$R3c"; echo tampered > "$R3c/app/bin/iAP2Service-2.0"
run "$R3c" start --yes
ck '[ "$(cat "$R3c/rc")" = 1 ]' "refuse 3: bad stock hash exits 1"
ck 'grep -q "does not match the stock pin" "$R3c/stdout.txt"' "refuse 3: reason printed"
ck "$NO_CHANGES \"$R3c/calls.log\"" "refuse 3: changed nothing"
ck 'grep -q "STATUS bound=no stock_hash=no" "$R3c/stdout.txt"' "refuse 3: STATUS shows stock_hash=no"

R3d=$W/r3d; mk "$R3d"; echo tampered > "$R3d/rw_data/hudnav/bin/iAP2Service-2.0.cp"
run "$R3d" start --yes
ck '[ "$(cat "$R3d/rc")" = 1 ]' "refuse 4: bad patched copy exits 1"
ck 'grep -q "kit files do not match SHA256SUMS.cp" "$R3d/stdout.txt"' "refuse 4: reason printed"
ck "$NO_CHANGES \"$R3d/calls.log\"" "refuse 4: changed nothing"

R3e=$W/r3e; mk "$R3e"; rm -f "$R3e/rw_data/hudnav/bin/cp_rg.so"
run "$R3e" start --yes
ck '[ "$(cat "$R3e/rc")" = 1 ]' "refuse 5: missing cp_rg.so exits 1"
ck 'grep -q "cp_rg.so is missing" "$R3e/stdout.txt"' "refuse 5: reason printed"
ck "$NO_CHANGES \"$R3e/calls.log\"" "refuse 5: changed nothing"

R3f=$W/r3f; mk "$R3f"; rm -f "$R3f/rw_data/hudnav/bin/SHA256SUMS.cp"
run "$R3f" start --yes
ck '[ "$(cat "$R3f/rc")" = 1 ]' "refuse 6: missing SHA256SUMS.cp exits 1"
ck 'grep -q "SHA256SUMS.cp is missing" "$R3f/stdout.txt"' "refuse 6: reason printed"
ck "$NO_CHANGES \"$R3f/calls.log\"" "refuse 6: changed nothing"

# ---- successful start (with --trace) ----
R4=$W/r4; mk "$R4"; run "$R4" start --yes --trace
ck '[ "$(cat "$R4/rc")" = 0 ]' "successful start exits 0"
ck 'grep -q "^mount --bind" "$R4/calls.log"' "mount --bind recorded"
ck 'grep -q "^touch .*cp-rg-enable" "$R4/calls.log"' "cp-rg-enable gate created"
ck 'grep -q "^touch .*cp-trace" "$R4/calls.log"' "cp-trace gate created on request"
ck 'grep -q "^chsmack -e App::projection" "$R4/calls.log"' "copy labelled with App::projection"
ck 'grep -q "^systemctl restart carplay_iAP2.service" "$R4/calls.log"' "systemd unit restarted"
ck 'grep -q "none rw 0 0" "$R4/proc/mounts"' "bind line in proc/mounts"
ck '[ "$(cat "$R4/pid")" = 901 ]' "new pid reported by fake pidof"
ck 'grep -q cp_rg.so "$R4/proc/901/maps"' "cp_rg.so mapped in new pid"
ck 'grep -q "new pid=901" "$R4/stdout.txt"' "script reports new pid"
ck 'order_ok "$R4"' "call order: gate, mount, systemctl restart, then new-pid poll"
ck '! grep -q "^kill " "$R4/calls.log"' "no direct kill (systemd owns the restart)"
ck '[ "$(tail -1 "$R4/stdout.txt")" = "STATUS bound=yes stock_hash=no lib_mapped=yes preload=absent gate=on" ]' "successful start STATUS is final line"
ck '[ ! -e "$R4/etc/ld.so.preload" ]' "no preload created"

# ---- service never returns: automatic stop path ----
R5=$W/r5; mk "$R5"; export HUDNAV_TEST_NO_RESTART=1
run "$R5" start --yes
unset HUDNAV_TEST_NO_RESTART
ck '[ "$(cat "$R5/rc")" = 1 ]' "no-restart start exits 1"
ck 'grep -q "did not come back" "$R5/stdout.txt"' "no-restart reason printed"
ck 'grep -q "^umount " "$R5/calls.log"' "automatic umount recorded"
ck 'grep -q "^systemctl stop carplay_iAP2.service" "$R5/calls.log"' "automatic systemctl stop recorded"
ck 'grep -q "^rm -f .*cp-rg-enable" "$R5/calls.log"' "automatic gate removal recorded"
ck '! grep -q "none rw 0 0" "$R5/proc/mounts"' "bind line removed by rollback"
ck '[ ! -e "$R5/rw_data/hudnav/cp-rg-enable" ]' "gate removed by rollback"
ck '[ "$(cat "$R5/pid")" = 900 ]' "no new pid (fake never restarts)"
ck '[ "$(shaof "$R5/app/bin/iAP2Service-2.0")" = "$(cat "$R5/pin")" ]' "stock hash restored after rollback"
ck '[ "$(tail -1 "$R5/stdout.txt")" = "STATUS bound=no stock_hash=yes lib_mapped=no preload=absent gate=off" ]' "rollback STATUS final line"

# ---- umount EBUSY then lazy umount ----
R6=$W/r6; mk "$R6"; run "$R6" start --yes; : > "$R6/calls.log"
export HUDNAV_TEST_EBUSY=1
run "$R6" stop --yes
unset HUDNAV_TEST_EBUSY
ck '[ "$(cat "$R6/rc")" = 0 ]' "lazy-umount stop exits 0"
ck 'grep -q "^umount /" "$R6/calls.log"' "plain umount attempted"
ck 'grep -q "^umount -l " "$R6/calls.log"' "lazy umount fallback recorded"
ck '! grep -q "none rw 0 0" "$R6/proc/mounts"' "bind line gone after lazy umount"
ck '[ "$(shaof "$R6/app/bin/iAP2Service-2.0")" = "$(cat "$R6/pin")" ]' "stock hash restored after lazy umount"
ck '[ "$(tail -1 "$R6/stdout.txt")" = "STATUS bound=no stock_hash=yes lib_mapped=no preload=absent gate=off" ]' "lazy-umount stop STATUS final line"

# ---- stop restores stock ----
R7=$W/r7; mk "$R7"; run "$R7" start --yes; : > "$R7/calls.log"
run "$R7" stop --yes
ck '[ "$(cat "$R7/rc")" = 0 ]' "stop --yes exits 0"
ck 'grep -q "^umount " "$R7/calls.log"' "umount recorded"
ck 'grep -q "^systemctl stop carplay_iAP2.service" "$R7/calls.log"' "systemctl stop recorded"
ck 'grep -q "^systemctl start carplay_iAP2.service" "$R7/calls.log"' "systemctl start recorded"
ck 'grep -q "^rm -f .*cp-rg-enable" "$R7/calls.log"' "gate removed"
ck '[ ! -e "$R7/rw_data/hudnav/cp-rg-enable" ]' "gate file gone"
ck '[ "$(cat "$R7/pid")" = 902 ]' "stock binary restarted with new pid"
ck '! grep -q "none rw 0 0" "$R7/proc/mounts"' "no bind line after stop"
ck '[ "$(shaof "$R7/app/bin/iAP2Service-2.0")" = "$(cat "$R7/pin")" ]' "stock_hash=yes after stop"
ck '[ "$(tail -1 "$R7/stdout.txt")" = "STATUS bound=no stock_hash=yes lib_mapped=no preload=absent gate=off" ]' "stop STATUS final line"
ck '[ -f "$R7/rw_data/hudnav/bin/iAP2Service-2.0.cp" ] && [ -f "$R7/rw_data/hudnav/bin/cp_rg.so" ]' "stop deletes no kit files"

# ---- stop without --yes refuses and leaves the session intact ----
R8=$W/r8; mk "$R8"; run "$R8" start --yes; : > "$R8/calls.log"
run "$R8" stop
ck '[ "$(cat "$R8/rc")" = 1 ]' "stop without --yes exits 1"
ck 'grep -q "REFUSED: stop needs --yes" "$R8/stdout.txt"' "stop refusal reason"
ck "$NO_CHANGES \"$R8/calls.log\"" "stop refusal changed nothing"
ck 'grep -q "none rw 0 0" "$R8/proc/mounts"' "stop refusal left bind intact"
ck '[ -e "$R8/rw_data/hudnav/cp-rg-enable" ]' "stop refusal left gate on"

# ---- syntax and line endings ----
ck 'sh -n "$S"' "cp-session.sh parses"
for f in "$S" "$HERE/../scripts/make-cp-session-kit.sh" "$0" "$HERE/../out/cp-session/cp-session.sh"; do
  [ -f "$f" ] || continue
  ck '[ "$(grep -c "$(printf "\r")" "$f" || true)" = 0 ]' "zero CR: $f"
done

echo "cp_session: $checks checks, $fails failures"
[ "$fails" = 0 ]
