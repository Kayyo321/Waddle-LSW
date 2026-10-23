#!/usr/bin/env python3
"""Gate actual Windows environment-guard code under a mock-only Linux boundary.

Borrow immutable source/header/tool inputs; own a fresh artifact directory. Native
Windows API execution is separate. Require all seven guard functions, >=90% of
production lines and branches, and unchanged inputs; fixture code never counts.
No threshold override, suppression, rewritten source, or shared profile output.
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
ProductionFunctions = {'valid_api', 'bounded_value_length', 'capture_value',
                       'waddle_loader_environment_guard_init',
                       'waddle_loader_environment_windows_api',
                       'waddle_loader_environment_begin', 'waddle_loader_environment_restore'}


def digest(path):
    """[in] Borrow regular path; [out] owned SHA256; read-only synchronous lifetime."""
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    """[in] Exclusive output-root CLI; [out] retained exact production coverage proof.

    Children/descriptors close synchronously; errors preserve diagnostics and fail.
    No source mutation. Caller serializes source access through completion.
    """
    assert len(sys.argv) == 2
    root = Path.cwd().resolve()
    output = Path(sys.argv[1]).resolve() / ('run-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ') + '-' + str(os.getpid()))
    output.mkdir(parents=True)
    source = root / 'tests/vgpu/loader_environment_windows.c'
    inputs = [source, root / 'tests/vgpu/loader_environment_windows.h',
              root / 'tests/vgpu/loader_environment_fake/windows.h', Path(__file__).resolve()]
    for tool in ('cc', 'gcov'):
        found = shutil.which(tool)
        assert found
        inputs.append(Path(found).resolve(strict=True))
    manifest = {str(path): digest(path) for path in inputs}
    text = source.read_text()
    marker = '#if defined(WaddleLoaderEnvironmentTest)'
    assert text.count(marker) == 1
    boundary = next(index for index, line in enumerate(text.splitlines(), 1) if line == marker)
    commands = [
        ['cc', '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-O0', '-g', '--coverage',
         '-DWaddleLoaderEnvironmentTest', '-DWaddleLoaderEnvironmentFakeWindows',
         '-I' + str(root / 'tests/vgpu/loader_environment_fake'), '-I' + str(root / 'tests/vgpu'),
         str(source), '-o', str(output / 'runner')],
        [str(output / 'runner')],
        ['gcov', '-j', '-b', '-c', 'runner-loader_environment_windows.gcno'],
    ]
    for index, command in enumerate(commands):
        with (output / ('command_' + str(index) + '.log')).open('wb') as stream:
            subprocess.run(command, cwd=output, check=True, timeout=60, stdout=stream, stderr=stream)
    artifact = output / 'runner-loader_environment_windows.gcov.json.gz'
    assert artifact.stat().st_size <= 1024 * 1024
    report = json.loads(gzip.decompress(artifact.read_bytes()))
    files = [entry for entry in report['files'] if Path(entry['file']).resolve() == source]
    assert len(files) == 1
    selected = files[0]
    functions = [entry for entry in selected['functions'] if entry['start_line'] < boundary]
    assert {entry['name'] for entry in functions} == ProductionFunctions and len(functions) == 7
    assert all(entry['end_line'] < boundary for entry in functions)
    lines = [entry for entry in selected['lines'] if entry['line_number'] < boundary]
    assert all(entry['function_name'] in ProductionFunctions for entry in lines)
    branches = [branch for entry in lines for branch in entry['branches']]
    assert lines and branches
    line_hit = sum(entry['count'] > 0 for entry in lines)
    branch_hit = sum(entry['count'] > 0 for entry in branches)
    assert manifest == {str(path): digest(path) for path in inputs}, 'coverage inputs changed'
    result = {'scope': 'actual guard production code; Linux mock Win32 APIs; no native Windows claim',
              'input_manifest': manifest, 'fixture_boundary': boundary, 'functions': sorted(ProductionFunctions),
              'minimum_percent': MinimumPercent, 'lines_hit': line_hit, 'lines_total': len(lines),
              'branches_hit': branch_hit, 'branches_total': len(branches), 'commands': commands,
              'raw_report': str(artifact), 'raw_report_sha256': digest(artifact)}
    (output / 'coverage.json').write_text(json.dumps(result, indent=2) + '\n')
    print('Loader environment production lines: %d/%d (%.2f%%); branches: %d/%d (%.2f%%); %s' %
          (line_hit, len(lines), 100 * line_hit / len(lines), branch_hit, len(branches),
           100 * branch_hit / len(branches), output), flush=True)
    assert 100 * line_hit >= MinimumPercent * len(lines), 'production line coverage below 90%'
    assert 100 * branch_hit >= MinimumPercent * len(branches), 'production branch coverage below 90%'


if __name__ == '__main__':
    main()
