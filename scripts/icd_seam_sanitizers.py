#!/usr/bin/env python3
"""Verify genuine ASan accesses across the complete private owned ICD seam.

Read-only source/header/helper inputs; all freshly rebuilt outputs belong to the
exclusive CLI directory. Native and Debug allocator binaries include every owned
embedded module and separately linked codec. C oracles use ASan/LSan/UBSan;
Zig retains Debug safety and exact original-module guard/reverse proofs. No driver,
Windows, physical-GPU or DXVK acceptance is inferred from this Linux seam gate.
"""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import subprocess
import sys

sys.dont_write_bytecode = True
import icd_dependency_sanitizers as dependency
import icd_owned_sanitizers as owned

# Exact contracted separately linked codec set; never consume public objects.
CodecNames = ('capabilities', 'command', 'objects', 'instance_wire', 'query_wire', 'values')
# Exact named allocator-suite inventory; totals alone cannot admit missing units.
CodecDebugUnitCounts = {'capabilities': 3, 'command': 9, 'objects': 4,
                       'instance_wire': 5, 'query_wire': 3, 'values': 4}
assert set(CodecDebugUnitCounts) == set(CodecNames)
# Independent pinned C encoder/receiver oracle sources and include boundaries.
GuestOracles = ('render_wire', 'descriptor_wire', 'compute_wire', 'graphics_wire',
                'graphics_pipeline_wire', 'graphics_command_wire', 'features_query', 'device_wire')
GuestIncludes = ('-Iinclude', '-Itests/vgpu/encoder', '-Ibuild/venus_protocol',
                 '-Isubmodules/venus_protocol/tests', '-Isubmodules/venus_protocol/include')
RendererIncludes = ('-Itests/vgpu/encoder', '-Ibuild/venus_renderer_protocol', '-Iinclude',
                    '-Isubmodules/venus_protocol/include', '-Isubmodules/venus_protocol/include/vulkan')
# Bound tool-generated text before streaming; no unbounded retained subprocess output.
MaxProofBytes = 256 * 1024 * 1024
MaxRetainedArtifacts = 512
CompleteSourceNames = (*owned.IcdEmbeddedSourceNames, *(f'venus_{name}.zig' for name in CodecNames))
assert len(set(CompleteSourceNames)) == len(CompleteSourceNames) == 20
# Freeze receipt must confirm whether the C frontend also needs device_wire.
NativeOracleNames = ('features_query', 'features_reply', 'device_wire')


def merge_inputs(provenance, additions):
    """[in] Borrow manifests; [out] mutate caller-owned provenance, reject conflicts.

    All values are immutable hashes of read-only files; no descriptor retained.
    Synchronous, no shared ownership; caller protects its exclusive output directory.
    """
    for path, digest in additions.items():
        assert path not in provenance or provenance[path] == digest, 'input snapshot changed: ' + path
        provenance[path] = digest


def validate_report(report, mode, sources):
    """[in] Borrow fresh object report/source set; [out] none; reject incomplete proof.

    No input mutation or retained storage. Runtime helper exemption is checked by
    the unchanged closure helper; every module must carry its proven source/hash.
    Synchronous; exceptions fail the gate before linkage/execution.
    """
    assert report['emission_mode'] == mode and report['mode'] == 'Debug'
    assert report['entire_module_reverse_proof'] is True
    assert report['uncovered_owned_functions'] == [] and report['guard_stores_preserved'] > 0
    modules = report['modules']
    assert len(modules) == len(sources)
    assert {Path(module['source']) for module in modules} == set(sources)
    for module in modules:
        assert module['uncovered_owned_functions'] == []
        assert module['source_sha256'] == dependency.file_hash(Path(module['source']))
        assert module['asan_access_hook_relocations'] > 0
        assert module['instrumented_definitions'] == len(module['symbols']) > 0
        assert all(module['required_symbol_access_hooks'].values())
        if mode == 'tests':
            assert not module['non_reachable_runtime_declarations']
    assert report['instrumented_definitions'] == sum(len(module['symbols']) for module in modules)


