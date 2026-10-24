# Linux session lease and VSOCK admission: frozen bounded owner

This is the normative bounded-owner contract for the #7 connection/VSOCK/singleton slice. It keeps
the existing Linux byte owner and pure peer API authoritative. It does not
implement GuestReady daemon IPC, profile parsing, manifest policy, IVSHMEM
mapping, feature dispatch, guest admission, Wayland, or epoch-reset proof.

## Files and actual runtime entry

Add `src/interop/linux_session_owner.h`, `src/interop/linux_session_owner.c`,
and `tests/interop/linux_session_owner.c`. The owner is a short-lived C wrapper
that retains the two exclusive per-device lock leases and creates the existing
`interop_linux_socket_t` from one AF_VSOCK connection. It owns no mapping,
manifest, or application resource.

The wrapper is useful only if the same bounded slice wires an actual entry point:
the new `waddle-session --device NAME` executable (or the agreed session-start
entry in the host CLI) resolves a registered profile, checks the port conflict,
acquires this owner, invokes the already-existing daemon client start/status
operations plus the separately implemented GuestReady request, then calls the
VSOCK admission method. Its main poll loop obtains the borrowed socket-owner
pointer from this wrapper and calls `interop_linux_socket_poll_events()` and
`interop_linux_socket_drive()` on that same thread. No new `waddle-session`
process or `session.sock` runtime currently exists; do not report an unused
library as an integrated session. `src/cli/session.c` remains the guest-exec
stdin/stdout multiplexer and is not this entry point.

## Proposed public C contract

Use C ABI and structured Doxygen on every public declaration/type/field. Types
end in `_t`; fields/functions are `snake_case`; enum values/macros use
`PascalCase`. All operations run on the creating pthread. No callback or caller
pointer is retained.

```c
typedef struct interop_linux_session_owner_t interop_linux_session_owner_t;

typedef enum interop_linux_session_result_t {
    InteropLinuxSessionSuccess = 0,
    InteropLinuxSessionInvalidArgument = 1,
    InteropLinuxSessionOutOfMemory = 2,
    InteropLinuxSessionBusy = 3,
    InteropLinuxSessionGuestPortConflict = 4,
    InteropLinuxSessionCancelled = 5,
    InteropLinuxSessionTimedOut = 6,
    InteropLinuxSessionConnectError = 7,
    InteropLinuxSessionPeerCreateError = 8,
    InteropLinuxSessionPeerAlreadyCreated = 9,
    InteropLinuxSessionSocketOwnerNotDestroyed = 10,
    InteropLinuxSessionCloseDiagnostic = 11
} interop_linux_session_result_t;

typedef struct interop_linux_session_config_t {
    const device_info_t *device; /* Borrowed selected profile; copied at acquire. */
    const uint8_t *agent_version; /* Borrowed bounded SemVer2; copied by peer create. */
    size_t agent_version_bytes;
    uint64_t connection_token; /* Nonzero stale-callback token, not a credential. */
    uint64_t local_capabilities; /* Only implemented protocol capability bits. */
    uint32_t drain_timeout_ms; /* 1..InteropLinuxSocketMaxDrainTimeoutMs. */
    uint32_t reserved_zero;
} interop_linux_session_config_t;

int interop_linux_session_acquire(
    const interop_linux_session_config_t *config,
    interop_linux_session_owner_t **out_owner);

int interop_linux_session_connect_peer(
    interop_linux_session_owner_t *owner,
    uint32_t connect_timeout_ms,
    int cancel_fd);

interop_linux_socket_t *interop_linux_session_socket_owner(
    const interop_linux_session_owner_t *owner);

int interop_linux_session_destroy_peer(
    interop_linux_session_owner_t *owner);

int interop_linux_session_release(
    interop_linux_session_owner_t **owner_slot);
```

`cancel_fd == -1` disables cancellation; otherwise it is a borrowed pollable fd
that the caller owns through the synchronous connect attempt. Cancellation
returns without changing lease ownership and leaves the caller able to retry.
`connect_timeout_ms` is one attempt's finite deadline, capped at 2000 ms by the
implementation. On `Success`, the wrapper owns the connected fd through the
socket owner. `socket_owner()` is borrowed and valid only until
`destroy_peer()` or `release()`; every peer operation and destruction remains on
the creating thread.

