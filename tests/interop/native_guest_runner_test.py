"""Isolated tests for native_guest_runner_t; they never start a VM or swtpm."""

from __future__ import annotations

import hashlib
import fcntl
import json
import os
from pathlib import Path
import signal
import stat
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

from native_guest_runner import (
    MaximumGuestVsockCid,
    MinimumGuestVsockCid,
    VhostVsockMajor,
    VhostVsockMinor,
    _check_no_writable_base_holder,
    _duplicate_vhost_vsock_fd,
    _protected_snapshot,
    _reject_external_qcow2_storage,
    _run_bounded,
    _validate_guest_vsock_cid,
    _validate_qemu,
    base_read_lease_t,
    native_guest_runner_t,
    qmp_client_t,
    result_channel_t,
    runner_config_t,
    runner_error_t,
)


class fake_process_t:
    def __init__(self, returncode: int | None = None) -> None:
        self.returncode = returncode
        self.terminated = False
        self.killed = False
        self.wait_calls = 0

    def poll(self) -> int | None:
        return self.returncode

    def terminate(self) -> None:
        self.terminated = True
        self.returncode = 0

    def kill(self) -> None:
        self.killed = True
        self.returncode = -9

    def wait(self, timeout: float | None = None) -> int:
        self.wait_calls += 1
        return self.returncode or 0


class slow_process_t(fake_process_t):
    def wait(self, timeout: float | None = None) -> int:
        self.wait_calls += 1
        if not self.killed:
            raise subprocess.TimeoutExpired("owned-child", timeout)
        self.returncode = -9
        return self.returncode


class fake_connection_t:
    def __init__(self, payload: bytes) -> None:
        self.payload = payload

    def __enter__(self) -> fake_connection_t:
        return self

    def __exit__(self, *_args: object) -> None:
        return None

    def settimeout(self, _timeout: float) -> None:
        return None

    def recv(self, limit: int) -> bytes:
        block = self.payload[:limit]
        self.payload = self.payload[len(block):]
        return block


class fake_listener_t:
    def __init__(self, payload: bytes) -> None:
        self.payload = payload
        self.path: str | None = None

    def bind(self, path: str) -> None:
        self.path = path
        Path(path).touch()

    def listen(self, _count: int) -> None:
        return None

    def settimeout(self, _timeout: float) -> None:
        return None

    def accept(self) -> tuple[fake_connection_t, None]:
        return fake_connection_t(self.payload), None

    def close(self) -> None:
        return None


class fake_qmp_socket_t:
    def __init__(self, *, fail_recv: bool = False, event_stream: bool = False) -> None:
        self.fail_recv = fail_recv
        self.event_stream = event_stream
        self.closed = False
        self.sent: list[bytes] = []

    def settimeout(self, _timeout: float) -> None:
        return None

    def connect(self, _path: str) -> None:
        return None

    def sendall(self, data: bytes) -> None:
        self.sent.append(data)

    def recv(self, _limit: int) -> bytes:
        if self.fail_recv:
            raise OSError("mock QMP receive failure")
        if self.event_stream:
            return b'{"event":"NOOP"}\n'
        return b'{"QMP":{}}\n'

    def close(self) -> None:
        self.closed = True


