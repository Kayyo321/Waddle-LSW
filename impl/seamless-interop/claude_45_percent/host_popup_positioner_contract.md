# Host Popup Positioner Contract

Owner: TODO #11 subtask 11c. Implements the positioner function of
`impl/seamless-interop/IMPL_DESC.md` section 8.44 as amended (merged from
`feat/seamless-45-percent`), section "Positioner (pure function)".

Files: `src/interop/host_popup_positioner.zig` (pure module and C ABI exports),
`src/interop/host_popup_positioner.h` (C header), `tests/interop/host_popup_positioner_abi.c`
(C consumer), and the lane file `scripts/interop_lane_window_map.mk`.

## Scope

In scope: the integer scale of a fractional scale, the positioner request for a
popup relative to its direct parent (anchor clamped into the parent's window
geometry, remainder as offset, size, and the frozen anchor, gravity and
constraint values), and the guest rectangle reported for a compositor-adjusted
position.

Out of scope: every Wayland object, the protocol-version check for
`set_reactive`, the grab decision, the `xdg_popup.configure` and `popup_done`
handling, and the choice of which parent is the direct parent (the table,
`host_window_map.zig`, supplies that).

## Types

`rect_t` is an `extern struct { x: i32, y: i32, width: u32, height: u32 }`
(16 bytes). Its meaning depends on the use:

| Use | `x`, `y` | `width`, `height` |
|---|---|---|
| Guest input to `request` | signed guest physical pixels, relative to the direct parent's top-left | unsigned guest physical pixels |
| Guest input and output of `reposition` | guest physical pixels | guest physical pixels, unchanged |

`popup_request_t` is an `extern struct` of 44 bytes:
`anchor_x, anchor_y` (set_anchor_rect origin), `anchor_width, anchor_height`
(always 1), `offset_x, offset_y` (set_offset), `width, height` (set_size),
`anchor` (always 5), `gravity` (always 8), `constraint_adjustment` (always 15).

Frozen constants: `MaxScale = 8`, `MaxLogicalExtent = 32768`, `AnchorTopLeft = 5`,
`GravityBottomRight = 8`, `ConstraintSlideFlip = 15` (`slide_x | slide_y | flip_x |
flip_y`). The Zig object exports them through `host_popup_positioner_constant`, and
the C test compares every header constant with that value.

`status_t` is `enum(c_int) { Ok = 0, InvalidArgument = 1, InvalidScale = 2,
OutOfRange = 3, Overflow = 4, InvalidParent = 5 }`. The pure functions return
`positioner_error_t!T` with errors `InvalidScale`, `OutOfRange`, `Overflow` and
`InvalidParent`, and the C exports map each one to its status.

## Scale

`integer_scale(numerator, denominator)` returns `ceil(numerator / denominator)`.
A zero denominator, a zero numerator, or any result outside 1..8 is
`InvalidScale`. Use: a fractional scale `n/d` (for example Wayland fractional
scale in 120ths) becomes the integer `ceil(n/d)`. Any scale given to `request`
or `reposition` outside 1..8 is `InvalidScale`.

## Request

`request(guest, s, parent_width, parent_height)` computes the positioner values.

1. The scale is validated (1..8). The parent size is validated: a zero dimension
   is `InvalidParent`, and a dimension above 32768 is `OutOfRange`.
2. Requested origin: `lx = floor(x / s)` and `ly = floor(y / s)`, with floor toward
   negative infinity.
3. Size: `width = max(1, ceil(w / s))`, and likewise for height.
4. Refusal (`OutOfRange`) if `|lx|`, `|ly|`, `width` or `height` exceeds 32768.
   The boundaries 32768 and -32768 are accepted; 32769 and -32769 are refused.
5. Anchor clamp: `anchor_x = clamp(lx, 0, parent_width - 1)` and likewise for y.
   The anchor point lies inside the parent's window geometry, as xdg-shell
   requires.
6. Offset: `offset_x = lx - anchor_x` and `offset_y = ly - anchor_y`. The sum
   `anchor + offset` is always the requested origin.

Unmoved, moved, negative and nested cases follow from these rules. A popup whose
origin lies inside its parent has zero offset. A popup whose origin is past the
parent's far edge is anchored at `parent - 1` with the excess as offset. The
direct parent sets the size, so a tooltip inside a 300 by 200 menu is clamped
against 300 by 200, not against the toplevel.

Offset refusal is not needed: the anchor lies in `[0, parent - 1]`, so
`|lx - anchor_x| <= |lx|`, and `|lx|` is already bounded by step 4. The offset
therefore never exceeds 32768 in absolute value. Section 8.44 lists offsets among
the refused quantities, but no input can produce one.

## Reposition

`reposition(guest, s, requested, adjusted_x, adjusted_y)` computes the rectangle
for PopupRepositioned, in the direct parent's coordinates:

- The requested origin is `origin = anchor + offset` from `requested`.
- `x' = guest.x + (adjusted_x - origin_x) * s`, and likewise for y. The
  displacement is the compositor's movement in logical units, scaled to guest
  pixels.
- `width` and `height` are the guest's own, unchanged. The constraint mask has no
  resize bits, and both values are the guest's own, so no dimension can exceed
  16384.

An unmoved popup (`adjusted = origin`) reports exactly its requested rectangle.
Floor rounding of the origin therefore never accumulates into a drift.

Overflow: `x'` or `y'` outside `i32` is `Overflow`, and no wrapped value is
returned. The intermediate product is computed in 64-bit arithmetic, which
cannot overflow for any `i32` adjusted position at scale 8.

## Errors and status

| Function | Errors |
|---|---|
| `integer_scale` | `InvalidScale` |
| `request` | `InvalidScale`, `InvalidParent`, `OutOfRange` |
| `reposition` | `InvalidScale`, `Overflow` |

C exports: `host_popup_positioner_integer_scale`, `host_popup_positioner_request`,
`host_popup_positioner_reposition`, and `host_popup_positioner_constant`. Each
takes the pointers it reads or writes and returns `host_popup_status_t`, or the
value for the constant selector. A NULL input or output pointer gives
`InvalidArgument` and writes nothing. Inputs are read into locals before any
output is written, so an output may alias its input. `host_popup_positioner_constant`
returns 0 for an index outside the five frozen constants.

## Threading and memory

The functions are pure and reentrant. They hold no state, allocate nothing, and
touch no operating-system API.

## Verification

Zig built-in tests in `host_popup_positioner.zig`:

- Every error code through the pure functions and the C exports, and each
  output is left untouched on refusal.
- Scale ceiling for fractions, zero and invalid denominators, scale bounds 1 and 8,
  and 0 and 9 as invalid.
- Unmoved requests; negative origins clamped with the remainder as offset; origins
  past the far edge; a 1 by 1 parent; a nested popup clamped against its direct
  parent.
- Dimension rounding, the 32768 boundaries for origins and dimensions, and the
  parent size limits.
- An exhaustive sweep of origins from -300000 to 300000 at every scale against an
  independent reference, and a seeded two-dimensional sample with the extremes.
- Unmoved reposition across the same exhaustive sweep; moved, clamped and nested
  displacements; and overflow boundaries. Seeded displacements, including the
  `i32` extremes, are checked against a 128-bit reference.
- The constant selector returns the module constants and 0 for unknown indexes.

The C consumer `tests/interop/host_popup_positioner_abi.c` checks the same values
through the ABI under `-fsanitize=address,leak,undefined`, and compares the header
constants with the Zig object. Coverage gate: 90% line and taken-branch coverage
through `tests/av/coverage.py host_popup_positioner`.
