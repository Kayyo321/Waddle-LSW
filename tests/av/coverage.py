#!/usr/bin/env python3
"""Gate production lines and conditional/switch edges in AV codecs at 90%.

Instrument Zig's ReleaseSafe LLVM IR before native code emission. Keep branches
whose innermost debug scope belongs to an owned codec function. Compiler safety
traps (panic and stack-canary failure destinations) and line-zero compiler blocks are reported separately;
they are not source error returns. Standard-library parser internals remain Zig's
responsibility; their allocation failures are exercised by the native unit tests.
"""
from datetime import datetime, timezone
import json
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from coverage_recorder import prepare_recorder
ModernOracleNames = ('shader', 'modern_sync', 'extra_objects', 'requirements2_query',
                     'requirements2_reply', 'image_transfer_wire', 'dynamic_rendering_wire',
                     'graphics_general_wire', 'graphics_dynamic_wire', 'sampler_descriptor_wire',
                     'properties_query', 'properties_reply', 'extensions_request', 'extensions_reply')
ModernModeOracles = {
    'venus_shader_wire': ('shader',),
    'venus_pipeline_wire_helpers': ('graphics_general_wire', 'render_wire'),
    'venus_modern_sync_wire': ('modern_sync', 'render_wire'),
    'venus_extra_objects_wire': ('extra_objects', 'render_wire'),
    'venus_requirements2_wire': ('requirements2_query', 'requirements2_reply', 'render_wire'),
    'venus_image_transfer_wire': ('image_transfer_wire', 'render_wire'),
    'venus_dynamic_rendering_wire': ('dynamic_rendering_wire', 'render_wire'),
    'venus_graphics_general_wire': ('graphics_general_wire', 'render_wire'),
    'venus_graphics_dynamic_wire': ('graphics_dynamic_wire', 'render_wire'),
    'venus_sampler_descriptor_wire': ('sampler_descriptor_wire', 'descriptor_wire', 'render_wire'),
}

