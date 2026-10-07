#!/usr/bin/env python3
"""Gate every production TCP peer owner line and branch at 90%, without exclusions."""
from datetime import datetime, timezone
import os
import gzip
import json
from pathlib import Path
import subprocess
import sys

root = Path.cwd()
owner = sys.argv[1] if len(sys.argv) > 1 else "client"
assert owner in ("client", "server")
module = "venus_tcp_" + owner
output = root / ("build/coverage/vgpu/" + module) / (
    "run-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ") + "-" + str(os.getpid()))
output.mkdir(parents=True, exist_ok=False)
print("preserved TCP peer coverage: " + str(output), flush=True)
source = root / ("src/vgpu/" + module + ".c")
strict = ["-D_GNU_SOURCE", "-Iinclude", "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-O0", "-g"]
for name in ("venus_request", "venus_capabilities", "venus_tcp_wire"):
    subprocess.run(["zig", "build-obj", f"src/vgpu/{name}.zig", "-Iinclude", "-O", "ReleaseSafe", "-fPIC", "-fcompiler-rt", "-lc", f"-femit-bin={output / (name + '.o')}"], check=True)
subprocess.run(["cc", *strict, "--coverage", "-fprofile-abs-path", "-c", str(source), "-o", str(output / (owner + ".o"))], check=True)
subprocess.run(["cc", *strict, "-DTcpPeerFaultTests", "tests/vgpu/tcp_receiver_server.c" if owner == "server" else "tests/vgpu/tcp_receiver.c", "tests/vgpu/tcp_wire_oracle.c", "src/vgpu/venus_tcp_socket.c", str(output / (owner + ".o")), *(str(output / (name + ".o")) for name in ("venus_request", "venus_capabilities", "venus_tcp_wire")), "--coverage", "-pthread", "-Wl,--wrap=send,--wrap=clock_gettime,--wrap=getrandom" + (",--wrap=shutdown,--wrap=recv" if owner == "client" else ",--wrap=recv"), "-o", str(output / "runner")], check=True)
subprocess.run([str(output / "runner")], check=True, timeout=45)
subprocess.run(["gcov", "--json-format", "-b", "-c", str(output / (owner + ".gcno"))], cwd=output, check=True)
report = json.loads(gzip.decompress((output / (owner + ".gcov.json.gz")).read_bytes()))
lines = next(record["lines"] for record in report["files"] if record["file"] == str(source))
branches = [branch for line in lines for branch in line.get("branches", [])]
for kind, records in (("line", lines), ("branch", branches)):
    assert records, "Production coverage must be nonempty"
    covered = sum(record["count"] > 0 for record in records)
    percentage = 100 * covered / len(records)
    print(f"{module} production {kind} coverage: {percentage:.2f}% ({covered}/{len(records)})", flush=True)
    assert percentage >= 90, "Uncovered production " + kind + " threshold"
