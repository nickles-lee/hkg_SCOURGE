#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Gen5W_L AU 260319
# Stage the Projection+ install tree (USB stick root or SSH upload dir).
# Usage: sh scripts/stage_payload.sh [BUILD_DIR] [OUT_DIR]
#   BUILD_DIR  output of scripts/build_artifacts.py (default: build): patched
#              layouts under qml_eco3.0/ plus manifest.txt (formats below).
#   OUT_DIR    default: BUILD_DIR/usb
#   PROJECTION_DAEMON_BIN  daemon binary (default: src/projection_daemon)
# Output manifest.txt: "<path under payload/> <base sha256|new> <sha256>".
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
BUILD=${1:-"$ROOT/build"}
OUT=${2:-"$BUILD/usb"}
DAEMON=${PROJECTION_DAEMON_BIN:-"$ROOT/src/projection_daemon"}
LAYOUTS="$BUILD/manifest.txt"

die() { echo "stage_payload: $*" >&2; exit 1; }
sha() { sha256sum "$1" | cut -d' ' -f1; }

[ -f "$LAYOUTS" ] || die "missing $LAYOUTS (run python3 scripts/build_artifacts.py first)"
[ -f "$DAEMON" ] || die "missing daemon $DAEMON (run make -C src)"

rm -rf "$OUT"
mkdir -p "$OUT/payload"
M="$OUT/manifest.txt"
: > "$M"

add() {
    rel=$1; src=$2; base=$3; mode=${4:-copy}
    case "$rel" in *' '*|*'|'*|*..*) die "unsupported path: $rel" ;; esac
    if grep -q "^$rel " "$M"; then return 0; fi
    mkdir -p "$OUT/payload/$(dirname "$rel")"
    if [ "$mode" = text ]; then
        # Repo checkouts may be CRLF (core.autocrlf); the car needs LF.
        tr -d '\r' < "$src" > "$OUT/payload/$rel"
    else
        cp "$src" "$OUT/payload/$rel"
    fi
    printf '%s %s %s\n' "$rel" "$base" "$(sha "$OUT/payload/$rel")" >> "$M"
}

# 1. Rows from build_artifacts.py win.  Two accepted formats:
#    5 columns: kind dest source(rel. to BUILD) sha256(source) base('-' = new)
#               (what build_artifacts.py writes; kind is patch or file)
#    3 columns: path base('new') sha256(path)   (path relative to BUILD)
# Every listed source must hash to the listed value.
sed -e 's/\r$//' -e 's/,/ /g' "$LAYOUTS" > "$OUT/.layouts"
while read -r c1 c2 c3 c4 c5 rest; do
    case "$c1" in ''|'#'*) continue ;; esac
    [ -z "$rest" ] || die "too many columns in $LAYOUTS: $c1 $c2"
    if [ -n "$c5" ]; then
        case "$c1" in patch|file) : ;; *) die "unknown kind in $LAYOUTS: $c1" ;; esac
        case "$c2" in
            /app/share/AppHomeScreen/qml_eco3.0/*|/app/share/AppHomeScreen/projection/*) : ;;
            *) die "unsupported destination in $LAYOUTS: $c2" ;;
        esac
        p=${c2#/app/share/AppHomeScreen/}; src="$BUILD/$c3"; pay=$c4; base=$c5
        [ "$base" != - ] || base=new
        [ "$c1" = file ] || [ "$base" != new ] || die "patch row without base sha: $c2"
    elif [ -n "$c3" ]; then
        p=${c1#./}; p=${p#build/}; src="$BUILD/$p"; base=$c2; pay=$c3
        case "$p" in qml_eco3.0/*) : ;; *) die "unexpected path in $LAYOUTS: $p" ;; esac
    else
        die "malformed line in $LAYOUTS: $c1"
    fi
    [ -f "$src" ] || die "listed file missing: $src"
    [ "$(sha "$src")" = "$pay" ] || die "sha256 of $src does not match $LAYOUTS"
    # Patched layouts are byte-pinned; our own new QML is LF-normalised.
    if [ "$base" = new ] && [ "${p##*.}" = qml ]; then
        add "$p" "$src" "$base" text
    else
        add "$p" "$src" "$base"
    fi
done < "$OUT/.layouts"
rm -f "$OUT/.layouts"

# 2. Our own QML (page + components): new files.
QSRC="$ROOT/artifacts/qml/AppHomeScreen"
for f in $(cd "$QSRC" && find qml_eco3.0 -type f -name '*.qml' | sort); do
    add "$f" "$QSRC/$f" new text
done
# 3. Icons.
for f in "$ROOT"/artifacts/assets/projection/*.png; do
    add "projection/$(basename "$f")" "$f" new
done
# 4. Daemon and unit.
add bin/projection_daemon "$DAEMON" new
add systemd/projection-daemon.service "$ROOT/artifacts/systemd/projection-daemon.service" new text
# 5. Restore tooling (copied to /rw_data/projection/tools/ before any write).
add tools/install_io.sh "$ROOT/../common/install_io.sh" new text
add tools/projection_txn.sh "$HERE/projection_txn.sh" new text
add tools/run_stuff_restore.sh "$HERE/run_stuff_restore.sh" new text

tr -d '\r' < "$HERE/run_stuff.sh" > "$OUT/run_stuff.sh"
tr -d '\r' < "$HERE/run_stuff_restore.sh" > "$OUT/run_stuff_restore.sh"
cat > "$OUT/README.txt" <<'EOF'
Projection+ (WIP, not car-validated)

SSH (as root): upload this directory to the car, e.g. /tmp/projection, then
  sh /tmp/projection/run_stuff.sh --from-dir /tmp/projection --dry-run
  sh /tmp/projection/run_stuff.sh --from-dir /tmp/projection
Restore:
  sh /rw_data/projection/tools/run_stuff_restore.sh

USB: copy this directory's contents to the stick root and plug it in. An
empty file named DRY_RUN makes it a dry run. To restore, rename
run_stuff_restore.sh to run_stuff.sh. Result: run_stuff_result.txt.

Re-run after any firmware update (it reverts /app).
EOF

n=$(grep -c . "$M")
echo "staged $n files into $OUT"
if command -v file >/dev/null 2>&1; then
    echo "daemon: $(file -b "$OUT/payload/bin/projection_daemon")"
fi
