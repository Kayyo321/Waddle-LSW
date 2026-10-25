# Live pointer buttons and wheel (TODO #16a)

Scope: `src/av/av_wayland.c` (host pointer callbacks), `src/av/av_input.c` (guest
admission and held buttons), `src/av/av_windows.c` (guest MOUSEINPUT), and
`src/av/av_codec.zig` (button width). Spec: IMPL_DESC 3.6.4. Not in this tranche:
activating click (3.6.1) and cursor synchronization.

## 1. Buttons

- Host map (`pointer_button_code`): BTN_LEFT 0x110 -> 1, BTN_RIGHT 0x111 -> 2,
  BTN_MIDDLE 0x112 -> 3, BTN_SIDE 0x113 -> 4 (XBUTTON1), BTN_EXTRA 0x114 -> 5
  (XBUTTON2). BTN_FORWARD 0x115 and BTN_BACK 0x116 are dropped, because 3.6.4 says
  "Others dropped". This is not a guess; the table in the spec lists only these five.
- Wire: `MsgInputButton` (and V3) width 1..5 (`av_codec.zig`). Widths 0 and 6+ are
  malformed.
- Guest: `button_input` maps width 1..5 to the left, right, middle or XDOWN/XUP forms.
  XBUTTON1 sets `mouseData` = 1 and XBUTTON2 sets it to 2. Other widths return -1
  before SendInput. Held buttons 1..5 are released together by release-all.
- Compatibility (open decision): the protocol has no version or capability field. A
  peer that decodes widths 1..3 only treats a frame with width 4 or 5 as malformed,
  which is a connection-level failure. Host and guest must therefore both carry this
  change to use XBUTTON. No other message changed.

## 2. Seat version and wheel

- Seat bind: `min(advertised, 9)`, refused below 5. The pointer inherits this
  version (`seat_version`).
- Per wl_pointer frame, each axis (vertical, horizontal) takes one source by
  precedence: `axis_value120` sum, then `axis_discrete` sum times 120, then the
  continuous `axis` sum converted with `value * 12 / 256` (10 px = 120 units). A frame
  that uses value120 never touches the continuous remainder.
- Natural scrolling: `axis_relative_direction` = INVERTED negates that axis's
  logical value before the sign rule. The direction persists until leave.
- Sign: horizontal sends the Wayland value (right positive). Vertical sends the
  negated value (Wayland down positive, Windows wheel away positive).
- Carry: the continuous path keeps a remainder in 1/256 units (`|carry| < 256`),
  truncating toward zero. The remainder is reset on pointer leave.
- Clamp and chunks: per frame, each axis is bounded to +-12000 units. Each
  `MsgInputWheel` carries at most +-1200 per axis (the wire limit). Larger frames are
  split into ordered messages. This is one message per frame except when a frame
  exceeds 1200 units.
- Leave resets the frame sums, the carry and the natural-scroll state.

## 3. Verification

- `tests/av/wayland_state.c`: BTN_SIDE/BTN_EXTRA widths 4/5, BTN_FORWARD/BTN_BACK
  dropped, leave releases held buttons; seat bind at v10 (binds v9) and v4 (refused);
  v5 discrete, v8 value120 precedence over discrete and continuous, natural scrolling,
  sign, frame summing (two events, one message), a frame without axes (nothing), 12000
  clamp split into ten 1200-unit messages, carry sequence 3, 4, 4 and reset on leave.
  A 1500-step randomized lifecycle (buttons, axes, frames, pointer leave, keyboard
  focus loss) checks against a guest model built from delivered events that no
  button is delivered down twice and that no button stays held after a loss.
- `tests/av/windows_lease.c` (`xbutton_tests`): exact MOUSEINPUT for XDOWN/XUP with
  mouseData 1 and 2, the left form, rejected widths 0 and 6, a down without an owned
  point, and a held XBUTTON2 released on revocation.
- `tests/av/input.c` (`xbutton_admission`): widths 4 and 5 admitted and released,
  widths 0 and 6 rejected.
- `tests/av/input_coverage.py`: line 100.00%, branch 95.51%.
- `av_codec`: widths 1..5 round trip, 0, 6 and 128 rejected.

## 4. Open items

- Native verification: XBUTTON and wheel delivery on real Windows is not run here.
- Compatibility of width 4/5 with peers built before this change (section 1).
