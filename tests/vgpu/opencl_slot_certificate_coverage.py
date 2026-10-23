#!/usr/bin/env python3
"""Gate every real TLS certificate function, >=90% lines/branches, no exclusions.

This runs API doubles. The Windows branch of the same test source is separately
cross-built and must execute on the exact admitted target before native credit.
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
    output = root / "build/coverage/vgpu/opencl_slot_certificate" / (
        datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ") + "-" + str(os.getpid()))
    output.mkdir(parents=True, exist_ok=False)
    print("TLS certificate coverage evidence: " + str(output), flush=True)
    gate.CoverageFiles = ("tests/vgpu/opencl_slot_certificate_test.c",
                          "tests/vgpu/opencl_slot_certificate_windows.inc")
    gate.CoverageSource = gate.CoverageFiles[1]
    obj = output / "host.o"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                    "-O0", "-g", "--coverage", "-fprofile-abs-path", "-c",
                    gate.CoverageFiles[0], "-o", str(obj)], cwd=root, check=True)
    subprocess.run(["cc", str(obj), "--coverage", "-o", str(output / "host")], check=True)
    subprocess.run([str(output / "host")], check=True)
    subprocess.run(["gcov", "--json-format", "-b", "-c", str(output / "host.gcno")], cwd=output, check=True)
    raw = output / "host.gcov.json.gz"
    report = json.loads(gzip.decompress(raw.read_bytes()))
    summary = gate.coverage_summary(report, root)
    target = next(item for item in report["files"] if item["file"] == str(root / gate.CoverageSource))
    expected = {"slot_read", "slot_admitted", "slot_snapshot", "slot_matches", "slot_new_token", "slot_certificate_probe"}
    functions = target.get("functions")
    if not isinstance(functions, list) or len(functions) != len(expected) or {f.get("name") for f in functions} != expected:
        raise ValueError("missing, duplicate or unexpected certificate function")
    if any(type(f.get("execution_count")) is not int or f["execution_count"] <= 0 for f in functions):
        raise ValueError("unexecuted certificate function")
    summary.update(source=gate.CoverageSource, minimum_percent=gate.CoverageMinimum,
                   functions=len(expected), native_execution=False,
                   source_sha256=hashlib.sha256((root / gate.CoverageSource).read_bytes()).hexdigest(),
                   gcov_sha256=hashlib.sha256(raw.read_bytes()).hexdigest())
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
