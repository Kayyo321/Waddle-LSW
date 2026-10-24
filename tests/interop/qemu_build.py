"""Isolated contract tests for scripts/qemu_build.py; no real QEMU build is run."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest


Root = Path(__file__).resolve().parents[2]
Helper = Root / "scripts" / "qemu_build.py"
CommitA = "a" * 40
CommitB = "b" * 40
Macros = "\n".join((
    "#define CONFIG_AUDIO_PIPEWIRE 1",
    "#define CONFIG_SLIRP 1",
    "#define CONFIG_USB_LIBUSB 1",
    "",
))


def _process_is_live(pid: int) -> bool:
    """Return whether a Linux process exists outside the zombie state."""
    try:
        process_stat = Path(f"/proc/{pid}/stat").read_text(encoding="utf-8")
    except FileNotFoundError:
        return False
    state = process_stat.rsplit(")", 1)[1].split()[0]
    return state != "Z"


class qemu_build_contract_tests_t(unittest.TestCase):
    # unittest requires these two camel-case lifecycle hook names.
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="qemu helper test ")
        self.root = Path(self.temporary.name)
        self.bin_dir = self.root / "mock tools"
        self.bin_dir.mkdir()
        self.source = self.root / "pinned qemu source"
        self.source.mkdir()
        self.build = self.root / "out of tree build"
        self.log = self.root / "calls.jsonl"
        self._tool("git", r'''import os, sys
args = sys.argv[1:]
if "rev-parse" in args:
    commit_file = os.environ.get("MOCK_GIT_COMMIT_FILE")
    print(open(commit_file, encoding="utf-8").read().strip() if commit_file else os.environ.get("MOCK_GIT_COMMIT", "''' + CommitA + r'''"))
elif "status" in args:
    print(os.environ.get("MOCK_GIT_DIRTY", ""))
else:
    raise SystemExit(2)
''')
        self._tool("cc", r'''import sys
print("Mock C compiler " + " ".join(sys.argv[1:]))
''')
        self._tool("ninja", r'''import json, os, pathlib, sys
if sys.argv[1:] == ["--version"]:
    print("1.12.1")
    raise SystemExit(0)
with open(os.environ["MOCK_LOG"], "a", encoding="utf-8") as log:
    log.write(json.dumps({"tool":"ninja", "args":sys.argv[1:]}) + "\n")
if os.environ.get("MOCK_NINJA_FAIL"):
    raise SystemExit(19)
commit_file = os.environ.get("MOCK_GIT_COMMIT_FILE")
if commit_file and os.environ.get("MOCK_GIT_AFTER_BUILD"):
    pathlib.Path(commit_file).write_text(os.environ["MOCK_GIT_AFTER_BUILD"], encoding="utf-8")
if not os.environ.get("MOCK_NINJA_NO_BINARY") and not os.environ.get("MOCK_NINJA_KEEP_BINARY"):
    pathlib.Path("qemu-system-x86_64").write_bytes(b"mock qemu binary " + os.environ.get("MOCK_BINARY_REV", "one").encode())
''')
        self._tool("pkg-config", r'''import os, sys
args = sys.argv[1:]
package = args[-1]
if args[0] == "--version":
    print(os.environ.get("MOCK_PKG_CONFIG_VERSION", "1.8.1"))
elif args[0] == "--modversion":
    if os.environ.get("MOCK_PKG_CONFIG_FAIL"):
        raise SystemExit(8)
    fallback = {"libpipewire-0.3":"1.4.2", "slirp":"4.8.0", "libusb-1.0":"1.0.27"}[package]
    print(os.environ.get("MOCK_" + package.upper().replace("-", "_").replace(".", "_") + "_VERSION", fallback))
elif args[0] == "--cflags":
    print(os.environ.get("MOCK_CFLAGS_PREFIX", "-I/mock/") + package)
elif args[0] == "--libs":
    print("-lmock_" + package.replace("-", "_"))
else:
    raise SystemExit(2)
''')
        configure = self.source / "configure"
        configure.write_text(r'''#!/usr/bin/env python3
import json, os, pathlib, subprocess, sys, time
with open(os.environ["MOCK_LOG"], "a", encoding="utf-8") as log:
    log.write(json.dumps({"tool":"configure", "args":sys.argv[1:], "stripped":{key:os.environ.get(key) for key in ("CFLAGS", "CPPFLAGS", "LDFLAGS", "CXXFLAGS", "PYTHON", "MESON")}}) + "\n")
if os.environ.get("MOCK_CONFIGURE_FAIL"):
    raise SystemExit(23)
pathlib.Path("config-host.mak").write_text("# mock generated config\\n")
macros = os.environ.get("MOCK_CONFIG_MACROS", "''' + Macros.replace("\n", "\\n") + r'''")
pathlib.Path("config-host.h").write_text(macros)
if os.environ.get("MOCK_CONFIGURE_BLOCK"):
    pathlib.Path(os.environ["MOCK_OWNER_MARKER"]).write_text("ready", encoding="utf-8")
    cleanup_path = os.environ.get("MOCK_CHILD_CLEANUP")
    if cleanup_path:
        child_script = pathlib.Path(os.environ["MOCK_CHILD_SCRIPT"])
        child_ready = pathlib.Path(os.environ["MOCK_CHILD_READY"])
        child_script.write_text("import os, pathlib, signal, sys, time\ndef on_term(*_):\n    pathlib.Path(" + repr(cleanup_path) + ").write_text('term')\n    if not os.environ.get('MOCK_CHILD_IGNORE_TERM'):\n        raise SystemExit(0)\nsignal.signal(signal.SIGTERM, on_term)\npathlib.Path(" + repr(str(child_ready)) + ").write_text('ready')\nwhile True: time.sleep(1)\n", encoding="utf-8")
        child = subprocess.Popen([sys.executable, str(child_script)])
        pathlib.Path(os.environ["MOCK_CHILD_PID"]).write_text(str(child.pid), encoding="utf-8")
        while not child_ready.exists():
            time.sleep(0.01)
    else:
        pathlib.Path(".waddle-qemu-build.json").write_text("active owner stamp", encoding="utf-8")
    while not pathlib.Path(os.environ["MOCK_RELEASE_MARKER"]).exists():
        time.sleep(0.01)
''', encoding="utf-8")
        configure.chmod(0o755)
        self.environment = os.environ.copy()
        self.environment.update({
            "PATH": str(self.bin_dir) + os.pathsep + os.environ.get("PATH", ""),
            "CC": str(self.bin_dir / "cc"),
            "PKG_CONFIG": str(self.bin_dir / "pkg-config"),
            "MOCK_LOG": str(self.log),
            "MOCK_GIT_COMMIT": CommitA,
        })
        for name in ("CFLAGS", "CPPFLAGS", "LDFLAGS", "CXXFLAGS", "PYTHON", "MESON"):
            self.environment[name] = "-DPOISONED_OVERRIDE"

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def _tool(self, name: str, body: str) -> None:
        path = self.bin_dir / name
        path.write_text("#!" + sys.executable + "\n" + body, encoding="utf-8")
        path.chmod(0o755)

    def _invoke(self, *, expected: str = CommitA, build: Path | None = None) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(Helper), "--source", str(self.source), "--build-dir",
             str(build or self.build), "--expected-source-commit", expected, "--jobs", "2"],
            env=self.environment, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
        )

    def _calls(self) -> list[dict[str, object]]:
        if not self.log.exists():
            return []
        return [json.loads(line) for line in self.log.read_text(encoding="utf-8").splitlines()]

    def _stamp(self) -> dict[str, object]:
        return json.loads((self.build / ".waddle-qemu-build.json").read_text(encoding="utf-8"))

    def test_initial_build_and_identical_signature_reuse(self) -> None:
        first = self._invoke()
        self.assertEqual(first.returncode, 0, first.stderr)
        calls = self._calls()
        configure = next(call for call in calls if call["tool"] == "configure")
        args = configure["args"]
        self.assertEqual(args[-4:], ["--enable-pipewire", "--enable-slirp", "--enable-libusb", "--disable-download"])
        self.assertIn("--target-list=x86_64-softmmu", args)
        stamp = self._stamp()
        binary = self.build / "qemu-system-x86_64"
        self.assertEqual(stamp["binary_sha256"], hashlib.sha256(binary.read_bytes()).hexdigest())
        self.assertEqual(stamp["inputs"]["source_commit"], CommitA)
        self.assertEqual(stamp["inputs"]["environment_inputs"]["CC"], str(self.bin_dir / "cc"))
        self.assertEqual(set(configure["stripped"].values()), {None})

        second = self._invoke()
        self.assertEqual(second.returncode, 0, second.stderr)
        calls = self._calls()
        self.assertEqual(sum(call["tool"] == "configure" for call in calls), 1)
        self.assertEqual(sum(call["tool"] == "ninja" for call in calls), 2)
        self.assertIn("reusing validated configuration", second.stdout)

    def test_signature_inputs_reconfigure(self) -> None:
        self.assertEqual(self._invoke().returncode, 0)
        original = self._stamp()["signature"]
        self.environment["MOCK_PKG_CONFIG_VERSION"] = "2.0.0"
        changed = self._invoke()
        self.assertEqual(changed.returncode, 0, changed.stderr)
        self.assertNotEqual(self._stamp()["signature"], original)
        self.assertEqual(sum(call["tool"] == "configure" for call in self._calls()), 2)

    def test_compiler_identity_change_reconfigures(self) -> None:
        self.assertEqual(self._invoke().returncode, 0)
        original = self._stamp()["signature"]
        compiler = self.bin_dir / "cc"
        compiler.write_text("#!" + sys.executable + "\nprint('Mock C compiler version two')\n", encoding="utf-8")
        compiler.chmod(0o755)
        changed = self._invoke()
        self.assertEqual(changed.returncode, 0, changed.stderr)
        self.assertNotEqual(self._stamp()["signature"], original)
        self.assertEqual(sum(call["tool"] == "configure" for call in self._calls()), 2)

    def test_dependency_version_and_flags_change_reconfigure(self) -> None:
        self.assertEqual(self._invoke().returncode, 0)
        original = self._stamp()["signature"]
        self.environment["MOCK_LIBUSB_1_0_VERSION"] = "1.0.28"
        self.environment["MOCK_CFLAGS_PREFIX"] = "-I/mock-v2/"
        changed = self._invoke()
        self.assertEqual(changed.returncode, 0, changed.stderr)
        self.assertNotEqual(self._stamp()["signature"], original)
        self.assertEqual(sum(call["tool"] == "configure" for call in self._calls()), 2)

    def test_malformed_stamp_reconfigures(self) -> None:
        self.assertEqual(self._invoke().returncode, 0)
        (self.build / ".waddle-qemu-build.json").write_text("not json", encoding="utf-8")
        changed = self._invoke()
        self.assertNotEqual(changed.returncode, 0)
        self.assertIn("incomplete or missing provenance", changed.stderr)
        self.assertFalse((self.build / ".waddle-qemu-build.json").exists())
        self.assertEqual(sum(call["tool"] == "ninja" for call in self._calls()), 1)

    def test_incomplete_stamp_cannot_bless_tampered_binary(self) -> None:
        self.assertEqual(self._invoke().returncode, 0)
        binary = self.build / "qemu-system-x86_64"
        original_stat = binary.stat()
        binary.write_bytes(b"tampered while retaining the original mtime")
        os.utime(binary, ns=(original_stat.st_atime_ns, original_stat.st_mtime_ns))
        stamp = self._stamp()
        stamp.pop("binary")
        stamp.pop("binary_sha256")
        (self.build / ".waddle-qemu-build.json").write_text(json.dumps(stamp), encoding="utf-8")
        self.environment["MOCK_NINJA_KEEP_BINARY"] = "1"
        result = self._invoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("incomplete or missing provenance", result.stderr)
        self.assertFalse((self.build / ".waddle-qemu-build.json").exists())
        self.assertEqual(sum(call["tool"] == "ninja" for call in self._calls()), 1)

    def test_config_digest_change_forces_reconfigure(self) -> None:
        self.assertEqual(self._invoke().returncode, 0)
        config_mak = self.build / "config-host.mak"
        config_mak.write_text(config_mak.read_text(encoding="utf-8") + "# external edit\n", encoding="utf-8")
        rebuilt = self._invoke()
        self.assertEqual(rebuilt.returncode, 0, rebuilt.stderr)
        self.assertEqual(sum(call["tool"] == "configure" for call in self._calls()), 2)

    def test_tampered_binary_fails_closed_before_ninja(self) -> None:
        self.assertEqual(self._invoke().returncode, 0)
        (self.build / "qemu-system-x86_64").write_bytes(b"externally changed")
        result = self._invoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("binary hash changed", result.stderr)
        self.assertIn("fresh build directory", result.stderr)
        self.assertFalse((self.build / ".waddle-qemu-build.json").exists())
        self.assertEqual(sum(call["tool"] == "ninja" for call in self._calls()), 1)

    def test_same_directory_lock_is_nonblocking_and_preserves_owner_stamp(self) -> None:
        marker = self.root / "configure ready"
        release = self.root / "release configure"
        owner_env = self.environment.copy()
        owner_env["MOCK_CONFIGURE_BLOCK"] = "1"
        owner_env["MOCK_OWNER_MARKER"] = str(marker)
        owner_env["MOCK_RELEASE_MARKER"] = str(release)
        owner = subprocess.Popen(
            [sys.executable, str(Helper), "--source", str(self.source), "--build-dir", str(self.build),
             "--expected-source-commit", CommitA, "--jobs", "2"],
            env=owner_env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        try:
            deadline = time.monotonic() + 10
            while not marker.exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue(marker.exists(), "owner did not reach configure")
            owner_stamp = self.build / ".waddle-qemu-build.json"
            self.assertEqual(owner_stamp.read_text(encoding="utf-8"), "active owner stamp")
            contender = self._invoke()
            self.assertNotEqual(contender.returncode, 0)
            self.assertIn("busy", contender.stderr)
            self.assertEqual(owner_stamp.read_text(encoding="utf-8"), "active owner stamp")
        finally:
            release.touch()
        owner_out, owner_err = owner.communicate(timeout=10)
        self.assertEqual(owner.returncode, 0, owner_out + owner_err)
        self.assertEqual(self._stamp()["schema"], 1)

    def test_interruption_stops_and_reaps_owned_configure_process_group(self) -> None:
        marker = self.root / "configure ready for interrupt"
        release = self.root / "release interrupted configure"
        child_pid_file = self.root / "owned child pid"
        child_script = self.root / "owned child.py"
        child_ready = self.root / "owned child ready"
        child_cleanup = self.root / "owned child received term"
        owner_env = self.environment.copy()
        owner_env.update({
            "MOCK_CONFIGURE_BLOCK": "1",
            "MOCK_OWNER_MARKER": str(marker),
            "MOCK_RELEASE_MARKER": str(release),
            "MOCK_CHILD_PID": str(child_pid_file),
            "MOCK_CHILD_SCRIPT": str(child_script),
            "MOCK_CHILD_READY": str(child_ready),
            "MOCK_CHILD_CLEANUP": str(child_cleanup),
            "MOCK_CHILD_IGNORE_TERM": "1",
        })
        owner = subprocess.Popen(
            [sys.executable, str(Helper), "--source", str(self.source), "--build-dir", str(self.build),
             "--expected-source-commit", CommitA, "--jobs", "2"],
            env=owner_env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        try:
            deadline = time.monotonic() + 10
            while not child_pid_file.exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue(child_pid_file.exists(), "configure did not start owned child")
            owner.send_signal(signal.SIGINT)
            owner_out, owner_err = owner.communicate(timeout=10)
            self.assertEqual(owner.returncode, 130, owner_out + owner_err)
            deadline = time.monotonic() + 5
            while not child_cleanup.exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue(child_cleanup.exists(), "owned child did not receive termination")
            child_pid = int(child_pid_file.read_text(encoding="utf-8"))
            deadline = time.monotonic() + 5
            while _process_is_live(child_pid) and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertFalse(_process_is_live(child_pid), "TERM-ignoring owned child survived group escalation")
            self.assertFalse((self.build / ".waddle-qemu-build.json").exists())
        finally:
            if owner.poll() is None:
                release.touch()
                owner.kill()
                owner.communicate(timeout=10)

    def test_preflight_failure_removes_old_success_stamp(self) -> None:
        self.assertEqual(self._invoke().returncode, 0)
        self.environment["MOCK_PKG_CONFIG_FAIL"] = "1"
        result = self._invoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.build / ".waddle-qemu-build.json").exists())

    def test_source_change_during_build_removes_stamp(self) -> None:
        commit_file = self.root / "source commit"
        commit_file.write_text(CommitA, encoding="utf-8")
        self.environment["MOCK_GIT_COMMIT_FILE"] = str(commit_file)
        self.environment["MOCK_GIT_AFTER_BUILD"] = CommitB
        result = self._invoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("commit mismatch", result.stderr)
        self.assertFalse((self.build / ".waddle-qemu-build.json").exists())

    def test_missing_binary_and_missing_tool_leave_no_stamp(self) -> None:
        self.environment["MOCK_NINJA_NO_BINARY"] = "1"
        result = self._invoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("did not produce", result.stderr)
        self.assertFalse((self.build / ".waddle-qemu-build.json").exists())
        self.environment.pop("MOCK_NINJA_NO_BINARY")
        self.assertEqual(self._invoke().returncode, 0)
        (self.bin_dir / "pkg-config").unlink()
        result = self._invoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("required tool not found", result.stderr)
        self.assertFalse((self.build / ".waddle-qemu-build.json").exists())

    def test_source_commit_and_dirty_tree_are_rejected_before_configure(self) -> None:
        self.assertEqual(self._invoke().returncode, 0)
        result = self._invoke(expected=CommitB)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("commit mismatch", result.stderr)
        self.assertFalse((self.build / ".waddle-qemu-build.json").exists())
        self.assertEqual(sum(call["tool"] == "configure" for call in self._calls()), 1)
        (self.build / "qemu-system-x86_64").unlink()
        self.environment["MOCK_GIT_DIRTY"] = " M configure"
        result = self._invoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("tracked modifications", result.stderr)
        self.assertFalse((self.build / ".waddle-qemu-build.json").exists())

    def test_stale_config_or_missing_macro_reconfigures(self) -> None:
        self.assertEqual(self._invoke().returncode, 0)
        (self.build / "config-host.mak").unlink()
        self.assertEqual(self._invoke().returncode, 0)
        (self.build / "config-host.h").write_text("#define CONFIG_AUDIO_PIPEWIRE 1\n#define CONFIG_SLIRP 1\n", encoding="utf-8")
        self.environment["MOCK_CONFIG_MACROS"] = "#define CONFIG_AUDIO_PIPEWIRE 1\n#define CONFIG_SLIRP 1\n"
        failed = self._invoke()
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn("did not enable all required", failed.stderr)
        self.assertFalse((self.build / ".waddle-qemu-build.json").exists())
        self.assertEqual(sum(call["tool"] == "configure" for call in self._calls()), 3)

    def test_dependency_minimum_and_configure_failure_leave_no_stamp(self) -> None:
        self.environment["MOCK_LIBUSB_1_0_VERSION"] = "1.0.12"
        result = self._invoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("require >= 1.0.13", result.stderr)
        self.assertFalse(any(call["tool"] == "configure" for call in self._calls()))
        self.environment.pop("MOCK_LIBUSB_1_0_VERSION")
        self.environment["MOCK_CONFIGURE_FAIL"] = "1"
        result = self._invoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.build / ".waddle-qemu-build.json").exists())

    def test_ninja_failure_removes_old_success_stamp(self) -> None:
        self.assertEqual(self._invoke().returncode, 0)
        self.environment["MOCK_NINJA_FAIL"] = "1"
        result = self._invoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.build / ".waddle-qemu-build.json").exists())

    def test_build_directory_must_be_disjoint_from_source(self) -> None:
        result = self._invoke(build=self.source / "build")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must be disjoint", result.stderr)
        self.assertFalse(any(call["tool"] == "configure" for call in self._calls()))


if __name__ == "__main__":
    unittest.main()
