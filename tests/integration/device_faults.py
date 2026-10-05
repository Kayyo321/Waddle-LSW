#!/usr/bin/env python3
"""Recover every durable move and single injected storage/utility boundary failure."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

executable = os.environ.get('WADDLE_TEST_EXECUTABLE', str(Path('build/waddle').resolve()))
checked = 0

def fixture(root):
    env = os.environ.copy()
    env.update(HOME=str(root), XDG_CONFIG_HOME=str(root/'config'),
               XDG_STATE_HOME=str(root/'state'), XDG_RUNTIME_DIR=str(root/'run'))
    return env

def call(env,*args,**extra):
    return subprocess.run([executable,'device',*args,'--json'],env={**env,**extra},
                          capture_output=True,text=True,timeout=30)

def success(env,*args):
    result=call(env,*args)
    assert result.returncode == 0,(args,result.stdout,result.stderr)
    return json.loads(result.stdout)

cases = {'init':2,'clone':2,'import':2,'export':1,'remove':3,'retain':4,'default':2,'config':2,'rename':5}
for operation,move_count in cases.items():
    def setup(root):
        env=fixture(root)
        success(env,'init','source','--blank-disk','1M')
        success(env,'default','source')
        if operation == 'import': success(env,'export','source','--output',str(root/'backup'))
        args = {
            'init':('init','destination','--blank-disk','1M'),
            'clone':('clone','source','destination'),
            'import':('import','destination','--input',str(root/'backup')),
            'export':('export','source','--output',str(root/'backup')),
            'remove':('remove','source','--yes'),
            'retain':('remove','source','--yes','--keep-data'),
            'default':('default','--clear'),
            'config':('config','set','source','vcpus=8'),
            'rename':('rename','source','destination'),
        }[operation]
        return env,args
    def verify(root,env):
        # Explicit repair acquires writer lock and idempotently completes recovery.
        success(env,'doctor','--all','--repair')
        success(env,'doctor','--all','--repair')
        registered = success(env,'list')['results']
        names = [item['name'] for item in registered]
        assert len(names) == len(set(names))
        cids=[]
        for item in registered:
            data=item['data'];assert data['config_valid'];cids.append(data['vsock_cid'])
            assert Path(data['disk_image']).is_file()
        assert len(cids) == len(set(cids))
        selected=success(env,'default')['results'][0]['data']['default_device']
        assert selected is None or selected in names
        if operation == 'rename': assert names in (['source'],['destination'])
        if operation in ('remove','retain'): assert names in (['source'],[])
        if operation == 'config':
            value=success(env,'config','get','source','vcpus')['results'][0]['data']['vcpus']
            assert value in (4,8)
        for parent in ('config','state'):
            transactions=root/parent/'waddle/transactions'
            assert not transactions.exists() or not list(transactions.iterdir()),(operation,transactions)
        assert not list(root.glob('.waddle-*'))
    # Termination before intent, staging, each journalled move and durable commit.
    # Default clear contains one move; remove includes one default move.
    if operation == 'default': move_count=1
    for point in ['prepared','staged',*[f'move_{i}' for i in range(move_count)],'committed']:
        with tempfile.TemporaryDirectory(prefix='wd-fault-') as temporary:
            root=Path(temporary);env,args=setup(root)
            result=call(env,*args,WADDLE_DEVICE_TEST_CRASH=point)
            assert result.returncode == 99,(operation,point,result.stdout,result.stderr)
            assert call(env,'list').returncode != 0,'reader must not recover pending intent'
            verify(root,env);checked+=1
    # Test each ordinal until it is past the final boundary in this transaction.
    for kind in ('write','fsync','rename','unlink','fork','exec','wait'):
        for ordinal in range(1,100):
            with tempfile.TemporaryDirectory(prefix='wd-fail-') as temporary:
                root=Path(temporary);env,args=setup(root)
                result=call(env,*args,WADDLE_DEVICE_TEST_FAIL=f'{kind}:{ordinal}')
                verify(root,env)
                if result.returncode == 0: break
                checked+=1
        else: raise AssertionError(('unbounded failure injection',operation,kind))
    print(f'device_faults: {operation} recovery and injected I/O/utility boundaries passed',flush=True)
print(f'device_faults: {checked} crash/failure boundaries recovered without mixed state or residual journals')