mode=sys.argv[1]
assert mode in ('av_audio','av_codec','av_layout','av_video','venus_bounds','venus_control','venus_receiver_bounds','venus_request','venus_capabilities','venus_dmabuf','venus_frame','venus_command','venus_objects','venus_values','venus_instance_wire','venus_query_wire','venus_icd','venus_render_wire','venus_descriptor_wire','venus_icd_profiles','venus_graphics_wire','venus_compute_wire','venus_graphics_pipeline_wire','venus_compute_state','venus_graphics_command_wire','venus_features_wire','venus_features_native','venus_tcp_wire','venus_graphics_state','venus_device_wire','venus_device_native','venus_extensions_wire','venus_properties_wire','venus_properties_native','venus_modern_sync_wire','venus_extra_objects_wire','venus_requirements2_wire','venus_image_transfer_wire','venus_dynamic_rendering_wire','venus_graphics_general_wire','venus_graphics_dynamic_wire','venus_sampler_descriptor_wire','venus_transfer2_native','venus_wsi','venus_pipeline_wire_helpers','venus_shader_wire','venus_descriptor_template_native','venus_image_transfer_native','venus_image_view_native')
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
source=('src/vgpu/' if mode in ('venus_bounds','venus_control','venus_receiver_bounds','venus_request','venus_capabilities','venus_dmabuf','venus_frame','venus_command','venus_objects','venus_values','venus_instance_wire','venus_query_wire','venus_icd','venus_render_wire','venus_descriptor_wire','venus_icd_profiles','venus_graphics_wire','venus_compute_wire','venus_graphics_pipeline_wire','venus_compute_state','venus_graphics_command_wire','venus_features_wire','venus_features_native','venus_tcp_wire','venus_graphics_state','venus_device_wire','venus_device_native','venus_extensions_wire','venus_properties_wire','venus_properties_native','venus_modern_sync_wire','venus_extra_objects_wire','venus_requirements2_wire','venus_image_transfer_wire','venus_dynamic_rendering_wire','venus_graphics_general_wire','venus_graphics_dynamic_wire','venus_sampler_descriptor_wire','venus_transfer2_native','venus_wsi','venus_pipeline_wire_helpers','venus_shader_wire','venus_descriptor_template_native','venus_image_transfer_native','venus_image_view_native') else 'src/av/')+mode+'.zig'
root=Path.cwd(); output=root/'build/coverage'/('vgpu' if mode in ('venus_bounds','venus_control','venus_receiver_bounds','venus_request','venus_capabilities','venus_dmabuf','venus_frame','venus_command','venus_objects','venus_values','venus_instance_wire','venus_query_wire','venus_icd','venus_render_wire','venus_descriptor_wire','venus_icd_profiles','venus_graphics_wire','venus_compute_wire','venus_graphics_pipeline_wire','venus_compute_state','venus_graphics_command_wire','venus_features_wire','venus_features_native','venus_tcp_wire','venus_graphics_state','venus_device_wire','venus_device_native','venus_extensions_wire','venus_properties_wire','venus_properties_native','venus_modern_sync_wire','venus_extra_objects_wire','venus_requirements2_wire','venus_image_transfer_wire','venus_dynamic_rendering_wire','venus_graphics_general_wire','venus_graphics_dynamic_wire','venus_sampler_descriptor_wire','venus_transfer2_native','venus_wsi','venus_pipeline_wire_helpers','venus_shader_wire','venus_descriptor_template_native','venus_image_transfer_native','venus_image_view_native') else 'av')/mode
# Retain every qualification attempt, including older modes with existing reports.
# Historical flat reports remain in the module directory and are never replaced.
output = output / ('run-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ') + '-' + str(os.getpid()))
output.mkdir(parents=True, exist_ok=False)
print('preserved ' + mode + ' coverage: ' + str(output), flush=True)
oracle = {'venus_device_wire': 'build/venus_device_wire_oracle.o', 'venus_values': 'build/venus_values_oracle.o', 'venus_instance_wire': 'build/venus_instance_oracle.o', 'venus_query_wire': 'build/venus_query_oracle.o', 'venus_icd': 'build/venus_icd_oracle.o', 'venus_render_wire': 'build/venus_render_wire_oracle.o', 'venus_descriptor_wire': 'build/venus_descriptor_wire_oracle.o', 'venus_graphics_wire': 'build/venus_graphics_wire_oracle.o', 'venus_compute_wire': 'build/venus_compute_wire_oracle.o', 'venus_graphics_pipeline_wire': 'build/venus_graphics_pipeline_wire_oracle.o', 'venus_graphics_command_wire': 'build/venus_graphics_command_wire_oracle.o', 'venus_features_wire': 'build/venus_features_query_oracle.o', 'venus_features_native': 'build/venus_features_query_oracle.o', 'venus_tcp_wire': 'build/venus_tcp_wire_oracle.o'}.get(mode)
if mode == 'venus_device_native':
    for name in ('device', 'render'):
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                        '-Iinclude', '-Itests/vgpu/encoder', '-Ibuild/venus_protocol',
                        '-Isubmodules/venus_protocol/tests', '-Isubmodules/venus_protocol/include',
                        '-c', 'tests/vgpu/' + name + '_wire_oracle.c',
                        '-o', str(output / (name + '_oracle.o'))], check=True)
    oracle = str(output / 'device_oracle.o')
if mode == 'venus_extensions_wire':
    for name, directory in (('wire', 'venus_protocol'), ('reply', 'venus_renderer_protocol')):
        includes = ['-Iinclude', '-Itests/vgpu/encoder', '-Ibuild/' + directory,
                    '-Isubmodules/venus_protocol/tests', '-Isubmodules/venus_protocol/include',
                    '-Isubmodules/venus_protocol/include/vulkan']
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-UNDEBUG',
                        *includes, '-c', 'tests/vgpu/extensions_' + name + '_oracle.c',
                        '-o', str(output / (name + '_oracle.o'))], check=True)
    oracle = str(output / 'wire_oracle.o')
