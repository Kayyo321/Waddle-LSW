#!/usr/bin/env python3
"""Portable fake-child controls for the real Windows collector; no Windows run."""
import copy
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

Spec = importlib.util.spec_from_file_location("m2_native_controls", Path(__file__).with_name("m2_native_controls.py"))
collector = importlib.util.module_from_spec(Spec)
Spec.loader.exec_module(collector)


def fixture():
    """Fresh independent synthetic expected status, never a real native receipt."""
    lines = []
    names = ("positive", "owned-leak-equal-total", "injected-release-failure", "unrelated-release-masks-total")
    for mode, name in enumerate(names):
        lines.append(f"M2_OWNERSHIP_NATIVE_INFO case={name} count_before=158 count_after=158 live={int(mode>0)} first_id={int(mode>0)} failures={8 if mode==2 else 0} rejected={int(mode>0)}")
        lines.append(f"M2_OWNERSHIP_NATIVE case={name} PASS")
    lines.append("M2_OWNERSHIP_NATIVE 4 cases PASS")
    return {"exit": "0", "timed_out": False, "stdout": "\n".join(lines) + "\n", "stderr": ""}


class native_collector_test_t(unittest.TestCase):
    """Pure shape tests plus owned bounded fake-child execution controls."""

    def test_exact_shape_and_each_control(self):
        """Every missing, duplicate, reordered or altered real-control line fails."""
        good = fixture(); self.assertTrue(collector.verify(good))
        self.assertTrue(collector.verify(dict(good, stderr="informational native diagnostic\n")))
        lines = good["stdout"].splitlines()
        for index in range(len(lines)):
            bad = copy.deepcopy(lines); bad[index] += " trailing"
            for changed in (lines[:index]+lines[index+1:], lines[:index]+[lines[index]]+lines[index:], bad):
                with self.assertRaises(ValueError): collector.verify(dict(good, stdout="\n".join(changed)))
        for before, after in (("live=1", "live=0"), ("live=1", "live=01"), ("first_id=1", "first_id=0"), ("rejected=1", "rejected=0"),
                              ("failures=8", "failures=0"), ("case=owned-leak-equal-total count_before=158", "case=owned-leak-equal-total count_before=159")):
            with self.assertRaises(ValueError): collector.verify(dict(good, stdout=good["stdout"].replace(before, after, 1)))
        for key, value in (("exit", 0), ("exit", "1"), ("timed_out", True), ("stderr", "FAIL"), ("stdout", "\0"),
                           ("stderr", lines[0]), ("stdout", None)):
            with self.assertRaises(ValueError): collector.verify(dict(good, **{key: value}))

    def test_real_exit_and_both_streams(self):
        """A stub's real failed exit cannot be overridden by all expected PASS lines."""
        good = fixture()
        with tempfile.TemporaryDirectory(prefix="m2-collector-") as temporary:
            root = Path(temporary)
            for exit_code in (0, 7):
                output = root / str(exit_code)
                code = f"import sys; sys.stdout.write({good['stdout']!r}); sys.stderr.write('informational diagnostic\\n'); sys.exit({exit_code})"
                self.assertEqual(collector.collect([sys.executable, "-c", code], output), int(exit_code != 0))
                status = json.loads((output / "guest_status.json").read_text())
                self.assertEqual(status["exit"], str(exit_code)); self.assertEqual(status["stderr"], "informational diagnostic\n")
                preserved = (output / "guest_status.json").read_bytes()
                with self.assertRaises(FileExistsError): collector.collect([sys.executable, "-c", "raise SystemExit(0)"], output)
                self.assertEqual((output / "guest_status.json").read_bytes(), preserved)

    def test_timeout_partial_output_and_launch_failure(self):
        """Bounded child timeout preserves partial streams and cannot pass."""
        with tempfile.TemporaryDirectory(prefix="m2-collector-") as temporary:
            root = Path(temporary)
            code = "import sys,time; print('partial stdout',flush=True); print('partial stderr',file=sys.stderr,flush=True); time.sleep(10)"
            self.assertEqual(collector.collect([sys.executable, "-c", code], root / "timeout", timeout=0.3), 1)
            status = json.loads((root / "timeout/guest_status.json").read_text())
            self.assertTrue(status["timed_out"]); self.assertEqual(status["exit"], "timeout")
            self.assertIn("partial stdout", status["stdout"]); self.assertIn("partial stderr", status["stderr"])
            self.assertEqual(collector.collect([str(root / "missing")], root / "launch"), 1)
            self.assertEqual(json.loads((root / "launch/guest_status.json").read_text())["exit"], "launch_error")


if __name__ == "__main__":
    unittest.main()
