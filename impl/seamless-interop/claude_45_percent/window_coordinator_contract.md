# Window Coordinator Contract (guest, portable core)

Scope of this document: `src/interop/window_coordinator.h` and `src/interop/window_coordinator.c`. The
coordinator is the single owner-thread component that connects the window observer, the window graph,
the slot runtime, the window-state reducer and adapter, capture, and the wire. It is written against
seams (`window_coordinator_io_t`, `window_capture_ops_t`) so the Linux suite can drive it with fakes.
The Win32 smoke and the real WGC capture are outside this document (see section 11).

## 1. Scope and boundaries

In scope:
- Admission of a window: observer report -> graph Announce -> slot admission -> adapter admission ->
  reducer -> capture start -> wire Announce.
- Publishing: one slot write per pass, a FrameReady per published slot, host releases that clear pins.
- Retirement: graph RetireIntent -> capture stop -> slot retire -> wire Retire, completed only after the
  host has released every pin of the generation.
- Host frames the guest must act on: WindowConfigure, WindowClose, FrameRelease, and popup frames
  (accepted, counted, no effect). Every other host type is counted as rejected.
- Transport loss, a host-proved reconnect into a fresh epoch, and teardown.

Out of scope:
- Capture itself (`window_capture_ops_t` is a seam; the suite fakes it and records a digest, not pixels).
- The graph's classification rules, the observer's hooks, and the adapter's Win32 calls. Those are
  separate modules with their own contracts.
- Wire framing and payload validation (the codec, `interop_codec.zig`).
- Any Windows runtime execution. The coordinator is compiled for mingw but not executed on Windows.

## 2. Public API

All entry points run on the owner thread that created the coordinator. Every status below is returned
by value; outputs are unchanged on any non-Ok status unless stated.

| Function | Effect |
|---|---|
| `window_coordinator_create(config, out)` | Builds the adapter, graph, observer and slot runtime for `session_epoch`. Starts the observer when `InteropCapWindows` is negotiated. |
| `window_coordinator_destroy(slot, host_buffers_destroyed)` | Frees the coordinator. Requires `1` as proof unless the coordinator is Idle. A Faulted coordinator with proof `1` ends its entries itself. |
| `window_coordinator_tick(c, budget, stats_out)` | One service pass. `budget >= 1` bounds observer keys per pump. |
| `window_coordinator_mark_lost(c)` | Transport loss without waiting for a send to fail. |
| `window_coordinator_reconnect(c, new_epoch, host_buffers_destroyed)` | Fresh epoch after loss. Requires `new_epoch != 0`, not the current epoch, and proof `1`. |
| `window_coordinator_state(c)`, `..._entry_count(c)`, `..._get_stats(c, out)` | Read-only. `state` and `entry_count` return 0 from a foreign thread. |

Status codes: `Ok`, `InvalidArgument`, `OutOfMemory`, `WrongThread`, `Reentrant`, `NoCapability`,
`BadState`, `MissingProof`, `BackendFailed`, `Busy`, `Faulted`, `Lost`. Wire send results:
`Sent`, `Backpressure`, `Disconnected`.

States: `Idle` (no `InteropCapWindows`; nothing observed or sent), `Running`, `Lost`, `Faulted`.

## 3. Ownership and lifetime

- The coordinator owns: the graph, the observer, the slot runtime, the adapter, one reducer per entry,
  the staging ring (256 graph events), the outbound FIFO (256 frames), and the 64-entry table.
- It does not own: the capture ops context, the wire context, the native API table, or the observer API
  table. Those are caller-owned for the coordinator's lifetime.
- Every entry's reducer is destroyed when the entry ends (retirement, phantom, reconnect, destroy).
- Destroy returns `Busy` while entries remain and no faulted proof applies; it never frees on a
  refused path.

## 4. State machine

```
Idle --(never leaves)--
Running --mark_lost / disconnect seen--> Lost --reconnect(new epoch, proof)--> Running
Running or Lost --slot drain or backend refusal--> Faulted   (terminal; destroy ends entries with proof)
```

- `Lost`: capture stopped for every announced entry, slot drain begun, graph invalidated (every Active
  generation becomes Retiring). Nothing is sent. Quarantines stay until reconnect supplies proof. A failed
  invalidation is retried on each pass (`invalidate_pending`).
- `Faulted`: no progress and no sends. `tick` returns `Faulted`.

## 5. Admission and the retire barrier

Admission is driven by staged graph events, processed in order by `process_staging`:

1. A pair already tracked is ignored (`Done`).
2. While any entry is Retiring (the retire barrier), an Announce is held. The barrier is only applied in
   Running; in Lost, announces become phantoms.
