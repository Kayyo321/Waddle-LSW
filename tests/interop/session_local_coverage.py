#!/usr/bin/env python3
"""Gate line and taken-branch coverage of the session command handler at 90 percent.

The unit and socket suite includes src/interop/session_local_handler.c, so one instrumented
build of tests/interop/session_local_handler_test.c measures the production handler exactly as
the suite runs it. Each run keeps its own evidence directory under build/coverage/interop.
"""
from datetime import datetime, timezone
import gzip
import json
from pathlib import Path
import subprocess
import uuid

Root = Path(__file__).resolve().parents[2]
Source = Root / 'src/interop/session_local_handler.c'
Test = Root / 'tests/interop/session_local_handler_test.c'
Attempt = Root / 'build/coverage/interop/session_local' / ('run-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S') + '-' + uuid.uuid4().hex[:8])
Attempt.mkdir(parents=True, exist_ok=False)
Flags = ['-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-D_GNU_SOURCE', '-UNDEBUG', '-O0', '-g',
         '-Iinclude', '-Isrc/interop', '-Isrc/daemon', '-Isrc/common']
subprocess.run(['cc', *Flags, '--coverage', '-fprofile-abs-path', '-c', str(Test), '-o', str(Attempt / 'suite.o')],
               cwd=Root, check=True)
# The same fault-injection wrappers the suite links with (scripts/interop_lane_launch.mk).
Wraps = ['socket', 'bind', 'listen', 'mkdir', 'lstat', 'unlink', 'send', 'recv', 'accept4', 'getsockopt',
         'epoll_create1', 'epoll_ctl', 'epoll_wait']
subprocess.run(['cc', '--coverage', str(Attempt / 'suite.o'), str(Root / 'build/interop_codec.o'), str(Root / 'build/interop_activation_store.o'),
                *[f'-Wl,--wrap={name}' for name in Wraps], '-o', str(Attempt / 'runner')], cwd=Root, check=True)
subprocess.run([str(Attempt / 'runner')], cwd=Root, check=True, stdout=subprocess.DEVNULL)
subprocess.run(['gcov', '--json-format', '-b', '-c', str(Attempt / 'suite.gcno')], cwd=Attempt, check=True,
               stdout=subprocess.DEVNULL)
Report = json.loads(gzip.decompress((Attempt / 'suite.gcov.json.gz').read_bytes()))
Lines = next(file['lines'] for file in Report['files'] if Path(file['file']).resolve() == Source.resolve())
Branches = [(line['line_number'], branch) for line in Lines for branch in line.get('branches', [])]
Failed = False
for kind, entries in [('line', [(line['line_number'], line) for line in Lines]), ('branch', Branches)]:
    covered = sum(entry['count'] > 0 for _, entry in entries)
    percentage = 100 * covered / len(entries)
    print(f'session local handler production {kind}: {percentage:.2f}% ({covered}/{len(entries)}); evidence {Attempt}', flush=True)
    if percentage < 90:
        print('Uncovered ' + kind + ' sites: ' + str([line for line, entry in entries if entry['count'] <= 0]), flush=True)
        Failed = True
if Failed:
    raise SystemExit(1)
