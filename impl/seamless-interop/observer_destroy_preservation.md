# O5 protected Destroy preservation: frozen observer contract

Status: normative, frozen by the lead before implementation. It uses the exact frozen `interop_window_graph_list_tracked_windows` ABI in `include/waddle/interop_window_graph.h`: `const interop_window_graph_t *`, `uint64_t session_epoch`, `uint64_t *windows`, `uint32_t capacity`, `uint32_t *count`, and `uint32_t *free_entry_count`. Capacity is at least `InteropMaxWindows` (64). On `Ok`, the list is sorted/unique Active+Retiring HWNDs and exact Free-entry count; Deferred entries are excluded. Non-`Ok` leaves graph, counters, and every output byte unchanged.

## Frozen values

Existing `window_observer_state_t` values remain `Created=1`, `Started=2`, `Stopped=3`, `Failed=4`. Append `Starting=5`, `Stopping=6`, `TrackingFault=7`; do not renumber. `Created` is unbound until `bind_graph` succeeds; no extra state value is needed. `Failed` means one or more hook tokens may remain and only stop retry / callback recording are allowed. Existing observer status values `0..15` remain unchanged. Append `GraphBindingMismatch=16`, `ProtectedStatePending=17`, `TrackingFault=18`. Graph ring pressure maps to existing `GraphQueueFull=3` and preserves `last_graph_status=InteropWindowGraphStatusQueueFull=10`; no new backpressure status.

Fixed bounds: ordinary dirty queue 512 entries (`WindowObserverDirtyCapacity` remains 512), protected Destroy mailbox 129 unique HWND keys, tracked snapshot 64 unique HWND keys, graph event ring 256. No heap allocation, waiting, graph call, clock read, or native query occurs inside `window_observer_note_hint`.

Protected mailbox entry layout (24 bytes, align 8):

```c
typedef struct window_observer_destroy_entry_t {
    uint64_t window;
    uint64_t destroy_stamp;
    uint32_t event_time_ms;
    uint32_t reserved_zero;
} window_observer_destroy_entry_t;
```

The ordinary dirty quarantine layout is the exact owned evidence, including Destroy, not a compressed kind-only record (48 bytes, align 8):

```c
typedef struct window_observer_dirty_record_t {
    uint64_t window;
    uint32_t kinds;
    uint32_t retries;
    uint64_t first_stamp;
    uint64_t last_stamp;
    uint64_t destroy_stamp;
    uint32_t destroy_event_ms;
    uint32_t reserved_zero;
} window_observer_dirty_record_t;
```

## Graph binding and ownership

Add `window_observer_bind_graph(observer, graph, session_epoch)`. It is owner-thread-only, allowed exactly once in Created, and stores a borrowed immutable graph pointer plus nonzero epoch only after successful graph invalidation and snapshot. The caller must suspend graph event consumption, capture/admission, and observer start while binding. Binding calls `interop_window_graph_invalidate_all(graph, epoch, &retired_count)` before taking the frozen callback-free snapshot. `QueueFull` returns `GraphQueueFull` with observer/binding/hooks unchanged and graph unchanged per that graph API. After invalidation succeeds, snapshot failure, wrong count, or impossible Free/count arithmetic latches permanent `TrackingFault`; the graph may already be mutated, so rollback is not claimed. A successful bind records the complete snapshot. The graph remains caller-owned and must outlive the observer.

Pump and refresh keep their existing `(observer, graph, epoch, ...)` signatures for source compatibility but require exact pointer and epoch equality with the immutable binding before reading time, invoking callbacks, or touching outputs. Mismatch returns `GraphBindingMismatch` with state/output unchanged. All admission-capable graph calls (`observe`, `refresh_offer`, refresh commit/flush, `note_destroyed` through the protected path, and invalidation) are observer-owned once bound. The sole permitted coordinator graph mutation is exact-proof `complete_retire`; immediately afterward and before any callback-capable operation, call `window_observer_sync_tracked(observer)`. No after-the-fact sync authorizes external admission.

`sync_tracked` uses only the frozen list snapshot and atomically replaces the observer’s sorted tracked key set. Any non-`Ok` graph status leaves the old set unchanged; an impossible count or post-mutation uncertainty permanently latches `TrackingFault`.

## Reservation API and mandatory safe point

Add:

```c
window_observer_status_t window_observer_prepare_callbacks(
    window_observer_t *observer,
    uint64_t candidate_window,
    uint32_t admission_may_add_entries);
```

