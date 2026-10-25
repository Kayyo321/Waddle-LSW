#!/usr/bin/env python3
"""Gate gcov line and branch coverage of the portable guest launch core at 90 percent.

src/interop/guest_launch.c is compiled with --coverage and driven by the Linux suite
(tests/interop/guest_launch_suite.c) over the native double and the real interop codec object.
The Win32 table (guest_launch_windows.c) is not part of this gate; it runs only on Windows.
Run from the worktree root: python3 tests/interop/guest_launch_coverage.py
"""
from datetime import datetime, timezone
import gzip
import json
from pathlib import Path
import subprocess
import sys
import uuid

Root = Path(__file__).resolve().parents[2]
Source = Root / 'src/interop/guest_launch.c'
Suite = Root / 'tests/interop/guest_launch_suite.c'
Attempt = Root / 'build/coverage/interop/guest_launch' / (
    'run-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S') + '-' + uuid.uuid4().hex[:8])
Attempt.mkdir(parents=True, exist_ok=False)
Flags = ['-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-Wno-unused-function', '-UNDEBUG',
         '-O0', '-g', '-pthread', '-Iinclude', '-Isrc/interop', '-Itests/interop']
Commands = [
    ['zig', 'build-obj', str(Root / 'src/interop/interop_codec.zig'), '-Iinclude', '-O', 'ReleaseSafe', '-fPIC',
     '-fcompiler-rt', '-lc', f'-femit-bin={Attempt / "codec.o"}'],
    ['cc', *Flags, '--coverage', '-fprofile-update=atomic', '-fprofile-abs-path', '-c', str(Source),
     '-o', str(Attempt / 'guest_launch.o')],
    ['cc', *Flags, '--coverage', '-c', str(Suite), '-o', str(Attempt / 'suite.o')],
    ['cc', str(Attempt / 'guest_launch.o'), str(Attempt / 'suite.o'), str(Attempt / 'codec.o'), '--coverage',
     '-pthread', '-o', str(Attempt / 'runner')],
    [str(Attempt / 'runner')],
    ['gcov', '--json-format', '-b', '-c', str(Attempt / 'guest_launch.gcno')],
]
for command in Commands:
    subprocess.run(command, cwd=Root if command[0] not in ('gcov',) else Attempt, check=True,
                   stdout=subprocess.DEVNULL if command[0] == 'gcov' else None)
Report = json.loads(gzip.decompress((Attempt / 'guest_launch.gcov.json.gz').read_bytes()))
Files = [entry for entry in Report['files'] if Path(entry['file']).resolve() == Source.resolve()]
if len(Files) != 1:
    raise RuntimeError('Expected exactly one guest launch scope in the gcov report')
Lines = Files[0]['lines']
Branches = [(line['line_number'], branch) for line in Lines for branch in line.get('branches', [])]
Failed = False
Summary = {}
for kind, entries in [('line', [(line['line_number'], line) for line in Lines]), ('branch', Branches)]:
    if not entries:
        raise RuntimeError('Empty guest launch ' + kind + ' scope')
    covered = sum(entry['count'] > 0 for _, entry in entries)
    percentage = 100 * covered / len(entries)
    missed = [line for line, entry in entries if entry['count'] <= 0]
    Summary[kind] = {'covered': covered, 'total': len(entries), 'percent': percentage, 'missed': missed}
    print(f'guest launch {kind}: {percentage:.2f}% ({covered}/{len(entries)}); evidence {Attempt}', flush=True)
    if percentage < 90:
        print('Uncovered ' + kind + ' sites: ' + str(missed), flush=True)
        Failed = True
(Attempt / 'summary.json').write_text(json.dumps(Summary, indent=2) + '\n')
if Failed:
    sys.exit(1)
