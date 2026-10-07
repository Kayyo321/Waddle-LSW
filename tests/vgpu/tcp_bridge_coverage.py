#!/usr/bin/env python3
"""Exercise actual immutable worker lifecycles and native acquisition faults, no exclusions."""
import gzip
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import subprocess
import sys
import time

root = Path.cwd()
mode = sys.argv[1] if len(sys.argv) > 1 else "coverage"
assert mode in ("coverage", "sanitizers")
output = root / ("build/tcp_controller_" + mode + "_" + secrets.token_hex(4))
output.mkdir(mode=0o700)
strict = ["-D_GNU_SOURCE", "-Iinclude", "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-g"]
flags = ["--coverage", "-fprofile-abs-path", "-O0", "-DTcpControllerCoverage"] if mode == "coverage" else ["-O1", "-fsanitize=address,leak,undefined", "-fno-omit-frame-pointer"]
native = ["venus_tcp_client", "venus_tcp_server", "venus_tcp_socket", "venus_guest", "venus_rpc", "venus_channel", "venus_session", "venus_region", "venus_ring", "venus_wait", "venus_stream_linux", "venus_worker"]
wraps = ["nanosleep", "open", "fstat", "read", "write", "close", "fsync", "setitimer", "sigaction", "memfd_create", "ftruncate", "mmap", "munmap", "socketpair", "venus_tcp_now_ms", "venus_region_init", "venus_session_init", "venus_channel_init", "venus_worker_create", "venus_channel_deadline", "venus_channel_handshake", "venus_rpc_init", "venus_rpc_exchange", "venus_capabilities_decode", "venus_capabilities_compatible", "venus_guest_init", "venus_tcp_server_negotiate", "venus_tcp_server_ack_retired", "venus_tcp_socket_listen", "venus_tcp_socket_accept", "venus_tcp_server_authenticate", "venus_tcp_server_step", "venus_worker_poll", "venus_worker_destroy"]
# Fresh private codecs; no shared object or worker rebuild. Pin every compiler input
# before/after compilation so a concurrent source edit fails this gate honestly.
codec = output / "codec"
codec.mkdir()
modules = ["venus_bounds", "venus_control", "venus_request", "venus_capabilities", "venus_tcp_wire"]
inputs = set((root / "include/waddle").glob("*.h"))
def collect(path):
    if path in inputs:
        return
    inputs.add(path)
    import re
    for name in re.findall(r'@import\("([^"/]+\.zig)"\)', path.read_text()):
        collect(path.parent / name)
for name in modules:
    collect(root / ("src/vgpu/" + name + ".zig"))
inputs.update(root / ("src/vgpu/" + name + ".c") for name in native)
inputs.update([root / "src/vgpu/tcp_bridge_main.c", root / "tests/vgpu/tcp_bridge.c"])
input_hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}
for name in modules:
    subprocess.run(["zig", "build-obj", "src/vgpu/" + name + ".zig", "-Iinclude", "-O", "ReleaseSafe", "-fPIC", "-fcompiler-rt", "-lc", "-femit-bin=" + str(codec / (name + ".o"))], check=True)
subprocess.run(["cc", *strict, *flags, "tests/vgpu/tcp_bridge.c", *("src/vgpu/" + name + ".c" for name in native), *(str(codec / (name + ".o")) for name in modules), "-Wl," + ",".join("--wrap=" + name for name in wraps), "-o", str(output / "runner")], check=True)
assert input_hashes == {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}, "Compiler inputs changed during private snapshot"
(codec / "snapshot.json").write_text(json.dumps(input_hashes, indent=2))
env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:halt_on_error=1")
runtime = output / "runtime"
runtime.mkdir()
worker_name = "waddle_vgpu_worker" if mode == "coverage" else "waddle_vgpu_worker_sanitized"
records = []
for name in [worker_name, "vendor/virglrenderer/src/libvirglrenderer.so.1", "vendor/virglrenderer/server/virgl_render_server"]:
    source = root / "build" / name
    destination = runtime / name
    destination.parent.mkdir(parents=True, exist_ok=True)
    before = source.stat()
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    shutil.copy2(source, destination)
    after = source.stat()
    assert (before.st_ino, before.st_size, before.st_mtime_ns) == (after.st_ino, after.st_size, after.st_mtime_ns)
    assert digest == hashlib.sha256(source.read_bytes()).hexdigest() == hashlib.sha256(destination.read_bytes()).hexdigest()
    records.append({"source": str(source), "copy": str(destination), "sha256": digest, "mtime_ns": before.st_mtime_ns})
