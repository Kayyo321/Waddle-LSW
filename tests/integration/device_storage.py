#!/usr/bin/env python3
"""Real qemu-img offline transaction and backup acceptance in private XDG roots."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import signal
import time
import shutil

executable = os.environ.get('WADDLE_TEST_EXECUTABLE', str(Path('build/waddle').resolve()))
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
    # A managed disk cannot be removed or renamed while another registration backs it.
    run('init', 'dependent', '--base-disk', omega['disk_image'])
    run('remove', 'omega', '--yes', status=1)
    run('rename', 'omega', 'blocked', status=1)
    run('remove', 'dependent', '--yes')
    # Orphan destinations and symlink state roots are never adopted.
    orphan = root/'state/waddle/devices/orphan'
    orphan.mkdir(mode=0o700)
    run('init', 'orphan', '--blank-disk', '8M', status=2)
    run('doctor', '--all', status=1)
    orphan.rmdir()
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
    manifest_original = (backup/'manifest.json').read_bytes()
    (backup/'extra').write_text('unrecognized')
    run('import', 'bad', '--input', str(backup), status=2)
    (backup/'extra').unlink()
    saved_disk = backup/'saved_disk'
    (backup/'disk.qcow2').rename(saved_disk)
    (backup/'disk.qcow2').symlink_to(saved_disk)
    run('import', 'bad', '--input', str(backup), status=2)
    (backup/'disk.qcow2').unlink()
    saved_disk.rename(backup/'disk.qcow2')
    link = root/'backup_link'
    link.symlink_to(backup, target_is_directory=True)
    run('import', 'bad', '--input', str(link), status=1)
    link.unlink()
    for broken in ('{"schema_version":1,"schema_version":1}', '{"unknown":true}', '[[[[[[[[[0]]]]]]]]]', '{', '\xff'):
        (backup/'manifest.json').write_bytes(broken.encode('latin1'))
        run('import', 'bad', '--input', str(backup), status=2)
    (backup/'manifest.json').write_bytes(manifest_original)
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

# SIGINT owns, kills and reaps a utility child, rolls back intent and renders JSON.
with tempfile.TemporaryDirectory(prefix="waddle-cancel-") as temporary:
    root = Path(temporary)
    env = os.environ.copy()
    env.update(HOME=str(root), XDG_CONFIG_HOME=str(root/'config'),
               XDG_STATE_HOME=str(root/'state'), XDG_RUNTIME_DIR=str(root/'run'))
    binary = root/'bin'; binary.mkdir()
    utility = binary/'qemu-img'
    utility.write_text('#!/usr/bin/python3\nimport os,sys,time\nfrom pathlib import Path\n'
                       'if sys.argv[1] == "create":\n'
                       ' Path(os.environ["WADDLE_CHILD_PID"]).write_text(str(os.getpid()))\n'
                       ' Path(sys.argv[-2]).write_bytes(b"partial")\n'
                       ' time.sleep(30)\n'
                       'else: os.execv('+repr(shutil.which('qemu-img'))+',sys.argv)\n')
    utility.chmod(0o700)
    env.update(PATH=str(binary)+':'+env['PATH'], WADDLE_CHILD_PID=str(root/'child'))
    process = subprocess.Popen([str(Path('build/waddle').resolve()),'device','init','cancelled','--blank-disk','8M','--json'],
                               env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    deadline = time.monotonic()+10
    while not (root/'child').exists() and time.monotonic() < deadline:
        time.sleep(.02)
    assert (root/'child').exists(), 'utility failed to spawn'
    child = int((root/'child').read_text())
    process.send_signal(signal.SIGINT)
    output, diagnostics = process.communicate(timeout=10)
    assert process.returncode == 130, (process.returncode,output,diagnostics)
    assert json.loads(output)['error']['code'] == 'cancelled'
    assert not Path('/proc',str(child)).exists(), 'cancelled utility was not reaped'
    assert not (root/'config/waddle/devices/cancelled.ini').exists()
    assert not list((root/'config/waddle/transactions').iterdir())
    assert not list((root/'state/waddle/transactions').iterdir())
print('device storage cancellation: SIGINT JSON/130, child reaped, transaction rollback clean')
