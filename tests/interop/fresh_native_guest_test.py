"""Isolated tests for fresh_native_guest_t; no VM or device is started."""

from __future__ import annotations

import hashlib
import io
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import xml.etree.ElementTree as ElementTree

import fresh_native_guest as fresh


class fresh_native_guest_tests_t(unittest.TestCase):
    """Exercise preparation behavior using private temporary artifacts only."""

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="fresh native guest ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.install_iso = self.root / "windows_server.iso"
        self.virtio_iso = self.root / "virtio_win.iso"
        self.install_iso.write_bytes(b"pinned windows evaluation fixture")
        self.virtio_iso.write_bytes(b"pinned virtio fixture")
        self.guest_exec = self.root / "waddle-guest-exec.exe"
        self.guest_exec.write_bytes(b"current guest executable")
        self.ovmf_code = self.root / "OVMF_CODE.fd"
        self.ovmf_vars = self.root / "OVMF_VARS.fd"
        self.ovmf_code.write_bytes(b"secure code vars fixture")
        self.ovmf_vars.write_bytes(b"fresh vars template")
        self.qemu_build = self.root / "qemu-build"
        self.qemu_build.mkdir()
        (self.qemu_build / "qemu-system-x86_64").write_bytes(b"pinned qemu fixture")
        (self.qemu_build / ".waddle-qemu-build.json").write_text("{}", encoding="ascii")
        for tool_name in ("qemu-img", "swtpm", "xorriso"):
            tool_path = self.root / tool_name
            tool_path.write_bytes(b"mock host tool")
            tool_path.chmod(0o700)
        self.run_parent = self.root / "build" / "fresh_native_guests"
        self.install_hash = hashlib.sha256(self.install_iso.read_bytes()).hexdigest()
        self.virtio_hash = hashlib.sha256(self.virtio_iso.read_bytes()).hexdigest()
        self.patches = [
            mock.patch.object(fresh, "WindowsServerIsoSha256", self.install_hash),
            mock.patch.object(fresh, "VirtioIsoSha256", self.virtio_hash),
            mock.patch.object(fresh.shutil, "which", side_effect=self._which),
        ]
        for patcher in self.patches:
            patcher.start()
            self.addCleanup(patcher.stop)

    def _which(self, name: str) -> str:
        return str(self.root / name)

    def _config(self, **kwargs: object) -> fresh.fresh_guest_config_t:
        values: dict[str, object] = {
            "workspace_root": self.root,
            "install_iso": self.install_iso,
            "virtio_iso": self.virtio_iso,
            "qemu_build_dir": self.qemu_build,
            "guest_exec": self.guest_exec,
            "run_parent": self.run_parent,
            "ovmf_code": self.ovmf_code,
            "ovmf_vars": self.ovmf_vars,
        }
        values.update(kwargs)
        return fresh.fresh_guest_config_t(**values)  # type: ignore[arg-type]

    def _qemu_validator(self, qemu: Path, stamp: Path) -> dict[str, object]:
        self.assertEqual(qemu, self.qemu_build / "qemu-system-x86_64")
        self.assertEqual(stamp, self.qemu_build / ".waddle-qemu-build.json")
        # Match native_guest_runner._validate_qemu: location is supplied by
        # the caller, while the validator returns provenance only.
        return {"version": "pinned-test-qemu", "sha256": "ab" * 32,
                "source_commit": "b2995afed254e4804611df52210067a4d5dbdf0b",
                "accelerators": "tcg", "stamp": str(stamp)}

    def test_validated_qemu_path_is_separate_from_real_provenance_shape(self) -> None:
        validated = fresh._validate_config(self._config(), qemu_validator=self._qemu_validator)
        self.assertEqual(validated["qemu_path"], str(self.qemu_build / "qemu-system-x86_64"))
        self.assertEqual(set(validated["qemu"]),
                         {"version", "sha256", "source_commit", "accelerators", "stamp"})

    def _preparation_run(self, *, fail_tool: str | None = None):
        def run(argv, **kwargs):
            self.assertIn(argv[0], {str(self.root / "qemu-img"), str(self.root / "xorriso")})
            self.assertTrue(kwargs["close_fds"])
            self.assertIs(kwargs["stdout"], subprocess.DEVNULL)
            self.assertIs(kwargs["stderr"], subprocess.DEVNULL)
            if argv[0].endswith("qemu-img"):
                self.assertEqual(argv[1:4], ["create", "-f", "qcow2"])
                Path(argv[-2]).write_bytes(b"new virtual blank disk")
                if fail_tool == "qemu-img":
                    return SimpleNamespace(returncode=12, stdout=b"private disk creation failed")
            else:
                output = Path(argv[argv.index("-o") + 1])
                if fail_tool == "xorriso":
                    return SimpleNamespace(returncode=4, stdout=b"ISO generation failure")
                output.write_bytes(b"private secret-bearing answer ISO")
            return SimpleNamespace(returncode=0, stdout=b"command completed")
        return run

    def test_prepare_writes_private_recoverable_nonsecret_manifest(self) -> None:
        secret = "Fresh-Test-Only-Password_0123456789"
        administrator_secret = "Fresh-Admin-Only-Password_9876543210"
        manifest = fresh.prepare_fresh_guest(
            self._config(), run=self._preparation_run(), password_factory=lambda: secret,
            administrator_password_factory=lambda: administrator_secret,
            qemu_validator=self._qemu_validator, clock=lambda: 1792800000.0)
        run_root = Path(manifest["run_root"])
        self.assertEqual(stat.S_IMODE(run_root.stat().st_mode), 0o700)
        self.assertEqual(stat.S_IMODE((run_root / "test-account.secret").stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE((run_root / "administrator.secret").stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE((run_root / "Autounattend.xml").stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE((run_root / "answer.iso").stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE((run_root / "run-manifest.json").stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE((run_root / "vars.fd").stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE((run_root / "tpm").stat().st_mode), 0o700)
        self.assertEqual((run_root / "test-account.secret").read_text(encoding="utf-8"), secret + "\n")
        self.assertEqual((run_root / "administrator.secret").read_text(encoding="utf-8"),
                         administrator_secret + "\n")
        manifest_text = (run_root / "run-manifest.json").read_text(encoding="utf-8")
        for password in (secret, administrator_secret):
            self.assertNotIn(password, manifest_text)
            self.assertNotIn(password, json.dumps(manifest))
        for password in (secret, administrator_secret):
            self.assertNotIn(password, (run_root / "qemu-img.log").read_text(encoding="utf-8"))
            self.assertNotIn(password, (run_root / "xorriso.log").read_text(encoding="utf-8"))
        self.assertEqual(manifest["guest_user_group"], "Users")
        self.assertFalse(manifest["guest_booted"])
        self.assertFalse(manifest["guest_vsock_listener_verified"])
        self.assertFalse(manifest["native_acceptance_credit"])
        self.assertFalse(manifest["administrator_password_in_manifest"])
        self.assertFalse(manifest["administrator_password_in_logs"])
        self.assertFalse(manifest["administrator_password_in_argv"])
        self.assertFalse(manifest["answer_iso_sha256_recorded"])
        self.assertTrue(manifest["virtual_disk"]["backing_file"] is None)
        self.assertEqual(manifest["virtual_disk"]["virtual_size_bytes"], 128 * 1024**3)
        self.assertFalse((run_root / "answer-stage").exists())

        answer = (run_root / "Autounattend.xml").read_bytes()
        self.assertIn(secret.encode(), answer)
        tree = ElementTree.fromstring(answer)
        namespace = {"u": fresh.UnattendNs}
        account = tree.find(".//u:LocalAccount", namespace)
        self.assertIsNotNone(account)
        assert account is not None
        self.assertEqual(account.findtext("u:Name", namespaces=namespace), "waddle_native")
        self.assertEqual(account.findtext("u:Group", namespaces=namespace), "Users")
        self.assertEqual(account.findtext("u:Password/u:Value", namespaces=namespace), secret)
        self.assertNotEqual(account.findtext("u:Password/u:Value", namespaces=namespace),
                            administrator_secret)
        self.assertEqual(tree.findtext(".//u:UserAccounts/u:AdministratorPassword/u:Value",
                                       namespaces=namespace), administrator_secret)
        self.assertEqual(tree.findtext(".//u:UserAccounts/u:AdministratorPassword/u:PlainText",
                                       namespaces=namespace), "true")
        self.assertEqual(tree.findtext(".//u:AutoLogon/u:LogonCount", namespaces=namespace), "2")
        self.assertEqual(tree.findtext(".//u:AutoLogon/u:Username", namespaces=namespace), "waddle_native")
        disk = tree.find(".//u:DiskConfiguration/u:Disk", namespace)
        self.assertIsNotNone(disk)
        assert disk is not None
        self.assertEqual(disk.findtext("u:DiskID", namespaces=namespace), "0")
        self.assertEqual(disk.findtext("u:WillWipeDisk", namespaces=namespace), "true")
        created = disk.findall("u:CreatePartitions/u:CreatePartition", namespace)
        self.assertEqual([(item.findtext("u:Order", namespaces=namespace),
                           item.findtext("u:Type", namespaces=namespace),
                           item.findtext("u:Size", namespaces=namespace),
                           item.findtext("u:Extend", namespaces=namespace)) for item in created],
                         [("1", "EFI", "200", None), ("2", "MSR", "16", None),
                          ("3", "Primary", None, "true")])
        modified = disk.findall("u:ModifyPartitions/u:ModifyPartition", namespace)
        self.assertEqual([(item.findtext("u:PartitionID", namespaces=namespace),
                           item.findtext("u:Format", namespaces=namespace)) for item in modified],
                         [("1", "FAT32"), ("3", "NTFS")])
        self.assertEqual(tree.findtext(".//u:InstallTo/u:DiskID", namespaces=namespace), "0")
        self.assertEqual(tree.findtext(".//u:InstallTo/u:PartitionID", namespaces=namespace), "3")
        self.assertEqual(manifest["selected_image_name"],
                         "Windows Server 2025 SERVERSTANDARD")
        self.assertEqual(tree.findtext(".//u:InstallFrom/u:MetaData/u:Value",
                                       namespaces=namespace), "Windows Server 2025 SERVERSTANDARD")
        command = manifest["qemu_argv_template"]
        self.assertIn("q35,accel=kvm", command)
        self.assertIn("once=d,order=c,menu=on", command)
        self.assertIn("-nic", command)
        self.assertIn("none", command)
        self.assertEqual(command[command.index("-display") + 1], "none")
        self.assertIn("vhost-vsock-pci,guest-cid=52942,vhostfd=CALLER_VALIDATED_VHOST_VSOCK_FD",
                      command)
        self.assertFalse(any("-netdev" in item or "-usb" in item or "waddle_export" in item
                             for item in command))

    def test_wrong_pinned_media_fails_before_output_creation(self) -> None:
        bad = self.root / "bad.iso"
        bad.write_bytes(b"not pinned")
        with self.assertRaisesRegex(fresh.fresh_guest_error_t, "does not match the pinned"):
            fresh.prepare_fresh_guest(self._config(install_iso=bad),
                                      run=self._preparation_run(), qemu_validator=self._qemu_validator)
        self.assertFalse(self.run_parent.exists())

    def test_symlink_media_and_symlink_output_ancestor_are_rejected(self) -> None:
        link = self.root / "virtio-link.iso"
        link.symlink_to(self.virtio_iso)
        with self.assertRaisesRegex(fresh.fresh_guest_error_t, "must not be a symlink"):
            fresh.prepare_fresh_guest(self._config(virtio_iso=link),
                                      run=self._preparation_run(), qemu_validator=self._qemu_validator)
        build_link = self.root / "build"
        build_link.symlink_to(self.root / "elsewhere", target_is_directory=True)
        with self.assertRaisesRegex(fresh.fresh_guest_error_t, "contains a symlink"):
            fresh.prepare_fresh_guest(self._config(), run=self._preparation_run(),
                                      qemu_validator=self._qemu_validator)
        self.assertFalse((self.root / "elsewhere").exists())

    def test_run_parent_outside_workspace_is_rejected(self) -> None:
        outside = Path(self.temporary.name).parent / f"not-fresh-{Path(self.temporary.name).name}"
        with self.assertRaisesRegex(fresh.fresh_guest_error_t, "must stay under"):
            fresh.prepare_fresh_guest(self._config(run_parent=outside),
                                      run=self._preparation_run(), qemu_validator=self._qemu_validator)
        self.assertFalse(outside.exists())

    def test_comma_in_keyval_path_and_nonprivate_existing_parent_are_rejected(self) -> None:
        self.run_parent.mkdir(parents=True, mode=0o700)
        comma_dir = self.run_parent / "fresh,guests"
        comma_dir.mkdir(mode=0o700)
        with self.assertRaisesRegex(fresh.fresh_guest_error_t, "comma or control"):
            fresh.prepare_fresh_guest(self._config(run_parent=comma_dir),
                                      run=self._preparation_run(), qemu_validator=self._qemu_validator)
        self.assertFalse(any(comma_dir.iterdir()))
        self.run_parent.chmod(0o755)
        with self.assertRaisesRegex(fresh.fresh_guest_error_t, "mode 0700"):
            fresh.prepare_fresh_guest(self._config(), run=self._preparation_run(),
                                      qemu_validator=self._qemu_validator)
        self.assertEqual(list(self.run_parent.iterdir()), [comma_dir])

    def test_qemu_img_failure_retains_private_run_and_redacts_secret(self) -> None:
        secret = "Fresh-Test-Only-Password_0123456789"
        with self.assertRaisesRegex(fresh.fresh_guest_error_t, "details suppressed"):
            fresh.prepare_fresh_guest(self._config(), run=self._preparation_run(fail_tool="qemu-img"),
                                      password_factory=lambda: secret,
                                      qemu_validator=self._qemu_validator)
        runs = list(self.run_parent.iterdir())
        self.assertEqual(len(runs), 1)
        self.assertEqual(stat.S_IMODE(runs[0].stat().st_mode), 0o700)
        manifest = (runs[0] / "run-manifest.json").read_text(encoding="utf-8")
        self.assertNotIn(secret, manifest)
        self.assertIn('"retained": true', manifest)
        self.assertTrue((runs[0] / "windows.qcow2").exists())

    def test_xorriso_failure_retains_answer_stage_and_no_secret_logs(self) -> None:
        secret = "Fresh-Test-Only-Password_0123456789"
        with self.assertRaisesRegex(fresh.fresh_guest_error_t, "details suppressed"):
            fresh.prepare_fresh_guest(self._config(), run=self._preparation_run(fail_tool="xorriso"),
                                      password_factory=lambda: secret,
                                      qemu_validator=self._qemu_validator)
        run_root = next(self.run_parent.iterdir())
        self.assertTrue((run_root / "answer-stage" / "Autounattend.xml").exists())
        self.assertNotIn(secret, (run_root / "xorriso.log").read_text(encoding="utf-8"))
        self.assertNotIn(secret, (run_root / "run-manifest.json").read_text(encoding="utf-8"))

    def test_injected_tool_exception_is_redacted_from_manifest_and_cli_error(self) -> None:
        secret = "User<Secret>&0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        administrator_secret = "Admin&Secret<0123456789abcdefghijkLMNOPQ"

        def run(argv, **_kwargs):
            if argv[0].endswith("qemu-img"):
                Path(argv[-2]).write_bytes(b"new virtual blank disk")
                raise RuntimeError(
                    "unsafe external diagnostic " + fresh.html.escape(secret, quote=True) + " " +
                    fresh.html.escape(administrator_secret, quote=True))
            return SimpleNamespace(returncode=0, stdout=b"")

        with self.assertRaises(fresh.fresh_guest_error_t) as caught:
            fresh.prepare_fresh_guest(
                self._config(), run=run, password_factory=lambda: secret,
                administrator_password_factory=lambda: administrator_secret,
                                      qemu_validator=self._qemu_validator)
        for password in (secret, administrator_secret):
            for variant in fresh._secret_variants(password):
                self.assertNotIn(variant, str(caught.exception))
        run_root = next(self.run_parent.iterdir())
        manifest_text = (run_root / "run-manifest.json").read_text(encoding="utf-8")
        for password in (secret, administrator_secret):
            for variant in fresh._secret_variants(password):
                self.assertNotIn(variant, manifest_text)
        qemu_log = run_root / "qemu-img.log"
        self.assertFalse(qemu_log.exists())

        stderr = io.StringIO()
        with mock.patch.object(fresh, "prepare_fresh_guest",
                               side_effect=fresh.fresh_guest_error_t(
                                   secret + " " + administrator_secret)), \
                mock.patch.object(fresh.sys, "stderr", stderr):
            self.assertEqual(fresh.main(["prepare"]), 2)
        for password in (secret, administrator_secret):
            for variant in fresh._secret_variants(password):
                self.assertNotIn(variant, stderr.getvalue())
        self.assertEqual(stderr.getvalue(), "fresh guest preparation refused; details suppressed\n")

    def test_staged_guest_executable_hash_is_checked_after_copy(self) -> None:
        real_copyfile = fresh.shutil.copyfile

        def tampering_copyfile(source, destination, *args, **kwargs):
            result = real_copyfile(source, destination, *args, **kwargs)
            if Path(destination).name == "waddle-guest-exec.exe":
                Path(destination).write_bytes(b"changed after validated source copy")
            return result

        with mock.patch.object(fresh.shutil, "copyfile", side_effect=tampering_copyfile):
            with self.assertRaisesRegex(fresh.fresh_guest_error_t, "details suppressed"):
                fresh.prepare_fresh_guest(
                    self._config(), run=self._preparation_run(),
                    password_factory=lambda: "Fresh-Test-Only-Password_0123456789",
                    administrator_password_factory=lambda: "Fresh-Admin-Only-Password_9876543210",
                    qemu_validator=self._qemu_validator)
        run_root = next(self.run_parent.iterdir())
        staged = run_root / "answer-stage" / "waddle-guest-exec.exe"
        self.assertEqual(staged.read_bytes(), b"changed after validated source copy")
        manifest = json.loads((run_root / "run-manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["failure"]["type"], "fresh_guest_error_t")
        self.assertEqual(manifest["failure"]["message"], "details suppressed")

    def test_guest_setup_script_uses_pinned_viosock_and_nonadmin_run_entry(self) -> None:
        script = fresh._guest_setup_script().decode("utf-8")
        self.assertIn("viosock\\2k25\\amd64", script)
        self.assertIn("pnputil.exe /add-driver", script)
        self.assertIn("HKLM:\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", script)
        self.assertIn("--vsock-port 5242", script)
        for prohibited in ("WinRM", "New-NetFirewallRule", "Enable-PSRemoting", "netsh interface",
                           "\\waddle_export", "Set-Clipboard", "Enable-Clipboard"):
            self.assertNotIn(prohibited, script)

    def test_cid_and_password_boundaries_reject_before_preparation(self) -> None:
        for cid in (False, 0, 1, 2, fresh.VsockMaximumCid + 1):
            with self.subTest(cid=cid), self.assertRaises(fresh.fresh_guest_error_t):
                fresh.prepare_fresh_guest(self._config(guest_cid=cid),
                                          run=self._preparation_run(), qemu_validator=self._qemu_validator)
        self.assertFalse(self.run_parent.exists())
        with self.assertRaisesRegex(fresh.fresh_guest_error_t, "details suppressed"):
            fresh.prepare_fresh_guest(self._config(), run=self._preparation_run(),
                                      password_factory=lambda: "short",
                                      qemu_validator=self._qemu_validator)
        self.assertEqual(len(list(self.run_parent.iterdir())), 1)
        manifest = (next(self.run_parent.iterdir()) / "run-manifest.json").read_text(encoding="utf-8")
        self.assertNotIn("short", manifest)

    def test_generated_password_has_windows_complexity_classes(self) -> None:
        password = fresh._new_test_password()
        self.assertEqual(len(password), 32)
        self.assertTrue(any(character.isupper() for character in password))
        self.assertTrue(any(character.islower() for character in password))
        self.assertTrue(any(character.isdigit() for character in password))
        self.assertTrue(any(character in "!#$%&()*+,-./:;=?@[]^_{|}~" for character in password))

    def test_configuration_rejects_bool_and_noninteger_resource_bounds(self) -> None:
        cases = ({"memory_mib": True}, {"memory_mib": 4096.0},
                 {"cpus": True}, {"cpus": 2.5}, {"preparation_timeout_seconds": True},
                 {"preparation_timeout_seconds": 1.5})
        for values in cases:
            with self.subTest(values=values), self.assertRaises(fresh.fresh_guest_error_t):
                fresh.prepare_fresh_guest(self._config(**values), run=self._preparation_run(),
                                          qemu_validator=self._qemu_validator)
        self.assertFalse(self.run_parent.exists())

    def test_command_output_is_discarded_instead_of_logged(self) -> None:
        log_path = self.root / "bounded-command.log"
        secret = "should-never-be-captured"
        fresh._run_command([sys.executable, "-c", f"import sys; print({secret!r}); print({secret!r}, file=sys.stderr)"],
                           timeout=5, log_path=log_path, run=subprocess.run)
        log = log_path.read_text(encoding="utf-8")
        self.assertNotIn(secret, log)
        self.assertEqual(log, "preparation command exit status: 0\n")

    def test_private_argv_builder_requires_valid_descriptor_shape_and_has_no_network(self) -> None:
        root = self.root / "private-run"
        for bad_fd in (True, -1):
            with self.subTest(fd=bad_fd), self.assertRaises(fresh.fresh_guest_error_t):
                fresh._qemu_argv(root, self.qemu_build / "qemu-system-x86_64", guest_cid=52942,
                                 vhost_fd=bad_fd, ovmf_code=self.ovmf_code,
                                 install_iso=self.install_iso, virtio_iso=self.virtio_iso,
                                 memory_mib=4096, cpus=2)
        argv = fresh._qemu_argv(root, self.qemu_build / "qemu-system-x86_64", guest_cid=52942,
                                vhost_fd=17, ovmf_code=self.ovmf_code, install_iso=self.install_iso,
                                virtio_iso=self.virtio_iso, memory_mib=4096, cpus=2)
        self.assertIn("guest-cid=52942,vhostfd=17", " ".join(argv))
        self.assertEqual(argv.count("-nic"), 1)
        self.assertEqual(argv[argv.index("-nic") + 1], "none")
        self.assertNotIn("-netdev", argv)
        self.assertNotIn("-usb", argv)


if __name__ == "__main__":
    unittest.main()
