#!/usr/bin/env python3
"""Gate gcov line and branch coverage of the portable window-state adapter at 90 percent.

The adapter source is compiled with --coverage and run under the double suite
(tests/interop/window_state_adapter_suite.c). The _WIN32 block is preprocessed out on Linux,
so only the portable logic is measured; the Win32 table is covered by the Windows smoke.
Run from the worktree root: python3 tests/interop/window_state_adapter_coverage.py
"""
from datetime import datetime, timezone
import gzip
import json
from pathlib import Path
import subprocess
import sys
import uuid

Root = Path(__file__).resolve().parents[2]
Source = Root / 'src/interop/window_state_adapter_windows.c'
Suite = Root / 'tests/interop/window_state_adapter_suite.c'
Attempt = Root / 'build/coverage/interop/window_state_adapter' / (
    'run-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S') + '-' + uuid.uuid4().hex[:8])
Attempt.mkdir(parents=True, exist_ok=False)
Flags = ['-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-UNDEBUG', '-O0', '-g',
         '-Iinclude', '-Isrc/interop', '-Itests/interop']
Commands = [
    ['cc', *Flags, '--coverage', '-fprofile-update=atomic', '-fprofile-abs-path', '-c', str(Source),
     '-o', str(Attempt / 'adapter.o')],
    ['cc', *Flags, '--coverage', '-c', str(Suite), '-o', str(Attempt / 'suite.o')],
    ['cc', str(Attempt / 'adapter.o'), str(Attempt / 'suite.o'), '--coverage', '-Wl,--wrap=calloc',
     '-o', str(Attempt / 'runner')],
    [str(Attempt / 'runner')],
    ['gcov', '--json-format', '-b', '-c', str(Attempt / 'adapter.gcno')],
]
for command in Commands:
    subprocess.run(command, cwd=Root if command[0] != 'gcov' else Attempt, check=True,
                   stdout=subprocess.DEVNULL if command[0] == 'gcov' else None)
Report = json.loads(gzip.decompress((Attempt / 'adapter.gcov.json.gz').read_bytes()))
Files = [entry for entry in Report['files'] if Path(entry['file']).resolve() == Source.resolve()]
if len(Files) != 1:
    raise RuntimeError('Expected exactly one portable window-state adapter scope in the gcov report')
Lines = Files[0]['lines']
Branches = [(line['line_number'], branch) for line in Lines for branch in line.get('branches', [])]
Failed = False
Summary = {}
for kind, entries in [('line', [(line['line_number'], line) for line in Lines]), ('branch', Branches)]:
    if not entries:
        raise RuntimeError('Empty portable window-state adapter ' + kind + ' scope')
    covered = sum(entry['count'] > 0 for _, entry in entries)
    percentage = 100 * covered / len(entries)
    missed = [line for line, entry in entries if entry['count'] <= 0]
    Summary[kind] = {'covered': covered, 'total': len(entries), 'percent': percentage, 'missed': missed}
    print(f'window-state adapter portable {kind}: {percentage:.2f}% ({covered}/{len(entries)}); evidence {Attempt}',
          flush=True)
    if percentage < 90:
        print('Uncovered ' + kind + ' sites: ' + str(missed), flush=True)
        Failed = True
(Attempt / 'summary.json').write_text(json.dumps(Summary, indent=2) + '\n')
if Failed:
    sys.exit(1)
