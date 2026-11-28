#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Host test of storeguard/hudnav-storeguard.sh against a fake root. No car, no network.
# Run through WSL: sh tests/test_storeguard.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
S="$HERE/../storeguard/hudnav-storeguard.sh"
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
fails=0; checks=0
ck() { checks=$((checks+1)); if ! eval "$1"; then echo "FAIL: $2"; fails=$((fails+1)); fi; }

NAMES='resource-table-cfg.itz default-data.itz configurable-default-data.itz'

mkfake() {  # mkfake ROOT : fake car with a healthy shared-public store
  r=$1; rm -rf "$r"
  mkdir -p "$r/Data/mnt-wt/shared/public" "$r/rw_data/hudnav-storeguard"
  printf 'KdB-index-0123456789\n' > "$r/Data/mnt-wt/shared/public/resource-table-cfg.itz"
  printf 'KdB-default-data-0123456789\n' > "$r/Data/mnt-wt/shared/public/default-data.itz"
  printf 'KdB-configurable-default-data-0123456789\n' > "$r/Data/mnt-wt/shared/public/configurable-default-data.itz"
  printf 'user-wt-values\n' > "$r/Data/mnt-wt/shared/public/wt.itz"
  printf 'cached-user-values\n' > "$r/Data/mnt-wt/shared/public/cached.itz"
  for n in $NAMES; do printf 'lock-%s\n' "$n" > "$r/Data/mnt-wt/shared/public/$n-lock"; done
}

