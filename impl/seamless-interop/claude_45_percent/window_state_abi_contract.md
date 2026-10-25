# Window-State Reducer C ABI Contract

Scope of this document: the C ABI over the portable reducer in
`src/interop/window_state.zig`. The reducer's own rules are fixed by
`impl/seamless-interop/window_state_contract.md` and are not restated here
except where the ABI adds a check. This ABI is a prerequisite for TODO #13
(native adapters, 25%; wire and compositor, 15%). It earns no progress credit on
its own.

## 1. Scope and boundaries

Inside scope:

- Five exported functions with `waddle_window_state_*` names, declared in
  `src/interop/window_state.h` and implemented in `src/interop/window_state_abi.zig`.
- Fixed-size, union-free plain-data structs: input, action, plan, snapshot.
- Argument validation of every raw C word, a per-handle integrity seal on plans,
  and status mapping of every reducer error.

Outside scope:

- Any platform call, native handle, thread, socket, or allocation other than the
  one handle made by `create`.
- The native adapters, the wire or compositor connection, and the coordinator
  C module (`src/interop/window_coordinator.c/.h`). Those consume this ABI later.
- Any change to `window_state.zig`. The ABI imports it and uses its public
  declarations only.

Files owned by this contract:

| Path | Role |
|:--|:--|
| `src/interop/window_state.h` | Public C declarations, status and kind values, struct layouts |
| `src/interop/window_state_abi.zig` | Exports, validation, seal, Zig tests |
| `tests/interop/window_state_abi.c` | C consumer tests, static layout assertions |
| `scripts/interop_lane_coordinator.mk` | `interop-window-state-abi-{test,sanitizers,coverage}` |
| `tests/av/coverage_modes/window_state_abi` | Empty marker for the repository recorder |

## 2. Layouts

All integers are little-endian on the supported x86_64 target. The ABI requires
a 64-bit `usize` and rejects other targets at compile time. Every field offset
below is the byte offset from the start of the struct. There is no implicit
padding inside a struct except where the last row says so.

### 2.1 `waddle_window_state_input_t`: size 28, alignment 4

| Offset | Type | Field | Meaning |
|--:|:--|:--|:--|
| 0 | `uint32_t` | `kind` | Input kind value (section 4.1) |
| 4 | `uint32_t` | `state` | Guest state for GuestObservation; configure state for HostConfigure |
| 8 | `int32_t` | `x` | HostConfigure x, passed through unchanged |
| 12 | `int32_t` | `y` | HostConfigure y, passed through unchanged |
| 16 | `uint32_t` | `width` | HostConfigure width |
| 20 | `uint32_t` | `height` | HostConfigure height |
| 24 | `uint32_t` | `maximize` | HostStateRequest only: 1 maximize, 0 unmaximize |

Fields not used by the selected kind are ignored.

### 2.2 `waddle_window_state_action_t`: size 24, alignment 4

| Offset | Type | Field | Meaning |
|--:|:--|:--|:--|
| 0 | `uint32_t` | `kind` | Action kind value (section 4.2) |
| 4 | `int32_t` | `x` | SetGeometry x |
| 8 | `int32_t` | `y` | SetGeometry y |
| 12 | `uint32_t` | `width` | SetGeometry width |
| 16 | `uint32_t` | `height` | SetGeometry height |
| 20 | `uint32_t` | `maximized` | SetGeometry maximized flag, 0 or 1 |

For every kind other than SetGeometry, offsets 4 through 20 are zero.

### 2.3 `waddle_window_state_plan_t`: size 248, alignment 8

| Offset | Type | Field | Meaning |
|--:|:--|:--|:--|
| 0 | `uint64_t` | `window_id` | Identity the plan applies to |
| 8 | `uint64_t` | `base_revision` | Machine revision read during planning |
| 16 | `uint64_t` | `action_count` | Initialized actions, 0 to 8 |
| 24 | `uint64_t` | `seal` | Integrity tag (section 5) |
| 32 | `uint32_t` | `generation` | Identity the plan applies to |
| 36 | `uint32_t` | `pending_kind` | Completion payload kind (section 4.3) |
| 40 | `uint32_t` | `pending_state` | Payload word, meaning by kind |
| 44 | `uint32_t` | `pending_capabilities` | GuestObservation capability bits |
| 48 | `uint32_t` | `pending_host_state` | GuestObservation persistent host state |
| 52 | `waddle_window_state_action_t[8]` | `actions` | Ordered actions; entries at index >= `action_count` are zero |
| 244 | bytes 244 to 247 | (tail) | Zero padding |

Callers must return a plan unmodified to `complete`. Section 5 defines the seal
that detects edits.

### 2.4 `waddle_window_state_snapshot_t`: size 56, alignment 8

