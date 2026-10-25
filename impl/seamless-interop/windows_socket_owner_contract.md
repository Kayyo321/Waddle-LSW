# Windows connected socket owner contract

## Scope and boundary

`interop_windows_socket_t` is a serialized, pull-driven owner for one connected WinSock `SOCK_STREAM` and one existing `interop_peer_t`. This slice owns copied byte staging, protocol-peer calls, timer advancement, and the lifetime transition of the accepted socket. It does not create a listener, connect a VSOCK endpoint, own or initialize `interop_mapping_windows_t`, perform compositor work, ACK a feature frame on behalf of a caller, or prove guest/host resource release. The later listener/session wrapper is responsible for accepting the correct native transport and coordinating the mapping owner.

The peer ABI remains the sole protocol state machine. All messages, version checks, role transitions, feature-frame copying, and token checks remain in `interop_peer_*`. The owner never invents a wire ACK. It acknowledges only bytes that `send` reports accepted, once per positive send, before asking the peer for further output.

## Native socket preconditions and ownership transfer

The caller passes an already connected, nonblocking `SOCK_STREAM` `SOCKET`, created after successful `WSAStartup`. That startup lifetime must outlive the owner and any caller-owned quarantine record. The socket must not be overlapped, have an outstanding operation, be registered with `WSAAsyncSelect` or `WSAEventSelect`, be used by callbacks, or be accessed concurrently. The creating thread remains the only thread allowed to call this owner or use the socket until close succeeds or quarantine handoff completes. These are caller preconditions: this synchronous owner does not keep callbacks, event registrations, overlapped state, or thread handles.

Before ownership transfer, `create` checks the output/config ranges, verifies `SO_TYPE == SOCK_STREAM` with an exact returned option length, requires `getpeername` to succeed with a returned address length between the address-family field size and `sizeof(sockaddr_storage)`, checks `GetHandleInformation` and rejects an inheritable socket, and reads `SO_LINGER` requiring `l_onoff == 0`. It allocates the owner and creates the peer before the final socket-mode operation. It then calls `ioctlsocket(FIONBIO, 1)` as the final fallible setup operation and publishes the owner only on success. `FIONBIO` is a setter; WinSock provides no query for the prior mode. Therefore the already-nonblocking input is a precondition and setup failure does not promise to restore an unknowable previous mode. The caller retains socket ownership on every failure, and the owner never changes the inherit flag.

On success the owner retains the exact numeric `SOCKET`, peer allocation, copied token, role, start timestamp, captured `GetCurrentThreadId`, fixed RX/TX arrays, timer state, poll-cookie generation, and copied terminal event. No caller pointer is retained. Owner allocations use `calloc`/`free`; no per-drive allocation occurs.

## API and limits

The public owner has ten operations: create, poll-interest snapshot, drive, mapping-result, begin-drain, abort, explicit close retry, quarantine handoff, closed snapshot, and destroy. The output structs are copied values. Every output range is aligned, nonwrapping, and checked for overlap with owner storage and sibling outputs before state changes. On argument, thread, token, time, phase, or stale-cookie errors, caller outputs remain byte-for-byte unchanged.

Each drive accepts at most 8192 RX bytes, 65536 TX bytes, 64 combined `recv`/`send` calls, and 64 peer/helper operations. RX carry is exactly 8192 bytes. The peer ABI assembles partial protocol frames; the owner reads one complete outward event/frame per drive into a caller-owned 65536-byte frame buffer. Partial sends are copied into a fixed 65536-byte owner TX array and resumed on later drives. A successful `send` count is ACKed immediately and exactly before another send call. WSAEINTR consumes one syscall attempt and may retry only while budget remains; WSAEWOULDBLOCK ends that direction for the current drive. Fatal errors close admission and initiate exactly one native close attempt.

`poll_events` returns only POLLRDNORM/POLLWRNORM input interests; error/hangup/invalid status is accepted in returned revents but is never requested. Microsoft WSAPoll rejects output-only error flags as input with WSAEINVAL.

