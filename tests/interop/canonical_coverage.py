#!/usr/bin/env python3
"""Gate every new filesystem-aware path function, without legacy quoting dilution."""
from datetime import datetime, timezone
import gzip
import json
from pathlib import Path
import subprocess
import uuid

Root = Path(__file__).resolve().parents[2]
Source = Root / 'src/common/path_filesystem.c'
Functions = {'normalize_absolute_path', 'validate_root_nofollow', 'waddle_path_roots_validate',
             'waddle_path_roots_free', 'resolve_absolute_input', 'waddle_translate_filesystem_rules'}
Attempt = Root / 'build/coverage/interop/canonical_paths' / ('run-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S') + '-' + uuid.uuid4().hex[:8])
Attempt.mkdir(parents=True, exist_ok=False)
Flags = ['-D_GNU_SOURCE', '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-O0', '-g', '-Iinclude', '-Isrc/common']
Wrappers = '-Wl,--wrap=malloc,--wrap=strdup,--wrap=strndup,--wrap=realpath,--wrap=open,--wrap=fstatat,--wrap=openat,--wrap=lstat'
subprocess.run(['cc', *Flags, '--coverage', '-fprofile-update=atomic', '-fprofile-abs-path', '-c', str(Source), '-o', str(Attempt / 'arguments.o')], cwd=Root, check=True)
subprocess.run(['cc', *Flags, 'tests/interop/canonical_paths.c', 'src/common/arguments.c', str(Attempt / 'arguments.o'), 'build/path_rules.o', '--coverage', Wrappers, '-o', str(Attempt / 'runner')], cwd=Root, check=True)
subprocess.run([str(Attempt / 'runner')], cwd=Root, check=True)
subprocess.run(['gcov', '--json-format', '-b', '-c', str(Attempt / 'arguments.gcno')], cwd=Attempt, check=True, stdout=subprocess.DEVNULL)
Report = json.loads(gzip.decompress((Attempt / 'arguments.gcov.json.gz').read_bytes()))
Lines = next(file['lines'] for file in Report['files'] if Path(file['file']).resolve() == Source.resolve())
ObservedFunctions = {line['function_name'] for line in Lines}
if ObservedFunctions != Functions:
    raise RuntimeError('Incomplete canonical production scope: ' + str(Functions - ObservedFunctions))
Branches = [(line['line_number'], branch) for line in Lines for branch in line.get('branches', [])]
Failed = False
for kind, entries in [('line', [(line['line_number'], line) for line in Lines]), ('branch', Branches)]:
    if not entries:
        raise RuntimeError('Empty canonical production ' + kind + ' scope')
    covered = sum(entry['count'] > 0 for _, entry in entries)
    percentage = 100 * covered / len(entries)
    print(f'canonical path production {kind}: {percentage:.2f}% ({covered}/{len(entries)}); evidence {Attempt}', flush=True)
    if percentage < 90:
        print('Uncovered ' + kind + ' sites: ' + str([line for line, entry in entries if entry['count'] <= 0]), flush=True)
        Failed = True
if Failed:
    raise SystemExit(1)
