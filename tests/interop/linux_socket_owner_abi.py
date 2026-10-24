#!/usr/bin/env python3
"""Check the ordinary Linux owner object exports exactly its frozen C API.

The runtime fixture includes the unchanged source to reach private scalar
boundaries. This separate ordinary-object check prevents that arrangement from
concealing release-build errors or accidentally exported test entry points.
"""
from pathlib import Path
import re
import subprocess
import sys

Root = Path(__file__).resolve().parents[2]
Expected = {
    'interop_linux_socket_create', 'interop_linux_socket_poll_events',
    'interop_linux_socket_get_closed_snapshot', 'interop_linux_socket_drive',
    'interop_linux_socket_mapping_result', 'interop_linux_socket_begin_drain',
    'interop_linux_socket_abort', 'interop_linux_socket_destroy',
}
Header = Root / 'include/waddle/interop_linux_socket.h'
Declared = set(re.findall(r'\bint\s+(interop_linux_socket_[a-z_]+)\s*\(', Header.read_text()))
if Declared != Expected:
    raise SystemExit('Linux socket owner header differs from the frozen eight-function API')
if len(sys.argv) != 2:
    raise SystemExit('usage: linux_socket_owner_abi.py OWNER_OBJECT')
Symbols = subprocess.check_output(['nm', '--defined-only', '--extern-only', sys.argv[1]], text=True)
Exported = {line.split()[-1] for line in Symbols.splitlines() if line.strip()}
if Exported != Expected:
    raise SystemExit('Linux socket owner exports mismatch: ' + repr(Exported ^ Expected))
print('linux_socket_owner: ordinary production object exports exactly eight frozen C functions')
