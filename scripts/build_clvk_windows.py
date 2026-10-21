#!/usr/bin/env python3
"""Cross-build the pinned clvk OpenCL runtime (with in-process clspv) for Windows.

Inputs are the clean verified submodules (scripts/verify_clvk.sh lists the pins),
the tracked export list scripts/clvk/OpenCL.def and the MinGW-w64/Ninja/CMake
toolchain. The approved tracked retirement patch is applied only to a private
copy. MinGW accommodations use include paths and linker flags. Upstream
submodule trees stay unmodified.

Outputs: a fresh ignored private build tree under build/clvk_client/ and the
receipt build/clvk_client.json (pins, tools, settings, driver/export-list hashes,
DLL hash and import list), published atomically under a process lock. Earlier
build trees and receipts are preserved. The shared native LLVM tool build lives in
build/clvk_client/native_<key>/ and is reused (incrementally) while its inputs match.

Cross-compilation is not execution: the DLL is unexecuted and unverified here.
No Rust or Go participates in this build.
"""
import fcntl
import argparse
import hashlib
import io
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import tarfile

Pins = (
    ('submodules/clvk', '8bd3672174764dce25ba220773e10baa752f0373'),
    ('submodules/clvk/external/clspv', '7ee3a40e652b3649d175758f72d7af98b9450a68'),
    ('submodules/clvk/external/OpenCL-Headers', '386ca390b2f52efeb3e1a55a500690eb8013f60e'),
    ('submodules/clvk/external/SPIRV-Headers', '2b7475f6d664efbad4d4315c94ecec9640831e5b'),
    ('submodules/clvk/external/SPIRV-Tools', 'ba9a9e32160e2500ce1e48cc563a8fedaf596e6e'),
    ('submodules/clvk/external/SPIRV-LLVM-Translator', 'c4a6b42303c6b45a64ede3d1c0399dce2d90f3fc'),
    ('submodules/llvm_project', '71a85ef89fc1997205104b72447e0dc2c4ad83da'),
    ('submodules/clspv_spirv_headers', 'b824a462d4256d720bebb40e78b9eb8f78bbb305'),
    ('submodules/clspv_spirv_tools', 'fb7471844504abb16b732dc4fc0837119a32ec24'),
)
# Private copies of the clvk tree skip VCS metadata; large third-party trees are symlinked.
CopyIgnore = shutil.ignore_patterns('.git', '.github')
ForbiddenImports = ('libgcc_s', 'libstdc++', 'libwinpthread')
RequiredExports = ('clGetPlatformIDs', 'clBuildProgram', 'clCreateProgramWithSource',
                   'clCreateKernel', 'clEnqueueNDRangeKernel', 'clFinish', 'clIcdGetPlatformIDsKHR',
                   'waddle_clvk_shutdown')
PrivateExports = ('waddle_clvk_shutdown',)
PrivatePatches = ('patches/clvk/0001-explicit-quiescent-windows-shutdown.patch',
                  'patches/clvk/0002-validate-one-dimensional-buffer-commands.patch',
                  'patches/clvk/0003-release-failed-buffer-map-command.patch')
LlvmSwitches = ('-DLLVM_INCLUDE_TESTS=OFF', '-DLLVM_INCLUDE_BENCHMARKS=OFF',
                '-DLLVM_INCLUDE_EXAMPLES=OFF', '-DLLVM_INCLUDE_DOCS=OFF',
                '-DCLANG_INCLUDE_TESTS=OFF', '-DCLANG_INCLUDE_DOCS=OFF',
                '-DLLVM_ENABLE_ZLIB=OFF', '-DLLVM_ENABLE_ZSTD=OFF', '-DLLVM_ENABLE_LIBXML2=OFF',
                '-DLLVM_ENABLE_TERMINFO=OFF', '-DLLVM_ENABLE_LIBEDIT=OFF',
                '-DLLVM_ENABLE_ASSERTIONS=OFF')

ArchiveNames = ('libgcc.a', 'libgcc_eh.a', 'libstdc++.a', 'libmingw32.a',
                'libmsvcrt.a', 'dllcrt2.o', 'crtbegin.o', 'crtend.o')
TlsLimits = {'max_objects': 32, 'max_bytes': 192, 'max_align': 16}
ShimSources = ('src/vgpu/clvk_runtime_tls.c', 'src/vgpu/clvk_runtime_eh_pool.c')
AuditorPath = 'scripts/clvk_runtime_audit.py'
ProbeExports = ('runtime_foreign_thread_probe', 'runtime_lifetime_probe')


def digest(path):
    """Return SHA-256 of a borrowed existing file's bytes without mutation."""
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(command, **options):
    """Run one owned child process; raise on a nonzero exit.

    Parameters are an argv list and subprocess keywords; no shell is used.
    """
    print('+', ' '.join(str(item) for item in command), flush=True)
    subprocess.run([str(item) for item in command], check=True, **options)


def capture(command):
    """Return stripped stdout of a short owned child process; raise on failure."""
    return subprocess.check_output([str(item) for item in command], text=True).strip()


def jobs():
    """Return the bounded parallel job count (WADDLE_CLVK_JOBS, default min(cpus,6))."""
    return str(int(os.environ.get('WADDLE_CLVK_JOBS', min(os.cpu_count() or 1, 6))))


def load_auditor(root):
    """Load the repository-owned static runtime admission helper."""
    path = root / AuditorPath
    spec = importlib.util.spec_from_file_location('clvk_runtime_audit', path)
    if spec is None or spec.loader is None:
        raise SystemExit('cannot load runtime admission helper: ' + str(path))
    module = importlib.util.module_from_spec(spec)
    module_name = spec.name
    previous = sys.modules.get(module_name)
    sys.modules[module_name] = module
    try:
        spec.loader.exec_module(module)
    except BaseException:
        if previous is None:
            sys.modules.pop(module_name, None)
        else:
            sys.modules[module_name] = previous
        raise
    return module


def resolved_tool(path):
    """Resolve an explicit executable selector and require a regular file."""
    candidate = Path(path).expanduser()
    resolved = candidate.resolve(strict=True)
    if not resolved.is_file():
        raise SystemExit('selected tool is not a file: ' + str(resolved))
    if not os.access(resolved, os.X_OK):
        raise SystemExit('selected tool is not executable: ' + str(resolved))
    return resolved


