# O5 priority-destroy API and state-transition proposal

This proposal is additive to the current graph/observer interfaces. It keeps the 32/64-bit wire layouts unchanged and does not add a host-drain message. Public functions are owner-thread-only. The graph is borrowed by the observer; no new API takes ownership of a mapping or graph allocation.

## Graph snapshot addition

Add the following read-only function to `include/waddle/interop_window_graph.h`:

```c
/**
 * Copy the unique HWNDs represented by Active or Retiring graph entries and
 * return the exact number of currently Free entry slots.
 *
 * @param graph [in, non-null] Owner-thread graph handle; not retained.
 * @param session_epoch [in] Exact nonzero epoch bound to the graph.
 * @param hwnds [out, non-null] Caller-owned array of at least
 *        InteropMaxWindows uint64_t elements. Entries are sorted by numeric
 *        HWND and contain no duplicates. No pointer is retained.
 * @param hwnd_capacity [in] Must be at least InteropMaxWindows.
 * @param out_count [out, non-null] Receives the number of unique returned
 *        HWNDs only on success.
 * @param out_free_entry_count [out, non-null] Receives the exact Free-entry
 *        count only on success.
 * @return InteropWindowGraphStatusOk on success;
 *         InteropWindowGraphStatusInvalidArgument for null, undersized,
 *         misaligned, overlapping, or overflowing spans; the existing
 *         InteropWindowGraphStatusWrongThread, Reentrant, or StaleEpoch result
 *         for those conditions. On every error, the array and both scalar outputs
 *         remain byte-for-byte unchanged.
 * @thread_safety Owner thread only; no callback, allocation, lock, or wait.
 */
interop_window_graph_status_t interop_window_graph_list_tracked_windows(
    interop_window_graph_t *graph,
    uint64_t session_epoch,
    uint64_t *hwnds,
    size_t hwnd_capacity,
    size_t *out_count,
    size_t *out_free_entry_count);
```

Validation is entirely pre-write: validate graph/epoch/thread/reentrancy, `hwnd_capacity >= InteropMaxWindows`, pointer alignment, pointer-end overflow, and pairwise disjointness of graph handle, `hwnds[0..InteropMaxWindows)`, `out_count`, and `out_free_entry_count`. Then collect into a local fixed-size array, deduplicate Active+Retiring HWND values, sort ascending, verify count <= InteropMaxWindows, and only then copy all results. `out_count` and `out_free_entry_count` are both mandatory. The snapshot reports only Active/Retiring HWNDs; Deferred records are not externally tracked and must become Active only through observer-owned prepared admissions.

## Observer public additions

Add a forward declaration for `interop_window_graph_t` from the graph public header and these functions/statuses in `src/interop/window_observer_windows.h`:

