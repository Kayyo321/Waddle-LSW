#!/usr/bin/env python3
"""Retain reproducible core coverage and sanitizer evidence for app_catalog."""
from datetime import datetime, timezone
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import uuid

Root = Path(__file__).resolve().parents[2]
AttemptRoot = Path(os.environ.get("APP_CATALOG_COVERAGE_DIR", "/tmp/app_catalog_coverage"))
Attempt = AttemptRoot / (
    "run-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S") + "-" +
    uuid.uuid4().hex[:8]
)
Attempt.mkdir(parents=True, exist_ok=False)
Inputs = [
    Path(__file__).resolve(),
    Root / "src/interop/app_catalog.c",
    Root / "src/interop/app_catalog.h",
    Root / "src/interop/app_catalog_validation.h",
    Root / "src/interop/app_catalog_validation.zig",
    Root / "src/interop/interop_codec.zig",
    Root / "src/interop/app_catalog_windows.c",
    Root / "src/interop/app_catalog_windows_private.h",
    Root / "include/waddle/interop_protocol.h",
    Root / "tests/interop/app_catalog.c",
    Root / "tests/interop/app_catalog_windows.c",
    Root / "impl/seamless-interop/app_catalog_contract.md",
]
Hashes = {str(path.relative_to(Root)): hashlib.sha256(path.read_bytes()).hexdigest()
          for path in Inputs}
LocalCache = Attempt / "zig-local"
GlobalCache = Attempt / "zig-global"
Environment = dict(os.environ)
Environment["ZIG_LOCAL_CACHE_DIR"] = str(LocalCache)
Environment["ZIG_GLOBAL_CACHE_DIR"] = str(GlobalCache)
Commands = []


def run_logged(command, *, cwd=Root, env=None, name):
    """Run one exact command and retain combined output plus its argv."""
    completed = subprocess.run(command, cwd=cwd, env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True)
    (Attempt / (name + ".log")).write_text("$ " + " ".join(map(str, command)) +
                                           "\n" + completed.stdout)
    Commands.append(list(map(str, command)))
    if completed.returncode != 0:
        print(completed.stdout, flush=True)
        raise subprocess.CalledProcessError(completed.returncode, command)
    return completed.stdout


Versions = {}
for name, command in (("zig", ["zig", "version"]), ("gcc", ["gcc", "--version"]),
                      ("gcov", ["gcov", "--version"])):
    Versions[name] = run_logged(command, name=name + "-version").splitlines()[0]
run_logged(["gcc", "-v"], name="gcc-sysroot-details")
CrossCompiler = shutil.which("x86_64-w64-mingw32-gcc")
if CrossCompiler:
    Versions["mingw_gcc"] = run_logged([CrossCompiler, "--version"],
                                        name="mingw-gcc-version").splitlines()[0]
    run_logged([CrossCompiler, "-v"], name="mingw-sdk-details")

ValidationArchive = Attempt / "libapp_catalog_validation.a"
CodecArchive = Attempt / "libinterop_codec.a"
run_logged(["zig", "build-lib", str(Root / "src/interop/app_catalog_validation.zig"),
            "-static", "-O", "ReleaseSafe", "-fcompiler-rt", "-femit-bin=" + str(ValidationArchive)],
           env=Environment, name="build-validation")
run_logged(["zig", "build-lib", str(Root / "src/interop/interop_codec.zig"),
            "-I", str(Root / "include"), "-lc", "-static", "-O", "ReleaseSafe", "-fcompiler-rt",
            "-femit-bin=" + str(CodecArchive)], env=Environment, name="build-codec")
run_logged(["zig", "test", str(Root / "src/interop/app_catalog_validation.zig")],
           env=Environment, name="zig-validation-tests")

BaseFlags = ["-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-UNDEBUG",
             "-O0", "-g", "-I", str(Root / "include"), "-I", str(Root / "src/interop")]
ProductionObject = Attempt / "app_catalog.o"
FixtureObject = Attempt / "app_catalog_test.o"
CoverageFlags = ["--coverage", "-fprofile-update=atomic", "-fprofile-abs-path"]
run_logged(["gcc", *BaseFlags, *CoverageFlags, "-c",
            str(Root / "src/interop/app_catalog.c"), "-o", str(ProductionObject)],
           name="compile-core-coverage")
