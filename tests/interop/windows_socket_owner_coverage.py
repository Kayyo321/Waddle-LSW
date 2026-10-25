#!/usr/bin/env python3
"""Retained sanitizer and 90% line/branch coverage gate for the Windows socket owner."""
from datetime import datetime, timezone
import gzip
import hashlib
import json
from pathlib import Path
import re
import subprocess
import uuid

Root = Path(__file__).resolve().parents[2]
Source = Root / 'src/interop/windows_socket_owner.c'
Header = Root / 'src/interop/windows_socket_owner.h'
Fixture = Root / 'tests/interop/windows_socket_owner.c'
Guards = Root / 'tests/interop/windows_socket_owner_guards.c'
Private = Root / 'tests/interop/windows_socket_owner_private.c'
Adapter = Root / 'tests/interop/windows_socket_owner_api_double.c'
AdapterHeader = Root / 'tests/interop/windows_socket_owner_api_double.h'
Contract = Root / 'impl/seamless-interop/windows_socket_owner_contract.md'
Attempt = Root / 'build/coverage/interop/windows_socket_owner' / (
    'run-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S') + '-' + uuid.uuid4().hex[:8])
Attempt.mkdir(parents=True, exist_ok=False)
WrapSymbols = sorted(set(re.findall(r'\b__wrap_([a-z_][a-z_0-9]*)\s*\(', Fixture.read_text())))
WrapFlags = ['-Wl,--wrap=' + name for name in WrapSymbols]
PeerWraps = sorted(name for name in WrapSymbols if name.startswith('interop_peer_'))
Inputs = [Path(__file__).resolve(), Source, Header, Fixture, Guards, Private, Adapter,
          AdapterHeader, Contract, Root / 'include/waddle/interop_peer.h',
          Root / 'include/waddle/interop_protocol.h', Root / 'build/interop_control_peer_abi.o',
          Root / 'build/interop_codec.o']
Hashes = {str(path.relative_to(Root)): hashlib.sha256(path.read_bytes()).hexdigest()
          for path in Inputs}
Flags = ['-DInteropWindowsSocketTestApi', '-std=c11', '-Wall', '-Wextra', '-Wpedantic',
         '-Werror', '-UNDEBUG', '-O0', '-g', '-Itests/interop', '-Iinclude',
         '-Isrc/interop', '-pthread']
Commands = [
    ['cc', *Flags, '--coverage', '-fprofile-update=atomic', '-fprofile-abs-path',
     '-c', str(Private), '-o', str(Attempt / 'owner.o')],
    ['cc', *Flags, '-c', str(Fixture), '-o', str(Attempt / 'fixture.o')],
    ['cc', *Flags, '-c', str(Guards), '-o', str(Attempt / 'guards.o')],
    ['cc', *Flags, '-c', str(Adapter), '-o', str(Attempt / 'adapter.o')],
    ['cc', str(Attempt / 'owner.o'), str(Attempt / 'fixture.o'), str(Attempt / 'guards.o'),
     str(Attempt / 'adapter.o'), 'build/interop_control_peer_abi.o', 'build/interop_codec.o',
     '--coverage', '-pthread', *WrapFlags, '-o', str(Attempt / 'coverage_runner')],
    [str(Attempt / 'coverage_runner')],
    ['gcov', '--json-format', '-b', '-c', str(Attempt / 'owner.gcno')],
]
SanitizerFlags = ['-fsanitize=address,leak,undefined', '-fno-omit-frame-pointer']
SanitizerCommands = [
    ['cc', *Flags, *SanitizerFlags, '-c', str(Private), '-o', str(Attempt / 'san_owner.o')],
    ['cc', *Flags, *SanitizerFlags, '-c', str(Fixture), '-o', str(Attempt / 'san_fixture.o')],
    ['cc', *Flags, *SanitizerFlags, '-c', str(Guards), '-o', str(Attempt / 'san_guards.o')],
    ['cc', *Flags, *SanitizerFlags, '-c', str(Adapter), '-o', str(Attempt / 'san_adapter.o')],
    ['cc', str(Attempt / 'san_owner.o'), str(Attempt / 'san_fixture.o'),
     str(Attempt / 'san_guards.o'), str(Attempt / 'san_adapter.o'),
     'build/interop_control_peer_abi.o', 'build/interop_codec.o', *SanitizerFlags,
     '-pthread', *WrapFlags, '-o', str(Attempt / 'sanitizer_runner')],
    [str(Attempt / 'sanitizer_runner')],
]
Metadata = {
    'source_sha256': Hashes,
    'coverage_commands': Commands,
    'sanitizer_commands': SanitizerCommands,
    'wrappers': WrapSymbols,
    'peer_wrappers': PeerWraps,
    'coverage_threshold_percent': 90,
    'sanitizers': ['address', 'leak', 'undefined'],
    'sanitizer_environment': {'ASAN_OPTIONS': 'detect_leaks=1:abort_on_error=1',
                              'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'},
    'toolchain': {tool: subprocess.check_output([tool, '--version'], text=True).splitlines()[0]
                  for tool in ['cc', 'gcov']},
}
(Attempt / 'inputs.json').write_text(json.dumps(Metadata, indent=2) + '\n')