def _compiler_record(path):
    """Capture identity for one selected compiler without building anything."""
    executable = resolved_tool(path)
    version = capture([executable, '--version']).splitlines()[0]
    dumpfullversion = capture([executable, '-dumpfullversion'])
    dumpmachine = capture([executable, '-dumpmachine'])
    # GCC prints the thread model to stderr for -v; do not depend on a shell or
    # on PATH aliases when identifying the selected compiler.
    result = subprocess.run([str(executable), '-v'], text=True, capture_output=True,
                            check=True, timeout=30)
    model = re.search(r'^Thread model:\s*(\S+)\s*$', result.stderr, re.MULTILINE)
    if not model:
        raise SystemExit('compiler does not report a thread model: ' + str(executable))
    return {'resolved_path': str(executable), 'sha256': digest(executable), 'version': version,
            'dumpfullversion': dumpfullversion, 'dumpmachine': dumpmachine,
            'thread_model': model.group(1)}


def _compiler_path(compiler, name):
    """Ask an explicit compiler for a runtime file and reject unresolved names."""
    reported = capture([compiler, '-print-file-name=' + name])
    path = Path(reported)
    if not path.is_absolute():
        raise SystemExit('compiler could not resolve runtime input ' + name + ': ' + reported)
    resolved = path.resolve(strict=True)
    if not resolved.is_file():
        raise SystemExit('compiler runtime input is not a file ' + name + ': ' + str(resolved))
    return resolved


def _critical_headers(cxx):
    """Capture the complete include closure for the runtime-facing C++ headers."""
    source = '#include <thread>\n#include <mutex>\n#include <exception>\n'
    result = subprocess.run([str(cxx), '-std=c++17', '-M', '-x', 'c++', '-'],
                            input=source, text=True, capture_output=True, check=True,
                            timeout=30)
    flattened = result.stdout.replace('\\\n', ' ')
    dependencies = flattened.split(':', 1)[1].split()
    headers = {}
    for item in dependencies:
        if item == '-':
            continue
        path = Path(item).resolve(strict=True)
        if path.is_file():
            headers[str(path)] = digest(path)
    if not headers:
        raise SystemExit('selected C++ compiler returned an empty critical-header closure')
    return dict(sorted(headers.items()))


def _compiler_programs(compiler):
    """Capture GCC backend executable identities used by selected compiler."""
    programs = {}
    for name in ('cc1', 'cc1plus', 'collect2', 'as', 'ld'):
        reported = capture([compiler, '-print-prog-name=' + name])
        candidate = Path(reported)
        if not candidate.is_absolute():
            candidate = Path(compiler).parent / candidate
        executable = resolved_tool(candidate)
        result = subprocess.run([str(executable), '--version'], text=True, capture_output=True,
                                check=True, timeout=30)
        version_output = (result.stdout or result.stderr).splitlines()
        programs[name] = {'resolved_path': str(executable), 'sha256': digest(executable),
                          'version': version_output[0] if version_output else None}
    return programs


def capture_toolchain_identity(cc, cxx):
    """Return compiler, backend, target, archive, and critical-header identities."""
    c_compiler = resolved_tool(cc)
    cxx_compiler = resolved_tool(cxx)
    archives = {}
    for name in ArchiveNames:
        path = _compiler_path(cxx_compiler, name)
        archives[name] = {'resolved_path': str(path), 'sha256': digest(path)}
    return {'target': 'x86_64-w64-mingw32', 'cc': _compiler_record(c_compiler),
            'cxx': _compiler_record(cxx_compiler),
            'backend_programs': {'cc': _compiler_programs(c_compiler),
                                 'cxx': _compiler_programs(cxx_compiler)},
            'runtime_archives': archives,
            'critical_headers': _critical_headers(cxx_compiler)}


