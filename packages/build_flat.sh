#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Ioniq 5 Gen5W_L EU 260128
# Build locally only; never format a drive or touch a car.
# Usage: sh packages/build_flat.sh [output directory]
# Owner input: SCOURGE_WIFI_TXT=/private/path (never a repository default).
set -eu
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
OUT=${1:-"$HERE/../out/appcombo-flat"}
mkdir -p "$OUT"
OUT=$(CDPATH= cd -- "$OUT" && pwd)
CLEANUP_SOURCE="$HERE/common/runtime_cleanup.sh"
[ -f "$CLEANUP_SOURCE" ] && [ ! -L "$CLEANUP_SOURCE" ] || {
    echo "missing canonical cleanup helper: $CLEANUP_SOURCE" >&2; exit 1; }
cleanup_sha=$(sha256sum "$CLEANUP_SOURCE" | cut -d' ' -f1)
# A stale top-level helper from an older three-file build is removed only when
# it is byte-identical to our canonical helper.  Any unknown file is preserved
# and the build refuses rather than unlinking owner data.
if [ -e "$OUT/runtime_cleanup.sh" ] || [ -L "$OUT/runtime_cleanup.sh" ]; then
    if [ -f "$OUT/runtime_cleanup.sh" ] && [ ! -L "$OUT/runtime_cleanup.sh" ] &&
       [ "$(sha256sum "$OUT/runtime_cleanup.sh" | cut -d' ' -f1)" = "$cleanup_sha" ]; then
        rm -f "$OUT/runtime_cleanup.sh"
    else
        echo "refusing to overwrite unknown file: $OUT/runtime_cleanup.sh" >&2
        exit 1
    fi
fi
STAGE=$(mktemp -d "${TMPDIR:-/tmp}/scourge-flat.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/tree"
# Standalone V24: Battery + Tweaks/Reboot + Wifi1b only.  No WiFi reinstall and
# no one-way migration stage; compose marks the bundle combo/standalone.
SCOURGE_STANDALONE=1 sh "$HERE/compose.sh" "$STAGE/tree" battery,tweaks,wifi1b
if [ ! -f "$STAGE/tree/combo/standalone" ] ||
   [ -L "$STAGE/tree/combo/standalone" ] ||
   ! grep -qx 'standalone=1' "$STAGE/tree/combo/standalone"; then
    echo "standalone marker missing or unreadable" >&2
    exit 1
fi
if [ -n "${SCOURGE_WIFI_TXT:-}" ]; then
    cp -f "$SCOURGE_WIFI_TXT" "$STAGE/tree/wifi.txt"
    chmod 600 "$STAGE/tree/wifi.txt"
fi
if [ -n "${SCOURGE_WIFI1B_TXT:-}" ]; then
    cp -f "$SCOURGE_WIFI1B_TXT" "$STAGE/tree/iphone_wifi.txt"
    chmod 600 "$STAGE/tree/iphone_wifi.txt"
fi
if [ -n "${SCOURGE_AUTHORIZED_KEYS:-}" ]; then
    cp -f "$SCOURGE_AUTHORIZED_KEYS" "$STAGE/tree/authorized_keys"
fi
# Exercise the exact packaged additive patcher on the final canonical base.
python3 "$HERE/wifi1b/scripts/check_combined.py" "$STAGE/tree/combo" "$STAGE/final.qml"
# Skip maps must reference manifest-covered payload files and packaged units:
# a typo would silently weaken the unchanged-stage check on the car.
python3 "$HERE/common/check_skip_maps.py" "$STAGE/tree/combo"
python3 "$HERE/common/test_layout_qt.py" \
    "$STAGE/final.qml"
(cd "$STAGE/tree" && tar -czf "$STAGE/appcombo.tar.gz" .)
digest=$(sha256sum "$STAGE/appcombo.tar.gz" | cut -d' ' -f1)
# Inject the canonical helper function definitions verbatim.  The flat pair
# stays two files; no external cleanup helper is copied or extracted.
[ "$(sha256sum "$CLEANUP_SOURCE" | cut -d' ' -f1)" = "$cleanup_sha" ] || {
    echo "canonical cleanup helper changed during build" >&2; exit 1; }
awk -v tarball="$digest" -v cleanup_sha="$cleanup_sha" \
    -v helper="$CLEANUP_SOURCE" '
    /^# @RUNTIME_CLEANUP_SHELL@$/ {
        while ((getline line < helper) > 0) print line
        close(helper)
        next
    }
    {
        gsub(/@TARBALL_SHA256@/, tarball)
        gsub(/@RUNTIME_CLEANUP_SHA256@/, cleanup_sha)
        print
    }
' "$HERE/flat_wrapper.sh" > "$STAGE/run_stuff.sh"
if grep -Eq '@[A-Z_][A-Z0-9_]*@' "$STAGE/run_stuff.sh"; then
    echo "unexpanded build placeholder in generated wrapper" >&2
    exit 1
