# Review: proposed protocol-v3 epoch drain

## Assessment

The two non-simultaneous sketches can be safe without a final receipt only if
`Ready`/`Complete` have strict one-way proof semantics and the receiver stays
quarantined when a required proof is lost. The proposal does not yet define
those semantics tightly enough to authorize metadata/audio/map reuse. Its
simultaneous case is not a state machine: it gives each direction an operation
ID but does not select the operation that wins, define which proof satisfies
which ID, or define when the losing ID is retired. This is the main blocker;
the three-message normal flows do not inherently deadlock, but their names and
message ownership make unsafe interpretations plausible.

## Concrete counterexamples and gaps

1. **Simultaneous initiation has no deterministic correlation.** H sends
   `DrainBegin(id_h)` while G independently sends `DrainBegin(id_g)`. The spec
   says both keep IDs and each sends `DrainReady` for the peer's Begin, then
   `DrainComplete` for its own. It does not say whether `DrainReady(id_h)` from
   G means “G producers stopped” or “H buffers drained”; the proposal uses
   those different meanings in the two ordinary directions. Nor does it say
   whether H may send `DrainComplete(id_h)` before it has received
   `DrainReady(id_h)`, or whether either side waits for a Complete that is
   itself waiting on the other ID. A crossed schedule can therefore leave each
   runtime holding a different active operation and waiting for the other
   `Complete`. A bounded two-ID table limits memory but does not resolve this
   state ambiguity.

2. **The `Complete` send is not a receipt.** In host-initiated flow, H can
   release its local buffers and send `DrainComplete(id_h)`, then the connection
   can close before G receives it. G must remain quarantined and must not
   reinitialize; H may already have unmapped. In guest-initiated flow, G
   receives host `DrainReady(id_g)`, may reinitialize, then sends
   `DrainComplete(id_g)`. If H requires receipt of that final message before
   it stops or unmaps, a lost Complete leaves H waiting while G has already
   reused metadata. If H does not require it, Complete is only diagnostic and
   its ID-release rule must say so. A complete transport write alone cannot
   prove peer consumption.

3. **Close after `Ready` is underspecified.** If H closes after sending
   `DrainReady(id_g)` but before G consumes it, G cannot reuse. If G closes
   after `DrainReady(id_h)` but before H consumes it, H cannot know guest
   producers stopped and must not release/reinitialize the mapping. Both cases
   must fail closed. The proposal says uncertain resources are preserved, but
   does not state which native owner retains them after the process or VM
   exits, nor whether an accepted message that was only queued counts as a
   proof. It must require complete-frame receive, correct epoch/ID, and local
   native completion before acting.

4. **Frame pins are only part of the mapping lifetime.** The host criteria
   correctly require exact `FrameRelease` for slot pins and completion of
   asynchronous Wayland imports/releases. They do not spell out the equivalent
   rule for audio: host audio-ring readers/workers must be stopped and joined,
   must hold no shared-memory references, and must cease heartbeat/descriptor
   reads before host readiness. A PipeWire buffer copied out of IVSHMEM can be
   drained or discarded by policy, but no reader may retain or revisit ring
   metadata after the proof. Guest readiness must stop and join every audio
   and frame writer, resolve Writing/provisional claims, and flush the last
   publications before its receipt.

5. **Map release is not stated as a prerequisite to host readiness.** “Stop
   metadata/heartbeat/map I/O” could mean merely pause reads while leaving an
   owner able to resume. To permit guest metadata reinitialization, host
   readiness must follow final local buffer/audio teardown and release of the
   host mapping (or a specified, irrevocable ownership state that cannot access
   it again). No host drain response may be sent before this point. The guest
   may not treat transport EOF, VM/process restart, `Goodbye`, or a shared Free
   state as a substitute.

6. **Request-ID lifetime is not specified for a multi-reply operation.** The
   proposal reserves one ID for Begin and echoes it in both Ready and Complete.
   It needs to state that Ready is nonterminal and does not release the
   initiator's request ID; Complete is terminal for that ID. The responder
   does not reserve the peer's ID in its own per-direction allocator. In a
   simultaneous drain the two directions may validly have the same numeric ID,
   so tracking must key on `(direction, id, session_epoch)` and the winning
   operation, not the number alone. Duplicate Begin/Ready/Complete must be
   idempotent only for an exact matching active operation; mismatched or late
   IDs must never authorize cleanup.