def final_access_proof(binary, reports, output):
    """[in] Borrow final binary/fresh object reports; [out] own complete module proof.

    Every selected symbol must survive uniquely in nm and disassembly. Count only
    actual resolved ASan calls in owned function intervals, including mandatory
    helper hooks. Tool descriptors close deterministically; failure rejects proof.
    No source/object mutation; synchronous, caller serializes its artifact directory.
    """
    modules = [dict(module) for report in reports for module in report['modules']]
    expected_sources = {str((Path('src/vgpu') / name).resolve()) for name in CompleteSourceNames}
    assert {module['source'] for module in modules} == expected_sources
    assert len(modules) == len(expected_sources) == 20
    definitions = {}
    for module in modules:
        module['final_executable_access_hooks'] = 0
        for symbol in module['symbols']:
            assert symbol not in definitions, 'duplicate linked owned symbol: ' + symbol
            definitions[symbol] = module
    files = {}
    for kind, arguments in (('symbols', ['nm', '--defined-only', str(binary)]),
                            ('disassembly', ['objdump', '-d', str(binary)])):
        path = output / (binary.name + '_' + kind + '.txt')
        with path.open('wb') as stream:
            owned.run(arguments, stdout=stream)
        assert 0 < path.stat().st_size <= MaxProofBytes
        files[kind] = path
    symbol_counts = {symbol: 0 for symbol in definitions}
    with files['symbols'].open() as stream:
        for line in stream:
            match = re.match(r'^[0-9a-fA-F]+\s+([a-zA-Z])\s+(.+)$', line.rstrip())
            if match and match[2] in symbol_counts:
                assert match[1] in ('t', 'T'), 'owned symbol is not defined code: ' + match[2]
                symbol_counts[match[2]] += 1
    assert all(count == 1 for count in symbol_counts.values()), 'missing/duplicate final owned symbol'
    boundaries = {symbol: 0 for symbol in definitions}
    hooks = {symbol: 0 for symbol in definitions}
    symbol = ''
    with files['disassembly'].open() as stream:
        for line in stream:
            match = re.match(r'^[0-9a-fA-F]+ <(.+)>:', line)
            if match:
                symbol = match[1]
                if symbol in boundaries:
                    boundaries[symbol] += 1
            if symbol in definitions and re.search(
                    r'\bcallq?\s+[^<]*<__asan_(?:report_load\d+|report_store\d+|memcpy|memmove|memset)(?:@[^>]*)?>', line):
                definitions[symbol]['final_executable_access_hooks'] += 1
                hooks[symbol] += 1
    assert all(count == 1 for count in boundaries.values()), 'ambiguous final function interval'
    for module in modules:
        assert module['final_executable_access_hooks'] > 0, 'no final owned ASan hooks: ' + module['source']
        module['final_required_symbol_access_hooks'] = {
            symbol: hooks[symbol] for symbol in module['required_access_hook_symbols']}
        assert all(module['final_required_symbol_access_hooks'].values()), 'required final helper lacks ASan hooks'
        module['final_defined_symbols'] = len(module['symbols'])
    return modules


def execute_fixture(binary, output, environment):
    """[in] Borrow binary/checker environment; [out] own execution log and unit count.

    Child is synchronously reaped within180 seconds. No leaked process/descriptor;
    all checker failures/nonzero/timeouts propagate. Native fixture returns count0;
    Zig runner success count is measured from its unchanged completion message.
    """
    log = output / (binary.name + '_execution.log')
    with log.open('wb') as stream:
        owned.run([str(binary)], stdout=stream, stderr=stream, env=environment)
    assert log.stat().st_size <= MaxProofBytes
    text = log.read_text()
    assert not re.search(r'ERROR: (?:AddressSanitizer|LeakSanitizer)|runtime error:', text)
    matches = re.findall(r'^All (\d+) tests passed\.$', text, re.M)
    assert len(matches) <= 1
    count = int(matches[0]) if matches else 0
    print(f'{binary.name}: passed; Debug units={count}; log={log}', flush=True)
    return count


def object_manifest(paths):
    """[in] Borrow ordered private object paths; [out] own ordered immutable hashes.

    Duplicate paths/nonfiles fail. Synchronous read-only hashing closes streams;
    no source/object ownership transfer. Caller holds artifact directory exclusively.
    """
    assert len(set(paths)) == len(paths)
    return [{'path': str(path), 'sha256': dependency.file_hash(path)} for path in paths]


