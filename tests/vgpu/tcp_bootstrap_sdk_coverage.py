#!/usr/bin/env python3
"""Measure actual bootstrap owner implementation with real FD/heap SDK shim.

Synthetic client/ICD admission is never Windows, remote retirement or GPU proof.
Every production C line/branch counts without exclusions; genuine native C ASan,
LSan and UBSan run separately against the same resource owner fault matrix.
"""
import gzip
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import uuid

root = Path.cwd()
sanitized = len(sys.argv) > 1 and sys.argv[1] == "sanitizers"
assert len(sys.argv) == 1 or sanitized
output = root / ("build/tcp_bootstrap_sdk_" + ("sanitizers_" if sanitized else "coverage_") + uuid.uuid4().hex[:8])
output.mkdir()
inputs = [root / path for path in ("tests/vgpu/tcp_bootstrap_sdk.c", "tests/vgpu/tcp_bootstrap_sdk.h", "src/vgpu/tcp_bootstrap_windows.c", "src/vgpu/venus_tcp_socket.c", "src/vgpu/venus_tcp_wire.zig", "include/waddle/venus_tcp.h", "include/waddle/venus_capabilities.h", "include/waddle/venus_command.h", "include/waddle/venus_request.h", "include/waddle/venus_ring.h")]
def hashes():
    return {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}
before = hashes()
subprocess.run(["zig", "build-obj", "src/vgpu/venus_tcp_wire.zig", "-Iinclude", "-O", "ReleaseSafe", "-fPIC", "-fcompiler-rt", "-lc", "-femit-bin=" + str(output / "wire.o")], check=True)
strict = ["-D_GNU_SOURCE", "-Iinclude", "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-g"]
flags = ["-O1", "-fsanitize=address,leak,undefined", "-fno-omit-frame-pointer"] if sanitized else ["-O0", "--coverage", "-fprofile-abs-path"]
subprocess.run(["cc", *strict, *flags, "tests/vgpu/tcp_bootstrap_sdk.c", "src/vgpu/venus_tcp_socket.c", str(output / "wire.o"), "-o", str(output / "runner")], check=True)
environment = os.environ.copy()
environment["ASAN_OPTIONS"] = "detect_leaks=1:abort_on_error=1:halt_on_error=1"
environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
subprocess.run([str(output / "runner")], check=True, env=environment, timeout=20)
if not sanitized:
    subprocess.run(["gcov", "--json-format", "-b", "-c", str(output / "runner-tcp_bootstrap_sdk.gcno")], cwd=output, check=True)
    report = json.loads(gzip.decompress((output / "runner-tcp_bootstrap_sdk.gcov.json.gz").read_bytes()))
    lines = next(record["lines"] for record in report["files"] if record["file"] == str(root / "src/vgpu/tcp_bootstrap_windows.c"))
    branches = [branch for line in lines for branch in line.get("branches", [])]
    for kind, records in (("line", lines), ("branch", branches)):
        assert records
        covered = sum(record["count"] > 0 for record in records)
        percentage = 100 * covered / len(records)
        print(f"bootstrap production {kind}: {covered}/{len(records)} ({percentage:.2f}%)", flush=True)
        assert percentage >= 90
assert hashes() == before, "Bootstrap source changed while tested"
(output / "provenance.json").write_text(json.dumps({"inputs": before, "sanitized_native_c": sanitized, "synthetic_transport": True, "actual_resource_owners": "Linux file descriptors and heap cells", "windows_or_gpu_acceptance": False}, indent=2) + "\n")
print("Preserved bootstrap ownership evidence:", output)
