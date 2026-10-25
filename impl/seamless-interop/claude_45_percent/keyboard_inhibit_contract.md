# Keyboard shortcut-inhibit policy reducer (TODO #15 shortcut inhibit)

`src/interop/keyboard_inhibit.zig` with C header `src/interop/keyboard_inhibit.h`
implements the decision logic for `zwp_keyboard_shortcuts_inhibit_v1` from
IMPL_DESC 3.6.3 "Shortcut inhibit" and 3.10.5. It does not bind the Wayland
protocol, create or destroy objects, read the escape chord, or inject input.
The caller maps `apply` results onto the real compositor objects. The module
returns ordered actions and a forwarding flag.

## Protocol and spec facts used

- `input.inhibit_shortcuts` is `never`, `fullscreen` (default) or `focused`
  (IMPL_DESC 3.1 configuration table). These map to `mode_t` values
  `Never`, `Fullscreen`, `Focused`.
- `focused`: the inhibitor is requested for the focused surface.
  `fullscreen`: requested only while the window is fullscreen or pointer-locked.
  `never`: never requested.
- 3.10.5: while `inhibit_shortcuts=focused`, host IBus/Fcitx shortcuts are
  inhibited for Waddle surfaces through `zwp_keyboard_shortcuts_inhibitor_v1`.
  This is the `focused` policy row above.
- 3.6.1: exactly one exported window holds input focus. The policy therefore
  tracks one focused identity, and an inhibitor belongs to that identity only.

## Items the spec does not define (decided here, documented as open)

- **The escape chord is not specified.** IMPL_DESC names a user override but
  does not define its key combination. This module takes an abstract
  `user_toggle` event for it. The caller owns the chord detection and must
  send one `user_toggle` per chord press. Defining the chord is an open item for
  the lead.
- **Forwarding rule.** The spec does not state when Super and Alt-Tab reach the
  guest. This module derives it: forward exactly when the inhibitor is reported
  active and no user override is set. An inactive or absent inhibitor leaves the
  compositor in charge of shortcuts.
- **Generation ordering** uses the same wrap-safe rule as the lock module:
  candidate is newer when its forward distance from the current generation is in
  1 through 2^31 - 1.

## Public API (Zig)

| Symbol | Kind | Meaning |
|---|---|---|
| `HalfGenerationSpace` | const u32 | 0x80000000 |
| `MaxActions` | const usize | 2 |
| `error_t` | error set | `InvalidIdentity` |
| `mode_t` | enum(u8) | `Never`=0, `Fullscreen`=1, `Focused`=2 |
| `identity_t` | struct | `window: u64`, `generation: u32` |
| `phase_t` | enum(u8) | `None`=0, `Pending`=1, `Active`=2 |
| `action_kind_t` | enum(u8) | `Create`=1, `Destroy`=2 |
| `action_t` | struct | `kind`, `identity` |
| `result_t` | struct | `actions: [2]action_t`, `count: u8`, `forward_shortcuts: bool` |
| `surface_t` | struct | `identity`, `fullscreen: bool` |
| `event_t` | tagged union | `focus_enter`, `focus_leave`, `inhibitor_active`, `inhibitor_inactive`, `surface_fullscreen`, `user_toggle`, `window_destroy`, `configure` |
| `state_t` | extern struct | `config`, `phase`, `focused`, `fullscreen`, `override`, `window`, `generation`. 24 bytes |
| `apply(state, event)` | fn | Policy step (below) |
| `init(state)` | fn | Zero state |
| `status_t` | enum(c_int) | `Ok`=0, `NullArgument`=1, `InvalidIdentity`=2, `InvalidKind`=3, `InvalidMode`=4 |
| `event_kind_t` | enum(u8) | ABI kinds 0 through 7 in declaration order |
| `event_abi_t`, `action_abi_t`, `result_abi_t` | extern structs | 24, 24 and 64 bytes |

C ABI: `waddle_keyboard_inhibit_init(state*)`,
`waddle_keyboard_inhibit_apply(state*, event*, out*)`. Every pointer is nullable
and a NULL returns `NullArgument`. The event kind byte and the mode byte are
checked before use. Output and state change only on `Ok`. `result_abi_t` entries
at index `count` and beyond are zero.

## State model

- `focused` with `window` and `generation` is the one focused identity.
- `fullscreen` is the last surface flag for that identity. It resets on each new
  focus.
- `override` is the escape-chord toggle. It resets on each new focus.
- `phase` is the live object for the focused identity: `None` (no object),
  `Pending` (created, not yet reported active), `Active` (reported active).
- `config` is the policy. Changing it reconciles the object while focused.

Derived rule: an object should exist exactly when
`focused and not override and wanted(config, fullscreen)`, where
`wanted(Never, _) = false`, `wanted(Fullscreen, f) = f`, `wanted(Focused, _) = true`.
Forwarding is `phase == Active`.

## Events and actions

`apply` takes one event and returns the ordered actions and the forward flag.

