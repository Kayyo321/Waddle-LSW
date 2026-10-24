#!/usr/bin/env python3
"""Copy a selected QEMU or virtiofsd executable with verifiable provenance."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import signal
import shutil
import stat
import subprocess
import sys
import tempfile
import time
from typing import Any, Iterator


# Identity format shared by package publishers and package readers.
IdentitySchema = 1
# Maximum combined retained stdout/stderr for executable probes.
MaxProbeOutputBytes = 65536
# Per-probe wall clock bound, including both --version and --help probes.
ProbeTimeoutSeconds = 10
# Grace period before killing the private probe process group.
ProbeTerminateGraceSeconds = 0.25
# Pinned QEMU build outputs must provide these configuration feature claims.
RequiredConfigs = ("CONFIG_AUDIO_PIPEWIRE", "CONFIG_SLIRP", "CONFIG_USB_LIBUSB")
# Pinned QEMU configure flags whose build is eligible for packaging.
RequiredConfigureFlags = ("--enable-pipewire", "--enable-slirp", "--enable-libusb", "--disable-download")
QemuBinaryName = "qemu-system-x86_64"


class build_error_t(RuntimeError):
    """Raised when a source, package, identity, or probe violates the contract."""


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _canonical_hash(value: Any) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def _kill_probe_group(process: subprocess.Popen[bytes]) -> None:
    """Terminate every process in the private probe group, including descendants."""
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    except OSError:
        pass
    try:
        process.wait(timeout=ProbeTerminateGraceSeconds)
    except subprocess.TimeoutExpired:
        pass
    # Kill the group even if its leader exited first; a descendant may remain.
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    except OSError:
        pass
    try:
        process.wait()
    except OSError:
        pass


def _probe(argv: list[str]) -> tuple[str, str]:
    """Run one bounded tool probe and return bounded stdout/stderr text."""
    try:
        process = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   start_new_session=True)
    except OSError as error:
        raise build_error_t(f"could not execute {argv[0]}: {error}") from error
    assert process.stdout is not None and process.stderr is not None
    selector = selectors.DefaultSelector()
    output = {process.stdout.fileno(): bytearray(), process.stderr.fileno(): bytearray()}
    selector.register(process.stdout, selectors.EVENT_READ)
    selector.register(process.stderr, selectors.EVENT_READ)
    deadline = time.monotonic() + ProbeTimeoutSeconds
    try:
        while selector.get_map():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise build_error_t(f"probe timed out after {ProbeTimeoutSeconds}s: {argv[0]}")
            for key, _ in selector.select(min(remaining, 0.2)):
                descriptor = key.fileobj.fileno()
                chunk = os.read(descriptor, 8192)
                if not chunk:
                    selector.unregister(key.fileobj)
                    continue
                total = sum(len(buffer) for buffer in output.values())
                room = MaxProbeOutputBytes - total
                if room <= 0 or len(chunk) > room:
                    raise build_error_t(f"probe output exceeded {MaxProbeOutputBytes} bytes: {argv[0]}")
                output[descriptor].extend(chunk)
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise build_error_t(f"probe timed out after {ProbeTimeoutSeconds}s: {argv[0]}")
        return_code = process.wait(timeout=remaining)
        stdout = output[process.stdout.fileno()].decode("utf-8", errors="replace")
        stderr = output[process.stderr.fileno()].decode("utf-8", errors="replace")
        if return_code != 0:
            detail = (stderr or stdout).strip()
            raise build_error_t(f"probe failed ({return_code}): {argv[0]}" + (f"\n{detail}" if detail else ""))
        return stdout.strip(), stderr.strip()
    except BaseException:
        _kill_probe_group(process)
        raise
    finally:
        selector.close()
        process.stdout.close()
        process.stderr.close()


def _validate_source(source: Path) -> tuple[Path, os.stat_result, str]:
    try:
        resolved = source.resolve(strict=True)
        source_stat = resolved.stat()
    except OSError as error:
        raise build_error_t(f"source executable is unavailable: {source}: {error}") from error
    if not resolved.is_file() or not os.access(resolved, os.X_OK):
        raise build_error_t(f"source is not an executable regular file: {resolved}")
    return resolved, source_stat, _sha256(resolved)


def _version(source: Path) -> str:
    stdout, stderr = _probe([str(source), "--version"])
    value = stdout or stderr
    if not value:
        raise build_error_t(f"version probe returned no output: {source}")
    return value[:MaxProbeOutputBytes]


def _virtiofsd_capability(source: Path) -> tuple[str, str]:
    version = _version(source)
    stdout, stderr = _probe([str(source), "--help"])
    help_text = stdout + "\n" + stderr
    if not re.search(r"(?m)^\s*--readonly(?:\s|$)", help_text):
        raise build_error_t(f"virtiofsd does not advertise --readonly: {source}")
    return version, help_text[:MaxProbeOutputBytes]


def _valid_digest(value: Any) -> bool:
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None


def _validate_qemu_stamp(stamp_path: Path, source_name: str, source_hash: str) -> dict[str, Any]:
    try:
        stamp = json.loads(stamp_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise build_error_t(f"cannot read QEMU build provenance {stamp_path}: {error}") from error
    if not isinstance(stamp, dict) or stamp.get("schema") != 1:
        raise build_error_t("QEMU provenance has an unsupported or malformed schema")
    signature = stamp.get("signature")
    inputs = stamp.get("inputs")
    configuration = stamp.get("configuration_sha256")
    required_macros = stamp.get("enabled_config_macros")
    binary_sha = stamp.get("binary_sha256")
    if not _valid_digest(signature) or not isinstance(inputs, dict) or _canonical_hash(inputs) != signature:
        raise build_error_t("QEMU provenance signature is malformed or does not match its inputs")
    if stamp.get("binary") != QemuBinaryName or source_name != QemuBinaryName:
        raise build_error_t(f"QEMU provenance must name {QemuBinaryName}")
    if binary_sha != source_hash:
        raise build_error_t("QEMU build stamp binary hash does not match source executable")
    if required_macros != list(RequiredConfigs):
        raise build_error_t("QEMU build stamp does not claim all required configuration macros")
    if not isinstance(configuration, dict) or set(configuration) != {"config-host.mak", "config-host.h"} or any(
        not _valid_digest(item) for item in configuration.values()
    ):
        raise build_error_t("QEMU build stamp has incomplete configuration file digests")
    for name, expected_hash in configuration.items():
        if _sha256(stamp_path.parent / name) != expected_hash:
            raise build_error_t(f"QEMU build stamp configuration digest does not match {name}")
    source_commit = inputs.get("source_commit")
    configure_argv = inputs.get("configure_argv")
    if not isinstance(source_commit, str) or re.fullmatch(r"[0-9a-f]{40}", source_commit) is None:
        raise build_error_t("QEMU build stamp has an invalid pinned source commit")
    if not isinstance(configure_argv, list) or not all(flag in configure_argv for flag in RequiredConfigureFlags):
        raise build_error_t("QEMU build stamp lacks required pinned configure flags")
    return stamp


def _canonical_destination(path: Path) -> Path:
    parent = path.parent.resolve(strict=True)
    if not parent.is_dir():
        raise build_error_t(f"package parent is not a directory: {parent}")
    return parent / path.name


def _check_package_paths(source: Path, destination: Path, identity: Path) -> tuple[Path, Path]:
    try:
        target = _canonical_destination(destination)
        identity_target = _canonical_destination(identity)
    except OSError as error:
        raise build_error_t(f"package destination parent is unavailable: {error}") from error
    lock_target = identity_target.parent / f".{identity_target.name}.lock"
    if target == source or identity_target == source or target == identity_target or \
            target == lock_target or lock_target == source:
        raise build_error_t("source, executable destination, and identity must be distinct paths")
    for path, label in ((target, "executable destination"), (identity_target, "identity destination")):
        try:
            info = path.lstat()
        except FileNotFoundError:
            continue
        except OSError as error:
            raise build_error_t(f"cannot inspect {label} {path}: {error}") from error
        if not info or not (path.is_file() and not path.is_symlink()):
            raise build_error_t(f"{label} must be a regular file, not a symlink or directory: {path}")
    return target, identity_target


@contextmanager
def _identity_lock(identity_path: Path) -> Iterator[None]:
    lock_path = identity_path.parent / f".{identity_path.name}.lock"
    try:
        descriptor = os.open(lock_path, os.O_CREAT | os.O_RDWR | os.O_NONBLOCK |
                             getattr(os, "O_NOFOLLOW", 0), 0o600)
    except OSError as error:
        raise build_error_t(f"cannot open package identity lock {lock_path}: {error}") from error
    try:
        if not stat.S_ISREG(os.fstat(descriptor).st_mode):
            raise build_error_t(f"package identity lock is not a regular file: {lock_path}")
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise build_error_t(f"package identity is busy: {identity_path}") from error
        yield
    finally:
        os.close(descriptor)


def _write_temp_file(parent: Path, prefix: str, writer: Any, mode: int) -> Path:
    temp_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile("wb", dir=parent, prefix=prefix, suffix=".tmp", delete=False) as stream:
            temp_path = Path(stream.name)
            writer(stream)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temp_path, mode)
        return temp_path
    except BaseException:
        if temp_path is not None:
            try:
                temp_path.unlink()
            except FileNotFoundError:
                pass
        raise


def _publish(source: Path, destination: Path, identity: Path, identity_value: dict[str, Any]) -> None:
    executable_temp: Path | None = None
    identity_temp: Path | None = None
    try:
        source_mode = source.stat().st_mode & 0o777
        if not source_mode & 0o111:
            raise build_error_t(f"source executable has no execute permission: {source}")
        def copy_source(output: Any) -> None:
            with source.open("rb") as input_stream:
                shutil.copyfileobj(input_stream, output)

        executable_temp = _write_temp_file(
            destination.parent, f".{destination.name}.vendor-", copy_source, source_mode,
        )
        if _sha256(executable_temp) != identity_value["package_sha256"]:
            raise build_error_t("source executable changed while it was being copied")
        encoded = (json.dumps(identity_value, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")
        identity_temp = _write_temp_file(identity.parent, f".{identity.name}.vendor-", lambda output: output.write(encoded), 0o600)
        os.replace(executable_temp, destination)
        executable_temp = None
        os.replace(identity_temp, identity)
        identity_temp = None
        _verify_package_unlocked(destination, identity, identity_value["package"])
    finally:
        for temporary in (executable_temp, identity_temp):
            if temporary is not None:
                try:
                    temporary.unlink()
                except FileNotFoundError:
                    pass


def _read_identity(identity_path: Path) -> dict[str, Any]:
    try:
        info = identity_path.lstat()
        if not identity_path.is_file() or identity_path.is_symlink():
            raise build_error_t(f"identity must be a regular file: {identity_path}")
        value = json.loads(identity_path.read_text(encoding="utf-8"))
    except build_error_t:
        raise
    except (OSError, ValueError) as error:
        raise build_error_t(f"cannot read package identity {identity_path}: {error}") from error
    if not isinstance(value, dict) or value.get("schema") != IdentitySchema:
        raise build_error_t("package identity has an unsupported or malformed schema")
    return value


def _verify_package_unlocked(destination: Path, identity: Path, expected_kind: str | None = None) -> dict[str, Any]:
    value = _read_identity(identity)
    if expected_kind is not None and value.get("kind") != expected_kind:
        raise build_error_t(f"package identity kind mismatch: expected {expected_kind}")
    try:
        info = destination.lstat()
    except OSError as error:
        raise build_error_t(f"cannot inspect packaged executable {destination}: {error}") from error
    if not destination.is_file() or destination.is_symlink() or not info.st_mode & 0o111:
        raise build_error_t(f"packaged executable is not a regular executable file: {destination}")
    if value.get("destination_name") != destination.name or value.get("package_sha256") != _sha256(destination):
        raise build_error_t(f"packaged executable does not match its identity: {destination}")
    if not _valid_digest(value.get("source_artifact_sha256")) or not _valid_digest(value.get("package_sha256")):
        raise build_error_t("package identity has incomplete artifact hashes")
    if value.get("source_artifact_sha256") != value.get("package_sha256"):
        raise build_error_t("package identity source and packaged artifact hashes differ")
    if value.get("source_kind") not in ("pinned-source", "system"):
        raise build_error_t("package identity has an invalid source kind")
    if value.get("kind") not in ("qemu", "virtiofsd"):
        raise build_error_t("package identity has an invalid package kind")
    if value.get("package") != value.get("kind"):
        raise build_error_t("package identity kind and package fields do not match")
    if value.get("kind") == "virtiofsd" and value.get("source_kind") != "system":
        raise build_error_t("virtiofsd package identity cannot claim pinned-source provenance")
    source_path = value.get("source_path")
    if not isinstance(source_path, str) or not Path(source_path).is_absolute():
        raise build_error_t("package identity source path must be absolute")
    if not isinstance(value.get("version"), str) or not value.get("version"):
        raise build_error_t("package identity has no version")
    pinned_fields = {"source_commit", "build_signature", "build_provenance_path"}
    if value.get("source_kind") == "system" and pinned_fields.intersection(value):
        raise build_error_t("system package identity contains pinned-source provenance fields")
    if value.get("kind") == "qemu" and value.get("source_kind") == "pinned-source":
        if not re.fullmatch(r"[0-9a-f]{40}", str(value.get("source_commit", ""))):
            raise build_error_t("pinned QEMU identity has no valid source commit")
        if not _valid_digest(value.get("build_signature")):
            raise build_error_t("pinned QEMU identity has no valid build signature")
        if not isinstance(value.get("build_provenance_path"), str) or \
                not Path(value["build_provenance_path"]).is_absolute():
            raise build_error_t("pinned QEMU identity has no absolute build provenance path")
    if value.get("kind") == "virtiofsd":
        if value.get("readonly_capability") != "advertised" or \
                not _valid_digest(value.get("readonly_probe_sha256")):
            raise build_error_t("virtiofsd identity lacks its read-only capability evidence")
    return value


def copy_package(kind: str, source_kind: str, source_arg: Path, destination_arg: Path,
                 identity_arg: Path, provenance_arg: Path | None = None) -> dict[str, Any]:
    """Preflight, probe and publish one package while holding its identity lock."""
    if kind not in ("qemu", "virtiofsd") or source_kind not in ("pinned-source", "system"):
        raise build_error_t("unsupported package or source kind")
    if kind == "virtiofsd" and source_kind != "system":
        raise build_error_t("virtiofsd pinned-source packaging is unsupported")
    if kind == "qemu" and source_kind == "pinned-source" and provenance_arg is None:
        raise build_error_t("pinned QEMU packaging requires --provenance")
    source, source_stat, source_hash = _validate_source(source_arg)
    destination, identity = _check_package_paths(source, destination_arg, identity_arg)
    with _identity_lock(identity):
        # All probes and provenance checks happen before either package file changes.
        stamp = None
        if kind == "qemu" and source_kind == "pinned-source":
            assert provenance_arg is not None
            stamp = _validate_qemu_stamp(provenance_arg, source_arg.name, source_hash)
        if kind == "virtiofsd":
            version, help_text = _virtiofsd_capability(source)
        else:
            version = _version(source)
            help_text = ""
        identity_value: dict[str, Any] = {
            "schema": IdentitySchema,
            "kind": kind,
            "package": kind,
            "source_kind": source_kind,
            "source_path": str(source),
            "source_artifact_sha256": source_hash,
            "destination_name": destination.name,
            "package_sha256": source_hash,
            "source_mode": source_stat.st_mode & 0o777,
            "version": version,
        }
        if kind == "virtiofsd":
            identity_value["readonly_capability"] = "advertised"
            identity_value["readonly_probe_sha256"] = hashlib.sha256(help_text.encode("utf-8")).hexdigest()
        if stamp is not None:
            identity_value.update({
                "source_commit": stamp["inputs"]["source_commit"],
                "build_signature": stamp["signature"],
                "build_provenance_path": str(provenance_arg.resolve(strict=True)),
            })
        _publish(source, destination, identity, identity_value)
        return _verify_package_unlocked(destination, identity, kind)


def verify_package(kind: str, destination_arg: Path, identity_arg: Path) -> dict[str, Any]:
    """Verify one package identity and executable hash under the publisher lock."""
    destination = _canonical_destination(destination_arg)
    identity = _canonical_destination(identity_arg)
    with _identity_lock(identity):
        return _verify_package_unlocked(destination, identity, kind)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    copy_parser = commands.add_parser("copy", help="preflight and publish a package")
    copy_parser.add_argument("--kind", choices=("qemu", "virtiofsd"), required=True)
    copy_parser.add_argument("--source-kind", choices=("pinned-source", "system"), required=True)
    copy_parser.add_argument("--source", type=Path, required=True)
    copy_parser.add_argument("--destination", type=Path, required=True)
    copy_parser.add_argument("--identity", type=Path, required=True)
    copy_parser.add_argument("--provenance", type=Path, help="QEMU source build helper stamp")
    verify_parser = commands.add_parser("verify", help="verify a published executable and identity")
    verify_parser.add_argument("--kind", choices=("qemu", "virtiofsd"), required=True)
    verify_parser.add_argument("--destination", type=Path, required=True)
    verify_parser.add_argument("--identity", type=Path, required=True)
    arguments = parser.parse_args(argv)
    try:
        if arguments.command == "copy":
            value = copy_package(arguments.kind, arguments.source_kind, arguments.source,
                                 arguments.destination, arguments.identity, arguments.provenance)
        else:
            value = verify_package(arguments.kind, arguments.destination, arguments.identity)
    except KeyboardInterrupt:
        print("vendor_copy.py: interrupted; owned temporary files were removed", file=sys.stderr)
        return 130
    except (build_error_t, OSError, ValueError) as error:
        print(f"vendor_copy.py: error: {error}", file=sys.stderr)
        return 1
    print(json.dumps({"status": "verified", "identity": value}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
