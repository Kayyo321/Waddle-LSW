#!/usr/bin/env python3
"""Measure session_app.c and session_supervisor.c line and branch coverage (>= 90%) and run the sanitizers."""
from datetime import datetime, timezone
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import uuid

Root = Path(__file__).resolve().parents[2]
App = Root / "src/interop/session_app.c"
Supervisor = Root / "src/interop/session_supervisor.c"
Builtin = Root / "src/interop/session_handlers_builtin.c"
AppTest = Root / "tests/interop/session_app.c"
SupervisorTest = Root / "tests/interop/session_supervisor.c"
Fixture = Root / "tests/interop/session_control_host.c"
Private = Root / "tests/interop/session_control_host_private.c"
AttemptRoot = Path("/tmp/session_app_coverage")
Attempt = AttemptRoot / ("run-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S") + "-" + uuid.uuid4().hex[:8])
Attempt.mkdir(parents=True, exist_ok=False)
WrapSymbols = sorted(set(re.findall(r"\b__wrap_([a-z_][a-z_0-9]*)\s*\(", Fixture.read_text())))
WrapFlags = ["-Wl,--wrap=" + name for name in WrapSymbols]
RuntimeSources = [
    Root / "src/interop/session_local_handler.c",
    Root / "src/interop/session_host.c",
    Root / "src/interop/linux_session_owner.c",
    Root / "src/interop/linux_socket_owner.c",
    Root / "src/interop/interop_mapping_linux.c",
    Root / "src/interop/interop_mapping_init.c",
    Root / "src/interop/interop_native_atomic.c",
    Root / "src/daemon/daemon_device.c",
]
ObjectPaths = [
    Root / "build/daemon_wire.o",
    Root / "build/device_storage.o",
    Root / "build/daemon_config.o",
    Root / "build/path_rules.o",
    Root / "build/interop_control_peer_abi.o",
    Root / "build/interop_codec.o",
    Root / "build/interop_layout_abi.o",
    Root / "build/interop_activation_store.o",
]
Flags = ["-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-UNDEBUG", "-O0", "-g",
         "-D_GNU_SOURCE", "-Iinclude", "-Isrc/daemon", "-Isrc", "-Isrc/common", "-Isrc/cli",
         "-Isrc/interop", "-Isubmodules/looking_glass/module", "-ffunction-sections",
         "-fdata-sections", "-pthread"]
Covered = [App, Supervisor, Builtin]
Inputs = [Path(__file__).resolve(), App, Supervisor, Builtin, AppTest, SupervisorTest, Fixture, Private,
          Root / "src/interop/session_app.h", Root / "src/interop/session_supervisor.h",
          Root / "src/interop/session_handler.h", Root / "src/interop/session_control_host.h",
          Root / "impl/seamless-interop/claude_45_percent/session_app_contract.md",
          *RuntimeSources, *ObjectPaths]
