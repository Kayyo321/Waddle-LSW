#!/bin/sh
# Synchronize every immutable gitlink; prefetch OpenSSL through its advertised tag.
# GitHub's shallow direct-SHA upload-pack requests can fail with HTTP 504. The
# tag must resolve to the exact existing gitlink; no dependency pin is changed.
set -eu
git config --global --add safe.directory '*'
OpenSslCommit=de90e54bbe82e5be4fb9608b6f5c308bb837d355
OpenSslTag=openssl-3.0.9
OpenSslUrl=https://github.com/openssl/openssl.git
advertised=$(git ls-remote "$OpenSslUrl" "refs/tags/$OpenSslTag^{}" | cut -f1)
[ "$advertised" = "$OpenSslCommit" ] || { echo 'OpenSSL advertised tag disagrees with immutable pin' >&2; exit 1; }
git -c submodule.recurse=false submodule update --init --depth 1 submodules/qemu
git -C submodules/qemu -c submodule.recurse=false submodule update --init --depth 1 roms/edk2
git -C submodules/qemu/roms/edk2 -c submodule.recurse=false submodule update --init --depth 1 SecurityPkg/DeviceSecurity/SpdmLib/libspdm

# [in] parent/path are trusted repository-relative existing submodule locations.
# No owned memory; Git owns descriptors/temporary packfiles. Fail closed on a tag
# or gitlink mismatch, and absorb each clone into the parent's submodule storage.
seed_openssl() {
    parent=$1
    openssl_path=$2
    expected=$(git -C "$parent" rev-parse "HEAD:$openssl_path")
    [ "$expected" = "$OpenSslCommit" ] || { echo 'Unexpected OpenSSL gitlink' >&2; exit 1; }
    git -C "$parent" submodule init "$openssl_path"
    target=$parent/$openssl_path
    if [ ! -e "$target/.git" ]; then
        git clone --depth 1 --branch "$OpenSslTag" "$OpenSslUrl" "$target"
    elif ! git -C "$target" cat-file -e "$OpenSslCommit^{commit}" 2>/dev/null; then
        git -C "$target" fetch --depth 1 origin "refs/tags/$OpenSslTag"
        fetched=$(git -C "$target" rev-parse 'FETCH_HEAD^{commit}')
        [ "$fetched" = "$OpenSslCommit" ] || { echo 'Fetched OpenSSL tag disagrees with pin' >&2; exit 1; }
    fi
    git -C "$target" checkout --detach "$OpenSslCommit"
    actual=$(git -C "$target" rev-parse HEAD)
    [ "$actual" = "$expected" ] || { echo 'OpenSSL checkout disagrees with gitlink' >&2; exit 1; }
    git -C "$parent" submodule absorbgitdirs "$openssl_path"
}
seed_openssl submodules/qemu/roms/edk2 CryptoPkg/Library/OpensslLib/openssl
seed_openssl submodules/qemu/roms/edk2/SecurityPkg/DeviceSecurity/SpdmLib/libspdm os_stub/openssllib/openssl
# Recurse over all remaining dependencies and verify every declared pin normally.
git submodule update --init --recursive --depth 1 --jobs 8
