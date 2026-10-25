# Keyboard shortcut-inhibit policy reducer (TODO #15 shortcut inhibit)

`src/interop/keyboard_inhibit.zig` with C header `src/interop/keyboard_inhibit.h`
implements the decision logic for `zwp_keyboard_shortcuts_inhibit_v1` from
IMPL_DESC 3.6.3 "Shortcut inhibit" and 3.10.5. It does not bind the Wayland
protocol, create or destroy objects, detect the escape chord, or inject input.
The caller maps `apply` results onto compositor objects. The module returns
ordered actions and a forwarding flag.

## Protocol and spec facts used

- `input.inhibit_shortcuts` is `never`, `fullscreen` (default) or `focused`
  (IMPL_DESC 3.1). They map to `mode_t` values `Fullscreen` (0), `Never` (1) and
  `Focused` (2). Fullscreen is the zero value, so zero-filled state starts with
  the default policy.
- `focused`: the inhibitor is requested for the focused surface.
  `fullscreen`: requested only while the window is fullscreen or pointer-locked.
  `never`: never requested.
- 3.10.5: while `inhibit_shortcuts=focused`, host IBus/Fcitx shortcuts are
  inhibited for Waddle surfaces through `zwp_keyboard_shortcuts_inhibitor_v1`.
- 3.6.1: exactly one exported window holds input focus. The module tracks one
  focused identity; an inhibitor belongs to that identity only.

## Items the spec does not define

- **The escape chord is not specified.** IMPL_DESC names a user override but not
  its key combination. The module takes an abstract `user_toggle` event. The
  caller detects the chord and sends one `user_toggle` per press. Defining the
  chord is an open item for the lead.
- **Forwarding rule.** The spec does not state when Super and Alt-Tab reach the
  guest. The module derives it: forward exactly when the inhibitor is reported
  active. An absent or inactive inhibitor leaves shortcuts with the compositor.
  The override, which destroys the object, therefore also stops forwarding.
- **Fullscreen and pointer lock** come from independent sources (the xdg
  fullscreen state and the pointer-constraints lock). The policy ORs them into
  one "fullscreen or locked" condition, and the two flags are stored separately.

## Public API (Zig)

| Symbol | Kind | Meaning |
|---|---|---|
| `MaxTrackedWindows` | const usize | 16; bounded remembered-window table |
| `HalfGenerationSpace` | const u32 | 0x80000000 |
| `MaxActions` | const usize | 2 |
| `error_t` | error set | `InvalidIdentity`, `WindowTableFull` |
| `mode_t` | enum(u8) | `Fullscreen`=0 (default), `Never`=1, `Focused`=2 |
| `identity_t` | struct | `window: u64`, `generation: u32` |
| `phase_t` | enum(u8) | `None`=0, `Pending`=1, `Active`=2 |
| `action_kind_t` | enum(u8) | `Create`=1, `Destroy`=2 |
| `action_t` | struct | `kind`, `identity` |
| `result_t` | struct | `actions: [2]action_t`, `count: u8`, `forward_shortcuts: bool` |
| `surface_t` | struct | `identity`, `fullscreen: bool`, `pointer_lock: bool` |
| `event_t` | tagged union | `focus_enter`, `focus_leave`, `inhibitor_active`, `inhibitor_inactive`, `surface_state`, `user_toggle`, `window_destroy`, `configure` |
| `entry_t` | extern struct | one remembered window: `window`, `generation`, `fullscreen: u8`, `pointer_lock: u8`. 16 bytes |
| `state_t` | extern struct | `config`, `phase`, `focused`, `override` (u8 each), `window`, `generation`, `table: [16]entry_t`. 280 bytes |
| `apply(state, event)` | fn | policy step (below) |
| `init(state)` | fn | zero state |
| `generation_newer(candidate, current)` | fn | wrap-safe generation order |
| `status_t` | enum(c_int) | `Ok`=0, `NullArgument`=1, `InvalidIdentity`=2, `InvalidKind`=3, `InvalidMode`=4, `InvalidFlag`=5, `InvalidState`=6, `WindowTableFull`=7 |
| `event_kind_t` | enum(u8) | 0 FocusEnter, 1 FocusLeave, 2 InhibitorActive, 3 InhibitorInactive, 4 SurfaceState, 5 UserToggle, 6 WindowDestroy, 7 Configure |
| `event_abi_t` | extern struct | `kind`, `mode`, `fullscreen`, `pointer_lock` (u8 each), `window`, `generation`. 24 bytes |
| `action_abi_t`, `result_abi_t` | extern structs | 24 and 64 bytes |