Hashes = {str(path.relative_to(Root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in Inputs}
Coverage = ["--coverage", "-fprofile-update=atomic", "-fprofile-abs-path"]
Commands = [
    ["cc", *Flags, *Coverage, "-c", str(Private), "-o", str(Attempt / "runtime-private.o")],
    ["cc", *Flags, *Coverage, "-c", str(App), "-o", str(Attempt / "app.o")],
    ["cc", *Flags, *Coverage, "-c", str(Supervisor), "-o", str(Attempt / "supervisor.o")],
    ["cc", *Flags, *Coverage, "-c", str(Builtin), "-o", str(Attempt / "builtin.o")],
    ["cc", *Flags, "-c", str(AppTest), "-o", str(Attempt / "app-test.o")],
    ["cc", *Flags, "-c", str(SupervisorTest), "-o", str(Attempt / "supervisor-test.o")],
]
for index, source in enumerate(RuntimeSources):
    Commands.append(["cc", *Flags, "-c", str(source), "-o", str(Attempt / f"dependency-{index}.o")])
DependencyObjects = [str(Attempt / f"dependency-{index}.o") for index in range(len(RuntimeSources))]
LinkObjects = [str(Attempt / "runtime-private.o"), str(Attempt / "app.o"), str(Attempt / "supervisor.o"),
               str(Attempt / "builtin.o"), str(Attempt / "app-test.o"), str(Attempt / "supervisor-test.o"),
               *DependencyObjects, *(str(path) for path in ObjectPaths)]
Commands.append(["cc", *LinkObjects, *Coverage, "-pthread", "-Wl,--gc-sections", *WrapFlags,
                 "-o", str(Attempt / "runner")])
Commands.append([str(Attempt / "runner")])
Commands.append(["gcov", "--json-format", "-b", "-c", str(Attempt / "app.gcno")])
Commands.append(["gcov", "--json-format", "-b", "-c", str(Attempt / "supervisor.gcno")])
Commands.append(["gcov", "--json-format", "-b", "-c", str(Attempt / "builtin.gcno")])
SanitizerFlags = ["-fsanitize=address,leak,undefined", "-fno-omit-frame-pointer"]
SanitizerCommands = [
    ["cc", *Flags, *SanitizerFlags, "-c", str(Private), "-o", str(Attempt / "runtime-private.san.o")],
    ["cc", *Flags, *SanitizerFlags, "-c", str(App), "-o", str(Attempt / "app.san.o")],
    ["cc", *Flags, *SanitizerFlags, "-c", str(Supervisor), "-o", str(Attempt / "supervisor.san.o")],
    ["cc", *Flags, *SanitizerFlags, "-c", str(Builtin), "-o", str(Attempt / "builtin.san.o")],
    ["cc", *Flags, *SanitizerFlags, "-c", str(AppTest), "-o", str(Attempt / "app-test.san.o")],
    ["cc", *Flags, *SanitizerFlags, "-c", str(SupervisorTest), "-o", str(Attempt / "supervisor-test.san.o")],
]
for index, source in enumerate(RuntimeSources):
    SanitizerCommands.append(["cc", *Flags, *SanitizerFlags, "-c", str(source),
                              "-o", str(Attempt / f"dependency-{index}.san.o")])
SanitizerObjects = [str(Attempt / "runtime-private.san.o"), str(Attempt / "app.san.o"),
                    str(Attempt / "supervisor.san.o"), str(Attempt / "builtin.san.o"),
                    str(Attempt / "app-test.san.o"), str(Attempt / "supervisor-test.san.o"),
                    *(str(Attempt / f"dependency-{index}.san.o") for index in range(len(RuntimeSources))),
                    *(str(path) for path in ObjectPaths)]
SanitizerCommands.append(["cc", *SanitizerObjects, *SanitizerFlags, "-pthread", "-Wl,--gc-sections",
                          *WrapFlags, "-o", str(Attempt / "runner-sanitized")])
Toolchain = {tool: subprocess.check_output([tool, "--version"], text=True).splitlines()[0] for tool in ("cc", "gcov")}
(Attempt / "inputs.json").write_text(json.dumps({"sha256": Hashes, "commands": Commands,
    "sanitizer_commands": SanitizerCommands, "toolchain": Toolchain}, indent=2) + "\n")
for command in Commands:
    subprocess.run(command, cwd=Attempt if command[0] == "gcov" else Root, check=True,
                   stdout=subprocess.DEVNULL if command[0] == "gcov" else None)
if any(hashlib.sha256(path.read_bytes()).hexdigest() != Hashes[str(path.relative_to(Root))] for path in Inputs):
    raise RuntimeError("coverage input changed during attempt")
Summary = {}
Failed = False
for source, report_name in ((App, "app.gcov.json.gz"), (Supervisor, "supervisor.gcov.json.gz"),
                            (Builtin, "builtin.gcov.json.gz")):
    Report = json.loads(gzip.decompress((Attempt / report_name).read_bytes()))
    Files = [file for file in Report["files"] if Path(file["file"]).resolve() == source.resolve()]
    if len(Files) != 1:
        raise RuntimeError("expected exactly one production scope for " + str(source))
    Lines = Files[0]["lines"]
    Branches = [(line["line_number"], branch) for line in Lines for branch in line.get("branches", [])]
    Summary[source.name] = {}
    for kind, entries in (("line", [(line["line_number"], line) for line in Lines]), ("branch", Branches)):
        if not entries:
            raise RuntimeError("empty production " + source.name + " " + kind + " scope")
        Hit = sum(entry["count"] > 0 for _, entry in entries)
        Percentage = 100 * Hit / len(entries)
        Missed = [line for line, entry in entries if entry["count"] <= 0]
        Summary[source.name][kind] = {"covered": Hit, "total": len(entries), "percent": Percentage, "missed": Missed}
        print(f"{source.name} production {kind}: {Percentage:.2f}% ({Hit}/{len(entries)}); evidence {Attempt}", flush=True)
        if Percentage < 90:
            print("Uncovered sites: " + str(Missed), flush=True)
            Failed = True
(Attempt / "summary.json").write_text(json.dumps(Summary, indent=2) + "\n")
if Failed:
    raise SystemExit(1)
SanitizerBuildLog = (Attempt / "sanitizer-build.log").open("w")
for command in SanitizerCommands:
    completed = subprocess.run(command, cwd=Root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    SanitizerBuildLog.write("$ " + " ".join(command) + "\n" + completed.stdout)
    if completed.returncode != 0:
        SanitizerBuildLog.close()
        print((Attempt / "sanitizer-build.log").read_text(), flush=True)
        raise subprocess.CalledProcessError(completed.returncode, command)
SanitizerBuildLog.close()
environment = dict(os.environ)
environment["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1:abort_on_error=1"
environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
completed = subprocess.run([str(Attempt / "runner-sanitized")], cwd=Root, env=environment,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
(Attempt / "sanitizer-app.log").write_text(completed.stdout)
if completed.returncode != 0:
    print(completed.stdout, flush=True)
    raise subprocess.CalledProcessError(completed.returncode, "runner-sanitized")
print(f"session_app ASan/LSan/UBSan integration: passed; evidence {Attempt / 'sanitizer-app.log'}", flush=True)
