#!/usr/bin/env python3
"""Gate real production readiness probe/client lines and taken branch arcs at90%."""
from datetime import datetime, timezone
import gzip
import hashlib
import json
from pathlib import Path
import re
import subprocess
import uuid

Root = Path(__file__).resolve().parents[2]
Modules = {"probe": ("daemon_guest_ready", "test_guest_ready"),
           "client": ("daemon_guest_ready_client", "test_guest_ready_client_faults")}
for name, (module, fixture_name) in Modules.items():
    source = Root / "src/daemon" / (module + ".c")
    fixture = Root / "tests/daemon" / (fixture_name + ".c")
    attempt = Root / "build/coverage/daemon" / module / (
        "run-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S") + "-" + uuid.uuid4().hex[:8])
    attempt.mkdir(parents=True, exist_ok=False)
    inputs = [source, fixture, Root / "src/daemon/daemon_client.h",
              Root / "src/daemon/daemon_guest_ready.h", Root / "src/daemon/daemon_wire.h",
              Root / "src/daemon/daemon_wire.zig", Root / "include/waddle/daemon_protocol.h",
              Root / "build/daemon_wire.o", Path(__file__).resolve()]
    hashes = {str(path.relative_to(Root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}
    wrappers = sorted(set(re.findall(r"\b__wrap_([a-z_][a-z_0-9]*)\s*\(", fixture.read_text())))
    flags = ["-D_GNU_SOURCE", "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
             "-UNDEBUG", "-O0", "-g", "-Iinclude", "-Isrc/daemon", "-pthread"]
    commands = [
        ["cc", *flags, "--coverage", "-fprofile-abs-path", "-c", str(source), "-o", str(attempt / "production.o")],
        ["cc", *flags, "-c", str(fixture), "-o", str(attempt / "fixture.o")],
        ["cc", str(attempt / "production.o"), str(attempt / "fixture.o"), "build/daemon_wire.o",
         "--coverage", "-pthread", *["-Wl,--wrap=" + wrapper for wrapper in wrappers], "-o", str(attempt / "runner")],
        [str(attempt / "runner")],
        ["gcov", "--json-format", "-b", "-c", str(attempt / "production.gcno")],
    ]
    (attempt / "inputs.json").write_text(json.dumps({"sha256": hashes, "commands": commands}, indent=2) + "\n")
    for command in commands:
        subprocess.run(command, cwd=attempt if command[0] == "gcov" else Root,
                       stdout=subprocess.DEVNULL if command[0] == "gcov" else None, check=True)
    if any(hashlib.sha256(path.read_bytes()).hexdigest() != hashes[str(path.relative_to(Root))] for path in inputs):
        raise RuntimeError("qualification inputs changed during run")
    report = json.loads(gzip.decompress((attempt / "production.gcov.json.gz").read_bytes()))
    files = [item for item in report["files"] if Path(item["file"]).resolve() == source]
    if len(files) != 1:
        raise RuntimeError("expected one exact production scope")
    lines = files[0]["lines"]
    branches = [branch for line in lines for branch in line.get("branches", [])]
    summary = {}
    failed = False
    for kind, entries in [("line", lines), ("taken_branch", branches)]:
        if not entries:
            raise RuntimeError("empty production scope")
        covered = sum(entry["count"] > 0 for entry in entries)
        summary[kind] = {"covered": covered, "total": len(entries), "percent": 100 * covered / len(entries)}
        print(module, kind, summary[kind], "evidence", attempt, flush=True)
        failed |= 100 * covered < 90 * len(entries)
    (attempt / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    if failed:
        raise SystemExit(1)
