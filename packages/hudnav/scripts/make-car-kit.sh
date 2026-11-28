#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
#
# Build the car kit: a static hudnavd (the 260319 rootfs glibc is 2.25; a host
# dynamic build needs GLIBC_2.34 and will not start), the gated aa_shim.so, and
# the two on-car helper scripts. Nothing here touches the car.
#
# Usage: sh scripts/make-car-kit.sh [output-dir]     (default out/car-kit)
#        PROFILE=profiles/<other>.json sh scripts/make-car-kit.sh [output-dir]
set -eu
cd "$(dirname "$0")/.."

OUT=${1:-out/car-kit}
CC="${CC:-gcc}"
# Generate the firmware-specific C header once, at build time. The car kit
# itself contains no JSON reader: all runtime gates in the .so files are intact.
PROFILE="${PROFILE:-profiles/NE_E.AUS.S5W_L.001.001.260319.json}"
mkdir -p build
python3 -I tools/profile_to_header.py --profile "$PROFILE" --output build/fw_profile.h
CFLAGS="-std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -D_POSIX_C_SOURCE=200809L -Ibuild"

rm -rf "$OUT"
mkdir -p "$OUT"

# Static: no libc version dependency on the target.
$CC $CFLAGS -static -o "$OUT/hudnavd" \
    src/daemon.c src/convert.c src/motion.c src/output.c src/output_can.c src/settings.c
# Diagnostic twin: log backend only, CAN TX not compiled in, logs every shim line
# ("RX ...") and ownership change ("APPMANAGER owner=..."). First-contact instrument.
$CC $CFLAGS -static -DHUDNAV_DIAGNOSTIC -o "$OUT/hudnavd-diag" \
    src/daemon.c src/convert.c src/motion.c src/output.c src/settings.c

# Shared: both shims must stay <= GLIBC_2.14 (dlsym is version-pinned in both).
$CC $CFLAGS -shared -fPIC -o "$OUT/aa_shim.so" src/aa_shim.c src/convert.c
$CC $CFLAGS -shared -fPIC -o "$OUT/cp_hook.so" \
    src/cp_hook.c src/cp_shim.c src/rgd_tlv.c src/convert.c -ldl

# Canary for the /etc/ld.so.preload mechanism: does nothing, needs nothing (not even libc).
$CC -shared -fPIC -nostdlib -o "$OUT/noop.so" tests/vm/noop.c

cp scripts/install-on-hu.sh "$OUT/install-on-hu.sh"
cp scripts/ld-preload.sh "$OUT/ld-preload.sh"
cp scripts/aa-preload.sh "$OUT/aa-preload.sh"
cp scripts/cp-preload.sh "$OUT/cp-preload.sh"
cp scripts/hudnav-run.sh "$OUT/hudnav-run.sh"
# Verbatim stock-frame replay (display-path test; uses only the installed micom-send).
cp scripts/hudnav-replay.sh "$OUT/hudnav-replay.sh"
# Optional: redacted replay files derived from the owner's own stock-nav capture.
# Car captures are NOT shipped with this package (privacy); a porter may provide
# their own directory with REPLAY_DIR=<dir>. Missing files are skipped, not faked.
REPLAY_DIR="${REPLAY_DIR:-}"
if [ -n "$REPLAY_DIR" ] && [ -d "$REPLAY_DIR" ]; then
    for f in replay-roundabout.txt replay-left.txt replay-right.txt \
             replay-dest.txt replay-full.txt replay-clear.txt; do
        if [ -f "$REPLAY_DIR/$f" ]; then cp "$REPLAY_DIR/$f" "$OUT/"; fi
    done
else
    echo "SKIP: no replay files (set REPLAY_DIR to your own redacted set)"
