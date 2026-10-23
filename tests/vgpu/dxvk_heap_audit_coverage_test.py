#!/usr/bin/env python3
"""Adversarial synthetic controls for strict audit-stage coverage evidence."""
import copy
import importlib.util
from pathlib import Path
import unittest

Root = Path(__file__).resolve().parents[2]
ModulePath = Root / 'scripts/dxvk_heap_audit_coverage.py'
Spec = importlib.util.spec_from_file_location('dxvk_heap_audit_coverage', ModulePath)
Coverage = importlib.util.module_from_spec(Spec)
Spec.loader.exec_module(Coverage)
Source = Root / 'tests/vgpu/dxvk_heap_audit_windows.inc'


def report_fixture():
    """Return synthetic evidence only; no native or actual coverage claim."""
    return {'files': [{'file': str(Source),
                       'functions': [{'name': name, 'execution_count': 1}
                                     for name in sorted(Coverage.ProductionFunctions)],
                       'lines': [{'line_number': index + 1, 'count': 1,
                                  'branches': [{'count': 1}, {'count': 1}]}
                                 for index in range(10)]}]}


class coverage_test_t(unittest.TestCase):
    """Own fresh synthetic records; never mutate source or saved evidence."""

    def test_exact_inventory_and_both_90_percent_floors(self):
        report = report_fixture()
        summary = Coverage.coverage_summary(report, Source)
        self.assertEqual(summary['functions_executed'], 2)
        self.assertEqual(summary['lines']['percent'], 100)
        self.assertEqual(summary['branches']['percent'], 100)
        for kind, misses in (('lines', 1), ('branches', 2)):
            candidate = report_fixture()
            if kind == 'lines':
                for line in candidate['files'][0]['lines'][:misses]:
                    line['count'] = 0
            else:
                branches = [branch for line in candidate['files'][0]['lines']
                            for branch in line['branches']]
                for branch in branches[:misses]:
                    branch['count'] = 0
            self.assertEqual(Coverage.coverage_summary(candidate, Source)[kind]['percent'], 90)

    def test_line_and_branch_floor_failures(self):
        for kind in ('lines', 'branches'):
            candidate = report_fixture()
            if kind == 'lines':
                for line in candidate['files'][0]['lines'][:2]:
                    line['count'] = 0
            else:
                branches = [branch for line in candidate['files'][0]['lines']
                            for branch in line['branches']]
                for branch in branches[:3]:
                    branch['count'] = 0
            with self.assertRaisesRegex(ValueError, 'below 90%'):
                Coverage.coverage_summary(candidate, Source)

    def test_function_inventory_mutations_fail_closed(self):
        good = report_fixture()
        for index in range(len(good['files'][0]['functions'])):
            for mutation in ('missing', 'unexecuted', 'duplicate', 'foreign'):
                candidate = copy.deepcopy(good)
                functions = candidate['files'][0]['functions']
                if mutation == 'missing':
                    functions.pop(index)
                elif mutation == 'unexecuted':
                    functions[index]['execution_count'] = 0
                elif mutation == 'duplicate':
                    functions.append(copy.deepcopy(functions[index]))
                else:
                    functions[index]['name'] = 'unreviewed_function'
                with self.subTest(mutation=mutation, index=index):
                    with self.assertRaises(ValueError):
                        Coverage.coverage_summary(candidate, Source)

    def test_source_line_and_branch_schema_mutations_fail_closed(self):
        for mutation in ('missing_source', 'duplicate_source', 'empty_lines', 'empty_branches',
                         'duplicate_line', 'bad_line_count', 'bad_branch_count'):
            candidate = report_fixture()
            record = candidate['files'][0]
            if mutation == 'missing_source':
                candidate['files'] = []
            elif mutation == 'duplicate_source':
                candidate['files'].append(copy.deepcopy(record))
            elif mutation == 'empty_lines':
                record['lines'] = []
            elif mutation == 'empty_branches':
                for line in record['lines']:
                    line['branches'] = []
            elif mutation == 'duplicate_line':
                record['lines'].append(copy.deepcopy(record['lines'][0]))
            elif mutation == 'bad_line_count':
                record['lines'][0]['count'] = True
            else:
                record['lines'][0]['branches'][0]['count'] = -1
            with self.subTest(mutation=mutation):
                with self.assertRaises(ValueError):
                    Coverage.coverage_summary(candidate, Source)


if __name__ == '__main__':
    unittest.main()
