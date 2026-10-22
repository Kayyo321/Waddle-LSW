#!/usr/bin/env python3
"""Create the private, patched Vulkan loader source tree used by Windows loader builds.

The pinned submodule is never modified. This script verifies that the checked-out submodule
commit equals the superproject gitlink, extracts that exact tree with `git archive`, applies
every tracked patch under patches/vulkan_loader/ in name order (each checked first with
`git apply --check`), and writes a receipt binding the pin, patches and output.

Usage: vulkan_loader_private_source.py <output-directory>
The output directory is replaced. Exit status is nonzero on any mismatch or apply failure.
Linux builds keep using the submodule directly: every patched line is Windows-only.
"""
from __future__ import annotations

import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

Root = Path(__file__).resolve().parents[1]
Submodule = Root / 'submodules/vulkan_loader'
PatchDirectory = Root / 'patches/vulkan_loader'
ExpectedPatches = ('0001-release-windows-initialization-module-references.patch',)


def run(arguments: list[str], cwd: Path, data: bytes | None = None, environment: dict[str, str] | None = None) -> bytes:
    """Run one command; return stdout bytes or raise with its stderr."""
    result = subprocess.run(arguments, cwd=cwd, input=data, capture_output=True, env=environment)
    if result.returncode:
        raise RuntimeError(' '.join(arguments) + ' failed: ' + result.stderr.decode(errors='replace'))
    return result.stdout


def pinned_commit() -> str:
    """Return the gitlink commit recorded by the superproject for the loader submodule."""
    fields = run(['git', 'ls-tree', 'HEAD', 'submodules/vulkan_loader'], Root).decode().split()
    if len(fields) != 4 or fields[0] != '160000' or fields[1] != 'commit':
        raise RuntimeError('superproject does not record the loader as a gitlink')
    return fields[2]


def extract(commit: str, output: Path) -> None:
    """Extract exactly the pinned tree; refuse links and paths escaping the output."""
    archive = run(['git', 'archive', '--format=tar', commit], Submodule)
    base = output.resolve()
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        for member in tar.getmembers():
            target = (base / member.name).resolve()
            if (target != base and base not in target.parents) or member.issym() or member.islnk():
                raise RuntimeError('unsafe archive member: ' + member.name)
        if hasattr(tarfile, 'data_filter'):
            tar.extractall(base, filter='data')
        else:
            tar.extractall(base)


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.split('\n\n')[2], file=sys.stderr)
        return 2
    output = Path(argv[1]).resolve()
    commit = pinned_commit()
    checked_out = run(['git', 'rev-parse', 'HEAD'], Submodule).decode().strip()
    if checked_out != commit:
        raise RuntimeError(f'loader submodule checkout {checked_out} differs from pinned gitlink {commit}')
    patches = sorted(path.name for path in PatchDirectory.glob('*.patch'))
    if tuple(patches) != ExpectedPatches:
        raise RuntimeError('tracked loader patch set differs from the reviewed set: ' + ', '.join(patches))
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)
    extract(commit, output)
    # The output usually lies inside this superproject's working tree. Without a ceiling, git
    # would discover the superproject and silently skip patch paths outside its cwd, reporting
    # success while changing nothing. Stop discovery at the output's parent, then prove each
    # patch is present by requiring its reverse to apply cleanly.
    environment = {key: value for key, value in os.environ.items() if not key.startswith('GIT_')}
    environment['GIT_CEILING_DIRECTORIES'] = str(output.parent)
    applied = []
    for name in patches:
        patch = PatchDirectory / name
        run(['git', 'apply', '--check', str(patch)], output, environment=environment)
        run(['git', 'apply', str(patch)], output, environment=environment)
        run(['git', 'apply', '--check', '--reverse', str(patch)], output, environment=environment)
        applied.append({'name': name, 'sha256': hashlib.sha256(patch.read_bytes()).hexdigest()})
    receipt = {'schema': 'waddle_vulkan_loader_private_source_v1', 'pinned_commit': commit,
               'patches': applied, 'output': str(output)}
    (output / 'waddle_private_source.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt))
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv))