```c
/* Append numeric values; do not renumber existing status members. */
WindowObserverStatusGraphBindingMismatch = 16,
WindowObserverStatusProtectedStatePending = 17,
WindowObserverStatusTrackingFault = 18,

/**
 * Bind one graph and epoch to a newly created observer and establish the
 * initial tracked-HWND/free-entry snapshot before hooks can be installed. Binding first atomically invalidates any Active entries and clears Deferred records, retaining Retiring entries and resources.
 *
 * @param observer [in,out, non-null] Created, unbound observer. It is not
 *        modified on failure.
 * @param graph [in, non-null] Owner-thread graph borrowed until observer
 *        destruction; caller must keep it alive for the entire binding.
 * @param session_epoch [in] Exact nonzero graph epoch; immutable for this
 *        observer lifetime.
 * @return WindowObserverStatusOk on success; WindowObserverStatusInvalidArgument,
 *         WindowObserverStatusWrongThread, WindowObserverStatusBadState,
 *         WindowObserverStatusGraphQueueFull, or WindowObserverStatusGraphError
 *         on failure.
 *         GraphQueueFull is retryable and leaves the graph and observer
 *         unchanged. Other preflight failures also leave them unchanged. The
 *         bind first atomically invalidates Active/Deferred graph state; if
 *         that succeeds but the subsequent snapshot is inconsistent, the
 *         observer becomes terminally TrackingFault. GraphError preserves
 *         the exact graph status in the observer diagnostic accessor.
 * @thread_safety Owner thread only. Does not install hooks or allocate.
 */
window_observer_status_t window_observer_bind_graph(
    window_observer_t *observer,
    interop_window_graph_t *graph,
    uint64_t session_epoch);

/**
 * Resynchronize after the sole permitted external mutation, an exact-proof
 * complete_retire which only removes a Retiring entry.
 *
 * @param observer [in,out, non-null] Bound observer on its owner thread.
 * @return Ok on success. A graph/snapshot inconsistency permanently sets the
 *         tracking fault and returns TrackingFault/GraphError; all other
 *         preflight errors leave the tracked snapshot and mailbox unchanged.
 * @thread_safety Owner thread only, callback-free. Call immediately after
 *        complete_retire and before dispatching another native callback.
 */
window_observer_status_t window_observer_sync_tracked(
    window_observer_t *observer);

/**
 * Destroy only a locally quiesced observer with no protected evidence and no
 * Active/Retiring graph entries. Does not destroy or unmap the borrowed graph.
 *
 * @param observer [in,out, non-null] Pointer to the caller's owning handle.
 *        Set to NULL only after successful destruction.
 * @return WindowObserverStatusOk on success;
 *         WindowObserverStatusProtectedStatePending if hooks/callbacks/evidence
 *         or Active/Retiring HWNDs remain; WindowObserverStatusTrackingFault
 *         for a poisoned observer.
 *         Failure leaves the handle and object unchanged.
 * @thread_safety Owner thread only; requires completed stop/unhook quiescence.
 *        This local check is not a host-drain proof.
 */
window_observer_status_t window_observer_destroy(
    window_observer_t **observer_slot);
```

Existing `window_observer_start` remains source-compatible but now requires a successful immutable graph binding and initial sync. Existing pump/refresh functions that accept `graph` and `session_epoch` must require exact pointer/epoch equality with the binding; mismatch returns GraphBindingMismatch before output writes, clock calls, mailbox drains, or graph calls. They do not rebind.

Add an owner-thread query so the event coordinator can hold external consumption while the observer still has protected work:

```c
/**
 * Report whether a delivered Destroy for one HWND remains unapplied in the
 * protected mailbox, including the item currently in flight.
 *
 * @param observer [in, non-null] Bound observer on its owner thread.
 * @param window [in] Nonzero numeric HWND key.
 * @param out_pending [out, non-null] Receives 0 or 1 only on success.
 * @return WindowObserverStatusOk; WindowObserverStatusInvalidArgument,
 *         WindowObserverStatusWrongThread, WindowObserverStatusReentrant,
 *         WindowObserverStatusBadState, or WindowObserverStatusTrackingFault
 *         otherwise. Output is unchanged on error, including for TrackingFault.
 * @thread_safety Owner thread only; callback-free and allocation-free.
 */
window_observer_status_t window_observer_destroy_pending(
    const window_observer_t *observer, uint64_t window, int *out_pending);
```

This query only reports mailbox application status; it is not a retirement/release proof. After it becomes false, the coordinator must still resolve staged graph events by `(HWND,generation)` and run the mandatory fresh-query rule if the observer discarded pre-destroy facts. An event is not externally consumable merely because the mailbox bit was cleared.

The observer owns a bounded 129-key protected-destroy mailbox and one immutable graph pointer/epoch. All observer-owned admission/refresh paths call an internal `prepare_graph_mutation(candidate_hwnd, may_admit)` before native queries, app-key callbacks, or graph calls. It takes a callback-free snapshot, reserves `pending ∪ tracked ∪ candidate ∪ free_entry_headroom`, drains only as required, and returns `window_observer_status_t.GraphQueueFull` before the candidate query/mutation if mailbox drain hits the graph ring limit. A successful observer graph mutation is followed immediately by a second callback-free snapshot and atomic tracked-set replacement. No public coordinator admission API exists while bound; this is intentional, because a post-mutation sync cannot reserve capacity in advance. The only public external graph mutation permitted is proof-gated `complete_retire`, followed synchronously by `window_observer_sync_tracked`.

