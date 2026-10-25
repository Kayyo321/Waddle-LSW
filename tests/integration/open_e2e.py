#!/usr/bin/env python3
"""End-to-end `waddle open` against a real session.sock path and the built binary.

Runs a copy of build/waddle, placed in the lab's bin directory, in a temporary HOME and
XDG_RUNTIME_DIR, so its waddle-session sibling is always the lab wrapper (the build tree's
waddle-session is never used). Every wrapper records its own $0 and the cases check it. The fake session server
(open_fake_session.py) listens on $XDG_RUNTIME_DIR/waddle/alpha/session.sock, decodes each
LocalLaunch with its own TLV reader, and answers with frames encoded by the production codec
(build/open_frame_dump). Cold-start cases use a fake waddle-session and a fake notify-send
placed first on PATH. The 120 s deadline case is slow and runs only with
WADDLE_OPEN_E2E_SLOW=1; it uses the real deadline, not a shortened one.

Harness mode: WADDLE_TEST_SESSION_HARNESS=PATH (build/interop_session_local_harness) replaces
the fake server with the real session command handler inside the production control runtime.
The harness answers each forwarded launch from its fake guest, so the built `waddle open` talks
to the real handler. Cases that need a server which misbehaves on purpose (a silent or partial
reply, an EOF) are the client's own rules and run only against the fake; the harness mode skips
them. Frame assertions read the guest's LaunchRequest frames, so request ids are guest ids and
every entry reports connection 1.
"""
import json
import os
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import open_fake_session as fake  # noqa: E402

ROOT_BUILD = Path.cwd()
WADDLE = Path(os.environ.get("WADDLE_TEST_EXECUTABLE", ROOT_BUILD / "build" / "waddle")).resolve()
DUMP = Path(os.environ.get("WADDLE_TEST_FRAME_DUMP", ROOT_BUILD / "build" / "open_frame_dump")).resolve()
SLOW = os.environ.get("WADDLE_OPEN_E2E_SLOW") == "1"
HARNESS_ENV = os.environ.get("WADDLE_TEST_SESSION_HARNESS")
HARNESS = Path(HARNESS_ENV).resolve() if HARNESS_ENV else None
# Cases whose server behaviour cannot be produced by the real handler (see the module docstring).
FAKE_ONLY_CASES = ("grace_handover",)
LAUNCH_REQUEST = 0x0700  # guest-bound LaunchRequest, as recorded by the harness
OUTSIDE_TEXT = ("'{file}' is outside the folders shared with Windows (alpha). "
                "Add a share with: waddle device config set filesystem.<name> <dir>:<drive>:\"")


