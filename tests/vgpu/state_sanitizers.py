#!/usr/bin/env python3
"""Instrument owned Zig state functions with ASan and run native allocator tests.

Input: required supported module name; no credentials. Output: ignored build
artifacts/diagnostics. No source mutation. Nonzero exits propagate tool/test errors.
LLVM instrumentation covers owned functions; Zig ReleaseSafe guards remain enabled.
The Zig test runner stays uninstrumented because compiler-version changes to its
stack-protected inline assembly are not a supported cross-compiler boundary.
ASan/LSan allocator interposition remains active process-wide. Parallel distinct
modules are independent; concurrent invocations for the same module are forbidden.
"""
import os
from pathlib import Path
import re
import subprocess
import sys

Modes = ('venus_graphics_state',)
mode = sys.argv[1]
assert mode in Modes
source = Path('src/vgpu') / (mode + '.zig')
output = Path('build/sanitizers/vgpu') / mode
output.mkdir(parents=True, exist_ok=True)
subprocess.run(['zig', 'test', str(source), '-lc', '-O', 'ReleaseSafe',
                '--test-no-exec', '-femit-llvm-ir=' + str(output / 'test.ll'),
                '-femit-bin=' + str(output / 'test')], check=True)
owned_functions = set(re.findall(r'fn (\w+)\(', source.read_text().split('// Test-only fixtures.', 1)[0]))
ir = (output / 'test.ll').read_text()
pattern = r'^(define .*?@' + re.escape(mode) + r'\.(' + '|'.join(sorted(owned_functions)) + r')\(.*?)( #\d+)( !dbg !\d+)? \{$'
instrumented, count = re.subn(pattern, r'\1 sanitize_address\3\4 {', ir, flags=re.M)
assert count > 0, 'no owned production function instrumented'
(output / 'sanitized.ll').write_text(instrumented)
subprocess.run(['clang-19', '-Wno-override-module', '-fsanitize=address,leak,undefined',
                '-g', '-c', str(output / 'sanitized.ll'), '-o', str(output / 'test.o')], check=True)
symbols = subprocess.check_output(['nm', '-u', str(output / 'test.o')], text=True)
assert '__asan_report_' in symbols, 'owned memory accesses lack ASan instrumentation'
runtime_paths = []
for runtime in ('libasan.so', 'libubsan.so'):
    path = Path(subprocess.check_output(['cc', '-print-file-name=' + runtime], text=True).strip())
    assert path.is_file(), 'missing native sanitizer runtime: ' + runtime
    if path.parent not in runtime_paths:
        runtime_paths.append(path.parent)
subprocess.run(['zig', 'cc', str(output / 'test.o'),
                *['-L' + str(path) for path in runtime_paths], '-lasan', '-lubsan',
                '-pthread', '-ldl', '-lm', '-o', str(output / 'runner')], check=True)
env = os.environ.copy()
env['ASAN_OPTIONS'] = 'detect_leaks=1:abort_on_error=1:halt_on_error=1'
subprocess.run([str(output / 'runner')], env=env, check=True)
print(f'{mode}: {count} emitted owned functions instrumented; sanitizer tests passed', flush=True)
