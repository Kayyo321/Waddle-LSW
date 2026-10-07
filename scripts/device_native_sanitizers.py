#!/usr/bin/env python3
"""Genuine owned native-device Debug access gates; immutable inputs, exclusive fresh outputs.

No native device-enable/GPU/Windows acceptance implied. C oracles have UBSan;
Zig keeps Debug defenses. Std/compiler hooks do not count as owned codec hooks.
"""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import subprocess
import sys

# Read-only helper imports must not modify their source directories.
sys.dont_write_bytecode = True
import icd_owned_sanitizers as owned
import icd_dependency_sanitizers as dependency

MaxProofBytes = 256 * 1024 * 1024


def final_access(binary, report, output):
    """[in] Borrow immutable binary/report; [out] owned three-module resolved hook proof.

    Require unique emitted symbols and actual owned-function calls; std/C excluded.
    Close bounded tool outputs and reap180s children; errors reject acceptance.
    No source mutation/retention; caller exclusively owns output directory.
    """
    modules = report['modules']
    assert len(modules) == 3
    lookup = {symbol: module for module in modules for symbol in module['symbols']}
    assert len(lookup) == sum(len(module['symbols']) for module in modules)
    counts = {symbol: 0 for symbol in lookup}
    boundaries = counts.copy()
    hooks = counts.copy()
    for kind, arguments in (('symbols', ['nm', '--defined-only', str(binary)]),
                            ('disassembly', ['objdump', '-d', str(binary)])):
        path = output / ('final_' + kind + '.txt')
        with path.open('wb') as stream:
            owned.run(arguments, stdout=stream)
        assert 0 < path.stat().st_size <= MaxProofBytes
    with (output / 'final_symbols.txt').open() as stream:
        for line in stream:
            match = re.match(r'^[0-9a-fA-F]+\s+([a-zA-Z])\s+(.+)$', line.rstrip())
            if match and match[2] in counts:
                assert match[1] in ('t', 'T')
                counts[match[2]] += 1
    assert all(count == 1 for count in counts.values())
    symbol = ''
    with (output / 'final_disassembly.txt').open() as stream:
        for line in stream:
            match = re.match(r'^[0-9a-fA-F]+ <(.+)>:', line)
            if match:
                symbol = match[1]
                if symbol in boundaries:
                    boundaries[symbol] += 1
            if symbol in lookup and re.search(
                    r'\bcallq?\s+[^<]*<__asan_(?:report_load\d+|report_store\d+|memcpy|memmove|memset)(?:@[^>]*)?>', line):
                hooks[symbol] += 1
    assert all(count == 1 for count in boundaries.values())
    for module in modules:
        module['final_access_hooks'] = sum(hooks[symbol] for symbol in module['symbols'])
        assert module['final_access_hooks'] > 0
        assert all(hooks[symbol] > 0 for symbol in module['required_access_hook_symbols'])


def main():
    """[in] CLI output base; [out] unique preserved run, exact safety/provenance report.

    Own and deterministically close all subprocesses/files; no shared artifact edits.
    Source/header/object changes, checker output, nonzero or180s timeout fail closed.
    Caller freezes inputs; separate callers require separate bases. No heap/runtime
    state belongs to the production codec; this host verification script ends normally.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    base = parser.parse_args().output.resolve()
    output = base / ('run-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ') + '-' + str(os.getpid()))
    output.mkdir(parents=True, exist_ok=False)
    print('preserved run: ' + str(output), flush=True)
    source = Path('src/vgpu/venus_device_native.zig').resolve()
    inventories = owned.source_inventory(source)
    assert {path.name for path in inventories} == {'venus_device_native.zig', 'venus_device_wire.zig', 'venus_render_wire.zig'}
    provenance = {str(path): dependency.file_hash(path) for path in inventories}
    for path in (Path(__file__).resolve(), Path(owned.__file__).resolve(), Path(dependency.__file__).resolve()):
        provenance[str(path)] = dependency.file_hash(path)
    includes = ['-Iinclude', '-Itests/vgpu/encoder', '-Ibuild/venus_protocol',
                '-Isubmodules/venus_protocol/tests', '-Isubmodules/venus_protocol/include']
    c_flags = ['-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-O1', '-g',
               '-fsanitize=address,leak,undefined', '-fno-omit-frame-pointer']
    objects = []
    for name in ('device', 'render'):
        frontend = Path('tests/vgpu/' + name + '_wire_oracle.c')
        folder = output / name
        folder.mkdir()
        inputs = dependency.input_provenance(source, frontend, includes, folder)
        for path, checksum in inputs.items():
            assert path not in provenance or provenance[path] == checksum
            provenance[path] = checksum
        obj = folder / 'oracle.o'
        owned.run(['cc', *c_flags, *includes, '-c', str(frontend), '-o', str(obj)])
        objects.append(str(obj))
    dependency.verify_inputs(provenance)
    runtime_paths = set()
    for name in ('libasan.so', 'libubsan.so'):
        path = Path(subprocess.check_output(['cc', '-print-file-name=' + name], text=True, timeout=180).strip())
        assert path.is_file()
        runtime_paths.add(str(path.parent))
    runtime = ['-L' + path for path in sorted(runtime_paths)] + ['-lasan', '-lubsan']
    owned.run(['zig', 'test', str(source), '-Iinclude', '-Isubmodules/venus_protocol/include',
               '-O', 'Debug', '-lc', *objects, *runtime, '--test-no-exec',
               '-femit-llvm-ir=' + str(output / 'original.ll'), '-femit-bin=' + str(output / 'original')])
    report = owned.instrument(source, output / 'original.ll', output / 'sanitized.ll', inventories, mode='tests')
    owned.run(['clang-19', '-Wno-override-module', '-fPIC', '-fsanitize=address', '-g', '-c',
               str(output / 'sanitized.ll'), '-o', str(output / 'sanitized.o')])
    owned.verify_access_hooks(output / 'sanitized.o', report, output / 'sanitized.ll')
    link_objects = [str(output / 'sanitized.o'), *objects]
    object_hashes = {path: dependency.file_hash(Path(path)) for path in link_objects}
    binary = output / 'runner'
    owned.run(['zig', 'cc', *link_objects, *runtime, '-pthread', '-ldl', '-lm', '-o', str(binary)])
    final_access(binary, report, output)
    binary_hash = dependency.file_hash(binary)
    environment = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1:halt_on_error=1',
                       LSAN_OPTIONS='exitcode=23', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    log = output / 'execution.log'
    dependency.verify_inputs(provenance)
    dependency.verify_inputs(object_hashes)
    with log.open('wb') as stream:
        owned.run([str(binary)], env=environment, stdout=stream, stderr=stream)
    assert log.stat().st_size <= MaxProofBytes
    text = log.read_text()
    assert not re.search(r'ERROR: (?:AddressSanitizer|LeakSanitizer)|runtime error:', text)
    unit_counts = re.findall(r'^All (\d+) tests passed\.$', text, re.M)
    assert len(unit_counts) == 1
    expected = sum(len(inventory['test_lines']) for inventory in inventories.values())
    assert int(unit_counts[0]) == expected
    dependency.verify_inputs(provenance)
    dependency.verify_inputs(object_hashes)
    assert dependency.file_hash(binary) == binary_hash
    report.update(inputs=provenance, link_objects=object_hashes, executable_sha256=binary_hash,
                  unit_tests=expected, excluded_object_scope='std/compiler outside owned hooks; both C oracles freshly ASan/LSan/C-UBSan compiled')
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'{expected} Debug units pass with genuine per-module final hooks and zero checker findings', flush=True)


if __name__ == '__main__':
    main()
