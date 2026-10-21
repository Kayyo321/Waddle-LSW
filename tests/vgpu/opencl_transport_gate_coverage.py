#!/usr/bin/env python3
"""Run actual qualification gate controls and enforce unchanged90% floors.

Each invocation retains source/tool hashes, exact commands, logs and raw gcov.
No Windows, actual bridge cancellation or GPU acceptance is inferred.
"""
import datetime
import gzip
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    """Own fresh artifacts; require actual helper function/line/branch coverage."""
    root = Path(__file__).resolve().parents[2]
    base = root / 'build/coverage/vgpu/opencl_transport_gate'
    base.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S_'), dir=base))
    source = root / 'tests/vgpu/opencl_transport_gate.c'
    test = root / 'tests/vgpu/opencl_transport_gate_test.c'
    socket = root / 'src/vgpu/venus_tcp_socket.c'
    validator = root / 'scripts/clvk_runtime_tls_coverage.py'
    paths = [source, test, socket, root / 'tests/vgpu/opencl_transport_gate.h',
             validator, Path(__file__).resolve()]
    for tool in ('cc', 'gcov'):
        path = shutil.which(tool)
        if not path:
            raise RuntimeError('missing required tool ' + tool)
        paths.append(Path(path).resolve())
    def hashes():
        return {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in paths}
    inputs = hashes()
    flags = ['-std=c11', '-D_POSIX_C_SOURCE=200809L', '-DWaddleOpenclTransportGate',
             '-I' + str(root / 'include'), '-I' + str(root / 'tests/vgpu'),
             '-Wall', '-Wextra', '-Werror', '-Wpedantic', '-O0', '-g', '-pthread']
    wrappers = ['-Wl,--wrap=' + name for name in
                ('fchmod', 'fstat', 'write', 'close', 'poll', 'venus_tcp_now_ms')]
    commands = [
        ['cc', *flags, '--coverage', '-fprofile-update=atomic', '-fprofile-abs-path',
         '-c', str(source), '-o', str(output / 'gate.o')],
        ['cc', *flags, str(test), str(socket), str(output / 'gate.o'),
         *wrappers, '--coverage', '-o', str(output / 'runner')],
        [str(output / 'runner')],
        ['gcov', '-j', '-b', '-c', 'gate.gcno'],
    ]
    rows = []
    for index, argv in enumerate(commands):
        with (output / (str(index) + '.log')).open('wb') as log:
            result = subprocess.run(argv, cwd=output, stdout=log, stderr=log, timeout=60)
        rows.append({'argv': argv, 'exit': result.returncode})
        (output / 'commands.json').write_text(json.dumps(rows, indent=2) + '\n')
        result.check_returncode()
    artifact = output / 'gate.gcov.json.gz'
    if artifact.stat().st_size > 1024 * 1024:
        raise RuntimeError('coverage artifact exceeds bound')
    spec = importlib.util.spec_from_file_location('gate_coverage_validator', validator)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    required = frozenset(('gate_copy_path', 'gate_publish', 'venus_opencl_transport_gate_wait'))
    summary = module.coverage_summary(json.loads(gzip.decompress(artifact.read_bytes())), source, required)
    if hashes() != inputs:
        raise RuntimeError('coverage source or tool changed')
    summary.update(inputs=inputs, raw_sha256=hashlib.sha256(artifact.read_bytes()).hexdigest(),
                   scope='actual POSIX qualification gate with controlled I/O failures; native OpenCL cancellation pending')
    (output / 'coverage.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, sort_keys=True)); print('artifacts:', output)


if __name__ == '__main__':
    main()
