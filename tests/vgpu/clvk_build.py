"""Exercise OpenCL build admission without building LLVM or changing submodules.

All temporary files and child processes belong to this serial test runner. Fake
git responses test shell control flow only; the real pinned export comparison is
also executed. These tests provide no Windows or GPU acceptance evidence.
"""
import dataclasses
import importlib.util
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

Root = Path(__file__).resolve().parents[2]
Spec = importlib.util.spec_from_file_location('clvk_build', Root / 'scripts/build_clvk_windows.py')
Driver = importlib.util.module_from_spec(Spec)
Spec.loader.exec_module(Driver)

BufferApiDefinitions = (
    'bool is_valid_command_queue(cl_command_queue queue)',
    'bool is_valid_mem_object(cl_mem mem)',
    'bool is_valid_buffer(cl_mem mem)',
    'bool is_valid_event(cl_event event)',
    'bool is_valid_event_wait_list(cl_uint num_events_in_wait_list,',
    'bool is_same_context(cl_command_queue queue, cl_mem mem)',
    'bool is_same_context(cl_command_queue queue, cl_uint num_events,',
    'bool map_flags_are_valid(cl_map_flags flags)',
    'void* cvk_enqueue_map_buffer(cvk_command_queue* cq, cvk_buffer* buffer,',
    'cl_int CLVK_API_CALL clEnqueueReadBuffer(',
    'cl_int CLVK_API_CALL clEnqueueWriteBuffer(',
    'cl_int CLVK_API_CALL clEnqueueCopyBuffer(',
    'void* CLVK_API_CALL clEnqueueMapBuffer(',
)


def extract_cpp_definition(source, signature):
    """Return one complete C++ definition beginning at its unique signature."""
    if source.count(signature) != 1:
        raise AssertionError('expected one definition for %r' % signature)
    start = source.index(signature)
    depth = 0
    opening_seen = False
    state = 'code'
    escaped = False
    index = start
    while index < len(source):
        character = source[index]
        following = source[index + 1] if index + 1 < len(source) else ''
        if state == 'line_comment':
            if character == '\n':
                state = 'code'
        elif state == 'block_comment':
            if character == '*' and following == '/':
                state = 'code'
                index += 1
        elif state in ('string', 'character'):
            delimiter = '"' if state == 'string' else "'"
            if escaped:
                escaped = False
            elif character == '\\':
                escaped = True
            elif character == delimiter:
                state = 'code'
        elif character == '/' and following == '/':
            state = 'line_comment'
            index += 1
        elif character == '/' and following == '*':
            state = 'block_comment'
            index += 1
        elif character == '"':
            state = 'string'
        elif character == "'":
            state = 'character'
        elif character == '{':
            opening_seen = True
            depth += 1
        elif character == '}':
            depth -= 1
            if opening_seen and depth == 0:
                return source[start:index + 1]
        index += 1
    raise AssertionError('unterminated C++ definition for %r' % signature)


