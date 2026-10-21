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
import ntpath
import sys

# Supported full workloads; smoke runs cannot satisfy this verifier.
Workloads = ("triangle_present", "wsi_lifetime")
# Protocol is explicit: historical count evidence can never imply ownership proof.
Protocols = ("strict_count_v1", "m2_owned_v1")
# ABI field order is fixed by the native declarations, not host Python layout.
IcdFields = ("version", "bytes", "flags", "command_state", "lost", "binding",
             "object_next_id", "wsi_next_id", "first_live_id", "objects", "resources",
             "surfaces", "swapchains", "images", "acquired_images", "backings",
             "mappings", "caches", "tickets", "references", "reservations")
OwnerFields = ("version", "bytes", "next_id", "acquired", "retired", "first_live_id",
               "first_live_parent", "first_live_value", "live", "pending", "failures", "reserved")
OwnerGroups = ("sink_dc", "fixture_dc", "fixture_window")
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
    "M2_OWNED_FAILURE", "Native GPU handle baseline=", "Native actual GPU fixture failed:",
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
    "M2_OWNED", "Authenticated native GPU ", "Native repeated application handle baseline",
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


def qualified_path(path):
    """in: exact Windows ASCII path. out: normalized comparison string.

    Pure/thread-safe; reject non-absolute/noncanonical or unsupported paths rather
    than resolving files on the verifier host. No file or native access.
    """
    if not isinstance(path, str) or not path or len(path) >= 260 or any(ord(c) < 32 or ord(c) > 126 for c in path):
        raise ValueError("qualified ICD path must be bounded printable ASCII")
    drive, tail = ntpath.splitdrive(path)
    if not drive or not tail.startswith("\\") or ntpath.normpath(path).lower() != path.lower():
        raise ValueError("qualified ICD path must be canonical and absolute")
    return path.lower()


def abi_values(value, fields, u32_indices):
    """in: borrowed fixed array/schema. out: newly owned field mapping.

    Reject booleans, floats, negative/oversized words or wrong length. Pure,
    thread-safe; signed lost must be zero anyway, so negative values fail here.
    """
    if not isinstance(value, list) or len(value) != len(fields):
        raise ValueError("missing or extra ownership ABI field")
    for index, word in enumerate(value):
        if type(word) is not int or word < 0 or word >= 1 << (32 if index in u32_indices else 64):
            raise ValueError("invalid ownership ABI integer")
    return dict(zip(fields, value))


def verify_ownership(record, session, workload, path, previous):
    """in: borrowed record/run identity/prior record. out: validated copied state.

    Require exact healthy ABIs, zero owners and new lifetime activity. Initial
    record establishes clean bound marks; no resets, live-owner forgiveness or
    total-count oracle. Pure/thread-safe; raises ValueError on incomplete proof.
    """
    keys = {"session", "workload", "lifetime", "module_hex", "handles", "icd", *OwnerGroups}
    if not isinstance(record, dict) or set(record) != keys:
        raise ValueError("missing or extra ownership record field")
    if record["session"] != session or record["workload"] != workload:
        raise ValueError("ownership record session/workload mismatch")
    lifetime = record["lifetime"]
    if type(lifetime) is not int or lifetime != (previous["lifetime"] + 1 if previous else 0) or lifetime > 8:
        raise ValueError("ownership lifetime missing, duplicated or out of order")
    module_hex = record["module_hex"]
    if not isinstance(module_hex, str) or not re.fullmatch(r"(?:[0-9a-f]{2})+", module_hex):
        raise ValueError("invalid loaded module path encoding")
    try:
        actual_path = bytes.fromhex(module_hex).decode("ascii")
    except (ValueError, UnicodeError) as error:
        raise ValueError("invalid loaded module path") from error
    if qualified_path(actual_path) != path:
        raise ValueError("wrong loaded qualified ICD path")
    count = record["handles"]
    if type(count) is not int or not 0 < count < 1 << 32:
        raise ValueError("missing process count diagnostic")
    icd = abi_values(record["icd"], IcdFields, range(6))
    if [icd[name] for name in IcdFields[:6]] != [1, 144, 0, 0, 0, 1] or any(icd[name] for name in IcdFields[8:]):
        raise ValueError("unhealthy, incompatible or retained ICD ownership")
    object_id, wsi_id = icd["object_next_id"], icd["wsi_next_id"]
    if not object_id >> 32 or not object_id & 0xffffffff or not wsi_id & 0xffffffff or object_id >> 32 != wsi_id >> 32:
        raise ValueError("invalid ICD identity namespace")
    if previous:
        old = previous["icd"]
        if object_id >> 32 != old["object_next_id"] >> 32 or object_id <= old["object_next_id"] or wsi_id <= old["wsi_next_id"]:
            raise ValueError("stale or reset ICD identity evidence")
    state = {"lifetime": lifetime, "icd": icd, "handles": count}
    for group in OwnerGroups:
        owner = abi_values(record[group], OwnerFields, (0, 1, 8, 9, 10, 11))
        if owner["version"] != 1 or owner["bytes"] != 72 or any(owner[name] for name in OwnerFields[5:]) or owner["acquired"] != owner["retired"] or owner["next_id"] <= owner["acquired"]:
            raise ValueError("incompatible, failed or retained exact native owner: " + group)
        if previous:
            old = previous[group]
            if owner["acquired"] <= old["acquired"] or owner["next_id"] <= old["next_id"] or owner["next_id"] - old["next_id"] < owner["acquired"] - old["acquired"]:
                raise ValueError("stale or reset native owner activity: " + group)
        state[group] = owner
    return state


