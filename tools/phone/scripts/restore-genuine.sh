#!/system/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Return the LUN to the single-segment genuine table. Use after a run, or
# before handing the phone to another machine.
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
. "$HERE/common.sh" || exit 1
need_root || exit 1

"$BIN" flip -c "$CFG" -mode genuine
sh "$HERE/status.sh"
