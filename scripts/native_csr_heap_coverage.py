#!/usr/bin/env python3
"""Compile portable API-double controls and gate actual CSR helper coverage."""
from __future__ import annotations

import gzip
import json
import os
import copy
from pathlib import Path
import subprocess
import tempfile

MinimumPercent = 90
ExpectedFunctions = {
    "native_csr_digest", "native_csr_module", "native_csr_capture",
    "native_csr_emit", "native_csr_stage",
}


def coverage_summary(report: dict, expected_source: Path) -> dict:
    """in: one gcov JSON document and exact source. out: strict line/branch summary.

    Every non-generated source line and every directional branch is counted;
    no exclusions or threshold substitutions. Pure; raises ValueError on drift.
    """
    files = report.get("files") if isinstance(report, dict) else None
    if not isinstance(files, list):
        raise ValueError("missing gcov files")
    expected = str(expected_source.resolve())
    matches = [item for item in files if isinstance(item, dict) and item.get("file") == expected]
    if len(matches) != 1:
        raise ValueError("missing or duplicate actual helper source")
    item = matches[0]
    functions = item.get("functions")
    if not isinstance(functions, list):
        raise ValueError("missing helper function inventory")
    names = [function.get("name") for function in functions if isinstance(function, dict)]
    if set(names) != ExpectedFunctions or len(names) != len(ExpectedFunctions):
        raise ValueError("unexpected, missing or duplicate helper function")
    if any(function.get("execution_count", 0) <= 0 for function in functions):
        raise ValueError("unexecuted helper function")
    lines = item.get("lines")
    if not isinstance(lines, list) or not lines:
        raise ValueError("missing source lines")
    line_numbers, line_counts, branch_counts = set(), [], []
    for line in lines:
        if not isinstance(line, dict):
            raise ValueError("malformed line record")
        number, count, branches = line.get("line_number"), line.get("count"), line.get("branches")
        if type(number) is not int or number <= 0 or number in line_numbers:
            raise ValueError("invalid or duplicate line number")
        if type(count) is not int or count < 0 or not isinstance(branches, list):
            raise ValueError("invalid line counter or branch list")
        line_numbers.add(number)
        line_counts.append(count)
        for branch in branches:
            branch_count = branch.get("count") if isinstance(branch, dict) else None
            if type(branch_count) is not int or branch_count < 0:
                raise ValueError("invalid branch counter")
            branch_counts.append(branch_count)
    if not branch_counts:
        raise ValueError("zero branch denominator")
    summary = {}
    for label, counts in (("lines", line_counts), ("branches", branch_counts)):
        covered, total = sum(count > 0 for count in counts), len(counts)
        if covered * 100 < total * MinimumPercent:
            raise ValueError(f"{label} below {MinimumPercent}%: {covered}/{total}")
        summary[label] = {"covered": covered, "total": total, "percent": 100 * covered / total}
    return summary


def coverage_controls(expected_source: Path) -> None:
    """Exercise malformed inventory and both below-floor cases before use."""
    functions = [{"name": name, "execution_count": 1} for name in sorted(ExpectedFunctions)]
    good_file = {"file": str(expected_source.resolve()), "functions": functions,
        "lines": [{"line_number": index + 1, "count": 1,
            "branches": [{"count": 1}, {"count": 1}]} for index in range(10)]}
    good = {"files": [good_file]}
    coverage_summary(good, expected_source)
    bad_records = []
    bad = copy.deepcopy(good); bad["files"] = []; bad_records.append(bad)
    bad = copy.deepcopy(good); bad["files"].append(copy.deepcopy(good_file)); bad_records.append(bad)
    bad = copy.deepcopy(good); bad["files"][0]["lines"] = []; bad_records.append(bad)
    bad = copy.deepcopy(good); bad["files"][0]["functions"].append(copy.deepcopy(functions[0])); bad_records.append(bad)
    bad = copy.deepcopy(good); bad["files"][0]["lines"][0]["count"] = 0; bad["files"][0]["lines"][1]["count"] = 0; bad_records.append(bad)
    bad = copy.deepcopy(good)
    for line in bad["files"][0]["lines"][:2]: line["branches"] = [{"count": 0}, {"count": 0}]
    bad_records.append(bad)
    for candidate in bad_records:
        try:
            coverage_summary(candidate, expected_source)
        except ValueError:
            continue
        raise ValueError("coverage mutation unexpectedly accepted")


def main() -> int:
    """Build/run controls in a unique temporary directory and gate helper coverage."""
    root = Path(__file__).resolve().parents[1]
    source = root / "tests/vgpu/native_csr_heap_windows.inc"
    test = root / "tests/vgpu/native_csr_heap_test.c"
    coverage_controls(source)
    with tempfile.TemporaryDirectory(prefix="waddle_native_csr_cov_") as temporary:
        output = Path(temporary)
        binary = output / "native_csr_heap_test"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Wpedantic",
            "-Werror", "-O0", "-g", "--coverage", "-fprofile-abs-path", "-I", str(root / "tests/vgpu"),
            str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        gcno = next(output.glob("*.gcno"))
        subprocess.run([os.environ.get("GCOV", "gcov"), "--json-format", "-b", "-c", str(gcno)],
            cwd=output, check=True)
        reports = list(output.glob("*.gcov.json.gz"))
        if len(reports) != 1:
            raise ValueError("expected exactly one gcov report")
        report = json.loads(gzip.decompress(reports[0].read_bytes()))
        summary = coverage_summary(report, source)
        print(json.dumps({"source": str(source), "minimum_percent": MinimumPercent,
            "coverage": summary, "no_exclusions": True}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
