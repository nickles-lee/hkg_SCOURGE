#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Ioniq 5 Gen5W_L EU; read-only inventory, not car-validated.
# Run on the head unit; paths are local to the machine running the script.

echo 'Read-only inventory; output can contain private filenames and device details.'
echo 'Sizes can change during collection. Missing commands or errors mean incomplete results.'
status=0

for path in /rw_data /log_data; do
    printf '\n=== %s ===\n' "$path"
    if [ ! -d "$path" ]; then
        echo 'Directory unavailable; skipped.'
        status=1
        continue
    fi
    echo '-- filesystem blocks --'
    df -Pk "$path" || { echo 'Block usage unavailable.'; status=1; }
    echo '-- filesystem inodes --'
    df -Pi "$path" || { echo 'Inode usage unavailable.'; status=1; }
    echo '-- mount information (may include parent mount) --'
    if command -v findmnt >/dev/null 2>&1; then
        findmnt -T "$path" || { echo 'Mount lookup failed.'; status=1; }
    else
        echo 'findmnt unavailable; inspect /proc/mounts manually.'
        status=1
    fi
    echo '-- top-level allocated usage in KiB (including hidden entries) --'
    for item in "$path"/* "$path"/.[!.]* "$path"/..?*; do
        [ -e "$item" ] || [ -L "$item" ] || continue
        du -skx "$item" || { printf 'Usage unavailable: %s\n' "$item"; status=1; }
    done
    echo '-- regular files larger than 10 MiB; do not follow other filesystems --'
    find "$path" -xdev -type f -size +10485760c -print ||
        { echo 'Large-file scan incomplete.'; status=1; }
done

exit "$status"
