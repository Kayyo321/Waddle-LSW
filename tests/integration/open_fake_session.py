#!/usr/bin/env python3
"""Fake session.sock server for the `waddle open` end-to-end tests.

Decodes client LocalLaunch frames with a minimal TLV reader written from IMPL_DESC
3.2.1-3.2.3 (16-byte header, u16 tag | u16 length | value | zero padding to 4, critical
bit 0x8000). Replies are encoded by the production codec through tests/interop/open_frame_dump
so the server side never hand-rolls a reply. Also runnable as the fake `waddle-session`:
`open_fake_session.py --socket PATH --record PATH --spawn-log PATH --dump PATH --scenario JSON
--delay SECONDS --idle-exit SECONDS`. With `--exec HARNESS --exec-device NAME --exec-reply REPLY
--exec-record PATH` it replaces itself with the real session command handler harness after the
spawn log and delay (open_e2e.py harness mode).
"""
import argparse
import json
import os
import socket
import struct
import subprocess
import sys
import threading
import time

MAGIC = 0x504F4957
VERSION = 2
HEADER = struct.Struct("<IHHII")
TYPE_LOCAL_LAUNCH = 0x7F00
TYPE_LAUNCH_RESULT = 0x0701
TYPE_ERROR = 0x0006
TYPE_LOCAL_STOP = 0x7F01
FROM_LOCAL = 3

# Field registry subset used by the open client: tag -> (name, kind).
FIELDS = {
    0x0008: ("app_key", "str"),
    0x0009: ("process_id", "u32"),
    0x002F: ("target_path", "str"),
    0x0030: ("arguments", "str"),
    0x0031: ("working_dir", "str"),
    0x0036: ("launch_kind", "u8"),
    0x0037: ("activation_token", "str"),
    0x0038: ("status", "u32"),
    0x0039: ("message", "str"),
    0x0046: ("error_code", "u32"),
}


class ProtocolError(Exception):
    """A frame that violates IMPL_DESC 3.2: magic, version, length, padding or tag order."""


def decode_payload(payload):
    """Return the ordered list of (name, critical, value) fields of a payload."""
    fields = []
    offset = 0
    last_base = -1
    while offset < len(payload):
        if offset + 4 > len(payload):
            raise ProtocolError("truncated field header")
        tag, length = struct.unpack_from("<HH", payload, offset)
        base = tag & 0x7FFF
        critical = bool(tag & 0x8000)
        offset += 4
        if offset + length > len(payload):
            raise ProtocolError("truncated field value")
        value_bytes = payload[offset:offset + length]
        offset += length
        padded = (length + 3) & ~3
        padding = payload[offset:offset + (padded - length)]
        if len(padding) != padded - length or any(padding):
            raise ProtocolError("nonzero or missing padding")
        offset += padded - length
        if base <= last_base:
            raise ProtocolError("tags not strictly increasing")
        last_base = base
        name, kind = FIELDS.get(base, ("tag_0x%04x" % base, "bytes"))
        if kind == "u8":
            value = value_bytes[0]
        elif kind == "u32":
            value = struct.unpack("<I", value_bytes)[0]
        elif kind == "str":
            value = value_bytes.decode("utf-8")
        else:
            value = value_bytes
        fields.append((name, critical, value))
    return fields


def decode_frame(data):
    """Decode one complete frame into a dict with type, request_id, direction-free fields."""
    if len(data) < HEADER.size:
        raise ProtocolError("short header")
    magic, version, kind, length, request_id = HEADER.unpack_from(data, 0)
    if magic != MAGIC or version != VERSION:
        raise ProtocolError("bad magic or version")
    if length % 4 or len(data) != HEADER.size + length:
        raise ProtocolError("bad length")
    fields = decode_payload(data[HEADER.size:])
    return {
        "type": kind,
        "request_id": request_id,
        "fields": fields,
        "by_name": {name: value for name, _critical, value in fields},
        "critical": {name: critical for name, critical, _value in fields},
    }


def read_exact(conn, count):
    chunks = []
    remaining = count
    while remaining:
        chunk = conn.recv(remaining)
        if not chunk:
            return None
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def read_frame(conn):
    header = read_exact(conn, HEADER.size)
    if header is None:
        return None
    _magic, _version, _kind, length, _request_id = HEADER.unpack(header)
    payload = read_exact(conn, length) if length else b""
    if payload is None:
        return None
    return header + payload


def encode_with_codec(dump, *args):
    """Frame bytes produced by the production codec through open_frame_dump."""
    result = subprocess.run([dump, *args], capture_output=True, text=True, check=True)
    return bytes.fromhex(result.stdout.strip())