`candidate_window` is zero for ordinary owner-loop message dispatch and nonzero for a query/offer candidate; `admission_may_add_entries` is exactly 0 or 1. It is not a graph mutation. Before yielding to any arbitrary owner-thread message dispatch that can deliver WinEvents, coordinator MUST call `prepare_callbacks(observer, 0, 0)` and must not dispatch unless it returns `Ok`. This call returns `Ok` only when the callback-only reserve has been established; otherwise it returns the exact local/fault status or `GraphQueueFull`. Before every callback-capable native query, enumeration pass, app-key lookup, or graph admission, observer itself calls the same reservation with the current candidate and the correct admission flag. The public safe-point call cannot replace internal preflights.

The reservation takes a complete frozen snapshot and forms `U = unique(mailbox HWNDs ∪ Active/Retiring HWNDs ∪ candidate if nonzero)`. Let `F` be the exact Free-entry count. A callback-only safe point requires `|U| <= 129`; a possible admission requires `|U| + F <= 129`. This reserves room for every key that one offer may create, including its primary and Deferred children flushed into Active; Active+Retiring share the 64-entry array, so the number of newly tracked unique keys cannot exceed pre-mutation `F`. The primary candidate is already counted in `U`. If the inequality fails, drain protected mail items in a bounded pass, recompute the snapshot and reserve, and repeat only while progress occurs. If `note_destroyed` hits graph `QueueFull`, retain the exact mailbox entry, return `GraphQueueFull`, and do not start candidate query/admission. The owner loop keeps callback dispatch behind its scheduling boundary until a callback-only reserve succeeds. Each call processes at most 129 mailbox entries and performs no unbounded retry.

Reentrant clock/hook callbacks during a mailbox graph call may add/merge Destroy only for keys already included in the reserved tracked/candidate union. A mailbox entry in flight remains occupied. On graph success, clear only if the live key still has the snapshotted `(window, destroy_stamp)`; a newer stamp remains pending. QueueFull or unexpected status clears nothing. Any impossible capacity state, full mailbox despite reservation, Destroy-stamp exhaustion, or inability to store a newly delivered protected Destroy sets permanent `TrackingFault` and `evidence_incomplete=1` atomically; never overwrite an entry or claim the event was retained. TrackingFault stops admission, event publication/capture, graph release, and bundle reclamation. Later callbacks may only preserve available evidence and maintain the fault latch. This is fail-closed service loss, not successful destroy processing.

## Callback and dirty-to-protected promotion rules

The protected key set comprises every HWND in the last complete Active+Retiring snapshot and the current reserved candidate. A Destroy hint matching a protected key enters/merges into the 129-key mailbox, retaining the latest exact stamp/event time. Repeated Destroy for one key uses one slot. Hints for other events continue to coalesce in the existing ordinary table. Untracked Destroy remains an ordinary dirty record while capacity exists; it is never erased by overflow recovery.

If an incoming untracked Destroy cannot be stored because the ordinary table is full, latch permanent `TrackingFault` with `evidence_incomplete=1` rather than discard it and later treat a full refresh as continuity proof. Ordinary non-Destroy overflow may retain the existing refresh latch. A full refresh may not clear or overwrite any Destroy payload.

Before querying/offering a dirty candidate, move any queued Destroy payload for that HWND into the protected mailbox (keeping the dirty copy until mailbox insertion succeeds), apply the exact Destroy first, discard stale query facts, and only then perform a fresh native query if the candidate remains eligible. Before a parent offer that may flush Deferred entries, retain the admission reservation above. Immediately after graph mutation, take the callback-free tracked snapshot before any native/user callback, refresh iteration, or event exposure. Scan newly tracked HWNDs against all queued dirty Destroy payloads, promote matching entries to protected mailbox before clearing their dirty copies, and apply them before returning events for external consumption. On QueueFull retain both evidence copies and block external event consumption for that HWND.

A Destroy reentered from `query_facts` invalidates that query result; apply Destroy before any offer and use only a later fresh query. A Destroy reentered from graph app-key lookup may arrive after the graph has committed an event; apply it immediately after the graph call and do not expose graph events to capture/transport until the observer returns and every corresponding key barrier is clear. Deferred-child facts are cached and not freshness proof: coordinator must re-query every child that a parent offer activated before capture and must discard a validation result if Destroy arrived during that query.

