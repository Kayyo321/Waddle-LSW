#!/usr/bin/python3
"""Deterministic CLI peer: records exact argv, streams output, and persists profiles."""
import json
import os
from pathlib import Path
import signal
import sys
import time

root = Path(os.environ['WADDLE_GUI_TEST_DIR'])
root.mkdir(parents=True, exist_ok=True)
args = sys.argv[1:]
with (root / 'calls.jsonl').open('a') as stream:
    stream.write(json.dumps(args) + '\n')
signal.signal(signal.SIGINT, lambda *_: sys.exit(130))
signal.signal(signal.SIGHUP, lambda *_: sys.exit(129))
path = root / 'profiles.json'
profiles = json.loads(path.read_text()) if path.exists() else ['alpha', 'beta', 'broken']

def record(name, data):
    return dict(name=name, ok=True, data=data, error=None)

def reply(results, ok=True):
    print(json.dumps(dict(schema_version=1, command=args[0], ok=ok,
                         results=results, error=None if ok else {'code': 'partial_failure'})), flush=True)

def data(name):
    return dict(state='running' if name == 'beta' else 'stopped', config_valid=name != 'broken',
                is_default=name == 'alpha', memory_mb=4096, vcpus=4,
                disk_image=str(root / name / 'disk.qcow2'), state_dir=str(root / name))

if args[:2] == ['device', 'list']:
    if (root / 'malformed').exists():
        print('{"schema_version":99,"results":[]}')
    elif (root / 'slow-list').exists():
        time.sleep(0.4)
        reply([record(n, data(n)) for n in profiles])
    else:
        reply([record(n, data(n)) for n in profiles])
elif args[:3] == ['device', 'config', 'get']:
    reply([record(args[3], dict(memory_mb=4096, vcpus=4, default_shell='powershell.exe',
                               vsock_port=5242, start_timeout=60, stop_timeout=15))])
elif args[0] == 'fs':
    reply([record(args[1], dict(mounts=[dict(host_path=str(root), guest_drive='Z:\\', read_only=True)]))])
elif args[0] == 'logs':
    if '--json' in args:
        reply([record(args[1], {'text': 'fixture log: ready\n'})])
    else:
        print('fixture log: ready', flush=True)
    if '--follow' in args:
        while True:
            print('fixture log: update', flush=True)
            time.sleep(0.05)
elif args[0] == 'repl':
    print('FIXTURE_TERMINAL_READY', flush=True)
    while True:
        line = sys.stdin.readline()
        if not line or line.strip() == 'exit':
            print('FIXTURE_TERMINAL_EXIT', flush=True)
            break
        print('TERMINAL_ECHO:' + line, flush=True)
elif args[:2] == ['device', 'init']:
    if '--dry-run' not in args:
        profiles.append(args[2])
        path.write_text(json.dumps(profiles))
    reply([record(args[2], {'dry_run': '--dry-run' in args})])
elif args[:3] == ['device', 'config', 'set']:
    if 'memory_mb=bad' in args:
        print('Configuration validation failed', flush=True)
        sys.exit(2)
    reply([record(args[3], {'applied': True})])
elif args[:2] == ['device', 'doctor'] and '--all' in args:
    reply([record(n, {'healthy': n != 'broken'}) for n in profiles], ok=False)
    sys.exit(1)
elif args[0] == 'exec' and '-Command' in args:
    print('{"pid":1234}', flush=True)
elif args[0] == 'av' and args[1] == 'run':
    print('VIEWER_ATTACHED', flush=True)
elif args[0] == 'start' and args[1] == 'broken':
    print('Device configuration is invalid', flush=True)
    sys.exit(2)
elif args[0] == 'long':
    for i in range(200):
        print(f'progress {i}', flush=True)
        time.sleep(0.05)
elif args[0] == 'large':
    sys.stdout.write('x' * (3 * 1024 * 1024))
elif args[0] == 'fail':
    print('explicit failure', flush=True)
    sys.exit(7)
else:
    reply([record(args[1] if len(args) > 1 and args[0] != 'device' else 'alpha', {'completed': True})])
