"""Isolated, test-only Windows guest runner for native acceptance work.

This module never uses the provisioner or registered devices.  Every writable
guest asset belongs to a new private run directory; children are supervised by
the exact ``Popen`` objects created here.  A forced stop is recorded as such
and is not evidence of guest quiescence or interop release.
"""

from __future__ import annotations

import dataclasses
import errno
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import signal
import socket
import stat
import subprocess
import tempfile
import threading
import time
from typing import Any, Callable, Sequence
import uuid


Root = Path(__file__).resolve().parents[2]
DefaultBaseDir = Root / "build" / "windows11_vm"
DefaultQemuBuildDir = Root / "build" / "qemu-seamless-source"
DefaultOvmfCode = Path("/usr/share/OVMF/OVMF_CODE_4M.secboot.fd")
MaxQmpLineBytes = 65536
MaxResultBytes = 65536
MaxPayloadBytes = 64 * 1024 * 1024
ResultPort = 54321
MaxPreparationCommandSeconds = 30.0
ForcedCleanupBudgetSeconds = 8.0
LeaseSafetyMarginSeconds = 5.0
LeasePollSeconds = 0.1


class base_read_lease_t:
    """Own a Linux read lease on the canonical base image until child teardown.

    The lease is acquired on the main thread and is the writer-exclusion
    authority. The SIGIO handler only records notification; callers poll
    ``check`` at bounded intervals and perform all cleanup synchronously.
    """

    def __init__(self) -> None:
        self.fd: int | None = None
        self.path: Path | None = None
        self.break_pending = False
        self.signal_seen = False
        self.acquired = False
        self.released = False
        self.break_time_seconds: int | None = None
        self.last_kernel_lease: int | None = None
        self._old_sigio_handler: Any = None
        self._handler_installed = False
        self._owner_thread = threading.main_thread().ident

    @staticmethod
    def _record_sigio(_signum: int, _frame: Any) -> None:
        lease = getattr(_active_base_lease, "current", None)
        if lease is not None:
            lease.signal_seen = True
            lease.break_pending = True

    def acquire(self, path: Path) -> None:
        """Acquire the read lease, refusing unsupported or conflicting hosts."""
        if threading.get_ident() != self._owner_thread:
            raise runner_error_t("base read lease must be acquired on the main thread")
        if self.fd is not None or self.acquired or self.released:
            raise runner_error_t("base read lease is single-use")
        if signal.getsignal(signal.SIGIO) != signal.SIG_DFL:
            raise runner_error_t("refusing to replace a non-default SIGIO handler")
        try:
            break_time_text = Path("/proc/sys/fs/lease-break-time").read_text(encoding="ascii").strip()
            break_time = int(break_time_text)
        except (OSError, UnicodeError, ValueError) as error:
            raise runner_error_t(f"cannot read Linux lease break-time: {error}") from error
        required = math.ceil(MaxPreparationCommandSeconds + ForcedCleanupBudgetSeconds + LeaseSafetyMarginSeconds)
        if break_time < required:
            raise runner_error_t(f"Linux lease break-time {break_time}s is below required {required}s margin")
        canonical = path.resolve(strict=True)
        info = canonical.stat()
        if not stat.S_ISREG(info.st_mode):
            raise runner_error_t("base disk lease target must be a regular file")
        fd = -1
        installed = False
        try:
            fd = os.open(canonical, os.O_RDONLY | os.O_CLOEXEC)
            opened = os.fstat(fd)
            if (opened.st_dev, opened.st_ino) != (info.st_dev, info.st_ino):
                raise runner_error_t("base disk identity changed while opening read lease")
            self.path = canonical
            self.fd = fd
            self.break_time_seconds = break_time
            self._old_sigio_handler = signal.getsignal(signal.SIGIO)
            _active_base_lease.current = self
            signal.signal(signal.SIGIO, self._record_sigio)
            self._handler_installed = True
            installed = True
            fcntl.fcntl(fd, fcntl.F_SETOWN, os.getpid())
            fcntl.fcntl(fd, fcntl.F_SETLEASE, fcntl.F_RDLCK)
            kernel_lease = fcntl.fcntl(fd, fcntl.F_GETLEASE)
            self.last_kernel_lease = kernel_lease
            if kernel_lease != fcntl.F_RDLCK or self.break_pending:
                raise runner_error_t("kernel did not retain the base disk read lease")
            self.acquired = True
        except BaseException as error:
            if fd >= 0:
                try:
                    if fcntl.fcntl(fd, fcntl.F_GETLEASE) == fcntl.F_RDLCK:
                        fcntl.fcntl(fd, fcntl.F_SETLEASE, fcntl.F_UNLCK)
                except OSError:
                    pass
                try:
                    os.close(fd)
                except OSError:
                    pass
            self.fd = None
            if installed:
                signal.signal(signal.SIGIO, self._old_sigio_handler)
                _active_base_lease.current = None
                self._handler_installed = False
            if isinstance(error, runner_error_t):
                raise
            raise runner_error_t(f"cannot acquire kernel base read lease: {error}") from error

    def check(self) -> None:
        """Fail closed when SIGIO or F_GETLEASE reports a lease break."""
        if self.fd is None or not self.acquired or self.released:
            raise runner_error_t("base read lease is not active")
        if threading.get_ident() != self._owner_thread:
            raise runner_error_t("base read lease must be checked on the main thread")
        try:
            current = fcntl.fcntl(self.fd, fcntl.F_GETLEASE)
        except OSError as error:
            self.break_pending = True
            raise runner_error_t(f"cannot query base read lease: {error}") from error
        self.last_kernel_lease = current
        if self.break_pending or current != fcntl.F_RDLCK:
            self.break_pending = True
            raise runner_error_t("base disk read lease was broken or is being broken")

    def snapshot(self) -> dict[str, Any]:
        return {"acquired": self.acquired, "broken": self.break_pending,
                "sigio_seen": self.signal_seen, "released": self.released,
                "break_time_seconds": self.break_time_seconds,
                "last_kernel_lease": self.last_kernel_lease,
                "fd_owned": self.fd is not None}

    def release(self) -> None:
        """Drop only this lease, close its fd, then restore SIGIO handling."""
        if self.fd is None:
            if self._handler_installed:
                signal.signal(signal.SIGIO, self._old_sigio_handler)
                _active_base_lease.current = None
                self._handler_installed = False
            return
        if threading.get_ident() != self._owner_thread:
            raise runner_error_t("base read lease must be released on the main thread")
        fd = self.fd
        unlock_error: OSError | None = None
        try:
            if fcntl.fcntl(fd, fcntl.F_GETLEASE) == fcntl.F_RDLCK:
                fcntl.fcntl(fd, fcntl.F_SETLEASE, fcntl.F_UNLCK)
        except OSError as error:
            unlock_error = error
        try:
            os.close(fd)
        except OSError as error:
            unlock_error = unlock_error or error
        self.fd = None
        self.released = True
        if self._handler_installed:
            signal.signal(signal.SIGIO, self._old_sigio_handler)
            _active_base_lease.current = None
            self._handler_installed = False
        if unlock_error is not None:
            raise runner_error_t(f"base read lease release required fd close fallback: {unlock_error}") from unlock_error


