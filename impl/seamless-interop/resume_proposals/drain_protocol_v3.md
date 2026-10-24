# Proposed versioned epoch-drain contract (NOT ACCEPTED)

This review proposal changes no production protocol or tracker credit yet.
Current tested baseline is protocol2/layout3 at3230877, total16.40%.
The connect-only readiness probe in §3.4 and cross-OS drain proof are incomplete.

## Minimal readiness and epoch correction

Keep GuestReady on the existing configurable guest-exec service (normally5242),
with status0 meaning that service is reachable, never that seamless5002 is Ready.
The seamless entry path must reject a profile whose guest-exec port collides
with reserved5002 before VM startup; general legacy exec behavior is separate.
Only the actual host session owner connects to5002 and negotiates Hello/Ack.
Do not distinguish a probe with a grace timer. Empty/aborted peer connections
remain harmless before facilities start, but they are not an ownership proof.

Define epoch per successfully authorized mapping initialization, not per agent
process or arbitrary accept. An epoch owner survives individual transports and
retains unknown pins/quarantine. Candidate epoch allocation: CNG chooses a
nonzero63-bit initial seed once per owner; use checked monotonic increments
within that owner, never wrap. No epoch uniqueness substitutes for detach proof.
After process crash, a retained epoch/backing-identity record must prohibit
initializing the old BAR unless proof is recovered or a trusted VM reset supplies
a distinct new backing object. Memory retained by an old compositor must never
be reinterpreted as the new epoch merely because a new process started.

## Why a reviewed wire amendment may be needed

The pure slot runtime accepts caller-provided producer-stop/host-drained facts.
No existing integrated native owner supplies both after transport crash. Exact
FrameRelease proves one frame only; shared Free, wl_buffer.destroy, EOF, timeout,
Goodbye or process restart alone cannot replace it. The channel must remain
alive during orderly stop so final FrameReady/FrameRelease facts can be flushed.
A one-way Goodbye-before-cleanup is insufficient as an epoch-wide completion
receipt, and an unversioned overload risks trusting an old sender incorrectly.

Candidate: bump protocol to3 (layout remains3), then add three explicit control
messages with REQUIRED existing session_epoch tag, no new layout/field widths:
DrainBegin=0x0007, DrainReady=0x0008, DrainComplete=0x0009, all both directions.
Old v2 peers fail the header version check safely; do not silently overload
Goodbye status. Initiator reserves one nonzero ID in the common request owner;
Ready/Complete echo that operation ID. Begin/Ready/Complete must bind to the
current owned Ready/Draining connection and exact negotiated epoch.

## Candidate normal order and native facts

Guest producer-ready means: close admission, stop/join all capture/audio mapping
writers, finish/cancel provisional writers safely, flush all previously queued
FrameReady publications before its producer-ready receipt. No new publications
after that receipt. Guest still accepts final FrameRelease while draining and
retains pins until exact release. Host buffer-ready means: observe guest's
producer-ready receipt, process all earlier guest publications by FIFO, release
never-attached frames, retire surfaces, keep Wayland dispatch alive until every
accepted/imported/submitted buffer and asynchronous import is actually released,
flush every exact FrameRelease, and stop metadata/heartbeat/map I/O. Only then
may host assert its native readiness. wl_buffer.destroy is not that assertion.

Host-initiated: H Begin(idH) -> G producer stop/flush -> G Ready(idH) ->
H buffers/release/metadata drain -> H Complete(idH). Guest sees all FrameRelease
before Complete on FIFO. Local guest producer proof plus host's epoch-bound
completion receipt and zero outstanding pins can permit actual epoch completion.
Host has the guest producer receipt plus its own native buffer proof.

Guest-initiated: G stops producers and flushes publications BEFORE Begin(idG) ->
H buffer/release/metadata drain -> H Ready(idG) -> G validates zero pins and
records completion -> G Complete(idG). Both maintain local stop facts too.
Thus Begin is a producer receipt when sent by Guest; Ready is the guest receipt
when replying to Host Begin. This role asymmetry must be judged carefully.

Simultaneous Begin: retain one operation ID from each direction, never allocate
an unbounded list. Each sends Ready for peer Begin only after its own role's
required native facts; each sends Complete for its own Begin only after peer
Ready and its own facts. Host must have guest producer-ready before finishing
host drain. Need explicit merge, duplicate, close and request-retirement rules.

## Failure and ownership bounds

A complete send is not proof the peer consumed it. Lost terminal receipts can
leave the recipient quarantined even when the sender has enough local proof;
never guess success from socket closure. Bounded drain timeout closes transport
but preserves uncertain epoch resources and records. A stopped/crashed host
without observed compositor releases cannot emit host-ready. Automatic ordinary
transport reconnect requires retained owners and recovered valid receipts; if
proof is missing, fail closed and request controlled reset, not in-place reinit.
A VM restart is useful only when trusted owner verifies old producer death AND
new mapping uses a distinct backing object, so old compositor references still
refer to the old object. Merely restarting a process or resetting metadata is
insufficient. Durable quarantine requirements/API are still unresolved.

This proposal needs critical review before adopting protocol3. In particular:
(a) simplify simultaneous drain and asymmetry if possible; (b) prove no cyclic
wait on guest pins vs host-ready; (c) define close criteria and final ID release;
(d) distinguish frame-resource proof from stopping header/audio readers; (e)
consider a smaller versioned change that achieves equivalent safety. No code
or feature credit follows merely from this document.
