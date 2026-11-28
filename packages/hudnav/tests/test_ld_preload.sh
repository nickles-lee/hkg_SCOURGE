#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# ld-preload.sh safety properties on the host (scratch files only; /etc is never touched).
# Needs root (the script refuses otherwise); skipped when not root.
set -eu
cd "$(dirname "$0")/.."
if [ "$(id -u)" != 0 ]; then echo "ld-preload test skipped (needs root)"; exit 0; fi
S=scripts/ld-preload.sh
T=$(mktemp -d /tmp/hudnav-ldp.XXXXXX)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/rw/bin"
export HUDNAV_RW_DIR="$T/rw" HUDNAV_LD_PRELOAD_FILE="$T/ld.so.preload" HUDNAV_REMOUNT=no

CC="${CC:-gcc}"
$CC -shared -fPIC -nostdlib -o "$T/rw/bin/noop.so" tests/vm/noop.c
cp out/aa_shim.so out/cp_hook.so "$T/rw/bin/"

# 1. preflight passes for the real shims and does not write anything
sh $S preflight > "$T/pf.txt" 2>&1 || { cat "$T/pf.txt"; echo "preflight failed for the real shims"; exit 1; }
grep -q 'OK   .*aa_shim.so' "$T/pf.txt"; grep -q 'OK   .*cp_hook.so' "$T/pf.txt"
[ ! -e "$T/ld.so.preload" ]

# 2. a library that breaks every process (unresolved symbol, bound at load) is caught
cat > "$T/bad.c" <<'EOC'
extern int hudnav_no_such_symbol(void);
int (*hudnav_bad_ptr)(void) = hudnav_no_such_symbol;
EOC
$CC -shared -fPIC -Wl,-z,now -Wl,--unresolved-symbols=ignore-all -o "$T/rw/bin/cp_hook.so" "$T/bad.c" 2> /dev/null
! sh $S preflight > "$T/pf_bad.txt" 2>&1
grep -q 'FAIL .*cp_hook.so' "$T/pf_bad.txt"

# 3. enable refuses without --yes, with a failing preflight, and without a surviving canary
! sh $S enable > /dev/null 2>&1
! sh $S enable --yes > /dev/null 2>&1
[ ! -e "$T/ld.so.preload" ]
cp out/cp_hook.so "$T/rw/bin/cp_hook.so"
! sh $S enable --yes > /dev/null 2>&1                       # no canary.ok yet
[ ! -e "$T/ld.so.preload" ]

# 4. canary lists only noop.so; verify unlocks enable only when the canary is mapped in the shell
sh $S canary --yes > /dev/null
[ "$(cat "$T/ld.so.preload")" = "$T/rw/bin/noop.so" ]
sh $S verify > /dev/null 2>&1
[ ! -e "$T/rw/backup/canary.ok" ]                            # this test shell does not map noop.so
: > "$T/rw/backup/canary.ok"

# 5. enable lists both shims, in order; disable removes the file
sh $S enable --yes > /dev/null
[ "$(cat "$T/ld.so.preload")" = "$(printf '%s\n%s' "$T/rw/bin/aa_shim.so" "$T/rw/bin/cp_hook.so")" ]
sh $S disable --yes > /dev/null
[ ! -e "$T/ld.so.preload" ]

# 6. a pre-existing preload file is restored by disable, not deleted
printf '/opt/other.so\n' > "$T/ld.so.preload"
rm -f "$T/rw/backup/ld.so.preload.before" "$T/rw/backup/ld.so.preload.was-absent"
sh $S canary --yes > /dev/null
sh $S disable --yes > /dev/null
[ "$(cat "$T/ld.so.preload")" = "/opt/other.so" ]
echo "ld-preload tests passed"
