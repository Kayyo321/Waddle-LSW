#!/usr/bin/env python3
"""Fail-closed controls for actual runtime-shim coverage evidence."""
import copy
import importlib.util
from pathlib import Path
import unittest

Root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("runtime_coverage", Root / "scripts/clvk_runtime_tls_coverage.py")
coverage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(coverage)
Source = Root / "src/vgpu/clvk_runtime_tls.c"


def report_fixture():
    """out: new synthetic evidence, never a native or real coverage result."""
    return {"files": [{"file": str(Source), "functions": [
        {"name": name, "execution_count": 1} for name in sorted(coverage.ProductionFunctions)],
        "lines": [{"line_number": i + 1, "count": 1, "branches": [{"count": 1}]} for i in range(10)]}]}


class coverage_test_t(unittest.TestCase):
    """Each test owns synthetic records; no shared mutable state or native jobs."""

    def test_both_floors_independently(self):
        for kind in ("line", "branch"):
            for missed, passed in ((1, True), (2, False)):
                report = report_fixture()
                for line in report["files"][0]["lines"][:missed]:
                    if kind == "line":
                        line["count"] = 0
                    else:
                        line["branches"][0]["count"] = 0
                if passed:
                    coverage.coverage_summary(report, Source)
                else:
                    with self.assertRaises(ValueError):
                        coverage.coverage_summary(report, Source)

    def test_required_function_presence_and_execution(self):
        good = report_fixture()
        for index in range(len(good["files"][0]["functions"])):
            for mutation in ("missing", "unexecuted", "duplicate"):
                report = copy.deepcopy(good)
                functions = report["files"][0]["functions"]
                if mutation == "missing":
                    functions.pop(index)
                elif mutation == "unexecuted":
                    functions[index]["execution_count"] = 0
                else:
                    functions.append(copy.deepcopy(functions[index]))
                with self.assertRaises(ValueError):
                    coverage.coverage_summary(report, Source)

    def test_source_and_zero_denominators(self):
        for report in (None, {}, {"files": []}, {"files": None}):
            with self.assertRaises(ValueError):
                coverage.coverage_summary(report, Source)
        for mutation in ("duplicate", "foreign", "empty_lines", "empty_branches"):
            report = report_fixture()
            if mutation == "duplicate":
                report["files"].append(copy.deepcopy(report["files"][0]))
            elif mutation == "foreign":
                report["files"][0]["file"] += ".fake"
            elif mutation == "empty_lines":
                report["files"][0]["lines"] = []
            else:
                for line in report["files"][0]["lines"]:
                    line["branches"] = []
            with self.assertRaises(ValueError):
                coverage.coverage_summary(report, Source)

    def test_malformed_counts_and_duplicate_lines(self):
        for value in (True, -1, None, "1"):
            for target in ("line", "branch", "function"):
                report = report_fixture()
                record = report["files"][0]
                if target == "line":
                    record["lines"][0]["count"] = value
                elif target == "branch":
                    record["lines"][0]["branches"][0]["count"] = value
                else:
                    record["functions"][0]["execution_count"] = value
                with self.assertRaises(ValueError):
                    coverage.coverage_summary(report, Source)
        report = report_fixture()
        report["files"][0]["lines"].append(copy.deepcopy(report["files"][0]["lines"][0]))
        with self.assertRaises(ValueError):
            coverage.coverage_summary(report, Source)


if __name__ == "__main__":
    unittest.main()
