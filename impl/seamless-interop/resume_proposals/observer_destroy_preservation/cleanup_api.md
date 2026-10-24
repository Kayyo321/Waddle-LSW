# O5 cleanup addendum: stopped drain and poisoned-owner handoff

This is an advisory addendum to `/tmp/seamless_priority_destroy_contract.md` and `/tmp/seamless_priority_destroy_api.md`. It does not authorize freeing a graph entry, surface, descriptor, mapping, native backing, or old bundle. It adds only deterministic ownership rules for observer-local memory and retained evidence. It does not create, encode, or imply a v2 host-drain receipt.

## 1. Ownership split

The observer owns its heap allocation, fixed ordinary dirty FIFO, tracked-key snapshot, and fixed protected-destroy mailbox. The coordinator owns the graph allocation, graph entries/backing, native resource bundle, and a fixed quarantine-record slot that outlives the observer. The graph pointer stored in that record is diagnostic binding metadata only; it is borrowed and never becomes ownership of the graph.

A successful local observer destroy frees the observer allocation only after the ordinary proof in the frozen proposal (no hooks, unpublished callback target, zero in-flight callbacks, no fault, empty mailbox, empty Active/Retiring snapshot). It never frees the graph or native resources. A quarantine handoff may also free observer-local storage from a healthy StoppedBound observer when the caller permanently suspends and retains the old bundle; this avoids retaining an observer solely because graph/native backing cannot yet be released. From TrackingFault it requires the same evidence handoff plus local callback quiescence. Neither operation permits graph reuse, mapping release, capture, restore, or epoch reset.

The quarantine record is not a proof, token, receipt, restart image, or authority to act on graph/native resources. It can only accompany a permanently suspended owner bundle for diagnosis and evidence retention. There is no import/restore API.

## 2. Fixed caller-owned quarantine record

Expose a public, versioned-by-size C type with no internal pointers except the borrowed graph identity. Suggested declaration shape (all public symbols require the project Doxygen contract):

```c
#define WindowObserverQuarantineVersion 1u

typedef struct window_observer_quarantine_destroy_t {
    uint64_t window;
    uint64_t destroy_stamp;
    uint32_t event_time_ms;
    uint32_t reserved;
} window_observer_quarantine_destroy_t;

typedef struct window_observer_quarantine_dirty_t {
    uint64_t window;
    uint64_t first_stamp;
    uint64_t last_stamp;
    uint64_t destroy_stamp;
    uint32_t kinds;
    uint32_t retries;
    uint32_t destroy_event_ms;
    uint32_t reserved;
} window_observer_quarantine_dirty_t;

typedef struct window_observer_quarantine_record_t {
    uint32_t version;
    uint32_t size;
    interop_window_graph_t *borrowed_graph;
    uint64_t session_epoch;
    uint32_t fault_code;
    uint32_t fault_graph_status;
    uint32_t tracking_fault;
    uint32_t evidence_incomplete;
    uint32_t protected_count;
    uint32_t dirty_count;
    uint32_t tracked_count;
    uint32_t tracked_snapshot_valid;
    uint32_t reserved;
    uint64_t candidate_window;
    window_observer_quarantine_destroy_t protected_destroys[WindowObserverDestroyCapacity];
    window_observer_quarantine_dirty_t pending_dirty[WindowObserverDirtyCapacity];
    uint64_t last_known_tracked[InteropMaxWindows];
} window_observer_quarantine_record_t;
```

`WindowObserverDestroyCapacity` is the accepted fixed mailbox capacity (129) and `WindowObserverDirtyCapacity` remains the ordinary FIFO bound (512). Copy mailbox slots in stable mailbox order, retaining each exact HWND/stamp/event-time tuple and any in-flight marker/index in an explicit field if the implementation can reach handoff with a mailbox drain in flight. In normal owner-thread execution the busy guard prevents handing off during a drain; still validate/record the in-flight state rather than silently omit it. Copy the complete pending dirty entries, not just Destroy bits, so no retained observer-side event is silently lost when the allocation is freed. `last_known_tracked` is explicitly diagnostic; on terminal fault `tracked_snapshot_valid` is false unless the last snapshot is known complete and exact. Never present a stale set as current graph state.