Add read-only `window_observer_destroy_pending(observer, window, out_pending)`: output is exactly 0/1, pointer is aligned/nonoverlapping, and unchanged on error. It reports mailbox membership only; it is not an OS incarnation fact. Coordinator stages graph ring events in fixed bounded storage (at least the 256-entry ring capacity), preserves order, and never sends/captures an Announce/Update while that HWND has a pending protected Destroy. Stop admissions before staging would overflow; retain staged events and backpressure. If Destroy remains pending because QueueFull, graph events remain staged and blocked. This staging and capture barrier are coordinator obligations, not proof produced by observer.

## Start, stop, retry, and epoch rules

`start` requires a successful binding. Before installing the first hook, set state `Starting`, publish the callback target, and hold the process-wide lifecycle reservation. In Starting/Stopping/Failed, callbacks accept only protected Destroy evidence; ordinary hints can set the full-refresh latch. Publish `Started` only after every hook is installed. On install failure, remove acquired hooks in reverse order while the target remains published. If all removals succeed, unpublish target, wait for `g_hint_inflight==0`, release lifecycle reservation, return to bound `Stopped`, and retain mailbox/dirty evidence. If any removal fails, retain target and tokens, enter `Failed`, return `HookFailed`/`UnhookFailed`, and allow only stop retry plus protected callback recording.

Before any unhook attempt, `stop` calls `invalidate_all` on the immutable bound graph. If `QueueFull`, return `GraphQueueFull` without changing state/hooks/target/graph. On success, old Active becomes Retiring and Deferred is invalidated; no resource is freed. Record `stop_invalidation_done=1`, set `Stopping`, and remove hooks in reverse order while callback target remains published. If any removal fails, enter `Failed`, retain target/failed tokens/evidence, and return `UnhookFailed`. A retry with `stop_invalidation_done=1` only retries remaining hooks; it does not invalidate again. Once every hook token is absent, unpublish target, wait until global in-flight count is zero, release lifecycle reservation, enter `Stopped`, retain mailbox and Retiring graph entries. Stop success is never host-drain or release proof.

Same-bound restart retains the mailbox and graph pointer/epoch and runs fresh enumeration/native queries only after hooks are installed. It does not infer continuity across the unhooked interval. Bind to another graph/epoch is forbidden for the same observer. Disconnect, EOF, Closed, new token, stop, and timeout never reset epoch or release the old graph/backing. Protocol v2 has no host-drain receipt. A new epoch requires distinct peer/graph/observer/registry and distinct backing after producers/callbacks are externally quiesced under a separately accepted host-drain gate; absent that gate, quarantine and retain.

## Stopped drain and normal destroy

Add `window_observer_drain_protected_destroys(observer)`, allowed only in Started or Stopped. It applies the bounded mailbox algorithm with the immutable graph binding and exact stamp checks; it runs no native query/admission and installs no hooks. QueueFull can leave earlier accepted mailbox items committed, but every remaining entry is unchanged. It cannot run in Starting, Stopping, Failed, or TrackingFault. It produces no release proof. This permits a successful stop with reentrant Destroy evidence to resolve without restarting hooks.

`window_observer_destroy(observer_slot)` no longer implicitly performs stop; caller must stop explicitly. It is permitted when an unbound Created observer has no hooks/target/in-flight callbacks and no evidence, or when a bound observer is Stopped with every hook absent, target unpublished, in-flight count zero, no TrackingFault, empty mailbox, and a fresh successful graph list snapshot returning zero Active/Retiring keys. Otherwise return `ProtectedStatePending` (or `TrackingFault` for poison) with slot and object unchanged. The unbound Created case needs no graph snapshot because it has never been allowed to observe or mutate one. On success free only observer allocation and null slot. It never destroys/invalidates graph, releases a Retiring entry, unmaps backing, or claims host drain. If a healthy Stopped observer still has graph Retiring entries, caller retains graph/native bundle and may apply exact release proofs then `sync_tracked` before retrying destroy.

## Quiesced, no-leak evidence handoff

Add fixed caller-owned `window_observer_quarantine_record_t` for diagnostic evidence transfer and freeing observer-local memory only. This is not a restart image or authority. It is allowed only in `Stopped` or `TrackingFault` after all hook tokens are absent, active callback target is not the observer, and global in-flight count is zero. If hooks remain, return `ProtectedStatePending`; no target unpublication is attempted by handoff. TrackingFault handoff preserves fault and evidence completeness; it never clears fault or releases graph/backing. Healthy Stopped handoff explicitly means caller permanently suspends and retains the whole old graph/native bundle. Handoff makes no graph/clock/OS calls, allocates nothing, and frees only observer storage after a complete staged record copy. There is no restore/import API.

