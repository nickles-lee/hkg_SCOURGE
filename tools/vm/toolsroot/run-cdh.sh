#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

set +e
ulimit -c unlimited
mkdir -p /rw_data/dlt /var/core /media/tools /log_data/dltlog
/usr/bin/dlt-daemon -d 2>/tmp/dlt-cdh.err
sleep 2
echo '|/usr/bin/dlt-cdh %p' > /proc/sys/kernel/core_pattern
echo "CORE_PATTERN: $(cat /proc/sys/kernel/core_pattern)"
cd /media/tools
./crash.elf
echo "CRASH_RC=$?"
sleep 5
echo "===PROC==="
ps -ef | grep -E 'dlt|crash' | grep -v grep
echo "===FILES==="
find /rw_data/dlt /var/core /log_data -maxdepth 3 -type f -printf '%p %s\n' 2>/dev/null | head -40
echo "===DLT ERR==="
cat /tmp/dlt-cdh.err
