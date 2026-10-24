> Unapproved design proposal preserved for lead review. No tracker credit or frozen contract change.

# TODO #5 next slice: pure pinned-slot and growth bookkeeping

## Recommendation

Implement one new allocation-free Zig module, `src/interop/interop_slot_runtime.zig`, for layout-v3 guest-side slot reservation, resize staging, per-slot run switching, release accounting, and session drain bookkeeping. Keep it separate from `src/av/av_layout.*`, `av_environment.*`, `av_guest.c`, and `av_host.c`: those files implement the legacy AV layout and lifecycle. Do not wire it into a running guest/host session in this slice, and do not describe it as live capture, compositor, or reconnect support.

This closes the runtime bookkeeping portion of TODO #5 while leaving actual handshake/mapping ownership and native compositor/capture integration for their own qualification work. It composes the accepted `interop_heap.zig` fixed allocator with the frozen descriptor table and the existing native atomic shim, without changing either wire ABI.

## Ownership and serialization

The caller owns one `slot_runtime_t` for one layout-v3 session. It contains the fixed `heap_allocator_t`, 64 window records, and 192 fixed slot records; it performs no allocation and retains no mapping pointer, OS handle, frame buffer, or host object. All calls are serialized by the guest session event thread. The runtime is the sole source of reservation identity and growth bookkeeping; callers must not call the heap allocator directly for slot allocations or release its `heap_run_t` values independently.

The caller separately owns the mapped shared memory and invokes `interop_native_atomic` for state transitions. Runtime functions do not perform atomic accesses. They accept an operation only at the documented point in the transition sequence: producer-side publish follows successful `Free -> Writing`; host release bookkeeping follows a successful `Consuming -> Free` for the exact `(window, generation, slot, frame_sequence)`; disconnect drain follows producer shutdown and host-side buffer teardown acknowledgement. State in the runtime is not proof that a mapping transition succeeded. Any adapter failure leaves the runtime unchanged.

## Proposed module API

Use public `snake_case` functions and `_t` types, with `PascalCase` constants, matching repository naming rules. Suggested stable surface:

- `slot_runtime_t.init(session_epoch: u64) error!slot_runtime_t`: initialize a fresh empty session; reject zero epoch. The caller supplies an epoch already accepted by the validated layout header and guarantees uniqueness on restart.
- `slot_runtime_t.admit_window(window: u64, generation: u64, width: u32, height: u32, output: *window_slots_t) error!void`: validate identity and frame geometry through `interop_layout.compute_frame_layout`; reserve all three exact page-rounded runs transactionally; assign a caller-selected free window index and its fixed descriptor indices `window_index * 3 + {0,1,2}`. On any failure, runtime and output remain unchanged, and any tentative runs are rolled back. Zero identity/generation and duplicate identity reject.
- `slot_runtime_t.prepare_growth(window, generation, width, height) error!void`: reserve a complete replacement triple before making it pending. On heap pressure, keep the active triple and geometry unchanged and return `HeapFull`; caller retries after releases. Reject a second pending growth request. Same/smaller capacity is a no-op or metadata-only update only if the lead confirms the geometry policy below.
- `slot_runtime_t.current_slot(window, generation, slot, output: *slot_frame_t) error!void`: return the active data offset/capacity and expected generation for a slot eligible to be written. It must not itself claim the shared descriptor.
- `slot_runtime_t.prepare_publish(window, generation, slot, frame_sequence, output: *slot_frame_t) error!void`: validate a Free runtime slot, no outstanding protocol release, and next nonzero sequence; return the active run metadata and record a provisional write claim before the caller attempts `Free -> Writing`. `commit_publish(...)` commits the pin only after metadata/pixels are written and `Writing -> Ready` succeeds. If either atomic transition fails, `cancel_publish(...)` clears only the provisional claim after a successful `Writing -> Free` cancellation. If `Writing -> Ready` succeeds but commit bookkeeping fails, fail the session closed and retain the run; never free/reuse it.
- `slot_runtime_t.validate_release(window, generation, slot, frame_sequence) error!void`: read-only exact match check before `Consuming -> Free`. `note_release(...)` then clears that slot's matching outstanding frame after the native transition succeeds. Duplicate, stale, wrong-slot, wrong-sequence, and wrong-generation releases reject without mutation.
- `slot_runtime_t.reap_growth(window, generation) error!bool`: after each exact successful release, switch only a now-Free descriptor's reservation to its replacement run. Return true only when all three descriptors have switched and every old-run frame is released; only then release the old three runs. A slot still Ready/Consuming on the old run remains pinned and is not retargeted. New-frame production may continue on switched slots while other old slots drain.
- `slot_runtime_t.begin_drain() error!void`: stop admission and new writes, retaining all runs. `slot_runtime_t.confirm_host_drained() error!void`: accept only after all producers joined and host has destroyed every compositor buffer for this epoch; release all active and pending runs and mark runtime drained. Reject until no outstanding frame or provisional writer exists. Reinitialize only by constructing a fresh runtime with a fresh epoch.

Exact Zig error names should be local, documented `error{...}` cases for invalid identity/geometry, duplicate/stale identity, no free window, heap full, pending growth exists, invalid slot/sequence, wrong frame release, outstanding pins, wrong lifecycle state, and allocator corruption. No public ABI additions are proposed.

## State and resize rules

