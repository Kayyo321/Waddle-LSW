# `waddle open` client contract (TODO #29b, #29d, client half of #29h)

Realizes IMPL_DESC 3.9.1 (client side), 8.44 (last paragraph: the session launch
socket retry), and the exit-code table of 3.9.1 and 6.5. Grammar, classification
and exit-code tables are in `open_request_contract.md`. The server side (the
session.sock listener and LocalLaunch dispatch) is another lane and is not part
of this contract.

## 1. Files and entry

- `src/interop/open_command.zig`: orchestration and production hooks. It lives
  in `src/interop` next to the modules it imports (`open_request.zig`,
  `activation_store.zig`, `pe_header.zig`), because Zig imports cannot leave the
  module root and `tests/av/coverage.py` locates interop modules there. This is
  a deviation from the lane plan, which named `src/cli/`.
- `src/cli/open_command.h`: the C entry, included by `host.c`.
- Entry: `int waddle_open_command(int argc, char **argv, exec_fn, resolve_fn)`.
  `argv` starts after the `open` word. `exec_fn` is `cmd_exec` and `resolve_fn`
  is `resolve_target_device`, both from `host.c`. Passing them keeps device
  selection and delegation in one place, with no refactor of host.c.

## 2. Effects (Hooks)

Every effect goes through `Hooks`, a struct of function pointers, so the
orchestration is tested with a scripted fake and a virtual clock. The production
implementation (`RealHooks`) uses:

| Effect | Production implementation |
|---|---|
| device | `resolve_fn(name, &device, socket_buf, 1024, 1)`; the session socket is `dirname(socket)/session.sock` |
| config | `daemon_config_load_file` on the device config path, once per invocation |
| read prefix | open and read up to 4096 bytes; ENOENT is NotFound |
| realpath | `std.posix.realpath` |
| translate | `daemon_fs_translate_path(path, &config)`; EXDEV or EINVAL is outside every share |
| runtime dir | `fstatat` with no-follow: must be a directory, owned by this uid, mode 0700; a missing directory is allowed |
| connect | `AF_UNIX` stream connect; ENOENT is NotFound, ECONNREFUSED is Refused |
| spawn | `posix_spawnp("waddle-session", "--device", NAME)`, new session (`POSIX_SPAWN_SETSID`), stdio on `/dev/null`, not waited for |
| notify | `posix_spawnp("notify-send", "--app-name=waddle", TEXT)`, best effort |
| clock | monotonic |
| delegate | `exec_fn(argc, argv, 2)` with `waddle exec --device NAME --translate-path -- GUEST ARG...` |

`waddle-session` is located next to `/proc/self/exe` when that file exists, and
otherwise by `PATH` lookup.

## 3. Parse, device and paths

1. `open_request.parse` validates the whole argv first. Usage errors (exit 2)
   print the error line and the usage line, and no callback runs.
2. The read callback runs only for `.exe` items during classification. Status
   NotFound is exit 127 (`'<path>': no such file`); unreadable is exit 126
   (`'<path>': cannot read the executable header`).
3. The device is resolved once. A resolver failure returns its own exit code,
   after the resolver has printed the reason.
4. For each non-App item: realpath (NotFound is exit 127; other failures 125),
   then translate. An outside path exits 126 with the IMPL 3.9.1 text:
   `'<file>' is outside the folders shared with Windows (<device>). Add a share with: waddle device config set filesystem.<name> <dir>:<drive>:"`.
   The notification is sent only when `integration.notify` is on. `<file>` is the
   path as the user gave it.

## 4. Delegation

- A request whose single item is DelegateExec is handed to `exec_fn` with the
  guest path and the raw argument tail. `cmd_exec` applies its own
  `--translate-path` to the arguments and returns the command's exit code.
- `DelegateConflict` (exit 2) is decided by open_request before the client runs.

## 5. Token

`XDG_ACTIVATION_TOKEN`, then `DESKTOP_STARTUP_ID`, chosen by
`activation_store.pick_token`. The chosen token is attached to the first sent
item only (`open_request_contract.md` section 4.2). A token that is not valid
under the frame rule is not attached.

## 6. Frames