The record has no `drained`, `safe_to_release`, `new_epoch_allowed`, `restore`, or similar authority field. Include a `tracking_fault` boolean and `fault_code` zero for a healthy stopped handoff. `evidence_incomplete` is set when the tracking fault means one or more delivered Destroy hints could not be retained (for example mailbox capacity exhaustion). Copying a record cannot make incomplete evidence complete. A healthy StoppedBound handoff may still contain Active/Retiring HWNDs and pending evidence; it is allowed only when the caller explicitly commits to permanent suspension/retention of the whole old bundle, not because those entries are safe to release.

The coordinator must allocate this record as fixed storage inside its already-owned quarantine bundle before the observer can fault or be permanently suspended. The handoff performs exactly one `free` of the observer allocation after the successful record copy; it performs no allocation and no other deallocation. The caller retains the full owner bundle and record until a separately accepted external teardown rule authorizes release; absence of such a v2 rule means retain them for the old session/process lifetime.

## 3. Quiesced quarantine handoff API

Suggested API:

```c
/**
 * Copy all observer-retained evidence and binding metadata into caller-owned fixed storage,
 * then free only the observer-local allocation. Does not query or mutate the borrowed graph.
 *
 * @param[in,out] observer_slot Non-NULL owning handle; set to NULL only on success.
 * @param[out] record Caller-owned, correctly aligned, writable record storage. The API validates
 *        disjointness from observer_slot and the known observer allocation. The caller must ensure
 *        it is also disjoint from the opaque graph allocation, backing, and other live objects;
 *        the API must not dereference an opaque graph merely to discover its allocation size.
 *        No storage is retained.
 * @return WindowObserverStatusOk after a complete copy and local free for either a TrackingFault
 *         observer or a healthy StoppedBound observer whose caller permanently suspends/retains
 *         its old graph/native bundle;
 *         WindowObserverStatusInvalidArgument for null/misaligned/invalid output storage;
 *         WindowObserverStatusWrongThread for a non-owner call;
 *         WindowObserverStatusReentrant while an observer operation is active;
 *         WindowObserverStatusBadState unless state is TrackingFault or StoppedBound;
 *         WindowObserverStatusProtectedStatePending while any hook token exists, the active
 *         callback target still names this observer, or a callback is in flight.
 *         Every error leaves observer_slot, observer bytes, and every byte of record unchanged.
 * @thread_safety Owner thread only. No graph, OS, hook, or backing operation occurs; exactly one
 *         free of the observer allocation occurs after the committed record copy.
 */
window_observer_status_t window_observer_quarantine_handoff(
    window_observer_t **observer_slot,
    window_observer_quarantine_record_t *record);
```

Ordering and checks are exact:

1. Validate non-NULL pointers, alignment, representable address spans, and disjointness of the record from the known `observer_slot` and observer allocation before writes. Graph/backing disjointness is an explicit caller precondition because the graph type is opaque and no allocation size is available here. Never attempt a partial copy.
2. Load but do not modify `*observer_slot`. Validate owner thread and entry/reentrancy state. Accept exactly TrackingFault or healthy StoppedBound; for the latter, caller is irrevocably choosing permanent suspension of the old owner bundle.
3. Require every hook token to be absent, the process-wide active-observer slot not to equal this observer, and `g_hint_inflight == 0`. Do not call `release_slot` implicitly here: only successful stop/unhook owns that transition. A remaining hook, published target, or in-flight callback returns ProtectedStatePending with outputs unchanged.
4. Do not call `list_tracked_windows`, `invalidate_all`, `note_destroyed`, graph sync, API callbacks, clocks, allocation, or any graph/native operation during handoff. On the poisoned path the graph may be exactly the source of the fault; on the healthy StoppedBound path the caller is choosing permanent suspension rather than normal destroy. Neither path needs another graph result to detach local storage.
5. Build a full temporary record, validate all stored counts against their fixed bounds, then copy the complete record in one final commit. If count/in-flight invariants cannot be represented, return TrackingFault without writes/free; do not truncate evidence.
6. Null `*observer_slot` and call `free(observer)` exactly once only after the record copy succeeds. The successful output now owns the evidence copy. No cleanup of graph/backing is attempted.

