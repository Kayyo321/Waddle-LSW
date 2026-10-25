# Host Window Map Contract

Owner: TODO #11 subtask 11a. Implements the mapping table of
`impl/seamless-interop/IMPL_DESC.md` section 8.44 as amended (merged from
`feat/seamless-45-percent`), with the role rules of section 3.2.4 and the identity
rules of section 3.2.8.

Files: `src/interop/host_window_map.zig` (pure module and C ABI exports),
`src/interop/host_window_map.h` (C header), `tests/interop/host_window_map_abi.c`
(C consumer), and the lane file `scripts/interop_lane_window_map.mk`.

## Scope

In scope: a fixed-size table of realized `(window, generation)` identities, the
decision whether one announced identity may be realized, the retire cascade of
realized children, popup re-parenting to the root toplevel, and epoch reset.

Out of scope: every Wayland call, every native `HWND` query, the `WindowAnnounce`
codec, wire ordering, and the guest's own deferral of announcements. The table
performs no action. It returns a status, and the caller creates or destroys the
native objects. Creating `wl_surface`, `xdg_toplevel`, `xdg_popup`, decorations
and app ids is tasks 11b, 11d, 11e and 11f and remains open.

## Limits

| Name | Value | Meaning |
|---|---|---|
| `MaxRealized` | 64 | Realized windows per session epoch. |
| `MaxOutput` | 64 | Minimum output capacity for `retire`. |

## No deferred state

Section 8.44 (amended) realizes a child only when its exact parent pair is
realized, and refuses it as a stale parent otherwise. The table therefore keeps
no deferred queue. A deferred entry is created only when its parent pair is
already deferred, and the first deferred entry ever would need such a parent.
Starting from an empty table, no entry can ever be deferred, so a queue would
never hold anything. The discard rule could never fire either.

The guest handles the ordering problem itself. IMPL_DESC section 3.2.8 requires
the guest to defer an announcement until its parent is announced and exported,
and to retry on the 250 ms refresh. A child that reaches the host before its
parent is therefore a protocol violation or a race that the guest resolves by
resending, not a state the host must hold.

## Identity and types

`window_ref_t` is `{ window: u64, generation: u64 }`. Protocol `window` and
`generation` are `u64`. A reference is **valid** when both fields are nonzero.
A **parent** reference is **none** when both fields are zero. A parent that is
neither none nor valid is invalid.

`window_entry_t` (stored in realization order):

| Field | Meaning |
|---|---|
| `window` | Realized identity. |
| `parent` | Requested parent identity, or none. Retire cascades follow this field. |
| `root` | Popup only: the positioning root (see Popup re-parenting). None for every other role. |
| `role` | Raw `interop_window_role_t` value, 1..6. |

`host_window_map_t` is an `extern struct` with `realized_count: u32` and
`realized: [64]window_entry_t`. Its size is 3592 bytes. The caller owns the
storage, and the table holds no pointers and no allocator memory.

## Role rules

| Role | Value | Parent rule | Result |
|---|---|---|---|
| Toplevel | 1 | parent must be none | realized, not a popup |
| Dialog | 2 | parent may be none or valid | realized, not a popup; the caller uses `lookup` on the parent to decide `set_parent` |
| Menu | 3 | parent must be valid | popup |
| Tooltip | 4 | parent must be valid | popup |
| UacConsent | 5 | parent must be none | realized, not a popup |
| ImePreedit | 6 | parent must be valid | popup |
| 0, 7..255 | | | `RefuseInvalid` |

A **popup** is Menu, Tooltip or ImePreedit. A popup whose parent is none is
`RefuseInvalid`. A Toplevel or UacConsent with a valid parent is `RefuseInvalid`.

## Announce

`announce(map, window, parent, role)` returns an `announce_status_t`. Checks run
in this order, and the first match wins. Every refusal changes no state.

1. `window` not valid, `parent` neither valid nor none, role outside 1..6,
   `parent.window == window.window`, or a role/parent rule violated: `RefuseInvalid`.
2. `window.window` realized under the same generation: `RefuseDuplicate`.
   Realized under another generation: `RefuseWindowConflict`. The other
   generation must be retired first.
3. Parent is valid and `parent.window` is not realized (never announced, or
   already retired): `RefuseStaleParent`.
4. Parent is valid, `parent.window` is realized, and under another generation:
   `RefuseStaleParent`.
5. The table already holds 64 realized windows: `RefuseExhausted`.
6. Otherwise the window is appended in realization order and the status is
   `Realize`. For a popup the root is computed as in Popup re-parenting.

Because a child is realized only after its parent, the realization order keeps
every parent before its children. The retire pass below depends on that
invariant.

Announce has no output list. A successful announce realizes exactly one window,
which is the one the caller passed in.

## Popup re-parenting

When a popup is realized, its positioning root is computed once:

- If its parent entry is a popup, `root` is that parent's `root`.
- Otherwise `root` is the parent pair itself.

