#!/usr/bin/env python3
"""End-to-end `waddle session stop` against session.sock in a temporary runtime directory.

Runs a copy of build/waddle through the open suite's Lab (temporary HOME and XDG_RUNTIME_DIR, a
valid device named alpha). The abnormal cases use the fake server (open_fake_session.py), whose
"stop" scenario key chooses the LocalStopResult, Error, silence, or end of stream. The lock
cases hold session.lock from this process. When WADDLE_TEST_SESSION_HARNESS names the real
session command harness, the harness cases stop a live session: the client's stop must end the
session gracefully, remove its socket, release session.lock, and make the next stop report
already stopped.
"""
import fcntl
import os
import shutil
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import open_e2e as oe  # noqa: E402  (Lab, the fake and harness servers, and the copy of waddle)

HARNESS = oe.HARNESS


def reset(lab):
    """Remove the runtime directory's device state so each case starts with no session."""
    waddle_dir = lab.runtime / "waddle"
    if waddle_dir.exists():
        shutil.rmtree(waddle_dir)


def stop(lab, *args):
    return lab.run("session", "stop", *args, timeout=60)


def fake_stop(lab, scenario):
    """Run stop against the fake server with one scenario; the server is closed afterwards."""
    reset(lab)
    server = oe.Server(lab, scenario)
    try:
        return stop(lab)
    finally:
        server.close()


def case_stop_ok(lab):
    result = fake_stop(lab, {"stop": "ok"})
    oe.check(result.returncode == 0, ("ok stop", result.returncode, result.stderr))
    oe.check("session stopped" in result.stderr, result.stderr)


def case_refused_with_message(lab):
    result = fake_stop(lab, {"stop": "status:3:busy"})
    oe.check(result.returncode == 1 and "did not stop: busy" in result.stderr,
             ("refused", result.returncode, result.stderr))


def case_refused_status_only(lab):
    result = fake_stop(lab, {"stop": "status:5"})
    oe.check(result.returncode == 1 and "did not stop: status 5" in result.stderr,
             ("refused by status", result.returncode, result.stderr))


def case_error_frame(lab):
    result = fake_stop(lab, {"stop": "error:nope"})
    oe.check(result.returncode == 1 and "refused the stop: nope" in result.stderr,
             ("error frame", result.returncode, result.stderr))


def case_wrong_request_id(lab):
    result = fake_stop(lab, {"stop": "wrong_id"})
    oe.check(result.returncode == 1 and "different request" in result.stderr,
             ("wrong id", result.returncode, result.stderr))


def case_end_of_stream(lab):
    result = fake_stop(lab, {"stop": "eof"})
    oe.check(result.returncode == 1 and "closed without a result" in result.stderr,
             ("eof", result.returncode, result.stderr))


def case_partial_reply(lab):
    result = fake_stop(lab, {"stop": "partial"})
    oe.check(result.returncode == 1 and "closed without a result" in result.stderr,
             ("partial", result.returncode, result.stderr))


def case_silence_until_deadline(lab):
    reset(lab)
    server = oe.Server(lab, {"stop": "silent", "hold": 8})
    started = time.monotonic()
    try:
        result = stop(lab)
    finally:
        server.close()
    elapsed = time.monotonic() - started
    oe.check(result.returncode == 1 and "no result within 5 seconds" in result.stderr,
             ("silent", result.returncode, result.stderr))
    oe.check(4.5 <= elapsed <= 9.0, ("silence bounded by 5 s", elapsed))


def case_usage(lab):
    for args in ((), ("start",), ("stop", "--bogus"), ("stop", "-d"), ("stop", "extra")):
        result = lab.run("session", *args)
        oe.check(result.returncode == 2 and "usage: waddle session stop" in result.stderr,
                 (args, result.returncode, result.stderr))


def case_unknown_device_uses_device_errors(lab):
    reset(lab)
    stopped = stop(lab, "-d", "nosuch")
    opened = lab.run("open", "-d", "nosuch", "/etc/hostname")
    oe.check(stopped.returncode == opened.returncode and "not found" in stopped.stderr,
             ("unknown device", stopped.returncode, opened.returncode, stopped.stderr))


def case_no_session_is_already_stopped(lab):
    reset(lab)
    result = stop(lab)
    oe.check(result.returncode == 0 and "already stopped" in result.stderr,
             ("no session", result.returncode, result.stderr))


def case_held_lock_without_socket(lab):
    """A process holds session.lock but no socket exists: the stop cannot be confirmed."""
    reset(lab)
    device_dir = lab.session_socket.parent
    device_dir.mkdir(parents=True)
    os.chmod(lab.runtime / "waddle", 0o700)
    os.chmod(device_dir, 0o700)
    lock_fd = os.open(device_dir / "session.lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(lock_fd, fcntl.LOCK_EX)
        started = time.monotonic()
        result = stop(lab)
        elapsed = time.monotonic() - started
        oe.check(result.returncode == 1 and "lock is held" in result.stderr,
                 ("held", result.returncode, result.stderr))
        oe.check(elapsed < 3.0, ("held lock is decided at once", elapsed))
        oe.check((device_dir / "session.lock").exists(), "the held lock file was not removed")
        fcntl.flock(lock_fd, fcntl.LOCK_UN)
        result = stop(lab)
        oe.check(result.returncode == 0 and "already stopped" in result.stderr,
                 ("released", result.returncode, result.stderr))
    finally:
        os.close(lock_fd)


def case_harness_stop_ends_the_session(lab):
    """The real handler: LocalStop ends the harness gracefully, and the next stop is a no-op."""
    reset(lab)
    server = oe.HarnessServer(lab, {"reply": "ok"})
    result = stop(lab)
    oe.check(result.returncode == 0 and "session stopped" in result.stderr,
             ("harness stop", result.returncode, result.stderr))
    try:
        exit_code = server.process.wait(timeout=10)
    except Exception as error:  # noqa: BLE001 - reported below with the output
        server.process.kill()
        raise AssertionError(("harness did not exit after stop", error))
    oe.check(exit_code is not None, "harness exited")
    deadline = time.monotonic() + 10.0
    while lab.session_socket.exists() and time.monotonic() < deadline:
        time.sleep(0.05)
    oe.check(not lab.session_socket.exists(), "harness removed its socket on the graceful stop")
    again = stop(lab)
    oe.check(again.returncode == 0 and "already stopped" in again.stderr,
             ("second stop", again.returncode, again.stderr))


FAKE_CASES = (case_stop_ok, case_refused_with_message, case_refused_status_only, case_error_frame,
              case_wrong_request_id, case_end_of_stream, case_partial_reply, case_silence_until_deadline,
              case_usage, case_unknown_device_uses_device_errors, case_no_session_is_already_stopped,
              case_held_lock_without_socket)
HARNESS_CASES = (case_harness_stop_ends_the_session,)


def main():
    cases = list(FAKE_CASES) + (list(HARNESS_CASES) if HARNESS is not None else [])
    with tempfile.TemporaryDirectory(prefix="waddle-session-stop-e2e-") as directory:
        lab = oe.Lab(os.path.realpath(directory))
        for case in cases:
            print("session stop e2e:", case.__name__, flush=True)
            case(lab)
            reset(lab)
    print("session stop e2e: passed (%d cases%s)" % (
        len(cases), ", session command harness" if HARNESS is not None else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
