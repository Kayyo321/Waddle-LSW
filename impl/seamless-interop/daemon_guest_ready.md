# GuestReady daemon runtime boundary

This normative extension implements IMPL_DESC §3.4 without probing the interop
port on behalf of the host session. Request0x10 carries one little-endian u32
milliseconds (0 means120000; otherwise1..120000). Response0x11 carries an i32
status and u32 configured CID. Only0, -ETIMEDOUT and -ENOTCONN are valid statuses.
A success means a completed connection to the configured legacy guest-exec
port, immediately closed; it gives no protocol, mapping or epoch authority.
An invalid request gets the existing correlated ErrorResp/EINVAL.

All new untrusted header/readiness decoding is allocation-free Zig. Decoders
require exact byte lengths, validate every field before committing caller-owned
outputs, and retain no pointers. The 16-byte header stays version1 and uses
little-endian magic0x57414444, version1, type, sequence and payload length≤65536.
Unknown types remain valid framing and get the existing ENOTSUP dispatcher.
GuestReady response CID must be3..UINT32_MAX-1; signed status is validated.

Each daemon client has one fixed receive frame and one fixed transmit frame.
Only one request is in flight on a connection. Header and body offsets survive
short reads/EAGAIN; reads never consume a following request. A pending reply or
GuestReady wait disables further request decoding for that client. Writes use
nonblocking send/MSG_NOSIGNAL; short writes retain their offset. A full server
closes a newly accepted fd without a blocking error write. All accepted sockets
are NONBLOCK/CLOEXEC and same-effective-UID SO_PEERCRED is checked before bytes
are decoded. Up to16 clients consume bounded heap storage, freed on teardown.
The existing synchronous Start/Stop dispatch remains synchronous; GuestReady
itself introduces no wait/sleep into that dispatcher.

Each readiness wait owns at most one NONBLOCK/CLOEXEC AF_VSOCK probe descriptor.
The creating event-loop thread owns init/start/step/cancel. A monotonic absolute
request deadline and a maximum2000ms individual connect deadline bound probes.
Refused/failed probes close once and retry no sooner than250ms. Running state
must still hold on every step; any other state cancels with -ENOTCONN. Deadline
expiry returns -ETIMEDOUT. CID/port are immutable snapshots from daemon config;
there is no fixed5242 replacement. A successful connect or SO_ERROR=0/POLLOUT
closes the probe before reporting0. POLLERR/HUP/NVAL is failure, not readiness.
Disconnect/client shutdown closes a pending probe once. No probe fd is handed
to an interop owner. Clock failure closes affected clients and pending probes.

The client helper uses one total monotonic deadline (requested duration, with
zero selecting120000ms, plus2000ms bounded IPC allowance) for request write and
response read, validates exact type/sequence/payload length and Zig-decoded
status/CID before committing output, reports old-daemon ENOTSUP, and leaves the
borrowed fd open. A partial transport failure requires its caller to close that
connection; the helper never retries a transaction on a desynchronized stream.

Tests must cover byte fragmentation/coalescing, exact bounds and output
preservation, all probe completion/failure/deadline/disconnect paths, two-client
responsiveness while one waits, and real socketpair request/reply correlation.
New parsers and state machines require≥90% production line and branch coverage,
leak-enabled sanitizer runs and zero allocator leaks. No native credit follows
from mocked probes. The actual waddle-session entry and guest integration remain
separate runtime work.

The legacy ErrorResp decoder requires exactly264 bytes and errno1..4095, leaving
its diagnostic tail opaque. A final nonblocking read that finishes at or beyond
the shared deadline is rejected before committing the response. Clock failure
in the daemon cancels clients/probes and keeps the VM/supervisor alive; it must
not exit through daemon_state_cleanup. A queued idle Shutdown response disables
new request dispatch until its complete nonblocking write, preventing an
intervening Start from changing the shutdown precondition.
