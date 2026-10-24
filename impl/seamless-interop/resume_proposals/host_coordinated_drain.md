# Minimal host-coordinated orderly epoch drain

## Recommendation

Use the five-message shape as the explicit orderly protocol, with one host
owned operation ID per epoch. Keep `DrainNotice` a coalesced guest request
(request ID zero), not a proof. `DrainBegin` is the sole epoch operation start;
`DrainGuestReady` proves guest producers stopped; `DrainHostReady` proves host
users stopped; `DrainAck` tells the host that the guest consumed all preceding
releases, verified zero pins and retired its old runtime. The host retains its
operation ID through Ack. This avoids simultaneous operation IDs and gives the
host a directly correlated guest-retirement receipt.

A two-receipt variant can be safe only if a later, tightly gated fresh `Hello`
acts as an implicit Ack and the host retains the old operation state until
that `Hello`. The host otherwise has no event that tells it the guest processed
the terminal releases and observed zero pins. Explicit Ack is clearer and
supports a small pure FSM. It still does not create common knowledge: a lost
Ack leaves the host quarantined. It does not prove crash recovery.

All five messages are protocol-v3 only, require the exact current nonzero
`SessionEpoch` tag, valid role/direction and an exact complete codec frame.
`DrainNotice` requires `request_id=0`. The host allocates a nonzero common
request ID for Begin, retains it through Ack, and echoes it on GuestReady,
HostReady and Ack. No request ID may be reused while this operation is active.
Layout remains v3. Protocol2 stays unchanged and cannot negotiate these
messages.

## Admission and FIFO cutoffs

- Host-triggered: immediately before queueing Begin, host closes admission to
  new local facility work. It continues parsing/servicing earlier guest frames
  until GuestReady. Receipt of Begin closes guest admission to new mapping
  writers. Guest stops and joins frame/audio producers, resolves provisional
  writes safely, then fully flushes its final allowed `FrameReady` and any
  other already-ordered facility publications before fully sending
  GuestReady. Same-direction stream order makes GuestReady a fence: every
  earlier guest publication is before it.
- Guest-triggered: guest first closes mapping-writer admission, stops/joins
  producers and flushes final publications; it sends coalesced Notice after
  that prefix, and sends no further mapping-backed publications. Notice is
  only a request, not producer proof. Host later sends Begin with its
  allocated ID; guest responds with GuestReady for that ID. If Notice is lost,
  guest stays quiesced and no reset is allowed.
- After GuestReady, guest sends no more epoch facility publications. Host
  continues consuming required frame/audio/control prefix and must reject any
  post-Ready mapping-backed producer message as a protocol violation. Host
  sends all exact FrameRelease frames before HostReady on its stream. Guest
  applies these before it can parse HostReady.
- Begin, GuestReady, HostReady and Ack are committed by the FSM only after a
  full decoded receive or matching complete-send confirmation. A queued or
  partial write is never a receipt. Before local cutoff the owner records its
  intent and closes admission; a send failure after cutoff enters Quarantined.

`DrainNotice` duplicates coalesce while the same epoch is Active, AwaitBegin,
or Draining. It creates no ID, does not authorize teardown, and cannot restart
an operation after terminal/quarantine. It may cross Begin in flight: the
host ignores/coalesces it for the already active epoch operation. There is
never a second concurrent Begin.

## Native proof carried by readiness events

`DrainGuestReady(id)` is valid only after all frame/audio mapping writers are
stopped and joined, no provisional writer remains, and all final publications
are fully before this frame in the guest-to-host byte stream. The guest still
keeps its old runtime and processes matching FrameRelease frames until
HostReady.

`DrainHostReady(id)` is valid only after HostReady's exact prefix is processed;
every accepted/importing/submitted frame is either proved never exposed to the
compositor or received a real buffer release; every exact FrameRelease was
fully flushed; asynchronous imports and PipeWire/audio/header readers are
stopped/joined; no mapping-backed read/write can resume; and the host mapping
lease has been irrevocably released. A timeout, display disconnect, proxy
destroy, or successful `wl_display_sync` alone cannot produce this event.

