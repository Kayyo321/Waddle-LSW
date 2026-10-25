# Windows guest control-only session runtime contract

## Scope and explicit non-goals

`interop_guest_session_run` is one synchronous, single-threaded Windows guest
session attempt. It owns Winsock initialization, the Viosock family-query
handle, one native Viosock listener, at most one accepted socket owner, one
Windows mapping owner, and the temporary borrowed view used to initialize and
observe layout-v3 metadata. It joins the frozen `interop_windows_socket_t`,
`interop_mapping_windows_t`, `interop_mapping_initialize`, peer ABI, and native
heartbeat helpers. It performs no feature dispatch: local capabilities are
always zero, and any feature frame after Ready ends the control-only attempt.

The runtime is not a Viosock installer, listener service registration, signed
driver deployment path, UIAccess process, logon/autostart integration, host-CID
acceptance test, compositor integration, epoch-reuse service, or mapping detach
proof generator. The standalone executable has no user option that asserts
freshness or detachment. Without a trusted caller supplying fresh-exclusive
authorization, it exits before opening Viosock or touching a mapping.

## Public API and ownership

`interop_guest_session_config_t` is a caller-owned, aligned value containing:

| Field | Contract |
|---|---|
| `device_index` | Explicit nonnegative selected Windows mapping device index; no enumeration-based implicit choice. |
| `connection_token` | Nonzero peer callback token, not authentication. |
| `fresh_exclusive_authorized` | Exactly 1 only when a trusted caller independently established that this is a fresh mapping and no old producer/host users can be live. This is an assertion boundary, not evidence derived by this function. |
| `stop_event` | Borrowed caller-owned manual/auto-reset event handle; never closed, reset, or signaled here. NULL is permitted to disable caller stop. |
| `reserved_zero` | Must be zero. |

`interop_guest_session_owner_t` is opaque and noncopyable. The run operation
allocates it before acquiring native resources. Success with no retained
resources frees it and leaves `out_owner` NULL. If any cleanup cannot complete,
the function returns the authentic owner through `out_owner`; the caller must
retain it and call `interop_guest_session_retry_cleanup` on the creating thread.
The owner records the exact Winsock reference, family-query HANDLE, listener or
accepted SOCKET, frozen socket owner, mapping owner, borrowed mapping view
publication history, first operational result, and cleanup diagnostics. No
failed close silently clears a native value or frees a record that still owns
resources.

`interop_guest_session_retry_cleanup(owner_slot, host_detached_assertion)` is
valid only after the run call returned and all local mapping/socket users are
quiescent. The assertion must be exactly 0 or 1. Before any successful mapping
open, cleanup uses proof 0. After open publishes a view, cleanup with assertion
0 performs no map release and retains the owner. Assertion 1 means the caller,
using independent external evidence, asserts both local users have stopped and
the host has detached from the mapping. It is never inferred from EOF, VSOCK
close, peer Draining/Closed, timeout, stop, process death, or guest heartbeat.
If a socket close is pending, one explicit close retry may be attempted. An
indeterminate socket close remains owned or quarantined under the frozen socket
owner contract; Winsock is not cleaned up while that native socket is
unresolved. Mapping cleanup failures retain the authentic mapping owner for a
later retry. Outputs expose no secret or mapping bytes.

`interop_guest_session_quarantine_socket` transfers exact unresolved listener,
accepted, and/or session socket identities into the caller's all-zero
quarantine bundle. Indeterminate raw SOCKETs are never passed to `closesocket`
again; only an explicit pending close retains the documented retry path. The
bundle transfers each close's copied WSA error and the exact Winsock startup reference too, which must outlive
every unresolved SOCKET. The caller retains it until process exit or separately
documented native resolution, then calls
`interop_guest_session_release_quarantine` with an independently established
socket-resolution assertion. This releases only the Winsock reference and
never closes/retries a SOCKET. Quarantine transfers no mapping proof or
epoch-reuse authority.

If frozen-owner destruction returns `InteropWindowsSocketPeerFailure` after
transport close, the session retains that exact owner for explicit destroy
retry. A peer-notification failure is not classified as indeterminate native
close and never offers an already-closed SOCKET for quarantine.

