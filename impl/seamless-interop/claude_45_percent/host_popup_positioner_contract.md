# Host Popup Positioner Contract

Owner: TODO #11 subtask 11c. Implements the pure positioner function of
`impl/seamless-interop/IMPL_DESC.md` section 8.44 ("Positioner (pure function)").
The compositor calls (`set_size`, `set_anchor_rect`, `set_anchor`, `set_gravity`,
`set_offset`, `set_constraint_adjustment`, `set_reactive`) and the
`xdg_popup.configure` handling belong to task 11d and are not part of this module.

Files: `src/interop/host_popup_positioner.zig` (pure module and C ABI exports),
`src/interop/host_popup_positioner.h` (C header), `tests/interop/host_popup_positioner_abi.c`
(C consumer), and the lane file `scripts/interop_lane_window_map.mk`.

## Scope

In scope:

- `integer_scale`: the integer host scale for a fractional scale.
- `request`: the logical popup request for a guest pixel rectangle.
- `restore`: the inverse conversion of a compositor-adjusted logical rectangle
  back to guest pixels.

Out of scope: any Wayland object, any compositor round trip, the protocol
version check for `set_reactive`, and the grab decision.

## Types

`rect_t` is an `extern struct { x: i32, y: i32, width: u32, height: u32 }`
(16 bytes, C layout). Its meaning depends on the function:

| Use | `x`, `y` | `width`, `height` |
|---|---|---|
| Guest input to `request` | signed guest physical pixels, relative to the parent window's top-left | unsigned guest physical pixels |
| Output of `request` | logical anchor-rectangle origin (`set_anchor_rect` x, y) | logical positioner size (`set_size`) |
| Input and output of `restore` | logical compositor-adjusted origin | logical size |
| Output of `restore` | guest physical pixels | guest physical pixels |

The scale is an unsigned integer `s` with `1 <= s <= 8` (`MaxScale`).

`status_t` is `enum(c_int) { Ok = 0, InvalidArgument = 1, InvalidScale = 2,
OutOfRange = 3, Overflow = 4 }`. Pure functions return
`positioner_error_t!T`, where `positioner_error_t = error{ InvalidScale,
OutOfRange, Overflow }`. The C exports map each error to its status.

Constants: `MaxScale = 8`, `MaxLogicalExtent = 32768`,
`AnchorTopLeft = 5`, `GravityBottomRight = 8`,
`ConstraintSlideFlip = 15` (`slide_x | slide_y | flip_x | flip_y`).
Both Zig and the C header define these values. The enum values match
`xdg_positioner` anchor and gravity `top_left` and `bottom_right`, and the
`constraint_adjustment` bits.

## Scale

`integer_scale(numerator, denominator)` returns `ceil(numerator / denominator)`
computed in 64-bit arithmetic.

- `denominator == 0`: `InvalidScale`.
- A result outside `1..8` (including a zero numerator): `InvalidScale`.

Use: a fractional scale `n/d` (for example Wayland fractional scale in 120ths)
becomes the integer `ceil(n/d)`. For example 150/120 gives 2, and 120/120 gives 1.
Any other scale value passed to `request` or `restore` is `InvalidScale`.

## Request

`request(rect, s)` converts a guest rectangle to the logical popup request.

With `s` validated, let `floor_div` round toward negative infinity:

- `x = floor_div(rect.x, s)` and `y = floor_div(rect.y, s)`. Negative guest
  coordinates round down: `-1/2` gives `-1`, `-2/2` gives `-1`, `-3/2` gives `-2`.
- `width = max(1, ceil(rect.width / s))`, and likewise for height. A zero guest
  dimension becomes 1 logical unit.

Refusal (`OutOfRange`): any of `|x|`, `|y|`, `width` or `height` (logical) above
`MaxLogicalExtent` (32768). The boundary values 32768 and -32768 are accepted,
and 32769 and -32769 are refused. No output is written on refusal.

The guest rectangle is not otherwise validated here. A guest width above 16384 is
rejected earlier by the codec, and this function still produces a correct value
for any `u32` input.

Positioner values produced for a successful request: anchor rectangle
`(x, y, 1, 1)`, size `(width, height)`, anchor `AnchorTopLeft`, gravity
`GravityBottomRight`, offset `(0, 0)`, constraint adjustment
`ConstraintSlideFlip`. The adapter also sets `set_reactive` when the protocol
version provides it. These fixed values are not in the output struct; the
adapter passes them directly.

## Restore

`restore(logical, s)` converts a compositor-adjusted logical rectangle back to
guest pixels by multiplying by `s`.

- `x` and `y` are multiplied as signed 64-bit values. An `i32` result outside the
  representable range is `Overflow`.
- `width` and `height` are multiplied as unsigned 64-bit values. A `u32` result
  outside the representable range is `Overflow`.
- A zero logical size stays zero. `restore` does not clamp to 1, because a
  compositor-reported zero size is passed through unchanged.

No logical extent check applies to `restore`; only the overflow checks do.
The `PopupRepositioned` consumer reads `x` and `y` only. The width and height
results are informational.

## Overflow decision

Multiplying `width` and `height` as well as `x` and `y` is the stricter reading
of "multiply by scale with overflow checks". It removes a case in which the
guest receives a size that would silently wrap.

## Errors and status

| Function | Errors |
|---|---|
| `integer_scale` | `InvalidScale` |
| `request` | `InvalidScale`, `OutOfRange` |
| `restore` | `InvalidScale`, `Overflow` |

C exports: `host_popup_positioner_integer_scale`,
`host_popup_positioner_request`, `host_popup_positioner_restore`. Each takes the
pointers it reads or writes and returns `host_popup_status_t`. A NULL input or
output pointer gives `InvalidArgument` and writes nothing. Inputs are read into
locals before any output is written, so an output may alias its input.

## Threading and memory

The functions are pure and reentrant. They hold no state, allocate nothing, and
touch no operating-system API. Callers own every pointer.

## Verification

Zig built-in tests in `host_popup_positioner.zig` cover:

- Every error, through both the pure functions and the C exports.
- Scale ceiling for fractions (for example 150/120 gives 2), zero and invalid
  denominators, and scale values 1 through 8, plus 0 and 9.
- Negative and boundary `x`, `y` for every scale 1..8, including floor rounding
  for negative values.
- Ceiling for width and height (including 0 and 1), and the 32768 boundary for
  offsets and dimensions.
- `i32` and `u32` extremes, and every overflow path of `restore`.
- Round trips: a request followed by a restore of the same logical origin gives
  `s * floor(x / s)`.
- The C export null rejection.

The C consumer `tests/interop/host_popup_positioner_abi.c` checks the same
values through the C ABI under `-fsanitize=address,leak,undefined`. Coverage
gate: 90% line and taken-branch coverage through
`tests/av/coverage.py host_popup_positioner`.
