#!/usr/bin/env python3
"""Read-only regression gates for the exact device sanitizer production closure.

Borrow repository sources; own and clean temporary synthetic parser inputs only.
No production changes, compiled fault variants, suppression, or runtime gate bypass.
Run from the repository root. Each failure exits nonzero before sanitizer emission.
"""
from pathlib import Path
import hashlib
import re
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
import device_native_sanitizers as native
import device_wire_sanitizers as wire
import icd_owned_sanitizers as owned

SourceRoot = Path(__file__).resolve().parents[2] / 'src/vgpu'
WireSources = {'venus_device_wire.zig', 'venus_render_wire.zig',
               'venus_image_view_native.zig', 'venus_pipeline_wire_helpers.zig'}
HelperFunctions = {
    'venus_pipeline_wire_helpers.zig': {'address', 'elements', 'collect_chain',
                                      'encode_chain', 'encode_specialization'},
    'venus_image_view_native.zig': {'format_class', 'compatible', 'aspects',
                                  'snapshot', 'view_usage', 'validate'},
}
HelperFixtures = {'venus_pipeline_wire_helpers.zig': set(),
                  'venus_image_view_native.zig': {'image_info', 'view_info'}}
M2OwnershipFunctions = {'m2_note_id', 'm2_snapshot'}


