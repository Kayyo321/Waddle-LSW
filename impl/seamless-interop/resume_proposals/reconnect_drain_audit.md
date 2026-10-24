# Reconnect and drain proof audit

## Finding

There is no existing integrated mechanism that can currently establish both
producer-stop and host-drained proof for a seamless-interop epoch after a
transport crash. The slot runtime deliberately accepts these as caller proofs;
it does not inspect threads, compositor objects, sockets, or mapping state.
The handshake machine also cannot supply either proof. Therefore a reconnect
must remain quarantined unless a native session owner can establish the two
conditions independently. Treating EOF, timeout, `Goodbye`, `Closed`, a shared
descriptor becoming `Free`, or a `wl_buffer_destroy` request as equivalent
proof would permit reuse while the compositor may still read the old bytes.

## What existing code does prove

The frozen v3 descriptor state is `Free -> Writing -> Ready -> Consuming ->
Free`. The last shared transition only records that the host stopped its
current shared-memory access protocol; it does not clear the guest runtime's
pin. `interop_slot_runtime_t.release_frame` clears one pin only for a validated
exact session/window/generation/slot/sequence `FrameRelease`. Its
`begin_drain` requires an explicit caller assertion that all producers have
stopped and rejects outstanding provisional writers. `confirm_host_drained`
requires Draining plus a separate caller assertion, then releases all
reservations. `interop_slot_runtime_abi.zig` forwards these booleans; it does
not authenticate them. See `IMPL_DESC.md` §8.9 and §8.10.

For a normally operating Wayland connection, the usable host-side evidence is
the `wl_buffer.release` callback for each submitted buffer. `av_wayland.c` sets
`busy` on attach and clears it in `buffer_release` after `av_video_release`
performs `Consuming -> Free`; its API docs explicitly say frames release
ownership only on that callback. Unattached/rejected imports can release their
slot locally because they were never submitted for compositor use. This is a
per-frame path, not an epoch-drain owner.

The existing AV teardown is not sufficient as a drain proof. `drain_buffers`
retires surfaces, dispatches for at most two seconds, and exits early on clock,
poll, display or timeout errors. `av_wayland_free` then calls `free_window`,
which destroys buffer proxies and disconnects regardless of remaining `busy`
buffers. Destroying a client-side `wl_buffer` proxy is not evidence that the
compositor released the underlying buffer. Once the display is disconnected,
no further release callbacks can be observed. The AV module is also a legacy
implementation, not an integrated seamless session owner.

`interop_mapping_initialize` only validates its arguments and a caller-provided
`exclusive_owner_proof`; with proof `1` it zeros and rebuilds the first 0x4000
metadata bytes, including all descriptors, while leaving pixel heap bytes
untouched. It cannot establish producer stop, host detach, epoch freshness, or
exclusive mapping ownership itself. Calling it on a reconnect while an old
host buffer references the heap could make an old pinned slot appear free and
allow its bytes to be overwritten. The raw Windows mapper owns the driver
lease only; its `detached_proof` is also an assertion, not evidence, and EOF is
explicitly excluded by its header and §8.18.

## Normal stop versus crash

For orderly stop, an owner could prove producer quiescence only after the guest
capture/session owner has closed admission, cancelled or completed every
provisional writer, stopped and joined every capture producer for the epoch,
and retained that result as epoch-scoped state. No current endpoint/runtime
adapter performs this orchestration. On the host, the owner must stop admitting
`FrameReady`, retire every surface, keep the Wayland display/event thread alive,
and observe completion of every outstanding `wl_buffer.release` (or establish
that a buffer was never attached) before asserting host-drained. It must also
resolve pending dma-buf import callbacks and include any buffer object the
compositor could have accepted. The existing two-second best-effort loop is
not enough if any operation exits early or any `busy` bit remains set.

After transport loss, neither side may infer the other side's status. A still
running host may continue the Wayland drain locally and report its local proof
to a supervisor; a guest may stop and join its own producers locally. But the
lost channel cannot carry new per-frame releases, and current frozen controls
provide no new drain acknowledgement. If the host process or its Wayland
connection dies before all releases are observed, host-drained is unknown.
Likewise, if the guest agent crashes, process termination can prove its
threads no longer run only if a trusted owner controls/observes that process;
it says nothing about compositor buffers. Keep the old mapping/runtime
quarantined in either uncertain case. A process restart or a new handshake is
not a reason to clear descriptors.

## Safe reconnect shape and missing native owner

The safe shape is an epoch owner that outlives individual transport
connections. A readiness probe may inspect a validated existing header but
must not initialize or reset it. While an epoch is Active, every connection
uses the existing mapping/runtime and cannot create a second producer set. On
disconnect, the owner enters Quarantined/Draining, blocks publication and
reinitialization, and preserves the old runtime, allocation reservations and
all known outstanding frame identities. A fresh epoch and fresh runtime may
be created only after the old guest producer set is stopped/joined and either
all exact releases have been observed or an independently trustworthy host
owner has proved every old buffer destroyed. Only then may the mapping-init
primitive run.

The missing implementation is a native lifecycle owner joining the guest
producer controller, `interop_slot_runtime`/ABI, host Wayland buffer registry,
transport/session endpoint, and raw mapping lease. It needs an explicit
epoch-scoped state machine and durable or supervisor-retained quarantine
state across reconnects; `session_handshake.teardown_complete` is not that
state. It must track submitted buffer identities through their actual release
callbacks, distinguish unattached imports from submitted buffers, and expose
proof to `confirm_host_drained` only after the evidence above. Because the
frozen contract adds no wire acknowledgement, the proof should be assembled
from local owners on both sides under a defined shutdown operation; if one
side becomes unreachable before proving its condition, the owner must retain
quarantine and refuse a fresh epoch. If product requirements demand recovery
after a host crash without waiting for a trusted external compositor/VM reset,
the contract needs a separately reviewed mechanism; no existing message or
boolean makes that safe.

Relevant frozen constraints: §4.7's legacy prose saying the guest keeps slots
free on disconnect is superseded for reuse safety by §8.9/§8.14, which
explicitly reject transport-loss and `Goodbye` as drain evidence. §8.11 is a
quiescent initializer, not a recovery mechanism.
