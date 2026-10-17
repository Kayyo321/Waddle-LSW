"""Exercise OpenCL build admission without building LLVM or changing submodules.

All temporary files and child processes belong to this serial test runner. Fake
git responses test shell control flow only; the real pinned export comparison is
also executed. These tests provide no Windows or GPU acceptance evidence.
"""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

Root = Path(__file__).resolve().parents[2]
Spec = importlib.util.spec_from_file_location('clvk_build', Root / 'scripts/build_clvk_windows.py')
Driver = importlib.util.module_from_spec(Spec)
Spec.loader.exec_module(Driver)


class export_tests_t(unittest.TestCase):
    """Verify source/export parity and PE runtime admission failures."""

    def test_pinned_api(self):
        exports = Driver.read_export_list(Root / 'scripts/clvk/OpenCL.def')
        self.assertEqual(len(exports), len(set(exports)))
        self.assertTrue(set(Driver.RequiredExports) <= set(exports))
        Driver.check_export_list_against_api(Root, exports)

    def test_each_missing_export(self):
        exports = Driver.read_export_list(Root / 'scripts/clvk/OpenCL.def')
        for name in exports:
            if name.startswith('clspv'):
                continue
            with self.subTest(name=name), self.assertRaisesRegex(SystemExit, 'missing='):
                Driver.check_export_list_against_api(Root, [item for item in exports if item != name])

    def test_unknown_export(self):
        exports = Driver.read_export_list(Root / 'scripts/clvk/OpenCL.def')
        with self.assertRaisesRegex(SystemExit, 'cl_nonexistent'):
            Driver.check_export_list_against_api(Root, exports + ['cl_nonexistent'])

    def pe_dump(self, exports, imports):
        return '\n'.join(['DLL Name: ' + name for name in imports] + [
            '[ %d] +base[ %d] abcdef %s' % (index, index, name)
            for index, name in enumerate(exports)])

    def test_system_imports(self):
        imports = ['vulkan-1.dll', 'KERNEL32.dll', 'msvcrt.dll']
        with patch.object(Driver, 'capture', return_value=self.pe_dump(Driver.RequiredExports, imports)):
            self.assertEqual(Driver.inspect_dll(Path('fixture.dll'), Driver.RequiredExports),
                             (imports, sorted(Driver.RequiredExports)))

    def test_missing_pe_exports(self):
        for name in Driver.RequiredExports:
            exports = [item for item in Driver.RequiredExports if item != name]
            with self.subTest(name=name), patch.object(Driver, 'capture', return_value=self.pe_dump(exports, [])):
                with self.assertRaisesRegex(SystemExit, name):
                    Driver.inspect_dll(Path('fixture.dll'), Driver.RequiredExports)

    def test_dynamic_runtime_imports(self):
        for name in ('libgcc_s_seh-1.dll', 'LIBSTDC++-6.DLL', 'libwinpthread-1.dll'):
            with self.subTest(name=name), patch.object(Driver, 'capture', return_value=self.pe_dump(Driver.RequiredExports, [name])):
                with self.assertRaisesRegex(SystemExit, 'unbundled'):
                    Driver.inspect_dll(Path('fixture.dll'), Driver.RequiredExports)

    def test_unknown_pe_export(self):
        exports = (*Driver.RequiredExports, 'unexpected_private_export')
        with patch.object(Driver, 'capture', return_value=self.pe_dump(exports, [])):
            with self.assertRaisesRegex(SystemExit, 'unexpected_private_export'):
                Driver.inspect_dll(Path('fixture.dll'), Driver.RequiredExports)

    def test_tool_failure(self):
        with patch.object(Driver, 'capture', side_effect=subprocess.CalledProcessError(7, 'objdump')):
            with self.assertRaises(subprocess.CalledProcessError):
                Driver.inspect_dll(Path('fixture.dll'), Driver.RequiredExports)


