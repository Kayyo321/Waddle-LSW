#!/usr/bin/env python3
"""Read-only final M2 guest-output gate; never launches native jobs or accepts M2.

The native supervisor must still pass its independent host retirement, source,
binary, integrity and process-owner checks. Run this gate against each final
collected guest_status.json as well. Historical runners and evidence stay intact.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

# Supported full workloads; smoke runs cannot satisfy this verifier.
Workloads = ("triangle_present", "wsi_lifetime")
# Fixed native fixture order, once per lifetime.
WsiCaseLabels = (
    "failed-replacement-retained-acquired-image",
    "zero-area-sticky-out-of-date-replacement-and-restore",
    "resize-capabilities-out-of-date-and-replacements",
    "destroyed-HWND-surface-loss-and-retirement-precedence",
)
# The fixture emits this only after its matching retirement Ack and DLL cleanup.
FinalPassLine = ("Actual native GPU outputs and matching retirement Ack PASS; "
                 "independent host result still required")
# Match markers anywhere to reject contradictory failures in interleaved logs.
FailureMarkers = (
    "Native GPU handle baseline=", "Native actual GPU fixture failed:",
    "Native WSI lifetime case failed:", "Native WSI lifetime pixel (",
    "Native WSI lifetime retained GDI/USER owner:",
    "Native Vulkan swapchain failed:", "Native Vulkan present pixel(",
    "Native Vulkan swapchain retained GDI/USER owner:",
    "ICD triangle acceptance failed:", "ICD triangle sample ",
    "ICD production acceptance missing entry point:",
    "Retained native session=", "Receipt handle release failed;",
    "Native fixture retained a released bootstrap/loader/ICD module",
    "Native GPU fixture requires medium integrity",
    "Retirement receipt must be fresh",
    "handle_diag: retirement failed", "handle_diag: GetProcessHandleCount failed",
)
# Any malformed or embedded acceptance marker is rejected, not silently ignored.
EventMarkers = (
    "Authenticated native GPU ", "Native repeated application handle baseline",
    "Native actual ", "exact-output PASS", "Native Vulkan actual swapchain ",
    "exact4096RGB/BGRA PASS", "Native WSI lifetime case=",
    "Actual native GPU outputs and matching retirement Ack",
)
# Exact decimal fields disallow leading signs, whitespace and leading zeroes.
HeaderPattern = re.compile(
    r"Authenticated native GPU session=([0-9a-f]{16}) workload=(\S+) public_api=1\.0")
BaselinePattern = re.compile(r"Native repeated application handle baseline=([1-9][0-9]*)")
LifetimePattern = re.compile(r"Native actual (\S+) lifetime=([1-9][0-9]*) device=(.+) exact-output PASS")
FramePattern = re.compile(r"Native Vulkan actual swapchain mode=(FIFO|IMMEDIATE) frame=([1-9][0-9]*) exact4096RGB/BGRA PASS")
CasePattern = re.compile(r"Native WSI lifetime case=(\S+) PASS")


def verify_guest_status(status, workload, device):
    """Validate borrowed status against the full ordered workload.

    in: non-null decoded guest-status object, supported workload and exact device
    name. No input is mutated or retained. out: newly owned summary dictionary.
    Raises ValueError for missing, malformed, failed or contradictory evidence.
    Pure and thread-safe; no file, process, native, network or global mutations.
    Diagnostics remain passive: snapshot availability and timing are not gates.
    """
    if workload not in Workloads:
        raise ValueError("unsupported full M2 workload")
    if not isinstance(device, str) or not device or any(c in device for c in "\r\n\0"):
        raise ValueError("expected device must be a nonempty single line")
    if not isinstance(status, dict) or status.get("exit") != "0":
        raise ValueError("guest exit must be the collected string '0'")
    if not all(isinstance(status.get(key), str) for key in ("stdout", "stderr")):
        raise ValueError("guest stdout and stderr must both be collected strings")
    for stream in (status["stdout"], status["stderr"]):
        if "\0" in stream:
            raise ValueError("NUL in collected output")
        for marker in FailureMarkers:
            if marker in stream:
                raise ValueError("contradictory fixture failure: " + marker)
    for marker in EventMarkers:
        if marker in status["stderr"]:
            raise ValueError("acceptance event unexpectedly present on stderr: " + marker)
    events = []
    session = None
    baseline = None
    lines = [line for line in re.split(r"\r\n|\n|\r", status["stdout"]) if line]
    for number, line in enumerate(lines, 1):
        header = HeaderPattern.fullmatch(line)
        baseline_match = BaselinePattern.fullmatch(line)
        lifetime = LifetimePattern.fullmatch(line)
        frame = FramePattern.fullmatch(line)
        case = CasePattern.fullmatch(line)
        if header:
            if header[1] == "0000000000000000" or header[2] != workload:
                raise ValueError("wrong authenticated session or workload")
            session = header[1]
            events.append(("header",))
        elif baseline_match:
            baseline = int(baseline_match[1])
            events.append(("baseline",))
        elif lifetime:
            events.append(("lifetime", lifetime[1], int(lifetime[2]), lifetime[3]))
        elif frame:
            events.append(("frame", frame[1], int(frame[2])))
        elif case:
            events.append(("case", case[1]))
        elif line == FinalPassLine:
            events.append(("final",))
        elif any(marker in line for marker in EventMarkers):
            raise ValueError(f"malformed acceptance event at stdout line {number}")
    expected = [("header",), ("baseline",)]
    for lifetime in range(1, 9):
        if workload == "triangle_present":
            expected.extend(("frame", mode, frame) for mode in ("FIFO", "IMMEDIATE")
                            for frame in range(1, 4))
        else:
            expected.extend(("case", label) for label in WsiCaseLabels)
        expected.append(("lifetime", workload, lifetime, device))
    expected.append(("final",))
    if events != expected:
        mismatch = next((i for i, pair in enumerate(zip(events, expected))
                         if pair[0] != pair[1]), min(len(events), len(expected)))
        raise ValueError(f"missing, extra or out-of-order acceptance event at index {mismatch}")
    if not lines or lines[-1] != FinalPassLine:
        raise ValueError("final retirement PASS must be the last nonempty stdout line")
    return {"workload": workload, "device": device, "session": session,
            "baseline": baseline, "ordered_lifetimes": list(range(1, 9)),
            "frames": 48 if workload == "triangle_present" else 0,
            "wsi_cases": 32 if workload == "wsi_lifetime" else 0}


def unique_object(pairs):
    """in: JSON key/value pairs. out: owned dict; duplicate keys raise ValueError.

    Does not retain or mutate pairs; pure and thread-safe. Used for every object
    so a contradictory earlier exit/stdout field cannot be overwritten silently.
    """
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key: " + key)
        result[key] = value
    return result


def main(argv=None):
    """in: optional borrowed CLI arguments, None uses sys.argv. out: exit code.

    Read only the status file and this verifier. Emit a JSON report to stdout;
    return 0 for the guest-output gate, 1 for invalid evidence or I/O, argparse
    exits 2 for invalid CLI. No native access or M2 acceptance is performed.
    Each invocation owns its parsed data; concurrent reads are independent.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--status", required=True, type=Path)
    parser.add_argument("--workload", required=True, choices=Workloads)
    parser.add_argument("--device", required=True)
    args = parser.parse_args(argv)
    report = {"schema": "m2_guest_output_verification_v1", "passed": False,
              "scope": "guest output only; independent host qualification still required",
              "status_path": str(args.status.resolve()),
              "verifier_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    try:
        raw = args.status.read_bytes()
        report["status_sha256"] = hashlib.sha256(raw).hexdigest()
        status = json.loads(raw.decode("utf-8"), object_pairs_hook=unique_object)
        report.update(verify_guest_status(status, args.workload, args.device))
        report["passed"] = True
    except (OSError, ValueError, RecursionError) as error:
        report["error"] = str(error)
    print(json.dumps(report, sort_keys=True))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
