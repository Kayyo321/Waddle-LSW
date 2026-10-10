#!/usr/bin/env python3
"""Run isolated hardware workloads with all twenty owned ICD sources instrumented.

Borrow frozen seam objects and production runtime graph read-only; compile private
C frontends and a version-mapped shared ICD. Full driver inventory, exact existing
workload assertions and prescribed postflush cleanup remain mandatory. Linux GPU
results do not establish Windows/DXVK or third-party driver access instrumentation.
"""
import argparse
import copy
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time

sys.dont_write_bytecode = True
import icd_dependency_sanitizers as dependency
import icd_owned_sanitizers as owned
import icd_seam_sanitizers as seam

# Frozen immutable source receipts; changed fixtures need a new audited contract.
SourceHashes = {
    'src/vgpu/venus_icd.zig': '7f78e3ce9f1c09bd3d07563b03d2b36f92aaf7dae6c4348985708bcb74a291da',
    'tests/vgpu/icd.c': '16cafa228d68b2f89d8c18e744ad2e2431edbdd0b3c800ba788ce099859f0221',
    'tests/vgpu/worker_presented.c': 'a68383c92faad311bdbe885fbb7da57622144d08164553469322a63fce953968',
}
ManifestNames = ('gfxstream_vk_icd.json', 'intel_hasvk_icd.json', 'intel_icd.json',
                 'lvp_icd.json', 'nouveau_icd.json', 'nvidia_icd.json', 'radeon_icd.json', 'virtio_icd.json')
RepairedLibraries = {
    'lvp_icd.json': 'build/vendor/mesa_cpu_cache_dual_owned/src/gallium/targets/lavapipe/libvulkan_lvp.so',
    'radeon_icd.json': 'build/vendor/mesa_cpu_cache_dual_owned/src/amd/vulkan/libvulkan_radeon.so',
}
PinHashes = {
    'submodules/mesa': '742a20f48c59e8649533c84c4d49dd95b403f5da',
    'submodules/vulkan_loader': '0508dee4ff864f5034ae6b7f68d34cb2822b827d',
    'submodules/virglrenderer': '500b41d5c8638f9b80dd558f4044f3301c7457a4',
}
TransportNames = ('frame', 'bounds', 'control', 'request')
CommonSources = ('venus_worker', 'venus_guest', 'venus_frame_linux', 'venus_rpc',
                 'venus_channel', 'venus_session', 'venus_region', 'venus_ring', 'venus_wait', 'venus_stream_linux')
CppFlags = ('-D_GNU_SOURCE', '-Iinclude', '-Isrc', '-Isrc/common', '-Isrc/cli', '-Isrc/daemon',
            '-Isrc/guest', '-Isrc/mock', '-Isrc/vgpu', '-Isubmodules/venus_protocol/include')
CFlags = ('-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-O1', '-g',
          '-fsanitize=address,leak,undefined', '-fno-omit-frame-pointer')
PublicExports = ('venus_icd_bind', 'venus_icd_bind_capabilities', 'venus_icd_unbind',
                 'venus_icd_abandon', 'venus_icd_negotiate_loader', 'venus_icd_get_instance_proc_addr',
                 'venus_icd_get_physical_proc_addr', 'vk_icdNegotiateLoaderICDInterfaceVersion',
                 'vk_icdGetInstanceProcAddr', 'vk_icdGetPhysicalDeviceProcAddr')
Workloads = ('triangle', 'compute', 'compute_push')
PhysicalTimeoutSeconds = 300
CleanupGraceSeconds = 5


def add_file(provenance, path):
    """[in] Borrow file/path; [out] mutate owned manifest with immutable SHA256.

    Resolve/require regular read-only input; stream closes synchronously. Conflicts
    fail instead of mixing snapshots. No ownership transfer or shared file mutation.
    """
    path = path.resolve(strict=True)
    seam.merge_inputs(provenance, {str(path): dependency.file_hash(path)})
    return path


def copy_input(provenance, origin, destination):
    """[in] Borrow source; [out] own byte-identical private copy/manifest entries.

    Caller owns destination directory exclusively. Source stays immutable; copy
    closes descriptors, retains mode, and both hashes must match. Synchronous.
    """
    origin = add_file(provenance, origin)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(origin, destination)
    assert dependency.file_hash(destination) == provenance[str(origin)]
    add_file(provenance, destination)
    return destination


