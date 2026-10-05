#!/usr/bin/env python3
"""Gate every production C transport line and conditional edge at 90 percent."""
import gzip
import json
from pathlib import Path
import shutil
import subprocess

root = Path.cwd()
output = root / 'build/coverage/vgpu/ring'
if output.exists():
    shutil.rmtree(output)
output.mkdir(parents=True)
subprocess.run([
    'cc', '-std=c11', '-Iinclude', '-Isrc/vgpu', '-O0', '-g', '--coverage',
    '-fprofile-abs-path', '-c', 'src/vgpu/venus_ring.c', '-o', str(output / 'ring.o'),
], check=True)
subprocess.run([
    'cc', '-std=c11', '-Iinclude', 'tests/vgpu/ring.c', str(output / 'ring.o'),
    'build/venus_bounds.o', '--coverage', '-o', str(output / 'runner'),
], check=True)
subprocess.run([str(output / 'runner')], check=True)
subprocess.run(['gcov', '--json-format', '-b', '-c', str(output / 'ring.gcno')],
               cwd=output, check=True)
report = json.loads(gzip.decompress((output / 'ring.gcov.json.gz').read_bytes()))
lines = next(file['lines'] for file in report['files']
             if file['file'].endswith('/src/vgpu/venus_ring.c'))
branches = [branch for line in lines for branch in line.get('branches', [])]
for kind, entries in [('line', lines), ('branch', branches)]:
    if not entries:
        raise RuntimeError(f'No production {kind} coverage recorded')
    covered = sum(entry['count'] > 0 for entry in entries)
    percent = 100 * covered / len(entries)
    print(f'venus_ring production {kind} coverage: {percent:.2f}% '
          f'({covered}/{len(entries)})', flush=True)
    if percent < 90:
        raise RuntimeError(f'{kind} coverage below 90%: {percent:.2f}%')
