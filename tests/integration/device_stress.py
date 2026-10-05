#!/usr/bin/env python3
"""1000 independent offline lifecycle cycles with concurrent registry readers."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading

executable = os.environ.get('WADDLE_TEST_EXECUTABLE', str(Path('build/waddle').resolve()))
cycles = int(os.environ.get('WADDLE_DEVICE_STRESS_CYCLES', '1000'))
assert cycles > 0
with tempfile.TemporaryDirectory(prefix='wd-stress-') as root:
    root = Path(root)
    env = dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root/'config'),
               XDG_STATE_HOME=str(root/'state'), XDG_RUNTIME_DIR=str(root/'run'))
    finished = threading.Event()
    failures = []
    observations = [0]
    def run(*args):
        result = subprocess.run([executable, 'device', *args, '--json'], env=env, capture_output=True, text=True)
        assert result.returncode == 0, (args, result.stdout, result.stderr)
        value = json.loads(result.stdout)
        assert value['ok'] and value['error'] is None
        return value
    def read_registry():
        try:
            while not finished.is_set():
                rows = run('list')['results']
                names = [row['name'] for row in rows]
                cids = [row['data']['vsock_cid'] for row in rows]
                assert names == sorted(names) and len(cids) == len(set(cids))
                assert all(row['data']['config_valid'] for row in rows)
                observations[0] += 1
        except BaseException as error:
            failures.append(error)
    reader = threading.Thread(target=read_registry, daemon=True)
    reader.start()
    try:
        for index in range(cycles):
            run('init', 'source', '--blank-disk', '1M')
            run('clone', 'source', 'copy')
            run('config', 'set', 'source', 'vcpus=8', 'memory_mb=512')
            run('rename', 'source', 'moved')
            run('remove', 'copy', '--yes')
            run('remove', 'moved', '--yes')
            if (index+1) % 100 == 0:
                print(f'device_stress: {index+1}/{cycles} cycles', flush=True)
            assert not failures, failures
    finally:
        finished.set()
        reader.join(timeout=30)
    assert not reader.is_alive() and not failures
    assert run('list')['results'] == []
    assert not list((root/'config/waddle/transactions').iterdir())
    assert not list((root/'state/waddle/transactions').iterdir())
    assert not list((root/'state/waddle/devices').iterdir())
    assert not (root/'run').exists()
    print(f'device_stress: {cycles} cycles, {observations[0]} concurrent snapshots; no residual devices, journals or runtime files')
