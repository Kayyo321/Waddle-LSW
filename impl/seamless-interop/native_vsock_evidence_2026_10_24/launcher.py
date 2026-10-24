"""Private acceptance launcher; only the fixed device-open helper runs as root."""
import array
import collections
import json
import os
from pathlib import Path
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, '/home/dev/Waddle-LSW/tests/interop')
from native_guest_runner import native_guest_runner_t, runner_config_t

Root = Path(tempfile.mkdtemp(prefix='waddle-vsock-access-'))
Cid = 52942
helper_code = '''import array, os, socket, stat, struct, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(sys.argv[1])
pid, uid, gid = struct.unpack('3i', s.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
if uid != 1001: raise RuntimeError('unexpected receiver uid')
fd = os.open('/dev/vhost-vsock', os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW)
try:
 info = os.fstat(fd)
 if not stat.S_ISCHR(info.st_mode) or (os.major(info.st_rdev), os.minor(info.st_rdev)) != (10,241):
  raise RuntimeError('unexpected device identity')
 s.sendmsg([b'V'], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [fd]))])
finally:
 os.close(fd)
 s.close()
'''
listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
listener.bind(str(Root / 'fd.sock'))
listener.listen(1)
listener.settimeout(10)
helper = subprocess.Popen(['sudo', '-n', '/usr/bin/python3', '-I', '-c', helper_code,
                           str(Root / 'fd.sock')], stdin=subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, close_fds=True)
received_fds = []
try:
    connection, _ = listener.accept()
    with connection:
        connection.settimeout(5)
        pid, uid, gid = struct.unpack('3i', connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
        if uid != 0:
            raise RuntimeError('device helper was not root')
        data, ancillary, flags, _ = connection.recvmsg(1, socket.CMSG_SPACE(array.array('i').itemsize),
                                                       socket.MSG_CMSG_CLOEXEC)
        for level, kind, payload in ancillary:
            if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                descriptors = array.array('i')
                descriptors.frombytes(payload)
                received_fds.extend(descriptors)
        if data != b'V' or flags & socket.MSG_CTRUNC or len(received_fds) != 1:
            raise RuntimeError('invalid device descriptor handoff')
    stdout, stderr = helper.communicate(timeout=10)
    if helper.returncode != 0:
        raise RuntimeError('fixed device-open helper failed')
finally:
    listener.close()
    if helper.poll() is None:
        helper.kill()
        helper.wait(timeout=5)
    (Root / 'fd.sock').unlink(missing_ok=True)

if os.geteuid() != 1001:
    raise RuntimeError('runner must execute as dev')
stop = threading.Event()
probe = {'cid': Cid, 'port': 5242, 'attempts': 0, 'errors': {}, 'reachable': False,
         'execution': None, 'uid': os.geteuid(), 'no_host_acl_changes': True}
errors = collections.Counter()

def probe_listener():
    deadline = time.monotonic() + 230
    while not stop.is_set() and time.monotonic() < deadline:
        peer = socket.socket(socket.AF_VSOCK, socket.SOCK_STREAM)
        peer.settimeout(1)
        try:
            probe['attempts'] += 1
            peer.connect((Cid, 5242))
            probe['reachable'] = True
            peer.close()
            result = subprocess.run(['/home/dev/Waddle-LSW/build/waddle', 'exec',
                                     '--vsock-cid', str(Cid), '--vsock-port', '5242',
                                     '--pipe', '--cwd', 'C:\\', '--timeout', '10',
                                     '--', 'cmd.exe', '/c', 'ver'],
                                    stdin=subprocess.DEVNULL, capture_output=True, timeout=15)
            probe['execution'] = {'exit': result.returncode,
                                  'stdout': result.stdout[:4096].decode(errors='replace'),
                                  'stderr': result.stderr[:4096].decode(errors='replace')}
            return
        except OSError as error:
            errors[str(error.errno)] += 1
        except subprocess.TimeoutExpired:
            probe['execution'] = {'timeout': True}
            return
        finally:
            peer.close()
        stop.wait(2)

worker = threading.Thread(target=probe_listener, name='native-guest-exec-probe')
runner = native_guest_runner_t(runner_config_t(run_seconds=180, startup_seconds=45,
                                              shutdown_seconds=20, guest_vsock_cid=Cid))
try:
    worker.start()
    result = runner.run_with_vhost_vsock_fd(received_fds[0])
    probe['run_root'] = result['run_root']
    probe['protected_inputs_unchanged'] = result['protected_inputs_unchanged']
    probe['forced_shutdown'] = result['forced_shutdown']
except BaseException as error:
    probe['failure'] = str(error)
    probe['run_root'] = str(runner.run_root) if runner.run_root else None
finally:
    stop.set()
    worker.join(timeout=20)
    for descriptor in received_fds:
        os.close(descriptor)
    probe['errors'] = dict(errors)
    probe['probe_joined'] = not worker.is_alive()
    (Root / 'probe.json').write_text(json.dumps(probe, indent=2) + '\n')
    print(json.dumps({'evidence': str(Root / 'probe.json'), **probe}, indent=2), flush=True)
if probe.get('failure'):
    raise SystemExit(1)
