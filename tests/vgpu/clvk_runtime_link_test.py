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
    out = root / 'build/qualification_runtime_link' / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    out.mkdir(parents=True, exist_ok=False)
    cc = shutil.which(os.environ.get('WADDLE_CLVK_CC', 'x86_64-w64-mingw32-gcc-win32'))
    cxx = shutil.which(os.environ.get('WADDLE_CLVK_CXX', 'x86_64-w64-mingw32-g++-win32'))
    if not cc or not cxx:
        raise RuntimeError('explicit reviewed Win32 compiler pair required')
    sources = [root / 'src/vgpu/clvk_runtime_tls.c', root / 'src/vgpu/clvk_runtime_eh_pool.c',
               root / 'tests/vgpu/clvk_runtime_windows.cpp', root / 'scripts/clvk/runtime_sections.ld',
               Path(__file__).resolve(), Path(cc).resolve(), Path(cxx).resolve()]
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
    link = [cxx, '-shared', '-mthreads', probe, *objects, '-static-libgcc', '-static-libstdc++',
            '-Wl,--gc-sections', '-Wl,--undefined=waddle_clvk_runtime_tls_limits',
            '-Wl,-T,' + str(sources[3]), '-Wl,--wrap=__gthr_win32_equal',
            '-Wl,--wrap=_ZSt16__at_thread_exitPSt20__at_thread_exit_elt',
            '-Wl,--wrap=_ZSt25notify_all_at_thread_exitRSt18condition_variableSt11unique_lockISt5mutexE']
    dll = out / 'runtime_link_positive.dll'
    run('positive', [*link, '-o', dll, '-Wl,-Map,' + str(out / 'positive.map') + ',--demangle'])
    nm = shutil.which('x86_64-w64-mingw32-nm')
    if not nm:
        raise RuntimeError('nm required to validate actual mapped symbol absence')
    symbols = subprocess.check_output([nm, str(dll)], text=True, timeout=60)
    (out / 'positive.nm').write_text(symbols)
    for line in symbols.splitlines():
        fields = line.split()
        if len(fields) == 3 and any(token in fields[2] for token in ('__at_thread_exit', 'notify_all_at_thread_exit')):
            raise RuntimeError('forbidden facility remains mapped: ' + line)
    prefix = '#include <condition_variable>\n#include <mutex>\nnamespace std { void __at_thread_exit(__at_thread_exit_elt*); }\nextern "C" int __gthr_win32_equal(void*, void*);\n'
    controls = {
        'internal_call': ('extern "C" __declspec(dllexport) void forbidden() { std::__at_thread_exit(nullptr); }', ['undefined reference', '__wrap__ZSt16__at_thread_exit']),
        'internal_address': ('extern "C" __declspec(dllexport) auto forbidden() -> decltype(&std::__at_thread_exit) { return &std::__at_thread_exit; }', ['undefined reference', '__wrap__ZSt16__at_thread_exit']),
        'notify_call': ('extern "C" __declspec(dllexport) void forbidden(std::condition_variable* c, std::mutex* m) { std::notify_all_at_thread_exit(*c, std::unique_lock<std::mutex>(*m)); }', ['undefined reference', '__wrap__ZSt25notify_all_at_thread_exit']),
        'notify_address': ('extern "C" __declspec(dllexport) auto forbidden() -> decltype(&std::notify_all_at_thread_exit) { return &std::notify_all_at_thread_exit; }', ['undefined reference', '__wrap__ZSt25notify_all_at_thread_exit']),
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
    print('RUNTIME_LINK_CONTROLS PASS positive=1 rejected=6 artifacts=' + str(out))


if __name__ == '__main__':
    main()
