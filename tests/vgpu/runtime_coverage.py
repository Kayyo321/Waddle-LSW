#!/usr/bin/env python3
"""Gate each owned runtime production source at 90% lines and branches."""
from datetime import datetime, timezone
import os
import gzip
import json
from pathlib import Path
import subprocess

output = Path.cwd() / 'build/coverage/vgpu/runtime' / (
    'run-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ') + '-' + str(os.getpid()))
output.mkdir(parents=True, exist_ok=False)
print('preserved runtime coverage: ' + str(output), flush=True)
objects = []
for module in ('rpc', 'dispatch'):
    obj = output / (module + '.o')
    subprocess.run(['cc', '-D_GNU_SOURCE', '-std=c11', '-Iinclude', '-Isrc/vgpu',
                    '-O0', '-g', '--coverage', '-fprofile-abs-path', '-c',
                    f'src/vgpu/venus_{module}.c', '-o', str(obj)], check=True)
    objects.append(str(obj))
subprocess.run(['cc', '-D_GNU_SOURCE', '-std=c11', '-Iinclude', '-Isrc/vgpu',
                'tests/vgpu/runtime.c', *objects, 'src/vgpu/venus_session.c',
                'src/vgpu/venus_region.c', 'src/vgpu/venus_ring.c', 'src/vgpu/venus_wait.c',
                'build/venus_request.o', 'build/venus_capabilities.o', 'build/venus_bounds.o', 'build/venus_control.o',
                '--coverage', '-o', str(output / 'runner')], check=True)
subprocess.run([str(output / 'runner')], check=True)
for module in ('rpc', 'dispatch'):
    subprocess.run(['gcov', '--json-format', '-b', '-c', str(output / (module + '.gcno'))],
                   cwd=output, check=True)
    report = json.loads(gzip.decompress((output / (module + '.gcov.json.gz')).read_bytes()))
    lines = next(item['lines'] for item in report['files']
                 if item['file'].endswith(f'/src/vgpu/venus_{module}.c'))
    branches = [branch for line in lines for branch in line.get('branches', [])]
    for kind, entries in (('line', lines), ('branch', branches)):
        assert entries, 'No production coverage recorded'
        covered = sum(entry['count'] > 0 for entry in entries)
        percent = 100 * covered / len(entries)
        print(f'venus_{module} production {kind} coverage: {percent:.2f}% '
              f'({covered}/{len(entries)})', flush=True)
        assert percent >= 90, [(line['line_number'], line['count']) for line in lines]