def publish_completion(output, latest_record, expected_units):
    """[in] Borrow exclusive run/latest path/count; [out] own immutable receipt.

    Require both complete same-binary proofs, exact current twenty-source identity
    and unit count. Hash every retained regular artifact and all borrowed inputs;
    no completion publication on failure. Per-run record is created exclusively.
    Optional caller-owned latest bytes are fsynced then atomically replaced; every
    temporary descriptor/file closes or unlinks deterministically on any error.
    """
    reports = {kind: json.loads((output / (kind + '_sanitized.json')).read_text())
               for kind in ('native', 'test')}
    native, tests = reports['native'], reports['test']
    source_manifest = {module['source']: module['source_sha256'] for module in native['modules']}
    expected_sources = {str((Path('src/vgpu') / name).resolve()) for name in CompleteSourceNames}
    assert set(source_manifest) == expected_sources and len(source_manifest) == 20
    assert {module['source']: module['source_sha256'] for module in tests['modules']} == source_manifest
    assert native['icd_native_suites'] == 1 and tests['icd_debug_units'] == expected_units > 0
    assert native['standalone_native_suites'] == tests['standalone_native_suites'] == 6
    assert native['standalone_debug_unit_counts'] == tests['standalone_debug_unit_counts'] == CodecDebugUnitCounts
    assert native['standalone_debug_units'] == tests['standalone_debug_units'] == sum(CodecDebugUnitCounts.values()) == 28
    assert native['input_manifest'] == tests['input_manifest']
    dependency.verify_inputs(source_manifest)
    dependency.verify_inputs(native['input_manifest'])
    report_references = {}
    for kind, report in reports.items():
        dependency.verify_inputs({entry['path']: entry['sha256'] for entry in report['ordered_link_manifest']})
        binary = output / (kind + '_runner')
        assert dependency.file_hash(binary) == report['executable_sha256']
        report_path = output / (kind + '_sanitized.json')
        report_references[kind] = {'path': str(report_path), 'sha256': dependency.file_hash(report_path)}
    retained = {}
    for path in sorted(output.rglob('*')):
        assert not path.is_symlink(), 'unexpected retained artifact symlink'
        if path.is_file():
            assert len(retained) < MaxRetainedArtifacts
            assert path.resolve().is_relative_to(output)
            retained[str(path.resolve())] = dependency.file_hash(path)
    dependency.verify_inputs(retained)
    completion = output / 'completion.json'
    record = {'schema': 'waddle_owned_icd_complete_seam_v1', 'complete': True,
              'output': str(output), 'completion_record': str(completion),
              'source_manifest': source_manifest, 'reports': report_references,
              'expected_icd_debug_units': expected_units, 'retained_artifacts': retained}
    content = (json.dumps(record, indent=2) + '\n').encode()
    with completion.open('xb') as stream:
        stream.write(content)
        stream.flush()
        os.fsync(stream.fileno())
    if latest_record is not None:
        latest_record = latest_record.resolve()
        assert latest_record != completion
        latest_record.parent.mkdir(parents=True, exist_ok=True)
        temporary = latest_record.with_name(latest_record.name + '.tmp-' + str(os.getpid()))
        owned_temporary = False
        try:
            with temporary.open('xb') as stream:
                owned_temporary = True
                stream.write(content)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, latest_record)
            owned_temporary = False
        finally:
            if owned_temporary:
                temporary.unlink(missing_ok=True)
    print('Complete immutable seam record: ' + str(completion), flush=True)
    return record


