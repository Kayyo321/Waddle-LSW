"""No-VM tests for the bounded fresh native install supervisor."""

from __future__ import annotations

import json
import os
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest
from unittest import mock

import fresh_native_install as install


class fake_child_t:
    def __init__(self, *, waits_to_timeout: bool = False) -> None:
        self.returncode = None
        self.waits_to_timeout = waits_to_timeout
        self.terminated = False
        self.killed = False

    def poll(self):
        return self.returncode

    def wait(self, timeout=None):
        if self.waits_to_timeout and not self.terminated:
            raise subprocess.TimeoutExpired("mock child", timeout)
        self.returncode = 0
        return self.returncode

    def terminate(self):
        self.terminated = True
        self.returncode = -15

    def kill(self):
        self.killed = True
        self.returncode = -9


class fake_qmp_t:
    def __init__(self, *, fail_commands: set[str] | None = None) -> None:
        self.calls: list[tuple[str, dict | None]] = []
        self.closed = False
        self.fail_commands = fail_commands or set()

    def execute(self, command, arguments=None, *, timeout_seconds=None):
        self.calls.append((command, arguments))
        if command in self.fail_commands:
            raise TimeoutError("mock QMP timeout")

    def close(self):
        self.closed = True


class fake_socket_t:
    def __init__(self, incoming: list[bytes], *, fail_connect: bool = False,
                 clock: list[float] | None = None) -> None:
        self.incoming = list(incoming)
        self.fail_connect = fail_connect
        self.clock = clock
        self.timeouts = []
        self.sent = []
        self.closed = False

    def settimeout(self, value):
        self.timeouts.append(value)

    def connect(self, _path):
        if self.fail_connect:
            raise OSError("mock connect failure")

    def recv(self, _size):
        if self.clock is not None:
            self.clock[0] += 2.0
        if self.incoming:
            return self.incoming.pop(0)
        return b""

    def sendall(self, data):
        self.sent.append(data)

    def close(self):
        self.closed = True


class unkillable_child_t:
    returncode = None

    def __init__(self) -> None:
        self.terminate_attempted = False
        self.kill_attempted = False

    def poll(self):
        return None

    def wait(self, timeout=None):
        raise subprocess.TimeoutExpired("mock child", timeout)

    def terminate(self):
        self.terminate_attempted = True
        raise OSError("mock terminate error")

    def kill(self):
        self.kill_attempted = True
        raise OSError("mock kill error")


class first_poll_error_child_t(fake_child_t):
    def __init__(self) -> None:
        super().__init__()
        self.poll_count = 0

    def poll(self):
        self.poll_count += 1
        if self.poll_count == 1:
            raise OSError("mock poll failure")
        return super().poll()


