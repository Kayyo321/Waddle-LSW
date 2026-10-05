#!/usr/bin/env python3
"""Enforce >=90% implementation line coverage for device registry/storage/CLI.

kcov records optimized ReleaseSafe Zig machine locations and native C locations;
this line gate does not claim source branch coverage. Parser tests are excluded.
"""
import os
from pathlib import Path
import shutil
import subprocess
import xml.etree.ElementTree as xml
root = Path.cwd()
output = root/'build/coverage/device'
if output.exists(): shutil.rmtree(output)
output.mkdir(parents=True)
sources = ['src/daemon/daemon_device.c','src/daemon/device_storage.zig','src/cli/device_commands.zig']
include = '--include-path='+','.join(str(root/source) for source in sources)
runner = output/'runner'
runner.write_text('#!/usr/bin/python3\nimport os,sys,tempfile\nfrom pathlib import Path\n'
                  'destination=tempfile.mkdtemp(prefix="capture-",dir='+repr(str(output))+')\n'
                  'os.execvp("kcov",["kcov","--collect-only",'+repr(include)+',destination,'+repr(str(root/'build/waddle'))+',*sys.argv[1:]])\n')
runner.chmod(0o700)
env = os.environ.copy();env['WADDLE_TEST_EXECUTABLE'] = str(runner)
for suite in ('device_commands','device_storage'):
    subprocess.run(['python3','tests/integration/'+suite+'.py'],env=env,check=True)
subprocess.run(['zig','test','src/cli/device_commands.zig','-D_GNU_SOURCE','-Iinclude','-Isrc/daemon',
                'src/daemon/daemon_device.c','src/daemon/daemon_client.c','src/daemon/daemon_protocol.c',
                'build/device_storage.o','build/daemon_config.o','-lc','-O','ReleaseSafe','--test-no-exec',
                '-femit-bin=build/test_device_commands'],check=True)
for target in ('test_daemon_device','test_device_commands'):
    subprocess.run(['kcov','--collect-only',include if target == 'test_daemon_device' else '--include-path='+str(root/'src/cli/device_commands.zig'),str(output/target),str(root/'build'/target)],check=True)
captures = [str(path) for path in output.iterdir() if path.is_dir()]
subprocess.run(['kcov','--merge',str(output/'merged'),*captures],check=True)
report = xml.parse(output/'merged/kcov-merged/cobertura.xml')
for source in sources:
    path = root/source
    excluded=set(); in_test=False
    for number,line in enumerate(path.read_text().splitlines(),1):
        if line.startswith('test '): in_test=True
        if in_test: excluded.add(number)
        if in_test and line == '}': in_test=False
    lines={}
    for entry in report.findall('.//class'):
        if not entry.attrib['filename'].endswith(source.removeprefix('src/')): continue
        for line in entry.findall('./lines/line'):
            number=int(line.attrib['number'])
            if number not in excluded: lines[number] = max(lines.get(number,0),int(line.attrib['hits']))
    total=len(lines); covered=sum(hits>0 for hits in lines.values())
    assert total, ('missing coverage',source)
    percent=100*covered/total
    print(f'{source}: {percent:.2f}% ({covered}/{total})',flush=True)
    assert percent>=90, ('device implementation coverage below 90%',source,percent)
