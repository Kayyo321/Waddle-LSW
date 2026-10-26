#!/usr/bin/env python3
"""Line and branch coverage gate for src/interop/host_wayland_core.c.

The test translation unit includes the module source, so gcov reports the module's lines and branches directly.
Nothing is excluded: every executable line and every branch gcov attributes to the module counts. The gate is 90
percent for lines and for branches. Objects, gcda and gcov output go to build/coverage/interop/host_wayland_core/,
which is recreated on each run. The protocol objects are the generated xdg-shell and xdg-decoration sources in build/.
"""
import gzip
import json
import shutil
import subprocess
import sys
from pathlib import Path

Threshold = 90.0
Module = "src/interop/host_wayland_core.c"
Wraps = ("-Wl,--wrap=wl_proxy_marshal_flags,--wrap=wl_proxy_get_version,--wrap=wl_proxy_add_listener,"
         "--wrap=wl_proxy_destroy,--wrap=wl_display_connect,--wrap=wl_display_disconnect,--wrap=wl_display_get_fd,"
         "--wrap=wl_display_get_error,--wrap=wl_display_dispatch_pending,--wrap=wl_display_prepare_read,"
         "--wrap=wl_display_read_events,--wrap=wl_display_flush,--wrap=wl_display_roundtrip")


def run(command, cwd=None):
    subprocess.run(command, cwd=cwd, check=True)


def main():
    root = Path.cwd()
    output = root / "build/coverage/interop/host_wayland_core"
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)
    pkg_cflags = subprocess.run(["pkg-config", "--cflags", "wayland-client"], check=True, capture_output=True, text=True).stdout.split()
    pkg_libs = subprocess.run(["pkg-config", "--libs", "wayland-client"], check=True, capture_output=True, text=True).stdout.split()
    flags = ["-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-UNDEBUG", "-O0", "-g", "--coverage",
             "-fprofile-abs-path", "-Iinclude", "-Isrc/interop", "-Ibuild"] + pkg_cflags
    # Compile the test unit on its own so its gcda and gcno land next to the object, then link.
    run(["cc", *flags, "-c", "tests/interop/host_wayland_core_test.c", "-o", str(output / "test.o")], cwd=root)
    run(["cc", *flags, "-c", "build/xdg_shell_protocol.c", "-o", str(output / "xdg_shell.o")], cwd=root)
    run(["cc", *flags, "-c", "build/xdg_decoration_protocol.c", "-o", str(output / "xdg_decoration.o")], cwd=root)
    run(["cc", "--coverage", str(output / "test.o"), str(output / "xdg_shell.o"), str(output / "xdg_decoration.o"),
         *pkg_libs, Wraps, "-o", str(output / "test")], cwd=root)
    run([str(output / "test")], cwd=output)
    run(["gcov", "--json-format", "-b", "-c", str(output / "test.gcno")], cwd=output)
    report = json.loads(gzip.decompress((output / "test.gcno.gcov.json.gz").read_bytes()) if (output / "test.gcno.gcov.json.gz").exists() else b"{}")
    files = report.get("files", [])
    if not files:
        for candidate in output.glob("*.gcov.json.gz"):
            report = json.loads(gzip.decompress(candidate.read_bytes()))
            files = report.get("files", [])
            if files:
                break
    entry = next((item for item in files if item["file"].endswith(Module)), None)
    if entry is None:
        print("coverage report does not contain " + Module, file=sys.stderr)
        return 1
    lines = [line for line in entry["lines"]]
    branches = [branch for line in lines for branch in line.get("branches", [])]
    line_covered = sum(1 for line in lines if line["count"] > 0)
    branch_covered = sum(1 for branch in branches if branch["count"] > 0)
    line_percent = 100.0 * line_covered / len(lines)
    branch_percent = 100.0 * branch_covered / len(branches) if branches else 100.0
    print(f"host_wayland_core production line coverage: {line_percent:.2f}% ({line_covered}/{len(lines)}); no exclusions", flush=True)
    print(f"host_wayland_core production branch coverage: {branch_percent:.2f}% ({branch_covered}/{len(branches)}); no exclusions", flush=True)
    missed = [line["line_number"] for line in lines if line["count"] == 0]
    if missed:
        print("uncovered lines: " + " ".join(str(number) for number in missed), flush=True)
    partial = [line["line_number"] for line in lines if any(branch["count"] == 0 for branch in line.get("branches", []))]
    if partial:
        print("lines with untaken branches: " + " ".join(str(number) for number in partial), flush=True)
    return 0 if line_percent >= Threshold and branch_percent >= Threshold else 1


if __name__ == "__main__":
    sys.exit(main())