if mode in ('venus_properties_wire', 'venus_properties_native','venus_modern_sync_wire','venus_extra_objects_wire','venus_requirements2_wire','venus_image_transfer_wire','venus_dynamic_rendering_wire','venus_graphics_general_wire','venus_graphics_dynamic_wire','venus_sampler_descriptor_wire','venus_transfer2_native','venus_wsi','venus_pipeline_wire_helpers','venus_shader_wire','venus_descriptor_template_native','venus_image_transfer_native','venus_image_view_native'):
    # Compile exactly three independent named C oracles into this unique attempt;
    # the full imported render unit suite remains linked and executed.
    for name, frontend, directory in (
            ('properties_query', 'properties_query_oracle.c', 'venus_protocol'),
            ('properties_reply', 'properties_reply_oracle.c', 'venus_renderer_protocol'),
            ('properties_render', 'render_wire_oracle.c', 'venus_protocol')):
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-UNDEBUG',
                        '-Iinclude', '-Itests/vgpu', '-Itests/vgpu/encoder', '-Ibuild/' + directory,
                        '-Isubmodules/venus_protocol/tests', '-Isubmodules/venus_protocol/include',
                        '-Isubmodules/venus_protocol/include/vulkan',
                        '-c', 'tests/vgpu/' + frontend, '-o', str(output / (name + '_oracle.o'))], check=True)
    oracle = str(output / 'properties_query_oracle.o')
link_objects = [oracle] if oracle or mode in ModernModeOracles or mode in ('venus_transfer2_native', 'venus_wsi','venus_pipeline_wire_helpers','venus_shader_wire','venus_descriptor_template_native','venus_image_transfer_native','venus_image_view_native') else []
if mode == 'venus_extensions_wire':
    link_objects += [str(output / 'reply_oracle.o')]
if mode in ('venus_properties_wire', 'venus_properties_native','venus_modern_sync_wire','venus_extra_objects_wire','venus_requirements2_wire','venus_image_transfer_wire','venus_dynamic_rendering_wire','venus_graphics_general_wire','venus_graphics_dynamic_wire','venus_sampler_descriptor_wire','venus_transfer2_native','venus_wsi','venus_pipeline_wire_helpers','venus_shader_wire','venus_descriptor_template_native','venus_image_transfer_native','venus_image_view_native'):
    link_objects += [str(output / 'properties_reply_oracle.o'), str(output / 'properties_render_oracle.o')]
if mode in ('venus_device_native', 'venus_device_wire', 'venus_descriptor_wire', 'venus_graphics_wire', 'venus_compute_wire', 'venus_graphics_pipeline_wire', 'venus_graphics_command_wire'):
    link_objects += [str(output / 'render_oracle.o') if mode == 'venus_device_native' else 'build/venus_render_wire_oracle.o']
if mode in ('venus_features_wire', 'venus_features_native'):
    link_objects += ['build/venus_features_reply_oracle.o','build/venus_render_wire_oracle.o']
if mode == 'venus_icd':
    link_objects += ['build/venus_capabilities.o','build/venus_command.o','build/venus_objects.o','build/venus_instance_wire.o',
                     'build/venus_query_wire.o','build/venus_values.o','build/venus_values_oracle.o',
                     'build/venus_features_query_oracle.o','build/venus_features_reply_oracle.o','build/venus_device_wire_oracle.o',
                     'build/venus_render_wire_oracle.o','build/venus_descriptor_wire_oracle.o',
                     'build/venus_compute_wire_oracle.o', 'build/venus_graphics_wire_oracle.o',
                     'build/venus_graphics_pipeline_wire_oracle.o', 'build/venus_graphics_command_wire_oracle.o']
if mode == 'venus_icd':
    link_objects += ['build/venus_' + name + '_oracle.o' for name in ModernOracleNames]
if mode in ModernModeOracles:
    link_objects += ['build/venus_' + name + '_oracle.o' for name in ModernModeOracles[mode]]
# Modern standalone attempts compile a fresh render oracle in their private
# directory. Keep that object once instead of also linking its public equivalent.
if str(output / 'properties_render_oracle.o') in link_objects:
    link_objects = [path for path in link_objects if path != 'build/venus_render_wire_oracle.o']
