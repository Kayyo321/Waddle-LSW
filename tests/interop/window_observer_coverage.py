#!/usr/bin/env python3
"""Gate exact production portable Windows observer C line/branch coverage at 90%."""
from datetime import datetime, timezone
import gzip
import hashlib
import json
from pathlib import Path
import re
import subprocess
import uuid

Root = Path(__file__).resolve().parents[2]
Source = Root / 'src/interop/window_observer_windows.c'
Fixture = Root / 'tests/interop/window_observer_windows.c'
Private = Root / 'tests/interop/window_observer_private.c'
FaultFixture = Root / 'tests/interop/window_observer_graph_faults.c'
import sys
GraphObject = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Root / 'build/window_graph/window_graph_abi.o'
Attempt = Root / 'build/coverage/interop/window_observer' / ('run-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S') + '-' + uuid.uuid4().hex[:8])
Attempt.mkdir(parents=True, exist_ok=False)
# Only source-owned __wrap_<identifier> declarations can form linker arguments.
# This keeps the fixture's private fault seams synchronized without a public test API.
WrapSymbols = sorted(set(re.findall(r'\b__wrap_([a-z_][a-z_0-9]*)\s*\(', Fixture.read_text())))
WrapFlags = ['-Wl,--wrap=' + name for name in WrapSymbols]
FaultWrapSymbols = sorted(set(re.findall(r'\b__wrap_([a-z_][a-z_0-9]*)\s*\(', FaultFixture.read_text())))
FaultWrapFlags = ['-Wl,--wrap=' + name for name in FaultWrapSymbols]
Inputs = [Path(__file__).resolve(), Source, Fixture, Private, FaultFixture, Root / 'src/interop/window_observer_windows.h', Root / 'tests/interop/window_observer_api_double.h', Root / 'include/waddle/interop_window_graph.h', Root / 'src/interop/window_graph.zig', Root / 'src/interop/window_graph_abi.zig', Root / 'impl/seamless-interop/observer_destroy_preservation.md', GraphObject]
def input_key(path):
    return str(path.relative_to(Root)) if path.is_relative_to(Root) else str(path)
Hashes = {input_key(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in Inputs}
Flags = ['-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-UNDEBUG', '-O0', '-g', '-Iinclude', '-Isrc/interop', '-pthread']
Commands = [
    ['cc', *Flags, '--coverage', '-fprofile-update=atomic', '-fprofile-abs-path', '-c', str(Private), '-o', str(Attempt / 'observer.o')],
    ['cc', *Flags, '-DWgObsWrapCalloc=1', '-DWgObsPrivateGuards=1', '-c', str(Fixture), '-o', str(Attempt / 'fixture.o')],
    ['cc', str(Attempt / 'observer.o'), str(Attempt / 'fixture.o'), str(GraphObject), '--coverage', '-pthread', *WrapFlags, '-o', str(Attempt / 'runner')],
    [str(Attempt / 'runner')],
    ['cc', *Flags, '-c', str(FaultFixture), '-o', str(Attempt / 'fault_fixture.o')],
    ['cc', str(Attempt / 'observer.o'), str(Attempt / 'fault_fixture.o'), str(GraphObject), '--coverage', '-pthread', *FaultWrapFlags, '-o', str(Attempt / 'fault_runner')],
    [str(Attempt / 'fault_runner')],
    ['gcov', '--json-format', '-b', '-c', str(Attempt / 'observer.gcno')],
]
Toolchain = {tool: subprocess.check_output([tool, '--version'], text=True).splitlines()[0] for tool in ['cc', 'gcov']}
(Attempt / 'inputs.json').write_text(json.dumps({'source_sha256': Hashes, 'commands': Commands, 'wrappers': WrapSymbols, 'fault_wrappers': FaultWrapSymbols, 'toolchain': Toolchain}, indent=2) + '\n')
for command in Commands:
    subprocess.run(command, cwd=Root if command[0] != 'gcov' else Attempt, check=True,
                   stdout=subprocess.DEVNULL if command[0] == 'gcov' else None)
if any(hashlib.sha256(p.read_bytes()).hexdigest() != Hashes[input_key(p)] for p in Inputs):
    raise RuntimeError('Qualification inputs changed during coverage attempt; no acceptance')
Report = json.loads(gzip.decompress((Attempt / 'observer.gcov.json.gz').read_bytes()))
Files = [file for file in Report['files'] if Path(file['file']).resolve() == Source.resolve()]
if len(Files) != 1:
    raise RuntimeError('Expected exactly one production portable Windows observer scope')
Lines = Files[0]['lines']
Branches = [(line['line_number'], branch) for line in Lines for branch in line.get('branches', [])]
Failed = False
Summary = {}
for kind, entries in [('line', [(line['line_number'], line) for line in Lines]), ('branch', Branches)]:
    if not entries:
        raise RuntimeError('Empty portable Windows observer production ' + kind + ' scope')
    covered = sum(entry['count'] > 0 for _, entry in entries)
    percentage = 100 * covered / len(entries)
    missed = [line for line, entry in entries if entry['count'] <= 0]
    Summary[kind] = {'covered': covered, 'total': len(entries), 'percent': percentage, 'missed': missed}
    print(f'portable Windows observer production {kind}: {percentage:.2f}% ({covered}/{len(entries)}); evidence {Attempt}', flush=True)
    if percentage < 90:
        print('Uncovered ' + kind + ' sites: ' + str(missed), flush=True)
        Failed = True
(Attempt / 'summary.json').write_text(json.dumps(Summary, indent=2) + '\n')
if Failed:
    raise SystemExit(1)