class native_guest_runner_contract_tests_t(unittest.TestCase):
    @property
    def root(self) -> Path:
        if not hasattr(self, "_temporary_root"):
            temporary = tempfile.TemporaryDirectory(prefix="native guest runner ")
            self.addCleanup(temporary.cleanup)
            self._temporary_root = Path(temporary.name)
        return self._temporary_root

    def _base(self) -> Path:
        base = self.root / "base"
        (base / "tpm").mkdir(parents=True)
        (base / "windows.qcow2").write_bytes(b"protected disk")
        (base / "vars.fd").write_bytes(b"private vars source")
        (base / "tpm" / "tpm2-00.permall").write_bytes(b"private tpm state")
        return base

    def test_protected_snapshot_detects_disk_and_tpm_changes(self) -> None:
        base = self._base()
        before = _protected_snapshot(base)
        self.assertEqual(_protected_snapshot(base), before)
        (base / "tpm" / "tpm2-00.permall").write_bytes(b"changed")
        with self.assertRaisesRegex(runner_error_t, "protected base disk"):
            from native_guest_runner import _verify_snapshot
            _verify_snapshot(base, before)

    def test_protected_state_rejects_symlink(self) -> None:
        base = self._base()
        (base / "windows.qcow2").unlink()
        (base / "windows.qcow2").symlink_to(self.root / "outside")
        with self.assertRaises(runner_error_t):
            _protected_snapshot(base)

    def test_protected_base_directory_alias_is_rejected(self) -> None:
        base = self._base()
        alias = self.root / "base-alias"
        alias.symlink_to(base, target_is_directory=True)
        with self.assertRaisesRegex(runner_error_t, "base directory"):
            _protected_snapshot(alias)

    def test_writable_base_descriptor_guard_matches_inode(self) -> None:
        base = self.root / "base-disk.qcow2"
        base.write_bytes(b"disk")
        proc_root = self.root / "proc"
        process = proc_root / str(os.getpid())
        fd_dir = process / "fd"
        fdinfo_dir = process / "fdinfo"
        fd_dir.mkdir(parents=True)
        fdinfo_dir.mkdir()
        (process / "maps").write_text("", encoding="utf-8")
        target_stat = base.stat()
        major, minor = os.major(target_stat.st_dev), os.minor(target_stat.st_dev)
        (process / "mountinfo").write_text(
            f"28 1 {major}:{minor} / / rw - ext4 test rw\n", encoding="ascii")
        descriptor = fd_dir / "7"
        descriptor.symlink_to(base)
        (fdinfo_dir / "7").write_text(
            f"pos:\t0\nflags:\t02\nmnt_id:\t28\nino:\t{target_stat.st_ino}\n", encoding="ascii")
        with self.assertRaisesRegex(runner_error_t, "writable open descriptor"):
            _check_no_writable_base_holder(base, proc_root)
        descriptor.unlink()
        self.assertEqual(_check_no_writable_base_holder(base, proc_root), [])

    def test_writable_base_scan_fails_closed_on_unreadable_same_uid_process(self) -> None:
        base = self.root / "base-disk.qcow2"
        base.write_bytes(b"disk")
        proc_root = self.root / "proc"
        process = proc_root / str(os.getpid())
        fd_dir = process / "fd"
        fd_dir.mkdir(parents=True)
        (process / "maps").write_text("", encoding="utf-8")
        original_iterdir = Path.iterdir

        def deny_fd_scan(path: Path):
            if path == fd_dir:
                raise PermissionError("mock same-user fd scan denial")
            return original_iterdir(path)

        with mock.patch.object(Path, "iterdir", deny_fd_scan):
            with self.assertRaisesRegex(runner_error_t, "current-uid process descriptors"):
                _check_no_writable_base_holder(base, proc_root)

    def test_writable_base_scan_fails_closed_on_matching_fdinfo_error(self) -> None:
        base = self.root / "base-disk.qcow2"
        base.write_bytes(b"disk")
        proc_root = self.root / "proc"
        process = proc_root / str(os.getpid())
        fd_dir = process / "fd"
        fd_dir.mkdir(parents=True)
        fdinfo_dir = process / "fdinfo"
        fdinfo_dir.mkdir()
        (fd_dir / "7").symlink_to(base)
        target_stat = base.stat()
        major, minor = os.major(target_stat.st_dev), os.minor(target_stat.st_dev)
        (process / "mountinfo").write_text(f"28 1 {major}:{minor} / / rw - ext4 test rw\n", encoding="ascii")
        (fdinfo_dir / "7").write_text(f"pos:\t0\nflags:\t02\nmnt_id:\t28\nino:\t{target_stat.st_ino}\n", encoding="ascii")
        original_read_text = Path.read_text

        def deny_fdinfo(path: Path, *args: object, **kwargs: object) -> str:
            if path == fdinfo_dir / "7":
                raise PermissionError("mock fdinfo denial")
            return original_read_text(path, *args, **kwargs)

        with mock.patch.object(Path, "read_text", deny_fdinfo):
            with self.assertRaisesRegex(runner_error_t, "cannot establish descriptor identity"):
                _check_no_writable_base_holder(base, proc_root)

    def test_kernel_read_lease_acquires_queries_releases_and_restores_sigio(self) -> None:
        base = self.root / "lease-base.qcow2"
        base.write_bytes(b"private lease fixture")
        previous_handler = signal.getsignal(signal.SIGIO)
        lease = base_read_lease_t()
        lease.acquire(base)
        self.assertEqual(fcntl.fcntl(lease.fd, fcntl.F_GETLEASE), fcntl.F_RDLCK)
        self.assertTrue(lease.snapshot()["acquired"])
        lease.check()
        lease.release()
        self.assertIsNone(lease.fd)
        self.assertTrue(lease.snapshot()["released"])
        self.assertEqual(signal.getsignal(signal.SIGIO), previous_handler)

    def test_kernel_read_lease_rejects_preexisting_writer(self) -> None:
        base = self.root / "lease-writer-conflict.qcow2"
        base.write_bytes(b"private writer conflict fixture")
        writer_fd = os.open(base, os.O_RDWR | os.O_CLOEXEC)
        lease = base_read_lease_t()
        try:
            with self.assertRaisesRegex(runner_error_t, "cannot acquire kernel base read lease"):
                lease.acquire(base)
            self.assertIsNone(lease.fd)
            self.assertEqual(signal.getsignal(signal.SIGIO), signal.SIG_DFL)
        finally:
            os.close(writer_fd)

    def test_kernel_read_lease_notifies_and_detects_writer_break_attempt(self) -> None:
        base = self.root / "lease-break.qcow2"
        base.write_bytes(b"private lease break fixture")
        lease = base_read_lease_t()
        lease.acquire(base)
        try:
            with self.assertRaises(BlockingIOError):
                os.open(base, os.O_WRONLY | os.O_NONBLOCK | os.O_CLOEXEC)
            with self.assertRaisesRegex(runner_error_t, "lease was broken"):
                lease.check()
            self.assertTrue(lease.snapshot()["broken"])
        finally:
            lease.release()

    def test_qemu_provenance_rejects_hash_mismatch_and_wrong_version(self) -> None:
        qemu = self.root / "qemu-system-x86_64"
        qemu.write_bytes(b"mock binary")
        stamp = self.root / ".waddle-qemu-build.json"
        stamp.write_text(json.dumps({"schema": 1, "binary": qemu.name,
            "binary_sha256": "0" * 64, "configuration_sha256": {},
            "inputs": {"schema": 1, "source_commit": "b2995afed254e4804611df52210067a4d5dbdf0b"}}))
        with self.assertRaisesRegex(runner_error_t, "hash does not match"):
            _validate_qemu(qemu, stamp)

        qemu.write_text("#!/bin/sh\necho 'QEMU emulator version 10.0.13'\n")
        qemu.chmod(0o700)
        stamp.write_text(json.dumps({"schema": 1, "binary": qemu.name,
            "binary_sha256": hashlib.sha256(qemu.read_bytes()).hexdigest(), "configuration_sha256": {},
            "inputs": {"schema": 1, "source_commit": "b2995afed254e4804611df52210067a4d5dbdf0b"}}))
        with self.assertRaisesRegex(runner_error_t, "unexpected QEMU version"):
            _validate_qemu(qemu, stamp)

    def test_qcow2_backing_chain_rejects_external_data_and_extra_backing(self) -> None:
        overlay = self.root / "overlay.qcow2"
        base = self.root / "base.qcow2"
        expected = [overlay, base]
        with mock.patch("native_guest_runner._run_bounded", return_value=json.dumps([
                {"filename": str(overlay), "format": "qcow2", "backing-filename": str(base),
                 "format-specific": {"data": {"data-file": str(self.root / "external.raw")}}},
                {"filename": str(base), "format": "qcow2"}])):
            with self.assertRaisesRegex(runner_error_t, "external data file"):
                _reject_external_qcow2_storage("qemu-img", overlay, expected)
        with mock.patch("native_guest_runner._run_bounded", return_value=json.dumps([
                {"filename": str(overlay), "format": "qcow2", "backing-filename": str(base)},
                {"filename": str(base), "format": "qcow2", "backing-filename": str(self.root / "third.qcow2")}])):
            with self.assertRaisesRegex(runner_error_t, "unapproved backing"):
                _reject_external_qcow2_storage("qemu-img", overlay, expected)

    def test_bounded_tool_output_is_stopped_before_unbounded_capture(self) -> None:
        with self.assertRaisesRegex(runner_error_t, "output exceeded"):
            _run_bounded([sys.executable, "-c", "import sys; sys.stdout.write('x' * 1000000)"],
                         timeout=3.0, output_limit=32)

    def test_qmp_constructor_closes_socket_after_greeting_failure(self) -> None:
        qmp_socket = fake_qmp_socket_t(fail_recv=True)
        with mock.patch("native_guest_runner.socket.socket", return_value=qmp_socket):
            with self.assertRaisesRegex(OSError, "mock QMP receive failure"):
                qmp_client_t(self.root / "qmp.sock", 0.1)
        self.assertTrue(qmp_socket.closed)

    def test_qmp_event_storm_is_bounded_by_one_command_deadline(self) -> None:
        qmp = qmp_client_t.__new__(qmp_client_t)
        qmp.timeout = 0.01
        qmp._on_tick = None
        qmp.socket = fake_qmp_socket_t(event_stream=True)
        with self.assertRaisesRegex(runner_error_t, "deadline expired"):
            qmp.execute("query-status")

    def test_result_channel_accepts_only_matching_nonce(self) -> None:
        listener = fake_listener_t(b'{"nonce":"expected-nonce","ok":true}\n')
        with mock.patch("native_guest_runner.socket.socket", return_value=listener):
            channel = result_channel_t(self.root, nonce="expected-nonce")
        self.assertEqual(channel.receive(2.0), {"nonce": "expected-nonce", "ok": True})
        channel.close()

    def test_result_channel_rejects_wrong_nonce_and_oversize(self) -> None:
        listener = fake_listener_t(b'{"nonce":"wrong"}\n')
        with mock.patch("native_guest_runner.socket.socket", return_value=listener):
            channel = result_channel_t(self.root, nonce="right")
        with self.assertRaisesRegex(runner_error_t, "nonce"):
            channel.receive(2.0)
        listener.payload = b"x" * (65536 + 1)
        with self.assertRaisesRegex(runner_error_t, "oversized"):
            channel.receive(2.0)
        channel.close()

    def test_child_stop_uses_only_the_owned_process_handle(self) -> None:
        runner = native_guest_runner_t(runner_config_t(run_parent=self.root))
        process = fake_process_t()
        other = fake_process_t()
        runner.children.append(("qemu", process))
        self.assertTrue(runner._stop_child(process, 1.0))
        self.assertTrue(process.terminated)
        self.assertFalse(other.terminated)
        self.assertTrue(runner._forced_shutdown)
        self.assertEqual(process.wait_calls, 1)

    def test_shutdown_timeout_kills_only_exact_child(self) -> None:
        runner = native_guest_runner_t(runner_config_t(run_parent=self.root))
        process = slow_process_t()
        other = fake_process_t()
        runner.children.append(("qemu", process))
        runner._stop_child(process, 0.01)
        self.assertTrue(process.terminated)
        self.assertTrue(process.killed)
        self.assertFalse(other.killed)
        self.assertEqual(process.wait_calls, 2)

    def test_partial_startup_qemu_failure_keeps_exact_swtpm_child_for_cleanup(self) -> None:
        runner = native_guest_runner_t(runner_config_t(run_parent=self.root))
        runner.run_root = self.root
        runner._base_lease.check = lambda: None  # type: ignore[method-assign]
        (self.root / "tpm.sock").touch()
        child = fake_process_t()
        runner._popen = lambda *args, **kwargs: child
        with mock.patch("native_guest_runner.shutil.which", return_value="/fixture-only/swtpm"):
            process, _socket_path = runner._start_swtpm()
        self.assertIs(process, child)
        runner._popen = lambda *args, **kwargs: (_ for _ in ()).throw(OSError("mock QEMU spawn failure"))
        with self.assertRaisesRegex(OSError, "QEMU spawn failure"):
            runner._start_child("qemu", ["qemu"], self.root / "qemu.log")
        self.assertEqual(runner.children, [("swtpm", child)])
        runner._stop_child(child, 1.0)
        self.assertTrue(child.terminated)

    def test_run_reaps_registered_swtpm_when_startup_raises_before_return(self) -> None:
        run_root = self.root / "run"
        run_root.mkdir(mode=0o700)
        runner = native_guest_runner_t(runner_config_t(run_parent=self.root))
        child = fake_process_t()

        def prepare() -> dict[str, object]:
            runner.run_root = run_root
            return {"base_snapshot": {}, "test_only": True, "interop_credit": False}

        def start_swtpm() -> tuple[object, Path]:
            runner.children.append(("swtpm", child))
            raise runner_error_t("mock private socket startup timeout")

        runner.prepare = prepare  # type: ignore[method-assign]
        runner._start_swtpm = start_swtpm  # type: ignore[method-assign]
        with mock.patch("native_guest_runner._verify_snapshot"):
            with self.assertRaisesRegex(runner_error_t, "evidence retained"):
                runner.run()
        self.assertTrue(child.terminated)
        self.assertTrue((run_root / "run-manifest.json").is_file())
        self.assertIn("private socket startup timeout",
                      (run_root / "run-manifest.json").read_text(encoding="utf-8"))

    def test_run_releases_base_lease_only_after_failed_startup_child_reaped(self) -> None:
        run_root = self.root / "run-lease-cleanup"
        run_root.mkdir(mode=0o700)
        runner = native_guest_runner_t(runner_config_t(run_parent=self.root))
        child = fake_process_t()
        release_observed: list[bool] = []

        class tracking_lease_t:
            def check(self) -> None:
                return None

            def release(self) -> None:
                release_observed.append(child.poll() is not None)

            def snapshot(self) -> dict[str, object]:
                return {"acquired": True, "released": bool(release_observed)}

        runner._base_lease = tracking_lease_t()  # type: ignore[assignment]

        def prepare() -> dict[str, object]:
            runner.run_root = run_root
            return {"base_snapshot": {}, "test_only": True, "interop_credit": False}

        def start_swtpm() -> tuple[object, Path]:
            runner.children.append(("swtpm", child))
            raise runner_error_t("mock startup cancellation")

        runner.prepare = prepare  # type: ignore[method-assign]
        runner._start_swtpm = start_swtpm  # type: ignore[method-assign]
        with mock.patch("native_guest_runner._verify_snapshot"):
            with self.assertRaisesRegex(runner_error_t, "evidence retained"):
                runner.run()
        self.assertEqual(release_observed, [True])

    def test_cleanup_close_failure_does_not_skip_tpm_reap(self) -> None:
        run_root = self.root / "run-cleanup"
        run_root.mkdir(mode=0o700)
        runner = native_guest_runner_t(runner_config_t(run_parent=self.root))
        child = fake_process_t()

        class broken_qmp_t:
            def close(self) -> None:
                raise OSError("mock QMP close failure")

        def prepare() -> dict[str, object]:
            runner.run_root = run_root
            runner._qmp = broken_qmp_t()  # type: ignore[assignment]
            return {"base_snapshot": {}, "test_only": True, "interop_credit": False}

        def start_swtpm() -> tuple[object, Path]:
            runner.children.append(("swtpm", child))
            raise runner_error_t("mock QEMU startup failure")

        runner.prepare = prepare  # type: ignore[method-assign]
        runner._start_swtpm = start_swtpm  # type: ignore[method-assign]
        with mock.patch("native_guest_runner._verify_snapshot"):
            with self.assertRaises(runner_error_t):
                runner.run()
        self.assertTrue(child.terminated)
        saved = (run_root / "run-manifest.json").read_text(encoding="utf-8")
        self.assertIn("QMP socket close failed", saved)

    def test_nan_and_infinite_deadlines_fail_preflight(self) -> None:
        for value in (float("nan"), float("inf"), float("-inf")):
            runner = native_guest_runner_t(runner_config_t(run_seconds=value))
            with self.assertRaisesRegex(runner_error_t, "finite"):
                runner.prepare()
        for config in (runner_config_t(run_seconds=3600.1),
                       runner_config_t(startup_seconds=300.1),
                       runner_config_t(shutdown_seconds=300.1)):
            with self.assertRaisesRegex(runner_error_t, "maximum bounds"):
                native_guest_runner_t(config).prepare()

    def test_qemu_arguments_are_private_tcg_without_host_vsock_usb_or_audio(self) -> None:
        runner = native_guest_runner_t(runner_config_t(run_parent=self.root, qemu_build_dir=self.root))
        runner.run_root = self.root
        argv = runner._qemu_argv(self.root / "tpm.sock")
        joined = " ".join(argv)
        self.assertIn("accel=tcg", joined)
        self.assertIn("windows-overlay.qcow2", joined)
        self.assertNotIn("vhost-vsock", joined)
        self.assertNotIn("usb-host", joined)
        self.assertNotIn("audiodev", joined)
        self.assertNotIn("windows.qcow2", joined)

    def test_guest_vsock_cid_rejects_reserved_and_noninteger_values(self) -> None:
        _validate_guest_vsock_cid(None)
        for value in (0, 1, 2, 0xFFFFFFFF, -1, MaximumGuestVsockCid + 1,
                      True, 3.0, "3"):
            with self.subTest(value=value), self.assertRaisesRegex(runner_error_t, "guest vsock CID"):
                _validate_guest_vsock_cid(value)  # type: ignore[arg-type]
        _validate_guest_vsock_cid(MinimumGuestVsockCid)
        _validate_guest_vsock_cid(MaximumGuestVsockCid)

    def test_vhost_vsock_duplicate_rejects_closed_regular_and_wrong_device_fds(self) -> None:
        with self.assertRaisesRegex(runner_error_t, "nonnegative open"):
            _duplicate_vhost_vsock_fd(-1)
        with self.assertRaisesRegex(runner_error_t, "duplicate"):
            _duplicate_vhost_vsock_fd(999999)
        source_fd = os.open(self.root / "ordinary-file", os.O_CREAT | os.O_RDWR | os.O_CLOEXEC, 0o600)
        self.addCleanup(os.close, source_fd)
        with self.assertRaisesRegex(runner_error_t, "character device"):
            _duplicate_vhost_vsock_fd(source_fd)
        char_info = SimpleNamespace(st_mode=stat.S_IFCHR | 0o600, st_rdev=os.makedev(10, 240))
        duplicates: list[int] = []
        original_dup = os.dup

        def tracked_dup(fd: int) -> int:
            duplicate_fd = original_dup(fd)
            duplicates.append(duplicate_fd)
            return duplicate_fd

        with mock.patch("native_guest_runner.os.fstat", return_value=char_info):
            with mock.patch("native_guest_runner.os.dup", side_effect=tracked_dup):
                with self.assertRaisesRegex(runner_error_t, "10:241"):
                    _duplicate_vhost_vsock_fd(source_fd)
        self.assertEqual(len(duplicates), 1)
        with self.assertRaises(OSError):
            fcntl.fcntl(duplicates[0], fcntl.F_GETFD)

    def test_vhost_vsock_fd_duplicates_cloexec_and_preserves_caller_fd(self) -> None:
        source_fd = os.open(self.root / "vhost-source", os.O_CREAT | os.O_RDWR | os.O_CLOEXEC, 0o600)
        self.addCleanup(os.close, source_fd)
        char_info = SimpleNamespace(st_mode=stat.S_IFCHR | 0o600,
                                    st_rdev=os.makedev(VhostVsockMajor, VhostVsockMinor))
        with mock.patch("native_guest_runner.os.fstat", return_value=char_info):
            duplicate_fd = _duplicate_vhost_vsock_fd(source_fd)
        try:
            self.assertNotEqual(duplicate_fd, source_fd)
            self.assertFalse(os.get_inheritable(duplicate_fd))
            self.assertTrue(os.fstat(source_fd))
        finally:
            os.close(duplicate_fd)

    def test_vhost_vsock_duplicate_failure_does_not_close_caller_fd(self) -> None:
        source_fd = os.open(self.root / "vhost-dup-failure", os.O_CREAT | os.O_RDWR | os.O_CLOEXEC, 0o600)
        self.addCleanup(os.close, source_fd)
        with mock.patch("native_guest_runner.os.dup", side_effect=OSError("mock dup failure")):
            with self.assertRaisesRegex(runner_error_t, "mock dup failure"):
                _duplicate_vhost_vsock_fd(source_fd)
        self.assertTrue(os.fstat(source_fd))

    def test_vhost_vsock_validation_failure_closes_duplicate(self) -> None:
        source_fd = os.open(self.root / "vhost-validation-failure", os.O_CREAT | os.O_RDWR | os.O_CLOEXEC, 0o600)
        self.addCleanup(os.close, source_fd)
        duplicates: list[int] = []
        original_dup = os.dup

        def tracked_dup(fd: int) -> int:
            duplicate_fd = original_dup(fd)
            duplicates.append(duplicate_fd)
            return duplicate_fd

        with mock.patch("native_guest_runner.os.dup", side_effect=tracked_dup):
            with mock.patch("native_guest_runner.os.set_inheritable", side_effect=OSError("mock CLOEXEC failure")):
                with self.assertRaisesRegex(runner_error_t, "mock CLOEXEC failure"):
                    _duplicate_vhost_vsock_fd(source_fd)
        self.assertEqual(len(duplicates), 1)
        with self.assertRaises(OSError):
            fcntl.fcntl(duplicates[0], fcntl.F_GETFD)
        self.assertTrue(os.fstat(source_fd))

    def test_vhost_vsock_rejects_readonly_and_writeonly_fds(self) -> None:
        char_info = SimpleNamespace(st_mode=stat.S_IFCHR | 0o600,
                                    st_rdev=os.makedev(VhostVsockMajor, VhostVsockMinor))
        original_dup = os.dup
        for access_mode in (os.O_RDONLY, os.O_WRONLY):
            with self.subTest(access_mode=access_mode):
                source_fd = os.open(self.root / f"vhost-mode-{access_mode}",
                                    os.O_CREAT | access_mode | os.O_CLOEXEC, 0o600)
                duplicates: list[int] = []

                def tracked_dup(fd: int) -> int:
                    duplicate_fd = original_dup(fd)
                    duplicates.append(duplicate_fd)
                    return duplicate_fd

                try:
                    with mock.patch("native_guest_runner.os.fstat", return_value=char_info):
                        with mock.patch("native_guest_runner.os.dup", side_effect=tracked_dup):
                            with mock.patch("native_guest_runner.fcntl.fcntl", return_value=access_mode):
                                with self.assertRaisesRegex(runner_error_t, "read/write access"):
                                    _duplicate_vhost_vsock_fd(source_fd)
                    self.assertEqual(len(duplicates), 1)
                    with self.assertRaises(OSError):
                        fcntl.fcntl(duplicates[0], fcntl.F_GETFD)
                    self.assertTrue(os.fstat(source_fd))
                finally:
                    os.close(source_fd)

    def test_vhost_vsock_run_api_closes_only_owned_duplicate(self) -> None:
        source_fd = os.open(self.root / "vhost-run-api", os.O_CREAT | os.O_RDWR | os.O_CLOEXEC, 0o600)
        self.addCleanup(os.close, source_fd)
        char_info = SimpleNamespace(st_mode=stat.S_IFCHR | 0o600,
                                    st_rdev=os.makedev(VhostVsockMajor, VhostVsockMinor))
        runner = native_guest_runner_t(runner_config_t(guest_vsock_cid=42))
        duplicated: list[int] = []
        original_dup = os.dup

        def tracked_dup(fd: int) -> int:
            result_fd = original_dup(fd)
            duplicated.append(result_fd)
            return result_fd

        with mock.patch("native_guest_runner.os.fstat", return_value=char_info):
            with mock.patch("native_guest_runner.os.dup", side_effect=tracked_dup):
                runner.run = mock.Mock(return_value={"test_only": True})  # type: ignore[method-assign]
                result = runner.run_with_vhost_vsock_fd(source_fd)
        self.assertEqual(result, {"test_only": True})
        self.assertIsNone(runner._vhost_vsock_fd)
        self.assertTrue(os.fstat(source_fd))
        self.assertEqual(len(duplicated), 1)
        with self.assertRaises(OSError):
            fcntl.fcntl(duplicated[0], fcntl.F_GETFD)
        self.assertEqual(runner.run.call_count, 1)  # type: ignore[attr-defined]

        with mock.patch("native_guest_runner.os.fstat", return_value=char_info):
            with mock.patch("native_guest_runner.os.dup", side_effect=tracked_dup):
                runner.run = mock.Mock(side_effect=runner_error_t("mock run failure"))  # type: ignore[method-assign]
                with self.assertRaisesRegex(runner_error_t, "mock run failure"):
                    runner.run_with_vhost_vsock_fd(source_fd)
        self.assertEqual(len(duplicated), 2)
        with self.assertRaises(OSError):
            fcntl.fcntl(duplicated[1], fcntl.F_GETFD)
        self.assertIsNone(runner._vhost_vsock_fd)

    def test_vhost_vsock_pair_and_qemu_arguments_are_explicit(self) -> None:
        runner = native_guest_runner_t(runner_config_t(guest_vsock_cid=42, run_parent=self.root, qemu_build_dir=self.root))
        runner.run_root = self.root
        with self.assertRaisesRegex(runner_error_t, "requires an owned"):
            runner._qemu_argv(self.root / "tpm.sock")
        runner._vhost_vsock_fd = 73
        argv = runner._qemu_argv(self.root / "tpm.sock")
        self.assertIn("vhost-vsock-pci,guest-cid=42,vhostfd=73", argv)
        self.assertEqual(argv[argv.index("-machine") + 1], "q35,accel=tcg")

    def test_qemu_child_receives_only_the_supplied_duplicate_fd(self) -> None:
        runner = native_guest_runner_t(runner_config_t(run_parent=self.root))
        calls: list[dict[str, object]] = []
        process = fake_process_t()

        def fake_popen(_argv: list[str], **kwargs: object) -> fake_process_t:
            calls.append(kwargs)
            return process

        runner._popen = fake_popen
        source_fd = os.open(self.root / "vhost-pass-fds", os.O_CREAT | os.O_RDWR | os.O_CLOEXEC, 0o600)
        duplicate_fd = os.dup(source_fd)
        os.set_inheritable(duplicate_fd, False)
        self.addCleanup(os.close, duplicate_fd)
        self.addCleanup(os.close, source_fd)
        runner._start_child("qemu", ["qemu-test"], self.root / "qemu-child.log",
                            pass_fds=(duplicate_fd,))
        self.assertEqual(calls[0]["pass_fds"], (duplicate_fd,))
        self.assertTrue(calls[0]["close_fds"])
        self.assertNotIn(source_fd, calls[0]["pass_fds"])
        self.assertEqual(calls[0]["stdin"], subprocess.DEVNULL)
        runner._close_child_log(process)

    def test_guest_vsock_configuration_requires_the_fd_callback_api(self) -> None:
        runner = native_guest_runner_t(runner_config_t(guest_vsock_cid=42))
        with self.assertRaisesRegex(runner_error_t, "requires run_with_vhost_vsock_fd"):
            runner.run()
        no_cid_runner = native_guest_runner_t(runner_config_t())
        with self.assertRaisesRegex(runner_error_t, "CID is required"):
            no_cid_runner.run_with_vhost_vsock_fd(999999)


if __name__ == "__main__":
    unittest.main()
