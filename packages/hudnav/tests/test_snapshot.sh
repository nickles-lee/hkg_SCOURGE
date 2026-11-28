#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Host test of recovery/car_snapshot.sh against a fake root. No car, no VM, no network.
# Run: sh tests/test_snapshot.sh   (the script is fed through stdin exactly as over SSH)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
S="$HERE/../recovery/car_snapshot.sh"
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
fails=0; checks=0
ck() { checks=$((checks+1)); if ! eval "$1"; then echo "FAIL: $2"; fails=$((fails+1)); fi; }

hash_tree() { ( cd "$1" && find . -type f -print | sort | while read -r f; do printf '%s  %s\n' "$(sha256sum "$f" | cut -d' ' -f1)" "$f"; done ); }
snap() { # snap ROOT TMPDIR OUT : feed the script through stdin like the car
  export HUDNAV_ROOT="$1" TMPDIR="$2"
  sh -s < "$S" > "$3" 2> "$3.err"
  echo $? > "$3.rc"
}

R="$W/root"; T="$W/tmp"; mkdir -p "$R" "$T"
mkdir -p "$R/Data/mnt-wt/shared/public" "$R/Data/mnt-backup" "$R/etc" "$R/app/bin" "$R/lib/systemd/system" "$R/rw_data/tweaks"
printf 'hello wt\n' > "$R/Data/mnt-wt/shared/public/a.txt"
printf 'other wt file\n' > "$R/Data/mnt-wt/other.txt"
printf 'hello backup\n' > "$R/Data/mnt-backup/b.txt"
printf '/rw_data/hudnav/bin/aa_shim.so\n' > "$R/etc/ld.so.preload"
printf 'export LD_PRELOAD=/usr/lib/libEGL.so.1\n' > "$R/app/bin/AppLauncher.sh"
printf '[Service]\n' > "$R/lib/systemd/system/carplay_iAP2.service"
printf 'key=value\n' > "$R/rw_data/tweaks/config.conf"

hash_tree "$R" > "$W/root.before"
snap "$R" "$T" "$W/out.tgz"
rc=$(cat "$W/out.tgz.rc")
hash_tree "$R" > "$W/root.after"

ck '[ "$rc" = 0 ]' "snapshot exit 0"
ck '[ -s "$W/out.tgz" ]' "gzip tar written to stdout"
ck '[ -s "$W/out.tgz.err" ]' "all messages went to stderr"
tar -tzf "$W/out.tgz" > "$W/list.txt" 2> "$W/tar.err"
ck '[ ! -s "$W/tar.err" ]' "tar listing succeeded"
ck 'grep -qx "status.txt" "$W/list.txt"' "archive lists status.txt"
ck 'grep -qx "Data/mnt-wt/shared/public/a.txt" "$W/list.txt"' "archive lists Data/mnt-wt/shared/public/a.txt"
ck 'grep -qx "Data/mnt-backup/b.txt" "$W/list.txt"' "archive lists Data/mnt-backup/b.txt"
ck 'grep -qx "etc/ld.so.preload" "$W/list.txt"' "archive lists etc/ld.so.preload"
ck 'grep -qx "app/bin/AppLauncher.sh" "$W/list.txt"' "archive lists app/bin/AppLauncher.sh"
ck 'grep -qx "lib/systemd/system/carplay_iAP2.service" "$W/list.txt"' "archive lists carplay unit"
ck 'grep -qx "rw_data/tweaks/config.conf" "$W/list.txt"' "archive lists tweaks config"

mkdir -p "$W/x"; tar -xzf "$W/out.tgz" -C "$W/x" 2>/dev/null
ck 'cmp -s "$W/x/Data/mnt-wt/shared/public/a.txt" "$R/Data/mnt-wt/shared/public/a.txt"' "archived file content matches"
ck 'grep -q "cat /proc/uptime" "$W/x/status.txt"' "status.txt contains uptime section"
ck 'grep -q "sha256 /etc/ld.so.preload" "$W/x/status.txt"' "status.txt contains preload hash section"
ck 'cmp -s "$W/root.before" "$W/root.after"' "fake root byte-identical before/after"
tempcount=$(find "$T" -mindepth 1 -print | wc -l | tr -d ' ')
ck '[ "$tempcount" = 0 ]' "no hudnav_snap.* folder left in TMPDIR"

# second fake root: optional files absent must not abort the run
R2="$W/root2"; T2="$W/tmp2"; mkdir -p "$R2" "$T2" "$R2/Data/mnt-backup"
printf 'only backup\n' > "$R2/Data/mnt-backup/c.txt"
snap "$R2" "$T2" "$W/out2.tgz"
rc2=$(cat "$W/out2.tgz.rc")
ck '[ "$rc2" = 0 ]' "missing optional files: still exit 0"
tar -tzf "$W/out2.tgz" > "$W/list2.txt" 2>/dev/null
ck 'grep -qx "status.txt" "$W/list2.txt"' "missing optional files: status.txt still present"
tempcount2=$(find "$T2" -mindepth 1 -print | wc -l | tr -d ' ')
ck '[ "$tempcount2" = 0 ]' "missing optional files: scratch still removed"

ck 'sh -n "$S"' "script parses"
echo "snapshot: $checks checks, $fails failures"
[ "$fails" = 0 ]
