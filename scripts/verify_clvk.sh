#!/bin/sh
# Verify immutable, clean OpenCL (clvk/clspv/LLVM) client sources. No downloads or mutations.
# Input: repository working directory. Output: diagnostics only; no secrets.
# Return: zero iff every required source pin matches and its tree is unmodified;
# nonzero on git/tool failure. Ownership/thread safety: borrows the checkout; safe
# with other read-only verifiers. Pins mirror impl/software-vgpu-slicing/IMPL_DESC.md 5.2.
set -eu
while read -r dependency_path dependency_sha; do
    actual_sha=$(git -C "$dependency_path" rev-parse HEAD)
    if [ "$actual_sha" != "$dependency_sha" ]; then
        echo "OpenCL prerequisite revision mismatch: $dependency_path" >&2
        exit 1
    fi
    git -C "$dependency_path" diff --quiet HEAD --
done <<'Pins'
submodules/clvk 8bd3672174764dce25ba220773e10baa752f0373
submodules/clvk/external/clspv 7ee3a40e652b3649d175758f72d7af98b9450a68
submodules/clvk/external/OpenCL-Headers 386ca390b2f52efeb3e1a55a500690eb8013f60e
submodules/clvk/external/SPIRV-Headers 2b7475f6d664efbad4d4315c94ecec9640831e5b
submodules/clvk/external/SPIRV-Tools ba9a9e32160e2500ce1e48cc563a8fedaf596e6e
submodules/clvk/external/SPIRV-LLVM-Translator c4a6b42303c6b45a64ede3d1c0399dce2d90f3fc
submodules/llvm_project 71a85ef89fc1997205104b72447e0dc2c4ad83da
submodules/clspv_spirv_headers b824a462d4256d720bebb40e78b9eb8f78bbb305
submodules/clspv_spirv_tools fb7471844504abb16b732dc4fc0837119a32ec24
Pins
for dependency_tool in cmake ninja python3 x86_64-w64-mingw32-gcc x86_64-w64-mingw32-g++; do
    command -v "$dependency_tool" >/dev/null
done
# Cross-compilation is distinct from native execution/compatibility acceptance.