| Offset | Type | Field | Meaning |
|--:|:--|:--|:--|
| 0 | `uint64_t` | `window_id` | Bound identity |
| 8 | `uint64_t` | `revision` | Current revision |
| 16 | `uint32_t` | `generation` | Bound generation |
| 20 | `uint32_t` | `observed_guest_state` | Last guest state bits (window bits only) |
| 24 | `uint32_t` | `applied_guest_state` | Last completed guest-native state |
| 28 | `uint32_t` | `applied_host_state` | Persistent maximized and fullscreen bits |
| 32 | `uint32_t` | `guest_capabilities` | Last guest capability bits |
| 36 | `uint32_t` | `host_request` | Maximize policy word (section 4.4) |
| 40 | `uint32_t` | `synced` | 0 or 1 |
| 44 | `uint32_t` | `close_posted` | 0 or 1 |
| 48 | `uint32_t` | `retired` | 0 or 1 |
| 52 | bytes 52 to 55 | (tail) | Zero padding |

The ABI tests and the C test verify each size, alignment and offset with
`_Static_assert` or comptime checks. A change to any of them fails both builds.

## 3. Handle and ownership

- `waddle_window_state_create` is the only allocating function. It makes exactly
  one allocation of the internal handle, through `abi_allocator`, which defaults
  to `std.heap.c_allocator`. It makes no other allocation, no OS call, and no
  random-number call.
- The handle contains the reducer's `machine_t` and a 64-bit seal seed. The seed
  is `mix64(e)`, where `e` is the value that `create` takes from a process-wide
  atomic counter (`fetchAdd(1, monotonic)`, starting at 0). `mix64` is the
  splitmix64 finalizer, which is bijective, so two handles created in the same
  process never share a seed until the counter wraps after 2^64 creations.
  An atomic add cannot fail, so `create` has no failure path beyond the single
  allocation.
- The caller owns the handle until `waddle_window_state_destroy`. Destroy frees it
  through the same allocator and sets the caller's slot to NULL.
- A destroyed handle, and every copy of its pointer, is dangling. The ABI does
  not detect use after destroy. Callers must not use it.
- No function retains a caller pointer after it returns. Plans and snapshots are
  copied into caller storage. Inputs are read by value.
- The seal seed is never exposed to C.

## 4. Enumerations

Every enumeration value is frozen. Values are stored as `uint32_t` in structs so
that an out-of-range word reaches the ABI check instead of being an undefined
enum value.

### 4.1 Input kinds

| Value | Name | Fields read |
|--:|:--|:--|
| 1 | `WaddleWindowStateInputGuestObservation` | `state` |
| 2 | `WaddleWindowStateInputHostConfigure` | `state`, `x`, `y`, `width`, `height` |
| 3 | `WaddleWindowStateInputHostStateRequest` | `maximize` |
| 4 | `WaddleWindowStateInputCloseRequest` | none |
| 5 | `WaddleWindowStateInputHideObserved` | none |
| 6 | `WaddleWindowStateInputDestroyObserved` | none |

### 4.2 Action kinds

The values are the reducer's `action_t` variants in declaration order.

| Value | Name | Reducer variant |
|--:|:--|:--|
| 1 | `WaddleWindowStateActionHostSetMinimized` | `HostSetMinimized` |
| 2 | `WaddleWindowStateActionHostSetMaximized` | `HostSetMaximized` |
| 3 | `WaddleWindowStateActionHostUnsetMaximized` | `HostUnsetMaximized` |
| 4 | `WaddleWindowStateActionHostSetFullscreen` | `HostSetFullscreen` |
| 5 | `WaddleWindowStateActionHostUnsetFullscreen` | `HostUnsetFullscreen` |
| 6 | `WaddleWindowStateActionGuestRestore` | `GuestRestore` |
| 7 | `WaddleWindowStateActionGuestMaximize` | `GuestMaximize` |
| 8 | `WaddleWindowStateActionGuestMinimize` | `GuestMinimize` |
| 9 | `WaddleWindowStateActionGuestEnterFullscreen` | `GuestEnterFullscreen` |
| 10 | `WaddleWindowStateActionGuestLeaveFullscreen` | `GuestLeaveFullscreen` |
| 11 | `WaddleWindowStateActionGuestActivate` | `GuestActivate` |
| 12 | `WaddleWindowStateActionGuestSetGeometry` | `GuestSetGeometry` (with geometry payload) |
| 13 | `WaddleWindowStateActionGuestPostClose` | `GuestPostClose` |

### 4.3 Completion payload kinds (`pending_kind`)

| Value | Name | Payload |
|--:|:--|:--|
| 1 | `WaddleWindowStatePendingGuestObservation` | `pending_state`, `pending_capabilities`, `pending_host_state` |
| 2 | `WaddleWindowStatePendingHostConfigure` | `pending_state` |
| 3 | `WaddleWindowStatePendingHostStateRequest` | `pending_state` is 0 or 1 |
| 4 | `WaddleWindowStatePendingCloseRequest` | none |
| 5 | `WaddleWindowStatePendingRetire` | none |

