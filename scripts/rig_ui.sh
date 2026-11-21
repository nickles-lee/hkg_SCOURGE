#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# rig_ui.sh — serve the build/provisioning dashboard on 127.0.0.1:8765.
#
#   scripts/rig_ui.sh [--port 8765] [--host 127.0.0.1]
#
# Installs the dashboard's own runtime into .venv on first use (fastapi,
# uvicorn, jinja2 — pinned in tools/rigdash/requirements.txt) and then serves.
# Secrets stay in config/rig.local.env: the page shows whether they are filled
# in, never what they are.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/lib.sh"

HOST="127.0.0.1"
PORT="8765"
while [ "$#" -gt 0 ]; do
    case "$1" in
        --host) HOST="${2:?--host needs a value}"; shift 2 ;;
        --port) PORT="${2:?--port needs a value}"; shift 2 ;;
        -h|--help) sed -n '4,12p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

REQ="$SCOURGE_ROOT/tools/rigdash/requirements.txt"
[ -x "$PYTHON" ] || {
    echo "no venv at $PYTHON — run scripts/00_env.sh first" >&2
    exit 1
}
if ! "$PYTHON" -c 'import fastapi, uvicorn, jinja2' >/dev/null 2>&1; then
    echo "== installing the dashboard runtime into .venv"
    if command -v uv >/dev/null 2>&1; then
        uv pip install --quiet --python "$PYTHON" -r "$REQ"
    else
        "$PYTHON" -m pip install --quiet -r "$REQ"
    fi
fi

# The rig's secrets file also carries the dashboard token.
LOCAL="$SCOURGE_ROOT/config/rig.local.env"
if [ -f "$LOCAL" ]; then
    # shellcheck disable=SC1090
    source "$LOCAL"
fi

case "$HOST" in
    127.0.0.1|localhost|::1)
        echo "dashboard: http://$HOST:$PORT  (Ctrl-C to stop)"
        ;;
    *)
        [ -n "${RIGDASH_TOKEN:-}" ] || {
            echo "refusing to listen on $HOST without RIGDASH_TOKEN: this page can" >&2
            echo "run build scripts and sudo on the rig. Add one to config/rig.local.env" >&2
            echo "  openssl rand -hex 16" >&2
            exit 1
        }
        # hostname -I is empty or missing on some hosts; ask the kernel instead.
        LAN_IP="$(ip -4 -o addr show scope global 2>/dev/null |
                  awk '{print $4}' | cut -d/ -f1 | head -n1 || true)"
        echo "dashboard: http://${LAN_IP:-<this-host>}:$PORT/?token=$RIGDASH_TOKEN"
        echo "           (open that once; the cookie carries it afterwards)"
        ;;
esac
cd "$SCOURGE_ROOT"
exec "$PYTHON" -m uvicorn tools.rigdash.app:app --host "$HOST" --port "$PORT"
