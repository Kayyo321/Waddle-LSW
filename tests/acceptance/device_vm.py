#!/usr/bin/env python3
"""Real two-device Windows acceptance; requires dedicated prepared VM fixtures.

Set WADDLE_DEVICE_VM_ACCEPTANCE=1, isolated XDG roots under /tmp/wdvm-*, and
WADDLE_TEST_EXECUTABLE. Pass two booted fixture device names. Both guests need
C:\\waddle\\waddle-guest-exec.exe as an automatic VSOCK listener and an X: export.
The installed image's UEFI/IDE/tag adapter is fixture setup, not a mock VM.
"""
import json
import os
from pathlib import Path
import pty
import select
import subprocess
import sys
import termios
import time

assert os.environ.get('WADDLE_DEVICE_VM_ACCEPTANCE') == '1'
assert len(sys.argv) == 3
for key in ('XDG_CONFIG_HOME', 'XDG_STATE_HOME', 'XDG_RUNTIME_DIR'):
    assert os.environ[key].startswith('/tmp/wdvm-'), 'dedicated temporary VM roots required'
executable = os.environ.get('WADDLE_TEST_EXECUTABLE', str(Path('build/waddle').resolve()))
first, second = sys.argv[1:]
def host(*args, status=0, structured=False):
    result = subprocess.run([executable, *args], capture_output=True, text=True, timeout=200)
    assert result.returncode == status, (args, result.returncode, result.stdout, result.stderr)
    return json.loads(result.stdout) if structured else result.stdout
def guest(name, command, status=0):
    return host('exec','--device',name,'--pipe','--cwd','C:\\','--timeout','60',
                '--','cmd.exe','/c',command,status=status)
left = host('device','show',first,'--json',structured=True)['results'][0]['data']
right = host('device','show',second,'--json',structured=True)['results'][0]['data']
assert left['vsock_cid'] != right['vsock_cid'] and left['disk_image'] != right['disk_image']
assert left['state'] == right['state'] == 'running'
guest(first,'echo FIRST_DEVICE>C:\\waddle\\identity.txt')
guest(second,'echo SECOND_DEVICE>C:\\waddle\\identity.txt')
assert 'FIRST_DEVICE' in guest(first,'type C:\\waddle\\identity.txt')
assert 'SECOND_DEVICE' in guest(second,'type C:\\waddle\\identity.txt')
guest(first,'exit 37',status=37)
guest(second,'exit 42',status=42)
host('device','remove',first,'--yes','--json',status=1)
host('device','config','set',first,'vcpus=4','--json',status=1)
host('device','clone',first,'blocked','--json',status=1)
# Exercise the public named shell, with a working directory in the exported root.
master, slave = pty.openpty()
before = termios.tcgetattr(slave)
frontend = subprocess.Popen([executable,'shell','--device',first], stdin=slave,
                            stdout=slave,stderr=slave,cwd=os.environ['HOME'])
try:
    captured = bytearray()
    deadline = time.monotonic()+60
    sent = False
    while time.monotonic() < deadline and frontend.poll() is None:
        ready,_,_ = select.select([master],[],[],.1)
        if ready:
            captured.extend(os.read(master,65536))
            if not sent and (b'PS ' in captured or b'Windows PowerShell' in captured):
                os.write(master,b"Write-Output ('NAMED_' + 'SHELL_PASS'); exit 23\r")
                sent = True
    assert frontend.wait(timeout=10) == 23, captured
    assert b'NAMED_SHELL_PASS' in captured, captured
    assert termios.tcgetattr(slave) == before, 'named shell must restore terminal mode'
finally:
    if frontend.poll() is None:
        frontend.kill();frontend.wait()
    os.close(master);os.close(slave)
host('stop',first,'--timeout','120','--json',structured=True)
host('stop',second,'--timeout','120','--json',structured=True)
original = Path(left['disk_image']).read_bytes()
host('device','rename',first,'vm_renamed','--json',structured=True)
renamed = host('device','show','vm_renamed','--json',structured=True)['results'][0]['data']
assert renamed['vsock_cid'] == left['vsock_cid']
assert Path(renamed['disk_image']).read_bytes() == original
host('start','vm_renamed','--wait','--timeout','120','--json',structured=True)
host('start',second,'--wait','--timeout','120','--json',structured=True)
assert 'FIRST_DEVICE' in guest('vm_renamed','type C:\\waddle\\identity.txt')
assert 'SECOND_DEVICE' in guest(second,'type C:\\waddle\\identity.txt')
host('stop','--all','--timeout','120','--json',structured=True)
host('device','remove','vm_renamed','--yes','--json',structured=True)
host('device','remove',second,'--yes','--json',structured=True)
assert host('device','list','--json',structured=True)['results'] == []
print('device VM acceptance: two CIDs/independent disks, exit forwarding, busy refusal, named shell/restoration, stop, rename/reboot, markers preserved, sorted batch shutdown/removal passed')