(runtime / "snapshot.json").write_text(json.dumps({"files": records, "boundary": "Worker C is instrumented in sanitizers mode; pinned normal renderer library/server are unchanged external dependencies."}, indent=2))
worker = runtime / worker_name
env["RENDER_SERVER_EXEC_PATH"] = str(runtime / "vendor/virglrenderer/server/virgl_render_server")
env["VK_DRIVER_FILES"] = ":".join(str(path.resolve()) for path in sorted((root / "build/mesa_cpu_cache_all_icds").glob("*.json")))
assert env["VK_DRIVER_FILES"]
subprocess.run([str(output / "runner"), "--helpers"], env=env, check=True, timeout=10)
assert subprocess.run([str(output / "runner")], env=env, capture_output=True, timeout=10).returncode == 2
faults = [(None, 1), ("clock", 1), ("clock_overflow", 1), ("timer", 1), ("timer", 2), ("timer", 3), ("timer", 4)]
faults += [("signal", index) for index in range(1, 5)]
faults += [(name, 1) for name in ["memfd", "truncate", "map", "socketpair", "venus_region_init", "venus_session_init", "venus_channel_init", "venus_worker_create", "venus_channel_deadline", "venus_channel_handshake", "venus_rpc_init", "venus_rpc_exchange", "venus_capabilities_decode", "venus_capabilities_compatible", "venus_guest_init", "venus_tcp_server_negotiate", "venus_tcp_server_ack_retired", "unmap", "budget_first", "budget_capture", "cap_status", "cap_extent", "cap_changed", "venus_tcp_socket_listen", "venus_tcp_socket_accept", "venus_tcp_server_authenticate", "venus_tcp_server_step", "forward_cancel", "venus_worker_poll", "venus_worker_destroy"]]
faults += [(name, 1) for name in ["bad_config", "relative_worker", "missing_worker", "existing_ready", "lost_reaping_proof", "lost_reaping_destroy"]]
faults += [("close", index) for index in [2, 6, 7, 8]]
for index, (fault, call) in enumerate(faults):
    directory = output / str(index)
    directory.mkdir(mode=0o700)
    reservation = socket.socket()
    reservation.bind(("127.0.0.1", 0))
    port = reservation.getsockname()[1]
    reservation.close()
    config = {"version": 1, "host": "127.0.0.1", "port": port, "token": secrets.token_hex(32), "exchange_timeout_ms": 5000, "icd_path": "C:\\fixture.dll"}
    path = directory / "config.json"
    path.write_text(json.dumps(config))
    path.chmod(0o600)
    selected_worker = str(worker)
    if fault == "bad_config":
        path.chmod(0o644)
    if fault == "relative_worker":
        selected_worker = "relative_worker"
    if fault == "missing_worker":
        selected_worker = str(directory / "missing")
    if fault == "existing_ready":
        (directory / "ready").write_text("existing\n")
    host_env = dict(env)
    if fault:
        host_env["WADDLE_TCP_TEST_FAULT"] = fault
        host_env["WADDLE_TCP_TEST_CALL"] = str(call)
    with (directory / "stdout.log").open("w") as stdout, (directory / "stderr.log").open("w") as stderr:
        host = subprocess.Popen([str(output / "runner"), str(path), selected_worker, str(directory / "ready"), str(directory / "result")], env=host_env, stdout=stdout, stderr=stderr)
        try:
            deadline = time.monotonic() + 8
            while host.poll() is None and not (directory / "ready").exists() and time.monotonic() < deadline:
                time.sleep(.01)
            if (directory / "ready").exists() and host.poll() is None:
                client = subprocess.run([str(output / "runner"), "--client", str(path)], env=env, capture_output=True, timeout=20)
                (directory / "client.log").write_bytes(client.stdout + client.stderr)
            if fault in ("lost_reaping_proof", "lost_reaping_destroy"):
                assert host.poll() is None
                assert not (directory / "result").exists()
                assert "tcp-venus-receiver" in Path(f"/proc/{host.pid}/maps").read_text()
                assert "lost worker reaping proof" in (directory / "stderr.log").read_text()
                host.kill()  # Trusted failure fixture terminates the retained controller only after proof checks.
                assert host.wait(timeout=10) < 0
                print("Actual controller lost-proof retention PASS: no Ack/result, views retained", flush=True)
                continue
            code = host.wait(timeout=20)
            assert code == (0 if fault is None else 1), (fault, call, code)
            report = (directory / "result").read_text()
            assert "worker_retired=1\n" in report
            fields = dict(line.split("=", 1) for line in report.splitlines())
            if int(fields["worker_pid"]):
                try:
                    os.kill(-int(fields["worker_pid"]), 0)
                except ProcessLookupError:
                    pass
                else:
                    raise AssertionError("Controller leaked an owned worker process group")
            assert "AddressSanitizer" not in (directory / "stderr.log").read_text()
            print("Actual controller case", index, fault or "normal", call, "PASS", flush=True)
        finally:
            if host.poll() is None:
                host.terminate()
                host.wait(timeout=15)
if mode == "coverage":
    files = list(output.glob("*tcp_bridge.gcno"))
    assert len(files) == 1
    subprocess.run(["gcov", "--json-format", "-b", "-c", str(files[0])], cwd=output, check=True)
    report = json.loads(gzip.decompress(next(output.glob("*tcp_bridge.gcov.json.gz")).read_bytes()))
    source = str(root / "src/vgpu/tcp_bridge_main.c")
    lines = next(record["lines"] for record in report["files"] if record["file"] == source)
    branches = [branch for line in lines for branch in line.get("branches", [])]
    for kind, records in (("line", lines), ("branch", branches)):
        covered = sum(record["count"] > 0 for record in records)
        percentage = 100 * covered / len(records)
        print(f"Actual controller production {kind} coverage: {percentage:.2f}% ({covered}/{len(records)})", flush=True)
        assert percentage >= 90
print("Actual lifecycle artifacts:", output)
