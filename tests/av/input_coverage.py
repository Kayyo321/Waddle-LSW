#!/usr/bin/env python3
"""Require 90% real line/branch coverage of portable input state, preserve runs."""
from datetime import datetime, timezone
import gzip
import json
import os
from pathlib import Path
import subprocess

Root = Path.cwd()
Output = Root / 'build/coverage/av/input' / (
    'run-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ') + '-' + str(os.getpid()))
Output.mkdir(parents=True, exist_ok=False)
Flags = ['-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-Iinclude', '-Isrc/av', '-O0', '-g']
subprocess.run(['cc', *Flags, '--coverage', '-fprofile-abs-path', '-c',
                'src/av/av_input.c', '-o', str(Output / 'input.o')], check=True)
subprocess.run(['cc', *Flags, 'tests/av/input.c', str(Output / 'input.o'),
                'build/av_codec.o', '--coverage', '-o', str(Output / 'runner')], check=True)
subprocess.run([str(Output / 'runner')], check=True)
subprocess.run(['gcov', '--json-format', '-b', '-c', str(Output / 'input.gcno')],
               cwd=Output, check=True)
Report = json.loads(gzip.decompress((Output / 'input.gcov.json.gz').read_bytes()))
Lines = next(item['lines'] for item in Report['files'] if item['file'].endswith('/src/av/av_input.c'))
Branches = [branch for line in Lines for branch in line.get('branches', [])]
Summary = {}
for kind, entries in [('line', Lines), ('branch', Branches)]:
    covered = sum(entry['count'] > 0 for entry in entries)
    percent = 100 * covered / len(entries)
    Summary[kind] = {'covered': covered, 'total': len(entries), 'percent': percent}
    print(f'av_input production {kind} coverage: {percent:.2f}% ({covered}/{len(entries)})', flush=True)
(Output / 'summary.json').write_text(json.dumps(Summary, indent=2) + '\n')
assert all(item['percent'] >= 90 for item in Summary.values()), Summary