extra_args = ['-Isubmodules/venus_protocol/include', *link_objects] if oracle or mode in ModernModeOracles or mode in ('venus_transfer2_native', 'venus_wsi','venus_pipeline_wire_helpers','venus_shader_wire','venus_descriptor_template_native','venus_image_transfer_native','venus_image_view_native') else []
test_source = 'src/vgpu/venus_graphics_general_wire.zig' if mode == 'venus_pipeline_wire_helpers' else source
compile_command=['zig','test',test_source,'-Iinclude',*extra_args,'-lc','-O','ReleaseSafe','--test-no-exec',
 '-femit-llvm-ir='+str(output/'test.ll'),'-femit-bin='+str(output/'test')]
source_hash=hashlib.sha256(Path(source).read_bytes()).hexdigest()
subprocess.run(compile_command,check=True)
if hashlib.sha256(Path(source).read_bytes()).hexdigest()!=source_hash:
    raise RuntimeError('coverage source changed during native compilation')

ir=(output/'test.ll').read_text()
raw_ir=ir
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
source_text = Path(source).read_text()
# Legacy modules predate the fixture delimiter. Their exact boundaries keep
# callback/encoder fixture branches out while retaining every runtime helper.
legacy_fixture_boundaries = {
    'venus_capabilities': 'fn fixture_value() venus_capabilities_t {',
    'venus_command': 'const fixture_t = struct {',
    'venus_wsi': 'const fixture_t = struct {',
    'venus_descriptor_template_native': 'fn create_info(entries: []const c.VkDescriptorUpdateTemplateEntry)',
    'venus_image_view_native': 'fn image_info() c.VkImageCreateInfo {',
    'venus_dynamic_rendering_wire': '// Test-only generated encoder oracle.',
    'venus_graphics_dynamic_wire': '// Test-only pinned generated oracles.',
    'venus_graphics_general_wire': '// Test-only independent generated encoder, never linked into production runtime.',
    'venus_image_transfer_wire': '// Test-only generated encoder oracle, never linked into production runtime.',
    'venus_sampler_descriptor_wire': '// Test-only independent pinned encoder entrypoints.',
    'venus_transfer2_native': 'test "every CopyCommands2 family preserves independent named fields at maximum quota"',
}
if mode in legacy_fixture_boundaries:
    boundary = legacy_fixture_boundaries[mode]
    assert '// Test-only fixtures.' not in source_text, 'conflicting legacy fixture boundary'
    assert source_text.count(boundary) == 1, 'missing or ambiguous legacy fixture boundary'
    source_text = source_text.split(boundary, 1)[0]
else:
    source_text = source_text.split('// Test-only fixtures.', 1)[0]
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
(output/'branches.json').write_text(json.dumps(records,indent=2))
# Native target/optimization and the exact records stay unchanged. The runtime
# owns one atomic mask plus arity per record within the original 512KiB budget.
diagnostics={
    'source_sha256': source_hash,
    'compile_command': compile_command,
    'zig_version': subprocess.check_output(['zig','version'],text=True).strip(),
    'clang_version': subprocess.check_output(['clang-19','--version'],text=True).splitlines()[0],
    'target_triple': re.search(r'^target triple = "([^"]+)"',raw_ir,re.M)[1],
    'target_cpus': sorted(set(re.findall(r'"target-cpu"="([^"]+)"',raw_ir))),
    'target_features': sorted(set(re.findall(r'"target-features"="([^"]+)"',raw_ir))),
}
instrumented_text,recorder_arg=prepare_recorder(output,raw_ir,'\n'.join(instrumented)+'\n',records,diagnostics)
(output/'instrumented.ll').write_text(instrumented_text)
print(f'{mode} instrumentation: {len(records)} exact metadata-bound sites; '
      f"{max(record['edges'] for record in records)}/32 maximum edges; "
      f"native target {diagnostics['target_triple']} CPUs {diagnostics['target_cpus']}",flush=True)
subprocess.run(['clang-19','-Wno-override-module','-c',str(output/'instrumented.ll'),'-o',str(output/'test.o')],check=True)
subprocess.run(['zig','cc',str(output/'test.o'),*link_objects,recorder_arg,'tests/device_branch_runtime.c','-o',str(output/'runner')],check=True)
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
