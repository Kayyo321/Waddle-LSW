#!/usr/bin/env python3
"""Retain Linux mapping fault-test, sanitizer, and production coverage evidence."""
from datetime import datetime, timezone
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import uuid

root = Path(__file__).resolve().parents[2]
source = root / "src/interop/interop_mapping_linux.c"
fixture = root / "tests/interop/mapping_linux.c"
private = root / "tests/interop/mapping_linux_private.c"
attempt_root = root / "build/coverage/interop/mapping_linux"
attempt_root.mkdir(parents=True, exist_ok=True)
attempt_name = "run-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S") + "-" + uuid.uuid4().hex[:8]
attempt = attempt_root / attempt_name
attempt.mkdir(parents=False, exist_ok=False)

inputs = [
    Path(__file__).resolve(), source, root / "src/interop/interop_mapping_linux.h",
    root / "src/interop/interop_mapping_init.c", root / "src/interop/interop_mapping_init.h",
    root / "src/interop/interop_native_atomic.c", root / "src/interop/interop_native_atomic.h",
    root / "src/interop/interop_layout.h", root / "src/av/av_kvmfr.h",
    root / "include/waddle/interop_memory.h", root / "submodules/looking_glass/module/kvmfr.h",
    fixture, private, root / "impl/seamless-interop/mapping_linux_contract.md",
    root / "build/interop_layout_abi.o",
]
input_hashes = {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in inputs}
wrap_symbols = sorted(set(re.findall(r"\b__wrap_([a-z_][a-z_0-9]*)\s*\(",
                                    fixture.read_text())))
wrap_flags = ["-Wl,--wrap=" + symbol for symbol in wrap_symbols]
base_flags = ["-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-UNDEBUG",
              "-O0", "-g", "-fno-omit-frame-pointer", "-Iinclude", "-Isrc/interop",
              "-Isubmodules/looking_glass/module", "-pthread"]
coverage_flags = ["--coverage", "-fprofile-update=atomic", "-fprofile-abs-path"]
commands = [
    ["cc", *base_flags, *coverage_flags, "-c", str(private), "-o", str(attempt / "owner.o")],
    ["cc", *base_flags, "-c", str(fixture), "-o", str(attempt / "fixture.o")],
    ["cc", *base_flags, "-c", str(root / "src/interop/interop_mapping_init.c"),
     "-o", str(attempt / "init.o")],
    ["cc", *base_flags, "-c", str(root / "src/interop/interop_native_atomic.c"),
     "-o", str(attempt / "atomic.o")],
    ["cc", str(attempt / "owner.o"), str(attempt / "fixture.o"), str(attempt / "init.o"),
     str(attempt / "atomic.o"), str(root / "build/interop_layout_abi.o"), "--coverage",
     "-pthread", *wrap_flags, "-o", str(attempt / "runner")],
    [str(attempt / "runner")],
    ["gcov", "--json-format", "-b", "-c", str(attempt / "owner.gcno")],
]
toolchain = {tool: subprocess.check_output([tool, "--version"], text=True).splitlines()[0]
             for tool in ("cc", "gcov")}
evidence = {
    "started_at": datetime.now(timezone.utc).isoformat(),
    "input_sha256": input_hashes,
    "commands": commands,
    "wrappers": wrap_symbols,
    "toolchain": toolchain,
}
(attempt / "inputs.json").write_text(json.dumps(evidence, indent=2) + "\n")

def run_command(index, command, cwd, env=None):
    result = subprocess.run(command, cwd=cwd, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, env=env)
    (attempt / f"command-{index:02d}.log").write_text(result.stdout)
    if result.returncode != 0:
        failure = {"index": index, "command": command, "returncode": result.returncode,
                   "output": result.stdout}
        (attempt / "failure.json").write_text(json.dumps(failure, indent=2) + "\n")
        print(result.stdout, end="")
        raise subprocess.CalledProcessError(result.returncode, command, output=result.stdout)
    return result.stdout