def verify_package_provenance(identity, provenance_path, extra_tools=None):
    """Bind private extracted GCC inputs to reviewed Debian package archives.

    Provenance JSON schema: {"schema":1,"package_receipt_path":...,
    "extracted_root":...,"packages":[{"name":...,"version":...,
    "deb_path":...,"sha256":...}]}. Every compiler, backend, runtime archive,
    and critical header must be byte-identical to a member of one declared .deb.
    Host binutils are recorded by exact path/hash/version but are not misidentified
    as package members when they live outside the private extraction.
    """
    provenance = _load_reviewed_json(provenance_path, 'toolchain package provenance')
    if set(provenance) != {'schema', 'package_receipt_path', 'extracted_root', 'packages'} or \
       provenance.get('schema') != 1:
        raise SystemExit('toolchain provenance must use schema 1 with exact required fields')
    try:
        receipt = json.loads(Path(provenance['package_receipt_path']).read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise SystemExit('cannot read toolchain package receipt: ' + str(exc)) from exc
    if not isinstance(receipt, list):
        raise SystemExit('toolchain package receipt must be a package list')
    receipt_by_name = {item.get('package'): item for item in receipt if isinstance(item, dict)}
    root = Path(provenance['extracted_root']).resolve(strict=True)
    declared = provenance['packages']
    if not isinstance(declared, list) or not declared:
        raise SystemExit('toolchain provenance must declare package archives')
    archives = {}
    for package in declared:
        if not isinstance(package, dict) or set(package) != {'name', 'version', 'deb_path', 'sha256'}:
            raise SystemExit('toolchain provenance package entry has invalid fields')
        name = package['name']
        receipt_item = receipt_by_name.get(name)
        archive = Path(package['deb_path']).resolve(strict=True)
        if not receipt_item or package['version'] != receipt_item.get('version') or \
           package['sha256'] != receipt_item.get('sha256') or \
           receipt_item.get('matched_apt_metadata') is not True or \
           receipt_item.get('host_installed') is not False or \
           digest(archive) != package['sha256']:
            raise SystemExit('toolchain package provenance does not match receipt/archive: ' + str(name))
        actual_name = capture(['dpkg-deb', '-f', archive, 'Package'])
        actual_version = capture(['dpkg-deb', '-f', archive, 'Version'])
        if (actual_name, actual_version) != (name, package['version']):
            raise SystemExit('toolchain package metadata mismatch: ' + str(name))
        contents = subprocess.run(['dpkg-deb', '--fsys-tarfile', str(archive)],
                                  capture_output=True, check=True, timeout=60).stdout
        members = {}
        with tarfile.open(fileobj=io.BytesIO(contents), mode='r:') as archive_tar:
            tar_members = archive_tar.getmembers()
            for member in tar_members:
                if member.isfile():
                    stream = archive_tar.extractfile(member)
                    if stream is not None:
                        members[member.name.removeprefix('./')] = hashlib.sha256(stream.read()).hexdigest()
            for member in tar_members:
                if member.islnk():
                    target = member.linkname.removeprefix('./')
                    if target in members:
                        members[member.name.removeprefix('./')] = members[target]
        archives[name] = members

    private_inputs = {}
    private_inputs.update({'cc': identity['cc'], 'cxx': identity['cxx']})
    for model in ('cc', 'cxx'):
        private_inputs.update({model + ':' + name: value
                               for name, value in identity['backend_programs'][model].items()
                               if name in ('cc1', 'cc1plus', 'collect2')})
    private_inputs.update({'archive:' + name: value for name, value in identity['runtime_archives'].items()})
    private_inputs.update({'header:' + name: {'resolved_path': name, 'sha256': value}
                           for name, value in identity['critical_headers'].items()})
    proof = {}
    external_tools = {}
    for label, item in private_inputs.items():
        path = Path(item['resolved_path']).resolve(strict=True)
        try:
            relative = path.relative_to(root).as_posix()
        except ValueError as exc:
            raise SystemExit('private toolchain input is outside the reviewed extraction: ' + str(path)) from exc
        matching = [name for name, members in archives.items()
                    if members.get(relative) == item['sha256']]
        if not matching:
            raise SystemExit('toolchain input lacks exact package-member proof: ' + label + ' (' + relative + ')')
        proof[label] = {'relative_path': relative, 'sha256': item['sha256'],
                        'package': matching[0]}
    external_programs = {}
    for model in ('cc', 'cxx'):
        for name, item in identity['backend_programs'][model].items():
            if name in ('cc1', 'cc1plus', 'collect2'):
                continue
            external_programs[model + ':' + name] = dict(item)
    for label, item in (extra_tools or {}).items():
        path = Path(item['resolved_path']).resolve(strict=True)
        try:
            relative = path.relative_to(root).as_posix()
        except ValueError:
            external_tools[label] = dict(item)
            continue
        matching = [name for name, members in archives.items()
                    if members.get(relative) == item['sha256']]
        if not matching:
            raise SystemExit('selected private build tool lacks exact package-member proof: ' + label)
        proof['tool:' + label] = {'relative_path': relative, 'sha256': item['sha256'],
                                  'package': matching[0]}
    return {'provenance': provenance, 'provenance_sha256': digest(provenance_path),
            'package_receipt_sha256': digest(provenance['package_receipt_path']),
            'package_member_proof': proof, 'external_backend_programs': external_programs,
            'external_build_tools': external_tools}


def source_identity(root):
    """Hash all runtime-driver inputs whose changes affect binary admission."""
    paths = (*ShimSources, 'tests/vgpu/clvk_runtime_windows.c',
             'tests/vgpu/clvk_runtime_windows.cpp', 'scripts/clvk/OpenCL.def',
             'tests/vgpu/clvk_runtime_link_test.py', 'scripts/clvk/runtime_sections.ld',
             'include/waddle/clvk.h', AuditorPath, 'scripts/build_clvk_windows.py',
             *PrivatePatches)
    return {name: digest(root / name) for name in paths}


def write_json_atomic(path, record):
    """Atomically replace one caller-owned JSON record after complete success."""
    destination = Path(path)
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + '.tmp')
    temporary.write_text(json.dumps(record, indent=2, sort_keys=True) + '\n')
    os.replace(temporary, destination)


