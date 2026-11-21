#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""01b_audit_inputs.py — audit staged firmware inputs against the locks.

Run automatically by 01_stage_inputs.sh; also safe to run by hand:

  scripts/01b_audit_inputs.py --layout staged --root input
  scripts/01b_audit_inputs.py --layout package --root /path/to/IONIQ5_EU

Exit codes: 0 = consistent (warnings allowed), 1 = error, 2 = usage.
"""
from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "tools"))

from firmware_audit import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