try:
    for index, command in enumerate(commands):
        run_command(index, command, root if command[0] != "gcov" else attempt)
    if any(hashlib.sha256(path.read_bytes()).hexdigest() != input_hashes[str(path.relative_to(root))]
           for path in inputs):
        raise RuntimeError("Inputs changed during this run; no coverage acceptance")

    report = json.loads(gzip.decompress((attempt / "owner.gcov.json.gz").read_bytes()))
    scoped_files = [entry for entry in report["files"]
                    if Path(entry["file"]).resolve() == source.resolve()]
    if len(scoped_files) != 1:
        raise RuntimeError("Expected exactly one production Linux mapping source scope")
    lines = scoped_files[0]["lines"]
    branches = [(line["line_number"], branch) for line in lines
                for branch in line.get("branches", [])]
    summary = {}
    failed = False
    for kind, entries in (("line", [(line["line_number"], line) for line in lines]),
                          ("branch", branches)):
        if not entries:
            raise RuntimeError("Empty production " + kind + " coverage scope")
        covered = sum(entry["count"] > 0 for _, entry in entries)
        percentage = 100 * covered / len(entries)
        summary[kind] = {"covered": covered, "total": len(entries), "percent": percentage,
                         "missed_lines": [line for line, entry in entries if entry["count"] <= 0]}
        print(f"Linux mapping production {kind}: {percentage:.2f}% ({covered}/{len(entries)})")
        if percentage < 90:
            failed = True
    (attempt / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    if failed:
        raise RuntimeError("Production mapping coverage is below the 90% gate")

    sanitizer_flags = ["-fsanitize=address,leak,undefined"]
    sanitized = [
        ["cc", *base_flags, *sanitizer_flags, "-c", str(private), "-o", str(attempt / "asan-owner.o")],
        ["cc", *base_flags, *sanitizer_flags, "-c", str(fixture), "-o", str(attempt / "asan-fixture.o")],
        ["cc", *base_flags, *sanitizer_flags, "-c",
         str(root / "src/interop/interop_mapping_init.c"), "-o", str(attempt / "asan-init.o")],
        ["cc", *base_flags, *sanitizer_flags, "-c",
         str(root / "src/interop/interop_native_atomic.c"), "-o", str(attempt / "asan-atomic.o")],
        ["cc", *sanitizer_flags, str(attempt / "asan-owner.o"),
         str(attempt / "asan-fixture.o"), str(attempt / "asan-init.o"),
         str(attempt / "asan-atomic.o"), str(root / "build/interop_layout_abi.o"),
         "-pthread", *wrap_flags, "-o", str(attempt / "asan-runner")],
        [str(attempt / "asan-runner")],
    ]
    evidence["sanitizer_commands"] = sanitized
    evidence["sanitizer_runtime_environment"] = {
        "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1:abort_on_error=1",
        "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
        "UBSAN_OPTIONS": "halt_on_error=1",
    }
    (attempt / "inputs.json").write_text(json.dumps(evidence, indent=2) + "\n")
    for index, command in enumerate(sanitized, start=len(commands)):
        env = None
        if index == len(commands) + 5:
            env = {**os.environ, "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1:abort_on_error=1",
        "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
                   "UBSAN_OPTIONS": "halt_on_error=1"}
        run_command(index, command, root, env)
    (attempt / "result.json").write_text(json.dumps({
        "status": "passed",
        "coverage": summary,
        "sanitizers": ["address", "leak", "undefined"],
        "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1:abort_on_error=1",
        "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
    }, indent=2) + "\n")
except Exception as error:
    if not (attempt / "failure.json").exists():
        (attempt / "failure.json").write_text(json.dumps({
            "status": "failed",
            "error_type": type(error).__name__,
            "error": str(error),
        }, indent=2) + "\n")
    print(f"Mapping Linux evidence retained at {attempt}")
    raise

print(f"Mapping Linux evidence retained at {attempt}")
