#!/usr/bin/env python3
"""Gate source conditional/switch edges in new bounded storage codecs at 90%.

Instrument Zig's ReleaseSafe LLVM IR before native code emission. Keep branches
whose innermost debug scope belongs to an owned codec function. Compiler safety
traps (panic destinations) and line-zero compiler blocks are reported separately;
they are not source error returns. Standard-library parser internals remain Zig's
responsibility; their allocation failures are exercised by the native unit tests.
"""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
mode=sys.argv[1] if len(sys.argv)>1 else 'storage'
assert mode in ('storage','cli')
source='src/daemon/device_storage.zig' if mode=='storage' else 'src/cli/device_commands.zig'
root=Path.cwd(); output=root/'build/coverage/branches'/mode
if output.exists(): shutil.rmtree(output)
output.mkdir(parents=True)
subprocess.run(['zig','test',source,'-D_GNU_SOURCE','-Iinclude','-Isrc/daemon',
 'src/daemon/daemon_device.c',*(['src/daemon/daemon_client.c','src/daemon/daemon_protocol.c','build/device_storage.o'] if mode=='cli' else []),'build/daemon_config.o','-lc','-O','ReleaseSafe','--test-no-exec',
 '-femit-llvm-ir='+str(output/'test.ll'),'-femit-bin='+str(output/'test')],check=True)
ir=(output/'test.ll').read_text()
metadata={int(match[1]):match[2] for match in re.finditer(r'^!(\d+) = (.+)$',ir,re.M)}
def scope_name(index):
    visited=set()
    while index not in visited:
        visited.add(index); value=metadata.get(index,'')
        name=re.search(r'!DISubprogram\(name: "([^"]+)"',value)
        if name:
            file=re.search(r'file: !(\d+)',value)
            filename=metadata.get(int(file[1]),'') if file else ''
            return name[1] if 'filename: "'+Path(source).name+'"' in filename else ''
        parent=re.search(r'scope: !(\d+)',value)
        if not parent: return ''
        index=int(parent[1])
    return ''
codecs={'relative_valid','name_valid','blank_size','settings_valid','parse_json','journal_location_valid'} if mode=='storage' else {'parse','name_valid'}
records=[];instrumented=[];traps=0
# Treat multiline switch instructions as one LLVM instruction.
ir=re.sub(r'^  switch [^\n]+\[\n.*?^  \][^\n]*',lambda match: match[0].replace('\n',' '),ir,flags=re.M|re.S)
# Blocks are inspected before instrumentation to identify compiler panic exits.
blocks={}
function=''; label='entry'
for line in ir.splitlines():
    if line.startswith('define '): function=line;label='entry'
    match=re.match(r'^([\w.]+):',line)
    if match: label=match[1]
    blocks.setdefault((function,label),[]).append(line)
function=''
for line in ir.splitlines():
    if line.startswith('define '): function=line
    location=re.search(r'!dbg !(\d+)',line)
    debug=metadata.get(int(location[1]),'') if location else ''
    lineno=re.search(r'line: (\d+)',debug);scope=re.search(r'scope: !(\d+)',debug)
    owned=scope_name(int(scope[1])) if scope else ''
    column=re.search(r'column: (\d+)',debug)
    branch=re.match(r'\s*br i1 ([\w%.]+), label %([\w.]+), label %([\w.]+)',line)
    # The bounded scanner's token switch is represented by successor groups.
    switch=re.match(r'\s*switch i(\d+) ([\w%.]+), label %([\w.]+) \[',line)
    eligible=owned in codecs and lineno and int(lineno[1])>0
    if eligible and branch:
        targets=branch.groups()[1:3]
        if any(any('panic' in body and 'call ' in body for body in blocks.get((function,target),[])) for target in targets):
            traps+=1
        else:
            index=len(records); records.append({'function':owned,'line':int(lineno[1]),'column':int(column[1]) if column else 0,'edges':2})
            instrumented.append(f'  %coverage_{index} = zext i1 {branch[1]} to i32')
            instrumented.append(f'  call void @waddle_branch_hit(i32 {index}, i32 %coverage_{index})')
    if eligible and switch:
        cases=re.findall(r'i'+switch[1]+r' (-?\d+), label %([\w.]+)',line)
        targets=list(dict.fromkeys([switch[3],*[target for _,target in cases]]))
        if any(any('panic' in body and 'call ' in body for body in blocks.get((function,target),[])) for target in targets):
            traps+=1
        else:
            index=len(records)
            records.append({'function':owned,'line':int(lineno[1]),'column':int(column[1]) if column else 0,'edges':len(targets)})
            previous='0'
            for case_index,(value,target) in enumerate(cases):
                condition=f'%coverage_switch_{index}_{case_index}'
                outcome=f'%coverage_outcome_{index}_{case_index}'
                instrumented.append(f'  {condition} = icmp eq i{switch[1]} {switch[2]}, {value}')
                instrumented.append(f'  {outcome} = select i1 {condition}, i32 {targets.index(target)}, i32 {previous}')
                previous=outcome
            instrumented.append(f'  call void @waddle_branch_hit(i32 {index}, i32 {previous})')
    instrumented.append(line)
# Conditional edges include scalar bounds, nested JSON depth, scanner/typed-parser
# error propagation and successful returns. Token switch outcomes are checked by
# the corpus through both opening/closing and scalar/default paths in that scanner.
instrumented.append('declare void @waddle_branch_hit(i32, i32)')
(output/'instrumented.ll').write_text('\n'.join(instrumented)+'\n')
(output/'branches.json').write_text(json.dumps(records,indent=2))
assert records,'no source branches instrumented'
subprocess.run(['clang-19','-Wno-override-module','-c',str(output/'instrumented.ll'),'-o',str(output/'test.o')],check=True)
subprocess.run(['cc','-D_GNU_SOURCE','-Iinclude','-Isrc/daemon','-O2','-g','-c','src/daemon/daemon_device.c','-o',str(output/'device.o')],check=True)
subprocess.run(['cc',str(output/'test.o'),str(output/'device.o'),'build/daemon_config.o',*(['src/daemon/daemon_client.c','src/daemon/daemon_protocol.c','build/device_storage.o','-D_GNU_SOURCE','-Iinclude','-Isrc/daemon'] if mode=='cli' else []),'tests/device_branch_runtime.c','-o',str(output/'runner')],check=True)
env=os.environ.copy();env['WADDLE_BRANCH_OUT']=str(output)
subprocess.run([str(output/'runner')],env=env,check=True)
hits=set()
for report in output.glob('*.edges'):
    for line in report.read_text().splitlines(): hits.add(tuple(map(int,line.split())))
sites={}
for index,record in enumerate(records):
    key=(record['function'],record['line'],record['column'])
    site=sites.setdefault(key,{'count':0,'hits':set()})
    site['count']=max(site['count'],record['edges'])
    site['hits'].update(edge for edge in range(record['edges']) if (index,edge) in hits)
covered=sum(len(site['hits']) for site in sites.values())
total=sum(site['count'] for site in sites.values())
percent=100*covered/total
print(f'{mode} codec source conditional/switch branches: {percent:.2f}% ({covered}/{total}); {traps} compiler panic guards excluded',flush=True)
missed=[{'function':key[0],'line':key[1],'column':key[2],'edge':edge} for key,site in sites.items() for edge in range(site['count']) if edge not in site['hits']]
(output/'missed.json').write_text(json.dumps(missed,indent=2))
assert percent>=90,('codec branch coverage below 90%',missed)
