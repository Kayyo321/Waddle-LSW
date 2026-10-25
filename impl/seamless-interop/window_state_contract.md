# Portable Window State Contract

## Scope and allocation

This module implements the pure, bounded reducer for one exact window
generation. It consumes validated protocol state observations and requests and
produces an ordered list of host or guest adapter actions. It owns no native
handles, calls no platform API, performs no allocation, and does not change the
frozen wire format. The reducer is one milestone within TODO #13, proposed at
50% of that TODO's allocation. Native `ShowWindow`, fullscreen, and
`PostMessage(WM_CLOSE)` adapters (25%), wire/compositor connection (15%), and
integrated native acceptance (10%) remain unimplemented and receive no credit
from these files.

## Inputs and frozen state bits

State values use the frozen mask `0x7f`: Minimized `0x01`, Maximized `0x02`,
Fullscreen `0x04`, Activated `0x08`, Resizable `0x10`, Maximizable `0x20`, and
Minimizable `0x40`. Guest observations may contain every bit except
Activated. Host configure may contain only Minimized, Maximized, Fullscreen,
and Activated. Unknown bits and directionally invalid bits are rejected.
Capabilities are observed separately from state and may contain only the
guest-derived Resizable, Maximizable, and Minimizable bits.

Every machine is bound at initialization to `(window_id, generation)`, where
both values are nonzero. Every operation names that pair. A mismatch is stale
and causes no state mutation. A machine is owner-thread-only; callers serialize
all calls and must not copy a live machine as an independent owner.

## State model and revision rules

The machine stores four distinct concepts: the last guest observation, the
last successfully applied guest state, the host's requested maximize policy,
and persistent host state successfully applied by its adapter. Persistent
host state contains only Maximized and Fullscreen. It also stores the last
guest capability bits, a monotonically increasing 64-bit revision, a `synced`
flag, a close-posted diagnostic flag, and a retired flag. Minimized and
Maximized are independent guest bits and may both be set. No request is
silently treated as an observation. Initialization is unsynchronized: the
first valid guest observation emits host reconciliation actions and must
complete successfully before configure, maximize-policy, or close requests
are accepted. If that observation is minimized, reconciliation emits the
one-shot minimize request; this is not stored as persistent host state. Each
fresh valid guest observation replaces the observed and applied guest state.
Once initialized, an observation updates persistent host state only through
actions it emits; an observation with no maximize edge preserves a
successfully applied maximize-policy request rather than overwriting it with
the old guest state. A fresh valid guest observation is the only operation
that restores `synced` after a partial adapter failure.

Planning is side-effect free. A plan carries the exact window identity and
base revision. Planning refuses to issue actions once the revision reaches
`UINT64_MAX`, so no adapter action is executed without a usable completion
ticket. Completion accepts only a plan for the current identity and
revision. Successful completion is accepted only when the reported completed
action count equals the plan length. A failed or partial completion advances
the revision and marks the machine unsynchronized; no later native-action plan
is allowed until a fresh observation reconciles state. Revision overflow is a
permanent error and never wraps. Thus an older plan cannot be applied after a
newer observation, request, retirement, or completion.

## Ordered reducer rules

* A guest minimized false-to-true observation produces one host
  `SetMinimized` request. Repeating the same observation produces none. The
  host compositor API has no persistent minimized-state setter or unsetter,
  so this is a one-shot request: it is never included in
  `applied_host_state`, and a later guest observation that clears Minimized
  does not produce a synthetic host restore action. On initial or failure
  reconciliation, a minimized guest observation may reissue this request,
  while the reducer tracks only persistent maximize/fullscreen host state.
* Guest maximize and fullscreen observations produce host set/unset actions
  only on observed transitions.
* `WindowStateRequest` changes only the host maximize policy and produces a
  host `SetMaximized` or `UnsetMaximized` action. It never directly produces a
  guest/native action. Maximizing is rejected when the last guest observation
  lacks Maximizable capability.
* A host configure containing Activated while the last successfully applied
  guest state is minimized produces `Restore` followed by `Activate`.
  The reducer suppresses a trailing `Minimize` in that same configure even if
  a stale minimized bit is present in its state; a later fresh guest
  observation determines the resulting state.
  The final applied guest state clears Minimized for that activation, so a
  repeated Activated configure does not restore an already restored window.
* A configure newly setting Maximized produces `Maximize` followed by its
  maximized `SetGeometry`; clearing Maximized produces `Restore` before
  geometry. A configure that changes fullscreen produces `EnterFullscreen` or
  `LeaveFullscreen` before geometry. When multiple transitions apply, the
  fixed order is restore-from-iconic, maximize/restore, fullscreen transition,
  activation, geometry.
* Geometry is omitted only when width and height are both zero. Otherwise both
  dimensions must be in `1..16384`; x/y are signed caller-prepared coordinates
  and are passed through unchanged.
* Every new close request before retirement may produce `PostClose`, including
  a later request after a successful post because the application may veto a
  prior `WM_CLOSE`. It does not retire the window. Only a matching Hide or
  Destroy observation marks it retired. Retired machines reject all later
  planning; a stale completion is rejected by its revision ticket.

The plan capacity is fixed at eight actions, while the current input grammar
can produce at most five. The reducer does not claim that native OS actions
are transactional: a partial execution leaves the machine unsynced until a
new observation.

## Memory, ownership, errors, and threading

All public values are fixed-size and contain no pointers or allocator-owned
memory. Callers own inputs and output plans. The module retains no caller
buffers. Public functions document invalid identity, invalid state/capability,
invalid geometry, stale revision, unsynchronized state, retired state, and
revision exhaustion errors. Pure calls on distinct machine values are
independent; calls mutating one machine require one serialized owner thread.

## Verification

Built-in tests cover every state bit and invalid direction, transitions and
edge deduplication, action ordering, zero/nonzero geometry bounds, close versus
retirement, identity/generation mismatch, stale plans, partial failures,
resynchronization, and monotonic revision exhaustion. Module qualification must
use the repository coverage recorder with at least 90% line and taken-branch
coverage and leak-enabled Zig tests. No native adapter or runtime behavior is
claimed by this portable module.
