"""Isolated contract tests for scripts/vendor_copy.py."""

from __future__ import annotations

import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


Root = Path(__file__).resolve().parents[2]
Helper = Root / "scripts" / "vendor_copy.py"
Spec = importlib.util.spec_from_file_location("vendor_copy", Helper)
assert Spec is not None and Spec.loader is not None
vendor_copy = importlib.util.module_from_spec(Spec)
sys.modules[Spec.name] = vendor_copy
Spec.loader.exec_module(vendor_copy)
Commit = "a" * 40
RequiredConfigs = ["CONFIG_AUDIO_PIPEWIRE", "CONFIG_SLIRP", "CONFIG_USB_LIBUSB"]


class vendor_copy_tests_t(unittest.TestCase):
    # unittest requires these two camel-case lifecycle hook names.
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="vendor copy test ")
        self.root = Path(self.temporary.name)
        self.source_dir = self.root / "source"
        self.package_dir = self.root / "package"
        self.source_dir.mkdir()
        self.package_dir.mkdir()
        self.qemu = self.source_dir / "qemu-system-x86_64"
        self.virtiofsd = self.source_dir / "virtiofsd"
        self.stamp = self.root / "qemu build stamp.json"
        self.destination = self.package_dir / "qemu-system-x86_64"
        self.identity = self.package_dir / "qemu_identity.json"
        self._executable(self.qemu, """import sys
if '--version' in sys.argv:
    print('QEMU emulator version 9.2.4')
else:
    print('mock qemu')
""")
        self._executable(self.virtiofsd, """import sys
if '--version' in sys.argv:
    print('virtiofsd 1.13.2')
elif '--help' in sys.argv:
    print('Usage: virtiofsd --shared-dir PATH --socket-path PATH')
    print('      --readonly Prevent guest writes')
else:
    raise SystemExit(2)
""")
        self._write_stamp(self.qemu)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def _executable(self, path: Path, body: str) -> None:
        path.write_text("#!" + sys.executable + "\n" + body, encoding="utf-8")
        path.chmod(0o755)

    def _write_stamp(self, binary: Path) -> dict[str, object]:
        config_files = {
            "config-host.mak": b"# QEMU fixture config\n",
            "config-host.h": b"#define CONFIG_AUDIO_PIPEWIRE 1\n#define CONFIG_SLIRP 1\n#define CONFIG_USB_LIBUSB 1\n",
        }
        for name, content in config_files.items():
            (self.stamp.parent / name).write_bytes(content)
        inputs: dict[str, object] = {
            "schema": 1,
            "source_commit": Commit,
            "configure_argv": [
                "/tmp/qemu/configure", "--enable-pipewire", "--enable-slirp",
                "--enable-libusb", "--disable-download",
            ],
        }
        stamp = {
            "schema": 1,
            "signature": hashlib.sha256(json.dumps(inputs, sort_keys=True, separators=(",", ":")).encode()).hexdigest(),
            "inputs": inputs,
            "enabled_config_macros": RequiredConfigs,
            "configuration_sha256": {name: hashlib.sha256(content).hexdigest() for name, content in config_files.items()},
            "binary": "qemu-system-x86_64",
            "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        }
        self.stamp.write_text(json.dumps(stamp), encoding="utf-8")
        return stamp

    def _copy(self, kind: str, source_kind: str, source: Path, *, identity: Path | None = None,
              provenance: Path | None = None) -> dict[str, object]:
        return vendor_copy.copy_package(
            kind, source_kind, source, self.destination,
            identity or self.identity, provenance,
        )

    def _verify(self, kind: str = "qemu") -> dict[str, object]:
        return vendor_copy.verify_package(kind, self.destination, self.identity)

    def test_pinned_qemu_copy_and_verify_records_gitlink_and_build_hash(self) -> None:
        identity = self._copy("qemu", "pinned-source", self.qemu, provenance=self.stamp)
        self.assertEqual(self.destination.read_bytes(), self.qemu.read_bytes())
        self.assertEqual(identity["source_kind"], "pinned-source")
        self.assertEqual(identity["source_commit"], Commit)
        self.assertEqual(identity["source_artifact_sha256"], hashlib.sha256(self.qemu.read_bytes()).hexdigest())
        self.assertEqual(identity["build_signature"], json.loads(self.stamp.read_text())["signature"])
        self.assertEqual(self._verify()["kind"], "qemu")

    def test_system_qemu_replaces_old_pinned_identity_with_system_identity(self) -> None:
        self._copy("qemu", "pinned-source", self.qemu, provenance=self.stamp)
        self._executable(self.qemu, """import sys
if '--version' in sys.argv: print('QEMU system version 10.0')
""")
        identity = self._copy("qemu", "system", self.qemu)
        self.assertEqual(identity["source_kind"], "system")
        self.assertEqual(identity["source_path"], str(self.qemu.resolve()))
        self.assertNotIn("source_commit", identity)
        self.assertNotIn("build_signature", identity)
        self.assertEqual(self._verify()["source_kind"], "system")

    def test_virtiofsd_requires_and_records_readonly_advertisement(self) -> None:
        destination = self.package_dir / "virtiofsd"
        identity_path = self.package_dir / "virtiofsd_identity.json"
        value = vendor_copy.copy_package("virtiofsd", "system", self.virtiofsd, destination, identity_path)
        self.assertEqual(value["version"], "virtiofsd 1.13.2")
        self.assertEqual(value["readonly_capability"], "advertised")
        self.assertEqual(vendor_copy.verify_package("virtiofsd", destination, identity_path)["kind"], "virtiofsd")

    def test_verify_rejects_incomplete_virtiofsd_and_inconsistent_system_identity(self) -> None:
        vfs_destination = self.package_dir / "virtiofsd"
        vfs_identity = self.package_dir / "virtiofsd_identity.json"
        vendor_copy.copy_package("virtiofsd", "system", self.virtiofsd, vfs_destination, vfs_identity)
        value = json.loads(vfs_identity.read_text(encoding="utf-8"))
        value.pop("readonly_capability")
        value.pop("readonly_probe_sha256")
        vfs_identity.write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaisesRegex(vendor_copy.build_error_t, "read-only capability evidence"):
            vendor_copy.verify_package("virtiofsd", vfs_destination, vfs_identity)

        self._copy("qemu", "system", self.qemu)
        for mutation, message in (
            ({"source_commit": Commit, "build_signature": "b" * 64}, "pinned-source provenance fields"),
            ({"package": "virtiofsd"}, "kind and package fields"),
            ({"source_path": "relative/qemu"}, "source path must be absolute"),
        ):
            value = json.loads(self.identity.read_text(encoding="utf-8"))
            value.update(mutation)
            self.identity.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(vendor_copy.build_error_t, message):
                self._verify("qemu")
            self._copy("qemu", "system", self.qemu)

    def test_nonregular_identity_lock_is_rejected(self) -> None:
        lock_path = self.package_dir / ".qemu_identity.json.lock"
        os.mkfifo(lock_path)
        with self.assertRaisesRegex(vendor_copy.build_error_t, "lock is not a regular file"):
            self._copy("qemu", "system", self.qemu)
        self.assertFalse(self.destination.exists())
        self.assertFalse(self.identity.exists())

    def test_malformed_stamp_and_binary_hash_mismatch_preserve_existing_package(self) -> None:
        self._copy("qemu", "system", self.qemu)
        old_binary = self.destination.read_bytes()
        old_identity = self.identity.read_bytes()
        self.stamp.write_text("not json", encoding="utf-8")
        with self.assertRaises(vendor_copy.build_error_t):
            self._copy("qemu", "pinned-source", self.qemu, provenance=self.stamp)
        self._write_stamp(self.qemu)
        self.qemu.write_bytes(self.qemu.read_bytes() + b"mutated")
        with self.assertRaises(vendor_copy.build_error_t):
            self._copy("qemu", "pinned-source", self.qemu, provenance=self.stamp)
        self.assertEqual(self.destination.read_bytes(), old_binary)
        self.assertEqual(self.identity.read_bytes(), old_identity)
        self.assertEqual(self._verify()["source_kind"], "system")

    def test_stamp_signature_and_configuration_claims_are_validated(self) -> None:
        stamp = self._write_stamp(self.qemu)
        stamp["inputs"]["source_commit"] = "d" * 40
        self.stamp.write_text(json.dumps(stamp), encoding="utf-8")
        with self.assertRaisesRegex(vendor_copy.build_error_t, "signature"):
            self._copy("qemu", "pinned-source", self.qemu, provenance=self.stamp)
        stamp = self._write_stamp(self.qemu)
        stamp["enabled_config_macros"] = RequiredConfigs[:-1]
        self.stamp.write_text(json.dumps(stamp), encoding="utf-8")
        with self.assertRaisesRegex(vendor_copy.build_error_t, "macros"):
            self._copy("qemu", "pinned-source", self.qemu, provenance=self.stamp)
        stamp = self._write_stamp(self.qemu)
        (self.stamp.parent / "config-host.h").write_text("#define CONFIG_SLIRP 1\n", encoding="utf-8")
        with self.assertRaisesRegex(vendor_copy.build_error_t, "digest does not match"):
            self._copy("qemu", "pinned-source", self.qemu, provenance=self.stamp)

    def test_missing_readonly_and_failed_probe_leave_existing_package_unchanged(self) -> None:
        self._copy("qemu", "system", self.qemu)
        old_binary = self.destination.read_bytes()
        old_identity = self.identity.read_bytes()
        self._executable(self.virtiofsd, """import sys
if '--version' in sys.argv: print('virtiofsd 1.2.0')
elif '--help' in sys.argv: print('Usage: virtiofsd')
""")
        vfs_dest = self.package_dir / "virtiofsd"
        vfs_identity = self.package_dir / "virtiofsd_identity.json"
        with self.assertRaisesRegex(vendor_copy.build_error_t, "does not advertise --readonly"):
            vendor_copy.copy_package("virtiofsd", "system", self.virtiofsd, vfs_dest, vfs_identity)
        self._executable(self.virtiofsd, """import sys
if '--version' in sys.argv: raise SystemExit(9)
elif '--help' in sys.argv: print('--readonly')
""")
        with self.assertRaisesRegex(vendor_copy.build_error_t, "probe failed"):
            vendor_copy.copy_package("virtiofsd", "system", self.virtiofsd, vfs_dest, vfs_identity)
        self.assertEqual(self.destination.read_bytes(), old_binary)
        self.assertEqual(self.identity.read_bytes(), old_identity)
        self.assertFalse(vfs_dest.exists())
        self.assertFalse(vfs_identity.exists())

    def test_refuses_symlink_destination_source_collision_and_wrong_package_mode(self) -> None:
        link_target = self.package_dir / "protected file"
        link_target.write_text("keep", encoding="utf-8")
        self.destination.symlink_to(link_target)
        with self.assertRaisesRegex(vendor_copy.build_error_t, "symlink"):
            self._copy("qemu", "system", self.qemu)
        self.assertEqual(link_target.read_text(encoding="utf-8"), "keep")
        self.destination.unlink()
        with self.assertRaisesRegex(vendor_copy.build_error_t, "distinct"):
            vendor_copy.copy_package("qemu", "system", self.qemu, self.qemu, self.identity)
        with self.assertRaisesRegex(vendor_copy.build_error_t, "pinned-source packaging is unsupported"):
            self._copy("virtiofsd", "pinned-source", self.virtiofsd, provenance=self.stamp)

    def test_publish_interruption_removes_owned_temps_and_verification_rejects_mixed_pair(self) -> None:
        self._copy("qemu", "system", self.qemu)
        old_identity = self.identity.read_bytes()
        self._executable(self.qemu, """import sys
if '--version' in sys.argv: print('QEMU emulator version 10.0')
""")
        real_replace = os.replace
        calls = 0

        def interrupt_second_rename(source: os.PathLike[str], target: os.PathLike[str]) -> None:
            nonlocal calls
            calls += 1
            if calls == 2:
                raise KeyboardInterrupt("simulated interruption between pair renames")
            real_replace(source, target)

        with mock.patch.object(vendor_copy.os, "replace", side_effect=interrupt_second_rename):
            with self.assertRaises(KeyboardInterrupt):
                self._copy("qemu", "system", self.qemu)
        self.assertNotEqual(self.destination.read_bytes(), b"")
        self.assertEqual(self.identity.read_bytes(), old_identity)
        with self.assertRaises(vendor_copy.build_error_t):
            self._verify()
        self.assertEqual(list(self.package_dir.glob(".qemu-system-x86_64.vendor-*.tmp")), [])
        self.assertEqual(list(self.package_dir.glob(".qemu_identity.json.vendor-*.tmp")), [])

    def test_nonblocking_identity_lock_rejects_concurrent_publisher(self) -> None:
        self._copy("qemu", "system", self.qemu)
        old_binary = self.destination.read_bytes()
        lock_path = self.package_dir / ".qemu_identity.json.lock"
        descriptor = os.open(lock_path, os.O_CREAT | os.O_RDWR, 0o600)
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaisesRegex(vendor_copy.build_error_t, "busy"):
                self._copy("qemu", "system", self.qemu)
        finally:
            os.close(descriptor)
        self.assertEqual(self.destination.read_bytes(), old_binary)
        self.assertEqual(self._verify()["kind"], "qemu")

    def test_cli_copy_and_verify(self) -> None:
        copied = subprocess.run(
            [sys.executable, str(Helper), "copy", "--kind", "qemu", "--source-kind", "system",
             "--source", str(self.qemu), "--destination", str(self.destination),
             "--identity", str(self.identity)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
        )
        self.assertEqual(copied.returncode, 0, copied.stderr)
        verified = subprocess.run(
            [sys.executable, str(Helper), "verify", "--kind", "qemu", "--destination", str(self.destination),
             "--identity", str(self.identity)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
        )
        self.assertEqual(verified.returncode, 0, verified.stderr)
        self.assertEqual(json.loads(verified.stdout)["status"], "verified")

    def test_oversized_probe_output_fails_before_replacing_package(self) -> None:
        self._copy("qemu", "system", self.qemu)
        old_binary = self.destination.read_bytes()
        old_identity = self.identity.read_bytes()
        self._executable(self.qemu, """import sys
if '--version' in sys.argv: print('v' * 70000)
""")
        with self.assertRaisesRegex(vendor_copy.build_error_t, "output exceeded"):
            self._copy("qemu", "system", self.qemu)
        self.assertEqual(self.destination.read_bytes(), old_binary)
        self.assertEqual(self.identity.read_bytes(), old_identity)

    def test_verify_rejects_tampered_binary_and_identity_symlink(self) -> None:
        self._copy("qemu", "system", self.qemu)
        self.destination.write_bytes(b"tampered")
        with self.assertRaisesRegex(vendor_copy.build_error_t, "does not match"):
            self._verify()
        self.identity.unlink()
        self.identity.symlink_to(self.stamp)
        with self.assertRaisesRegex(vendor_copy.build_error_t, "regular file"):
            self._verify()


if __name__ == "__main__":
    unittest.main()
