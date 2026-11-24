#!/system/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Read-only status gates. Exits nonzero on any failed gate.
# Usage: sh status.sh [--live|--dryrun]
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
. "$HERE/common.sh" || exit 1
need_root || exit 1

MODE=${1:-}
GADGET_DIR=${GADGET_DIR:-/config/usb_gadget/g1}
LUN_DIR=$GADGET_DIR/functions/$MASS_STORAGE_FN/$LUN_NAME
fail=0
check() { if [ "$1" = 1 ]; then echo "  FAIL: $2"; fail=1; else echo "  OK: $2"; fi; }

echo "=== RIG STATUS $(date) ==="
echo ""
echo "-- LUN table (must be 1-segment GENUINE before a run) --"
DM=$(dm_name)
DM_NODE=$(dm_node_for "$DM" 2>/dev/null || echo "")
table=$("$BIN" status -n "$DM" 2>&1)
echo "$table" | head -6
GENDEV=$(sed -n 's/.*"gendev"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$CFG" | head -1)
GENARG=$(dev_arg "$GENDEV" 2>/dev/null)
if echo "$table" | grep -q 'targets=1' && [ -n "$GENARG" ] && echo "$table" | grep -q "linear $GENARG 0"; then
    check 0 "table is one segment pointing at genuine device $GENARG"
else
    check 1 "table is not one genuine segment (want linear $GENARG 0)"
fi

echo ""
echo "-- gadget LUN (file must be the dm node, ro must be 1) --"
if [ -d "$LUN_DIR" ]; then
    file=$(cat "$LUN_DIR/file" 2>/dev/null)
    ro=$(cat "$LUN_DIR/ro" 2>/dev/null)
    echo "file=$file ro=$ro"
    [ -n "$file" ] && [ "$file" = "$DM_NODE" ] && check 0 "LUN file is the dm node" || check 1 "LUN file is not $DM_NODE"
    [ "$ro" = 1 ] && check 0 "LUN is read-only" || check 1 "LUN ro is not 1"
else
    check 1 "LUN directory missing: $LUN_DIR"
fi

echo ""
echo "-- detector --"
found=none
for c in /proc/[0-9]*/cmdline; do
    if grep -aq 'phone-ri[g]' "$c" 2>/dev/null; then
        cc=$(tr '\0' ' ' < "$c" 2>/dev/null)
        p=${c#/proc/}; p=${p%/cmdline}
        case "$cc" in
            *watchtrace*logonly*) echo "LOG-ONLY pid $p: $cc"; found=dryrun ;;
            *watchtrace*) echo "LIVE pid $p: $cc"; found=live ;;
            *) echo "UNEXPECTED pid $p: $cc"; found=bad ;;
        esac
    fi
done
[ "$found" = none ] && echo "NOT RUNNING"
case "$MODE" in
    --live)   [ "$found" = live ]   && check 0 "live detector running" || check 1 "live detector not running" ;;
    --dryrun) [ "$found" = dryrun ] && check 0 "log-only detector running" || check 1 "log-only detector not running" ;;
    *)        [ "$found" = none ]   && check 0 "no detector running (provisioning/hand-off state)" || check 1 "detector running; none expected for plain status" ;;
esac

echo ""
echo "-- boot auto-arm flag (must be ABSENT) --"
[ -f "$AUTO_FLAG" ] && check 1 "auto-arm flag PRESENT" || check 0 "auto-arm flag absent"

echo ""
echo "-- USB gadget UDC --"
uc=$(cat "$GADGET_DIR/UDC" 2>/dev/null)
if [ -n "$uc" ]; then echo "bound to $uc"; check 0 "UDC bound"; else check 1 "UDC NOT bound"; fi

echo ""
echo "-- recent log --"
tail -10 "$LOG" 2>/dev/null || echo "(no log yet)"

echo ""
if [ "$fail" = 0 ]; then echo "STATUS: PASS"; else echo "STATUS: FAIL"; fi
exit "$fail"
