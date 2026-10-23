#!/usr/bin/env python3
"""Verify launcher dispatch with the real CLI and an adjacent argv-recording GUI."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

binary = Path('build/waddle').resolve()
with tempfile.TemporaryDirectory(prefix='waddle-gui-dispatch-') as temporary:
    root = Path(temporary)
    env = os.environ.copy()
    env.update(XDG_CONFIG_HOME=str(root/'config'), XDG_STATE_HOME=str(root/'state'),
               XDG_RUNTIME_DIR=str(root/'runtime'), WADDLE_GUI_RECORD=str(root/'record.json'))
    (root/'runtime').mkdir(mode=0o700)
    env.pop('DISPLAY', None)
    env.pop('WAYLAND_DISPLAY', None)
    def run(*args, expected=0, display=False):
        selected = env.copy()
        if display:
            selected['DISPLAY'] = ':99'
        result = subprocess.run([str(root/'waddle'), *args], env=selected, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)
        assert result.returncode == expected, (args, result.returncode, result.stdout, result.stderr)
        return result
    shutil.copy2(binary, root/'waddle')
    shim = root/'waddle-gui'
    shim.write_text('#!/usr/bin/python3\nimport json,os,sys\n'
                    'open(os.environ["WADDLE_GUI_RECORD"],"w").write(json.dumps(sys.argv[1:]))\n')
    shim.chmod(0o700)
    assert 'graphical session' in run(expected=1).stderr
    assert 'waddle repl' in run('gui', expected=1).stderr
    for command in ('repl', 'shell'):
        assert 'repl' in run(command, '--help').stdout
        run(command, '--device', 'not_registered', expected=2)
        run(command, 'alpha', 'beta', expected=2)
    run('--version')
    run('device', 'init', 'alpha', '--blank-disk', '1M', '--json')
    run('device', 'init', 'beta', '--blank-disk', '1M', '--json')
    for args, expected in [((), []), (('alpha',), ['--device', 'alpha']),
                           (('gui',), []), (('gui', '--device', 'beta'), ['--device', 'beta'])]:
        run(*args, display=True)
        assert json.loads((root/'record.json').read_text()) == expected
    run('alpha', 'ignored', expected=2, display=True)
    run('gui', '--device', '../bad', expected=2, display=True)
    devices = json.loads(run('device', 'list', '--json').stdout)
    assert all(item['data']['state'] == 'stopped' for item in devices['results'])
    assert not list((root/'runtime').rglob('daemon.sock')), 'GUI opening started a supervisor'
    shim.unlink()
    assert 'Install waddle-gui beside waddle' in run(expected=1, display=True).stderr
print('Bare/named/explicit GUI dispatch, REPL compatibility, headless and missing-GUI checks passed.')
