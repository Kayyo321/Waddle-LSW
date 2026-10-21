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
from datetime import datetime, timezone
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
MaxSourceModules = 31
# Exact current ICD recursive import contract; ordinary codec inventories remain generic.
IcdEmbeddedSourceNames = ('venus_compute_state.zig', 'venus_compute_wire.zig', 'venus_descriptor_template_native.zig', 'venus_descriptor_wire.zig', 'venus_device_native.zig', 'venus_device_wire.zig', 'venus_dynamic_rendering_wire.zig', 'venus_extensions_wire.zig', 'venus_extra_objects_wire.zig', 'venus_features_native.zig', 'venus_features_wire.zig', 'venus_graphics_command_wire.zig', 'venus_graphics_dynamic_wire.zig', 'venus_graphics_general_wire.zig', 'venus_graphics_state.zig', 'venus_graphics_wire.zig', 'venus_icd.zig', 'venus_icd_profiles.zig', 'venus_image_transfer_native.zig', 'venus_image_transfer_wire.zig', 'venus_image_view_native.zig', 'venus_modern_sync_wire.zig', 'venus_pipeline_wire_helpers.zig', 'venus_properties_native.zig', 'venus_properties_wire.zig', 'venus_render_wire.zig', 'venus_requirements2_wire.zig', 'venus_sampler_descriptor_wire.zig', 'venus_shader_wire.zig', 'venus_transfer2_native.zig', 'venus_wsi.zig')
# Audited exact fixture separators; never infer production boundaries from first test.
# None retains every non-test function, including production declared after tests.
FixtureBoundaries = {
    'venus_descriptor_template_native.zig': 'fn create_info(entries: []const c.VkDescriptorUpdateTemplateEntry) c.VkDescriptorUpdateTemplateCreateInfo {',
    'venus_dynamic_rendering_wire.zig': '// Test-only generated encoder oracle.',
    'venus_graphics_dynamic_wire.zig': '// Test-only pinned generated oracles.',
    'venus_graphics_general_wire.zig': '// Test-only independent generated encoder, never linked into production runtime.',
    'venus_image_transfer_native.zig': None,
    'venus_image_transfer_wire.zig': '// Test-only generated encoder oracle, never linked into production runtime.',
    'venus_sampler_descriptor_wire.zig': '// Test-only independent pinned encoder entrypoints.',
    'venus_transfer2_native.zig': None,
    'venus_wsi.zig': '// Independent native sink/backend fixtures, emitted only by Zig tests.',
}
# Audited declaration identities absent in the native IR and called by Zig tests.
# These are not public C ABI paths: production uses separate cached/validated
# handlers. Tests must emit/instrument each declaration; unexpected native holes fail.
NativeTestSignatures = {'venus_descriptor_wire.zig': {'allocate_sets': 'pub fn allocate_sets(info: *const '
                                                'c.VkDescriptorSetAllocateInfo, layouts: []const u64, '
                                                'device_id: u64, pool_id: u64, set_ids: []const u64) '
                                                '!writer_t {',
                               'create_pool': 'pub fn create_pool(info: *const c.VkDescriptorPoolCreateInfo, '
                                              'device_id: u64, pool_id: u64) !writer_t {',
                               'put': 'fn put(writer: *writer_t, comptime word_t: type, word: word_t) void {',
                               'update_sets': 'pub fn update_sets(device_id: u64, writes: []const '
                                              'buffer_write_t, copies: []const copy_t) !writer_t {'},
 'venus_extra_objects_wire.zig': {'destroy': 'pub fn destroy(device:u64,id:u64,opcode:u32) !writer_t {'},
 'venus_features_native.zig': {'publish_batch': 'pub fn publish_batch(chain: *const chain_t, nodes: []const '
                                                'node_t) !void {'},
 'venus_graphics_command_wire.zig': {'copy_image_to_buffer': 'pub fn copy_image_to_buffer(command_id: u64, '
                                                             'image_id: u64, buffer_id: u64, layout: '
                                                             'c.VkImageLayout, width: u32, height: u32, '
                                                             'buffer_size: u64, regions: []const '
                                                             'c.VkBufferImageCopy) !writer_t {'},
 'venus_graphics_general_wire.zig': {'create_graphics_pipeline': 'pub fn create_graphics_pipeline(device: '
                                                                 'u64, info: *const '
                                                                 'c.VkGraphicsPipelineCreateInfo, '
                                                                 'shader_ids: []const u64, layout: u64, '
                                                                 'pass: u64, output: u64) !writer_t {'},
 'venus_icd.zig': {'encode_device': 'fn encode_device(request: *const device_native.owned_request_t, '
                                    'physical_id: u64, id: u64) !render_wire.writer_t {'},
 'venus_icd_profiles.zig': {'create_set_profile': 'pub fn create_set_profile(layout: *const '
                                                  'descriptor_layout_t) !descriptor_set_t {',
                            'normalize_bindings': 'pub fn normalize_bindings(bindings: []const binding_t) '
                                                  '!descriptor_layout_t {'},
 'venus_image_transfer_wire.zig': {'copy_image': 'pub fn copy_image(command: u64, source: u64, '
                                                 'source_image_layout: u32, target: u64, '
                                                 'target_image_layout: u32, regions: []const c.VkImageCopy) '
                                                 '!writer_t {'},
 'venus_modern_sync_wire.zig': {'append': 'fn append(writer:*writer_t,bytes:[]const u8) !void {',
                                'set_event2': 'pub fn set_event2(command_id:u64,event_id:u64,info:*const '
                                              'c.VkDependencyInfo) !writer_t {',
                                'wait_events2': 'pub fn wait_events2(command_id:u64,events:[]const '
                                                'u64,infos:[]const c.VkDependencyInfo) !writer_t {'},
 'venus_render_wire.zig': {'create_compute_pipeline': 'pub fn create_compute_pipeline(info: *const '
                                                      'c.VkComputePipelineCreateInfo, device_id: u64, '
                                                      'shader_id: u64, layout_id: u64, pipeline_id: u64) '
                                                      '!writer_t {',
                           'create_descriptor_layout': 'pub fn create_descriptor_layout(info: *const '
                                                       'c.VkDescriptorSetLayoutCreateInfo, device_id: u64, '
                                                       'layout_id: u64) !writer_t {',
                           'create_shader_module': 'pub fn create_shader_module(info: *const '
                                                   'c.VkShaderModuleCreateInfo, device_id: u64, module_id: '
                                                   'u64) !writer_t {'}}