class _active_base_lease_t:
    current: base_read_lease_t | None = None


_active_base_lease = _active_base_lease_t()


class runner_error_t(RuntimeError):
    """Raised when preflight or an owned child fails; run evidence is retained."""


@dataclasses.dataclass(frozen=True)
class runner_config_t:
    """Caller-selected bounded run settings.

    ``base_dir`` is read-only input.  ``run_parent`` receives a fresh private
    run directory.  ``run_seconds`` bounds the observation period after QMP
    becomes available; startup and shutdown each have separate finite limits.
    """

    base_dir: Path = DefaultBaseDir
    qemu_build_dir: Path = DefaultQemuBuildDir
    run_parent: Path = Path("/tmp")
    run_seconds: float = 60.0
    startup_seconds: float = 45.0
    shutdown_seconds: float = 30.0
    memory_mib: int = 4096
    cpus: int = 2
    payload_files: tuple[Path, ...] = ()
    expected_payload_hashes: tuple[tuple[str, str], ...] = ()
    enable_test_result_channel: bool = False


@dataclasses.dataclass(frozen=True)
class file_fingerprint_t:
    """Stable identity for a protected regular file or tree entry."""

    path: str
    device: int
    inode: int
    size: int
    mtime_ns: int
    ctime_ns: int
    mode: int
    sha256: str | None


def _sha256_file(path: Path) -> str:
    """Return a streaming SHA-256 digest for one regular file."""
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            block = stream.read(1024 * 1024)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def _fingerprint(path: Path, *, hash_contents: bool = True) -> file_fingerprint_t:
    """Capture lstat identity and, when requested, a content digest."""
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode):
        raise runner_error_t(f"protected input is not a regular file: {path}")
    return file_fingerprint_t(
        str(path.resolve(strict=True)), info.st_dev, info.st_ino, info.st_size,
        info.st_mtime_ns, info.st_ctime_ns, stat.S_IMODE(info.st_mode),
        _sha256_file(path) if hash_contents else None,
    )


def _tree_fingerprints(root: Path) -> list[file_fingerprint_t]:
    if not root.is_dir() or root.is_symlink():
        raise runner_error_t(f"protected state directory is missing or unsafe: {root}")
    result: list[file_fingerprint_t] = []
    for path in sorted(root.iterdir()):
        if path.name == ".lock":
            # The lock file is not copied, but its presence and contents remain
            # part of the protected-source verification.
            pass
        if path.is_symlink() or not path.is_file():
            raise runner_error_t(f"TPM source contains a non-regular entry: {path}")
        result.append(_fingerprint(path))
    if not result:
        raise runner_error_t(f"protected TPM state is empty: {root}")
    return result


def _protected_snapshot(base_dir: Path) -> dict[str, Any]:
    """Capture disk metadata plus exact UEFI and TPM source content hashes."""
    if base_dir.is_symlink():
        raise runner_error_t("protected base directory must not be a symlink")
    base_dir = base_dir.resolve(strict=True)
    disk = _fingerprint(base_dir / "windows.qcow2", hash_contents=False)
    vars_file = _fingerprint(base_dir / "vars.fd")
    tpm = _tree_fingerprints(base_dir / "tpm")
    return {"base_disk": dataclasses.asdict(disk), "vars": dataclasses.asdict(vars_file),
            "tpm": [dataclasses.asdict(entry) for entry in tpm]}


def _verify_snapshot(base_dir: Path, expected: dict[str, Any]) -> None:
    """Reject any protected input whose identity or content changed."""
    actual = _protected_snapshot(base_dir)
    if actual != expected:
        raise runner_error_t("a protected base disk, UEFI vars, or TPM source changed during the run")


def _process_mount_devices(process_dir: Path) -> dict[int, tuple[int, int]]:
    """Map a process mount ID to its major/minor device identity."""
    try:
        lines = (process_dir / "mountinfo").read_text(encoding="ascii").splitlines()
    except (FileNotFoundError, ProcessLookupError):
        raise
    except (OSError, UnicodeError) as error:
        raise runner_error_t(f"cannot inspect same-UID process mount table: {process_dir}") from error
    devices: dict[int, tuple[int, int]] = {}
    for line in lines:
        fields = line.split()
        if len(fields) < 3:
            continue
        try:
            mount_id = int(fields[0])
            major, minor = (int(part) for part in fields[2].split(":"))
        except (ValueError, TypeError):
            continue
        devices[mount_id] = (major, minor)
    return devices


def _fdinfo_identity(fdinfo_path: Path, mount_devices: dict[int, tuple[int, int]]) -> tuple[int, tuple[int, int], int]:
    """Return the file inode, device pair, and open flags from Linux fdinfo."""
    try:
        fields = {}
        for line in fdinfo_path.read_text(encoding="ascii").splitlines():
            name, separator, value = line.partition(":")
            if separator:
                fields[name] = value.strip()
        inode = int(fields["ino"])
        mount_id = int(fields["mnt_id"])
        flags = int(fields["flags"], 8)
        device = mount_devices[mount_id]
    except (FileNotFoundError, ProcessLookupError):
        raise
    except (OSError, UnicodeError, KeyError, ValueError) as error:
        raise runner_error_t(f"cannot establish descriptor identity from {fdinfo_path}") from error
    return inode, device, flags


