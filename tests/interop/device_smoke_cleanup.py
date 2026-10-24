#!/usr/bin/env python3
"""Check failed native-smoke cleanup without starting a VM or changing an ACL."""
import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

Spec = importlib.util.spec_from_file_location('device_smoke', Path(__file__).with_name('device_smoke.py'))
Smoke = importlib.util.module_from_spec(Spec)
Spec.loader.exec_module(Smoke)


class cleanup_test_t(unittest.TestCase):
    """Own private fake tool state; assert evidence and retention on each exit."""

    def run_failure(self, *, kill_fails=False, restore_fails=False, save_fails=False):
        """Return cleanup evidence after a deliberate init failure and dispose it."""
        with tempfile.TemporaryDirectory(prefix='waddle-smoke-test-') as directory:
            root = Path(directory)
            binary = root / 'waddle'
            binary.write_bytes(b'fake executable; never launched')
            output = root / 'evidence'
            output.mkdir()
            calls = []
            private_roots = []
            acl = 'original acl\n'
            original_write = Path.write_text

            def save_text(path, *args, **kwargs):
                if save_fails and path.name == 'original.acl':
                    raise RuntimeError('injected ACL backup failure')
                return original_write(path, *args, **kwargs)

            def fake_tool(argv, *, environment=None, timeout=30):
                calls.append(list(argv))
                if argv[0] == 'getfacl':
                    return acl
                if argv[0] == 'sudo':
                    if restore_fails and any(value.startswith('--restore=') for value in argv):
                        raise RuntimeError('injected restore failure')
                    return ''
                private_roots.append(Path(environment['XDG_CONFIG_HOME']).parent)
                if argv[1] == 'device':
                    raise RuntimeError('injected init failure')
                self.assertEqual(argv[1], 'kill')
                if kill_fails:
                    raise RuntimeError('injected kill failure')
                return json.dumps({'ok': True, 'results': [{'data': {}}]})

            try:
                with patch.object(Smoke, 'run_tool', side_effect=fake_tool), patch.object(Smoke.os, 'kill', side_effect=AssertionError('PID signaling is forbidden')), patch.object(Path, 'write_text', new=save_text):
                    with self.assertRaises(RuntimeError):
                        Smoke.smoke(binary, output, True)
                report = json.loads((output / 'receipt.json').read_text())
                self.assertEqual(report['status'], 'failed')
                if save_fails:
                    self.assertFalse(any(call[0] == 'sudo' for call in calls))
                else:
                    self.assertTrue(any(any(value.startswith('--restore=') for value in call) for call in calls))
                self.assertTrue(private_roots)
                if kill_fails or restore_fails:
                    retained = Path(report['retained_private_root'])
                    self.assertEqual(retained, private_roots[0])
                    self.assertTrue(retained.is_dir())
                    self.assertIn('cleanup_error', report)
                else:
                    if not save_fails:
                        self.assertTrue(report['vhost_acl_restored'])
                    self.assertFalse(private_roots[0].exists())
                return report
            finally:
                for private in set(private_roots):
                    if private.exists():
                        shutil.rmtree(private)

    def test_acl_backup_failure_does_not_attempt_a_restore(self):
        """An unsaved original ACL is never passed to the restore tool."""
        self.run_failure(save_fails=True)

    def test_initial_failure_restores_acl_and_removes_private_state(self):
        """A normal cleanup preserves the original exception and restores the ACL."""
        self.run_failure()

    def test_cli_cleanup_failure_retains_private_state_without_pid_signals(self):
        """A supervisor failure is recorded and retains all private recovery data."""
        report = self.run_failure(kill_fails=True)
        self.assertIn('injected kill failure', report['cleanup_error'])
        self.assertTrue(report['vhost_acl_restored'])

    def test_acl_restore_failure_retains_original_acl_and_private_state(self):
        """A failed restore cannot be misreported as a passing native test."""
        report = self.run_failure(restore_fails=True)
        self.assertIn('injected restore failure', report['cleanup_error'])
        self.assertNotIn('vhost_acl_restored', report)


if __name__ == '__main__':
    unittest.main()