# This source contributes type layouts to native code, with no emitted functions.
# Its four legacy encoders must all be present/instrumented in the allocator image.
NativeTypeOnlySources = ('venus_descriptor_wire.zig',)
ComptimeMaskSignature = 'fn boolean_mask(comptime native_t: type, comptime first: []const u8, comptime count: usize, comptime names: anytype) [count]u32 {'
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
        boundary = FixtureBoundaries.get(current.name, '// Test-only fixtures.')
        if current.name == 'venus_render_wire.zig':
            assert '// Test-only fixtures.' not in code and code.count(fixture_signature) == 1
            production = code
        elif boundary is None:
            production = code
        else:
            assert code.count(boundary) == 1, 'missing unique fixture boundary: ' + str(current)
            production = code.split(boundary, 1)[0]
        functions, excluded, lazy_native = {}, {}, {}
        for line, text in enumerate(production.splitlines(), 1):
            match = re.match(r'\s*(?:(?:pub|export) )?(?:(?:inline|noinline) )?fn (\w+)\(', text)
            if not match:
                continue
            name = match[1]
            assert name not in excluded, 'duplicate excluded declaration: ' + name
            duplicate = name in functions or any(key.startswith(name + '@') for key in functions)
            if name in functions:
                previous = functions.pop(name)
                functions[name + '@' + str(previous)] = previous
            identity = name + '@' + str(line) if duplicate else name
            if current.name == 'venus_render_wire.zig' and name == 'image_fixture':
                assert text == fixture_signature
                excluded[name] = {'line': line, 'reason': 'interleaved test-only fixture'}
            elif current.name == 'venus_icd.zig' and name == 'boolean_mask':
                assert text == ComptimeMaskSignature
                excluded[name] = {'line': line, 'reason': 'compile-time feature-mask value constructor; no runtime definition'}
            elif current.name == 'venus_icd_profiles.zig' and name == 'slot_type':
                assert text == type_signature
                excluded[name] = {'line': line, 'reason': 'compile-time type constructor; no runtime definition'}
            else:
                functions[identity] = line
        assert functions
        for name, signature in NativeTestSignatures.get(current.name, {}).items():
            assert production.count(signature) == 1 and name in functions
            lazy_native[name] = {'line': functions[name], 'declaration': signature,
                                 'reason': 'audited test-only caller; native uses separate production path',
                                 'require_access_hooks': current == NativeBatchSource and name == 'publish_batch'}
        test_lines = {line for line, text in enumerate(code.splitlines(), 1)
                      if re.match(r'\s*test "', text)}
        inventories[current] = {'functions': functions, 'excluded': excluded, 'test_lines': test_lines,
                                'source_sha256': hashlib.sha256(code.encode()).hexdigest(),
                                'fixture_boundary': len(production.splitlines()) + 1, 'lazy_native_declarations': lazy_native}
        imports = re.findall(r'@import\("([^"\n]+)"\)', code)
        assert len(imports) == code.count('@import('), 'unsupported source import grammar'
        for name in imports:
            if name in ('std', 'builtin'):
                continue
            assert re.fullmatch(r'venus_\w+\.zig', name), 'unsupported source import: ' + name
            imported = (current.parent / name).resolve()
            assert imported.parent == source.parent
            pending.append(imported)
    return inventories


