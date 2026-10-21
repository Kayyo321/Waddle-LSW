#!/usr/bin/env python3
"""Malformed-evidence regressions for the final M2 guest-output gate."""
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

# The tracked verifier is imported without executing its CLI or any native job.
Root = Path(__file__).resolve().parents[2]
VerifierPath = Root / "scripts/vgpu_m2_output.py"
spec = importlib.util.spec_from_file_location("vgpu_m2_output", VerifierPath)
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)
Device = "NVIDIA GeForce RTX 5080"
# Independent expected labels: changing the verifier contract cannot change tests.
Labels = (
    "failed-replacement-retained-acquired-image",
    "zero-area-sticky-out-of-date-replacement-and-restore",
    "resize-capabilities-out-of-date-and-replacements",
    "destroyed-HWND-surface-loss-and-retirement-precedence",
)
Final = "Actual native GPU outputs and matching retirement Ack PASS; independent host result still required"


def fixture(workload):
    """in: supported workload. out: fresh synthetic collected-status dictionary.

    Owns all output; no I/O. Pure and thread-safe. Never represents a native run.
    """
    lines = [f"Authenticated native GPU session=1234567890abcdef workload={workload} public_api=1.0",
             "Independent Windows initialization diagnostic",
             "Native repeated application handle baseline=199"]
    for lifetime in range(1, 9):
        if workload == "triangle_present":
            lines.extend(f"Native Vulkan actual swapchain mode={mode} frame={frame} exact4096RGB/BGRA PASS"
                         for mode in ("FIFO", "IMMEDIATE") for frame in range(1, 4))
        else:
            lines.extend(f"Native WSI lifetime case={label} PASS" for label in Labels)
        lines.append(f"Native actual {workload} lifetime={lifetime} device={Device} exact-output PASS")
        lines.append(f"handle_diag: lifetime={lifetime} iteration_index={lifetime-1} samples=1 count=199 start_ms=0 check_start_ms=0 check_done_ms=0")
    lines.append(Final)
    return {"exit": "0", "stdout": "\r\n".join(lines) + "\r\n",
            "stderr": "WARNING | LAYER: Registry lookup failed to get layer manifest files.\r\n"}