Exact record layouts (x86_64 supported ABI):

```c
#define WindowObserverDestroyCapacity 129u
#define WindowObserverTrackedCapacity InteropMaxWindows /* 64 */
#define WindowObserverDirtyCapacity 512u /* existing */
#define WindowObserverQuarantineVersion 1u

typedef struct window_observer_quarantine_record_t {
    uint32_t version;                 /* offset 0 */
    uint32_t size;                    /* offset 4; sizeof(type) */
    interop_window_graph_t *borrowed_graph; /* offset 8; identity metadata only */
    uint64_t session_epoch;            /* offset 16 */
    uint32_t observer_state;           /* offset 24 */
    uint32_t last_graph_status;        /* offset 28 */
    uint32_t tracking_fault;           /* offset 32; 0/1 */
    uint32_t evidence_incomplete;      /* offset 36; 0/1 */
    uint32_t protected_count;         /* offset 40; <=129 */
    uint32_t dirty_count;              /* offset 44; <=512 */
    uint32_t tracked_count;            /* offset 48; <=64 */
    uint32_t tracked_snapshot_valid;   /* offset 52; 0/1 */
    uint64_t candidate_window;         /* offset 56; zero if none */
    uint32_t mailbox_inflight_index;   /* offset 64; UINT32_MAX if none */
    uint32_t reserved_zero;            /* offset 68 */
    window_observer_destroy_entry_t protected_destroys[129]; /* offset 72 */
    window_observer_dirty_record_t pending_dirty[512];       /* offset 3168 */
    uint64_t last_known_tracked_windows[64];                 /* offset 27744 */
} window_observer_quarantine_record_t; /* sizeof 28256, align 8 */
```

Compile-time asserts freeze destroy entry size 24, dirty record size 48, record offsets above, and record size 28256 on the supported x86_64 C ABI. If another target ABI is enabled, it needs its own asserted layout before using this record.

Add `window_observer_quarantine_handoff(window_observer_t **observer_slot, window_observer_quarantine_record_t *record)`. Preflight NULL/alignment/nonwrapping complete spans and record disjoint from slot and known observer allocation; graph/backing disjointness is caller precondition because graph is opaque. Then validate owner thread, nonreentrancy, allowed state, hook absence, target absent, inflight zero, and all counts in range. Every error leaves slot, observer bytes and every record byte unchanged. Stage complete fixed record locally, copy record once, set slot NULL, free observer once. The graph pointer is copied as borrowed identity only; handoff does not call it or free it. If the faulted observer cannot represent exact mailbox/dirty contents, do not truncate: return `TrackingFault` with outputs unchanged and keep observer allocated for explicit process-lifetime retention. Handoff record has no `drained`, `safe_to_release`, `restore`, or `new_epoch_allowed` field.

## Acceptance matrix

- Bind/unbound and exact graph-pointer/epoch mismatch; QueueFull and post-invalidation snapshot fault; output/state byte preservation.
- Safe-point reserve for no candidate and candidate admission with F=64, F=0, existing keys, and old mailbox keys; force QueueFull while draining and prove next callback-capable operation/admission never runs.
- Full ordinary 512-entry hint table: protected Destroy still enters reserved mailbox; ordinary non-Destroy overflow never clears a Destroy; untracked Destroy inability to store poisons and blocks admissions.
- Destroy during query: no offer uses stale facts. Destroy during app-key lookup: event remains staged behind pending-key barrier. Parent Deferred flush: all matching ordinary Destroy payloads promoted before external event consumption; every child receives fresh native validation before capture.
- Same-key reentrant newer stamp during clock callback; old success clears only old stamp. QueueFull preserves exact tuple. Graph failure cannot consume evidence.
- Starting hook-install callback, partial rollback, Failed stop retry, Destroy during unhook and successful callback quiescence. Target/token remain until last hook removed and in-flight zero.
- Bind/stop invalidation retires but never frees backing; QueueFull leaves state/hooks/graph unchanged; retry does not double-invalidate.
- Stopped protected drain and normal destroy gate. Destroy is blocked by mailbox, poison, hook, inflight, Active/Retiring graph entries. Success nulls only observer handle.
- Handoff sentinels and byte-exact record for healthy Stopped and poisoned quiesced observer; reject while hook/token/target/inflight exists; verify graph/native backing remains live. No handoff restore or reset.
- Callback overflow/stamp exhaustion and impossible capacity latch permanent poison/evidence-incomplete; later operations cannot admit, capture, release, or destroy resources.
- Native WinEvent qualification reports real OS hook behavior separately from fake synchronous reentrancy tests. No VM/GPU or host-drain credit is inferred from socketpair/API doubles.