## Smaller safe state machine

Keep the versioned wire change, but make one epoch drain a single operation
with one primary ID and asymmetric proof messages. Preserve the old v2
protocol unchanged. Define these exact meanings:

- `DrainBegin(id)`: H→G requests guest quiescence. G→H is a guest-initiated
  drain and is sent only after G has stopped/joined all mapping writers; it
  carries the guest producer-stop proof.
- `DrainReady(id)`: G→H means guest producer-stop proof in response to a host
  Begin. H→G means host teardown is complete: all earlier guest publications
  were processed; every slot pin received exact release; async compositor/
  import users ended; all audio and metadata readers stopped/joined; host
  mapping access was irrevocably stopped and mapping released.
- `DrainComplete` is unnecessary. It cannot prove receipt and adds a terminal
  wait/ID lifetime without making either side safer. Use the request ID only
  for `DrainBegin` and its terminal host `DrainReady`; the initiator releases
  its request ID only after the matching terminal Ready (in the host-initiated
  flow, H retains the ID after the guest's intermediate Ready). A responder
  forgets the peer ID after fully sending its last Ready. A sender that does
  not receive terminal Ready stays quarantined. Sending Ready never proves it
  was received.

Flows:

1. **Host initiated:** H reserves `id_h`, closes its new work admission, sends
   `DrainBegin(id_h)`. G closes producer admission, stops/joins frame and audio
   writers, resolves any provisional Writing claims, flushes all final frame
   publications, and replies `DrainReady(id_h)` only after these facts hold.
   H processes the FIFO prefix through that receipt, releases every imported or
   submitted frame with exact `FrameRelease`, stops/joins audio and metadata
   readers, completes asynchronous compositor teardown, releases the mapping,
   then sends `DrainReady(id_h)`. G may retire its runtime and initialize a
   fresh epoch only after it receives the complete matching Ready and verifies
   all exact pins are gone. H can close after Ready is fully sent; if G misses
   it, G remains quarantined.

2. **Guest initiated:** G first performs the same full producer-stop sequence,
   reserves `id_g`, then sends `DrainBegin(id_g)`; in this direction Begin
   itself is the producer-stop proof. H drains all frame/audio/map users with
   the same host prerequisites and sends `DrainReady(id_g)` only after host
   mapping access is irrevocably stopped. G validates its exact pins and may
   then retire/reinitialize. There is no third message: H already received the
   guest producer-stop proof and has its own host proof. If G misses Ready, it
   quarantines; H need not infer receipt from close.

3. **Simultaneous initiation:** Host local event-loop order elects one primary
   operation. If H has already committed/sent its Begin when it receives
   `DrainBegin(id_g)`, `id_h` wins; G adopts `id_h` when it receives H's Begin,
   and sends `DrainReady(id_h)` as the producer-stop response. H treats G's
   Begin as the producer-stop proof, waits for the matching Ready if it has
   already sent its Begin, and sends terminal `DrainReady(id_h)` after host
   teardown.
   If H receives G's Begin before it has committed a local Begin, `id_g` wins;
   H folds any local drain trigger into that operation, sends no second Begin,
   and responds Ready for `id_g`. The loser ID is retired as merged once the
   peer's winning Begin is accepted; it never receives a second completion.
   Each endpoint stores at most one active ID. Duplicates of the winning Begin
   are idempotent; a conflicting late ID is a protocol error and cannot reopen
   or complete a drain. Host event-loop serialization makes the winner rule
   deterministic and bounds state to one operation.

For all flows, epoch equality, message direction, role, complete codec
validation, active phase, exact ID, and successful complete-frame send/receive
must be checked before state changes. The epoch remains the current negotiated
epoch throughout drain. Wrong/late IDs cannot authorize any native cleanup.
Close/EOF/timeout before the required peer receipt preserves quarantine and
resources. No new mapping initialization is permitted until local writers are
stopped, all local readers are gone, and the peer's required proof was actually
received; these protocol receipts remain claims by trusted endpoint code, not
proofs generated by the wire itself.

This proposal is smaller than three messages in each direction and removes the
unacknowledged `Complete` phase. It still requires native acceptance proving
the host can finish compositor/audio/map teardown while keeping the control
connection alive, and proving that an agent crash cannot erase the retained
quarantine/ownership state needed to prevent unsafe remapping.
