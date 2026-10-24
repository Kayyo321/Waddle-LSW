#!/usr/bin/env python3
"""Gate production C Windows raw mapping owner line/branch coverage without fixture inflation."""
from datetime import datetime, timezone
import gzip
import json
from pathlib import Path
import subprocess
import uuid

Root = Path(__file__).resolve().parents[2]
Source = Root / 'src/interop/interop_mapping_windows.c'
Attempt = Root / 'build/coverage/interop/mapping_windows' / ('run-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S') + '-' + uuid.uuid4().hex[:8])
Attempt.mkdir(parents=True, exist_ok=False)
Flags = ['-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-O0', '-g', '-Iinclude', '-Isrc/interop', '-Itests/interop', '-Itests/interop/win32_shim', '-Isubmodules/looking_glass/vendor/ivshmem', '-DInteropMappingWindowsTestApi="mapping_windows_api_double.h"']
subprocess.run(['cc', *Flags, '--coverage', '-fprofile-update=atomic', '-fprofile-abs-path', '-c', 'tests/interop/mapping_windows.c', '-o', str(Attempt / 'native.o')], cwd=Root, check=True)
subprocess.run(['cc', str(Attempt / 'native.o'), '--coverage', '-o', str(Attempt / 'runner')], cwd=Root, check=True)
subprocess.run([str(Attempt / 'runner')], cwd=Root, check=True)
subprocess.run(['gcov', '--json-format', '-b', '-c', str(Attempt / 'native.gcno')], cwd=Attempt, check=True, stdout=subprocess.DEVNULL)
Report = json.loads(gzip.decompress((Attempt / 'native.gcov.json.gz').read_bytes()))
Lines = next(file['lines'] for file in Report['files'] if Path(file['file']).resolve() == Source.resolve())
Branches = [(line['line_number'], branch) for line in Lines for branch in line.get('branches', [])]
Failed = False
for kind, entries in [('line', [(line['line_number'], line) for line in Lines]), ('branch', Branches)]:
    if not entries:
        raise RuntimeError('Empty Windows raw mapping owner production ' + kind + ' scope')
    covered = sum(entry['count'] > 0 for _, entry in entries)
    percentage = 100 * covered / len(entries)
    print(f'mapping Windows owner production {kind}: {percentage:.2f}% ({covered}/{len(entries)}); evidence {Attempt}', flush=True)
    if percentage < 90:
        print('Uncovered ' + kind + ' sites: ' + str([line for line, entry in entries if entry['count'] <= 0]), flush=True)
        Failed = True
if Failed:
    raise SystemExit(1)