`destroy_peer()` closes/destroys the connected socket owner once, but does not
release either singleton lease. `release()` is allowed only after peer-owner
destruction and after the caller has completed all external Wayland/mapping
teardown obligations. It closes AV lease then session lease and frees the
wrapper. A close failure is reported after fd ownership is relinquished; it is
never retried. There is deliberately no `resources_released` boolean, epoch
setter, reconnect method, mapping callback, status-poll callback, or arbitrary
release-proof API. The caller's documented precondition—not a fabricated
boolean—is that external ownership is actually quiescent before releasing the
AV lease.

## Profile validation and lock ownership

`acquire()` consumes a borrowed, already selected `device_info_t` produced by
the existing profile-selection path, then validates/copies only the required
fields. The real session entry resolves an explicit name with
`daemon_device_find()` or resolves the saved default/list using existing device
selection rules; it does not parse the INI itself. Require `config_valid`, a
valid registered device name, nonzero CID, nonzero configured guest-exec port,
and bounded NUL-terminated state/daemon paths. Reject
`device->vsock_port == InteropControlPort` (5002) before daemon ensure/start,
lock acquisition, or any VSOCK operation. Do not silently replace the
configured guest-exec port. The VSOCK connector always targets the selected
device CID and reserved port 5002.

Acquire locks in this exact order and retain both until explicit release:

1. Derive the per-device runtime directory from the existing
   `daemon_device_get_socket_path(device->name, ...)` result by validating and
   removing its final `/daemon.sock` component; open the directory with
   `daemon_device_open_directory(path, 1)`, then verify it is a directory owned
   by `geteuid()` with no group/other permissions. Create/open `session.lock`
   relative to that directory using `openat(O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,
   0600)`. Verify with `fstat` that it is a regular file owned by `geteuid()`
   and private (`mode & 0077 == 0`), then take `flock(LOCK_EX|LOCK_NB)`. `EWOULDBLOCK`
   means another session owns it and maps to `InteropLinuxSessionBusy`. Never
   unlink a held lock or infer a stale owner from a PID.
2. Open `<device->state_dir>/av` through `daemon_device_open_directory(..., 1)`;
   verify directory ownership/private permissions; then openat `command.lock`
   with the same no-follow/0600 and regular-file/owner/private checks used by
   `src/cli/av_managed.inc`. Take `flock(LOCK_EX|LOCK_NB)`. This is the existing
   AV command singleton in IMPL_DESC §6.10 and must remain held across admission,
   Ready, Draining, native teardown and unmap permission.

If AV acquisition fails, close its temporary descriptors, then release the
session lock. If later connect/create fails before a connected peer has been
published, keep the leases available to the session entry for retry only while
the daemon is Running and its operation remains pre-admission. Once a VSOCK
connect succeeds, mark the attempt consumed: if peer-owner creation fails, close
the connected fd and do not reconnect from this owner instance. Guest code may
have observed that accepted connection; the wrapper must fail closed. The
session entry retains the AV lease until its external teardown path has handled
the attempted session and then releases it explicitly.

The daemon's `waddle.lock` remains owned by `waddled`; it is not used for a
session singleton. Manifest operations follow the separate documented order
`session.lock -> AV command.lock -> manifest.lock`; this API never opens
`manifest.lock` or calls the parse-only `manifest.zig` module.

## Connector semantics and handoff

`connect_peer()` performs exactly one bounded connection attempt. It creates
`socket(AF_VSOCK, SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC, 0)`, connects to
`(device.vsock_cid, InteropControlPort)`, polls `POLLOUT` with the remaining
monotonic deadline, and confirms completion with `getsockopt(SOL_SOCKET,
SO_ERROR)`. EINTR retries against the same deadline; cancellation is polled
alongside the socket; every unsuccessful attempt closes its temporary fd once
and leaves outputs/leases unchanged. It does not perform GuestReady, status
queries, sleeps or retry policy. The session event loop uses the existing daemon
client status/GuestReady helpers to decide whether to call another attempt and
implements the frozen 250 ms → 4 s retry schedule only while daemon reports
Running. This keeps daemon IPC in the daemon/client layer rather than cloning
its parser/protocol inside the owner.

