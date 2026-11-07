# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import hashlib
import io
from pathlib import Path
import shlex
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

VM = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(VM))

import prepare_graphics_runtime as graphics
from harness.config import load_profile, load_scenario, scenario_command
from harness.results import evaluate
from harness.audit import parse_audit
from test_storage_audit import fixture_audit


class GraphicsRuntimeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='update-runtime-', dir='/tmp/opencode')
        self.root = Path(self.temporary.name)
        self.output = self.root / 'runtime'
        self.output.mkdir()

    def tearDown(self):
        self.temporary.cleanup()

    def package(self, entries):
        payload = self.root / 'data.tar.gz'
        with tarfile.open(payload, 'w:gz') as archive:
            for name, kind, content in entries:
                item = tarfile.TarInfo(name)
                if kind == 'file':
                    item.size = len(content)
                    archive.addfile(item, io.BytesIO(content))
                else:
                    item.type = tarfile.SYMTYPE if kind == 'symlink' else tarfile.LNKTYPE
                    item.linkname = content
                    archive.addfile(item)
        deb = self.root / 'fixture.deb'
        subprocess.run(['ar', 'qc', str(deb), str(payload)], check=True, capture_output=True)
        return deb

    def test_extracts_only_shared_libraries_without_running_package_scripts(self):
        deb = self.package([
            ('./usr/lib/x86_64-linux-gnu/libfixture.so.1', 'symlink', 'libfixture.so.1.0'),
            ('./usr/lib/x86_64-linux-gnu/libfixture.so.1.0', 'file', b'not executable test data'),
            ('./usr/bin/postinst', 'file', b'not an instruction'),
            ('../../outside', 'file', b'no'),
            ('/usr/lib/x86_64-linux-gnu/libabsolute.so', 'file', b'no'),
        ])
        names = graphics.extract_libraries(deb, self.output)
        self.assertEqual(len(names), 2)
        self.assertEqual((self.output / 'libfixture.so.1').read_bytes(), b'not executable test data')
        self.assertEqual(set(p.name for p in self.output.iterdir()), set(names))
        self.assertFalse((self.root.parent / 'outside').exists())

    def test_rejects_unsafe_link_targets(self):
        for link in ('../escape.so', '/tmp/libescape.so', 'not-a-library'):
            with self.subTest(link=link):
                deb = self.package([('usr/lib/x86_64-linux-gnu/libfixture.so', 'symlink', link)])
                with self.assertRaises(ValueError):
                    graphics.extract_libraries(deb, self.output)
                deb.unlink()

    def test_rejects_hardlinks(self):
        deb = self.package([('lib/x86_64-linux-gnu/libfixture.so', 'hardlink', '/outside')])
        with self.assertRaisesRegex(ValueError, 'unsupported library'):
            graphics.extract_libraries(deb, self.output)

    def test_rejects_unresolved_links(self):
        deb = self.package([('lib/x86_64-linux-gnu/libfixture.so', 'symlink', 'libmissing.so.1')])
        with self.assertRaisesRegex(ValueError, 'unresolved'):
            graphics.extract_libraries(deb, self.output)

    def test_rejects_duplicate_members(self):
        entry = ('lib/x86_64-linux-gnu/libfixture.so', 'file', b'test')
        deb = self.package([entry, entry])
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            graphics.extract_libraries(deb, self.output)

    def test_does_not_overwrite_existing_file_or_follow_existing_symlink(self):
        keep = self.root / 'keep'
        keep.write_bytes(b'user bytes')
        entry = self.output / 'libfixture.so'
        entry.symlink_to(keep)
        deb = self.package([('lib/x86_64-linux-gnu/libfixture.so', 'file', b'replace')])
        with self.assertRaisesRegex(ValueError, 'collision'):
            graphics.extract_libraries(deb, self.output)
        self.assertEqual(keep.read_bytes(), b'user bytes')

    def test_rejects_package_without_runtime_libraries(self):
        deb = self.package([('usr/bin/tool', 'file', b'test')])
        with self.assertRaisesRegex(ValueError, 'no runtime libraries'):
            graphics.extract_libraries(deb, self.output)

    def test_package_pin_is_enforced_for_download_and_cache_bytes(self):
        content = b'!<arch>\nfixture'
        with patch.dict(graphics.SHA256, {'fixture': hashlib.sha256(content).hexdigest()}):
            self.assertEqual(graphics.verify_package('fixture_1_amd64.deb', content),
                             hashlib.sha256(content).hexdigest())
            with self.assertRaisesRegex(ValueError, 'digest mismatch'):
                graphics.verify_package('fixture_1_amd64.deb', content + b'modified')
        self.assertEqual(set(graphics.SHA256), {name.split('_')[0] for _, name in graphics.PACKAGES})

    def test_bundle_builder_refuses_existing_output_before_any_network_access(self):
        with patch.object(sys, 'argv', ['prepare_graphics_runtime.py', '--output', str(self.output)]), \
                patch.object(graphics.urllib.request, 'urlopen') as network:
            with self.assertRaises(ValueError):
                graphics.main()
            network.assert_not_called()


