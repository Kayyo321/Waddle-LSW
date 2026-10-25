"""Bounded supervisor for a prepared, disposable Windows install guest.

Importing this module has no host effects. The sole launch API requires an
already-open vhost-vsock descriptor and starts only exact Popen handles owned
by the caller. It is intentionally unavailable through the prepare CLI.
"""

from __future__ import annotations

import fcntl
import json
import math
import os
from pathlib import Path
import select
import socket
import stat
import subprocess
import time
from typing import Any, Callable, Sequence

InstallDeadlineSeconds = 30 * 60
ShutdownGraceSeconds = 90
KillWaitSeconds = 5
QmpLineLimitBytes = 65536
VhostMajor = 10
VhostMinor = 241


class fresh_install_error_t(RuntimeError):
    """A supervisor failure; the private run and its evidence are retained."""


class qmp_client_t:
    """Minimal bounded QMP client for one private QEMU endpoint."""

    def __init__(self, socket_path: Path, deadline: float,
                 monotonic: Callable[[], float] = time.monotonic,
                 connect_deadline: float | None = None) -> None:
        self._socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._deadline = deadline
        self._active_deadline = min(deadline, connect_deadline) if connect_deadline is not None else deadline
        self._monotonic = monotonic
        self._buffer = bytearray()
        self._next_id = 0
        try:
            self._set_remaining_timeout()
            self._socket.connect(str(socket_path))
            greeting = self._read_line()
            if not isinstance(greeting.get("QMP"), dict):
                raise fresh_install_error_t("QMP greeting is invalid")
            self.execute("qmp_capabilities")
            self._active_deadline = deadline
        except BaseException:
            self._socket.close()
            raise

    def _set_remaining_timeout(self) -> float:
        remaining = self._active_deadline - self._monotonic()
        if remaining <= 0:
            raise fresh_install_error_t("overall install deadline expired during QMP")
        self._socket.settimeout(remaining)
        return remaining

    def _read_line(self) -> dict[str, Any]:
        while b"\n" not in self._buffer:
            if len(self._buffer) >= QmpLineLimitBytes:
                raise fresh_install_error_t("QMP response exceeded its line bound")
            self._set_remaining_timeout()
            block = self._socket.recv(min(4096, QmpLineLimitBytes - len(self._buffer)))
            if not block:
                raise fresh_install_error_t("QMP closed before a complete response")
            self._buffer.extend(block)
        line, _, rest = self._buffer.partition(b"\n")
        self._buffer = bytearray(rest)
        try:
            value = json.loads(line)
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise fresh_install_error_t("QMP returned malformed JSON") from error
        if not isinstance(value, dict):
            raise fresh_install_error_t("QMP returned an invalid response")
        return value

    def execute(self, command: str, arguments: dict[str, Any] | None = None, *,
                timeout_seconds: float | None = None) -> Any:
        previous_deadline = self._active_deadline
        if timeout_seconds is None:
            timeout_seconds = 5.0
        if timeout_seconds is not None:
            if (isinstance(timeout_seconds, bool) or not isinstance(timeout_seconds, (int, float)) or
                    not math.isfinite(timeout_seconds) or timeout_seconds <= 0):
                raise fresh_install_error_t("QMP command timeout is invalid")
            command_deadline = min(self._deadline, self._monotonic() + timeout_seconds)
            self._active_deadline = min(previous_deadline, command_deadline)
        command_id = self._next_id
        self._next_id += 1
        request: dict[str, Any] = {"execute": command}
        if arguments is not None:
            request["arguments"] = arguments
        request["id"] = command_id
        try:
            self._set_remaining_timeout()
            self._socket.sendall(json.dumps(request, separators=(",", ":")).encode() + b"\r\n")
            event_count = 0
            while True:
                response = self._read_line()
                if "event" in response:
                    event_count += 1
                    if event_count > 32:
                        raise fresh_install_error_t("QMP event stream exceeded its command bound")
                    continue
                if response.get("id") != command_id:
                    raise fresh_install_error_t("QMP response correlation failed")
                if "error" in response:
                    raise fresh_install_error_t(f"QMP {command} command failed")
                return response.get("return")
        finally:
            self._active_deadline = previous_deadline

    def close(self) -> None:
        self._socket.close()


def _make_qmp_client(socket_path: Path, deadline: float, connect_deadline: float) -> qmp_client_t:
    return qmp_client_t(socket_path, deadline, connect_deadline=connect_deadline)


