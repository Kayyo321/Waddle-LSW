# Host Window Map Contract

Owner: TODO #11 subtask 11a. Implements the mapping table of
`impl/seamless-interop/IMPL_DESC.md` section 8.44 as amended (merged from
`feat/seamless-45-percent`), with the role rules of section 3.2.4, the identity
rules of section 3.2.8, and the refusal and retry rules of section 8.44.

Files: `src/interop/host_window_map.zig` (pure module and C ABI exports),
`src/interop/host_window_map.h` (C header, which includes
`include/waddle/interop_protocol.h`), `tests/interop/host_window_map_abi.c` (C
consumer), and the lane file `scripts/interop_lane_window_map.mk`.

## Scope

In scope: a fixed table of realized `(window, generation)` identities in stable
slots; the decision whether an announced identity may be realized; the retire
and reset teardown orders; reparenting a Dialog; the dismissed flag of a popup;
an adapter value per slot; protocol error codes for every refusal; and epoch reset.

Out of scope: every Wayland call, every native `HWND` query, the `WindowAnnounce`
codec, wire ordering, the guest's own deferral of announcements, and the
`popup_done` and `configure` handling. The table performs no action. It returns
a status and a slot, and the caller creates or destroys the native objects.
Creating `wl_surface`, `xdg_toplevel`, `xdg_popup`, decorations and app ids is
tasks 11b, 11d, 11e and 11f and remains open.

## Limits

| Name | Value | Meaning |
|---|---|---|
| `MaxRealized` | 64 | Realized windows per session epoch, and the number of slots. |
| `MaxOutput` | 64 | Minimum output capacity for `retire` and `reset`. |
| `NoSlot` | 64 | Slot reported by a refused announce. |

## Storage

`host_window_map_t` is an `extern struct`, 3600 bytes, owned by the caller. It
holds `realized_count: u32`, `next_seq: u64` and `slots: [64]slot_t`. A
`slot_t` (56 bytes) holds the window, its direct parent, its realization sequence
number, its adapter `user_data`, its role, an `occupied` flag and a `dismissed`
flag. The table holds no pointers and no allocator memory.

A zero-filled table is the empty table. A table is **corrupt** when
`realized_count` exceeds 64, or when it disagrees with the number of occupied
slots. Every operation checks this first and refuses a corrupt table rather than
using it. The caller clears corrupt storage by zero-filling it, because `reset`
refuses it.

## Identity and roles

`window_ref_t` is `{ window: u64, generation: u64 }`. A reference is **valid**
when both fields are nonzero. A parent reference is **none** when both fields are
zero. A parent that is neither none nor valid is invalid.

Roles (`interop_window_role_t`, checked against the protocol header by a
`_Static_assert` in the C header and by a Zig test that imports the same header):

| Role | Value | Parent rule | Kind |
|---|---|---|---|
| Toplevel | 1 | parent must be none | toplevel |
| Dialog | 2 | parent may be none or valid | toplevel; may be reparented |
| Menu | 3 | parent must be valid | popup |
| Tooltip | 4 | parent must be valid | popup |
| UacConsent | 5 | parent must be none | toplevel |
| ImePreedit | 6 | parent must be valid | popup |
| 0, 7..255 | | | `RefuseInvalid` |

A **popup** is Menu, Tooltip or ImePreedit. A popup's parent is its direct
requested parent, which may itself be a popup. A nested popup is never re-parented
to the root (section 8.44). Only a Dialog changes parent.

## Announce

`announce(map, window, parent, role)` returns `announce_result_t { status, slot }`.
Checks run in this order, and the first match wins. Every refusal changes no state
and reports `slot = NoSlot`.

1. `window` not valid, `parent` neither valid nor none, role outside 1..6,
   `parent.window == window.window`, a role/parent rule violated, or a corrupt
   table: `RefuseInvalid`.
2. A realized slot holds `window.window`. Under the same generation:
   `RefuseDuplicate`. Under another generation: `RefuseWindowConflict`. The
   conflict test precedes the parent test, so a live window under another
   generation is a conflict even when its parent is also stale.
3. The parent is named, and no realized slot holds `parent.window`, or the
   realized one has another generation: `RefuseStaleParent`. A child whose parent
   was never announced, was retired, or belongs to another generation is refused
   here, never held.
4. The table already holds 64 windows, or the sequence counter is at its maximum
   value: `RefuseExhausted`.
5. Otherwise `Realize`. The window takes the **lowest free slot** and the next
   sequence number, which is then incremented without wrapping. Its `user_data`
   is zero and its `dismissed` flag is clear.

The table never defers. A deferred state would need a deferred parent, so from an
empty table none could ever start (section 8.44 as amended). The guest defers an
announcement until its parent is exported (section 3.2.8) and resends it on the
250 ms refresh.