fi
# A CRLF shebang ("#!/bin/sh\r") makes the target report "not found" for the script
# (this is what killed the VM boot earlier). Never ship one.
for s in install-on-hu.sh aa-preload.sh cp-preload.sh ld-preload.sh hudnav-run.sh hudnav-replay.sh; do
    if grep -q "$(printf '\r')" "$OUT/$s"; then
        echo "ERROR: $s has CRLF line endings; run: sed -i 's/\\r\$//' scripts/$s" >&2
        exit 1
    fi
done
# Stock 260319 pins for the Phase 0 check (cd / && sha256sum -c firmware.sha256).
cat > "$OUT/firmware.sha256" <<'PINS'
726c69ce627e9183771d11bdfac9acf9d92286a9b6c1a0d88bfbd6b5ac5a7d50  app/bin/AppProjection
59c31ae0359a8720e75afc3ee3037c3c18f15ffba71347fb46dd5d9691726865  app/lib/liblinux-head-unit.so
e663d5129177a8bd241cb875e36ec5e507d39a2c8111216acafc272bdc3fa88b  app/bin/iAP2Service-2.0
dc9487eb2f18218f20715881ea80011e597a29b778fc37ba754d90a8c4d4e39d  app/bin/AppLauncher.sh
f7cf915509005a4aaa18d09e3a6ccd0abdd72ef2b2816641ea9a592a1e3f4495  lib/systemd/system/carplay_iAP2.service
PINS
chmod 755 "$OUT/install-on-hu.sh" "$OUT/aa-preload.sh" "$OUT/cp-preload.sh" "$OUT/ld-preload.sh" \
    "$OUT/hudnav-run.sh" "$OUT/hudnav-replay.sh" "$OUT/hudnavd" "$OUT/hudnavd-diag"

# Fail the build if either shim ever grows a symbol newer than the target glibc.
for so in aa_shim.so cp_hook.so; do
    if readelf -V "$OUT/$so" 2>/dev/null | grep -qE 'Name: GLIBC_2\.(2[6-9]|[3-9][0-9])'; then
        echo "ERROR: $so requires a glibc newer than the target 2.25" >&2
        readelf -V "$OUT/$so" | grep 'Name: GLIBC' >&2
        exit 1
    fi
done

# aa_shim.so is also listed in /etc/ld.so.preload and runs inside AppProjection, which loads NO libdl: it must not
# reference dlsym either (VM binding test 2026-11-25: with eager binding ld.so aborted AppProjection on it; with lazy
# binding the first call would have killed the process). Symbol lookup uses dl_iterate_phdr (see shim_lookup).
if readelf --dyn-syms -W "$OUT/aa_shim.so" | grep -q ' dlsym'; then
    echo "ERROR: aa_shim.so references dlsym (AppProjection has no libdl)" >&2
    exit 1
fi
if readelf -d "$OUT/aa_shim.so" | grep NEEDED | grep -vqE 'libc\.so\.6'; then
    echo "ERROR: aa_shim.so has an unexpected NEEDED library" >&2
    readelf -d "$OUT/aa_shim.so" | grep NEEDED >&2
    exit 1
fi
# cp_hook.so goes into /etc/ld.so.preload (every process). It must not reference dlsym: on glibc 2.25
# dlsym is in libdl, which most processes lack, and ld.so then aborts them at startup (VM rehearsal
# 2026-11-25: even `rm` failed). Also it may only need libc and the loader.
if readelf --dyn-syms -W "$OUT/cp_hook.so" | grep -q ' dlsym'; then
    echo "ERROR: cp_hook.so references dlsym (unsafe in /etc/ld.so.preload)" >&2
    exit 1
fi
if readelf -d "$OUT/cp_hook.so" | grep NEEDED | grep -vqE 'libc\.so\.6|ld-linux-x86-64\.so\.2'; then
    echo "ERROR: cp_hook.so has an unexpected NEEDED library" >&2
    readelf -d "$OUT/cp_hook.so" | grep NEEDED >&2
    exit 1
fi

cat > "$OUT/README.txt" <<'EOF'
HUD Nav car kit (offline build)
===============================

