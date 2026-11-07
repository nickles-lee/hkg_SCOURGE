# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Read actual manager query replies; never substitute missing states with readiness."""
from __future__ import annotations

import re


def parse_manager_snapshots(text: str) -> dict:
    snapshots = {}
    current = None
    method = None
    for line in text.splitlines():
        if line.startswith('MANAGER_SNAPSHOT '):
            current = line.split(' ', 1)[1]
            if current in snapshots:
                raise ValueError('duplicate manager snapshot')
            snapshots[current] = {}
            method = None
        elif current and line.startswith('MANAGER_QUERY '):
            method = line.split(' ', 1)[1]
            if method in snapshots[current]:
                raise ValueError('duplicate manager query')
            snapshots[current][method] = None
        elif current and method:
            match = re.fullmatch(r'\s+int32 (-?\d+)\s*', line)
            if match:
                snapshots[current][method] = int(match[1])
                method = None
            elif line.startswith(('Error ', 'MANAGER_', '===', 'HARNESS_')):
                method = None
    return {'schema_version': 1, 'snapshots': snapshots,
            'update_ready_observed': any(s.get('ask_standby_status') == 12 and
                                         s.get('ask_acc_status') == 1 and
                                         s.get('ask_ign_status') == 1 for s in snapshots.values()),
            'stock_equivalence_verified': False,
            'limitations': ['Query snapshots are observations, not proof of full service initialization.',
                            'Standby 11 is not AppDMClient update-ready standby 12.',
                            'A missing/error reply remains null, not success.']}
