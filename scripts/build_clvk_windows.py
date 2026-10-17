#!/usr/bin/env python3
"""Cross-build the pinned clvk OpenCL runtime (with in-process clspv) for Windows.

Inputs are the clean verified submodules (scripts/verify_clvk.sh lists the pins),
the tracked export list scripts/clvk/OpenCL.def and the MinGW-w64/Ninja/CMake
toolchain. Nothing upstream is patched: the sources are copied to a private tree,
and every MinGW accommodation (case-insensitive Windows.h, export list, static
C++ runtime) is supplied from this driver through include paths and linker flags.

Outputs: a fresh ignored private build tree under build/clvk_client/ and the
receipt build/clvk_client.json (pins, tools, settings, driver/export-list hashes,
DLL hash and import list), published atomically under a process lock. Earlier
build trees and receipts are preserved. The shared native LLVM tool build lives in
build/clvk_client/native_<key>/ and is reused (incrementally) while its inputs match.

Cross-compilation is not execution: the DLL is unexecuted and unverified here.
No Rust or Go participates in this build.
"""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

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
Tools = ('cmake', 'ninja', 'x86_64-w64-mingw32-gcc-posix', 'x86_64-w64-mingw32-g++-posix',
         'x86_64-w64-mingw32-dlltool', 'x86_64-w64-mingw32-objdump', 'x86_64-w64-mingw32-nm')
# Private copies of the clvk tree skip VCS metadata; large third-party trees are symlinked.
CopyIgnore = shutil.ignore_patterns('.git', '.github')
ForbiddenImports = ('libgcc_s', 'libstdc++', 'libwinpthread')
RequiredExports = ('clGetPlatformIDs', 'clBuildProgram', 'clCreateProgramWithSource',
                   'clCreateKernel', 'clEnqueueNDRangeKernel', 'clFinish', 'clIcdGetPlatformIDsKHR')
LlvmSwitches = ('-DLLVM_INCLUDE_TESTS=OFF', '-DLLVM_INCLUDE_BENCHMARKS=OFF',
                '-DLLVM_INCLUDE_EXAMPLES=OFF', '-DLLVM_INCLUDE_DOCS=OFF',
                '-DCLANG_INCLUDE_TESTS=OFF', '-DCLANG_INCLUDE_DOCS=OFF',
                '-DLLVM_ENABLE_ZLIB=OFF', '-DLLVM_ENABLE_ZSTD=OFF', '-DLLVM_ENABLE_LIBXML2=OFF',
                '-DLLVM_ENABLE_TERMINFO=OFF', '-DLLVM_ENABLE_LIBEDIT=OFF',
                '-DLLVM_ENABLE_ASSERTIONS=OFF')


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


def read_export_list(path):
    """Return the ordered export names listed in a module-definition file."""
    return [line.strip() for line in Path(path).read_text().splitlines() if line.startswith('    ')]


def check_export_list_against_api(root, exports):
    """Fail closed when the export list and clvk's api.cpp definitions disagree.

    Every `CLVK_API_CALL cl*` function defined by api.cpp must be exported, and every
    exported cl* name must be defined there. The two clspv entry points are exempt.
    """
    source = (root / 'submodules/clvk/src/api.cpp').read_text()
    defined = set(re.findall(r'CLVK_API_CALL\s+(cl\w+)\s*\(', source))
    exported = {name for name in exports if not name.startswith('clspv')}
    missing = sorted(defined - exported)
    extra = sorted(exported - defined)
    if missing or extra:
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


