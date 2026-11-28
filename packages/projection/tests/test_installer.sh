#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Offline installer/restore test for Projection+ on a fake root.
# Usage: sh tests/test_installer.sh
# Builds a small fake build/manifest.txt (two fake "car" layouts), stages it
# with scripts/stage_payload.sh, then drives run_stuff.sh and
# run_stuff_restore.sh with stubbed systemctl/dbus-send/chsmack/getfattr.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
TMP=$(mktemp -d /tmp/projection-installer-test.XXXXXX)
trap 'rm -rf "$TMP"' EXIT

PASS=0
FAILS=0
ok() { PASS=$((PASS + 1)); echo "ok   $*"; }
bad() { FAILS=$((FAILS + 1)); echo "FAIL $*"; }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
sha() { sha256sum "$1" | cut -d' ' -f1; }

FR="$TMP/root"
QD="$FR/app/share/AppHomeScreen/qml_eco3.0"
ADV="HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
WIDE="HomeScreen/QHMI_HomeScreenMain_Wide.qml"

# ---- fake build output (what build_artifacts.py produces) ------------------
mkdir -p "$TMP/build/qml_eco3.0/HomeScreen" "$TMP/base"
printf '// car base Advanced (Tweaks+WiFi patched)\nItem {}\n' > "$TMP/base/adv"
printf '// car base Wide\nItem {}\n' > "$TMP/base/wide"
printf '// PROJECTION_PAYLOAD Advanced\nItem { ProjectionTile {} }\n' > "$TMP/build/qml_eco3.0/$ADV"
printf '// PROJECTION_PAYLOAD Wide\nItem { ProjectionTile {} }\n' > "$TMP/build/qml_eco3.0/$WIDE"
# Both accepted formats: build_artifacts.py's 5-column rows and the 3-column form.
{
    echo "# kind dest source sha256(source) sha256(base)"
    printf 'patch\t/app/share/AppHomeScreen/qml_eco3.0/%s\tqml_eco3.0/%s\t%s\t%s\n' \
        "$ADV" "$ADV" "$(sha "$TMP/build/qml_eco3.0/$ADV")" "$(sha "$TMP/base/adv")"
    echo "build/qml_eco3.0/$WIDE,$(sha "$TMP/base/wide"),$(sha "$TMP/build/qml_eco3.0/$WIDE")"
} > "$TMP/build/manifest.txt"
printf '#!/bin/sh\necho fake projection_daemon\n' > "$TMP/fake_daemon"
PROJECTION_DAEMON_BIN="$TMP/fake_daemon" sh "$ROOT/scripts/stage_payload.sh" "$TMP/build" "$TMP/usb" > "$TMP/stage.log"
check "stage: manifest has layouts + app files" \
    '[ "$(grep -c . "$TMP/usb/manifest.txt")" -ge 10 ] && grep -q "^qml_eco3.0/$WIDE [0-9a-f]\{64\} " "$TMP/usb/manifest.txt"'

# ---- stubs -----------------------------------------------------------------
mkdir -p "$TMP/bin"
for t in systemctl dbus-send setfattr mount; do
    cat > "$TMP/bin/$t" <<EOF
#!/bin/sh
echo "$t \$*" >> "$TMP/calls.log"
exit 0
EOF
done
cat > "$TMP/bin/chsmack" <<EOF
#!/bin/sh
echo "chsmack \$*" >> "$TMP/calls.log"
[ "\${CHSMACK_FAIL:-0}" = 1 ] && exit 1
exit 0
EOF
cat > "$TMP/bin/getfattr" <<'EOF'
#!/bin/sh
case "$*" in
  *security.SMACK64EXEC*) printf '_' ;;
  *security.SMACK64*) printf '_' ;;
  *) exit 1 ;;
esac
EOF
chmod 755 "$TMP/bin/"*

export PROJECTION_FAKE_ROOT="$FR" INSTALL_IO_ROOT="$FR" INSTALL_IO_MOUNTS="$TMP/mounts"
export PROJECTION_SMACK_SELF=_ PROJECTION_DAEMON_SMACK=_ PROJECTION_ALLOW_NONROOT=1
export PROJECTION_RESTART_SLEEP=0 PROJECTION_PATH="$TMP/bin:/usr/bin:/bin"
unset USB_DIR 2>/dev/null || true
printf 'device %s ext4 rw 0 0\n' "$FR" > "$TMP/mounts"