class FakeSession:
    """Serves LocalLaunch frames on a unix socket and answers according to a scenario.

    scenario keys: reply = ok | status:N | error:MESSAGE | partial | silent | eof ;
    hold = seconds that a silent connection stays open before the server closes it.
    Every decoded LocalLaunch is appended to record_path as one JSON line, tagged with the
    connection index so tests can assert that several items shared one connection.
    """

    def __init__(self, socket_path, dump, scenario, record_path=None, idle_exit=None):
        self.socket_path = socket_path
        self.dump = dump
        self.scenario = scenario
        self.record_path = record_path
        self.idle_exit = idle_exit
        self.connections = 0
        self.listener = None

    def bind(self):
        parent = os.path.dirname(self.socket_path)
        os.makedirs(parent, mode=0o700, exist_ok=True)
        # makedirs applies the mode to the leaf only; the intermediate waddle directory would take
        # the umask, and the client refuses ancestors that others can write.
        os.chmod(parent, 0o700)
        os.chmod(os.path.dirname(parent), 0o700)
        if os.path.exists(self.socket_path):
            os.unlink(self.socket_path)
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(self.socket_path)
        self.listener.listen(8)
        if self.idle_exit is not None:
            self.listener.settimeout(self.idle_exit)

    def serve_forever(self):
        while True:
            try:
                conn, _ = self.listener.accept()
            except OSError:
                return
            self.connections += 1
            index = self.connections
            threading.Thread(target=self.serve_connection, args=(conn, index), daemon=True).start()

    def record(self, entry):
        if self.record_path is None:
            return
        with open(self.record_path, "a", encoding="utf-8") as log:
            log.write(json.dumps(entry) + "\n")

    def serve_connection(self, conn, index):
        try:
            while True:
                data = read_frame(conn)
                if data is None:
                    return
                frame = decode_frame(data)
                self.record({"connection": index, "time": time.time(), "type": frame["type"],
                             "request_id": frame["request_id"], "fields": frame["by_name"],
                             "critical": frame["critical"]})
                if frame["type"] == TYPE_LOCAL_STOP:
                    if not self.answer_stop(conn, frame["request_id"]):
                        return
                    continue
                if frame["type"] != TYPE_LOCAL_LAUNCH:
                    continue
                if not self.answer(conn, frame["request_id"]):
                    return
        except (ProtocolError, OSError) as error:
            self.record({"connection": index, "error": str(error)})
        finally:
            conn.close()

    def answer_stop(self, conn, request_id):
        """Send the scenario's "stop" reply for one LocalStop. Returns False when the connection ends.

        Values: ok | status:N | status:N:MESSAGE | error:MESSAGE | wrong_id | eof | partial | silent.
        """
        reply = self.scenario.get("stop", "ok")
        if reply == "ok":
            conn.sendall(encode_with_codec(self.dump, "stop-result", str(request_id), "0", "-"))
        elif reply.startswith("status:"):
            parts = reply.split(":", 2)
            message = parts[2] if len(parts) > 2 else "-"
            conn.sendall(encode_with_codec(self.dump, "stop-result", str(request_id), parts[1], message))
        elif reply.startswith("error:"):
            message = reply.split(":", 1)[1] or "-"
            conn.sendall(encode_with_codec(self.dump, "error", str(request_id), "3", message))
        elif reply == "wrong_id":
            conn.sendall(encode_with_codec(self.dump, "stop-result", str(request_id + 1), "0", "-"))
        elif reply == "partial":
            frame = encode_with_codec(self.dump, "stop-result", str(request_id), "0", "-")
            conn.sendall(frame[: len(frame) // 2])
            return False
        elif reply == "eof":
            return False
        elif reply == "silent":
            time.sleep(float(self.scenario.get("hold", 8)))
            return False
        else:
            raise ValueError("unknown scenario stop reply %r" % reply)
        return True

    def answer(self, conn, request_id):
        """Send the scenario's reply for one request. Returns False when the connection ends."""
        reply = self.scenario.get("reply", "ok")
        if reply == "ok":
            conn.sendall(encode_with_codec(self.dump, "result", str(request_id), "0", "-"))
        elif reply.startswith("status:"):
            status = reply.split(":", 1)[1]
            conn.sendall(encode_with_codec(self.dump, "result", str(request_id), status, "-"))
        elif reply.startswith("error:"):
            message = reply.split(":", 1)[1] or "-"
            conn.sendall(encode_with_codec(self.dump, "error", str(request_id), "3", message))
        elif reply == "partial":
            frame = encode_with_codec(self.dump, "result", str(request_id), "0", "-")
            conn.sendall(frame[: len(frame) // 2])
            return False
        elif reply == "eof":
            return False
        elif reply == "silent":
            time.sleep(float(self.scenario.get("hold", 5)))
            return False
        else:
            raise ValueError("unknown scenario reply %r" % reply)
        return True


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--socket", required=True)
    parser.add_argument("--record")
    parser.add_argument("--spawn-log")
    parser.add_argument("--dump", required=True)
    parser.add_argument("--scenario", default='{"reply": "ok"}')
    parser.add_argument("--delay", type=float, default=0.0)
    parser.add_argument("--idle-exit", type=float, default=20.0)
    parser.add_argument("--exec", dest="exec_path",
                        help="after the spawn log and delay, replace this process with the session harness")
    parser.add_argument("--exec-device")
    parser.add_argument("--exec-reply")
    parser.add_argument("--exec-record")
    args = parser.parse_args(argv)
    if args.spawn_log:
        with open(args.spawn_log, "a", encoding="utf-8") as log:
            log.write(json.dumps({"pid": os.getpid(), "sid": os.getsid(0), "argv": sys.argv,
                                  "time": time.time()}) + "\n")
    time.sleep(args.delay)
    if args.exec_path:
        # Same pid and session as the spawn log entry, so the spawn checks still hold.
        os.execv(args.exec_path, [args.exec_path, "--runtime-dir", os.environ["XDG_RUNTIME_DIR"],
                                  "--device", args.exec_device, "--reply", args.exec_reply,
                                  "--record", args.exec_record])
    server = FakeSession(args.socket, args.dump, json.loads(args.scenario), args.record, args.idle_exit)
    server.bind()
    server.serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