class m2_output_test_t(unittest.TestCase):
    """Owns temporary fixtures per test; isolated, no VM/GPU or shared artifacts."""

    def test_valid_workloads_and_line_endings(self):
        """Both exact workloads accept CRLF, LF and CR without weakening events."""
        for workload in ("triangle_present", "wsi_lifetime"):
            for separator in ("\r\n", "\n", "\r"):
                with self.subTest(workload=workload, separator=repr(separator)):
                    status = fixture(workload)
                    status["stdout"] = status["stdout"].replace("\r\n", separator)
                    result = verifier.verify_guest_status(status, workload, Device)
                    self.assertEqual(result["ordered_lifetimes"], list(range(1, 9)))
                    self.assertEqual(result["frames"], 48 if workload == "triangle_present" else 0)
                    self.assertEqual(result["wsi_cases"], 32 if workload == "wsi_lifetime" else 0)

    def test_lifetime_numbers_and_identity(self):
        """Count-preserving duplicates, reordering, numbering and identity drift fail."""
        for workload in ("triangle_present", "wsi_lifetime"):
            good = fixture(workload)
            changes = (("lifetime=2 device=", "lifetime=1 device="),
                       ("lifetime=2 device=", "lifetime=9 device="),
                       ("lifetime=2 device=", "lifetime=0 device="),
                       ("lifetime=2 device=", "lifetime=02 device="),
                       ("lifetime=2 device=", "lifetime=-2 device="),
                       (f"device={Device}", "device=another GPU"),
                       (f"Native actual {workload} lifetime=2", "Native actual triangle_present_smoke lifetime=2"))
            for old, new in changes:
                with self.subTest(workload=workload, replacement=new):
                    bad = dict(good, stdout=good["stdout"].replace(old, new, 1))
                    self.assertEqual(bad["stdout"].count("exact-output PASS"), 8)
                    with self.assertRaises(ValueError):
                        verifier.verify_guest_status(bad, workload, Device)
            bad = dict(good, stdout=good["stdout"].replace("lifetime=2 device=", "lifetime=X device=")
                       .replace("lifetime=3 device=", "lifetime=2 device=").replace("lifetime=X device=", "lifetime=3 device="))
            with self.assertRaises(ValueError):
                verifier.verify_guest_status(bad, workload, Device)

    def test_every_required_event_deletion_duplication_and_swap(self):
        """Every required event is tested at its real position, not just totals."""
        for workload in ("triangle_present", "wsi_lifetime"):
            good = fixture(workload)
            lines = good["stdout"].splitlines()
            indices = [i for i, line in enumerate(lines) if line.startswith(
                ("Authenticated", "Native repeated", "Native actual", "Native Vulkan", "Native WSI", "Actual native"))]
            for position, index in enumerate(indices):
                mutations = {"missing": lines[:index] + lines[index+1:],
                             "duplicate": lines[:index] + [lines[index]] + lines[index:]}
                if position + 1 < len(indices):
                    swapped = lines.copy()
                    other = indices[position + 1]
                    swapped[index], swapped[other] = swapped[other], swapped[index]
                    mutations["reordered"] = swapped
                for mutation, changed in mutations.items():
                    with self.subTest(workload=workload, index=index, mutation=mutation):
                        with self.assertRaises(ValueError):
                            verifier.verify_guest_status(dict(good, stdout="\n".join(changed)), workload, Device)

    def test_malformed_frame_case_and_wrapped_markers(self):
        """Misnumbered frames, changed case labels and embedded PASS cannot hide."""
        changes = {
            "triangle_present": (("frame=2 ", "frame=1 "), ("mode=FIFO", "mode=IMMEDIATE"),
                                 ("frame=2 ", "frame=4 "), ("frame=2 ", "frame=02 ")),
            "wsi_lifetime": ((Labels[1], Labels[0]), (Labels[1], "unknown-case")),
        }
        for workload, replacements in changes.items():
            good = fixture(workload)
            for old, new in replacements + (("Native actual ", "prefix Native actual "),
                                            ("exact-output PASS", "exact-output PASS trailing")):
                with self.subTest(workload=workload, replacement=new):
                    with self.assertRaises(ValueError):
                        verifier.verify_guest_status(dict(good, stdout=good["stdout"].replace(old, new, 1)), workload, Device)
            foreign = ("Native WSI lifetime case=" + Labels[0] + " PASS" if workload == "triangle_present"
                       else "Native Vulkan actual swapchain mode=FIFO frame=1 exact4096RGB/BGRA PASS")
            with self.assertRaises(ValueError):
                verifier.verify_guest_status(dict(good, stdout=foreign + "\n" + good["stdout"]), workload, Device)

    def test_failures_in_either_stream_even_when_counts_pass(self):
        """Each known fixture failure contradicts PASS, even inside a loader line."""
        for workload in ("triangle_present", "wsi_lifetime"):
            for marker in verifier.FailureMarkers:
                for stream in ("stdout", "stderr"):
                    with self.subTest(workload=workload, marker=marker, stream=stream):
                        bad = fixture(workload)
                        bad[stream] = "loader prefix " + marker + " detail\n" + bad[stream]
                        with self.assertRaises(ValueError):
                            verifier.verify_guest_status(bad, workload, Device)

    def test_exit_missing_fields_and_wrong_schema(self):
        """No missing/failed exit, implicit output coercion or smoke workload passes."""
        good = fixture("triangle_present")
        for value in (None, 0, False, "1", "-1", "", "00"):
            with self.subTest(exit=value):
                with self.assertRaises(ValueError):
                    verifier.verify_guest_status(dict(good, exit=value), "triangle_present", Device)
        for key in ("exit", "stdout", "stderr"):
            for mutation in ("missing", "null", "array"):
                bad = copy.deepcopy(good)
                if mutation == "missing":
                    del bad[key]
                else:
                    bad[key] = None if mutation == "null" else []
                with self.subTest(key=key, mutation=mutation):
                    with self.assertRaises(ValueError):
                        verifier.verify_guest_status(bad, "triangle_present", Device)
        for status in (None, [], "0"):
            with self.assertRaises(ValueError):
                verifier.verify_guest_status(status, "triangle_present", Device)
        with self.assertRaises(ValueError):
            verifier.verify_guest_status(good, "triangle_present_smoke", Device)

    def test_header_final_and_output_channel(self):
        """Headers and end markers bind the whole run and cannot be misplaced."""
        good = fixture("wsi_lifetime")
        for old, new in (("1234567890abcdef", "0000000000000000"),
                         ("workload=wsi_lifetime", "workload=triangle_present"),
                         ("public_api=1.0", "public_api=1.3"),
                         ("baseline=199", "baseline=0")):
            with self.subTest(new=new):
                with self.assertRaises(ValueError):
                    verifier.verify_guest_status(dict(good, stdout=good["stdout"].replace(old, new)), "wsi_lifetime", Device)
        for stream, extra in (("stdout", "trailing text\n"), ("stderr", Final), ("stdout", "\0")):
            bad = dict(good)
            bad[stream] += extra
            with self.assertRaises(ValueError):
                verifier.verify_guest_status(bad, "wsi_lifetime", Device)

    def test_diagnostics_are_passive(self):
        """Unavailable NT snapshots and clocks never change the native count gate."""
        good = fixture("wsi_lifetime")
        good["stdout"] = "handle_diag: snapshot unavailable (baseline: missing export)\n" + good["stdout"]
        verifier.verify_guest_status(good, "wsi_lifetime", Device)

    def test_cli_json_failure_exit_and_optimized_python(self):
        """The real CLI fails closed under -O and rejects duplicate JSON keys."""
        good = fixture("triangle_present")
        bad_order = dict(good, stdout=good["stdout"].replace("lifetime=2 device=", "lifetime=1 device="))
        inputs = ((json.dumps(good).encode(), 0), (json.dumps(bad_order).encode(), 1),
                  (b'{"exit":"1",' + json.dumps(good)[1:].encode(), 1),
                  (b'{"stdout":"failed",' + json.dumps(good)[1:].encode(), 1),
                  (b'{', 1), (b'\xff', 1), (b'null', 1))
        with tempfile.TemporaryDirectory(prefix="waddle_m2_output_") as directory:
            path = Path(directory) / "status.json"
            for optimization in ([], ["-O"]):
                for raw, expected_exit in inputs:
                    with self.subTest(optimization=optimization, raw=raw[:24]):
                        path.write_bytes(raw)
                        result = subprocess.run([sys.executable, "-B", *optimization, str(VerifierPath),
                            "--status", str(path), "--workload", "triangle_present", "--device", Device],
                            capture_output=True, text=True, check=False)
                        self.assertEqual(result.returncode, expected_exit, result.stderr)
                        report = json.loads(result.stdout)
                        self.assertEqual(report["passed"], expected_exit == 0)
                        self.assertEqual(path.read_bytes(), raw)
                        self.assertEqual(len(report["status_sha256"]), 64)
                        self.assertEqual(len(report["verifier_sha256"]), 64)
            result = subprocess.run([sys.executable, "-B", str(VerifierPath), "--status", str(path.parent / "missing"),
                "--workload", "triangle_present", "--device", Device], capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 1)
            self.assertFalse(json.loads(result.stdout)["passed"])


if __name__ == "__main__":
    unittest.main()
