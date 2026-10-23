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


Candidate = r"C:\M2\waddle_vulkan_experimental.dll"


def ownership_record(workload, lifetime):
    """Owned synthetic protocol record, never native execution evidence."""
    owner = [1, 72, lifetime + 1, lifetime, lifetime, 0, 0, 0, 0, 0, 0, 0]
    return {"session": "1234567890abcdef", "workload": workload, "lifetime": lifetime,
            "module_hex": Candidate.encode("ascii").hex(), "handles": 199 + lifetime,
            "icd": [1, 144, 0, 0, 0, 1, 0x100000001 + lifetime, 0x100000001 + lifetime] + [0] * 13,
            "sink_dc": owner.copy(), "fixture_dc": owner.copy(), "fixture_window": owner.copy()}


def owned_fixture(workload):
    """Fresh synthetic complete ownership fixture with diagnostic count drift."""
    status = fixture(workload)
    lines = status["stdout"].splitlines()
    result = []
    for line in lines:
        if line.startswith("Native actual "):
            lifetime = int(line.split(" lifetime=")[1].split()[0])
            result.append("M2_OWNED_V1 " + json.dumps(ownership_record(workload, lifetime)))
        result.append(line)
        if line.startswith("Native repeated "):
            result.append("M2_OWNED_V1 " + json.dumps(ownership_record(workload, 0)))
    status["stdout"] = "\n".join(result) + "\n"
    return status


def replace_record(status, lifetime, record):
    """Return independently owned evidence with exactly one mutated boundary."""
    lines = status["stdout"].splitlines()
    indices = [i for i, line in enumerate(lines) if line.startswith("M2_OWNED_V1 ")]
    lines[indices[lifetime]] = "M2_OWNED_V1 " + json.dumps(record)
    return dict(status, stdout="\n".join(lines) + "\n")


