#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
#
# Runs the real install-on-hu.sh from a freshly built car kit into a scratch directory.
# The host has no SMACK, so three lines are neutralised in a COPY of the installer
# (smackfs check, label read, chsmack presence); everything else is the shipped script.
# Needs root (the installer refuses otherwise); skipped when not root.
set -eu
cd "$(dirname "$0")/.."
if [ "$(id -u)" != 0 ]; then echo "installer test skipped (needs root)"; exit 0; fi

sh scripts/make-car-kit.sh out/car-kit > /dev/null
T=$(mktemp -d /tmp/hudnav-inst.XXXXXX)
trap 'rm -rf "$T"' EXIT
cp -r out/car-kit "$T/kit"
( cd "$T/kit" && sha256sum -c SHA256SUMS > /dev/null )

sed -e '/smackfs not mounted/d' -e '/command -v chsmack/d' \
    -e 's|^LABEL=.*|LABEL=_|' "$T/kit/install-on-hu.sh" > "$T/kit/install-test.sh"
mkdir "$T/fakebin"
for c in chsmack getfattr; do printf '#!/bin/sh\nexit 0\n' > "$T/fakebin/$c"; chmod 755 "$T/fakebin/$c"; done

export HUDNAV_RW_DIR="$T/rw" HUDNAV_PATH="$T/fakebin:/usr/bin:/bin"
sh "$T/kit/install-test.sh" > "$T/out.txt" 2>&1 || { cat "$T/out.txt"; exit 1; }
sh "$T/kit/install-test.sh" --check > "$T/check.txt" 2>&1
! grep -q '^missing:' "$T/check.txt" || { cat "$T/check.txt"; echo "installer left files missing"; exit 1; }

for f in hudnavd hudnavd-diag hudnav-run.sh micom-send micom-send-tp CANService; do
    [ -x "$T/rw/bin/$f" ] || { echo "not executable: $f"; exit 1; }
done
# micom-send-tp: sends only 0x6E7 arrays, in order, via SetCANTPMicomRecv; refuses anything else
# before calling the helper (a recording fake stands in for the car's dbus-send copy).
printf '#!/bin/sh\necho "$*" >> %s/calls\nexit 0\n' "$T" > "$T/rw/bin/CANService"
E='array:byte:0x06,0xe7,0x03,0xf2,0x00,0x00,0x00'; L='array:byte:0x06,0xe7,0x08,0xf2,0x01,0x43,0x00,0x00,0x00,0x00,0x00,0x00'
"$T/rw/bin/micom-send-tp" "$E" "$L"
[ "$(wc -l < "$T/calls")" = 2 ]
grep -q "SetCANTPMicomRecv $E\$" "$T/calls"
tail -n 1 "$T/calls" | grep -q "SetCANTPMicomRecv $L\$"
rm -f "$T/calls"
! "$T/rw/bin/micom-send-tp" "$E" 'array:byte:0x04,0xcc,0x08,0x21,0x00,0x00,0x00,0x00,0x00,0x00,0x00' 2> /dev/null
[ "$(wc -l < "$T/calls" 2>/dev/null || echo 0)" -le 1 ]    # the first (valid) one may go; the bad one never
! grep -q '0x04,0xcc' "$T/calls"
# The shipped unit must go through the runner, in the non-transmitting mode.
grep -q 'hudnav-run.sh run diag' "$T/rw/hudnavd.service"
! grep -q 'micom-send\|--can' "$T/rw/hudnavd.service"
# Installed bytes are the kit's bytes.
cmp "$T/kit/hudnavd" "$T/rw/bin/hudnavd"
cmp "$T/kit/hudnavd-diag" "$T/rw/bin/hudnavd-diag"
cmp "$T/kit/hudnav-run.sh" "$T/rw/bin/hudnav-run.sh"
# hudnavd --help prints usage and exits (the installer's smoke check must not hang).
"$T/rw/bin/hudnavd" --help 2>&1 | grep -q '^usage: hudnavd'
echo "installer test passed"