Contents
  hudnavd            static x86-64 daemon; transmits CAN only in `tx` mode
  hudnavd-diag       static diagnostic twin: log backend, CAN TX not compiled in
  aa_shim.so         LD_PRELOAD interposer, self-gated to AppProjection
  cp_hook.so         LD_PRELOAD interposer, self-gated to iAP2Service-2.0
  hudnav-run.sh      the ONLY supported way to run the daemon (it feeds the daemon the
                     AppManager ownership signals it needs): start diag|dry|tx, stop,
                     status, log, pause, resume
  install-on-hu.sh   installs into /rw_data/hudnav (root, no service start)
  aa-preload.sh      enable/disable/status/verify the AppLauncher.sh preload
  cp-preload.sh      enable/disable/status/verify the carplay_iAP2.service preload
  firmware.sha256    stock 260319 pins: cd / && sha256sum -c /tmp/car-kit/firmware.sha256

Order on the car (parked, owner present); full detail in CAR_DAY_RUNBOOK.md:
  0. id ; cd / && sha256sum -c /tmp/car-kit/firmware.sha256
  1. Copy this directory to the head unit, e.g. /tmp/car-kit.
  2. sh /tmp/car-kit/install-on-hu.sh
  3. Read the next-step lines it prints. Do NOT enable CAN TX on the first pass.
  4. For Android Auto only:
       sh /tmp/car-kit/aa-preload.sh enable --yes
       reboot
       sh /tmp/car-kit/aa-preload.sh verify
       sh /rw_data/hudnav/bin/hudnav-run.sh start diag
  5. Reverse with:
       sh /rw_data/hudnav/bin/hudnav-run.sh stop
       sh /tmp/car-kit/aa-preload.sh disable --yes
       reboot

/etc/ld.so.preload route (what actually works on this car; env LD_PRELOAD does not, see
docs/hudnav/AA_PROTOCOL.md):
  sh /tmp/car-kit/ld-preload.sh preflight       # read-only, must print OK for both libs
  sh /tmp/car-kit/ld-preload.sh canary --yes ; reboot ; sh /tmp/car-kit/ld-preload.sh verify
  sh /tmp/car-kit/ld-preload.sh enable --yes ; reboot ; sh /tmp/car-kit/ld-preload.sh verify
  sh /tmp/car-kit/ld-preload.sh disable --yes   # undo

CarPlay (same log-only pass):
  sh /tmp/car-kit/cp-preload.sh enable --yes
  sh /tmp/car-kit/cp-preload.sh verify
  sh /tmp/car-kit/cp-preload.sh disable --yes
  (add --restart only with owner sign-off; the shim emits no CAN)

Safety
  - install-on-hu.sh copies the car's own /usr/bin/dbus-send to
    /rw_data/hudnav/bin/CANService. It does not patch any vendor binary.
  - aa-preload.sh changes exactly one line of the data-only
    /app/bin/AppLauncher.sh, keeps a byte-identical backup, and restores it.
  - Nothing here sends CAN unless you run `hudnav-run.sh start tx --yes`. `diag` cannot
    transmit at all (not compiled in); `dry` only logs the frames it WOULD send.
  - Do NOT run `hudnavd --appmanager-stdin` by hand or from a systemd unit: with nothing
    feeding stdin it silently goes mute. Use hudnav-run.sh.
EOF

KIT_FILES="hudnavd hudnavd-diag aa_shim.so cp_hook.so noop.so hudnav-run.sh hudnav-replay.sh"
# Include replay files in the manifest only when they were supplied by the porter.
for f in replay-roundabout.txt replay-left.txt replay-right.txt \
         replay-dest.txt replay-full.txt replay-clear.txt; do
    if [ -f "$OUT/$f" ]; then KIT_FILES="$KIT_FILES $f"; fi
done
( cd "$OUT" && sha256sum $KIT_FILES \
    install-on-hu.sh aa-preload.sh cp-preload.sh ld-preload.sh firmware.sha256 > SHA256SUMS )
echo "car kit: $OUT"
cat "$OUT/SHA256SUMS"
