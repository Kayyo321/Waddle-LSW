#!/usr/bin/env python3
"""Measure the exact PNG writer source and run independent/sanitized checks."""
from datetime import datetime, timezone
import gzip
import hashlib
import json
import os
from pathlib import Path
import subprocess
import uuid

Root = Path(__file__).resolve().parents[2]
Source = Root / "src/interop/png_writer.c"
Header = Root / "src/interop/png_writer.h"
Fixture = Root / "tests/interop/png_writer.c"
Checker = Root / "tests/interop/png_check.py"
Contract = Root / "impl/seamless-interop/png_writer_contract.md"
AttemptRoot = Path("/tmp/png_writer_coverage")
Attempt = AttemptRoot / (
    "run-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S") + "-" +
    uuid.uuid4().hex[:8]
)
Attempt.mkdir(parents=True, exist_ok=False)
PngDirectory = Attempt / "png"
PngDirectory.mkdir()
Inputs = [Path(__file__).resolve(), Source, Header, Fixture, Checker, Contract]
Hashes = {str(path.relative_to(Root)): hashlib.sha256(path.read_bytes()).hexdigest()
          for path in Inputs}
BaseFlags = ["-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-UNDEBUG",
             "-O0", "-g", "-I."]
Commands = [
    ["cc", *BaseFlags, "--coverage", "-fprofile-update=atomic", "-fprofile-abs-path",
     "-c", str(Source), "-o", str(Attempt / "png_writer.o")],
    ["cc", *BaseFlags, "--coverage", str(Fixture), str(Attempt / "png_writer.o"),
     "-o", str(Attempt / "runner")],
    [str(Attempt / "runner"), str(PngDirectory)],
    ["python3", str(Checker), str(PngDirectory)],
    ["gcov", "--json-format", "-b", "-c", str(Attempt / "png_writer.gcno")],
]
(Attempt / "inputs.json").write_text(json.dumps({"sha256": Hashes, "commands": Commands},
                                                 indent=2) + "\n")
for command in Commands:
    subprocess.run(command, cwd=Attempt if command[0] == "gcov" else Root,
                   check=True, stdout=subprocess.DEVNULL if command[0] == "gcov" else None)
Report = json.loads(gzip.decompress((Attempt / "png_writer.gcov.json.gz").read_bytes()))
Matches = [file for file in Report["files"]
           if Path(file["file"]).resolve() == Source.resolve()]
if len(Matches) != 1:
    raise RuntimeError("expected exactly one production png_writer.c coverage scope")
Lines = Matches[0]["lines"]
Arcs = [branch for line in Lines for branch in line.get("branches", [])]
Summary = {}
for kind, entries in (("lines", Lines), ("taken_arcs", Arcs)):
    if not entries:
        raise RuntimeError("empty PNG writer " + kind + " scope")
    covered = sum(entry["count"] > 0 for entry in entries)
    percent = 100.0 * covered / len(entries)
    Summary[kind] = {"covered": covered, "total": len(entries), "percent": percent}
    print(f"PNG writer production {kind}: {percent:.2f}% ({covered}/{len(entries)}); "
          f"evidence {Attempt}", flush=True)
    if percent < 90.0:
        raise SystemExit("PNG writer production coverage is below 90%")
(Attempt / "summary.json").write_text(json.dumps(Summary, indent=2) + "\n")

SanitizerFlags = ["-fsanitize=address,leak,undefined", "-fno-omit-frame-pointer"]
SanitizerCommands = [
    ["cc", *BaseFlags, *SanitizerFlags, str(Source), str(Fixture),
     "-o", str(Attempt / "runner-sanitized")],
]
with (Attempt / "sanitizer-build.log").open("w") as build_log:
    for command in SanitizerCommands:
        completed = subprocess.run(command, cwd=Root, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True)
        build_log.write("$ " + " ".join(command) + "\n" + completed.stdout)
        if completed.returncode != 0:
            raise subprocess.CalledProcessError(completed.returncode, command)
SanitizerEnvironment = dict(os.environ)
SanitizerEnvironment["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1:abort_on_error=1"
SanitizerEnvironment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
SanitizerPngDirectory = Attempt / "png-sanitized"
SanitizerPngDirectory.mkdir()
sanitizer_run = subprocess.run([str(Attempt / "runner-sanitized"),
                                str(SanitizerPngDirectory)], cwd=Root,
                               env=SanitizerEnvironment, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True)
(Attempt / "sanitizer-run.log").write_text(sanitizer_run.stdout)
if sanitizer_run.returncode != 0:
    print(sanitizer_run.stdout, flush=True)
    raise subprocess.CalledProcessError(sanitizer_run.returncode,
                                       str(Attempt / "runner-sanitized"))
subprocess.run(["python3", str(Checker), str(SanitizerPngDirectory)], cwd=Root, check=True)
print(f"PNG writer ASan/LSan/UBSan: passed with leak detection; evidence {Attempt}",
      flush=True)
if any(hashlib.sha256(path.read_bytes()).hexdigest() != Hashes[str(path.relative_to(Root))]
       for path in Inputs):
    raise RuntimeError("PNG writer input changed during qualification")
