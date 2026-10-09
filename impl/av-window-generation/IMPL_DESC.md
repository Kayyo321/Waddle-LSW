# Generation-safe AV window lifecycle

## Scope and explicit limits

Bind modern AV Close, Geometry (including minimize/fullscreen), Destroy and input
to the admitted `(window_id, incarnation)` after an **observed** retirement and
readmission. An HWND recycled before the WinEvent retirement callback is observed
is still an unproved native race. FrameReady.sequence remains the video frame
counter; this change does not make frames independently generation/replay safe.
There is no AV-to-WSI binding, HWND-to-swapchain ownership, direct GPU integration,
physical Windows/compositor/RTX acceptance, or new dependency.

## Architecture and ownership

The existing single event thread owns the guest HWND tracker, native controls,
input state, capture state and peer FIFO. The host event thread owns its admission
high-water, Wayland objects, focus, callbacks and buffer leases. `av_identity.c`
contains portable admission and effect-gating logic. Native operations occur only
through the gate's callback after a current pair match; that callback additionally
checks the live HWND's process and validity before changing Windows state.

Wayland windows retain an immutable incarnation separately from mutable geometry
and frame counters. Retired windows may retain busy compositor buffers while a
replacement with the same numeric ID uses a different free pool/window record.
Old releases free only their original slot. A stale lifecycle message changes
neither the replacement, focus, styles, buffers nor pool ownership.

## Wire layout and compatibility

The envelope is exactly 328 bytes. Header magic is u16 LE 0x574c at offset 0,
type u16 LE at 2, payload length u32 LE 320 at 4. Existing fields and offsets
remain unchanged: window_id u64 at 8, x/y i32 at 16/20, width/height u32 at
24/28, flags u32 at 32, DPI u32 at 36, process u32 at 40, buffer index u32
at 44, sequence u64 at 48, damage fields at 56..71, UTF-8 title at 72..327.
Existing IDs 1..12 retain their wire meanings. CreateV2 is ID 13 and its sequence
must be nonzero. CreateV2.sequence and modern lifecycle/control.sequence are the
immutable incarnation. FrameReady.sequence and diagnostic-flash.sequence retain
their frame-counter and RGB24 meanings respectively.

The first Create fixes connection mode. CreateV2 selects modern; Create selects
legacy regardless of whether its sequence is zero or nonzero. Mixed Create types
fail the session. Modern admission tokens strictly increase across the connection;
gaps are accepted, duplicates/regressions fail, live duplicate window IDs fail.
UINT64_MAX can be the last token; attempting another admission is terminal.
Reconnect creates fresh connection-local admission state, never a cross-session
identity guarantee.

New host plus either pre-input or input-era old guest is display-only, with an
explicit diagnostic. No Close, resize/fullscreen or input requests are emitted.
Legacy Geometry and Destroy preserve old display behavior. New guest plus old
host sends CreateV2; the old codec rejects unknown ID 13 and its normal protocol
failure closes the transport. Coordinated upgrade is required; there is no
fallback or round-trip capability negotiation. No first-window capability can
be learned before the first eligible window appears.

The codec remains allocation-free Zig and rejects invalid length/header/field
values transactionally. Since legacy lifecycle sequence zero is wire-compatible,
modern direction endpoints reject zero lifecycle/control tokens before lookup.
Zero is never a wildcard. Invalid direction/type or malformed fields fail even
for absent IDs. A well-formed nonzero stale/absent pair is a successful no-op.

## Execution paths

1. Guest start resets connection counters, installs hooks and enumerates windows.
   Admission reserves a fresh nonzero token and emits CreateV2 with a free pool.
   Pool exhaustion may defer Create before publication; queue exhaustion cannot.
2. Geometry copies the admitted token. Retirement emits Destroy with that token,
   releases held input for that identity and retires capture. Reappearance obtains
   another token, even when Windows assigns the same HWND value.
3. Host validates Create capability/high-water and pool ownership before creating
   Wayland resources. Modern callbacks copy the immutable token into controls.
4. Guest controls validate and compare current identity before calling the native
   effect callback. The callback rechecks HWND process/liveness. Stale requests
   produce no effects; native failures terminate the session.
5. Guest Geometry/Destroy validation and current-pair matching precede host
   Wayland/focus/buffer effects. A legacy connection never gains controls from a
   nonzero field in an old message.
6. Retired, shutdown, delivery-failed and legacy callbacks cannot emit requests.
   Async import completion still disposes its original unpresented buffer and
   returns its original slot. Submitted busy buffers wait for wl_buffer.release.
7. Lifecycle/control/input queue failure is terminal. Guest latches failure and
   stop event immediately and checks it between message dispatch, receive, refresh
   and capture. Subsequent callbacks cannot enqueue or apply another mutation.
   The existing drop-busy/drop-full policy remains only for video frames.
8. Teardown disables callbacks, releases input, restores live tracked fullscreen
   overrides, drains compositor ownership for the existing bounded timeout and
   frees resources symmetrically. It cannot promise recovery of missing native
   callbacks or compositor releases after a dead compositor.

## Concurrency, errors and memory

No locks, heap ownership or threads are introduced by the portable identity core.
Its callers serialize access on their existing event thread. Existing slot atomic
acquire/release ownership is unchanged. A socket/session failure is not recoverable
inside that session. A new session starts with zeroed mode/high-water. No counter
wrap is accepted. Format errors return -1 without changing output; stale controls
return success without invoking the effect callback. Busy pool alias admission is
rejected so old compositor ownership cannot be reinterpreted as the new window.

## Verification

Codec golden bytes cover new type/token offsets plus truncated/malformed
transactional rejection. Portable identity tests count native effect callbacks
for A-retire/B-readmit same numeric HWND, stale close/geometry/minimize/fullscreen,
matching B controls, absent IDs, zero, wrong process and invalid types. Admission
tests cover mode mixing, zero/regressed/duplicate/exhausted tokens and reconnect.
Wayland callback/state tests cover old busy leases, unchanged replacement focus,
stale Destroy/Geometry, legacy controls and retired/shutdown/failed callbacks.
Socket fixtures cover fragmentation, cross-direction FIFO races, queue overflow,
partial EOF and both compatibility directions using the historical codec source
extracted from a pinned repository commit at build time, never a copied dependency.
Full AV/input suites, >=90% production codec and identity line/branch coverage,
strict Linux host link and Windows guest/native fixture cross-link are required.
ASan/UBSan and detect_leaks=1 CI remain gates. Native Windows/compositor/RTX tests
remain explicit manual gates; cloud API doubles and cross-linking do not replace
those tests. Evidence is recorded in TRACKER.md as checks actually finish.