make_car() {
    rm -rf "$FR"
    mkdir -p "$QD/HomeScreen" "$QD/component" "$FR/etc/systemd/system" \
             "$FR/rw_data/hudnav" "$FR/root/.cache/AppHomeScreen" "$FR/tmp"
    cp "$TMP/base/adv" "$QD/$ADV"
    cp "$TMP/base/wide" "$QD/$WIDE"
    printf 'Item {}\n' > "$QD/component/QHomeItem.qml"
    printf 'other.service\n' > "$FR/etc/systemd/system/other.service"
    : > "$FR/rw_data/hudnav/enabled"
    printf 'aa_steps=balanced\n' > "$FR/rw_data/hudnav/settings.conf"
    printf 'cache\n' > "$FR/root/.cache/AppHomeScreen/qmlcache"
}

# Snapshot of the fake root: type, path, sha256 / link target / mode.
# $1 = optional path prefix (relative) to exclude.
snap() {
    (cd "$FR" && find . | sort | while IFS= read -r f; do
        [ -n "${1:-}" ] && case "$f" in "./$1"|"./$1"/*) continue ;; esac
        if [ -L "$f" ]; then echo "L $f $(readlink "$f")"
        elif [ -d "$f" ]; then echo "D $f"
        else echo "F $f $(sha256sum "$f" | cut -d' ' -f1) $(stat -c %a "$f")"; fi
    done)
}

INST="$TMP/usb/run_stuff.sh"
run_install() { sh "$INST" --from-dir "$TMP/usb" "$@" > "$TMP/out.log" 2>&1; }
run_restore() { sh "$FR/rw_data/projection/tools/run_stuff_restore.sh" > "$TMP/out.log" 2>&1; }
res() { sed -n "s/^$1=//p" "$TMP/usb/run_stuff_result.txt" | head -n 1; }
rres() { sed -n "s/^$1=//p" "$FR/rw_data/projection/restore-result.txt" | head -n 1; }
pay() { sha "$TMP/usb/payload/$1"; }

# ---- 1. dry run changes nothing --------------------------------------------
make_car
snap > "$TMP/pristine"
if run_install --dry-run; then rc=0; else rc=$?; fi
snap > "$TMP/after"
check "dry-run: exit 0" '[ "$rc" = 0 ]'
check "dry-run: status DRY_RUN" '[ "$(res status)" = DRY_RUN ]'
check "dry-run: reports changes" '[ "$(res would_change)" -gt 0 ]'
check "dry-run: car byte-identical" 'cmp -s "$TMP/pristine" "$TMP/after"'
check "dry-run: no systemctl/dbus calls" '[ ! -s "$TMP/calls.log" ]'

# ---- 2. corrupted payload refuses ------------------------------------------
cp -a "$TMP/usb" "$TMP/usb.good"
printf ' ' >> "$TMP/usb/payload/qml_eco3.0/HomeScreen/ProjectionPage.qml"
if run_install; then rc=0; else rc=$?; fi
snap > "$TMP/after"
check "corrupt payload: refused" '[ "$rc" != 0 ] && [ "$(res status)" = FAIL ]'
check "corrupt payload: names the file" 'grep -q "corrupt.*ProjectionPage.qml" "$TMP/usb/run_stuff_result.txt"'
check "corrupt payload: car byte-identical" 'cmp -s "$TMP/pristine" "$TMP/after"'
rm -rf "$TMP/usb"; cp -a "$TMP/usb.good" "$TMP/usb"

# ---- 3. wrong base hash refuses --------------------------------------------
printf '// someone else patched this\n' >> "$QD/$WIDE"
snap > "$TMP/before"
if run_install; then rc=0; else rc=$?; fi
snap > "$TMP/after"
check "wrong base: refused" '[ "$rc" != 0 ] && [ "$(res status)" = FAIL ]'
check "wrong base: names the layout" 'grep -q "Wide.qml does not match the expected base" "$TMP/usb/run_stuff_result.txt"'
check "wrong base: car byte-identical" 'cmp -s "$TMP/before" "$TMP/after"'
check "wrong base: no /rw_data/projection" '[ ! -e "$FR/rw_data/projection" ]'
cp "$TMP/base/wide" "$QD/$WIDE"

# ---- 4. wrong SMACK label / foreign file / bad path refuse ------------------
if PROJECTION_SMACK_SELF=System run_install; then rc=0; else rc=$?; fi
snap > "$TMP/after"
check "smack label not _: refused, car unchanged" '[ "$rc" != 0 ] && grep -q "SMACK floor" "$TMP/usb/run_stuff_result.txt" && cmp -s "$TMP/pristine" "$TMP/after"'
printf 'foreign\n' > "$QD/component/ProjectionTile.qml"
snap > "$TMP/before"
if run_install; then rc=0; else rc=$?; fi
snap > "$TMP/after"
check "foreign file at new path: refused, car unchanged" '[ "$rc" != 0 ] && grep -q "unexpected existing file" "$TMP/usb/run_stuff_result.txt" && cmp -s "$TMP/before" "$TMP/after"'
rm -f "$QD/component/ProjectionTile.qml"
cp "$TMP/usb/manifest.txt" "$TMP/manifest.keep"
echo "qml_eco3.0/../../../../rw_data/hudnav/enabled new $(pay bin/projection_daemon)" >> "$TMP/usb/manifest.txt"
if run_install; then rc=0; else rc=$?; fi
snap > "$TMP/after"
check "path escaping into hudnav: refused, car unchanged" '[ "$rc" != 0 ] && cmp -s "$TMP/pristine" "$TMP/after"'
cp "$TMP/manifest.keep" "$TMP/usb/manifest.txt"

# ---- 5. fresh install -------------------------------------------------------
: > "$TMP/calls.log"
if run_install; then rc=0; else rc=$?; cat "$TMP/out.log"; fi
check "install: exit 0, status OK" '[ "$rc" = 0 ] && [ "$(res status)" = OK ]'
all_match=1
while read -r rel base p; do
    case "$rel" in
        qml_eco3.0/*|projection/*) d="$FR/app/share/AppHomeScreen/$rel" ;;
        bin/projection_daemon) d="$FR/rw_data/projection/projection_daemon" ;;
        systemd/*) d="$FR/etc/systemd/system/projection-daemon.service" ;;
        tools/*) d="$FR/rw_data/projection/$rel" ;;
    esac
    [ -f "$d" ] && [ "$(sha "$d")" = "$p" ] || { echo "  mismatch: $rel"; all_match=0; }
done < "$TMP/usb/manifest.txt"
check "install: every manifest file in place" '[ "$all_match" = 1 ]'
check "install: daemon executable" '[ -x "$FR/rw_data/projection/projection_daemon" ]'
check "install: daemon exec label _" 'grep -q "^chsmack -e _ $FR/rw_data/projection/projection_daemon$" "$TMP/calls.log"'
check "install: QML SMACK64 label re-applied" 'grep -q "^setfattr -n security.SMACK64 -v _ $QD/$ADV$" "$TMP/calls.log"'
check "install: unit enabled + restarted" 'grep -q "^systemctl enable projection-daemon.service$" "$TMP/calls.log" && grep -q "^systemctl restart projection-daemon.service$" "$TMP/calls.log"'
check "install: only our unit restarted" '[ "$(grep -c "^systemctl \(restart\|start\|stop\)" "$TMP/calls.log")" = 1 ]'
check "install: no live AppHomeScreen restart (car 2026-11-27), reboot asked" '! grep -q applauncher "$TMP/calls.log" && [ "$(res restart)" = reboot-required ]'
check "install: QML cache cleared" '[ ! -e "$FR/root/.cache/AppHomeScreen" ]'
check "install: hudnav untouched" '[ "$(snap | grep "rw_data/hudnav")" = "$(grep "rw_data/hudnav" "$TMP/pristine")" ]'
check "install: backup/latest complete" '[ "$(cat "$FR/rw_data/projection/backup/latest/state")" = complete ]'
check "install: backup holds original layout" 'cmp -s "$FR/rw_data/projection/backup/latest/files/qml_eco3.0/$ADV" "$TMP/base/adv"'
check "install: smack_check ok" '[ "$(res smack_check)" = ok ]'
snap rw_data/projection > "$TMP/installed"
BK1=$(cd "$FR/rw_data/projection/backup/latest" && pwd -P)

# ---- 6. re-run is idempotent -----------------------------------------------
: > "$TMP/calls.log"
if run_install; then rc=0; else rc=$?; cat "$TMP/out.log"; fi
snap rw_data/projection > "$TMP/after"
check "re-run: exit 0, status OK" '[ "$rc" = 0 ] && [ "$(res status)" = OK ]'
check "re-run: changed=0" '[ "$(res changed)" = 0 ]'
check "re-run: car files byte-identical" 'cmp -s "$TMP/installed" "$TMP/after"'
check "re-run: same backup, no new one" '[ "$(cd "$FR/rw_data/projection/backup/latest" && pwd -P)" = "$BK1" ] && [ "$(ls "$FR/rw_data/projection/backup" | grep -vc latest)" = 1 ]'
check "re-run: AppHomeScreen not restarted" '! grep -q applauncher "$TMP/calls.log" && [ "$(res restart)" = skipped-unchanged ]'

# ---- 6b. restore refuses when another package changed a file after install --
cp "$QD/$WIDE" "$TMP/wide.installed"
printf '// reinstalled by another package\n' >> "$QD/$WIDE"
snap > "$TMP/before"
if run_restore; then rc=0; else rc=$?; fi
snap > "$TMP/after"
grep -v "rw_data/projection/restore" "$TMP/before" > "$TMP/before.nr"
grep -v "rw_data/projection/restore" "$TMP/after" > "$TMP/after.nr"
check "restore conflict: refused" '[ "$rc" != 0 ] && [ "$(rres status)" = FAIL ] && grep -q "Wide.qml" "$FR/rw_data/projection/restore-conflicts.txt"'
check "restore conflict: nothing written" 'cmp -s "$TMP/before.nr" "$TMP/after.nr"'
cp "$TMP/wide.installed" "$QD/$WIDE"

# ---- 7. restore gives the byte-identical original ---------------------------
: > "$TMP/calls.log"
if run_restore; then rc=0; else rc=$?; cat "$TMP/out.log"; fi
snap rw_data/projection > "$TMP/after"
grep -v "root/.cache" "$TMP/pristine" | grep -v "rw_data/projection" > "$TMP/pristine.nc"
grep -v "root/.cache" "$TMP/after" > "$TMP/after.nc"
check "restore: exit 0, status OK" '[ "$rc" = 0 ] && [ "$(rres status)" = OK ]'
check "restore: car byte-identical to pristine (bar cache)" 'cmp -s "$TMP/pristine.nc" "$TMP/after.nc" || { diff "$TMP/pristine.nc" "$TMP/after.nc"; false; }'
check "restore: daemon + unit gone" '[ ! -e "$FR/rw_data/projection/projection_daemon" ] && [ ! -e "$FR/etc/systemd/system/projection-daemon.service" ]'
check "restore: unit stopped + disabled" 'grep -q "^systemctl stop projection-daemon.service$" "$TMP/calls.log" && grep -q "^systemctl disable projection-daemon.service$" "$TMP/calls.log"'
check "restore: no live AppHomeScreen restart" '! grep -q "request.launch int32:11" "$TMP/calls.log"'
check "restore: backup marked restored" '[ "$(cat "$FR/rw_data/projection/backup/latest/state")" = restored ]'
if run_restore; then rc=0; else rc=$?; fi
snap rw_data/projection | grep -v "root/.cache" > "$TMP/after.nc"
check "restore twice: still OK and identical" '[ "$rc" = 0 ] && cmp -s "$TMP/pristine.nc" "$TMP/after.nc"'

# ---- 8. reinstall after restore uses a fresh backup -------------------------
sleep 1
if run_install; then rc=0; else rc=$?; cat "$TMP/out.log"; fi
check "reinstall: OK with a new backup" '[ "$rc" = 0 ] && [ "$(cd "$FR/rw_data/projection/backup/latest" && pwd -P)" != "$BK1" ]'
printf '// reinstalled by another package\n' >> "$QD/$ADV"
if sh "$FR/rw_data/projection/tools/run_stuff_restore.sh" --force > "$TMP/out.log" 2>&1; then rc=0; else rc=$?; cat "$TMP/out.log"; fi
snap rw_data/projection | grep -v "root/.cache" > "$TMP/after.nc"
check "restore --force: overrides the conflict, byte-identical" '[ "$rc" = 0 ] && cmp -s "$TMP/pristine.nc" "$TMP/after.nc"'

# ---- 9. interrupted install, then restore -----------------------------------
# kill -9 after the Nth installed file: no traps run, as on a power cut.
interrupt_run() {
    sed "s|# @TEST_INTERRUPT_POINT|n_done=\$((\${n_done:-0} + 1)); [ \"\$n_done\" -ge $1 ] \&\& kill -9 \$\$|" \
        "$TMP/usb/run_stuff.sh" > "$TMP/usb/run_stuff_int.sh"
    if sh "$TMP/usb/run_stuff_int.sh" --from-dir "$TMP/usb" > "$TMP/out.log" 2>&1; then rc=0; else rc=$?; fi
}
n_tx=$(grep -vc "^tools/" "$TMP/usb/manifest.txt")
for point in 2 $((n_tx - 1)); do
    make_car
    rm -f "$TMP/usb/run_stuff_result.txt"
    interrupt_run "$point"
    check "interrupt@$point: installer was killed" '[ "$rc" = 137 ]'
    check "interrupt@$point: car is half-installed" '! snap rw_data/projection | grep -v "root/.cache" | cmp -s "$TMP/pristine.nc" -'
    if [ "$point" -gt 2 ]; then
        n_patched=0
        cmp -s "$QD/$ADV" "$TMP/base/adv" || n_patched=$((n_patched + 1))
        cmp -s "$QD/$WIDE" "$TMP/base/wide" || n_patched=$((n_patched + 1))
        check "interrupt@$point: exactly one layout patched" '[ "$n_patched" = 1 ]'
    fi
    check "interrupt@$point: backup pending" '[ "$(cat "$FR/rw_data/projection/backup/latest/state")" = pending ]'
    if run_restore; then rc=0; else rc=$?; cat "$TMP/out.log"; fi
    snap rw_data/projection | grep -v "root/.cache" > "$TMP/after.nc"
    check "interrupt@$point: restore OK, car byte-identical" '[ "$rc" = 0 ] && cmp -s "$TMP/pristine.nc" "$TMP/after.nc" || { diff "$TMP/pristine.nc" "$TMP/after.nc"; false; }'
done

# Interrupted, then the installer is simply re-run: it resumes the same backup.
make_car
interrupt_run $((n_tx - 1))
BKI=$(cd "$FR/rw_data/projection/backup/latest" && pwd -P)
if run_install; then rc=0; else rc=$?; cat "$TMP/out.log"; fi
check "interrupt+rerun: install completes on the same backup" '[ "$rc" = 0 ] && [ "$(cd "$FR/rw_data/projection/backup/latest" && pwd -P)" = "$BKI" ]'
run_restore || true
snap rw_data/projection | grep -v "root/.cache" > "$TMP/after.nc"
check "interrupt+rerun: restore byte-identical" 'cmp -s "$TMP/pristine.nc" "$TMP/after.nc"'
rm -f "$TMP/usb/run_stuff_int.sh"

# ---- 10. a failure mid-install rolls back -----------------------------------
make_car
if CHSMACK_FAIL=1 run_install; then rc=0; else rc=$?; fi
snap rw_data/projection | grep -v "root/.cache" > "$TMP/after.nc"
check "mid-install failure: FAIL + rollback ok" '[ "$rc" != 0 ] && [ "$(res rollback)" = ok ]'
check "mid-install failure: car byte-identical" 'cmp -s "$TMP/pristine.nc" "$TMP/after.nc" || { diff "$TMP/pristine.nc" "$TMP/after.nc"; false; }'

# ---- 11. optional: the real build against the car snapshot ------------------
# Runs only when build_artifacts.py output and the owner's car snapshot
# (gitignored base/car_*/qml_eco3.0) are present.
SNAP=$(ls -d "$ROOT"/base/car_*/qml_eco3.0 2>/dev/null | sort | tail -n 1)
if [ -f "$ROOT/build/manifest.txt" ] && [ -n "$SNAP" ]; then
    rm -rf "$TMP/usb"
    PROJECTION_DAEMON_BIN="$TMP/fake_daemon" sh "$ROOT/scripts/stage_payload.sh" "$ROOT/build" "$TMP/usb" > "$TMP/stage.log"
    make_car
    rm -rf "$QD"; cp -a "$SNAP" "$QD"
    snap > "$TMP/pristine"
    grep -v "root/.cache" "$TMP/pristine" > "$TMP/pristine.nc"
    if run_install --dry-run; then rc=0; else rc=$?; cat "$TMP/out.log"; fi
    check "real build: dry-run on car snapshot passes every base gate" '[ "$rc" = 0 ] && [ "$(res status)" = DRY_RUN ]'
    if run_install; then rc=0; else rc=$?; cat "$TMP/out.log"; fi
    check "real build: install OK" '[ "$rc" = 0 ] && [ "$(res status)" = OK ]'
    if run_install; then rc=0; else rc=$?; fi
    check "real build: re-run changed=0" '[ "$rc" = 0 ] && [ "$(res changed)" = 0 ]'
    if run_restore; then rc=0; else rc=$?; cat "$TMP/out.log"; fi
    snap rw_data/projection | grep -v "root/.cache" > "$TMP/after.nc"
    check "real build: restore byte-identical to the car snapshot" '[ "$rc" = 0 ] && cmp -s "$TMP/pristine.nc" "$TMP/after.nc"'
else
    echo "skip real build (no build/manifest.txt or base/car_*/qml_eco3.0)"
fi

echo "passed=$PASS failed=$FAILS"
[ "$FAILS" = 0 ]
