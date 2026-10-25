# Activation token store contract (TODO #29e)

Module: `src/interop/activation_store.zig`, C header `src/interop/activation_store.h`.
Realizes the token rules of IMPL_DESC 3.9.1 (`XDG_ACTIVATION_TOKEN`, then
`DESKTOP_STARTUP_ID`), 3.9.3 (the host holds `(activation_token, process_id)` for
30 s and calls `xdg_activation_v1.activate` on first commit), and the wire rule
of `interop_codec.zig` (`printable`, 1..255 bytes).

## 1. Scope and the host-only rule

Inside: token validation, environment selection, a bounded array of the host's
own pending tokens, and a bounded ring of recently spent tokens, both with a
30 s lifetime, plus fail-closed clock rollback.

Outside: reading the environment, the `activate` call (29j), LaunchResult
correlation and first-commit trigger (29f), and any allocation.

**Host-only rule.** The store holds only tokens the host generated or received
from its own environment (`XDG_ACTIVATION_TOKEN`, `DESKTOP_STARTUP_ID`) and
stored as pending for a launch. It never stores a token echoed by the guest in
`LaunchResult.activation_token`. The guest's echo is opaque, unauthenticated
data; the host ignores it for store and activation decisions.

**Single-token rule for multi-item launches** (the integration layer, see
`open_request_contract.md` section 4.2). One environment token is stored for
one launch and attached to the first launched item only. Later items of the
same request are launched without a token.

## 2. Token validity

A token is valid when all of these hold:

- Length is 1..255 bytes.
- Every byte is in `0x21..0x7E` (printable ASCII without space), the predicate
  of the codec `printable()` for the wire field `activation_token`.

No normalization, trimming or case folding. Empty is invalid.

### 2.1 `pick_activation_token(xdg, desktop_startup_id)`

- Inputs are borrowed candidates; either may be null (unset).
- The result is the first valid candidate in the order `xdg`, then
  `desktop_startup_id`. An invalid or absent first candidate falls through.
  If neither is valid the result is absent.
- The result borrows the candidate bytes while the environment storage lives.

C ABI: `interop_activation_pick_token(const char *xdg, const char *startup,
const char **token_out, size_t *length_out)`. Each non-NULL candidate is read up
to its NUL; at most 256 bytes are examined, and a candidate with no NUL within
256 bytes is longer than 255 and therefore invalid. Returns 0 when a token was
chosen (outputs set), 1 when none is valid (outputs unchanged), 2 when
`token_out` or `length_out` is NULL (outputs unchanged).

## 3. Store

### 3.1 Parameters

| Name | Value | Meaning |
|---|---|---|
| `MaxEntries` | 32 | Capacity of the pending array and of the spent ring. |
| `MaxTokenBytes` | 255 | Maximum token length. |
| `TokenBufferBytes` | 256 | Take output buffer: token plus NUL. |
| `TtlMs` | 30000 | An entry is live while `now < inserted + 30000`. |

Deadline `inserted +| TtlMs` saturates at the `u64` maximum. A deadline that
saturated is live for every `now` below the `u64` maximum and expires when `now`
reaches it (`now >= deadline`); the last representable instant is the expiry
instant, so no saturated entry is ever live at `now == u64 maximum`.

### 3.2 Memory layout (extern, sizes asserted in Zig and in the C consumer)

```
entry_t  (280 bytes, 8-aligned)
  token[256]      u8   token bytes; first token_length meaningful
  token_length    u8   1..255 when occupied; 0 when free
  occupied        u8   0 or 1
  reserved[2]     u8   zero
  process_id      u32  owning process while pending; 0 in a spent entry
  deadline_ms     u64  inserted (or spent) + TtlMs, saturating
  sequence        u64  insertion order; larger is newer

store_t  (17960 bytes, 8-aligned)
  pending[32]     entry_t   host tokens not yet taken          @0
  spent[32]       entry_t   tokens returned by take, refused   @8960
  next_sequence   u64       sequence of the next insertion; the first is 0   @17920
  last_now        u64       most recent accepted clock value   @17928
  count           u32       occupied pending slots             @17936
  spent_count     u32       occupied spent slots               @17940
  clock_started   u32       0 before the first timed operation, else 1   @17944
  rollbacks       u32       detected clock rollbacks           @17948
  reserved[2]     u32       zero                              @17952
```

All-zero bytes are a valid empty store. No pointers are stored and nothing is
allocated. C callers may initialize with `{0}`.

### 3.3 `put(token, process_id, now)`

1. Validate: token per section 2 and `process_id != 0`. Otherwise return the
   error. Nothing else changes.
2. **Refusals, decided before the clock is observed.** Unless the clock has rolled
   back (`now < last_now`, see 3.4):
   a. A live pending entry with identical bytes exists: `Duplicate`.
   b. A live spent entry with identical bytes exists: `TokenSpent`.
   c. All 32 spent slots are live: `SpentRingFull`. A spent token could not be
      recognized, so no token is accepted until a slot expires.
   A refused put changes nothing at all, including the clock baseline and the
   expiry of other entries.
3. Observe the clock (3.4). Expire stale pending and spent entries.
4. If the pending array is full, evict the pending entry with the smallest
   sequence (the oldest). This evicted token was never returned, so it is not a
   double return; it is dropped by design.
5. Insert into the first free pending slot with the next sequence.