def extract_buffer_api_header(api_source):
    """Extract exact production helpers and the four 1D API bodies under test."""
    definitions = [extract_cpp_definition(api_source, signature)
                   for signature in BufferApiDefinitions]
    return '\n\n\n'.join(definitions) + '\n'


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
            'Number in:', ' Export Address Table %08x' % len(exports),
            ' [Name Pointer/Ordinal] Table %08x' % len(exports),
            'Export Address Table -- Ordinal Base 1',
            '[ 0] +base[ 1] abcdef Export RVA', '',
            '[Ordinal/Name Pointer] Table -- Ordinal Base 1'] + [
            '[ %d] +base[ %d] abcdef %s' % (index, index, name)
            for index, name in enumerate(exports)])

    def test_actual_pe_name_table_and_export_named_export(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'exports.c'
            definition = Path(directory) / 'exports.def'
            dll = Path(directory) / 'exports.dll'
            for extra in ((), ('Export',)):
                exports = (*Driver.RequiredExports, *extra)
                source.write_text('\n'.join('void %s(void) {}' % name for name in exports))
                definition.write_text('EXPORTS\n' + '\n'.join('    ' + name for name in exports) + '\n')
                subprocess.run(['x86_64-w64-mingw32-gcc-posix', '-shared', str(source),
                                str(definition), '-o', str(dll)], check=True, timeout=60)
                if extra:
                    with self.assertRaisesRegex(SystemExit, 'Export'):
                        Driver.inspect_dll(dll, Driver.RequiredExports)
                else:
                    _, names = Driver.inspect_dll(dll, Driver.RequiredExports)
                    self.assertEqual(names, sorted(Driver.RequiredExports))

    def test_missing_table_and_unnamed_export_fail_closed(self):
        good = self.pe_dump(Driver.RequiredExports, [])
        for bad in (good.replace('[Ordinal/Name Pointer] Table', 'Wrong table'),
                    good.replace('Export Address Table %08x' % len(Driver.RequiredExports),
                                 'Export Address Table %08x' % (len(Driver.RequiredExports) + 1))):
            with self.subTest(dump=bad), patch.object(Driver, 'capture', return_value=bad):
                with self.assertRaisesRegex(SystemExit, 'PE export'):
                    Driver.inspect_dll(Path('fixture.dll'), Driver.RequiredExports)

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


class runtime_driver_tests_t(unittest.TestCase):
    """Exercise source/tool binding, cache admission, and receipt publication."""

    def test_capture_only_writes_identity_and_skips_build(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'toolchain.json'
            identity = {'target': 'captured-only'}
            provenance_path = Path(directory) / 'provenance.json'
            provenance_path.write_text('{}')
            with patch.object(Driver, 'capture_toolchain_identity', return_value=identity), \
                 patch.object(Driver, 'verify_package_provenance', return_value={'verified': True}), \
                 patch.object(Driver, 'build_native', side_effect=AssertionError('build started')):
                result = Driver.main(['--cc', '/compiler/c', '--cxx', '/compiler/cxx',
                                      '--dlltool', '/tool/dlltool', '--objdump', '/tool/objdump',
                                      '--nm', '/tool/nm', '--windres', '/tool/windres',
                                      '--ar', '/tool/ar', '--ranlib', '/tool/ranlib',
                                      '--toolchain-provenance', str(provenance_path),
                                      '--capture-toolchain', str(output)])
            self.assertEqual(result, 0)
            self.assertEqual(json.loads(output.read_text()), identity)
            self.assertFalse(output.with_name(output.name + '.tmp').exists())

    def test_build_requires_reviewed_identity_before_preflight(self):
        arguments = ['--cc', '/compiler/c', '--cxx', '/compiler/cxx',
                     '--dlltool', '/tool/dlltool', '--objdump', '/tool/objdump',
                     '--nm', '/tool/nm', '--windres', '/tool/windres',
                     '--ar', '/tool/ar', '--ranlib', '/tool/ranlib',
                     '--toolchain-provenance', '/fixture/provenance.json']
        with patch.object(Driver, '_preflight', side_effect=AssertionError('preflight started')):
            with self.assertRaisesRegex(SystemExit, '--reviewed-toolchain'):
                Driver.main(arguments)

    def test_real_auditor_load_registers_dataclass_module(self):
        auditor = Driver.load_auditor(Root)
        self.assertIs(sys.modules.get(auditor.__name__), auditor)
        self.assertTrue(dataclasses.is_dataclass(auditor.section_t))

    def test_real_reviewed_preflight_stops_before_any_build(self):
        private = Root / 'build/qualification_20261017/mingw_win32_private_1402/extracted/usr/bin'
        evidence = Root / 'build/qualification_feature_20261021/root_tls'
        args = type('Args', (), {
            'cc': str(private / 'x86_64-w64-mingw32-gcc-win32'),
            'cxx': str(private / 'x86_64-w64-mingw32-g++-win32'),
            'dlltool': '/usr/bin/x86_64-w64-mingw32-dlltool',
            'objdump': '/usr/bin/x86_64-w64-mingw32-objdump',
            'nm': '/usr/bin/x86_64-w64-mingw32-nm',
            'windres': '/usr/bin/x86_64-w64-mingw32-windres',
            'ar': str(private / 'x86_64-w64-mingw32-gcc-ar-win32'),
            'ranlib': str(private / 'x86_64-w64-mingw32-gcc-ranlib-win32'),
            'reviewed_toolchain': str(evidence / 'reviewed_toolchain.json'),
            'toolchain_provenance': str(evidence / 'toolchain_provenance.json'),
            'runtime_policy': str(evidence / 'production_runtime_policy.json'),
            'probe_runtime_policy': str(evidence / 'probe_runtime_policy.json')})()
        selected = Driver._preflight(Root, args)
        self.assertEqual(selected['runtime_policy'],
                         json.loads(Path(args.runtime_policy).read_text()))
        self.assertEqual(selected['probe_runtime_policy'],
                         json.loads(Path(args.probe_runtime_policy).read_text()))
        self.assertEqual(selected['inputs']['toolchain_identity']['identity'],
                         json.loads(Path(args.reviewed_toolchain).read_text()))
        self.assertIn('toolchain_provenance', selected['inputs'])
        self.assertIn('host_cc', selected['inputs']['build_tools'])

    def test_package_provenance_rejects_archive_hash_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / 'package.deb'
            archive.write_bytes(b'not a Debian archive')
            receipt = root / 'receipt.json'
            receipt.write_text(json.dumps([{'package': 'reviewed-package',
                                            'version': '1.0', 'sha256': '0' * 64,
                                            'matched_apt_metadata': True,
                                            'host_installed': False}]))
            provenance = root / 'provenance.json'
            provenance.write_text(json.dumps({'schema': 1,
                                              'package_receipt_path': str(receipt),
                                              'extracted_root': str(root),
                                              'packages': [{'name': 'reviewed-package',
                                                            'version': '1.0',
                                                            'deb_path': str(archive),
                                                            'sha256': '0' * 64}]}))
            with self.assertRaisesRegex(SystemExit, 'does not match receipt/archive'):
                Driver.verify_package_provenance({}, provenance)

    def test_preflight_uses_explicit_compiler_selectors(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Root
            paths = {}
            for key in ('cc', 'cxx', 'dlltool', 'objdump', 'nm', 'windres', 'ar', 'ranlib'):
                path = Path(directory) / key
                path.write_text('fixture')
                path.chmod(0o700)
                paths[key] = str(path)
            manifest = Path(directory) / 'reviewed.json'
            manifest.write_text('{"reviewed": true}')
            policy_path = Path(directory) / 'policy.json'
            policy = {'allowed_api_callers': {'TlsAlloc': ['key_create'],
                                               'FlsAlloc': [], 'DuplicateHandle': []},
                      'tls_pairs': {'key_create': 'key_delete'},
                      'dynamic_lookup_callers': []}
            policy_path.write_text(json.dumps(policy))
            probe_policy_path = Path(directory) / 'probe-policy.json'
            probe_policy = {'allowed_api_callers': {'TlsAlloc': [], 'FlsAlloc': ['probe_fls'],
                                                     'DuplicateHandle': []},
                            'tls_pairs': {}, 'dynamic_lookup_callers': ['probe_lookup']}
            probe_policy_path.write_text(json.dumps(probe_policy))
            args = type('Args', (), {**paths, 'reviewed_toolchain': str(manifest),
                                     'runtime_policy': str(policy_path),
                                     'probe_runtime_policy': str(probe_policy_path),
                                     'toolchain_provenance': str(Path(directory) / 'provenance.json')})()
            Path(args.toolchain_provenance).write_text('{}')
            expected_capture = {'target': 'x86_64-w64-mingw32',
                                'cc': {'thread_model': 'win32', 'dumpmachine': 'x86_64-w64-mingw32',
                                       'dumpfullversion': '14-win32'},
                                'cxx': {'thread_model': 'win32', 'dumpmachine': 'x86_64-w64-mingw32',
                                        'dumpfullversion': '14-win32'}}
            package_provenance = {'verified': True}
            expected_capture['package_provenance'] = package_provenance

            class fake_auditor_t:
                def verify_toolchain_identity(self, actual, expected):
                    if actual != expected_capture or expected != {'reviewed': True}:
                        raise AssertionError('identity arguments were not source-bound')
                    return {'identity': actual, 'reviewed_identity': expected}

            with patch.object(Driver, 'capture_toolchain_identity', return_value=expected_capture) as capture_call, \
                 patch.object(Driver, 'verify_package_provenance', return_value=package_provenance), \
                 patch.object(Driver, 'load_auditor', return_value=fake_auditor_t()), \
                 patch.object(Driver, '_tool_record', return_value={'fixture': True}), \
                 patch.object(Driver, '_required_tool', side_effect=lambda name: Path(directory) / name):
                for name in ('cmake', 'ninja', 'cc', 'c++'):
                    (Path(directory) / name).write_text('fixture')
                selected = Driver._preflight(root, args)
            self.assertEqual(Path(selected['cc']), Path(paths['cc']).resolve())
            self.assertEqual(Path(selected['cxx']), Path(paths['cxx']).resolve())
            self.assertEqual(selected['runtime_policy'], policy)
            self.assertEqual(selected['probe_runtime_policy'], probe_policy)
            capture_call.assert_called_once_with(Path(paths['cc']).resolve(),
                                                 Path(paths['cxx']).resolve())

    def test_toolchain_file_uses_only_selected_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'toolchain.cmake'
            values = [Path(directory) / name for name in
                      ('gcc-win32', 'gxx-win32', 'windres-win32', 'ar-win32',
                       'ranlib-win32', 'sysroot')]
            Driver.write_toolchain(path, *values)
            text = path.read_text()
            for value in values[:5]:
                self.assertIn(str(value), text)
            self.assertIn('CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER', text)
            self.assertNotIn('-posix', text)
            self.assertNotIn('winpthread', text.lower())

    def test_runtime_objects_precede_runtime_archives_in_link_flags(self):
        text = Driver.linker_flags(('runtime/clvk_runtime_tls.o',
                                    'runtime/clvk_runtime_eh_pool.o'),
                                   'scripts/clvk/OpenCL.def', 'cross/OpenCL.map')
        self.assertLess(text.index('clvk_runtime_tls.o'), text.index('clvk_runtime_eh_pool.o'))
        self.assertLess(text.index('clvk_runtime_eh_pool.o'), text.index('OpenCL.def'))
        self.assertLess(text.index('OpenCL.def'), text.index('-static-libgcc'))
        self.assertIn('-mthreads', text)
        self.assertNotIn('winpthread', text.lower())
        for flag in ('--gc-sections', '--undefined=waddle_clvk_runtime_tls_limits',
                     '-T,' + str(Root / 'scripts/clvk/runtime_sections.ld'),
                     '--wrap=__gthr_win32_equal',
                     '--wrap=_ZSt16__at_thread_exitPSt20__at_thread_exit_elt',
                     '--wrap=_ZSt25notify_all_at_thread_exitRSt18condition_variableSt11unique_lockISt5mutexE'):
            self.assertIn(flag, text)

    def test_runtime_objects_compile_as_c_with_win32_model_and_limits(self):
        with tempfile.TemporaryDirectory() as directory:
            calls = []
            with patch.object(Driver, 'run', side_effect=lambda command, **kwargs: calls.append(command)):
                objects = Driver.build_runtime_objects(Root, '/private/gcc-win32', Path(directory))
            self.assertEqual([item.name for item in objects],
                             ['clvk_runtime_tls.o', 'clvk_runtime_eh_pool.o'])
            self.assertEqual(len(calls), 2)
            for command, source in zip(calls, Driver.ShimSources):
                self.assertIn('-std=c11', command)
                self.assertIn('-mthreads', command)
                self.assertIn('-Wpedantic', command)
                self.assertIn('-DWaddleRuntimeTlsArenaBytes=192', command)
                self.assertIn('-DWaddleRuntimeTlsSlotCapacity=32', command)
                self.assertEqual(command[command.index('-c') + 1], Root / source)

    def test_cached_runtime_is_reaudited_against_exact_linked_files(self):
        with tempfile.TemporaryDirectory() as directory:
            linked = Path(directory) / 'libOpenCL.dll'
            link_map = Path(directory) / 'OpenCL.map'
            published = Path(directory) / 'OpenCL-copy.dll'
            linked.write_bytes(b'linked')
            link_map.write_bytes(b'map')
            shutil.copyfile(linked, published)
            record = {'linked_dll': {'path': str(linked), 'sha256': Driver.digest(linked)},
                      'link_map': {'path': str(link_map), 'sha256': Driver.digest(link_map),
                                   'working_directory': directory},
                      'dll': {'path': str(published), 'sha256': Driver.digest(published)}}
            policy = {'reviewed': True}
            with patch.object(Driver, 'build_admission', return_value={'fresh': True}) as admit:
                self.assertEqual(Driver._admit_recorded_runtime(Root, record, nm='/private/nm',
                                                                runtime_policy=policy),
                                 {'fresh': True})
            admit.assert_called_once_with(Root, linked, link_map, nm='/private/nm',
                                          map_working_directory=directory, runtime_policy=policy)
            link_map.write_bytes(b'tampered map')
            with self.assertRaisesRegex(SystemExit, 'link map hash'):
                Driver._admit_recorded_runtime(Root, record, nm='/private/nm', runtime_policy=policy)

    def test_failed_admission_preserves_previous_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            receipt = Path(directory) / 'receipt.json'
            receipt.write_text('{"previous": true}\n')
            linked = Path(directory) / 'libOpenCL.dll'
            link_map = Path(directory) / 'OpenCL.map'
            published = Path(directory) / 'OpenCL-copy.dll'
            for path in (linked, link_map, published):
                path.write_bytes(path.name.encode())
            shutil.copyfile(linked, published)
            record = {'linked_dll': {'path': str(linked), 'sha256': Driver.digest(linked)},
                      'link_map': {'path': str(link_map), 'sha256': Driver.digest(link_map),
                                   'working_directory': directory},
                      'dll': {'path': str(published), 'sha256': Driver.digest(published)}}
            inputs = {'source_hashes': Driver.source_identity(Root),
                      'export_list_sha256': Driver.digest(Root / 'scripts/clvk/OpenCL.def'),
                      'shutdown_abi_sha256': Driver.digest(Root / 'include/waddle/clvk.h'),
                      'private_patches': {name: Driver.digest(Root / name)
                                          for name in Driver.PrivatePatches}}
            before = receipt.read_bytes()
            with patch.object(Driver, '_verify_inputs_unchanged'), \
                 patch.object(Driver, 'build_admission', side_effect=SystemExit('rejected image')):
                with self.assertRaisesRegex(SystemExit, 'rejected image'):
                    Driver.publish_admitted_record(Root, receipt, record, inputs, nm='/private/nm',
                                                    runtime_policy={'reviewed': True})
            self.assertEqual(receipt.read_bytes(), before)

    def test_probe_preflight_reports_rejection_and_never_calls_build_gate(self):
        class fake_auditor_t:
            def __init__(self):
                self.link_args = None

            def verify_runtime_link(self, *args, **kwargs):
                self.link_args = (args, kwargs)
                return {'link_checked': True}

            def audit_runtime_tls(self, dll_path, **kwargs):
                raise ValueError('forbidden __at_thread_exit symbol')

        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            selected = {'cc': '/private/gcc-win32', 'cxx': '/private/gxx-win32',
                        'nm': '/private/nm', 'runtime_policy': {'production': True},
                        'probe_runtime_policy': {'probe': 'separate policy'}}
            commands = []

            def fake_run(command, **kwargs):
                commands.append(command)
                output = Path(command[command.index('-o') + 1])
                output.write_bytes(b'compiled probe fixture')
                for argument in command:
                    if str(argument).startswith('-Wl,-Map='):
                        map_name = str(argument).split('=', 1)[1].split(',', 1)[0]
                        Path(map_name).write_bytes(b'probe map fixture')

            auditor = fake_auditor_t()
            with patch.object(Driver, 'build_runtime_objects',
                              return_value=(folder / 'tls.o', folder / 'eh.o')), \
                 patch.object(Driver, 'run', side_effect=fake_run), \
                 patch.object(Driver, 'load_auditor', return_value=auditor), \
                 patch.object(Driver, 'build_native', side_effect=AssertionError('LLVM build started')):
                with self.assertRaisesRegex(SystemExit, 'strict Win32 runtime probe admission rejected'):
                    Driver.preflight_runtime_probe(Root, selected, folder)
            report = json.loads((folder / 'probe_preflight/probe_preflight.json').read_text())
            self.assertEqual(report['status'], 'rejected')
            self.assertFalse(report['native_acceptance'])
            self.assertIn('__at_thread_exit', report['rejection'])
            self.assertEqual(auditor.link_args[1]['runtime_policy'], selected['probe_runtime_policy'])
            self.assertEqual(auditor.link_args[1]['expected_output_name'], 'runtime_v2_probe.dll')
            dll_command = next(command for command in commands if str(command[0]).endswith('gxx-win32'))
            for flag in ('-Wpedantic', '-Wl,--gc-sections',
                         '-Wl,--undefined=waddle_clvk_runtime_tls_limits',
                         '-Wl,-T,' + str(Root / 'scripts/clvk/runtime_sections.ld'),
                         '-Wl,--wrap=__gthr_win32_equal',
                         '-Wl,--wrap=_ZSt16__at_thread_exitPSt20__at_thread_exit_elt',
                         '-Wl,--wrap=_ZNSt13__future_base13_State_baseV211_Make_ready6_M_setEv',
                         '-Wl,--wrap=_ZSt25notify_all_at_thread_exitRSt18condition_variableSt11unique_lockISt5mutexE'):
                self.assertIn(flag, dll_command)

    def test_native_llvm_key_excludes_mingw_but_binds_host_tools(self):
        build_tools = {name: {'sha256': name + '-hash'}
                       for name in ('host_cc', 'host_cxx', 'cmake', 'ninja', 'cc', 'cxx', 'ar')}
        inputs = {'build_tools': build_tools, 'toolchain_identity': {'mingw': 'first'},
                  'pins': {'llvm': 'pin'}, 'settings': {'switches': 'first'}}
        first = Driver._native_cache_key(inputs)
        inputs['toolchain_identity'] = {'mingw': 'changed'}
        inputs['build_tools']['cc'] = {'sha256': 'different-cross-tool'}
        inputs['settings'] = {'switches': 'changed'}
        self.assertEqual(Driver._native_cache_key(inputs), first)
        inputs['build_tools']['host_cc'] = {'sha256': 'different-host-compiler'}
        self.assertNotEqual(Driver._native_cache_key(inputs), first)
        inputs['build_tools']['host_cc'] = build_tools['host_cc']
        changed_pin = tuple((name, 'different-llvm-pin' if name == 'submodules/llvm_project' else value)
                            for name, value in Driver.Pins)
        with patch.object(Driver, 'Pins', changed_pin):
            self.assertNotEqual(Driver._native_cache_key(inputs), first)
        with patch.object(Driver, 'LlvmSwitches', (*Driver.LlvmSwitches, '-DTEST_SWITCH=ON')):
            self.assertNotEqual(Driver._native_cache_key(inputs), first)

    def test_source_identity_binds_linker_fences_and_negative_controls(self):
        identity = Driver.source_identity(Root)
        self.assertIn('scripts/clvk/runtime_sections.ld', identity)
        self.assertIn('tests/vgpu/clvk_runtime_link_test.py', identity)

    def test_default_and_custom_receipt_paths(self):
        self.assertEqual(Driver._receipt_path(Root, 'build/clvk_client'),
                         Root / 'build/clvk_client.json')
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(Driver._receipt_path(Path(directory), 'custom-build'),
                             Path(directory) / 'custom-build/clvk_client.json')


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

    def test_actual_buffer_api_bodies_sanitized_and_original_negative_control(self):
        with tempfile.TemporaryDirectory() as directory:
            source = self.source_copy(directory)
            Driver.apply_private_patches(Root, source)
            original_api = (Root / 'submodules/clvk/src/api.cpp').read_text()
            patched_api = (source / 'src/api.cpp').read_text()
            original_header = extract_buffer_api_header(original_api)
            patched_header = extract_buffer_api_header(patched_api)
            map_helper_signature = 'void* cvk_enqueue_map_buffer(cvk_command_queue* cq, cvk_buffer* buffer,'
            patched_map_helper = extract_cpp_definition(patched_api, map_helper_signature)
            self.assertEqual(patched_map_helper.count('delete cmd;'), 1,
                             'patched map build failure must delete its command')
            self.assertNotEqual(hashlib.sha256(original_api.encode()).digest(),
                                hashlib.sha256(patched_api.encode()).digest())
            self.assertNotEqual(hashlib.sha256(original_header.encode()).digest(),
                                hashlib.sha256(patched_header.encode()).digest())

            opencl_headers = Root / 'submodules/clvk/external/OpenCL-Headers'
            binary = Path(directory) / 'buffer_api_test'
            compile_flags = [
                'g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wconversion',
                '-Wshadow', '-fsanitize=address,leak,undefined',
                '-fno-omit-frame-pointer', '-DCL_TARGET_OPENCL_VERSION=300',
            ]
            test_source = str(Root / 'tests/vgpu/clvk_buffer_api.cpp')
            environment = dict(os.environ,
                               ASAN_OPTIONS='detect_leaks=1:abort_on_error=1:halt_on_error=1',
                               LSAN_OPTIONS='exitcode=23:report_objects=1',
                               UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')

            patched_include = Path(directory) / 'patched'
            patched_include.mkdir()
            (patched_include / 'clvk_buffer_api_under_test.hpp').write_text(patched_header)
            patched_command = [*compile_flags, '-I', str(patched_include), '-I',
                               str(opencl_headers), test_source, '-o', str(binary)]
            subprocess.run(patched_command, check=True, timeout=120)
            patched_result = subprocess.run([str(binary)], capture_output=True, text=True,
                                            env=environment, timeout=30)
            self.assertEqual(patched_result.returncode, 0, patched_result.stderr)
            self.assertIn('clvk extracted buffer API tests passed', patched_result.stdout)

            # Removing only the production cleanup must fail this same test at
            # the build-error ownership assertion, before any counter reset.
            unclean_map_helper = patched_map_helper.replace('        delete cmd;\n', '', 1)
            self.assertNotEqual(unclean_map_helper, patched_map_helper)
            unclean_header = patched_header.replace(patched_map_helper,
                                                     unclean_map_helper, 1)
            self.assertNotEqual(unclean_header, patched_header)
            unclean_include = Path(directory) / 'unclean'
            unclean_include.mkdir()
            (unclean_include / 'clvk_buffer_api_under_test.hpp').write_text(unclean_header)
            unclean_command = [*compile_flags, '-I', str(unclean_include), '-I',
                               str(opencl_headers), test_source, '-o', str(binary)]
            subprocess.run(unclean_command, check=True, timeout=120)
            unclean_result = subprocess.run([str(binary)], capture_output=True,
                                             text=True, env=environment, timeout=30)
            self.assertNotEqual(unclean_result.returncode, 0,
                                'map helper without failure cleanup unexpectedly passed')
            self.assertRegex(
                unclean_result.stderr,
                r'check failed at .*clvk_buffer_api\.cpp:\d+: map_command_allocations == 1 && map_command_destructions == 1',
            )

            original_include = Path(directory) / 'original'
            original_include.mkdir()
            (original_include / 'clvk_buffer_api_under_test.hpp').write_text(original_header)
            original_command = [*compile_flags, '-I', str(original_include), '-I',
                                str(opencl_headers), test_source, '-o', str(binary)]
            subprocess.run(original_command, check=True, timeout=120)
            original_result = subprocess.run([str(binary)], capture_output=True, text=True,
                                             env=environment, timeout=30)
            self.assertNotEqual(original_result.returncode, 0,
                                'unpatched original API unexpectedly passed the regression')
            self.assertRegex(
                original_result.stderr,
                r'check failed at .*clvk_buffer_api\.cpp:\d+: clEnqueueReadBuffer.*CL_INVALID_VALUE',
            )


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