`GraphQueueFull` during pre-admission reserve means the candidate query/admission has not begun; earlier successful mailbox drain items may already have been applied. If QueueFull occurs while applying a Destroy discovered during query, the native query already happened but its facts are discarded and no graph offer occurs. If Destroy is reentered from graph app-key lookup, that graph call may already have committed; the observer applies the mailbox immediately after return, while the coordinator holds its generated events behind the capture barrier. GraphBindingMismatch, InvalidArgument, WrongThread, Reentrant, and ordinary BadState failures are preflight/local and leave observer and graph unchanged. GraphError before a mutation leaves the old snapshot/evidence unchanged. Any impossible result after mutation latches TrackingFault, which is terminal. `TrackingFault` returns thereafter leave graph/mapping/resource ownership unchanged and require quarantine/retention. HookFailed/UnhookFailed may coexist with newly captured mailbox entries; their documented state is FailedHooks or StoppedBound as described below, not rollback of OS-delivered evidence.

## Observer state transition table

| Current state | Operation/event | Guard and action | Next state | Error/output guarantee |
|---|---|---|---|---|
| CreatedUnbound | bind_graph(g,e) | Validate owner thread/epoch, atomically invalidate Active+Deferred graph state, then take callback-free snapshot and store immutable borrowed binding | CreatedBound | QueueFull leaves graph/observer unchanged; impossible post-invalidation snapshot failure poisons and retains |
| CreatedUnbound | start/pump/refresh/sync | No binding exists | CreatedUnbound | BadState; no clock, callback, or graph call |
| CreatedBound | start | Publish callback target; enter Starting before first hook install; then install hooks | Starting, then Started on all success | Hook callbacks may add protected destroy records during Starting |
| Starting | accepted tracked/candidate Destroy | Merge `(HWND, stamp, event_time)` into protected mailbox; ordinary hints only latch refresh | Starting | Successful enqueue retains exact evidence; capacity exhaustion marks TrackingFault rather than overwrite |
| Starting | all hooks install | Keep target published | Started | No output-bearing API result changes |
| Starting | install failure, all installed hooks removed | Clear target only after all unhooks; wait in-flight callbacks; preserve mailbox | StoppedBound | Return HookFailed; evidence is retained for retry |
| Starting | rollback unhook failure | Keep target/tokens; process only protected destroys | FailedHooks | Return UnhookFailed; no evidence cleared |
| Started | prepared observer-owned admission/refresh succeeds | Pre-reserve union/free headroom; perform query/mutation; callback-free snapshot and atomic tracked-set replacement | Started | Candidate proceeds only after reservation; snapshot failure instead poisons |
| Started | pre-admission reserve cannot drain due graph QueueFull | Do not query/mutate candidate; preserve remaining mailbox | Started | GraphQueueFull; earlier successful drain work may remain applied |
| Started | exact-proof complete_retire externally applied, then sync_tracked | Snapshot and atomically replace tracked set; no external admission is allowed | Started | Snapshot failure poisons; no output scalars are exposed |
| Started | stop | Atomically invalidate Active+Deferred graph state before entering Stopping/unhooking | Started on QueueFull; otherwise Stopping | QueueFull leaves graph/hooks/state unchanged; successful invalidation retains Retiring state |
| Stopping | all hooks removed | Unpublish target; wait for in-flight callbacks | StoppedBound | Preserve mailbox/tracked set; no host-drain claim |
| Stopping | any unhook fails | Retain target and failed tokens; continue accepting protected destroys only | FailedHooks | UnhookFailed; no evidence cleared |
| FailedHooks | stop retry succeeds | Remove remaining hooks, then unpublish target and wait for callbacks | StoppedBound | Preserve mailbox/tracked set |
| FailedHooks | start/pump/admission | No new starts/admissions; only stop retry and protected callbacks allowed | FailedHooks | BadState or HookFailed, no graph mutation |
| Any bound non-poison state | unexpected post-mutation snapshot failure / impossible capacity invariant | Latch terminal fault; callback path becomes evidence/fault-only; coordinator quarantines and retains old bundle | TrackingFault | TrackingFault; no new admission, release, output mutation, or destruction |
| StoppedBound | same-binding start | Initial snapshot is revalidated before publishing target; then same Starting path | Starting | Existing mailbox and graph epoch persist |
| StoppedBound | destroy, local conditions unmet | Hooks/inflight/mailbox/tracked HWND or fault remain | StoppedBound/TrackingFault | ProtectedStatePending or TrackingFault; handle/object unchanged |
| StoppedBound | destroy, local conditions met | Free observer allocation only; graph/backing untouched | Destroyed (caller handle NULL) | Ok; not a host-drain proof |
| Any state | graph pointer/epoch differs from immutable binding | Reject before clock, callback, graph operation, or output write | Unchanged | GraphBindingMismatch |
| Any state | disconnect/EOF/Closed/new token | No implicit graph/observer/mailbox reset | Unchanged | Existing control-peer semantics; old bundle remains retained |

