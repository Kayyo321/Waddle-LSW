#!/usr/bin/env python3
"""Gate every production C transport line and conditional edge at 90 percent."""
import gzip
import json
from pathlib import Path
import shutil
import subprocess
import sys

mode = sys.argv[1] if len(sys.argv) > 1 else 'ring'
if mode not in ('ring', 'region', 'mapping', 'wait', 'session', 'receiver'):
    raise ValueError(mode)
root = Path.cwd()
output = root / 'build/coverage/vgpu' / mode
if output.exists():
    shutil.rmtree(output)
output.mkdir(parents=True)
subprocess.run([
    'cc', '-D_GNU_SOURCE', '-std=c11', '-Iinclude', '-Isrc/vgpu', '-O0', '-g', '--coverage',
    '-fprofile-abs-path',
    *(['-Isubmodules/virglrenderer/src', '-Ibuild/vendor/virglrenderer/src'] if mode == 'receiver' else []),
    '-c', 'tests/vgpu/receiver_owner.c' if mode == 'receiver' else f'src/vgpu/venus_{mode}{"_linux" if mode == "mapping" else ""}.c',
    '-o', str(output / f'{mode}.o'),
], check=True)
subprocess.run([
    'cc', '-D_GNU_SOURCE', '-std=c11', '-Iinclude',
    *([f'tests/vgpu/{mode}.c'] if mode != 'receiver' else []), str(output / f'{mode}.o'),
    *(['src/vgpu/venus_ring.c', '-Isrc/vgpu'] if mode in ('region', 'mapping', 'wait', 'session') else []),
    *(['src/vgpu/venus_region.c', 'build/venus_control.o'] if mode == 'session' else []),
    *(['src/vgpu/venus_region.c', '-Isrc/vgpu', '-Wl,--wrap=memfd_create,--wrap=ftruncate,--wrap=fcntl,--wrap=mmap,--wrap=venus_region_init,--wrap=venus_region_attach'] if mode == 'mapping' else []),
    *(['-pthread'] if mode == 'receiver' else []),
    'build/venus_receiver_bounds.o' if mode == 'receiver' else 'build/venus_bounds.o', '--coverage', '-o', str(output / 'runner'),
], check=True)
subprocess.run([str(output / 'runner')], check=True)
subprocess.run(['gcov', '--json-format', '-b', '-c', str(output / f'{mode}.gcno')],
               cwd=output, check=True)
report = json.loads(gzip.decompress((output / f'{mode}.gcov.json.gz').read_bytes()))
lines = next(file['lines'] for file in report['files']
             if file['file'].endswith(f'/src/vgpu/venus_{mode}{"_linux" if mode == "mapping" else ""}.c'))
branches = [branch for line in lines for branch in line.get('branches', [])]
for kind, entries in [('line', lines), ('branch', branches)]:
    if not entries:
        raise RuntimeError(f'No production {kind} coverage recorded')
    covered = sum(entry['count'] > 0 for entry in entries)
    percent = 100 * covered / len(entries)
    print(f'venus_{mode} production {kind} coverage: {percent:.2f}% '
          f'({covered}/{len(entries)})', flush=True)
    if percent < 90:
        raise RuntimeError(f'{kind} coverage below 90%: {percent:.2f}%')
