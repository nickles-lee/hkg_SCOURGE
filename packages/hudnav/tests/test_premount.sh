#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Host test of the new boot-time overlay path: persist/hudnav-premount.sh and
# persist/hudnav-premount.service. Fake root + fake mount/umount/chsmack/systemctl;
# no car, no VM, no real mount. Run: sh tests/test_premount.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
P="$HERE/../persist"
S="$P/hudnav-premount.sh"
U="$P/hudnav-premount.service"
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
fails=0; checks=0
ck() { checks=$((checks+1)); if ! eval "$1"; then echo "FAIL: $2"; fails=$((fails+1)); fi; }

# ---- fake tools: record every call in $TLOG, simulate proc/mounts and file content ----
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
EOF
  cat > "$d/umount" <<'EOF'
#!/bin/sh
echo "umount $*" >> "$TLOG"
d=$1; [ "$d" = "-l" ] && d=$2
[ -f "$d.stock" ] && cp "$d.stock" "$d"
if [ -f "$HUDNAV_ROOT/proc/mounts" ]; then
  grep -v -F " $d none rw 0 0" "$HUDNAV_ROOT/proc/mounts" > "$HUDNAV_ROOT/proc/mounts.new" || true
  mv "$HUDNAV_ROOT/proc/mounts.new" "$HUDNAV_ROOT/proc/mounts"
fi
EOF
  cat > "$d/chsmack" <<'EOF'
#!/bin/sh
echo "chsmack $*" >> "$TLOG"
if [ "$1" = "-e" ]; then
  printf '%s\n' "$2" > "$HUDNAV_ROOT/label.txt"
else
  printf '%s access="_" execute="App::projection"\n' "$1"
fi
EOF
  # If premount ever restarts or kills anything, the call lands here and a test fails.
  cat > "$d/systemctl" <<'EOF'
#!/bin/sh
echo "systemctl $*" >> "$TLOG"
EOF
  cat > "$d/kill" <<'EOF'
