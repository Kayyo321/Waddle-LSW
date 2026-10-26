#!/usr/bin/env python3
"""Gate the session catalog handler at 90% line and taken-arc coverage, then run sanitizers.

The fixture links the real codec and catalog runtime objects (built here with Zig 0.13) and drives
them through a fake feature sender. Coverage is measured only for src/interop/session_catalog_handler.c.
"""
from datetime import datetime, timezone
import gzip
import hashlib
import json
import os
from pathlib import Path
import subprocess
import uuid

Root = Path(__file__).resolve().parents[2]
Source = Root / "src/interop/session_catalog_handler.c"
Header = Root / "src/interop/session_catalog_handler.h"
Fixture = Root / "tests/interop/session_catalog_handler.c"
Runtime = Root / "src/interop/catalog_runtime.zig"
Codec = Root / "src/interop/interop_codec.zig"
Contract = Root / "impl/seamless-interop/claude_45_percent/catalog_runtime_contract.md"
Attempt = (Root / "build/coverage/interop/session_catalog_handler" /
           ("run-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S") + "-" + uuid.uuid4().hex[:8]))
Attempt.mkdir(parents=True, exist_ok=False)
Work = Attempt / "work"
Work.mkdir()
Inputs = [Path(__file__).resolve(), Source, Header, Fixture, Runtime, Codec, Contract]
Hashes = {str(path.relative_to(Root)): hashlib.sha256(path.read_bytes()).hexdigest()
          for path in Inputs}
Zig = os.environ.get("ZIG", "zig")
ZigFlags = ["-Iinclude", "-O", "ReleaseSafe", "-fPIC", "-fcompiler-rt", "-lc"]
CFlags = ["-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-UNDEBUG", "-O0", "-g",
          "-Iinclude", "-Isrc/interop", "-Isrc/daemon"]
Objects = {"catalog_runtime": Attempt / "catalog_runtime.o", "interop_codec": Attempt / "interop_codec.o"}
Commands = [
    [Zig, "build-obj", str(Runtime), *ZigFlags, f"-femit-bin={Objects['catalog_runtime']}"],
    [Zig, "build-obj", str(Codec), *ZigFlags, f"-femit-bin={Objects['interop_codec']}"],
    ["cc", *CFlags, "--coverage", "-fprofile-update=atomic", "-fprofile-abs-path",
     "-c", str(Source), "-o", str(Attempt / "session_catalog_handler.o")],
    ["cc", *CFlags, "--coverage", str(Fixture), str(Attempt / "session_catalog_handler.o"),
     str(Objects["catalog_runtime"]), str(Objects["interop_codec"]), "-lc", "-o", str(Attempt / "runner")],
    [str(Attempt / "runner"), str(Work)],
    ["gcov", "--json-format", "-b", "-c", str(Attempt / "session_catalog_handler.gcno")],
]
(Attempt / "inputs.json").write_text(json.dumps({"sha256": Hashes, "commands": Commands},
                                                 indent=2) + "\n")
for command in Commands:
    subprocess.run(command, cwd=Attempt if command[0] == "gcov" else Root,
                   check=True, stdout=subprocess.DEVNULL if command[0] == "gcov" else None)
Report = json.loads(gzip.decompress((Attempt / "session_catalog_handler.gcov.json.gz").read_bytes()))
Matches = [file for file in Report["files"] if Path(file["file"]).resolve() == Source.resolve()]
if len(Matches) != 1:
    raise RuntimeError("expected exactly one production session_catalog_handler.c coverage scope")
Lines = Matches[0]["lines"]
Arcs = [branch for line in Lines for branch in line.get("branches", [])]
Summary = {}
for kind, entries in (("lines", Lines), ("taken_arcs", Arcs)):
    if not entries:
        raise RuntimeError("empty session catalog handler " + kind + " scope")
    covered = sum(entry["count"] > 0 for entry in entries)
    percent = 100.0 * covered / len(entries)
    Summary[kind] = {"covered": covered, "total": len(entries), "percent": percent}
    print(f"session catalog handler production {kind}: {percent:.2f}% ({covered}/{len(entries)}); "
          f"evidence {Attempt}", flush=True)
    if percent < 90.0:
        raise SystemExit("session catalog handler production coverage is below 90%")
(Attempt / "summary.json").write_text(json.dumps(Summary, indent=2) + "\n")

SanitizerFlags = ["-fsanitize=address,leak,undefined", "-fno-omit-frame-pointer"]
SanitizerBuild = [
    "cc", *CFlags, *SanitizerFlags, str(Source), str(Fixture), str(Objects["catalog_runtime"]),
    str(Objects["interop_codec"]), "-lc", "-o", str(Attempt / "runner-sanitized"),
]
completed = subprocess.run(SanitizerBuild, cwd=Root, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True)
(Attempt / "sanitizer-build.log").write_text("$ " + " ".join(SanitizerBuild) + "\n" + completed.stdout)
if completed.returncode != 0:
    raise subprocess.CalledProcessError(completed.returncode, SanitizerBuild)
SanitizerWork = Attempt / "work-sanitized"
SanitizerWork.mkdir()
SanitizerEnvironment = dict(os.environ)
SanitizerEnvironment["ASAN_OPTIONS"] = "detect_leaks=1:abort_on_error=1:halt_on_error=1"
SanitizerEnvironment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
subprocess.run([str(Attempt / "runner-sanitized"), str(SanitizerWork)], cwd=Root,
               env=SanitizerEnvironment, check=True)
print("session catalog handler ASan/LSan/UBSan run passed; evidence " + str(Attempt), flush=True)