After successful connect, read a fresh monotonic timestamp at the point
`getsockopt(SO_ERROR)` confirms connection, convert it to milliseconds with
checked/saturating arithmetic, then build an `interop_linux_socket_config_t`
with role `InteropPeerHost`, the supplied unique nonzero callback token, the
captured acceptance timestamp, the borrowed version/capability facts, and drain
timeout.
Call `interop_linux_socket_create(fd, &socket_config, &owner->socket_owner)`
immediately, before any read/write/poll on the fd. On create failure the
connector still owns and closes fd; classify the remote accept as consumed and
prohibit another connect attempt from this lease owner. On create success, the
existing socket owner owns the descriptor exclusively; no other code may
close, duplicate, poll, read, or write it. `interop_linux_socket_poll_events`
and `drive` are the sole I/O path.

GuestReady status 0 is only guest-exec reachability on configured
`vsock_port`—not interop Ready, host trust, mapping permission or epoch
authority. The guest owns and initializes the fresh nonzero mapping epoch before
Hello. The host accepts `ValidateMapping` only after actual connection protocol
traffic, and its separate native mapper validates actual mapping facts before
calling socket-owner `mapping_result`; this API never does that work. The
connection token prevents stale callback reuse but is not authentication.

## Session sequencing and errors

The actual session entry sequence is: resolve selected device; reject 5002
collision; acquire session+AV leases; connect Wayland/local runtime as its own
session layer; ask daemon GuestReady (120 s); connect AF_VSOCK attempts while
status remains Running; create peer/socket owner; enter the session poll loop.
The connector API starts only after GuestReady succeeds. Do not make it start a
VM itself: the existing daemon client start/status APIs own that lifecycle.

On pre-connect cancellation, timeout, GuestReady failure or daemon stop, close
temporary descriptors and release leases in reverse order if no mapping/session
ownership began. After a successful VSOCK accept, failures are treated as
session teardown, not as a fresh connect retry. Peer drain/abort/transport close
does not prove mapping release. Keep AV lock through compositor-buffer release,
producer quiescence and native mapping teardown; then destroy peer owner and
release leases. If those external completion facts cannot be established, retain
the lease and report failure rather than reconnecting or incrementing/reusing an
epoch. A daemon Running/Stopped status, guest-exec probe, successful LocalStop,
socket EOF, connection token or peer destroy is never an epoch-wide proof.

Local `session.sock` and `SO_PEERCRED` enforcement belong to the separate local
control server/daemon-control slice: this owner neither binds that socket nor
accepts LocalLaunch/LocalStop. The listener must authenticate same-user
credentials before protocol decoding and enforce `InteropFromLocal`. Guest
single-host-connection admission is guest-side and not established by the host
session lock.

## Tests

Use only isolated temporary trees and wrapped system calls; no VM, VSOCK device,
credential impersonation, GPU, or shared build. Verify port collision is
rejected before any directory/daemon operation; secure directory/file checks;
session-lock contention; AV-lock contention and exact ordering; reverse cleanup
on every failure; cancellation/deadline including EINTR; immediate connect and
EINPROGRESS/SO_ERROR outcomes; fd flags; actual CID/port target; retry-policy
tests at the caller/daemon integration boundary; socket-owner create success and
failure ownership; one successful peer admission per lease; no second connection
after accepted-fd/create failure; explicit lease retention through Draining and
release only after peer owner destruction plus caller-controlled teardown;
close error ownership; and no automatic epoch mutation. Link existing real peer
ABI/codec objects and reuse the independent socketpair suite for protocol bytes.
These tests prove local lease/connector mechanics only, not production VSOCK,
guest initialization, mapping validity, buffer release or VM acceptance.

