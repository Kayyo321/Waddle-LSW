# Host Window Map Contract

Owner: TODO #11 subtask 11a. Implements the bounded mapping table of
`impl/seamless-interop/IMPL_DESC.md` section 8.44 ("Mapping table"), with the
role rules of section 3.2.4 and the identity rules of section 3.2.8.

Files: `src/interop/host_window_map.zig` (pure module and C ABI exports),
`src/interop/host_window_map.h` (C header), `tests/interop/host_window_map_abi.c`
(C consumer), and the lane file `scripts/interop_lane_window_map.mk`.

## Scope

In scope: a fixed-size table of realized `(window, generation)` identities and
deferred children, the ordered output the caller must act on, the parent
retire cascade, popup re-parenting to the root toplevel, and epoch reset.

Out of scope: every Wayland call, every native `HWND` query, the `WindowAnnounce`
codec (`interop_codec.zig`), wire ordering, and the 250 ms guest refresh. The
table never performs an action. It tells the caller which pairs to realize or
retire, in order. Creating `wl_surface`, `xdg_toplevel`, `xdg_popup`, decorations
and app ids is task 11b/11d/11e/11f and remains open.

## Limits

| Name | Value | Meaning |
|---|---|---|
| `MaxRealized` | 64 | Realized windows per session epoch. |
| `MaxDeferred` | 64 | Deferred children per session epoch. |
| `MaxOutput` | 64 | Minimum caller output capacity for `announce` and `retire`. |

Realized windows, including windows produced by unblocking, count against the 64
realized entries. Deferred children count against the 64 deferred entries and
nothing else. A realize operation never leaves the realized table above 64.

## Identity and types

`window_ref_t` is `{ window: u64, generation: u64 }` (protocol `window` and
`generation` are `u64`). A reference is **valid** when both fields are nonzero.
A **parent** reference is **none** when both fields are zero. A parent that is
neither none nor valid is invalid.

`window_entry_t` (realized, stored in realization order):

| Field | Meaning |
|---|---|
| `window` | Realized identity. |
| `parent` | Requested parent identity, or none. Used for every cascade. |
| `root` | Popup only: the positioning root (see Popup re-parenting). None for every other role. |
| `role` | Raw `interop_window_role_t` value, 1..6. |

`deferred_entry_t` (deferred, stored in deferral order): `window`, `parent`, `role`.

`host_window_map_t` is an `extern struct` with `realized_count: u32`,
`deferred_count: u32`, `realized: [64]window_entry_t`, `deferred: [64]deferred_entry_t`.
The caller owns the storage (about 6 KiB). The table holds no pointers and no
allocator memory.

## Role rules

| Role | Value | Parent rule | Result |
|---|---|---|---|
| Toplevel | 1 | parent must be none | realized, not a popup |
| Dialog | 2 | parent may be none or valid | realized, not a popup; caller uses `lookup` on the parent to decide `set_parent` |
| Menu | 3 | parent must be valid | popup |
| Tooltip | 4 | parent must be valid | popup |
| UacConsent | 5 | parent must be none | realized, not a popup |
| ImePreedit | 6 | parent must be valid | popup |
| 0, 7..255 | | | `RefuseInvalid` |

A **popup** is Menu, Tooltip or ImePreedit. A popup whose parent is none is
`RefuseInvalid`. Dialog with a none parent is realized without a parent. Toplevel
or UacConsent with a valid parent is `RefuseInvalid`.

## Announce

`announce(map, window, parent, role, out)` returns `announce_result_t
{ status, count }`. Checks run in this exact order, and the first match wins:

1. `out` capacity below `MaxOutput`, `window` not valid, `parent` not valid-or-none,
   role outside 1..6, `parent.window == window.window`, a role/parent rule from
   the table above violated: `RefuseInvalid`.
2. A realized entry with the same `window`: same generation gives
   `RefuseDuplicate`; a different generation gives `RefuseWindowConflict`.
   A deferred entry with the same `window` follows the same rule.
3. Parent is valid and a realized entry has the same `parent.window`:
   a different generation gives `RefuseStaleParent`. Parent not realized
   (no entry with that window) continues to step 4.
4. Parent is valid and not realized: `Defer`. If the deferred table already holds
   `MaxDeferred` entries: `RefuseExhausted`. Otherwise the child is appended to the
   deferred table in deferral order and `count` is 0.
