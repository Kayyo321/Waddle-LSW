#!/usr/bin/env python3
"""Gate owned lifecycle framing and Linux syscall boundary coverage separately."""
import gzip
import json
from pathlib import Path
import shutil
import subprocess

output = Path.cwd() / 'build/coverage/vgpu/channel'
if output.exists():
    shutil.rmtree(output)
output.mkdir(parents=True)
sources = ['src/vgpu/venus_channel.c', 'src/vgpu/venus_stream_linux.c']
for source in sources:
    subprocess.run(['cc', '-D_GNU_SOURCE', '-std=c11', '-Iinclude', '-Isrc/vgpu',
                    '-O0', '-g', '--coverage', '-fprofile-abs-path', '-c', source,
                    '-o', str(output / (Path(source).stem + '.o'))], check=True)
subprocess.run(['cc', '-D_GNU_SOURCE', '-std=c11', '-Iinclude', '-Isrc/vgpu',
                'tests/vgpu/channel.c', *[str(output / (Path(s).stem + '.o')) for s in sources],
                'src/vgpu/venus_session.c', 'src/vgpu/venus_region.c', 'src/vgpu/venus_ring.c',
                'src/vgpu/venus_wait.c', 'build/venus_control.o', 'build/venus_bounds.o',
                '--coverage', '-pthread', '-Wl,--wrap=clock_gettime,--wrap=poll,--wrap=recv,--wrap=send',
                '-o', str(output / 'runner')], check=True)
subprocess.run([str(output / 'runner')], check=True)
for source in sources:
    stem = Path(source).stem
    subprocess.run(['gcov', '--json-format', '-b', '-c', str(output / (stem + '.gcno'))],
                   cwd=output, check=True)
    report = json.loads(gzip.decompress((output / (stem + '.gcov.json.gz')).read_bytes()))
    lines = next(file['lines'] for file in report['files'] if file['file'].endswith('/' + source))
    branches = [branch for line in lines for branch in line.get('branches', [])]
    for kind, entries in [('line', lines), ('branch', branches)]:
        if not entries:
            raise RuntimeError('No production coverage: ' + source)
        covered = sum(entry['count'] > 0 for entry in entries)
        percent = 100 * covered / len(entries)
        print(f'{stem} production {kind} coverage: {percent:.2f}% ({covered}/{len(entries)})')
        if percent < 90:
            raise RuntimeError('Coverage below 90%: ' + source)
