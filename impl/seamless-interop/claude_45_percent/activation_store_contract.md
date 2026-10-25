# Activation token capture and store contract (TODO #29e)

Module: `src/interop/activation_store.zig`, C header `src/interop/activation_store.h`.
Realizes the token rules of IMPL_DESC 3.9.1 (`XDG_ACTIVATION_TOKEN`, then
`DESKTOP_STARTUP_ID`), 3.9.3 (the host stores `(activation_token, process_id)`
for 30 s), and the wire rule of `interop_codec.zig` (`printable`, 1..255 bytes).

## 1. Scope

Inside: validating an activation token, selecting the first valid token from
the two environment candidates, and a fixed-capacity store of
`(token, process_id)` pairs with a 30 s expiry driven by a caller monotonic
millisecond clock.

Outside: reading the environment (the caller passes the two values), the
`xdg_activation_v1.activate` call (29j), the LaunchResult correlation and first
commit trigger (29f), process-id-to-window association, and any allocation.

## 2. Token validity

A token is valid when all of these hold:

- Length is 1..255 bytes.
- Every byte is in `0x21..0x7E` (printable ASCII without space), the same
  predicate as the codec `printable()` used for the wire field
  `activation_token`.

Anything else is invalid. No normalization, trimming or case folding. A
zero-length value is invalid.

### 2.1 `pick_activation_token(xdg, desktop_startup_id)`

- Inputs are borrowed candidate values; either may be absent (null).
- The result is the first valid candidate in the order `xdg`, then
  `desktop_startup_id`. An invalid or absent first candidate falls through to
  the second. If neither is valid the result is absent.
- The result borrows the candidate bytes; it is valid while the caller's
  environment storage lives.

C ABI: `interop_activation_pick_token(const char *xdg, const char *startup,
const char **token_out, size_t *length_out)`. Each non-NULL candidate is read
up to its NUL, and at most 256 bytes are examined; a candidate with no NUL
within 256 bytes is longer than 255 and therefore invalid. Return 0 when a
token was chosen (outputs set), 1 when none was valid (outputs unchanged),
2 when `token_out` or `length_out` is NULL (outputs unchanged).

## 3. Store

### 3.1 Parameters

| Name | Value | Meaning |
|---|---|---|
| `MaxEntries` | 32 | Maximum stored pairs. |
| `MaxTokenBytes` | 255 | Maximum token length. |
| `TtlMs` | 30000 | An entry is live while `now < inserted + 30000`. |

Expiry boundary: an entry inserted at `t` is live for every `now` in
`[t, t+30000)` and expired at `now >= t+30000`. `deadline = inserted +| TtlMs`
saturates at `u64` maximum; saturated entries never expire by time.

### 3.2 Memory layout (extern, all sizes asserted)

```
entry_t  (280 bytes, 8-aligned)
  token[256]        u8   token bytes; only the first token_length are meaningful
  token_length      u8   1..255 when occupied
  occupied          u8   0 or 1
  reserved[2]       u8   zero
  process_id        u32  nonzero when occupied
  deadline_ms       u64  inserted + TtlMs (saturating)
  sequence          u64  insertion/replacement order; larger is newer

store_t  (8992 bytes, 8-aligned)
  entries[32]       entry_t
  next_sequence     u64  next sequence value, starts at 1
  last_now          u64  most recent accepted clock value
  count             u32  occupied entries
  clock_started     u32  0 before the first timed operation, else 1
  rollbacks         u32  count of detected clock rollbacks
  reserved          u32  zero
```

All-zero bytes are a valid empty store. C callers may initialize with `{0}`.
No pointer is stored; no allocation exists.

### 3.3 Operations

- `put(token, process_id, now)`:
  1. Validate token (section 2) and `process_id != 0`. On failure return the
     error and change nothing, including the clock state.
  2. Observe the clock (section 3.4).
  3. Expire stale entries: remove every occupied entry with `now >= deadline`.
  4. If an entry with identical token bytes exists, replace it: set
     `process_id`, `deadline = now + TtlMs`, and `sequence = next_sequence`
     (it becomes the newest). The count does not change.
  5. Otherwise, if `count == 32`, evict the occupied entry with the smallest
     `sequence` (the oldest). Eviction happens only after step 3.
  6. Insert the token in the lowest-index unoccupied slot with `sequence =
     next_sequence`, then increment `next_sequence`.
