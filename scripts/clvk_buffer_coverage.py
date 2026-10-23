#!/usr/bin/env python3
"""Gate exact private clvk buffer API bodies, including every directional edge.

Own a unique output directory and private source copy. Borrow pinned sources,
patches, fixture and tools read-only; execute CPU doubles only. This gives no
native Windows, Vulkan mapping or physical GPU acceptance. No coverage exclusions
or configurable threshold; fixture/double code never enters the denominator.
"""
from collections import Counter
from datetime import datetime, timezone
import gzip
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
MinimumPercent = 90


def digest(path):
    """[in] Borrow regular file; [out] owned SHA256; synchronous read-only call."""
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    """[in] Exclusive output root; [out] retained >=90% production proof.

    Own and close each compiler/runner child and log before proceeding. Errors
    retain diagnostics and fail; caller serializes every input through return.
    """
    assert len(sys.argv) == 2
    root = Path.cwd().resolve()
    output = Path(sys.argv[1]).resolve() / ('run-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ') + '-' + str(os.getpid()))
    output.mkdir(parents=True)
    helper = root / 'tests/vgpu/clvk_build.py'
    spec = importlib.util.spec_from_file_location('clvk_coverage_helpers', helper)
    unit = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(unit)
    driver = unit.Driver
    fixture = root / 'tests/vgpu/clvk_buffer_api.cpp'
    headers = root / 'submodules/clvk/external/OpenCL-Headers'
    tools = [Path(shutil.which(name)).resolve(strict=True) for name in ('g++', 'gcov')]
    inputs = [Path(__file__).resolve(), helper, root / 'scripts/build_clvk_windows.py',
              root / 'submodules/clvk/src/api.cpp', fixture, *tools,
              *(root / path for path in driver.PrivatePatches), *sorted(headers.rglob('*.h'))]
    manifest = {str(path): digest(path) for path in inputs}
    (output / 'input_manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    source = output / 'source'
    shutil.copytree(root / 'submodules/clvk/src', source / 'src')
    driver.apply_private_patches(root, source)
    header = output / 'clvk_buffer_api_under_test.hpp'
    header.write_text(unit.extract_buffer_api_header((source / 'src/api.cpp').read_text()))
    binary = output / 'runner'
    commands = [
        ['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wconversion', '-Wshadow',
         '-O0', '-g', '--coverage', '-DCL_TARGET_OPENCL_VERSION=300', '-I' + str(output),
         '-I' + str(headers), str(fixture), '-o', str(binary)],
        [str(binary)],
        ['gcov', '-j', '-b', '-c', 'runner-clvk_buffer_api.gcno'],
    ]
    for index, command in enumerate(commands):
        with (output / ('command_' + str(index) + '.log')).open('wb') as stream:
            subprocess.run(command, cwd=output, check=True, timeout=120, stdout=stream, stderr=stream)
    artifact = output / 'runner-clvk_buffer_api.gcov.json.gz'
    assert artifact.stat().st_size <= 4 * 1024 * 1024
    report = json.loads(gzip.decompress(artifact.read_bytes()))
    selected = [entry for entry in report['files'] if Path(entry['file']).resolve() == header]
    assert len(selected) == 1
    selected = selected[0]
    expected = Counter(re.search(r'(\w+)\(', signature).group(1) for signature in unit.BufferApiDefinitions)
    functions = selected['functions']
    actual = Counter(entry['demangled_name'].split('(', 1)[0] for entry in functions)
    assert actual == expected and len(functions) == 13
    assert all(entry['execution_count'] > 0 for entry in functions)
    names = {entry['name'] for entry in functions}
    lines = selected['lines']
    assert lines and all(entry['function_name'] in names for entry in lines)
    branches = [branch for entry in lines for branch in entry['branches']]
    assert branches
    line_hit = sum(entry['count'] > 0 for entry in lines)
    branch_hit = sum(entry['count'] > 0 for entry in branches)
    assert manifest == {str(path): digest(path) for path in inputs}, 'coverage inputs changed'
    assert set(headers.rglob('*.h')) <= set(inputs), 'header inventory changed'
    result = {'scope': '13 exact clvk API/helper definitions; CPU object/command doubles',
              'minimum_percent': MinimumPercent, 'lines_hit': line_hit, 'lines_total': len(lines),
              'branches_hit': branch_hit, 'branches_total': len(branches),
              'branch_definition': 'all directional gcov outcomes, including compiler throw edges; no exclusions',
              'input_manifest': manifest, 'generated_header_sha256': digest(header),
              'patched_api_sha256': digest(source / 'src/api.cpp'), 'commands': commands,
              'functions': functions, 'raw_report': str(artifact), 'raw_report_sha256': digest(artifact)}
    (output / 'coverage.json').write_text(json.dumps(result, indent=2) + '\n')
    print('clvk buffer production lines: %d/%d (%.2f%%); directional branches: %d/%d (%.2f%%); %s' %
          (line_hit, len(lines), 100 * line_hit / len(lines), branch_hit, len(branches),
           100 * branch_hit / len(branches), output), flush=True)
    assert 100 * line_hit >= MinimumPercent * len(lines), 'production lines below 90%'
    assert 100 * branch_hit >= MinimumPercent * len(branches), 'production directional branches below 90%'


if __name__ == '__main__':
    main()