The poisoned object remains available for `window_observer_stop` retries while hooks exist. If a stop retry fails to remove any hook, it must leave the callback target/object valid and return UnhookFailed; handoff then returns ProtectedStatePending and leaves both caller outputs unchanged. After all hook removals succeed, stop may unpublish the target and wait for `g_hint_inflight` to become zero. If TrackingFault is latched, stop retries only hook removal: it must not call graph invalidation, snapshot, sync, `note_destroyed`, or any other graph operation. The fault remains latched; stop must not clear mailbox or dirty evidence. Handoff is then the API that frees the poisoned observer-local allocation. A healthy StoppedBound observer may use the same evidence copy/free path only when its caller keeps the bundle permanently suspended; this is not normal destroy.

Do not permit handoff for a healthy Started, Created, or FailedHooks observer. Healthy normal destroy keeps its stricter graph snapshot / empty tracked / empty mailbox proof. The sole healthy exception is a fully StoppedBound observer whose caller permanently suspends and retains the whole owner bundle. Do not make the quarantine record usable to bind a new observer or continue graph processing.

### Honest stop/destroy status after side effects

Only argument, owner-thread, reentrancy, binding preflight, and `invalidate_all == QueueFull` failures promise that stop has not changed graph or lifecycle state. Once invalidation succeeds, a later hook-removal failure can return UnhookFailed with the graph already invalidated, some hooks removed, and the observer retained in FailedHooks; it must not claim the earlier state was restored. If `window_observer_destroy` performs stop internally and then discovers pending protected evidence or Retiring entries, it may return ProtectedStatePending with `*observer_slot` still unchanged even though hooks were removed and graph invalidation committed. This is an honest retained-handle result, not a transaction rollback. Prefer requiring an explicit successful stop before destroy if the implementation wants destroy errors to remain purely preflight; either contract must state which operations may have committed before ProtectedStatePending.

## 4. StoppedBound protected-destroy drain

A normal stop can preserve a protected mailbox item delivered during unhook, while current pump/refresh are Started-only and destroy requires an empty mailbox. Avoid requiring a hook restart merely to apply retained graph evidence by adding one bounded owner-thread operation, or specify same-binding restart as the only allowed drain route. Preferred operation:

```c
/**
 * Apply retained protected Destroy items to the immutably bound graph without native queries
 * or admission, including while StoppedBound. Clear an item only after exact graph acceptance.
 *
 * @param[in,out] observer Non-NULL bound observer; state Started or StoppedBound only.
 * @return Ok if all currently applicable retained items were applied;
 *         GraphQueueFull if the graph ring blocks further progress (applied earlier items stay
 *         applied, remaining items stay byte-for-byte retained);
 *         GraphError/TrackingFault for unexpected graph inconsistency, preserving remaining
 *         items and poisoning if a mutation may have succeeded but exact state is unknowable;
 *         WrongThread, Reentrant, BadState, or TrackingFault for their respective preflights.
 * @thread_safety Owner thread only; graph clock callbacks may reenter hint recording. Allocation-free
 *         and bounded by mailbox capacity.
 */
window_observer_status_t window_observer_drain_protected_destroys(
    window_observer_t *observer);
```

This operation performs no native query, `observe`, `refresh_offer`, refresh admission, or new Active-entry admission. It does call graph `note_destroyed`; the graph may call its clock callback, which can reenter hint recording. Before each graph call it must preserve the same reserved-union capacity invariant, keep the in-flight mailbox slot reserved, and compare the exact snapshotted stamp before clearing so a newer reentrant Destroy survives. Drain no more than `WindowObserverDestroyCapacity` entries per call. It may run in StoppedBound after successful unhook; it does not reopen callback publication. In Started it may be used as the same priority drain primitive. Do not allow it in Stopping/Failed while hook callbacks can race. If TrackingFault is latched, it returns TrackingFault without graph calls; use the quarantine handoff instead.