## Stable slots

A realized window keeps its slot index until it is retired. Retiring another window
never moves it. A freed slot is reused by the next announce, lowest index first.
Lookups return the slot, so the adapter can key native objects by slot.

## Retire

`retire(map, window, out)` returns `retire_result_t { status, count }`.

- `RefuseInvalid`: an invalid `window`, `out` shorter than 64, or a corrupt table.
- `Stale`: no realized slot holds exactly `window`. This covers a pair never
  realized, already retired, and a wrong generation.
- `Retired`: the realized subtree rooted at `window` is removed.

The subtree follows requested parents to any depth. The output lists the removed
pairs **children first**: children are visited in descending realization order,
each child's subtree before the child, and the target is last. Every descendant
therefore precedes its ancestor. If the subtree would exceed 64 entries or 64
levels, the table is corrupt and retire refuses. Retire never realizes anything.

## Reset

`reset(map, out)` tears the whole table down. It writes every realized window to
`out` and then clears the table.

- Order: the roots (windows with no parent) in descending realization order, and
  within each root the subtree children first, as `retire` orders it. The caller
  destroys native objects in list order.
- `RefuseInvalid`, with the table unchanged: `out` shorter than 64, a corrupt
  table, or a parent cycle that leaves some slot unreachable from every root.
- `Reset`: the list was written and the table is zero.

## Reparent

`reparent(map, window, new_parent)` changes the direct parent of a realized Dialog.

- `RefuseInvalid`: an invalid `window` or `new_parent`, a self parent, or a corrupt table.
- `Stale`: `window` is not realized exactly.
- `RefuseRole`: the window is not a Dialog. Toplevels and UacConsent carry no
  parent; popups keep the parent they were realized with.
- `RefuseStaleParent`: `new_parent` is not realized exactly.
- `RefuseCycle`: `new_parent` is the window or one of its descendants. The walk up
  from `new_parent` is bounded by 64 steps, so a cycle in corrupt storage is
  refused as `RefuseInvalid` rather than followed.
- `Reparented`: the parent is changed. The slot and the realization sequence are
  unchanged, so retire order and slot ids stay stable.

## Dismissed popups

`mark_dismissed(map, window)` records that the compositor reported `popup_done`
for a realized popup. The popup stays realized, with its slot and user data,
until the guest retires it. Only popups may be dismissed: a toplevel or dialog
gives `RefuseInvalid`. Marking again is `Marked` and changes nothing further. A
pair that is not realized exactly gives `Stale`. Announce clears the flag when a
slot is reused.

## Per-slot user data

`set_user_data(map, slot, window, value)` and `get_user_data(map, slot, window, out)`
store and read one `u64` per realized window. Both require the slot index to be
below 64 and the identity to match the slot exactly. Otherwise they return `Stale`
(free slot, or another window or generation) or `RefuseInvalid` (zero identity,
slot 64 or more, or a corrupt table). A `Stale` or refused call changes nothing.
The `get` output must not overlap the table. Announce sets the value to zero.

## Lookup, currency and counts

- `lookup(map, window)` returns a copy of the exact slot: window, parent, sequence
  number, user data, slot index, role and dismissed flag. Otherwise none.
- `is_current(map, window)` is true exactly when `lookup` returns an entry.
- `realized_count` returns the count, or 0 for a corrupt table.

## Protocol error codes

Each refusal has one protocol error code (`interop_error_t` in
`include/waddle/interop_protocol.h`). The Zig functions and the C exports return the
same value, so the adapter never restates the mapping.

| Result | Error code |
|---|---|
| announce `Realize` | `InteropErrorNone` |
| announce `RefuseDuplicate` | `InteropErrorProtocolViolation` |
| announce `RefuseStaleParent`, `RefuseWindowConflict` | `InteropErrorStaleWindow` |
| announce `RefuseExhausted` (64 windows or the sequence exhausted) | `InteropErrorQuotaExceeded` |
| announce `RefuseInvalid` | `InteropErrorMalformedFrame` |
| retire `Retired` | `InteropErrorNone` |
| retire `Stale` | `InteropErrorStaleWindow` |
| retire `RefuseInvalid` | `InteropErrorMalformedFrame` |
| reparent `Reparented` | `InteropErrorNone` |
| reparent `Stale`, `RefuseStaleParent` | `InteropErrorStaleWindow` |
| reparent `RefuseCycle`, `RefuseRole` | `InteropErrorProtocolViolation` |
| reparent `RefuseInvalid` | `InteropErrorMalformedFrame` |

Section 8.44 directs the host to answer a refused Announce with
`Error{refused_type = WindowAnnounce, error_code = QuotaExceeded | StaleWindow}`
and to record nothing. The duplicate and malformed codes are the same protocol
family, chosen for the cases section 8.44 does not name.

