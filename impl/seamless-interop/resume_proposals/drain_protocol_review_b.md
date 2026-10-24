# Critical review B: proposed protocol-v3 epoch drain

## Verdict

The proposal correctly rejects transport loss, `Goodbye`, shared `Free`, and
process restart as proof. Its normal orderly path can be made safe, but the
three-message exchange does not yet define a terminal agreement the host can
recover after a lost/partial `DrainComplete`. Durable epoch ownership and a
distinct-backing-object reset are stated as requirements, not mechanisms. As
written, crash recovery must remain fail-closed; no wire exchange can recover
proof that was lost with the process that observed it.

## Where the happy path is sound

If all of the following are true, exact per-frame `FrameRelease` plus a
completed epoch receipt is sufficient for the data-lifetime question:

1. `DrainReady` is completely received on the current connection and means the
   guest has closed admission, stopped/joined every writer, and flushed all
   earlier `FrameReady` frames on that same ordered stream.
2. Host processing has accounted for every earlier publication, classified
   never-attached frames, and observed actual release for every compositor
   buffer/import that could reference the BAR. A destroy request, `wl_display`
   sync alone, or local surface retirement is not enough.
3. The host sends each exact `FrameRelease` completely before
   `DrainComplete`; the guest parses the ordered stream, applies every exact
   release and verifies zero remaining pins before it accepts completion.
4. Neither side reuses or reinitializes the epoch until its local proof is
   recorded and the other side's receipt is known at the required boundary.

This is sufficient in an orderly, single-connection execution because
same-direction stream ordering closes the gap between per-frame releases and
the epoch receipt. The proposal should state these as formal preconditions and
make the native owner enforce them rather than treating a boolean/receipt as
self-authenticating evidence.

## Concrete failure gaps

**Lost or partial terminal receipt.** Host drains all buffers and fully sends
`DrainComplete`, then the connection drops before the guest receives it. Host
has local compositor proof plus the guest's earlier `DrainReady`, so it can
know the physical mapping is no longer read/written by either endpoint. The
guest still holds the old runtime/pins and cannot safely initialize a new
epoch. If host restarts and forgets the local completion or the new guest
connects directly, both can disagree about which epoch is reusable. The
proposal says to quarantine on lost receipts, but has no `CompleteAck` or
durable operation record by which a still-running host can learn that the
guest validated completion. Add a terminal guest acknowledgment; the host
must not authorize a new session/epoch on the same backing object until it
receives it. Guest may locally free old reservations after validating
`Complete`, but should not start a replacement epoch until the owning host
confirms/permits it. Any lost acknowledgment leaves host quarantine intact.

**Host Wayland loss.** A transport can remain healthy while the Wayland
display dies, or the Wayland display can die during `DrainBegin`. The host
cannot emit `DrainComplete` unless it had already observed all release
callbacks or can establish a stronger native detach proof. Reconnecting a new
Wayland client does not prove resources of the old connection were destroyed
and no longer referenced by the compositor. If the process loses the
per-buffer registry, it must retain quarantine; do not synthesize host-ready
from connection close or proxy destruction.

**Guest restart.** If the agent crashes before `DrainReady`, a new agent cannot
infer that earlier writers stopped merely from a new process or Hello. If
all writers are strictly contained in the terminated agent process, a trusted
supervisor could prove process death, but it still must drain old host buffers
and retain old epoch identity. If any writer, callback, service, or mapping
owner can survive that process, process death is insufficient too. The CNG
63-bit seed proposal is not restart-safe unless its owner and monotonic record
survive crashes/rollback and is bound to the backing object; uniqueness alone
still does not prove detach.

**Host restart / lost in-memory owner.** A native completion receipt received
before a crash is not useful after restart unless it was persisted with the
proof state. Persisting an operation ID alone is insufficient: a crash can
occur between a release callback and recording it, between recording and
receipt publication, or while replacing the journal. Recovery must treat
partial records as unknown, never as completed. If no independently durable
owner survives, the only safe recovery is an external reset whose authority
proves old producers are dead and every compositor reference is gone.

