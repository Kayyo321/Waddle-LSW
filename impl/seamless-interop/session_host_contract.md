# Host session admission contract

## Purpose and boundary

`session_host` is the Linux host-side admission entry that joins the existing per-device singleton leases, local daemon readiness, the configured guest-exec readiness probe, and the one-shot interop VSOCK connector. It returns only after the existing host peer/socket owner has taken ownership of one connected interop stream. It does not create or release a shared-memory mapping, choose or reset an epoch, touch Wayland, or claim teardown proof.

The implementation is additive and confined to `src/interop/session_host.c` and `.h`, with focused fixtures in `tests/interop/session_host*`. The root owns build wiring and any CLI or daemon integration. The caller still owns the local daemon control descriptor and must serialize all use of that stream while admission runs.

## API shape

```c
typedef struct interop_host_session_config_t {
    const device_info_t *device;
    int daemon_fd;
    const uint8_t *agent_version;
    size_t agent_version_bytes;
    uint64_t connection_token;
    uint64_t local_capabilities;
    uint64_t deadline_ms;       /* absolute CLOCK_MONOTONIC milliseconds */
    int cancel_fd;              /* -1 disables cancellation; borrowed, pollable */
    uint32_t guest_ready_timeout_ms;
    uint32_t drain_timeout_ms;
    uint32_t reserved_zero;
} interop_host_session_config_t;

interop_host_session_result_t interop_host_session_admit(
    const interop_host_session_config_t *config,
    interop_linux_session_owner_t **out_owner,
    uint32_t *out_control_channel_invalidated);
```

The owner output slot must be aligned, writable, NULL-valued, and disjoint from every input range. The invalidation output is a disjoint aligned writable u32 and is initialized to zero after all argument preflight succeeds. If an interrupted or malformed local control transaction leaves a partial request/reply or unconsumed reply in the stream, it is set to one; the caller must close that borrowed stream before reuse. The function copies all owner-retained bytes through `interop_linux_session_acquire`; it retains no caller pointers or descriptors. On success, the returned existing session owner owns both singleton leases and the connected socket owner. The owner remains confined to the calling pthread and is accessed through `interop_linux_session_socket_owner`, `interop_linux_session_destroy_peer`, and `interop_linux_session_release`. The caller continues to own `daemon_fd` and `cancel_fd`.

The API has a distinct result enum for local-control/readiness failures and preserves the underlying `errno`. It reports invalid input, allocation failure, session or AV contention, cancelled, timed out, daemon transport/protocol failure, daemon not Running, guest-exec unavailable, selected-device/status CID mismatch, and the underlying one-shot session connector outcomes. Exact numeric values are local C API values, not wire values. `out_owner` remains NULL on ordinary failures, but may receive an authentic owner with `HostSessionCleanupRequired` when lease or peer cleanup fails; the caller must retry cleanup through the existing owner API. The invalidation output is initialized after argument preflight and may be set on a failed control transaction.

## Admission sequence

1. Validate every top-level and nested pointer range, alignment, overlap, string termination, scalar bound, reserved field, deadline, cancellation descriptor convention, and output state before side effects.
2. Reject an already-signaled cancellation fd and an expired deadline before filesystem work.
3. Verify `daemon_fd` is a connected AF_UNIX SOCK_STREAM with `SO_PEERCRED.uid == geteuid()` before any reads or writes. The caller serializes its use for the whole call.
4. Acquire the existing session and AV singleton leases using `interop_linux_session_acquire`. This serializes concurrent admission for the selected device before readiness checks.
5. Send one StatusReq and receive one exact StatusResp using nonblocking `send(MSG_DONTWAIT|MSG_NOSIGNAL)`, `recv(MSG_DONTWAIT)`, and `poll` over the daemon and cancellation descriptors. Both operations share the single absolute monotonic deadline. Validate framing through the existing Zig daemon-wire header decoder, exact response type/sequence/body length, and the Zig status-body decoder. Require `SubsystemStateRunning`, matching VSOCK CID, and a guest-exec port equal to `device->vsock_port`. Status is observational; it never starts/stops a VM.
6. Issue one GuestReady request on the same serialized channel, with its timeout capped by the configured limit and the remaining absolute deadline. Receive its one correlated exact response using the same bounded/cancellable I/O loop. Decode response/error payloads through existing Zig daemon-wire decoders. Require status zero and matching CID. GuestReady probes the configured guest-exec port only; the existing Linux session owner connects the interop peer to reserved port 5002. Neither probe is interop handshake or mapping proof.
7. Recheck cancellation and the absolute monotonic deadline. Compute remaining time and call `interop_linux_session_connect_peer` exactly once, with a connect timeout in `[1, 2000]` ms and the borrowed cancellation fd.
8. Publish the existing session owner only after successful VSOCK connection and socket-owner creation. On any earlier failure, release the leases and leave the output NULL. On any connector failure, do not retry within this entry point; the underlying owner distinguishes definite refusal from consumed/ambiguous outcomes.