class UpdateScenarioTests(unittest.TestCase):
    def test_decrypt_probe_never_accepts_exit_zero_bytes_or_elf_as_stock_proof(self):
        # Execute our own probe logic with an inert shell stand-in, never a
        # firmware executable. Includes the no-trace path used in the real VM.
        with tempfile.TemporaryDirectory(prefix='crypto-verdict-', dir='/tmp/opencode') as directory:
            root = Path(directory)
            fake = root / 'fake-decryptor'
            output = root / 'fixture-output'
            fake.write_text('#!/bin/sh\ncat ' + shlex.quote(str(output)) + '\n')
            fake.chmod(0o700)
            probe = (VM / 'probes/keyfile-candidates.sh').read_text().split('matches=0\n', 1)[1]
            probe = 'set -eu\nmatches=0\n' + probe
            probe = probe.replace('/app/share/AppUpgrade/DecryptToPIPE', str(fake))
            probe = probe.replace('/app/bin/AppUpgrade', str(output))
            probe = probe.replace('/tmp/keyfile-probes', str(root))
            for data in (b'not plaintext\n  ! mbedtls_rsa_pkcs1_decrypt returned -16512\n',
                         b'\x7fELFcontrolled test fixture, not a firmware executable'):
                output.write_bytes(data)
                result = subprocess.run(['bash', '-c', probe], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 77, result.stdout + result.stderr)
                self.assertNotIn('HARNESS_CHECK real-keyfile-decryption PASS', result.stdout)
                self.assertIn('HARNESS_BLOCKED', result.stdout)

    def test_audit_reports_zero_entropy_without_claiming_getrandom_was_tested(self):
        report = parse_audit(fixture_audit({'entropy': '0'}))
        self.assertTrue(any('getrandom may block' in gap for gap in report['gaps']))
        report = parse_audit(fixture_audit({'entropy': '512'}))
        self.assertFalse(any('getrandom may block' in gap for gap in report['gaps']))

    def test_new_scripts_are_valid_bash_without_target_execution_on_host(self):
        scripts = [VM / 'probes/graphics-diagnose.sh', VM / 'probes/keyfile-candidates.sh',
                   VM / 'toolsroot/phase4.sh']
        commands = [p.read_text() for p in scripts]
        for name in ('entropy-ready', 'update-graphics'):
            scenario = load_scenario(name)
            commands.append(scenario_command(scenario, load_profile('conformance')))
        for text in commands:
            result = subprocess.run(['bash', '-n'], input=text, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_graphics_does_not_pass_when_only_context_and_timer_are_observed(self):
        scenario = load_scenario('update-graphics')
        text = 'GL_VERSION: OpenGL ES 3.0\nGRAPHICS_PROBE_TIMER\nQML_PROBE_VERDICT_RC=1\n'
        self.assertEqual(evaluate(text, 1, scenario)['verdict'], 'FAIL')
        self.assertEqual(evaluate(text, 0, scenario)['verdict'], 'FAIL')

    def test_entropy_is_an_explicit_substrate_substitution_not_crypto_bypass(self):
        scenario = load_scenario('entropy-ready')
        text = 'HARNESS_SUBSTITUTION cpu-rng-entropy\nHARNESS_CHECK entropy-ready PASS\n'
        result = evaluate(text, 0, scenario, load_profile('conformance'))
        self.assertEqual(result['verdict'], 'PASS')
        self.assertEqual(result['applied_substitutions'], ['cpu-rng-entropy'])
        self.assertEqual(evaluate('HARNESS_BLOCKED virtual CPU RDRAND is unavailable\n', 77,
                                  scenario)['verdict'], 'BLOCKED')


if __name__ == '__main__':
    unittest.main()
