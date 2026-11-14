# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from regress import summarize_run


class RegressSummaryTests(unittest.TestCase):
    def test_failure_details_are_bounded(self) -> None:
        detail = {
            'verdict': 'FAIL', 'guest_rc': 7, 'error': 'boom',
            'checks': {'a': 'PASS', 'b': 'FAIL'},
            'failures': [f'failure {index}' for index in range(6)],
        }
        row = summarize_run(2, 'sample', detail, 7, Path('/tmp/r.json'),
                            Path('/tmp/log'))
        self.assertEqual(row['iteration'], 2)
        self.assertEqual(row['verdict'], 'FAIL')
        self.assertEqual(row['guest_rc'], 7)
        self.assertEqual(row['error'], 'boom')
        self.assertEqual(row['failure_count'], 6)
        self.assertEqual(len(row['failures']), 5)
        self.assertEqual(row['checks'], {'a': 'PASS', 'b': 'FAIL'})
        self.assertEqual(row['result'], '/tmp/r.json')

    def test_pass_and_missing_result(self) -> None:
        row = summarize_run(1, 'boot', {'verdict': 'PASS'}, 0, None,
                            Path('/tmp/log'))
        self.assertEqual(row['verdict'], 'PASS')
        self.assertEqual(row['failures'], [])
        self.assertEqual(row['failure_count'], 0)
        self.assertIsNone(row['result'])
        self.assertEqual(row['guest_rc'], None)


if __name__ == '__main__':
    unittest.main()
