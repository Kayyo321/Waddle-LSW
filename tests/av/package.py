"""Verify complete AV package publication and preservation of existing archives."""
import hashlib
import pathlib
import subprocess
import tarfile
import tempfile

Root = pathlib.Path(__file__).resolve().parents[2]
Binaries = ('waddle', 'waddled', 'waddle-av-host', 'waddle-av-setup',
            'waddle-guest-exec.exe', 'waddle-guest-av.exe', 'av_wgc.dll', 'av_windows_test.exe')
Drivers = ('ivshmem.inf', 'ivshmem.sys', 'ivshmem.cat', 'LICENSE.txt')
Module = ('kvmfr.c', 'kvmfr.h', 'Makefile', 'LICENSE')


def run(arguments, directory):
    return subprocess.run(arguments, cwd=directory, check=True,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


with tempfile.TemporaryDirectory(prefix='waddle_av_package_') as temporary:
    base = pathlib.Path(temporary)
    dependency = base / 'submodules/looking_glass'
    dependency.mkdir(parents=True)
    for repo in (base, dependency):
        run(['git', 'init', '-q'], repo)
        run(['git', 'config', 'user.email', 'fixture@example.invalid'], repo)
        run(['git', 'config', 'user.name', 'Package Fixture'], repo)
        (repo / 'LICENSE').write_text('fixture source license\n')
        run(['git', 'add', 'LICENSE'], repo)
        run(['git', 'commit', '-qm', 'Fixture source'], repo)
    run(['git', 'add', 'submodules/looking_glass'], base)
    run(['git', 'commit', '-qm', 'Pin fixture dependency'], base)
    inputs = [pathlib.Path('build') / name for name in Binaries]
    inputs += [pathlib.Path('build/vendor/av/ivshmem') / name for name in Drivers]
    inputs += [pathlib.Path('build/vendor/av/module') / name for name in Module]
    for path in inputs:
        destination = base / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(str(path).encode() + b'\n')
    description = base / 'impl/high-perf-av-passthrough'
    description.mkdir(parents=True)
    for name in ('IMPL_DESC.md', 'TRACKER.md'):
        (description / name).write_text('Fixture documentation\n')
    script = Root / 'scripts/package_av.sh'
    run(['sh', str(script)], base)
    archive = base / 'build/waddle_av.tar.gz'
    original = archive.read_bytes()
    with tarfile.open(archive) as packed:
        names = packed.getnames()
        for name in Binaries:
            assert 'waddle_av/' + name in names
        assert not any(name.endswith('.ko') for name in names)
        assert 'waddle_av/source/looking_glass.tar.gz' in names
        manifest = packed.extractfile('waddle_av/sha256sums.txt').read().decode()
        for line in manifest.splitlines():
            digest, name = line.split('  ', 1)
            data = packed.extractfile('waddle_av/' + name[2:]).read()
            assert hashlib.sha256(data).hexdigest() == digest
    for path in inputs:
        destination = base / path
        contents = destination.read_bytes()
        destination.unlink()
        result = subprocess.run(['sh', str(script)], cwd=base,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        assert result.returncode != 0, path
        assert archive.read_bytes() == original, path
        assert not list((base / 'build').glob('av_dist_*'))
        destination.write_bytes(contents)
print('AV package: complete manifest verified; every missing input preserves existing archive')