5. Otherwise the pair is realized (parent none, or the exact parent pair is
   realized). If the realized table already holds 64 entries, or realizing the
   pair plus its transitively unblocked children would exceed 64:
   `RefuseExhausted`, no state change.
6. Otherwise `Realize`. The output lists the realized pairs in realization order.

Only `Realize` writes output. Every refusal and `Defer` report `count = 0` and
change no state.

### Unblocking order

After a pair `P` is realized, the table realizes the deferred children whose
parent is exactly `P`, in deferral order (oldest deferral first). Each realized
child is then processed the same way. The order is breadth-first:

- `out[0]` is the announced pair.
- `out[1..]` are the children of `out[0]` in deferral order, then the children of
  `out[1]` in deferral order, and so on.

This is the ordering contract. A caller that realizes the list in output order
always has every parent realized before its children, and the realized table
keeps that order (see Retire).

Planning is a bounded pass over the deferred table with a local `marked` mask.
The table is mutated only after the whole closure fits. The worst case is
`64 * 64` pair comparisons.

## Popup re-parenting

When a popup is realized, its positioning root is computed once:

- If its parent entry is a popup, `root` is that parent's `root`.
- Otherwise `root` is the parent pair itself.

The requested `parent` is kept unchanged. Cascades always follow `parent`, so
popup-of-popup chains retire with their true ancestors. The caller positions the
popup relative to `root`, which is always a non-popup window.

## Retire

`retire(map, window, retired_out, discarded_out)` returns `retire_result_t
{ status, retired_count, discarded_count }`.

- `RefuseInvalid`: `window` not valid, or either output capacity below 64. No
  state change.
- `Stale`: no realized entry has `window.window`, or its generation differs. No
  state change. This includes a pair that is only deferred: a `retire` naming a
  deferred pair changes nothing (see Limitations).
- `Retired`: the realized subtree rooted at `window` is removed.

Subtree: every realized entry whose requested parent is in the subtree, to any
depth. Because a child is always realized after its parent, one forward pass
over the realized table in realization order closes the subtree.

Retired output order is **reverse realization order** over the removed set. Every
descendant appears before its ancestor, and the target window is last. Retired
output contains the target, so `retired_count >= 1`.

Discarded children: every deferred entry whose parent is a removed realized pair
is discarded. Discards then propagate: a deferred entry whose parent is a
discarded deferred pair is also discarded, because that parent can no longer
become realized in this epoch. Discarded output lists the discarded pairs in
deferral order and is reported separately. The caller replies `Error{StaleWindow}`
for each discarded pair, and it never realizes them.

Retire never realizes anything, so it never unblocks deferred children.

**Reachability of discards.** Through the public operations, the discard list is
always empty. A deferred entry is created only when its parent pair is not
realized, and realizing a pair unblocks every deferred child that names it. A
retire removes only realized pairs, so no deferred entry can name a removed pair.
The discard pass is kept so the reporting contract stays exact for any state the
table could be given by other means; the white-box test in
`host_window_map.zig` exercises it directly. Section 8.44 does not say how a
child announced after its parent was retired is to be treated, and that case is
deferred (see Limitations). It is an open question for the lead, not a silent
choice.

## Lookup, currency and counts

- `lookup(map, window)` returns the realized entry only when both `window` and
  `generation` match. Otherwise none.
- `is_current(map, window)` is true exactly when `lookup` returns an entry.
- `realized_count` and `deferred_count` return the current counts.

Any other `(window, generation)` message is stale and changes nothing. This
includes a `window` realized under another generation and a `window` that is
deferred.

## Epoch reset

`reset(map)` sets the whole structure to zero (both counts zero, every entry
zeroed). Zero-filled storage is therefore equivalent to a reset table. A
caller must call `reset` before first use of storage it did not zero itself.
Reset emits no output. The table owns no native objects; the caller tears down
its own native objects for the old epoch before it calls reset.

## C ABI

All exports use `callconv(.C)` and are declared in `host_window_map.h`.

