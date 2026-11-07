#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Retry the TCG boot-stall race. The guest stalls at timer/CPU bring-up on some
# boots; a fresh QEMU run almost always succeeds within a few attempts.
set -u
cd "$(dirname "$0")/.."
n=${VM_RETRIES:-3}
i=1
while [ "$i" -le "$n" ]; do
    if ./vm/launch.py "$@"; then
        exit 0
    fi
    echo "vm: attempt $i failed; retrying ($i/$n)" >&2
    i=$((i + 1))
done
exit 1
