# Unaccepted draft: CLI kill also shuts down the daemon, so the FD-after-kill assertion fails.
# Use only after reviewing checkpoint_2026_10_24.md; create fresh task-owned test roots.
from pathlib import Path
import json,os,signal,socket,subprocess,time
root=Path('/tmp/seamless_native_smoke_root').read_text().strip();root=Path(root)
environment=os.environ.copy();environment.update(XDG_CONFIG_HOME=str(root/'config'),XDG_STATE_HOME=str(root/'state'),XDG_RUNTIME_DIR=str(root/'run'))
waddle=str(Path('build/waddle').resolve())
acl_path=root/'vhost-vsock.acl'
acl_path.write_bytes(subprocess.check_output(['getfacl','-p','/dev/vhost-vsock']))
report={'schema':1,'root':str(root),'guest_os':False,'cycles':[],'limits':['Empty disk boot/QMP/backend attach only','No guest drive binding or read-only write-denial proof']}
daemon_pid=None

def command(*args):
    result=subprocess.run([waddle,*args,'interop-smoke','--json'],env=environment,capture_output=True,text=True,timeout=30)
    with (root/'native_commands.log').open('a') as output:output.write(json.dumps({'args':args,'rc':result.returncode,'stdout':result.stdout,'stderr':result.stderr})+'\n')
    if result.returncode:raise RuntimeError(result.stdout+result.stderr)
    return json.loads(result.stdout)['results'][0]['data']

def qmp(command_name,stream,identifier):
    stream.write((json.dumps({'execute':command_name,'id':identifier})+'\n').encode());stream.flush()
    for _ in range(32):
        raw=stream.readline(65537)
        if not raw or len(raw)>65536:raise RuntimeError('invalid QMP response')
        value=json.loads(raw)
        if value.get('id')==identifier:
            if 'error'in value:raise RuntimeError(str(value['error']))
            return value['return']
    raise RuntimeError('QMP reply count exceeded')

try:
    subprocess.run(['sudo','-n','setfacl','-m',f'u:{os.getuid()}:rw','/dev/vhost-vsock'],check=True)
    for cycle in range(3):
        data=command('start','--no-wait')
        daemon_pid=data['daemon_pid'];qemu_pid=data['qemu_pid']
        qmp_path=root/'run/waddle/interop-smoke/qmp.sock'
        deadline=time.monotonic()+10
        while not qmp_path.exists() and time.monotonic()<deadline:time.sleep(.02)
        with socket.socket(socket.AF_UNIX) as client:
            client.settimeout(5);client.connect(str(qmp_path))
            with client.makefile('rwb') as stream:
                greeting=json.loads(stream.readline(65537))
                if 'QMP'not in greeting:raise RuntimeError('no QMP greeting')
                qmp('qmp_capabilities',stream,1)
                status=qmp('query-status',stream,2)
                pci=qmp('query-pci',stream,3)
                for name in ['waddle_fs_dev_d','waddle_fs_dev_e']:
                    if name not in json.dumps(pci):raise RuntimeError('missing '+name)
        children=[]
        for proc in Path('/proc').iterdir():
            if not proc.name.isdigit():continue
            try:
                stat=(proc/'stat').read_text();parent=int(stat[stat.rfind(')')+2:].split()[1])
                args=(proc/'cmdline').read_bytes().split(b'\0')
                if parent==daemon_pid and any(b'virtiofsd'in arg for arg in args[:1]):children.append((int(proc.name),[a.decode()for a in args if a]))
            except (OSError,ValueError):continue
        if len(children)!=2:raise RuntimeError(f'expected2 FS children, got{children}')
        readonly=[]
        for pid,args in children:
            if '--readonly'in args:readonly.append(pid)
        if len(readonly)!=1:raise RuntimeError(f'expected1 readonly backend: {children}')
        command('kill')
        for pid in [qemu_pid,*[p for p,_ in children]]:
            if Path(f'/proc/{pid}').exists():raise RuntimeError(f'owned child not reaped: {pid}')
        count=len(list(Path(f'/proc/{daemon_pid}/fd').iterdir()))
        report['cycles'].append({'cycle':cycle,'qemu_pid':qemu_pid,'mount_pids':[p for p,_ in children],'readonly_pids':readonly,'qmp':status,'mount_ids':['waddle_fs_dev_d','waddle_fs_dev_e'],'daemon_fds_after_stop':count,'children_reaped':True})
    counts=[cycle['daemon_fds_after_stop']for cycle in report['cycles']]
    if len(set(counts))!=1:raise RuntimeError(f'daemon descriptor growth: {counts}')
    report['status']='passed'
finally:
    try:command('kill')
    except Exception:pass
    if daemon_pid:
        try:
            cmdline=Path(f'/proc/{daemon_pid}/cmdline').read_bytes()
            if str(root/'run').encode()in cmdline:os.kill(daemon_pid,signal.SIGTERM)
        except ProcessLookupError:pass
        except FileNotFoundError:pass
    subprocess.run(['sudo','-n','setfacl','--restore='+str(acl_path)],check=True)
    report['vhost_acl_restored']=subprocess.check_output(['getfacl','-p','/dev/vhost-vsock'])==acl_path.read_bytes()
    (root/'native_receipt.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report))
