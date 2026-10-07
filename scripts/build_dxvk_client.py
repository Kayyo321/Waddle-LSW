#!/usr/bin/env python3
"""Build pinned DXVK with the repository-owned Windows resource-owner fix.

Inputs are the clean verified submodules, tracked patch and toolchain. Outputs
are a fresh ignored source/build directory and build/dxvk_client.json containing
pin, patch, tool and DLL hashes. Existing builds and snapshots are preserved.
Only the private source copy is patched. A process lock serializes publication.
"""
import fcntl
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile

Dependencies = (
    ('submodules/dxvk', '.'),
    ('submodules/dxvk/include/native/directx', 'include/native/directx'),
    ('submodules/dxvk/include/spirv', 'include/spirv'),
    ('submodules/dxvk/include/vulkan', 'include/vulkan'),
    ('submodules/dxvk/subprojects/libdisplay-info', 'subprojects/libdisplay-info'),
)
Settings = ('-Dbuildtype=release', '-Denable_dxgi=true', '-Denable_d3d11=true',
            '-Denable_d3d8=false', '-Denable_d3d9=false', '-Denable_d3d10=false')


def digest(path):
    """Return SHA-256 of borrowed existing file bytes without mutation."""
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    """Build/reuse exact inputs and atomically publish owned output provenance."""
    root = Path.cwd().resolve()
    subprocess.run(['sh', 'scripts/verify_dxvk.sh'], check=True)
    build = root / 'build'
    build.mkdir(exist_ok=True)
    patch = root / 'patches/dxvk/setupapi_device_info_owner.patch'
    pins = {name: subprocess.check_output(['git', '-C', name, 'rev-parse', 'HEAD'], text=True).strip()
            for name, unused in Dependencies}
    tools = {name: subprocess.check_output([name, '--version'], text=True).splitlines()[0]
             for name in ('meson', 'ninja', 'glslangValidator', 'x86_64-w64-mingw32-gcc', 'x86_64-w64-mingw32-g++')}
    inputs = {'pins': pins, 'patch_sha256': digest(patch), 'driver_sha256': digest(Path(__file__)),
              'tools': tools, 'settings': Settings, 'cross_file': 'build-win64.txt'}
    cache_key = hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()
    receipt = build / 'dxvk_client.json'
    with (build / '.dxvk_client.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if receipt.exists():
            prior = json.loads(receipt.read_text())
            if prior['cache_key'] == cache_key:
                for item in prior['dlls'].values():
                    assert digest(Path(item['path'])) == item['sha256'], 'Cached DXVK DLL changed'
                print('Verified patched DXVK cache:', receipt, flush=True)
                return
        folder = Path(tempfile.mkdtemp(prefix='dxvk_client_' + cache_key[:12] + '_', dir=build))
        source = folder / 'source'
        source.mkdir()
        for name, relative in Dependencies:
            destination = source / relative
            destination.mkdir(parents=True, exist_ok=True)
            data = subprocess.check_output(['git', '-C', name, 'archive', pins[name]])
            with tarfile.open(fileobj=io.BytesIO(data)) as archive:
                archive.extractall(destination, filter='data')
        subprocess.run(['git', 'apply', '--check', str(patch)], cwd=source, check=True)
        subprocess.run(['git', 'apply', str(patch)], cwd=source, check=True)
        (folder / 'inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
        environment = dict(os.environ, GIT_CEILING_DIRECTORIES=str(folder))
        objects = folder / 'objects'
        subprocess.run(['meson', 'setup', str(objects), str(source), '--cross-file', str(source / 'build-win64.txt'),
                        '--wrap-mode=nodownload', *Settings], env=environment, check=True)
        subprocess.run(['ninja', '-C', str(objects), '-j2'], env=environment, check=True)
        dlls = {name: {'path': str(objects / 'src' / name / (name + '.dll')),
                       'sha256': digest(objects / 'src' / name / (name + '.dll'))}
                for name in ('dxgi', 'd3d11')}
        assert digest(patch) == inputs['patch_sha256'], 'Patch changed during build'
        subprocess.run(['sh', 'scripts/verify_dxvk.sh'], check=True)
        record = {'cache_key': cache_key, 'inputs': inputs, 'source': str(source), 'objects': str(objects), 'dlls': dlls}
        (folder / 'provenance.json').write_text(json.dumps(record, indent=2) + '\n')
        temporary = build / 'dxvk_client.json.next'
        temporary.write_text(json.dumps(record, indent=2) + '\n')
        temporary.replace(receipt)
        print('Pinned DXVK plus tracked owner patch:', receipt, flush=True)


if __name__ == '__main__':
    main()
