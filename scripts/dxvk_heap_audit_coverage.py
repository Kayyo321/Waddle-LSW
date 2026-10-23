#!/usr/bin/env python3
"""Sanitize and cover the actual DXVK native heap audit-stage include.

The portable runner includes the production C source against deterministic Win32
file/time doubles. Every source line and branch remains in the coverage denominator.
"""
from datetime import datetime, timezone
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

MinimumPercent = 90
ProductionFunctions = frozenset(('audit_directory', 'audit_stage'))


def coverage_summary(report, source):
    """[in] Borrow gcov JSON and canonical included source; [out] coverage summary.

    Reject absent/duplicate/malformed files, functions, lines, branches, zero
    denominators, unexecuted functions, or either metric below the fixed 90% floor.
    No source mutation or coverage exclusions are accepted.
    """
    if not isinstance(report, dict) or not isinstance(report.get('files'), list):
        raise ValueError('missing gcov source array')
    files = [entry for entry in report['files']
             if isinstance(entry, dict) and entry.get('file') == str(source)]
    if len(files) != 1:
        raise ValueError('expected exactly one actual audit-stage source')
    functions = files[0].get('functions')
    if not isinstance(functions, list) or len(functions) != len(ProductionFunctions):
        raise ValueError('production function inventory missing or duplicated')
    names = set()
    for function in functions:
        if not isinstance(function, dict):
            raise ValueError('invalid production function record')
        name, count = function.get('name'), function.get('execution_count')
        if (not isinstance(name, str) or name not in ProductionFunctions or name in names or
                type(count) is not int or count <= 0):
            raise ValueError('wrong, duplicate, or unexecuted production function')
        names.add(name)
    if names != set(ProductionFunctions):
        raise ValueError('incomplete production function inventory')
    lines = files[0].get('lines')
    if not isinstance(lines, list) or not lines:
        raise ValueError('empty production line denominator')
    line_counts, branch_counts, numbers = [], [], set()
    for line in lines:
        if not isinstance(line, dict):
            raise ValueError('invalid production line')
        number, count, branches = line.get('line_number'), line.get('count'), line.get('branches')
        if type(number) is not int or number <= 0 or number in numbers:
            raise ValueError('invalid or duplicate production line identity')
        if type(count) is not int or count < 0 or not isinstance(branches, list):
            raise ValueError('invalid production line count or branches')
        numbers.add(number)
        line_counts.append(count)
        for branch in branches:
            branch_count = branch.get('count') if isinstance(branch, dict) else None
            if type(branch_count) is not int or branch_count < 0:
                raise ValueError('invalid production branch outcome')
            branch_counts.append(branch_count)
    if not branch_counts:
        raise ValueError('empty production branch denominator')
    summary = {'functions_executed': len(names), 'minimum_percent': MinimumPercent}
    for label, counts in (('lines', line_counts), ('branches', branch_counts)):
        hit, total = sum(count > 0 for count in counts), len(counts)
        summary[label] = {'covered': hit, 'total': total, 'percent': 100 * hit / total}
        if hit * 100 < MinimumPercent * total:
            raise ValueError('actual production ' + label + ' below 90%: ' + str(hit) + '/' + str(total))
    return summary


def main():
    """[in] Optional output-root; [out] retain compile/run/coverage proof or fail.

    Synchronous bounded compiler, test and gcov children own no persistent state.
    The production include and harness remain read-only; all artifacts are unique.
    """
    arguments = sys.argv[1:]
    if len(arguments) > 1:
        raise SystemExit('usage: dxvk_heap_audit_coverage.py [output-root]')
    root = Path(__file__).resolve().parents[1]
    base = Path(arguments[0]) if arguments else root / 'build/coverage/vgpu/dxvk_heap_audit'
    output = base.resolve() / ('run-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ') + '-' + str(os.getpid()))
    output.mkdir(parents=True, exist_ok=False)
    source = root / 'tests/vgpu/dxvk_heap_audit_windows.inc'
    test = root / 'tests/vgpu/dxvk_heap_audit_test.c'
    inputs = [source, test, Path(__file__).resolve()]
    tools = {}
    for name in ('cc', 'gcov'):
        found = shutil.which(name)
        if not found:
            raise SystemExit('missing coverage tool: ' + name)
        tools[name] = Path(found).resolve(strict=True)
        inputs.append(tools[name])

    def hashes():
        return {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}

    manifest = hashes()
    compiler = [str(tools['cc']), '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                '-O0', '-g', '--coverage', '-fprofile-update=atomic', '-fprofile-abs-path',
                '-fsanitize=address,leak,undefined', '-fno-omit-frame-pointer',
                '-I' + str(root / 'tests/vgpu'), str(test), '-o', str(output / 'runner')]
    commands = [compiler, [str(output / 'runner')],
                [str(tools['gcov']), '-j', '-b', '-c', 'runner-dxvk_heap_audit_test.gcno']]
    env = dict(os.environ)
    env.update({'ASAN_OPTIONS': 'detect_leaks=1:halt_on_error=1',
                'LSAN_OPTIONS': 'exitcode=23',
                'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'})
    for index, command in enumerate(commands):
        with (output / ('command_' + str(index) + '.log')).open('wb') as log:
            subprocess.run(command, cwd=output, env=env, check=True, timeout=90, stdout=log, stderr=log)
    report_path = output / 'runner-dxvk_heap_audit_test.gcov.json.gz'
    if not report_path.is_file() or report_path.stat().st_size > 4 * 1024 * 1024:
        raise SystemExit('missing or oversized gcov JSON')
    report = json.loads(gzip.decompress(report_path.read_bytes()))
    summary = coverage_summary(report, source.resolve())
    if manifest != hashes():
        raise SystemExit('audit-stage coverage inputs changed during execution')
    summary.update(inputs=manifest, gcov_sha256=hashlib.sha256(report_path.read_bytes()).hexdigest(),
                   scope='actual production include under portable Win32 doubles; native Windows acknowledgement semantics remain separately gated')
    (output / 'coverage.json').write_text(json.dumps(summary, indent=2) + '\n')
    print('DXVK heap audit-stage coverage:', json.dumps(summary, sort_keys=True), 'artifacts:', output)
    return 0


if __name__ == '__main__':
    sys.exit(main())
