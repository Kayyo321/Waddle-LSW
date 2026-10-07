#!/usr/bin/env python3
"""Gate production lines and conditional/switch edges in AV codecs at 90%.

Instrument Zig's ReleaseSafe LLVM IR before native code emission. Keep branches
whose innermost debug scope belongs to an owned codec function. Compiler safety
traps (panic and stack-canary failure destinations) and line-zero compiler blocks are reported separately;
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
mode=sys.argv[1]
assert mode in ('av_audio','av_codec','av_layout','av_video','venus_bounds','venus_control','venus_receiver_bounds','venus_request','venus_capabilities','venus_dmabuf','venus_frame','venus_command','venus_objects','venus_values','venus_instance_wire','venus_query_wire','venus_icd','venus_render_wire','venus_descriptor_wire','venus_icd_profiles','venus_graphics_wire','venus_compute_wire','venus_graphics_pipeline_wire','venus_compute_state','venus_graphics_command_wire','venus_features_wire','venus_features_native','venus_tcp_wire','venus_graphics_state')
if mode == 'av_video':
    import gzip
    output=Path.cwd()/'build/coverage/av/video'
    if output.exists(): shutil.rmtree(output)
    output.mkdir(parents=True)
    subprocess.run(['cc','-D_GNU_SOURCE','-std=c11','-Iinclude','-Isrc/av','-O0','-g','--coverage','-fprofile-abs-path','-c','src/av/av_video.c','-o',str(output/'video.o')],check=True)
    subprocess.run(['cc','-D_GNU_SOURCE','-std=c11','-Iinclude','-Isrc/av','-Isubmodules/looking_glass/module','tests/av/transport.c','src/av/av_dmabuf.c',str(output/'video.o'),'build/av_audio.o','build/av_layout.o','--coverage','-pthread','-o',str(output/'runner')],check=True)
    subprocess.run([str(output/'runner')],check=True)
    subprocess.run(['gcov','--json-format','-b','-c',str(output/'video.gcno')],cwd=output,check=True)
    report=json.loads(gzip.decompress((output/'video.gcov.json.gz').read_bytes()))
    lines=next(file['lines'] for file in report['files'] if file['file'].endswith('/src/av/av_video.c'))
    branches=[branch for line in lines for branch in line.get('branches',[])]
    for kind,entries in [('line',lines),('branch',branches)]:
        covered=sum(entry['count']>0 for entry in entries)
        percent=100*covered/len(entries)
        print(f'av_video production {kind} coverage: {percent:.2f}% ({covered}/{len(entries)})',flush=True)
        assert percent>=90
    sys.exit(0)
source=('src/vgpu/' if mode in ('venus_bounds','venus_control','venus_receiver_bounds','venus_request','venus_capabilities','venus_dmabuf','venus_frame','venus_command','venus_objects','venus_values','venus_instance_wire','venus_query_wire','venus_icd','venus_render_wire','venus_descriptor_wire','venus_icd_profiles','venus_graphics_wire','venus_compute_wire','venus_graphics_pipeline_wire','venus_compute_state','venus_graphics_command_wire','venus_features_wire','venus_features_native','venus_tcp_wire','venus_graphics_state') else 'src/av/')+mode+'.zig'
root=Path.cwd(); output=root/'build/coverage'/('vgpu' if mode in ('venus_bounds','venus_control','venus_receiver_bounds','venus_request','venus_capabilities','venus_dmabuf','venus_frame','venus_command','venus_objects','venus_values','venus_instance_wire','venus_query_wire','venus_icd','venus_render_wire','venus_descriptor_wire','venus_icd_profiles','venus_graphics_wire','venus_compute_wire','venus_graphics_pipeline_wire','venus_compute_state','venus_graphics_command_wire','venus_features_wire','venus_features_native','venus_tcp_wire','venus_graphics_state') else 'av')/mode
if output.exists(): shutil.rmtree(output)
output.mkdir(parents=True)
oracle = {'venus_values': 'build/venus_values_oracle.o', 'venus_instance_wire': 'build/venus_instance_oracle.o', 'venus_query_wire': 'build/venus_query_oracle.o', 'venus_icd': 'build/venus_icd_oracle.o', 'venus_render_wire': 'build/venus_render_wire_oracle.o', 'venus_descriptor_wire': 'build/venus_descriptor_wire_oracle.o', 'venus_graphics_wire': 'build/venus_graphics_wire_oracle.o', 'venus_compute_wire': 'build/venus_compute_wire_oracle.o', 'venus_graphics_pipeline_wire': 'build/venus_graphics_pipeline_wire_oracle.o', 'venus_graphics_command_wire': 'build/venus_graphics_command_wire_oracle.o', 'venus_features_wire': 'build/venus_features_query_oracle.o', 'venus_features_native': 'build/venus_features_query_oracle.o', 'venus_tcp_wire': 'build/venus_tcp_wire_oracle.o'}.get(mode)
link_objects = [oracle] if oracle else []
if mode in ('venus_descriptor_wire', 'venus_graphics_wire', 'venus_compute_wire', 'venus_graphics_pipeline_wire', 'venus_graphics_command_wire'):
    link_objects += ['build/venus_render_wire_oracle.o']
if mode in ('venus_features_wire', 'venus_features_native'):
    link_objects += ['build/venus_features_reply_oracle.o','build/venus_render_wire_oracle.o']
if mode == 'venus_icd':
    link_objects += ['build/venus_capabilities.o','build/venus_command.o','build/venus_objects.o','build/venus_instance_wire.o',
                     'build/venus_query_wire.o','build/venus_values.o','build/venus_values_oracle.o',
                     'build/venus_render_wire_oracle.o','build/venus_descriptor_wire_oracle.o',
                     'build/venus_compute_wire_oracle.o', 'build/venus_graphics_wire_oracle.o',
                     'build/venus_graphics_pipeline_wire_oracle.o', 'build/venus_graphics_command_wire_oracle.o']
extra_args = ['-Isubmodules/venus_protocol/include', *link_objects] if oracle else []
subprocess.run(['zig','test',source,'-Iinclude',*extra_args,'-lc','-O','ReleaseSafe','--test-no-exec',
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
            return re.sub(r'__anon_\d+$', '', name[1]) if 'filename: "'+Path(source).name+'"' in filename else ''
        parent=re.search(r'scope: !(\d+)',value)
        if not parent: return ''
        index=int(parent[1])
    return ''
source_text = Path(source).read_text().split('// Test-only fixtures.', 1)[0]
codecs=set(re.findall(r'(?:export )?fn (\w+)\(', source_text))
line_sites={}
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
    # Record all matching CFG locations, excluding phi/exception-pad placement.
    if eligible and not re.search(r'= phi |landingpad|catchpad|cleanuppad',line):
        key=(owned,int(lineno[1]))
        if key not in line_sites:
            line_sites[key]=len(records)
            records.append({'kind':'line','function':owned,'line':key[1],'column':0,'edges':1})
        instrumented.append(f'  call void @waddle_branch_hit(i32 {line_sites[key]}, i32 0)')
    if eligible and branch:
        targets=branch.groups()[1:3]
        if any(any(('panic' in body or '@__stack_chk_fail(' in body) and 'call ' in body for body in blocks.get((function,target),[])) for target in targets):
            traps+=1
        else:
            index=len(records); records.append({'function':owned,'line':int(lineno[1]),'column':int(column[1]) if column else 0,'edges':2})
            instrumented.append(f'  %coverage_{index} = zext i1 {branch[1]} to i32')
            instrumented.append(f'  call void @waddle_branch_hit(i32 {index}, i32 %coverage_{index})')
    if eligible and switch:
        cases=re.findall(r'i'+switch[1]+r' (-?\d+), label %([\w.]+)',line)
        targets=list(dict.fromkeys([switch[3],*[target for _,target in cases]]))
        if any(any(('panic' in body or '@__stack_chk_fail(' in body) and 'call ' in body for body in blocks.get((function,target),[])) for target in targets):
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
runtime_source=Path('tests/device_branch_runtime.c').read_text()
max_branches=int(re.search(r'MaxBranches = (\d+)',runtime_source)[1])
max_edges=int(re.search(r'MaxEdges = (\d+)',runtime_source)[1])
assert len(records)<=max_branches, f'coverage sites {len(records)} exceed recorder {max_branches}'
assert max(record['edges'] for record in records)<=max_edges, 'coverage switch exceeds recorder'
print(f'{mode} instrumentation: {len(records)}/{max_branches} sites; '
      f"{max(record['edges'] for record in records)}/{max_edges} maximum edges",flush=True)
subprocess.run(['clang-19','-Wno-override-module','-c',str(output/'instrumented.ll'),'-o',str(output/'test.o')],check=True)
subprocess.run(['zig','cc',str(output/'test.o'),*link_objects,'tests/device_branch_runtime.c','-o',str(output/'runner')],check=True)
env=os.environ.copy();env['WADDLE_BRANCH_OUT']=str(output)
subprocess.run([str(output/'runner')],env=env,check=True)
hits=set()
for report in output.glob('*.edges'):
    for line in report.read_text().splitlines(): hits.add(tuple(map(int,line.split())))
failed=[]
for kind in ('branch','line'):
    sites={}
    for index,record in enumerate(records):
        if record.get('kind','branch')!=kind: continue
        key=(record['function'],record['line'],record['column'])
        site=sites.setdefault(key,{'count':0,'hits':set()})
        site['count']=max(site['count'],record['edges'])
        site['hits'].update(edge for edge in range(record['edges']) if (index,edge) in hits)
    covered=sum(len(site['hits']) for site in sites.values())
    total=sum(site['count'] for site in sites.values())
    percent=100*covered/total
    print(f'{mode} production {kind} coverage: {percent:.2f}% ({covered}/{total}); {traps} compiler panic/stack-canary guards excluded',flush=True)
    missed=[{'function':key[0],'line':key[1],'column':key[2],'edge':edge} for key,site in sites.items() for edge in range(site['count']) if edge not in site['hits']]
    (output/(kind+'_missed.json')).write_text(json.dumps(missed,indent=2))
    if percent < 90: failed.append((kind,missed))
assert not failed,failed