def _duplicate_vhost_fd(source_fd: int) -> int:
    """Duplicate and verify vhost-vsock 10:241; caller owns returned fd."""
    if isinstance(source_fd, bool) or not isinstance(source_fd, int) or source_fd < 0:
        raise fresh_install_error_t("vhost-vsock descriptor is invalid")
    try:
        duplicate_fd = os.dup(source_fd)
    except OSError as error:
        raise fresh_install_error_t("cannot duplicate caller vhost-vsock descriptor") from error
    try:
        os.set_inheritable(duplicate_fd, False)
        info = os.fstat(duplicate_fd)
        if (not stat.S_ISCHR(info.st_mode) or
                (os.major(info.st_rdev), os.minor(info.st_rdev)) != (VhostMajor, VhostMinor) or
                fcntl.fcntl(duplicate_fd, fcntl.F_GETFL) & os.O_ACCMODE != os.O_RDWR):
            raise fresh_install_error_t("descriptor is not an open read/write vhost-vsock device")
        return duplicate_fd
    except BaseException:
        os.close(duplicate_fd)
        raise


def _write_manifest(run_root: Path, record: dict[str, Any]) -> None:
    """Atomically write secret-free supervisor metadata with private mode."""
    temporary = run_root / ".install-manifest.tmp"
    target = run_root / "install-manifest.json"
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as output:
            json.dump(record, output, indent=2, sort_keys=True)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, target)
        os.chmod(target, 0o600)
    except BaseException:
        try:
            os.close(fd)
        except OSError:
            pass
        try:
            temporary.unlink()
        except OSError:
            pass
        raise


def _require_private_run(run_root: Path) -> Path:
    if run_root.is_symlink():
        raise fresh_install_error_t("install run directory must not be a symlink")
    root = run_root.resolve(strict=True)
    if not root.is_dir() or stat.S_IMODE(root.stat().st_mode) != 0o700:
        raise fresh_install_error_t("install run directory must be a mode-0700 directory")
    return root


def _require_ordinary_uid() -> None:
    if os.geteuid() == 0 or os.geteuid() != os.getuid():
        raise fresh_install_error_t("install supervisor requires the ordinary, non-root caller UID")