def _load_reviewed_json(path, label):
    """Load a required reviewed JSON object and reject empty or malformed input."""
    try:
        value = json.loads(Path(path).read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise SystemExit('cannot read reviewed ' + label + ': ' + str(exc)) from exc
    if not isinstance(value, dict) or not value:
        raise SystemExit('reviewed ' + label + ' must be a nonempty JSON object')
    return value


def build_admission(root, dll_path, map_path, *, nm, map_working_directory, runtime_policy,
                    expected_output_name='libOpenCL.dll', expected_exports=None):
    """Run both static admission checks on one exact freshly linked DLL/map."""
    audit = load_auditor(root)
    link = audit.verify_runtime_link(map_path, dll_path,
                                     expected_output_name=expected_output_name,
                                     shim_object='clvk_runtime_tls.o',
                                     eh_pool_object='clvk_runtime_eh_pool.o',
                                     runtime_policy=runtime_policy,
                                     map_working_directory=map_working_directory)
    image = audit.audit_runtime_tls(dll_path, nm=nm, limits=TlsLimits,
                                    expected_exports=(expected_exports or
                                                      read_export_list(root / 'scripts/clvk/OpenCL.def')),
                                    runtime_policy=runtime_policy)
    return {'link': link, 'runtime': image}


def _required_tool(name):
    """Resolve one non-compiler build tool from PATH before the build starts."""
    found = shutil.which(name)
    if not found:
        raise SystemExit('required build tool is missing: ' + name)
    return resolved_tool(found)


def _tool_record(path):
    """Capture a selected helper executable's path, digest, and version string."""
    executable = resolved_tool(path)
    return {'resolved_path': str(executable), 'sha256': digest(executable),
            'version': capture([executable, '--version']).splitlines()[0]}


def _validate_runtime_policy(runtime_policy, label):
    """Validate exact caller-owned API lists without deriving them from binaries."""
    required_policy_keys = {'allowed_api_callers', 'tls_pairs', 'dynamic_lookup_callers'}
    if set(runtime_policy) != required_policy_keys:
        raise SystemExit(label + ' must contain exactly ' + ', '.join(sorted(required_policy_keys)))
    allowed = runtime_policy['allowed_api_callers']
    if not isinstance(allowed, dict) or set(allowed) != {'TlsAlloc', 'FlsAlloc', 'DuplicateHandle'}:
        raise SystemExit(label + ' must enumerate exact TlsAlloc, FlsAlloc, and DuplicateHandle caller sets')
    for api, callers in allowed.items():
        if not isinstance(callers, list) or any(not isinstance(item, str) or not item for item in callers) or \
           len(callers) != len(set(callers)):
            raise SystemExit(label + ' caller list is invalid: ' + api)
    pairs = runtime_policy['tls_pairs']
    if not isinstance(pairs, dict) or any(not isinstance(key, str) or not key or
                                          not isinstance(value, str) or not value
                                          for key, value in pairs.items()):
        raise SystemExit(label + ' TLS pair names must be nonempty strings')
    if set(pairs) != set(allowed['TlsAlloc']):
        raise SystemExit(label + ' TLS pair keys must exactly match its TlsAlloc caller set')
    dynamic = runtime_policy['dynamic_lookup_callers']
    if not isinstance(dynamic, list) or any(not isinstance(item, str) or not item for item in dynamic) or \
       len(dynamic) != len(set(dynamic)):
        raise SystemExit(label + ' dynamic lookup caller list is invalid')
    return runtime_policy


def _native_cache_key(inputs):
    """Key host LLVM tools only from host compilers/tools, pin, and switches."""
    host_tool_names = ('host_cc', 'host_cxx', 'cmake', 'ninja')
    identity = {'llvm': dict(Pins)['submodules/llvm_project'],
                'host_tools': {name: inputs['build_tools'][name] for name in host_tool_names},
                'switches': LlvmSwitches}
    return hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()


def _receipt_path(root, output_root):
    """Keep the canonical receipt path while allowing isolated custom roots."""
    if output_root == 'build/clvk_client':
        return root / 'build/clvk_client.json'
    return (root / output_root).resolve() / 'clvk_client.json'


def _preflight(root, args):
    """Validate all identities and inputs before any expensive build step."""
    cc = resolved_tool(args.cc)
    cxx = resolved_tool(args.cxx)
    dlltool = resolved_tool(args.dlltool)
    objdump = resolved_tool(args.objdump)
    nm = resolved_tool(args.nm)
    windres = resolved_tool(args.windres)
    ar = resolved_tool(args.ar)
    ranlib = resolved_tool(args.ranlib)
    actual_identity = capture_toolchain_identity(cc, cxx)
    core_provenance = verify_package_provenance(actual_identity, args.toolchain_provenance)
    actual_identity['package_provenance'] = core_provenance
    audit = load_auditor(root)
    reviewed_identity = _load_reviewed_json(args.reviewed_toolchain, 'toolchain identity')
    identity_result = audit.verify_toolchain_identity(actual_identity, reviewed_identity)
    runtime_policy = _load_reviewed_json(args.runtime_policy, 'runtime policy')
    _validate_runtime_policy(runtime_policy, 'reviewed runtime policy')
    probe_runtime_policy = _load_reviewed_json(args.probe_runtime_policy,
                                               'probe runtime policy')
    _validate_runtime_policy(probe_runtime_policy, 'reviewed probe runtime policy')
    selected_tools = {'dlltool': _tool_record(dlltool), 'objdump': _tool_record(objdump),
                      'nm': _tool_record(nm), 'windres': _tool_record(windres),
                      'ar': _tool_record(ar), 'ranlib': _tool_record(ranlib),
                      'cmake': _tool_record(_required_tool('cmake')),
                      'ninja': _tool_record(_required_tool('ninja')),
                      'host_cc': _tool_record(_required_tool('cc')),
                      'host_cxx': _tool_record(_required_tool('c++'))}
    provenance_result = verify_package_provenance(actual_identity, args.toolchain_provenance,
                                                  selected_tools)
    for compiler in ('cc', 'cxx'):
        identity = actual_identity[compiler]
        if identity['thread_model'] != 'win32':
            raise SystemExit('selected compiler is not MinGW win32-thread-model: ' + compiler)
        if identity['dumpmachine'] != 'x86_64-w64-mingw32':
            raise SystemExit('selected compiler must target x86_64-w64-mingw32')
    for name in (*ShimSources, AuditorPath, 'scripts/clvk/OpenCL.def', 'include/waddle/clvk.h',
                 *PrivatePatches):
        if not (root / name).is_file():
            raise SystemExit('missing required build input: ' + name)
    inputs = {'source_hashes': source_identity(root), 'toolchain_identity': identity_result,
              'toolchain_provenance': provenance_result,
              'toolchain_provenance_path': str(Path(args.toolchain_provenance).resolve()),
              'build_tools': selected_tools, 'tls_limits': dict(TlsLimits),
              'runtime_policy': runtime_policy, 'runtime_policy_sha256': digest(args.runtime_policy),
              'runtime_policy_path': str(Path(args.runtime_policy).resolve()),
              'probe_runtime_policy': probe_runtime_policy,
              'probe_runtime_policy_sha256': digest(args.probe_runtime_policy),
              'probe_runtime_policy_path': str(Path(args.probe_runtime_policy).resolve()),
              'pins': dict(Pins),
              'private_patches': {name: digest(root / name) for name in PrivatePatches},
              'export_list_sha256': digest(root / 'scripts/clvk/OpenCL.def'),
              'shutdown_abi_sha256': digest(root / 'include/waddle/clvk.h')}
    return {'cc': cc, 'cxx': cxx, 'dlltool': dlltool, 'objdump': objdump, 'nm': nm,
            'windres': windres, 'ar': ar, 'ranlib': ranlib,
            'runtime_policy': runtime_policy, 'probe_runtime_policy': probe_runtime_policy,
            'inputs': inputs}


def _verify_inputs_unchanged(root, inputs):
    """Refuse publication if source, runtime policy, patches, or exports changed."""
    if source_identity(root) != inputs['source_hashes']:
        raise SystemExit('OpenCL runtime source/tool contract changed during the build')
    if digest(inputs['runtime_policy_path']) != inputs['runtime_policy_sha256']:
        raise SystemExit('reviewed OpenCL runtime policy changed during the build')
    if digest(inputs['probe_runtime_policy_path']) != inputs['probe_runtime_policy_sha256']:
        raise SystemExit('reviewed OpenCL probe runtime policy changed during the build')
    identity = inputs['toolchain_identity']['identity']
    current_identity = capture_toolchain_identity(identity['cc']['resolved_path'],
                                                  identity['cxx']['resolved_path'])
    core_provenance = verify_package_provenance(current_identity,
                                                inputs['toolchain_provenance_path'])
    provenance = verify_package_provenance(current_identity,
                                           inputs['toolchain_provenance_path'],
                                           inputs['build_tools'])
    current_identity['package_provenance'] = core_provenance
    if current_identity != identity:
        raise SystemExit('selected OpenCL compiler, archive, or header identity changed during the build')
    if provenance != inputs['toolchain_provenance']:
        raise SystemExit('selected OpenCL compiler package provenance changed during the build')
    for tool in inputs['build_tools'].values():
        if digest(tool['resolved_path']) != tool['sha256']:
            raise SystemExit('selected OpenCL build tool changed during the build: ' + tool['resolved_path'])
    if inputs['export_list_sha256'] != digest(root / 'scripts/clvk/OpenCL.def') or \
       inputs['shutdown_abi_sha256'] != digest(root / 'include/waddle/clvk.h') or \
       inputs['private_patches'] != {name: digest(root / name) for name in PrivatePatches}:
        raise SystemExit('OpenCL export, ABI header, or patch inputs changed during the build')


def _admit_recorded_runtime(root, record, *, nm, runtime_policy):
    """Recheck cached map/image provenance and both admission gates on reuse."""
    linked = Path(record['linked_dll']['path'])
    link_map = Path(record['link_map']['path'])
    published = Path(record['dll']['path'])
    for label, path, expected in (('linked DLL', linked, record['linked_dll']['sha256']),
                                  ('link map', link_map, record['link_map']['sha256']),
                                  ('published DLL', published, record['dll']['sha256'])):
        if not path.is_file() or digest(path) != expected:
            raise SystemExit('cached ' + label + ' hash or file mismatch')
    if digest(linked) != digest(published):
        raise SystemExit('cached published DLL differs from admitted linked DLL')
    admission = build_admission(root, linked, link_map, nm=nm,
                                map_working_directory=record['link_map']['working_directory'],
                                runtime_policy=runtime_policy)
    return admission


def publish_admitted_record(root, receipt, record, inputs, *, nm, runtime_policy):
    """Recheck fresh provenance and atomically replace the receipt only on pass."""
    _verify_inputs_unchanged(root, inputs)
    admission = _admit_recorded_runtime(root, record, nm=nm, runtime_policy=runtime_policy)
    if admission['runtime']['dll_sha256'] != record['dll']['sha256']:
        raise SystemExit('final DLL admission digest differs from publication record')
    _verify_inputs_unchanged(root, inputs)
    write_json_atomic(receipt, record)
    return admission


def linker_flags(runtime_objects, export_file, link_map):
    """Build explicit link flags with Waddle objects ahead of runtime archives."""
    ordered = [*runtime_objects, export_file]
    prefix = ' '.join(shlex.quote(str(item)) for item in ordered)
    root = Path(__file__).resolve().parents[1]
    required_fences = ('-Wl,--gc-sections',
                       '-Wl,--undefined=waddle_clvk_runtime_tls_limits',
                       '-Wl,-T,' + shlex.quote(str(root / 'scripts/clvk/runtime_sections.ld')),
                       '-Wl,--wrap=__gthr_win32_equal',
                       '-Wl,--wrap=_ZSt16__at_thread_exitPSt20__at_thread_exit_elt',
                       '-Wl,--wrap=_ZSt25notify_all_at_thread_exitRSt18condition_variableSt11unique_lockISt5mutexE')
    return ' '.join((prefix, '-mthreads', '-static-libgcc', '-static-libstdc++',
                     *required_fences,
                     '-Wl,-Map=' + shlex.quote(str(link_map)) + ',--demangle'))


def read_export_list(path):
    """Return the ordered export names listed in a module-definition file."""
    return [line.strip() for line in Path(path).read_text().splitlines() if line.startswith('    ')]


def check_export_list_against_api(root, exports):
    """Fail closed when the export list and clvk's api.cpp definitions disagree.

    Every `CLVK_API_CALL cl*` function defined by api.cpp must be exported, and every
    exported name must be defined there. Only the exact clspv and private
    retirement entry points are exempt; private exports remain mandatory.
    """
    source = (root / 'submodules/clvk/src/api.cpp').read_text()
    defined = set(re.findall(r'CLVK_API_CALL\s+(cl\w+)\s*\(', source))
    exported = set(exports) - set(PrivateExports) - {'clspvCompileFromSourcesString', 'clspvFreeOutputBuildObjs'}
    missing = sorted((defined | set(PrivateExports)) - set(exports))
    extra = sorted(exported - defined)
    if missing or extra or len(exports) != len(set(exports)):
        raise SystemExit('scripts/clvk/OpenCL.def disagrees with api.cpp: missing=%s extra=%s' % (missing, extra))


def prepare_source(root, source):
    """Create the private source tree: copied clvk, symlinked large third-party trees.

    [in] root repository; [out] source new directory owned by the caller. Submodules
    stay unmodified; clspv's third_party layout is reproduced with symlinks.
    """
    shutil.copytree(root / 'submodules/clvk', source, ignore=CopyIgnore, symlinks=True)
    third_party = source / 'external/clspv/third_party'
    third_party.mkdir(exist_ok=True)
    for name, target in (('llvm', 'submodules/llvm_project'),
                         ('SPIRV-Headers', 'submodules/clspv_spirv_headers'),
                         ('SPIRV-Tools', 'submodules/clspv_spirv_tools')):
        link = third_party / name
        if link.exists() or link.is_symlink():
            if link.is_symlink() or link.is_file():
                link.unlink()
            else:
                shutil.rmtree(link)
        link.symlink_to(root / target)
    apply_private_patches(root, source)


def apply_private_patches(root, source):
    """Apply exact tracked patches to a new caller-owned copy, never submodules.

    [in] root repository and source private copy; both nonnull absolute paths.
    Fail before mutation on a mismatching preimage. Git discovery is bounded to
    the copy's parent so the enclosing checkout/index cannot become the target.
    Single build-lock owner only; raises on any failed check/application.
    """
    if source.resolve() == (root / 'submodules/clvk').resolve():
        raise SystemExit('Refusing to patch the pinned clvk submodule')
    environment = dict(os.environ, GIT_CEILING_DIRECTORIES=str(source.parent.resolve()))
    for relative in PrivatePatches:
        patch = root / relative
        run(['git', 'apply', '--no-index', '--check', str(patch)], cwd=source, env=environment)
        run(['git', 'apply', '--no-index', str(patch)], cwd=source, env=environment)


def build_native(root, native, libclc, *, host_cc, host_cxx):
    """Build host LLVM TableGen tools and the Vulkan libclc bitcode (incremental).

    [in] native persistent directory; [out] libclc directory of symlinks to the
    spirv{32,64}-unknown-vulkan libclc.bc files clspv links in.
    """
    llvm = root / 'submodules/llvm_project'
    run(['cmake', '-G', 'Ninja', '-S', llvm / 'llvm', '-B', native,
         '-C', llvm / 'libclc/cmake/caches/spirv-vulkan.cmake', '-DCMAKE_BUILD_TYPE=Release',
         '-DCMAKE_C_COMPILER=' + str(host_cc), '-DCMAKE_CXX_COMPILER=' + str(host_cxx),
         '-DLLVM_TARGETS_TO_BUILD=Native', '-DLLVM_ENABLE_PROJECTS=clang',
         '-DLLVM_PARALLEL_LINK_JOBS=2', *LlvmSwitches])
    run(['ninja', '-C', native, '-j', jobs(), 'llvm-tblgen', 'clang-tblgen', 'libclc'])
    libclc.mkdir(exist_ok=True)
    produced = sorted(native.glob('lib/clang/*/lib/spirv*-unknown-vulkan/libclc.bc'))
    if len(produced) != 2:
        raise SystemExit('Expected spirv32 and spirv64 Vulkan libclc bitcode, found %d' % len(produced))
    for item in produced:
        link = libclc / item.parent.name
        link.mkdir(exist_ok=True)
        target = link / 'libclc.bc'
        if target.is_symlink() or target.exists():
            target.unlink()
        target.symlink_to(item)


def prepare_vulkan(root, vulkan, dlltool):
    """Create the Vulkan header/import-library inputs from repository pins only.

    Headers come from the pinned Vulkan-Headers inside DXVK; the import library is
    generated from the pinned loader's export definition. No system header is used.
    """
    include = vulkan / 'include'
    include.mkdir(parents=True)
    headers = root / 'submodules/dxvk/include/vulkan/include'
    for name in ('vulkan', 'vk_video'):
        (include / name).symlink_to(headers / name)
    run([dlltool, '-d', root / 'submodules/vulkan_loader/loader/vulkan-1.def',
         '-l', vulkan / 'libvulkan-1.dll.a', '-D', 'vulkan-1.dll'])
    shim = vulkan / 'shim'
    shim.mkdir()
    (shim / 'Windows.h').write_text('#include <windows.h>\n')
    return include, shim


def write_toolchain(path, cc, cxx, windres, ar, ranlib, find_root):
    """Write a CMake toolchain bound to explicit Win32-model compiler paths."""
    path.write_text('\n'.join((
        'set(CMAKE_SYSTEM_NAME Windows)',
        'set(CMAKE_SYSTEM_PROCESSOR x86_64)',
        'set(CMAKE_C_COMPILER "' + str(cc).replace('"', '\\"') + '")',
        'set(CMAKE_CXX_COMPILER "' + str(cxx).replace('"', '\\"') + '")',
        'set(CMAKE_RC_COMPILER "' + str(windres).replace('"', '\\"') + '")',
        'set(CMAKE_AR "' + str(ar).replace('"', '\\"') + '")',
        'set(CMAKE_RANLIB "' + str(ranlib).replace('"', '\\"') + '")',
        'set(CMAKE_FIND_ROOT_PATH "' + str(find_root).replace('"', '\\"') + '")',
        'set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)',
        'set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)',
        'set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)',
        'set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)', '')))


def build_runtime_objects(root, cc, folder):
    """Compile both Waddle C runtime objects before archive link resolution."""
    runtime = folder / 'runtime'
    runtime.mkdir()
    objects = (runtime / 'clvk_runtime_tls.o', runtime / 'clvk_runtime_eh_pool.o')
    for source, output in zip(ShimSources, objects):
        run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-mthreads',
             '-DWaddleRuntimeTlsArenaBytes=' + str(TlsLimits['max_bytes']),
             '-DWaddleRuntimeTlsSlotCapacity=' + str(TlsLimits['max_objects']),
             '-Isrc/vgpu', '-c', root / source, '-o', output], cwd=root)
    return objects


