#!/usr/bin/env python3
"""Coverage gate for the guest keyboard executor (src/interop/guest_input.c).

Builds guest_input.c and its test with --coverage, runs the test, and requires at least 90% line
coverage and 90% branch coverage (branches taken at least once) of guest_input.c from gcov.
The gcov block is matched by an anchored file name so that headers and other sources cannot
satisfy the gate.
"""
import os
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
THRESHOLD = 90.0
CFLAGS = ["-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-UNDEBUG", "-O0", "-g", "--coverage",
          f"-I{ROOT / 'include'}", f"-I{ROOT / 'src' / 'interop'}", f"-I{ROOT / 'src' / 'av'}"]
LIBS = ["build/av_codec.o", "build/interop_keyboard_lock.o", "build/interop_keyboard_layouts.o"]


def run(args, cwd):
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stdout + result.stderr)
        raise SystemExit(f"command failed: {' '.join(args)}")
    return result.stdout


def main():
    for lib in LIBS:
        if not (ROOT / lib).exists():
            raise SystemExit(f"missing {lib}; build it with the keyboard lane first")
    with tempfile.TemporaryDirectory(prefix="guest_input_cov_") as scratch:
        scratch_path = pathlib.Path(scratch)
        executor_obj = scratch_path / "guest_input.o"
        test_obj = scratch_path / "guest_input_test.o"
        av_obj = scratch_path / "av_input.o"
        test_bin = scratch_path / "guest_input_test"
        run(["cc", *CFLAGS, "-c", str(ROOT / "src/interop/guest_input.c"), "-o", str(executor_obj)], ROOT)
        run(["cc", *CFLAGS, "-c", str(ROOT / "tests/interop/guest_input_test.c"), "-o", str(test_obj)], ROOT)
        run(["cc", *CFLAGS, "-c", str(ROOT / "src/av/av_input.c"), "-o", str(av_obj)], ROOT)
        run(["cc", *CFLAGS, str(test_obj), str(executor_obj), str(av_obj), *[str(ROOT / lib) for lib in LIBS],
             "-lc", "-o", str(test_bin)], ROOT)
        run([str(test_bin)], scratch_path)
        text = run(["gcov", "-b", "-o", str(scratch_path), str(executor_obj)], scratch_path)
    block = re.search(r"File '[^']*/src/interop/guest_input\.c'\n(.*?)(?:\n\n|\Z)", text, re.S)
    if not block:
        raise SystemExit("gcov output has no block for src/interop/guest_input.c")
    body = block.group(1)
    line_match = re.search(r"Lines executed:([0-9.]+)% of ([0-9]+)", body)
    branch_match = re.search(r"Taken at least once:([0-9.]+)% of ([0-9]+)", body)
    if not line_match or not branch_match:
        raise SystemExit("gcov output did not contain the expected summary")
    line_pct = float(line_match.group(1))
    branch_pct = float(branch_match.group(1))
    print(f"guest_input.c: line {line_pct:.2f}% of {line_match.group(2)}, "
          f"branch (taken) {branch_pct:.2f}% of {branch_match.group(2)}")
    if line_pct < THRESHOLD or branch_pct < THRESHOLD:
        raise SystemExit(f"coverage below {THRESHOLD:.0f}%")
    return 0


if __name__ == "__main__":
    os.chdir(ROOT)
    sys.exit(main())
