#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Host test of scripts/aa-session.sh against a fake root with fake mount/umount/
# pidof/kill/chsmack. No car, no VM, no real mount. Run: sh tests/test_aa_session.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
S="$HERE/../scripts/aa-session.sh"
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
fails=0; checks=0
ck() { checks=$((checks+1)); if ! eval "$1"; then echo "FAIL: $2"; fails=$((fails+1)); fi; }
shaof() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1; }

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
dst=$1
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
  cat > "$d/kill" <<'EOF'
#!/bin/sh
echo "kill $*" >> "$TLOG"
[ "${HUDNAV_TEST_NO_RESTART:-}" = 1 ] && exit 0
old=$(cat "$PIDFILE" 2>/dev/null); new=$((old + 1))
echo "$new" > "$PIDFILE"
mkdir -p "$HUDNAV_ROOT/proc/$new"
if [ "$(cat "$HUDNAV_ROOT/mounted" 2>/dev/null)" = 1 ]; then
  printf '/x /rw_data/hudnav/bin/aa_shim.so\n' > "$HUDNAV_ROOT/proc/$new/maps"
else
  printf '/x /app/bin/AppProjection\n' > "$HUDNAV_ROOT/proc/$new/maps"
fi
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
  chmod +x "$d"/*
}

mk() {
  r=$1
  rm -rf "$r"
  mkdir -p "$r/app/bin" "$r/rw_data/hudnav/bin" "$r/proc/900" "$r/etc" "$r/fakebin"
  printf 'stock binary\n' > "$r/app/bin/AppProjection"
  printf 'patched copy\n' > "$r/rw_data/hudnav/bin/AppProjection.aa"
  printf 'shim\n' > "$r/rw_data/hudnav/bin/aa_shim.so"
  ( cd "$r/rw_data/hudnav/bin" && sha256sum AppProjection.aa aa_shim.so > SHA256SUMS )
  printf 'rootfs / ext4 ro 0 0\nproc /proc proc rw 0 0\n' > "$r/proc/mounts"
  printf '/x /app/bin/AppProjection\n' > "$r/proc/900/maps"
  echo 900 > "$r/pid"
  echo 0 > "$r/mounted"
  shaof "$r/app/bin/AppProjection" > "$r/pin"
  : > "$r/rw_data/hudnav/aa-nav-enable"
  : > "$r/rw_data/hudnav/aa-trace"
  mktools "$r/fakebin"
  : > "$r/calls.log"
}

run() {
  r=$1; shift
  ( export HUDNAV_ROOT="$r" HUDNAV_PATH_PREPEND="$r/fakebin" HUDNAV_PIN_AP="$(cat "$r/pin")" \
      TLOG="$r/calls.log" PIDFILE="$r/pid" HUDNAV_KILL="$r/fakebin/kill" \
      HUDNAV_RESTART_TRIES=2 HUDNAV_RESTART_DELAY=0
    sh "$S" "$@" > "$r/stdout.txt" 2>&1; echo $? > "$r/rc" )
}

R1=$W/r1; mk "$R1"; run "$R1" status
ck '[ "$(cat "$R1/rc")" = 0 ]' "status exits 0"
ck 'grep -q "bind-mounted: no" "$R1/stdout.txt"' "status reports not bound"
ck 'grep -q "gate aa-nav-enable:.*present" "$R1/stdout.txt"' "status shows nav-enable gate"
ck 'grep -q "gate aa-trace:.*present" "$R1/stdout.txt"' "status shows trace gate"
ck 'grep -q "/etc/ld.so.preload:.*absent" "$R1/stdout.txt"' "status shows preload absent"
ck 'grep -q "STATUS bound=no stock_hash=yes shim_mapped=no preload=absent" "$R1/stdout.txt"' "status STATUS line"
ck '! grep -qE "^(mount|umount|kill) " "$R1/calls.log"' "status is read-only (no mount/umount/kill)"
ck '[ "$(cat "$R1/mounted")" = 0 ] && [ ! -e "$R1/app/bin/AppProjection.stock" ]' "status created no state files"

R2=$W/r2; mk "$R2"; run "$R2" start
ck '[ "$(cat "$R2/rc")" = 0 ]' "dry start exits 0"
ck 'grep -q "DRY RUN" "$R2/stdout.txt"' "dry start says DRY RUN"
ck '! grep -qE "^(mount|umount|kill|chsmack) " "$R2/calls.log"' "dry start changed nothing"
ck '! grep -q "none rw 0 0" "$R2/proc/mounts"' "dry start added no bind line"
ck 'grep -q "STATUS bound=no stock_hash=yes shim_mapped=no preload=absent" "$R2/stdout.txt"' "dry start STATUS line"

# ---- five refusal conditions ----
R3a=$W/r3a; mk "$R3a"; echo /rw_data/hudnav/bin/aa_shim.so > "$R3a/etc/ld.so.preload"
run "$R3a" start --yes
ck '[ "$(cat "$R3a/rc")" = 1 ]' "refuse 1: preload exits 1"
ck 'grep -q "REFUSED: /etc/ld.so.preload exists" "$R3a/stdout.txt"' "refuse 1: reason printed"
ck '! grep -qE "^(mount|umount|kill|chsmack) " "$R3a/calls.log"' "refuse 1: changed nothing"
ck 'grep -q "STATUS bound=no stock_hash=yes shim_mapped=no preload=present" "$R3a/stdout.txt"' "refuse 1: STATUS line"

R3b=$W/r3b; mk "$R3b"
( export HUDNAV_ROOT="$R3b" TLOG="$R3b/calls.log" PIDFILE="$R3b/pid"
  "$R3b/fakebin/mount" --bind "$R3b/rw_data/hudnav/bin/AppProjection.aa" "$R3b/app/bin/AppProjection" )
: > "$R3b/calls.log"
run "$R3b" start --yes
ck '[ "$(cat "$R3b/rc")" = 1 ]' "refuse 2: already bound exits 1"
ck 'grep -q "REFUSED: already bind-mounted" "$R3b/stdout.txt"' "refuse 2: reason printed"
ck '! grep -qE "^(mount|umount|kill|chsmack) " "$R3b/calls.log"' "refuse 2: changed nothing"
ck 'grep -q "STATUS bound=yes" "$R3b/stdout.txt"' "refuse 2: STATUS shows bound"

R3c=$W/r3c; mk "$R3c"; echo tampered > "$R3c/app/bin/AppProjection"
run "$R3c" start --yes
ck '[ "$(cat "$R3c/rc")" = 1 ]' "refuse 3: bad stock hash exits 1"
ck 'grep -q "does not match the stock pin" "$R3c/stdout.txt"' "refuse 3: reason printed"
ck '! grep -qE "^(mount|umount|kill|chsmack) " "$R3c/calls.log"' "refuse 3: changed nothing"
ck 'grep -q "STATUS bound=no stock_hash=no" "$R3c/stdout.txt"' "refuse 3: STATUS shows stock_hash=no"

R3d=$W/r3d; mk "$R3d"; echo tampered > "$R3d/rw_data/hudnav/bin/AppProjection.aa"
run "$R3d" start --yes
ck '[ "$(cat "$R3d/rc")" = 1 ]' "refuse 4: bad patched copy exits 1"
ck 'grep -q "kit files do not match SHA256SUMS" "$R3d/stdout.txt"' "refuse 4: reason printed"
ck '! grep -qE "^(mount|umount|kill|chsmack) " "$R3d/calls.log"' "refuse 4: changed nothing"

R3e=$W/r3e; mk "$R3e"; rm -f "$R3e/rw_data/hudnav/bin/aa_shim.so"
run "$R3e" start --yes
ck '[ "$(cat "$R3e/rc")" = 1 ]' "refuse 5: missing shim exits 1"
ck 'grep -q "aa_shim.so is missing" "$R3e/stdout.txt"' "refuse 5: reason printed"
ck '! grep -qE "^(mount|umount|kill|chsmack) " "$R3e/calls.log"' "refuse 5: changed nothing"

# ---- successful start ----
R4=$W/r4; mk "$R4"; run "$R4" start --yes
ck '[ "$(cat "$R4/rc")" = 0 ]' "successful start exits 0"
ck 'grep -q "^mount --bind" "$R4/calls.log"' "mount --bind recorded"
ck 'grep -q "^chsmack -e App::projection" "$R4/calls.log"' "copy labelled with App::projection"
ck 'grep -q "^kill -TERM 900" "$R4/calls.log"' "old AppProjection TERMed"
ck 'grep -q "none rw 0 0" "$R4/proc/mounts"' "bind line in proc/mounts"
ck '[ "$(cat "$R4/pid")" = 901 ]' "new pid reported by fake pidof"
ck 'grep -q aa_shim.so "$R4/proc/901/maps"' "shim mapped in new pid"
ck 'grep -q "new pid=901" "$R4/stdout.txt"' "script reports new pid"
ck 'grep -q "STATUS bound=yes stock_hash=no shim_mapped=yes preload=absent" "$R4/stdout.txt"' "successful start STATUS line"
ck '[ ! -e "$R4/etc/ld.so.preload" ]' "no preload created"

# ---- app never comes back: automatic stop path ----
R5=$W/r5; mk "$R5"; export HUDNAV_TEST_NO_RESTART=1
run "$R5" start --yes
unset HUDNAV_TEST_NO_RESTART
ck '[ "$(cat "$R5/rc")" = 1 ]' "no-restart start exits 1"
ck 'grep -q "did not come back" "$R5/stdout.txt"' "no-restart reason printed"
ck 'grep -q "^umount " "$R5/calls.log"' "automatic umount recorded"
ck 'grep -q "^kill -TERM" "$R5/calls.log"' "kill recorded"
ck '! grep -q "none rw 0 0" "$R5/proc/mounts"' "bind line removed by rollback"
ck '[ "$(cat "$R5/pid")" = 900 ]' "no new pid (fake never restarts)"
ck '[ "$(shaof "$R5/app/bin/AppProjection")" = "$(cat "$R5/pin")" ]' "stock hash restored after rollback"
ck 'grep -q "STATUS bound=no stock_hash=yes shim_mapped=no preload=absent" "$R5/stdout.txt"' "rollback STATUS line"

# ---- stop restores ----
R6=$W/r6; mk "$R6"; run "$R6" start --yes
: > "$R6/calls.log"
run "$R6" stop --yes
ck '[ "$(cat "$R6/rc")" = 0 ]' "stop --yes exits 0"
ck 'grep -q "^umount " "$R6/calls.log"' "umount recorded"
ck 'grep -q "^kill -TERM 901" "$R6/calls.log"' "kill recorded"
ck '[ "$(cat "$R6/pid")" = 902 ]' "stock binary restarted with new pid"
ck '! grep -q "none rw 0 0" "$R6/proc/mounts"' "no bind line after stop"
ck '[ "$(shaof "$R6/app/bin/AppProjection")" = "$(cat "$R6/pin")" ]' "stock_hash=yes after stop"
ck 'grep -q "STATUS bound=no stock_hash=yes shim_mapped=no preload=absent" "$R6/stdout.txt"' "stop STATUS line"
ck '[ -f "$R6/rw_data/hudnav/bin/AppProjection.aa" ] && [ -f "$R6/rw_data/hudnav/bin/aa_shim.so" ]' "stop deletes no files"

R7=$W/r7; mk "$R7"; run "$R7" start --yes
: > "$R7/calls.log"
run "$R7" stop
ck '[ "$(cat "$R7/rc")" = 1 ]' "stop without --yes exits 1"
ck 'grep -q "REFUSED: stop needs --yes" "$R7/stdout.txt"' "stop refusal reason"
ck '! grep -qE "^(umount|kill) " "$R7/calls.log"' "stop refusal changed nothing"
ck 'grep -q "none rw 0 0" "$R7/proc/mounts"' "stop refusal left bind intact"

# ---- syntax and line endings ----
ck 'sh -n "$S"' "aa-session.sh parses"
for f in "$S" "$HERE/../scripts/make-aa-session-kit.sh" "$HERE/../src/hudnav_setlabel.c"; do
  [ -f "$f" ] || continue
  ck '[ "$(grep -c "$(printf "\r")" "$f" || true)" = 0 ]' "zero CR: $f"
done

echo "aa_session: $checks checks, $fails failures"
[ "$fails" = 0 ]