The GuestReady daemon request/client, local `session.sock` peer credentials,
full Wayland loop, mapping implementation, manifest transaction layer, guest
single-connection gate, native runtime and acceptance matrix remain separate
work and receive no credit from this API alone.


## Lead corrections and exact implementation boundary

These corrections override the candidate wording above. Source/header/fixtures
may now be implemented; no progress is awarded until a real session entry uses
it and all local/CI/native gates for the corresponding allocation pass.

- Copy every retained device/version field during acquire. No borrowed config,
  device or agent_version pointer survives. Version is5..32 bytes in a fixed
  32-byte owner array; validate full SemVer using the existing Zig-backed
  interop_writer_init/interop_put_bytes(InteropTagAgentVersion) on a bounded64-
  byte local frame before filesystem/socket actions. Do not add a C parser or
  temporary peer allocation. Reject capability bits outside InteropCapabilityMask.
- Append statuses WrongThread=12, AvBusy=13, BadState=14, OsError=15,
  ClockError=16, AttemptConsumed=17. Busy=3 means session.lock contention only
  (runtime exit0); AvBusy is an actual conflicting AV command (runtime exit1).
  Preserve errno for OS failures. All public pointer ranges must be aligned,
  nonwrapping and disjoint from supplied output slots and known owner storage.
  Accessible authentic storage remains a caller precondition, not memory probing.
- Require selected config_valid=1, bounded NUL-terminated valid device name,
  state_dir/socket_path, CID>=3 except UINT32_MAX, and nonzero guest-exec port.
  Reject port5002 before allocation/any filesystem or daemon action. Derive
  state/socket paths with the existing device helpers and require equality with
  the selected profile. Snapshot derived paths during acquire; don't re-resolve
  mutable environment later. Caller obtains profile through the existing registry.
- Lease files are regular, exactly owner mode0600 (no special/group/other bits),
  same effective UID, link count1. Directories are same UID and private. Open
  with CLOEXEC/NOFOLLOW; lock session then AV. Verify the named inode still
  matches the acquired descriptor after flock; never unlink/replace lease files.
  Reuse the real component-walking directory helpers. Pre-connect failures close
  AV then session, invalidate ownership before each close and never retry EINTR.
- Caller-owned cancel_fd is -1 or an authentic stable pollable fd through one
  connect attempt; never read/write/close it. connect_timeout_ms is1..2000.
  Use a finite monotonic deadline and poll with remaining time, not a fresh
  timeout on EINTR. Admission timestamps are sampled only after completion;
  a clock failure after connection is consumed/fatal, never a new retry.
- A completed connection is consumed before socket-owner creation; all subsequent
  connect calls return AttemptConsumed without a socket operation. Timeout,
  cancellation or indeterminate error after issuing an in-progress connect is
  also consumed: the remote may have accepted. Retrying the same owner is allowed
  only after a definite pre-admission failure (socket creation failure, immediate
  ECONNREFUSED/ENETUNREACH/EHOSTUNREACH/ENODEV, or an equivalent completion
  refusal), never on ECONNRESET, ambiguous HUP, timeout or post-accept failure.
  Conservatively consume any unclassified post-connect outcome. Cancellation
  detected before issuing connect remains retryable. Caller retry policy must
  use these distinctions and daemon Running; no hidden automatic reconnect.
- socket_owner returns NULL on absent peer or wrong thread (errno=EPERM for
  wrong thread) without mutation. Its pointer is borrowed: callers may use
  serialized poll/drive/mapping/drain/abort, never destroy it through a copied
  slot. destroy_peer is the sole destructor. Unexpected cleanup failures retain
  the wrapper/peer for retry; release refuses while any peer allocation remains.
- Release requires caller-established external resource quiescence, not an
  accepted boolean. It frees only the wrapper and closes AV then session lease;
  close errors return CloseDiagnostic after all local ownership is relinquished.
  No mapping, epoch or shared backing is released here.
- Root owns actual waddle-session entry, daemon GuestReady/local control, build,
  docs and integration. Worker owns only this header/source and its fixture.
  An unused library cannot earn the connection/VSOCK/singleton10% allocation.
