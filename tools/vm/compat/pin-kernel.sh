#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Record the installed compat kernel, config, required modules and initramfs
# recipe into compat/kernel.lock.json. Nothing is downloaded or committed.
set -eu
cd "$(dirname "$0")/.."
python3 -m harness.compat pin-kernel "$@"
echo "next: sh compat/build-initramfs.sh"
