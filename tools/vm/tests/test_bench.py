# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from bench import (build_argv, compare_bench, result_path, source_state,
                   resolve_bench_arg, row_verdict, summarize, validate_child,
                   validate_matrix, variants)
from harness.result_schema import check_all_bench, validate_bench
from harness.config import load_scenario

VM = Path(__file__).resolve().parents[1]


def args(**overrides):
    values = {
        'profile': 'conformance', 'kernel': 'vendor', 'timeout': 120,
        'storage': 'flat',
        'storage_size': [], 'cache': None, 'smp': None, 'memory': None,
        'pin_cpu': None, 'apic': False, 'usb': None, 'tools': None,
        'payload': None, 'share': None, 'matrix': False,
        'matrix_cache': [], 'matrix_cpu': [],
    }
    values.update(overrides)
    return SimpleNamespace(**values)


def bench_doc(summary=None, metrics=None, **overrides):
    data = {
        'schema_version': 1,
        'run': 'test',
        'ok': True,
        'repeats': 1,
        'warmup': 0,
        'scenarios': ['boot'],
        'variants': [{'cache': None, 'pin_cpu': None}],
        'rows': [],
        'summary': summary or {},
        'metrics': metrics or {},
    }
    data.update(overrides)
    return data


def child_result(**overrides):
    data = {
        'schema_version': 3, 'run_dir': '/tmp/child', 'verdict': 'PASS',
        'rc': 0, 'timeout': 30, 'profile': {'name': 'conformance'},
        'images': {}, 'overlays': {}, 'applied_substitutions': [],
    }
    data.update(overrides)
    return data


