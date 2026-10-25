#!/usr/bin/env python3
"""Measure exact session_control_host production C line and branch coverage."""
from datetime import datetime, timezone
import gzip
import hashlib
import json
from pathlib import Path
import re
import subprocess
import uuid

Root = Path(__file__).resolve().parents[2]
Source = Root / "src/interop/session_control_host.c"
Fixture = Root / "tests/interop/session_control_host.c"
Private = Root / "tests/interop/session_control_host_private.c"
CliFixture = Root / "tests/interop/session_main.c"
CliPrivate = Root / "tests/interop/session_main_private.c"
AttemptRoot = Path("/tmp/session_control_host_coverage")
Attempt = AttemptRoot / (
    "run-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S") + "-" +
    uuid.uuid4().hex[:8]
)
Attempt.mkdir(parents=True, exist_ok=False)
WrapSymbols = sorted(set(re.findall(
    r"\b__wrap_([a-z_][a-z_0-9]*)\s*\(", Fixture.read_text()
)))
WrapFlags = ["-Wl,--wrap=" + name for name in WrapSymbols]
ObjectPaths = [
    Root / "build/daemon_wire.o",
    Root / "build/device_storage.o",
    Root / "build/daemon_config.o",
    Root / "build/path_rules.o",
    Root / "build/interop_control_peer_abi.o",
    Root / "build/interop_codec.o",
    Root / "build/interop_layout_abi.o",
]
Sources = [
    Root / "src/interop/session_app.c",
    Root / "src/interop/session_supervisor.c",
    Root / "src/interop/session_handlers_builtin.c",
    Root / "src/interop/session_host.c",
    Root / "src/interop/linux_session_owner.c",
    Root / "src/interop/linux_socket_owner.c",
    Root / "src/interop/interop_mapping_linux.c",
    Root / "src/interop/interop_mapping_init.c",
    Root / "src/interop/interop_native_atomic.c",
    Root / "src/daemon/daemon_device.c",
]
Inputs = [Path(__file__).resolve(), Source, Fixture, Private, CliFixture, CliPrivate,
          Root / "src/interop/session_main.c",
          Root / "src/interop/session_control_host.h",
          Root / "src/interop/session_host.h",
          Root / "src/daemon/daemon_wire.h",
          Root / "include/waddle/interop_linux_socket.h",
          Root / "include/waddle/interop_memory.h",
          Root / "impl/seamless-interop/session_control_host_contract.md",
          *Sources, *ObjectPaths]
Hashes = {str(path.relative_to(Root)): hashlib.sha256(path.read_bytes()).hexdigest()
          for path in Inputs}
Flags = ["-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-UNDEBUG",
         "-O0", "-g", "-D_GNU_SOURCE", "-Iinclude", "-Isrc/daemon", "-Isrc",
         "-Isrc/common", "-Isrc/cli", "-Isubmodules/looking_glass/module",
         "-ffunction-sections", "-fdata-sections", "-pthread"]
Commands = [["cc", *Flags, "--coverage", "-fprofile-update=atomic",
             "-fprofile-abs-path", "-c", str(Private), "-o", str(Attempt / "runtime-private.o")],
            ["cc", *Flags, "--coverage", "-fprofile-update=atomic",
             "-fprofile-abs-path", "-c", str(CliPrivate), "-o", str(Attempt / "cli-private.o")],
            ["cc", *Flags, "-c", str(Fixture), "-o", str(Attempt / "fixture.o")]]
for index, source in enumerate(Sources):
    Commands.append(["cc", *Flags, "-c", str(source), "-o",
                     str(Attempt / f"dependency-{index}.o")])
LinkInputs = [str(Attempt / "runtime-private.o"), str(Attempt / "cli-private.o"),
              str(Attempt / "fixture.o"),
              *(str(Attempt / f"dependency-{index}.o")
                for index in range(len(Sources))),
              *(str(path) for path in ObjectPaths)]