def tool_text(arguments, path):
    """[in] Borrow command; [out] own bounded text artifact/value.

    Synchronous180s child is reaped; descriptor closes even on error. Tool failure
    or >256MiB result rejects proof. No caller/shared resources retained.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('wb') as stream:
        owned.run(arguments, stdout=stream, stderr=stream)
    assert path.stat().st_size <= seam.MaxProofBytes
    return path.read_text()


def driver_inventory(provenance, output):
    """[in] Borrow frozen full inventory; [out] own policy proof and driver env value.

    All eight original/private manifests and actual repaired DSOs are hashed.
    No rewrite/filter/selection; any metadata or snapshot inconsistency fails.
    Base soname resolution remains unchanged. Synchronous, no retained descriptors.
    """
    folder = Path('build/mesa_cpu_cache_dual_all_icds').resolve()
    manifest_paths = sorted(folder.glob('*.json'))
    assert tuple(path.name for path in manifest_paths) == ManifestNames
    receipt_path = add_file(provenance, Path('build/mesa_cpu_cache_dual_all_icds_provenance.json'))
    receipt = json.loads(receipt_path.read_text())
    assert set(receipt) == set(ManifestNames)
    policy = {}
    for private in manifest_paths:
        original = add_file(provenance, Path('/usr/share/vulkan/icd.d') / private.name)
        add_file(provenance, private)
        assert dependency.file_hash(private) == receipt[private.name]['sha256']
        original_json, private_json = json.loads(original.read_text()), json.loads(private.read_text())
        repaired = private.name in RepairedLibraries
        assert receipt[private.name]['replaced_library_path_only'] is repaired
        if repaired:
            library = add_file(provenance, Path(RepairedLibraries[private.name]))
            expected = copy.deepcopy(original_json)
            expected['ICD']['library_path'] = str(library)
            assert private_json == expected, 'repaired manifest changed metadata'
        else:
            assert original.read_bytes() == private.read_bytes(), 'ordinary driver manifest changed'
        policy[private.name] = {'original': str(original), 'private': str(private),
                                'original_sha256': dependency.file_hash(original),
                                'private_sha256': dependency.file_hash(private),
                                'library_path': private_json['ICD']['library_path'],
                                'replaced_library_path_only': repaired}
    for name in ('VK_ICD_FILENAMES', 'VK_ADD_DRIVER_FILES', 'VK_LOADER_DRIVERS_SELECT',
                 'VK_LOADER_DRIVERS_DISABLE', 'VK_INSTANCE_LAYERS', 'LD_PRELOAD', 'LD_LIBRARY_PATH'):
        assert not os.environ.get(name), 'conflicting driver/layer policy: ' + name
    (output / 'driver_policy.json').write_text(json.dumps(policy, indent=2) + '\n')
    return ':'.join(map(str, manifest_paths)), policy


def completed_seam(provenance, seam_output, seam_record):
    """[in] Borrow one explicit completed seam reference; [out] canonical run.

    Both interfaces validate the same immutable completion bytes, exact current
    twenty-source set, native/test reports and every retained object/IR input.
    Reject stale, missing, redirected or partial receipts before borrowing objects.
    Descriptors close synchronously; provenance owns hashes only, never input files.
    """
    assert (seam_output is None) != (seam_record is None)
    reference = (seam_output / 'completion.json' if seam_output is not None else seam_record)
    reference = add_file(provenance, reference)
    assert 0 < reference.stat().st_size <= seam.MaxProofBytes
    record = json.loads(reference.read_text())
    assert set(record) == {'schema', 'complete', 'output', 'completion_record', 'source_manifest',
                           'reports', 'expected_icd_debug_units', 'retained_artifacts'}
    assert record['schema'] == 'waddle_owned_icd_complete_seam_v1' and record['complete'] is True
    folder = Path(record['output']).resolve(strict=True)
    assert folder.is_dir() and str(folder) == record['output']
    if seam_output is not None:
        assert folder == seam_output.resolve(strict=True)
    completion = add_file(provenance, folder / 'completion.json')
    assert record['completion_record'] == str(completion)
    assert completion.read_bytes() == reference.read_bytes(), 'reference differs from immutable run receipt'
    expected_sources = {str((Path('src/vgpu') / name).resolve()) for name in seam.CompleteSourceNames}
    assert set(record['source_manifest']) == expected_sources and len(expected_sources) == 20
    assert record['expected_icd_debug_units'] == 117
    seam.merge_inputs(provenance, record['source_manifest'])
    retained = record['retained_artifacts']
    assert 0 < len(retained) <= seam.MaxRetainedArtifacts
    actual_retained = set()
    for path in folder.rglob('*'):
        assert not path.is_symlink(), 'retained seam artifact symlink'
        if path.is_file() and path != completion:
            assert len(actual_retained) < seam.MaxRetainedArtifacts
            assert path.resolve().is_relative_to(folder)
            actual_retained.add(str(path.resolve()))
    assert set(retained) == actual_retained, 'retained seam artifact set changed'
    seam.merge_inputs(provenance, retained)
    assert set(record['reports']) == {'native', 'test'}
    for kind, reference_report in record['reports'].items():
        report_path = folder / (kind + '_sanitized.json')
        assert reference_report['path'] == str(report_path)
        assert reference_report['sha256'] == retained[str(report_path)]
        report = json.loads(report_path.read_text())
        assert report['mode'] == 'Debug' and report['emission_mode'] == ('native' if kind == 'native' else 'tests')
        assert report['entire_module_reverse_proof'] is True and not report['uncovered_owned_functions']
        modules = report['modules']
        assert len(modules) == 20
        assert {module['source']: module['source_sha256'] for module in modules} == record['source_manifest']
        assert report['linked_instrumented_definitions'] == sum(module['instrumented_definitions'] for module in modules)
        assert report['linked_guard_stores_preserved'] > 0
        symbols = [symbol for module in modules for symbol in module['symbols']]
        assert len(symbols) == len(set(symbols)), 'duplicate final owned declaration'
        for module in modules:
            assert not module['uncovered_owned_functions']
            assert module['instrumented_definitions'] == module['final_defined_symbols'] == len(module['symbols']) > 0
            assert module['asan_access_hook_relocations'] > 0 and module['final_executable_access_hooks'] > 0
            assert all(module['required_symbol_access_hooks'].values())
            assert all(module['final_required_symbol_access_hooks'].values())
            if kind == 'test' or Path(module['source']) != owned.NativeBatchSource:
                assert not module['non_reachable_runtime_declarations']
            else:
                lazy = module['non_reachable_runtime_declarations']
                assert set(lazy) == {'publish_batch'}
                assert lazy['publish_batch']['declaration'] == owned.NativeBatchSignature
        helper = next(module for module in modules if Path(module['source']) == owned.NativeBatchSource)
        if kind == 'test':
            assert helper['final_required_symbol_access_hooks']['venus_features_native.publish_batch'] > 0
        assert report['standalone_native_suites'] == 6 and report['standalone_debug_units'] == 25
        assert report['icd_native_suites'] == (1 if kind == 'native' else 0)
        assert report['icd_debug_units'] == (0 if kind == 'native' else 117)
        assert dependency.file_hash(folder / (kind + '_runner')) == report['executable_sha256']
        links = report['ordered_link_manifest']
        assert len({entry['path'] for entry in links}) == len(links)
        expected_oracles = set(seam.NativeOracleNames if kind == 'native' else (*seam.GuestOracles, 'features_reply'))
        oracle_paths = {str(folder / 'oracles' / name / 'oracle.o') for name in expected_oracles}
        assert {entry['path'] for entry in links if '/oracles/' in entry['path']} == oracle_paths
        for entry in links:
            assert retained[entry['path']] == entry['sha256']
        seam.merge_inputs(provenance, report['input_manifest'])
    dependency.verify_inputs(provenance)
    return folder


def reprove_seam(provenance, output, folder):
    """[in] Borrow completed frozen seam; [out] own reproduced proofs/object copies.

    Recreate exact inventories/normalized native IR, require byte equality and actual
    object hooks before reuse. Existing original/normalized/object/report artifacts
    stay read-only. Every subprocess/descriptor is bounded/reaped synchronously.
    """
    report_path = add_file(provenance, folder / 'native_sanitized.json')
    completed = json.loads(report_path.read_text())
    assert completed['icd_native_suites'] == 1 and completed['standalone_debug_units'] == 25
    assert len(completed['modules']) == 20 and completed['entire_module_reverse_proof'] is True
    seam.merge_inputs(provenance, completed['input_manifest'])
    seam.merge_inputs(provenance, {entry['path']: entry['sha256'] for entry in completed['ordered_link_manifest']})
    assert dependency.file_hash(folder / 'native_runner') == completed['executable_sha256']
    add_file(provenance, folder / 'native_runner')
    test_path = add_file(provenance, folder / 'test_sanitized.json')
    completed_test = json.loads(test_path.read_text())
    assert completed_test['icd_debug_units'] == 117
    assert dependency.file_hash(folder / 'test_runner') == completed_test['executable_sha256']
    add_file(provenance, folder / 'test_runner')
    helper_module = next(module for module in completed_test['modules']
                         if Path(module['source']).name == 'venus_features_native.zig')
    assert helper_module['final_required_symbol_access_hooks']['venus_features_native.publish_batch'] > 0
    snapshots = [(Path('src/vgpu/venus_icd.zig'), folder,
                  owned.icd_source_inventory(Path('src/vgpu/venus_icd.zig')))]
    for name in seam.CodecNames:
        source = Path('src/vgpu/venus_' + name + '.zig')
        snapshots.append((source, folder / 'dependencies' / name,
                          dependency.dependency_inventory(source, dependency.ModuleConfigs[name]['boundary'])))
    expected_modules = {module['source']: module for module in completed['modules']}
    reports, objects = [], []
    for source, origin, inventory in snapshots:
        validation = output / 'validation' / source.stem
        validation.mkdir(parents=True, exist_ok=True)
        original = add_file(provenance, origin / 'native_original.ll')
        old_normalized = add_file(provenance, origin / 'native_sanitized.ll')
        old_object = add_file(provenance, origin / 'native_sanitized.o')
        normalized = validation / 'native_sanitized.ll'
        report = owned.instrument(source, original, normalized, inventory, mode='native')
        assert dependency.file_hash(normalized) == dependency.file_hash(old_normalized), 'native IR reproof changed bytes'
        owned.verify_access_hooks(old_object, report, normalized)
        seam.validate_report(report, 'native', set(inventory))
        for module in report['modules']:
            expected = expected_modules[module['source']]
            for key in ('source_sha256', 'runtime_functions', 'symbols', 'non_reachable_runtime_declarations',
                        'instrumented_definitions', 'asan_access_hook_relocations'):
                assert module[key] == expected[key], 'native source/object accounting drift: ' + key
        reports.append(report)
        objects.append(copy_input(provenance, old_object, output / 'icd' / (source.stem + '.o')))
    assert sum(report['guard_stores_preserved'] for report in reports) == completed['linked_guard_stores_preserved']
    dependency.verify_inputs(provenance)
    print('Native seam reproof: 20 unchanged sources, original guards/reverse bytes and actual object hooks passed', flush=True)
    return reports, objects


def copy_runtime(provenance, output):
    """[in] Borrow frozen production runtime; [out] own private byte-identical graph.

    Preserve relative SONAME links and worker runpath; no interposer, LD_LIBRARY_PATH
    or source rebuild. Capture dependency metadata and sanitizer references honestly.
    Synchronous bounded tools/copies close all process/descriptor resources.
    """
    runtime = output / 'runtime'
    worker = copy_input(provenance, Path('build/waddle_vgpu_worker_sanitized'), runtime / 'waddle_vgpu_worker_sanitized')
    server = copy_input(provenance, Path('build/vendor/virglrenderer/server/virgl_render_server'),
                        runtime / 'vendor/virglrenderer/server/virgl_render_server')
    artifacts = {'worker': worker, 'server': server}
    metadata = {}
    libraries = [('renderer', Path('build/vendor/virglrenderer/src/libvirglrenderer.so.1'),
                  runtime / 'vendor/virglrenderer/src', ('libvirglrenderer.so', 'libvirglrenderer.so.1')),
                 ('loader_normal', Path('build/vendor/vulkan_loader/loader/libvulkan.so.1'),
                  runtime / 'vendor/vulkan_loader/loader', ('libvulkan.so', 'libvulkan.so.1')),
                 ('loader_asan', Path('build/vendor/vulkan_loader_sanitized/loader/libvulkan.so.1'),
                  runtime / 'vendor/vulkan_loader_sanitized/loader', ('libvulkan.so', 'libvulkan.so.1'))]
    for name, origin, destination_folder, links in libraries:
        real_origin = origin.resolve(strict=True)
        library = copy_input(provenance, real_origin, destination_folder / real_origin.name)
        for name_link in links:
            original_link = origin.parent / name_link
            assert original_link.is_symlink()
            target = os.readlink(original_link)
            assert not Path(target).is_absolute() and '/' not in target
            private_link = destination_folder / name_link
            assert not private_link.exists() and not private_link.is_symlink(), 'runtime output already owns SONAME link'
            private_link.symlink_to(target)
        assert (destination_folder / origin.name).resolve() == library.resolve()
        artifacts[name] = destination_folder / origin.name
    for name, binary in artifacts.items():
        dynamic = tool_text(['readelf', '-d', str(binary)], output / 'runtime_proof' / (name + '_dynamic.txt'))
        resolved = tool_text(['ldd', str(binary)], output / 'runtime_proof' / (name + '_dependencies.txt'))
        assert 'not found' not in resolved
        dependencies = []
        for match in re.finditer(r'=> (/\S+)', resolved):
            resolved_path = add_file(provenance, Path(match[1]))
            dependencies.append(str(resolved_path))
        metadata[name] = {'binary': str(binary), 'dynamic': dynamic, 'resolved_dependencies': dependencies}
        if name in ('worker', 'loader_asan'):
            symbols = tool_text(['nm', '-D', '--undefined-only', str(binary)], output / 'runtime_proof' / (name + '_symbols.txt'))
            assert '__asan_' in symbols and 'libasan.so' in dynamic
    assert str(artifacts['renderer'].resolve()) in metadata['worker']['resolved_dependencies']
    assert '$ORIGIN/vendor/virglrenderer/src' in metadata['worker']['dynamic']
    (output / 'runtime_graph.json').write_text(json.dumps(metadata, indent=2) + '\n')
    return artifacts, metadata


def transport_inputs(provenance):
    """[in] Borrow four copied transport profiles; [out] own exact source/header map.

    Follow only actual bounded literal sibling imports; std/builtin remain excluded.
    C import headers must belong to the later compiler-discovered frontend closure.
    No unrelated header glob, source rebuild or access-instrumentation claim.
    Read-only synchronous hashes close descriptors; malformed/new imports fail.
    """
    root = Path('src/vgpu').resolve()
    pending = [root / ('venus_' + name + '.zig') for name in TransportNames]
    sources, headers = set(), set()
    while pending:
        source = pending.pop().resolve(strict=True)
        if source in sources:
            continue
        assert source.parent == root and len(sources) < owned.MaxSourceModules
        sources.add(add_file(provenance, source))
        text = source.read_text()
        imports = re.findall(r'@import\("([^"\n]+)"\)', text)
        assert len(imports) == text.count('@import('), 'unsupported transport import grammar'
        for name in imports:
            if name in ('std', 'builtin'):
                continue
            assert re.fullmatch(r'venus_\w+\.zig', name), 'new transport import contract needed'
            pending.append(root / name)
        includes = re.findall(r'@cInclude\("([^"\n]+)"\)', text)
        assert len(includes) == text.count('@cInclude('), 'unsupported transport C include grammar'
        for name in includes:
            assert re.fullmatch(r'waddle/venus_\w+\.h', name), 'new transport header contract needed'
            headers.add((Path('include') / name).resolve(strict=True))
    return {'sources': sorted(map(str, sources)), 'c_import_headers': sorted(map(str, headers)),
            'profile_scope': 'original ReleaseSafe copied bytes; imports inventoried without ASan access claim'}



def proc_identity(pid):
    """[in] Borrow PID; [out] return exact Linux identity or None after exit.

    The stat descriptor closes synchronously. Include session/start time to avoid
    authorizing reused numeric identifiers. Other read/format failures propagate.
    """
    try:
        text = (Path('/proc') / str(pid) / 'stat').read_text(errors='surrogateescape')
    except (FileNotFoundError, ProcessLookupError):
        return None
    prefix, suffix = text.rsplit(')', 1)
    assert int(prefix.split('(', 1)[0]) == pid
    fields = suffix.split()
    assert len(fields) >= 20
    return {'pid': pid, 'state': fields[0], 'pgroup': int(fields[2]),
            'session': int(fields[3]), 'start_time': int(fields[19])}


def session_members(session_id):
    """[in] Borrow acquired session ID; [out] own current identity inventory.

    Inspect /proc synchronously without persistent descriptors. Only exact matching
    sessions are owned; global names, ancestors and unrelated sessions are excluded.
    """
    members = []
    for entry in Path('/proc').iterdir():
        if not entry.name.isdecimal():
            continue
        identity = proc_identity(int(entry.name))
        if identity and identity['session'] == session_id:
            members.append(identity)
    return members


def signal_members(session_id, signal_number, accounting):
    """[in] Borrow session/signal; [out] update exact pidfd-backed cleanup receipt.

    Open each observed pidfd, recheck PID/session/start-time identity, then signal
    that process only. Exit/reuse races authorize no signal. Every pidfd closes in
    finally, including diagnostics/errors; unrelated process identities are borrowed.
    """
    for before in session_members(session_id):
        try:
            descriptor = os.pidfd_open(before['pid'])
        except ProcessLookupError:
            continue
        try:
            after = proc_identity(before['pid'])
            keys = ('pid', 'session', 'start_time')
            if not after or any(before[key] != after[key] for key in keys):
                continue
            accounting['observed_pgroups'].add(after['pgroup'])
            try:
                signal.pidfd_send_signal(descriptor, signal_number)
                accounting['term_signals' if signal_number == signal.SIGTERM else 'kill_signals'] += 1
            except ProcessLookupError:
                pass
        finally:
            os.close(descriptor)


def stop_owned_session(process):
    """[in] Own acquired child/session; [out] return complete bounded cleanup proof.

    Signal all exact session members, including separate descendant process groups.
    Five seconds TERM and five seconds KILL include rescans and direct-child reaping.
    Empty session and reaped child are mandatory; failure never earns acceptance.
    """
    accounting = {'term_signals': 0, 'kill_signals': 0, 'observed_pgroups': set()}
    for signal_number in (signal.SIGTERM, signal.SIGKILL):
        deadline = time.monotonic() + CleanupGraceSeconds
        while True:
            process.poll()
            signal_members(process.pid, signal_number, accounting)
            if not session_members(process.pid):
                process.wait(timeout=max(0.001, deadline - time.monotonic()))
                accounting['observed_pgroups'] = sorted(accounting['observed_pgroups'])
                accounting['session_empty'] = True
                accounting['direct_child_reaped'] = process.returncode is not None
                return accounting
            if time.monotonic() >= deadline:
                break
            time.sleep(0.01)
    process.wait(timeout=0.001)
    raise RuntimeError('owned fixture session remained after bounded TERM/KILL cleanup')


def run_owned_session(arguments, *, cwd, environment, stream, timeout):
    """[in] Borrow launch/config/log; [out] return status after empty-session proof.

    Synchronously own one Linux session. Defer cancellation until Popen's handle is
    acquired, catch every subsequent exception, pidfd-clean all session members and
    reap child. Restore main-thread handlers on all paths. Successful fixtures
    must release their own session; leftover processes fail without success credit.
    """
    def interrupted(signal_number, frame):
        del signal_number, frame
        raise InterruptedError('owned fixture launcher received SIGTERM')

    previous_term = signal.signal(signal.SIGTERM, interrupted)
    process = None
    try:
        pending = set()

        def deferred(signal_number, frame):
            del frame
            pending.add(signal_number)

        previous_int = signal.signal(signal.SIGINT, deferred)
        signal.signal(signal.SIGTERM, deferred)
        try:
            process = subprocess.Popen(arguments, cwd=cwd, env=environment, stdout=stream, stderr=stream,
                                       start_new_session=True)
        finally:
            signal.signal(signal.SIGINT, previous_int)
            signal.signal(signal.SIGTERM, interrupted)
        if pending:
            if signal.SIGINT in pending:
                raise KeyboardInterrupt('owned fixture launcher received SIGINT')
            raise InterruptedError('owned fixture launcher received SIGTERM during acquisition')
        status = process.wait(timeout=timeout)
        deadline = time.monotonic() + CleanupGraceSeconds
        while session_members(process.pid):
            if time.monotonic() >= deadline:
                raise RuntimeError('fixture exited with live owned-session descendants')
            time.sleep(0.01)
        return status
    except BaseException as error:
        if process is not None:
            previous_int = signal.signal(signal.SIGINT, signal.SIG_IGN)
            signal.signal(signal.SIGTERM, signal.SIG_IGN)
            try:
                error.owned_session_cleanup = stop_owned_session(process)
            finally:
                signal.signal(signal.SIGINT, previous_int)
        raise
    finally:
        signal.signal(signal.SIGTERM, previous_term)

# Harmless separate-group ownership fixture; no Vulkan/GPU/source interaction.
ProcessOwnerChildCode = '''import json, os, pathlib, signal, sys
if sys.argv[2] == "1": signal.signal(signal.SIGTERM, signal.SIG_IGN)
pathlib.Path(sys.argv[1]).write_text(json.dumps({"pid": os.getpid(), "pgroup": os.getpgrp(), "session": os.getsid(0), "blocked": list(signal.pthread_sigmask(signal.SIG_BLOCK, []))}))
while True: signal.pause()
'''
ProcessOwnerFixtureCode = '''import json, os, pathlib, signal, subprocess, sys, time
folder, mode = pathlib.Path(sys.argv[1]), sys.argv[2]
ignore = "1" if mode in ("forced_timeout", "cancel", "acquisition_term", "acquisition_int") else "0"
if ignore == "1": signal.signal(signal.SIGTERM, signal.SIG_IGN)
child_ready = folder / "child.json"
child = subprocess.Popen([sys.executable, "-c", {child_code}, str(child_ready), ignore], process_group=0)
deadline = time.monotonic() + 10
while not child_ready.exists():
 if time.monotonic() >= deadline: raise RuntimeError("child readiness timeout")
 time.sleep(0.01)
entries = [{{"pid": os.getpid(), "pgroup": os.getpgrp(), "session": os.getsid(0), "blocked": list(signal.pthread_sigmask(signal.SIG_BLOCK, []))}}, json.loads(child_ready.read_text())]
(folder / "ready.json").write_text(json.dumps(entries))
if mode == "normal":
 child.terminate()
 child.wait(timeout=5)
 sys.exit(0)
if mode == "cancel": os.kill(os.getppid(), signal.SIGTERM)
while True: signal.pause()
'''.format(child_code=repr(ProcessOwnerChildCode))


def owner_selftests(output):
    """[in] Borrow exclusive output root; [out] own six real owner-path receipts.

    Harmless Python children exercise normal exit, graceful/forced timeout and actual
    delivered SIGTERM cancellation and both acquisition signals with separate groups.
    The acquisition wrapper launches real children and sends real signals before the
    real constructor returns; no child status/resource outcome is mocked. Assert exact
    empty-session/reaped-child/closed-pidfd ownership and restored signal handlers.
    No GPU/production source mutation or sanitizer instrumentation credit is involved.
    """
    folder = output / 'owner_tests'
    folder.mkdir()
    fixture = folder / 'fixture.py'
    fixture.write_text(ProcessOwnerFixtureCode)
    receipts = []
    for mode in ('normal', 'graceful_timeout', 'forced_timeout', 'cancel', 'acquisition_term', 'acquisition_int'):
        case = folder / mode
        case.mkdir()
        descriptors = len(os.listdir('/proc/self/fd'))
        before = signal.getsignal(signal.SIGTERM)
        before_int = signal.getsignal(signal.SIGINT)
        constructor = subprocess.Popen

        def acquire_then_signal(*arguments, **options):
            acquired = constructor(*arguments, **options)
            try:
                deadline = time.monotonic() + CleanupGraceSeconds
                while not (case / 'ready.json').exists():
                    if time.monotonic() >= deadline:
                        raise RuntimeError('acquisition fixture readiness timeout')
                    time.sleep(0.01)
                os.kill(os.getpid(), signal.SIGTERM if mode == 'acquisition_term' else signal.SIGINT)
            except BaseException:
                stop_owned_session(acquired)
                raise
            return acquired

        try:
            if mode.startswith('acquisition_'):
                subprocess.Popen = acquire_then_signal
            with (case / 'fixture.log').open('wb') as stream:
                try:
                    status = run_owned_session([sys.executable, str(fixture), str(case), mode],
                                               cwd=folder, environment=dict(os.environ), stream=stream, timeout=5)
                    assert mode == 'normal' and status == 0
                    cleanup = None
                except (subprocess.TimeoutExpired, InterruptedError, KeyboardInterrupt) as error:
                    expected_error = (KeyboardInterrupt if mode == 'acquisition_int' else
                                      InterruptedError if mode in ('cancel', 'acquisition_term') else subprocess.TimeoutExpired)
                    assert type(error) is expected_error
                    cleanup = error.owned_session_cleanup
                    assert cleanup['session_empty'] and cleanup['direct_child_reaped']
                    assert cleanup['term_signals'] >= 2
                    if mode in ('forced_timeout', 'cancel', 'acquisition_term', 'acquisition_int'):
                        assert cleanup['kill_signals'] >= 2
        finally:
            subprocess.Popen = constructor
        entries = json.loads((case / 'ready.json').read_text())
        assert len(entries) == 2 and entries[0]['session'] == entries[1]['session'] == entries[0]['pid']
        assert entries[0]['pgroup'] != entries[1]['pgroup']
        assert all(signal.SIGTERM not in entry['blocked'] and signal.SIGINT not in entry['blocked'] for entry in entries)
        assert not session_members(entries[0]['session'])
        assert signal.getsignal(signal.SIGTERM) == before
        assert signal.getsignal(signal.SIGINT) == before_int
        assert len(os.listdir('/proc/self/fd')) == descriptors
        if cleanup:
            assert cleanup['observed_pgroups'] == sorted(entry['pgroup'] for entry in entries)
        receipts.append({'case': mode, 'members': entries, 'cleanup': cleanup, 'fd_count': descriptors,
                         'fixture_sha256': dependency.file_hash(fixture),
                         'log_sha256': dependency.file_hash(case / 'fixture.log')})
        print(mode + ': real session ownership, separate group, no fd/session remainder passed', flush=True)
    (folder / 'report.json').write_text(json.dumps(receipts, indent=2) + '\n')
    return receipts


def physical_run(binary, profile, workload, forced, environment, expected_device, cwd, output):
    """[in] Borrow binary/config/immutable graph; [out] own strict execution receipt/log.

    Own one child session and log descriptor, synchronously reap within original300s.
    Timeout kills only that session and fails; no timeout earns leak/success credit.
    Positive status0 and prescribed postflush status1 require exact fixture witnesses
    and no sanitizer diagnostic. Frozen fixture owns GPU/assertion/FD cleanup.
    """
    name = profile + '_' + workload + ('_postflush_failure' if forced else '_positive')
    path = output / 'runs' / (name + '.log')
    path.parent.mkdir(parents=True, exist_ok=True)
    print('Starting ' + name, flush=True)
    with path.open('wb') as stream:
        try:
            status = run_owned_session([str(binary)], cwd=cwd, environment=environment, stream=stream,
                                       timeout=PhysicalTimeoutSeconds)
        except BaseException as error:
            if hasattr(error, 'owned_session_cleanup'):
                path.with_suffix('.cleanup.json').write_text(json.dumps(error.owned_session_cleanup, indent=2) + '\n')
            if isinstance(error, subprocess.TimeoutExpired):
                raise RuntimeError('physical fixture timeout: ' + name) from error
            raise
    assert path.stat().st_size <= seam.MaxProofBytes
    text = path.read_text()
    assert not re.search(r'AddressSanitizer|LeakSanitizer|runtime error:', text), 'sanitizer diagnostic in ' + name
    observed = re.findall(r'^ICD production acceptance device: (.+)$', text, re.M)
    marker = 'Presented production worker negotiation, isolation and unknown-release shutdown passed'
    if forced:
        stage = ('triangle' if workload == 'triangle' else 'compute') + ' mapped poison and flush'
        assert status == 1 and marker not in text and observed == [expected_device]
        assert 'ICD ' + ('triangle' if workload == 'triangle' else 'compute') + ' acceptance failed: ' + stage in text
        assert 'Presented worker binding failed: mode=0' in text
    else:
        assert status == 0 and text.count(marker) == 1 and observed == [expected_device, expected_device]
        assert not re.search(r'acceptance failed|binding failed|missing entry point', text)
    print(name + ': exact expected status' + str(status) + ', physical identity and checker cleanup passed', flush=True)
    return {'profile': profile, 'workload': workload, 'postflush_failure': forced,
            'status': status, 'observed_devices': observed, 'positive_lifetimes': 0 if forced else 16,
            'owned_session_empty': True, 'direct_child_reaped': True,
            'log': str(path), 'log_sha256': dependency.file_hash(path),
            'environment': {name: environment.get(name) for name in ('VK_DRIVER_FILES', 'WADDLE_TEST_WORKLOAD',
                             'WADDLE_TEST_LOADER_FAILURE', 'WADDLE_PRODUCTION_WORKER', 'RENDER_SERVER_EXEC_PATH',
                             'WADDLE_TEST_VULKAN_LOADER', 'ASAN_OPTIONS', 'LSAN_OPTIONS', 'UBSAN_OPTIONS')}}


def main():
    """[in] CLI exclusive output/physical identity; [out] proofs and15 serial receipts.

    Root holds GPU/source/runtime freeze. Inputs remain borrowed read-only; output,
    compilation descriptors and child sessions are exclusively owned/released here.
    Any mismatch, signal, timeout or checker failure stops acceptance, preserving logs.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--expected-device')
    parser.add_argument('--process-owner-selftest', action='store_true')
    seam_arguments = parser.add_mutually_exclusive_group()
    seam_arguments.add_argument('--seam-output', type=Path)
    seam_arguments.add_argument('--seam-record', type=Path)
    arguments = parser.parse_args()
    if not arguments.process_owner_selftest and not arguments.expected_device:
        parser.error('--expected-device is required for physical acceptance')
    if not arguments.process_owner_selftest and not (arguments.seam_output or arguments.seam_record):
        parser.error('one explicit --seam-output or --seam-record is required for physical acceptance')
    artifact_root = arguments.output.resolve()
    artifact_root.mkdir(parents=True, exist_ok=True)
    output = artifact_root / ('run-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ') + '-' + str(os.getpid()))
    output.mkdir()
    print('Exclusive physical artifacts: ' + str(output), flush=True)
    ownership = owner_selftests(output)
    if arguments.process_owner_selftest:
        return
    provenance = {}
    seam_folder = completed_seam(provenance, arguments.seam_output, arguments.seam_record)
    for name, digest in SourceHashes.items():
        path = add_file(provenance, Path(name))
        assert provenance[str(path)] == digest, 'frozen source changed: ' + name
    for path, digest in PinHashes.items():
        actual = subprocess.check_output(['git', '-C', path, 'rev-parse', 'HEAD'], text=True, timeout=180).strip()
        assert actual == digest
    for path in (Path(__file__), Path(dependency.__file__), Path(owned.__file__), Path(seam.__file__),
                 Path('scripts/vgpu.mk'), Path('scripts/vulkan_loader.cmake'), Path('GNUmakefile'),
                 Path('src/vgpu/venus_icd.map')):
        add_file(provenance, path)
    drivers, policy = driver_inventory(provenance, output)
    reports, icd_objects = reprove_seam(provenance, output, seam_folder)
    runtime, runtime_metadata = copy_runtime(provenance, output)
    transport = []
    for name in TransportNames:
        transport.append(copy_input(provenance, Path('build/venus_' + name + '.o'), output / 'transport' / (name + '.o')))
    transport_manifest = transport_inputs(provenance)
    common_objects = []
    for name in CommonSources:
        source = Path('src/vgpu') / (name + '.c')
        assert 'VgpuIcdLoader' not in source.read_text()
        folder = output / 'frontend' / name
        folder.mkdir(parents=True, exist_ok=True)
        seam.merge_inputs(provenance, dependency.input_provenance(Path('src/vgpu/venus_icd.zig'), source,
                                                                 (*CppFlags, *CFlags), folder))
        object_path = folder / 'source.o'
        owned.run(['cc', *CppFlags, *CFlags, '-c', str(source), '-o', str(object_path)])
        common_objects.append(object_path)
    cwd = output / 'run'
    private_build = cwd / 'build'
    private_build.mkdir(parents=True)
    shared_icd = private_build / 'libwaddle_vulkan_experimental.so'
    version_map = Path('src/vgpu/venus_icd.map').resolve()
    owned.run(['cc', '-shared', *map(str, icd_objects), '-pthread', '-fsanitize=address,leak,undefined',
               '-Wl,--version-script=' + str(version_map), '-o', str(shared_icd)])
    exports = tool_text(['nm', '-D', '--defined-only', str(shared_icd)], output / 'shared_exports.txt')
    public = []
    for line in exports.splitlines():
        match = re.match(r'^[0-9a-fA-F]+\s+([a-zA-Z])\s+(.+)$', line)
        assert match
        name = match[2].split('@')[0]
        if name == 'WaddleIcd':
            assert match[1] == 'A'
        else:
            assert match[1] == 'T'
            public.append(name)
    assert set(public) == set(PublicExports) and len(public) == len(PublicExports)
    manifest = private_build / 'waddle_vulkan_experimental.json'
    manifest.write_text(json.dumps({'file_format_version': '1.0.0', 'ICD': {
        'library_path': str(shared_icd), 'api_version': '1.0.0'}}, indent=2) + '\n')
    proofs = {'shared_icd': seam.final_access_proof(shared_icd, reports, output)}
    binaries, linked_manifests = {}, {'shared_icd': seam.object_manifest(icd_objects)}
    for profile, defines in (('direct', []), ('shared', ['-DVgpuIcdLoader'])):
        folder = output / 'frontend' / profile
        folder.mkdir(parents=True)
        source = Path('tests/vgpu/worker_presented.c')
        seam.merge_inputs(provenance, dependency.input_provenance(Path('src/vgpu/venus_icd.zig'), source,
                                                                 (*CppFlags, *CFlags, *defines), folder))
        object_path = folder / 'fixture.o'
        owned.run(['cc', *CppFlags, *CFlags, *defines, '-c', str(source), '-o', str(object_path)])
        inputs = [object_path, *common_objects, *icd_objects, *transport]
        binary = output / (profile + '_runner')
        owned.run(['cc', *CFlags, *map(str, inputs), *(['-ldl'] if profile == 'shared' else []), '-o', str(binary)])
        binaries[profile] = binary
        linked_manifests[profile] = seam.object_manifest(inputs)
        proofs[profile] = seam.final_access_proof(binary, reports, output)
    for entries in linked_manifests.values():
        seam.merge_inputs(provenance, {entry['path']: entry['sha256'] for entry in entries})
    for path in (*binaries.values(), shared_icd, manifest):
        add_file(provenance, path)
    assert all(path in provenance for path in transport_manifest['c_import_headers']), 'transport C header absent from compiler closure'
    dependency.verify_inputs(provenance)
    report = {'scope': 'owned20 ICD accesses plus C fixture/worker; other transport Zig ReleaseSafe and host/vendor code excluded',
              'seam_output': str(seam_folder),
              'source_hashes': SourceHashes, 'pins': PinHashes, 'expected_device': arguments.expected_device,
              'cpp_flags': CppFlags, 'c_flags': CFlags, 'driver_policy': policy, 'owner_tests': ownership,
              'runtime_graph': runtime_metadata, 'input_manifest': provenance,
              'ordered_link_manifests': linked_manifests, 'actual_access_proofs': proofs,
              'transport_scope': 'copied original ReleaseSafe objects; no ASan access claim',
              'transport_input_manifest': transport_manifest,
              'postflush_failure_scope': 'existing VgpuIcdLoader fixture only; direct build has no failure flag',
              'runs': []}
    report_path = output / 'physical_report.json'
    report_path.write_text(json.dumps(report, indent=2) + '\n')
    profiles = (('direct', binaries['direct'], None), ('shared_normal_loader', binaries['shared'], runtime['loader_normal']),
                ('shared_asan_loader', binaries['shared'], runtime['loader_asan']))
    for forced in (False, True):
        # The frozen direct fixture does not implement WADDLE_TEST_LOADER_FAILURE.
        # Failure receipts require the actual shared fixture branch, never status0.
        selected_profiles = profiles[1:] if forced else profiles
        for profile, binary, loader in selected_profiles:
            for workload in Workloads:
                environment = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1:halt_on_error=1',
                                   LSAN_OPTIONS='exitcode=23', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1',
                                   VK_DRIVER_FILES=drivers, WADDLE_TEST_WORKLOAD=workload,
                                   WADDLE_PRODUCTION_WORKER=str(runtime['worker']), RENDER_SERVER_EXEC_PATH=str(runtime['server']))
                environment.pop('WADDLE_TEST_LOADER_FAILURE', None)
                environment.pop('WADDLE_TEST_VULKAN_LOADER', None)
                if loader:
                    environment['WADDLE_TEST_VULKAN_LOADER'] = str(loader)
                if forced:
                    environment['WADDLE_TEST_LOADER_FAILURE'] = '1'
                dependency.verify_inputs(provenance)
                receipt = physical_run(binary, profile, workload, forced, environment, arguments.expected_device, cwd, output)
                dependency.verify_inputs(provenance)
                report['runs'].append(receipt)
                report_path.write_text(json.dumps(report, indent=2) + '\n')
    assert len(report['runs']) == 15 and sum(run['positive_lifetimes'] for run in report['runs']) == 144
    assert sum(run['postflush_failure'] for run in report['runs']) == 6
    report['complete'] = True
    report['positive_lifetimes'] = 144
    report_path.write_text(json.dumps(report, indent=2) + '\n')
    print('Complete physical matrix:9 positive16-lifetime GPU runs and6 exact shared exit1 postflush cleanup runs passed', flush=True)


if __name__ == '__main__':
    main()
