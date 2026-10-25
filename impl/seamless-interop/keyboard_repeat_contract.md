# Bounded host keyboard repeat timing

TODO #15 retains its existing20%repeat allocation. The allocation is now
explicitly split: pure timing/held-key reducer10%, actual Wayland event-loop,
queue and guest typematic composition10%. This module can qualify only the
first10%; no protocol, injection, compositor or installed-session claim follows.

`keyboard_repeat.zig` owns no allocations or OS resources. One owner-thread
state belongs to one focused(window,generation) and is reset on focus change,
leave, disconnect or failed event delivery. Focus requires nonzero IDs. Held
physical keys occupy a fixed768bool array. The caller supplies already-decoded
key events only after foreground/focus and translation admission. Repeatable
is a borrowed scalar from the keymap/typed translation policy; the reducer also
unconditionally excludes Shift/Ctrl/Alt/Meta, Caps/Num/ScrollLock, Pause, Hangul
(122) and Hanja (123, make-only keys whose repeat would toggle an IME),
power/sleep and unsupported numeric codes0or>767. No text or wire is parsed.

A fresh down marks held and cancels the previous candidate, including when the
new key is nonrepeatable. Duplicate downs do not restart delay or create a new
candidate. A matching up clears the candidate; releasing other keys has no
effect and never revives an older held key. Inherited focus-enter held keys
must stay suppressed by the caller until release; this module never synthesizes
focus-enter downs. Disable/rate0or timing reconfiguration clears the candidate
but keeps physical held bookkeeping, so re-enabling never repeats an old hold.

Rate is0..1000Hz; delay0..60000ms. Negative or larger values return InvalidTiming
without mutation. All time inputs use one nondecreasing monotonic nanosecond
clock. Backward time or unrepresentable deadline disarms the candidate and
returns ClockDiscontinuity; no wrapping deadline or repeat is published. A down
at time T is first due at T+delay. A due poll emits at most one key and schedules
ceil(1e9/rate)ns after the actual poll time. Missed historical intervals are
skipped, never burst-replayed. This bounded overrun policy can run slightly
slower than the requested rate and cannot exceed it. With zero delay the fresh
down itself is still emitted by the caller; the first subsequent poll is due.
The returned repeat is another pressed=true event with no intervening release;
it carries no activation/grab serial authority. If the caller cannot queue it,
it must terminate/reset the input focus rather than retry a consumed repeat.

`next_deadline` returns null unless an enabled nonzero-rate focused held
candidate exists. Polling an empty state is a no-op. The caller bounds its
existing event-loop wait by that deadline and cancels on every focus-loss and
transport-failure path. The module itself does not install a timer or inject.
Every mutator is synchronous, single-owner, non-reentrant, allocation-free.

Verification covers exact delay and period boundaries, all excluded codes,
multiple keys, duplicate downs, unrelated releases, focus identity changes,
rate/delay changes, disabled timing, invalid values without mutation, backward
clock, arithmetic overflow, delayed polling without bursts and long deterministic
stress. Full production lines/taken-arcs must exceed90%with existing retained
LLVM recorder and no new exclusions. Pure Zig requires no C sanitizer claim.
