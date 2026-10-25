#!/usr/bin/env python3
"""Require real AV input line/branch coverage and run leak-enabled sanitizers."""
from datetime import datetime, timezone
import hashlib
import gzip
import json
import os
from pathlib import Path
import subprocess

root = Path.cwd()
output_root = Path('/tmp/waddle-keyboard-scan-coverage')
output = output_root / ('run-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ') + '-' + str(os.getpid()))
output.mkdir(parents=True, exist_ok=False)
base_flags = ['-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-Iinclude', '-Isrc/av', '-O0', '-g']
InputPaths = [
    'src/av/av_input.c', 'src/av/av_input.h', 'include/waddle/av_protocol.h',
    'tests/av/input.c', 'tests/interop/keyboard_scan.c', 'tests/interop/keyboard_scan_coverage.py',
    'tests/interop/scancodes.csv', 'impl/seamless-interop/keyboard_scan_contract.md', 'build/av_codec.o',
]
commands = []


def file_hashes():
    return {path: hashlib.sha256((root / path).read_bytes()).hexdigest() for path in InputPaths}


def run(command, **options):
    item = {'argv': command, 'cwd': str(options.get('cwd', root))}
    if 'env' in options:
        item['env_overrides'] = {key: options['env'][key] for key in ('ASAN_OPTIONS', 'UBSAN_OPTIONS') if key in options['env']}
    commands.append(item)
    return subprocess.run(command, **options)


if not (root / 'build/av_codec.o').is_file():
    raise SystemExit('build/av_codec.o is required; build only that existing AV codec object first')
input_hashes_before = file_hashes()
compiler_versions = {}
for compiler in ('cc', 'gcov'):
    result = run([compiler, '--version'], cwd=root, check=True, text=True, capture_output=True)
    compiler_versions[compiler] = result.stdout.splitlines()[0]

for source, object_name, extra in [
    ('src/av/av_input.c', 'input.o', []),
    ('tests/av/input.c', 'legacy_input.o', ['-Dmain=av_input_legacy_test_main']),
    ('tests/interop/keyboard_scan.c', 'keyboard_scan.o', []),
]:
    run(['cc', *base_flags, '--coverage', '-fprofile-abs-path', *extra, '-c', source,
         '-o', str(output / object_name)], cwd=root, check=True)
run(['cc', *base_flags, '--coverage', str(output / 'input.o'),
     str(output / 'legacy_input.o'), str(output / 'keyboard_scan.o'),
     'build/av_codec.o', '-o', str(output / 'runner')], cwd=root, check=True)
run([str(output / 'runner')], cwd=root, check=True)
run(['gcov', '--json-format', '-b', '-c', str(output / 'input.gcno')], cwd=output, check=True)
report = json.loads(gzip.decompress((output / 'input.gcov.json.gz').read_bytes()))
lines = next(item['lines'] for item in report['files'] if item['file'].endswith('/src/av/av_input.c'))
branches = [branch for line in lines for branch in line.get('branches', [])]
summary = {}
for kind, entries in [('line', lines), ('branch', branches)]:
    covered = sum(entry['count'] > 0 for entry in entries)
    percent = 100 * covered / len(entries)
    summary[kind] = {'covered': covered, 'total': len(entries), 'percent': percent}
    print(f'av_input production {kind} coverage: {percent:.2f}% ({covered}/{len(entries)})', flush=True)
(output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
assert all(item['percent'] >= 90 for item in summary.values()), summary

sanitizer_flags = ['-fsanitize=address,leak,undefined', '-fno-omit-frame-pointer']
for source, object_name, extra in [
    ('src/av/av_input.c', 'input_sanitized.o', []),
    ('tests/av/input.c', 'legacy_input_sanitized.o', ['-Dmain=av_input_legacy_test_main']),
    ('tests/interop/keyboard_scan.c', 'keyboard_scan_sanitized.o', []),
]:
    run(['cc', *base_flags, *sanitizer_flags, *extra, '-c', source,
         '-o', str(output / object_name)], cwd=root, check=True)
run(['cc', *base_flags, *sanitizer_flags, str(output / 'input_sanitized.o'),
     str(output / 'legacy_input_sanitized.o'), str(output / 'keyboard_scan_sanitized.o'),
     'build/av_codec.o', '-o', str(output / 'runner_sanitized')], cwd=root, check=True)
environment = os.environ.copy()
environment['ASAN_OPTIONS'] = 'detect_leaks=1:halt_on_error=1'
environment['UBSAN_OPTIONS'] = 'halt_on_error=1'
run([str(output / 'runner_sanitized')], cwd=root, env=environment, check=True)
input_hashes_after = file_hashes()
unchanged = input_hashes_before == input_hashes_after
qualification = {
    'compiler_versions': compiler_versions,
    'commands': commands,
    'inputs_before_sha256': input_hashes_before,
    'inputs_after_sha256': input_hashes_after,
    'inputs_unchanged_during_run': unchanged,
    'coverage': summary,
    'address_leak_undefined_sanitizers_passed': True,
}
(output / 'qualification.json').write_text(json.dumps(qualification, indent=2) + '\n')
assert unchanged, 'source input changed during coverage/sanitizer qualification'
print(f'Preserved keyboard scan coverage and sanitizer evidence: {output}', flush=True)
