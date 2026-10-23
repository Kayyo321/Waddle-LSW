#!/usr/bin/env python3
"""Preserve and gate the actual runtime shim under Linux Win32 API doubles.

Every compiled production function and >=90% of lines and branch outcomes are
required, without exclusions. Windows TEB access is a separate native gate.
"""
from datetime import datetime, timezone
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

# Existing repository minimum; no environment/CLI override exists.
MinimumPercent = 90
# Exact fake-mode production inventory; never count test-double functions.
ProductionFunctions = frozenset((
    "runtime_tls_checked_record", "runtime_tls_current_record", "runtime_tls_fail",
    "runtime_tls_assign", "waddle_test___emutls_get_address",
    "waddle_test___emutls_register_common", "gthread_entry",
    "waddle_test___gthr_win32_create", "waddle_test___gthr_win32_join",
    "waddle_test___gthr_win32_self",
))


def coverage_summary(report, source, required_functions=ProductionFunctions):
    """in: borrowed gcov JSON and canonical source path; out: owned summary.

    required_functions: nonempty exact trusted source-function inventory.
    Raise ValueError for absent/duplicate/empty/malformed/below-floor evidence or
    an unexecuted function. No mutation, I/O, exclusions or retained references.
    Pure/thread-safe; all target lines and branch outcomes contribute.
    """
    if not required_functions or any(not isinstance(name, str) or not name for name in required_functions):
        raise ValueError("missing required source function inventory")
    if not isinstance(report, dict) or not isinstance(report.get("files"), list):
        raise ValueError("missing gcov source array")
    files = [f for f in report["files"] if isinstance(f, dict) and f.get("file") == str(source)]
    if len(files) != 1:
        raise ValueError("expected exactly one actual production source")
    functions = files[0].get("functions")
    if not isinstance(functions, list) or len(functions) != len(required_functions):
        raise ValueError("production function inventory missing or duplicated")
    names = set()
    for function in functions:
        if not isinstance(function, dict):
            raise ValueError("invalid function record")
        name, count = function.get("name"), function.get("execution_count")
        if not isinstance(name, str) or name in names or name not in required_functions:
            raise ValueError("wrong or duplicate production function")
        if type(count) is not int or count <= 0:
            raise ValueError("unexecuted or invalid production function: " + name)
        names.add(name)
    if names != set(required_functions):
        raise ValueError("incomplete production function inventory")
    lines = files[0].get("lines")
    if not isinstance(lines, list) or not lines:
        raise ValueError("empty production line denominator")
    line_counts, branch_counts, numbers = [], [], set()
    for line in lines:
        if not isinstance(line, dict):
            raise ValueError("invalid production line")
        number, count, branches = line.get("line_number"), line.get("count"), line.get("branches")
        if type(number) is not int or number <= 0 or number in numbers:
            raise ValueError("invalid or duplicate production line identity")
        if type(count) is not int or count < 0 or not isinstance(branches, list):
            raise ValueError("invalid production line count/branches")
        numbers.add(number)
        line_counts.append(count)
        for branch in branches:
            count = branch.get("count") if isinstance(branch, dict) else None
            if type(count) is not int or count < 0:
                raise ValueError("invalid branch outcome count")
            branch_counts.append(count)
    if not branch_counts:
        raise ValueError("empty production branch denominator")
    result = {"functions_executed": len(names), "minimum_percent": MinimumPercent}
    for kind, counts in (("lines", line_counts), ("branches", branch_counts)):
        hit, total = sum(c > 0 for c in counts), len(counts)
        result[kind] = {"covered": hit, "total": total, "percent": 100 * hit / total}
        if hit * 100 < MinimumPercent * total:
            raise ValueError(f"actual production {kind} below {MinimumPercent}%: {hit}/{total}")
    return result


def main():
    """Run synchronous owned children; preserve raw coverage/logs or fail.

    Optional in: output root, default build/coverage/vgpu/clvk_runtime_tls.
    out: exit 0 only on sufficient unchanged-source evidence, otherwise nonzero.
    Every invocation owns a unique artifact directory. No native Windows/GPU.
    """
    arguments = sys.argv[1:]
    eh_pool = bool(arguments and arguments[0] == "--eh-pool")
    if eh_pool:
        arguments = arguments[1:]
    if len(arguments) > 1:
        raise SystemExit("usage: clvk_runtime_tls_coverage.py [--eh-pool] [output-root]")
    root = Path(__file__).resolve().parents[1]
    stem = "clvk_runtime_eh_pool" if eh_pool else "clvk_runtime_tls"
    base = Path(arguments[0]) if arguments else root / "build/coverage/vgpu" / stem
    output = base.resolve() / ("run-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ") + "-" + str(os.getpid()))
    output.mkdir(parents=True, exist_ok=False)
    source = root / "src/vgpu" / (stem + ".c")
    test = root / "tests/vgpu" / (stem + "_test.c")
    inputs = [source, test, Path(__file__).resolve()]
    required = frozenset(("waddle_clvk_eh_pool_register", "waddle_clvk_eh_pool_release")) if eh_pool else ProductionFunctions
    if not eh_pool:
        inputs.append(root / "tests/vgpu/clvk_runtime_tls_fake/windows.h")
    for tool in ("cc", "gcov"):
        found = shutil.which(tool)
        if not found:
            raise SystemExit("missing coverage tool: " + tool)
        inputs.append(Path(found).resolve(strict=True))
    def hashes():
        return {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    manifest = hashes()
    commands = [
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-O0", "-g",
         "--coverage", "-fprofile-update=atomic", "-fprofile-abs-path", "-pthread",
         *([] if eh_pool else ["-DWaddleRuntimeTlsFakeWindows"]), "-I" + str(root / "tests/vgpu"),
         str(test), "-o", str(output / "runner")],
        [str(output / "runner")],
        ["gcov", "-j", "-b", "-c", "runner-" + stem + "_test.gcno"],
    ]
    for index, command in enumerate(commands):
        with (output / f"command_{index}.log").open("wb") as log:
            subprocess.run(command, cwd=output, check=True, timeout=60, stdout=log, stderr=log)
    artifact = output / ("runner-" + stem + "_test.gcov.json.gz")
    if artifact.stat().st_size > 1024 * 1024:
        raise SystemExit("unexpectedly oversized coverage report")
    summary = coverage_summary(json.loads(gzip.decompress(artifact.read_bytes())), source, required)
    if manifest != hashes():
        raise SystemExit("coverage inputs changed during execution")
    summary.update(inputs=manifest, gcov_sha256=hashlib.sha256(artifact.read_bytes()).hexdigest(),
                   scope="Linux CRT double; Windows CRT ordering/native pool not qualified" if eh_pool else
                         "Linux Win32 double over real shim; Windows accessor/native loader not qualified")
    (output / "coverage.json").write_text(json.dumps(summary, indent=2) + "\n")
    print("runtime TLS coverage:", json.dumps(summary, sort_keys=True), "artifacts:", output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
