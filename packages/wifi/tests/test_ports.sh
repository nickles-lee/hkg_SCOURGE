#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Port-conflict smoke test for the SSH fallback.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
BIN="$ROOT/src/wifi_daemon"
[ -x "$BIN" ] || make -C "$ROOT/src"

python3 - <<'EOF' &
import socket, time
socks = []
for port in (2222, 2223):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    s.listen(1)
    socks.append(s)
time.sleep(8)
EOF
DUMMY=$!
sleep 0.5

out=$("$BIN" --choose-port 2222 2>/dev/null)
echo "$out" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["ok"] is True; assert d["port"]==2224, d'

kill "$DUMMY" 2>/dev/null || true
wait "$DUMMY" 2>/dev/null || true

out=$("$BIN" --choose-port 2222 2>/dev/null)
echo "$out" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["ok"] is True; assert d["port"]==2222, d'

status=$("$BIN" --status 2>/dev/null)
echo "$status" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert "ssh_port_fallback" in d; assert "vendor_ssh_22" in d; assert "secure_console" in d; assert "debug_console" in d'

echo "PASS: SSH port fallback and status fields"
