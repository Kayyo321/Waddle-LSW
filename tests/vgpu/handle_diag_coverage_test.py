#!/usr/bin/env python3
"""Fail-closed controls for handle diagnostic coverage evidence."""
import copy
from pathlib import Path
import unittest
from handle_diag_coverage import coverage_summary

# Stable synthetic absolute root; no files at this path are read or created.
Root = Path("/synthetic/waddle")
Files = ("tests/vgpu/tcp_gpu_handle_diag_host.c", "tests/vgpu/tcp_gpu_handle_diag_windows.inc",
         "tests/vgpu/win32_fake/handle_diag_fake.h")


def report_fixture():
    """out: owned synthetic 100% gcov-shaped report, without I/O or shared state."""
    lines = [{"line_number": index + 1, "count": 1,
              "branches": [{"count": 1}, {"count": 1}]} for index in range(10)]
    return {"files": [{"file": str(Root / name), "lines": copy.deepcopy(lines)} for name in Files]}


class handle_diag_coverage_test_t(unittest.TestCase):
    """Independent malformed/boundary controls; each test owns its report."""

    def test_exact_floor_and_complete_source(self):
        """The real floor is inclusive and all line/outcome entries contribute."""
        report = report_fixture()
        lines = report["files"][1]["lines"]
        lines[0]["count"] = 0
        lines[0]["branches"] = [{"count": 0}, {"count": 0}]
        summary = coverage_summary(report, Root)
        self.assertEqual(summary["lines"], {"covered": 9, "total": 10, "percent": 90.0})
        self.assertEqual(summary["branches"], {"covered": 18, "total": 20, "percent": 90.0})
        self.assertEqual(coverage_summary(report_fixture(), Root)["lines"]["percent"], 100.0)

    def test_each_floor_fails_independently(self):
        """Neither high line nor high branch coverage masks the other's failure."""
        for kind in ("lines", "branches"):
            report = report_fixture()
            lines = report["files"][1]["lines"]
            for line in lines[:2]:
                if kind == "lines":
                    line["count"] = 0
                else:
                    line["branches"] = [{"count": 0}, {"count": 0}]
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                coverage_summary(report, Root)

    def test_source_identity_is_exact(self):
        """Missing, duplicate and unexpected reports cannot select another source."""
        for index in range(3):
            report = report_fixture()
            del report["files"][index]
            with self.assertRaises(ValueError):
                coverage_summary(report, Root)
            report = report_fixture()
            report["files"].append(copy.deepcopy(report["files"][index]))
            with self.assertRaises(ValueError):
                coverage_summary(report, Root)
        report = report_fixture()
        report["files"][1]["file"] = "/other/tcp_gpu_handle_diag_windows.inc"
        with self.assertRaises(ValueError):
            coverage_summary(report, Root)

    def test_empty_or_missing_evidence(self):
        """Zero denominators and absent fields fail rather than report 100%."""
        for report in (None, {}, {"files": []}, {"files": None}, {"files": [None]}):
            with self.subTest(report=report), self.assertRaises(ValueError):
                coverage_summary(report, Root)
        for value in (None, [], {}):
            report = report_fixture()
            report["files"][1]["lines"] = value
            with self.assertRaises(ValueError):
                coverage_summary(report, Root)
        report = report_fixture()
        for line in report["files"][1]["lines"]:
            line["branches"] = []
        with self.assertRaises(ValueError):
            coverage_summary(report, Root)

    def test_malformed_counts_and_duplicate_lines(self):
        """Invalid counters cannot silently count as covered outcomes."""
        for key in ("count", "line_number", "branches"):
            for value in (-1, None, True, "1", {}):
                report = report_fixture()
                report["files"][1]["lines"][0][key] = value
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    coverage_summary(report, Root)
        for value in (-1, None, True, "1", {}):
            report = report_fixture()
            report["files"][1]["lines"][0]["branches"][0]["count"] = value
            with self.subTest(branch=value), self.assertRaises(ValueError):
                coverage_summary(report, Root)
        report = report_fixture()
        report["files"][1]["lines"].append(copy.deepcopy(report["files"][1]["lines"][0]))
        with self.assertRaises(ValueError):
            coverage_summary(report, Root)


if __name__ == "__main__":
    unittest.main()
