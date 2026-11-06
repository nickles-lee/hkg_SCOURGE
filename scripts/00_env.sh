#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# 00_env.sh — create the Python venv used by the build tools.
#
# Build dependencies (deliberately small — no Ghidra needed for builds):
#   pycryptodome, deflate (libdeflate bindings), cryptography.
#   PyQt5 is OPTIONAL (only for the .qm runtime sanity check).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/lib.sh"

VENV="${SCOURGE_ROOT}/.venv"
python3 -m venv "$VENV"
"$VENV/bin/pip" install --quiet --upgrade pip
"$VENV/bin/pip" install --quiet pycryptodome deflate cryptography
# Optional; the patcher skips the runtime .qm check when absent.
"$VENV/bin/pip" install --quiet PyQt5 || echo "(PyQt5 not installed — optional .qm check will be skipped)"

"$VENV/bin/python" - <<'PY'
import Crypto, deflate, cryptography
print("pycryptodome", Crypto.__version__)
print("deflate", deflate.__version__)
print("cryptography", cryptography.__version__)
try:
    import PyQt5  # noqa
    print("PyQt5 ok (optional .qm check enabled)")
except Exception:
    print("(PyQt5 absent — optional .qm check disabled)")
PY
echo "venv ready: $VENV"
