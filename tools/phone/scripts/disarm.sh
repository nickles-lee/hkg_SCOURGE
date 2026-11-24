#!/system/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Stop all detectors and remove the boot auto-arm flag. Does NOT change the
# served table - run restore-genuine.sh afterwards if needed.
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
. "$HERE/common.sh" || exit 1
need_root || exit 1

stop_watchers
rm -f "$AUTO_FLAG"
echo "watchers stopped, auto-arm flag removed"
sh "$HERE/status.sh"
