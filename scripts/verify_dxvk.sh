#!/bin/sh
# Verify immutable, clean upstream client sources. No downloads or mutations.
# Input: repository working directory. Output: diagnostics only; no secrets.
# Return: zero iff every required source pin matches; nonzero on git/tool failure.
# Ownership/thread safety: borrows checkout; safe with other read-only verifiers.
set -eu
while read -r dependency_path dependency_sha; do
    actual_sha=$(git -C "$dependency_path" rev-parse HEAD)
    if [ "$actual_sha" != "$dependency_sha" ]; then
        echo "DXVK prerequisite revision mismatch: $dependency_path" >&2
        exit 1
    fi
    git -C "$dependency_path" diff --quiet HEAD --
done <<'Pins'
submodules/dxvk c3dd74be6baec53786d4e064a572185b70347a17
submodules/dxvk/include/native/directx 9df86f2341616ef1888ae59919feaa6d4fad693d
submodules/dxvk/include/spirv 8b246ff75c6615ba4532fe4fde20f1be090c3764
submodules/dxvk/include/vulkan 234c4b7370a8ea3239a214c9e871e4b17c89f4ab
submodules/dxvk/subprojects/libdisplay-info 275e6459c7ab1ddd4b125f28d0440716e4888078
Pins
for dependency_tool in meson ninja glslangValidator x86_64-w64-mingw32-gcc x86_64-w64-mingw32-g++; do
    command -v "$dependency_tool" >/dev/null
done