`interop_guest_session_get_state` returns copied resource-presence and
diagnostic integers only. It never returns a borrowed mapping pointer, raw
HANDLE/SOCKET, host proof, or content snapshot. State reads are serialized on
the owner thread.

## Native listener admission

The runner validates all config/output ranges, checks the explicit
fresh-exclusive authorization before any driver/mapping operation, captures
`GetCurrentThreadId`, and starts Winsock 2.2. It opens `\\.\Viosock` with the
existing guest listener's read-only access and performs the existing family
`DeviceIoControl` query (`0x0801300c`). The result must be exactly one DWORD,
nonzero, and representable in the 16-bit family field. The query handle is
retained until `CloseHandle` succeeds.

The listener uses `WSASocketW(family, SOCK_STREAM, 0, ..., WSA_FLAG_NO_HANDLE_INHERIT)`.
It is explicitly nonblocking before bind. The Viosock address is the existing
12-byte native-endian value `{family, reserved=0, port=InteropControlPort,
cid=UINT32_MAX}`; no AF_INET, AF_UNIX, or fallback listener is created. `listen`
uses backlog 1. The accept deadline is absolute 30 seconds from listener
readiness. Stop is checked at least every 100 milliseconds using a zero-timeout
wait on the borrowed event plus bounded `WSAPoll`; no worker, callback,
overlapped operation, or blocking unbounded accept exists.

Only one authenticated-by-address peer is admitted, where “authenticated” here
means only the native address policy, not cryptographic identity. Each accepted
socket is explicitly made noninheritable and nonblocking. `getpeername` must
return the queried family, a full 12-byte Viosock address, reserved zero, and
CID exactly 2 (the configured host CID). A bad address is closed once and
rejected. The accepted socket transfers to `interop_windows_socket_create`
only on successful creation. No second accepted connection is retained or
queued. The listener is closed after the one accepted peer or before a clean
failure return.

## Mapping initialization and handshake

After valid admission, the runner creates a mapping owner and opens only the
explicit `device_index`. A successful mapping open publishes a borrowed view
and sets a sticky published-view bit. The runtime obtains a fresh nonzero
64-bit epoch using `BCryptGenRandom` with the system-preferred RNG. Failure or
zero output aborts before metadata writes. The trusted caller's
`fresh_exclusive_authorized == 1` is the only authority to call
`interop_mapping_initialize(base, length, epoch, 1)`. The initializer writes
only layout-v3 metadata bytes `[0, 0x4000)`; it does not establish external
quiescence or authorize epoch reuse.

Only a successful metadata initialization is followed by
`interop_windows_socket_mapping_result(..., epoch, InteropLayoutVersion, 1)`.
That call is the sole step that permits guest Hello to be queued. Handshake
frames remain entirely in the existing peer/socket owner. After the copied
Ready event, the runtime snapshots the immutable header with an ordinary-byte
copy into a private 4096-byte buffer, but loads each live atomic heartbeat
individually through `interop_heartbeat_load`. It validates the private
snapshot with `interop_layout_validate_shm_header` against the selected epoch
and exact mapping facts. It never `memcpy`s live atomic objects and never
touches descriptors, heap, audio rings, window slots, or pixels.

Capabilities are fixed to zero. An event carrying any feature frame is an
unexpected-feature failure; it is not dispatched and receives no synthetic
ACK. Ready alone grants no facility authority.

## Heartbeat and bounded session loop

The post-accept run deadline is absolute 120 seconds and is never extended by
progress. The loop checks the stop event and clock at entry and after every
wait, uses poll waits no longer than 100 milliseconds, and drives one fresh
socket-owner poll cookie exactly once. Socket-owner budgets remain the frozen
8192 RX bytes, 65536 TX bytes, 64 syscalls, and 64 internal steps per drive.

After Ready, the guest is sole writer of `InteropGuestHeartbeat`. Every 500
milliseconds it acquire/relaxed-loads the guest heartbeat with
`interop_heartbeat_load`, requires that value to equal the last value observed
or written by this guest, rejects `UINT64_MAX`, then stores exactly `observed +
1` with `interop_heartbeat_store_next`. The host heartbeat is a host-owned
concurrent field: the guest does not freeze or reject it. Heartbeat failure
stops feature admission and begins bounded transport drain; it is not a
mapping-detach signal.

