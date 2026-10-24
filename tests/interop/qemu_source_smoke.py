#!/usr/bin/env python3
"""Check source-built QEMU features in diskless paused guests; no guest OS acceptance."""
import argparse
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time


def qmp_command(stream, command, identifier):
    """Borrow one connected stream and return a matching bounded QMP reply."""
    stream.write((json.dumps({'execute': command, 'id': identifier}) + '\n').encode())
    stream.flush()
    for _ in range(32):
        raw = stream.readline(65537)
        if not raw or len(raw) > 65536:
            raise RuntimeError('invalid QMP reply size/EOF')
        reply = json.loads(raw)
        if reply.get('id') == identifier:
            if 'error' in reply:
                raise RuntimeError(str(reply['error']))
            return reply['return']
    raise RuntimeError('QMP reply limit exceeded')


def smoke(binary, pipewire):
    """Own a temporary directory and paused child; always stop and reap that child."""
    with tempfile.TemporaryDirectory(prefix='wd-qemu-smoke-') as temporary:
        root = Path(temporary)
        qmp_path = root / 'qmp.sock'
        cups_path = root / 'cups.sock'
        with socket.socket(socket.AF_UNIX) as cups:
            cups.bind(str(cups_path))
            cups.listen(1)
            audio = 'pipewire,id=waddle_audio,out.name=waddle-discard.smoke,in.voices=0' if pipewire else 'none,id=waddle_audio'
            args = [str(binary), '-machine', 'q35,accel=tcg', '-m', '128', '-nodefaults',
                    '-no-user-config', '-display', 'none', '-monitor', 'none', '-serial', 'none', '-S',
                    '-audiodev', audio, '-device', 'ich9-intel-hda,id=waddle_hda',
                    '-device', 'hda-duplex,audiodev=waddle_audio', '-device', 'qemu-xhci,id=waddle_xhci',
                    '-chardev', f'socket,id=waddle_cups,path={cups_path}',
                    '-netdev', 'user,id=waddle_net,guestfwd=tcp:10.0.2.100:631-chardev:waddle_cups',
                    '-device', 'virtio-net-pci,netdev=waddle_net,id=waddle_nic',
                    '-qmp', f'unix:{qmp_path},server=on,wait=off']
            with (root / 'qemu.log').open('w+') as log:
                child = subprocess.Popen(args, stdout=log, stderr=log)
                try:
                    deadline = time.monotonic() + 10
                    while not qmp_path.exists() and child.poll() is None and time.monotonic() < deadline:
                        time.sleep(.02)
                    with socket.socket(socket.AF_UNIX) as client:
                        client.settimeout(5)
                        client.connect(str(qmp_path))
                        with client.makefile('rwb') as stream:
                            greeting = json.loads(stream.readline(65537))
                            if 'QMP' not in greeting:
                                raise RuntimeError(f'missing QMP greeting: {greeting}')
                            qmp_command(stream, 'qmp_capabilities', 1)
                            status = qmp_command(stream, 'query-status', 2)
                            if status.get('running') is not False:
                                raise RuntimeError(f'guest unexpectedly running: {status}')
                            pci = qmp_command(stream, 'query-pci', 3)
                            serialized = json.dumps(pci)
                            for identifier in ('waddle_hda', 'waddle_xhci', 'waddle_nic'):
                                if identifier not in serialized:
                                    raise RuntimeError(f'missing PCI device: {identifier}')
                            qmp_command(stream, 'quit', 4)
                    if child.wait(timeout=5) != 0:
                        raise RuntimeError('QEMU did not exit successfully')
                    print(json.dumps({'audio': 'pipewire' if pipewire else 'none', 'paused': True,
                                      'qmp': 'passed', 'network': 'user+socket-guestfwd',
                                      'devices': ['HDA', 'xHCI', 'virtio-net'], 'guest_os': False}))
                except BaseException:
                    log.flush()
                    log.seek(0)
                    print(log.read(65536))
                    raise
                finally:
                    if child.poll() is None:
                        child.terminate()
                        try:
                            child.wait(timeout=3)
                        except subprocess.TimeoutExpired:
                            child.kill()
                            child.wait()


def main():
    """Parse borrowed CLI arguments; return after owned diskless child cleanup."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, required=True)
    parser.add_argument('--pipewire', action='store_true', help='also connect the host PipeWire backend without running guest CPUs')
    arguments = parser.parse_args()
    binary = arguments.qemu.resolve(strict=True)
    for options, required in [(['-audiodev', 'help'], 'pipewire'), (['-netdev', 'help'], 'user'), (['-device', 'help'], 'usb-host')]:
        result = subprocess.run([str(binary), *options], capture_output=True, text=True, timeout=10, check=True)
        if required not in result.stdout + result.stderr:
            raise RuntimeError(f'missing QEMU capability: {required}')
    smoke(binary, False)
    if arguments.pipewire:
        smoke(binary, True)


if __name__ == '__main__':
    main()