def _launch_child(argv: Sequence[str], log_path: Path, *, pass_fds: tuple[int, ...] = (),
                  popen: Callable[..., Any] = subprocess.Popen) -> tuple[Any, Any]:
    fd = os.open(log_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        os.fchmod(fd, 0o600)
        log_file = os.fdopen(fd, "wb")
    except BaseException:
        try:
            os.close(fd)
        except OSError:
            pass
        try:
            log_path.unlink()
        except OSError:
            pass
        raise
    try:
        child = popen(list(argv), stdin=subprocess.DEVNULL, stdout=log_file,
                      stderr=subprocess.STDOUT, close_fds=True, pass_fds=pass_fds)
        return child, log_file
    except BaseException:
        try:
            log_file.close()
        except OSError:
            pass
        try:
            log_path.unlink()
        except OSError:
            pass
        raise


def _wait_for_swtpm_socket(socket_path: Path, child: Any, *, deadline: float,
                           monotonic: Callable[[], float],
                           sleep: Callable[[float], None],
                           stop_poller: Any | None = None) -> bool:
    """Wait for the exact swtpm child to create its private Unix socket."""
    ready_deadline = min(deadline, monotonic() + 10.0)
    while monotonic() < ready_deadline:
        if stop_poller is not None and stop_poller.poll(0):
            return False
        if child.poll() is not None:
            raise fresh_install_error_t("owned swtpm child exited before becoming ready")
        try:
            if socket_path.is_socket() and not socket_path.is_symlink():
                return True
        except OSError:
            pass
        sleep(min(0.1, max(0.0, ready_deadline - monotonic())))
    raise fresh_install_error_t("owned swtpm socket did not become ready")


def _cleanup_child(child: Any, label: str, record: dict[str, Any], *,
                   grace_seconds: float, kill_wait_seconds: float) -> bool:
    """Boundedly stop and reap one exact Popen object, recording each failure."""
    try:
        if child.poll() is not None:
            record[f"{label}_reaped"] = True
            return True
    except BaseException as error:
        record[f"{label}_poll_error"] = type(error).__name__
    try:
        child.wait(timeout=grace_seconds)
        record[f"{label}_reaped"] = True
        return True
    except subprocess.TimeoutExpired:
        pass
    except BaseException as error:
        record[f"{label}_wait_error"] = type(error).__name__
    try:
        child.terminate()
        record[f"{label}_terminate_requested"] = True
    except BaseException as error:
        record[f"{label}_terminate_error"] = type(error).__name__
    try:
        child.wait(timeout=kill_wait_seconds)
        record[f"{label}_reaped"] = True
        return True
    except subprocess.TimeoutExpired:
        pass
    except BaseException as error:
        record[f"{label}_post_terminate_wait_error"] = type(error).__name__
    try:
        child.kill()
        record[f"{label}_kill_requested"] = True
    except BaseException as error:
        record[f"{label}_kill_error"] = type(error).__name__
    try:
        child.wait(timeout=kill_wait_seconds)
        record[f"{label}_reaped"] = True
        return True
    except BaseException as error:
        record[f"{label}_final_wait_error"] = type(error).__name__
    try:
        reaped = child.poll() is not None
    except BaseException as error:
        record[f"{label}_final_poll_error"] = type(error).__name__
        reaped = False
    record[f"{label}_reaped"] = reaped
    if not reaped:
        record[f"{label}_unreaped"] = True
    return reaped


def _argv_from_manifest(manifest: dict[str, Any], run_root: Path,
                        vhost_fd: int) -> tuple[list[str], list[str]]:
    if manifest.get("run_root") != str(run_root):
        raise fresh_install_error_t("preparation manifest belongs to another run directory")
    qemu_template = manifest.get("qemu_argv_template")
    swtpm_argv = manifest.get("swtpm_argv")
    marker = "CALLER_VALIDATED_VHOST_VSOCK_FD"
    if not isinstance(qemu_template, list) or not all(isinstance(arg, str) for arg in qemu_template):
        raise fresh_install_error_t("prepared QEMU argv template is missing")
    if not isinstance(swtpm_argv, list) or not all(isinstance(arg, str) for arg in swtpm_argv):
        raise fresh_install_error_t("prepared swtpm argv is missing")
    if sum(arg.count(marker) for arg in qemu_template) != 1:
        raise fresh_install_error_t("prepared QEMU argv has an invalid vhost descriptor marker")
    qemu = [arg.replace(marker, str(vhost_fd)) for arg in qemu_template]
    for argv in (qemu, swtpm_argv):
        if any("\x00" in arg for arg in argv):
            raise fresh_install_error_t("prepared process argument contains a control byte")
    if f"unix:{run_root / 'qmp.sock'},server=on,wait=off" not in qemu:
        raise fresh_install_error_t("QMP endpoint does not belong to the private run")
    expected_private_args = (
        f"if=pflash,format=raw,file={run_root / 'vars.fd'}",
        f"if=none,id=windows,format=qcow2,file={run_root / 'windows.qcow2'}",
        f"if=none,id=answer,media=cdrom,readonly=on,format=raw,file={run_root / 'answer.iso'}",
        f"socket,id=chrtpm,path={run_root / 'tpm.sock'}",
        str(run_root / "qemu.log"),
    )
    if any(arg not in qemu for arg in expected_private_args):
        raise fresh_install_error_t("QEMU argv does not use only this run's private writable state")
    if qemu.count("-nic") != 1 or qemu[qemu.index("-nic") + 1] != "none":
        raise fresh_install_error_t("QEMU argv must disable guest networking")
    prohibited = {"-netdev", "-usb", "-virtfs", "-smb", "-spice", "-vnc", "-incoming"}
    if prohibited.intersection(qemu):
        raise fresh_install_error_t("QEMU argv contains a prohibited host integration")
    expected_tpm_tail = ["socket", "--tpm2", "--tpmstate", f"dir={run_root / 'tpm'}",
                         "--ctrl", f"type=unixio,path={run_root / 'tpm.sock'}"]
    if swtpm_argv[1:] != expected_tpm_tail:
        raise fresh_install_error_t("swtpm argv does not use this run's private TPM state")
    return qemu, list(swtpm_argv)


def run_fresh_install(run_root: Path, manifest: dict[str, Any], source_vhost_fd: int, *,
                      popen: Callable[..., Any] = subprocess.Popen,
                      qmp_factory: Callable[[Path, float, float], Any] = _make_qmp_client,
                      monotonic: Callable[[], float] = time.monotonic,
                      sleep: Callable[[float], None] = time.sleep,
                      screenshot_interval_seconds: float = 60.0,
                      stop_fd: int | None = None) -> dict[str, Any]:
    """Install until guest exit or the 30-minute deadline, then reap owned children.

    The caller retains source_vhost_fd; this function owns and closes a
    validated duplicate. It creates no process group and signals only returned
    Popen objects. Failure manifests and all private artifacts remain in place.
    """
    _require_ordinary_uid()
    if not os.path.exists("/dev/kvm") or not os.access("/dev/kvm", os.R_OK | os.W_OK):
        raise fresh_install_error_t("caller lacks read/write access to /dev/kvm")
    if (isinstance(screenshot_interval_seconds, bool) or
            not isinstance(screenshot_interval_seconds, (int, float)) or
            not math.isfinite(screenshot_interval_seconds) or
            screenshot_interval_seconds <= 0 or screenshot_interval_seconds > 300):
        raise fresh_install_error_t("screenshot interval is outside supported bounds")
    stop_poller = None
    if stop_fd is not None:
        if isinstance(stop_fd, bool) or not isinstance(stop_fd, int) or stop_fd < 0:
            raise fresh_install_error_t("stop notification descriptor is invalid")
        try:
            if fcntl.fcntl(stop_fd, fcntl.F_GETFL) & os.O_NONBLOCK == 0:
                raise fresh_install_error_t("stop notification descriptor must be nonblocking")
            stop_poller = select.poll()
            stop_poller.register(stop_fd, select.POLLIN | select.POLLHUP | select.POLLERR)
        except (OSError, ValueError) as error:
            raise fresh_install_error_t("cannot validate caller stop notification descriptor") from error
    root = _require_private_run(run_root)
    # Validate prepared metadata before taking ownership of a descriptor.
    _argv_from_manifest(manifest, root, 1)
    owned_fd = _duplicate_vhost_fd(source_vhost_fd)
    qemu_child: Any | None = None
    tpm_child: Any | None = None
    qemu_log: Any | None = None
    tpm_log: Any | None = None
    qmp: Any | None = None
    qmp_usable = True
    record: dict[str, Any] = {
        "run_root": str(root), "install_deadline_seconds": InstallDeadlineSeconds,
        "started": False, "deadline_reached": False, "acpi_requested": False,
        "forced_qemu_termination": False, "qemu_reaped": False,
        "swtpm_reaped": False, "failure": None, "screenshots": [],
        "guest_booted": False, "native_acceptance_credit": False,
        "secret_contents_recorded": False, "answer_media_hash_recorded": False,
        "stop_requested": False,
    }
    try:
        qemu_argv, tpm_argv = _argv_from_manifest(manifest, root, owned_fd)
        # KVM is explicit and QEMU receives only this validated duplicate.
        if not any("accel=kvm" in arg for arg in qemu_argv):
            raise fresh_install_error_t("prepared QEMU argv does not require KVM")
        # One absolute deadline covers launch, readiness, QMP, observation and
        # screenshots. Shutdown/reap has its own fixed cleanup allowance.
        deadline = monotonic() + InstallDeadlineSeconds
        cleanup_deadline = deadline + ShutdownGraceSeconds + (2 * KillWaitSeconds) + 10.0
        tpm_child, tpm_log = _launch_child(tpm_argv, root / "install-swtpm.log", popen=popen)
        record["swtpm_started"] = True
        _wait_for_swtpm_socket(root / "tpm.sock", tpm_child, deadline=deadline,
                               monotonic=monotonic, sleep=sleep,
                               stop_poller=stop_poller)
        if stop_poller is not None and stop_poller.poll(0):
            record["stop_requested"] = True
            raise fresh_install_error_t("install stop was requested before QEMU launch")
        if monotonic() >= deadline:
            raise fresh_install_error_t("install deadline expired before QEMU launch")
        qemu_child, qemu_log = _launch_child(qemu_argv, root / "install-qemu.log",
                                             pass_fds=(owned_fd,), popen=popen)
        record["qemu_started"] = True
        record["started"] = True
        _write_manifest(root, record)
        next_screenshot = monotonic()
        qmp_deadline = min(deadline, monotonic() + 30.0)
        while qemu_child.poll() is None and monotonic() < qmp_deadline:
            if stop_poller is not None and stop_poller.poll(0):
                record["stop_requested"] = True
            try:
                qmp = qmp_factory(root / "qmp.sock", cleanup_deadline, qmp_deadline)
                break
            except (OSError, TimeoutError):
                sleep(0.25)
        if qmp is None:
            raise fresh_install_error_t("private QMP endpoint did not become available")
        record["qmp_connected"] = True
        while qemu_child.poll() is None and monotonic() < deadline:
            if tpm_child.poll() is not None:
                raise fresh_install_error_t("owned swtpm child exited during installation")
            if stop_poller is not None and stop_poller.poll(0):
                record["stop_requested"] = True
                break
            now = monotonic()
            if not qmp_usable:
                sleep(min(1.0, max(0.0, deadline - now)))
                continue
            if now >= next_screenshot:
                screenshot = root / f"install-screen-{len(record['screenshots']) + 1:03d}.ppm"
                try:
                    qmp.execute("screendump", {"filename": str(screenshot)}, timeout_seconds=1.0)
                    if screenshot.is_file() and not screenshot.is_symlink():
                        os.chmod(screenshot, 0o600)
                        record["screenshots"].append(screenshot.name)
                except Exception:
                    record["screenshot_errors"] = int(record.get("screenshot_errors", 0)) + 1
                    qmp_usable = False
                    record["qmp_poisoned"] = True
                next_screenshot = now + float(screenshot_interval_seconds)
            sleep(min(1.0, max(0.0, deadline - now)))
        if record["stop_requested"] and qemu_child.poll() is None and qmp_usable:
            try:
                qmp.execute("system_powerdown", timeout_seconds=5.0)
                record["acpi_requested"] = True
            except BaseException as error:
                record["acpi_request_error"] = type(error).__name__
                qmp_usable = False
        if qemu_child.poll() is not None and monotonic() < deadline and not record["stop_requested"]:
            record["qemu_exit_code"] = qemu_child.returncode
            raise fresh_install_error_t("owned QEMU child exited before the install deadline")
        record["deadline_reached"] = qemu_child.poll() is None and monotonic() >= deadline
        if qemu_child.poll() is None and not record["acpi_requested"] and qmp_usable:
            try:
                qmp.execute("system_powerdown")
                record["acpi_requested"] = True
            except Exception:
                record["acpi_request_failed"] = True
                qmp_usable = False
                record["qmp_poisoned"] = True
    except BaseException as error:
        record["failure"] = type(error).__name__
    finally:
        qemu_running = False
        if qemu_child is not None:
            try:
                qemu_running = qemu_child.poll() is None
            except BaseException as error:
                record["qemu_cleanup_poll_error"] = type(error).__name__
                qemu_running = True
        if qemu_running and not record["acpi_requested"]:
            if qmp is not None and not qmp_usable:
                try:
                    qmp.close()
                except BaseException as error:
                    record["qmp_poisoned_close_error"] = type(error).__name__
                qmp = None
            if qmp is None:
                try:
                    qmp = qmp_factory(root / "qmp.sock", cleanup_deadline,
                                      min(cleanup_deadline, monotonic() + 5.0))
                    qmp_usable = True
                    record["qmp_reconnected_for_cleanup"] = True
                except BaseException as error:
                    record["qmp_reconnect_error"] = type(error).__name__
            if qmp is not None:
                try:
                    qmp.execute("system_powerdown", timeout_seconds=5.0)
                    record["acpi_requested"] = True
                except BaseException as error:
                    record["acpi_request_error"] = type(error).__name__
                    qmp_usable = False
        if qmp is not None:
            try:
                qmp.close()
            except BaseException as error:
                record["qmp_close_error"] = type(error).__name__
        if qemu_child is not None:
            qemu_reaped = _cleanup_child(qemu_child, "qemu", record,
                                          grace_seconds=ShutdownGraceSeconds,
                                          kill_wait_seconds=KillWaitSeconds)
            if record.get("qemu_terminate_requested") or record.get("qemu_kill_requested"):
                record["forced_qemu_termination"] = True
            record["qemu_reaped"] = qemu_reaped
        if tpm_child is not None:
            record["swtpm_reaped"] = _cleanup_child(
                tpm_child, "swtpm", record, grace_seconds=0,
                kill_wait_seconds=KillWaitSeconds)
        for label, log_handle in (("qemu", qemu_log), ("swtpm", tpm_log)):
            if log_handle is not None:
                try:
                    log_handle.close()
                    record[f"{label}_log_closed"] = True
                except BaseException as error:
                    record[f"{label}_log_close_error"] = type(error).__name__
        try:
            os.close(owned_fd)
            record["owned_vhost_fd_closed"] = True
        except BaseException as error:
            record["owned_vhost_fd_close_error"] = type(error).__name__
        try:
            private_outputs = (root / "install-qemu.log", root / "install-swtpm.log",
                               root / "qemu.log", *root.glob("install-screen-*.ppm"))
        except BaseException as error:
            record["output_scan_error"] = type(error).__name__
            private_outputs = ()
        for private_output in private_outputs:
            try:
                if private_output.is_file() and not private_output.is_symlink():
                    os.chmod(private_output, 0o600)
            except BaseException as error:
                record["output_mode_fix_error"] = type(error).__name__
        try:
            _write_manifest(root, record)
        except BaseException as error:
            record["manifest_write_error"] = type(error).__name__
    cleanup_failed = any(key.endswith("_error") or key.endswith("_unreaped")
                         for key in record)
    if (record.get("failure") or cleanup_failed or not record["qemu_reaped"] or
            not record["swtpm_reaped"]):
        raise fresh_install_error_t("fresh installation supervisor failed; private run retained")
    return record
