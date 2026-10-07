#!/usr/bin/env python3
"""Instrument owned Zig state functions with ASan and run native allocator tests.

Input: required supported module name; no credentials. Output: ignored build
artifacts/diagnostics. No source mutation. Nonzero exits propagate tool/test errors.
LLVM instrumentation covers emitted owned source functions and imports; Zig Debug guards remain enabled.
The Zig test runner stays uninstrumented because compiler-version changes to its
stack-protected inline assembly are not a supported cross-compiler boundary.
ASan/LSan allocator interposition remains active process-wide. Each invocation
retains a unique artifact directory. Linked objects must be supplied by the caller;
this helper does not claim access instrumentation of external objects.
"""
import argparse
from datetime import datetime, timezone
import os
from pathlib import Path
import re
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('mode', help='owned src/vgpu/venus_*.zig test root')
parser.add_argument('--link', nargs='*', default=[], help='existing native objects required by the test root')
args = parser.parse_args()
mode = args.mode
assert re.fullmatch(r'venus_[a-z0-9_]+', mode), 'invalid owned module'
source = Path('src/vgpu') / (mode + '.zig')
assert source.is_file(), 'missing owned test root'
output = Path('build/sanitizers/vgpu') / mode / (
    'run-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ') + '-' + str(os.getpid()))
output.mkdir(parents=True, exist_ok=False)
print('preserved sanitizer artifacts: ' + str(output), flush=True)
includes = ['-Iinclude', '-Isubmodules/venus_protocol/include']
subprocess.run(['zig', 'test', str(source), *includes, '-lc', '-O', 'Debug',
                *args.link, '--test-no-exec', '-femit-llvm-ir=' + str(output / 'test.ll'),
                '-femit-bin=' + str(output / 'test')], check=True)
# Reuse the established hook injection, selecting emitted owned definitions by
# their compiler debug source. Imported owned modules participate automatically;
# compiler/std/test-runner definitions are outside this native source scope.
ir = (output / 'test.ll').read_text()
metadata = {int(match[1]): match[2] for match in
            re.finditer(r'^!(\d+) = (.+)$', ir, re.M)}
def owned_definition(match):
    entry = metadata.get(int(match[2]), '')
    file_id = re.search(r'file: !(\d+)', entry)
    file = metadata.get(int(file_id[1]), '') if file_id else ''
    if not re.search(r'filename: "venus_[a-z0-9_]+\.zig"', file):
        return match[0]
    return match[1] + ' sanitize_address !dbg !' + match[2] + ' {'
pattern = r'^(define [^\n]+?) !dbg !(\d+) \{$'
instrumented = re.sub(pattern, owned_definition, ir, flags=re.M)
count = instrumented.count(' sanitize_address ') - ir.count(' sanitize_address ')
assert count > 0, 'no emitted owned function instrumented'
# Zig0.13 emits explicit guard reloads plus llvm.stackprotector. LLVM19 ASan
# relocates stack slots but does not preserve that intrinsic's explicit store;
# the retained reload then reads an uninitialized guard-slot pointer. Reuse the
# existing owned-sanitizer compatibility lowering: store the identical guard
# value in the identical slot, keeping every reload/check/failure edge intact.
guard_pattern = re.compile(r'call void @llvm\.stackprotector\(ptr (%[\w.]+), ptr (%[\w.]+)\)')
guard_stores = 0
def preserve_guard(match):
    global guard_stores
    guard_stores += 1
    return 'store volatile ptr ' + match[1] + ', ptr ' + match[2] + ', align 8'
instrumented = guard_pattern.sub(preserve_guard, instrumented)
assert instrumented.count('call void @__stack_chk_fail(') == ir.count('call void @__stack_chk_fail(')
print('preserved explicit stack guard stores: ' + str(guard_stores), flush=True)
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
                *args.link, '-pthread', '-ldl', '-lm', '-o', str(output / 'runner')], check=True)
env = os.environ.copy()
env['ASAN_OPTIONS'] = 'detect_leaks=1:abort_on_error=1:halt_on_error=1'
subprocess.run([str(output / 'runner')], env=env, check=True)
print(f'{mode}: {count} emitted owned functions instrumented; sanitizer tests passed', flush=True)