class m2_output_test_t(unittest.TestCase):
    """Owns temporary fixtures per test; isolated, no VM/GPU or shared artifacts."""

    def test_valid_workloads_and_line_endings(self):
        """Both exact workloads accept CRLF, LF and CR without weakening events."""
        for workload in ("triangle_present", "wsi_lifetime"):
            for separator in ("\r\n", "\n", "\r"):
                with self.subTest(workload=workload, separator=repr(separator)):
                    status = fixture(workload)
                    status["stdout"] = status["stdout"].replace("\r\n", separator)
                    result = verifier.verify_guest_status(status, workload, Device, "strict_count_v1")
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
                        verifier.verify_guest_status(bad, workload, Device, "strict_count_v1")
            bad = dict(good, stdout=good["stdout"].replace("lifetime=2 device=", "lifetime=X device=")
                       .replace("lifetime=3 device=", "lifetime=2 device=").replace("lifetime=X device=", "lifetime=3 device="))
            with self.assertRaises(ValueError):
                verifier.verify_guest_status(bad, workload, Device, "strict_count_v1")

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
                            verifier.verify_guest_status(dict(good, stdout="\n".join(changed)), workload, Device, "strict_count_v1")

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
                        verifier.verify_guest_status(dict(good, stdout=good["stdout"].replace(old, new, 1)), workload, Device, "strict_count_v1")
            foreign = ("Native WSI lifetime case=" + Labels[0] + " PASS" if workload == "triangle_present"
                       else "Native Vulkan actual swapchain mode=FIFO frame=1 exact4096RGB/BGRA PASS")
            with self.assertRaises(ValueError):
                verifier.verify_guest_status(dict(good, stdout=foreign + "\n" + good["stdout"]), workload, Device, "strict_count_v1")

    def test_failures_in_either_stream_even_when_counts_pass(self):
        """Each known fixture failure contradicts PASS, even inside a loader line."""
        for workload in ("triangle_present", "wsi_lifetime"):
            for marker in verifier.FailureMarkers:
                for stream in ("stdout", "stderr"):
                    with self.subTest(workload=workload, marker=marker, stream=stream):
                        bad = fixture(workload)
                        bad[stream] = "loader prefix " + marker + " detail\n" + bad[stream]
                        with self.assertRaises(ValueError):
                            verifier.verify_guest_status(bad, workload, Device, "strict_count_v1")

    def test_exit_missing_fields_and_wrong_schema(self):
        """No missing/failed exit, implicit output coercion or smoke workload passes."""
        good = fixture("triangle_present")
        for value in (None, 0, False, "1", "-1", "", "00"):
            with self.subTest(exit=value):
                with self.assertRaises(ValueError):
                    verifier.verify_guest_status(dict(good, exit=value), "triangle_present", Device, "strict_count_v1")
        for key in ("exit", "stdout", "stderr"):
            for mutation in ("missing", "null", "array"):
                bad = copy.deepcopy(good)
                if mutation == "missing":
                    del bad[key]
                else:
                    bad[key] = None if mutation == "null" else []
                with self.subTest(key=key, mutation=mutation):
                    with self.assertRaises(ValueError):
                        verifier.verify_guest_status(bad, "triangle_present", Device, "strict_count_v1")
        for status in (None, [], "0"):
            with self.assertRaises(ValueError):
                verifier.verify_guest_status(status, "triangle_present", Device, "strict_count_v1")
        with self.assertRaises(ValueError):
            verifier.verify_guest_status(good, "triangle_present_smoke", Device, "strict_count_v1")

    def test_header_final_and_output_channel(self):
        """Headers and end markers bind the whole run and cannot be misplaced."""
        good = fixture("wsi_lifetime")
        for old, new in (("1234567890abcdef", "0000000000000000"),
                         ("workload=wsi_lifetime", "workload=triangle_present"),
                         ("public_api=1.0", "public_api=1.3"),
                         ("baseline=199", "baseline=0")):
            with self.subTest(new=new):
                with self.assertRaises(ValueError):
                    verifier.verify_guest_status(dict(good, stdout=good["stdout"].replace(old, new)), "wsi_lifetime", Device, "strict_count_v1")
        for stream, extra in (("stdout", "trailing text\n"), ("stderr", Final), ("stdout", "\0")):
            bad = dict(good)
            bad[stream] += extra
            with self.assertRaises(ValueError):
                verifier.verify_guest_status(bad, "wsi_lifetime", Device, "strict_count_v1")

    def test_diagnostics_are_passive(self):
        """Unavailable NT snapshots and clocks never change the native count gate."""
        good = fixture("wsi_lifetime")
        good["stdout"] = "handle_diag: snapshot unavailable (baseline: missing export)\n" + good["stdout"]
        verifier.verify_guest_status(good, "wsi_lifetime", Device, "strict_count_v1")

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
                            "--status", str(path), "--workload", "triangle_present", "--device", Device, "--protocol", "strict_count_v1"],
                            capture_output=True, text=True, check=False)
                        self.assertEqual(result.returncode, expected_exit, result.stderr)
                        report = json.loads(result.stdout)
                        self.assertEqual(report["passed"], expected_exit == 0)
                        self.assertEqual(path.read_bytes(), raw)
                        self.assertEqual(len(report["status_sha256"]), 64)
                        self.assertEqual(len(report["verifier_sha256"]), 64)
            result = subprocess.run([sys.executable, "-B", str(VerifierPath), "--status", str(path.parent / "missing"),
                "--workload", "triangle_present", "--device", Device, "--protocol", "strict_count_v1"], capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 1)
            self.assertFalse(json.loads(result.stdout)["passed"])


