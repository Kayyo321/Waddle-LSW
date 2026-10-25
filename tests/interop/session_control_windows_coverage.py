#!/usr/bin/env python3
"""Retained coverage and fail-fast sanitizer qualification for guest session C."""
from datetime import datetime, timezone
import gzip
import hashlib
import json
from pathlib import Path
import subprocess
import uuid

Root = Path(__file__).resolve().parents[2]
AttemptRoot = Path("/tmp/session_control_windows_coverage")
Attempt = AttemptRoot / ("run-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S") +
                         "-" + uuid.uuid4().hex[:8])
Attempt.mkdir(parents=True, exist_ok=False)
Production = Root / "src/interop/session_control_windows.c"
Private = Root / "tests/interop/session_control_windows_private.c"
Double = Root / "tests/interop/session_control_windows_api_double.c"
DoubleHeader = Root / "tests/interop/session_control_windows_api_double.h"
Dependencies = [Root / "src/interop/interop_mapping_init.c",
                Root / "src/interop/interop_native_atomic.c"]
Inputs = [Path(__file__).resolve(), Production, Private, Double, DoubleHeader,
          Root / "src/interop/session_control_windows.h",
          Root / "src/interop/windows_socket_owner.h",
          Root / "src/interop/interop_mapping_init.h",
          Root / "src/interop/interop_native_atomic.h",
          Root / "src/interop/session_guest_main.c",
          Root / "impl/seamless-interop/session_control_windows_contract.md",
          *Dependencies]
Hashes = {str(path.relative_to(Root)): hashlib.sha256(path.read_bytes()).hexdigest()
          for path in Inputs}
Flags = ["-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-UNDEBUG",
         "-O0", "-g", "-DInteropGuestSessionTestApi", "-DInteropWindowsSocketTestApi",
         "-Itests/interop", "-Iinclude", "-Isrc/interop", "-ffunction-sections",
         "-fdata-sections"]
Objects = [Attempt / "production.o", Attempt / "fixture.o", Attempt / "double.o",
           Attempt / "mapping_init.o", Attempt / "native_atomic.o"]
Commands = []
for source, output in ((Production, Objects[0]), (Private, Objects[1]), (Double, Objects[2]),
                       (Dependencies[0], Objects[3]), (Dependencies[1], Objects[4])):
    Commands.append(["cc", *Flags, "--coverage", "-fprofile-update=atomic",
                     "-fprofile-abs-path", "-c", str(source), "-o", str(output)])
Commands.append(["cc", *(str(path) for path in Objects), "--coverage",
                 "-Wl,--gc-sections", "-o", str(Attempt / "coverage-test")])
Commands.append([str(Attempt / "coverage-test")])
Commands.append(["gcov", "--json-format", "-b", "-c", str(Attempt / "production.gcno")])
SanitizerFlags = ["-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-UNDEBUG",
                  "-O1", "-g", "-DInteropGuestSessionTestApi", "-DInteropWindowsSocketTestApi",
                  "-Itests/interop", "-Iinclude", "-Isrc/interop", "-fno-omit-frame-pointer",
                  "-fsanitize=address,leak,undefined"]
Commands.append(["cc", *SanitizerFlags, str(Production), str(Private), str(Double),
                 *(str(path) for path in Dependencies), "-o", str(Attempt / "sanitizer-test")])
Commands.append(["env", "ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:strict_string_checks=1",
                 "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1", str(Attempt / "sanitizer-test")])
WindowsCompile = ["x86_64-w64-mingw32-gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                  "-D_WIN32_WINNT=0x0600", "-Iinclude", "-Isrc/interop",
                  "-Isubmodules/looking_glass/module", "-fsyntax-only",
                  "src/interop/session_control_windows.c", "src/interop/session_guest_main.c"]
Commands.append(WindowsCompile)
Toolchain = {tool: subprocess.check_output([tool, "--version"], text=True).splitlines()[0]
             for tool in ("cc", "gcov", "x86_64-w64-mingw32-gcc")}
(Attempt / "inputs.json").write_text(json.dumps({"sha256": Hashes, "commands": Commands,
    "toolchain": Toolchain}, indent=2) + "\n")
for command in Commands:
    subprocess.run(command, cwd=Attempt if command[0] == "gcov" else Root, check=True)
if any(hashlib.sha256(path.read_bytes()).hexdigest() != Hashes[str(path.relative_to(Root))]
       for path in Inputs):
    raise RuntimeError("coverage input changed during qualification")
Report = json.loads(gzip.decompress((Attempt / "production.gcov.json.gz").read_bytes()))
Files = [item for item in Report["files"] if Path(item["file"]).resolve() == Production.resolve()]
if len(Files) != 1:
    raise RuntimeError("expected exactly one production runtime coverage scope")
Lines = Files[0]["lines"]
Branches = [(line["line_number"], arc) for line in Lines for arc in line.get("branches", [])]
Metrics = {}
for kind, entries in (("line", [(line["line_number"], line) for line in Lines]),
                      ("taken_arc", Branches)):
    if not entries:
        raise RuntimeError("empty production " + kind + " scope")
    Covered = sum(entry["count"] > 0 for _, entry in entries)
    Percent = 100 * Covered / len(entries)
    Missed = [line for line, entry in entries if entry["count"] <= 0]
    Metrics[kind] = {"covered": Covered, "total": len(entries), "percent": Percent,
                     "missed_lines_or_arc_sites": Missed}
    print(f"session runtime {kind}: {Percent:.2f}% ({Covered}/{len(entries)}); evidence {Attempt}",
          flush=True)
(Attempt / "summary.json").write_text(json.dumps(Metrics, indent=2) + "\n")
if Metrics["line"]["percent"] < 90 or Metrics["taken_arc"]["percent"] < 90:
    raise SystemExit(1)
