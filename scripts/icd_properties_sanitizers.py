#!/usr/bin/env python3
"""Genuine three-source Properties2 native-OS unit access gate.

Read-only production sources and pinned generated headers. Every attempt owns a
unique private directory; failures retain partial artifacts without a completion
record. Exactly three fresh independent C oracles receive ASan/LSan/UBSan. Owned
Zig native/wire/render definitions retain Debug checks and full original-module
reverse proof, plus actual final executable ASan accesses. No production-object
mode, public ICD import, support advertisement, GPU or Windows acceptance claim.
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

# Exact imported production closure and named actual Debug suite inventory.
SourceNames = ('venus_properties_native.zig', 'venus_properties_wire.zig', 'venus_render_wire.zig')
UnitCounts = {'venus_properties_native.zig': 5, 'venus_properties_wire.zig': 7, 'venus_render_wire.zig': 17}
OracleNames = ('properties_query', 'properties_reply', 'render_wire')
OracleSymbols = (
    'venus_properties_test_query', 'venus_properties_test_fixture', 'venus_properties_test_encode',
    'venus_render_test_image', 'venus_render_test_view', 'venus_render_test_barrier',
    'venus_render_test_shader', 'venus_render_test_descriptor_layout',
    'venus_render_test_pipeline_layout', 'venus_render_test_compute',
)
GuestIncludes = ('-Iinclude', '-Itests/vgpu/encoder', '-Ibuild/venus_protocol',
                 '-Isubmodules/venus_protocol/tests', '-Isubmodules/venus_protocol/include')
RendererIncludes = ('-Iinclude', '-Itests/vgpu/encoder', '-Ibuild/venus_renderer_protocol',
                    '-Isubmodules/venus_protocol/include', '-Isubmodules/venus_protocol/include/vulkan')
MaxProofBytes = 256 * 1024 * 1024
MaxRetainedArtifacts = 128


def merge_inputs(provenance, additions):
    """[in] Borrow immutable manifests; [in,out] exclusive caller-owned dictionary.

    Reject conflicting read-only snapshots. No retained stream/caller storage;
    synchronous, no shared ownership or source mutation.
    """
    for path, digest in additions.items():
        assert path not in provenance or provenance[path] == digest, 'input changed: ' + path
        provenance[path] = digest


def checked_tool(arguments, log, environment=None):
    """[in] Borrow bounded command/environment; [out] own exclusive diagnostic file.

    Reap child within180 seconds, close stream on every path, propagate all errors.
    Reject empty/oversized output only when the caller requires content separately.
    No retained descriptor or shared mutation; caller serializes private directory.
    """
    with log.open('wb') as stream:
        owned.run(arguments, stdout=stream, stderr=stream, env=environment)
    assert log.stat().st_size <= MaxProofBytes


def final_access_proof(binary, report, output):
    """[in] Borrow fresh object report/final binary; [out] own per-module proof.

    Require each owned definition once in nm and disassembly, with genuine resolved
    ASan calls in every module. Independent oracle public entry points appear once
    and no unrelated oracle may appear. No source/binary mutation, retained child
    or descriptor; bounded synchronous tools, private output owner only.
    """
    modules = [dict(module) for module in report['modules']]
    assert {Path(module['source']).name for module in modules} == set(SourceNames)
    assert len(modules) == len(SourceNames) == 3
    definitions = {}
    for module in modules:
        module['final_executable_access_hooks'] = 0
        for symbol in module['symbols']:
            assert symbol not in definitions, 'duplicate owned symbol: ' + symbol
            definitions[symbol] = module
    symbol_file = output / 'final_symbols.txt'
    disassembly_file = output / 'final_disassembly.txt'
    checked_tool(['nm', '--defined-only', str(binary)], symbol_file)
    checked_tool(['objdump', '-d', str(binary)], disassembly_file)
    assert symbol_file.stat().st_size > 0 and disassembly_file.stat().st_size > 0
    counts = {symbol: 0 for symbol in (*definitions, *OracleSymbols)}
    with symbol_file.open() as stream:
        for line in stream:
            match = re.match(r'^[0-9a-fA-F]+\s+([a-zA-Z])\s+(.+)$', line.rstrip())
            if match and match[2] in counts:
                assert match[1] in ('t', 'T'), 'owned/oracle symbol is not code: ' + match[2]
                counts[match[2]] += 1
            if match and re.match(r'^venus_\w+_test_', match[2]):
                assert match[2] in OracleSymbols, 'unexpected independent oracle: ' + match[2]
    assert all(count == 1 for count in counts.values()), 'missing/duplicate final owned/oracle symbol'
    boundaries = {symbol: 0 for symbol in definitions}
    hooks = {symbol: 0 for symbol in definitions}
    symbol = ''
    with disassembly_file.open() as stream:
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
        assert module['final_executable_access_hooks'] > 0, 'no final ASan calls: ' + module['source']
        module['final_required_symbol_access_hooks'] = {
            symbol: hooks[symbol] for symbol in module['required_access_hook_symbols']}
        assert all(module['final_required_symbol_access_hooks'].values())
        module['final_symbol_access_hooks'] = {symbol: hooks[symbol] for symbol in module['symbols']}
        assert sum(module['final_symbol_access_hooks'].values()) == module['final_executable_access_hooks']
        module['final_defined_symbols'] = len(module['symbols'])
    return modules


def main():
    """[in] CLI private base directory; [out] immutable complete three-source receipt.

    Every attempt uses a fresh UTC/PID child. Source/headers/helpers/objects are
    hashed before and after proof/execution; no stale borrowed reports or lazy
    exclusions. Publication follows29 actual Debug units and every proof only.
    All subprocesses/descriptors are bounded/closed; failures preserve incomplete
    artifacts, no source/shared output is changed and no completion is published.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    arguments = parser.parse_args()
    base = arguments.output.resolve()
    base.mkdir(parents=True, exist_ok=True)
    output = base / ('run-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ') + '-' + str(os.getpid()))
    output.mkdir(exist_ok=False)
    print('Private Properties2 artifacts: ' + str(output), flush=True)
    descriptors_before = len(os.listdir('/proc/self/fd'))
    source = Path('src/vgpu/venus_properties_native.zig')
    inventories = owned.source_inventory(source)
    assert {path.name for path in inventories} == set(SourceNames)
    assert len(inventories) == 3 and set(UnitCounts) == set(SourceNames)
    for path, inventory in inventories.items():
        assert len(inventory['test_lines']) == UnitCounts[path.name], 'named unit inventory changed'
        assert inventory['lazy_native_declarations'] == {}, 'no permitted lazy body in Properties2 closure'
        if path.name == 'venus_render_wire.zig':
            assert set(inventory['excluded']) == {'image_fixture'}
        else:
            assert inventory['excluded'] == {}
    provenance = {str(path): dependency.file_hash(path) for path in inventories}
    for path in (Path(__file__), Path(dependency.__file__), Path(owned.__file__),
                 Path('impl/software-vgpu-slicing/properties_field_inventory.json'),
                 Path('scripts/vgpu_properties_safety.mk')):
        resolved = path.resolve()
        merge_inputs(provenance, {str(resolved): dependency.file_hash(resolved)})
    c_flags = ('-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-UNDEBUG',
               '-O1', '-g', '-fsanitize=address,leak,undefined', '-fno-omit-frame-pointer')
    objects = {}
    for name in OracleNames:
        folder = output / name
        folder.mkdir()
        frontend = Path('tests/vgpu') / (name + '_oracle.c')
        includes = RendererIncludes if name == 'properties_reply' else GuestIncludes
        merge_inputs(provenance, dependency.input_provenance(source, frontend, includes, folder))
        target = folder / 'oracle.o'
        checked_tool(['cc', *c_flags, *includes, '-c', str(frontend), '-o', str(target)], folder / 'compile.log')
        assert name not in objects and target.is_file()
        objects[name] = target
    assert tuple(objects) == OracleNames and len(set(objects.values())) == 3
    (output / 'inputs.json').write_text(json.dumps(provenance, indent=2) + '\n')
    immutable_objects = {str(path.resolve()): dependency.file_hash(path) for path in objects.values()}
    runtime_paths = set()
    for name in ('libasan.so', 'libubsan.so'):
        path = Path(subprocess.check_output(['cc', '-print-file-name=' + name], text=True, timeout=180).strip())
        assert path.is_file(), 'missing sanitizer runtime: ' + name
        runtime_paths.add(str(path.parent))
    runtime = ['-L' + path for path in sorted(runtime_paths)] + ['-lasan', '-lubsan']
    original = output / 'original.ll'
    sanitized = output / 'sanitized.ll'
    sanitized_object = output / 'sanitized.o'
    oracle_arguments = [str(path) for path in objects.values()]
    dependency.verify_inputs(provenance)
    checked_tool(['zig', 'test', str(source), '-Iinclude', '-Isubmodules/venus_protocol/include',
                  '-O', 'Debug', '-lc', *oracle_arguments, *runtime, '--test-no-exec',
                  '-femit-llvm-ir=' + str(original), '-femit-bin=' + str(output / 'original')], output / 'zig_compile.log')
    report = owned.instrument(source, original, sanitized, inventories, mode='tests')
    assert report['entire_module_reverse_proof'] is True
    assert report['uncovered_owned_functions'] == [] and report['guard_stores_preserved'] > 0
    checked_tool(['clang-19', '-Wno-override-module', '-fPIC', '-fsanitize=address', '-g',
                  '-c', str(sanitized), '-o', str(sanitized_object)], output / 'asan_compile.log')
    owned.verify_access_hooks(sanitized_object, report, sanitized)
    for module in report['modules']:
        assert module['non_reachable_runtime_declarations'] == {}
        assert module['uncovered_owned_functions'] == [] and module['asan_access_hook_relocations'] > 0
        assert module['source_sha256'] == provenance[module['source']]
        assert module['instrumented_definitions'] == len(module['symbols']) > 0
    binary = output / 'runner'
    checked_tool(['zig', 'cc', str(sanitized_object), *oracle_arguments, *runtime, '-pthread', '-ldl', '-lm',
                  '-o', str(binary)], output / 'link.log')
    report['final_modules'] = final_access_proof(binary, report, output)
    environment = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1:halt_on_error=1',
                       LSAN_OPTIONS='exitcode=23', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    log = output / 'execution.log'
    checked_tool([str(binary)], log, environment)
    execution = log.read_text()
    assert re.findall(r'^All (\d+) tests passed\.$', execution, re.M) == [str(sum(UnitCounts.values()))]
    assert not re.search(r'(?:AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer):|runtime error:', execution)
    dependency.verify_inputs(provenance)
    dependency.verify_inputs(immutable_objects)
    assert len(os.listdir('/proc/self/fd')) == descriptors_before, 'gate descriptor owner leaked'
    report['actual_debug_units'] = dict(UnitCounts)
    report['oracles'] = {name: str(path.resolve()) for name, path in objects.items()}
    report['sanitizer_scope'] = 'owned Zig Debug access ASan/full guards; C oracle ASan/LSan/UBSan; testing allocator zero'
    report['excluded_scope'] = 'std/compiler runtime access instrumentation; no host loader/vendor/GPU or Windows acceptance'
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    retained = [path for path in sorted(output.rglob('*')) if path.is_file()]
    assert len(retained) <= MaxRetainedArtifacts
    artifacts = {str(path.resolve()): dependency.file_hash(path) for path in retained}
    completion = {'schema': 'waddle_properties_owned_units_v1', 'output': str(output),
                  'sources': {str(path): inventory['source_sha256'] for path, inventory in inventories.items()},
                  'named_debug_units': dict(UnitCounts), 'total_debug_units': sum(UnitCounts.values()),
                  'oracle_names': list(OracleNames), 'report_sha256': dependency.file_hash(output / 'report.json'),
                  'retained_artifacts': artifacts, 'complete': True}
    (output / 'completion.json').write_text(json.dumps(completion, indent=2) + '\n')
    print('Properties2 owned access gate passed: ' + str(output / 'completion.json'), flush=True)


if __name__ == '__main__':
    main()
