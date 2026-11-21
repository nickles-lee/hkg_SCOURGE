#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# 18_deploy_rig_agent.sh — put the rig's control page on the board.
#
#   scripts/18_deploy_rig_agent.sh [--print] [--restart]
#
# Source on this host            → target on the rig
#   tools/rigdash/pi/rig_wifi_agent.py    /usr/local/lib/rig/rig_wifi_agent.py
#   tools/rigdash/pi/rig-ap-fallback.sh   /usr/local/sbin/rig-ap-fallback
#   tools/rigdash/pi/rig-ap-fallback.service  /etc/systemd/system/
#   tools/rigdash/pi/rig-wifi-agent.service   /etc/systemd/system/
#
# The secrets these units read (/etc/rig/wifi.env) are NOT copied from here:
# scripts/17_provision_wifi_fallback.sh writes them on the rig, once.
#
# --print only shows the mapping (what goes where, from which folder) and exits.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/lib.sh"

SRC_REL="tools/rigdash/pi"
SRC="$SCOURGE_ROOT/$SRC_REL"
DRY=0
RESTART=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --print) DRY=1; shift ;;
        --restart) RESTART=1; shift ;;
        -h|--help) sed -n '4,18p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

[ -n "$PI_HOST" ] || { echo "set PI_HOST/PI_USER in config/paths.local.env" >&2; exit 1; }

# The rig's sudo needs a password unless the image was built otherwise; keep it
# in config/rig.local.env (gitignored) and feed it to `sudo -S` on stdin.
LOCAL="$SCOURGE_ROOT/config/rig.local.env"
if [ -f "$LOCAL" ]; then
    # shellcheck disable=SC1090
    source "$LOCAL"
fi

# On a stock image the login account's password is also its sudo password, so
# RIG_PASSWORD (which the first-boot template sets) answers here too.
SUDO_PASSWORD="${SUDO_PASSWORD:-${RIG_PASSWORD:-}}"

rig_sudo() {   # rig_sudo <command...>
    if [ -n "${SUDO_PASSWORD:-}" ]; then
        printf '%s\n' "$SUDO_PASSWORD" | ssh "$PI_USER@$PI_HOST" "sudo -S -p '' $*"
    else
        ssh "$PI_USER@$PI_HOST" "sudo -n $*" || {
            echo "  sudo needs a password — put SUDO_PASSWORD or RIG_PASSWORD in config/rig.local.env" >&2
            return 1
        }
    fi
}
[ -f "$SRC/rig_wifi_agent.py" ] || { echo "missing $SRC/rig_wifi_agent.py" >&2; exit 1; }

map_file() {
    case "$1" in
        rig_wifi_agent.py) echo "/usr/local/lib/rig/rig_wifi_agent.py 0755" ;;
        rig-ap-fallback.sh) echo "/usr/local/sbin/rig-ap-fallback 0755" ;;
        *.service) echo "/etc/systemd/system/$1 0644" ;;
        *) echo "" ;;
    esac
}

printf '%-44s %s\n' "from (this host)" "to (rig)"
printf '%-44s %s\n' "--------------------------------------------" "------------------------------------------"
for f in "$SRC"/*; do
    name="$(basename "$f")"
    mapping="$(map_file "$name")"
    [ -n "$mapping" ] || continue
    printf '%-44s %s\n' "$SRC_REL/$name" "${mapping% *}"
done
[ "$DRY" -eq 1 ] && exit 0

echo
echo "== copying $SRC_REL to $PI_USER@$PI_HOST"
rsync -a --relative -e 'ssh -o BatchMode=yes' "$SRC_REL" "$PI_USER@$PI_HOST:/home/$PI_USER/$PI_DIR/"

changed=0
for f in "$SRC"/*; do
    name="$(basename "$f")"
    mapping="$(map_file "$name")" || true
    [ -n "$mapping" ] || continue
    target="${mapping% *}"
    mode="${mapping#* }"
    if ssh "$PI_USER@$PI_HOST" "cmp -s /home/$PI_USER/$PI_DIR/$SRC_REL/$name $target"; then
        echo "  unchanged  $target"
        continue
    fi
    changed=1
    rig_sudo "install -D -m $mode /home/$PI_USER/$PI_DIR/$SRC_REL/$name $target"
    echo "  installed  $target"
done

if [ "$changed" -eq 1 ] || [ "$RESTART" -eq 1 ]; then
    # One sudo per command: a single password on stdin only unlocks the first
    # command of a && chain.
    rig_sudo systemctl daemon-reload
    rig_sudo systemctl restart rig-wifi-agent
    echo "  restarted  rig-wifi-agent.service"
fi
echo
echo "page: http://$PI_HOST/  (no token: reachable on the rig's own network)"
