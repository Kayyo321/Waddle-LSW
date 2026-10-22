#!/usr/bin/env python3
"""Adversarial final-DLL output controls; fabricated transcripts are tests only."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

Root = Path(__file__).resolve().parents[2]
Spec = importlib.util.spec_from_file_location("opencl_reload_output", Root / "scripts/opencl_reload_output.py")
Gate = importlib.util.module_from_spec(Spec)
Spec.loader.exec_module(Gate)
Hash = "a" * 64
Creation = "0000000100000037"
Module = "C:\\candidate\\OpenCL.dll"


def proof(cycle):
    worker, renderer = 10000 + cycle, 50000 + cycle*2
    members = [
        {"pid": 9000, "parent": 8000, "starttime": 200, "group": 9000, "session": 9000, "state": "S", "executable": "/frozen/controller"},
        {"pid": worker, "parent": 9000, "starttime": 300+cycle, "group": worker, "session": 9000, "state": "S", "executable": "/frozen/waddle_vgpu_worker"},
        {"pid": renderer, "parent": worker, "starttime": 400+cycle, "group": renderer, "session": 9000, "state": "S", "executable": "/frozen/virgl_render_server"},
        {"pid": renderer+1, "parent": renderer, "starttime": 401+cycle, "group": renderer, "session": 9000, "state": "S", "executable": "/frozen/virgl_render_server"},
    ]
    owners = [{"pid": m["pid"], "starttime": m["starttime"], "owner_pid": 8000, "descriptor": i+6, "open": True} for i,m in enumerate(members)]
    retired = [{"pid": m["pid"], "starttime": m["starttime"], "verified_absent": True, "observed_starttime": None,
                "group": m["group"], "group_empty": True, "session": m["session"], "session_empty": True, "query_error": None} for m in members]
    closed = [{"pid": o["pid"], "starttime": o["starttime"], "descriptor": o["descriptor"], "closed": True} for o in owners]
    return {"fields": {"session": f"{cycle:016x}", "worker_pid": str(worker), "worker_retired": "1", "status": "0", "worker_exit_status": "0"},
            "graph": {"owned_session_empty": True, "signals": [], "controller_exit": 0, "observer_error": None, "journal_error": None,
                      "controller": {k:v for k,v in members[0].items() if k != "executable"}, "members": members, "pidfd_owners": owners,
                      "adopted_reaps": [{"pid": renderer+1, "starttime": 401+cycle, "wait_status": 65280}]},
            "retired_members": retired,
            "owner_close": {"owner_pid": 8000, "all_pidfds_closed": True, "error": None, "pidfd_owners": closed}}


def record(value):
    return Gate.Prefix + json.dumps(value, separators=(",", ":"))


def valid():
    baseline = {"event": "baseline", "pid": 1234, "creation": Creation, "cycles": 1100, "contexts_per_cycle": 3, "idle_threads": 4,
                "handles": 62, "tls": 8, "fls": 12, "heap_acceptance": False, "entry": [50, 8, 12],
                "os_preinit": "user32,winsock,bcrypt,dxgi_factory,search_order,package_family", "module_utf16": Module.encode("utf-16-be").hex()}
    lines = [record(baseline)]
    hosts = []
    for cycle in range(1, 1101):
        lines.extend([f"DXVK heap capture reload_before_{cycle} pid=1234 creation={Creation} handles=62",
                      f"Authenticated real OpenCL session={cycle:016x}"])
        lines.extend([Gate.Buffer, Gate.Lifetime]*3)
        lines.extend([Gate.Callers, Gate.Retirement])
        lines.append(record({"event": "cycle", "cycle": cycle, "pid": 1234, "creation": Creation, "session": f"{cycle:016x}", "foreign": [501, 502],
                             "contexts": 3, "module_exact": 1, "modules_absent": 1, "handles": 62, "tls": 8, "fls": 12}))
        lines.append(f"DXVK heap capture reload_after_{cycle} pid=1234 creation={Creation} handles=62")
        hosts.append({"cycle": cycle, "proof": f"/proof/{cycle}.json", "sha256": "b"*64})
    lines.append(f"DXVK heap capture reload_cleanup pid=1234 creation={Creation} handles=50")
    lines.append(record({"event": "complete", "cycles": 1100, "contexts": 3300, "cleanup": True, "heap_acceptance": False, "entry": [50, 8, 12]}))
    manifest = {"schema": "opencl_reload_manifest_v1", "cycles": 1100, "pid": 1234, "creation": Creation, "module_path": Module, "candidate_sha256": Hash, "fixture_sha256": Hash, "hosts": hosts}
    return lines, manifest


class OutputTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lines, cls.manifest = valid()

    def verify(self, lines=None, manifest=None, stderr="", code=0, candidate=Hash, loader=None, fixture=Hash, expected_candidate=Hash, expected_fixture=Hash):
        return Gate.verify("\n".join(self.lines if lines is None else lines)+"\n", stderr, code,
                           self.manifest if manifest is None else manifest, candidate, fixture, expected_candidate, expected_fixture,
                           loader or (lambda entry: proof(entry["cycle"])))

    def reject_record(self, event, field, value=None, remove=False):
        lines = self.lines.copy()
        for index, line in enumerate(lines):
            if line.startswith(Gate.Prefix):
                data = json.loads(line[len(Gate.Prefix):])
                if data["event"] == event:
                    if remove:
                        del data[field]
                    else:
                        data[field] = value
                    lines[index] = record(data)
                    break
        with self.assertRaises(ValueError):
            self.verify(lines)

    def test_positive_scope(self):
        result = self.verify(stderr="[CLVK] expected invalid kernel build error\n")
        self.assertEqual(result["contexts"], 3300)
        self.assertFalse(result["heap_acceptance"])
        self.assertEqual(result["scope"], "full_dll_reload_lifecycle_only")

    def test_baseline_shape_and_identity(self):
        for field in json.loads(self.lines[0][len(Gate.Prefix):]):
            with self.subTest(missing=field):
                self.reject_record("baseline", field, remove=True)
        for field, value in [("cycles", 8), ("cycles", True), ("contexts_per_cycle", 8), ("idle_threads", 0), ("pid", 0),
                             ("creation", "0"*16), ("module_utf16", "ff"), ("heap_acceptance", True), ("handles", True),
                             ("tls", -1), ("fls", "12"), ("entry", [50, 8]), ("entry", [True, 8, 12]), ("extra", 1),
                             ("os_preinit", "user32,winsock,bcrypt,dxgi_factory"), ("os_preinit", ""), ("os_preinit", None),
                             ("os_preinit", "user32,winsock,bcrypt,dxgi_factory,search_order,package_family,extra")]:
            with self.subTest(field=field, value=value):
                self.reject_record("baseline", field, value)

    def test_cycle_shape_and_resources(self):
        example = next(json.loads(line[len(Gate.Prefix):]) for line in self.lines if line.startswith(Gate.Prefix) and '"event":"cycle"' in line)
        for field in example:
            with self.subTest(missing=field):
                self.reject_record("cycle", field, remove=True)
        for field, value in [("cycle", 0), ("cycle", 2), ("cycle", True), ("pid", 4321), ("creation", "f"*16), ("session", "0"*16),
                             ("session", "xyz"), ("foreign", [501, 501]), ("foreign", [0, 502]), ("foreign", [True, 502]),
                             ("foreign", [1]), ("module_exact", 0), ("modules_absent", False), ("handles", 63), ("tls", 7),
                             ("fls", 11), ("contexts", 8), ("extra", 1)]:
            with self.subTest(field=field, value=value):
                self.reject_record("cycle", field, value)

    def test_terminal_shape_and_cleanup(self):
        terminal = json.loads(self.lines[-1][len(Gate.Prefix):])
        for field in terminal:
            with self.subTest(missing=field):
                self.reject_record("complete", field, remove=True)
        for field, value in [("cycles", 1099), ("contexts", 3297), ("cleanup", False), ("cleanup", 1),
                             ("heap_acceptance", True), ("entry", [51, 8, 12]), ("extra", 0)]:
            with self.subTest(field=field):
                self.reject_record("complete", field, value)

    def test_missing_duplicate_reordered_output(self):
        cycle = next(i for i, line in enumerate(self.lines) if '"event":"cycle"' in line)
        mutations = [self.lines[1:], self.lines[:-1], self.lines[:cycle]+self.lines[cycle+1:],
                     self.lines[:cycle]+[self.lines[cycle]]+self.lines[cycle:], self.lines+[self.lines[-1]],
                     self.lines+["unexpected trailing data"], [self.lines[0]]+self.lines, self.lines[:cycle]+[self.lines[-1]],
                     self.lines[:1]+self.lines[2:]]
        for lines in mutations:
            with self.subTest(size=len(lines)):
                with self.assertRaises(ValueError):
                    self.verify(lines)
        lines = self.lines.copy()
        second = next(i for i, line in enumerate(lines) if '"cycle":2,' in line)
        data = json.loads(lines[second][len(Gate.Prefix):]);data["session"] = "0000000000000001";lines[second] = record(data)
        with self.assertRaises(ValueError):
            self.verify(lines)

    def test_workload_and_handshake_binding(self):
        for needle in [Gate.Lifetime, Gate.Buffer, Gate.Callers, Gate.Retirement, "Authenticated real OpenCL session=0000000000000001",
                       self.lines[1], self.lines[-2], next(line for line in self.lines if line.startswith("DXVK heap capture reload_after_1 "))]:
            lines = self.lines.copy();lines.remove(needle)
            with self.subTest(missing=needle):
                with self.assertRaises(ValueError):
                    self.verify(lines)
        lines = self.lines.copy();lines[1] = lines[1].replace("handles=62", "handles=63")
        with self.assertRaises(ValueError):
            self.verify(lines)

    def test_failure_exit_and_strict_json(self):
        for code in [1, -9, None, True]:
            with self.subTest(code=code):
                with self.assertRaises(ValueError):
                    self.verify(code=code)
        # Independent literal controls: removing a new failure prefix must not
        # silently turn an otherwise complete transcript into acceptance.
        for marker in ["OpenCL reload probe allocation failed kind=tls error=8 acquired=3",
                       "OpenCL reload control allocation failed kind=fls error=0"]:
            for stream in ("stdout", "stderr"):
                with self.subTest(allocation_failure=marker, stream=stream):
                    with self.assertRaises(ValueError):
                        lines=self.lines[:2]+[marker]+self.lines[2:] if stream=="stdout" else None
                        self.verify(lines,stderr=marker if stream=="stderr" else "")
        for marker in Gate.Forbidden:
            for stream in ("stdout", "stderr"):
                with self.subTest(marker=marker, stream=stream):
                    with self.assertRaises(ValueError):
                        self.verify(self.lines+[marker] if stream == "stdout" else None, stderr=marker if stream == "stderr" else "")
        for line in [Gate.Prefix+"null", Gate.Prefix+"[]", Gate.Prefix+'{"event":"baseline","event":"cycle"}', Gate.Prefix+'{"x":NaN}',
                     "OPENCL_RELOAD_V1 broken", Gate.Prefix+'{"event":"unknown"}']:
            with self.assertRaises(ValueError):
                self.verify([line]+self.lines[1:])
        with self.assertRaises(ValueError):
            self.verify(stderr=self.lines[0])

    def test_manifest_candidate_and_host_binding(self):
        for key in self.manifest:
            manifest = copy.deepcopy(self.manifest);del manifest[key]
            with self.subTest(missing=key):
                with self.assertRaises(ValueError):
                    self.verify(manifest=manifest)
        for key, value in [("schema", "old"), ("cycles", 2048), ("cycles", True), ("pid", True), ("creation", "0"*16),
                           ("module_path", "relative.dll"), ("candidate_sha256", "c"*64), ("fixture_sha256", "c"*64), ("hosts", [])]:
            manifest = copy.deepcopy(self.manifest);manifest[key] = value
            with self.subTest(key=key):
                with self.assertRaises(ValueError):
                    self.verify(manifest=manifest)
        manifest = copy.deepcopy(self.manifest);manifest["hosts"][0]["cycle"] = 2
        with self.assertRaises(ValueError):
            self.verify(manifest=manifest)
        for section, field, value in [("fields", "session", "f"*16), ("fields", "worker_retired", "0"), ("fields", "status", "1"),
                                      ("fields", "worker_exit_status", "9"), ("fields", "worker_pid", "0"),
                                      ("graph", "owned_session_empty", False), ("graph", "signals", [9]), ("graph", "controller_exit", True),
                                      ("graph", "observer_error", "failed"), ("graph", "journal_error", "failed"),
                                      ("graph", "members", []), ("graph", "pidfd_owners", []), ("graph", "controller", {})]:
            def bad(entry):
                result = proof(entry["cycle"]);result[section][field] = value;return result
            with self.subTest(section=section, field=field):
                with self.assertRaises(ValueError):
                    self.verify(loader=bad)

    def test_complete_host_retirement_and_closure(self):
        original = proof(1)
        Gate.verify_host(original, "0000000000000001")
        # Real supervisor histories include controller, worker and renderer tree;
        # historical S states, open pidfds and child exit255 are all valid here.
        self.assertEqual(len(original["graph"]["members"]), 4)
        cases = []
        for section, key in [("graph", "members"), ("graph", "pidfd_owners"),
                             ("owner_close", "pidfd_owners")]:
            for operation in ("missing", "duplicate", "extra", "reused"):
                value = copy.deepcopy(original);items = value[section][key]
                if operation == "missing":
                    items.pop()
                elif operation == "duplicate":
                    items.append(copy.deepcopy(items[-1]))
                elif operation == "extra":
                    extra = copy.deepcopy(items[-1]);extra["pid"] += 70000;items.append(extra)
                else:
                    extra = copy.deepcopy(items[-1]);extra["starttime"] += 1;items.append(extra)
                cases.append(value)
        for operation in ("missing", "duplicate", "extra", "reused"):
            value = copy.deepcopy(original);items = value["retired_members"]
            if operation == "missing":
                items.pop()
            else:
                extra = copy.deepcopy(items[-1])
                if operation == "extra":extra["pid"] += 70000
                if operation == "reused":extra["starttime"] += 1
                items.append(extra)
            cases.append(value)
        for name in original["retired_members"][0]:
            value = copy.deepcopy(original);del value["retired_members"][0][name];cases.append(value)
        for name, bad in [("verified_absent", False), ("observed_starttime", 900000), ("group", 123),
                          ("group_empty", False), ("session", 123), ("session_empty", False), ("query_error", "denied")]:
            value = copy.deepcopy(original);value["retired_members"][3][name] = bad;cases.append(value)
        for section, index, name, bad in [("members", 3, "session", 777), ("members", 3, "parent", 777),
                                          ("members", 3, "parent", original["graph"]["members"][3]["pid"]),
                                          ("members", 1, "executable", "/wrong/program"),
                                          ("pidfd_owners", 3, "open", False), ("pidfd_owners", 3, "owner_pid", 777),
                                          ("pidfd_owners", 3, "descriptor", 6)]:
            value = copy.deepcopy(original);value["graph"][section][index][name] = bad;cases.append(value)
        for name, bad in [("all_pidfds_closed", False), ("error", "close failed"), ("owner_pid", 777)]:
            value = copy.deepcopy(original);value["owner_close"][name] = bad;cases.append(value)
        for name, bad in [("closed", False), ("descriptor", 90), ("starttime", 99999)]:
            value = copy.deepcopy(original);value["owner_close"]["pidfd_owners"][3][name] = bad;cases.append(value)
        for name, bad in [("pid", 900000), ("starttime", 999999), ("wait_status", True), ("wait_status", -1)]:
            value = copy.deepcopy(original);value["graph"]["adopted_reaps"][0][name] = bad;cases.append(value)
        value = copy.deepcopy(original);value["graph"]["adopted_reaps"] *= 2;cases.append(value)
        for value in cases:
            with self.subTest(proof=value):
                with self.assertRaises(ValueError):Gate.verify_host(value, "0000000000000001")
        # Empty reap history is valid; complete receipts remain mandatory.
        value = copy.deepcopy(original);value["graph"]["adopted_reaps"] = []
        Gate.verify_host(value, "0000000000000001")

    def test_independent_pre_run_hash_binding(self):
        for expected in (None, "", "g"*64, "c"*64):
            with self.subTest(expected=expected):
                with self.assertRaises(ValueError):self.verify(expected_candidate=expected)
                with self.assertRaises(ValueError):self.verify(expected_fixture=expected)
        for key in ("candidate_sha256", "fixture_sha256"):
            manifest=copy.deepcopy(self.manifest);manifest[key]="c"*64
            with self.assertRaises(ValueError):
                self.verify(manifest=manifest, candidate="c"*64 if key=="candidate_sha256" else Hash,
                            fixture="c"*64 if key=="fixture_sha256" else Hash)

    def test_make_requires_both_frozen_hashes(self):
        command=["make", "-n", "vgpu-opencl-reload-verify", "VgpuOpenclReloadStdout=dummy", "VgpuOpenclReloadStderr=dummy",
                 "VgpuOpenclReloadManifest=dummy", "VgpuOpenclReloadDll=dummy", "VgpuOpenclReloadFixture=dummy", "VgpuOpenclReloadExit=0"]
        for variable in ("VgpuOpenclReloadExpectedCandidateSha256", "VgpuOpenclReloadExpectedFixtureSha256"):
            result=subprocess.run(command,cwd=Root,capture_output=True,text=True)
            self.assertNotEqual(result.returncode,0)
            self.assertIn(variable+" is required", result.stderr)
            command.append(variable+"="+Hash)
        result=subprocess.run(command,cwd=Root,capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn("--expected-candidate-sha256",result.stdout)
        self.assertIn("--expected-fixture-sha256",result.stdout)

    def test_frozen_proof_and_operational_cli(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder);manifest = copy.deepcopy(self.manifest)
            dll = root/"OpenCL.dll";dll.write_bytes(b"API-double candidate; not Windows executable")
            manifest["candidate_sha256"] = hashlib.sha256(dll.read_bytes()).hexdigest()
            fixture = root/"fixture.exe";fixture.write_bytes(b"API-double fixture")
            manifest["fixture_sha256"] = hashlib.sha256(fixture.read_bytes()).hexdigest()
            for entry in manifest["hosts"]:
                path = root/f"host_{entry['cycle']}.json";path.write_text(json.dumps(proof(entry["cycle"])))
                entry["proof"] = str(path);entry["sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
            stdout = root/"stdout";stdout.write_text("\n".join(self.lines)+"\n")
            stderr = root/"stderr";stderr.write_text("")
            record_path = root/"manifest.json";record_path.write_text(json.dumps(manifest))
            command = [sys.executable, "-B", str(Root/"scripts/opencl_reload_output.py"), "--stdout", str(stdout), "--stderr", str(stderr),
                       "--exit-code", "0", "--manifest", str(record_path), "--dll", str(dll), "--fixture", str(fixture),
                       "--expected-candidate-sha256", manifest["candidate_sha256"], "--expected-fixture-sha256", manifest["fixture_sha256"]]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse(json.loads(result.stdout)["heap_acceptance"])
            # Replacing both mutable file and manifest still cannot satisfy
            # the independently frozen pre-run command arguments.
            for path,key in ((dll,"candidate_sha256"),(fixture,"fixture_sha256")):
                old_bytes=path.read_bytes();old_hash=manifest[key]
                path.write_bytes(b"substituted after execution")
                manifest[key]=hashlib.sha256(path.read_bytes()).hexdigest();record_path.write_text(json.dumps(manifest))
                rejected=subprocess.run(command,capture_output=True,text=True)
                self.assertNotEqual(rejected.returncode,0)
                self.assertIn("differs from frozen pre-run hash",rejected.stderr)
                path.write_bytes(old_bytes);manifest[key]=old_hash;record_path.write_text(json.dumps(manifest))
            for flag in ("--expected-candidate-sha256","--expected-fixture-sha256"):
                missing=command.copy();index=missing.index(flag);del missing[index:index+2]
                rejected=subprocess.run(missing,capture_output=True,text=True)
                self.assertNotEqual(rejected.returncode,0);self.assertIn(flag,rejected.stderr)
            Path(manifest["hosts"][0]["proof"]).write_text("{}")
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("proof hash mismatch", result.stderr)


if __name__ == "__main__":
    unittest.main()