#!/bin/sh
echo "kill $*" >> "$TLOG"
EOF
  chmod +x "$d"/*
}

mk() {
  r=$1; rm -rf "$r"
  mkdir -p "$r/app/bin" "$r/rw_data/hudnav/bin" "$r/proc" "$r/fakebin" "$r/etc"
  printf 'stock AppProjection\n' > "$r/app/bin/AppProjection"
  printf 'stock iAP2Service\n' > "$r/app/bin/iAP2Service-2.0"
  printf 'patched AppProjection\n' > "$r/rw_data/hudnav/bin/AppProjection.aa"
  printf 'shim\n' > "$r/rw_data/hudnav/bin/aa_shim.so"
  printf 'patched iAP2Service\n' > "$r/rw_data/hudnav/bin/iAP2Service-2.0.cp"
  printf 'library\n' > "$r/rw_data/hudnav/bin/cp_rg.so"
  b="$r/rw_data/hudnav/bin"
  ( cd "$b" && sha256sum AppProjection.aa aa_shim.so > SHA256SUMS )
  ( cd "$b" && sha256sum iAP2Service-2.0.cp cp_rg.so > SHA256SUMS.cp )
  sha256sum "$r/app/bin/AppProjection" | cut -d' ' -f1 > "$r/pin_ap"
  sha256sum "$r/app/bin/iAP2Service-2.0" | cut -d' ' -f1 > "$r/pin_cp"
  printf 'rootfs / ext4 ro 0 0\n' > "$r/proc/mounts"
  : > "$r/rw_data/hudnav/enabled"
  : > "$r/rw_data/hudnav/aa-nav-enable"
  : > "$r/rw_data/hudnav/cp-enabled"
  mktools "$r/fakebin"
  : > "$r/calls.log"
}

run() { # run ROOT [extra env assignments via EXTRA]
  r=$1
  ( export HUDNAV_ROOT="$r" HUDNAV_PATH_PREPEND="$r/fakebin" TLOG="$r/calls.log" \
      HUDNAV_PIN_AP="$(cat "$r/pin_ap")" HUDNAV_PIN_CP="$(cat "$r/pin_cp")" ${EXTRA:-}
    sh "$S" > "$r/stdout.txt" 2>&1; echo $? > "$r/rc" )
}
NO_RESTART='! grep -qE "^(systemctl|kill) "'
STOCK_AP() { cmp -s "$1/app/bin/AppProjection" "$1/app/bin/AppProjection.stock" 2>/dev/null || grep -q '^stock AppProjection$' "$1/app/bin/AppProjection"; }
STOCK_CP() { cmp -s "$1/app/bin/iAP2Service-2.0" "$1/app/bin/iAP2Service-2.0.stock" 2>/dev/null || grep -q '^stock iAP2Service$' "$1/app/bin/iAP2Service-2.0"; }

# ---- unit file: ordering and no restart semantics ----
ck "sh -n '$S'" "premount script parses"
ck 'grep -q "^\[Unit\]" "$U" && grep -q "^After=local-fs.target$" "$U"' "unit: waits for local-fs.target (/rw_data mounted)"
ck 'grep -q "^Before=carplay_iAP2.service ApplicationLauncher.service hudnav-aa.service$" "$U"' "unit: ordered before CP, launcher and boot script"
ck 'grep -q "^Type=oneshot$" "$U" && grep -q "^RemainAfterExit=yes$" "$U"' "unit: oneshot, no daemon"
ck 'grep -q "^ExecStart=/bin/sh /rw_data/hudnav/bin/hudnav-premount.sh$" "$U"' "unit: correct ExecStart"
ck 'grep -q "^WantedBy=multi-user.target$" "$U"' "unit: enabled via multi-user.target"
ck '! grep -qE "^(ExecStartPost|ExecStop|Restart)=" "$U"' "unit: contains no service restart directive"
ck 'grep -q "After=.*hudnav-premount.service" "$P/hudnav-aa.service"' "boot script unit waits for premount explicitly"

# ---- kill switches and fail-closed refusals ----
R=$W/r1; mk "$R"; rm "$R/rw_data/hudnav/enabled"; EXTRA= run "$R"
ck '[ "$(cat "$R/rc")" = 0 ] && [ ! -s "$R/calls.log" ]' "no enabled -> no mount calls"
ck 'grep -q "not enabled" "$R/rw_data/hudnav/premount.log"' "no enabled -> logged stock boot"

R=$W/r2; mk "$R"; rm "$R/rw_data/hudnav/aa-nav-enable" "$R/rw_data/hudnav/cp-enabled"; run "$R"
ck '[ ! -s "$R/calls.log" ] && grep -q "no aa-nav-enable and no cp-enabled" "$R/rw_data/hudnav/premount.log"' "no overlay switches -> no mounts"

R=$W/r3; mk "$R"; echo /rw_data/hudnav/bin/aa_shim.so > "$R/etc/ld.so.preload"; run "$R"
ck '[ ! -s "$R/calls.log" ] && grep -q "REFUSE: .*ld.so.preload exists" "$R/rw_data/hudnav/premount.log"' "preload present -> refuse, no mounts"

R=$W/r4; mk "$R"; printf 'tampered\n' >> "$R/app/bin/AppProjection"; run "$R"
ck '[ ! -s "$R/calls.log" ] && grep -q "REFUSE: .*AppProjection sha256" "$R/rw_data/hudnav/premount.log"' "wrong stock AP hash -> refuse everything"

R=$W/r5; mk "$R"; printf 'tampered\n' >> "$R/rw_data/hudnav/bin/cp_rg.so"; run "$R"
ck '[ ! -s "$R/calls.log" ] && grep -q "REFUSE: SHA256SUMS.cp failed" "$R/rw_data/hudnav/premount.log"' "tampered CP kit -> refuse everything"

R=$W/r6; mk "$R"; rm "$R/rw_data/hudnav/bin/aa_shim.so"; run "$R"
ck '[ ! -s "$R/calls.log" ] && grep -q "REFUSE: missing" "$R/rw_data/hudnav/premount.log"' "missing kit file -> refuse everything"

# ---- happy path: both overlays, no restart ----
R=$W/g1; mk "$R"; run "$R"
ck '[ "$(cat "$R/rc")" = 0 ]' "happy path exits 0"
ck '[ "$(grep -c "^mount --bind " "$R/calls.log")" = 2 ]' "happy path: exactly two bind mounts"
ck 'grep -q "AA: bind mount verified" "$R/rw_data/hudnav/premount.log" && grep -q "CP: bind mount verified" "$R/rw_data/hudnav/premount.log"' "happy path: both mounts verified in proc/mounts"
ck 'grep -q "/app/bin/AppProjection none rw" "$R/proc/mounts" && grep -q "/app/bin/iAP2Service-2.0 none rw" "$R/proc/mounts"' "happy path: both targets appear in proc/mounts"
ck '[ -f "$R/rw_data/hudnav/cp-rg-enable" ]' "happy path: CP library gate created"
ck '[ "$(grep -c "^chsmack -e " "$R/calls.log")" = 2 ]' "happy path: both copies relabelled"
ck "$NO_RESTART \"$R/calls.log\"" "happy path: NO systemctl restart and NO kill"
ck 'grep -q "no service restarted" "$R/rw_data/hudnav/premount.log"' "happy path: log says no restart"

# ---- individual kill switches ----
R=$W/s1; mk "$R"; rm "$R/rw_data/hudnav/aa-nav-enable"; run "$R"
ck '[ "$(grep -c "^mount --bind " "$R/calls.log")" = 1 ] && grep -q "iAP2Service-2.0.cp" "$R/calls.log"' "aa-nav-enable off -> only CarPlay mounted"
ck 'grep -q "^stock AppProjection$" "$R/app/bin/AppProjection"' "aa-nav-enable off -> stock AppProjection untouched"
R=$W/s2; mk "$R"; rm "$R/rw_data/hudnav/cp-enabled"; run "$R"
ck '[ "$(grep -c "^mount --bind " "$R/calls.log")" = 1 ] && grep -q "AppProjection.aa" "$R/calls.log"' "cp-enabled off -> only AA mounted"
ck '[ ! -e "$R/rw_data/hudnav/cp-rg-enable" ]' "cp-enabled off -> no CP library gate"

# ---- idempotency: premount already applied ----
R=$W/i1; mk "$R"
printf '%s %s none rw 0 0\n' "$R/rw_data/hudnav/bin/AppProjection.aa" "$R/app/bin/AppProjection" >> "$R/proc/mounts"
cp "$R/rw_data/hudnav/bin/AppProjection.aa" "$R/app/bin/AppProjection"; : > "$R/calls.log"; run "$R"
ck '[ "$(grep -c "^mount --bind " "$R/calls.log")" = 1 ]' "already-bound AA is not mounted again"
ck 'grep -q "AA: already bind-mounted" "$R/rw_data/hudnav/premount.log"' "already-bound AA logged"

# ---- rollback: CP mount fails after AA succeeded ----
R=$W/f1; mk "$R"; EXTRA="HUDNAV_TEST_MOUNT_FAIL=1" run "$R"
ck '[ "$(cat "$R/rc")" = 0 ]' "CP mount failure still exits 0 (boot continues)"
ck "$NO_RESTART \"$R/calls.log\" && grep -q \"^umount \" \"$R/calls.log\"" "CP mount failure rolls back the AA mount"
ck 'grep -q "AA: stock restored after rollback" "$R/rw_data/hudnav/premount.log"' "rollback log: AA stock restored"
ck 'STOCK_AP "$R" && STOCK_CP "$R"' "rollback restores both stock binaries"
ck '[ ! -e "$R/rw_data/hudnav/cp-rg-enable" ]' "rollback removes the CP library gate"
ck '! grep -q "iAP2Service-2.0 none rw" "$R/proc/mounts"' "rollback leaves no CP bind line"

echo "premount: $checks checks, $fails failures"
[ "$fails" = 0 ]
