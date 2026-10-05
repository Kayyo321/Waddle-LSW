#!/usr/bin/env python3
"""Isolated native CLI acceptance for discovery, default selection and inspection."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import pty
import signal
import select

with tempfile.TemporaryDirectory(prefix="waddle-dev-") as root:
    root = Path(root)
    env = os.environ.copy()
    env.update(HOME=str(root), XDG_CONFIG_HOME=str(root / 'config"日本語'),
               XDG_STATE_HOME=str(root / "state"), XDG_RUNTIME_DIR=str(root / "run"))
    env.pop("WADDLE_MOCK_GUEST_SOCK", None)
    executable = os.environ.get("WADDLE_TEST_EXECUTABLE", str(Path("build/waddle").resolve()))

    def run(*args, expected=0, structured=False):
        result = subprocess.run([executable, *args], env=env, capture_output=True, text=True)
        assert result.returncode == expected, (args, result.returncode, result.stdout, result.stderr)
        if structured:
            document = json.loads(result.stdout)
            assert document["schema_version"] == 1
            assert document["ok"] == (expected == 0)
            assert isinstance(document["results"], list)
            assert document["error"] is None if expected == 0 else document["error"] is not None
            return document
        return result

    assert run("device", "list", "--json", structured=True)["results"] == []
    assert run("device", "default", "--json", structured=True)["results"][0]["data"]["default_device"] is None
    assert run("status", "--all", "--json", structured=True)["results"] == []
    run("stop", "--all", "--json", structured=True)
    run("status", "--all", "--device", "alpha", "--json", expected=2, structured=True)
    run("start", "alpha", "--wait", "--no-wait", expected=2)
    assert not Path(env["XDG_CONFIG_HOME"]).exists()
    assert not Path(env["XDG_STATE_HOME"]).exists()
    run("status", expected=2)
    run("device", "show", "a/b", "--json", expected=2, structured=True)
    run("device", "list", "--json", "--json", expected=2, structured=True)
    run("device", "list", "extra", expected=2)
    run("device", "list", "--running", "--stopped", expected=2)
    run("device", "show", "--clear", "alpha", expected=2)
    run("device", "config", "get", "alpha", "bogus", expected=2)
    run("init", "beta", "--blank-disk", "64M")
    run("init", "alpha", "--blank-disk", "64M")
    listed = run("devices", "--json", structured=True)["results"]
    assert [entry["name"] for entry in listed] == ["alpha", "beta"]
    assert all(entry["data"]["config_valid"] for entry in listed)
    assert all(entry["data"]["state"] == "stopped" for entry in listed)
    assert run("device", "list", "--running", "--json", structured=True)["results"] == []
    assert len(run("device", "list", "--stopped", "--json", structured=True)["results"]) == 2
    run("status", expected=2)
    run("device", "default", "alpha", "--json", structured=True)
    run("status")
    states = run("status", "--all", "--json", structured=True)["results"]
    assert [item["name"] for item in states] == ["alpha", "beta"]
    assert all(item["data"]["daemon_pid"] is None for item in states)
    run("stop", "--all", "--json", structured=True)
    run("kill", "--all", "--json", structured=True)
    run("status", "alpha", "--device", "alpha", expected=2)
    run("status", "--force", expected=2)
    run("stop", "--timeout", "0", expected=2)

    shown = run("device", "show", "alpha", "--json", structured=True)["results"][0]
    assert shown["data"]["is_default"]
    assert '"日本語' in shown["data"]["config_path"]
    assert shown["data"]["config"]["default_shell"] == "powershell.exe"
    values = run("device", "config", "get", "alpha", "--json", structured=True)["results"][0]["data"]
    assert values == dict(memory_mb=4096, vcpus=4, vsock_port=5242,
                          default_shell="powershell.exe", start_timeout=60, stop_timeout=15)
    value = run("device", "config", "get", "alpha", "memory_mb", "--json", structured=True)
    assert value["results"][0]["data"] == {"memory_mb": 4096}
    run("device", "config", "get", "alpha", "bogus", "--json", expected=2, structured=True)
    run("device", "default", "absent", "--json", expected=2, structured=True)
    assert run("device", "default", "--json", structured=True)["results"][0]["data"]["default_device"] == "alpha"
    run("device", "default", "--clear")
    run("device", "default", "--clear")
    run("status", expected=2)
    default_file = Path(env["XDG_CONFIG_HOME"]) / "waddle/default_device"
    default_file.write_text("absent\n")
    default_file.chmod(0o600)
    run("status", expected=2)
    default_file.write_bytes(b"alpha\x00\n")
    run("device", "default", "--json", expected=2, structured=True)
    default_file.unlink()
    run("exec", "--device", "alpha", "--vsock-cid", "3", "--", "cmd.exe", expected=2)
    run("exec", "--socket-path", str(root / "absent.sock"), "--", "cmd.exe", expected=125)
    profile = Path(shown["data"]["config_path"])
    original = profile.read_bytes()
    run("device", "config", "set", "alpha", "memory_mb=8192", "vcpus=8", "--dry-run", "--json", structured=True)
    assert profile.read_bytes() == original
    run("device", "config", "set", "alpha", "memory_mb=8192", "vcpus=0", expected=2)
    assert profile.read_bytes() == original
    run("device", "config", "set", "alpha", "vcpus=8", "vcpus=4", expected=2)
    assert profile.read_bytes() == original
    run("device", "config", "set", "alpha", "memory_mb=8192", "vcpus=8")
    assert "mount =" in profile.read_text()
    values = run("device", "config", "get", "alpha", "--json", structured=True)["results"][0]["data"]
    assert values["memory_mb"] == 8192 and values["vcpus"] == 8
    run("device", "config", "reset", "alpha", "memory_mb", "vcpus")
    assert run("device", "config", "get", "alpha", "memory_mb", "--json", structured=True)["results"][0]["data"] == {"memory_mb": 4096}
    # Same-user orphan disk holders must block mutations even without a socket.
    with open(shown["data"]["disk_image"], "rb"):
        run("device", "config", "set", "alpha", "vcpus=8", expected=1)
    mounts = run("fs", "alpha", "--json", structured=True)["results"][0]["data"]["mounts"]
    assert len(mounts) == 1 and mounts[0]["host_path"] == str(root)
    run("fs", "test", "alpha", "--json", structured=True)
    log_dir = Path(shown["data"]["state_dir"]) / "logs"
    log_dir.mkdir(mode=0o700)
    (log_dir / "qemu.log").write_text("one\ntwo\nthree\n")
    log = run("logs", "alpha", "--target", "qemu", "--lines", "2", "--json", structured=True)
    assert log["results"][0]["data"]["text"] == "two\nthree\n"
    run("logs", "alpha", "--follow", "--json", expected=2)
    run("logs", "alpha", "--target", "bogus", expected=2)
    run("shell", "alpha", "--device", "alpha", expected=2)
    for subcommand in ("start", "stop", "restart", "kill", "status", "logs", "fs", "shell", "exec"):
        run(subcommand, "--help")
    run("device", "doctor", "alpha", "--json", structured=True)
    # A stale private socket is reported and only explicit repair removes it.
    import socket
    runtime = Path(env["XDG_RUNTIME_DIR"]) / "waddle/alpha"
    runtime.mkdir(parents=True, mode=0o700)
    for parent in (Path(env["XDG_RUNTIME_DIR"]), runtime.parent):
        parent.chmod(0o700)
    stale = socket.socket(socket.AF_UNIX)
    stale.bind(str(runtime / "daemon.sock"))
    stale.close()
    run("device", "doctor", "alpha", "--json", expected=1, structured=True)
    run("device", "doctor", "alpha", "--repair", "--dry-run", "--json", expected=0, structured=True)
    assert (runtime / "daemon.sock").exists()
    run("device", "doctor", "alpha", "--repair", "--json", structured=True)
    assert not (runtime / "daemon.sock").exists()
    runtime.rmdir()
    runtime.parent.rmdir()
    Path(env["XDG_RUNTIME_DIR"]).rmdir()
    profile.write_text("[subsystem]\nvsock_cid=2\n")
    listed = run("device", "list", "--json", structured=True)["results"]
    assert not listed[0]["data"]["config_valid"]
    run("device", "default", "alpha", expected=2)
    run("device", "config", "get", "alpha", expected=2)
    run("status", "alpha", expected=2)
    batch = run("status", "--all", "--json", expected=1, structured=True)["results"]
    assert len(batch) == 2 and not batch[0]["ok"] and batch[1]["ok"]
    run("device", "doctor", "alpha", "--json", expected=1, structured=True)

    assert not Path(env["XDG_RUNTIME_DIR"]).exists(), "read commands must never spawn or create runtime files"
print("device_commands: isolated JSON, selection, inspection, malformed and raw-transport cases passed")

# Text confirmation is cancellable, exact-name only and never applies on mismatch.
for mode in ('cancel','confirm','mismatch','overlong'):
    with tempfile.TemporaryDirectory(prefix='wd-confirm-') as temporary:
        root=Path(temporary)
        env=dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root/'config'),
                 XDG_STATE_HOME=str(root/'state'), XDG_RUNTIME_DIR=str(root/'run'))
        native_executable=str(Path('build/waddle').resolve())
        driver=native_executable if mode=='cancel' else os.environ.get('WADDLE_TEST_EXECUTABLE',native_executable)
        subprocess.run([driver,'device','init','confirmed','--blank-disk','1M'],env=env,check=True,capture_output=True)
        master,slave=pty.openpty()
        process=subprocess.Popen([driver,'device','remove','confirmed'],env=env,stdin=slave,
                                 stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        try:
            assert select.select([process.stdout],[],[],10)[0], 'confirmation prompt missing'
            assert 'Type the exact name' in os.read(process.stdout.fileno(),4096).decode()
            if mode=='cancel': process.send_signal(signal.SIGINT)
            else: os.write(master, {'confirm':b'confirmed\n','mismatch':b'wrong\n','overlong':b'x'*80+b'\n'}[mode])
            output,diagnostics=process.communicate(timeout=10)
            assert process.returncode==(0 if mode=='confirm' else 130),(mode,process.returncode,output,diagnostics)
            assert (root/'config/waddle/devices/confirmed.ini').is_file()==(mode!='confirm')
            assert (root/'state/waddle/devices/confirmed/disk.qcow2').is_file()==(mode!='confirm')
        finally:
            if process.poll() is None: process.kill();process.wait()
            os.close(master);os.close(slave)
print('device confirmation: exact name removes; SIGINT/mismatch/overlong cancel and preserve device')
