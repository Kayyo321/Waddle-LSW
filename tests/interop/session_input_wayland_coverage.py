#!/usr/bin/env python3
"""Coverage gate for the Wayland keyboard glue (session_input_wayland.c).

Builds the glue and its test with --coverage, runs the test, and requires at least 90% line
coverage and 90% branch coverage (branches taken at least once) from gcov. The keyboard handler
and reducer objects are linked from build/ as the lane builds them; the protocol object is the
generated keyboard-shortcuts-inhibit code. The handler and protocol code are compiled here without
sanitizers so that the coverage link needs no sanitizer runtime; the lane objects in build/ are sanitizer builds.
"""
import os
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
THRESHOLD = 90.0
WAYLAND_CFLAGS = subprocess.run(["pkg-config", "--cflags", "wayland-client"], capture_output=True, text=True,
    check=True).stdout.split()
WAYLAND_LIBS = subprocess.run(["pkg-config", "--libs", "wayland-client"], capture_output=True, text=True,
    check=True).stdout.split()
CFLAGS = ["-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-UNDEBUG", "-O0", "-g", "--coverage",
          f"-I{ROOT / 'include'}", f"-I{ROOT / 'src' / 'interop'}", f"-I{ROOT / 'src' / 'daemon'}",
          f"-I{ROOT / 'submodules' / 'looking_glass' / 'module'}", f"-I{ROOT / 'build'}", *WAYLAND_CFLAGS]
LIBS = ["build/interop_codec.o", "build/interop_keyboard_lock.o", "build/interop_keyboard_layouts.o",
        "build/interop_keyboard_inhibit.o", "build/interop_keyboard_repeat_abi.o", "build/interop_keymap_names.o"]
WRAPS = "-Wl,--wrap=interop_host_feature_send,--wrap=clock_gettime,--wrap=wl_proxy_add_listener," \
        "--wrap=wl_proxy_marshal_flags,--wrap=wl_proxy_get_version"


def run(args, cwd):
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stdout + result.stderr)
        raise SystemExit(f"command failed: {' '.join(args)}")
    return result.stdout


def main():
    for lib in LIBS + ["build/keyboard_shortcuts_inhibit_client.h"]:
        if not (ROOT / lib).exists():
            raise SystemExit(f"missing {lib}; build it with the keyboard lane first")
    with tempfile.TemporaryDirectory(prefix="session_input_wayland_cov_") as scratch:
        scratch_path = pathlib.Path(scratch)
        glue_obj = scratch_path / "session_input_wayland.o"
        test_obj = scratch_path / "session_input_wayland_test.o"
        handler_obj = scratch_path / "session_input_handler.o"
        protocol_obj = scratch_path / "keyboard_shortcuts_inhibit_protocol.o"
        test_bin = scratch_path / "session_input_wayland_test"
        run(["cc", *CFLAGS, "-c", str(ROOT / "src/interop/session_input_wayland.c"), "-o", str(glue_obj)], ROOT)
        run(["cc", *CFLAGS, "-c", str(ROOT / "tests/interop/session_input_wayland_test.c"), "-o", str(test_obj)], ROOT)
        run(["cc", *CFLAGS, "-c", str(ROOT / "src/interop/session_input_handler.c"), "-o", str(handler_obj)], ROOT)
        run(["cc", *CFLAGS, "-c", str(ROOT / "build/keyboard_shortcuts_inhibit_protocol.c"), "-o", str(protocol_obj)], ROOT)
        run(["cc", *CFLAGS, str(test_obj), str(glue_obj), str(handler_obj), str(protocol_obj),
             *[str(ROOT / lib) for lib in LIBS], WRAPS,
             *WAYLAND_LIBS, "-lc", "-o", str(test_bin)], ROOT)
        run([str(test_bin)], scratch_path)
        out = run(["gcov", "-b", "-o", str(scratch_path), str(glue_obj)], scratch_path)
    block = re.search(r"File '[^']*session_input_wayland\.c'\n(.*?)(?:\n\n|\Z)", out, re.S)
    if not block:
        raise SystemExit("gcov output has no block for session_input_wayland.c")
    text = block.group(1)
    line_match = re.search(r"Lines executed:([0-9.]+)%", text)
    branch_match = re.search(r"Taken at least once:([0-9.]+)%", text)
    if not line_match or not branch_match:
        raise SystemExit("gcov output did not contain the expected summary")
    line_pct = float(line_match.group(1))
    branch_pct = float(branch_match.group(1))
    print(f"session_input_wayland: line {line_pct:.2f}%, branch (taken) {branch_pct:.2f}%")
    if line_pct < THRESHOLD or branch_pct < THRESHOLD:
        raise SystemExit(f"coverage below {THRESHOLD:.0f}%")
    return 0


if __name__ == "__main__":
    os.chdir(ROOT)
    sys.exit(main())
