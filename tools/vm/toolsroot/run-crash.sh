#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

set +e
ulimit -c unlimited
echo "CORE_PATTERN: $(cat /proc/sys/kernel/core_pattern)"
ls -la /usr/bin/dlt-cdh /rw_data/dlt 2>&1
/usr/bin/dlt-daemon -d 2>/tmp/dlt-crash.err
sleep 2
./crash.elf
echo "CRASH_RC=$?"
sleep 3
echo "===CORE FILES==="
ls -la /var/core /rw_data/dlt /log_data/dltlog 2>&1
echo "===DLT==="
ps -ef | grep -E 'dlt|crash' | grep -v grep
cat /tmp/dlt-crash.err 2>&1
