#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Install the SCOURGE policy hooks (AGENTS.md, Enforcement) for THIS clone:
# git config core.hooksPath scripts/githooks
# Run once per clone; nothing is copied and `git config --unset core.hooksPath`
# undoes it. Git requires the hook files to be executable, so this also
# restores the +x bits the repo convention says not to rely on elsewhere.

set -e
REPO=$(git rev-parse --show-toplevel)
[ -n "$REPO" ] || { echo "not a git repo" >&2; exit 2; }

chmod +x "$REPO/scripts/githooks/pre-commit" "$REPO/scripts/githooks/pre-push"

if [ "${1:-}" = "-y" ]; then
    ans=y
else
    printf "Install SCOURGE policy hooks (core.hooksPath) for this clone? [y/N] "
    read -r ans
fi
case "$ans" in
    y|Y|yes|Yes)
        git config core.hooksPath scripts/githooks
        echo "installed: $(git config core.hooksPath) (pre-commit, pre-push)"
        ;;
    *)
        echo "skipped (run again, or: git config core.hooksPath scripts/githooks)"
        ;;
esac