def verify_guest_status(status, workload, device, protocol, icd_path=None):
    """Validate borrowed status against the full ordered workload.

    in: non-null decoded guest-status object, supported workload, exact device
    name and explicit protocol. icd_path is the exact absolute candidate path for
    ownership mode, otherwise None. No input is mutated or retained.
    out: newly owned summary dictionary.
    Raises ValueError for missing, malformed, failed or contradictory evidence.
    Pure and thread-safe; no file, process, native, network or global mutations.
    Diagnostics remain passive: snapshot availability and timing are not gates.
    """
    if protocol not in Protocols:
        raise ValueError("explicit supported qualification protocol required")
    path = qualified_path(icd_path) if protocol == "m2_owned_v1" else None
    if protocol == "strict_count_v1" and icd_path is not None:
        raise ValueError("legacy protocol does not accept an ownership module path")
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
    ownership = []
    session = None
    baseline = None
    lines = [line for line in re.split(r"\r\n|\n|\r", status["stdout"]) if line]
    for number, line in enumerate(lines, 1):
        header = HeaderPattern.fullmatch(line)
        baseline_match = BaselinePattern.fullmatch(line)
        lifetime = LifetimePattern.fullmatch(line)
        frame = FramePattern.fullmatch(line)
        case = CasePattern.fullmatch(line)
        if line.startswith("M2_OWNED_V1 "):
            if protocol != "m2_owned_v1":
                raise ValueError("ownership evidence in a different protocol")
            record = json.loads(line[len("M2_OWNED_V1 "):], object_pairs_hook=unique_object)
            state = verify_ownership(record, session, workload, path, ownership[-1] if ownership else None)
            if not ownership and state["handles"] != baseline:
                raise ValueError("initial diagnostic count disagrees with baseline")
            ownership.append(state)
            events.append(("ownership", state["lifetime"]))
        elif header:
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
    if protocol == "m2_owned_v1":
        expected.append(("ownership", 0))
    for lifetime in range(1, 9):
        if workload == "triangle_present":
            expected.extend(("frame", mode, frame) for mode in ("FIFO", "IMMEDIATE")
                            for frame in range(1, 4))
        else:
            expected.extend(("case", label) for label in WsiCaseLabels)
        if protocol == "m2_owned_v1":
            expected.append(("ownership", lifetime))
        expected.append(("lifetime", workload, lifetime, device))
    expected.append(("final",))
    if events != expected:
        mismatch = next((i for i, pair in enumerate(zip(events, expected))
                         if pair[0] != pair[1]), min(len(events), len(expected)))
        raise ValueError(f"missing, extra or out-of-order acceptance event at index {mismatch}")
    if not lines or lines[-1] != FinalPassLine:
        raise ValueError("final retirement PASS must be the last nonempty stdout line")
    return {"protocol": protocol, "icd_path": icd_path,
            "ownership_boundaries": len(ownership),
            "diagnostic_handle_counts": [item["handles"] for item in ownership],
            "workload": workload, "device": device, "session": session,
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
    parser.add_argument("--protocol", required=True, choices=Protocols)
    parser.add_argument("--icd-path")
    args = parser.parse_args(argv)
    report = {"schema": "m2_guest_output_verification_v1", "passed": False,
              "scope": "guest output only; independent host qualification still required",
              "status_path": str(args.status.resolve()),
              "verifier_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    try:
        raw = args.status.read_bytes()
        report["status_sha256"] = hashlib.sha256(raw).hexdigest()
        status = json.loads(raw.decode("utf-8"), object_pairs_hook=unique_object)
        report.update(verify_guest_status(status, args.workload, args.device, args.protocol, args.icd_path))
        report["passed"] = True
    except (OSError, ValueError, RecursionError) as error:
        report["error"] = str(error)
    print(json.dumps(report, sort_keys=True))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
