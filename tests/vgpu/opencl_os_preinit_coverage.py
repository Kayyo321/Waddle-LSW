#!/usr/bin/env python3
"""Gate every OS pre-initialization function and >=90% lines/branches of the actual include.
Own a unique output directory and preserve raw gcov JSON and source hashes.
No exclusions, optional thresholds, native Windows execution or GPU use.
"""
from datetime import datetime, timezone
import gzip
import hashlib
import json
import os
from pathlib import Path
import subprocess
import handle_diag_coverage as gate


def main():
    root = Path(__file__).resolve().parents[2]
    output = root / "build/coverage/vgpu/opencl_os_preinit" / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ") + "-" + str(os.getpid()))
    output.mkdir(parents=True, exist_ok=False)
    print("OS pre-initialization coverage evidence: " + str(output), flush=True)
    gate.CoverageFiles = ("tests/vgpu/opencl_os_preinit_test.c", "tests/vgpu/opencl_os_preinit_windows.inc")
    gate.CoverageSource = gate.CoverageFiles[1]
    obj = output / "host.o"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-O0", "-g", "-Itests/vgpu", "--coverage", "-fprofile-abs-path", "-c", gate.CoverageFiles[0], "-o", str(obj)], cwd=root, check=True)
    subprocess.run(["cc", str(obj), "--coverage", "-o", str(output / "host")], check=True)
    subprocess.run([str(output / "host")], check=True)
    subprocess.run(["gcov", "--json-format", "-b", "-c", str(output / "host.gcno")], cwd=output, check=True)
    raw = output / "host.gcov.json.gz"
    report = json.loads(gzip.decompress(raw.read_bytes()))
    summary = gate.coverage_summary(report, root)
    target = next(item for item in report["files"] if item["file"] == str(root / gate.CoverageSource))
    expected = {"os_preinit_load", "os_preinit_resolve", "os_preinit_user32", "os_preinit_winsock",
                "os_preinit_bcrypt", "os_preinit_dxgi_module", "os_preinit_search_order",
                "os_preinit_package_family", "os_preinit_winsock_connect", "os_preinit_user_hive",
                "os_preinit_random", "os_preinit_d3dkmt_adapters", "os_preinitialize"}
    functions = target.get("functions")
    if not isinstance(functions, list) or len(functions) != len(expected) or {f.get("name") for f in functions} != expected:
        raise ValueError("missing, duplicate or unexpected OS pre-initialization function inventory")
    if any(type(f.get("execution_count")) is not int or f["execution_count"] <= 0 for f in functions):
        raise ValueError("unexecuted OS pre-initialization function")
    summary.update(source=gate.CoverageSource, minimum_percent=gate.CoverageMinimum,
                   functions=len(expected), source_sha256=hashlib.sha256((root / gate.CoverageSource).read_bytes()).hexdigest(),
                   gcov_sha256=hashlib.sha256(raw.read_bytes()).hexdigest())
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
