#!/usr/bin/env python3
"""Verify one source-bound1100-reload native lifecycle stress, never heap acceptance.
All paths/manifest are explicit caller inputs. Read only; no native process/API
execution. Fail on incomplete native or actual host-retirement evidence.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

Cycles = 1100
# Approved OS-only pre-initialization witness (OPENCL_RELOAD_CONTRACT.md amendments 1-3).
OsPreinit = "user32,winsock,bcrypt,dxgi_module,search_order,package_family,winsock_connect"
Prefix = "OPENCL_RELOAD_V1 "
Lifetime = "OpenCL exact add/local-barrier/atomics/max-group/buffers/events/build-error lifetime passed."
Buffer = "OpenCL malformed buffer range, pointer, overlap, context and event oracles passed."
Callers = "OpenCL reload three exact context lifetimes and raw pre-load callers joined."
Retirement = "OpenCL reload shutdown, session stop and module retirement passed."
Forbidden = ("OpenCL reload FAILED", "OpenCL reload fatal:", "OpenCL reload probe allocation failed", "OpenCL reload control allocation failed", "OpenCL acceptance failed", "OpenCL line ", "OpenCL buffer oracle ", "OpenCL buffer map oracle ",
             "OpenCL loader environment setup failed", "OpenCL caller thread creation failed",
             "Retained native session=", "Retained clvk", "OpenCL loader environment retained", "OpenCL transport-loss",
             "OpenCL eight exact context lifetimes", "OpenCL concurrent independent context callers",
             "AddressSanitizer", "LeakSanitizer", "UndefinedBehaviorSanitizer", "runtime error:")


def require(condition, message):
    """Reject false evidence even under optimized Python; pure, raises ValueError."""
    if not condition:
        raise ValueError(message)


def unique_object(pairs):
    """Reject duplicate JSON keys rather than letting later evidence overwrite."""
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate JSON field")
        result[key] = value
    return result


def decode(text):
    """Decode strict JSON with unique keys and no non-finite constants."""
    def invalid(value):
        raise ValueError("invalid JSON constant: " + value)
    return json.loads(text, object_pairs_hook=unique_object, parse_constant=invalid)


def integer(value, minimum=0):
    return type(value) is int and minimum <= value <= 0xffffffff


def fields(record, expected):
    require(isinstance(record, dict) and set(record) == set(expected), "missing or unexpected record fields")


def identity(record):
    """Read one exact Linux process identity; start ticks are unsigned64, not PID."""
    require(isinstance(record, dict) and integer(record.get("pid"), 1) and
            type(record.get("starttime")) is int and 0 < record["starttime"] <= 0xffffffffffffffff,
            "invalid exact process identity")
    return record["pid"], record["starttime"]


def identity_map(records, label, allow_empty=False):
    """Reject duplicate identities and PID reuse in a bounded historical graph."""
    require(isinstance(records, list) and (allow_empty or records), "missing " + label)
    result, pids = {}, set()
    for record in records:
        key = identity(record)
        require(key not in result and key[0] not in pids, "duplicate/reused PID in " + label)
        result[key] = record
        pids.add(key[0])
    return result


def verify_host(proof, session):
    """Bind every historical owner to fresh absence and post-close receipts.
    Historical states and open pidfds describe observation time, not liveness.
    Root performs native queries/closure; this read-only gate checks exact proof.
    """
    require(isinstance(proof, dict), "missing host proof")
    data, graph = proof.get("fields"), proof.get("graph")
    require(isinstance(data, dict) and isinstance(graph, dict), "missing host fields/graph")
    require(data.get("session") == session and data.get("worker_retired") == "1" and
            data.get("status") == "0" and data.get("worker_exit_status") == "0", "failed or different host session")
    worker = data.get("worker_pid")
    require(isinstance(worker, str) and re.fullmatch(r"[1-9][0-9]*", worker) and integer(int(worker), 1), "invalid worker identity")
    worker = int(worker)
    required = {"owned_session_empty", "signals", "controller_exit", "observer_error", "journal_error", "members", "controller", "pidfd_owners", "adopted_reaps"}
    require(required <= set(graph), "incomplete host retirement graph")
    require(graph["owned_session_empty"] is True and graph["signals"] == [] and
            type(graph["controller_exit"]) is int and graph["controller_exit"] == 0 and
            graph["observer_error"] is None and graph["journal_error"] is None, "unclean host retirement")
    controller = graph["controller"]
    controller_key = identity(controller)
    controller_pid = controller_key[0]
    require(integer(controller.get("parent"), 1) and controller["parent"] != controller_pid and
            integer(controller.get("group"), 1) and controller["group"] == controller_pid and
            integer(controller.get("session"), 1) and controller["session"] == controller_pid and
            controller_pid != worker, "invalid controller identity")
    members = identity_map(graph["members"], "host membership")
    owners = identity_map(graph["pidfd_owners"], "pidfd owners")
    retired = identity_map(proof.get("retired_members"), "member retirement receipts")
    require(members.keys() == owners.keys() == retired.keys() and controller_key in members,
            "incomplete member/owner/retirement bijection")
    by_pid = {key[0]: member for key, member in members.items()}
    require(controller["parent"] not in by_pid and worker in by_pid and
            all(members[controller_key].get(name) == controller[name] for name in ("parent", "group", "session")), "unbound controller/worker")
    descriptors = set()
    for key, member in members.items():
        require(integer(member.get("parent"), 1) and integer(member.get("group"), 1) and
                member["group"] in by_pid and integer(member.get("session"), 1) and
                member["session"] == controller_pid and isinstance(member.get("executable"), str) and
                Path(member["executable"]).is_absolute(), "unbound member process topology")
        cursor, seen = member, set()
        while cursor["pid"] != controller_pid:
            require(cursor["pid"] not in seen and cursor["parent"] in by_pid, "unbound/cyclic process ancestry")
            seen.add(cursor["pid"])
            cursor = by_pid[cursor["parent"]]
        owner = owners[key]
        require(integer(owner.get("owner_pid"), 1) and owner["owner_pid"] == controller["parent"] and
                integer(owner.get("descriptor")) and owner["descriptor"] not in descriptors and
                owner.get("open") is True, "unbound historical pidfd owner")
        descriptors.add(owner["descriptor"])
        receipt = retired[key]
        fields(receipt, {"pid", "starttime", "verified_absent", "observed_starttime", "group", "group_empty", "session", "session_empty", "query_error"})
        require(receipt["verified_absent"] is True and receipt["observed_starttime"] is None and
                integer(receipt["group"], 1) and receipt["group"] == member["group"] and receipt["group_empty"] is True and
                integer(receipt["session"], 1) and receipt["session"] == member["session"] and receipt["session_empty"] is True and
                receipt["query_error"] is None, "member/group/session retirement unproved")
    require(Path(by_pid[worker]["executable"]).name == "waddle_vgpu_worker" and
            by_pid[worker]["group"] == worker and by_pid[worker]["parent"] == controller_pid, "unbound actual worker identity")
    reaps = identity_map(graph["adopted_reaps"], "adopted reaps", allow_empty=True)
    require(reaps.keys() <= members.keys() and all(type(record.get("wait_status")) is int and
            0 <= record["wait_status"] <= 0xffff for record in reaps.values()), "unbound adopted reap")
    closure = proof.get("owner_close")
    fields(closure, {"owner_pid", "all_pidfds_closed", "error", "pidfd_owners"})
    require(integer(closure["owner_pid"], 1) and closure["owner_pid"] == controller["parent"] and
            closure["all_pidfds_closed"] is True and closure["error"] is None, "pidfd owner closure unproved")
    closed = identity_map(closure["pidfd_owners"], "closed pidfd owners")
    require(closed.keys() == owners.keys(), "incomplete pidfd closure bijection")
    for key, record in closed.items():
        fields(record, {"pid", "starttime", "descriptor", "closed"})
        require(integer(record["descriptor"]) and record["descriptor"] == owners[key]["descriptor"] and
                record["closed"] is True, "wrong or unclosed pidfd owner")


def verify(stdout, stderr, native_exit, manifest, candidate_sha256, fixture_sha256,
           expected_candidate_sha256, expected_fixture_sha256, load_proof):
    """Validate borrowed strings and manifest; return owned lifecycle-only summary.
    Expected hashes are frozen before execution, independent of mutable files
    and manifest. Actual-file hashes and manifest must both match them.
    load_proof(entry) must verify a frozen host proof's SHA256 before decoding it.
    No subprocess, resource mutation, ownership cleanup or heap claim occurs.
    """
    require(type(native_exit) is int and native_exit == 0, "native process failed or did not exit")
    for actual, expected, label in ((candidate_sha256, expected_candidate_sha256, "candidate"),
                                    (fixture_sha256, expected_fixture_sha256, "fixture")):
        require(isinstance(expected, str) and re.fullmatch(r"[0-9a-f]{64}", expected) and actual == expected,
                label + " differs from frozen pre-run hash")

    fields(manifest, {"schema", "cycles", "pid", "creation", "module_path", "candidate_sha256", "fixture_sha256", "hosts"})
    require(manifest["schema"] == "opencl_reload_manifest_v1" and type(manifest["cycles"]) is int and manifest["cycles"] == Cycles, "wrong stress contract")
    require(integer(manifest["pid"], 1) and isinstance(manifest["creation"], str) and
            re.fullmatch(r"[0-9a-f]{16}", manifest["creation"]) and int(manifest["creation"], 16), "invalid native identity")
    require(isinstance(candidate_sha256, str) and re.fullmatch(r"[0-9a-f]{64}", candidate_sha256) and
            manifest["candidate_sha256"] == candidate_sha256, "candidate hash mismatch")
    require(isinstance(manifest["module_path"], str) and re.match(r"^[A-Za-z]:\\", manifest["module_path"]), "invalid exact module path")
    require(isinstance(fixture_sha256, str) and re.fullmatch(r"[0-9a-f]{64}", fixture_sha256) and manifest["fixture_sha256"] == fixture_sha256, "fixture hash mismatch")
    hosts = manifest["hosts"]
    require(isinstance(hosts, list) and len(hosts) == Cycles, "missing host lifetimes")
    require(all(marker not in stdout and marker not in stderr for marker in Forbidden), "contradictory failure or wrong workload")
    require(Prefix not in stderr, "reload protocol on wrong stream")
    baseline = None
    sessions = set()
    current_lines = []
    cycle = 0
    complete = False
    for line in stdout.splitlines():
        if not line.startswith(Prefix):
            require(not line.startswith("OPENCL_RELOAD"), "unknown or malformed reload record")
            current_lines.append(line)
            continue
        record = decode(line[len(Prefix):])
        require(isinstance(record, dict) and not complete, "record after completion or malformed record")
        event = record.get("event")
        if baseline is None:
            fields(record, {"event", "pid", "creation", "cycles", "contexts_per_cycle", "idle_threads", "handles", "tls", "fls", "heap_acceptance", "entry", "os_preinit", "module_utf16"})
            require(event == "baseline" and not current_lines, "missing first baseline")
            require(record["pid"] == manifest["pid"] and type(record["pid"]) is int and record["creation"] == manifest["creation"], "wrong native identity")
            require(type(record["cycles"]) is int and record["cycles"] == Cycles and
                    type(record["contexts_per_cycle"]) is int and record["contexts_per_cycle"] == 3 and
                    type(record["idle_threads"]) is int and record["idle_threads"] == 4 and
                    record["heap_acceptance"] is False, "wrong baseline workload")
            require(record["module_utf16"] == manifest["module_path"].encode("utf-16-be").hex(), "wrong module path")
            require(record["os_preinit"] == OsPreinit, "approved OS pre-initialization witness missing or different")
            require(all(integer(record[name], 1) for name in ("handles", "tls", "fls")), "invalid baseline counts")
            require(isinstance(record["entry"], list) and len(record["entry"]) == 3 and all(integer(value, 1) for value in record["entry"]), "invalid entry baseline")
            baseline = record
        elif event == "cycle":
            fields(record, {"event", "cycle", "pid", "creation", "session", "foreign", "contexts", "module_exact", "modules_absent", "handles", "tls", "fls"})
            cycle += 1
            require(type(record["cycle"]) is int and record["cycle"] == cycle <= Cycles, "out-of-order or extra cycle")
            require(type(record["pid"]) is int and record["pid"] == manifest["pid"] and record["creation"] == manifest["creation"], "mixed native process")
            session = record["session"]
            require(isinstance(session, str) and re.fullmatch(r"[0-9a-f]{16}", session) and int(session, 16) and session not in sessions, "missing/duplicate session")
            sessions.add(session)
            foreign = record["foreign"]
            require(isinstance(foreign, list) and len(foreign) == 2 and all(integer(tid, 1) for tid in foreign) and foreign[0] != foreign[1], "invalid foreign callers")
            require(all(type(record[name]) is int and record[name] == value for name, value in (("contexts", 3), ("module_exact", 1), ("modules_absent", 1))), "incomplete caller/module proof")
            require(all(integer(record[name]) and record[name] == baseline[name] for name in ("handles", "tls", "fls")), "resource baseline mismatch")
            require(current_lines.count(Lifetime) == 3 and current_lines.count(Buffer) == 3 and
                    current_lines.count(Callers) == 1 and current_lines.count(Retirement) == 1, "incomplete exact GPU workload")
            authenticated = [match.group(1) for text in current_lines if (match := re.fullmatch(r"Authenticated real OpenCL session=([0-9a-f]{16})", text))]
            require(authenticated == [session], "native session not bound to workload")
            stages = [text for text in current_lines if text.startswith("DXVK heap capture ")]
            expected_stages = ([f"reload_after_{cycle-1}"] if cycle > 1 else []) + [f"reload_before_{cycle}"]
            require(stages == [f"DXVK heap capture {stage} pid={manifest['pid']} creation={manifest['creation']} handles={baseline['handles']}" for stage in expected_stages], "missing/reordered/mixed cycle handshakes")
            entry = hosts[cycle-1]
            fields(entry, {"cycle", "proof", "sha256"})
            require(type(entry["cycle"]) is int and entry["cycle"] == cycle, "host cycle order mismatch")
            verify_host(load_proof(entry), session)
        elif event == "complete":
            fields(record, {"event", "cycles", "contexts", "cleanup", "heap_acceptance", "entry"})
            require(cycle == Cycles and type(record["cycles"]) is int and record["cycles"] == Cycles and
                    type(record["contexts"]) is int and record["contexts"] == Cycles*3 and
                    record["cleanup"] is True and record["heap_acceptance"] is False, "incomplete terminal retirement")
            require(record["entry"] == baseline["entry"] and all(integer(value, 1) for value in record["entry"]), "final entry baseline mismatch")
            require(current_lines == [f"DXVK heap capture reload_after_{Cycles} pid={manifest['pid']} creation={manifest['creation']} handles={baseline['handles']}", f"DXVK heap capture reload_cleanup pid={manifest['pid']} creation={manifest['creation']} handles={baseline['entry'][0]}"], "missing/reordered cleanup handshakes")
            complete = True
        else:
            raise ValueError("unknown reload event")
        current_lines = []
    require(complete and not current_lines, "missing terminal success or trailing output")
    return {"status": "PASS", "scope": "full_dll_reload_lifecycle_only", "cycles": Cycles,
            "contexts": Cycles*3, "heap_acceptance": False, "candidate_sha256": candidate_sha256, "fixture_sha256": fixture_sha256}


def load_proof(entry):
    require(isinstance(entry["proof"], str) and Path(entry["proof"]).is_absolute(), "host proof must be absolute")
    require(isinstance(entry["sha256"], str) and re.fullmatch(r"[0-9a-f]{64}", entry["sha256"]), "invalid proof hash")
    path = Path(entry["proof"])
    require(path.stat().st_size <= 1048576, "host proof exceeds bound")
    data = path.read_bytes()
    require(hashlib.sha256(data).hexdigest() == entry["sha256"], "host proof hash mismatch")
    return decode(data.decode("utf-8"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("stdout", "stderr", "manifest", "dll", "fixture"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--exit-code", type=int, required=True)
    parser.add_argument("--expected-candidate-sha256", required=True)
    parser.add_argument("--expected-fixture-sha256", required=True)
    args = parser.parse_args()
    require(args.stdout.stat().st_size <= 64*1024*1024 and args.stderr.stat().st_size <= 1100*1024*1024, "native log exceeds declared bound")
    require(args.manifest.stat().st_size <= 4*1024*1024, "manifest exceeds bound")
    with args.dll.open("rb") as stream:
        candidate_sha256 = hashlib.file_digest(stream, "sha256").hexdigest()
    with args.fixture.open("rb") as stream:
        fixture_sha256 = hashlib.file_digest(stream, "sha256").hexdigest()
    # Preserve/inspect every stderr byte without loading a stress-sized log into RAM.
    with args.stderr.open("rb") as stream:
        while data := stream.readline(1048577):
            require(len(data) <= 1048576, "native stderr line exceeds bound")
            line = data.decode("utf-8")
            require(Prefix not in line and all(marker not in line for marker in Forbidden), "contradictory native stderr")
    result = verify(args.stdout.read_text(), "", args.exit_code,
                    decode(args.manifest.read_text()), candidate_sha256, fixture_sha256,
                    args.expected_candidate_sha256, args.expected_fixture_sha256, load_proof)
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, TypeError, KeyError) as error:
        raise SystemExit("OpenCL reload verification FAILED: " + str(error))