3. No free entry: held. The graph exports at most 64 windows, so this does not occur in normal use.
4. A child whose parent is not announced is a phantom.
5. Outbound FIFO full: held (not for phantoms).
6. Slot admission: `Retry` on refusal (resource, not permanent). Up to 64 consecutive retries, then `Faulted`.
7. Adapter admission, observe, reducer create, capture start: any refusal undoes what was taken and makes
   the entry a phantom. A phantom is answered by the graph's RetireIntent and sends nothing.
8. Success: encode and queue an Announce (ascending tag order). The first frame follows on the next publish.

Retirement (`handle_retire_intent`): capture stop, slot retire, reducer destroy, native release, Retiring,
Retire encoded. A RetireIntent for a generation with no entry completes untracked (no wire message).
Held if the outbound FIFO is full.

`progress_retirements` calls `interop_window_graph_complete_retire` only when the entry has no pins and
no child still retiring. Children complete before parents. Completion with `Ok` or `Stale` frees the entry.

## 6. Publish path and pins

`publish_one` chooses a slot not pinned, prepares the write, calls capture `write_frame`, then marks the
writer entered and commits. The slot's pin is set to the frame sequence and a FrameReady is queued.

- Prepare refused: one publish skipped (`frames_skipped`); the frame follows on a later pass.
- Capture write refused: the writer is cancelled and the failure is counted (`capture_failures`).
- Mark-writer refused: the prepared slot is cancelled (`cancel_unentered`) and the coordinator faults.
- Commit refused: the entered writer is cancelled (`cancel_entered`) and the coordinator faults. Without
  the cancel the slot runtime refuses to drain, and teardown leaks it.
- Outbound full: no publish (`frames_skipped`).

A host FrameRelease clears a pin only when slot and sequence match. A release for an unknown generation,
a slot of 3 or more, or a sequence that does not match is ignored or rejected (counted).

## 7. Wire encoding

All encoders write canonical ascending tag order (the codec rejects any other order). Tags used:
Window 1, Generation 2, Parent 3, ParentGeneration 4, Role 5, Rect 6, Title 7, AppKey 8, ProcessId 9,
State 0xA, Dpi 0xB, Slot 0xC, FrameSequence 0xD, MinSize 0x4B, ErrorCode 0x46.

| Message | Sent when |
|---|---|
| Announce 0x100 | Admission succeeds. Always carries Window, Generation, Role, Rect, AppKey, State and Dpi; Parent and ParentGeneration when the window has a parent; Title when non-empty; ProcessId when non-zero; MinSize when either minimum is non-zero. |
| Update 0x101 | An announced generation's rect, state, dpi, title or min size changes. Held if the outbound FIFO is full. |
| Retire 0x102 | RetireIntent for an announced generation. |
| FrameReady 0x108 | A slot is committed. Pins it until FrameRelease. |
| Error 0x0006 | A host frame for an unknown or non-announced window is answered with ErrorCode `InteropErrorStaleWindow`. |

Only the Announce, Update, Retire, FrameReady and stale-error messages are emitted. WindowStateRequest is
not emitted. Observed transitions go through Update (IMPL 4.5).

## 8. Host frames

`receive_host_frames` reads at most 16 frames per pass. A frame whose length does not match its header is
rejected and counted. Dispatch:

- WindowConfigure: state is read (missing state rejects), an optional Rect is parsed, and the reducer is
  planned and executed through the adapter. Unknown windows are answered stale.
- WindowClose: applied through the reducer as a close.
- FrameRelease: described in section 6.
- PopupRepositioned: stale for an unknown or non-announced window, otherwise counted and accepted with
  no effect.
- Any other type: rejected and counted.

Reducer planning is done once per input. An unsynchronized reducer is resynchronized once and planned
again; a failed resync or plan is counted as `reducer_failures` and the input is dropped.

## 9. Transport loss, reconnect, destroy

- A send that reports `Disconnected`, or a receive that fails, moves the coordinator to Lost (counted as
  `disconnects`). `mark_lost` does the same on request.
- Reconnect requires `Lost`, a new nonzero epoch, and proof `1`. It completes every quarantine: each entry
  is ended, graph events are drained, and every Active generation is retired with no wire message. The
  observer's protected Destroy entries are applied first (`drain_observer_destroys`), then the observer is
  destroyed and new backends are created for the new epoch. Admission then re-announces the visible
  windows through the normal refresh.
- A backend refusal during reconnect faults the coordinator. Teardown after that still frees everything.
- Destroy on a Lost coordinator with entries returns `Busy`; the host must reconnect with proof first.
  Destroy on a Faulted coordinator with proof `1` ends every entry and frees the backends.