def main():
    """[in] CLI exclusive output directory; [out] native/test complete seam reports.

    Caller freezes all source/header inputs and serializes output. No source/shared
    output mutation; every descriptor/process closes after synchronous bounded work.
    Exceptions/nonzero propagate; success requires all actual suites and access proofs.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--latest-record', type=Path)
    parser.add_argument('--expected-icd-units', type=int, required=True)
    arguments = parser.parse_args()
    assert arguments.expected_icd_units > 0
    artifact_root = arguments.output.resolve()
    artifact_root.mkdir(parents=True, exist_ok=True)
    output = artifact_root / ('run-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ') + '-' + str(os.getpid()))
    output.mkdir()
    print('Exclusive complete seam artifacts: ' + str(output), flush=True)
    source = Path('src/vgpu/venus_icd.zig')
    frontend = Path('tests/vgpu/icd.c')
    inventories = owned.icd_source_inventory(source)
    assert len(inventories) == 14 and set(dependency.ModuleConfigs) == set(CodecNames)
    provenance = {str(path): dependency.file_hash(path) for path in inventories}
    merge_inputs(provenance, {str(Path(__file__).resolve()): dependency.file_hash(Path(__file__).resolve()),
                             str(Path(dependency.__file__).resolve()): dependency.file_hash(Path(dependency.__file__).resolve()),
                             str(Path(owned.__file__).resolve()): dependency.file_hash(Path(owned.__file__).resolve())})
    standalone_reports, codec_objects, standalone_units = [], [], 0
    standalone_counts = {}
    for name in CodecNames:
        folder = output / 'dependencies' / name
        folder.mkdir(parents=True, exist_ok=True)
        with (folder / 'gate.log').open('wb') as stream:
            owned.run([sys.executable, '-B', str(Path(dependency.__file__).resolve()), name, str(folder)],
                      stdout=stream, stderr=stream)
        merge_inputs(provenance, json.loads((folder / 'inputs.json').read_text()))
        dependency.verify_inputs(provenance)
        reports = {kind: json.loads((folder / (kind + '_sanitized.json')).read_text())
                   for kind in ('native', 'test')}
        for kind, report in reports.items():
            validate_report(report, 'native' if kind == 'native' else 'tests',
                            {Path('src/vgpu/venus_' + name + '.zig').resolve()})
        observed_units = re.findall(r'^All (\d+) tests passed\.$', (folder / 'gate.log').read_text(), re.M)
        assert observed_units == [str(CodecDebugUnitCounts[name])], 'named codec execution count differs from freeze: ' + name
        assert reports['native']['unit_tests'] == 0
        assert reports['test']['unit_tests'] == CodecDebugUnitCounts[name]
        standalone_counts[name] = reports['test']['unit_tests']
        standalone_units += reports['test']['unit_tests']
        standalone_reports.append(reports['native'])
        codec_objects.append(folder / 'native_sanitized.o')
        print(f'{name}: fresh native and {reports["test"]["unit_tests"]} Debug units passed', flush=True)
    warnings = ['-Wall', '-Wextra', '-Wpedantic', '-Werror']
    safety = ['-fsanitize=address,leak,undefined', '-fno-omit-frame-pointer']
    c_flags = ['-std=c11', '-D_GNU_SOURCE', '-O1', '-g', *warnings, *safety]
    zig_flags = ['-Iinclude', '-Isubmodules/venus_protocol/include', '-O', 'Debug', '-lc']
    oracle_objects = {}
    for name in (*GuestOracles, 'features_reply'):
        includes = RendererIncludes if name == 'features_reply' else GuestIncludes
        oracle_source = Path('tests/vgpu') / (name + '_oracle.c')
        folder = output / 'oracles' / name
        folder.mkdir(parents=True, exist_ok=True)
        merge_inputs(provenance, dependency.input_provenance(source, oracle_source, includes, folder))
        oracle_object = folder / 'oracle.o'
        owned.run(['cc', *c_flags, *includes, '-c', str(oracle_source), '-o', str(oracle_object)])
        assert name not in oracle_objects
        oracle_objects[name] = oracle_object
    merge_inputs(provenance, dependency.input_provenance(source, frontend, GuestIncludes, output))
    (output / 'inputs.json').write_text(json.dumps(provenance, indent=2) + '\n')
    environment = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1:halt_on_error=1',
                       LSAN_OPTIONS='exitcode=23', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    runtime_paths = set()
    for name in ('libasan.so', 'libubsan.so'):
        runtime_path = Path(subprocess.check_output(['cc', '-print-file-name=' + name], text=True, timeout=180).strip())
        assert runtime_path.is_file(), 'missing sanitizer runtime: ' + name
        runtime_paths.add(str(runtime_path.parent))
    runtime = ['-L' + path for path in sorted(runtime_paths)] + ['-lasan', '-lubsan']
    native_frontend = output / 'native_frontend.o'
    test_frontend = output / 'native_oracle.o'
    owned.run(['cc', *c_flags, *GuestIncludes, '-c', str(frontend), '-o', str(native_frontend)])
    owned.run(['cc', *c_flags, *GuestIncludes, '-Dmain=venus_icd_native_fixture', '-c', str(frontend), '-o', str(test_frontend)])
    values_oracle = output / 'dependencies' / 'values' / 'oracle.o'
    assert set(oracle_objects) == {*GuestOracles, 'features_reply'}
    assert len(oracle_objects) == 9 and len(set(oracle_objects.values())) == 9
    native_dependencies = [*codec_objects, values_oracle, *(oracle_objects[name] for name in NativeOracleNames)]
    test_dependencies = [test_frontend, *codec_objects, values_oracle, *oracle_objects.values()]
    immutable_objects = object_manifest([native_frontend, *test_dependencies])
    object_inputs = {entry['path']: entry['sha256'] for entry in immutable_objects}
    reports = {}
    for kind in ('native', 'test'):
        dependency.verify_inputs(provenance)
        dependency.verify_inputs(object_inputs)
        original = output / (kind + '_original.ll')
        normalized = output / (kind + '_sanitized.ll')
        sanitized_object = output / (kind + '_sanitized.o')
        if kind == 'native':
            owned.run(['zig', 'build-obj', str(source), *zig_flags, '-fPIC', '-fcompiler-rt',
                       '-femit-llvm-ir=' + str(original), '-femit-bin=' + str(output / 'native_original.o')])
        else:
            owned.run(['zig', 'test', str(source), *zig_flags, *map(str, test_dependencies), *runtime,
                       '--test-no-exec', '-femit-llvm-ir=' + str(original), '-femit-bin=' + str(output / 'test_original')])
        report = owned.instrument(source, original, normalized, inventories, mode='native' if kind == 'native' else 'tests')
        owned.run(['clang-19', '-Wno-override-module', '-fPIC', '-fsanitize=address', '-g', '-c',
                   str(normalized), '-o', str(sanitized_object)])
        owned.verify_access_hooks(sanitized_object, report, normalized)
        validate_report(report, 'native' if kind == 'native' else 'tests', set(inventories))
        binary = output / (kind + '_runner')
        linked_objects = ([native_frontend, sanitized_object, *native_dependencies] if kind == 'native'
                          else [sanitized_object, *test_dependencies])
        manifest = object_manifest(linked_objects)
        current_objects = {entry['path']: entry['sha256'] for entry in manifest}
        dependency.verify_inputs(provenance)
        dependency.verify_inputs(object_inputs)
        dependency.verify_inputs(current_objects)
        if kind == 'native':
            owned.run(['cc', *c_flags, *map(str, linked_objects), '-pthread', '-o', str(binary)])
        else:
            owned.run(['zig', 'cc', *map(str, linked_objects), *runtime, '-pthread', '-ldl', '-lm', '-o', str(binary)])
        modules = final_access_proof(binary, [report, *standalone_reports], output)
        binary_hash = dependency.file_hash(binary)
        unit_count = execute_fixture(binary, output, environment)
        assert kind != 'test' or unit_count == arguments.expected_icd_units, 'Debug unit completion count differs from freeze'
        dependency.verify_inputs(provenance)
        dependency.verify_inputs(object_inputs)
        dependency.verify_inputs(current_objects)
        assert binary_hash == dependency.file_hash(binary), 'binary changed during execution'
        report['modules'] = modules
        report['linked_owned_source_functions'] = sum(module['owned_source_functions'] for module in modules)
        report['linked_instrumented_definitions'] = sum(module['instrumented_definitions'] for module in modules)
        report['linked_guard_stores_preserved'] = report['guard_stores_preserved'] + sum(r['guard_stores_preserved'] for r in standalone_reports)
        report['excluded_object_scope'] = 'std/compiler runtime only; all separately linked owned Zig codec objects instrumented'
        report['ubsan_scope'] = 'all fresh C fixtures/oracles; owned Zig Debug runtime safety retained'
        report['oracle_scope'] = 'fresh private pinned static-inline C encoders/receivers; no Vulkan driver linkage'
        report['ordered_link_manifest'] = manifest
        report['input_manifest'] = provenance
        report['executable_sha256'] = binary_hash
        report['standalone_native_suites'] = len(CodecNames)
        report['standalone_debug_units'] = standalone_units
        report['standalone_debug_unit_counts'] = standalone_counts
        report['icd_native_suites'] = 1 if kind == 'native' else 0
        report['icd_debug_units'] = unit_count
        normalized.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
        reports[kind] = {key: value for key, value in report.items()
                         if key not in ('symbols', 'modules', 'input_manifest', 'ordered_link_manifest')}
        reports[kind]['modules'] = [{key: value for key, value in module.items()
                                    if key not in ('symbols', 'runtime_functions')}
                                   for module in modules]
    publish_completion(output, arguments.latest_record, arguments.expected_icd_units)
    print(json.dumps(reports, indent=2), flush=True)


if __name__ == '__main__':
    main()