## Implementation seam map (current source, for the lead's later freeze)

- `src/interop/window_observer_windows.c:148-182`: observer-private fields and process-wide atomics. Add immutable graph/epoch, tracked snapshot/count/validity, fixed mailbox/inflight stamp, candidate, stop-invalidation latch, evidence/fault latches here. Keep all O5 storage in the existing observer allocation to avoid additional lifetime owners.
- `:545-581`: `stop_locked`/destroy currently unpublish callbacks before unhook and clear dirty hints. Replace with invalidate-before-unhook, Starting/Stopping/Failed semantics, evidence preservation, callback-quiescence check, and separate normal destroy/handoff gates. Do not erase dirty Destroy entries.
- `:634-684`: `start_locked` currently installs hooks before publishing Started and resets dirty state. Publish callback target and Starting first; retain reentrant Destroy during install and rollback. Successful rollback must clear target only after all hooks are gone and inflight is zero.
- `:690-754`: `note_hint_active` currently drops all callbacks unless Started and stores every kind in the ordinary FIFO. Route tracked/candidate Destroy to the mailbox in Starting/Started/Stopping/Failed; route untracked Destroy to dirty FIFO, poisoning instead of losing it on full/exhausted state. Callback path remains allocation-/graph-/wait-free.
- `:850-885`: `query_window`/facts assembly is callback-capable and must be entered only after candidate reserve. After query returns, compare mailbox candidate stamp before offering; stale facts are discarded.
- `:981-1210`: refresh body, overflow recovery, and pump contain all observer-owned graph admission paths. Put reservation before every native callback and graph call; after a graph mutation snapshot before another callback, promote matching dirty destroys and apply mailbox before returning graph events. Deferred flush children require fresh coordinator validation.
- `:1202+`: refresh public entry and all public graph/epoch inputs must first match immutable binding; no direct alternate admission may bypass the observer.
- `include/waddle/interop_window_graph.h:1284-1295` and `src/interop/window_graph_abi.zig` snapshot implementation: use this existing const/u32 API only; do not add a second list ABI or mutable graph access.
- `tests/interop/window_observer_windows.c`: augment fake hook/query/app-key/clock reentrancy. Add QueueFull capacity-reservation tests, successful and failing start/unhook reentry, candidate stale-query discard, Deferred child promotion/fresh-validation gate, stop invalidation retry, stopped drain, poison/handoff byte-preservation, and graph/backing retention counters. Keep native hook delivery separately qualified by `tests/interop/window_observer_native_windows.c`; fake callbacks do not prove Windows delivery.


## Lead amendments (normative; take precedence)

The observer-local contract above is accepted. Coordinator staging/capture and
installed-session acceptance remain separate required implementations; this
contract alone does not finish O5 or award progress.

- `current_thread_id` is the sole API-table operation allowed inside note_hint;
  its existing contract requires a trivial nonblocking thread-ID read. No other
  native/user callback, clock, graph operation, allocation or wait is allowed.
- All new APIs reject local argument/thread/reentrancy/binding/state failures
  before clocks, graph mutation and output writes. Owner-thread identification
  is necessarily read through current_thread_id. Graph binding is authentic and
  borrowed; arbitrary/freed graph/observer pointers violate caller preconditions.
  Pump/refresh local failures also preserve optional stats. Operational results
  after work begins may update counters/stats; no rollback is claimed.
- Created-unbound permits ordinary destroy and rejects start/stop. Created-bound
  permits explicit stop to reach Stopped even if start was never attempted.
  Normal destroy never calls stop implicitly. Stopped-bound permits restart,
  sync, read-only pending query, protected drain and handoff. Failed permits only
  stop retry, status/pending diagnostics and protected callback recording.
