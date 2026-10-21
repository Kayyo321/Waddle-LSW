#!/usr/bin/env python3
"""Bounded collector for the four real Win32 M2 ownership controls, without GPU.

The executable is built separately. Preserve real exit and both streams, including
partial timeout output. A timeout kills/reaps only this directly launched child.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

# Exact order emitted by m2_ownership_native.c, independently checked here.
Cases = ("positive", "owned-leak-equal-total", "injected-release-failure", "unrelated-release-masks-total")
InfoPattern = re.compile(r"M2_OWNERSHIP_NATIVE_INFO case=(\S+) count_before=([1-9][0-9]*) count_after=([1-9][0-9]*) live=(0|1) first_id=(0|1) failures=(0|8) rejected=(0|1)")


def verify(status):
    """in: borrowed collected status. out: True or ValueError for incomplete proof.

    Pure/thread-safe, no I/O or mutation. Require all four exact real control
    decisions; negatives must expose one live identity despite equal totals.
    Informational stderr alone is not a test failure; acceptance on stderr is.
    """
    if not isinstance(status, dict) or status.get("exit") != "0" or status.get("timed_out") is not False:
        raise ValueError("native controls failed, timed out or lack a real zero exit")
    if not all(isinstance(status.get(key), str) for key in ("stdout", "stderr")):
        raise ValueError("native output streams missing")
    for stream in (status["stdout"], status["stderr"]):
        if "FAIL" in stream or "\0" in stream:
            raise ValueError("contradictory native control failure")
    if "M2_OWNERSHIP_NATIVE" in status["stderr"]:
        raise ValueError("native acceptance unexpectedly on stderr")
    lines = status["stdout"].splitlines()
    if len(lines) != 9 or lines[-1] != "M2_OWNERSHIP_NATIVE 4 cases PASS":
        raise ValueError("missing or extra native control output")
    for mode, name in enumerate(Cases):
        match = InfoPattern.fullmatch(lines[2 * mode])
        if not match or match[1] != name or lines[2 * mode + 1] != f"M2_OWNERSHIP_NATIVE case={name} PASS":
            raise ValueError("missing or out-of-order native control")
        before, after, live, identity, failures, rejected = map(int, match.groups()[1:])
        if before >= 1 << 32 or after >= 1 << 32 or (live, identity, failures, rejected) != ((0, 0, 0, 0) if mode == 0 else (1, 1, 8 if mode == 2 else 0, 1)):
            raise ValueError("incorrect native exact-owner decision")
        if mode and before != after:
            raise ValueError("negative control did not preserve equal process totals")
    return True


def collect(command, output, timeout=60):
    """in: borrowed exact command/fresh output path/seconds. out: real gate code.

    Own one bounded child and immutable output directory; no shell or descendants
    are launched by the native fixture. Preserve both streams/real return code;
    timeout/error fails. Intended for real Windows CI; tests use explicit stubs.
    """
    output.mkdir(parents=True, exist_ok=False)
    status = {"exit": None, "stdout": "", "stderr": "", "timed_out": False}
    try:
        run = subprocess.run(command, capture_output=True, timeout=timeout, check=False)
        status.update(exit=str(run.returncode), stdout=run.stdout.decode("utf-8", errors="replace"), stderr=run.stderr.decode("utf-8", errors="replace"))
    except subprocess.TimeoutExpired as error:
        status.update(exit="timeout", timed_out=True, stdout=(error.stdout or b"").decode("utf-8", errors="replace"), stderr=(error.stderr or b"").decode("utf-8", errors="replace"))
    except OSError as error:
        status.update(exit="launch_error", stderr=str(error))
    (output / "guest_status.json").write_text(json.dumps(status, indent=2) + "\n", encoding="utf-8")
    (output / "stdout.log").write_text(status["stdout"], encoding="utf-8")
    (output / "stderr.log").write_text(status["stderr"], encoding="utf-8")
    report = {"passed": False, "command": list(command), "timeout_seconds": timeout,
              "collector_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "status_sha256": hashlib.sha256((output / "guest_status.json").read_bytes()).hexdigest()}
    try:
        verify(status)
        report["passed"] = True
    except ValueError as error:
        report["error"] = str(error)
    (output / "verification.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, sort_keys=True))
    return 0 if report["passed"] else 1


def main():
    """Read CLI, bind exact executable hash, run bounded controls and preserve proof.

    Return zero only on complete controls. An existing output directory fails
    before execution; no prior artifact is modified. Sole invocation owns child.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    digest = hashlib.sha256(executable.read_bytes()).hexdigest()
    result = collect([str(executable)], args.output_dir)
    (args.output_dir / "binary.json").write_text(json.dumps({"path": str(executable), "sha256": digest}, indent=2) + "\n", encoding="utf-8")
    return result


if __name__ == "__main__":
    sys.exit(main())