def preflight_runtime_probe(root, selected, folder):
    """Build and statically audit the isolated Win32 runtime probe before LLVM."""
    probe = folder / 'probe_preflight'
    probe.mkdir()
    objects = build_runtime_objects(root, selected['cc'], probe)
    probe_dll = probe / 'runtime_v2_probe.dll'
    probe_map = probe / 'runtime_v2_probe.map'
    command = [selected['cxx'], '-std=c++17', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
               '-mthreads', '-shared', *(str(item) for item in objects),
               str(root / 'tests/vgpu/clvk_runtime_windows.cpp'),
               '-Wl,-Map=' + str(probe_map) + ',--demangle', '-static-libgcc', '-static-libstdc++',
               '-Wl,--gc-sections', '-Wl,--undefined=waddle_clvk_runtime_tls_limits',
               '-Wl,-T,' + str(root / 'scripts/clvk/runtime_sections.ld'),
               '-Wl,--wrap=__gthr_win32_equal',
               '-Wl,--wrap=_ZSt16__at_thread_exitPSt20__at_thread_exit_elt',
               '-Wl,--wrap=_ZSt25notify_all_at_thread_exitRSt18condition_variableSt11unique_lockISt5mutexE',
               '-o', str(probe_dll)]
    run(command, cwd=root)
    harnesses = []
    for cycles, name in ((3, 'runtime_v2_cycle3.exe'), (2048, 'runtime_v2_stress2048.exe')):
        output = probe / name
        run([selected['cc'], '-std=c11', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-mthreads',
             '-DWaddleRuntimeLifetimeCount=' + str(cycles),
             str(root / 'tests/vgpu/clvk_runtime_windows.c'), '-o', output], cwd=root)
        harnesses.append({'path': str(output.resolve()), 'sha256': digest(output),
                          'bytes': output.stat().st_size, 'cycles': cycles, 'executed': False})
    report = {'probe_dll': {'path': str(probe_dll.resolve()), 'sha256': digest(probe_dll),
                            'bytes': probe_dll.stat().st_size},
              'probe_map': {'path': str(probe_map.resolve()), 'sha256': digest(probe_map)},
              'harnesses': harnesses, 'native_acceptance': False, 'status': 'compiled'}
    try:
        audit = load_auditor(root)
        report['admission'] = build_admission(
            root, probe_dll, probe_map, nm=selected['nm'],
            map_working_directory=probe, runtime_policy=selected['probe_runtime_policy'],
            expected_output_name=probe_dll.name, expected_exports=ProbeExports)
    except (OSError, ValueError, SystemExit) as exc:
        report['status'] = 'rejected'
        report['rejection'] = str(exc)
        write_json_atomic(probe / 'probe_preflight.json', report)
        raise SystemExit('strict Win32 runtime probe admission rejected the build: ' + str(exc)) from exc
    write_json_atomic(probe / 'probe_preflight.json', report)
    return report