class Lab:
    """A temporary HOME and runtime directory with a valid device named alpha."""

    def __init__(self, root):
        self.root = Path(root)
        self.home = self.root / "home"
        self.home.mkdir()
        self.runtime = self.root / "run"
        self.runtime.mkdir(mode=0o700)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        # `waddle open` finds its waddle-session sibling through its own executable path. Running a
        # copy from the lab's bin directory makes that sibling the lab's wrapper, never a binary
        # from the build tree (build/waddle-session exists after a plain `make`).
        self.waddle = self.bin / "waddle"
        shutil.copy2(WADDLE, self.waddle)
        self.waddle.chmod(0o755)
        self.invocations = self.root / "session-invocations.log"
        self.notify_log = self.root / "notify.jsonl"
        self.spawn_log = self.root / "spawn.jsonl"
        self.session_socket = self.runtime / "waddle" / "alpha" / "session.sock"
        self.env = os.environ.copy()
        self.env.update(
            HOME=str(self.home),
            XDG_CONFIG_HOME=str(self.root / "config"),
            XDG_STATE_HOME=str(self.root / "state"),
            XDG_RUNTIME_DIR=str(self.runtime),
        )
        for name in ("XDG_ACTIVATION_TOKEN", "DESKTOP_STARTUP_ID"):
            self.env.pop(name, None)
        self.env.pop("WADDLE_MOCK_GUEST_SOCK", None)
        self.env["PATH"] = str(self.bin) + os.pathsep + self.env["PATH"]
        self.write_notify_send()
        self.init_device()
        self.ini = self.root / "config" / "waddle" / "devices" / "alpha.ini"

    def run(self, *args, extra=None, timeout=60):
        env = self.env if not extra else {**self.env, **extra}
        return subprocess.run([str(self.waddle), *args], env=env, capture_output=True, text=True,
                              timeout=timeout)

    def assert_sibling_is_lab_wrapper(self):
        """Setup check: the only waddle-session the client can resolve is the lab's wrapper."""
        check(Path(self.waddle).parent == self.bin, ("client is not the lab copy", self.waddle))
        names = sorted(path.name for path in self.bin.iterdir() if path.name.startswith("waddle"))
        check(names == ["waddle", "waddle-session"], ("lab bin holds other waddle binaries", names))

    def spawned_wrappers(self):
        """Paths the session wrappers recorded as their own $0, one per invocation."""
        if not self.invocations.exists():
            return []
        return [line for line in self.invocations.read_text().splitlines() if line]

    def init_device(self):
        result = self.run("init", "alpha", "--blank-disk", "64M", timeout=300)
        assert result.returncode == 0, (result.stdout, result.stderr)

    def write_notify_send(self):
        script = self.bin / "notify-send"
        script.write_text(
            "#!/usr/bin/env python3\n"
            "import json, sys, time\n"
            "with open(%r, 'a', encoding='utf-8') as log:\n"
            "    log.write(json.dumps({'time': time.time(), 'args': sys.argv[1:]}) + '\\n')\n"
            % str(self.notify_log))
        script.chmod(0o755)

    def write_wrapper(self, body):
        """Write the lab's waddle-session wrapper; it records its own $0 before running body.

        The invocation log is reset here, so each case counts only its own spawns.
        """
        self.invocations.unlink(missing_ok=True)
        script = self.bin / "waddle-session"
        script.write_text(
            "#!/bin/sh\n"
            "echo \"$0\" >> %s\n"
            "%s\n" % (shlex.quote(str(self.invocations)), body))
        script.chmod(0o755)
        self.assert_sibling_is_lab_wrapper()

    def write_session_wrapper(self, scenario, delay, record):
        exec_options = ""
        if HARNESS is not None:
            exec_options = (" --exec %s --exec-device \"$2\" --exec-reply %s --exec-record %s"
                            % (str(HARNESS), shlex.quote(scenario.get("reply", "ok")), str(record)))
        self.write_wrapper(
            "exec python3 %s --socket \"$XDG_RUNTIME_DIR/waddle/$2/session.sock\" --dump %s"
            " --spawn-log %s --record %s --delay %s --idle-exit 20 --scenario %s%s"
            % (str(HERE / "open_fake_session.py"), str(DUMP), str(self.spawn_log), str(record),
               delay, "'" + json.dumps(scenario) + "'", exec_options))

    def write_silent_session_wrapper(self):
        """A session that exits 0 and never creates a socket (a handover that never completes)."""
        self.write_wrapper("exit 0")

    def write_exit_session_wrapper(self, status):
        """A session that exits nonzero before creating any socket."""
        self.write_wrapper("exit %d" % status)

    def set_notify(self, value):
        """Set integration.notify in the device file, whatever its current value is."""
        lines = self.ini.read_text(encoding="utf-8").splitlines()
        lines = ["notify = %s" % value if line.startswith("notify = ") else line for line in lines]
        self.ini.write_text("\n".join(lines) + "\n", encoding="utf-8")

    def make_file(self, relative, data="x"):
        path = self.home / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(data, encoding="utf-8")
        return path

    def notices(self):
        if not self.notify_log.exists():
            return []
        return [json.loads(line) for line in self.notify_log.read_text().splitlines()]