Commands.append(["cc", *LinkInputs, "--coverage", "-pthread", "-Wl,--gc-sections",
                 *WrapFlags, "-o", str(Attempt / "runner")])
Commands.append([str(Attempt / "runner")])
Commands.extend([
    ["cc", *Flags, "-DWaddleSessionNoRuntimeMain", "-c", str(Fixture),
     "-o", str(Attempt / "fixture-no-main.o")],
    ["cc", *Flags, "-c", str(CliFixture), "-o", str(Attempt / "cli-fixture.o")],
    ["cc", *Flags, "-c", str(Source), "-o", str(Attempt / "cli-control.o")],
    ["cc", str(Attempt / "cli-private.o"), str(Attempt / "cli-fixture.o"),
     str(Attempt / "cli-control.o"), str(Attempt / "fixture-no-main.o"),
     *(str(Attempt / f"dependency-{index}.o") for index in range(len(Sources))),
     *(str(path) for path in ObjectPaths), "--coverage", "-pthread",
     "-Wl,--gc-sections", *WrapFlags, "-o", str(Attempt / "cli-runner")],
    [str(Attempt / "cli-runner")],
    ["gcov", "--json-format", "-b", "-c", str(Attempt / "runtime-private.gcno")],
    ["gcov", "--json-format", "-b", "-c", str(Attempt / "cli-private.gcno")],
])
SanitizerFlags = ["-fsanitize=address,leak,undefined", "-fno-omit-frame-pointer"]
SanitizerBaseFlags = [*Flags, *SanitizerFlags]
SanitizerCommands = [
    ["cc", *SanitizerBaseFlags, "-c", str(Private), "-o",
     str(Attempt / "runtime-private.san.o")],
    ["cc", *SanitizerBaseFlags, "-c", str(CliPrivate), "-o",
     str(Attempt / "cli-private.san.o")],
    ["cc", *SanitizerBaseFlags, "-c", str(Fixture), "-o",
     str(Attempt / "fixture.san.o")],
]
for index, source in enumerate(Sources):
    SanitizerCommands.append(["cc", *SanitizerBaseFlags, "-c", str(source),
        "-o", str(Attempt / f"dependency-{index}.san.o")])
SanitizerRuntimeObjects = [str(Attempt / "runtime-private.san.o"),
                           str(Attempt / "cli-private.san.o"),
                           str(Attempt / "fixture.san.o"),
                           *(str(Attempt / f"dependency-{index}.san.o")
                             for index in range(len(Sources))),
                           *(str(path) for path in ObjectPaths)]
SanitizerCommands.extend([
    ["cc", *SanitizerRuntimeObjects, *SanitizerFlags, "-pthread",
     "-Wl,--gc-sections", *WrapFlags, "-o", str(Attempt / "runner-sanitized")],
    ["cc", *SanitizerBaseFlags, "-DWaddleSessionNoRuntimeMain", "-c",
     str(Fixture), "-o", str(Attempt / "fixture-no-main.san.o")],
    ["cc", *SanitizerBaseFlags, "-c", str(CliFixture), "-o",
     str(Attempt / "cli-fixture.san.o")],
    ["cc", *SanitizerBaseFlags, "-c", str(Source), "-o",
     str(Attempt / "cli-control.san.o")],
    ["cc", str(Attempt / "cli-private.san.o"),
     str(Attempt / "cli-fixture.san.o"), str(Attempt / "cli-control.san.o"),
     str(Attempt / "fixture-no-main.san.o"),
     *(str(Attempt / f"dependency-{index}.san.o")
       for index in range(len(Sources))),
     *(str(path) for path in ObjectPaths), *SanitizerFlags, "-pthread",
     "-Wl,--gc-sections", *WrapFlags, "-o", str(Attempt / "cli-runner-sanitized")],
])
Toolchain = {tool: subprocess.check_output([tool, "--version"], text=True).splitlines()[0]
             for tool in ("cc", "gcov")}
