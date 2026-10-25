#!/usr/bin/env python3
"""End-to-end check of `waddle integration` against a temporary HOME and XDG layout.

Runs the built binary (WADDLE_TEST_EXECUTABLE or build/waddle) with a scrubbed
environment, so no real user directory is read or written. Asserts exit codes,
the JSON envelope, the handler desktop entry bytes, and the state-file layout.
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile

executable = os.environ.get("WADDLE_TEST_EXECUTABLE", str(Path("build/waddle").resolve()))


def run(env, *args):
    return subprocess.run([executable, "integration", *args], env=env, capture_output=True, text=True)


def expect(result, status, label):
    assert result.returncode == status, (label, result.returncode, result.stdout, result.stderr)
    return result


with tempfile.TemporaryDirectory(prefix="waddle-integration-") as root_text:
    root = Path(root_text)
    home = root / "home"
    home.mkdir()
    env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": str(home)}

    expect(run(env, "status", "--json"), 0, "status empty")
    doc = json.loads(run(env, "status", "--json").stdout)
    assert doc["schema_version"] == 1 and doc["command"] == "integration" and doc["operation"] == "status"
    assert doc["ok"] is True and doc["results"] == [] and doc["error"] is None

    installed = expect(run(env, "install"), 0, "install")
    assert "installed" in installed.stdout, installed.stdout
    entry = home / ".local/share/applications/waddle-open.desktop"
    text = entry.read_text()
    assert "Exec=\"" in text and " open %F\n" in text and "NoDisplay=true\n" in text, text
    assert "MimeType=application/x-ms-dos-executable;" in text, text
    assert (home / ".local/state/waddle/integration/manifest.json").is_file()
    assert oct((home / ".local/state/waddle/integration").stat().st_mode & 0o777) == "0o700"

    again = expect(run(env, "install"), 0, "install repeat")
    assert "unchanged" in again.stdout, again.stdout
    expect(run(env, "status"), 0, "status ok")

    entry.write_text("user edit\n")
    drift = expect(run(env, "status"), 0, "status drift exits zero")
    assert "modified" in drift.stdout, drift.stdout
    blocked = expect(run(env, "repair"), 1, "repair blocked by modified file")
    assert "blocked" in blocked.stdout, blocked.stdout
    expect(run(env, "remove"), 1, "remove keeps modified file")
    assert entry.read_text() == "user edit\n"

    forced = expect(run(env, "repair", "--force"), 0, "repair --force")
    assert "replaced" in forced.stdout and "backup=" in forced.stdout, forced.stdout
    entry.unlink()
    expect(run(env, "repair"), 0, "repair missing")
    assert entry.is_file()

    removed = expect(run(env, "remove", "--json"), 0, "remove")
    assert json.loads(removed.stdout)["results"][0]["action"] == "removed"
    expect(run(env, "remove"), 0, "remove absent")

    expect(run(env, "bogus"), 2, "unknown subcommand is usage")
    expect(run(env, "install", "--force"), 2, "--force outside repair is usage")
    expect(run(env, "install", "--device", "bad/name"), 2, "invalid device name is usage")
    expect(run(env, "remove", "--all", "--device", "alpha"), 2, "--all with --device is usage")
    expect(run(env, "remove", "--guest"), 1, "guest is not implemented")
    expect(run(env, "install", "--device", "missing"), 1, "unregistered device is not found")
    assert not entry.exists()

    if os.geteuid() != 0:
        system = expect(run(env, "status", "--system"), 1, "system scope needs root")
        assert "permission_denied" in system.stderr, system.stderr

    relative = {"PATH": env["PATH"], "HOME": "relative-home"}
    expect(run(relative, "status"), 1, "relative HOME is an invalid environment")

    xdg_home = root / "xdg"
    xdg_env = {"PATH": env["PATH"], "HOME": str(home), "XDG_DATA_HOME": str(xdg_home / "data"), "XDG_STATE_HOME": str(xdg_home / "state")}
    expect(run(xdg_env, "install"), 0, "install under XDG overrides")
    assert (xdg_home / "data/applications/waddle-open.desktop").is_file()
    assert (xdg_home / "state/waddle/integration/manifest.json").is_file()

print("integration commands end-to-end: ok")