def icd_source_inventory(source):
    """[in] Borrow exact ICD root; [out] own thirty-one-module immutable inventory.

    Generic inventory preserves all original source/function/guard ownership rules.
    Require exact canonical sibling names; no minimum/count-only admission, extra
    import or new native lazy exception. Read-only synchronous descriptor lifetime.
    """
    assert source.resolve().name == 'venus_icd.zig'
    inventories = source_inventory(source)
    assert {path.name for path in inventories} == set(IcdEmbeddedSourceNames)
    assert len(inventories) == len(IcdEmbeddedSourceNames) == 31
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
                      'emission_mode': mode, 'owned_source_functions': len(inventory['functions']),
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
        identity = canonical if canonical in inventory['functions'] else canonical + '@' + str(line)
        if identity not in inventory['functions']:
            assert not ((canonical == 'slot_type' and path.name == 'venus_icd_profiles.zig') or
                        (canonical == 'boolean_mask' and path.name == 'venus_icd.zig')), 'compile-time constructor emitted at runtime'
            test_definition = name[1].startswith('test.') and line in inventory['test_lines']
            excluded = inventory['excluded'].get(canonical)
            assert (line >= inventory['fixture_boundary'] or test_definition or
                    (excluded and excluded['line'] == line)), 'unknown owned runtime definition: ' + str(path) + ':' + name[1]
            report['excluded_emitted_definitions'] += 1
            continue
        assert inventory['functions'][identity] == line, 'owned declaration location mismatch: ' + canonical
        assert 'sanitize_address' not in match[0]
        replacement = re.sub(r'( #\d+ !dbg !\d+ \{)$', r' sanitize_address\1', match[0])
        assert replacement != match[0]
        changes.append((match.start(), match.end(), match[0], replacement))
        found[path].add(identity)
        symbol = match[1] or match[2]
        symbols.append(symbol)
        report['symbols'].append(symbol)
        if mode == 'tests' and inventory.get('lazy_native_declarations', {}).get(canonical, {}).get('require_access_hooks'):
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
                if inventory['lazy_native_declarations'][name]['require_access_hooks']:
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


def native_type_only(module):
    """[in] Borrow module proof; [out] bool for the exact native layout-only source.

    No mutation or resource ownership. Reject contract drift: its four legacy
    helpers must be absent natively and all must be instrumented in test mode.
    This is not an access exemption: there is no native definition/access to skip.
    """
    if module['emission_mode'] != 'native' or Path(module['source']).name not in NativeTypeOnlySources:
        return False
    assert module['symbols'] == []
    assert set(module['non_reachable_runtime_declarations']) == set(module['runtime_functions'])
    assert set(module['runtime_functions']) == set(NativeTestSignatures[Path(module['source']).name])
    return True


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
        if native_type_only(module):
            assert module['asan_access_hook_relocations'] == 0
        else:
            assert module['asan_access_hook_relocations'] > 0, 'no actual ASan accesses in ' + module['source']
        module['instrumented_definitions'] = len(module['symbols'])
        module['required_symbol_access_hooks'] = {symbol: hooks[symbol] for symbol in module['required_access_hook_symbols']}
        assert all(module['required_symbol_access_hooks'].values()), 'required test helper lacks ASan accesses'
    output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')


def main():
    """[in] Exact ICD source/output CLI; [out] fresh complete seam qualification.

    Borrow immutable source; delegate to the same synchronous complete-seam gate
    used by make. No stale public objects are linked. All child failures propagate;
    caller exclusively owns the new artifact directory. No source mutation.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    arguments = parser.parse_args()
    assert arguments.source.resolve() == Path('src/vgpu/venus_icd.zig').resolve()
    subprocess.run([os.sys.executable, '-B', str(Path(__file__).with_name('icd_seam_sanitizers.py')),
                    str(arguments.output), '--expected-icd-units', '399'], check=True, timeout=3600)


if __name__ == '__main__':
    main()