arm() { : > "$1/rw_data/hudnav-storeguard/armed"; }
run() {  # run ROOT MODE OUTFILE [extra env is not needed]; stdout goes outside the fake root
  HUDNAV_ROOT="$1" sh "$S" "$2" > "$3" 2>&1
}
snap() { (cd "$1" && { find . -type d | sort; find . -type f | sort | while IFS= read -r f; do sha256sum "$f"; done; }); }
LOG() { echo "$1/rw_data/hudnav-storeguard/log"; }
GOOD() { echo "$1/rw_data/hudnav-storeguard/good"; }
STORE() { echo "$1/Data/mnt-wt/shared/public"; }
others() {  # sha256 of wt.itz, cached.itz and all three lock files
  (cd "$1" && sha256sum Data/mnt-wt/shared/public/wt.itz Data/mnt-wt/shared/public/cached.itz Data/mnt-wt/shared/public/*-lock)
}

# 1. not armed: check changes nothing (fake root byte-identical).
R1="$W/r1"; mkfake "$R1"
snap "$R1" > "$W/s1.before"
run "$R1" check "$W/o1"
snap "$R1" > "$W/s1.after"
ck 'cmp -s "$W/s1.before" "$W/s1.after"' "1: not armed -> fake root byte-identical"
ck 'grep -q "not armed" "$W/o1"' "1: check reports not armed on stdout"

# 2. save on a healthy store: three good copies + a verifying SHA256SUMS.
R2="$W/r2"; mkfake "$R2"; arm "$R2"
run "$R2" save "$W/o2"
for n in $NAMES; do
  ck '[ -f "$(GOOD "$R2")/'"$n"'" ]' "2: good copy of $n exists"
  ck '[ "$(sha256sum "$(STORE "$R2")/'"$n"'" | cut -d" " -f1)" = "$(sha256sum "$(GOOD "$R2")/'"$n"'" | cut -d" " -f1)" ]' "2: good $n matches live"
done
ck '[ -f "$(GOOD "$R2")/SHA256SUMS" ]' "2: SHA256SUMS exists"
ck '(cd "$(GOOD "$R2")" && sha256sum -c SHA256SUMS) >/dev/null 2>&1' "2: SHA256SUMS verifies all good copies"
ck 'grep -q "saved" "$W/o2"' "2: save prints what was saved"
ck 'grep -q "saved resource-table-cfg.itz" "$(LOG "$R2")"' "2: save logs what was saved"

# 3. save refuses when one live file is invalid, changing nothing.
R3="$W/r3"; mkfake "$R3"; arm "$R3"
printf 'XXX-bad-default-data-no-header\n' > "$(STORE "$R3")/default-data.itz"
snap "$R3" > "$W/s3.before"
run "$R3" save "$W/o3"; rc3=$?
snap "$R3" > "$W/s3.after"
ck '[ "$rc3" = 1 ]' "3: save exits 1 on an unhealthy store"
ck 'grep -q "store not healthy, not saving" "$W/o3"' "3: save prints refusal message"
ck 'cmp -s "$W/s3.before" "$W/s3.after"' "3: unhealthy save leaves tree byte-identical"
ck '[ ! -d "$(GOOD "$R3")" ]' "3: no good/ directory created"

# 4. check restores a missing index and a bad-header default data; valid live
#    files, user data and lock files stay untouched (compare sha256).
R4="$W/r4"; mkfake "$R4"; arm "$R4"
run "$R4" save "$W/o4.save"   # seed the known-good copy from a healthy store
HCFG=$(sha256sum "$(STORE "$R4")/configurable-default-data.itz" | cut -d' ' -f1)
HGOODCFG=$(sha256sum "$(GOOD "$R4")/configurable-default-data.itz" | cut -d' ' -f1)
others "$R4" > "$W/h4.others.before"
rm -f "$(STORE "$R4")/resource-table-cfg.itz"
printf 'XXX-damaged-default-data\n' > "$(STORE "$R4")/default-data.itz"
run "$R4" check "$W/o4"
ck '[ -f "$(STORE "$R4")/resource-table-cfg.itz" ]' "4: missing index restored"
ck '[ "$(head -c 3 "$(STORE "$R4")/resource-table-cfg.itz")" = KdB ]' "4: restored index has KdB header"
ck '[ "$(head -c 3 "$(STORE "$R4")/default-data.itz")" = KdB ]' "4: restored default data has KdB header"
ck '[ "$(sha256sum "$(STORE "$R4")/resource-table-cfg.itz" | cut -d" " -f1)" = "$(sha256sum "$(GOOD "$R4")/resource-table-cfg.itz" | cut -d" " -f1)" ]' "4: index matches good copy"
ck '[ "$(sha256sum "$(STORE "$R4")/default-data.itz" | cut -d" " -f1)" = "$(sha256sum "$(GOOD "$R4")/default-data.itz" | cut -d" " -f1)" ]' "4: default data matches good copy"
ck '[ "$(sha256sum "$(STORE "$R4")/configurable-default-data.itz" | cut -d" " -f1)" = "$HCFG" ]' "4: valid configurable file untouched"
ck '[ "$HCFG" = "$HGOODCFG" ]' "4: configurable good copy also unchanged"
others "$R4" > "$W/h4.others.after"
ck 'cmp -s "$W/h4.others.before" "$W/h4.others.after"' "4: wt.itz, cached.itz and all -lock files unchanged (sha256)"
ck '[ -z "$(find "$(STORE "$R4")" -name "*.storeguard.tmp" -print)" ]' "4: no .storeguard.tmp left behind"
ck 'grep -q "restored resource-table-cfg.itz" "$(LOG "$R4")"' "4: restored index logged"
ck 'grep -q "restored default-data.itz" "$(LOG "$R4")"' "4: restored default data logged"

# 5. tampered good copy (sha mismatch): change nothing, even if live index is missing.
R5="$W/r5"; mkfake "$R5"; arm "$R5"
run "$R5" save "$W/o5.save"
printf 'XXX-tampered-good-copy\n' > "$(GOOD "$R5")/resource-table-cfg.itz"
rm -f "$(STORE "$R5")/resource-table-cfg.itz"
snap "$R5" > "$W/s5.before"
run "$R5" check "$W/o5"
snap "$R5" > "$W/s5.after"
ck 'cmp -s "$W/s5.before" "$W/s5.after"' "5: tampered good copy -> tree byte-identical"
ck 'grep -q "no valid good copy" "$W/o5"' "5: check reports no valid good copy"

# 6. healthy store: check changes nothing.
R6="$W/r6"; mkfake "$R6"; arm "$R6"
run "$R6" save "$W/o6.save"
snap "$R6" > "$W/s6.before"
run "$R6" check "$W/o6"
snap "$R6" > "$W/s6.after"
ck 'cmp -s "$W/s6.before" "$W/s6.after"' "6: healthy store -> check leaves tree byte-identical"
ck '[ ! -s "$W/o6" ]' "6: healthy store -> check prints nothing"

# 7. status is read-only and reports state plus last log lines.
R7="$W/r7"; mkfake "$R7"; arm "$R7"
run "$R7" save "$W/o7.save"
snap "$R7" > "$W/s7.before"
run "$R7" status "$W/o7"
snap "$R7" > "$W/s7.after"
ck 'cmp -s "$W/s7.before" "$W/s7.after"' "7: status leaves tree byte-identical"
ck 'grep -q "armed: yes" "$W/o7"' "7: status reports armed"
ck 'grep -q "live resource-table-cfg.itz: valid" "$W/o7"' "7: status reports live validity"
ck 'grep -q "good copy: present" "$W/o7"' "7: status reports good copy validity"
ck 'grep -q "saved" "$W/o7"' "7: status prints last log lines"

ck 'sh -n "$S"' "script parses"
echo "storeguard: $checks checks, $fails failures"
[ "$fails" = 0 ]
