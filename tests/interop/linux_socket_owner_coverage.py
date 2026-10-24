#!/usr/bin/env python3
"""Gate exact production Linux stream-owner C line/branch coverage at 90%."""
from datetime import datetime, timezone
import gzip
import hashlib
import json
from pathlib import Path
import re
import subprocess
import uuid

Root = Path(__file__).resolve().parents[2]
Source = Root / 'src/interop/linux_socket_owner.c'
Fixture = Root / 'tests/interop/linux_socket_owner.c'
Guards = Root / 'tests/interop/linux_socket_owner_guards.c'
Private = Root / 'tests/interop/linux_socket_owner_private.c'
Attempt = Root / 'build/coverage/interop/linux_socket_owner' / ('run-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S') + '-' + uuid.uuid4().hex[:8])
Attempt.mkdir(parents=True, exist_ok=False)
# Only source-owned __wrap_<identifier> declarations can form linker arguments.
# This keeps the fixture's private fault seams synchronized without a public test API.
WrapSymbols = sorted(set(re.findall(r'\b__wrap_([a-z_][a-z_0-9]*)\s*\(', Fixture.read_text())))
WrapFlags = ['-Wl,--wrap=' + name for name in WrapSymbols]
Inputs = [Path(__file__).resolve(), Source, Fixture, Guards, Private, Root / 'include/waddle/interop_linux_socket.h', Root / 'include/waddle/interop_peer.h', Root / 'include/waddle/interop_protocol.h', Root / 'impl/seamless-interop/linux_socket_owner.md', Root / 'build/interop_control_peer_abi.o', Root / 'build/interop_codec.o']
Hashes = {str(p.relative_to(Root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in Inputs}
Flags = ['-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-UNDEBUG', '-O0', '-g', '-Iinclude', '-pthread']
Commands = [
    # The private TU includes the exact production source, without source
    # rewrites or a production test API. Only Source is counted below.
    ['cc', *Flags, '--coverage', '-fprofile-update=atomic', '-fprofile-abs-path', '-c', str(Private), '-o', str(Attempt / 'owner.o')],
    ['cc', *Flags, '-c', str(Fixture), '-o', str(Attempt / 'fixture.o')],
    ['cc', *Flags, '-c', str(Guards), '-o', str(Attempt / 'guards.o')],
    ['cc', str(Attempt / 'owner.o'), str(Attempt / 'fixture.o'), str(Attempt / 'guards.o'), 'build/interop_control_peer_abi.o', 'build/interop_codec.o', '--coverage', '-pthread', *WrapFlags, '-o', str(Attempt / 'runner')],
    [str(Attempt / 'runner')],
    ['gcov', '--json-format', '-b', '-c', str(Attempt / 'owner.gcno')],
]
Toolchain = {tool: subprocess.check_output([tool, '--version'], text=True).splitlines()[0] for tool in ['cc', 'gcov']}
(Attempt / 'inputs.json').write_text(json.dumps({'source_sha256': Hashes, 'commands': Commands, 'wrappers': WrapSymbols, 'toolchain': Toolchain}, indent=2) + '\n')
for command in Commands:
    subprocess.run(command, cwd=Root if command[0] != 'gcov' else Attempt, check=True,
                   stdout=subprocess.DEVNULL if command[0] == 'gcov' else None)
if any(hashlib.sha256(p.read_bytes()).hexdigest() != Hashes[str(p.relative_to(Root))] for p in Inputs):
    raise RuntimeError('Qualification inputs changed during coverage attempt; no acceptance')
Report = json.loads(gzip.decompress((Attempt / 'owner.gcov.json.gz').read_bytes()))
Files = [file for file in Report['files'] if Path(file['file']).resolve() == Source.resolve()]
if len(Files) != 1:
    raise RuntimeError('Expected exactly one production Linux socket-owner scope')
Lines = Files[0]['lines']
Branches = [(line['line_number'], branch) for line in Lines for branch in line.get('branches', [])]
Failed = False
Summary = {}
for kind, entries in [('line', [(line['line_number'], line) for line in Lines]), ('branch', Branches)]:
    if not entries:
        raise RuntimeError('Empty Linux socket-owner production ' + kind + ' scope')
    covered = sum(entry['count'] > 0 for _, entry in entries)
    percentage = 100 * covered / len(entries)
    missed = [line for line, entry in entries if entry['count'] <= 0]
    Summary[kind] = {'covered': covered, 'total': len(entries), 'percent': percentage, 'missed': missed}
    print(f'Linux socket-owner production {kind}: {percentage:.2f}% ({covered}/{len(entries)}); evidence {Attempt}', flush=True)
    if percentage < 90:
        print('Uncovered ' + kind + ' sites: ' + str(missed), flush=True)
        Failed = True
(Attempt / 'summary.json').write_text(json.dumps(Summary, indent=2) + '\n')
if Failed:
    raise SystemExit(1)