| Export | Result |
|---|---|
| `host_window_map_reset(map)` | `void`. NULL `map` is a no-op. |
| `host_window_map_announce(map, window, parent, role, out, out_capacity, out_count)` | `host_window_map_announce_status_t`. |
| `host_window_map_retire(map, window, retired_out, retired_capacity, retired_count, discarded_out, discarded_capacity, discarded_count)` | `host_window_map_retire_status_t`. |
| `host_window_map_lookup(map, window, out)` | `bool`. `out` may be NULL for a presence test. |
| `host_window_map_is_current(map, window)` | `bool`. |
| `host_window_map_realized_count(map)` / `host_window_map_deferred_count(map)` | `uint32_t`. NULL gives 0. |

ABI rules for `announce` and `retire`:

- A NULL `map`, NULL output pointer, NULL count pointer, output capacity below
  64, or any overlap between the map storage, an output array, or a count
  pointer, gives `RefuseInvalid` (retire: `RefuseInvalid`). The count is not
  written in these cases.
- Overlap uses saturating address ranges, so a pointer near the top of the address
  space is never treated as disjoint by wrap-around.
- For every other call the count is written with the result count, which is 0
  for every refusal.

## Threading

The table is owner-thread state. Callers serialize every call and every output
read. There is no internal locking. A value copy is a snapshot, not a second
owner.

## Memory and ownership

- The module allocates nothing. Every call uses only the caller's storage and
  bounded stack locals (about 1 KiB of marks and index arrays).
- `out` and the count pointers belong to the caller. The table keeps no pointer
  to them after the call returns.

## Error and status values

`announce_status_t`: `Realize = 1`, `Defer = 2`, `RefuseDuplicate = 3`,
`RefuseStaleParent = 4`, `RefuseWindowConflict = 5`, `RefuseExhausted = 6`,
`RefuseInvalid = 7`. `RefuseWindowConflict` is an addition to the set in
section 8.44: a window already realized or deferred under another generation
must be retired first, and an unrelated generation is never silently replaced.

`retire_status_t`: `Retired = 1`, `Stale = 2`, `RefuseInvalid = 3`.

## Decisions taken where the spec was silent

1. "Announce order" means realization order. A deferred child's position is set
   when it is realized, not when it was first announced. Deferred children are
   therefore never retired before their own children.
2. `RefuseStaleParent` applies only when the parent window is realized under a
   different generation. A parent window that has never been realized in this
   epoch defers, because the table cannot tell "stale" from "not yet" without
   unbounded history.
3. Duplicate deferred pairs are `RefuseDuplicate`, not a second entry.
4. Discards cascade through deferred children. This is the stricter reading:
   a child of a discarded deferred pair cannot realize in this epoch.
5. Output capacity below 64 is `RefuseInvalid` rather than a partial result, so
   a caller never sees a truncated realize or retire list.

## Limitations (explicit, bounded)

- A `retire` naming a deferred pair changes nothing, per section 8.44. A deferred
  child stays deferred until its parent pair realizes or the epoch resets. It
  occupies one of the 64 deferred slots meanwhile.
- A child announced after its parent pair was retired is not stale to the table.
  The table cannot tell a retired parent from one not yet announced without
  unbounded history, so the child defers and stays until epoch reset. Section
  8.44's rule "a deferred child whose parent pair is retired is discarded" therefore
  never fires for such a child, because the discard happens only at retire time.
  Conforming guests send a child only after its parent is exported, so this is
  reachable only by a protocol violation or after a refused parent. A retired-pair
  record would close the gap; section 8.44 does not provide one.
- A deferred child whose parent window is later realized under a different
  generation is not discarded by `announce`. `announce` has no discard output.
  Such an entry also stays until epoch reset.

## Verification

Zig built-in tests in `host_window_map.zig` cover every status, both capacity
edges (64 realized, 65th refused; 64 deferred, 65th refused; closure that would
overflow), deferral chains with unblocking order, the parent retire cascade in
reverse realization order, stale generations, duplicate announce, popup-of-popup
re-parenting, epoch reset, every ABI null and overlap rejection, and a
randomized sequence (several seeds, long runs) compared operation by operation
with an independent reference model written in the test. The C consumer
`tests/interop/host_window_map_abi.c` exercises the C ABI under
`-fsanitize=address,leak,undefined`. Coverage gate: 90% line and taken-branch
coverage through `tests/av/coverage.py host_window_map`.
