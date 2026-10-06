#!/usr/bin/env python3
"""Instrument owned ICD production accesses with ASan; preserve compiler defenses.

Input: owned Zig source pathname and exclusive artifact directory. The currently
specified integration is venus_icd.zig, with its native C frontend and pinned
oracle dependencies. Debug retains Zig runtime safety and prevents late ASan from
misreading ReleaseSafe-coalesced stack lifetimes. UBSan instruments C, not Zig IR.
All original compiler guards, lifetime hints, panic paths and initializers survive.
No suppression, fault injection, source rewrite or intentionally failing variant.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

if not __debug__:
    raise RuntimeError("preservation gates require Python assertions enabled")

MaxIrBytes = 256 * 1024 * 1024


def run(arguments, **kwargs):
    """[in] Borrow arguments/options; no retained caller storage.

    [out] Child diagnostics; returns None on success, raises on nonzero/180s timeout.
    No ownership beyond synchronous child; caller serializes its artifact directory.
    """
    subprocess.run(arguments, check=True, timeout=180, **kwargs)


def instrument(source, original, output):
    """Own only source-defined functions; normalize guard stores without erasure.

    Borrow source/original paths, own output/JSON. Reject unknown guard grammar,
    missing production definitions or failed byte-for-byte reverse transformation.
    Volatile same-value/same-slot stores implement llvm.stackprotector semantics;
    existing volatile reload, compare, failure edge and SSP attribute remain intact.
    """
    assert original.stat().st_size <= MaxIrBytes
    ir = original.read_text()
    source_code = source.read_text().split('// Test-only fixtures.', 1)[0]
    owned = set(re.findall(r'(?:export )?fn (\w+)\(', source_code))
    metadata = {int(match[1]): match[2] for match in re.finditer(r'^!(\d+) = (.+)$', ir, re.M)}
    changes = []
    found = set()
    symbols = []
    pattern = re.compile(r'^define .*?@(?:"([^"]+)"|([^ (]+))\(.*? #\d+ !dbg !(\d+) \{$', re.M)
    for match in pattern.finditer(ir):
        value = metadata[int(match[3])]
        name = re.search(r'!DISubprogram\(name: "([^"]+)"', value)
        file_id = re.search(r'file: !(\d+)', value)
        if not name or not file_id:
            continue
        filename = metadata[int(file_id[1])]
        if 'filename: "' + source.name + '"' not in filename:
            continue
        canonical = re.sub(r'__anon_\d+$', '', name[1])
        if canonical not in owned:
            continue
        assert 'sanitize_address' not in match[0]
        replacement = re.sub(r'( #\d+ !dbg !\d+ \{)$', r' sanitize_address\1', match[0])
        assert replacement != match[0]
        changes.append((match.start(), match.end(), match[0], replacement))
        found.add(canonical)
        symbols.append(match[1] or match[2])
    assert found == owned, 'unaccounted owned functions: ' + str(sorted(owned - found))
    for start, end, before, after in reversed(changes):
        ir = ir[:start] + after + ir[end:]
    guard_pattern = re.compile(r'call void @llvm\.stackprotector\(ptr (%[\w.]+), ptr (%[\w.]+)\)')
    guard_changes = []
    for match in guard_pattern.finditer(ir):
        value, slot = match[1], match[2]
        # SSA names repeat across functions. Validate each guard in its own body,
        # not by finding an unrelated function's slot reload or failure edge.
        function_start = ir.rfind('\ndefine ', 0, match.start()) + 1
        function_end = ir.index('\n}', match.end())
        body = ir[function_start:function_end]
        assert re.search(re.escape(slot) + r' = alloca ptr, align 8', body)
        assert re.search(re.escape(value) + r' = load volatile ptr, ptr addrspace\(257\) inttoptr \(i32 40 to ptr addrspace\(257\)\), align 8', body)
        assert re.search(r'load volatile ptr, ptr ' + re.escape(slot) + r', align 8', body)
        assert 'icmp eq ptr' in body and 'call void @__stack_chk_fail()' in body
        guard_changes.append((match.start(), match.end(), match[0],
                              'store volatile ptr ' + match[1] + ', ptr ' + slot + ', align 8'))
    assert guard_changes
    assert len(guard_changes) == len(re.findall(r'call void @llvm\.stackprotector\(', ir))
    for start, end, before, after in reversed(guard_changes):
        ir = ir[:start] + after + ir[end:]
    # Undo the precise accepted replacements and require the original entire module.
    reverse = ir
    for _, _, before, after in guard_changes:
        reverse = reverse.replace(after, before, 1)
    for _, _, before, after in changes:
        reverse = reverse.replace(after, before, 1)
    assert reverse == original.read_text(), 'guard/function/module preservation failed'
    assert ir.count('call void @__stack_chk_fail(') == reverse.count('call void @__stack_chk_fail(')
    output.write_text(ir)
    report = {'source': str(source.resolve()), 'mode': 'Debug',
              'owned_source_functions': len(owned), 'instrumented_definitions': len(symbols),
              'uncovered_owned_functions': [], 'guard_stores_preserved': len(guard_changes),
              'symbols': symbols, 'entire_module_reverse_proof': True,
              'ubsan_scope': 'C frontend only; Zig Debug runtime safety retained'}
    output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    """[in] CLI source/exclusive directory; [out] owned reports/ordinary test artifacts.

    Returns None on complete ordinary verification; errors propagate as nonzero.
    No source or existing object mutation; serial invocation per output directory.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    arguments = parser.parse_args()
    source, output = arguments.source, arguments.output.resolve()
    assert source.name == 'venus_icd.zig', 'additional module integration requires its own dependency contract'
    output.mkdir(parents=True, exist_ok=True)
    warnings = ['-Wall', '-Wextra', '-Wpedantic', '-Werror']
    includes = ['-Iinclude', '-Itests/vgpu/encoder', '-Ibuild/venus_protocol',
                '-Isubmodules/venus_protocol/tests', '-Isubmodules/venus_protocol/include']
    dependencies = ['build/venus_capabilities.o', 'build/venus_command.o', 'build/venus_objects.o', 'build/venus_instance_wire.o',
                    'build/venus_query_wire.o', 'build/venus_values.o', 'build/venus_values_oracle.o']
    oracles = ['build/venus_render_wire_oracle.o', 'build/venus_descriptor_wire_oracle.o',
               'build/venus_compute_wire_oracle.o', 'build/venus_graphics_wire_oracle.o',
               'build/venus_graphics_pipeline_wire_oracle.o', 'build/venus_graphics_command_wire_oracle.o']
    safety = ['-fsanitize=address,leak,undefined', '-fno-omit-frame-pointer']
    runtime_paths = sorted({str(Path(subprocess.check_output(['cc', '-print-file-name=' + name], text=True).strip()).parent)
                            for name in ('libasan.so', 'libubsan.so')})
    runtime = [item for path in runtime_paths for item in ('-L' + path,)] + ['-lasan', '-lubsan']
    environment = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1:halt_on_error=1',
                       UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    run(['zig', 'build-obj', str(source), '-Iinclude', '-Isubmodules/venus_protocol/include',
         '-O', 'Debug', '-fPIC', '-fcompiler-rt', '-lc',
         '-femit-llvm-ir=' + str(output / 'native_original.ll'), '-femit-bin=' + str(output / 'native_original.o')])
    native_report = instrument(source, output / 'native_original.ll', output / 'native_sanitized.ll')
    run(['clang-19', '-Wno-override-module', '-fPIC', '-fsanitize=address', '-g', '-c',
         str(output / 'native_sanitized.ll'), '-o', str(output / 'native_sanitized.o')])
    symbols = subprocess.check_output(['nm', '-u', str(output / 'native_sanitized.o')], text=True)
    assert '__asan_report_store' in symbols and '__asan_report_load' in symbols
    run(['cc', '-std=c11', '-D_GNU_SOURCE', '-O1', '-g', *warnings, *safety, *includes,
         'tests/vgpu/icd.c', str(output / 'native_sanitized.o'), *dependencies, '-pthread', '-o', str(output / 'native_runner')])
    run([str(output / 'native_runner')], env=environment)
    run(['cc', '-std=c11', '-D_GNU_SOURCE', '-O1', '-g', *warnings, *safety, '-Dmain=venus_icd_native_fixture',
         *includes, '-c', 'tests/vgpu/icd.c', '-o', str(output / 'native_oracle.o')])
    run(['zig', 'test', str(source), '-Iinclude', '-Isubmodules/venus_protocol/include',
         str(output / 'native_oracle.o'), *dependencies, *oracles, '-lc', '-O', 'Debug', '--test-no-exec',
         '-femit-llvm-ir=' + str(output / 'test_original.ll'), '-femit-bin=' + str(output / 'test_original'), *runtime])
    test_report = instrument(source, output / 'test_original.ll', output / 'test_sanitized.ll')
    run(['clang-19', '-Wno-override-module', '-fPIC', '-fsanitize=address', '-g', '-c',
         str(output / 'test_sanitized.ll'), '-o', str(output / 'test_sanitized.o')])
    run(['zig', 'cc', str(output / 'test_sanitized.o'), str(output / 'native_oracle.o'),
         *dependencies, *oracles, *runtime, '-pthread', '-ldl', '-lm', '-o', str(output / 'test_runner')])
    run([str(output / 'test_runner')], env=environment)
    summary = {name: {key: value for key, value in report.items() if key != 'symbols'}
               for name, report in (('native', native_report), ('zig_tests', test_report))}
    print(json.dumps(summary, indent=2), flush=True)


if __name__ == '__main__':
    main()