fi
sh -n "$STAGE/run_stuff.sh"
cp -f "$STAGE/appcombo.tar.gz" "$STAGE/run_stuff.sh" "$OUT/"
cat > "$OUT/README.txt" <<'EOF'
SCOURGE AppComboFlatV24MicomStandalone — Battery, Tweaks/Reboot and Wifi1b
=========================================================================
Version: V24 (BlueLink-safe modem routing + Orange Pi USB-SSH integration)
See docs/wireless-carplay/V24.md in the source repository for the gate ledger.
This bundle does NOT install the WiFi app, does NOT reinstall WiFi, and has
NO one-way migration stage. It installs Battery, Tweaks/Reboot and Wifi1b on
the canonical standalone Battery/Tweaks base. Wifi1b also works beside an
existing WiFi app; if WiFi is still installed, this bundle leaves it untouched
and only adds Wifi1b.
No manual WiFi Off step is needed: the installer preflight stops only its own
radio daemons before backing up or changing files.
V24 networking:
- WiFi client remains active and SSH works, but while the telematics modem
  (rmnet_usb0/rmnet_usb1) has an IPv4 address Wifi1b installs NO WiFi default
  route. WiFi is an SSH-only link in that state, exactly as requested.
- Unverified legacy wlan0 defaults and ownership records are preserved.
  Wifi1b refuses route changes rather than guessing ownership from a format.
- If the modem data call has no IPv4 address, Wifi1b installs a bounded
  high-metric proto-66 fallback so WiFi can still carry traffic.
- A 30 s audit re-checks the intent: modem data present removes the WiFi
  fallback; modem data gone restores it. Routes on other interfaces are never touched.
- The Orange Pi ECM link is usb0 at 10.55.0.2/24 with no default route. Its
  SSH port follows the configured Wifi1b SSH port and stays available when
  WiFi is switched Off, so USB-SSH remains the out-of-band channel.
- ConnMan is kept away from rmnet_usb0/1 and usb0; the drop-in is upgraded
  from the known V21 hash without overwriting an unknown owner override.
Advanced owners can set `client_route_policy=fallback` in
/rw_data/wifi1b/config.conf if they need a WiFi default while the modem is
up; the default `modem_safe` is the BlueLink-safe setting.
OFFLINE-TESTED / WORK IN PROGRESS; not car-validated. No firmware/update stick
present, and no vehicle deployment is authorized by this bundle.
Copy BOTH files to the stick root: run_stuff.sh and appcombo.tar.gz. The
wrapper verifies the archive SHA256 and readback before running any stage; do
not extract the archive. The canonical cleanup helper is embedded in
run_stuff.sh; no third file is needed.
Bounded runtime log truncation happens first. Removed-WiFi cleanup is
evidence-gated: it runs only with readable QML/asset absence evidence, no
existing or dangling WiFi page/service, no running owned WiFi daemon, and a
newly verified private archive of the current WiFi state. Otherwise owner
credentials and backups are preserved and reported.
SENSITIVE: appcombo.tar.gz contains the compiled Wifi1b payload, which may
include owner credentials from the private source, plus any
SCOURGE_WIFI1B_TXT/SCOURGE_WIFI_TXT or SCOURGE_AUTHORIZED_KEYS inputs.
Treat the pair as private, store and dispose of it accordingly, and keep it
out of any shared location. Optional iphone_wifi.txt (or wifi.txt) and
authorized_keys at the stick root override packed inputs.
Wifi1b SSH: user wifi1b, UID 0, key-only, configured port (default 2224),
bind-all. The same port is opened on the Orange Pi usb0 link, and SSH stays
up over usb0 when WiFi is Off. This is a deliberate product default account,
not a hidden credential, and it is NOT car-validated. Change owner keys
deliberately; existing owner keys in /rw_data/wifi1b are preserved unless you
supply a replacement.
CarPlay control: the Wifi1b page's CarPlay tab switches between "Wifi1b
wireless" and "Stock USB". Wifi1b clears the vendor wireless flag whenever
the AP is not running, so stock USB CarPlay is restored when wireless is
idle; the switch also stops the AP.
BlueLink/nav: the modem default route is never replaced. The hash-gated
ConnMan drop-in keeps ConnMan off rmnet_usb0/1 (the telematics data routes)
and usb0 (the Orange Pi management link); stock USB behavior is preserved.
Icons are best-effort; critical files and readback checks still fail closed.
Beeps: 1 start, 2 per failed stage, 5 success, 3 finished with failures.
Read wrapper_result.txt, run_stuff_result.txt and *_result.txt.
combo.log and *_install.log contain the current run's errors.
qml_readback_* contains private before/after QML snapshots for diagnosis.
runtime_snapshot_*_status.json can be from an older running daemon; it is
not proof that this installation succeeded. Restore logs are not exported
during an install run.
Use the default all install. Restore runs Wifi1b, Tweaks, Battery in reverse
order and restores each package's pre-install state.
Changing only MODE does NOT change the failsafe's run_stuff.sh MD5 gate:
also change a comment in run_stuff.sh when rerunning the same pair.
The experimental MDNSD_ROOT_ARM marker is NOT included; leave it disarmed.
Restore uses saved app backups; do not erase backups after a failed install.
EOF
echo "Built $OUT (tarball SHA256=$digest); standalone layout; car validation pending"