Value 0 is never valid. An all-zero plan therefore fails `complete` with
`InvalidPlan`.

### 4.4 Maximize policy word (`host_request` in the snapshot)

| Value | Name | Meaning |
|--:|:--|:--|
| 0 | `WaddleWindowStateHostRequestNone` | No request has completed |
| 1 | `WaddleWindowStateHostRequestUnmaximize` | The last completed request was unmaximize |
| 2 | `WaddleWindowStateHostRequestMaximize` | The last completed request was maximize |

### 4.5 Status codes

Every function returns one of these. Zero is the only success value.

| Value | Name | Produced by |
|--:|:--|:--|
| 0 | `Ok` | Every function, on success |
| 1 | `InvalidArgument` | Any NULL pointer; a boolean word other than 0 or 1; an input kind outside 1 to 6 |
| 2 | `OutOfMemory` | `create` allocation failure |
| 3 | `InvalidIdentity` | `create` with window_id 0 or generation 0 |
| 4 | `InvalidState` | Guest state with Activated or an undefined bit; configure state with a capability or undefined bit |
| 5 | `InvalidGeometry` | Configure with exactly one zero extent, or an extent above 16384 |
| 6 | `StaleWindow` | `plan` with an identity other than the bound identity |
| 7 | `StalePlan` | `complete` with a plan whose base revision is not the current revision |
| 8 | `Retired` | Any plan or completion after a Hide or Destroy completion |
| 9 | `Unsynchronized` | Host-side plan (configure, request, close) before a successful observation |
| 10 | `UnsupportedStateRequest` | Maximize request while the guest lacks the Maximizable capability |
| 11 | `RevisionExhausted` | Revision equals UINT64_MAX |
| 12 | `InvalidCompletion` | Completed count above the plan length; success with a count different from the plan length; Retire completion that failed or reported actions |
| 13 | `InvalidPlan` | Plan with an unknown pending or action kind, an action count above 8, a boolean field outside 0 or 1, or a seal mismatch |

## 5. Check order and plan integrity

### 5.1 `waddle_window_state_plan`

1. `out_plan` NULL: `InvalidArgument`. Otherwise `*out_plan` is set to all zero.
   Every later non-Ok status leaves it zero.
2. `handle` NULL, or `input` NULL: `InvalidArgument`.
3. Input decoding: kind outside 1 to 6, or a HostStateRequest `maximize` other
   than 0 or 1: `InvalidArgument`.
4. Reducer `plan`, in the reducer's order: identity (`StaleWindow`), retirement
   (`Retired`), revision exhaustion (`RevisionExhausted`), synchronization
   (`Unsynchronized`, unless the input is GuestObservation, Hide or Destroy), then
   the input's own rules (`InvalidState`, `InvalidGeometry`,
   `UnsupportedStateRequest`).
5. On Ok, the reducer plan is encoded into `*out_plan` and sealed.

### 5.2 `waddle_window_state_complete`

1. `handle` NULL, `plan` NULL, or `succeeded` other than 0 or 1: `InvalidArgument`.
2. Plan decoding, before the reducer runs. Each failure is `InvalidPlan`:
   - `action_count` greater than 8;
   - `pending_kind` outside 1 to 5;
   - HostStateRequest `pending_state` other than 0 or 1;
   - an action kind outside 1 to 13;
   - a SetGeometry `maximized` other than 0 or 1;
   - a seal that does not match the recomputed value.
3. Reducer `complete`, in the reducer's order: identity (`StaleWindow`), retirement
   (`Retired`), base revision (`StalePlan`), completed count
   (`InvalidCompletion`), revision exhaustion (`RevisionExhausted`), Retire rule
   (`InvalidCompletion`).
4. Every non-Ok status leaves the machine unchanged. Only Ok changes it.

### 5.3 Seal

- Algorithm: `std.hash.Wyhash` with seed = the handle's `seal_seed`. The hash is
  computed over these values in this order, each as its native-endian bytes:
  `window_id`, `base_revision`, `action_count`, `generation`, `pending_kind`,
  `pending_state`, `pending_capabilities`, `pending_host_state`, then for each
  action with index below `action_count`: `kind`, `x`, `y`, `width`, `height`,
  `maximized`.
- The `seal` field itself is never hashed. Bytes of actions at index
  `action_count` and above are never hashed.
- Purpose: detect accidental edits, plans issued by a different handle, and plans
  issued by a handle that was destroyed and recreated with the same identity. Each
  handle has a distinct seed (section 3), so the second handle rejects the first
  handle's plan even when both have the same identity and revision.