Do not invent successful acceptance for an unknown graph status. QueueFull retains the exact current and later evidence; successful accepted stale/unchanged statuses may clear only according to the graph's documented `note_destroyed` acceptance semantics. If exact acceptance cannot be distinguished, retain and fail closed.

This drain does not release Retiring records, emit a host receipt, authorize surface/descriptor release, or claim that a HWND was destroyed by Windows. It only applies already-delivered Destroy evidence to the existing graph. Normal healthy `window_observer_destroy` remains blocked until a fresh callback-free graph snapshot confirms no Active/Retiring HWNDs, the mailbox is empty, hooks are absent, and callbacks are quiescent. Quarantine handoff is a distinct permanent-suspension path and never grants release authority.

## 5. Required tests and failure atomicity

1. **Poison before hooks:** bind invalidation succeeds; injected initial snapshot fails; observer is TrackingFault with no installed hooks/active target/in-flight callback. Handoff copies exact binding/fault fields and any retained mailbox/dirty state, nulls only observer handle, and leaves graph bytes/state unchanged relative to immediately before handoff. This proves the observer-local allocation has a cleanup path without freeing graph/backing.
2. **Poison with hooks:** induce post-mutation snapshot fault while Started; first stop/unhook attempt fails. Assert target and token remain, callbacks can append/merge protected evidence, record sentinel and observer pointer remain unchanged, handoff returns ProtectedStatePending. Retry unhook successfully; assert target unpublished and in-flight count zero; handoff copies evidence exactly and frees only observer.
3. **No graph call on poison handoff:** fake graph call counters remain unchanged across successful handoff; output carries borrowed graph pointer/epoch only as metadata. Verify no graph release, backing release/unmap, or native close callback is invoked.
4. **Evidence overflow:** force capacity exhaustion; quarantine record sets `evidence_incomplete`, preserves all entries that did fit, and never labels the bundle drainable/releasable. Handoff cannot repair lost evidence.
5. **Output sentinels:** null/misaligned record and runtime-valued overlap with the known observer/slot, wrong thread, reentrant call, unsupported state, remaining hook token, active target, and nonzero inflight each return their exact status with record and `observer_slot` unchanged. Graph/backing overlap is a documented trusted caller precondition, not something tested by dereferencing an opaque graph. No invalid test dereferences hostile memory; misalignment cases use a valid containing byte array and check that no bytes are read or written through the misaligned view.
6. **Stopped-bound drain:** deliver protected Destroy during successful unhook; stop leaves it in mailbox. Drain while StoppedBound applies it exactly once without installing hooks or running native queries. A reentrant graph clock callback adds a newer same-key Destroy; the drain clears only the old snapshot. QueueFull leaves remaining tuple/stamp unchanged and retry succeeds after graph event drain. Healthy destroy is then attempted only if graph release proofs actually remove every Retiring entry.
7. **No inferred release:** after stopped drain or handoff, assert graph/native backing owner counters remain live; neither operation reports or synthesizes host-drain proof.
8. **No restart requirement:** `drain_protected_destroys` lets a StoppedBound observer with pending mailbox reach exact graph acceptance without hooks/start. It may invoke only the graph's clock callback, with reserved-slot/reentrant-stamp tests proving evidence is not lost.
9. **Permanent suspension handoff:** healthy StoppedBound with Retiring entries may hand off and free only observer-local storage; graph/backing counters remain live, record carries no restore/release authority, and the caller keeps the entire old bundle. It is distinct from normal destroy and may not be followed by a new observer bound to that graph/epoch.

## 6. Decision boundary

A permanently retained observer allocation is only acceptable if the project explicitly chooses a process-lifetime quarantine exception to the zero-leak rule and accounts for its exact maximum lifetime/count. The preferred split is: retain graph/native bundle forever absent the external gate; transfer observer-local evidence to caller-owned fixed storage once callback-quiesced; then free the local observer. This is memory cleanup only. It must never be described as releasing the old session, graph, surface backing, or epoch.