C ABI: `waddle_keyboard_inhibit_init(state*)` and
`waddle_keyboard_inhibit_apply(state*, event*, out*)`. All pointers are nullable.
A NULL returns `NullArgument`.

## Input validation at the ABI

Every byte the caller supplies is checked before it is read as a typed value:

- `kind` must be 0 through 7 (`InvalidKind`). `mode` must be 0 through 2, and is
  used only by Configure (`InvalidMode`).
- `fullscreen` and `pointer_lock` must each be 0 or 1 for every kind
  (`InvalidFlag`). Fields a kind does not use must be zero.
- The state is checked before the event is applied: `config`, `phase`, `focused`
  and `override` must be in range, and every table entry's flags must be 0 or 1
  (`InvalidState`). A corrupt state therefore never reaches typed code.
- Zero window or zero generation: `InvalidIdentity`.

Output and state change only on `Ok`.

## State model

- `focused`, `window`, `generation`: the one focused identity.
- `override`: the escape-chord toggle. Cleared on each new focus.
- `phase`: the live object for the focused identity (`None`, `Pending`, `Active`).
- `config`: the policy.
- `table`: fullscreen and pointer-lock flags of up to 16 windows, kept **whether
  or not the window is focused**. A window's flags are read on focus enter, so a
  window fullscreened while unfocused gets its inhibitor as soon as it is focused.
  An entry is keyed by window and holds the newest generation seen. A newer or
  equal generation replaces the flags, and an older one is ignored. Window 0
  marks a free slot.

Derived rule: the object exists exactly when
`focused and not override and wanted(config, fullscreen or pointer_lock)`, where
`wanted(Fullscreen, f) = f`, `wanted(Never, _) = false` and `wanted(Focused, _) = true`.
The flags are those of the focused generation. An entry for another generation
reads as neither flag. Forwarding is `phase == Active`.

## Events

`apply` takes one event and returns the ordered actions and the forward flag.

- `focus_enter(id)`: rejects zero identity. A duplicate of the focused identity
  does nothing. The same window with a generation that is not newer does nothing.
  Otherwise the previous focus is released (Destroy if an object exists), the new
  focus is set with `override` cleared, and the object is reconciled from the
  table. A focus change emits at most two actions, Destroy then Create.
- `focus_leave(id)`: rejects zero identity. If `id` is the focused identity,
  release the focus (Destroy if an object exists). The table entry is kept.
- `window_destroy(id)`: rejects zero identity. Forgets the window's entry when the
  destroyed generation is not older than the remembered one. If `id` is focused,
  release the focus. If the focused window's entry was forgotten but the focus is
  a different generation, reconcile.
- `inhibitor_active(id)`: if `id` is focused and the phase is `Pending`, move to
  `Active`. The caller must deliver only events from the currently live object.
- `inhibitor_inactive(id)`: if `id` is focused and the phase is `Active`, move to
  `Pending`.
- `surface_state(surface)`: rejects zero identity. Stores the flags for the
  window (`WindowTableFull` if the window is new and the table is full, with no
  change). If the focused window is the same window, in any generation, reconcile.
  Reconciling for any generation of the focused window is required, because a
  newer generation can overwrite the flags the focus was using.
- `user_toggle(id)`: if `id` is focused, flip `override` and reconcile. Turning
  the override on destroys an active or pending object, and turning it off creates
  one in `Pending`.
- `configure(mode)`: set the policy and reconcile.

Each event produces at most two actions. Each identity-carrying event rejects a
zero window or generation before any change. Stale and duplicate events return
`Ok` with no actions.

## Ownership, lifetime and threading

- `state_t` is caller storage of 280 bytes. Zero-filled storage is valid (policy
  Fullscreen, unfocused, nothing remembered). There is no allocation and no free
  function.
