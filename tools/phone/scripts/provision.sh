#!/system/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# One-time, ON-DEVICE preparation of a fresh phone: attach the two backing
# images to loop devices, create the dm device, and bind the USB gadget LUN.
# All device names and paths come from the rig.env file pushed by deploy.sh.
# Nothing here touches a vehicle. Run under su.
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
. "$HERE/common.sh" || exit 1
need_root || exit 1

[ -f "$GEN_IMAGE" ] || { echo "missing $GEN_IMAGE" >&2; exit 1; }
[ -f "$EVIL_IMAGE" ] || { echo "missing $EVIL_IMAGE" >&2; exit 1; }
[ -n "$UDC" ] && [ "$UDC" != CHANGEME ] || { echo "set UDC in phone.env" >&2; exit 1; }
DM=$(dm_name)
[ -n "$DM" ] || { echo "could not read dmname from $CFG" >&2; exit 1; }

# 1. Attach the backing images; reuse loops already attached to these files so
#    a re-run does not leak devices.
GEN_LOOP=$(losetup -j "$GEN_IMAGE" 2>/dev/null | head -1 | cut -d: -f1)
[ -n "$GEN_LOOP" ] || { GEN_LOOP=$(losetup -f); losetup "$GEN_LOOP" "$GEN_IMAGE" || exit 1; }
EVIL_LOOP=$(losetup -j "$EVIL_IMAGE" 2>/dev/null | head -1 | cut -d: -f1)
[ -n "$EVIL_LOOP" ] || { EVIL_LOOP=$(losetup -f); losetup "$EVIL_LOOP" "$EVIL_IMAGE" || exit 1; }
echo "genuine=$GEN_LOOP evil=$EVIL_LOOP"

# 2. Create the dm device WITHOUT -c: loadCfg resolves loop paths and the
#    config still contains placeholders at this stage. The name alone is enough.
if ! DM_NODE=$(dm_node_for "$DM"); then
    "$BIN" create -n "$DM" || exit 1
fi
DM_NODE=$(dm_node_for "$DM") || { echo "could not resolve dm node for $DM" >&2; exit 1; }
[ -b "$DM_NODE" ] || { echo "$DM_NODE is not a block device" >&2; exit 1; }
echo "dmnode=$DM_NODE"

# 3. Save the resolved values for the owner BEFORE touching the gadget: the UDC
#    rebind will disconnect ADB.
OUT=$RIG_DIR/provision.out
{
    echo "dmname=$DM"
    echo "dmnode=$DM_NODE"
    echo "gendev=$GEN_LOOP"
    echo "evildev=$EVIL_LOOP"
} > "$OUT"
echo ""
echo "Resolved values written to $OUT:"
cat "$OUT"
echo "Fill these into $CFG, then run 'sh restore-genuine.sh' before status.sh."

# 4. Bind the gadget last: unbind first (the existing gadget may own the UDC),
#    then attach the mass-storage function with the dm node, read-only.
FN=$GADGET_DIR/functions/$MASS_STORAGE_FN
LUN=$FN/$LUN_NAME
CFGDIR=$GADGET_DIR/configs/$CONFIG_NAME
echo "" > "$GADGET_DIR/UDC" 2>/dev/null
mkdir -p "$LUN" "$CFGDIR"
echo "$DM_NODE" > "$LUN/file" || exit 1
echo 1 > "$LUN/ro" || exit 1
echo 0 > "$LUN/removable" || exit 1
# Replace the link explicitly: ln -sf follows an existing symlink-to-directory.
rm -f "$CFGDIR/$MASS_STORAGE_FN"
ln -s "$FN" "$CFGDIR/$MASS_STORAGE_FN" || exit 1
echo "$UDC" > "$GADGET_DIR/UDC" || { echo "UDC bind failed" >&2; exit 1; }
echo "UDC bound; ADB may now be disconnected."