## Deadline and cancellation semantics

`deadline_ms` is one absolute monotonic deadline and is never extended. The added control exchange does not call blocking client helpers. Every send/receive/poll loop includes the cancellation descriptor (when enabled) and is bounded by the same deadline; deadline and cancellation are rechecked after transport and decoder boundaries before advancing or publishing an owner. GuestReady timeout is set to the smaller of its configured value and remaining time; if no positive interval remains, admission fails before sending. Cancellation after any request byte or before the full correlated response is consumed marks the borrowed control channel invalidated. Cancellation during VSOCK connect is passed to the existing owner, which closes its candidate descriptor and records consumed/ambiguous attempts according to its current contract. If a late deadline is detected after peer creation, cleanup is attempted; if cleanup cannot complete, the authentic owner is returned with `HostSessionCleanupRequired`.

## Failure cleanup and recovery

Before a peer owner exists, failed admission closes no caller-owned descriptor and releases the acquired singleton locks through the existing owner release API. A failed or ambiguous interop connect is never retried by this wrapper. If a connected descriptor reached peer creation and that creation failed, the existing connector closes it before returning and retains the attempt-consumed state; wrapper cleanup then releases the now-empty session owner. A release close diagnostic is surfaced when there is no earlier failure; otherwise the original admission error remains primary and `errno` is restored.

After success, this API never releases either lease automatically. A future native session teardown must quiesce callers and prove its own mapping/window obligations before calling peer destruction and lease release. Stream closure, EOF, cancellation, Goodbye, and daemon shutdown do not prove shared memory or Wayland resources are detached.

## Verification boundary

Fixtures use a real AF_UNIX stream socketpair and an AF_UNIX daemon control server that exchanges Status and GuestReady frames. The VSOCK syscall seam creates a real socketpair-backed stream and asserts exactly one interop connect attempt to reserved port 5002; this checks host orchestration without claiming a live Windows guest or AF_VSOCK acceptance. Current cases cover successful admission/release, one-shot VSOCK refusal, non-Running state, status CID/guest-exec-port mismatch, GuestReady unavailable/CID mismatch, malformed status and ready framing, wrong response sequence, valid daemon ErrorResp, partial response, cancellation/deadline during local exchange, pre-cancellation, expired deadline, rejected peer UID, common invalid configuration fields, clock failure, retryable send/receive progress, poll EINTR/EIO/POLLNVAL, and post-connect late deadline with recoverable cleanup ownership. The full fixture passed locally; its AF_UNIX and descriptor operations used native pass-through behavior, with EPERM fallback branches present only for restricted environments. The latest isolated production-object run reached 99.17% line and 90.91% taken-arc coverage; the root-owned integrated gate must rerun against the frozen inputs. Sanitizer and leak qualification are being rerun by the root. Native installed-session acceptance, real AF_VSOCK acceptance, session/AV contention variants, and the missing shared-memory/Wayland mapping lifecycle remain open.

## Required root integration

The root must add the new C source and test target to `GNUmakefile`, link daemon wire Zig code, existing Linux socket/session owners and the imported Zig peer/codec objects, add any exercised test prerequisite to the existing interop gates, and decide where the selected device and connected daemon control fd originate in the product CLI/session path. `device_info_t.vsock_port` is the configured guest-exec port (normally 5242); the existing Linux session owner independently dials reserved interop port 5002. Those shared files are intentionally outside this proposal's ownership boundary.

## Root qualification

Reviewed source and fixtures pass the normal make target and fail-fast
ASan/LSan/UBSan with detect_leaks=1. Root's retained exact production gate
run-20261025T152619-4dbbae7b covers359/362lines and341/374taken branch arcs
(99.17%/91.18%), without source exclusions. Evidence/input hashes are saved in
session_host_evidence_2026_10_25. This qualifies the admission component; actual
`waddle-session` entry, Hello/live-mapping composition and local control commands
remain separate integration gates. No installed guest or native VSOCK credit.