def inspect_dll(dll, expected_exports, objdump=None):
    """Return import and export names of a PE file; fail on a forbidden runtime import."""
    selected_objdump = objdump or shutil.which('x86_64-w64-mingw32-objdump')
    if not selected_objdump:
        raise SystemExit('no explicit MinGW objdump selected for PE inspection')
    dump = capture([selected_objdump, '-p', dll])
    imports = re.findall(r'DLL Name:\s*(\S+)', dump)
    for name in imports:
        if name.lower().startswith(ForbiddenImports):
            raise SystemExit('OpenCL.dll imports an unbundled C++ runtime DLL: ' + name)
    # The address table contains rows ending in "Export RVA"; these are not
    # symbol names. Restrict parsing to the separately labeled name table.
    table = re.search(r'(?ms)^\[Ordinal/Name Pointer\] Table[^\n]*\n(.*?)(?:\n\s*\n|\Z)', dump)
    counts = re.search(r'Number in:\s*\n\s*Export Address Table\s+([0-9a-fA-F]+)\s*\n\s*'
                       r'\[Name Pointer/Ordinal\] Table\s+([0-9a-fA-F]+)', dump)
    if not table or not counts:
        raise SystemExit('OpenCL.dll is missing the PE export name table/counts')
    names = re.findall(r'^\s*\[\s*\d+\]\s+(?:\+base\[\s*\d+\]\s+[0-9a-fA-F]+\s+)?(\S+)\s*$',
                       table[1], flags=re.MULTILINE)
    exports = set(names)
    if int(counts[1], 16) != len(names) or int(counts[2], 16) != len(names) or len(names) != len(exports):
        raise SystemExit('OpenCL.dll PE export count mismatch or duplicate names')
    absent = [name for name in RequiredExports if name not in exports]
    if absent:
        raise SystemExit('OpenCL.dll is missing required exports: %s' % absent)
    if exports != set(expected_exports):
        raise SystemExit('OpenCL.dll export mismatch: missing=%s extra=%s' %
                         (sorted(set(expected_exports) - exports), sorted(exports - set(expected_exports))))
    return imports, sorted(exports)


