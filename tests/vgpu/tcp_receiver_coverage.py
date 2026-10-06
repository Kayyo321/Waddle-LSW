#!/usr/bin/env python3
"""Gate every production TCP peer owner line and branch at 90%, without exclusions."""
import gzip
import json
from pathlib import Path
import shutil
import subprocess

root = Path.cwd()
output = root / "build/coverage/vgpu/venus_tcp_client"
if output.exists():
    shutil.rmtree(output)
output.mkdir(parents=True)
source = root / "src/vgpu/venus_tcp_client.c"
strict = ["-D_GNU_SOURCE", "-Iinclude", "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-O0", "-g"]
for name in ("venus_request", "venus_capabilities", "venus_tcp_wire"):
    subprocess.run(["zig", "build-obj", f"src/vgpu/{name}.zig", "-Iinclude", "-O", "ReleaseSafe", "-fPIC", "-fcompiler-rt", "-lc", f"-femit-bin={output / (name + '.o')}"], check=True)
subprocess.run(["cc", *strict, "--coverage", "-fprofile-abs-path", "-c", str(source), "-o", str(output / "client.o")], check=True)
subprocess.run(["cc", *strict, "-DTcpPeerFaultTests", "tests/vgpu/tcp_receiver.c", "tests/vgpu/tcp_wire_oracle.c", "src/vgpu/venus_tcp_socket.c", str(output / "client.o"), *(str(output / (name + ".o")) for name in ("venus_request", "venus_capabilities", "venus_tcp_wire")), "--coverage", "-pthread", "-Wl,--wrap=send,--wrap=clock_gettime,--wrap=getrandom,--wrap=shutdown", "-o", str(output / "runner")], check=True)
subprocess.run([str(output / "runner")], check=True, timeout=45)
subprocess.run(["gcov", "--json-format", "-b", "-c", str(output / "client.gcno")], cwd=output, check=True)
report = json.loads(gzip.decompress((output / "client.gcov.json.gz").read_bytes()))
lines = next(record["lines"] for record in report["files"] if record["file"] == str(source))
branches = [branch for line in lines for branch in line.get("branches", [])]
for kind, records in (("line", lines), ("branch", branches)):
    assert records, "Production coverage must be nonempty"
    covered = sum(record["count"] > 0 for record in records)
    percentage = 100 * covered / len(records)
    print(f"venus_tcp_client production {kind} coverage: {percentage:.2f}% ({covered}/{len(records)})", flush=True)
    assert percentage >= 90, "Uncovered production " + kind + " threshold"
