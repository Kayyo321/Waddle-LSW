#!/usr/bin/env python3
"""Verify the tracked Vulkan loader private patch and its private-source builder.

Runs against the real pinned submodule: the patch set is exact, touches only the three
reviewed Windows files, applies to the pin, is proven present after application, and leaves
every other file byte-identical. A preimage mismatch or an unreviewed patch fails closed.
No build, native Windows execution or GPU use.
"""
import hashlib
import importlib.util
import io
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
Root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('vulkan_loader_private_source', Root / 'scripts/vulkan_loader_private_source.py')
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)
Patch = Root / 'patches/vulkan_loader/0001-release-windows-initialization-module-references.patch'
Touched = {'loader/loader.c', 'loader/loader_windows.c', 'loader/loader_windows.h'}


def pinned_files():
    archive = subprocess.run(['git', 'archive', '--format=tar', builder.pinned_commit()], cwd=builder.Submodule,
                             check=True, capture_output=True).stdout
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        return {m.name: hashlib.sha256(tar.extractfile(m).read()).hexdigest() for m in tar.getmembers() if m.isfile()}


def tree_files(base):
    return {str(p.relative_to(base)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in base.rglob('*') if p.is_file() and p.name != 'waddle_private_source.json'}


class vulkan_loader_patch_tests(unittest.TestCase):
    def setUp(self):
        # Inside the superproject working tree on purpose: the builder must still apply.
        (Root / 'build').mkdir(exist_ok=True)
        self.work = Path(tempfile.mkdtemp(prefix='loader_patch_test_', dir=Root / 'build'))

    def tearDown(self):
        shutil.rmtree(self.work)

    def test_patch_scope_is_windows_only(self):
        text = Patch.read_text()
        files = set(re.findall(r'^\+\+\+ b/(\S+)$', text, re.M))
        self.assertEqual(files, Touched)
        self.assertEqual(set(re.findall(r'^--- a/(\S+)$', text, re.M)), Touched)
        loader_c = text.split('diff --git a/loader/loader.c b/loader/loader.c', 1)[1].split('diff --git', 1)[0]
        added = [line[1:] for line in loader_c.splitlines() if line.startswith('+') and not line.startswith('+++')]
        self.assertEqual([line for line in added if line.strip()], ['#if defined(_WIN32)', '    windows_teardown();', '#endif'])

    def test_builder_applies_and_preserves_everything_else(self):
        output = self.work / 'private'
        self.assertEqual(builder.main(['builder', str(output)]), 0)
        pinned, private = pinned_files(), tree_files(output)
        self.assertEqual(set(pinned), set(private))
        self.assertEqual({name for name in pinned if pinned[name] != private[name]}, Touched)
        windows = (output / 'loader/loader_windows.c').read_text()
        self.assertIn('FreeLibrary(windows_dxgi_module);', windows)
        self.assertIn('FreeLibrary(windows_gdi32_module);', windows)
        self.assertIn('windows_gdi32_module = LoadLibraryEx("gdi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);', windows)
        release = (output / 'loader/loader.c').read_text().split('void loader_release() {', 1)[1].split('\n}\n', 1)[0]
        self.assertLess(release.index('loader_unload_preloaded_icds();'), release.index('windows_teardown();'))
        self.assertLess(release.index('windows_teardown();'), release.index('teardown_global_loader_settings();'))

    def test_mismatching_preimage_fails_closed(self):
        broken = self.work / 'patches'; broken.mkdir()
        (broken / Patch.name).write_text(Patch.read_text().replace('loader_unload_preloaded_icds();\n \n', 'loader_unload_preloaded_icds(); \n \n', 1))
        original = builder.PatchDirectory
        builder.PatchDirectory = broken
        try:
            with self.assertRaises(RuntimeError):
                builder.main(['builder', str(self.work / 'private')])
        finally:
            builder.PatchDirectory = original

    def test_unreviewed_patch_set_rejected(self):
        extra = self.work / 'patches'; extra.mkdir()
        shutil.copy(Patch, extra / Patch.name)
        (extra / '0002-unreviewed.patch').write_text('')
        original = builder.PatchDirectory
        builder.PatchDirectory = extra
        try:
            with self.assertRaisesRegex(RuntimeError, 'reviewed set'):
                builder.main(['builder', str(self.work / 'private')])
        finally:
            builder.PatchDirectory = original

    def test_usage_error(self):
        self.assertEqual(builder.main(['builder']), 2)


if __name__ == '__main__':
    unittest.main()