def argument_parser():
    """Create explicit compiler, admission, capture-only, and output selectors."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', required=True, help='explicit MinGW win32 C compiler path')
    parser.add_argument('--cxx', required=True, help='explicit MinGW win32 C++ compiler path')
    parser.add_argument('--dlltool', required=True)
    parser.add_argument('--objdump', required=True)
    parser.add_argument('--nm', required=True)
    parser.add_argument('--windres', required=True)
    parser.add_argument('--ar', required=True)
    parser.add_argument('--ranlib', required=True)
    parser.add_argument('--reviewed-toolchain', help='reviewed identity JSON required for builds')
    parser.add_argument('--toolchain-provenance', required=True,
                        help='reviewed Debian package provenance JSON required for capture and builds')
    parser.add_argument('--runtime-policy', help='reviewed exact runtime API caller policy JSON')
    parser.add_argument('--probe-runtime-policy',
                        help='separate reviewed exact runtime API policy for the isolated probe')
    parser.add_argument('--capture-toolchain', help='capture identity JSON and exit without building')
    parser.add_argument('--output-root', default='build/clvk_client')
    return parser


def main(argv=None):
    """Capture identity or build, statically admit, and atomically publish OpenCL.dll."""
    args = argument_parser().parse_args(argv)
    root = Path.cwd().resolve()
    if args.capture_toolchain:
        identity = capture_toolchain_identity(args.cc, args.cxx)
        provenance = verify_package_provenance(identity, args.toolchain_provenance)
        identity['package_provenance'] = provenance
        write_json_atomic(args.capture_toolchain, identity)
        print('Captured toolchain identity (review required):', args.capture_toolchain, flush=True)
        return 0
    if not args.reviewed_toolchain:
        raise SystemExit('--reviewed-toolchain is required unless --capture-toolchain is selected')
    if not args.runtime_policy:
        raise SystemExit('--runtime-policy is required unless --capture-toolchain is selected')
    if not args.probe_runtime_policy:
        raise SystemExit('--probe-runtime-policy is required unless --capture-toolchain is selected')

    selected = _preflight(root, args)
    run(['sh', 'scripts/verify_clvk.sh'], cwd=root)
    exports = read_export_list(root / 'scripts/clvk/OpenCL.def')
    check_export_list_against_api(root, exports)
    settings = {'cmake_build_type': 'Release', 'clvk_vulkan_implementation': 'custom',
                'clvk_clspv_online_compiler': 'ON', 'llvm_targets_to_build': 'X86',
                'thread_model': 'win32', 'static_runtime': 'libgcc,libstdc++',
                'llvm_switches': LlvmSwitches}
    inputs = dict(selected['inputs'], settings=settings)
    cache_key = hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()
    native_key = _native_cache_key(selected['inputs'])
    base = (root / args.output_root).resolve()
    base.mkdir(parents=True, exist_ok=True)
    receipt = _receipt_path(root, args.output_root)
    receipt.parent.mkdir(parents=True, exist_ok=True)
    with (base / '.clvk_client.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        _verify_inputs_unchanged(root, selected['inputs'])
        if receipt.exists():
            try:
                prior = json.loads(receipt.read_text())
                if prior.get('cache_key') == cache_key:
                    _admit_recorded_runtime(root, prior, nm=selected['nm'],
                                            runtime_policy=selected['runtime_policy'])
                    _verify_inputs_unchanged(root, selected['inputs'])
                    print('Re-admitted cached OpenCL output:', receipt, flush=True)
                    return 0
            except (KeyError, OSError, ValueError, TypeError, SystemExit):
                # A bad or stale receipt is not success; proceed to a fresh build.
                pass

        folder = Path(tempfile.mkdtemp(prefix='clvk_' + cache_key[:12] + '_', dir=base))
        probe_record = preflight_runtime_probe(root, selected, folder)
        _verify_inputs_unchanged(root, selected['inputs'])
        native = base / ('native_' + native_key[:12])
        native.mkdir(exist_ok=True)
        libclc = native / 'libclc_vulkan'
        build_native(root, native, libclc,
                     host_cc=selected['inputs']['build_tools']['host_cc']['resolved_path'],
                     host_cxx=selected['inputs']['build_tools']['host_cxx']['resolved_path'])
        runtime_objects = build_runtime_objects(root, selected['cc'], folder)
        source = folder / 'source'
        prepare_source(root, source)
        include, shim = prepare_vulkan(root, folder / 'vulkan', selected['dlltool'])
        toolchain = folder / 'mingw-w64-x86_64.cmake'
        find_root = Path(selected['cc']).parent.parent
        write_toolchain(toolchain, selected['cc'], selected['cxx'], selected['windres'],
                        selected['ar'], selected['ranlib'], find_root)
        cross = folder / 'cross'
        link_map = cross / 'OpenCL.map'
        export_file = root / 'scripts/clvk/OpenCL.def'
        linker = linker_flags(runtime_objects, export_file, link_map)
        cmake = selected['inputs']['build_tools']['cmake']['resolved_path']
        ninja = selected['inputs']['build_tools']['ninja']['resolved_path']
        run([cmake, '-G', 'Ninja', '-S', source, '-B', cross,
             '-DCMAKE_TOOLCHAIN_FILE=' + str(toolchain), '-DCMAKE_BUILD_TYPE=Release',
             '-DCLVK_VULKAN_IMPLEMENTATION=custom', '-DVulkan_INCLUDE_DIRS=' + str(include),
             '-DVulkan_LIBRARIES=' + str(folder / 'vulkan/libvulkan-1.dll.a'),
             '-DCLVK_CLSPV_ONLINE_COMPILER=ON', '-DCLVK_COMPILER_AVAILABLE=ON',
             '-DCLVK_BUILD_TESTS=OFF', '-DCLSPV_BUILD_TESTS=OFF',
             '-DCLSPV_EXTERNAL_LIBCLC_DIR=' + str(libclc),
             '-DLLVM_TABLEGEN=' + str(native / 'bin/llvm-tblgen'),
             '-DCLANG_TABLEGEN=' + str(native / 'bin/clang-tblgen'),
             '-DLLVM_NATIVE_TOOL_DIR=' + str(native / 'bin'), '-DLLVM_TARGETS_TO_BUILD=X86',
             '-DCMAKE_CXX_FLAGS=-mthreads -isystem ' + shlex.quote(str(shim)),
             '-DCMAKE_C_FLAGS=-mthreads -isystem ' + shlex.quote(str(shim)),
             '-DCMAKE_SHARED_LINKER_FLAGS=' + linker, *LlvmSwitches], cwd=root)
        run([ninja, '-C', cross, '-j', jobs(), 'OpenCL'], cwd=root)
        produced = [item for item in cross.rglob('libOpenCL.dll') if item.is_file()]
        if len(produced) != 1 or not link_map.is_file():
            raise SystemExit('Expected one fresh libOpenCL.dll and its exact link map')
        linked_dll = produced[0].resolve()
        if linked_dll.name.lower() != 'libopencl.dll':
            raise SystemExit('unexpected linked OpenCL output name: ' + str(linked_dll))
        _verify_inputs_unchanged(root, selected['inputs'])
        admission = build_admission(root, linked_dll, link_map, nm=selected['nm'],
                                    map_working_directory=cross,
                                    runtime_policy=selected['runtime_policy'])
        imports, built_exports = inspect_dll(linked_dll, exports, objdump=selected['objdump'])
        output = folder / 'out'
        output.mkdir()
        dll = output / 'OpenCL.dll'
        shutil.copyfile(linked_dll, dll)
        if digest(dll) != admission['runtime']['dll_sha256']:
            raise SystemExit('published OpenCL copy differs from statically admitted image')
        _verify_inputs_unchanged(root, selected['inputs'])
        run(['sh', 'scripts/verify_clvk.sh'], cwd=root)
        record = {'cache_key': cache_key, 'inputs': inputs, 'native_dir': str(native),
                  'linked_dll': {'path': str(linked_dll), 'sha256': digest(linked_dll),
                                 'bytes': linked_dll.stat().st_size},
                  'link_map': {'path': str(link_map.resolve()), 'sha256': digest(link_map),
                               'working_directory': str(cross.resolve())},
                  'admission': admission,
                  'probe_preflight': probe_record,
                  'dll': {'path': str(dll.resolve()), 'sha256': digest(dll), 'bytes': dll.stat().st_size},
                  'imports': imports, 'export_count': len(built_exports), 'executed': False,
                  'native_acceptance': False,
                  'note': 'Cross-built and statically admitted only; native runtime behavior is unverified.'}
        (folder / 'provenance.json').write_text(json.dumps(record, indent=2, sort_keys=True) + '\n')
        _verify_inputs_unchanged(root, selected['inputs'])
        publish_admitted_record(root, receipt, record, inputs, nm=selected['nm'],
                                runtime_policy=selected['runtime_policy'])
        print('Published statically admitted OpenCL output:', receipt, flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
