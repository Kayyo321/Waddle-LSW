#!/usr/bin/env python3
"""Measure the real GUI diagnostic include, retaining raw unfiltered gcov data.
Reuse the existing tested strict gcov schema validator with this TU's exact file
set. Floor remains 90%; no exclusions/threshold arguments. No Windows execution.
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
    output = root / "build/coverage/vgpu/gui_diag" / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ") + "-" + str(os.getpid()))
    output.mkdir(parents=True, exist_ok=False)
    print("GUI coverage evidence: " + str(output), flush=True)
    cases = {
        "windows": ("tests/vgpu/tcp_gpu_gui_diag_host.c", "tests/vgpu/tcp_gpu_gui_diag_windows.inc"),
        "owners": ("tests/vgpu/gui_owner_snapshot_test.c", "tests/vgpu/tcp_gpu_m2_owned_windows.inc", "tests/vgpu/m2_owned_fixture_test.c"),
    }
    summaries = {}
    for name, files in cases.items():
        folder = output / name
        folder.mkdir()
        gate.CoverageFiles, gate.CoverageSource = files, files[1]
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-O0", "-g", "--coverage", "-fprofile-abs-path", "-c", files[0], "-o", str(folder / "host.o")], cwd=root, check=True)
        objects = [str(folder / "host.o")]
        if name == "owners":
            subprocess.run(["cc", "-std=c11", "-O0", "-c", "tests/vgpu/m2_owner_ledger.c", "-o", str(folder / "ledger.o")], cwd=root, check=True)
            objects.append(str(folder / "ledger.o"))
        subprocess.run(["cc", *objects, "--coverage", "-o", str(folder / "host")], check=True)
        subprocess.run([str(folder / "host")], check=True)
        subprocess.run(["gcov", "--json-format", "-b", "-c", str(folder / "host.gcno")], cwd=folder, check=True)
        raw = folder / "host.gcov.json.gz"
        summary = gate.coverage_summary(json.loads(gzip.decompress(raw.read_bytes())), root)
        summary.update(source=gate.CoverageSource, minimum_percent=gate.CoverageMinimum,
                       source_sha256=hashlib.sha256((root / gate.CoverageSource).read_bytes()).hexdigest(),
                       gcov_sha256=hashlib.sha256(raw.read_bytes()).hexdigest())
        summaries[name] = summary
    (output / "summary.json").write_text(json.dumps(summaries, indent=2) + "\n")
    print(json.dumps(summaries, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