- A bind post-invalidation failure retains attempted graph/epoch as quarantine
  metadata and latches TrackingFault even though binding did not commit. QueueFull
  before successful invalidation leaves all binding metadata unchanged. Validate
  first_stamp above graph stamp_high_water before invalidation; equal/lower is
  InvalidArgument. Matching uac_mode remains an explicit caller precondition.
- Fault is sticky independently of hook ownership. A fault during installation
  immediately rolls back acquired hooks with callback target retained. Faulted
  stop performs hook cleanup only, never graph calls. Unhook failure returns
  UnhookFailed with fault and hooks retained; once all hooks/target/inflight are
  absent it returns TrackingFault, enabling quiesced handoff. No reset exists.
- Every snapshot is checked for sorted unique nonzero HWNDs, count<=64 and
  free<=64 with count+free<=64 (duplicates across generations explain strict
  inequality). Install only a complete validated snapshot. A non-Ok snapshot
  after external complete_retire or a possible graph mutation permanently faults;
  no retry pretends the old set is current. Fixed embedded private counts remain
  bounded on every valid fault path, so handoff has a deterministic one-free
  cleanup path. Private memory corruption is outside the authentic-handle API;
  it is not an intentional normal lifetime-leak exception.
- Stamp exhaustion returns TrackingFault on bound execution paths; legacy numeric
  StampExhausted remains reserved for ABI compatibility. Preserve evidence,
  latch evidence_incomplete, and disallow new admission/publication/release.
- Full ordinary-table recovery must retain every stored Destroy payload and
  compact/drop only entries without Destroy. Untracked Destroy that cannot be
  stored faults explicitly; no full refresh may claim lost evidence restored.
- Public prepare_callbacks establishes a reservation, never permission for direct
  coordinator admission. Candidate protection persists until the next successful
  prepared boundary clears/replaces it. Internal observer operations always
  prepare their own candidate before the first callback-capable operation.
- Coordinator staging will use exactly256 event entries. While a barrier or
  staged queue remains, stop all graph admissions and refresh offers; only
  bounded protected-destroy drain and exact retirement completion may mutate.
  Stage a full existing graph ring once to free room. With admissions stopped,
  at most64 Active records can generate new RetireIntent events, so protected
  drain cannot require unlimited additional staging. Resolve staged Announce/
  Update using exact graph check_action and fresh native validation; stale
  generations are suppressed even before their later RetireIntent is dequeued.
  Never infer native release from suppression. The forthcoming coordinator
  contract must specify its complete fresh-query/capture/queue lifecycle before
  this observer contract can earn coordinator or installed-session credit.

## Candidate-transition correction from implementation review

Before a successful reservation returns, the previously reserved candidate must
remain protected even inside graph clock callbacks. Use private temporary
transition-candidate storage; outside the serialized prepare operation it is
zero. Before promoting the proposed candidate or invoking a callback-capable
drain, establish callback-only capacity for mailbox∪tracked∪old candidate∪new
candidate. If adding the new key cannot fit, first drain under the still-valid
old reservation with the new candidate unpublished; bound all work by the same
129-operation budget. Return ProtectedStatePending/GraphQueueFull with both
stored evidence and the old persistent candidate retained if no bounded progress
can establish the transition reserve. Do not manufacture an incomplete-evidence
fault merely because the bounded budget ended while all tuples are retained.

Once the transition reserve fits, callbacks preserve Destroy for mailbox members,
tracked keys and both candidates. Promote/apply any queued candidate Destroy
before native queries, then compute the final new-candidate reserve plus exact
Free count for admission. Commit persistent candidate only on Ok; clear temporary
transition state on every exit. Every direct candidate assignment (including a
popped ordinary Destroy in process_key) must use this safe transition rather than
replace the persistent key before graph callbacks. Repeated Destroy for any
existing mailbox key always merges there even if it left the tracked set.

A bind graph-read failure before invalidation has no irreversible mutation:
return the exact WrongThread/Reentrant local guard or GraphError for another
unexpected read status, preserving binding/state. TrackingFault is reserved for
an actually latched permanent fault, including a possible-mutation snapshot
failure. A malformed preflight snapshot fails closed, without installing its
partial data; distinguish retryable graph Reentrant from permanent invalid
snapshot facts. Normal destroy of a healthy bound Stopped observer may discard
ordinary hints once hooks/target/lifecycle/inflight are absent, mailbox empty,
and a fresh complete graph snapshot has no Active/Retiring entries; no graph or
backing resource is released by that observer-only free.
