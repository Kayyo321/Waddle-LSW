#!/usr/bin/env python3
"""Gate real M2 fixture-controller include coverage; retain unfiltered gcov evidence.

The host fixture includes the actual native ownership controller over API doubles.
Only the real include contributes to coverage; this does not execute Windows.
"""
from datetime import datetime, timezone
import gzip
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import os

# Fixed repository memory-code floor; no command-line override or exclusions.
CoverageMinimum = 90
# Exactly these source files contribute gcov records to the compiled host TU.
CoverageFiles = ("tests/vgpu/m2_owned_fixture_test.c",
                 "tests/vgpu/tcp_gpu_m2_owned_windows.inc")
# The existing compiled native implementation is the only measured denominator.
CoverageSource = "tests/vgpu/tcp_gpu_m2_owned_windows.inc"


def coverage_summary(report, root):
    """in: borrowed decoded gcov report and repository root; out: owned summary.

    Require exactly one record per expected file, nonempty target lines/branches,
    unique positive line numbers and nonnegative integer counts. Every recorded
    target line and branch outcome contributes; there are no exclusions. Raise
    ValueError on malformed, missing or below-floor evidence. Pure/thread-safe,
    no mutation, retained input, file access or external resource ownership.
    """
    if not isinstance(report, dict) or not isinstance(report.get("files"), list):
        raise ValueError("missing gcov files array")
    expected = {str(root / name) for name in CoverageFiles}
    files = {}
    for item in report["files"]:
        if not isinstance(item, dict) or not isinstance(item.get("file"), str):
            raise ValueError("malformed gcov file record")
        name = item["file"]
        if name not in expected or name in files:
            raise ValueError("unexpected or duplicate gcov file: " + name)
        files[name] = item
    if set(files) != expected:
        raise ValueError("missing expected gcov source file")
    lines = files[str(root / CoverageSource)].get("lines")
    if not isinstance(lines, list) or not lines:
        raise ValueError("no production lines recorded")
    numbers = set()
    line_counts = []
    branch_counts = []
    for line in lines:
        if not isinstance(line, dict):
            raise ValueError("malformed production line")
        number = line.get("line_number")
        count = line.get("count")
        branches = line.get("branches")
        if type(number) is not int or number <= 0 or number in numbers:
            raise ValueError("invalid or duplicate production line number")
        if type(count) is not int or count < 0 or not isinstance(branches, list):
            raise ValueError("invalid production line count or branch array")
        numbers.add(number)
        line_counts.append(count)
        for branch in branches:
            count = branch.get("count") if isinstance(branch, dict) else None
            if type(count) is not int or count < 0:
                raise ValueError("invalid production branch outcome count")
            branch_counts.append(count)
    if not branch_counts:
        raise ValueError("no production branch outcomes recorded")
    summary = {}
    for kind, counts in (("lines", line_counts), ("branches", branch_counts)):
        covered = sum(count > 0 for count in counts)
        total = len(counts)
        summary[kind] = {"covered": covered, "total": total,
                         "percent": 100 * covered / total}
        if 100 * covered < CoverageMinimum * total:
            raise ValueError(f"production {kind} coverage below {CoverageMinimum}%: {covered}/{total}")
    return summary


def main():
    """Compile/run the normal fixture and gate its preserved real-source report.

    No input arguments. Return 0 on sufficient coverage; tool/schema/coverage
    failures propagate nonzero. Own a fresh build directory, binary and gcov
    files per invocation; retain them for review. No VM/GPU/native Windows use.
    Separate invocations have unique output directories and independent counters.
    """
    root = Path(__file__).resolve().parents[2]
    output = root / "build/coverage/vgpu/m2_owned_fixture" / (
        "run-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ") + "-" + str(os.getpid()))
    output.mkdir(parents=True, exist_ok=False)
    print("preserved M2 fixture controller coverage: " + str(output), flush=True)
    obj = output / "host.o"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                    "-O0", "-g", "--coverage", "-fprofile-abs-path", "-c", CoverageFiles[0],
                    "-o", str(obj)], cwd=root, check=True)
    subprocess.run(["cc", str(obj), "tests/vgpu/m2_owner_ledger.c", "--coverage", "-o", str(output / "runner")], cwd=root, check=True)
    subprocess.run([str(output / "runner")], cwd=root, check=True)
    subprocess.run(["gcov", "--json-format", "-b", "-c", str(output / "host.gcno")], cwd=output, check=True)
    raw_path = output / "host.gcov.json.gz"
    report = json.loads(gzip.decompress(raw_path.read_bytes()))
    summary = coverage_summary(report, root)
    summary["source"] = CoverageSource
    summary["source_sha256"] = hashlib.sha256((root / CoverageSource).read_bytes()).hexdigest()
    summary["gcov_sha256"] = hashlib.sha256(raw_path.read_bytes()).hexdigest()
    summary["minimum_percent"] = CoverageMinimum
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    for kind in ("lines", "branches"):
        value = summary[kind]
        print(f"M2 fixture controller production {kind} coverage: {value['percent']:.2f}% "
              f"({value['covered']}/{value['total']}); no exclusions", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
