"""Cross-compile real native probe variants and reject unsafe audit settings.

No Windows, loader TLS, heap-retirement, or physical GPU acceptance is implied.
Each invocation retains its exact compiler output and source/binary hashes.
"""
import datetime
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

Root = Path(__file__).resolve().parents[2]
Output = Root / 'build/qualification_runtime_probe'
Output.mkdir(parents=True, exist_ok=True)
Folder = Path(tempfile.mkdtemp(prefix=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S_'), dir=Output))
Compiler = shutil.which('x86_64-w64-mingw32-gcc-posix')
Source = Root / 'tests/vgpu/clvk_runtime_windows.c'


class probe_build_tests_t(unittest.TestCase):
    """Compile actual fixture boundaries, preserving every diagnostic."""

    def compile_variant(self, name, flags, expected_success):
        self.assertIsNotNone(Compiler, 'native probe cross compiler is required')
        argv = [Compiler, '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                '-O2', *flags, str(Source), '-o', str(Folder / (name + '.exe'))]
        result = subprocess.run(argv, cwd=Root, capture_output=True, text=True, timeout=60)
        (Folder / (name + '.log')).write_text(result.stdout + result.stderr)
        (Folder / (name + '.json')).write_text(json.dumps({'argv': argv, 'exit': result.returncode}, indent=2) + '\n')
        if expected_success:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('static assertion failed', result.stderr)
            self.assertFalse((Folder / (name + '.exe')).exists())

    def test_idle_matrix(self):
        for idle in (0, 4, 16):
            for audit in (False, True):
                with self.subTest(idle=idle, audit=audit):
                    flags = [f'-DWaddleRuntimeIdleThreadCount={idle}', '-DWaddleRuntimeLifetimeCount=3']
                    if audit:
                        flags.append('-DWaddleRuntimeHeapAudit=1')
                    name = f'idle_{idle}_audit_{int(audit)}'
                    self.compile_variant(name, flags, True)
                    image = (Folder / (name + '.exe')).read_bytes()
                    audit_environment = 'WADDLE_DXVK_HEAP_AUDIT'.encode('utf-16le')
                    self.assertEqual(audit_environment in image, audit)

    def test_idle_capacity_rejected(self):
        for idle in (-1, 17):
            with self.subTest(idle=idle):
                self.compile_variant('invalid_idle_' + str(idle), [f'-DWaddleRuntimeIdleThreadCount={idle}'], False)

    def test_audit_capture_bound_rejected(self):
        self.compile_variant('invalid_audit_cycles', ['-DWaddleRuntimeHeapAudit=1', '-DWaddleRuntimeLifetimeCount=9'], False)

    @classmethod
    def tearDownClass(cls):
        paths = [Source, Root / 'tests/vgpu/dxvk_heap_audit_windows.inc', Path(__file__).resolve(), *Folder.glob('*.exe')]
        receipt = {'scope': 'cross-build only; native acceptance pending',
                   'sha256': {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in paths}}
        (Folder / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
        print('Retained probe build evidence:', Folder)


if __name__ == '__main__':
    unittest.main()
