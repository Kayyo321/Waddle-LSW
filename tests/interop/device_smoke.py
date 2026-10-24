#!/usr/bin/env python3
"""Exercise disposable Waddle device/backend lifecycle; no guest OS acceptance."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import tempfile
import time

DeviceName = 'interop-smoke'


def run_tool(argv, *, environment=None, timeout=30):
    """Borrow argv/environment; return bounded owned text or raise on tool failure."""
    result = subprocess.run(argv, env=environment, capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f'{argv[0]} failed ({result.returncode}): {result.stdout[:4096]} {result.stderr[:4096]}')
    return result.stdout


def process_record(pid):
    """Return a borrowed-process identity snapshot; no process is changed."""
    try:
        root = Path('/proc') / str(pid)
        stat = (root / 'stat').read_text()
        tail = stat[stat.rfind(')') + 2:].split()
        return {'pid': pid, 'parent': int(tail[1]), 'start_time': tail[19],
                'args': [value.decode(errors='replace') for value in (root / 'cmdline').read_bytes().split(b'\0') if value]}
    except (OSError, ValueError, IndexError):
        return None


def qmp_command(stream, name, identifier):
    """Borrow connected stream; return matching bounded QMP reply or raise."""
    stream.write((json.dumps({'execute': name, 'id': identifier}) + '\n').encode())
    stream.flush()
    for _ in range(32):
        raw = stream.readline(65537)
        if not raw or len(raw) > 65536:
            raise RuntimeError('invalid QMP response size/EOF')
        reply = json.loads(raw)
        if reply.get('id') == identifier:
            if 'error' in reply:
                raise RuntimeError(str(reply['error']))
            return reply['return']
    raise RuntimeError('QMP reply limit exceeded')


def pci_mount_ids(value):
    """Return every exact mount qdev ID, preserving duplicates for rejection."""
    found = []
    if isinstance(value, dict):
        identifier = value.get('qdev_id')
        if isinstance(identifier, str) and identifier.startswith('waddle_fs_dev_'):
            found.append(identifier)
        for child in value.values():
            found.extend(pci_mount_ids(child))
    elif isinstance(value, list):
        for child in value:
            found.extend(pci_mount_ids(child))
    return found


def wait_absent(records):
    """Wait boundedly for exactly recorded process incarnations to disappear."""
    deadline = time.monotonic() + 10
    while True:
        live = [old for old in records if (new := process_record(old['pid'])) and new['start_time'] == old['start_time']]
        if not live:
            return
        if time.monotonic() >= deadline:
            raise RuntimeError(f'owned processes not reaped: {[value["pid"] for value in live]}')
        time.sleep(.02)


def smoke(waddle, output, temporary_acl):
    """Own a private profile/disk and all recorded children; restore ACL on all exits."""
    report = {'schema': 1, 'guest_os': False, 'status': 'failed', 'cycles': [],
              'waddle_sha256': hashlib.sha256(waddle.read_bytes()).hexdigest(),
              'limits': ['Empty disk boot/QMP/backend configuration only',
                         'No guest drive binding, read-only write-denial, audio or peripheral acceptance']}
    acl = None
    acl_mutation_attempted = False
    owned = []
    temporary = tempfile.mkdtemp(prefix='wdi-')
    preserve_root = False
    restore_mask = None
    try:
        root = Path(temporary)
        environment = os.environ.copy()
        for name in ('config', 'state', 'run'):
            (root / name).mkdir(mode=0o700)
        environment.update(XDG_CONFIG_HOME=str(root / 'config'), XDG_STATE_HOME=str(root / 'state'), XDG_RUNTIME_DIR=str(root / 'run'))

        def command(*arguments):
            value = run_tool([str(waddle), *arguments, '--json'], environment=environment)
            with (output / 'commands.jsonl').open('a') as log:
                log.write(json.dumps({'arguments': arguments, 'result': json.loads(value)}) + '\n')
            result = json.loads(value)
            if not result.get('ok'):
                raise RuntimeError(value)
            return result['results'][0]['data']

        try:
            if temporary_acl:
                acl = run_tool(['getfacl', '-p', '/dev/vhost-vsock'])
                (output / 'original_acl.json').write_text(json.dumps({'getfacl': acl}) + '\n')
                (output / 'original.acl').write_text(acl)
                acl_mutation_attempted = True
                run_tool(['sudo', '-n', 'setfacl', '-m', f'u:{os.getuid()}:rw', '/dev/vhost-vsock'])
            command('device', 'init', DeviceName, '--blank-disk', '64M', '--memory-mb', '512', '--vcpus', '1')
            config_path = root / 'config/waddle/devices' / (DeviceName + '.ini')
            initial = config_path.read_text()
            for cycle, mount_count in enumerate((0, 1, 8)):
                shares = []
                for index in range(mount_count):
                    path = root / f'share-{index}'
                    path.mkdir(mode=0o700, exist_ok=True)
                    (path / 'sentinel').write_text(f'private share {index}\n')
                    shares.append(f'mount = {path}:{chr(68 + index)}:\\:{"ro" if index % 2 else "rw"}')
                config = initial.replace('vsock_cid = 3', f'vsock_cid = {40000 + os.getpid() % 20000}')
                start = config.index('[filesystem]')
                end = config.index('[network]', start)
                config = config[:start] + '[filesystem]\n' + '\n'.join(shares) + '\n' + config[end:]
                config = config.replace('mode = user', 'mode = none').replace('cups_forward = true', 'cups_forward = false')
                config = config.replace('backend = pipewire', 'backend = none').replace('microphone = true', 'microphone = false').replace('per_app = true', 'per_app = false')
                config_path.write_text(config)
                data = command('start', DeviceName, '--no-wait')
                daemon_pid, qemu_pid = data['daemon_pid'], data['qemu_pid']
                for pid, executable in ((daemon_pid, 'waddled'), (qemu_pid, 'qemu-system-x86_64')):
                    value = process_record(pid)
                    if value is None or not value['args'] or Path(value['args'][0]).name != executable or not any(str(root) in arg for arg in value['args']):
                        raise RuntimeError('device process identity changed before QMP startup')
                    owned.append(value)
                qmp_path = root / 'run/waddle' / DeviceName / 'qmp.sock'
                deadline = time.monotonic() + 10
                while not qmp_path.exists() and time.monotonic() < deadline:
                    time.sleep(.02)
                with socket.socket(socket.AF_UNIX) as client:
                    client.settimeout(5)
                    client.connect(str(qmp_path))
                    with client.makefile('rwb') as stream:
                        greeting = json.loads(stream.readline(65537))
                        if 'QMP' not in greeting:
                            raise RuntimeError('no QMP greeting')
                        qmp_command(stream, 'qmp_capabilities', 1)
                        status = qmp_command(stream, 'query-status', 2)
                        if status.get('running') is not True:
                            raise RuntimeError(f'QEMU is not running: {status}')
                        pci = qmp_command(stream, 'query-pci', 3)
                        expected = [f'waddle_fs_dev_{chr(100 + index)}' for index in range(mount_count)]
                        observed = pci_mount_ids(pci)
                        if sorted(observed) != sorted(expected):
                            raise RuntimeError(f'PCI mounts {observed} != {expected}')
                backends = []
                for path in Path('/proc').iterdir():
                    if path.name.isdigit() and (value := process_record(int(path.name))):
                        if value['parent'] == daemon_pid and value['args'] and Path(value['args'][0]).name == 'virtiofsd':
                            backends.append(value)
                owned.extend(backends)
                if len(backends) != mount_count:
                    raise RuntimeError(f'backend count {len(backends)} != {mount_count}')
                for index in range(mount_count):
                    matches = [entry for entry in backends if '--shared-dir=' + str(root / f'share-{index}') in entry['args']]
                    if len(matches) != 1 or ('--readonly' in matches[0]['args']) != bool(index % 2):
                        raise RuntimeError(f'incorrect read-only policy for share {index}: {backends}')
                cycle_owned = list(owned)
                command('kill', DeviceName)
                wait_absent(cycle_owned)
                owned.clear()
                report['cycles'].append({'cycle': cycle, 'mount_count': mount_count, 'readonly_count': mount_count // 2,
                                         'qmp_running': True, 'mount_ids': expected, 'all_children_and_supervisor_reaped': True})
            report['status'] = 'passed'
        finally:
            # Defer repeated interrupts until ACL restoration and receipt saving
            # finish; pending signals are delivered when the original mask returns.
            restore_mask = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGINT, signal.SIGTERM})
            try:
                command('kill', DeviceName)
            except Exception as error:
                report['cleanup_cli_error'] = str(error)
            # Cleanup authority stays with the private CLI supervisor. A PID
            # snapshot cannot authorize fallback signaling across PID reuse.
            cleanup_error = RuntimeError(report['cleanup_cli_error']) if 'cleanup_cli_error' in report else None
            try:
                if owned:
                    wait_absent(owned)
            except Exception as error:
                cleanup_error = error
            finally:
                if acl is not None and acl_mutation_attempted:
                    try:
                        run_tool(['sudo', '-n', 'setfacl', '--restore=' + str(output / 'original.acl')])
                        report['vhost_acl_restored'] = run_tool(['getfacl', '-p', '/dev/vhost-vsock']) == acl
                        if not report['vhost_acl_restored']:
                            raise RuntimeError('vhost-vsock ACL restore mismatch')
                    except Exception as error:
                        cleanup_error = error
            if cleanup_error is not None:
                report['status'] = 'failed'
                report['cleanup_error'] = str(cleanup_error)
                preserve_root = True
                report['retained_private_root'] = str(root)
            for source in (root / 'state').rglob('*.log'):
                shutil.copyfile(source, output / source.name)
            (output / 'receipt.json').write_text(json.dumps(report, indent=2) + '\n')
            if cleanup_error is not None:
                raise cleanup_error
    finally:
        try:
            if not preserve_root:
                shutil.rmtree(temporary)
        finally:
            if restore_mask is not None:
                signal.pthread_sigmask(signal.SIG_SETMASK, restore_mask)
    return report


def main():
    """Parse owned-test options and print a complete result after cleanup."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--waddle', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True, help='new evidence directory')
    parser.add_argument('--temporary-vsock-acl', action='store_true', help='grant current uid access only during this test, then restore exact ACL')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    def interrupted(_signal_number, _frame):
        raise InterruptedError('native smoke interrupted')
    signal.signal(signal.SIGTERM, interrupted)
    report = smoke(args.waddle.resolve(strict=True), args.out.resolve(), args.temporary_vsock_acl)
    print(json.dumps(report, sort_keys=True))


if __name__ == '__main__':
    main()