run_logged(["gcc", *BaseFlags, *CoverageFlags, "-c",
            str(Root / "tests/interop/app_catalog.c"), "-o", str(FixtureObject)],
           name="compile-fixture-coverage")
CoverageBinary = Attempt / "app_catalog_coverage"
run_logged(["gcc", "--coverage", "-no-pie", str(ProductionObject), str(FixtureObject),
            str(ValidationArchive), str(CodecArchive), "-o", str(CoverageBinary)],
           name="link-core-coverage")
run_logged([str(CoverageBinary)], name="run-core-coverage")

CoverageJson = Attempt / "app_catalog.gcov.json.gz"
run_logged(["gcov", "--json-format", "-b", "-c", str(Attempt / "app_catalog.gcno")],
           cwd=Attempt, name="gcov-core")
Report = json.loads(gzip.decompress(CoverageJson.read_bytes()))
Matches = [item for item in Report["files"]
           if Path(item["file"]).resolve() == Inputs[1].resolve()]
if len(Matches) != 1:
    raise RuntimeError("expected exactly one production app_catalog.c coverage scope")
Production = Matches[0]
Lines = Production["lines"]
Arcs = [branch for line in Lines for branch in line.get("branches", [])]
Summary = {}
for kind, entries in (("lines", Lines), ("taken_arcs", Arcs)):
    if not entries:
        raise RuntimeError("empty app_catalog " + kind + " coverage scope")
    covered = sum(entry["count"] > 0 for entry in entries)
    percentage = 100.0 * covered / len(entries)
    Summary[kind] = {"covered": covered, "total": len(entries), "percent": percentage}
    print(f"app_catalog.c production {kind}: {percentage:.2f}% ({covered}/{len(entries)}); "
          f"evidence {Attempt}", flush=True)
    if percentage < 90.0:
        raise SystemExit("app_catalog.c production coverage is below 90%")
(Attempt / "summary.json").write_text(json.dumps(Summary, indent=2) + "\n")

SanitizerFlags = ["-fsanitize=address,leak,undefined", "-fno-omit-frame-pointer", "-g", "-no-pie"]
SanitizedBinary = Attempt / "app_catalog_sanitized"
run_logged(["gcc", *BaseFlags, *SanitizerFlags,
            str(Root / "src/interop/app_catalog.c"), str(Root / "tests/interop/app_catalog.c"),
            str(ValidationArchive), str(CodecArchive), "-o", str(SanitizedBinary)],
           name="build-sanitized")
SanitizerEnvironment = dict(Environment)
SanitizerEnvironment["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1:abort_on_error=1"
SanitizerEnvironment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
run_logged([str(SanitizedBinary)], env=SanitizerEnvironment, name="run-sanitized")

NativeCompile = None
if CrossCompiler:
    NativeCompile = []
    for source, output, name in (
        (Root / "src/interop/app_catalog.c", Attempt / "app_catalog_windows_core.o", "core"),
        (Root / "src/interop/app_catalog_windows.c", Attempt / "app_catalog_windows.o", "adapter"),
        (Root / "tests/interop/app_catalog_windows.c", Attempt / "app_catalog_windows_test.o", "test"),
    ):
        command = [CrossCompiler, "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                   "-I", str(Root / "include"), "-I", str(Root / "src/interop"),
                   "-c", str(source), "-o", str(output)]
        run_logged(command, name="mingw-compile-" + name)
        NativeCompile.append(command)

if any(hashlib.sha256(path.read_bytes()).hexdigest() != Hashes[str(path.relative_to(Root))]
       for path in Inputs):
    raise RuntimeError("an app catalog input changed during qualification")
(Attempt / "qualification.json").write_text(json.dumps({
    "source_sha256": Hashes,
    "tool_versions": Versions,
    "commands": Commands,
    "coverage": Summary,
    "zig_validator_coverage": "Retained separately by tests/av/coverage.py app_catalog_validation;"
                                      " Zig tests here are not claimed as a second coverage measurement.",
    "native_windows_compile": "warning-clean MinGW cross-compilation only; runtime requires Windows CI",
}, indent=2) + "\n")
print(f"app_catalog qualification and raw gcov/sanitizer evidence: {Attempt}", flush=True)
