#!/usr/bin/env python3
"""Build pinned QEMU out of tree with the seamless-interoperability features."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import selectors
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from typing import Any


# Ordered immutable QEMU configure arguments for the supported x86_64/KVM build.
ConfigureFlags = (
    "--target-list=x86_64-softmmu",
    "--enable-kvm",
    "--disable-docs",
    "--disable-gtk",
    "--disable-sdl",
    "--disable-vnc",
    "--enable-pipewire",
    "--enable-slirp",
    "--enable-libusb",
    "--disable-download",
)
# Required pkg-config modules and minimum versions where the feature contract sets one.
RequiredPackages = (("libpipewire-0.3", (0, 3, 60)), ("slirp", None), ("libusb-1.0", (1, 0, 13)))
# QEMU config-host.h feature macros that must be enabled after configuration.
RequiredConfigs = ("CONFIG_AUDIO_PIPEWIRE", "CONFIG_SLIRP", "CONFIG_USB_LIBUSB")
# Per-out-of-tree-directory provenance stamp and format version.
StampName = ".waddle-qemu-build.json"
StampSchema = 1
# Maximum retained stdout/stderr bytes from small identity and pkg-config probes.
MaxToolOutputBytes = 65536
ProcessTerminateTimeoutSeconds = 2


class build_error_t(RuntimeError):
    """Raised when inputs or a build stage fail validation."""


def _run(argv: list[str], *, cwd: Path | None = None, env: dict[str, str] | None = None,
         stream_output: bool = False) -> str:
    if stream_output:
        print(f"[qemu-build] running: {shlex.join(argv)}", flush=True)
        try:
            process = subprocess.Popen(argv, cwd=cwd, env=env, start_new_session=True)
        except OSError as error:
            raise build_error_t(f"could not execute {argv[0]}: {error}") from error
        try:
            return_code = process.wait()
            if return_code:
                raise build_error_t(f"command failed ({return_code}): {shlex.join(argv)}")
        except BaseException:
            _terminate_owned_process_group(process)
            raise
        return ""
    try:
        process = subprocess.Popen(argv, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   start_new_session=True)
    except OSError as error:
        raise build_error_t(f"could not execute {argv[0]}: {error}") from error
    assert process.stdout is not None and process.stderr is not None
    stdout_descriptor = process.stdout.fileno()
    stderr_descriptor = process.stderr.fileno()
    output = {stdout_descriptor: bytearray(), stderr_descriptor: bytearray()}
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    selector.register(process.stderr, selectors.EVENT_READ)
    try:
        while selector.get_map():
            for key, _ in selector.select():
                descriptor = key.fileobj.fileno()
                chunk = os.read(descriptor, 8192)
                if not chunk:
                    selector.unregister(key.fileobj)
                    continue
                buffer = output[descriptor]
                room = MaxToolOutputBytes - len(buffer)
                if room > 0:
                    buffer.extend(chunk[:room])
        return_code = process.wait()
    except BaseException:
        _terminate_owned_process_group(process)
        raise
    finally:
        selector.close()
        process.stdout.close()
        process.stderr.close()
    stdout = output[stdout_descriptor].decode("utf-8", errors="replace").strip()
    stderr = output[stderr_descriptor].decode("utf-8", errors="replace").strip()
    if return_code:
        detail = (stderr or stdout).strip()
        raise build_error_t(f"command failed ({return_code}): {shlex.join(argv)}" +
                         (f"\n{detail}" if detail else ""))
    return stdout


def _terminate_owned_process_group(process: subprocess.Popen[bytes]) -> None:
    """Stop and reap a subprocess and descendants in its private session."""
    process_group = process.pid
    try:
        os.killpg(process_group, signal.SIGTERM)
    except ProcessLookupError:
        pass
    except OSError:
        pass

    deadline = time.monotonic() + ProcessTerminateTimeoutSeconds
    while time.monotonic() < deadline:
        # Reap the leader as soon as it exits, but keep checking the group:
        # children can outlive a leader that handled TERM by exiting.
        process.poll()
        try:
            os.killpg(process_group, 0)
        except ProcessLookupError:
            break
        except OSError:
            # An inaccessible group is conservatively treated as still alive.
            pass
        time.sleep(min(0.02, max(0.0, deadline - time.monotonic())))
    else:
        try:
            os.killpg(process_group, signal.SIGKILL)
        except ProcessLookupError:
            pass
        except OSError:
            pass

    # Always collect the direct child, including when it outlived the grace
    # period or exited before its descendants.
    try:
        process.wait()
    except OSError:
        pass


def _resolve_tool(name: str) -> str:
    if Path(name).is_file():
        return str(Path(name).resolve())
    path = shutil.which(name)
    if not path:
        raise build_error_t(f"required tool not found: {name}")
    return str(Path(path).resolve())


def _command_words(specification: str) -> list[str]:
    if Path(specification).is_file() or os.sep in specification or (os.altsep and os.altsep in specification):
        return [specification]
    return shlex.split(specification)


def _tool_identity(path: str, version_args: tuple[str, ...] = ("--version",)) -> dict[str, str]:
    return {"path": path, "version": _run([path, *version_args])}


def _version_tuple(version: str) -> tuple[int, ...]:
    match = re.match(r"^\s*(\d+(?:\.\d+)*)", version)
    if not match:
        raise build_error_t(f"unrecognized dependency version: {version!r}")
    return tuple(int(part) for part in match.group(1).split("."))


def _probe_dependencies(pkg_config: str, env: dict[str, str]) -> dict[str, dict[str, str]]:
    result: dict[str, dict[str, str]] = {}
    for package, minimum in RequiredPackages:
        version = _run([pkg_config, "--modversion", package], env=env)
        if minimum is not None and _version_tuple(version) < minimum:
            minimum_text = ".".join(str(part) for part in minimum)
            raise build_error_t(f"{package} {version} is too old; require >= {minimum_text}")
        result[package] = {
            "version": version,
            "cflags": _run([pkg_config, "--cflags", package], env=env),
            "libs": _run([pkg_config, "--libs", package], env=env),
        }
    return result


def _source_identity(source: Path, expected_commit: str) -> str:
    commit = _run(["git", "-C", str(source), "rev-parse", "HEAD"]).strip().lower()
    if commit != expected_commit.lower():
        raise build_error_t(f"QEMU source commit mismatch: expected {expected_commit}, found {commit}")
    dirty = _run(["git", "-C", str(source), "status", "--porcelain", "--untracked-files=no"])
    if dirty:
        raise build_error_t("QEMU source has tracked modifications; refusing a non-reproducible build")
    return commit


def _config_valid(build_dir: Path) -> bool:
    config_mak = build_dir / "config-host.mak"
    config_h = build_dir / "config-host.h"
    if not config_mak.is_file() or not config_h.is_file():
        return False
    content = config_h.read_text(encoding="utf-8", errors="replace")
    for name in RequiredConfigs:
        if not re.search(rf"^\s*#\s*define\s+{re.escape(name)}(?:\s+1)?\s*$", content, re.MULTILINE):
            return False
    return True


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _load_stamp(path: Path) -> dict[str, Any] | None:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
        if isinstance(value, dict) and value.get("schema") == StampSchema:
            return value
    except (OSError, ValueError):
        pass
    return None


def _complete_stamp(value: dict[str, Any] | None) -> bool:
    """Return whether stamp fields prove prior binary and config provenance."""
    if value is None or value.get("schema") != StampSchema:
        return False
    digest = re.compile(r"^[0-9a-f]{64}$")
    configuration = value.get("configuration_sha256")
    return (
        isinstance(value.get("signature"), str)
        and digest.fullmatch(value["signature"]) is not None
        and isinstance(value.get("inputs"), dict)
        and value.get("enabled_config_macros") == list(RequiredConfigs)
        and value.get("binary") == "qemu-system-x86_64"
        and isinstance(value.get("binary_sha256"), str)
        and digest.fullmatch(value["binary_sha256"]) is not None
        and isinstance(configuration, dict)
        and set(configuration) == {"config-host.mak", "config-host.h"}
        and all(isinstance(item, str) and digest.fullmatch(item) for item in configuration.values())
    )


def _atomic_stamp(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temp_name: str | None = None
    try:
        with tempfile.NamedTemporaryFile("w", encoding="utf-8", dir=path.parent,
                                         prefix=path.name + ".", suffix=".tmp", delete=False) as stream:
            temp_name = stream.name
            json.dump(value, stream, sort_keys=True, separators=(",", ":"))
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp_name, path)
    finally:
        if temp_name and os.path.exists(temp_name):
            os.unlink(temp_name)


def _inside(path: Path, other: Path) -> bool:
    try:
        path.relative_to(other)
        return True
    except ValueError:
        return False


@contextmanager
def _build_lock(build_dir: Path):
    lock_path = build_dir / ".waddle-qemu-build.lock"
    try:
        descriptor = os.open(lock_path, os.O_CREAT | os.O_RDWR, 0o600)
    except OSError as error:
        raise build_error_t(f"could not open QEMU build lock {lock_path}: {error}") from error
    try:
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise build_error_t(f"QEMU build directory is busy: {build_dir}") from error
        yield
    finally:
        os.close(descriptor)


def _configuration_hashes(build_dir: Path) -> dict[str, str]:
    return {
        "config-host.mak": _sha256(build_dir / "config-host.mak"),
        "config-host.h": _sha256(build_dir / "config-host.h"),
    }


def _build(source: Path, build_dir: Path, expected_commit: str, jobs: int | None) -> Path:
    source = source.resolve()
    build_dir = build_dir.resolve()
    if _inside(build_dir, source) or _inside(source, build_dir):
        raise build_error_t("QEMU build directory and source tree must be disjoint")
    build_dir.mkdir(parents=True, exist_ok=True)
    with _build_lock(build_dir):
        return _build_locked(source, build_dir, expected_commit, jobs)


def _build_locked(source: Path, build_dir: Path, expected_commit: str, jobs: int | None) -> Path:
    # This path is reached only after the same-directory exclusive lock is held.
    build_dir.mkdir(parents=True, exist_ok=True)
    stamp_path = build_dir / StampName
    # Retain prior inputs only for cache comparison; invalidate prior success
    # before any source, toolchain, or dependency preflight can fail.
    old_stamp = _load_stamp(stamp_path)
    try:
        stamp_path.unlink()
    except FileNotFoundError:
        pass

    binary = build_dir / "qemu-system-x86_64"
    if binary.exists() or binary.is_symlink():
        if not _complete_stamp(old_stamp):
            raise build_error_t(
                "existing QEMU binary has incomplete or missing provenance; use a fresh build directory"
            )
    elif old_stamp is not None and _complete_stamp(old_stamp):
        raise build_error_t("cached QEMU binary is missing; use a fresh build directory")

    if not re.fullmatch(r"[0-9a-fA-F]{40}", expected_commit):
        raise build_error_t("--expected-source-commit must be a 40-digit Git commit SHA")
    source = source.resolve(strict=True)
    configure = source / "configure"
    if not configure.is_file():
        raise build_error_t(f"QEMU configure script not found: {configure}")
    commit = _source_identity(source, expected_commit)

    # Never inherit ambient compiler/linker flags or tool overrides whose
    # identities are not part of the cache key. The selected compiler and
    # pkg-config inputs are captured below.
    env = os.environ.copy()
    for name in ("CFLAGS", "CPPFLAGS", "LDFLAGS", "CXXFLAGS", "PYTHON", "MESON"):
        env.pop(name, None)
    supported = {"CC", "PKG_CONFIG", "PKG_CONFIG_PATH", "PKG_CONFIG_LIBDIR", "PKG_CONFIG_SYSROOT_DIR"}
    ignored_overrides = sorted(name for name in env if name in {
        "CXX", "AR", "LD", "HOST_CC", "CROSS_PREFIX", "QEMU_CFLAGS", "QEMU_LDFLAGS",
        "CONFIG_SITE", "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "LIBRARY_PATH",
        "GCC_EXEC_PREFIX", "COMPILER_PATH", "SDKROOT", "MACOSX_DEPLOYMENT_TARGET",
        "PYTHONPATH", "PYTHONHOME"
    } and env.get(name))
    if ignored_overrides:
        raise build_error_t("unsupported build environment override(s): " + ", ".join(ignored_overrides))

    compiler_spec = env.get("CC", "cc")
    compiler_words = _command_words(compiler_spec)
    if not compiler_words:
        raise build_error_t("CC is empty")
    compiler_path = _resolve_tool(compiler_words[0])
    compiler_words[0] = compiler_path
    compiler_identity = {"argv": compiler_words, "version": _run([*compiler_words, "--version"], env=env)}
    ninja_path = _resolve_tool("ninja")
    ninja_identity = _tool_identity(ninja_path)
    python_identity = _tool_identity(_resolve_tool("python3"))
    meson_candidate = shutil.which("meson")
    meson_identity = _tool_identity(str(Path(meson_candidate).resolve())) if meson_candidate else None
    pkg_config_spec = env.get("PKG_CONFIG", "pkg-config")
    pkg_config_words = _command_words(pkg_config_spec)
    if not pkg_config_words:
        raise build_error_t("PKG_CONFIG is empty")
    if len(pkg_config_words) != 1:
        raise build_error_t("PKG_CONFIG must name one executable without extra arguments")
    pkg_config_path = _resolve_tool(pkg_config_words[0])
    pkg_identity = _tool_identity(pkg_config_path)
    dependencies = _probe_dependencies(pkg_config_path, env)
    environment_inputs = {name: env.get(name, "") for name in sorted(supported | {"PATH", "LANG", "LC_ALL"})}
    configure_argv = [str(configure), *ConfigureFlags]
    helper_hash = _sha256(Path(__file__).resolve())
    signature_value = {
        "schema": StampSchema,
        "source_commit": commit,
        "configure_argv": configure_argv,
        "helper_sha256": helper_hash,
        "compiler": compiler_identity,
        "ninja": ninja_identity,
        "python": python_identity,
        "meson": meson_identity,
        "pkg_config": pkg_identity,
        "dependencies": dependencies,
        "environment_inputs": environment_inputs,
    }
    signature = hashlib.sha256(json.dumps(signature_value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    if _complete_stamp(old_stamp):
        cached_binary = binary
        if old_stamp.get("binary") != cached_binary.name or not cached_binary.is_file():
            raise build_error_t(
                "cached QEMU binary is missing or has unexpected provenance; use a fresh build directory"
            )
        if old_stamp.get("binary_sha256") != _sha256(cached_binary):
            raise build_error_t(
                "cached QEMU binary hash changed outside this helper; use a fresh build directory"
            )
    matching_stamp = bool(old_stamp and old_stamp.get("signature") == signature and _config_valid(build_dir))
    needs_configure = not matching_stamp
    if matching_stamp:
        needs_configure = old_stamp.get("configuration_sha256") != _configuration_hashes(build_dir)
    if needs_configure:
        run_env = dict(env)
        run_env["CC"] = shlex.join(compiler_words)
        _run(configure_argv, cwd=build_dir, env=run_env, stream_output=True)
        if not _config_valid(build_dir):
            raise build_error_t("configure did not enable all required QEMU features: " + ", ".join(RequiredConfigs))
    else:
        print("QEMU configure signature unchanged; reusing validated configuration")

    ninja_argv = [ninja_path]
    if jobs is not None:
        ninja_argv.extend(("-j", str(jobs)))
    ninja_argv.append("qemu-system-x86_64")
    _run(ninja_argv, cwd=build_dir, env=env, stream_output=True)
    if not _config_valid(build_dir):
        try:
            stamp_path.unlink()
        except FileNotFoundError:
            pass
        raise build_error_t("required QEMU feature macros changed during build")
    if not binary.is_file():
        raise build_error_t(f"QEMU build did not produce {binary}")
    # Verify that neither the selected gitlink nor tracked source changed while
    # configure/build was running. The resulting stamp describes this exact tree.
    final_commit = _source_identity(source, expected_commit)
    if final_commit != commit:
        raise build_error_t("QEMU source identity changed during build")
    stamp = {
        "schema": StampSchema,
        "signature": signature,
        "inputs": signature_value,
        "enabled_config_macros": list(RequiredConfigs),
        "configuration_sha256": _configuration_hashes(build_dir),
        "binary": binary.name,
        "binary_sha256": _sha256(binary),
    }
    _atomic_stamp(stamp_path, stamp)
    return binary


def main(argv: list[str] | None = None) -> int:
    """Build the selected pinned source in its separate output directory.

    The source tree and argv are read-only inputs. The helper owns the selected
    build directory's generated files, lock, and provenance stamp; return 0 only
    after configure validation, Ninja success, source revalidation, and atomic
    stamping, otherwise return 1. Same-directory processes serialize under a
    nonblocking exclusive lock, and a concurrent loser leaves the owner's stamp
    untouched.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True, help="pinned QEMU source directory")
    parser.add_argument("--build-dir", type=Path, required=True, help="fresh out-of-tree build directory")
    parser.add_argument("--expected-source-commit", required=True, help="expected QEMU gitlink commit SHA")
    parser.add_argument("--jobs", type=int, help="maximum parallel Ninja jobs")
    args = parser.parse_args(argv)
    if args.jobs is not None and args.jobs < 1:
        parser.error("--jobs must be at least 1")
    try:
        binary = _build(args.source, args.build_dir, args.expected_source_commit, args.jobs)
    except KeyboardInterrupt:
        print("qemu_build.py: interrupted; owned subprocesses were stopped", file=sys.stderr)
        return 130
    except (build_error_t, OSError) as error:
        print(f"qemu_build.py: error: {error}", file=sys.stderr)
        return 1
    print(f"pinned QEMU source build: {binary}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