## C ABI

All exports use `callconv(.C)` and are declared in `host_window_map.h`.

| Export | Result |
|---|---|
| `host_window_map_reset(map, out, out_capacity, out_count)` | `host_window_map_reset_status_t` |
| `host_window_map_announce(map, window, parent, role)` | `host_window_map_announce_result_t { status, slot }` |
| `host_window_map_retire(map, window, out, out_capacity, out_count)` | `host_window_map_retire_status_t` |
| `host_window_map_reparent(map, window, new_parent)` | `host_window_map_reparent_status_t` |
| `host_window_map_mark_dismissed(map, window)` | `host_window_map_dismiss_status_t` |
| `host_window_map_set_user_data(map, slot, window, value)` | `host_window_map_user_data_status_t` |
| `host_window_map_get_user_data(map, slot, window, value)` | `host_window_map_user_data_status_t` |
| `host_window_map_lookup(map, window, out)` | `bool`. `out` may be NULL for a presence test. |
| `host_window_map_is_current(map, window)` | `bool` |
| `host_window_map_realized_count(map)` | `uint32_t`. NULL gives 0. |
| `host_window_map_announce_error_code`, `host_window_map_retire_error_code`, `host_window_map_reparent_error_code` | `uint32_t` protocol error code |

Output and count rules, for `reset` and `retire`:

- A NULL table, NULL output array, NULL count pointer, capacity below 64, or any
  overlap between the table, the output array and the count pointer refuses the
  call with `RefuseInvalid`. The count is **not written** in these cases, and the
  caller's value is unchanged.
- For every other call that reaches the table, the count is written: the result
  count, which is 0 for a stale pair.
- Overlap uses saturating address ranges, so a pointer near the top of the address
  space is never treated as disjoint by wrap-around.

## Threading

The table is owner-thread state. Callers serialize every call and every output
read. There is no internal locking. A value copy is a snapshot, not a second owner.

## Memory and ownership

The module allocates nothing. Every call uses only the caller's storage and stack
locals. `out` and the count pointers belong to the caller, and the table keeps no
pointer to them after the call returns.

## Decisions taken where section 8.44 was silent

1. "Announce order" and "reverse announce order" are realization order: the
   sequence number is the realization sequence, and a table that never defers has
   no other order.
2. Retire and reset emit children first, siblings in descending realization order.
   A reparented window keeps its realization sequence, so among its new siblings
   it is ordered by that original sequence, not by the time it was reparented.
3. A stale parent is refused, never held, because the table cannot tell a retired
   parent from one that was never announced without unbounded history.
4. Stale-parent refusal precedes exhaustion. A child whose parent is not realized
   is refused as a stale parent even when the table is full.
5. A conflict (a live window under another generation) precedes the parent test.
6. Toplevels and UacConsent cannot be reparented, because they carry no parent
   (announce refuses one with a parent), so the reparent of such a window is
   `RefuseRole`.
7. Only Dialogs may be reparented. `WindowUpdate` for a popup that changes its
   parent is refused, and the popup keeps its parent, as section 8.44 requires.
8. `RefuseWindowConflict` and `RefuseCycle` are additions to the status set of
   section 8.44; each is a stale or cycle refusal with its own code.

## Limitations

- A child refused as a stale parent is dropped by the table. The guest resends it
  after its parent is exported (section 3.2.8). The table sends nothing back and
  holds no retry state.
- The sequence counter stops at its maximum value and refuses further announces
  with `RefuseExhausted`. It never wraps. A session cannot reach that value in
  practice.
- A corrupt table is refused by every call. Recovery is zero-filling its storage;
  this is the caller's responsibility, and the table cannot detect corruption that
  leaves `realized_count` consistent with the occupied slots while the parent links
  are wrong, beyond the cycle and depth bounds.

## Verification

Zig built-in tests in `host_window_map.zig` cover every announce, retire, reset,
reparent, dismissal and user-data status; the 64 capacity edge and the 65th
refusal; stable slot indices across retire and reuse; the sequence counter at its
maximum; corrupt counts and cycles in caller storage; the teardown order for
reset; reparenting of a Dialog, every refusal, and a cycle created and then
broken; popup chains that keep their direct parent; error-code mapping for every
status; the protocol values imported from the header; NULL and overlap rejection
through the ABI; and five seeded random sequences compared operation by operation
with an independent reference model that predicts statuses, slots, sequence
numbers, teardown lists, user data and dismissal.

The C consumer `tests/interop/host_window_map_abi.c` exercises the same interface
under `-fsanitize=address,leak,undefined`, and checks the header's role and error
values at compile time. Coverage gate: 90% line and taken-branch coverage through
`tests/av/coverage.py host_window_map`.
