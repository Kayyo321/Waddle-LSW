"""Offline exact-owner and inverse-patch tests; no real lease, VM or GPU writes."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
sys.dont_write_bytecode = True
Here = Path(__file__).resolve().parent
Root = Path('/home/dev/Waddle-LSW')
Base = Root / 'build/qualification_feature_20261021'
Frozen = Base / 'opencl_reload_runner_r10/frozen'
sys.path.insert(0, str(Here))
sys.path.insert(0, str(Frozen))
import lease_owner_wrapper as w
ExecutorSha256 = 'b571dbbe6909ba986ae0b0a16206e0c5b95f88e54524d8561d44387e2279f2e4'
FrozenOwner = 'Codex independent M2 review (sole VM/GPU runner)'


def fresh_executor(name):
    """Borrow the preserved, hash-pinned executor; return an isolated module."""
    path = Frozen / 'opencl_reload_execute.py'
    if w.digest(path) != ExecutorSha256:
        raise RuntimeError('preserved executor differs from frozen original')
    return w.load_runner(path, name)


class wrapper_tests_t(unittest.TestCase):
    def setUp(self):
        self.ex = w.wrap_validator(fresh_executor('lease_r2_test_executor'))
        self.tmp = tempfile.TemporaryDirectory()
        self.proc = Path(self.tmp.name)
        self.identity = {'qemu': {'pid': 101, 'start_ticks': '5001'},
                         'swtpm': {'pid': 102, 'start_ticks': '5002'}}
        for name, item in self.identity.items():
            self.write_stat(item['pid'], name, 'S', item['start_ticks'])

    def tearDown(self):
        self.tmp.cleanup()

    def write_stat(self, pid, name, state, start):
        directory = self.proc / str(pid)
        directory.mkdir(exist_ok=True)
        fields = [state] + ['0'] * 18 + [start, '0', '0']
        (directory / 'stat').write_text(f'{pid} ({name}) ' + ' '.join(fields) + '\n')

    def lease(self, **changes):
        value = {'owner': w.ContinuationOwner, 'previous_owner': w.PreviousOwner,
                 'runner_state': 'idle; test', 'identity': json.loads(json.dumps(self.identity))}
        value.update(changes)
        return value

    def check(self, lease, identity=None):
        return self.ex.validate_vm_lease(lease, proc_root=self.proc,
                                       expected_identity=self.identity if identity is None else identity)

    def test_accepts_exact_codex_owner_and_claude_predecessor_without_mutation(self):
        lease = self.lease()
        before = json.dumps(lease, sort_keys=True)
        self.assertIs(self.check(lease), lease)
        self.assertEqual(json.dumps(lease, sort_keys=True), before)

    def test_rejects_other_owners(self):
        for owner in (FrozenOwner, w.PreviousOwner, 'someone else', w.ContinuationOwner + ' ', None):
            with self.subTest(owner=owner), self.assertRaises(RuntimeError):
                self.check(self.lease(owner=owner))

    def test_rejects_missing_or_wrong_predecessor(self):
        for previous in (None, FrozenOwner, 'other', w.ContinuationOwner):
            with self.subTest(previous=previous), self.assertRaises(RuntimeError):
                self.check(self.lease(previous_owner=previous))
        lease = self.lease()
        del lease['previous_owner']
        with self.assertRaises(RuntimeError):
            self.check(lease)

    def test_original_idle_state_checks_preserved(self):
        for state in ('active run', 'idle; running', 'idle; uncertain owner', 'busy', 'idle;', None):
            with self.subTest(state=state), self.assertRaises((RuntimeError, ValueError)):
                self.check(self.lease(runner_state=state))

    def test_original_identity_checks_preserved(self):
        other = json.loads(json.dumps(self.identity))
        other['qemu']['start_ticks'] = '9'
        with self.assertRaises((RuntimeError, ValueError)):
            self.check(self.lease(), identity=other)
        for state, start in (('S', '5999'), ('Z', '5001'), ('X', '5001'), ('x', '5001')):
            self.write_stat(101, 'qemu', state, start)
            with self.assertRaises((RuntimeError, ValueError)):
                self.check(self.lease())
        self.write_stat(101, 'qemu', 'S', '5001')
        for identity in ({}, {'qemu': self.identity['qemu']}, {**self.identity, 'extra': {}}):
            with self.assertRaises((RuntimeError, ValueError)):
                self.check(self.lease(identity=identity), identity=identity)

    def test_original_missing_process_checks_preserved(self):
        (self.proc / '101/stat').unlink()
        with self.assertRaises(OSError):
            self.check(self.lease())

    def test_wrap_is_idempotent_and_frozen_constant_unchanged(self):
        before = self.ex.validate_vm_lease
        self.assertIs(w.wrap_validator(self.ex).validate_vm_lease, before)
        self.assertEqual(self.ex.admission.TargetOwner, FrozenOwner)

    def test_install_wraps_only_executor_loads(self):
        calls = []
        def load(name, path):
            calls.append(path)
            if Path(path).resolve() == w.ExecutorPath.resolve():
                return fresh_executor(name)
            return types.SimpleNamespace(validate_vm_lease='untouched')
        runner = w.install_on_runner(types.SimpleNamespace(load=load))
        self.assertTrue(runner.load('lease_r2_a', w.ExecutorPath).validate_vm_lease.__lease_owner_wrapper__)
        self.assertEqual(runner.load('lease_r2_b', Path('/nonexistent/x.py')).validate_vm_lease, 'untouched')
        self.assertEqual(len(calls), 2)

    def test_original_validator_must_accept_the_mapped_lease(self):
        executor = types.SimpleNamespace(admission=types.SimpleNamespace(TargetOwner=FrozenOwner),
                                         validate_vm_lease=lambda *args, **kwargs: {})
        wrapped = w.wrap_validator(executor)
        with self.assertRaisesRegex(RuntimeError, 'original lease validator'):
            wrapped.validate_vm_lease(self.lease(), proc_root=self.proc, expected_identity=self.identity)

    def test_non_dict_lease_rejected(self):
        for lease in (None, [], 'invalid'):
            with self.assertRaises(RuntimeError):
                self.check(lease)

    def test_inverse_patch_is_exact_r1(self):
        original = Base / 'lease_owner_wrapper_r1/lease_owner_wrapper.py'
        self.assertEqual(w.digest(original), '94a367b84a93dcc7442f4ef082f7470cfae49ab5b85181583cdbb4589463bd8d')
        with tempfile.TemporaryDirectory() as directory:
            copy = Path(directory) / 'wrapper.py'
            shutil.copyfile(Here / 'lease_owner_wrapper.py', copy)
            subprocess.run(['patch', '-R', '-s', '--no-backup-if-mismatch', str(copy),
                            str(Here / 'lease_owner_wrapper_r2.patch')], check=True)
            self.assertEqual(copy.read_bytes(), original.read_bytes())


if __name__ == '__main__':
    unittest.main()