class inventory_tests_t(unittest.TestCase):
    """Own test assertions; sources remain immutable and temporary inputs are scoped."""

    def test_exact_device_closures(self):
        """Require every recursively imported production module and every unit suite."""
        for root, names, expected, units in (
                ('venus_device_wire.zig', wire.SourceNames, WireSources, 32),
                ('venus_device_native.zig', native.SourceNames,
                 WireSources | {'venus_device_native.zig'}, 49)):
            with self.subTest(root=root):
                inventory = owned.source_inventory(SourceRoot / root)
                self.assertEqual(len(names), len(set(names)))
                self.assertEqual(set(names), expected)
                self.assertEqual({path.name for path in inventory}, expected)
                self.assertEqual(sum(len(item['test_lines']) for item in inventory.values()), units)

    def test_helpers_keep_every_production_declaration(self):
        """Require all eleven helper declarations above the unique fixture boundary."""
        for name, expected in HelperFunctions.items():
            with self.subTest(source=name):
                source = SourceRoot / name
                code = source.read_text()
                self.assertEqual(code.count('// Test-only fixtures.'), 1)
                production, fixtures = code.split('// Test-only fixtures.')
                declaration = r'^\s*(?:pub |export )?fn (\w+)\('
                self.assertEqual(set(re.findall(declaration, production, re.M)), expected)
                self.assertEqual(set(re.findall(declaration, fixtures, re.M)), HelperFixtures[name])
                item = owned.source_inventory(source)[source.resolve()]
                self.assertEqual(set(item['functions']), expected)
                self.assertEqual(item['excluded'], {})
                self.assertEqual(item['lazy_native_declarations'], {})
                self.assertTrue(all(line < item['fixture_boundary'] for line in item['functions'].values()))

    def test_interleaved_render_fixture_stays_exact(self):
        """The existing render exception excludes only its one private fixture."""
        source = SourceRoot / 'venus_render_wire.zig'
        item = owned.source_inventory(source)[source.resolve()]
        self.assertEqual(set(item['excluded']), {'image_fixture'})
        self.assertEqual(item['excluded']['image_fixture']['reason'], 'interleaved test-only fixture')
        self.assertEqual(len(item['functions']), 15)

    def test_exact_extended_icd_closure(self):
        """All31 embedded owners are selected; no separately linked codec is hidden."""
        inventory = owned.icd_source_inventory(SourceRoot / 'venus_icd.zig')
        self.assertEqual(len(inventory), 31)
        self.assertEqual({path.name for path in inventory}, set(owned.IcdEmbeddedSourceNames))
        self.assertEqual(sum(len(item['functions']) for item in inventory.values()), 674)
        icd = inventory[(SourceRoot / 'venus_icd.zig').resolve()]
        self.assertTrue(M2OwnershipFunctions <= set(icd['functions']))
        self.assertTrue(M2OwnershipFunctions.isdisjoint(icd['excluded']))
        self.assertTrue({'venus_wsi.zig', 'venus_transfer2_native.zig',
                         'venus_descriptor_template_native.zig', 'venus_shader_wire.zig',
                         'venus_image_transfer_native.zig'} <= {path.name for path in inventory})
        self.assertTrue({'venus_capabilities.zig', 'venus_command.zig', 'venus_objects.zig',
                         'venus_instance_wire.zig', 'venus_query_wire.zig',
                         'venus_values.zig'}.isdisjoint(path.name for path in inventory))

    def test_exact_native_test_helper_signatures(self):
        """Only exact audited lazy helpers may be absent natively; tests must emit them."""
        inventory = owned.icd_source_inventory(SourceRoot / 'venus_icd.zig')
        observed = 0
        for source, item in inventory.items():
            expected = owned.NativeTestSignatures.get(source.name, {})
            self.assertEqual(set(item['lazy_native_declarations']), set(expected))
            for name, declaration in item['lazy_native_declarations'].items():
                self.assertEqual(declaration['declaration'], expected[name])
                self.assertEqual(declaration['line'], item['functions'][name])
                self.assertEqual(source.read_text().count(expected[name]), 1)
                if name in M2OwnershipFunctions:
                    self.assertTrue(declaration['require_access_hooks'])
                observed += 1
        self.assertEqual(observed, 20)
        self.assertEqual(owned.NativeTestAccessHooks['venus_icd.zig'], M2OwnershipFunctions)

    def test_m2_probe_import_export_guards_preserve_public_abi(self):
        """Qualification ABI stays macro-gated; ordinary eleven exports stay exact."""
        code = (SourceRoot / 'venus_icd.zig').read_text()
        self.assertEqual(code.count('const m2_c = if (builtin.is_test or @hasDecl(c, "WaddleM2OwnershipProbe")) @cImport({'), 1)
        self.assertEqual(code.count('@cInclude("../tests/vgpu/m2_ownership_probe.h");'), 1)
        self.assertRegex(code, r'comptime \{\n\s+if \(@hasDecl\(c, "WaddleM2OwnershipProbe"\)\)\n\s+@export\(m2_snapshot, \.\{ \.name = "venus_icd_m2_snapshot" \}\);\n\}')
        ordinary_functions = set(re.findall(r'^export fn (\w+)\(', code, re.M))
        self.assertEqual(ordinary_functions, {
            'venus_icd_bind', 'venus_icd_bind_capabilities', 'venus_icd_bind_timed',
            'venus_icd_unbind', 'venus_icd_abandon', 'venus_icd_negotiate_loader',
            'venus_icd_get_instance_proc_addr', 'venus_icd_get_physical_proc_addr'})
        ordinary_aliases = set(re.findall(r'\.name = "(vk_[^"]+)"', code))
        self.assertEqual(ordinary_aliases, {
            'vk_icdNegotiateLoaderICDInterfaceVersion', 'vk_icdGetInstanceProcAddr',
            'vk_icdGetPhysicalDeviceProcAddr'})

    def test_test_image_rejects_missing_m2_and_unknown_owned_functions(self):
        """Neither lazy M2 helpers nor any other inventory entry can disappear in tests."""
        with tempfile.TemporaryDirectory(prefix='waddle-owned-missing-') as folder:
            root = Path(folder)
            source = root / 'venus_icd.zig'
            source.write_text('fn retained() void {}\n')
            source = source.resolve()
            original = root / 'module.ll'
            original.write_text(
                'define void @retained() #0 !dbg !3 {\n ret void\n}\n'
                '!2 = !DIFile(filename: "venus_icd.zig", directory: "' + str(root) + '")\n'
                '!3 = !DISubprogram(name: "retained", file: !2, line: 1)\n')
            base = {'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
                    'excluded': {}, 'test_lines': set(), 'fixture_boundary': 2}
            output = root / 'instrumented.ll'
            for missing_functions, lazy, expected_error in (
                    ({'retained': 1, 'm2_snapshot': 2},
                     {'m2_snapshot': {'require_access_hooks': True}}, 'required helper missing from Zig tests'),
                    ({'retained': 1, 'unknown_owner': 2}, {}, 'unaccounted owned functions')):
                inventory = dict(base, functions=missing_functions, lazy_native_declarations=lazy)
                with self.subTest(missing=tuple(missing_functions)):
                    with self.assertRaisesRegex(AssertionError, expected_error):
                        owned.instrument(source, original, output, {source: inventory}, mode='tests')

    def test_native_layout_only_contract_fails_on_runtime_emission(self):
        """Only the exact four layout-source helpers may have zero native code."""
        functions = owned.NativeTestSignatures['venus_descriptor_wire.zig']
        module = {'source': str(SourceRoot / 'venus_descriptor_wire.zig'),
                  'emission_mode': 'native', 'symbols': [],
                  'runtime_functions': dict.fromkeys(functions, 1),
                  'non_reachable_runtime_declarations': dict.fromkeys(functions, {})}
        self.assertTrue(owned.native_type_only(module))
        module['symbols'] = ['unexpected_live_definition']
        with self.assertRaises(AssertionError):
            owned.native_type_only(module)
        module['symbols'] = []
        module['non_reachable_runtime_declarations'].pop('put')
        with self.assertRaises(AssertionError):
            owned.native_type_only(module)
        module['emission_mode'] = 'tests'
        self.assertFalse(owned.native_type_only(module))

    def test_noinline_sync_array_validation_is_required(self):
        """An explicit noinline modifier cannot hide borrowed-array validation."""
        source = SourceRoot / 'venus_modern_sync_wire.zig'
        item = owned.source_inventory(source)[source.resolve()]
        self.assertEqual(item['functions']['array'], 17)
        self.assertEqual(item['excluded'], {})

    def test_interleaved_image_transfer_production_is_required(self):
        """A first-test cutoff must never hide later live format validation."""
        source = SourceRoot / 'venus_image_transfer_native.zig'
        item = owned.source_inventory(source)[source.resolve()]
        self.assertLess(min(item['test_lines']), item['functions']['numeric_class'])
        self.assertLess(min(item['test_lines']), item['functions']['blit_compatible'])
        self.assertEqual(item['excluded'], {})
        self.assertEqual(item['lazy_native_declarations'], {})

    def test_same_named_member_and_function_keep_both_declarations(self):
        """Instrument both packet member put and module put by exact name/location."""
        source = SourceRoot / 'venus_graphics_general_wire.zig'
        item = owned.source_inventory(source)[source.resolve()]
        declarations = {name: line for name, line in item['functions'].items() if name.startswith('put@')}
        self.assertEqual(declarations, {'put@29': 29, 'put@38': 38})
        self.assertNotIn('put', item['functions'])
        self.assertNotIn('link', item['functions'])
        self.assertNotIn('compare_fixture', item['functions'])

    def test_wsi_test_exports_are_excluded_but_every_live_method_is_selected(self):
        """Only the audited fixture suffix may supply test-side native sink symbols."""
        source = SourceRoot / 'venus_wsi.zig'
        item = owned.source_inventory(source)[source.resolve()]
        self.assertIn('present', item['functions'])
        self.assertIn('create_surface', item['functions'])
        self.assertIn('destroy_swapchain', item['functions'])
        self.assertFalse(any(name.startswith('test_') for name in item['functions']))
        self.assertLess(item['functions']['present'], item['fixture_boundary'])
        self.assertEqual(item['lazy_native_declarations'], {})

    def test_parser_rejects_ambiguous_fixture_boundaries(self):
        """Reject synthetic missing/duplicate markers without editing repository inputs."""
        with tempfile.TemporaryDirectory(prefix='waddle-sanitizer-inventory-') as folder:
            source = Path(folder) / 'venus_fixture.zig'
            for suffix in ('', '// Test-only fixtures.\n// Test-only fixtures.\n'):
                with self.subTest(markers=suffix.count('// Test-only fixtures.')):
                    source.write_text('pub fn production() void {}\n' + suffix)
                    with self.assertRaisesRegex(AssertionError, 'missing unique fixture boundary'):
                        owned.source_inventory(source)
            source.write_text('pub fn production() void {}\n// Test-only fixtures.\nfn fixture() void {}\n')
            item = owned.source_inventory(source)[source.resolve()]
            self.assertEqual(item['functions'], {'production': 1})
            self.assertEqual(item['fixture_boundary'], 2)


if __name__ == '__main__':
    unittest.main()