Each active window owns exactly three descriptor indices and three allocator tokens. Each slot records active run, optional replacement run, last sequence, and whether a published frame is outstanding. A successful `Writing -> Ready` publication creates one host pin; an exact `FrameRelease` is the only normal event that removes it. The host first completes `Ready -> Consuming -> Free`, then sends the matching release. Even if the producer observes shared state Free before that control message is processed, runtime policy forbids re-claiming the slot until the matching release has been validated and accounted for. This avoids overlapping old/new sequence bookkeeping and keeps the old run pinned. The runtime must not infer a release from socket loss, heartbeat timeout, window destruction, or a state value observed without a successful native transition.

Growth allocates all replacement runs first. For each descriptor, replacement becomes active only while the old slot is known Free and after its previous frame release has been accounted for. This allows already-switched slots to publish from the new run while other old slots remain pinned. Release old runs only after all three slots have switched and no old frame remains outstanding. A failed replacement allocation leaves the old triple usable and does not partially publish the new geometry. Heap pressure suspends only further frames that require the larger geometry; it never evicts the window or reclaims an old run early.

On disconnect, mark the runtime draining, stop/join guest capture/audio producers, and retain every heap token until the host has destroyed compositor buffers and confirms drain. The caller then retires the runtime and mapping. A reconnect is a newly validated layout session with a fresh epoch and fresh runtime; no old runtime or token is reused. This module does not create, initialize, map, or reset shared memory.

## Acceptance tests

- All 64 window records and all 192 descriptor indices; duplicate identity, zero identity, generation mismatch, exhaustion, and exact output/state preservation on every rejected call.
- Geometry and 2 MiB rounding at minimum, page boundary, max 7680×4320, over-limit/clamped input policy, invalid stride/overflow, and allocations at the final heap page.
- Three-run admission all-or-nothing under fragmentation and near-full heap; prove rollback releases tentative allocations and preserves existing runs.
- Resize with zero outstanding frames; resize with one/two/all three old slots outstanding; release out of order; wrong/stale/duplicate release; sequence monotonicity and wrap/exhaustion policy.
- Growth heap-full failure followed by release and retry; prove old slots stay valid and no old run is released before the final old pin; prove only free descriptors switch and new frames may use switched runs.
- Publication cancellation before Ready has no host pin; failure after a native transition must leave runtime bookkeeping retryable or fail closed without releasing memory.
- Window retirement and session drain while Ready/Consuming/pending growth; reject premature drain, retain old/new runs, then release exactly once after explicit host teardown confirmation.
- Use allocator snapshots and sentinel outputs to prove no mutation on all errors. Run Zig tests with `std.testing.allocator` if the module uses no allocation, plus the project coverage mode at >=90% branches/lines and ASan/LSan/native qualification owned by the lead.

## Explicit exclusions

No C ABI, wire message, shared header, descriptor, audio ring, or protocol changes. No live `FrameReady`/`FrameRelease` message handler, capture worker, Wayland/wl_buffer, mapping validation, header initialization, mmap/driver ownership, heartbeat/liveness decision, reconnect loop, daemon/QEMU lifecycle, or native atomic operation is added. The current `src/av` runtime remains legacy and unchanged. This unit establishes tested bookkeeping semantics for a later integration change; it is not evidence of native Linux/Windows/GPU functionality.

## Lead decisions needed before implementation

1. **Geometry clamping:** §3.3 says dimensions above 7680×4320 are clamped, while allocation input can be interpreted as either already-clamped capture dimensions or raw OS dimensions. Decide whether this module rejects over-limit dimensions and requires the capture layer to clamp, or performs the clamp itself before deriving stride/capacity.
2. **Same-capacity resize:** Decide whether width/height changes that fit the existing `data_bytes` may update the active descriptor in place once its slot is Free, or must stage a full triple replacement. The frozen text specifies allocation on growth beyond reserved size but does not define shrink or same-run metadata transitions.
3. **Sequence reset:** Descriptor validation binds nonzero generation and frame sequence, but the wire text does not explicitly say whether `frame_sequence` is per-window monotonic across slot changes or may reset on a new generation. Recommend strictly increasing per window generation; reset only for a new generation, never wrap.
4. **Window destruction/re-admission:** `WindowDestroy` behavior for in-flight buffers and whether the same descriptor indices may be assigned to a new window generation before all old `FrameRelease`s arrive needs explicit confirmation. Recommend keep descriptor indices quarantined until all old pins drain, then increment generation and reuse.
5. **Release ordering:** Specify whether a host sends `FrameRelease` only after `Ready -> Consuming -> Free` succeeds. Recommend yes; runtime validates identity/sequence before the host transition, then accounts for the release only after successful `Consuming -> Free`. Confirm that the guest waits for the control release message before claiming a now-Free slot, even though the shared state may become Free first.
6. **Host drain acknowledgement:** The contract requires compositor buffers destroyed before unmap but does not currently identify an explicit host-to-guest acknowledgment message. Confirm whether drain acknowledgement is out-of-band local lifecycle evidence or requires a future protocol message. This slice can expose `confirm_host_drained` but must not invent a message.
7. **Resume semantics:** For a guest agent that remains listening after disconnect, decide whether each accepted reconnect always reinitializes the mapping under the device/session lock with a fresh epoch, or can preserve a validated header. Recommend fresh epoch and reinitialization only after all prior host-owned buffers and producers are gone, as §8.1 states.

## Status

Design proposal only. No repository files, ABI, build configuration, tracker, or tests have been changed. No build, VM, GPU, or Git operation was run.