Teardown rules learned from the seam tests (all enforced in code):

1. The slot runtime refuses `begin_drain` while a record still has a writer entered, and refuses destroy
   until a drain was confirmed. Teardown cancels the entered writer on a commit refusal, and ends each
   announced entry's slot admission before destroy.
2. The observer refuses to destroy while it is started, while protected Destroy entries remain, or while
   the bound graph still lists a window. Teardown stops it, drains protected destroys, completes every
   quarantine, and only then destroys it.
3. A bound observer that never started answers `BadState` to stop; that is treated as already stopped.

## 10. Threading, queues, faults

- Owner thread only: `enter`/`leave` gate every entry point. A foreign thread gets `WrongThread` (and
  zero from the read-only queries). A call from inside the observer's query or the graph's callbacks gets
  `Reentrant` and touches nothing.
- Queues: staging 256 graph events; outbound 256 frames. A full outbound FIFO holds admissions, updates
  and retirements, and skips publishes. The graph's event ring is drained each pass.
- Retry vs hold: a resource refusal at slot admission is a retry (64 in a row faults). A benign hold is
  not counted toward the retry limit.
- Saturating counters: every statistic saturates at `UINT64_MAX`.

## 11. Verification

Commands (run from the worktree root):

```
make interop-window-coordinator-test          # suite, plain
make interop-window-coordinator-sanitizers    # suite, ASan + LSan + UBSan, detect_leaks=1
make interop-window-coordinator-coverage      # gcov line and branch gate, 90 percent
make interop-window-coordinator-windows       # mingw -Werror cross-build of the coordinator object
```

The suite has 31 tests. Each of the following is covered:
- The admission and retire barrier, children before parents, phantoms, the publish path with cancel
  semantics, canonical encoders, stale and popup frames, configure and close through reducer and adapter,
  backpressure order, transport loss and reconnect with proof.
- Every injected seam refusal (`window_coordinator_faults.h`): create per backend, slot admit, prepare,
  writer, commit, cancel, release, retire, drain, confirm; graph create, complete, invalidate, next event,
  drain protected destroys; observer create, bind, start, stop, sync, destroy; adapter create, admit,
  observe, execute, release; reducer create, plan, complete, unsynchronized; writer finish; calloc.
- Random long sequences: 10 seeded rounds of 300 steps. Each step admits or removes windows, reuses an
  HWND, changes titles and widths through hints, releases pins (including bogus releases), sends configure
  and close, applies backpressure and seam faults, disconnects and reconnects. A host model checks the
  wire after every pass: no Update or FrameReady for a pair that was not announced, no duplicate Announce,
  no Retire for an unknown pair. Each round ends with every window removed and every pin released; the
  coordinator must be empty, every announced pair retired, and 64 fresh admissions must all announce
  (slot accounting).

Results on this branch:
- Plain suite: all 31 tests pass.
- ASan + LSan + UBSan with `detect_leaks=1`: all pass, zero leaked bytes.
- Coverage of `window_coordinator.c`: 97.17 percent lines (789/812), 90.14 percent branches (457/507).
- Random rounds (10): 717 announces, 679 retires, 872 frames, 32 updates, 214 hints, 205 Lost passes,
  22 reconnects. Hold and retry counters are exercised by the seam and backpressure tests.
- mingw cross-build (`x86_64-w64-mingw32-gcc -Werror`, `-D_WIN32_WINNT=0x0A00`): compiles.

Uncovered sites (23 lines, 50 branches) are defensive: unreachable argument combinations behind
other checks, saturation at `UINT64_MAX`, the observer's `GraphQueueFull` loop with more than four
attempts, and the graph's `Cycle` and `LookupFailed` mapping (owned by the graph and observer lanes).

## 12. Known limits

- No real capture. The capture fake records a digest of each write, not the mapped pixels.
- No Windows runtime: the coordinator is cross-compiled but not hosted. The Win32 smoke for the adapter
  is the only native execution, and it is run by CI.
- Popups are accepted and counted; they have no effect yet.
- WindowStateRequest is not emitted; observed transitions go through Update.
- Retirement completes only after the host releases every pin. There is no timeout; a host that never
  releases holds its generation until it reconnects.
- Reconnect requires host proof. Transport loss alone never completes quarantines.
- Restore does not leave fullscreen (the adapter's documented deviation).
- The Win32 native table reports min size 0, so minimum sizes reach the wire only from facts supplied by
  the observer (exercised in tests, not from a live window).
- Windows execution of the observer and adapter is covered by their own lanes; this document does not
  claim it.
