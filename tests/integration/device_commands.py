#!/usr/bin/env python3
"""Isolated native CLI acceptance for discovery, default selection and inspection."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

with tempfile.TemporaryDirectory(prefix="waddle-dev-") as root:
    root = Path(root)
    env = os.environ.copy()
    env.update(HOME=str(root), XDG_CONFIG_HOME=str(root / 'config"日本語'),
               XDG_STATE_HOME=str(root / "state"), XDG_RUNTIME_DIR=str(root / "run"))
    env.pop("WADDLE_MOCK_GUEST_SOCK", None)
    executable = str(Path("build/waddle").resolve())

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
    assert not Path(env["XDG_CONFIG_HOME"]).exists()
    assert not Path(env["XDG_STATE_HOME"]).exists()
    run("status", expected=2)
    run("device", "show", "a/b", "--json", expected=2, structured=True)
    run("device", "list", "--json", "--json", expected=2, structured=True)
    run("device", "list", "extra", expected=2)
    run("device", "list", "--running", "--stopped", expected=2)
    run("device", "show", "--clear", "alpha", expected=2)
    run("device", "config", "get", "alpha", "bogus", expected=2)
    run("init", "beta")
    run("init", "alpha")
    listed = run("devices", "--json", structured=True)["results"]
    assert [entry["name"] for entry in listed] == ["alpha", "beta"]
    assert all(entry["data"]["config_valid"] for entry in listed)
    assert all(entry["data"]["state"] == "stopped" for entry in listed)
    assert run("device", "list", "--running", "--json", structured=True)["results"] == []
    assert len(run("device", "list", "--stopped", "--json", structured=True)["results"]) == 2
    run("status", expected=2)
    run("device", "default", "alpha", "--json", structured=True)
    run("status")
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
    profile.write_text("[subsystem]\nvsock_cid=2\n")
    listed = run("device", "list", "--json", structured=True)["results"]
    assert not listed[0]["data"]["config_valid"]
    run("device", "default", "alpha", expected=2)
    run("device", "config", "get", "alpha", expected=2)
    run("status", "alpha", expected=2)
    assert not Path(env["XDG_RUNTIME_DIR"]).exists(), "read commands must never spawn or create runtime files"
print("device_commands: isolated JSON, selection, inspection, malformed and raw-transport cases passed")