The implementation should keep numeric values of existing public states/statuses stable; append only the specific values listed in the exact-values section below.

## Native fixture and test migration

1. In `tests/interop/window_observer_windows.c`, extend the API-double fake-hook installer so installation can synchronously call the registered hook callback with `Destroy` for a pre-bound tracked HWND (now Retiring after bind invalidation). Assert the protected mailbox contains the event while state is Starting and it is later drained exactly once.
2. In `tests/interop/window_observer_windows.c`, add partial-install failure after that callback; make the first unhook attempt fail and the retry succeed. Assert target/tokens remain valid in FailedHooks, evidence is not cleared, and restart retains it.
3. In `tests/interop/window_observer_windows.c`, add unhook reentrancy: `remove_hook` sends Destroy before returning. Test both all-unhook success and one-unhook failure; successful stop transitions only after callback quiescence; failed stop remains FailedHooks with mailbox intact.
4. In `tests/interop/window_observer_windows.c`, add a graph fault-injection seam for the callback-free post-mutation snapshot only. After forcing failure, dispatch additional hook callbacks and assert permanent TrackingFault, no further graph admission/release, no mailbox clearing, and no observer destroy.
5. Exercise `bind_graph` on a prepopulated Active/Retiring graph before `start`; verify Active state is atomically invalidated to Retiring, Deferred state is cleared, and the initial snapshot contains all old Retiring HWNDs; verify start without binding rejects without installing hooks; verify a different graph pointer or epoch on each pump/refresh API returns GraphBindingMismatch and preserves sentinels/state.
6. Assert forbidden external admission paths in API tests/docs: coordinator uses observer admission only. Test `complete_retire` plus immediate `sync_tracked` removes a key only after its existing proof; no callback-capable operation intervenes.
7. Test capacity at 129, forced QueueFull backpressure, 64-entry Deferred flush with reserved headroom, in-flight stamp replacement during the clock callback, and snapshot-count inconsistency. Any failed reserve must not run the fake native query or app-key callback.
8. Destroy tests: reject while hook installed, callback inflight, mailbox pending, poisoned, or Active/Retiring graph entries remain; on success null only the observer handle and verify the borrowed graph bytes/state and backing owner are unchanged. Test description must state this is local quiescence, not host drain.
9. In `tests/interop/window_observer_native_windows.c`, use the existing Create/Destroy WinEvent fixture and fake hook reentrancy tests; do not infer OS-delivered event guarantees from synthetic callback injection. Report native callback behavior separately from deterministic state-machine proof.

## Stop/restart gap rule (supersedes the simpler state rows above)

Keeping the same observer and mailbox across stop is necessary but does not preserve incarnation continuity while hooks are absent. Before any unhook attempt, `window_observer_stop` must call `interop_window_graph_invalidate_all` on the immutable bound graph/epoch with the callback target still published. The graph call is all-or-nothing. If it returns QueueFull, observer stop returns the existing `WindowObserverStatusGraphQueueFull`; observer remains Started, all hooks/target remain live, and the graph is unchanged. After the caller drains graph events (without treating mere invalidation as frame-release proof), it retries. On success, all Active entries are Retiring and Deferred records are cleared. Only then state becomes Stopping and hook removal begins. Callback Destroy events during unhook are accepted into the mailbox; no admission is permitted while Stopping/FailedHooks. Successful stop preserves those events and Retiring resources. Same-binding restart re-snapshots before hooks and may admit only after fresh native queries following hook installation; it does not reuse pre-stop facts. Retiring generations and backing persist until exact release proofs.

