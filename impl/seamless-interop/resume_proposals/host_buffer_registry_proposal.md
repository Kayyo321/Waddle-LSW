# Host Wayland buffer registry proposal

## Boundary and source seams

Introduce a small C registry beside the future interop host adapter (suggested `src/interop/host_buffer_registry.c/.h`), then let the VSOCK/Wayland event loop own one registry per mapped session epoch. Do not implement this by extending the legacy `av_wayland_t` ABI in the first slice. The registry is metadata/lifetime bookkeeping; the Wayland adapter remains the only code issuing Wayland requests and dispatching callbacks.

Current behavior to extract carefully:

- `src/av/av_wayland.c:video_buffer_t` owns a `wl_buffer`, optional async dmabuf params, a window/slot pointer, geometry, and a `busy` bit. `video_window_t` embeds three such records (`buffers[3]`).
- `buffer_release` at `src/av/av_wayland.c:427` is the only current callback that calls `av_video_release` for an attached busy frame. `attach_buffer` marks busy before attach/commit (`:438`); `imported_buffer` installs the listener then attaches (`:482`); rejected dmabuf import falls back to shm or returns the slot if it cannot create a buffer (`:499`). `FrameReady` handling starts the read at `:900+` and validates slot sequence/geometry before import/attach.
- `free_window` (`:706`) destroys params and every buffer then clears the window record. `av_wayland_free` invokes `drain_buffers` before freeing windows (`:720+`). `drain_buffers` (`:989+`) stops window admission/retire routes and polls for at most 2 seconds. That legacy timeout is a bounded best-effort shutdown policy; it is not evidence that a compositor released a buffer and cannot authorize unpin/unmap in the interop path.
- `src/vgpu/venus_present.c:present_slot_t` is another useful shape reference: its callback context is stable per accepted slot and retains params/buffer/layout/frame until completion. Its `finish_slot` destroys a buffer before notifying completion, so the new interop contract still must distinguish actual release from local proxy destruction.
- Frozen protocol §4.3 says host sends `FrameRelease` when `wl_buffer.release` arrives. §8.9 says guest clears the runtime pin only for matching `(session_epoch, window, generation, slot, frame_sequence)`, shared `Free` alone is insufficient, and producer shutdown plus explicit host-buffer teardown proof precedes final drain. A protocol proposal from another lane is not assumed accepted here.

## Registry identity, states and ownership

The key for each accepted frame is exactly:

```c
typedef struct host_frame_key_t {
    uint64_t session_epoch;
    uint64_t window;
    uint64_t generation;
    uint32_t slot;
    uint64_t frame_sequence;
} host_frame_key_t;
```

All key components except `slot` are required nonzero; `slot` must be in the frozen 0..2 per-window range. The registry rejects duplicate live keys and rejects reuse of a `(epoch, window, generation, slot)` while its prior frame is pending or attached. It is bounded to the layout's 192 slot descriptors; no unbounded per-frame allocation is needed. Window retirement blocks new imports for that exact epoch/window/generation but does not discard outstanding frame records. A new generation or epoch never aliases an old record.

Each record has an explicit state:

1. `ImportPending`: the frame has been accepted and its IVSHMEM/backing reference is held; async dmabuf params or other import work may still callback. No compositor release can be claimed yet.
2. `Attached`: a `wl_buffer` has a listener whose stable callback context resolves to this immutable frame key, and `wl_surface_attach` plus commit has been issued. Keep the backing reference and proxy record until the exact `wl_buffer.release` callback.
3. `ReleasedPendingNotify`: the matching release callback ran exactly once. This is the only attached-frame transition that permits host slot release and generation of exact `FrameRelease`. Queue/backpressure may delay notification, so retain a small identity/tombstone record until the host's slot-state update and enqueue have both succeeded.
4. `NeverAttachedDone`: async import failed, was rejected, or was canceled before any surface attach/commit. First ensure no later callback can use freed callback context (resolve the params callback or retain a tombstone until proxy/display teardown); then destroy any never-attached proxy and return the exact slot through the host's non-compositor completion path. If the wire contract requires every `FrameReady` to terminate with `FrameRelease`, send that exact key but report internally that it is `never_attached`; never call it a Wayland release. Lead must confirm whether this is the intended wire-visible behavior.

Do not recycle a callback context while a listener or async params callback can still reach it. Prefer fixed stable records with monotonically changing local generations or separate immutable callback nodes; callbacks validate the full key and state before mutation. An old callback after window/epoch retirement must only complete its own record, never a new record that reused the array index.

The record holds a reference to the IVSHMEM mapping/backing lifetime for as long as Wayland may read it. `wl_buffer_destroy`, `wl_surface_destroy`, retiring the window, socket disconnect, EOF, or elapsed time do not decrement the attached-buffer reference. Only `wl_buffer.release` does. Once released, the registry may drop that particular compositor-use reference, but whole mapping teardown still requires the independently specified producer/host drain proof and zero other references.

## API sketch and event-loop order

The registry should remain Wayland-type-free and report actions; the adapter owns proxies:

```c
host_buffer_status_t host_buffer_registry_init(
    host_buffer_registry_t *registry, uint64_t session_epoch,
    host_frame_complete_fn complete, void *context);
host_buffer_status_t host_buffer_registry_begin_import(
    host_buffer_registry_t *registry, const host_frame_key_t *key,
    uint64_t backing_cookie, host_buffer_ticket_t *out_ticket);
host_buffer_status_t host_buffer_registry_mark_attached(
    host_buffer_registry_t *registry, host_buffer_ticket_t ticket,
    void *stable_buffer_identity);
host_buffer_status_t host_buffer_registry_import_failed(
    host_buffer_registry_t *registry, host_buffer_ticket_t ticket,
    uint32_t failure_code);
host_buffer_status_t host_buffer_registry_buffer_released(
    host_buffer_registry_t *registry, void *stable_buffer_identity,
    host_frame_key_t *out_key);
host_buffer_status_t host_buffer_registry_notify_queued(
    host_buffer_registry_t *registry, host_buffer_ticket_t ticket);
host_buffer_status_t host_buffer_registry_begin_drain(
    host_buffer_registry_t *registry, uint64_t session_epoch);
host_buffer_status_t host_buffer_registry_snapshot(
    const host_buffer_registry_t *registry, host_buffer_counts_t *out_counts);
```

The production API should use opaque tickets, never caller-supplied array indices. The callback identity can instead be the stable ticket context passed to Wayland; returned output keys are copied scalars. The exact names are provisional; public declarations need Doxygen ownership/nullability/error/thread docs.

For a valid `FrameReady`, the serialized event loop validates protocol fields and shared descriptor identity, begins a registry record before starting import, and holds the mapping reference before any import can read pixels. An async params callback either resolves to a real `wl_buffer` and then attaches/commits (transition to `Attached`), or reports failure and enters the never-attached path. A release callback checks the exact live attached record, transitions it once, updates host slot state according to the frozen host rule, and attempts to enqueue `FrameRelease` containing the exact five-key identity. If enqueue is backpressured, retain `ReleasedPendingNotify` and retry; do not attach that slot again or lose the release. Once the outbound queue owns the encoded release frame and the shared slot transition is committed, the registry can retire its notification record; socket write completion is not compositor proof and is not required to keep compositor backing alive after the callback.

At drain start, stop accepting new `FrameReady` records first. Continue dispatching Wayland events and completing/retrying imports and releases. An attached record remains owned until its release callback, even if the surface/window is retired. An import still pending at timeout remains outstanding; timeout can return `StillBusy` and preserve mapping/proxies, but cannot synthesize releases or free callback storage. If the Wayland connection is lost, mark callback delivery impossible and preserve unresolved backing ownership until an independent safe-session teardown procedure authorizes ending the mapping lifetime. A fixed timeout is useful only to report drain progress/failure to the caller.

## Invariants to test

- Every accepted frame key has at most one live registry record; one compositor callback can produce at most one exact `FrameRelease`.
- No `FrameRelease` for an attached frame is produced by buffer-proxy destruction, surface destruction, frame callback, output-presentation feedback, timeout, EOF, or window retirement.
- Failed import is explicitly `never_attached`, and its callback/context/backing is retained until the async import cannot callback; any wire release policy is a separately asserted path.
- Drain blocks new imports immediately but keeps every pending/attached record and the mapping reference alive.
- A release for a stale epoch, different generation, wrong slot/sequence, unknown proxy, or duplicate callback changes no new frame state.
- Release notification backpressure preserves exact key and does not repeat the shared slot transition; retries enqueue once.
- Array-index reuse cannot redirect a delayed params/release callback to a different frame or epoch.
- Mapping release is impossible while any `ImportPending` or `Attached` record exists; zero registry records is necessary but not sufficient for full session unmap because producer stop/drain proof is external.

## Test plan

First add a deterministic protocol-double test around the adapter/registry boundary, not a compositor claim. Fake params-created, params-failed, and `wl_buffer.release` callbacks independently; record when buffers are attached, destroyed, backing references acquired/released, host slot transitions occur, and FrameRelease frames are queued. Exercise zero frames, exact key round-trip, duplicate/wrong/stale keys, every import failure point, delayed callbacks after retirement, release exactly once, queue-full retry, drain with pending import, drain with attached buffer, and timeout that leaves all ownership intact. Assert the fake `wl_buffer_destroy` never invokes release and never decreases attached backing count.

Then run an opt-in native compositor test (nested headless Weston or another controlled Wayland compositor) with private shm/dmabuf buffers and a private IVSHMEM fixture. Verify: attach produces no early FrameRelease; destroy/retire/2-second timeout alone produces none; real `wl_buffer.release` produces the exact epoch/window/generation/slot/sequence once; and a delayed release keeps the mapped backing alive. Include dmabuf rejection→shm fallback and connection loss while busy. This validates Wayland behavior only; it does not establish guest-agent acceptance, cross-process detach proof, or safety of unmapping after compositor disconnect.

## Decisions for lead before implementation

1. Freeze what happens on import failure before attach: likely exact FrameRelease is still required to clear the guest pin because the host demonstrably never attached/retained compositor use, but it must be a distinct never-attached completion path and not be described as `wl_buffer.release`.
2. Specify the host-side shared slot transition paired with the callback (e.g. `Consuming -> Free`) and its ordering relative to FrameRelease enqueue. Existing §4.3 names the message but does not fully define callback failure/backpressure ordering.
3. Define final mapping teardown on Wayland connection loss when release callbacks can no longer arrive. Do not infer that disconnect proves compositor stores/access are gone; the outer OS/session boundary needs an explicit accepted proof or must retain/quarantine backing.
4. Decide whether this registry is a new interop component first, with `av_wayland.c` unchanged, or a shared extraction that legacy AV also adopts. Preserve legacy behavior unless that extraction is separately reviewed and tested.

This proposal adds no wire message and makes no claim that proxy destruction, a 2-second wait, Wayland disconnect, or transport loss proves compositor/guest detachment.
