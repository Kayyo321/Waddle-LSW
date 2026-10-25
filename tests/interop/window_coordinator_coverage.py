#!/usr/bin/env python3
"""Gate gcov line and branch coverage of the window coordinator at 90 percent.

The coordinator source is compiled with --coverage and driven by the coordinator suite
(tests/interop/window_coordinator_suite.c), which links the real graph, slot-runtime, codec and
reducer Zig ABI objects built here with ReleaseSafe and not instrumented. The observer and the
window-state adapter are linked uninstrumented as the suite's collaborators. The Windows branches
are preprocessed out on Linux; the Windows-only pieces are covered by the mingw object build.
Run from the worktree root: python3 tests/interop/window_coordinator_coverage.py
"""
from datetime import datetime, timezone
import gzip
import json
from pathlib import Path
import subprocess
import sys
import uuid

Root = Path(__file__).resolve().parents[2]
Source = Root / 'src/interop/window_coordinator.c'
Suite = Root / 'tests/interop/window_coordinator_suite.c'
Observer = Root / 'src/interop/window_observer_windows.c'
Adapter = Root / 'src/interop/window_state_adapter_windows.c'
Attempt = Root / 'build/coverage/interop/window_coordinator' / (
    'run-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S') + '-' + uuid.uuid4().hex[:8])
Attempt.mkdir(parents=True, exist_ok=False)
# Seam wrappers of tests/interop/window_coordinator_faults.h, listed once in window_coordinator_wraps.txt.
Wraps = ['-Wl,--wrap=' + line.strip() for line in (Root / 'tests/interop/window_coordinator_wraps.txt').read_text().splitlines()
         if line.strip() and not line.startswith('#')]
Zig = ['zig', 'build-obj']
ZigFlags = ['-Iinclude', '-Isrc/interop', '-O', 'ReleaseSafe', '-fPIC', '-fcompiler-rt', '-lc']
Flags = ['-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-Wno-unused-function', '-UNDEBUG',
         '-O0', '-g', '-pthread', '-Iinclude', '-Isrc/interop', '-Itests/interop']
ZigObjects = {
    'graph.o': 'src/interop/window_graph_abi.zig',
    'slot.o': 'src/interop/interop_slot_runtime_abi.zig',
    'wstate.o': 'src/interop/window_state_abi.zig',
    'codec.o': 'src/interop/interop_codec.zig',
}
Commands = [
    *[[*Zig, str(Root / path), *ZigFlags, f'-femit-bin={Attempt / name}'] for name, path in ZigObjects.items()],
    ['cc', *Flags, '--coverage', '-fprofile-update=atomic', '-fprofile-abs-path', '-c', str(Source),
     '-o', str(Attempt / 'coordinator.o')],
    ['cc', *Flags, '--coverage', '-c', str(Suite), '-o', str(Attempt / 'suite.o')],
    ['cc', *Flags, '-c', str(Observer), '-o', str(Attempt / 'observer.o')],
    ['cc', *Flags, '-c', str(Adapter), '-o', str(Attempt / 'adapter.o')],
    ['cc', str(Attempt / 'coordinator.o'), str(Attempt / 'suite.o'), str(Attempt / 'observer.o'),
     str(Attempt / 'adapter.o'), *[str(Attempt / name) for name in ZigObjects], '--coverage', '-pthread',
     *Wraps, '-o', str(Attempt / 'runner')],
    [str(Attempt / 'runner')],
    ['gcov', '--json-format', '-b', '-c', str(Attempt / 'coordinator.gcno')],
]
for command in Commands:
    subprocess.run(command, cwd=Root if command[0] not in ('gcov',) else Attempt, check=True,
                   stdout=subprocess.DEVNULL if command[0] == 'gcov' else None)
Report = json.loads(gzip.decompress((Attempt / 'coordinator.gcov.json.gz').read_bytes()))
Files = [entry for entry in Report['files'] if Path(entry['file']).resolve() == Source.resolve()]
if len(Files) != 1:
    raise RuntimeError('Expected exactly one window coordinator scope in the gcov report')
Lines = Files[0]['lines']
Branches = [(line['line_number'], branch) for line in Lines for branch in line.get('branches', [])]
Failed = False
Summary = {}
for kind, entries in [('line', [(line['line_number'], line) for line in Lines]), ('branch', Branches)]:
    if not entries:
        raise RuntimeError('Empty window coordinator ' + kind + ' scope')
    covered = sum(entry['count'] > 0 for _, entry in entries)
    percentage = 100 * covered / len(entries)
    missed = [line for line, entry in entries if entry['count'] <= 0]
    Summary[kind] = {'covered': covered, 'total': len(entries), 'percent': percentage, 'missed': missed}
    print(f'window coordinator {kind}: {percentage:.2f}% ({covered}/{len(entries)}); evidence {Attempt}',
          flush=True)
    if percentage < 90:
        print('Uncovered ' + kind + ' sites: ' + str(missed), flush=True)
        Failed = True
(Attempt / 'summary.json').write_text(json.dumps(Summary, indent=2) + '\n')
if Failed:
    sys.exit(1)