**Simultaneous begin.** Retaining one ID from each direction avoids unbounded
state but does not define what happens when one endpoint receives peer
`Begin`, has locally sent its own `Begin`, or loses one `Ready` in transit.
Different tie-break or duplicate rules can produce crossed completions, an
ID being retired while the other side still waits, or one peer entering a new
epoch while the other remains draining. The same ID also needs specified
retry semantics after partial send. Do not ship the symmetric state machine
until every crossed message ordering is specified and tested.

**Request ID lifecycle.** The candidate reserves an ID in a common request
owner, but does not define when Begin/Ready/Complete IDs retire, whether
duplicates are idempotent, or what a late receipt for a quarantined epoch
does. IDs must remain occupied until terminal agreement or permanent
quarantine; late/duplicate messages must not reopen or complete a newer epoch.

## Minimal safe versioned contract

The smallest protocol that is reviewable is **host-coordinated orderly drain**:

- A single `DrainBegin(id)` from host to guest closes the epoch to new work.
- Guest stops and joins producers, flushes prior publications, then sends
  `DrainReady(id)` only after every queued frame before it is fully written.
- Host processes earlier publications, drains real Wayland buffer releases,
  sends all exact `FrameRelease`s, and sends `DrainComplete(id)` last only
  after its local proof. Both sides bind every message to protocol-v3,
  current connection, and the exact nonzero epoch; complete writes and receive
  ordering are explicit.
- Guest accepts Complete only after processing the preceding release frames
  and confirming zero pins. It replies `DrainCompleteAck(id)`; host does not
  authorize a replacement epoch on that backing object before receiving the
  exact ack. Duplicates are idempotent only for the same live operation/epoch.
- Guest-originated orderly shutdown sends a separate `DrainRequest` (not an
  ambiguous symmetric Begin); host remains coordinator. A guest crash or
  unreachable host cannot finish this exchange and remains quarantined.
- Any partial send, peer loss, deadline expiry, unexpected ID/epoch, callback
  failure, or uncertain native resource transitions to a persistent
  Quarantined state. Do not reset mapping metadata or begin a fresh epoch.

This limits normal support to cases where both endpoint owners stay alive
through the handshake. It deliberately does not claim transparent recovery
after host/compositor failure.

## Crash recovery and backing-object reset

The proposal's requirement for a *distinct* BAR backing object is necessary,
but no current API identifies or verifies one. A process ID, a new random
epoch, a new driver handle, a VM reboot request, or a successful metadata
initializer is not an object identity/reset proof. The native layer needs a
trusted generation/identity from the VM/driver allocation owner, with a rule
that the old backing object is no longer mappable and has no compositor
imports. If the platform cannot provide this, crash recovery must remain
unsupported and operators must perform a separately verified reset sequence;
even then, old compositor buffer release needs evidence or compositor
termination ordered after resource destruction.

Any durable journal should record exact backing identity, epoch, operation ID,
per-side proof phases, and quarantine status with crash-consistent publication.
It may only move forward after the local evidence is observed; missing,
malformed, rolled-back, or ambiguous state means quarantine. A durable record
cannot recreate a missing Wayland callback or prove an external compositor
destroyed a resource, so the native owner must first have an actual proof source.

## Unsupported/fail-closed cases

Until the above is implemented and qualified, do not claim: automatic reinit
after VSOCK EOF; cleanup after host session or Wayland-process crash; automatic
recovery from guest-agent restart; VM reset safety without verifiable new
backing identity; simultaneous guest/host drain; or use of a timeout to finish
drain. Protocol v2 and the current layout remain unchanged. A v3 proposal
should not be accepted as complete until normal-order tests, every partial
send/drop boundary, crossed/duplicate operation behavior (if supported),
host Wayland loss, guest restart, and durable-state crash cuts are specified
and exercised.
