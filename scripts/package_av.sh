#!/bin/sh
# Assemble one complete AV artifact; never publish a bundle with missing inputs.
# Inputs are repository build outputs. A private staging tree owns all copied
# bytes and is removed on success/error/signal. Existing archives survive failure.
set -eu
for name in waddle waddled waddle-av-host waddle-av-setup waddle-guest-exec.exe waddle-guest-av.exe av_wgc.dll av_windows_test.exe; do
    test -s "build/$name" || { echo "AV package: missing build/$name" >&2; exit 1; }
done
for name in ivshmem.inf ivshmem.sys ivshmem.cat LICENSE.txt; do
    test -s "build/vendor/av/ivshmem/$name" || { echo "AV package: missing signed driver $name" >&2; exit 1; }
done
for name in kvmfr.c kvmfr.h Makefile LICENSE; do
    test -s "build/vendor/av/module/$name" || { echo "AV package: missing patched module source $name" >&2; exit 1; }
done
stage=$(mktemp -d build/av_dist_XXXXXX)
trap 'rm -rf "$stage"' EXIT
trap 'exit 1' HUP INT TERM
bundle="$stage/waddle_av"
mkdir -p "$bundle/vendor/av/ivshmem" "$bundle/vendor/av/module" "$bundle/source" "$bundle/impl/high-perf-av-passthrough"
for name in waddle waddled waddle-av-host waddle-av-setup waddle-guest-exec.exe waddle-guest-av.exe av_wgc.dll av_windows_test.exe; do
    cp "build/$name" "$bundle/$name"
done
for name in ivshmem.inf ivshmem.sys ivshmem.cat LICENSE.txt; do
    cp "build/vendor/av/ivshmem/$name" "$bundle/vendor/av/ivshmem/$name"
done
for name in kvmfr.c kvmfr.h Makefile LICENSE; do
    cp "build/vendor/av/module/$name" "$bundle/vendor/av/module/$name"
done
cp LICENSE "$bundle/LICENSE"
cp impl/high-perf-av-passthrough/IMPL_DESC.md impl/high-perf-av-passthrough/TRACKER.md "$bundle/impl/high-perf-av-passthrough/"
git archive --format=tar HEAD -o "$stage/waddle_source.tar"
gzip -n -c "$stage/waddle_source.tar" > "$bundle/source/waddle.tar.gz"
git -C submodules/looking_glass archive --format=tar HEAD -o "$(pwd)/$stage/looking_glass_source.tar"
gzip -n -c "$stage/looking_glass_source.tar" > "$bundle/source/looking_glass.tar.gz"
{
    printf 'Waddle commit: '
    git rev-parse HEAD
    printf 'Looking Glass pin: '
    git rev-parse HEAD:submodules/looking_glass
    printf 'Looking Glass checked-out source: '
    git -C submodules/looking_glass rev-parse HEAD
    printf 'Driver archive SHA256: c2415a5a0c405f1d6aa936986bdd4b806c50574b4521747e113c3be2be047b1b\n'
} > "$bundle/provenance.txt"
# Fail if module/driver sources no longer match the immutable recorded gitlink.
test "$(git rev-parse HEAD:submodules/looking_glass)" = "$(git -C submodules/looking_glass rev-parse HEAD)"
cat > "$bundle/INSTALL.txt" <<'TEXT'
Extract this archive to a private directory owned by the invoking Linux user.
Keep its layout intact: run ./waddle, with sibling host/guest binaries, DLL,
privileged setup helper, signed driver package, and patched module sources.
Use waddle device commands to select a licensed Windows image and its execution
bridge, then ./waddle av setup --device NAME [--uefi] [--kvmfr] [--gpu PCI_BDF].
Setup provisions AV devices, drivers, host access and deploys the guest adapter;
./waddle av probe --device NAME validates native guest and host readiness.
./waddle av run PROCESS_ID --device NAME presents the selected guest process.
For host-clock commit RTT, run av_windows_test.exe --round-trip-fixture in the
interactive Windows desktop, then ./waddle av run PID --device NAME --latency.
The fixture runs for at most two minutes; the host measures 16 visible and 16
occluded color challenges. This measures compositor commit processing, not scanout.

Linux x86_64 base packages: QEMU with KVM/VSOCK/IVSHMEM, virtiofsd, OVMF for UEFI,
Wayland client, PipeWire, libc, sudo, make, acl, kmod, running-kernel build headers.
These system packages and a licensed Windows installation are not redistributed.
Do not mix Windows artifacts from another source commit. This CI bundle includes
its own matching Windows SDK-built av_wgc.dll; no manual artifact assembly is needed.
GPU assignment, kernel signature restrictions and actual latency remain hardware
capabilities. A VGA refresh argument does not prove a Windows 144-Hz display mode.
See impl/high-perf-av-passthrough/IMPL_DESC.md for ownership and verification limits.

Application source, original pinned driver/module source and license notices are
included under source/ and vendor/av/. The Waddle source contains the exact patch
used for the shipped module sources. Kernel module builds occur on the target
host; no kernel-version-specific .ko is redistributed. Existing live modules are
preserved. Keep sources and licenses when redistributing this package.
TEXT
(cd "$bundle" && find . -type f ! -name sha256sums.txt -print | LC_ALL=C sort | xargs sha256sum > sha256sums.txt)
tar -czf "$stage/waddle_av.tar.gz" -C "$stage" waddle_av
mv "$stage/waddle_av.tar.gz" build/waddle_av.tar.gz
printf 'AV package: build/waddle_av.tar.gz\n'