def _check_no_writable_base_holder(base_disk: Path, proc_root: Path = Path("/proc"),
                                   *, diagnostic_only: bool = False) -> list[dict[str, int | str]]:
    """Report /proc visibility and any writable holders of the base disk.

    With ``diagnostic_only``, inaccessible proc entries are recorded and
    skipped; the independently acquired kernel lease remains authoritative.
    A visible writable descriptor or mapping always raises. Matching device
    and inode avoids trusting a path string that may have been renamed.
    """
    target = base_disk.stat()
    proc = proc_root
    skipped: list[dict[str, int | str]] = []
    if not proc.is_dir():
        if diagnostic_only:
            return [{"pid": -1, "uid": -1, "reason": "/proc unavailable; kernel lease is authority"}]
        raise runner_error_t("cannot verify that the base disk has no writable holder")
    for process_dir in proc.iterdir():
        if not process_dir.name.isdecimal():
            continue
        try:
            process_uid = process_dir.stat().st_uid
        except (FileNotFoundError, ProcessLookupError):
            continue
        except OSError as error:
            if diagnostic_only:
                skipped.append({"pid": int(process_dir.name), "uid": -1,
                                "reason": f"process owner unavailable: {error}"})
                continue
            raise runner_error_t(f"cannot identify process owner while checking base disk: {process_dir}: {error}") from error
        inspectable_uid = process_uid == os.geteuid()
        if not inspectable_uid:
            skipped.append({"pid": int(process_dir.name), "uid": process_uid,
                            "reason": "foreign uid outside runner inspection scope"})
        fd_dir = process_dir / "fd"
        try:
            descriptors = list(fd_dir.iterdir())
        except (FileNotFoundError, ProcessLookupError):
            continue
        except PermissionError as error:
            if inspectable_uid:
                if not diagnostic_only:
                    raise runner_error_t(f"cannot inspect current-uid process descriptors: {process_dir}") from error
                skipped.append({"pid": int(process_dir.name), "uid": process_uid,
                                "reason": "same-uid descriptor scan inaccessible; kernel lease is authority"})
                continue
            skipped[-1]["reason"] = "foreign uid descriptor scan inaccessible"
            continue
        for descriptor in descriptors:
            try:
                info = descriptor.stat()
                matches_base = (info.st_dev, info.st_ino) == (target.st_dev, target.st_ino)
            except (FileNotFoundError, ProcessLookupError):
                continue
            except PermissionError as error:
                if not inspectable_uid:
                    skipped[-1]["reason"] = "foreign uid descriptor identity inaccessible"
                    continue
                try:
                    mount_devices = _process_mount_devices(process_dir)
                    inode, device, flags = _fdinfo_identity(process_dir / "fdinfo" / descriptor.name,
                                                           mount_devices)
                except (FileNotFoundError, ProcessLookupError):
                    continue
                except runner_error_t as error:
                    if not diagnostic_only:
                        raise
                    skipped.append({"pid": int(process_dir.name), "uid": process_uid,
                                    "reason": f"fdinfo unavailable; kernel lease is authority: {error}"})
                    continue
                matches_base = inode == target.st_ino and device == (os.major(target.st_dev), os.minor(target.st_dev))
                if not matches_base:
                    continue
            if matches_base:
                try:
                    mount_devices = _process_mount_devices(process_dir)
                    inode, device, flags = _fdinfo_identity(process_dir / "fdinfo" / descriptor.name,
                                                           mount_devices)
                except (FileNotFoundError, ProcessLookupError):
                    continue
                if inode != target.st_ino or device != (os.major(target.st_dev), os.minor(target.st_dev)):
                    raise runner_error_t(f"base descriptor identity changed during scan: {descriptor}")
                if flags & os.O_ACCMODE != os.O_RDONLY:
                    raise runner_error_t(f"base disk has a writable open descriptor in process {process_dir.name}")
        maps_path = process_dir / "maps"
        try:
            maps = maps_path.read_text(encoding="utf-8", errors="replace")
        except (FileNotFoundError, ProcessLookupError):
            continue
        except PermissionError as error:
            if inspectable_uid:
                if not diagnostic_only:
                    raise runner_error_t(f"cannot inspect current-uid process mappings: {process_dir}") from error
                skipped.append({"pid": int(process_dir.name), "uid": process_uid,
                                "reason": "same-uid mapping scan inaccessible; kernel lease is authority"})
                continue
            skipped[-1]["reason"] = "foreign uid mapping scan inaccessible"
            continue
        for line in maps.splitlines():
            fields = line.split()
            if len(fields) < 5 or "w" not in fields[1]:
                continue
            try:
                device = tuple(int(part, 16) for part in fields[3].split(":"))
                inode = int(fields[4])
            except ValueError:
                continue
            if device == (os.major(target.st_dev), os.minor(target.st_dev)) and inode == target.st_ino:
                raise runner_error_t(f"base disk has a writable mapping in process {process_dir.name}")
    return skipped


def _run_bounded(argv: Sequence[str], *, timeout: float, env: dict[str, str] | None = None,
                 output_limit: int = 256 * 1024,
                 on_tick: Callable[[], None] | None = None) -> str:
    """Run a capability/preparation command with time and output bounds."""
    process: subprocess.Popen[bytes] | None = None
    selector = None
    output = bytearray()
    deadline = time.monotonic() + timeout
    try:
        process = subprocess.Popen(list(argv), stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, env=env, close_fds=True)
        assert process.stdout is not None
        import selectors
        selector = selectors.DefaultSelector()
        selector.register(process.stdout, selectors.EVENT_READ)
        eof = False
        while not eof or process.poll() is None:
            if on_tick is not None:
                on_tick()
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise runner_error_t(f"bounded command timed out: {argv[0]}")
            ready = selector.select(min(remaining, LeasePollSeconds if on_tick is not None else 0.2))
            for key, _events in ready:
                block = os.read(key.fd, min(8192, output_limit + 1 - len(output)))
                if not block:
                    selector.unregister(key.fileobj)
                    eof = True
                    continue
                output.extend(block)
                if len(output) > output_limit:
                    raise runner_error_t(f"command output exceeded {output_limit} bytes: {argv[0]}")
        return_code = process.wait(timeout=max(0.001, deadline - time.monotonic()))
        text_output = bytes(output).decode("utf-8", errors="replace")
        if return_code != 0:
            raise runner_error_t(f"command failed ({return_code}): {argv[0]}: {text_output[-2048:]}")
        return text_output
    except runner_error_t:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=1.0)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=1.0)
        raise
    except (OSError, subprocess.TimeoutExpired) as error:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=1.0)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=1.0)
        raise runner_error_t(f"bounded command failed: {argv[0]}: {error}") from error
    finally:
        if selector is not None:
            selector.close()
        if process is not None and process.stdout is not None:
            process.stdout.close()