- `take_for_process(process_id, now, out[255])`:
  1. `process_id == 0` is `InvalidProcess` and changes nothing.
  2. Observe the clock; expire stale entries.
  3. Among occupied entries with that `process_id`, select the one with the
     largest `sequence` (the newest). If none, return NotFound.
  4. Copy its token into `out`, remove the entry, return the length. A token
     is returned at most once: the removed entry cannot be returned again.
- `expire(now)`: observe the clock; remove every expired entry; return the
  number removed.
- `clear()`: remove every entry. Does not change `last_now`, `rollbacks` or
  `next_sequence`.

### 3.4 Clock and rollback

- `now` is a caller-supplied monotonic millisecond value.
- `observe(now)`: if `clock_started` and `now < last_now`, the clock has rolled
  back. Then all entries are cleared, `rollbacks` is incremented, and the
  clock baseline becomes `now`. In all cases set `last_now = now` and
  `clock_started = 1`.
- Rationale: after a rollback, a stored deadline can no longer be compared
  with the current clock. Clearing fails closed: no token issued before the
  rollback can ever be returned. Entries put after the rollback use the new
  clock consistently.
- A rollback is reported only through the `rollbacks` counter; the operation
  that observed it still completes normally (for example, `put` inserts its
  token after the clear).

### 3.5 Errors

Zig: `error_t = error{ InvalidToken, InvalidProcess }` for `put`;
`take_for_process` returns `error{InvalidProcess}!?usize`. Errors never change
state.

C ABI status codes:

| Function | 0 | 1 | 2 | 3 |
|---|---|---|---|---|
| `interop_activation_put` | Ok | InvalidArgument (NULL store, or NULL token with length > 0) | InvalidToken | InvalidProcess |
| `interop_activation_take` | Found | InvalidArgument (NULL store, out or length_out) | NotFound | InvalidProcess |
| `interop_activation_pick_token` | Found | None valid | InvalidArgument | (unused) |

`interop_activation_expire` returns the removed count (0 for a NULL store).
`interop_activation_clear` is a no-op for a NULL store.

## 4. Threading and ownership

- The store is caller-owned storage of 8992 bytes, 8-byte aligned. It is not
  thread-safe: one owner thread serializes every call on one store.
- Independent stores are independent.
- Tokens returned by `take_for_process` are copies. Candidate strings passed to
  `pick_activation_token` are borrowed and must outlive the returned pointer.

## 5. Ambiguities resolved (stricter reading)

- Multiple live tokens for one `process_id`: `take_for_process` returns the
  newest. Other tokens for that process remain until expiry, eviction or their
  own take.
- Replacing an existing token refreshes its deadline and makes it the newest,
  so repeated launches of the same token extend its life only through a `put`.
- A clock that moves forward by any amount is accepted; expiry is judged only
  against the current `now`.
- Clearing on rollback drops tokens that a naive comparison might still
  consider valid. This is intentional fail-closed behavior.

## 6. Test plan

- Token validity: empty, 255-byte accepted, 256-byte rejected, space, DEL,
  control bytes, every printable boundary (0x20, 0x21, 0x7E, 0x7F).
- `pick_activation_token`: xdg valid; xdg invalid falls to startup; both
  invalid; both absent; empty xdg falls through; over-length xdg falls through.
- Put/take: exact once; wrong process id leaves the entry; process id 0 is
  InvalidProcess and changes nothing (including the clock baseline).
- Expiry boundary: live at `t+29999`, expired at `t+30000`; saturation near
  `u64` maximum never expires.
- Duplicate: same token replaces pid and deadline; old pid no longer matches.
- Eviction: 33rd distinct token evicts the oldest; stale entries are expired
  before any eviction, so a full store of expired entries evicts nothing.
- Rollback: a clock moving backward clears entries, increments `rollbacks`,
  and never returns an earlier token; rollback during `put` still inserts the
  new token.
- Newest-wins for a shared process id.
- Randomized sequences: 20000 seeded operation streams over a pool of 40 tokens
  (so eviction occurs), 5 process ids, and a clock with forward steps and
  occasional rollbacks, compared step by step against a reference model that
  keeps an unordered list with explicit insertion counters.
- Layout: `sizeof(store_t) == 8992`, entry size 280, field offsets, and the C
  struct size asserted in the C consumer.
- Gates: `python3 tests/av/coverage.py activation_store` >= 90 percent line and
  taken branch; `interop-activation-store-test` runs the Zig suite and the C
  consumer; the sanitizer target runs the C consumer under
  `-fsanitize=address,leak,undefined`. Zig sanitizer execution is not claimed;
  Zig ownership is proven by the absence of allocation (no allocator is used).