def run(command, name, cwd=Root, env=None):
    result = subprocess.run(command, cwd=cwd, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (Attempt / name).write_text(result.stdout)
    if result.returncode != 0:
        (Attempt / 'failure.json').write_text(json.dumps(
            {'command': command, 'returncode': result.returncode, 'log': name}, indent=2) + '\n')
        raise SystemExit(f'{name} failed; retained evidence: {Attempt}')

try:
    for index, command in enumerate(Commands):
        if command[0] == 'gcov':
            run(command, 'gcov.log', Attempt)
        elif command[0] == str(Attempt / 'coverage_runner'):
            run(command, 'coverage_test.log')
        else:
            run(command, f'coverage_build_{index}.log')
    if any(hashlib.sha256(path.read_bytes()).hexdigest() != Hashes[str(path.relative_to(Root))]
           for path in Inputs):
        raise RuntimeError('Qualification inputs changed during coverage attempt')
    ReportPath = Attempt / 'owner.gcov.json.gz'
    Report = json.loads(gzip.decompress(ReportPath.read_bytes()))
    Scope = [item for item in Report['files']
             if Path(item['file']).resolve() == Source.resolve()]
    if len(Scope) != 1:
        raise RuntimeError('Expected one production Windows socket-owner coverage scope')
    Lines = Scope[0]['lines']
    Branches = [(line['line_number'], branch) for line in Lines
                for branch in line.get('branches', [])]
    Summary = {}
    failed = False
    for kind, entries in [('line', [(line['line_number'], line) for line in Lines]),
                          ('branch', Branches)]:
        if not entries:
            raise RuntimeError('Empty production coverage scope: ' + kind)
        covered = sum(entry['count'] > 0 for _, entry in entries)
        percentage = 100 * covered / len(entries)
        missed = [line for line, entry in entries if entry['count'] <= 0]
        Summary[kind] = {'covered': covered, 'total': len(entries),
                         'percent': percentage, 'missed': missed}
        print(f'Windows socket owner {kind}: {percentage:.2f}% ({covered}/{len(entries)}); '
              f'evidence {Attempt}', flush=True)
        failed |= percentage < 90
    (Attempt / 'summary.json').write_text(json.dumps(Summary, indent=2) + '\n')
    SanitizerEnvironment = dict(__import__('os').environ)
    SanitizerEnvironment['ASAN_OPTIONS'] = 'detect_leaks=1:abort_on_error=1'
    SanitizerEnvironment['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
    for index, command in enumerate(SanitizerCommands):
        if command[0] == str(Attempt / 'sanitizer_runner'):
            run(command, 'sanitizer_test.log', env=SanitizerEnvironment)
        else:
            run(command, f'sanitizer_build_{index}.log')
    if failed:
        raise SystemExit(1)
except BaseException as error:
    if not (Attempt / 'failure.json').exists():
        (Attempt / 'failure.json').write_text(json.dumps(
            {'error': repr(error)}, indent=2) + '\n')
    raise
