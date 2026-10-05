#!/usr/bin/env python3
"""Real qemu-img offline transaction and backup acceptance in private XDG roots."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

executable = str(Path('build/waddle').resolve())
with tempfile.TemporaryDirectory(prefix='wd-storage-') as root:
    root = Path(root)
    env = dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root/'config'),
               XDG_STATE_HOME=str(root/'state'), XDG_RUNTIME_DIR=str(root/'run'))
    def run(*args, status=0):
        result = subprocess.run([executable, 'device', *args, '--json'], env=env, capture_output=True, text=True)
        assert result.returncode == status, (args, result.returncode, result.stdout, result.stderr)
        document = json.loads(result.stdout)
        assert document['ok'] == (status == 0), document
        return document
    def show(name):
        return run('show',name)['results'][0]['data']
    run('init','alpha',status=2)  # no base silently creates an unbootable disk
    run('init','alpha','--blank-disk','0M',status=2)
    run('init','alpha','--blank-disk','8M','--dry-run')
    assert not (root/'state').exists()
    run('init','alpha','--blank-disk','8M','--memory-mb','8192','--vcpus','8','--shell','cmd.exe')
    alpha = show('alpha')
    disk = Path(alpha['disk_image'])
    original = disk.read_bytes()
    profile = Path(alpha['config_path'])
    original_config = profile.read_text()
    profile.write_text(original_config+'\n# opaque preservation\n[vendor]\ntoken = keep-me\n')
    profile.chmod(0o600)
    run('default','alpha')
    run('rename','alpha','omega','--dry-run')
    assert disk.read_bytes() == original
    run('rename','alpha','omega')
    run('show','alpha',status=2)
    omega = show('omega')
    assert omega['vsock_cid'] == alpha['vsock_cid'] and omega['is_default']
    assert omega['config'] == alpha['config']
    assert Path(omega['disk_image']).read_bytes() == original
    assert 'token = keep-me' in Path(omega['config_path']).read_text()
    run('clone','omega','copy')
    copy = show('copy')
    assert copy['vsock_cid'] != omega['vsock_cid'] and not copy['is_default']
    assert copy['config'] == omega['config']
    info = json.loads(subprocess.check_output(['qemu-img','info','--output=json',copy['disk_image']]))
    assert 'backing-filename' not in info
    backup = root/'backup'
    run('export','omega','--output',str(backup))
    manifest = json.loads((backup/'manifest.json').read_text())
    assert manifest['disk_sha256'] == hashlib.sha256((backup/'disk.qcow2').read_bytes()).hexdigest()
    run('export','omega','--output',str(backup),status=2)
    run('import','restored','--input',str(backup))
    restored = show('restored')
    assert restored['config'] == omega['config'] and restored['mount_count'] == 0
    assert restored['vsock_cid'] not in (omega['vsock_cid'], copy['vsock_cid'])
    manifest['disk_sha256'] = '0'*64
    (backup/'manifest.json').write_text(json.dumps(manifest))
    run('import','bad','--input',str(backup),status=2)
    run('show','bad',status=2)
    run('remove','omega',status=2)  # JSON deletion requires explicit acknowledgement
    run('remove','omega','--yes','--dry-run')
    assert Path(omega['disk_image']).exists()
    retained = run('remove','omega','--yes','--keep-data')['results'][0]['data']['path']
    assert Path(retained,'disk.qcow2').read_bytes() == original
    assert Path(retained,'original.ini').is_file()
    assert run('default')['results'][0]['data']['default_device'] is None
    run('remove','copy','--yes')
    assert not Path(copy['state_dir']).exists()
    assert not Path(copy['config_path']).exists()
    run('remove','copy','--yes',status=2)
    run('remove','restored','--yes')
    assert run('list')['results'] == []
    assert not list((root/'config/waddle/transactions').glob('*.json'))
    # Terminate at durable boundaries and verify repeatable rollback/commit recovery.
    for point in ('prepared', 'staged', 'move_0', 'move_1', 'move_2', 'move_3', 'move_4', 'committed'):
        run('init', 'before', '--blank-disk', '8M')
        run('default', 'before')
        before = show('before')
        crash_env = dict(env, WADDLE_DEVICE_TEST_CRASH=point)
        crashed = subprocess.run([executable, 'device', 'rename', 'before', 'after', '--json'], env=crash_env, capture_output=True)
        assert crashed.returncode == 99, (point, crashed.stdout, crashed.stderr)
        run('list', status=1)  # readers cannot see mixed metadata
        # Any real mutation first recovers. Clearing default tests recovery itself.
        run('default', '--clear')
        name = 'after' if point == 'committed' else 'before'
        other = 'before' if point == 'committed' else 'after'
        assert show(name)['vsock_cid'] == before['vsock_cid']
        run('show', other, status=2)
        run('default', '--clear')  # recovery is idempotent
        run('remove', name, '--yes')
    assert run('list')['results'] == []

print('device_storage: real QCOW2 init/rename/clone/export/import/retention/removal passed')
