#!/usr/bin/env python3
"""Line and branch coverage gate for src/interop/session_windows_handler.c (session windows handler, TODO #11g, #12, #13).

The handler is compiled as its own translation unit and linked with the test unit, the Wayland core and the window
realization, as the lane links them. Instrumentation is gcov (--coverage) without sanitizers, as the windows gate is:
UBSan adds check edges that are not branches of the module. The sanitizer run of the same test is a separate target.
Nothing is excluded: every executable line and every branch gcov attributes to the module counts. The gate is 90 percent
for lines and for branches.

The window-state reducer object build/window_state_abi/window_state_abi.o and the codec, protocol and window-table
objects must exist (the lane builds them). Objects, gcda and gcov output go to
build/coverage/interop/session_windows_handler/, which is recreated on each run. The Wayland core's figures are printed
too, for information only.
"""
import gzip
import json
import shutil
import subprocess
import sys
from pathlib import Path

Threshold = 90.0
Module = "src/interop/session_windows_handler.c"
Core = "src/interop/host_wayland_core.c"

Objects = [
    "build/host_window_map.o",
    "build/host_popup_positioner.o",
    "build/interop_codec.o",
    "build/window_state_abi/window_state_abi.o",
]
Wraps = ([
    "wl_proxy_marshal_flags", "wl_proxy_get_version", "wl_proxy_add_listener", "wl_proxy_destroy",
    "wl_display_connect", "wl_display_disconnect", "wl_display_get_fd", "wl_display_get_error",
    "wl_display_dispatch_pending", "wl_display_prepare_read", "wl_display_read_events", "wl_display_flush",
    "wl_display_roundtrip", "interop_host_feature_send",
])
# The gate measures source lines and branches, so the instrumented build carries no sanitizer: UBSan adds check edges that
# are never taken and are not branches of the module. The sanitizer run of the same test is a separate lane target.
Sanitizers = []


def run(command, cwd=None, env=None):
    subprocess.run(command, cwd=cwd, check=True, env=env)


def load_report(output, gcno):
    """Return the gcov JSON document for one gcno file, decompressed."""
    run(["gcov", "--json-format", "-b", "-c", str(gcno)], cwd=output)
    candidates = sorted(output.glob(gcno.name + ".gcov.json.gz")) or sorted(output.glob("*.gcov.json.gz"))
    if not candidates:
        raise RuntimeError("gcov produced no JSON for " + gcno.name)
    return json.loads(gzip.decompress(candidates[0].read_bytes()))


def summarize(entry):
    lines = entry["lines"]
    branches = [branch for line in lines for branch in line.get("branches", [])]
    line_covered = sum(1 for line in lines if line["count"] > 0)
    branch_covered = sum(1 for branch in branches if branch["count"] > 0)
    line_percent = 100.0 * line_covered / len(lines) if lines else 100.0
    branch_percent = 100.0 * branch_covered / len(branches) if branches else 100.0
    return lines, branches, line_covered, branch_covered, line_percent, branch_percent


def main():
    root = Path.cwd()
    missing = [item for item in Objects if not (root / item).exists()]
    if missing:
        print("coverage needs the lane objects first: " + " ".join(missing), file=sys.stderr)
        return 1
    output = root / "build/coverage/interop/session_windows_handler"
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)
    pkg_cflags = subprocess.run(["pkg-config", "--cflags", "wayland-client"], check=True, capture_output=True, text=True).stdout.split()
    pkg_libs = subprocess.run(["pkg-config", "--libs", "wayland-client"], check=True, capture_output=True, text=True).stdout.split()
    flags = ["-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-UNDEBUG", "-O0", "-g", "--coverage",
             "-fprofile-abs-path", *Sanitizers, "-fno-omit-frame-pointer", "-Iinclude", "-Isrc/interop", "-Isrc/daemon", "-Ibuild"] + pkg_cflags
    # Each translation unit is compiled on its own so its gcno lands next to its object.
    units = {
        "test": "tests/interop/session_windows_handler_test.c",
        "handler": Module,
        "core": Core,
        "windows": "src/interop/host_wayland_windows.c",
        "xdg_shell": "build/xdg_shell_protocol.c",
        "xdg_decoration": "build/xdg_decoration_protocol.c",
    }
    for name, source in units.items():
        run(["cc", *flags, "-c", source, "-o", str(output / (name + ".o"))], cwd=root)
    wraps = ["-Wl,--wrap=" + symbol for symbol in Wraps]
    run(["cc", "--coverage", *Sanitizers, str(output / "test.o"), str(output / "handler.o"), str(output / "core.o"),
         str(output / "windows.o"), str(output / "xdg_shell.o"), str(output / "xdg_decoration.o"), *Objects, *pkg_libs, *wraps,
         "-o", str(output / "test")], cwd=root)
    run([str(output / "test")], cwd=output)

    entry_handler = next((item for item in load_report(output, output / "handler.gcno").get("files", []) if item["file"].endswith(Module)), None)
    if entry_handler is None:
        print("coverage report does not contain " + Module, file=sys.stderr)
        return 1
    lines, branches, line_covered, branch_covered, line_percent, branch_percent = summarize(entry_handler)
    print(f"session_windows_handler production line coverage: {line_percent:.2f}% ({line_covered}/{len(lines)}); no exclusions", flush=True)
    print(f"session_windows_handler production branch coverage: {branch_percent:.2f}% ({branch_covered}/{len(branches)}); no exclusions", flush=True)
    missed = [line["line_number"] for line in lines if line["count"] == 0]
    if missed:
        print("uncovered lines: " + " ".join(str(number) for number in missed), flush=True)
    partial = [line["line_number"] for line in lines if any(branch["count"] == 0 for branch in line.get("branches", []))]
    if partial:
        print("lines with untaken branches: " + " ".join(str(number) for number in partial), flush=True)

    entry_core = next((item for item in load_report(output, output / "core.gcno").get("files", []) if item["file"].endswith(Core)), None)
    if entry_core is not None:
        _, _, core_lines, core_branches, core_line_percent, core_branch_percent = summarize(entry_core)
        print(f"information only, host_wayland_core.c under this build: lines {core_line_percent:.2f}% ({core_lines}), branches {core_branch_percent:.2f}%", flush=True)
    return 0 if line_percent >= Threshold and branch_percent >= Threshold else 1


if __name__ == "__main__":
    sys.exit(main())
