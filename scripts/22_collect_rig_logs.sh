#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# 22_collect_rig_logs.sh — pull the rig's TOCTOU evidence into out/rig-logs/<stamp>/
#
#   scripts/22_collect_rig_logs.sh
#
# Everything the last run left behind lives on the rig's SD card, so this is a
# plain rsync over the same ssh path the other rig scripts use (PI_HOST/PI_USER
# in config/paths.local.env). On the car that means: join the rig's fallback AP
# (10.42.0.1) or have it back on a known WiFi. Nothing here needs root on the
# rig; /var/log/toctou*.log is world-readable.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/lib.sh"

# --host/--user override the config file: on the car the rig is only reachable as
# 10.42.0.1 (its fallback AP), which is not what config/paths.local.env holds.
WATCH=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --host) PI_HOST="${2:?--host needs an address}"; shift 2 ;;
        --user) PI_USER="${2:?--user needs a name}"; shift 2 ;;
        --watch) WATCH="${2:-300}"; shift 2 ;;
        -h|--help) sed -n '4,13p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1 (try --help)" >&2; exit 2 ;;
    esac
done
[ -n "$PI_HOST" ] || { echo "set PI_HOST/PI_USER in config/paths.local.env, or pass --host" >&2; exit 1; }

collect_once() {
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
DEST="$OUT_DIR/rig-logs/$STAMP"
mkdir -p "$DEST"
echo "== $PI_USER@$PI_HOST -> $DEST"

# The plugin log is the primary evidence; the read log is the per-read timeline.
# Missing files are not an error: an unarmed rig has neither.
# One source per path: quoting a list after the colon hands the remote shell a
# single filename with a space in it, and rsync then finds nothing.
MISSING=0
for name in toctou.log toctou-reads.log; do
    rsync -a --no-perms -e 'ssh -o BatchMode=yes' \
        "$PI_USER@$PI_HOST:/var/log/$name" "$DEST/" \
        || { echo "   could not collect /var/log/$name"; MISSING=1; }
done

# Keep the archived benchmarks too: they explain a session that started before
# someone cleaned up, and they cost a few hundred kB.
rsync -a --no-perms -e 'ssh -o BatchMode=yes' \
    "$PI_USER@$PI_HOST:/var/log/toctou-reads.log.bench" "$DEST/" 2>/dev/null || true

ssh -o BatchMode=yes "$PI_USER@$PI_HOST" '
    echo "## uname"; uname -a
    echo "## uptime"; uptime -p
    echo "## service"; systemctl is-enabled toctou-gadget; systemctl is-active toctou-gadget
    echo "## udc"; ls /sys/class/udc
    echo "## regions.json"; cat ~/scourge/build/regions.json 2>/dev/null
    echo "## regions-stock.json"; cat ~/scourge/build/regions-stock.json 2>/dev/null
    echo "## serve.img ->"; readlink ~/scourge/build/serve.img 2>/dev/null || echo "(missing)"
echo "## nbdkit baseline"; pgrep -a nbdkit 2>/dev/null | sed -n 's/.*baseline=\([^ ]*\).*/\1/p'
' >"$DEST/rig-state.txt" 2>&1 || true

ssh -o BatchMode=yes "$PI_USER@$PI_HOST" \
    'journalctl -u toctou-gadget --no-pager -n 3000 2>/dev/null' >"$DEST/journal-toctou-gadget.txt" 2>&1 || true

# The page's own view of the same log; useful because it is what the phone showed.
# PI_HOST may be an ssh alias with no DNS record, so ask the board for its address.
IP="$(ssh -o BatchMode=yes "$PI_USER@$PI_HOST" "hostname -I 2>/dev/null | awk '{print \$1}'" 2>/dev/null || true)"
[ -n "${IP:-}" ] || IP="$PI_HOST"
if curl -sf --max-time 10 "http://$IP/api/toctou" >"$DEST/api-toctou.json" 2>/dev/null; then
    echo "   page: http://$IP/api/toctou"
else
    rm -f "$DEST/api-toctou.json"
    echo "   (page not reachable over http at $IP — the log files above are the same data)"
fi

echo
echo "== what the passes say"
# The numbers, not the narrative: the pass table is what a run is judged on, so it
# goes out as JSON too (values.json) and can be pasted into the docs without
# re-deriving anything by hand.
"$PYTHON" - "$DEST/toctou.log" "$DEST/values.json" <<'PY'
import json
import pathlib
import re
import sys

log, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
values = {"log": str(log.name), "session": "", "serving": "", "image_size": 0,
          "region": {}, "passes": [], "switched_at_pass": 0,
          "pass_seconds_total": 0.0}
if not log.exists():
    print("no toctou.log collected")
    out.write_text(json.dumps(values, indent=2) + "\n")
    raise SystemExit(0)

lines = log.read_text(errors="replace").splitlines()
for line in lines:
    match = re.search(r"open baseline=(\S+) size=(\d+) regions=(\d+)", line)
    if match:
        values["serving"] = pathlib.Path(match.group(1)).name
        values["image_size"] = int(match.group(2))
        continue
    match = re.search(r"region \[(\d+),(\d+)\) mode=(\S+) switch_after_pass=(\d+) evil=(\S+)", line)
    if match:
        off, end, mode, switch, evil = match.groups()
        values["region"] = {"off": int(off), "len": int(end) - int(off),
                            "mode": mode, "switch_after_pass": int(switch),
                            "evil": pathlib.Path(evil).name}

starts = [i for i, line in enumerate(lines) if "SESSION START" in line]
session = (lines[starts[-1]:] if starts else lines)
last_pass = None
for index, line in enumerate(session):
    if "SESSION START" in line:
        match = re.search(r"sess=(\w+)", line)
        values["session"] = match.group(1) if match else "?"
        print(f"session {values['session']}")
        continue
    match = re.search(r"PASS #(\d+) END .*bytes_genuine=(\d+) bytes_evil=(\d+) "
                      r"served=(\w+).*dur=([\d.]+)s", line)
    if match:
        number, genuine, evil, served, dur = match.groups()
        total = int(genuine) + int(evil)
        rate = round(total / float(dur) / 1e6, 1) if float(dur) > 0 else 0.0
        values["passes"].append({"n": int(number), "served": served,
                                 "bytes": total, "genuine": int(genuine),
                                 "evil": int(evil), "dur_s": float(dur),
                                 "mbps": rate})
        values["pass_seconds_total"] = round(
            values["pass_seconds_total"] + float(dur), 1)
        last_pass = index
        print(f"  pass #{number}: {served:<7} {total/1e6:7.0f} MB "
              f"{float(dur):6.1f} s {rate:5.1f} MB/s")
        continue
    match = re.search(r"SWITCH at pass #(\d+)", line)
    if match:
        values["switched_at_pass"] = int(match.group(1))
        print("  " + line.split("] ", 1)[-1])

# A pass is closed either by the next sweep starting, or by the session ending.
# In the second case the plugin's duration runs from the first read to shutdown,
# so most of it can be idle time — the rate is a lower bound, not a measurement.
if last_pass is not None and values["passes"]:
    closed_by_session_end = any("SESSION END" in line for line in session[last_pass:])
    values["passes"][-1]["closed_at_session_end"] = closed_by_session_end
    if closed_by_session_end:
        values["passes"][-1]["mbps_lower_bound_only"] = True

out.write_text(json.dumps(values, indent=2) + "\n")
PY

echo
echo "collected into $DEST"
ls -1 "$DEST" | sed 's/^/  /'

if [ "$MISSING" -eq 1 ]; then
    echo >&2
    echo "WARNING: the plugin log is missing from this bundle." >&2
    echo "  Either the gadget was never armed, or ssh as $PI_USER@$PI_HOST failed." >&2
    echo "  --host must be something ssh can authenticate to: an alias from" >&2
    echo "  ~/.ssh/config is best, an address only if it matches a key/agent." >&2
    return 1
fi

# Keep the shelf bounded: a watching run over a long evening adds one bundle per
# change, and ten of them are plenty to compare.
ls -1dt "$OUT_DIR"/rig-logs/*/ 2>/dev/null | tail -n +11 | while read -r old; do rm -rf "$old"; done
}

if [ "$WATCH" = 0 ]; then
    collect_once
    exit $?
fi

# --watch: pull a bundle whenever the rig's plugin log changes, and say nothing
# while it is unreachable (that is the normal state at the car — no WiFi, or the
# fallback AP is not joined here). The rig itself never pushes anywhere: this is
# a host-side loop, so it needs no key or service on the board.
STATE="$OUT_DIR/rig-logs/.fingerprint"
mkdir -p "$OUT_DIR/rig-logs"
echo "watching $PI_USER@$PI_HOST every ${WATCH}s — Ctrl-C to stop"
while :; do
    now="$(ssh -o BatchMode=yes -o ConnectTimeout=8 "$PI_USER@$PI_HOST" \
        'md5sum /var/log/toctou*.log 2>/dev/null | md5sum' 2>/dev/null || true)"
    if [ -n "$now" ] && [ "$now" != "$(cat "$STATE" 2>/dev/null || true)" ]; then
        printf '%s\n' "$now" >"$STATE"
        collect_once || true
    fi
    sleep "$WATCH"
done