### 3.4 `take_for_process(process_id, now, out[256])`

1. `process_id == 0` is `InvalidProcess` and changes nothing.
2. Observe the clock; expire stale entries.
3. Select the newest live pending entry for the process (largest sequence).
   None: return null.
4. Copy its token into `out[0..len]` and write `out[len] = 0`. Bytes after the NUL
   are not written. Return `len`.
5. Remove the pending entry and record the token as spent with
   `deadline = now + TtlMs`. If the spent ring is full, evict its oldest entry
   first. This is safe: while the ring is full, every put is refused with
   `SpentRingFull`, so an evicted token cannot be accepted again before its
   own 30 s window ends.

A token is returned at most once: after step 4 it can only be refused, never
returned again, until its spent deadline passes.

### 3.5 Clock and rollback

- `observe(now)`: when `clock_started` and `now < last_now`, the clock rolled back.
  Pending and spent entries are both dropped, `rollbacks` is incremented, and
  the baseline becomes `now`. Otherwise the baseline advances to `now` if it is
  larger.
- Rationale: after a rollback, stored deadlines cannot be compared with the
  clock, so both arrays are dropped. Dropping spent entries means a token taken
  before the rollback is accepted again. This is the accepted trade-off of a
  clock fault and is observable through `rollbacks`.
- A rollback never refuses: it is detected before the refusal checks, so a put
  that observes a rollback inserts its token after the clear.

### 3.6 `expire(now)` and `clear()`

- `expire(now)`: observe the clock, expire stale entries of both arrays, return
  the number of pending entries removed.
- `clear()` (C: `interop_activation_clear`): removes every pending entry. Spent
  entries and clock state are kept, so a token taken before the clear is still
  refused until its deadline.

### 3.7 Errors and C ABI status codes

Zig: `error_t = error{ InvalidToken, InvalidProcess, Duplicate, TokenSpent, SpentRingFull }`
for `put`. `take_for_process` returns `error{InvalidProcess}!?usize`.

| Function | Codes |
|---|---|
| `interop_activation_put` | 0 Ok; 1 InvalidArgument (NULL store, or NULL token with length above 0); 2 InvalidToken; 3 InvalidProcess; 4 Duplicate; 5 TokenSpent; 6 SpentRingFull |
| `interop_activation_take` | 0 Found; 1 InvalidArgument (NULL store, out or length_out); 2 NotFound; 3 InvalidProcess |
| `interop_activation_pick_token` | 0 Found; 1 none valid; 2 InvalidArgument (NULL output) |

`interop_activation_expire` returns the pending count removed (0 for a NULL store).
`interop_activation_clear` is a no-op for a NULL store.

## 4. Threading and ownership

- The store is caller-owned, 17960 bytes, 8-byte aligned. Not thread-safe:
  one owner thread serializes every call on one store.
- Independent stores are independent.
- The take output is a copy. Candidate strings for `pick_activation_token` are
  borrowed and must outlive the returned pointer.

## 5. Stricter readings

- A token is refused for 30 s after it is taken, whoever asks. The window is the
  same 30 s as the pending lifetime.
- After the window ends, a token is accepted again. That is a new launch
  request only if the host generates it; the store does not remember tokens
  past their window.
- Multiple live pending tokens for one process id: take returns the newest.
- Eviction of a pending token when the pending array is full drops that token
  without returning it.

## 6. Test plan

- Token rule boundaries (empty, 255 accepted, 256 rejected, 0x20, 0x21, 0x7E,
  0x7F, multi-byte UTF-8 rejected) and `pick_activation_token` order and
  fallbacks.
- Put and take: exact once; wrong process leaves the entry; process id 0 is
  InvalidProcess and changes nothing.
- Refusals change nothing: Duplicate leaves the clock baseline and the owner
  unchanged; TokenSpent and SpentRingFull likewise.
- Spent window: TokenSpent at `t+29999`, accepted at `t+30000`.
- Spent ring full: 32 live spent entries refuse any new token with SpentRingFull
  until one expires; `take` on a full ring evicts the oldest spent entry and the
  evicted token stays refused by the ring-full rule.
- Pending expiry boundary at 29999 and 30000 ms; eviction only after expiry.
- Rollback clears both arrays, increments `rollbacks`, and never refuses or
  returns a pending token; rollback during put inserts the new token.
- Saturation: a deadline at the `u64` maximum expires only at `now == u64` maximum.
- Newest-wins for a shared process id.
- Take output is NUL-terminated at index length, and bytes after it are untouched.
- Randomized: 30000 seeded operation streams over 40 tokens and 5 process ids,
  with forward steps and occasional rollbacks, compared step by step against a
  reference model (two lists, liveness by elapsed time, explicit order counter).
  It predicts Ok, Duplicate, TokenSpent, the taken token, expire counts, live
  counts and the rollback count. SpentRingFull is covered by directed tests, not
  by the random stream, which does not fill 32 live spent slots.
- C consumer: layout constants, status codes 0..6, the 256-byte NUL-terminated
  take buffer, NULL handling, pick statuses, and the 30 s boundary.
- Gates: `python3 tests/av/coverage.py activation_store` >= 90 percent line and
  taken branch; `interop-activation-store-test`; the sanitizer target runs the C
  consumer under `-fsanitize=address,leak,undefined`. Zig sanitizer execution is
  not claimed; the module allocates nothing.