def build_native(root, native, libclc):
    """Build host LLVM TableGen tools and the Vulkan libclc bitcode (incremental).

    [in] native persistent directory; [out] libclc directory of symlinks to the
    spirv{32,64}-unknown-vulkan libclc.bc files clspv links in.
    """
    llvm = root / 'submodules/llvm_project'
    run(['cmake', '-G', 'Ninja', '-S', llvm / 'llvm', '-B', native,
         '-C', llvm / 'libclc/cmake/caches/spirv-vulkan.cmake', '-DCMAKE_BUILD_TYPE=Release',
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


def prepare_vulkan(root, vulkan):
    """Create the Vulkan header/import-library inputs from repository pins only.

    Headers come from the pinned Vulkan-Headers inside DXVK; the import library is
    generated from the pinned loader's export definition. No system header is used.
    """
    include = vulkan / 'include'
    include.mkdir(parents=True)
    headers = root / 'submodules/dxvk/include/vulkan/include'
    for name in ('vulkan', 'vk_video'):
        (include / name).symlink_to(headers / name)
    run(['x86_64-w64-mingw32-dlltool', '-d', root / 'submodules/vulkan_loader/loader/vulkan-1.def',
         '-l', vulkan / 'libvulkan-1.dll.a', '-D', 'vulkan-1.dll'])
    shim = vulkan / 'shim'
    shim.mkdir()
    (shim / 'Windows.h').write_text('#include <windows.h>\n')
    return include, shim


def write_toolchain(path):
    """Write the MinGW-w64 (posix threads) CMake toolchain file."""
    path.write_text('\n'.join((
        'set(CMAKE_SYSTEM_NAME Windows)',
        'set(CMAKE_SYSTEM_PROCESSOR x86_64)',
        'set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc-posix)',
        'set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++-posix)',
        'set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)',
        'set(CMAKE_AR x86_64-w64-mingw32-ar)',
        'set(CMAKE_RANLIB x86_64-w64-mingw32-ranlib)',
        'set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)',
        'set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)',
        'set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)',
        'set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)',
        'set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)', '')))


def inspect_dll(dll):
    """Return import and export names of a PE file; fail on a forbidden runtime import."""
    dump = capture(['x86_64-w64-mingw32-objdump', '-p', dll])
    imports = re.findall(r'DLL Name:\s*(\S+)', dump)
    for name in imports:
        if name.lower().startswith(ForbiddenImports):
            raise SystemExit('OpenCL.dll imports an unbundled C++ runtime DLL: ' + name)
    exports = set(re.findall(r'\[\s*\d+\]\s+\+base\[\s*\d+\]\s+[0-9a-f]+\s+(\S+)', dump))
    absent = [name for name in RequiredExports if name not in exports]
    if absent:
        raise SystemExit('OpenCL.dll is missing required exports: %s' % absent)
    return imports, sorted(exports)


def main():
    """Verify pins, build or reuse OpenCL.dll, and atomically publish the receipt."""
    root = Path.cwd().resolve()
    run(['sh', 'scripts/verify_clvk.sh'])
    build = root / 'build'
    build.mkdir(exist_ok=True)
    export_file = root / 'scripts/clvk/OpenCL.def'
    exports = read_export_list(export_file)
    check_export_list_against_api(root, exports)
    tools = {name: capture([name, '--version']).splitlines()[0] for name in Tools}
    settings = {'cmake_build_type': 'Release', 'clvk_vulkan_implementation': 'custom',
                'clvk_clspv_online_compiler': 'ON', 'llvm_targets_to_build': 'X86',
                'static_runtime': 'libgcc,libstdc++,winpthread', 'llvm_switches': LlvmSwitches}
    inputs = {'pins': dict(Pins), 'driver_sha256': digest(Path(__file__)),
              'export_list_sha256': digest(export_file), 'tools': tools, 'settings': settings}
    cache_key = hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()
    native_key = hashlib.sha256(json.dumps({'llvm': dict(Pins)['submodules/llvm_project'], 'tools': tools,
                                            'switches': LlvmSwitches}, sort_keys=True).encode()).hexdigest()
    receipt = build / 'clvk_client.json'
    base = build / 'clvk_client'
    base.mkdir(exist_ok=True)
    with (build / '.clvk_client.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if receipt.exists():
            prior = json.loads(receipt.read_text())
            if prior['cache_key'] == cache_key and digest(prior['dll']['path']) == prior['dll']['sha256']:
                print('Verified OpenCL cache:', receipt, flush=True)
                return
        native = base / ('native_' + native_key[:12])
        native.mkdir(exist_ok=True)
        libclc = native / 'libclc_vulkan'
        build_native(root, native, libclc)
        folder = Path(tempfile.mkdtemp(prefix='clvk_' + cache_key[:12] + '_', dir=base))
        source = folder / 'source'
        prepare_source(root, source)
        include, shim = prepare_vulkan(root, folder / 'vulkan')
        toolchain = folder / 'mingw-w64-x86_64.cmake'
        write_toolchain(toolchain)
        cross = folder / 'cross'
        linker = ' '.join((str(export_file), '-static-libgcc', '-static-libstdc++',
                           '-Wl,--whole-archive', '-Wl,-Bstatic', '-lwinpthread',
                           '-Wl,--no-whole-archive', '-Wl,-Bdynamic'))
        # LLVM_PARALLEL_LINK_JOBS is deliberately not set here: clvk and clspv include LLVM's
        # option handling several times and Ninja rejects the resulting duplicate link pool.
        run(['cmake', '-G', 'Ninja', '-S', source, '-B', cross, '-DCMAKE_TOOLCHAIN_FILE=' + str(toolchain),
             '-DCMAKE_BUILD_TYPE=Release', '-DCLVK_VULKAN_IMPLEMENTATION=custom',
             '-DVulkan_INCLUDE_DIRS=' + str(include), '-DVulkan_LIBRARIES=' + str(folder / 'vulkan/libvulkan-1.dll.a'),
             '-DCLVK_CLSPV_ONLINE_COMPILER=ON', '-DCLVK_COMPILER_AVAILABLE=ON',
             '-DCLVK_BUILD_TESTS=OFF', '-DCLSPV_BUILD_TESTS=OFF',
             '-DCLSPV_EXTERNAL_LIBCLC_DIR=' + str(libclc),
             '-DLLVM_TABLEGEN=' + str(native / 'bin/llvm-tblgen'), '-DCLANG_TABLEGEN=' + str(native / 'bin/clang-tblgen'),
             '-DLLVM_NATIVE_TOOL_DIR=' + str(native / 'bin'), '-DLLVM_TARGETS_TO_BUILD=X86',
             '-DCMAKE_CXX_FLAGS=-isystem ' + str(shim), '-DCMAKE_C_FLAGS=-isystem ' + str(shim),
             '-DCMAKE_SHARED_LINKER_FLAGS=' + linker, *LlvmSwitches])
        run(['ninja', '-C', cross, '-j', jobs(), 'OpenCL'])
        produced = [item for item in cross.rglob('libOpenCL.dll') if item.is_file()] or \
                   [item for item in cross.rglob('OpenCL.dll') if item.is_file()]
        if len(produced) != 1:
            raise SystemExit('Expected exactly one OpenCL DLL, found %d' % len(produced))
        output = folder / 'out'
        output.mkdir()
        dll = output / 'OpenCL.dll'
        shutil.copyfile(produced[0], dll)
        imports, built_exports = inspect_dll(dll)
        record = {'cache_key': cache_key, 'inputs': inputs, 'native_dir': str(native),
                  'dll': {'path': str(dll), 'sha256': digest(dll), 'bytes': dll.stat().st_size},
                  'imports': imports, 'export_count': len(built_exports), 'executed': False,
                  'note': 'Cross-built only; runtime behavior is unverified.'}
        temporary = receipt.with_suffix('.json.tmp')
        temporary.write_text(json.dumps(record, indent=2, sort_keys=True) + '\n')
        os.replace(temporary, receipt)
        print('Published', receipt, flush=True)


if __name__ == '__main__':
    main()