On a stop request, session deadline, unexpected feature, protocol failure, or
peer close, the runner ends local admission and gives the socket owner its
finite 5-second drain interval. It continues bounded poll/drive until native
close, explicit close retry state, or drain expiry. The first operational
failure is preserved separately from cleanup diagnostics. No operation in
this phase asserts remote mapping detach.

Diagnostics preserve API return conventions rather than sampling unrelated
thread-local error slots. A nonzero `WSAStartup` return code is copied directly
to `last_wsa_error`; `WSAGetLastError` is not consulted for that failure.
`BCryptGenRandom` failure stores its exact 32-bit `NTSTATUS` bit pattern in
`last_ntstatus` and does not read `GetLastError`. A successful RNG call that
produces epoch zero, or a successful family query with an invalid family or
byte count, records deterministic `ERROR_INVALID_DATA` in `last_win32_error`.
For other Win32 diagnostics, the first value is retained across later cleanup
failures so teardown cannot overwrite the original cause.

## Failure and cleanup states

Every acquisition records the returned native object before the next fallible
operation. Cleanup attempts independent resources even after one failure but
never clears a failed resource field. A raw accepted SOCKET that has not
transferred remains in the controller until one successful `closesocket`; a
pending/indeterminate frozen socket owner remains in the controller. The
family HANDLE and listener SOCKET remain recorded if their close API fails.
Winsock startup remains active until every socket/listener/query-owned step
that requires it is resolved. An unresolved pending/indeterminate socket can
leave only through the caller-owned quarantine bundle, which carries the exact
`WSAStartup` reference until separate native resolution. `WSACleanup` never
runs while a quarantined socket may still be live.

Before view publication, mapping cleanup can use proof 0. After view
publication, the runtime is the only local mapping user and stops those users
before returning, but the host may still have mapped or cached the resource.
Without independent host-detached assertion the runtime intentionally returns
`InteropGuestSessionCleanupRequired` and retains the mapping owner. A later
retry with false proof still makes no release attempt. A true proof is a caller
assertion that is checked only for exact value 1 and forwarded to the frozen
mapping owner's cleanup contract; this module does not verify or manufacture
it. The epoch is never reset or reused by this API.

## Threading and memory model

The creating Windows thread is the sole owner of Winsock, Viosock query/listener,
accepted peer, mapping, and socket-owner calls. There are no locks because no
cross-thread access is allowed. The borrowed stop event is only polled and is
never modified or closed. The mapping view is borrowed only between successful
open and the first valid close attempt; it is not exposed from this API. Header
ordinary bytes are copied only after Ready selects the epoch. The two atomic
heartbeat objects are accessed only with their native atomic helpers. The host
heartbeat may change concurrently and is not compared against a stale snapshot.

## Verification requirements

The Linux controller fixture is a negative unit fixture: it compiles the exact
production controller against narrow WinSock/Win32/mapping doubles to exercise
deterministic failure and retry paths. Its fabricated peer and mapping results
are not positive protocol, validation, or runtime evidence. Production line
and taken-arc coverage must each be at least 90%; ASan, LSan with leak
detection enabled, and UBSan are fail-fast.

The separate native Windows composition fixture includes the exact production
controller and mapping owner with only Viosock discovery/address translation
and SetupAPI/driver calls substituted. It joins the production connected
socket owner, real host/guest peer ABI, actual Zig layout validator, native
atomic helpers, and mapping initializer. The mapping seam reserves and commits
a real 2 GiB `VirtualAlloc` range and releases it through the authentic mapping
owner's proof-gated close path. A second thread owns a real AF_INET loopback
host socket and host-role peer; it validates the guest's initialized header
before sending its real Ack. Cases cover successful Hello/Ack/Ready and guest
heartbeat, EOF before Ready, and handshake timeout before Ready, followed by
joined local users and explicit cleanup. This fixture does not qualify an
installed Viosock driver or native VSOCK family/CID policy, production
fresh-exclusive authorization, independent host-detach evidence, signed
installation, or autostart. Those remain separate acceptance gates. Native
Windows CI execution is required before this composition evidence is accepted.
