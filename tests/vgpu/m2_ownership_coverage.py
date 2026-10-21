#!/usr/bin/env python3
"""Gate both actual M2 ledger/observer sources at 90%; preserve unfiltered gcov."""
from datetime import datetime, timezone
import gzip
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

# Each separately compiled source must independently meet both fixed floors.
CoverageSources = ("tests/vgpu/m2_owner_ledger.c", "tests/vgpu/m2_ownership_probe.c")
CoverageMinimum = 90


def coverage_summary(reports, root):
    """in: borrowed gcov reports/root. out: owned per-source summary, no mutation.

    Raise ValueError for absent/duplicate/unexpected files, invalid counters or
    below-floor evidence. All recorded lines/outcomes count, without exclusions.
    Pure/thread-safe; no files, processes, shared state or retained input owners.
    """
    if not isinstance(reports, list) or len(reports) != len(CoverageSources):
        raise ValueError("missing or extra source report")
    expected = {str(root / name): name for name in CoverageSources}
    files = {}
    for report in reports:
        if not isinstance(report, dict) or not isinstance(report.get("files"), list) or len(report["files"]) != 1:
            raise ValueError("each compiled source must have exactly one gcov record")
        item = report["files"][0]
        if not isinstance(item, dict) or not isinstance(item.get("file"), str):
            raise ValueError("invalid source record")
        name = item["file"]
        if name not in expected or name in files:
            raise ValueError("unexpected or duplicate source")
        files[name] = item
    if set(files) != set(expected):
        raise ValueError("missing source")
    summary = {}
    for path, name in expected.items():
        lines = files[path].get("lines")
        if not isinstance(lines, list) or not lines:
            raise ValueError("missing source lines")
        numbers, line_counts, branch_counts = set(), [], []
        for line in lines:
            if not isinstance(line, dict):
                raise ValueError("invalid line record")
            number, count, branches = line.get("line_number"), line.get("count"), line.get("branches")
            if type(number) is not int or number <= 0 or number in numbers:
                raise ValueError("invalid or duplicate line number")
            if type(count) is not int or count < 0 or not isinstance(branches, list):
                raise ValueError("invalid line count/branches")
            numbers.add(number)
            line_counts.append(count)
            for branch in branches:
                count = branch.get("count") if isinstance(branch, dict) else None
                if type(count) is not int or count < 0:
                    raise ValueError("invalid branch count")
                branch_counts.append(count)
        if not branch_counts:
            raise ValueError("zero branch denominator")
        summary[name] = {}
        for kind, counts in (("lines", line_counts), ("branches", branch_counts)):
            covered, total = sum(count > 0 for count in counts), len(counts)
            if 100 * covered < CoverageMinimum * total:
                raise ValueError(f"{name} {kind} below {CoverageMinimum}%: {covered}/{total}")
            summary[name][kind] = {"covered": covered, "total": total, "percent": 100 * covered / total}
    return summary


def main():
    """Compile/run portable controls in a unique retained directory; return zero.

    Own local subprocesses, files and parsed data; tool/schema/floor errors fail
    nonzero. Instrument actual ledger and observer, including standalone control
    code, without exclusions. This is API-double coverage, not Windows execution.
    """
    root = Path(__file__).resolve().parents[2]
    output = root / "build/coverage/vgpu/m2_ownership" / (
        "run-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ") + "-" + str(os.getpid()))
    output.mkdir(parents=True, exist_ok=False)
    print("preserved M2 ownership coverage: " + str(output), flush=True)
    flags = ["-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-O0", "-g",
             "-Iinclude", "-Itests/vgpu", "-Itests/vgpu/win32_fake", "-D_WIN32",
             "-DWaddleM2OwnershipProbe", "-DWaddleM2OwnershipFaults"]
    objects = []
    for name in CoverageSources:
        obj = output / (Path(name).stem + ".o")
        subprocess.run(["cc", *flags, "--coverage", "-fprofile-abs-path", "-c", name, "-o", str(obj)], cwd=root, check=True)
        objects.append(str(obj))
    subprocess.run(["cc", *flags, "tests/vgpu/m2_owner_ledger_test.c", objects[0], "--coverage", "-o", str(output / "ledger")], cwd=root, check=True)
    subprocess.run(["cc", *flags, "tests/vgpu/m2_ownership_portable.c", "src/vgpu/venus_win32_present.c", *objects, "--coverage", "-o", str(output / "observer")], cwd=root, check=True)
    for runner in ("ledger", "observer"):
        subprocess.run([str(output / runner)], cwd=root, check=True)
    reports, hashes = [], {}
    for name in CoverageSources:
        stem = Path(name).stem
        subprocess.run(["gcov", "--json-format", "-b", "-c", str(output / (stem + ".gcno"))], cwd=output, check=True)
        raw = (output / (stem + ".gcov.json.gz")).read_bytes()
        reports.append(json.loads(gzip.decompress(raw)))
        hashes[stem + ".gcov.json.gz"] = hashlib.sha256(raw).hexdigest()
    summary = coverage_summary(reports, root)
    for name, values in summary.items():
        for kind, value in values.items():
            print(f"{name} {kind}: {value['covered']}/{value['total']} ({value['percent']:.2f}%); no exclusions", flush=True)
    evidence = {"minimum_percent": CoverageMinimum, "coverage": summary, "gcov_sha256": hashes,
                "source_sha256": {name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in CoverageSources}}
    (output / "summary.json").write_text(json.dumps(evidence, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