`window_observer_bind_graph` also calls `invalidate_all` before committing its initial snapshot/binding. This makes attachment safe even if the graph contains Active or Deferred records from an interval not observed by this object. Graph QueueFull means no binding and no graph mutation; return WindowObserverStatusGraphQueueFull and retry after event drain. The caller must suspend capture/admission while bind is pending. If invalidate_all succeeds but the subsequent callback-free snapshot fails or is inconsistent, latch TrackingFault permanently; never claim an unchanged graph, and quarantine/retain the graph and native bundle. Successful binding stores the exact pointer and epoch and cannot be reset. This invalidation retires old Active state only; it is not a release, detach, or host-drain proof.

Update the transition table: `CreatedUnbound -> bind_graph` has an internal invalidate_all stage before the successful snapshot; `Started -> stop` has an invalidate_all stage before `Stopping`; either QueueFull returns GraphQueueFull without state change, hook change, output change, or graph mutation. During `Stopping` and `FailedHooks`, admissions/pump/refresh are rejected, while hook callbacks may only preserve protected Destroy evidence. Add fixture cases for bind QueueFull/retry, stop QueueFull/retry, graph event drain without early release, invalidation leaving exact Retiring backing intact, and post-stop same-epoch restart requiring fresh queries before new admissions.

## Reentrant destroy between query and graph offer

The admission reservation prevents mailbox overflow but does not by itself order graph events. A Destroy for the tracked/candidate HWND can be delivered reentrantly from `query_facts` or app-key lookup after the observer stamped the facts but before `observe` / `refresh_offer` commits them. A previously captured snapshot must never be offered after that newer Destroy, or an Announce could be emitted for a known-destroyed HWND.

After every callback-capable query returns and before `observe` or `refresh_offer`, the observer checks for a newer protected Destroy for the candidate. If present, it applies that exact `(window, destroy_stamp, event_time)` with `note_destroyed` first. On success, it discards the pre-callback facts/query result and does not offer, capture, or publish from them. The existing monotonically increasing observer stamp ensures the destroy is newer than the query snapshot. For a previously untracked candidate, `note_destroyed` may return Ok/Unchanged and advance graph stamp/tick state without creating a record; the old facts are still discarded, and only a subsequent fresh native query with a later stamp may admit the HWND. For a tracked key, the graph's destroy timestamp/stamp rules decide whether it retires; regardless, the old pre-callback facts are not reused in this pass. During refresh, abort or continue the refresh without offering that old snapshot; do not mark the key seen from stale pre-destroy facts.

If applying that mailbox event returns QueueFull, keep the mailbox item and suppress the candidate offer. Return WindowObserverStatusGraphQueueFull. The coordinator may drain graph events only into an ordered, bounded staging area that does not expose Announce/Update to capture or transport; it must preserve their order for later processing. After graph ring space is available, the observer retries the exact destroy. Only after the protected mailbox is drained for a key may the coordinator consume staged events for that key or initiate capture/publication. A pending Destroy blocks event consumption for its HWND, including Announce already enqueued before the destroy callback. No event for that key may escape the barrier.

Deferred children flushed by a parent offer are cached graph facts, not fresh native validation. The observer's pre-admission reserve only protects mailbox capacity; it does not prove those child facts still describe live windows. Before any flushed child can reach capture/publication, the coordinator must apply its mandatory fresh native validation gate to that HWND and require no pending protected Destroy for it. If query/Destroy reentrancy happens at that gate, discard the stale validation result and retry only with a later query. This is coordinator capture policy, not a property claimed by the observer or graph.

