#!/usr/bin/env python3
"""Fail-closed evidence controls for each real M2 ownership source."""
import copy
from pathlib import Path
import unittest
from m2_ownership_coverage import coverage_summary

# Synthetic report root; no file access or global mutations.
Root = Path("/synthetic/waddle")
Sources = ("tests/vgpu/m2_owner_ledger.c", "tests/vgpu/m2_ownership_probe.c")


def reports_fixture():
    """Return new independent gcov-shaped data with ten fully covered lines."""
    return [{"files": [{"file": str(Root / name), "lines": [
        {"line_number": i + 1, "count": 1, "branches": [{"count": 1}, {"count": 1}]}
        for i in range(10)]}]} for name in Sources]


class m2_ownership_coverage_test_t(unittest.TestCase):
    """Each test owns synthetic data; no native or filesystem operations."""

    def test_independent_floors(self):
        """Every source independently needs >=90% lines and branch outcomes."""
        for source in range(2):
            reports = reports_fixture()
            lines = reports[source]["files"][0]["lines"]
            lines[0].update(count=0, branches=[{"count": 0}, {"count": 0}])
            self.assertEqual(coverage_summary(reports, Root)[Sources[source]]["lines"]["percent"], 90.0)
            for kind in ("line", "branch"):
                bad = copy.deepcopy(reports)
                if kind == "line":
                    bad[source]["files"][0]["lines"][1]["count"] = 0
                else:
                    bad[source]["files"][0]["lines"][1]["branches"] = [{"count": 0}, {"count": 0}]
                with self.assertRaises(ValueError):
                    coverage_summary(bad, Root)
        self.assertEqual(len(coverage_summary(reports_fixture(), Root)), 2)

    def test_exact_sources(self):
        """Missing, duplicated, renamed and extra sources cannot substitute."""
        for reports in (None, [], reports_fixture()[:1], reports_fixture() * 2,
                        [reports_fixture()[0], reports_fixture()[0]], [None, {}]):
            with self.assertRaises(ValueError):
                coverage_summary(reports, Root)
        for source in range(2):
            for change in ("empty", "duplicate", "unexpected", "malformed", "missing_files"):
                reports = reports_fixture()
                if change == "empty": reports[source]["files"] = []
                if change == "duplicate": reports[source]["files"] *= 2
                if change == "unexpected": reports[source]["files"][0]["file"] = "/other/source.c"
                if change == "malformed": reports[source]["files"][0] = None
                if change == "missing_files": reports[source] = {}
                with self.assertRaises(ValueError): coverage_summary(reports, Root)

    def test_empty_and_malformed_lines(self):
        """Empty/duplicate/invalid lines cannot produce a passing denominator."""
        for source in range(2):
            for lines in (None, [], [None], [{"line_number": 1}],
                          [{"line_number": 1, "count": 1, "branches": []}]):
                reports = reports_fixture()
                reports[source]["files"][0]["lines"] = lines
                with self.assertRaises(ValueError): coverage_summary(reports, Root)
            for number in (0, -1, True, "1", 2):
                reports = reports_fixture()
                reports[source]["files"][0]["lines"][0]["line_number"] = number
                with self.assertRaises(ValueError): coverage_summary(reports, Root)

    def test_invalid_counts(self):
        """Boolean, negative and non-integer evidence is never a count."""
        for source in range(2):
            for count in (None, True, -1, "1", 0.5):
                for field in ("line", "branch"):
                    reports = reports_fixture()
                    line = reports[source]["files"][0]["lines"][0]
                    if field == "line": line["count"] = count
                    else: line["branches"][0]["count"] = count
                    with self.assertRaises(ValueError): coverage_summary(reports, Root)
            for branches in (None, "bad", [None], [{}]):
                reports = reports_fixture()
                reports[source]["files"][0]["lines"][0]["branches"] = branches
                with self.assertRaises(ValueError): coverage_summary(reports, Root)


if __name__ == "__main__":
    unittest.main()