- Fields are private. The C header marks them private, and the ABI validates them.
- The state belongs to the single owner thread that receives focus, surface,
  override and compositor inhibitor events.
- No function allocates, logs or performs I/O.

## Error codes

| Status | Trigger | State change |
|---|---|---|
| `NullArgument` | C NULL pointer | none |
| `InvalidIdentity` | window 0 or generation 0 | none |
| `InvalidKind` | kind byte not 0 through 7 | none |
| `InvalidMode` | Configure mode byte not 0 through 2 | none |
| `InvalidFlag` | a flag byte not 0 or 1 | none |
| `InvalidState` | a state byte out of range | none |
| `WindowTableFull` | new window needs a slot, none free | none |

## Verification

Zig built-in tests in `keyboard_inhibit.zig` (run by `make interop-keyboard-lanes-test`):

- Zero state is the Fullscreen policy and creates nothing until a window is
  fullscreen.
- Focused policy: create on enter, duplicate ignored, destroy on leave, second
  leave ignored.
- Fullscreen flag set while unfocused creates the object on the next enter.
- Refocus of a fullscreen window recreates the object. This is the regression
  reported in review.
- Pointer lock alone triggers the policy. The two flags are ORed: clearing one
  while the other holds keeps the object.
- Never policy requests nothing and forwards nothing.
- Active and inactive reports: transitions and forwarding.
- User override: destroy on toggle on, create on toggle off.
- Policy changes while focused reconcile in both directions.
- Focus moving between windows: Destroy then Create. Stale events are no-ops.
- Flags of an unfocused window are used only for that window's focus.
- Generations: older enter stale, newer enter replaces focus. A stale surface
  state is not remembered. A newer generation of the focused window recomputes the
  focused flags (regression, failed before the fix). Destroy of a newer generation
  of the focused window releases the inhibitor (regression).
- Wrap-safe generation order and the 2^31 boundary.
- Destroy exactly once. Destroy forgets the window's flags. Destroy of an older
  generation keeps the newer entry.
- Table capacity: 16 windows fit; the 17th new window returns `WindowTableFull`
  with no change; a destroy frees its slot.
- Zero identity rejected for every identity-carrying event with no change.
- Randomized 20000-step sequence of mixed events, including rejected and
  unfocused-surface events, checked against a reference model that keeps its own
  per-window records.
- ABI struct sizes: entry 16, state 280, event 24, action 24, result 64.
- C ABI: every NULL position, every status, the full decode path, and state
  validation for each byte. `status_of` and `encode` are tested with opaque inputs.

`keyboard_inhibit_abi.c` is an independent C consumer. It is built with
`-fsanitize=address,leak,undefined` and run with `detect_leaks=1`
(`make interop-keyboard-lanes-sanitizers`). It checks the 280-byte state and
16-byte entry sizes with `_Static_assert`.

Coverage (`python3 tests/av/coverage.py keyboard_inhibit`), measured at this
commit: line 100.00% (170/170), branch 97.40% (150/154). The tool excluded 49
compiler panic and stack-canary guard branches. That count is higher than before
because the enum conversions and bounds checks of the new table each carry a
safety trap. Those exclusions are the tool's standard handling, not hand-chosen.
The four missed edges are: one unreachable default arm of the reducer's exhaustive
switch, one unreachable default arm of the test reference's switch, one `and`
operand in `reconcile`, and one edge of the exported `init`.

Zig sanitizer runs are not claimed. The Zig suite uses no allocator, so
`std.testing.allocator` and `FailingAllocator` sweeps do not apply.

## Deviations and open items

- The escape chord is undefined in IMPL_DESC (see above).
- The forwarding rule is derived, not quoted from the spec (see above).
- The table bounds the number of remembered windows at 16. A seventeenth window
  that sends surface state while the table is full is rejected with
  `WindowTableFull`, and the caller must treat that as a fault. The table frees a
  slot only when the window is destroyed. Windows that never send
  `window_destroy` keep their slots.
- The caller must deliver compositor active and inactive reports only for the
  object it holds. The module cannot tell two objects for one identity apart.