`poll_events` performs no I/O or wait. It returns the current socket identity, WSAPoll interests, a timeout from zero through 100 milliseconds, an immediate-work bit, and a fresh nonzero single-use cookie. The caller invokes `WSAPoll` and submits its `revents` to one `drive`. Any successful owner mutation invalidates the previous cookie. The caller supplies nondecreasing monotonic milliseconds; timer-driving calls occur at least every 100ms. The first transition to Draining fixes a saturating absolute deadline from `drain_timeout_ms` in `[1, 60000]`; later calls cannot extend it.

## Handshake and mapping gate

Host peers begin Active and await Hello. The peer can produce `InteropPeerEventValidateMapping` only after receiving the complete peer frame. The owner copies that event into caller storage and enters MappingWait. Only the outer wrapper may validate the actual mapping and call `mapping_result`; failure is forwarded to the peer state machine and may begin drain.

Guest peers begin in MappingWait/`InteropPeerInitializingMapping`. The outer wrapper first initializes the mapping owner and completes the required guest mapping initialization, then calls `mapping_result` with the peer epoch and layout. Only a successful peer transition permits Hello to be queued. The socket owner does not create, initialize, validate, publish, release, or reset a map. No mapping proof, drain proof, or epoch-reuse permission is implied by any socket state or event.

## Poll, drive, and drain sequence

1. The caller prepares a valid copied `interop_windows_socket_config_t` and already-connected socket under the preconditions above.
2. `create` validates inputs and native socket properties, allocates owner and peer, sets nonblocking mode, captures the calling thread id, and transfers ownership at the final publication.
3. The event loop calls `poll_events(now_ms)`, waits using caller-owned WSAPoll, and calls `drive` once with the same cookie and revents. It may instead call drive immediately when `immediate_work` is one.
4. Drive first checks thread, token, time, cookie, ranges, budgets, and revents. It advances the peer timer, services buffered RX, performs a bounded number of reads, copies one outward event/frame at most, and performs bounded partial writes with exact peer ACKs.
5. A mapping event moves to MappingWait until the outer wrapper calls `mapping_result`. No network data is dispatched into the peer while mapping is pending.
6. On a local stop or peer Draining phase, the owner stops admission, fixes one deadline, and flushes only already-queued peer output. Empty queue closes the transport; deadline expiry closes it. This is a local transport lifecycle only.
7. EOF, a fatal socket/poll result, peer abort, drain completion/deadline, or explicit destroy initiates one `closesocket` attempt. Only a successful `closesocket` clears the socket field and permits one `interop_peer_transport_lost` notification. If the current drive has exhausted its helper-step budget, notification is deferred to a later poll or destroy call; the later notification consumes no peer event and cannot replay a terminal event.

## WinSock close semantics and unresolved ownership

The owner requires SO_LINGER disabled at create and the caller must not mutate socket options or use the socket after transfer. Microsoft documents `WSAEWOULDBLOCK` as the special nonblocking close failure for nonzero linger timeout; in that documented case the handle remains valid and another `closesocket` is required. The normal owner precondition excludes that configuration, but the error is handled safely if a provider reports it.

On close success, the owner clears its stored `SOCKET`, records TransportClosed, then notifies the peer exactly once. On `WSAEWOULDBLOCK`, it retains the exact socket and peer, closes admission, enters ClosePendingState, and does not notify transport loss. The caller may explicitly invoke `retry_close`; every retry is one new bounded `closesocket` call, never an automatic loop. Success then performs the normal notification; another `WSAEWOULDBLOCK` preserves pending state. Any other `closesocket` error enters CloseIndeterminateState. Since the WinSock contract does not establish whether that socket handle remains valid for these errors, the owner does not retry, clear the handle, notify transport loss, or free the owner.