Before sending `DrainAck(id)`, guest has parsed every preceding FrameRelease,
verified no runtime pin/provisional claim remains, used its guest-stop proof
to enter drain, retired the old runtime, and will never resume an old-epoch
writer. Ack contains no new proof beyond this state transition. Host may mark
the orderly operation complete only after receiving exact Ack.

Host is the gate for accepting a new session on the same backing object. A
guest may initialize new metadata only after HostReady and local zero-pin
retirement; it must not publish/Hello for a replacement epoch until its owner
has locally completed that gate. Host accepts a replacement only after the
old operation's Ack and only if the retained native owner still proves the
same backing object is released from the old epoch. A process restart that
lost this owner is Quarantined, not a new authorization.

## Finite host-side transition table

States: `Active`, `AwaitGuestReady(id)`, `DrainingHost(id)`,
`AwaitGuestAck(id)`, `Complete`, `Quarantined`. `DrainNoticeSeen` is a boolean
coalescing flag while Active; it is not a separate proof state.

| State | Event and guard | Next state / action |
|---|---|---|
| Active | local stop, or valid current-epoch Notice | Set/coalesce notice intent; remain Active until Begin is committed |
| Active | `start_begin`: allocate unused nonzero ID, close new host admission, queue Begin | `AwaitGuestReady(id)`; retain ID |
| AwaitGuestReady | Notice, current epoch, request ID 0 | Remain; coalesce, do not allocate another ID |
| AwaitGuestReady | exact GuestReady(id), complete valid frame | `DrainingHost(id)`; freeze guest publication prefix and drain every accepted frame/audio/import |
| AwaitGuestReady | exact duplicate GuestReady(id) | Idempotent; no duplicate release or new transition |
| DrainingHost | local native proof incomplete | Remain; continue compositor dispatch / frame releases; no HostReady |
| DrainingHost | all host proof complete, all releases fully flushed, host mapping released | Queue HostReady(id); after full-send confirmation enter `AwaitGuestAck(id)` |
| AwaitGuestAck | exact Ack(id), current epoch | `Complete`; retire host request ID; permit next-session gate only if backing owner validates |
| Any nonterminal | wrong role/direction, wrong epoch/ID, malformed frame, publication after GuestReady, or conflicting duplicate | `Quarantined`; no proof or ID retirement; preserve any still-owned resources |
| Any nonterminal | EOF, transport failure, send failure, Wayland failure, liveness/drain deadline | `Quarantined`; timeout only aborts, never completes |
| Complete / Quarantined | late/duplicate old drain message | No reopening and no fresh proof; exact duplicate may be ignored diagnostically, otherwise reject |

`start_begin` is serialized on the host event loop, so Notice/local stop
coalescing has one winner. ID allocation failure leaves the owner Active with
admission unchanged; failure to queue after admission cutoff quarantines.
The host's `DrainNotice` is informational; no guest proof is inferred from
its receipt.

## Finite guest-side transition table

States: `Active`, `NoticeQuiescing`, `AwaitBegin`, `Quiescing(id)`,
`AwaitHostReady(id)`, `RetiredAwaitGate(id)`, `Quarantined`.

| State | Event and guard | Next state / action |
|---|---|---|
| Active | local guest stop | Close writer admission; stop/join producers; flush final guest prefix; queue Notice(0); after complete send enter `AwaitBegin` |
| Active or AwaitBegin | exact current-epoch Begin(id), nonzero host-owned ID | Close admission immediately if not already closed; stop/join producers; flush prefix; enter `Quiescing(id)` |
| AwaitBegin | Notice send failure/EOF/timeout | `Quarantined`; do not restart writers or initialize |
| Quiescing(id) | producer join/prefix pending | Remain; no Ready |
| Quiescing(id) | producers joined, no provisional writes, all final publications fully sent | Queue GuestReady(id); after full-send confirmation enter `AwaitHostReady(id)` |
| AwaitHostReady(id) | exact duplicate Begin(id) | Idempotent; do not restart producers or allocate ID |
| AwaitHostReady(id) | exact HostReady(id), after all preceding FrameRelease frames applied and zero pins verified | Retire old runtime; queue Ack(id); after complete send enter `RetiredAwaitGate(id)` |
| AwaitHostReady(id) | HostReady with outstanding/mismatched pin or native writer | `Quarantined`; preserve old runtime and do not initialize |
| RetiredAwaitGate(id) | host gate authorizes replacement using retained operation/backing owner | Begin fresh epoch/runtime only after all local old-owner calls are gone |
| Any nonterminal | wrong role/direction, epoch, ID, unexpected post-cutoff publication or conflicting duplicate | `Quarantined`; preserve old runtime/resources |
| Any nonterminal | EOF, timeout, partial send failure, host/display failure | `Quarantined`; never infer missing receipt |