class HarnessServer:
    """Runs the real session command handler harness for one scenario (harness mode).

    The harness writes each guest LaunchRequest as one JSON line; launches() decodes them with
    the Python TLV reader. stop() sends SIGTERM, which the harness turns into a graceful local
    stop, and checks that it exited cleanly.
    """

    def __init__(self, lab, scenario):
        self.record = lab.root / ("harness-%d.jsonl" % time.monotonic_ns())
        self.ready = Path(str(self.record) + ".ready")
        # The harness takes its runtime and state directories from the lab environment, the same
        # ones the client uses. Inheriting the developer's XDG_RUNTIME_DIR would share session.lock
        # with other runs and make admission fail with Busy.
        self.process = subprocess.Popen(
            [str(HARNESS), "--runtime-dir", str(lab.runtime), "--device", "alpha",
             "--reply", scenario.get("reply", "ok"), "--record", str(self.record)],
            env=lab.env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        deadline = time.monotonic() + 15.0
        while not self.ready.exists():
            if self.process.poll() is not None:
                raise AssertionError(("harness exited before ready", self.process.stdout.read()))
            if time.monotonic() > deadline:
                self.process.kill()
                raise AssertionError("harness did not bind the socket")
            time.sleep(0.01)

    def launches(self):
        if not self.record.exists():
            return []
        entries = []
        for line in self.record.read_text().splitlines():
            frame = fake.decode_frame(bytes.fromhex(json.loads(line)["frame"]))
            entries.append({"connection": 1, "type": frame["type"], "request_id": frame["request_id"],
                            "fields": frame["by_name"], "critical": frame["critical"]})
        return [entry for entry in entries if entry["type"] == LAUNCH_REQUEST]

    def close(self):
        self.process.terminate()
        output, _ = self.process.communicate(timeout=20)
        check(self.process.returncode == 0, ("harness stop", self.process.returncode, output))


def make_server(lab, scenario):
    """The server for one case: the fake, or the real harness in harness mode."""
    if HARNESS is not None:
        return HarnessServer(lab, scenario)
    return Server(lab, scenario)


class Server:
    """Runs a FakeSession on the lab's session socket in a background thread."""

    def __init__(self, lab, scenario):
        self.record = lab.root / ("record-%d.jsonl" % time.monotonic_ns())
        self.session = fake.FakeSession(str(lab.session_socket), str(DUMP), scenario, str(self.record))
        self.session.bind()
        self.thread = threading.Thread(target=self.session.serve_forever, daemon=True)
        self.thread.start()

    def launches(self):
        if not self.record.exists():
            return []
        entries = [json.loads(line) for line in self.record.read_text().splitlines()]
        return [entry for entry in entries if entry.get("type") == fake.TYPE_LOCAL_LAUNCH]

    def close(self):
        self.session.listener.close()


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def warm_path(lab):
    target = lab.make_file("notes.txt")
    server = make_server(lab, {"reply": "ok"})
    try:
        result = lab.run("open", "--wait", str(target), extra={"XDG_ACTIVATION_TOKEN": "tok-1"})
        check(result.returncode == 0, ("ok reply", result.returncode, result.stderr))
        launches = server.launches()
        check(len(launches) == 1, ("one launch", launches))
        launch = launches[0]
        check(launch["request_id"] == 1, launch)
        fields = launch["fields"]
        check(fields["launch_kind"] == 2, fields)
        check(fields["target_path"] == "Z:\\notes.txt", fields)
        check("arguments" not in fields and "app_key" not in fields, fields)
        check(fields["activation_token"] == "tok-1", fields)
        check("working_dir" not in fields, "working_dir is not sent (open_command_contract 6)")
        check(launch["critical"]["launch_kind"] is True, launch)
    finally:
        server.close()


def statuses(lab):
    expected = {0: 0, 1: 1, 2: 1, 4: 1, 9: 1}
    for status, code in expected.items():
        target = lab.make_file("status-%d.txt" % status)
        server = make_server(lab, {"reply": "status:%d" % status})
        try:
            result = lab.run("open", "--wait", str(target))
            check(result.returncode == code, ("status", status, result.returncode, result.stderr))
        finally:
            server.close()


def error_and_eof(lab):
    target = lab.make_file("err.txt")
    server = make_server(lab, {"reply": "error:not allowed"})
    try:
        result = lab.run("open", "--wait", str(target))
        if HARNESS is not None:
            # The guest reports GuestError with a message; the client prints it as a launch failure.
            check(result.returncode == 1 and "not allowed" in result.stderr,
                  ("guest error reply", result.returncode, result.stderr))
        else:
            check(result.returncode == 1 and "launch refused: not allowed" in result.stderr,
                  ("error reply", result.returncode, result.stderr))
    finally:
        server.close()
    if HARNESS is not None:
        return
    for reply in ("partial", "eof"):
        server = make_server(lab, {"reply": reply})
        try:
            result = lab.run("open", "--wait", str(target))
            check(result.returncode == 125, (reply, result.returncode, result.stderr))
        finally:
            server.close()
    server = make_server(lab, {"reply": "eof"})
    try:
        result = lab.run("open", str(target))
        check(result.returncode == 0, ("eof is a handover without --wait", result.returncode))
    finally:
        server.close()


def grace_handover(lab):
    target = lab.make_file("silent.txt")
    server = make_server(lab, {"reply": "silent", "hold": 6})
    try:
        started = time.monotonic()
        result = lab.run("open", str(target))
        elapsed = time.monotonic() - started
        check(result.returncode == 0, ("grace handover", result.returncode, result.stderr))
        check(1.8 <= elapsed <= 5.5, ("grace duration", elapsed))
    finally:
        server.close()


def multiple_items(lab):
    paths = [lab.make_file("batch/%s.txt" % name) for name in ("a", "b", "c")]
    server = make_server(lab, {"reply": "ok"})
    try:
        result = lab.run("open", "--wait", *map(str, paths), extra={"XDG_ACTIVATION_TOKEN": "tok-batch"})
        check(result.returncode == 0, ("batch", result.returncode, result.stderr))
        launches = server.launches()
        check([entry["request_id"] for entry in launches] == [1, 2, 3], launches)
        check(len({entry["connection"] for entry in launches}) == 1, "one connection for all items")
        check(launches[0]["fields"].get("activation_token") == "tok-batch", launches[0])
        check(all("activation_token" not in entry["fields"] for entry in launches[1:]), launches)
        check(launches[2]["fields"]["target_path"] == "Z:\\batch\\c.txt", launches[2])
    finally:
        server.close()


def outside_share(lab):
    target = "/etc/hosts"
    server = make_server(lab, {"reply": "ok"})
    try:
        result = lab.run("open", "--wait", target)
        check(result.returncode == 126, ("outside", result.returncode, result.stderr))
        check(result.stderr.strip() == OUTSIDE_TEXT.format(file=target), result.stderr)
        check(not server.launches(), "nothing is sent for an outside path")
    finally:
        server.close()


def usage_errors(lab):
    result = lab.run("open", "--wait")
    check(result.returncode == 2 and "usage: waddle open" in result.stderr, result.stderr)
    result = lab.run("open", "--app", "a-x", str(lab.home / "x.txt"))
    check(result.returncode == 2, ("mixed targets", result.returncode))
    result = lab.run("open", "--bogus", str(lab.home / "x.txt"))
    check(result.returncode == 2, ("unknown option", result.returncode))


def wrong_mode_directory(lab):
    lab.session_socket.parent.mkdir(parents=True, exist_ok=True)
    os.chmod(lab.session_socket.parent, 0o755)
    target = lab.make_file("mode.txt")
    try:
        result = lab.run("open", "--wait", str(target))
        check(result.returncode == 125 and "not private to this user" in result.stderr,
              ("wrong mode", result.returncode, result.stderr))
    finally:
        os.chmod(lab.session_socket.parent, 0o700)


def wait_for_notices(lab, count, prefix="", timeout=5.0):
    """Poll the notify log until count notices start with prefix (or the timeout ends)."""
    deadline = time.monotonic() + timeout
    while True:
        notices = [entry for entry in lab.notices() if entry["args"][-1].startswith(prefix)]
        if len(notices) >= count or time.monotonic() >= deadline:
            return notices
        time.sleep(0.05)


def stop_session(lab):
    """Stop any session a cold-start case left running (the fake or the harness)."""
    if HARNESS is not None:
        subprocess.run(["pkill", "-TERM", "-f", "interop_session_local_harness --runtime-dir %s" % lab.runtime],
                       check=False)
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline and lab.session_socket.exists():
            time.sleep(0.05)
        check(not lab.session_socket.exists(), "harness removed its socket on graceful stop")
        return
    subprocess.run(["pkill", "-f", "open_fake_session.py --socket %s" % lab.session_socket], check=False)


def cold_path(lab):
    target = lab.make_file("cold.txt")
    record = lab.root / "cold-record.jsonl"
    lab.write_session_wrapper({"reply": "ok"}, delay=3.0, record=record)
    if lab.session_socket.parent.exists():
        shutil.rmtree(lab.session_socket.parent)
    lab.notify_log.unlink(missing_ok=True)
    started = time.time()
    result = lab.run("open", "--wait", str(target), timeout=120)
    check(result.returncode == 0, ("cold", result.returncode, result.stderr))
    check(lab.spawned_wrappers() == [str(lab.bin / "waddle-session")],
          ("the spawn must be the lab wrapper", lab.spawned_wrappers()))
    spawns = [json.loads(line) for line in lab.spawn_log.read_text().splitlines()]
    check(len(spawns) == 1, ("exactly one spawn", spawns))
    spawn = spawns[0]
    check(spawn["sid"] == spawn["pid"], ("spawned into a new session", spawn))
    check(spawn["sid"] != os.getsid(0), "new session differs from the test session")
    notices = wait_for_notices(lab, 1, prefix="Starting Windows")
    check(len(notices) == 1, ("one notice", lab.notices()))
    check(notices[0]["args"][-1] == "Starting Windows\u2026", notices)
    check(notices[0]["time"] - started >= 1.4, ("notice after 1.5 s", notices[0]["time"] - started))
    if HARNESS is None and record.exists():
        check(json.loads(record.read_text().splitlines()[0])["fields"]["launch_kind"] == 2, "record")
    lab.spawn_log.unlink()
    lab.notify_log.unlink(missing_ok=True)
    stop_session(lab)


def cold_path_without_notice(lab):
    target = lab.make_file("quiet.txt")
    lab.set_notify("false")
    try:
        lab.write_session_wrapper({"reply": "ok"}, delay=1.0, record=lab.root / "quiet.jsonl")
        if lab.session_socket.parent.exists():
            shutil.rmtree(lab.session_socket.parent)
        result = lab.run("open", "--wait", str(target), timeout=120)
        check(result.returncode == 0, ("quiet cold", result.returncode, result.stderr))
        time.sleep(0.5)
        check(not wait_for_notices(lab, 1, prefix="Starting Windows", timeout=1.0),
              ("notify off sends no notice", lab.notices()))
    finally:
        lab.set_notify("true")
        stop_session(lab)


def outside_share_notifies(lab):
    lab.notify_log.unlink(missing_ok=True)
    result = lab.run("open", "/etc/hosts")
    check(result.returncode == 126, result.returncode)
    notices = wait_for_notices(lab, 1, prefix="'/etc/hosts'")
    check(notices and notices[0]["args"][-1] == OUTSIDE_TEXT.format(file="/etc/hosts"), notices)


def cold_path_deadline(lab):
    target = lab.make_file("deadline.txt")
    lab.write_silent_session_wrapper()
    if lab.session_socket.parent.exists():
        shutil.rmtree(lab.session_socket.parent)
    started = time.monotonic()
    result = lab.run("open", "--wait", str(target), timeout=200)
    elapsed = time.monotonic() - started
    check(result.returncode == 125 and "did not start" in result.stderr,
          ("deadline", result.returncode, result.stderr))
    check(120 <= elapsed <= 150, ("deadline duration", elapsed))
    # Exit 0 is a handover, so the client kept polling the socket and spawned exactly once.
    check(lab.spawned_wrappers() == [str(lab.bin / "waddle-session")], lab.spawned_wrappers())


def cold_path_child_exits(lab):
    """A waddle-session that exits 3 without a socket fails at once, naming the status."""
    target = lab.make_file("exits.txt")
    if lab.session_socket.parent.exists():
        shutil.rmtree(lab.session_socket.parent)
    lab.invocations.unlink(missing_ok=True)
    lab.write_exit_session_wrapper(3)
    started = time.monotonic()
    result = lab.run("open", "--wait", str(target), timeout=120)
    elapsed = time.monotonic() - started
    check(result.returncode == 125, ("child exit", result.returncode, result.stderr))
    check("waddle-session exited with status 3" in result.stderr, ("status in message", result.stderr))
    # The client waits one extra backoff after seeing the exit, then fails; the deadline is 120 s.
    check(elapsed < 2.0, ("failed promptly", elapsed))
    check(lab.spawned_wrappers() == [str(lab.bin / "waddle-session")], lab.spawned_wrappers())
    check(not lab.session_socket.exists(), "no socket was created")


def codec_cross_check(lab):  # noqa: ARG001 - no device state needed
    """The Python TLV reader agrees with frames encoded by the production Zig codec."""
    launch = fake.decode_frame(bytes.fromhex(subprocess.run(
        [str(DUMP), "launch", "9", "1", "a-Microsoft_App", "--x 'a b'", "tok-9"],
        capture_output=True, text=True, check=True).stdout.strip()))
    check(launch["type"] == fake.TYPE_LOCAL_LAUNCH and launch["request_id"] == 9, launch)
    check(launch["by_name"] == {"app_key": "a-Microsoft_App", "arguments": "--x 'a b'",
                                "launch_kind": 1, "activation_token": "tok-9"}, launch["by_name"])
    check(launch["critical"]["app_key"] and launch["critical"]["launch_kind"]
          and not launch["critical"]["arguments"], launch["critical"])
    result = fake.decode_frame(bytes.fromhex(subprocess.run(
        [str(DUMP), "result", "9", "4", "gone"], capture_output=True, text=True,
        check=True).stdout.strip()))
    check(result["type"] == fake.TYPE_LAUNCH_RESULT and result["by_name"] == {
        "process_id": 4242, "status": 4, "message": "gone"}, result)
    error = fake.decode_frame(bytes.fromhex(subprocess.run(
        [str(DUMP), "error", "9", "3", "no"], capture_output=True, text=True,
        check=True).stdout.strip()))
    check(error["type"] == fake.TYPE_ERROR and error["by_name"] == {"message": "no", "error_code": 3},
          error)


def main():
    cases = [codec_cross_check, warm_path, statuses, error_and_eof, grace_handover, multiple_items,
             outside_share, usage_errors, wrong_mode_directory, cold_path, cold_path_without_notice,
             outside_share_notifies, cold_path_child_exits]
    if SLOW:
        cases.append(cold_path_deadline)
    if HARNESS is not None:
        cases = [case for case in cases if case.__name__ not in FAKE_ONLY_CASES]
    with tempfile.TemporaryDirectory(prefix="waddle-open-e2e-") as directory:
        lab = Lab(os.path.realpath(directory))
        for case in cases:
            print("open e2e:", case.__name__, "(harness)" if HARNESS is not None else "", flush=True)
            case(lab)
            if lab.session_socket.parent.exists():
                shutil.rmtree(lab.session_socket.parent)
    print("open e2e: passed (%d cases%s%s)" % (len(cases), ", slow deadline included" if SLOW else "",
                                             ", session command harness" if HARNESS is not None else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
