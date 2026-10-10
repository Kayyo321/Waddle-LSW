#!/usr/bin/env python3
"""Read-only regression gates for the exact device sanitizer production closure.

Borrow repository sources; own and clean temporary synthetic parser inputs only.
No production changes, compiled fault variants, suppression, or runtime gate bypass.
Run from the repository root. Each failure exits nonzero before sanitizer emission.
"""
from pathlib import Path
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


class inventory_tests_t(unittest.TestCase):
    """Own test assertions; sources remain immutable and temporary inputs are scoped."""

    def test_exact_device_closures(self):
        """Require every recursively imported production module and every unit suite."""
        for root, names, expected, units in (
                ('venus_device_wire.zig', wire.SourceNames, WireSources, 32),
                ('venus_device_native.zig', native.SourceNames,
                 WireSources | {'venus_device_native.zig'}, 45)):
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