def _validate_qemu(qemu: Path, stamp_path: Path) -> dict[str, Any]:
    """Verify the pinned QEMU build stamp, binary digest, version and features."""
    try:
        if qemu.is_symlink() or stamp_path.is_symlink():
            raise runner_error_t("QEMU binary and build stamp must not be symlinks")
        stamp = json.loads(stamp_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise runner_error_t(f"cannot read QEMU build provenance: {error}") from error
    inputs = stamp.get("inputs")
    if (stamp.get("schema") != 1 or stamp.get("binary") != qemu.name or
            not isinstance(stamp.get("binary_sha256"), str) or
            not isinstance(inputs, dict) or inputs.get("source_commit") != "b2995afed254e4804611df52210067a4d5dbdf0b" or
            inputs.get("schema") != 1 or not isinstance(stamp.get("configuration_sha256"), dict)):
        raise runner_error_t("QEMU build stamp is incomplete or not the required pinned 9.2.4 build")
    if _sha256_file(qemu) != stamp["binary_sha256"]:
        raise runner_error_t("QEMU binary hash does not match its build stamp")
    version = _run_bounded([str(qemu), "--version"], timeout=5.0, output_limit=4096)
    if "QEMU emulator version 9.2.4 " not in version:
        raise runner_error_t(f"unexpected QEMU version: {version.strip()}")
    accels = _run_bounded([str(qemu), "-accel", "help"], timeout=5.0, output_limit=16384)
    devices = _run_bounded([str(qemu), "-device", "help"], timeout=5.0, output_limit=256 * 1024)
    if "tcg" not in accels or "ivshmem-plain" not in devices:
        raise runner_error_t("pinned QEMU lacks required TCG or ivshmem-plain capability")
    return {"version": version.strip(), "sha256": stamp["binary_sha256"],
            "source_commit": inputs["source_commit"], "accelerators": accels.strip(),
            "stamp": str(stamp_path)}


def _reject_external_qcow2_storage(qemu_img: str, image: Path, expected_chain: list[Path],
                                   *, on_tick: Callable[[], None] | None = None) -> None:
    """Require the exact QCOW2 chain and reject every external data/backing file."""
    text = _run_bounded([qemu_img, "info", "--backing-chain", "--output=json", str(image)],
                        timeout=15.0, output_limit=128 * 1024, on_tick=on_tick)
    try:
        chain = json.loads(text)
    except json.JSONDecodeError as error:
        raise runner_error_t("qemu-img returned malformed backing-chain metadata") from error
    if not isinstance(chain, list) or len(chain) != len(expected_chain):
        raise runner_error_t("QCOW2 image has an unexpected or external backing chain")

    def check_external_fields(value: Any) -> None:
        if isinstance(value, dict):
            for key, child in value.items():
                if key in {"data-file", "full-data-file"} and child not in (None, ""):
                    raise runner_error_t("QCOW2 image uses an external data file")
                if key in {"backing-filename", "full-backing-filename"} and child not in (None, ""):
                    resolved = Path(str(child)).resolve()
                    if resolved not in [entry.resolve() for entry in expected_chain]:
                        raise runner_error_t("QCOW2 image references an unapproved backing file")
                check_external_fields(child)
        elif isinstance(value, list):
            for child in value:
                check_external_fields(child)

    for item, expected in zip(chain, expected_chain):
        if not isinstance(item, dict) or item.get("format") != "qcow2":
            raise runner_error_t("every image in the approved backing chain must be QCOW2")
        filename = item.get("filename")
        if not isinstance(filename, str) or Path(filename).resolve() != expected.resolve():
            raise runner_error_t("QCOW2 backing-chain path does not match the approved path")
        check_external_fields(item)


def _copy_private_file(source: Path, destination: Path, mode: int = 0o600) -> None:
    """Copy a regular source into a new exclusive private destination."""
    info = source.lstat()
    if not stat.S_ISREG(info.st_mode) or source.is_symlink():
        raise runner_error_t(f"refusing unsafe protected-state copy: {source}")
    with source.open("rb") as input_stream, destination.open("xb") as output_stream:
        shutil.copyfileobj(input_stream, output_stream, length=1024 * 1024)
        output_stream.flush()
        os.fsync(output_stream.fileno())
    os.chmod(destination, mode)


def _stage_payload(config: runner_config_t, run_root: Path, nonce: str | None,
                   *, on_tick: Callable[[], None] | None = None) -> dict[str, Any] | None:
    """Stage hash-verified payloads into a read-only ISO for explicit test runs."""
    if not config.payload_files:
        if config.enable_test_result_channel:
            raise runner_error_t("result channel requires at least one staged payload")
        return None
    expected = dict(config.expected_payload_hashes)
    if len(expected) != len(config.expected_payload_hashes):
        raise runner_error_t("duplicate expected payload hash names")
    stage = run_root / "payload"
    stage.mkdir(mode=0o700)
    records: list[dict[str, str | int]] = []
    total = 0
    reserved_names = {"waddle-run.json"} if nonce is not None else set()
    for supplied_source in config.payload_files:
        if supplied_source.is_symlink():
            raise runner_error_t(f"payload must be a regular non-symlink file: {supplied_source}")
        source = supplied_source.resolve(strict=True)
        if not source.is_file() or source.name in {".", ".."}:
            raise runner_error_t(f"payload must be a regular non-symlink file: {source}")
        if Path(source.name).name != source.name or any(ch in source.name for ch in "\r\n\x00"):
            raise runner_error_t(f"unsafe payload name: {source.name!r}")
        if source.name in reserved_names:
            raise runner_error_t(f"payload name is reserved by the test runner: {source.name}")
        if (stage / source.name).exists():
            raise runner_error_t(f"duplicate payload basename: {source.name}")
        size = source.stat().st_size
        total += size
        if total > MaxPayloadBytes:
            raise runner_error_t("staged payload exceeds the bounded ISO input size")
        digest = _sha256_file(source)
        if source.name not in expected or expected[source.name] != digest:
            raise runner_error_t(f"payload hash is missing or mismatched: {source.name}")
        shutil.copyfile(source, stage / source.name)
        os.chmod(stage / source.name, 0o444)
        if _sha256_file(stage / source.name) != digest:
            raise runner_error_t(f"staged payload hash changed during copy: {source.name}")
        records.append({"name": source.name, "size": size, "sha256": digest})
    if nonce is not None:
        request = stage / "waddle-run.json"
        request.write_text(json.dumps({"nonce": nonce, "result_port": ResultPort,
                                      "channel": "test-only guestfwd TCP"}) + "\n", encoding="utf-8")
        os.chmod(request, 0o444)
        records.append({"name": request.name, "size": request.stat().st_size,
                        "sha256": _sha256_file(request)})
    iso = run_root / "test-payload.iso"
    _run_bounded(["xorriso", "-as", "mkisofs", "-quiet", "-J", "-r", "-o", str(iso), str(stage)],
                 timeout=MaxPreparationCommandSeconds, output_limit=16384, on_tick=on_tick)
    os.chmod(iso, 0o444)
    return {"files": records, "iso": str(iso), "iso_sha256": _sha256_file(iso)}


class result_channel_t:
    """Bounded nonce-correlated JSON-line result receiver over a private AF_UNIX socket."""

    def __init__(self, run_root: Path, nonce: str | None = None) -> None:
        self.nonce = nonce or uuid.uuid4().hex
        self.path = run_root / "guest-result.sock"
        if len(str(self.path).encode()) >= 104:
            raise runner_error_t("private result socket path exceeds conservative Unix socket bound")
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(str(self.path))
        os.chmod(self.path, 0o600)
        self.listener.listen(1)
        self.listener.settimeout(0.2)

    def receive(self, timeout: float, *, on_tick: Callable[[], None] | None = None) -> dict[str, Any]:
        """Return one bounded matching-nonce JSON result before ``timeout``."""
        if not math.isfinite(timeout) or timeout <= 0:
            raise runner_error_t("result receive deadline must be finite and positive")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if on_tick is not None:
                on_tick()
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            self.listener.settimeout(min(LeasePollSeconds if on_tick is not None else 0.2, remaining))
            try:
                connection, _ = self.listener.accept()
            except TimeoutError:
                continue
            with connection:
                connection.settimeout(min(LeasePollSeconds, max(0.001, deadline - time.monotonic()))
                                      if on_tick is not None else max(0.001, deadline - time.monotonic()))
                data = bytearray()
                while len(data) <= MaxResultBytes:
                    if on_tick is not None:
                        on_tick()
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise runner_error_t("timed out receiving bounded guest result")
                    connection.settimeout(min(LeasePollSeconds, remaining) if on_tick is not None else remaining)
                    try:
                        block = connection.recv(min(4096, MaxResultBytes + 1 - len(data)))
                    except socket.timeout:
                        if on_tick is not None:
                            continue
                        raise
                    if not block:
                        break
                    data.extend(block)
                    if b"\n" in data:
                        break
                if len(data) > MaxResultBytes or b"\n" not in data:
                    raise runner_error_t("guest result was oversized or lacked its terminating newline")
                line, suffix = bytes(data).split(b"\n", 1)
                if suffix.strip():
                    raise runner_error_t("guest result contained trailing data")
                try:
                    result = json.loads(line.decode("utf-8"))
                except (UnicodeDecodeError, json.JSONDecodeError) as error:
                    raise runner_error_t(f"guest result is not valid JSON: {error}") from error
                if not isinstance(result, dict) or result.get("nonce") != self.nonce:
                    raise runner_error_t("guest result nonce or object shape is invalid")
                return result
        raise runner_error_t("timed out waiting for nonce-correlated guest result")

    def close(self) -> None:
        """Close and unlink this runner-owned private result endpoint."""
        self.listener.close()
        try:
            self.path.unlink()
        except FileNotFoundError:
            pass


class qmp_client_t:
    """Small bounded QMP client; command and response lines are capped."""

    def __init__(self, path: Path, timeout: float,
                 on_tick: Callable[[], None] | None = None) -> None:
        self.timeout = timeout
        self._on_tick = on_tick
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            deadline = time.monotonic() + timeout
            self.socket.settimeout(min(timeout, LeasePollSeconds) if on_tick is not None else timeout)
            self.socket.connect(str(path))
            self._read_line(deadline)
            self.execute("qmp_capabilities")
        except BaseException:
            self.socket.close()
            raise

    def _read_line(self, deadline: float) -> dict[str, Any]:
        if not hasattr(self, "_buffer"):
            self._buffer = bytearray()
        while len(self._buffer) <= MaxQmpLineBytes:
            if self._on_tick is not None:
                self._on_tick()
            if b"\n" in self._buffer:
                line, _, extra = self._buffer.partition(b"\n")
                self._buffer = bytearray(extra)
                return json.loads(line)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise runner_error_t("QMP response deadline expired")
            self.socket.settimeout(min(remaining, LeasePollSeconds) if self._on_tick is not None else remaining)
            try:
                block = self.socket.recv(4096)
            except socket.timeout:
                if self._on_tick is not None:
                    continue
                raise
            if not block:
                raise runner_error_t("QMP closed before a complete response")
            self._buffer.extend(block)
        raise runner_error_t("QMP response exceeded its line bound")

    def execute(self, command: str, arguments: dict[str, Any] | None = None,
                *, timeout: float | None = None) -> Any:
        """Issue one bounded QMP command and return its response value."""
        command_timeout = self.timeout if timeout is None else timeout
        if not math.isfinite(command_timeout) or command_timeout <= 0:
            raise runner_error_t("QMP command timeout must be finite and positive")
        request: dict[str, Any] = {"execute": command}
        if arguments is not None:
            request["arguments"] = arguments
        deadline = time.monotonic() + command_timeout
        if self._on_tick is not None:
            self._on_tick()
        self.socket.settimeout(min(command_timeout, LeasePollSeconds) if self._on_tick is not None else command_timeout)
        self.socket.sendall(json.dumps(request, separators=(",", ":")).encode() + b"\r\n")
        while True:
            response = self._read_line(deadline)
            if "event" in response:
                continue
            if "error" in response:
                raise runner_error_t(f"QMP {command} failed: {response['error']}")
            return response.get("return")

    def close(self) -> None:
        """Close the QMP connection without altering the VM lifecycle."""
        self.socket.close()


class native_guest_runner_t:
    """Create and run one isolated guest; never discovers or signals foreign PIDs."""

    def __init__(self, config: runner_config_t, *, popen: Callable[..., Any] = subprocess.Popen,
                 clock: Callable[[], float] = time.monotonic,
                 sleep: Callable[[float], None] = time.sleep) -> None:
        self.config = config
        self._popen = popen
        self._clock = clock
        self._sleep = sleep
        self.run_root: Path | None = None
        self.children: list[tuple[str, Any]] = []
        self._forced_shutdown = False
        self._qmp: qmp_client_t | None = None
        self._result_channel: result_channel_t | None = None
        self._base_lease = base_read_lease_t()
        self._proc_visibility_skips: list[dict[str, int | str]] = []

    def prepare(self) -> dict[str, Any]:
        """Validate inputs, create private copies and an overlay, and retain evidence."""
        config = self.config
        limits = ((config.run_seconds, 3600.0), (config.startup_seconds, 300.0),
                  (config.shutdown_seconds, 300.0))
        if any(not math.isfinite(value) or value <= 0 or value > maximum for value, maximum in limits):
            raise runner_error_t("runner deadlines must be finite, positive, and within their maximum bounds")
        if not 256 <= config.memory_mib <= 16384 or not 1 <= config.cpus <= 16:
            raise runner_error_t("guest memory or CPU count is outside the harness bounds")
        if config.base_dir.is_symlink():
            raise runner_error_t("base directory must not be a symlink")
        base = config.base_dir.resolve(strict=True)
        base_disk = base / "windows.qcow2"
        if base_disk.is_symlink() or not base_disk.is_file():
            raise runner_error_t("base Windows disk is missing or unsafe")
        self._base_lease.acquire(base_disk)
        before = _protected_snapshot(base)
        initial_scan_skips = _check_no_writable_base_holder(base_disk, diagnostic_only=True)
        self._proc_visibility_skips = initial_scan_skips
        if config.qemu_build_dir.is_symlink():
            raise runner_error_t("QEMU build directory must not be a symlink")
        qemu = config.qemu_build_dir.resolve(strict=True) / "qemu-system-x86_64"
        stamp_path = config.qemu_build_dir.resolve(strict=True) / ".waddle-qemu-build.json"
        qemu_info = _validate_qemu(qemu, stamp_path)
        for tool in ("qemu-img", "swtpm"):
            if shutil.which(tool) is None:
                raise runner_error_t(f"required host capability is missing: {tool}")
        if config.payload_files and shutil.which("xorriso") is None:
            raise runner_error_t("xorriso is required to stage a read-only payload ISO")
        if DefaultOvmfCode.is_symlink() or not DefaultOvmfCode.is_file():
            raise runner_error_t(f"matching OVMF4M CODE firmware is unavailable: {DefaultOvmfCode}")

        qemu_img = shutil.which("qemu-img")
        if qemu_img is None:
            raise runner_error_t("required host capability is missing: qemu-img")
        self._base_lease.check()
        _reject_external_qcow2_storage(qemu_img, base_disk, [base_disk], on_tick=self._base_lease.check)
        parent = config.run_parent.resolve(strict=True)
        temporary_root = Path(tempfile.gettempdir()).resolve(strict=True)
        if (config.run_parent.is_symlink() or not parent.is_dir() or
                (parent != temporary_root and temporary_root not in parent.parents)):
            raise runner_error_t("run parent must be a real directory beneath the system temporary root")
        self.run_root = Path(tempfile.mkdtemp(prefix="waddle-native-guest-", dir=parent))
        os.chmod(self.run_root, 0o700)
        if config.enable_test_result_channel:
            self._result_channel = result_channel_t(self.run_root)
        private_vars = self.run_root / "vars.fd"
        _copy_private_file(base / "vars.fd", private_vars)
        private_tpm = self.run_root / "tpm"
        private_tpm.mkdir(mode=0o700)
        for entry in before["tpm"]:
            source = Path(entry["path"])
            if source.name == ".lock":
                continue
            _copy_private_file(source, private_tpm / source.name)
        overlay = self.run_root / "windows-overlay.qcow2"
        self._base_lease.check()
        _run_bounded([qemu_img, "create", "-f", "qcow2", "-F", "qcow2", "-b",
                      str(base_disk.resolve(strict=True)), str(overlay)], timeout=MaxPreparationCommandSeconds,
                     on_tick=self._base_lease.check)
        self._base_lease.check()
        _reject_external_qcow2_storage(qemu_img, overlay, [overlay, base_disk], on_tick=self._base_lease.check)
        payload = _stage_payload(config, self.run_root,
                                 self._result_channel.nonce if self._result_channel is not None else None,
                                 on_tick=self._base_lease.check)
        ivshmem = self.run_root / "ivshmem.bin"
        with ivshmem.open("xb") as stream:
            stream.truncate(64 * 1024 * 1024)
        os.chmod(ivshmem, 0o600)
        manifest = {"created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                    "base_snapshot": before, "qemu": qemu_info,
                    "firmware": {"path": str(DefaultOvmfCode), "sha256": _sha256_file(DefaultOvmfCode)},
                    "overlay": str(overlay), "payload": payload,
                    "run_root": str(self.run_root),
                    "writable_holder_scan_skipped_foreign_processes": initial_scan_skips,
                    "base_read_lease": self._base_lease.snapshot(),
                    "proc_holder_scan_authority": "diagnostic-only; kernel read lease is required",
                    "result_channel": ({"transport": "test-only guestfwd TCP", "port": ResultPort,
                                        "nonce": self._result_channel.nonce}
                                       if self._result_channel is not None else None),
                    "test_only": True, "interop_credit": False}
        (self.run_root / "run-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        return manifest

    def _start_child(self, name: str, argv: Sequence[str], log_path: Path) -> Any:
        log = log_path.open("wb")
        try:
            process = self._popen(list(argv), stdin=subprocess.DEVNULL, stdout=log,
                                  stderr=subprocess.STDOUT, close_fds=True)
        except BaseException:
            log.close()
            raise
        self.children.append((name, process))
        setattr(process, "_waddle_log", log)
        return process

    def _stop_child(self, process: Any, timeout: float) -> bool:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=timeout)
            self._forced_shutdown = True
        else:
            process.wait()
        self._close_child_log(process)
        return process.returncode == 0

    @staticmethod
    def _close_child_log(process: Any) -> None:
        log = getattr(process, "_waddle_log", None)
        if log is not None and not log.closed:
            log.close()

    def _wait_child(self, process: Any, deadline: float) -> None:
        while process.poll() is None and self._clock() < deadline:
            self._sleep(0.05)
        if process.poll() is not None:
            raise runner_error_t(f"owned child exited early with status {process.returncode}")

    def _start_swtpm(self) -> tuple[Any, Path]:
        assert self.run_root is not None
        tool = shutil.which("swtpm")
        if tool is None:
            raise runner_error_t("swtpm disappeared after preflight")
        tpm_socket = self.run_root / "tpm.sock"
        process = self._start_child("swtpm", [tool, "socket", "--tpm2", "--tpmstate",
            f"dir={self.run_root / 'tpm'}", "--ctrl", f"type=unixio,path={tpm_socket}"],
            self.run_root / "swtpm.log")
        deadline = self._clock() + min(self.config.startup_seconds, 10.0)
        while self._clock() < deadline:
            self._base_lease.check()
            if process.poll() is not None:
                raise runner_error_t(f"owned swtpm exited during startup ({process.returncode})")
            if tpm_socket.exists():
                return process, tpm_socket
            self._sleep(0.05)
        raise runner_error_t("timed out waiting for private swtpm control socket")

    def _qemu_argv(self, tpm_socket: Path) -> list[str]:
        assert self.run_root is not None
        root = self.run_root
        qemu = self.config.qemu_build_dir.resolve(strict=True) / "qemu-system-x86_64"
        argv = [str(qemu), "-name", "waddle-native-private", "-machine", "q35,accel=tcg",
                "-cpu", "max", "-smp", str(self.config.cpus), "-m", str(self.config.memory_mib),
                "-nodefaults", "-no-user-config", "-display", "none", "-monitor", "none",
                "-serial", "none", "-drive", f"if=pflash,format=raw,readonly=on,file={DefaultOvmfCode}",
                "-drive", f"if=pflash,format=raw,file={root / 'vars.fd'}", "-device", "ich9-ahci,id=sata",
                "-drive", f"if=none,id=windows,format=qcow2,file={root / 'windows-overlay.qcow2'}",
                "-device", "ide-hd,drive=windows,bus=sata.0", "-device", "VGA",
                "-object", f"memory-backend-file,id=waddle_av,size=64M,mem-path={root / 'ivshmem.bin'},share=on",
                "-device", "ivshmem-plain,memdev=waddle_av", "-chardev",
                f"socket,id=chrtpm,path={tpm_socket}", "-tpmdev", "emulator,id=tpm0,chardev=chrtpm",
                "-device", "tpm-tis,tpmdev=tpm0", "-qmp",
                f"unix:{root / 'qmp.sock'},server=on,wait=off", "-D", str(root / "qemu.log"), "-no-reboot"]
        if self.config.payload_files:
            argv.extend(["-drive", f"file={root / 'test-payload.iso'},media=cdrom,readonly=on,format=raw"])
        if self._result_channel is not None:
            argv.extend(["-chardev", f"socket,id=interop_result,path={self._result_channel.path},server=off,wait=off",
                         "-netdev", f"user,id=interop_guestnet,restrict=on,guestfwd=tcp:10.0.2.100:{ResultPort}-chardev:interop_result",
                         "-device", "e1000,netdev=interop_guestnet"])
        return argv

    def _revalidate_launch_inputs(self, manifest: dict[str, Any]) -> None:
        """Repeat protected-input and executable checks immediately before QEMU spawn."""
        base = self.config.base_dir.resolve(strict=True)
        base_disk = base / "windows.qcow2"
        self._base_lease.check()
        _verify_snapshot(base, manifest["base_snapshot"])
        launch_scan_skips = _check_no_writable_base_holder(base_disk, diagnostic_only=True)
        manifest["writable_holder_scan_skipped_foreign_processes_at_launch"] = launch_scan_skips
        self._proc_visibility_skips = launch_scan_skips
        qemu_img = shutil.which("qemu-img")
        if qemu_img is None or self.run_root is None:
            raise runner_error_t("launch preflight lost qemu-img or the private run root")
        overlay = self.run_root / "windows-overlay.qcow2"
        _reject_external_qcow2_storage(qemu_img, overlay, [overlay, base_disk], on_tick=self._base_lease.check)
        qemu = self.config.qemu_build_dir.resolve(strict=True) / "qemu-system-x86_64"
        expected_hash = manifest["qemu"]["sha256"]
        if qemu.is_symlink() or not qemu.is_file() or _sha256_file(qemu) != expected_hash:
            raise runner_error_t("QEMU binary changed after initial provenance validation")
        self._base_lease.check()

    def _connect_qmp(self, qemu: Any) -> qmp_client_t:
        assert self.run_root is not None
        path = self.run_root / "qmp.sock"
        deadline = self._clock() + self.config.startup_seconds
        while self._clock() < deadline:
            if qemu.poll() is not None:
                raise runner_error_t(f"owned QEMU exited during startup ({qemu.returncode})")
            if path.exists():
                try:
                    self._base_lease.check()
                    return qmp_client_t(path, min(2.0, self.config.startup_seconds),
                                        on_tick=self._base_lease.check)
                except (OSError, TimeoutError, runner_error_t):
                    pass
            self._base_lease.check()
            self._sleep(LeasePollSeconds)
        raise runner_error_t("timed out waiting for private QMP endpoint")

    def _shutdown_qemu(self, qemu: Any) -> None:
        try:
            self._base_lease.check()
        except runner_error_t:
            if qemu.poll() is None:
                self._stop_child(qemu, min(2.0, self.config.shutdown_seconds))
            return
        if qemu.poll() is not None:
            qemu.wait()
            self._close_child_log(qemu)
            return
        deadline = self._clock() + self.config.shutdown_seconds
        try:
            assert self._qmp is not None
            self._qmp.execute("system_powerdown", timeout=min(2.0, max(0.001, deadline - self._clock())))
        except Exception:
            pass
        while qemu.poll() is None and self._clock() < deadline:
            try:
                self._base_lease.check()
            except runner_error_t:
                self._stop_child(qemu, min(2.0, self.config.shutdown_seconds))
                return
            self._sleep(LeasePollSeconds)
        if qemu.poll() is None:
            try:
                assert self._qmp is not None
                remaining = deadline - self._clock()
                if remaining > 0:
                    self._qmp.execute("quit", timeout=min(2.0, remaining))
            except Exception:
                pass
            while qemu.poll() is None and self._clock() < deadline:
                try:
                    self._base_lease.check()
                except runner_error_t:
                    self._stop_child(qemu, min(2.0, self.config.shutdown_seconds))
                    return
                self._sleep(LeasePollSeconds)
            if qemu.poll() is None:
                self._stop_child(qemu, min(2.0, self.config.shutdown_seconds))
            else:
                self._forced_shutdown = True
        else:
            qemu.wait()
        if qemu.poll() is not None:
            self._close_child_log(qemu)

    def run(self) -> dict[str, Any]:
        """Run one bounded boot observation and retain its private evidence."""
        if self.run_root is not None:
            raise runner_error_t("runner instances are single-use")
        try:
            manifest = self.prepare()
        except BaseException as error:
            try:
                self._base_lease.release()
            except BaseException as release_error:
                error = runner_error_t(f"{error}; base lease cleanup failed: {release_error}")
            if self.run_root is not None:
                evidence = {"test_only": True, "interop_credit": False,
                            "preparation_failure": f"{type(error).__name__}: {error}",
                            "base_read_lease": self._base_lease.snapshot(),
                            "proc_holder_scan_authority": "diagnostic-only; kernel read lease is required",
                            "proc_holder_scan_skipped": self._proc_visibility_skips}
                (self.run_root / "run-manifest.json").write_text(
                    json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
                raise runner_error_t(f"runner preparation failed; evidence retained at {self.run_root}: {error}") from error
            raise
        assert self.run_root is not None
        root = self.run_root
        qemu_process = None
        tpm_process = None
        failure: str | None = None
        try:
            tpm_process, tpm_socket = self._start_swtpm()
            self._revalidate_launch_inputs(manifest)
            self._base_lease.check()
            qemu_process = self._start_child("qemu", self._qemu_argv(tpm_socket), root / "qemu-console.log")
            self._qmp = self._connect_qmp(qemu_process)
            status = self._qmp.execute("query-status")
            topology = self._qmp.execute("query-pci")
            manifest["qmp_initial_status"] = status
            manifest["qmp_pci"] = topology
            manifest["boot_observation_started"] = True
            (root / "run-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
            deadline = self._clock() + self.config.run_seconds
            while qemu_process.poll() is None and self._clock() < deadline:
                self._base_lease.check()
                self._sleep(LeasePollSeconds)
            if qemu_process.poll() is not None:
                raise runner_error_t(f"owned QEMU exited during observation ({qemu_process.returncode})")
            if qemu_process.poll() is None:
                try:
                    self._qmp.execute("screendump", {"filename": str(root / "final-screen.ppm")})
                except Exception:
                    pass
                manifest["run_deadline_reached"] = True
            if self._result_channel is not None:
                remaining = max(0.1, deadline - self._clock()) if qemu_process.poll() is None else min(2.0, self.config.run_seconds)
                manifest["guest_result"] = self._result_channel.receive(remaining,
                                                                         on_tick=self._base_lease.check)
                (root / "guest-result.json").write_text(json.dumps(manifest["guest_result"], indent=2) + "\n",
                                                        encoding="utf-8")
            self._shutdown_qemu(qemu_process)
        except BaseException as error:
            failure = f"{type(error).__name__}: {error}"
        finally:
            qemu_owned = qemu_process
            if qemu_owned is None:
                qemu_owned = next((process for name, process in reversed(self.children) if name == "qemu"), None)
            if qemu_owned is not None and qemu_owned.poll() is None:
                try:
                    self._shutdown_qemu(qemu_owned)
                except BaseException as error:
                    try:
                        self._stop_child(qemu_owned, min(2.0, self.config.shutdown_seconds))
                    except BaseException as stop_error:
                        failure = (failure or "") + f"; QEMU cleanup failed: {error}; forced reap failed: {stop_error}"
            qemu_reaped = qemu_owned is None or qemu_owned.poll() is not None
            if self._qmp is not None:
                try:
                    self._qmp.close()
                except BaseException as error:
                    failure = (failure or "") + f"; QMP socket close failed: {error}"
            if qemu_reaped:
                if qemu_owned is not None:
                    self._close_child_log(qemu_owned)
                if self._result_channel is not None:
                    try:
                        self._result_channel.close()
                    except BaseException as error:
                        failure = (failure or "") + f"; result endpoint close failed: {error}"
                # Reverse creation order: QEMU is confirmed reaped before swtpm.
                for name, process in reversed(self.children):
                    if name == "swtpm" and process.poll() is None:
                        try:
                            self._stop_child(process, min(2.0, self.config.shutdown_seconds))
                        except BaseException as error:
                            failure = (failure or "") + f"; swtpm cleanup failed: {error}"
                children_reaped = all(process.poll() is not None for _name, process in self.children)
                if children_reaped:
                    try:
                        self._base_lease.release()
                    except BaseException as error:
                        failure = (failure or "") + f"; base read lease release failed: {error}"
                else:
                    failure = (failure or "") + "; owned child remains alive; base read lease retained"
            else:
                failure = (failure or "") + "; QEMU is still alive; swtpm and result endpoint retained"
        manifest["base_read_lease"] = self._base_lease.snapshot()
        manifest["forced_shutdown"] = self._forced_shutdown
        manifest["successful_guest_quiescence"] = False
        manifest["interop_credit"] = False
        manifest["failure"] = failure
        try:
            _verify_snapshot(self.config.base_dir, manifest["base_snapshot"])
            manifest["protected_inputs_unchanged"] = True
        except BaseException as error:
            manifest["protected_inputs_unchanged"] = False
            failure = (failure or "") + f"; protected-input verification failed: {error}"
            manifest["failure"] = failure
        (root / "run-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        if failure:
            raise runner_error_t(f"native guest run failed; evidence retained at {root}: {failure}")
        return manifest


def main(argv: Sequence[str] | None = None) -> int:
    """Run a bounded boot-only check and retain all evidence in a fresh /tmp root."""
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-seconds", type=float, default=60.0)
    parser.add_argument("--startup-seconds", type=float, default=45.0)
    parser.add_argument("--shutdown-seconds", type=float, default=30.0)
    parser.add_argument("--payload", type=Path, action="append", default=[])
    parser.add_argument("--expected-payload-hash", action="append", default=[], metavar="NAME=SHA256")
    parser.add_argument("--test-result-channel", action="store_true")
    args = parser.parse_args(argv)
    try:
        hashes = []
        for item in args.expected_payload_hash:
            name, separator, digest = item.partition("=")
            if (not separator or len(digest) != 64 or
                    any(character not in "0123456789abcdefABCDEF" for character in digest)):
                raise runner_error_t("expected payload hash must be NAME=64_HEX_DIGITS")
            hashes.append((name, digest.lower()))
        config = runner_config_t(run_seconds=args.run_seconds, startup_seconds=args.startup_seconds,
                                 shutdown_seconds=args.shutdown_seconds, payload_files=tuple(args.payload),
                                 expected_payload_hashes=tuple(hashes),
                                 enable_test_result_channel=args.test_result_channel)
        result = native_guest_runner_t(config).run()
        print(json.dumps({"run_root": result.get("run_root"),
                          "protected_inputs_unchanged": result.get("protected_inputs_unchanged"),
                          "forced_shutdown": result.get("forced_shutdown"),
                          "interop_credit": False}, indent=2))
        return 0
    except runner_error_t as error:
        print(str(error), file=__import__("sys").stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
