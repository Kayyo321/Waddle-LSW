#!/usr/bin/env python3
"""Instrument owned ICD/imported production accesses with ASan; preserve defenses.

Input: owned Zig source pathname and exclusive artifact directory. The currently
specified integration is venus_icd.zig, with its native C frontend and pinned
oracle dependencies. Debug retains Zig runtime safety and prevents late ASan from
misreading ReleaseSafe-coalesced stack lifetimes. UBSan instruments C, not Zig IR.
The recursive embedded source closure is accounted for by exact path/name/line.
Separately linked objects remain outside this access-instrumentation scope.
All original compiler guards, lifetime hints, panic paths and initializers survive.
No suppression, fault injection, source rewrite or intentionally failing variant.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

if not __debug__:
    raise RuntimeError("preservation gates require Python assertions enabled")

MaxIrBytes = 256 * 1024 * 1024
# Immutable recursion ceiling; every source stays borrowed within one synchronous gate.
MaxSourceModules = 16
# Exact non-C helper; only an absent native definition can be reported unreachable.
NativeBatchSource = Path("src/vgpu/venus_features_native.zig").resolve()
NativeBatchSignature = "pub fn publish_batch(chain: *const chain_t, nodes: []const node_t) !void {"


def run(arguments, **kwargs):
    """[in] Borrow arguments/options; no retained caller storage.

    [out] Child diagnostics; returns None on success, raises on nonzero/180s timeout.
    No ownership beyond synchronous child; caller serializes its artifact directory.
    """
    subprocess.run(arguments, check=True, timeout=180, **kwargs)


def source_inventory(source):
    """[in] Borrow root; [out] own bounded immutable sibling-source inventory.

    No source mutation/retention of caller buffers; synchronous read-only operation.
    Reject unknown imports, duplicate names and unsupported fixture/type signatures.
    Runtime functions require emitted definitions; the exact type constructor does not.
    """
    source = source.resolve()
    pending, inventories = [source], {}
    while pending:
        current = pending.pop()
        if current in inventories:
            continue
        assert current.parent == source.parent and current.is_file()
        assert len(inventories) < MaxSourceModules
        code = current.read_text()
        fixture_signature = 'fn image_fixture() c.VkImageCreateInfo {'
        type_signature = 'fn slot_type(comptime profile_t: type) type {'
        if current.name == 'venus_render_wire.zig':
            assert '// Test-only fixtures.' not in code and code.count(fixture_signature) == 1
            production = code
        else:
            assert code.count('// Test-only fixtures.') == 1, 'missing unique fixture boundary: ' + str(current)
            production = code.split('// Test-only fixtures.', 1)[0]
        functions, excluded, lazy_native = {}, {}, {}
        for line, text in enumerate(production.splitlines(), 1):
            match = re.match(r'\s*(?:pub |export )?fn (\w+)\(', text)
            if not match:
                continue
            name = match[1]
            assert name not in functions and name not in excluded, 'duplicate declaration: ' + name
            if current.name == 'venus_render_wire.zig' and name == 'image_fixture':
                assert text == fixture_signature
                excluded[name] = {'line': line, 'reason': 'interleaved test-only fixture'}
            elif current.name == 'venus_icd_profiles.zig' and name == 'slot_type':
                assert text == type_signature
                excluded[name] = {'line': line, 'reason': 'compile-time type constructor; no runtime definition'}
            else:
                functions[name] = line
        assert functions
        if current == NativeBatchSource:
            assert production.count(NativeBatchSignature) == 1
            assert 'publish_batch' in functions
            lazy_native['publish_batch'] = {'line': functions['publish_batch'],
                                            'declaration': NativeBatchSignature,
                                            'reason': 'no emitted native definition; private Zig helper has no C export'}
        test_lines = {line for line, text in enumerate(code.splitlines(), 1)
                      if re.match(r'\s*test "', text)}
        inventories[current] = {'functions': functions, 'excluded': excluded, 'test_lines': test_lines,
                                'source_sha256': hashlib.sha256(code.encode()).hexdigest(),
                                'fixture_boundary': len(production.splitlines()) + 1, 'lazy_native_declarations': lazy_native}
        for name in re.findall(r'@import\("([^"\n]+)"\)', code):
            if name in ('std', 'builtin'):
                continue
            assert re.fullmatch(r'venus_\w+\.zig', name), 'unsupported source import: ' + name
            imported = (current.parent / name).resolve()
            assert imported.parent == source.parent
            pending.append(imported)
    return inventories


def instrument(source, original, output, inventories, mode="native"):
    """Own source-closure runtime definitions; normalize guards without erasure.

    Borrow source/original/inventory, own output/JSON. Reject unknown guard grammar,
    missing production definitions or failed byte-for-byte reverse transformation.
    Volatile same-value/same-slot stores implement llvm.stackprotector semantics;
    existing volatile reload, compare, failure edge and SSP attribute remain intact.
    """
    assert mode in ('native', 'tests'), 'unknown emission mode'
    assert original.stat().st_size <= MaxIrBytes
    ir = original.read_text()
    metadata = {int(match[1]): match[2] for match in re.finditer(r'^!(\d+) = (.+)$', ir, re.M)}
    changes, symbols = [], []
    reports = {path: {'source': str(path), 'source_sha256': inventory['source_sha256'],
                      'owned_source_functions': len(inventory['functions']),
                      'runtime_functions': inventory['functions'], 'excluded_declarations': inventory['excluded'],
                      'symbols': [], 'uncovered_owned_functions': [], 'excluded_emitted_definitions': 0,
                      'non_reachable_runtime_declarations': {}, 'required_access_hook_symbols': []}
               for path, inventory in inventories.items()}
    found = {path: set() for path in inventories}
    pattern = re.compile(r'^define .*?@(?:"([^"]+)"|([^ (]+))\(.*? #\d+ !dbg !(\d+) \{$', re.M)
    for match in pattern.finditer(ir):
        value = metadata[int(match[3])]
        name = re.search(r'!DISubprogram\(name: "([^"]+)"', value)
        file_id = re.search(r'file: !(\d+)', value)
        if not name or not file_id:
            continue
        filename = metadata[int(file_id[1])]
        file_match = re.search(r'!DIFile\(filename: "([^"]+)", directory: "([^"]+)"', filename)
        if not file_match:
            continue
        path = (Path(file_match[2]) / file_match[1]).resolve()
        if path not in inventories:
            continue
        inventory, report = inventories[path], reports[path]
        line_match = re.search(r'line: (\d+)', value)
        assert line_match, 'owned definition lacks declaration line: ' + name[1]
        line = int(line_match[1])
        canonical = re.sub(r'__anon_\d+$', '', name[1])
        if canonical not in inventory['functions']:
            assert canonical != 'slot_type' or path.name != 'venus_icd_profiles.zig', 'type constructor emitted at runtime'
            test_definition = name[1].startswith('test.') and line in inventory['test_lines']
            excluded = inventory['excluded'].get(canonical)
            assert (line >= inventory['fixture_boundary'] or test_definition or
                    (excluded and excluded['line'] == line)), 'unknown owned runtime definition: ' + str(path) + ':' + name[1]
            report['excluded_emitted_definitions'] += 1
            continue
        assert inventory['functions'][canonical] == line, 'owned declaration location mismatch: ' + canonical
        assert 'sanitize_address' not in match[0]
        replacement = re.sub(r'( #\d+ !dbg !\d+ \{)$', r' sanitize_address\1', match[0])
        assert replacement != match[0]
        changes.append((match.start(), match.end(), match[0], replacement))
        found[path].add(canonical)
        symbol = match[1] or match[2]
        symbols.append(symbol)
        report['symbols'].append(symbol)
        if mode == 'tests' and canonical in inventory.get('lazy_native_declarations', {}):
            report['required_access_hook_symbols'].append(symbol)
    for path, inventory in inventories.items():
        missing = set(inventory['functions']) - found[path]
        if mode == 'native':
            for name, declaration in inventory.get('lazy_native_declarations', {}).items():
                if name in missing:
                    reports[path]['non_reachable_runtime_declarations'][name] = declaration
                    missing.remove(name)
        else:
            for name in inventory.get('lazy_native_declarations', {}):
                assert name in found[path], 'required helper missing from Zig tests: ' + name
                assert reports[path]['required_access_hook_symbols']
        missing = sorted(missing)
        assert not missing, 'unaccounted owned functions: ' + str(path) + str(missing)
        assert hashlib.sha256(path.read_bytes()).hexdigest() == inventory['source_sha256'], 'source changed during gate'
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
    report = {'source': str(source.resolve()), 'mode': 'Debug', 'emission_mode': mode,
              'owned_source_functions': sum(len(value['functions']) for value in inventories.values()),
              'instrumented_definitions': len(symbols), 'modules': list(reports.values()),
              'uncovered_owned_functions': [], 'guard_stores_preserved': len(guard_changes),
              'symbols': symbols, 'entire_module_reverse_proof': True,
              'ubsan_scope': 'C frontend only; Zig Debug runtime safety retained',
              'excluded_object_scope': 'separately linked Zig dependencies and C wire oracles'}
    output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def verify_access_hooks(binary, report, output):
    """[in] Borrow sanitized object/report; [out] append actual per-module hook proof.

    Read-only disassembly; retain no subprocess resources. Reject a selected module
    without a real ASan access-hook relocation. Compiler/fixture symbols do not count.
    Caller serializes report mutation and artifact directory; no production mutation.
    """
    definitions = {symbol: module for module in report['modules'] for symbol in module['symbols']}
    for module in report['modules']:
        module['asan_access_hook_relocations'] = 0
    hooks = {symbol: 0 for symbol in definitions}
    disassembly = subprocess.check_output(['objdump', '-dr', str(binary)], text=True, timeout=180)
    symbol = ''
    for line in disassembly.splitlines():
        match = re.match(r'^[0-9a-f]+ <(.+)>:', line)
        if match:
            symbol = match[1]
        if re.search(r'R_\w+\s+__asan_(?:report_load|report_store|load|store|memcpy|memmove|memset)', line):
            if symbol in definitions:
                definitions[symbol]['asan_access_hook_relocations'] += 1
                hooks[symbol] += 1
    for module in report['modules']:
        assert module['asan_access_hook_relocations'] > 0, 'no actual ASan accesses in ' + module['source']
        module['instrumented_definitions'] = len(module['symbols'])
        module['required_symbol_access_hooks'] = {symbol: hooks[symbol] for symbol in module['required_access_hook_symbols']}
        assert all(module['required_symbol_access_hooks'].values()), 'required test helper lacks ASan accesses'
    output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')


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
    inventories = source_inventory(source)
    warnings = ['-Wall', '-Wextra', '-Wpedantic', '-Werror']
    includes = ['-Iinclude', '-Itests/vgpu/encoder', '-Ibuild/venus_protocol',
                '-Isubmodules/venus_protocol/tests', '-Isubmodules/venus_protocol/include']
    dependencies = ['build/venus_capabilities.o', 'build/venus_command.o', 'build/venus_objects.o', 'build/venus_instance_wire.o',
                    'build/venus_query_wire.o', 'build/venus_values.o', 'build/venus_values_oracle.o',
                    'build/venus_features_query_oracle.o', 'build/venus_features_reply_oracle.o']
    oracles = ['build/venus_render_wire_oracle.o', 'build/venus_descriptor_wire_oracle.o',
               'build/venus_compute_wire_oracle.o', 'build/venus_graphics_wire_oracle.o',
               'build/venus_graphics_pipeline_wire_oracle.o', 'build/venus_graphics_command_wire_oracle.o']
    safety = ['-fsanitize=address,leak,undefined', '-fno-omit-frame-pointer']
    runtime_paths = sorted({str(Path(subprocess.check_output(['cc', '-print-file-name=' + name], text=True).strip()).parent)
                            for name in ('libasan.so', 'libubsan.so')})
    runtime = [item for path in runtime_paths for item in ('-L' + path,)] + ['-lasan', '-lubsan']
    environment = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1:halt_on_error=1',
                       LSAN_OPTIONS='exitcode=23', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    run(['zig', 'build-obj', str(source), '-Iinclude', '-Isubmodules/venus_protocol/include',
         '-O', 'Debug', '-fPIC', '-fcompiler-rt', '-lc',
         '-femit-llvm-ir=' + str(output / 'native_original.ll'), '-femit-bin=' + str(output / 'native_original.o')])
    native_report = instrument(source, output / 'native_original.ll', output / 'native_sanitized.ll', inventories, mode='native')
    run(['clang-19', '-Wno-override-module', '-fPIC', '-fsanitize=address', '-g', '-c',
         str(output / 'native_sanitized.ll'), '-o', str(output / 'native_sanitized.o')])
    verify_access_hooks(output / 'native_sanitized.o', native_report, output / 'native_sanitized.ll')
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
    test_report = instrument(source, output / 'test_original.ll', output / 'test_sanitized.ll', inventories, mode='tests')
    run(['clang-19', '-Wno-override-module', '-fPIC', '-fsanitize=address', '-g', '-c',
         str(output / 'test_sanitized.ll'), '-o', str(output / 'test_sanitized.o')])
    verify_access_hooks(output / 'test_sanitized.o', test_report, output / 'test_sanitized.ll')
    run(['zig', 'cc', str(output / 'test_sanitized.o'), str(output / 'native_oracle.o'),
         *dependencies, *oracles, *runtime, '-pthread', '-ldl', '-lm', '-o', str(output / 'test_runner')])
    run([str(output / 'test_runner')], env=environment)
    summary = {}
    for name, report in (('native', native_report), ('zig_tests', test_report)):
        compact = {key: value for key, value in report.items() if key not in ('symbols', 'modules')}
        compact['modules'] = [{key: value for key, value in module.items()
                               if key not in ('symbols', 'runtime_functions')}
                              for module in report['modules']]
        summary[name] = compact
    print(json.dumps(summary, indent=2), flush=True)


if __name__ == '__main__':
    main()