- `focus_enter(id)`: rejects zero identity. If `id` is the focused identity, it
  is a duplicate and does nothing. If it is the same window with a generation
  that is not newer, it is stale and does nothing. Otherwise the previous focus
  is released (a Destroy is emitted if an object exists), the new focus is set
  with `fullscreen` and `override` cleared, and the object is reconciled. A focus
  change therefore emits at most two actions, Destroy then Create.
- `focus_leave(id)` and `window_destroy(id)`: reject zero identity. If `id` is
  the focused identity, release the focus and emit Destroy if an object exists.
  Otherwise no-op. The second destroy of the same window is a no-op, so an object
  is destroyed exactly once.
- `inhibitor_active(id)`: if `id` is focused and the phase is `Pending`, move to
  `Active`. Otherwise no-op. The caller must deliver only events from the
  currently live inhibitor object; object-level staleness is the caller's job.
- `inhibitor_inactive(id)`: if `id` is focused and the phase is `Active`, move to
  `Pending`. Otherwise no-op. The object stays and may become active again.
- `surface_fullscreen(identity, flag)`: if `identity` is focused, set `fullscreen`
  and reconcile. Otherwise no-op.
- `user_toggle(id)`: if `id` is focused, flip `override` and reconcile. Turning
  the override on destroys an active or pending object. Turning it off creates
  one again in `Pending`.
- `configure(mode)`: set the policy and reconcile. Unfocused reconcile emits
  nothing.

Every event that carries an identity rejects a zero window or a zero generation
with `InvalidIdentity` before any state change, including events that would
otherwise be no-ops.

## Ownership, lifetime and threading

- `state_t` is caller storage of 24 bytes. Zero-filled storage is valid
  (`Never`, no focus, no object). There is no allocation and no free function.
- Enum fields are private. Callers must not write them. The C header describes
  the fields as private for this reason.
- The state belongs to the single owner thread that receives focus, surface,
  override and compositor inhibitor events. It is not shared.
- Events and results are by value or borrowed only for the call.
- No function allocates, logs or performs I/O.

## Error codes

| Status | Zig error | Trigger | State change |
|---|---|---|---|
| `NullArgument` | n/a | C NULL pointer | none |
| `InvalidIdentity` | `error.InvalidIdentity` | window 0 or generation 0 | none |
| `InvalidKind` | n/a (ABI decode) | kind byte not 0 through 7 | none |
| `InvalidMode` | n/a (ABI decode) | Configure mode byte not 0 through 2 | none |

A stale or duplicate event is not an error. It returns `Ok` with `count = 0`.

## Verification

Zig built-in tests in `keyboard_inhibit.zig` (run by `make interop-keyboard-lanes-test`):

- Fresh state: nothing created, nothing forwarded.
- Focused policy: create on enter, duplicate enter ignored, destroy on leave,
  second leave ignored.
- Fullscreen policy: no creation while unfocused, flag on and off, create and
  destroy follow the flag.
- Never policy: no creation, no forwarding, even when active is reported.
- Active and inactive reports: phase transitions, forwarding follows Active,
  repeated reports are idempotent.
- User override: destroy on toggle on, create in Pending on toggle off.
- Configuration change while focused reconciles in both directions.
- Focus moving between windows: Destroy then Create, and stale events for the
  old window are no-ops.
- Generations: older generation stale, newer generation replaces the focus, stale
  active, toggle and fullscreen are no-ops.
- Wrap-safe generation order at 0xFFFFFFFF to 1, and the 2^31 boundary.
- Window destroy exactly once, and destroy of an unfocused window is a no-op.
- Zero identity rejected for every identity-carrying event with state unchanged.
- Randomized 20000-step sequence of mixed events, including rejected ones, checked
  against a reference model: object create and destroy in valid order, at most one
  object, the object present exactly when the policy wants it, the object identity
  equal to the focus, and forwarding equal to active-and-live.
- ABI struct sizes 24, 24, 24 and 64 match the C header.
- C ABI: every NULL position, invalid kind, invalid mode, zero identity, and a full
  session through the ABI with unused action slots checked as zero.

`keyboard_inhibit_abi.c` is an independent C consumer built with
`-fsanitize=address,leak,undefined` and run with `detect_leaks=1`
(`make interop-keyboard-lanes-sanitizers`).

Coverage (`python3 tests/av/coverage.py keyboard_inhibit`): line 97.92% (94/96),
branch 91.95% (80/87), measured at this commit. The coverage tool excluded 9
compiler panic and stack-canary guard branches, as it does for every module.

Zig sanitizer runs are not claimed. The Zig suite uses no allocator, so
`std.testing.allocator` and `FailingAllocator` sweeps do not apply.

## Deviations and open items

- The escape chord is undefined in IMPL_DESC. It is an open item (see above).
- The forwarding rule is derived, not quoted from the spec (see above).
- The caller must deliver compositor active and inactive reports only for the
  object it currently holds. The module cannot distinguish two objects for the
  same identity.