For either pending classification, the caller can use `quarantine_handoff` after owner-thread quiescence. Its caller-owned output must initially be an all-zero record. Handoff copies only the exact numeric socket value, WSA error, close classification, captured thread id, and connection token to that record; destroys peer allocations; frees the owner; and NULLs the owner slot. Handoff does not call `closesocket`, authorize another thread to close/reuse the numeric handle, assert that the handle is live, notify transport loss, establish mapping detach, drain, or epoch-reuse proof. The caller retains the record for the remaining process lifetime or until an external native resolution establishes what happened. This explicit quarantine transfer avoids an unreachable heap allocation while preserving unresolved native ownership.

## Threading and concurrency

The owner captures `GetCurrentThreadId()` at create. Every owner operation except no-op NULL-slot destroy checks the exact thread id before consulting mutable peer/socket state. One owner has no internal locks: callers serialize every call and every `WSAPoll`/socket access. The caller does not issue callbacks, overlapped operations, or concurrent WinSock calls. This precondition makes owner return a local quiescence point for synchronous RX/TX and permits safe peer destruction during quarantine handoff. No cross-thread callback quiescence or remote mapping quiescence is inferred.

All peer output is copied field-by-field into zeroed `interop_peer_event_t` storage, including reserved bytes. Frame bytes are written only to the caller's output after preflight. Terminal diagnostics are stable copied values. The WSA error is captured immediately after the failing native call and stored as an integer; the owner never stores a pointer to thread-local last-error state and does not use `errno` as a WinSock diagnostic.

## Errors and recovery

Invalid pointer/range/alias, unsupported scalar, stale token/thread/time/cookie, or wrong phase does not mutate operation outputs. Peer ABI invariant errors stop admission and initiate native close. Fatal recv/send/poll errors capture `WSAGetLastError` before any other WinSock call and become the primary terminal diagnostic. The work record preserves that first terminal WSA error separately from `close_wsa_error`, which carries the most recent `closesocket` diagnostic. `GetHandleInformation`, socket-type, peer, linger, and final FIONBIO failures leave socket ownership with the caller; allocated peer/owner memory is unwound before return. `closesocket` errors follow the pending/indeterminate rules above. `retry_close` is legal only in ClosePendingState after WSAEWOULDBLOCK; it preserves the original terminal error while updating the close-specific diagnostic. `quarantine_handoff` is legal only for the two unresolved states, requires the owner thread, exact token, zero-initialized output, and disjoint output, and performs no native close. `destroy` returns without discarding an unresolved handle; caller must retry explicitly or hand off quarantine.

## Verification criteria

The Linux fixture compiles the exact production source against a narrow WinSock test adapter and injects allocation, socket query, mode setup, read/write, poll-error query, close, and peer-ABI failures. It uses real connected POSIX loopback/socketpair descriptors where the adapter can faithfully represent synchronous stream semantics. Tests cover partial frames, partial send ACK ordering, exact budgets, mapping wait, thread/token/time/cookie guards, output aliasing, close retry, indeterminate quarantine handoff, exact handle preservation, and descriptor reuse. Production source line and branch coverage must both be at least 90%, and tests run under AddressSanitizer, LeakSanitizer, and UndefinedBehaviorSanitizer with leak detection enabled.

Acceptance requires Windows CI to run and pass real AF_INET loopback sessions through WinSock, verifying native connected-stream, noninheritance, nonblocking, poll, close, and thread-owner behavior. Those tests do not qualify an AF_VSOCK driver, listener, host CID policy, or VSOCK endpoint acceptance. No VM or guest credentials are needed for this unit.

## Primary WinSock references

- [`closesocket`](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-closesocket): nonblocking `WSAEWOULDBLOCK` with nonzero linger timeout leaves the handle valid; socket values can be reused immediately after successful close.
- [Socket IOCTL opcode summary](https://learn.microsoft.com/en-us/windows/win32/winsock/summary-of-socket-ioctl-opcodes-2): FIONBIO enables/disables nonblocking mode and has no output query.
- [`ioctlsocket`](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-ioctlsocket): FIONBIO is a mode-setting operation.

- [WSAPoll](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsapoll): request only provider-supported input interests; error status is returned automatically.