(Attempt / "inputs.json").write_text(json.dumps({"sha256": Hashes,
    "commands": Commands, "sanitizer_commands": SanitizerCommands,
    "sanitizer_flags": SanitizerFlags, "wrappers": WrapSymbols,
    "toolchain": Toolchain}, indent=2) + "\n")
for command in Commands:
    subprocess.run(command, cwd=Attempt if command[0] == "gcov" else Root,
                   check=True, stdout=subprocess.DEVNULL if command[0] == "gcov" else None)
if any(hashlib.sha256(path.read_bytes()).hexdigest() != Hashes[str(path.relative_to(Root))]
       for path in Inputs):
    raise RuntimeError("coverage input changed during attempt")
Summary = {}
Failed = False
for scope, report_name, source in (("runtime", "runtime-private.gcov.json.gz", Source),
                                   ("cli", "cli-private.gcov.json.gz",
                                    Root / "src/interop/session_main.c")):
    Report = json.loads(gzip.decompress((Attempt / report_name).read_bytes()))
    Files = [file for file in Report["files"]
             if Path(file["file"]).resolve() == source.resolve()]
    if len(Files) != 1:
        raise RuntimeError("expected exactly one " + scope + " production source scope")
    Lines = Files[0]["lines"]
    Branches = [(line["line_number"], branch) for line in Lines
                for branch in line.get("branches", [])]
    Summary[scope] = {}
    for kind, entries in (("line", [(line["line_number"], line) for line in Lines]),
                          ("branch", Branches)):
        if not entries:
            raise RuntimeError("empty production " + scope + " " + kind + " scope")
        Covered = sum(entry["count"] > 0 for _, entry in entries)
        Percentage = 100 * Covered / len(entries)
        Missed = [line for line, entry in entries if entry["count"] <= 0]
        Summary[scope][kind] = {"covered": Covered, "total": len(entries),
                                "percent": Percentage, "missed": Missed}
        print(f"session {scope} production {kind}: {Percentage:.2f}% "
              f"({Covered}/{len(entries)}); evidence {Attempt}", flush=True)
        if Percentage < 90:
            print("Uncovered sites: " + str(Missed), flush=True)
            Failed = True
(Attempt / "summary.json").write_text(json.dumps(Summary, indent=2) + "\n")
if Failed:
    raise SystemExit(1)

SanitizerEnvironment = dict(__import__("os").environ)
SanitizerEnvironment["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1:abort_on_error=1"
SanitizerEnvironment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
SanitizerBuildLog = (Attempt / "sanitizer-build.log").open("w")
for command in SanitizerCommands:
    completed = subprocess.run(command, cwd=Root, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True)
    SanitizerBuildLog.write("$ " + " ".join(command) + "\n" + completed.stdout)
    if completed.returncode != 0:
        SanitizerBuildLog.close()
        print((Attempt / "sanitizer-build.log").read_text(), flush=True)
        raise subprocess.CalledProcessError(completed.returncode, command)
SanitizerBuildLog.close()
for name, executable in (("runtime", Attempt / "runner-sanitized"),
                         ("cli", Attempt / "cli-runner-sanitized")):
    log_path = Attempt / f"sanitizer-{name}.log"
    completed = subprocess.run([str(executable)], cwd=Root,
        env=SanitizerEnvironment, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True)
    log_path.write_text(completed.stdout)
    if completed.returncode != 0:
        print(completed.stdout, flush=True)
        raise subprocess.CalledProcessError(completed.returncode, str(executable))
    print(f"session {name} ASan/LSan/UBSan integration: passed; evidence {log_path}",
          flush=True)
if any(hashlib.sha256(path.read_bytes()).hexdigest() != Hashes[str(path.relative_to(Root))]
       for path in Inputs):
    raise RuntimeError("coverage/sanitizer input changed during attempt")