One LocalLaunch frame per sent item (`0x7F00`, direction `InteropFromLocal`),
built with the codec in ascending tag order: target or app key (0x2F or 0x08),
arguments (0x30, the quoted tail from `open_request.quote_into`), launch_kind
(0x36, the wire mapping of the item kind), activation_token (0x37, first item
only). `working_dir` is not sent: the guest uses the executable's directory,
as IMPL 3.9.3 specifies. Request ids are 1, 2, ... in argv order.

## 7. Connect and cold start (IMPL 8.44)

- Before each attempt the runtime directory is checked. An unsafe directory is
  refused (exit 125) with no connect.
- On NotFound or Refused, `waddle-session --device NAME` is spawned once per
  invocation. Attempts retry with backoff 25 ms, 50, 100, 200, then 250 ms.
- One notice, `Starting Windows…`, is sent when at least 1500 ms have elapsed
  and `integration.notify` is on.
- A 120 000 ms monotonic deadline ends the loop with exit 125 and the message
  `waddle: Windows (<device>) did not start`.
- Any other connect failure is exit 125.

## 8. Replies and exit codes

Each item is written in full, then its reply is read.

| Event | `--wait` | no `--wait` |
|---|---|---|
| LaunchResult status 0 (Ok) | 0 | 0 |
| LaunchResult other status | `exit_code_for_launch_status` = 1, message printed | same (decided by the reply, if it arrives in 2 s) |
| Error frame with the request id | 1, message printed | 1 |
| Frame of another type or request id | skipped | skipped |
| Timeout (no reply) | cannot happen: waits for the reply or end of stream | 0 (handover, the server accepted the request) |
| End of stream at a frame boundary | 125 (no result) | 0 (handover); the next item reconnects without a spawn |
| End of stream mid-frame, or a protocol error | 125 | 125 |

`--wait` waits without a timeout for a reply or end of stream, so the
connection lifetime bounds it. Without `--wait` the grace is 2000 ms
(`GraceMs`) for the first byte of each reply.

Multiple items use one connection, in order. The exit code is the first
nonzero item code. A failed item does not stop later items.

Notices from replies are printed to stderr as `waddle open: <message>`.

## 9. Security checks

- The runtime directory must be owned by this uid and mode 0700, and not be a
  symlink. It is checked before every connect.
- The socket is connected only after that check. The socket file's owner is not
  checked separately; the directory check bounds it.

## 10. Open decisions for the lead

1. Module location (section 1): `src/interop/` instead of `src/cli/`.
2. Reply policy without `--wait` (section 8): a 2 s grace for an immediate Error
   or non-Ok LaunchResult, else handover. The spec does not name a value.
3. Usage messages are terse (`invalid request (code N)`) and could get human
   wording.

## 11. Verification

- Unit tests in `open_command.zig` (104): scripted fakes with a virtual clock,
  and production hooks against real directories, Unix sockets, file reads,
  realpath, mount config, a pseudo-terminal, detached spawn and notify scripts on
  PATH. Allocation-failure sweeps use `std.testing.FailingAllocator` over five
  argument shapes; the allocator tracks every block so the sweep reaches each
  allocation site; `testing.allocator` detects leaks.
- Coverage (`tests/av/coverage.py open_command`): 94.03 percent branches
  (126/134) and 95.19 percent lines (277/291).
- End-to-end (`tests/integration/open_e2e.py`, target `interop-open-e2e-test`):
  runs the built `build/waddle` against a fake `session.sock` in a temporary
  `XDG_RUNTIME_DIR/waddle/alpha/` (directory mode 0700). The fake server decodes
  each frame with a Python TLV reader (`open_fake_session.py`) and replies with
  frames encoded by the production codec (`tests/interop/open_frame_dump.c`).
  The Python reader is cross-checked against Zig-codec frames for each field kind
  and the critical bits. Twelve cases: see the commit message of `996aec3`.
- Slow case (`interop-open-e2e-slow`): the real 120 s cold-start deadline.

Decisions made while implementing section 7's test requirements:

- The 120 s deadline is tested with the real timeout in the slow case, not a
  shortened one. A test-only deadline override would change production
  semantics, which the lane rules forbid. The deadline logic is also covered by
  unit tests with a virtual clock.
- `working_dir` is not sent (section 6). The end-to-end test asserts that it is
  absent.
- The device name in the outside-share message was a dangling stack pointer
  until the end-to-end run found it (fixed in `c6977ed`).