Add regressions where query_facts reenters with Destroy and returns Exists, where app-key lookup reenters with Destroy, and where refresh_offer sees the same sequence. For query_facts reentrancy, assert note_destroyed precedes any offer and old facts are discarded. For app-key lookup reentrancy, observe may already have committed before note_destroyed can run; assert the observer applies the mailbox item immediately after the graph call returns and the coordinator stages, but does not publish/capture, the generated event until the post-call destroy is resolved. If the sequence contains Announce/Update followed by RetireIntent for the same generation, suppress the unsent Announce/Update. QueueFull retains the event and blocks event consumption; a later fresh query can safely admit/update a still-existing HWND. Also test an offer that flushes Deferred children: each child must pass fresh validation and mailbox-barrier checks before any capture action.

## Exact public lifecycle state values and output behavior

Keep existing public lifecycle values Created=1, Started=2, Stopped=3, Failed=4 unchanged. Append `WindowObserverStateStarting=5`, `WindowObserverStateStopping=6`, and `WindowObserverStateTrackingFault=7`. `Created` with no stored graph is unbound; `Created` after successful bind is bound (no extra numeric state is needed). `Failed` means hook tokens remain after failed install rollback or stop and is the existing state used for `FailedHooks` semantics. Append status values without renumbering: `WindowObserverStatusGraphBindingMismatch=16`, `WindowObserverStatusProtectedStatePending=17`, `WindowObserverStatusTrackingFault=18`. Do not add a separate ProtectedBackpressure status: event-ring pressure is reported as the existing `WindowObserverStatusGraphQueueFull`, with `last_graph_status=InteropWindowGraphStatusQueueFull`.

The new list API's error set is the existing graph status enum: InvalidArgument, WrongThread, Reentrant, StaleEpoch, plus any existing lifecycle error required by `enter`; it does not invent a graph status. It never partially writes. The observer binding, sync, and destroy APIs have no scalar output except the existing destroy handle slot. Their status is the only output. All validation failures happen before observer fields or caller handle change. `window_observer_destroy` writes `*observer_slot=NULL` only after hooks are absent, callback target is unpublished, in-flight count is zero, no poison is latched, mailbox is empty, and list_tracked_windows reports zero Active/Retiring HWNDs. The graph is borrowed and never destroyed by it.

`window_observer_start` and `window_observer_stop` preserve their existing signatures. Start requires a successful fixed binding. Stop calls graph `invalidate_all` before changing state or removing hooks; QueueFull leaves hooks, callback target, observer state and graph unchanged and returns the existing GraphQueueFull. On invalidation success it retains all Retiring resources and then performs callback-safe unhook. A failed hook removal returns UnhookFailed with Failed state and callback target still active for protected Destroy records. Stop does not claim that the host drained any Retiring entry.

## Capture/event barrier rules

The graph ring is bounded (`InteropWindowGraphEventCapacity` 256). While a protected Destroy is pending, the coordinator may need to free graph-ring room so `note_destroyed` can complete. It therefore stages graph events in a bounded queue of at least one full graph-ring capacity and preserves order. Staging is not external consumption: no Announce/Update can reach transport, capture, or a surface importer until the corresponding protected-key barrier is clear. If staging fills while the destroy is still pending, stop graph admissions and apply ordinary coordinator backpressure; retain mailbox, staged events, graph, and mappings. Do not discard or overwrite the oldest staged item.

After mailbox processing, resolve staged events by `(HWND, generation)` before delivery. If an unsent Announce/Update is followed by RetireIntent for the same generation, suppress the unsent Announce/Update and use the registry's never-attached completion path only if its own exact contract permits it; do not fabricate proof or free a mapping based on this graph event alone. If event resolution needs native release/callback proof, retain the generation until that proof arrives. For a Destroy rejected by graph timestamp rules, discard any pre-callback facts, run a fresh native query after the destroy stamp, and admit only those new facts. This gate applies to enumeration facts, ordinary dirty queries, refresh offers, and cached Deferred children.

The coordinator must perform mandatory fresh native validation of every Deferred child before capture even when the parent's offer flushed it into Active. Neither the graph snapshot nor the observer's capacity reservation certifies cached child facts as live. A pending Destroy for that HWND blocks the validation result if it arrived during the query; discard that result and requery after the mailbox has been applied. The observer reports only delivered hints and graph bookkeeping; it does not provide a native incarnation discriminator.