- Predictability: the seed is a function of creation order, which an attacker
  inside the process can observe. That is acceptable under the stated limit below.
- Limits: this is not a cryptographic authenticator. The seed is not secret. A
  forger who knows the handle's seed, for example one who can read its memory or
  one who can count the handles created in the process, can compute valid tags
  for chosen plans. The reducer remains memory safe for any input,
  so a forged plan can change only the state of its own handle. A caller who can
  read the handle is outside the trust boundary.

## 6. Threading

- A handle belongs to one caller at a time. The caller serializes every call
  that uses the same handle.
- `plan` and `snapshot` only read the handle. They may run concurrently with each
  other on one handle when no `complete` or `destroy` runs on it.
- `complete` and `destroy` mutate or free the handle. They must not run
  concurrently with any other call on that handle.
- Calls on distinct handles are independent and may run on any threads.
- `create` uses `abi_allocator` and the process-wide seal counter. `create` may run
  on any thread. The counter is an atomic and `c_allocator` is thread-safe.
  `abi_allocator` is a global variable that tests replace; no production code
  writes it.

## 7. Error handling summary

- Every error leaves the handle unchanged, except that `create` on failure leaves
  the output slot NULL, `destroy` on success always leaves the slot NULL, and
  `plan` on failure leaves the output plan all zero.
- No function aborts, logs, or waits on a lock or an OS service. The only
  operation that can fail is the allocation in `create`, which reports
  OutOfMemory. The allocator is the one external dependency and is the caller's
  responsibility if it blocks.
- A caller passing a pointer that is neither NULL nor a live handle (for example a
  destroyed handle) has undefined behavior; the ABI cannot detect it.

## 8. Verification

Zig, `zig test src/interop/window_state_abi.zig -Iinclude -Isrc/interop -lc`
(17 ABI tests; the imported reducer and graph tests also run):

- create: NULL slot, zero identity, forbidden guest state, double-destroy
  idempotence, NULL-slot destroy, FailingAllocator at the single allocation
  (OutOfMemory, slot NULL), and success when the injected index is never reached.
- Ticket flow: reconciliation order (one-shot minimize, unmaximize, unfullscreen),
  restore with activation and geometry, and the zero-length plan.
- Argument checks: NULL handle, input, output and plan; every bad kind and
  boolean word; plan output zeroed on every failure.
- Misaligned caller storage read and written by value.
- Every reducer action kind survives encode and decode; unknown action kinds are
  InvalidPlan.
- Every status mapping, and every plan, completion, identity, sync, state,
  geometry, capability and revision error through real calls.
- Stale plans, partial failure and resynchronization, retirement, the maximize
  request and its snapshot words.
- Forged plans: edited identity, pending word and action; a plan from another
  handle; a plan with an oversized count; resealed bad pending, action, boolean
  and maximized values; NULL arguments of complete; state unchanged after each.
- RevisionExhausted through a resealed plan at UINT64_MAX.
- A fixed-seed random sequence of 12 rounds and up to 300 steps each, with
  replays of earlier plans. Every ABI status and every encoded action is compared
  with a direct reducer machine driven in lockstep; the full machine state is
  compared after each step. The test asserts that the run includes more than 200
  accepted plans, more than 100 accepted completions and at least one rejected
  completion.

C, `tests/interop/window_state_abi.c`, built with `-std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG`:

- Ten tests: invalid arguments, lifecycle and slot clearing, ticket flow and
  action order, partial failure with resynchronization, stale plans and identity,
  the maximize capability gate, retirement, forged and foreign plans, misaligned
  storage, and read-only snapshots.
- Static assertions for every size, alignment and offset in section 2, and for
  the frozen status and kind values.

Sanitizers: the C test under `-fsanitize=address,leak,undefined` with
`ASAN_OPTIONS=detect_leaks=1`. The Zig object is not instrumented. Its
allocations go through libc `malloc`, so LeakSanitizer sees them. Zig ownership is
proven by `std.testing.allocator` in the Zig tests, not by a sanitizer.

Coverage: `python3 tests/av/coverage.py window_state_abi`. The recorder needs
`-Isrc/interop` for this module's `@cInclude("window_state.h")`, which requires
the one-line change described in the lane commit. With that change, measured on
the patched recorder: branch 100.00% (34 of 34), line 97.04% (131 of 135). The four
line records not attributed are the `return switch (err)` header lines of
`status_of_input_error`, `status_of_plan_error`, `status_of_complete_error` and
`status_of_decode_error`. Every arm those switches dispatch to is covered by the
branch measure and by the mapping test. The recorder does not report a hit for the
header instruction itself; the cause is not established.

Known gaps: none in the portable surface. Native adapters, the compositor
connection and the coordinator's use of this ABI are not implemented and earn no
credit under this contract.