The requested `parent` is kept unchanged. Cascades always follow `parent`, so
popup-of-popup chains retire with their true ancestors. The caller positions the
popup relative to `root`, which is always a non-popup window.

## Retire

`retire(map, window, out)` returns `retire_result_t { status, count }`.

- `RefuseInvalid`: `window` not valid, or `out` shorter than 64. No change.
- `Stale`: no realized entry with `window.window`, or its generation differs. This
  covers a pair that was never realized, a pair already retired, and a wrong
  generation. No change.
- `Retired`: the realized subtree rooted at `window` is removed.

The subtree is every realized entry whose requested parent is in the subtree, to
any depth. Because a child is always realized after its parent, one forward pass
over the realized table closes the subtree.

The output lists the removed pairs in **reverse realization order**. Every
descendant appears before its ancestor, and the target is last. `count` is at
least 1 for `Retired`.

Retire never realizes anything.

## Lookup, currency and counts

- `lookup(map, window)` returns the realized entry only when both `window` and
  `generation` match. Otherwise none.
- `is_current(map, window)` is true exactly when `lookup` returns an entry.
- `realized_count` returns the current count.

## Epoch reset

`reset(map)` sets the whole structure to zero. Zero-filled storage is therefore
equivalent to a reset table. A caller must call `reset` before first use of
storage it did not zero itself. Reset emits no output. The table owns no native
objects; the caller tears down its own objects for the old epoch before reset.

## C ABI

All exports use `callconv(.C)` and are declared in `host_window_map.h`.

| Export | Result |
|---|---|
| `host_window_map_reset(map)` | `void`. NULL `map` is a no-op. |
| `host_window_map_announce(map, window, parent, role)` | `host_window_map_announce_status_t`. NULL `map` gives `RefuseInvalid`. |
| `host_window_map_retire(map, window, out, out_capacity, out_count)` | `host_window_map_retire_status_t`. |
| `host_window_map_lookup(map, window, out)` | `bool`. `out` may be NULL for a presence test. |
| `host_window_map_is_current(map, window)` | `bool`. |
| `host_window_map_realized_count(map)` | `uint32_t`. NULL gives 0. |

ABI rules for `retire`:

- A NULL `map`, NULL `out`, NULL `out_count`, `out_capacity` below 64, or any
  overlap between the table storage, the output array, and the count pointer
  gives `RefuseInvalid`. The count is not written in these cases.
- Overlap uses saturating address ranges, so a pointer near the top of the
  address space is never treated as disjoint by wrap-around.
- For every other call the count is written with the removed count, which is 0
  for `Stale`.

## Threading

The table is owner-thread state. Callers serialize every call and every output
read. There is no internal locking. A value copy is a snapshot, not a second
owner.

## Memory and ownership

- The module allocates nothing. Every call uses only the caller's storage and
  stack locals.
- `out` and the count pointer belong to the caller. The table keeps no pointer
  to them after the call returns.

## Status values

`announce_status_t`: `Realize = 1`, `RefuseDuplicate = 2`,
`RefuseStaleParent = 3`, `RefuseWindowConflict = 4`, `RefuseExhausted = 5`,
`RefuseInvalid = 6`.

`retire_status_t`: `Retired = 1`, `Stale = 2`, `RefuseInvalid = 3`.

`RefuseWindowConflict` is not in section 8.44. It covers a second generation of
a window that is still realized: the old incarnation must be retired first, and
a new one is never silently accepted.

## Decisions taken where the spec was silent

1. "Announce order" means realization order. Retire cascades children first in
   reverse realization order, which is reverse announce order for a table that
   never defers.
2. A stale parent is refused, never held. The table cannot tell a retired parent
   from one that was never announced, and it keeps no history of retired pairs.
   Both are refused at the same point, which keeps the table bounded.
3. Stale-parent refusal precedes exhaustion. A child whose parent is not
   realized is refused as a stale parent even when the table is full, because
   that refusal is the true reason it cannot be realized.
4. `retire` requires an output capacity of 64. Any retire can then report its
   whole subtree in one call, and no truncated result exists.

## Limitations

- A child refused as a stale parent is dropped by the table. The guest must
  resend it once the parent is exported (section 3.2.8). The host sends nothing
  back to request the resend, and the host has no retry state.
- Announce has no output list and retire has no discard list. These are
  deliberate: section 8.44 as amended has no deferred state for them to report.

## Verification

Zig built-in tests in `host_window_map.zig` cover every announce and retire
status, the 64 capacity edge, the 65th refusal, the retire cascade order and
the unrelated-child case, stale parents (never announced, retired, and another
generation), popup re-parenting for popup-of-toplevel, popup-of-dialog and
popup-of-popup, dialog and UacConsent parent rules, epoch reset, every ABI null
and overlap rejection, and a randomized comparison over five seeded sequences
against an independent reference model written in the test.

The C consumer `tests/interop/host_window_map_abi.c` exercises the C ABI under
`-fsanitize=address,leak,undefined`. Coverage gate: 90% line and taken-branch
coverage through `tests/av/coverage.py host_window_map`.
