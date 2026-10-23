#!/usr/bin/env python3
"""Actual GApplication forwarding in one D-Bus/Xvfb session, with no guest start."""
import os
from pathlib import Path
import subprocess
import tempfile
import time

binary = Path('build/waddle').resolve()
with tempfile.TemporaryDirectory(prefix='waddle-gui-instance-') as temporary:
    root = Path(temporary)
    env = os.environ.copy()
    env.update(XDG_CONFIG_HOME=str(root/'config'), XDG_STATE_HOME=str(root/'state'),
               XDG_RUNTIME_DIR=str(root/'runtime'), GTK_A11Y='none', GSK_RENDERER='cairo')
    (root/'runtime').mkdir(mode=0o700)
    for name in ('first', 'second'):
        subprocess.run([str(binary), 'device', 'init', name, '--blank-disk', '1M', '--json'], env=env, check=True, stdout=subprocess.DEVNULL)
    with (root/'gui.log').open('w') as log:
        first = subprocess.Popen([str(binary)], env=env, stdout=log, stderr=log)
        try:
            def window_named(pattern):
                return subprocess.run(['xdotool', 'search', '--onlyvisible', '--name', pattern], env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline and window_named('first.*Waddle').returncode:
                assert first.poll() is None, (root/'gui.log').read_text()
                time.sleep(0.05)
            assert window_named('first.*Waddle').returncode == 0, (root/'gui.log').read_text()
            second = subprocess.run([str(binary), 'gui', '--device', 'second'], env=env, timeout=10, capture_output=True, text=True)
            assert second.returncode == 0, second.stderr
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and window_named('second.*Waddle').returncode:
                time.sleep(0.05)
            matches = window_named('second.*Waddle')
            assert matches.returncode == 0 and len(matches.stdout.splitlines()) == 1
            assert first.poll() is None
            # Exercise the actual application accelerators and palette by keyboard.
            main_window = matches.stdout.splitlines()[0]
            subprocess.run(['xdotool', 'windowfocus', '--sync', main_window], env=env, check=True)
            subprocess.run(['xdotool', 'key', 'ctrl+k'], env=env, check=True)
            deadline = time.monotonic() + 5
            palette = window_named('^Command palette$')
            while time.monotonic() < deadline and palette.returncode:
                time.sleep(0.05)
                palette = window_named('^Command palette$')
            assert palette.returncode == 0, (root/'gui.log').read_text()
            palette_window = palette.stdout.splitlines()[0]
            subprocess.run(['xdotool', 'windowfocus', '--sync', palette_window], env=env, check=True)
            subprocess.run(['xdotool', 'type', 'Go to Activity'], env=env, check=True)
            time.sleep(0.2)
            subprocess.run(['xdotool', 'key', 'Return'], env=env, check=True)
            time.sleep(0.2)
            assert window_named('^Command palette$').returncode != 0
            subprocess.run(['xdotool', 'windowfocus', '--sync', main_window], env=env, check=True)
            subprocess.run(['xdotool', 'key', 'ctrl+k'], env=env, check=True)
            time.sleep(0.2)
            palette = window_named('^Command palette$')
            assert palette.returncode == 0
            subprocess.run(['xdotool', 'windowfocus', '--sync', palette.stdout.splitlines()[0]], env=env, check=True)
            subprocess.run(['xdotool', 'key', 'Escape'], env=env, check=True)
            time.sleep(0.2)
            assert window_named('^Command palette$').returncode != 0
            assert not list((root/'runtime').rglob('daemon.sock'))
            assert not list((root/'runtime').rglob('qmp.sock'))
        finally:
            first.terminate()
            first.wait(timeout=10)
print('Actual single-instance launch forwarding selects another device without starting a VM.')
