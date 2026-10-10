#!/usr/bin/env python3
"""Gate every Linux production TCP socket line and branch at90%, without exclusions."""
import gzip
import json
from pathlib import Path
import secrets
import subprocess

root = Path.cwd()
output = root / "build/coverage/vgpu" / ("venus_tcp_socket_" + secrets.token_hex(8))
output.mkdir(parents=True)
wrapped = ["socket", "setsockopt", "fcntl", "bind", "listen", "getsockname", "connect", "getsockopt",
           "accept", "poll", "send", "recv", "getrandom", "clock_gettime", "shutdown"]
subprocess.run(["cc", "-D_GNU_SOURCE", "-Iinclude", "-std=c11", "-Wall", "-Wextra",
                "-Wpedantic", "-Werror", "-O0", "-g", "--coverage", "-fprofile-abs-path",
                "-c", "src/vgpu/venus_tcp_socket.c", "-o", str(output / "socket.o")], check=True)
subprocess.run(["cc", "-D_GNU_SOURCE", "-DTcpFaultTests", "-Iinclude", "-std=c11",
                "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-O0", "-g",
                "tests/vgpu/tcp_transport.c", str(output / "socket.o"), "--coverage", "-pthread",
                "-Wl," + ",".join("--wrap=" + name for name in wrapped), "-o", str(output / "runner")], check=True)
subprocess.run([str(output / "runner")], check=True)
subprocess.run(["gcov", "--json-format", "-b", "-c", str(output / "socket.gcno")], cwd=output, check=True)
report = json.loads(gzip.decompress((output / "socket.gcov.json.gz").read_bytes()))
lines = next(record["lines"] for record in report["files"]
             if record["file"] == str(root / "src/vgpu/venus_tcp_socket.c"))
branches = [branch for line in lines for branch in line.get("branches", [])]
for kind, records in (("line", lines), ("branch", branches)):
    assert records, "Production coverage must be nonempty"
    covered = sum(record["count"] > 0 for record in records)
    percentage = 100 * covered / len(records)
    print(f"venus_tcp_socket production {kind} coverage: {percentage:.2f}% ({covered}/{len(records)})", flush=True)
    assert percentage >= 90, "Uncovered production " + kind + " threshold"
