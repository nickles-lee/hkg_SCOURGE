#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Assemble a USB payload tree for the Battery app.
#
# Usage: sh scripts/build_payload.sh /path/to/output
set -eu
PATH=/bin:/sbin:/usr/bin:/usr/sbin

HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=${1:-}
if [ -z "$OUT" ]; then
    echo "usage: sh scripts/build_payload.sh /path/to/output" >&2
    exit 2
fi
if [ -e "$OUT" ] && [ ! -d "$OUT" ]; then
    echo "output exists and is not a directory: $OUT" >&2
    exit 1
fi
mkdir -p "$OUT"

if [ -e "$OUT/payload" ] || [ -e "$OUT/run_stuff.sh" ]; then
    echo "refusing to overwrite an existing payload in: $OUT" >&2
    exit 1
fi

# Build the daemon.
( cd "$HERE/src" && make )

mkdir -p "$OUT/payload/qml" "$OUT/payload/assets" "$OUT/payload/bin" \
         "$OUT/payload/systemd" "$OUT/backup" "$OUT/backup_current" "$OUT/docs"
mkdir -p "$OUT/payload/lib"
cp "$HERE/../common/install_io.sh" "$OUT/payload/lib/"

cp -a "$HERE/artifacts/qml/AppHomeScreen" "$OUT/payload/qml/"
cp -a "$HERE/artifacts/assets/battery" "$OUT/payload/assets/"
cp -a "$HERE/artifacts/systemd/battery-daemon.service" "$OUT/payload/systemd/"
cp -a "$HERE/src/battery_daemon" "$OUT/payload/bin/battery_daemon"
cp -a "$HERE/backup/AppHomeScreen" "$OUT/backup/"
cp -a "$HERE/docs/README_STICK.txt" "$OUT/README.txt"

# Hash of the Hello-patched Advanced file this payload was generated from.
md5sum "$HERE/backup/hello/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.qml" \
    | cut -d' ' -f1 > "$OUT/payload/hello_base.md5"

cp -a "$HERE/scripts/run_stuff.sh" "$OUT/run_stuff.sh"
cp -a "$HERE/scripts/run_stuff_restore.sh" "$OUT/run_stuff_restore.sh"
cp -a "$HERE/scripts/run_stuff_diag.sh" "$OUT/run_stuff_diag.sh"

(
    cd "$OUT"
    find payload backup/AppHomeScreen -type f -print \
        | sort \
        | while IFS= read -r f; do md5sum "$f"; done \
        > manifest.md5
    md5sum -c manifest.md5 >/dev/null
)

echo "payload built at: $OUT"
echo "copy run_stuff.sh, payload/, backup/, backup_current/, manifest.md5 and README.txt to the stick root."
echo "restore: copy run_stuff_restore.sh over run_stuff.sh and plug the stick in."
