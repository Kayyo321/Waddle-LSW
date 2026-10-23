#!/usr/bin/env python3
"""Actual GCC/PE link controls for absent unsupported runtime facilities.

No native execution. Every negative source must compile before its link fails
for the exact forbidden reference. Preserve each command, output and digest.
"""
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def main():
    """Own a unique build directory; raise on unexpected success/failure.

    in: WADDLE_CLVK_CC/CXX explicit compiler selectors, default Win32 MinGW.
    out: retained JSON/logs; zero only for positive link plus six real rejects.
    No SDK files or existing artifacts modified; no native, VM or GPU action.
    """
    root = Path(__file__).resolve().parents[2]
    requested_out = os.environ.get('WADDLE_CLVK_LINK_TEST_DIR')
    out = (Path(requested_out).resolve() if requested_out else
           root / 'build/qualification_runtime_link' / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ'))
    out.mkdir(parents=True, exist_ok=False)
    cc = shutil.which(os.environ.get('WADDLE_CLVK_CC', 'x86_64-w64-mingw32-gcc-win32'))
    cxx = shutil.which(os.environ.get('WADDLE_CLVK_CXX', 'x86_64-w64-mingw32-g++-win32'))
    if not cc or not cxx:
        raise RuntimeError('explicit reviewed Win32 compiler pair required')
    sources = [root / 'src/vgpu/clvk_runtime_tls.c', root / 'src/vgpu/clvk_runtime_eh_pool.c',
               root / 'tests/vgpu/clvk_runtime_windows.cpp', root / 'scripts/clvk/runtime_sections.ld',
               Path(__file__).resolve(), root / 'scripts/build_clvk_windows.py',
               root / 'scripts/clvk_runtime_audit.py', Path(cc).resolve(), Path(cxx).resolve()]
    hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
    rows = []

    def run(name, argv, reject=None):
        result = subprocess.run([str(a) for a in argv], cwd=root, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=60)
        (out / (name + '.log')).write_bytes(result.stdout)
        rows.append({'name': name, 'argv': [str(a) for a in argv], 'exit': result.returncode,
                     'log_sha256': hashlib.sha256(result.stdout).hexdigest(), 'expected_reject': reject})
        (out / 'receipt.json').write_text(json.dumps({'source_sha256': hashes, 'runs': rows,
                                                    'native_execution': False}, indent=2) + '\n')
        if reject:
            text = result.stdout.decode(errors='replace')
            if result.returncode == 0 or any(token not in text for token in reject):
                raise RuntimeError('missing exact negative link failure: ' + name)
        elif result.returncode:
            raise RuntimeError('positive build failed: ' + name)

    objects = []
    for source in sources[:2]:
        obj = out / (source.stem + '.o')
        run(source.stem, [cc, '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                          '-O2', '-mthreads', '-c', source, '-o', obj])
        objects.append(obj)
    probe = out / 'probe.o'
    run('probe_compile', [cxx, '-std=c++17', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                          '-O2', '-mthreads', '-c', sources[2], '-o', probe])
    future_source = out / 'future_positive.cpp'
    future_source.write_text('#include <future>\nextern "C" __declspec(dllexport) int ordinary_future() { std::promise<int> value; auto result = value.get_future(); value.set_value(17); result.wait(); return result.get(); }\n')
    future_object = out / 'future_positive.o'
    run('future_positive_compile', [cxx, '-std=c++17', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                                    '-O2', '-mthreads', '-c', future_source, '-o', future_object])
    link = [cxx, '-shared', '-mthreads', probe, future_object, *objects, '-static-libgcc', '-static-libstdc++',
            '-Wl,--gc-sections', '-Wl,--undefined=waddle_clvk_runtime_tls_limits',
            '-Wl,-T,' + str(sources[3]), '-Wl,--wrap=__gthr_win32_equal',
            '-Wl,--wrap=_ZSt16__at_thread_exitPSt20__at_thread_exit_elt',
            '-Wl,--wrap=_ZNSt13__future_base13_State_baseV211_Make_ready6_M_setEv',
            '-Wl,--wrap=_ZSt25notify_all_at_thread_exitRSt18condition_variableSt11unique_lockISt5mutexE']
    dll = out / 'runtime_link_positive.dll'
    run('positive', [*link, '-o', dll, '-Wl,-Map,' + str(out / 'positive.map') + ',--demangle'])
    nm = shutil.which('x86_64-w64-mingw32-nm')
    if not nm:
        raise RuntimeError('nm required to validate actual mapped symbol absence')
    symbols = subprocess.check_output([nm, str(dll)], text=True, timeout=60)
    (out / 'positive.nm').write_text(symbols)
    positive_map = (out / 'positive.map').read_text()
    boundary = 'Linker script and memory map'
    if positive_map.count(boundary) != 1:
        raise RuntimeError('positive map has no unique active-output boundary')
    active_map = positive_map.split(boundary, 1)[1]
    active_map = '\n'.join(line for line in active_map.splitlines()
                            if not line.lstrip().startswith('*('))
    if '_ZNSt13__future_base13_State_baseV211_Make_ready6_M_setEv' in active_map or '_Make_ready::_M_set' in active_map:
        raise RuntimeError('forbidden set-at-thread-exit helper remains in active positive link map')
    for line in symbols.splitlines():
        fields = line.split()
        if len(fields) == 3 and any(token in fields[2] for token in ('__at_thread_exit', 'notify_all_at_thread_exit',
                                                                      '_ZNSt13__future_base13_State_baseV211_Make_ready6_M_setEv')):
            raise RuntimeError('forbidden facility remains mapped: ' + line)
    prefix = '#include <condition_variable>\n#include <mutex>\nnamespace std { void __at_thread_exit(__at_thread_exit_elt*); }\nextern "C" int __gthr_win32_equal(void*, void*);\n'
    controls = {
        'internal_call': ('extern "C" __declspec(dllexport) void forbidden() { std::__at_thread_exit(nullptr); }', ['undefined reference', '__wrap__ZSt16__at_thread_exit']),
        'internal_address': ('extern "C" __declspec(dllexport) auto forbidden() -> decltype(&std::__at_thread_exit) { return &std::__at_thread_exit; }', ['undefined reference', '__wrap__ZSt16__at_thread_exit']),
        'notify_call': ('extern "C" __declspec(dllexport) void forbidden(std::condition_variable* c, std::mutex* m) { std::notify_all_at_thread_exit(*c, std::unique_lock<std::mutex>(*m)); }', ['undefined reference', '__wrap__ZSt25notify_all_at_thread_exit']),
        'notify_address': ('extern "C" __declspec(dllexport) auto forbidden() -> decltype(&std::notify_all_at_thread_exit) { return &std::notify_all_at_thread_exit; }', ['undefined reference', '__wrap__ZSt25notify_all_at_thread_exit']),
        'future_thread_exit_call': ('#include <future>\nextern "C" __declspec(dllexport) void forbidden(std::promise<int>* value) { value->set_value_at_thread_exit(17); }', ['undefined reference', '__wrap__ZNSt13__future_base13_State_baseV211_Make_ready6_M_setEv']),
        'future_thread_exit_address': ('#include <future>\nusing method_t = void (std::promise<int>::*)(int&&);\nextern "C" __declspec(dllexport) method_t forbidden() { return static_cast<method_t>(&std::promise<int>::set_value_at_thread_exit); }', ['undefined reference', '__wrap__ZNSt13__future_base13_State_baseV211_Make_ready6_M_setEv']),
        'equal_call': ('extern "C" __declspec(dllexport) int forbidden(void* a, void* b) { return __gthr_win32_equal(a,b); }', ['undefined reference', '__wrap___gthr_win32_equal']),
        'equal_address': ('extern "C" __declspec(dllexport) auto forbidden() -> decltype(&__gthr_win32_equal) { return &__gthr_win32_equal; }', ['undefined reference', '__wrap___gthr_win32_equal']),
    }
    for name, (body, error) in controls.items():
        source = out / (name + '.cpp')
        source.write_text(prefix + body + '\n')
        obj = source.with_suffix('.o')
        run(name + '_compile', [cxx, '-std=c++17', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                                '-O2', '-mthreads', '-c', source, '-o', obj])
        run(name + '_reject', [*link, obj, '-o', out / (name + '.dll')], error)
    if hashes != {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}:
        raise RuntimeError('source/tool changed during link controls')
    driver = sources[5].read_text()
    expected_fence = '-Wl,--wrap=_ZNSt13__future_base13_State_baseV211_Make_ready6_M_setEv'
    if driver.count(expected_fence) != 2:
        raise RuntimeError('driver must apply the _Make_ready::_M_set wrap to both probe and production links')
    audit_source = sources[6].read_text()
    if '_Make_ready::_M_set' not in audit_source or '_ZNSt13__future_base13_State_baseV211_Make_ready6_M_setEv' not in audit_source:
        raise RuntimeError('final admission must reject the discarded helper in demangled maps and PE symbols')
    print('RUNTIME_LINK_CONTROLS PASS positive=1 rejected=8 artifacts=' + str(out))


if __name__ == '__main__':
    main()