class fresh_native_install_tests_t(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="fresh install supervisor ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.run_root = self.root / "fresh-run"
        self.run_root.mkdir(mode=0o700)
        self.run_root.chmod(0o700)

    def _manifest(self):
        qmp = f"unix:{self.run_root / 'qmp.sock'},server=on,wait=off"
        return {
            "run_root": str(self.run_root),
            "qemu_argv_template": ["qemu-system-x86_64", "-machine", "q35,accel=kvm",
                                   "-nic", "none", "-drive",
                                   f"if=pflash,format=raw,file={self.run_root / 'vars.fd'}",
                                   "-drive", f"if=none,id=windows,format=qcow2,file={self.run_root / 'windows.qcow2'}",
                                   "-drive", f"if=none,id=answer,media=cdrom,readonly=on,format=raw,file={self.run_root / 'answer.iso'}",
                                   "-chardev", f"socket,id=chrtpm,path={self.run_root / 'tpm.sock'}",
                                   "-D", str(self.run_root / "qemu.log"), "-qmp", qmp, "-device",
                                   "vhost-vsock-pci,guest-cid=52942,vhostfd="
                                   "CALLER_VALIDATED_VHOST_VSOCK_FD"],
            "swtpm_argv": ["swtpm", "socket", "--tpm2", "--tpmstate",
                           f"dir={self.run_root / 'tpm'}", "--ctrl",
                           f"type=unixio,path={self.run_root / 'tpm.sock'}"],
        }

    def test_supervises_only_exact_children_and_records_private_outcome(self) -> None:
        children = [fake_child_t(), fake_child_t(waits_to_timeout=True)]
        popen_calls = []

        def popen(argv, **kwargs):
            popen_calls.append((argv, kwargs))
            return children[len(popen_calls) - 1]

        qmp = fake_qmp_t()
        now = [0.0]

        def sleep(seconds):
            now[0] += seconds

        with (mock.patch.object(install, "_require_ordinary_uid"),
              mock.patch.object(install.os.path, "exists", return_value=True),
              mock.patch.object(install.os, "access", return_value=True),
              mock.patch.object(install, "_duplicate_vhost_fd", return_value=77),
              mock.patch.object(install, "_wait_for_swtpm_socket"),
              mock.patch.object(install.os, "close"),
              mock.patch.object(install, "InstallDeadlineSeconds", 2),
              mock.patch.object(install, "ShutdownGraceSeconds", 1)):
            result = install.run_fresh_install(
                self.run_root, self._manifest(), 12, popen=popen,
                qmp_factory=lambda _path, _deadline, _connect_deadline: qmp,
                monotonic=lambda: now[0], sleep=sleep,
                screenshot_interval_seconds=1)

        self.assertEqual(len(popen_calls), 2)
        self.assertEqual(popen_calls[0][1]["pass_fds"], ())
        self.assertEqual(popen_calls[1][1]["pass_fds"], (77,))
        self.assertIn("vhostfd=77", " ".join(popen_calls[1][0]))
        self.assertTrue(all(call[1]["close_fds"] for call in popen_calls))
        self.assertIn(("system_powerdown", None), qmp.calls)
        self.assertTrue(qmp.closed)
        self.assertTrue(children[1].terminated)
        self.assertTrue(result["forced_qemu_termination"])
        self.assertTrue(result["qemu_reaped"] and result["swtpm_reaped"])
        self.assertFalse(result["native_acceptance_credit"])
        manifest_path = self.run_root / "install-manifest.json"
        self.assertEqual(stat.S_IMODE(manifest_path.stat().st_mode), 0o600)
        persisted = json.loads(manifest_path.read_text(encoding="utf-8"))
        self.assertTrue(persisted["deadline_reached"])
        self.assertNotIn("password", json.dumps(persisted).lower())

    def test_explicit_install_boot_key_is_finite_and_does_not_grant_acceptance(self) -> None:
        children = [fake_child_t(), fake_child_t()]
        qmp = fake_qmp_t()
        clock = [0.0]
        with (mock.patch.object(install, "_require_ordinary_uid"),
              mock.patch.object(install.os.path, "exists", return_value=True),
              mock.patch.object(install.os, "access", return_value=True),
              mock.patch.object(install, "_duplicate_vhost_fd", return_value=77),
              mock.patch.object(install, "_wait_for_swtpm_socket"),
              mock.patch.object(install.os, "close"),
              mock.patch.object(install, "InstallDeadlineSeconds", 13)):
            result = install.run_fresh_install(
                self.run_root, self._manifest(), 12,
                popen=lambda _argv, **_kwargs: children.pop(0),
                qmp_factory=lambda *_args: qmp,
                monotonic=lambda: clock[0],
                sleep=lambda delay: clock.__setitem__(0, clock[0]+delay),
                install_boot_key=True)
        self.assertEqual(result["install_boot_keys"], 24)
        self.assertEqual(len([command for command, _ in qmp.calls if command == "send-key"]), 24)
        self.assertFalse(result["native_acceptance_credit"])
        self.assertFalse(result["guest_booted"])

    def test_requires_private_run_and_valid_descriptor_marker_before_launch(self) -> None:
        bad = dict(self._manifest())
        bad["qemu_argv_template"] = ["qemu", "-machine", "q35,accel=kvm"]
        with mock.patch.object(install, "_require_ordinary_uid"), \
                mock.patch.object(install.os.path, "exists", return_value=True), \
                mock.patch.object(install.os, "access", return_value=True), \
                self.assertRaises(install.fresh_install_error_t):
            install.run_fresh_install(self.run_root, bad, 12, popen=mock.Mock())

        self.run_root.chmod(0o755)
        with self.assertRaisesRegex(install.fresh_install_error_t, "mode-0700"):
            install._require_private_run(self.run_root)

    def test_child_log_files_are_created_private_and_exclusive(self) -> None:
        log_path = self.run_root / "child.log"
        child = fake_child_t()
        with mock.patch("builtins.open", side_effect=AssertionError("unexpected")):
            # _launch_child uses os.open and passes the private descriptor only
            # as stdout to a mocked Popen; no process is actually created.
            launched, log_handle = install._launch_child(
                ["harmless"], log_path, popen=lambda _argv, **_kwargs: child)
        self.assertIs(launched, child)
        self.assertFalse(log_handle.closed)
        log_handle.close()
        self.assertEqual(stat.S_IMODE(log_path.stat().st_mode), 0o600)
        with self.assertRaises(FileExistsError):
            install._launch_child(["harmless"], log_path, popen=mock.Mock())

    def test_qmp_validates_greeting_correlates_ids_and_caps_events(self) -> None:
        socket_instance = fake_socket_t([
            b'{"QMP":{"version":{}}}\n', b'{"return":{},"id":0}\n',
            *([b'{"event":"RESET"}\n'] * 33),
        ])
        with mock.patch.object(install.socket, "socket", return_value=socket_instance):
            client = install.qmp_client_t(self.root / "qmp.sock", deadline=100.0,
                                          monotonic=lambda: 1.0)
            self.assertEqual(json.loads(socket_instance.sent[0])["id"], 0)
            with self.assertRaisesRegex(install.fresh_install_error_t, "event stream"):
                client.execute("query-status")
            self.assertTrue(socket_instance.sent)
            client.close()
        self.assertTrue(socket_instance.closed)

    def test_vhost_duplicate_is_validated_closed_on_bad_identity_and_never_mutates_source(self) -> None:
        source_fd = os.open(self.root / "borrowed-descriptor", os.O_CREAT | os.O_RDWR, 0o600)
        self.addCleanup(os.close, source_fd)
        os.set_inheritable(source_fd, True)
        valid_info = mock.Mock(st_mode=stat.S_IFCHR, st_rdev=os.makedev(10, 241))
        with (mock.patch.object(install.os, "fstat", return_value=valid_info),
              mock.patch.object(install.fcntl, "fcntl", return_value=os.O_RDWR)):
            duplicate_fd = install._duplicate_vhost_fd(source_fd)
        self.assertFalse(os.get_inheritable(duplicate_fd))
        self.assertTrue(os.get_inheritable(source_fd))
        os.fstat(source_fd)
        os.close(duplicate_fd)

        invalid_cases = ((mock.Mock(st_mode=stat.S_IFREG, st_rdev=os.makedev(10, 241)), os.O_RDWR),
                         (mock.Mock(st_mode=stat.S_IFCHR, st_rdev=os.makedev(1, 3)), os.O_RDWR),
                         (valid_info, os.O_RDONLY), (valid_info, os.O_WRONLY))
        for info, open_flags in invalid_cases:
            with (self.subTest(info=info, open_flags=open_flags),
                  mock.patch.object(install.os, "fstat", return_value=info),
                  mock.patch.object(install.fcntl, "fcntl", return_value=open_flags)):
                with self.assertRaises(install.fresh_install_error_t):
                    install._duplicate_vhost_fd(source_fd)
            os.fstat(source_fd)
            self.assertTrue(os.get_inheritable(source_fd))

        with mock.patch.object(install.os, "dup", side_effect=OSError("mock dup failure")):
            with self.assertRaises(install.fresh_install_error_t):
                install._duplicate_vhost_fd(source_fd)
        os.fstat(source_fd)
        self.assertTrue(os.get_inheritable(source_fd))

    def test_qmp_absolute_deadline_and_connect_failure_close_socket(self) -> None:
        clock = [0.0]
        socket_instance = fake_socket_t([], fail_connect=True)
        with mock.patch.object(install.socket, "socket", return_value=socket_instance):
            with self.assertRaises(OSError):
                install.qmp_client_t(self.root / "qmp.sock", deadline=10.0,
                                     monotonic=lambda: clock[0])
        self.assertTrue(socket_instance.closed)

        socket_instance = fake_socket_t([
            b'{"QMP":{"version":{}}}\n', b'{"return":{},"id":0}\n', b'{"partial"',
        ], clock=clock)
        with mock.patch.object(install.socket, "socket", return_value=socket_instance):
            client = install.qmp_client_t(self.root / "qmp.sock", deadline=10.0,
                                          monotonic=lambda: clock[0])
            client._deadline = clock[0] + 1.0
            with self.assertRaisesRegex(install.fresh_install_error_t, "deadline"):
                client.execute("query-status")
            client.close()

        socket_instance = fake_socket_t([b'{"invalid":{}}\n'])
        with mock.patch.object(install.socket, "socket", return_value=socket_instance):
            with self.assertRaisesRegex(install.fresh_install_error_t, "greeting"):
                install.qmp_client_t(self.root / "qmp.sock", deadline=10.0,
                                     monotonic=lambda: 0.0)
        self.assertTrue(socket_instance.closed)

        socket_instance = fake_socket_t([
            b'{"QMP":{"version":{}}}\n', b'{"return":{},"id":0}\n',
            b'{"return":{},"id":999}\n',
        ])
        with mock.patch.object(install.socket, "socket", return_value=socket_instance):
            client = install.qmp_client_t(self.root / "qmp.sock", deadline=10.0,
                                          monotonic=lambda: 0.0)
            with self.assertRaisesRegex(install.fresh_install_error_t, "correlation"):
                client.execute("query-status")
            client.close()

    def test_qemu_launch_failure_reaps_started_swtpm(self) -> None:
        tpm = fake_child_t()
        launch_count = [0]

        def popen(_argv, **_kwargs):
            launch_count[0] += 1
            if launch_count[0] == 1:
                return tpm
            raise OSError("mock QEMU launch failure")

        with (mock.patch.object(install, "_require_ordinary_uid"),
              mock.patch.object(install.os.path, "exists", return_value=True),
              mock.patch.object(install.os, "access", return_value=True),
              mock.patch.object(install, "_duplicate_vhost_fd", return_value=77),
              mock.patch.object(install, "_wait_for_swtpm_socket"),
              mock.patch.object(install.os, "close")):
            with self.assertRaises(install.fresh_install_error_t):
                install.run_fresh_install(self.run_root, self._manifest(), 12, popen=popen)
        self.assertTrue(tpm.poll() is not None)
        persisted = json.loads((self.run_root / "install-manifest.json").read_text())
        self.assertEqual(persisted["failure"], "OSError")

    def test_qemu_poll_failure_does_not_skip_cleanup(self) -> None:
        tpm = fake_child_t()
        qemu = first_poll_error_child_t()
        children = [tpm, qemu]
        count = [0]

        def popen(_argv, **_kwargs):
            child = children[count[0]]
            count[0] += 1
            return child

        qmp = fake_qmp_t()
        with (mock.patch.object(install, "_require_ordinary_uid"),
              mock.patch.object(install.os.path, "exists", return_value=True),
              mock.patch.object(install.os, "access", return_value=True),
              mock.patch.object(install, "_duplicate_vhost_fd", return_value=77),
              mock.patch.object(install, "_wait_for_swtpm_socket"),
              mock.patch.object(install.os, "close")):
            with self.assertRaises(install.fresh_install_error_t):
                install.run_fresh_install(
                    self.run_root, self._manifest(), 12, popen=popen,
                    qmp_factory=lambda _path, _deadline, _connect_deadline: qmp)
        self.assertIsNotNone(qemu.poll())
        self.assertIsNotNone(tpm.poll())
        self.assertIn(("system_powerdown", None), qmp.calls)
        record = json.loads((self.run_root / "install-manifest.json").read_text())
        self.assertEqual(record["failure"], "OSError")
        self.assertTrue(record["qemu_reaped"] and record["swtpm_reaped"])

    def test_qemu_cleanup_errors_do_not_skip_swtpm_fd_or_manifest_attempt(self) -> None:
        read_fd, write_fd = os.pipe2(os.O_NONBLOCK | os.O_CLOEXEC)
        self.addCleanup(os.close, read_fd)
        self.addCleanup(os.close, write_fd)
        qemu = unkillable_child_t()
        tpm = fake_child_t()
        children = [tpm, qemu]
        calls = []

        def popen(argv, **kwargs):
            calls.append((argv, kwargs))
            return children[len(calls) - 1]

        qmp = fake_qmp_t()
        stop_requested = [False]

        def request_stop(_seconds):
            if not stop_requested[0]:
                os.write(write_fd, b"stop")
                stop_requested[0] = True

        with (mock.patch.object(install, "_require_ordinary_uid"),
              mock.patch.object(install.os.path, "exists", return_value=True),
              mock.patch.object(install.os, "access", return_value=True),
              mock.patch.object(install, "_duplicate_vhost_fd", return_value=77),
              mock.patch.object(install, "_wait_for_swtpm_socket"),
              mock.patch.object(install, "ShutdownGraceSeconds", 0),
              mock.patch.object(install, "KillWaitSeconds", 0),
              mock.patch.object(install.os, "close") as close_fd):
            with self.assertRaisesRegex(install.fresh_install_error_t, "failed"):
                install.run_fresh_install(
                    self.run_root, self._manifest(), 12, popen=popen,
                    qmp_factory=lambda _path, _deadline, _connect_deadline: qmp,
                    sleep=request_stop, stop_fd=read_fd)

        self.assertTrue(qemu.terminate_attempted and qemu.kill_attempted)
        self.assertTrue(tpm.poll() is not None)
        close_fd.assert_called_once_with(77)
        record = json.loads((self.run_root / "install-manifest.json").read_text())
        self.assertTrue(record["qemu_unreaped"])
        self.assertTrue(record["swtpm_reaped"])
        self.assertTrue(record["owned_vhost_fd_closed"])

    def test_manifest_write_failure_still_reaps_both_owned_children(self) -> None:
        tpm = fake_child_t()
        qemu = fake_child_t()
        children = [tpm, qemu]
        count = [0]

        def popen(argv, **kwargs):
            count[0] += 1
            return children[count[0] - 1]

        with (mock.patch.object(install, "_require_ordinary_uid"),
              mock.patch.object(install.os.path, "exists", return_value=True),
              mock.patch.object(install.os, "access", return_value=True),
              mock.patch.object(install, "_duplicate_vhost_fd", return_value=77),
              mock.patch.object(install, "_wait_for_swtpm_socket"),
              mock.patch.object(install, "_write_manifest", side_effect=OSError("disk full")),
              mock.patch.object(install.os, "close") as close_fd):
            with self.assertRaises(install.fresh_install_error_t):
                install.run_fresh_install(self.run_root, self._manifest(), 12, popen=popen)
        self.assertIsNotNone(qemu.poll())
        self.assertIsNotNone(tpm.poll())
        close_fd.assert_called_once_with(77)

    def test_swtpm_readiness_failure_never_launches_qemu(self) -> None:
        tpm = fake_child_t()
        argv_seen = []

        def popen(argv, **_kwargs):
            argv_seen.append(argv)
            return tpm

        with (mock.patch.object(install, "_require_ordinary_uid"),
              mock.patch.object(install.os.path, "exists", return_value=True),
              mock.patch.object(install.os, "access", return_value=True),
              mock.patch.object(install, "_duplicate_vhost_fd", return_value=77),
              mock.patch.object(install, "_wait_for_swtpm_socket",
                                side_effect=install.fresh_install_error_t("not ready")),
              mock.patch.object(install.os, "close")):
            with self.assertRaises(install.fresh_install_error_t):
                install.run_fresh_install(self.run_root, self._manifest(), 12, popen=popen)
        self.assertEqual(len(argv_seen), 1)
        self.assertEqual(argv_seen[0][0], "swtpm")
        self.assertIsNotNone(tpm.poll())

    def test_qmp_connect_failure_reaps_both_children_with_short_injected_deadline(self) -> None:
        tpm = fake_child_t()
        qemu = fake_child_t()
        children = [tpm, qemu]
        launch_count = [0]

        def popen(_argv, **_kwargs):
            launch_count[0] += 1
            return children[launch_count[0] - 1]

        with (mock.patch.object(install, "_require_ordinary_uid"),
              mock.patch.object(install.os.path, "exists", return_value=True),
              mock.patch.object(install.os, "access", return_value=True),
              mock.patch.object(install, "_duplicate_vhost_fd", return_value=77),
              mock.patch.object(install, "_wait_for_swtpm_socket"),
              mock.patch.object(install, "InstallDeadlineSeconds", 0.01),
              mock.patch.object(install.os, "close")):
            with self.assertRaisesRegex(install.fresh_install_error_t, "failed"):
                install.run_fresh_install(
                    self.run_root, self._manifest(), 12, popen=popen,
                    qmp_factory=mock.Mock(side_effect=OSError("QMP unavailable")))
        self.assertEqual(launch_count[0], 2)
        self.assertIsNotNone(qemu.poll())
        self.assertIsNotNone(tpm.poll())

    def test_screenshot_qmp_timeout_reconnects_before_acpi(self) -> None:
        tpm = fake_child_t()
        qemu = fake_child_t()
        children = [tpm, qemu]
        index = [0]

        def popen(_argv, **_kwargs):
            child = children[index[0]]
            index[0] += 1
            return child

        poisoned_qmp = fake_qmp_t(fail_commands={"screendump"})
        replacement_qmp = fake_qmp_t()
        clients = [poisoned_qmp, replacement_qmp]
        qmp_index = [0]

        def qmp_factory(_path, _deadline, _connect_deadline):
            client = clients[qmp_index[0]]
            qmp_index[0] += 1
            return client

        with (mock.patch.object(install, "_require_ordinary_uid"),
              mock.patch.object(install.os.path, "exists", return_value=True),
              mock.patch.object(install.os, "access", return_value=True),
              mock.patch.object(install, "_duplicate_vhost_fd", return_value=77),
              mock.patch.object(install, "_wait_for_swtpm_socket"),
              mock.patch.object(install, "InstallDeadlineSeconds", 0.02),
              mock.patch.object(install.os, "close")):
            result = install.run_fresh_install(
                self.run_root, self._manifest(), 12, popen=popen, qmp_factory=qmp_factory,
                screenshot_interval_seconds=1)
        self.assertTrue(result["qmp_poisoned"])
        self.assertTrue(result["qmp_reconnected_for_cleanup"])
        self.assertTrue(result["acpi_requested"])
        self.assertIn(("system_powerdown", None), replacement_qmp.calls)
        self.assertTrue(poisoned_qmp.closed and replacement_qmp.closed)

    def test_nan_screenshot_interval_and_blocking_stop_fd_refuse_before_launch(self) -> None:
        read_fd, write_fd = os.pipe()
        self.addCleanup(os.close, read_fd)
        self.addCleanup(os.close, write_fd)
        popen = mock.Mock()
        with (mock.patch.object(install, "_require_ordinary_uid"),
              mock.patch.object(install.os.path, "exists", return_value=True),
              mock.patch.object(install.os, "access", return_value=True)):
            for interval in (float("nan"), float("inf")):
                with self.subTest(interval=interval), self.assertRaises(install.fresh_install_error_t):
                    install.run_fresh_install(self.run_root, self._manifest(), 12,
                                              screenshot_interval_seconds=interval, popen=popen)
            with self.assertRaisesRegex(install.fresh_install_error_t, "nonblocking"):
                install.run_fresh_install(self.run_root, self._manifest(), 12,
                                          stop_fd=read_fd, popen=popen)
        popen.assert_not_called()

    def test_swtpm_socket_readiness_and_stop_polling_are_bounded_and_borrowed(self) -> None:
        socket_path = self.run_root / "tpm.sock"
        with mock.patch.object(Path, "is_socket", return_value=True):
            self.assertTrue(install._wait_for_swtpm_socket(
                socket_path, fake_child_t(), deadline=10.0, monotonic=lambda: 0.0,
                sleep=lambda _seconds: self.fail("ready socket should not sleep")))
        read_fd, write_fd = os.pipe2(os.O_NONBLOCK | os.O_CLOEXEC)
        self.addCleanup(os.close, read_fd)
        self.addCleanup(os.close, write_fd)
        os.write(write_fd, b"stop")
        poller = __import__("select").poll()
        poller.register(read_fd, __import__("select").POLLIN)
        self.assertFalse(install._wait_for_swtpm_socket(
            socket_path, fake_child_t(), deadline=10.0, monotonic=lambda: 0.0,
            sleep=lambda _seconds: self.fail("stop request should return immediately"),
            stop_poller=poller))
        self.assertEqual(os.read(read_fd, 8), b"stop")

    def test_caller_stop_pipe_is_borrowed_and_requests_acpi_shutdown(self) -> None:
        read_fd, write_fd = os.pipe2(os.O_NONBLOCK | os.O_CLOEXEC)
        self.addCleanup(os.close, read_fd)
        self.addCleanup(os.close, write_fd)
        children = [fake_child_t(), fake_child_t()]
        calls = []

        def popen(argv, **kwargs):
            calls.append(kwargs)
            return children[len(calls) - 1]

        qmp = fake_qmp_t()
        now = [0.0]

        def request_stop(seconds):
            now[0] += seconds
            try:
                os.write(write_fd, b"stop")
            except BlockingIOError:
                pass
        with (mock.patch.object(install, "_require_ordinary_uid"),
              mock.patch.object(install.os.path, "exists", return_value=True),
              mock.patch.object(install.os, "access", return_value=True),
              mock.patch.object(install, "_duplicate_vhost_fd", return_value=77),
              mock.patch.object(install, "_wait_for_swtpm_socket"),
              mock.patch.object(install.os, "close")):
            result = install.run_fresh_install(
                self.run_root, self._manifest(), 12, popen=popen,
                qmp_factory=lambda _path, _deadline, _connect_deadline: qmp,
                monotonic=lambda: now[0], sleep=request_stop, stop_fd=read_fd)

        self.assertTrue(result["stop_requested"])
        self.assertFalse(result["native_acceptance_credit"])
        self.assertIn(("system_powerdown", None), qmp.calls)
        os.write(write_fd, b"still-open")
        self.assertEqual(os.read(read_fd, 64), b"stopstill-open")


if __name__ == "__main__":
    unittest.main()