If the host never authorizes a replacement after Ack, guest remains retired and
stopped. A lost Ack cannot make the host accept a new epoch. A newly started
agent must consult retained quarantine/gate state; process restart cannot
default to fresh mapping initialization.

## IDs, deadlines, duplicates, and liveness

- The host reserves one request ID; GuestReady/HostReady/Ack echo it. Host
  retains it until exact Ack, permanent quarantine, or a separately verified
  reset that ends the old backing object. Guest does not allocate a second
  operation ID. No ID is reused after timeout while the epoch owner remains.
- Duplicate exact `DrainNotice` coalesces. Duplicate exact Begin/Ready/HostReady
  is idempotent only in the matching active phase/epoch/ID and cannot replay
  native teardown. Duplicate Ack after completion has no effect. Conflicting
  IDs, direction/role, or epoch are protocol failure and cannot advance proof.
- All event handlers check session epoch, role, active operation ID, direction,
  state, full frame validation, and time before mutation. Local argument/token/
  backwards-time errors preserve state. A valid but unexpected wire event
  transitions to Quarantined (except exact idempotent duplicates).
- Normal Ready-session Ping cadence is stopped during drain. Use a separate
  monotonic absolute drain budget established when Begin is committed; it
  bounds work but timeout only transitions to Quarantined. Do not let Ping/
  Pong extend the drain budget or count as either native proof.
- Do not claim common knowledge: each endpoint knows only its own native facts
  and the exact peer receipt it consumed. Host knows guest pin retirement only
  after Ack; guest knows host release only after HostReady. A lost final Ack
  leaves host quarantined even if guest safely retired.

## Smaller no-Ack option

Without Ack, HostReady tells the guest that the host has completed native
drain, and same-stream ordering means the guest has processed all earlier
FrameRelease messages before it sees HostReady. The guest can then prove zero
pins locally and safely retire. But the host never observes that guest-side
fact. To keep this safe, the host must retain the old operation and accept no
replacement until an exact, explicitly specified event proves the guest
crossed the gate. A fresh Hello can serve as an implicit Ack only if it is
bound to the old operation/epoch and a still-live host owner rejects Hello
unless GuestReady and HostReady were completed and the guest runtime gate
requires zero pins first. That coupling is more fragile than the fifth
message. With the stated goal that the host knows zero pins and gates the next
epoch, retain explicit Ack.

## Critical counterexamples that remain fail-closed

1. GuestReady is partially sent or transport disappears before full receive:
   host has no producer proof; quarantine.
2. HostReady follows a failed wl_display drain, pending import, outstanding
   release, or audio/header worker: invalid sender behavior; guest cannot use
   it as native proof; quarantine.
3. HostReady is partially sent/lost: guest retains old runtime; host retains
   old operation/quarantine even though local map may already be released.
4. Ack is lost after guest retirement: host does not accept next session;
   neither side treats socket closure as Ack.
5. Host or guest process crashes after any proof but before the owner's
   retained state is recovered: quarantine. The FSM is not durable storage.
6. The Wayland connection dies with possibly outstanding compositor buffers:
   no HostReady; require independently verified compositor/backing reset or
   remain quarantined.
7. A VM/process restart supplies a new random epoch but same BAR backing
   object: no reset proof; do not initialize the metadata.

This contract supports only orderly stop while the retained native owners
remain alive. Crash recovery, cross-process durable proof recovery, and a
verifiable distinct backing-object reset remain separate unsupported work.