class BenchTests(unittest.TestCase):
    def test_check_all_bench_reports_documents(self) -> None:
        with tempfile.TemporaryDirectory(prefix='vm-bench-all-') as tmp:
            root = Path(tmp)
            good = root / 'runs' / 'bench-good'
            good.mkdir(parents=True)
            (good / 'bench.json').write_text(json.dumps(
                {'ok': True, 'repeats': 1, 'scenarios': ['boot'],
                 'rows': [], 'summary': {}}))
            bad = root / 'runs' / 'bench-bad'
            bad.mkdir()
            (bad / 'bench.json').write_text(json.dumps({'ok': 'yes'}))
            empty = root / 'runs' / 'bench-empty'
            empty.mkdir()
            report = check_all_bench(root)
            self.assertFalse(report['ok'])
            self.assertEqual(report['checked'], 2)
            self.assertEqual(report['valid'], 1)
            self.assertEqual(report['invalid'], 1)
            self.assertEqual(report['problems'][0]['bench'], 'bench-bad')
            (bad / 'bench.json').unlink()
            report = check_all_bench(root)
            self.assertTrue(report['ok'])
            self.assertEqual(report['checked'], 1)

    def test_bench_schema_row_consistency(self) -> None:
        row = {'scenario': 'boot', 'iteration': 1, 'cache': None,
               'pin_cpu': None, 'verdict': 'PASS', 'rc': 0, 'log': 'x.log'}
        self.assertEqual(validate_bench(bench_doc(rows=[row])), [])
        missing = bench_doc(rows=[dict(row, scenario='other')])
        self.assertIn('not a benchmarked scenario',
                      '\n'.join(validate_bench(missing)))
        variant = bench_doc(rows=[dict(row, cache='writeback')])
        self.assertIn('not a declared variant',
                      '\n'.join(validate_bench(variant)))
        too_many = bench_doc(rows=[dict(row, iteration=2)])
        self.assertIn('exceeds repeats', '\n'.join(validate_bench(too_many)))
        warm = bench_doc(warmups=[{'scenario': 'boot', 'warmup': 1,
                                   'cache': None, 'pin_cpu': None,
                                   'verdict': 'PASS', 'rc': 0,
                                   'log': 'w.log'}])
        self.assertIn('exceeds warmup count',
                      '\n'.join(validate_bench(warm)))
        warm_variant = bench_doc(warmups=[{'scenario': 'boot', 'warmup': 1,
                                           'cache': 'writeback',
                                           'pin_cpu': None,
                                           'verdict': 'PASS', 'rc': 0,
                                           'log': 'w.log'}],
                                 warmup=1)
        self.assertIn('not a declared variant',
                      '\n'.join(validate_bench(warm_variant)))

    def test_bench_schema_rejects_duplicates(self) -> None:
        row = {'scenario': 'boot', 'iteration': 1, 'cache': None,
               'pin_cpu': None, 'verdict': 'PASS', 'rc': 0, 'log': 'x.log'}
        warm = {'scenario': 'boot', 'warmup': 1, 'cache': None,
                'pin_cpu': None, 'verdict': 'PASS', 'rc': 0, 'log': 'w.log'}
        document = bench_doc(
            variants=[{'cache': None, 'pin_cpu': None},
                      {'cache': None, 'pin_cpu': None}],
            rows=[row, dict(row)], warmups=[warm, dict(warm)], warmup=1)
        joined = '\n'.join(validate_bench(document))
        self.assertIn('duplicates an earlier variant', joined)
        self.assertIn('duplicates an earlier row', joined)
        self.assertIn('duplicates an earlier warmup', joined)

    def test_validate_child_result(self) -> None:
        self.assertEqual(validate_child(child_result(), 0), ('PASS', []))
        verdict, problems = validate_child(child_result(), 2)
        self.assertEqual(verdict, 'FAIL')
        self.assertEqual(problems, [])
        verdict, problems = validate_child(child_result(verdict='BAD'), 0)
        self.assertEqual(verdict, 'FAIL')
        self.assertTrue(any('verdict' in problem for problem in problems))
        verdict, problems = validate_child({}, 0)
        self.assertEqual(verdict, 'FAIL')
        self.assertIn('missing', problems[0])

    def test_row_verdict_normalizes_rc_mismatch(self) -> None:
        self.assertEqual(row_verdict('PASS', 0), 'PASS')
        self.assertEqual(row_verdict('PASS', 2), 'FAIL')
        self.assertEqual(row_verdict('BLOCKED', 77), 'BLOCKED')
        self.assertEqual(row_verdict(None, 1), 'FAIL')

    def test_summarize(self) -> None:
        self.assertEqual(summarize([]), {})
        self.assertEqual(summarize([2.0, 4.0, 6.0]),
                         {'count': 3, 'mean': 4.0, 'median': 4.0,
                          'min': 2.0, 'max': 6.0})

    def test_build_argv_passes_required_and_optional_flags(self) -> None:
        scenario = load_scenario('can-smoke')
        argv = build_argv(args(cache='writeback', pin_cpu=0, smp=2,
                               memory=3072, payload=VM / 'payload/tweaks-vm',
                               storage_size=['update=32']), scenario)
        joined = ' '.join(argv)
        self.assertIn('--no-lock', argv)
        self.assertIn('--scenario can-smoke', joined)
        self.assertIn('--cache writeback', joined)
        self.assertIn('--pin-cpu 0', joined)
        self.assertIn('--smp 2', joined)
        self.assertIn('--memory 3072', joined)
        self.assertIn('--storage-size update=32', joined)
        self.assertIn('--payload', argv)

    def test_variants_single_and_matrix(self) -> None:
        self.assertEqual(variants(args()), [(None, None)])
        self.assertEqual(variants(args(matrix=True)),
                         [('writeback', None), ('writethrough', None)])
        self.assertEqual(
            variants(args(matrix=True, matrix_cache=['directsync'],
                          matrix_cpu=[0, 1])),
            [('directsync', None), ('directsync', 0), ('directsync', 1)])
        self.assertEqual(variants(args(cache='unsafe', pin_cpu=2)),
                         [('unsafe', 2)])

    def test_validate_matrix_rejects_unavailable_cpu(self) -> None:
        allowed = os.sched_getaffinity(0)
        missing = max(allowed) + 1
        while missing in allowed:
            missing += 1
        with self.assertRaisesRegex(ValueError, 'matrix cpu .* not available'):
            validate_matrix(args(matrix=True, matrix_cpu=[missing]))
        validate_matrix(args(matrix=True, matrix_cpu=[sorted(allowed)[0]]))
        validate_matrix(args())

    def test_variants_deduplicate_caches(self) -> None:
        self.assertEqual(
            variants(args(matrix=True,
                          matrix_cache=['writeback', 'writeback'],
                          matrix_cpu=[0, 0])),
            [('writeback', None), ('writeback', 0)])

    def test_build_argv_cache_pin_override(self) -> None:
        scenario = load_scenario('boot')
        argv = build_argv(args(kernel='compat'), scenario,
                          cache='writethrough', pin_cpu=3)
        joined = ' '.join(argv)
        self.assertIn('--kernel compat', joined)
        self.assertIn('--cache writethrough', joined)
        self.assertIn('--pin-cpu 3', joined)

    def test_mode_share_and_visible_frame_wiring(self) -> None:
        wifi = load_scenario('wifi-ui')
        argv = build_argv(args(kernel='compat', share=Path('/tmp/share')),
                          wifi)
        joined = ' '.join(argv)
        self.assertIn('--kernel compat', joined)
        self.assertIn('--share /tmp/share', joined)
        with self.assertRaisesRegex(ValueError, 'requires --kernel compat'):
            build_argv(args(), wifi)
        with self.assertRaisesRegex(ValueError, 'requires --share'):
            build_argv(args(kernel='compat'), wifi)

        smoke = load_scenario('ui-smoke')
        argv = build_argv(args(kernel='compat'), smoke)
        self.assertIn('--qmp-screenshot', argv)
        with self.assertRaisesRegex(ValueError, 'requires --kernel compat'):
            build_argv(args(), smoke)

    def test_missing_required_input_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, 'requires --payload'):
            build_argv(args(), load_scenario('can-smoke'))

    def test_resolve_bench_arg_accepts_directory(self) -> None:
        with tempfile.TemporaryDirectory(prefix='vm-bench-dir-') as tmp:
            root = Path(tmp)
            (root / 'bench.json').write_text('{}')
            self.assertEqual(resolve_bench_arg(root), root / 'bench.json')
            file_path = root / 'bench.json'
            self.assertEqual(resolve_bench_arg(file_path), file_path)

    def test_check_bench_cli(self) -> None:
        with tempfile.TemporaryDirectory(prefix='vm-check-bench-') as tmp:
            root = Path(tmp)
            good = root / 'good'
            good.mkdir()
            (good / 'bench.json').write_text(json.dumps(bench_doc()))
            completed = subprocess.run(
                [sys.executable, '-B', str(VM / 'launch.py'), 'check-bench',
                 str(good)], capture_output=True, text=True, check=False)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertTrue(json.loads(completed.stdout)['ok'])

            bad = root / 'bad'
            bad.mkdir()
            (bad / 'bench.json').write_text('{not json')
            completed = subprocess.run(
                [sys.executable, '-B', str(VM / 'launch.py'), 'check-bench',
                 str(bad)], capture_output=True, text=True, check=False)
            self.assertEqual(completed.returncode, 1)
            self.assertIn('cannot read', completed.stdout)

            missing = subprocess.run(
                [sys.executable, '-B', str(VM / 'launch.py'), 'check-bench',
                 str(root / 'missing')], capture_output=True, text=True,
                check=False)
            self.assertEqual(missing.returncode, 1)
            self.assertIn('cannot read', missing.stdout)

            all_root = root / 'all-root'
            (all_root / 'bench-good').mkdir(parents=True)
            (all_root / 'bench-good' / 'bench.json').write_text(
                json.dumps(bench_doc()))
            every = subprocess.run(
                [sys.executable, '-B', str(VM / 'launch.py'), 'check-bench',
                 '--all', '--root', str(all_root)],
                capture_output=True, text=True, check=False)
            report = json.loads(every.stdout)
            self.assertEqual(report['checked'], 1)
            self.assertEqual(report['valid'], 1)
            self.assertEqual(every.returncode, 0)
            (all_root / 'bench-bad').mkdir()
            (all_root / 'bench-bad' / 'bench.json').write_text('{bad')
            every = subprocess.run(
                [sys.executable, '-B', str(VM / 'launch.py'), 'check-bench',
                 '--all', '--root', str(all_root)],
                capture_output=True, text=True, check=False)
            report = json.loads(every.stdout)
            self.assertEqual(report['checked'], 2)
            self.assertEqual(report['invalid'], 1)
            self.assertEqual(every.returncode, 1)

    def test_compare_cli_accepts_bench_directory(self) -> None:
        with tempfile.TemporaryDirectory(prefix='vm-bench-dir-') as tmp:
            root = Path(tmp)
            document = bench_doc(summary={
                'boot cache=default pin=none': {
                    'count': 1, 'mean': 10.0, 'median': 10.0,
                    'min': 10.0, 'max': 10.0}})
            (root / 'bench.json').write_text(json.dumps(document))
            completed = subprocess.run(
                [sys.executable, '-B', str(VM / 'bench.py'), '--compare',
                 str(root), str(root)], capture_output=True, text=True,
                check=False)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            report = json.loads(completed.stdout)
            self.assertTrue(report['ok'])
            self.assertEqual(report['baseline'], 'test')

    def test_result_path_reads_run_line(self) -> None:
        with tempfile.TemporaryDirectory(prefix='vm-bench-') as tmp:
            log = Path(tmp) / 'case.log'
            log.write_text('noise\nrun: /tmp/example-run\nmore\n')
            self.assertEqual(result_path(log), Path('/tmp/example-run/result.json'))
            log.write_text('no result\n')
            self.assertIsNone(result_path(log))

    def test_source_state_has_revision(self) -> None:
        state = source_state()
        self.assertIsInstance(state['git_head'], str)
        self.assertTrue(state['git_head'])
        self.assertIsInstance(state['dirty'], bool)

    def test_warmup_is_recorded_but_not_measured(self) -> None:
        completed = subprocess.run(
            [sys.executable, "-B", str(VM / "bench.py"),
             "--scenario", "stock-update-stage", "--repeats", "1",
             "--warmup", "1", "--usb", "/dev/null", "--tools", "/dev/null",
             "--lock-timeout", "10"],
            cwd=VM, capture_output=True, text=True, timeout=60)
        combined = completed.stdout + completed.stderr
        if "another VM boot holds" in combined or "lock timeout" in combined:
            self.skipTest("VM lab is busy with another run")
        self.assertEqual(completed.returncode, 1, combined)
        line = next(line for line in completed.stdout.splitlines()
                    if line.startswith("bench: "))
        bench_path = Path(line[7:])
        try:
            bench = json.loads(bench_path.read_text())
            self.assertEqual(validate_bench(bench), [])
            self.assertEqual(bench["warmup"], 1)
            self.assertEqual(len(bench["warmups"]), 1)
            self.assertEqual(len(bench["rows"]), 1)
            self.assertEqual(bench["warmups"][0]["verdict"], "BLOCKED")
            self.assertEqual(bench["rows"][0]["verdict"], "BLOCKED")
            self.assertFalse(bench["ok"])
            self.assertIn("source", bench)
        finally:
            shutil.rmtree(bench_path.parent, ignore_errors=True)

    def test_compare_bench_statuses(self) -> None:
        base = {'summary': {'a': {'mean': 10.0}, 'b': {'mean': 20.0},
                            'c': {'mean': 30.0}, 'd': {'mean': 0.0}}}
        current = {'summary': {'a': {'mean': 10.2}, 'b': {'mean': 22.0},
                               'c': {'mean': 28.0}, 'd': {'mean': 1.0}}}
        report = compare_bench(base, current, threshold=0.05)
        statuses = {row['key']: row['status'] for row in report['rows']}
        self.assertEqual(statuses, {'a': 'SAME', 'b': 'REGRESSED',
                                    'c': 'IMPROVED', 'd': 'REGRESSED'})
        self.assertFalse(report['ok'])
        self.assertEqual(report['rows'][1]['pct'], 10.0)

    def test_compare_metric_readiness(self) -> None:
        base = {'metrics': {'k': {'boot_seconds': {'mean': 10.0},
                                  'readiness_seconds': {'mean': 20.0}}}}
        current = {'metrics': {'k': {'boot_seconds': {'mean': 10.0},
                                     'readiness_seconds': {'mean': 21.2}}}}
        report = compare_bench(base, current, threshold=0.05,
                               metric='readiness_seconds')
        self.assertEqual(report['metric'], 'readiness_seconds')
        self.assertEqual(report['rows'][0]['status'], 'REGRESSED')
        self.assertAlmostEqual(report['rows'][0]['pct'], 6.0)
        self.assertFalse(report['ok'])

    def test_compare_metric_missing_from_legacy_docs(self) -> None:
        report = compare_bench({'summary': {'k': {'mean': 10.0}}},
                               {'summary': {'k': {'mean': 10.0}}},
                               metric='readiness_seconds')
        self.assertEqual(report['rows'], [])
        self.assertEqual(report['error'], 'no comparable metrics')
        self.assertFalse(report['ok'])

    def test_compare_metric_legacy_rows_fallback(self) -> None:
        base = {'rows': [{'scenario': 'boot', 'cache': None, 'pin_cpu': None,
                          'readiness_seconds': 4.0}]}
        current = {'rows': [{'scenario': 'boot', 'cache': None, 'pin_cpu': None,
                             'readiness_seconds': 4.4}]}
        report = compare_bench(base, current, threshold=0.05,
                               metric='readiness_seconds')
        self.assertEqual(report['rows'][0]['key'],
                         'boot cache=default pin=none')
        self.assertEqual(report['rows'][0]['status'], 'REGRESSED')

    def test_compare_reports_missing_keys(self) -> None:
        report = compare_bench({'summary': {'a': {'mean': 1.0}}},
                               {'summary': {'b': {'mean': 1.0}}})
        self.assertFalse(report['ok'])
        self.assertEqual([row['status'] for row in report['rows']],
                         ['MISSING', 'MISSING'])

    def test_bench_schema_modern_and_legacy(self) -> None:
        self.assertEqual(validate_bench(bench_doc()), [])
        legacy = {'ok': True, 'repeats': 1, 'scenarios': ['boot'],
                  'rows': [], 'summary': {}}
        self.assertEqual(validate_bench(legacy), [])
        self.assertIn('run must be a non-empty string',
                      '\n'.join(validate_bench(bench_doc(run=''))))
        bad = bench_doc()
        bad['rows'] = [{'scenario': 'boot', 'iteration': 0, 'verdict': 'PASS',
                        'rc': 0, 'log': ''}]
        self.assertIn('rows[0].iteration', '\n'.join(validate_bench(bad)))
        bad = bench_doc(summary={'k': {'count': 1, 'mean': 'fast'}})
        self.assertIn('summary', '\n'.join(validate_bench(bad)))

    def test_invalid_matrix_cpu_rejected_before_lock(self) -> None:
        allowed = os.sched_getaffinity(0)
        missing = max(allowed) + 1
        while missing in allowed:
            missing += 1
        before = set((VM / 'runs').glob('bench-*'))
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'bench.py'), '--scenario', 'boot',
             '--matrix', '--matrix-cpu', str(missing)],
            cwd=VM, capture_output=True, text=True, check=False)
        self.assertEqual(completed.returncode, 2, completed.stderr)
        self.assertIn('not available', completed.stderr)
        self.assertEqual(set((VM / 'runs').glob('bench-*')), before)

    def test_compare_cli_rejects_invalid_threshold(self) -> None:
        for value in ('-1', 'nan', 'inf'):
            completed = subprocess.run(
                [sys.executable, '-B', str(VM / 'bench.py'), '--compare',
                 'a.json', 'b.json', '--threshold', value],
                cwd=VM, capture_output=True, text=True, check=False)
            self.assertEqual(completed.returncode, 2, completed.stderr)
            self.assertIn('threshold', completed.stderr)

    def test_compare_cli_rejects_malformed_documents(self) -> None:
        with tempfile.TemporaryDirectory(prefix='vm-bench-bad-') as tmp:
            left = Path(tmp) / 'base.json'
            right = Path(tmp) / 'current.json'
            left.write_text(json.dumps({'summary': {}}))
            right.write_text(json.dumps(bench_doc()))
            completed = subprocess.run(
                [sys.executable, '-B', str(VM / 'bench.py'), '--compare',
                 str(left), str(right)], capture_output=True, text=True, check=False)
            self.assertEqual(completed.returncode, 2, completed.stderr)
            report = json.loads(completed.stdout)
            self.assertFalse(report['ok'])
            self.assertTrue(report['baseline_problems'])

    def test_compare_shape_mismatch(self) -> None:
        summary = {'k': {'count': 1, 'mean': 10.0, 'median': 10.0,
                         'min': 10.0, 'max': 10.0}}
        base = bench_doc(summary=summary, repeats=3)
        current = bench_doc(summary=summary, repeats=1)
        report = compare_bench(base, current)
        self.assertFalse(report['shape_match'])
        self.assertIn('repeats', report['shape_warning'])
        self.assertTrue(report['ok'])
        strict = compare_bench(base, current, require_same_shape=True)
        self.assertFalse(strict['ok'])
        same = compare_bench(base, base, require_same_shape=True)
        self.assertTrue(same['shape_match'])
        self.assertTrue(same['ok'])

    def test_compare_source_mismatch(self) -> None:
        base = bench_doc(summary={'k': {'count': 1, 'mean': 10.0,
                                        'median': 10.0, 'min': 10.0, 'max': 10.0}},
                         source={'git_head': 'a' * 40, 'dirty': False})
        current = bench_doc(summary={'k': {'count': 1, 'mean': 10.0,
                                           'median': 10.0, 'min': 10.0, 'max': 10.0}},
                            source={'git_head': 'b' * 40, 'dirty': False})
        report = compare_bench(base, current)
        self.assertFalse(report['source_match'])
        self.assertIn('git_head differs', report['source_warning'])
        self.assertTrue(report['ok'])
        strict = compare_bench(base, current, require_source_match=True)
        self.assertFalse(strict['source_match'])
        self.assertFalse(strict['ok'])

    def test_compare_cli_exit_codes(self) -> None:
        baseline = bench_doc({'boot cache=default pin=none': {'count': 1, 'mean': 10.0,
                                                              'median': 10.0, 'min': 10.0,
                                                              'max': 10.0}})
        current = bench_doc({'boot cache=default pin=none': {'count': 1, 'mean': 10.2,
                                                             'median': 10.2, 'min': 10.2,
                                                             'max': 10.2}})
        with tempfile.TemporaryDirectory(prefix='vm-bench-compare-') as tmp:
            left = Path(tmp) / 'base.json'
            right = Path(tmp) / 'current.json'
            left.write_text(json.dumps(baseline))
            right.write_text(json.dumps(current))
            same = subprocess.run(
                [sys.executable, '-B', str(VM / 'bench.py'), '--compare',
                 str(left), str(right), '--threshold', '0.05'],
                capture_output=True, text=True, check=False)
            self.assertEqual(same.returncode, 0, same.stderr)
            self.assertEqual(json.loads(same.stdout)['ok'], True)
            worse = subprocess.run(
                [sys.executable, '-B', str(VM / 'bench.py'), '--compare',
                 str(left), str(right), '--threshold', '0.01'],
                capture_output=True, text=True, check=False)
            self.assertEqual(worse.returncode, 1)
            self.assertEqual(json.loads(worse.stdout)['rows'][0]['status'],
                             'REGRESSED')

    def test_failure_path_writes_bench_json_and_exits_nonzero(self) -> None:
        completed = subprocess.run(
            [sys.executable, "-B", str(VM / "bench.py"),
             "--scenario", "stock-update-stage", "--repeats", "1",
             "--usb", "/dev/null", "--tools", "/dev/null",
             "--lock-timeout", "10"],
            cwd=VM, capture_output=True, text=True, timeout=60)
        combined = completed.stdout + completed.stderr
        if "another VM boot holds" in combined or "lock timeout" in combined:
            self.skipTest("VM lab is busy with another run")
        self.assertEqual(completed.returncode, 1, combined)
        line = next(line for line in completed.stdout.splitlines()
                    if line.startswith("bench: "))
        bench_path = Path(line[7:])
        try:
            bench = json.loads(bench_path.read_text())
            self.assertEqual(validate_bench(bench), [])
            self.assertFalse(bench["ok"])
            self.assertEqual(bench["rows"][0]["verdict"], "BLOCKED")
            self.assertEqual(bench["rows"][0]["rc"], 77)
        finally:
            shutil.rmtree(bench_path.parent, ignore_errors=True)

    def test_cli_help_exposes_repeats(self) -> None:
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'bench.py'), '--help'],
            capture_output=True, text=True, check=True)
        self.assertIn('--repeats', completed.stdout)
        self.assertIn('--matrix', completed.stdout)
        self.assertIn('--kernel', completed.stdout)
        self.assertIn('--compare', completed.stdout)
        self.assertIn('--metric', completed.stdout)
        self.assertIn('--require-source-match', completed.stdout)
        self.assertIn('--require-same-shape', completed.stdout)
        self.assertIn('--warmup', completed.stdout)


if __name__ == '__main__':
    unittest.main()
