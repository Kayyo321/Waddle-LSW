#!/usr/bin/env python3
"""Gate the production Win32 sink at 90% lines/branches using native API doubles.

No SDK/vendor implementation is copied. Outputs are unique retained local build
artifacts. This measures owned C paths on Linux, not actual Windows execution.
"""
from datetime import datetime, timezone
import gzip
import json
import os
from pathlib import Path
import subprocess

output = Path.cwd() / 'build/coverage/vgpu/wsi_native' / (
    'run-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ') + '-' + str(os.getpid()))
output.mkdir(parents=True, exist_ok=False)
print('preserved WSI native coverage: ' + str(output), flush=True)
obj = output / 'native.o'
includes = ['-Iinclude', '-Itests/vgpu/win32_fake']
subprocess.run(['cc', '-std=c11', '-D_WIN32', *includes, '-O0', '-g', '--coverage',
                '-fprofile-abs-path', '-c', 'src/vgpu/venus_win32_present.c', '-o', str(obj)], check=True)
subprocess.run(['cc', '-std=c11', '-D_WIN32', *includes,
                'tests/vgpu/win32_present_portable.c', str(obj), '--coverage',
                '-o', str(output / 'runner')], check=True)
subprocess.run([str(output / 'runner')], check=True)
subprocess.run(['gcov', '--json-format', '-b', '-c', str(output / 'native.gcno')],
               cwd=output, check=True)
report = json.loads(gzip.decompress((output / 'native.gcov.json.gz').read_bytes()))
lines = next(item['lines'] for item in report['files']
             if item['file'].endswith('/src/vgpu/venus_win32_present.c'))
branches = [branch for line in lines for branch in line.get('branches', [])]
for kind, entries in (('line', lines), ('branch', branches)):
    assert entries, 'No production coverage recorded'
    covered = sum(entry['count'] > 0 for entry in entries)
    percent = 100 * covered / len(entries)
    print(f'venus_win32_present production {kind} coverage: {percent:.2f}% '
          f'({covered}/{len(entries)})', flush=True)
    assert percent >= 90, [(line['line_number'], line['count']) for line in lines]
