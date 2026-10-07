#!/usr/bin/env python3
"""Genuine native/allocator ASan gates for separately compiled owned Zig codecs.

Read-only sources, headers and generated pinned protocol declarations. Each caller
exclusively owns its output directory; no public object, source or closure artifact
is replaced. C frontends/oracles receive ASan/LSan/UBSan; Zig retains Debug safety
plus actual ASan access instrumentation with complete original-module reverse proof.
No host Vulkan/renderer linkage, suppression, fault injection or bytecode artifacts.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

sys.dont_write_bytecode = True
import icd_owned_sanitizers as owned

# Immutable nonsystem dependency/output bounds; no retained subprocess buffers.
MaxHeaderDependencies = 256
MaxDependencyBytes = 1048576
HashChunkBytes = 65536
# Explicit native module registrations; each registration has its own verified commit.
ModuleConfigs = {
    'capabilities': {
        'boundary': 'fn fixture_value() venus_capabilities_t {',
        'includes': ['-Iinclude'],
        'oracle_macro': None,
    },
    'command': {
        'boundary': 'const fixture_t = struct {',
        'includes': ['-Iinclude'],
        'oracle_macro': None,
    },
    'objects': {
        'boundary': None,
        'includes': ['-Iinclude', '-Isubmodules/venus_protocol/include'],
        'oracle_macro': None,
    },
    'instance_wire': {
        'boundary': None,
        'includes': ['-Itests/vgpu/encoder', '-Ibuild/venus_protocol',
                     '-Isubmodules/venus_protocol/tests', '-Iinclude',
                     '-Isubmodules/venus_protocol/include'],
        'oracle_macro': 'VgpuInstanceOracle',
    },
    'query_wire': {
        'boundary': None,
        'includes': ['-Itests/vgpu/encoder', '-Ibuild/venus_protocol',
                     '-Isubmodules/venus_protocol/tests', '-Iinclude',
                     '-Isubmodules/venus_protocol/include'],
        'oracle_macro': 'VgpuQueryOracle',
    },
    'values': {
        'boundary': None,
        'includes': ['-Itests/vgpu/encoder', '-Ibuild/venus_renderer_protocol',
                     '-Iinclude', '-Isubmodules/venus_protocol/include',
                     '-Isubmodules/venus_protocol/include/vulkan'],
        'oracle_macro': 'VgpuValuesOracle',
    },
}


def file_hash(path):
    """[in] Borrow readable path; [out] return SHA256, no retained stream/storage.

    Synchronously owns/closes one read-only descriptor; exceptions fail the gate.
    Thread-safe for immutable inputs; caller protects files against concurrent edits.
    """
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        while chunk := stream.read(HashChunkBytes):
            digest.update(chunk)
    return digest.hexdigest()


def dependency_inventory(source, boundary):
    """[in] Borrow immutable source/boundary; [out] own complete runtime inventory.

    Runtime std is excluded; any new source import fails closed. Exact legacy
    boundaries or unique marker separate fixture ownership without source mutation.
    No allocation/resource survives the returned ordinary Python values; synchronous.
    """
    source = source.resolve()
    code = source.read_text()
    imports = re.findall(r'@import\("([^"\n]+)"\)', code)
    assert len(imports) == code.count('@import(') and set(imports) == {'std'}, 'new import needs dependency contract'
    if boundary:
        assert code.count(boundary) == 1 and '// Test-only fixtures.' not in code
        production = code.split(boundary, 1)[0]
    else:
        assert code.count('// Test-only fixtures.') == 1
        production = code.split('// Test-only fixtures.', 1)[0]
    functions = {}
    for line, text in enumerate(production.splitlines(), 1):
        match = re.match(r'\s*(?:pub |export )?fn (\w+)\(', text)
        if match:
            assert match[1] not in functions, 'duplicate runtime declaration'
            functions[match[1]] = line
    assert functions
    test_lines = {line for line, text in enumerate(code.splitlines(), 1)
                  if re.match(r'\s*test "', text)}
    assert test_lines, 'unit suite missing'
    return {source: {'functions': functions, 'excluded': {}, 'test_lines': test_lines,
                     'source_sha256': file_hash(source),
                     'fixture_boundary': len(production.splitlines()) + 1}}


def input_provenance(source, frontend, includes, output):
    """[in] Borrow source/frontend/includes; [out] own bounded hash manifest/artifact.

    Read-only preprocessor discovers all nonsystem headers; errors fail closed.
    Its output descriptor is owned/closed here. Every canonical input must remain
    inside the workspace; no generator/shared output mutation or ownership transfer.
    """
    root = Path.cwd().resolve()
    dependencies = output / 'frontend_dependencies.d'
    with dependencies.open('wb') as stream:
        owned.run(['cc', '-MM', '-MT', 'owned_fixture', *includes, str(frontend)], stdout=stream)
    assert dependencies.stat().st_size <= MaxDependencyBytes
    content = dependencies.read_text().replace('\\\n', ' ')
    assert content.startswith('owned_fixture:')
    paths = {Path(name).resolve() for name in shlex.split(content.split(':', 1)[1])}
    paths.add(source.resolve())
    assert 0 < len(paths) <= MaxHeaderDependencies
    for path in paths:
        assert path.is_relative_to(root) and path.is_file(), 'unexpected dependency: ' + str(path)
    return {str(path): file_hash(path) for path in sorted(paths)}


def verify_inputs(provenance):
    """[in] Borrow hash manifest; [out] none; reject any changed/missing input.

    Synchronous read-only file hashes close each descriptor deterministically.
    Raises on mismatch; caller serializes the source/header snapshot for the gate.
    """
    for name, digest in provenance.items():
        assert file_hash(Path(name)) == digest, 'input changed during gate: ' + name


def main():
    """[in] CLI registered module/exclusive output; [out] reports and test artifacts.

    Returns None after native and all Debug units pass; nonzero/180s timeout
    propagates. Source/header storage stays borrowed; all child processes are reaped.
    Caller exclusively owns output and immutable input snapshot, no shared mutation.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('module', choices=sorted(ModuleConfigs))
    parser.add_argument('output', type=Path)
    arguments = parser.parse_args()
    config = ModuleConfigs[arguments.module]
    source = Path('src/vgpu') / ('venus_' + arguments.module + '.zig')
    frontend = Path('tests/vgpu') / (arguments.module + '.c')
    output = arguments.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    inventories = dependency_inventory(source, config['boundary'])
    provenance = input_provenance(source, frontend, config['includes'], output)
    (output / 'inputs.json').write_text(json.dumps(provenance, indent=2) + '\n')
    warnings = ['-Wall', '-Wextra', '-Wpedantic', '-Werror']
    safety = ['-fsanitize=address,leak,undefined', '-fno-omit-frame-pointer']
    c_flags = ['-std=c11', '-D_GNU_SOURCE', '-O1', '-g', *warnings, *safety, *config['includes']]
    zig_flags = ['-Iinclude', '-Isubmodules/venus_protocol/include', '-lc', '-O', 'Debug']
    runtime_paths = set()
    for name in ('libasan.so', 'libubsan.so'):
        runtime_path = Path(subprocess.check_output(['cc', '-print-file-name=' + name], text=True, timeout=180).strip())
        assert runtime_path.is_file(), 'missing sanitizer runtime: ' + name
        runtime_paths.add(str(runtime_path.parent))
    runtime = ['-L' + path for path in sorted(runtime_paths)] + ['-lasan', '-lubsan']
    environment = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1:halt_on_error=1',
                       LSAN_OPTIONS='exitcode=23', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    reports = {}
    oracle = []
    if config['oracle_macro']:
        oracle = [str(output / 'oracle.o')]
        owned.run(['cc', *c_flags, '-D' + config['oracle_macro'], '-c', str(frontend), '-o', oracle[0]])
    for kind in ('native', 'test'):
        original_ir = output / (kind + '_original.ll')
        sanitized_ir = output / (kind + '_sanitized.ll')
        sanitized_object = output / (kind + '_sanitized.o')
        if kind == 'native':
            owned.run(['zig', 'build-obj', str(source), *zig_flags, '-fPIC', '-fcompiler-rt',
                       '-femit-llvm-ir=' + str(original_ir), '-femit-bin=' + str(output / 'native_original.o')])
        else:
            owned.run(['zig', 'test', str(source), *zig_flags, *oracle, *runtime, '--test-no-exec',
                       '-femit-llvm-ir=' + str(original_ir), '-femit-bin=' + str(output / 'test_original')])
        report = owned.instrument(source, original_ir, sanitized_ir, inventories,
                                  mode='native' if kind == 'native' else 'tests')
        owned.run(['clang-19', '-Wno-override-module', '-fPIC', '-fsanitize=address', '-g', '-c',
                   str(sanitized_ir), '-o', str(sanitized_object)])
        owned.verify_access_hooks(sanitized_object, report, sanitized_ir)
        symbols = subprocess.check_output(['nm', '-u', str(sanitized_object)], text=True, timeout=180)
        assert '__asan_report_load' in symbols and '__asan_report_store' in symbols
        runner = output / (kind + '_runner')
        if kind == 'native':
            owned.run(['cc', *c_flags, str(frontend), str(sanitized_object), '-o', str(runner)])
        else:
            owned.run(['zig', 'cc', str(sanitized_object), *oracle, *runtime,
                       '-pthread', '-ldl', '-lm', '-o', str(runner)])
        owned.run([str(runner)], env=environment)
        verify_inputs(provenance)
        report['excluded_object_scope'] = 'no separately linked owned production objects; std/compiler runtime excluded'
        report['oracle_scope'] = 'fresh private C ASan/LSan/UBSan oracle' if oracle else 'none'
        report['unit_tests'] = len(next(iter(inventories.values()))['test_lines']) if kind == 'test' else 0
        sanitized_ir.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
        reports[kind] = {key: value for key, value in report.items() if key not in ('symbols', 'modules')}
        reports[kind]['modules'] = [{key: value for key, value in module.items()
                                   if key not in ('symbols', 'runtime_functions')}
                                  for module in report['modules']]
    print(json.dumps(reports, indent=2), flush=True)


if __name__ == '__main__':
    main()