class private_patch_tests_t(unittest.TestCase):
    """Check exact patch preimages and compile actual patched owner accounting."""

    def source_copy(self, directory):
        source = Path(directory) / 'source'
        source.mkdir()
        shutil.copytree(Root / 'submodules/clvk/src', source / 'src')
        return source

    def test_patch_preimage_and_no_submodule_mutation(self):
        with tempfile.TemporaryDirectory() as directory:
            source = self.source_copy(directory)
            before = Driver.digest(Root / 'submodules/clvk/src/init.cpp')
            Driver.apply_private_patches(Root, source)
            self.assertEqual(before, Driver.digest(Root / 'submodules/clvk/src/init.cpp'))
            self.assertTrue((source / 'src/waddle_lifetime.hpp').is_file())
            with self.assertRaises(subprocess.CalledProcessError):
                Driver.apply_private_patches(Root, source)
        with self.assertRaisesRegex(SystemExit, 'submodule'):
            Driver.apply_private_patches(Root, Root / 'submodules/clvk')

    def test_patch_mismatch_has_no_partial_mutation(self):
        with tempfile.TemporaryDirectory() as directory:
            source = self.source_copy(directory)
            (source / 'src/init.cpp').write_text('preimage mismatch\n')
            before = Driver.digest(source / 'src/objects.hpp')
            with self.assertRaises(subprocess.CalledProcessError):
                Driver.apply_private_patches(Root, source)
            self.assertEqual(before, Driver.digest(source / 'src/objects.hpp'))
            self.assertFalse((source / 'src/waddle_lifetime.hpp').exists())

    def test_actual_pool_retirement_interleaving(self):
        with tempfile.TemporaryDirectory() as directory:
            source = self.source_copy(directory)
            Driver.apply_private_patches(Root, source)
            text = (source / 'src/queue.hpp').read_text()
            begin = text.index('struct cvk_executor_thread_pool {')
            end = text.index('struct cvk_command_buffer {', begin)
            pool = text[begin:end]
            self.assertIn('bool waddle_quiescent()', pool)
            (Path(directory) / 'clvk_pool_under_test.hpp').write_text(pool)
            binary = Path(directory) / 'pool'
            command = ['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-O1',
                       '-fsanitize=address,leak,undefined', '-fno-omit-frame-pointer', '-pthread',
                       '-I', directory, str(Root / 'tests/vgpu/clvk_pool_lifetime.cpp'), '-o', str(binary)]
            subprocess.run(command, check=True, timeout=120)
            subprocess.run([str(binary)], check=True, timeout=30,
                           env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1:halt_on_error=1'))

    def test_actual_owner_accounting_sanitized(self):
        with tempfile.TemporaryDirectory() as directory:
            source = self.source_copy(directory)
            Driver.apply_private_patches(Root, source)
            binary = Path(directory) / 'owners'
            command = ['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-O1',
                       '-fsanitize=address,leak,undefined', '-fno-omit-frame-pointer', '-pthread']
            for include in (source / 'src', Root / 'submodules/clvk/external/OpenCL-Headers',
                            Root / 'submodules/clvk/external/SPIRV-Headers/include',
                            Root / 'submodules/dxvk/include/vulkan/include'):
                command += ['-isystem', str(include)]
            command += [str(Root / 'tests/vgpu/clvk_lifetime.cpp'), '-o', str(binary)]
            subprocess.run(command, check=True, timeout=120)
            subprocess.run([str(binary)], check=True, timeout=30,
                           env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1:halt_on_error=1'))


class verifier_tests_t(unittest.TestCase):
    """Inject each dependency failure into a private PATH; never mutate git trees."""

    def run_verifier(self, directory, mode, selected=0, missing_tool='', extra_env=None):
        directory = Path(directory)
        binary = directory / 'bin'
        binary.mkdir(exist_ok=True)
        log = directory / 'calls'
        log.write_text('')
        pins = '\n'.join('%s %s' % item for item in Driver.Pins)
        (directory / 'pins').write_text(pins + '\n')
        # The fake executes only POSIX shell builtins, so missing tools cannot be
        # accidentally satisfied by inherited system PATH entries.
        git = binary / 'git'
        git.write_text('''#!/bin/sh
set -eu
printf '%s %s\\n' "$2" "$3" >> "$CALL_LOG"
index=0
while read -r dependency_path dependency_sha; do
 if [ "$2" = "$dependency_path" ]; then
  if [ "$3" = status ]; then
   if [ "$MODE" = real_status ]; then
    shift; shift; exec "$REAL_GIT" -C "$REAL_TREE" "$@"
   fi
   if [ "$index" = "$SELECTED" ]; then
    case "$MODE" in
     untracked) printf '%s\\n' '?? injected.c';;
     ignored) printf '%s\\n' '!! ignored.c';;
     status_error) exit 128;;
    esac
   fi
   exit 0
  fi
  if [ "$3" = rev-parse ]; then
   if [ "$index" = "$SELECTED" ]; then
    case "$MODE" in
     missing) exit 128;;
     mismatch) printf '%s\\n' deadbeef; exit 0;;
    esac
   fi
   printf '%s\\n' "$dependency_sha"; exit 0
  fi
  if [ "$index" = "$SELECTED" ] && [ "$MODE" = dirty ]; then exit 1; fi
  exit 0
 fi
 index=$((index+1))
done < "$PIN_FILE"
exit 129
''')
        git.chmod(0o700)
        for name in ('cmake', 'ninja', 'python3', 'x86_64-w64-mingw32-gcc', 'x86_64-w64-mingw32-g++'):
            tool = binary / name
            if tool.exists():
                tool.unlink()
            if name != missing_tool:
                tool.write_text('#!/bin/sh\nexit 0\n')
                tool.chmod(0o700)
        environment = dict(os.environ, PATH=str(binary), CALL_LOG=str(log),
                           PIN_FILE=str(directory / 'pins'), MODE=mode, SELECTED=str(selected))
        environment.update(extra_env or {})
        result = subprocess.run(['/bin/sh', str(Root / 'scripts/verify_clvk.sh')],
                                cwd=directory, env=environment, capture_output=True, text=True, timeout=10)
        return result, log.read_text().splitlines()

    def test_all_pins_verified(self):
        with tempfile.TemporaryDirectory() as directory:
            result, calls = self.run_verifier(directory, 'clean')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(calls, [path + ' ' + operation for path, _ in Driver.Pins
                                     for operation in ('rev-parse', 'diff', 'status')])

    def test_each_dependency_failure_stops(self):
        with tempfile.TemporaryDirectory() as directory:
            for selected, (path, _) in enumerate(Driver.Pins):
                for mode in ('missing', 'mismatch', 'dirty', 'untracked', 'ignored', 'status_error'):
                    with self.subTest(path=path, mode=mode):
                        result, calls = self.run_verifier(directory, mode, selected)
                        self.assertNotEqual(result.returncode, 0)
                        operations = 2 if mode == 'dirty' else 1 if mode in ('missing', 'mismatch') else 3
                        self.assertEqual(len(calls), selected * 3 + operations)
                        if mode == 'mismatch':
                            self.assertIn(path, result.stderr)

    def test_each_missing_tool_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            for name in ('cmake', 'ninja', 'python3', 'x86_64-w64-mingw32-gcc', 'x86_64-w64-mingw32-g++'):
                with self.subTest(name=name):
                    result, calls = self.run_verifier(directory, 'clean', missing_tool=name)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(len(calls), len(Driver.Pins) * 3)

    def test_real_git_rejects_untracked_and_ignored_source(self):
        real_git = shutil.which('git')
        self.assertIsNotNone(real_git)
        with tempfile.TemporaryDirectory() as directory:
            tree = Path(directory) / 'source'
            tree.mkdir()
            for arguments in (['init', '-q'], ['config', 'user.email', 'fixture@example.invalid'],
                              ['config', 'user.name', 'OpenCL Fixture']):
                subprocess.run([real_git, *arguments], cwd=tree, check=True, capture_output=True)
            (tree / '.gitignore').write_text('ignored.c\n')
            subprocess.run([real_git, 'add', '.gitignore'], cwd=tree, check=True)
            subprocess.run([real_git, 'commit', '-qm', 'Pin isolated fixture'], cwd=tree, check=True)
            environment = {'REAL_GIT': real_git, 'REAL_TREE': str(tree)}
            result, _ = self.run_verifier(directory, 'real_status', extra_env=environment)
            self.assertEqual(result.returncode, 0, result.stderr)
            for name in ('untracked.c', 'ignored.c'):
                with self.subTest(name=name):
                    injected = tree / name
                    injected.write_text('/* must not enter a pinned build */\n')
                    result, calls = self.run_verifier(directory, 'real_status', extra_env=environment)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(len(calls), 3)
                    self.assertIn('source tree is not clean', result.stderr)
                    injected.unlink()


if __name__ == '__main__':
    unittest.main(verbosity=2)