class m2_owned_output_test_t(unittest.TestCase):
    """Exact ownership record and actual C serialization false-pass controls."""

    def test_protocol_path_and_legacy_separation(self):
        """Both complete protocols require explicit selection and cannot substitute."""
        for workload in verifier.Workloads:
            good = owned_fixture(workload)
            result = verifier.verify_guest_status(good, workload, Device, "m2_owned_v1", Candidate)
            self.assertEqual(result["ownership_boundaries"], 9)
            self.assertEqual(result["diagnostic_handle_counts"], list(range(199, 208)))
            for status, protocol, path in ((fixture(workload), "m2_owned_v1", Candidate),
                    (good, "strict_count_v1", None), (good, "unknown", Candidate),
                    (good, "m2_owned_v1", None), (good, "m2_owned_v1", r"C:\other.dll"),
                    (good, "strict_count_v1", Candidate)):
                with self.subTest(protocol=protocol, path=path):
                    with self.assertRaises(ValueError):
                        verifier.verify_guest_status(status, workload, Device, protocol, path)
        for path in ("", "relative.dll", r"C:relative.dll", r"C:\x\..\candidate.dll", "C:\\bad\n", "C:\\" + "x" * 260, "C:\\é"):
            with self.assertRaises(ValueError):
                verifier.qualified_path(path)

    def test_every_boundary_deletion_duplicate_swap_and_channel(self):
        """All nine owner records bind their exact position and stdout channel."""
        for workload in verifier.Workloads:
            good = owned_fixture(workload)
            lines = good["stdout"].splitlines()
            indices = [i for i, line in enumerate(lines) if line.startswith("M2_OWNED_V1 ")]
            for index in indices:
                swapped = lines.copy()
                swapped[index], swapped[index+1] = swapped[index+1], swapped[index]
                for changed in (lines[:index] + lines[index+1:], lines[:index] + [lines[index]] + lines[index:], swapped):
                    with self.assertRaises(ValueError):
                        verifier.verify_guest_status(dict(good, stdout="\n".join(changed)), workload, Device, "m2_owned_v1", Candidate)
                for stream in ("stdout", "stderr"):
                    bad = dict(good)
                    bad[stream] += "prefix " + lines[index] + "\n"
                    with self.assertRaises(ValueError):
                        verifier.verify_guest_status(bad, workload, Device, "m2_owned_v1", Candidate)

    def test_every_abi_field_and_owner_malformed(self):
        """Every retained slot, malformed word, length and ABI independently fails."""
        for workload in verifier.Workloads:
            good = owned_fixture(workload)
            for group in ("icd", "sink_dc", "fixture_dc", "fixture_window"):
                values = ownership_record(workload, 3)[group]
                for index in range(len(values)):
                    for malformed in (None, True, -1, 1.0, "0", 1 << 64):
                        record = ownership_record(workload, 3)
                        record[group][index] = malformed
                        with self.subTest(group=group, index=index, malformed=malformed):
                            with self.assertRaises(ValueError):
                                verifier.verify_guest_status(replace_record(good, 3, record), workload, Device, "m2_owned_v1", Candidate)
                for malformed in (values[:-1], values + [0], {}, None):
                    record = ownership_record(workload, 3); record[group] = malformed
                    with self.assertRaises(ValueError):
                        verifier.verify_guest_status(replace_record(good, 3, record), workload, Device, "m2_owned_v1", Candidate)
                for index in ([0, 1, 2, 3, 4, 5] + list(range(8, 21)) if group == "icd" else [0, 1] + list(range(5, 12))):
                    record = ownership_record(workload, 3)
                    record[group][index] = record[group][index] + 1
                    with self.assertRaises(ValueError):
                        verifier.verify_guest_status(replace_record(good, 3, record), workload, Device, "m2_owned_v1", Candidate)

    def test_identity_activity_schema_and_diagnostic_failures(self):
        """Missing/extra fields, stale marks, hidden native faults and no count fail."""
        good = owned_fixture("wsi_lifetime")
        original = ownership_record("wsi_lifetime", 3)
        mutations = []
        for key in original:
            record = copy.deepcopy(original); del record[key]; mutations.append(record)
        for key, value in (("extra", 0), ("session", "0000000000000000"), ("workload", "triangle_present"),
                           ("lifetime", True), ("lifetime", 2), ("module_hex", "ff"),
                           ("module_hex", "xx"), ("handles", 0), ("handles", True), ("handles", 1 << 32)):
            record = copy.deepcopy(original); record[key] = value; mutations.append(record)
        for index, value in ((6, 0), (6, 0x100000003), (7, 0x100000003), (7, 0x200000004), (6, 0x100000000)):
            record = copy.deepcopy(original); record["icd"][index] = value; mutations.append(record)
        record = copy.deepcopy(original); record["icd"][6] += 1 << 32; record["icd"][7] += 1 << 32; mutations.append(record)
        for group in ("sink_dc", "fixture_dc", "fixture_window"):
            for index, value in ((2, 3), (3, 2), (4, 2)):
                record = copy.deepcopy(original); record[group][index] = value; mutations.append(record)
            record = copy.deepcopy(original); record[group] = ownership_record("wsi_lifetime", 2)[group]; mutations.append(record)
            # More successful acquisitions than newly reserved identities is impossible.
            record = copy.deepcopy(original); record[group][3:5] = [10, 10]; record[group][2] = 11
            previous = ownership_record("wsi_lifetime", 2); previous[group][2] = 10
            altered = replace_record(good, 2, previous)
            with self.assertRaises(ValueError):
                verifier.verify_guest_status(replace_record(altered, 3, record), "wsi_lifetime", Device, "m2_owned_v1", Candidate)
        for record in mutations:
            with self.assertRaises(ValueError):
                verifier.verify_guest_status(replace_record(good, 3, record), "wsi_lifetime", Device, "m2_owned_v1", Candidate)
        record = ownership_record("wsi_lifetime", 0); record["handles"] = 200
        with self.assertRaises(ValueError):
            verifier.verify_guest_status(replace_record(good, 0, record), "wsi_lifetime", Device, "m2_owned_v1", Candidate)
        for marker in verifier.FailureMarkers:
            with self.assertRaises(ValueError):
                verifier.verify_guest_status(dict(good, stderr=good["stderr"] + marker), "wsi_lifetime", Device, "m2_owned_v1", Candidate)
        duplicate = good["stdout"].replace('"handles": 202', '"handles": 0, "handles": 202')
        with self.assertRaises(ValueError):
            verifier.verify_guest_status(dict(good, stdout=duplicate), "wsi_lifetime", Device, "m2_owned_v1", Candidate)

    def test_actual_c_serialization_and_cli(self):
        """Compile actual controller over doubles; verify serialized records and CLI."""
        with tempfile.TemporaryDirectory(prefix="m2-controller-") as temporary:
            binary = Path(temporary) / "controller"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-O2",
                str(Root / "tests/vgpu/m2_owned_fixture_test.c"), str(Root / "tests/vgpu/m2_owner_ledger.c"),
                "-o", str(binary)], check=True, capture_output=True, text=True)
            for workload in verifier.Workloads:
                result = subprocess.run([str(binary), "--emit-workload", workload], check=True, capture_output=True, text=True)
                status = {"exit": "0", "stdout": result.stdout, "stderr": result.stderr}
                self.assertEqual(verifier.verify_guest_status(status, workload, Device, "m2_owned_v1", Candidate)["ownership_boundaries"], 9)
                path = Path(temporary) / "status.json"; path.write_text(json.dumps(status))
                for optimized in ([], ["-O"]):
                    command = [sys.executable, *optimized, "-B", str(VerifierPath), "--status", str(path), "--workload", workload,
                               "--device", Device, "--protocol", "m2_owned_v1", "--icd-path", Candidate]
                    run = subprocess.run(command, capture_output=True, text=True)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                    self.assertTrue(json.loads(run.stdout)["passed"])
                    self.assertEqual(subprocess.run(command[:command.index("--protocol")], capture_output=True, text=True).returncode, 2)
                    bad = dict(status)
                    # Actual array word 21 is the last zero owner field before sink_dc.
                    bad["stdout"] = status["stdout"].replace('0],"sink_dc"', '1],"sink_dc"', 1)
                    path.write_text(json.dumps(bad))
                    self.assertEqual(subprocess.run(command, capture_output=True, text=True).returncode, 1)
                    path.write_text(json.dumps(status))

    def test_actual_controller_coverage_schema_controls(self):
        """The new real-include coverage gate rejects omitted/duplicate/empty data."""
        coverage_spec = importlib.util.spec_from_file_location("m2_controller_coverage", Root / "tests/vgpu/m2_owned_fixture_coverage.py")
        coverage = importlib.util.module_from_spec(coverage_spec); coverage_spec.loader.exec_module(coverage)
        files = [{"file": str(Root / name), "lines": [{"line_number": 1, "count": 1, "branches": [{"count": 1}, {"count": 1}]}]}
                 for name in coverage.CoverageFiles]
        self.assertEqual(coverage.coverage_summary({"files": files}, Root)["branches"]["percent"], 100)
        for malformed in (None, {}, {"files": []}, {"files": files[:-1]}, {"files": files + [files[0]]}):
            with self.assertRaises(ValueError): coverage.coverage_summary(malformed, Root)
        for mutation in ("unknown", "zero-lines", "zero-branches", "below-floor", "duplicate-line", "bool-count"):
            bad = copy.deepcopy(files); target = next(item for item in bad if item["file"] == str(Root / coverage.CoverageSource))
            if mutation == "unknown": target["file"] = "unexpected"
            if mutation == "zero-lines": target["lines"] = []
            if mutation == "zero-branches": target["lines"][0]["branches"] = []
            if mutation == "below-floor": target["lines"][0]["branches"][0]["count"] = 0
            if mutation == "duplicate-line": target["lines"].append(copy.deepcopy(target["lines"][0]))
            if mutation == "bool-count": target["lines"][0]["count"] = True
            with self.assertRaises(ValueError): coverage.coverage_summary({"files": bad}, Root)


if __name__ == "__main__":
    unittest.main()
