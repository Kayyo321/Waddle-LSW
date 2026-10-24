# Implementation Description: Guest Window Graph (TODO #10 slice)

## 1. Title & High-Level Scope

This task delivers the session-wide guest window graph and its Win32 observation adapter. The graph is a pure,
bounded state machine written in Zig behind a C ABI. It receives value snapshots of native windows ("facts"),
classifies them with the frozen table of `impl/seamless-interop/IMPL_DESC.md` section 4.4, assigns monotonic
identities `(session_epoch, window, generation)`, orders parents before children, bounds all storage, and emits value
events (Announce, Update, RetireIntent) into a bounded ring. It never calls the operating system. The observer is a
C11 adapter that turns WinEvent hints and enumeration into facts through an injectable API table and feeds a
caller-owned graph. Retirement of a surface is quarantine: only the session coordinator ends it, with proof that
capture stopped and every frame pin drained (`interop_window_graph_complete_retire`). Loss of the connection is never
proof.

### 1.1 In scope

1. Public C ABI `include/waddle/interop_window_graph.h`: constants, enums, 10 fixed-layout value structs (the nine
   facts, event, observation, live, retiring entry, retire proof, capacity diagnostic, stats, config structs plus the
   24-byte classification verdict) with `_Static_assert` size and offset checks, 3 callback typedefs and 21 functions
   (create, destroy, classify, observe, note_destroyed, complete_retire, check_action, invalidate_all, refresh_begin,
   refresh_offer, refresh_keep, refresh_unseen, refresh_commit, refresh_abort, next_event, drain,
   take_capacity_diagnostic, get_stats, stamp_high_water, find_live, list_retiring). `interop_window_graph_classify` is
   a pure export that needs no graph; the observer uses it to classify an owner window (OwnerHidden, section 4.4).
2. Zig engine `src/interop/window_graph.zig`: facts validation (trust boundary), classification (steps 0 to 8, with
   the OwnerHidden refinement of section 4.4), identity tuple and generations, deferred (parent-pending) records,
   capacity limits, retirement cascade, 64-bit unwrapping of the 32-bit tick values, refresh/seen-mark protocol
   (offer, keep, unseen, commit), event ring with coalescing, capacity diagnostics, counters. The engine uses the C
   structs of the header directly through `@cImport`; there are no Zig mirrors of public structs.
3. Zig ABI layer `src/interop/window_graph_abi.zig`: opaque handle, pointer validation in header order, output
   staging, the single allocation seam `abi_allocator`.
4. C11 observer `src/interop/window_observer_windows.{h,c}`: dirty-hint table, logical clock, hook lifecycle,
   pump, full refresh (enumeration, per-window query, keep/unseen completion), 250 ms cadence helpers, three
   out-of-context hooks, a busy flag against re-entrancy, owner-hidden computation, 11 functions plus
   `window_observer_native_api` under `_WIN32`, with all Win32 access behind `window_observer_api_t`.
5. Tests: `tests/interop/window_graph.c`, `tests/interop/window_observer_windows.c`,
   `tests/interop/window_observer_api_double.h`, Zig test blocks below `// Test-only fixtures.`.
6. Standalone build `scripts/interop_window_graph.mk` (targets prefixed `window-graph-`, output under
   `build/window_graph/`) and these task-local documents.

### 1.2 Out of scope (explicit)

- No FrameReady, no frame pins, no capture, no heap runs, no shared-memory or slot runtime, no transport.
- No wire serialization: events are C value structs; mapping to `interop_protocol.h` messages is the coordinator's.
- No input injection, no Wayland or host-side code, no clipboard, no cursor.
- No application catalog and no icons: the app key comes from the caller-supplied `app_key_lookup` callback.
- No minimum-size query: `min_width` and `min_height` are always reported as 0 (no `WM_GETMINMAXINFO`).
- No native UAC pipeline: mode Native only hides the consent dialog from export; reading it is the coordinator's job.
- No message is ever sent to a window of the agent's own process, and no title is read for one (AgentOwned windows
  report an empty title); no message is sent to foreign windows either.
- No session main loop: the observer exposes `refresh_due` and `next_timeout_ms`, the caller owns the loop.
- No edit of `README.md`, global `TRACKER.md`, global `IMPL_DESC.md`, `GNUmakefile`, `scripts/interop.mk`,
  `tests/av/coverage.py`, `include/waddle/interop_protocol.h`, and no global TODO #10 percentage is claimed.
- No new library, no submodule, no Rust, Go or C++.

### 1.3 Relation to global TODO #10 and remaining work for Codex

This slice provides the graph and observation half of TODO #10; it is not credited in the global tracker. Remaining
for Codex: (a) map graph events to protocol messages and order them against FrameReady; (b) the session main loop
(message pump, `next_timeout_ms`, pump budget, drain, `complete_retire` after slot-runtime proof, epoch roll-over and
starting a replacement observer above `stamp_high_water`); (c) the application catalog behind `app_key_lookup`, and
icons; (d) the minimum-size query; (e) the native UAC pipeline for mode Native; (f) execution of the native Win32
table on a real Windows guest (here it is only cross-compiled), including confirming that `GetTickCount` shares the
base of the WinEvent `dwmsEventTime`; (g) global tracker credit and merging this document into the global description.

## 2. Architecture & Inter-Component Interactions

### 2.1 Component diagram

```
                      Windows guest, one thread (message loop owner)
 +--------------------------------------------------------------------------------+
 | WinEvent hook proc --note_hint--> [dirty table 512] <--pop-- window_observer_pump |
 |  (hints only, no facts)                  |                        |               |
 |                                          |   query_facts(hwnd)    v               |
 |  EnumWindows ---------- refresh ---------+-----> facts (1656 B value, stamped)    |
 +------------------------------------------|---------------------|---------------+
                     interop_window_graph.h  |  C ABI              |
 +------------------------------------------v---------------------v---------------+
 | window_graph_abi.zig: validate handle/thread/reentry/epoch, stage outputs         |
 | window_graph.zig:  validate_facts -> classify -> plan -> pre-check -> 1 lookup -> apply|
 |   entries[64] (Active|Retiring)   deferred[64]   ring[256] events   counters        |
 +-----------------------------------------|---------------------------------------+
        app_key_lookup (only callback)       | next_event / drain (value copies)
        ^                                    v
 +------|------------------------------------------------------------------------+
 | session coordinator (Codex): maps events to wire, stops capture, drains pins,    |
 |   then complete_retire(epoch, window, generation, {capture_stopped, pins_drained}) |
 +----------------------------------------------------------------------------------+
```

Flow: hint (no authority except Destroy, which only proposes) -> dirty table -> requery on the owner thread -> facts
-> `observe` / `note_destroyed` / `refresh_offer` -> plan (read-only) -> pre-check (facts, stamp, classification,
cycle, capacity, generation, ring room) -> single `app_key_lookup` -> infallible apply -> events in ring -> drained by
the coordinator. The coordinator's retirement proof enters only through `complete_retire`, after it has consumed the
RetireIntent event, stopped capture and drained pins; that call is the sole transition Retiring -> Free.

### 2.2 Layers

| File | Language | Role | Caller |
|---|---|---|---|
| `include/waddle/interop_window_graph.h` | C11 header | Public ABI, constants, layouts, documentation | Observer, tests, coordinator, `window_graph_abi.zig` via `@cImport` |
| `src/interop/window_graph.zig` | Zig 0.13 | Engine: validation, classification, storage, identity, events | `window_graph_abi.zig` and Zig tests only |
| `src/interop/window_graph_abi.zig` | Zig 0.13 | `export fn interop_window_graph_*`, handle, allocation seam | C callers (observer tests, coordinator) |
| `src/interop/window_observer_windows.h` | C11 header | Observer ABI, api table, status enums | Coordinator, tests |
| `src/interop/window_observer_windows.c` | C11 | Portable core (dirty table, pump, refresh, lifecycle) plus `#ifdef _WIN32` native table | Coordinator, hook proc, tests |
| `tests/interop/window_graph.c` | C11 | ABI layout, link and behaviour test of the graph | `make window-graph-*` |
| `tests/interop/window_observer_windows.c`, `window_observer_api_double.h` | C11 | Double-driven lifecycle tests with the real graph | `make window-graph-*` |
| `scripts/interop_window_graph.mk` | make | Standalone targets, not included by `GNUmakefile` | Developer, CI |

Zig is used for the engine because it parses untrusted input (facts, application keys) and needs bounds-checked
buffers; C is the default language for the adapter and the ABI; no C++ is required because no Windows API with only a
C++ interface is used (DWM, user32, kernel32 expose C interfaces).

### 2.3 Ownership of every allocation

| Object | Allocator | Owner | Freed by | Notes |
|---|---|---|---|---|
| `graph_t` (entries, deferred, ring, counters, config copy) | exactly one `abi_allocator.create(graph_t)` (default `std.heap.c_allocator`) | the C caller from create until destroy | `interop_window_graph_destroy`, once; caller slot set to NULL | No other heap use: no list, no string copy, no growth. |
| `window_observer_t` (state, api copy, dirty[512], enum_buffer[2048], stats) | exactly one `calloc` | the C caller from create until destroy | `window_observer_destroy`; slot set to NULL | Process-wide `g_active_observer` is a non-owning pointer. |
| `interop_window_facts_t` | caller stack or caller storage | caller | caller | Graph copies fields it keeps; nothing borrowed survives the call. |
| Events, observation, live, stats, diagnostic outputs | caller storage | caller | caller | Graph writes a staged local and copies it only on Ok. |
| App key bytes | inline `[200]u8` in `record_t` | graph | with `graph_t` | The callback writes into a graph-provided buffer; no pointer is retained. |
| WinEvent hook tokens | OS | observer | `stop` or `destroy` | Removed in reverse install order. |

Neither component allocates after create. The observer never owns, retires or frees graph state; the graph owns no
external resource (no heap run, mapping, capture, thread, timer or handle), so `destroy` discards quarantined
entries without proof and the coordinator must call `list_retiring` first.

## 3. Data Structures, Protocols & Memory Layouts

### 3.1 Public structs (authoritative layouts and field docs are in `interop_window_graph.h` and `window_observer_windows.h`)

All structs have 8-byte alignment, no implicit padding holes (every gap is an explicit `reserved0`, except the
observer config whose gap is `uac_mode`), and are guarded by `_Static_assert` on size and every offset;
`sizeof(void *) == 8` is asserted. The engine includes the header through `@cImport` and uses these C structs
directly (no Zig `extern struct` mirrors exist); `comptime` asserts check the `@cImport` sizes and offsets that the
engine relies on.

| Struct | Size | Key offsets | Fill and zero rules |
|---|---:|---|---|
| `interop_window_facts_t` | 1656 | window 0, owner 8, owner_root 16, root 24, process_start_time 32, stamp 40, style 48, ex_style 52, flags 56, process_id 60, thread_id 64, captured_ms 68, x 72, y 76, width 80, height 84, dpi 88, min_width 92, min_height 96, class_length 100, image_length 104, title_length 108, alpha 112, reserved0 113 (3 bytes), class_name 116 (256), image_name 372 (260), title 632 (1024) | Input. Caller zero-fills the whole struct; `reserved0` must be 0 (else InvalidFacts); bytes past each stated length are ignored and never copied. `flags` holds 13 bits (`InteropWindowFactMask` 0x1FFF); bit 12 is `InteropWindowFactOwnerHidden`, valid only with `owner != 0` (else InvalidFacts). `captured_ms` stays a 32-bit wire tick. |
| `interop_window_verdict_t` | 24 | kind 0, role 4, reason 8, reserved0 12, parent_window 16 | Output of `interop_window_graph_classify`, written only on Ok. `kind` is `interop_window_verdict_kind_t` (Eligible 1, NotExported 2, Suspended 3); `role` and `parent_window` are meaningful only for Eligible, `reason` only for NotExported; unused fields and `reserved0` are 0. |
| `interop_window_event_t` | 1328 | session_epoch 0, window 8, generation 16, parent_window 24, parent_generation 32, x 40, y 44, width 48, height 52, kind 56, role 60, state 64, dpi 68, min_width 72, min_height 76, process_id 80, changed_mask 84, reason 88, title_length 92, app_key_length 96, reserved0 100, title 104 (1024), app_key 1128 (200) | Output. Engine zero-initializes the event, then fills; `reserved0` and every byte past `title_length` / `app_key_length` are 0, so no stale bytes leak. |
| `interop_window_observation_t` | 56 | window 0, generation 8, retired_generation 16, disposition 24, role 28, reason 32, events_queued 36, entries_admitted 40, entries_retired 44, deferred_dropped 48, reserved0 52 | Output, fully written on Ok, untouched otherwise. `reserved0` is 0. |
| `interop_window_live_t` | 48 | window 0, generation 8, parent_window 16, parent_generation 24, role 32, state 36, process_id 40, reserved0 44 | Output, fully written on Ok. |
| `interop_window_retiring_entry_t` | 24 | window 0, generation 8, reason 16, role 20 | Output array element; ascending generation. |
| `interop_window_retire_proof_t` | 8 | capture_stopped 0, pins_drained 4 | Input; each flag must equal exactly 1. |
| `interop_window_capacity_diagnostic_t` | 56 | at_capacity 0, deferred_evicted 8, queue_full 16, generation_exhausted 24, elapsed_ms 32, active_count 40, retiring_count 44, deferred_count 48, events_pending 52 | Output, fully written on Ok. |
| `interop_window_graph_stats_t` | 256 | session_epoch 0, generations_issued 8, stamp_high_water 16, five u32 gauges 24..40, reserved0 44, then 26 u64 counters 48..248 | Output, fully written on Ok; counters saturate at `UINT64_MAX`. |
| `interop_window_graph_config_t` | 48 | session_epoch 0, uac_mode 8, reserved0 12, app_key_lookup 16, app_key_context 24, now_ms 32, now_ms_context 40 | Input, copied at create; `reserved0` must be 0; pointers borrowed, contexts must outlive the graph. |
| `window_observer_api_t` | 64 | install_hook 0, remove_hook 8, query_facts 16, enum_top_level_windows 24, query_input_desktop 32, current_thread_id 40, monotonic_ms 48, tick_ms 56 | Eight function pointers, all required non-NULL; copied at create. |
| `window_observer_config_t` | 40 | api 0, api_context 8, first_stamp 16, preedit_window 24, own_process_id 32, uac_mode 36 | Input, copied at create; `uac_mode` (0..2, same meaning as the graph config's) is used only for the owner classification (OwnerHidden) and must equal the graph's `uac_mode`; above 2 is InvalidArgument. It replaces the former `reserved0` at the same offset and size. |
| `window_observer_stats_t` | 176 | next_stamp 0, state 8, last_graph_status 12, five u32 gauges 16..28, then 18 u64 counters 32..168 (hints_received 32 ... uac_native_hidden 168) | Output, fully written on Ok. |

Handles `interop_window_graph_t` and `window_observer_t` are opaque; their layout is private. Enum types are 4 bytes
(asserted). Numeric constants (`InteropMaxWindows` 64, `InteropMaxTitleBytes` 1024, `InteropMaxAppKeyBytes` 200, the
role and state values, `InteropStateMinimized` etc.) come from the frozen `interop_protocol.h`; this task never
redefines them.

### 3.2 Private engine state (`window_graph.zig`; names fixed by the naming sheet)

Constants: `MaxEntries` 64, `MaxDeferred` 64, `EventCapacity` 256, `MaxCallEvents` 192, `RefreshIntervalMs` 250,
`DiagnosticIntervalMs` 60000; comptime asserts mirror the header (`MaxCallEvents == 3 * MaxEntries`,
`MaxCallEvents <= EventCapacity`, `MaxDeferred == MaxEntries`, `EventCapacity` a power of two).

`record_t` (plain struct, one incarnation; 1368 bytes: 11 u64 = 88, x/y = 8, 12 u32 = 48, title 1024, app_key 200):

| Field | Type | Unit and valid range |
|---|---|---|
| window | u64 | HWND zero-extended, nonzero |
| generation | u64 | entries: 1..2^64-1 issued by the graph; deferred records: 0 |
| parent_window | u64 | 0 = none; otherwise nonzero and different from window |
| parent_generation | u64 | entries: generation of the Active parent at admission, 0 iff parent_window is 0; deferred: 0 |
| process_start_time | u64 | 100 ns FILETIME units, 0 = unknown |
| class_hash | u64 | FNV-1a 64 of the class bytes (3.5) |
| admit_stamp | u64 | entries: `stamp` of the facts announced (for a promoted deferred record, its `last_stamp`); deferred: 0 |
| last_stamp | u64 | stamp of the newest accepted facts, >= admit_stamp, monotone per window |
| seq | u64 | deferred age from `next_seq` (>= 1); entries: 0 |
| x, y | i32 | guest desktop pixels, any value |
| width, height | u32 | 1..16384 (a zero extent fails the step 8 gate, so no record ever holds 0) |
| role | u32 | protocol `interop_window_role_t` value of the verdict |
| state | u32 | protocol state bits derived by 3.5 |
| dpi | u32 | 96..960 |
| min_width, min_height | u32 | 0..16384, 0 = none |
| process_id, thread_id | u32 | as reported |
| admit_captured_ms, last_captured_ms | u64 | unwrapped 64-bit ticks (`tick_base` unwrap, 3.5), compared with plain `<`; admit_* 0 on deferred |
| retire_reason | u32 | 0 while Active or deferred; 1..16 (`interop_window_reason_t`) once Retiring |
| title_length | u32 | 0..1024; `title: [1024]u8` valid UTF-8, no NUL |
| app_key_length | u32 | 3..200; `app_key: [200]u8` passes `app_key_valid` |

`slot_state_t = enum(u8) { Free, Active, Retiring }` (PascalCase members per AGENTS.md; the naming sheet's lowercase
spelling is superseded). `entry_t = { state: slot_state_t, seen: bool, record: record_t }` (1376 bytes after
rounding to alignment 8). `deferred_t = { used: bool, seen: bool, record: record_t }` (1376 bytes). Invariants:
I-1 a window has at most one of {Active entry, deferred record}; I-2 a window has at most one Active entry and any
number of Retiring entries with distinct generations; I-3 `exported = count(state != Free) <= 64`;
I-4 every child's `generation` exceeds its Active parent's; I-5 deferred records form no cycle.

`graph_t` scalar fields: `allocator` (std.mem.Allocator), `config: interop_window_graph_config_t` (48-byte C struct copy), `owner_thread:
std.Thread.Id` (captured at create), `busy: bool` (set for the whole call by `enter`, cleared by `defer` on every
return path), `generations_issued: u64` (0..2^64-1), `stamp_high_water: u64`, `next_seq: u64` (starts 1,
saturating; ties broken by lowest index), `ring_head: u32` (0..255), `ring_len: u32` (0..256), `refresh_open: bool`,
`last_diag_ms: u64`, `have_diag: bool`, `tick_base: u64` (starts `2^32`, only ever raised), `stats: interop_window_graph_stats_t` (256 B C struct), and four `u64` diagnostic counters
`diag_at_capacity`, `diag_deferred_evicted`, `diag_queue_full`, `diag_generation_exhausted`. Arrays:
`entries: [64]entry_t`, `deferred: [64]deferred_t`, `ring: [256]event_t`.

Single allocation size: entries 64 x 1376 = 88,064; deferred 64 x 1376 = 88,064; ring 256 x 1328 = 339,968; arrays
total 516,096 bytes; scalars and stats add under 1 KiB, so the estimate is about 516.6 KB (504.5 KiB). A `comptime`
assert requires `@sizeOf(graph_t) <= 524288`. Gauges (active, retiring, deferred, events_pending) are recomputed from
the arrays when read, never stored, so they cannot drift.

### 3.3 Event ring

- Storage: `ring: [256]event_t` used as a circular queue; `EventCapacity` is a power of two so the index of the
  i-th oldest event (0 <= i < ring_len) is `(ring_head + i) & 255`. Push writes slot `(ring_head + ring_len) & 255`
  then `ring_len += 1`; pop copies slot `ring_head`, then `ring_head = (ring_head + 1) & 255`, `ring_len -= 1`.
  `ring_room(n)` is `256 - ring_len >= n`. Nothing is overwritten except by coalescing (below); no event is ever
  dropped to make room.
- Per-call bound: the header fixes `InteropWindowGraphMaxCallEvents` at 192 (3 x 64) and asserts it is <= 256. The
  tighter fact is that exported capacity counts Active plus Retiring, so retiring X entries and admitting Y entries in
  one call satisfies X + Y <= 64; 192 is the declared over-approximation and must hold even if a later change adds
  one Update per entry. After a full drain (`ring_len == 0`) any single call therefore fits; with a partly full ring
  a call whose plan needs more room returns QueueFull before any mutation.
- Plan accounting: `plan.event_count` = RetireIntents + Announces + Updates the plan would append. A new Announce
  always counts 1 (a fresh generation has no earlier event). An Update counts 0 when the coalescing scan finds a
  target and 1 otherwise. The scan is read-only, so planning stays side-effect free; apply then repeats the same
  decision on unchanged ring contents.
- Coalescing scan (`ring_push_or_coalesce` for kind Announce or Update): for i from `ring_len - 1` down to 0
  (newest first) read `ev = ring[(ring_head + i) & 255]`; the first event with `ev.window == window and
  ev.generation == generation` decides: if `ev.kind` is Announce or Update, overwrite its payload in place (x, y,
  width, height, state, dpi, min_width, min_height, title, title_length), keep kind (an Announce stays an Announce),
  `changed_mask |= new_mask`, `events_coalesced` +1 and `events_queued` of the call +0; if it is a RetireIntent,
  stop and do not coalesce (events never merge across a retirement); if no event matches, append a new event.
  Cost: at most 256 iterations of two u64 compares. Role, parent fields, app key, process id and epoch never change
  within a generation, so they are not rewritten. An event that was already drained cannot be coalesced into.
- Event construction zero-initializes `event_t`, sets `session_epoch` from the config, copies the record's fields,
  sets `reason` only for RetireIntent (0 otherwise), `changed_mask` to 0x1F for Announce, the diff mask for
  Update, 0 for RetireIntent.
- Ordering guarantees (enforced by apply order, tested): a parent's Announce precedes every child's Announce;
  children's RetireIntents precede their parent's (descending generation); FIFO across calls.

### 3.4 Observer private state (`window_observer_windows.c`)

`dirty_entry_t` (48 bytes): `window` u64 (HWND, nonzero), `kinds` u32 (bit set, below), 4 bytes padding,
`first_stamp` u64, `last_stamp` u64 (both observer stamps), `destroy_stamp` u64 (0 = no Destroy hint pending),
`destroy_event_ms` u32 (tick domain, meaningful only when `destroy_stamp != 0`), `retries` u32
(0..`WindowObserverQueryRetries`). Kind bits are private: Create 1, Destroy 2, Show 4, Hide 8, Name 16, Other 32
(reorder, focus, selection, state change, location change, parent change), Minimize 64 (start and end), Cloak 128 (cloaked and uncloaked). Only the Destroy bit's
companion fields carry authority, and then only as a proposal that `note_destroyed` verifies.

`window_observer_t` fields: `api` (64-byte copy), `api_context`, `owner_thread` (u32 from
`api.current_thread_id` at create), `state` (`window_observer_state_t`), `hook_object`, `hook_system` and `hook_cloak`
(uintptr_t, 0 = not installed), `busy` (0/1, owner thread only, see 5.4), `popped_in_flight` (0/1, a key is held by pump
and one dirty slot is reserved for its put-back), `uac_mode` (u32 from the config; the owner sub-query is folded into the query counters, no extra counter exists), `dirty[512]` plus `dirty_count` (0..512), `next_stamp` (u64, nonzero while valid, 0 means
exhausted), `preedit_window` (u64), `own_process_id` (u32, nonzero), `overflow` and `refresh_requested` and
`desktop_active` (0/1 latches and gauge), `last_refresh_attempt_ms` (u64, monotonic, stamped before the desktop check) with `have_refresh_attempt`, `latch_ms` (u64, monotonic, set whenever the full-refresh latch is set),
`enum_buffer[2048]` (u64 HWNDs), `stats` (176-byte `window_observer_stats_t` plus `last_graph_status`). The single
`calloc` block is `sizeof(window_observer_t)`: dirty 24,576 + enum buffer 16,384 + stats/api/scalars under 1 KiB,
about 41.5 KB. Two process-wide atomics: `static _Atomic(window_observer_t *) g_active_observer` (initially NULL, non-owning) and `static _Atomic(uint32_t) g_hints_in_flight` (initially 0, the in-flight counter of `note_hint`).

Dirty table rules. The table is an insertion-ordered array; index 0 is the oldest first-seen key. `dirty_add`
scans for the window (at most 512 compares): found -> `kinds |=`, `last_stamp` = newest, if Destroy then
`destroy_stamp` and `destroy_event_ms` = latest, `hints_coalesced` +1; absent and count < 512 -> append, order
preserved; absent and full -> `overflow = 1`, `hint_overflows` +1, no entry (the lost key is recovered by
invalidate_all plus a full refresh). "Full" counts the reserved slot: while pump holds a popped key (`popped_in_flight`
== 1) the table is full when `dirty_count + popped_in_flight >= 512`, so the put-back of the popped key always finds a slot
and never forces an overflow (a hint for an already present window is merged and needs no slot). Every accepted hint
increments `hints_received`. `dirty_pop_front` copies entry 0
into a local and shifts the remainder down (at most 511 x 48 bytes) before any api call, so a hint arriving during
the requery re-adds the key with a fresh entry. A Failed query re-inserts the local copy with `retries + 1` at the
tail via `dirty_push_back_retry`, unless the window is already in the table (a newer hint already schedules a
requery; the retry is then dropped) and unless `retries == WindowObserverQueryRetries` (then
`query_retries_exhausted` +1 and the key is dropped). After a graph QueueFull the local copy is put back at the TAIL
(the same helper as the retry, without incrementing `retries`): the coordinator drains and calls again, and the keys
behind it must not starve behind a key that keeps hitting a full ring. After a graph Reentrant status or any other
unexpected graph status the local copy is re-inserted at index 0 by the private helper `dirty_push_front` (merge rules of
`dirty_add` when the window was re-added meanwhile; destroy information keeps the larger `destroy_stamp`). Because of the
reserved slot a put-back always fits; the `overflow` latch in the put-back helpers is a defensive branch only.
The code never holds an index or iterator across an api call.

Hook slots. `start` installs three out-of-context hooks in this order: the object range
[EVENT_OBJECT_CREATE 0x8000 .. EVENT_OBJECT_PARENTCHANGE 0x800F] into `hook_object`, the system range
[EVENT_SYSTEM_MINIMIZESTART 0x0016 .. EVENT_SYSTEM_MINIMIZEEND 0x0017] into `hook_system`, and the DWM range
[EVENT_OBJECT_CLOAKED 0x8017 .. EVENT_OBJECT_UNCLOAKED 0x8018] into `hook_cloak`. The cloak range exists because a DWM
cloak flips the Visible fact without a show or hide event. If any install fails, every hook installed before it is
removed (reverse order) and the state becomes Failed. `stop` and `destroy` remove `hook_cloak`, `hook_system`, then
`hook_object`, attempt all installed hooks even if a removal fails, and set the slot to 0 after each attempt. The hint
filter (`note_hint`) accepts exactly these three ranges.

State machine (`window_observer_state_t`):

```
 create -> Created --start ok--> Started --stop ok--> Stopped --start ok--> Started
              |                    |  \                  |
              |                    |   +--stop: remove_hook fails--> Failed
              +--start: install_hook fails (hooks undone)--------+--> Failed
 Stopped --start: install_hook fails--> Failed
 Failed: only destroy is legal (start, stop, pump, refresh -> BadState)
 Created/Stopped --stop--> same state, Ok;  Started --start--> BadState;  Started --destroy--> stop, then free
 start while another observer holds the slot -> AlreadyActive, state unchanged
```

Process-wide slot and thread rule. `start` publishes `this` with `atomic_compare_exchange_strong(NULL -> this)`
(acq_rel); `stop`, failed-start cleanup and `destroy` clear it with `compare_exchange(this -> NULL)`, always before
freeing, so the slot is non-NULL if and only if that observer is Started. A process-wide `static _Atomic(uint32_t)
g_hints_in_flight` counter protects the observer against a concurrent hint: `note_hint` increments it (seq_cst)
BEFORE it loads the slot and decrements it (release) on every return path; the clearing code (`stop`, failed-start
rollback, `destroy`) clears the slot first and then spins (yielding, no lock, bounded by the duration of one hint, which
never blocks) until the counter reads zero before it returns or frees. A hint that loaded the slot before the clear is
therefore finished before `destroy` frees the observer. `note_hint` loads the slot (acquire); NULL -> return;
`api.current_thread_id` differing from `owner_thread` -> one atomic increment of `hints_wrong_thread` and return without
reading any mutable field; otherwise run the filter and `dirty_add`. Real WinEvent OUT_OF_CONTEXT hooks are delivered
only on the installing thread, so the wrong-thread path exists for doubles; with the in-flight counter an off-thread
double can no longer touch freed memory after `destroy` returns. `note_hint` never reads or writes the `busy` flag.

Busy flag. `busy` (0/1, owner thread only) is set by every entry point that mutates observer state or calls the api (create
excepted, as no observer exists; `start`, `stop`, `destroy`, `set_preedit_window`, `pump`, `refresh`) and only tested by
the read-only ones (`refresh_due`, `next_timeout_ms`, `status`). An entry point that finds it set returns
`WindowObserverStatusReentrant` (0 / `WindowObserverWaitForever` for the two scalar queries) and touches nothing, so a
message dispatched inside a Win32 call (enumeration, a query) that calls back into the observer cannot corrupt the dirty
table, the popped key or the enumeration buffer, which only the outermost `pump` or `refresh` uses. A `destroy` that finds
the flag set returns Reentrant and frees nothing.

Logical clock. `next_stamp` starts at `config.first_stamp` (nonzero; the caller guarantees it exceeds the graph's
`stamp_high_water`). `stamp_next` returns the current value and increments; the increment that wraps to 0 marks the
clock exhausted: later requests fail, pump and refresh return StampExhausted, and a hint under exhaustion latches
`overflow` and counts `hint_overflows`. Every accepted hint, every fresh requery stamp and every refresh requery
stamp comes from this one counter, so stamps are strictly increasing in observer call order. `captured_ms` is
`api.tick_ms` sampled right after the stamp and before `query_facts` (the 32-bit wire value; the graph unwraps it).

### 3.5 Algorithms (pseudocode; identifiers are the final Zig names)

```
validate_facts(f) -> bool                       // true = valid; false -> InvalidFacts, rejected_invalid_facts +1
  1  f.window != 0 and f.root != 0 and f.stamp != 0
  2  (f.owner != 0) == (f.owner_root != 0)       // owned needs a root, ownerless forbids one
  3  (f.flags & ~0x1FFF) == 0                    // InteropWindowFactMask (13 bits, bit 12 = OwnerHidden)
  4  AgentPreedit set -> AgentOwned set;  LayeredAlphaKnown set -> Layered set;  OwnerHidden set -> f.owner != 0
  5  ImageUnknown set  <=>  f.image_length == 0
  6  96 <= f.dpi <= 960
  7  f.width, f.height, f.min_width, f.min_height each <= 16384     // never clamped
  8  1 <= f.class_length <= 256;  f.image_length <= 260;  f.title_length <= 1024
  9  f.reserved0[0..3] all zero
 10  utf8_valid_no_nul(class_name[0..class_length]) and (image_name[0..image_length]) and (title[0..title_length])
     // rules 1..9 are O(1); rule 10 runs last (at most 1540 bytes). Layered flag vs ex_style bit 0x80000 and
     // Iconic with Zoomed are not cross-checked: classification reads only the flags named below.

utf8_valid_no_nul(s) -> bool                    // strict RFC 3629 over a bounded slice
  i = 0
  while i < len(s):
    b = s[i]
    if b == 0: return false
    if b < 0x80: i += 1; continue
    if 0xC2 <= b <= 0xDF: need 1 continuation
    elif b == 0xE0: second in A0..BF, then 1 continuation               // rejects overlong
    elif 0xE1 <= b <= 0xEC or b == 0xEE or b == 0xEF: need 2 continuations
    elif b == 0xED: second in 80..9F, then 1 continuation              // rejects surrogates
    elif b == 0xF0: second in 90..BF, then 2 continuations             // rejects overlong
    elif 0xF1 <= b <= 0xF3: need 3 continuations
    elif b == 0xF4: second in 80..8F, then 2 continuations             // rejects > U+10FFFF
    else: return false                                                 // 80..C1, F5..FF
    a continuation byte is 80..BF; a sequence cut by the end of s is invalid; i += sequence length
  return true

app_key_valid(k) -> bool                        // the frozen protocol-v2 application key rule
  3 <= len(k) <= 200 and k[0] in {'a','p'} and k[1] == '-'
  and every k[i] (i >= 2) in [A-Z a-z 0-9 _ -]        // ASCII only, so UTF-8 and NUL need no separate test

class_hash_of(bytes) -> u64                     // FNV-1a 64 over the exact bytes (case-sensitive, length implied)
  h = 0xcbf29ce484222325
  for b in bytes: h = (h ^ b) *% 0x00000100000001b3
  return h

ascii_eql_ignore_case(a, b) -> bool             // fold 'A'..'Z' only; bytes >= 0x80 compare exactly
  len(a) == len(b) and for all i: fold(a[i]) == fold(b[i])   // never a prefix or substring match

identity_equal(rec, f) -> bool                  // the incarnation tuple, compared literally
  rec.process_id == f.process_id and rec.thread_id == f.thread_id
  and rec.process_start_time == f.process_start_time
  and rec.class_hash == class_hash_of(f.class_name[0..f.class_length])
  // known start time vs 0 (access denied) counts as a different incarnation: conservative churn, never a stale surface.
  // A 64-bit hash collision between two distinct class names of the same process and thread is accepted (2^-64).

unwrap_tick(base: u64, t32: u32) -> u64         // pure; base is graph.tick_base (>= 2^32, so the result is never negative)
  d: i32 = bitcast(t32 -% truncate_u32(base))    // wrapping 32-bit subtraction, read as signed
  return base + sign_extend_i64(d)               // exact for |t - base| < 2^31 ms (24.8 days)
  // The only 32-bit tick arithmetic in the engine is this subtraction; every comparison afterwards is a plain
  // 64-bit "<" on unwrapped values. There is no 32-bit tick comparison helper. Example with base = 2^32 + 0xFFFF0000:
  // unwrap(0x00000010) = base + 0x20010 = 2^33 + 0x10 (the 32-bit counter wrapped, the 64-bit value did not).

tick_commit(t64: u64)                           // called only by a call that is about to return Ok
  graph.tick_base = max(graph.tick_base, t64)    // never moves backwards: a delayed older tick unwraps below the base
                                                 // and leaves it unchanged; a non-Ok call never changes tick_base
  // Destroy hint test on unwrapped values:
  //   destroys(L) = hint_stamp > L.admit_stamp and not (e64 < L.admit_captured_ms)   with e64 = unwrap_tick(tick_base, event_time_ms)
  // Deferred destroy test: drop D iff hint_stamp > D.last_stamp and not (e64 < D.last_captured_ms)

classify_facts(f, uac_mode) -> verdict_t        // pure; first matching step wins; f already validated; the export
                                                // interop_window_graph_classify = validate_facts(f) then this (uac_mode > 2 and
                                                // NULL pointers are InvalidArgument, a validation failure is InvalidFacts,
                                                // the verdict struct is written only on Ok; no graph, thread or allocation).
  verdict_t = { Eligible(role, parent) | NotExported(reason) | Suspended }   // PascalCase tag enum
  owned = f.owner != 0 and not OwnerHidden       // OwnerHidden: the owner is never exportable or gone (4.4); treated as not owned
  0  if not OnInputDesktop: return Suspended
     if not Visible:        return NotExported(NotVisible)       // Visible already excludes DWM-cloaked windows
  cls = f.class_name[0..class_length];  img = f.image_name[0..image_length]
  class_is(x) = not ClassTruncated and ascii_eql_ignore_case(cls, x)    // a truncated class matches no class rule
  1  if class_is any of {Shell_TrayWnd, Shell_SecondaryTrayWnd, Progman, WorkerW, NotifyIconOverflowWindow,
                          TaskListThumbnailWnd, "MSCTFIME UI", IME}:  return NotExported(ExcludedClass)
     if class_is("Windows.UI.Core.CoreWindow") and not ImageUnknown and any(ascii_eql_ignore_case(img, n)
            for n in {ShellExperienceHost.exe, StartMenuExperienceHost.exe, SearchHost.exe}):
                                                                       return NotExported(ExcludedCoreWindow)
     if AgentOwned:
         if not AgentPreedit: return NotExported(ExcludedAgent)
         role = ImePreedit; parent = owned ? f.owner_root : 0; goto 8   // steps 2..7 skipped; parent lets the host map a popup
  2  if not ImageUnknown and ascii_eql_ignore_case(img, "consent.exe"):
         if class_is("$$$Secure UAP Dummy Window Class For Interim Dialog"): return NotExported(ExcludedUacDummy)
         if class_is("Credential Dialog Xaml Host"):
             if uac_mode == Native: return NotExported(UacNativeHidden)
             role = UacConsent; parent = 0; goto 8               // modes Window and Auto
         // any other consent.exe window continues with step 3
  3  if f.root != f.window: return NotExported(Child)
  oroot = owned ? f.owner_root : 0               // an owned window whose owner is hidden is Toplevel with parent 0 (step 7)
  4  menu_class = class_is("#32768") or class_is("ComboLBox")                       // unconditional
     menu_style = (style & Popup) != 0 and owned and (style & 0x00C00000) != 0x00C00000   // title bar absent
                  and (ex_style & NoActivate) == 0                                  // NOACTIVATE binds ONLY here
     if menu_class or menu_style: role = Menu; parent = oroot; goto 8
  5  if class_is("tooltips_class32") or (ex_style & NoActivate) != 0
        or ((ex_style & ToolWindow) != 0 and owned):  role = Tooltip; parent = oroot; goto 8
  6  if owned and (ex_style & AppWindow) == 0: role = Dialog; parent = oroot; goto 8
  7  role = Toplevel; parent = 0                                // includes owned windows with WS_EX_APPWINDOW
  8  if f.width == 0 or f.height == 0: return NotExported(ZeroArea)
     if Layered and LayeredAlphaKnown and f.alpha == 0: return NotExported(Transparent)
     return Eligible(role, parent)                              // Enabled is ignored; color-key-only layered is opaque
  // NOACTIVATE reading: a "#32768"/"ComboLBox" window with WS_EX_NOACTIVATE is a Menu (step 4 class clause); an owned
  // no-caption WS_POPUP window with WS_EX_NOACTIVATE skips step 4 and becomes a Tooltip (step 5); an unowned
  // no-caption popup is never a Menu and is a Toplevel unless step 5 matches.
  // Cycle test, after classification and only for Eligible verdicts: owned (OwnerHidden clear) and f.owner_root == f.window -> Cycle;
  // the full order in observe is identity check first, then this test (4.5).

derive_state(f) -> u32                          // guest-to-host bits only; Activated is never produced
  s = 0
  Iconic -> Minimized;  Zoomed -> Maximized;  Fullscreen flag -> Fullscreen
  (style & ThickFrame) -> Resizable;  (style & MaximizeBox) -> Maximizable;  (style & MinimizeBox) -> Minimizable

bump(c: *u64) { c.* +|= 1 }      bump_by(c: *u64, n: u64) { c.* +|= n }
  // every stats_t and diag_* counter uses saturating add and never wraps; gauges are recomputed on read;
  // per-call observation fields are u32 and bounded by 64 + 64 (admitted, retired, evicted) or 192 (events), so
  // they cannot overflow; a counter may change on any error status, graph state may not.
```

## 4. Step-by-Step Execution Sequence

Notation. `ev<n>` is the n-th event appended to the ring by the call being described, in append order. `g<n>` is a generation number. `W<hex>` is an HWND. `[stamp s, tick t]` is the observer logical-clock value and the 32-bit tick sample attached to a snapshot. "Counters" lists only the counters that change. Stamp bookkeeping (`last_stamp`, `last_captured_ms`, `stamp_high_water`, refresh seen marks) is not counted as graph state when a call is said to have "no effect". All Zig names are the private names of the naming sheet; all C names are those of the committed headers.

Invariants used throughout.
- I1. For one HWND at most one of {Active entry, deferred record} exists. Any number of Retiring entries of older generations may coexist with it. Entries are keyed by the pair (window, generation); `find_active(window)` finds the live one, `find_exact(window, generation)` finds any state.
- I2. A child's generation is always greater than its parent's (a child is admitted after its parent). Sorting by descending generation therefore sorts children before parents.
- I3. Exported slots in use = Active + Retiring. `free_slots = MaxEntries - (active_count + retiring_count)`. Entries retired by a call do not free slots during that call.
- I4. The apply phase of every mutating call is infallible: it writes only into the preallocated arrays, calls nothing external and cannot observe other graph calls (the `busy` guard).

### 4.1 Create (graph, then observer)

Graph: `interop_window_graph_create(config, out_graph)`.
1. `out_graph == NULL` returns InvalidArgument (nothing to null). Otherwise `*out_graph = NULL` immediately.
2. `config == NULL`, `session_epoch == 0`, `uac_mode > InteropWindowUacNative`, `reserved0 != 0` or `app_key_lookup == NULL` returns InvalidArgument. `now_ms == NULL` selects the default monotonic clock (a read of `std.time.Instant`, converted to milliseconds; a failed read repeats the previous value, which counts as zero elapsed time).
3. `owner_thread = std.Thread.getCurrentId()`.
4. `allocator.create(graph_t)` (the module-level `abi_allocator`, `std.heap.c_allocator` by default) is the only allocation. Failure returns NoMemory with `*out_graph == NULL`; nothing else was allocated, so nothing leaks.
5. The struct is zero-initialized, then: `config` copied by value, `owner_thread` set, `busy=false`, `generations_issued=0`, `stamp_high_water=0`, `next_seq=1` (seq 0 means "not deferred"), every `entry_t.state=free`, every `deferred_t.used=false`, `ring_head=ring_len=0`, `refresh_open=false`, `have_diag=false`, `tick_base=2^32` (so that no unwrapped tick is negative), all counters 0.
6. `*out_graph` receives the handle; returns Ok. State afterwards: 0 entries, 0 deferred, 0 events, no refresh. Counters: none.

Observer: `window_observer_create(config, out_observer)`.
1. `out_observer == NULL` returns InvalidArgument; otherwise `*out_observer = NULL`.
2. Validate in this order: `config != NULL`; `first_stamp != 0` (and below `UINT64_MAX`, the exhaustion sentinel); `own_process_id != 0`; `uac_mode <= 2` (InvalidArgument otherwise); `api` NULL selects `window_observer_native_api()` under `_WIN32` and is InvalidArgument elsewhere; a non-NULL `api` with any of its eight entries NULL is InvalidArgument. Native checks (only when `api` is NULL under `_WIN32`, before anything is allocated): `GetDpiForWindow` must resolve at run time (Windows 10 1607 or later) and the calling thread's DPI awareness context (`GetThreadDpiAwarenessContext`) must equal `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2` (`AreDpiAwarenessContextsEqual`); otherwise the call returns `WindowObserverStatusUnsupported` with `*out_observer == NULL`. This is the single place the condition is reported and the single behaviour everywhere: a created native observer always runs, and a caller-supplied api table is never checked this way. The observer cannot compare `first_stamp` with the graph's `stamp_high_water` (it holds no graph); the coordinator passes `stamp_high_water + 1` or higher and an equal-or-lower value only yields StaleFacts statuses later (counted, never harmful).
3. `owner_thread = api->current_thread_id(api_context)`.
4. One `calloc(1, sizeof(window_observer_t))` (about 41 KB: 512 dirty entries x 48 bytes, 2048 enumeration slots x 8 bytes, state). Failure returns NoMemory with `*out_observer == NULL`.
5. Copy `api` and the config fields; `next_stamp = first_stamp`; `state = Created`; `hook_object = hook_system = hook_cloak = 0`; `busy = popped_in_flight = 0`; `uac_mode` copied; `desktop_active = 0`; every counter 0. No hook exists, the active slot is untouched, no graph is referenced. Returns Ok.

### 4.2 Start and hook installation, with partial-failure rollback

`window_observer_start(obs)`:
1. `obs == NULL`: InvalidArgument. Caller thread differs from `owner_thread`: WrongThread. `state` is Started or Failed: BadState. Created and Stopped continue.
2. Claim the process-wide slot: `atomic_compare_exchange_strong(&g_active_observer, expected=NULL, desired=obs)` with acq_rel on success. Failure returns AlreadyActive; `obs` is unchanged (still Created or Stopped).
3. Clear the dirty table (`count=0`), `overflow=0`, `refresh_requested=0`, `have_refresh_attempt=0`, `latch_ms=0`. The state is still Created or Stopped, so any hint dispatched now is ignored by `note_hint` (state check); the latch in step 7 covers every event that could be missed.
4. `api->install_hook(ctx, WindowObserverEventObjectCreate, WindowObserverEventObjectParentChange, &hook_object)` (0x8000..0x800F). Nonzero: release the slot (CAS `obs` -> NULL), wait for the in-flight counter to reach zero, `state = Failed`, return HookFailed. Nothing was installed, so nothing is removed.
5. `api->install_hook(ctx, WindowObserverEventSystemMinimizeStart, WindowObserverEventSystemMinimizeEnd, &hook_system)` (0x0016..0x0017). Nonzero: `api->remove_hook(ctx, hook_object)` once, `hook_object = 0`, release the slot, wait for the in-flight counter, `state = Failed`, return HookFailed.
6. `api->install_hook(ctx, WindowObserverEventObjectCloaked, WindowObserverEventObjectUncloaked, &hook_cloak)` (0x8017..0x8018). Nonzero: remove `hook_system` then `hook_object` (one `remove_hook` call each, in that order), zero both slots, release the slot, wait for the in-flight counter, `state = Failed`, return HookFailed. If any rollback removal also fails the token is forgotten, never retried, and that hook stays registered until process exit; its callbacks find the slot NULL and are ignored. HookFailed is the only status reported.
7. All three installed: `state = Started`, `desktop_active = 1` (assumed Default until the first check), `refresh_requested = 1`, `latch_ms = api->monotonic_ms(ctx)`, `have_refresh_attempt = 0` (the first refresh is due immediately because no attempt exists). Return Ok.
Counters: none. A Failed observer accepts only `destroy`. The busy flag is held for the whole call.

### 4.3 Steady state: hint to pump

Hint path, `window_observer_note_hint(event, window, object_id, child_id, event_thread_id, event_time_ms)`, run by the hook procedure on the owner thread:
1. `atomic_fetch_add(&g_hints_in_flight, 1)` (seq_cst), then `obs = atomic_load(&g_active_observer, acquire)`; NULL: decrement and return, no counter exists to bump. Every later return path decrements the in-flight counter (release); `stop`, a failed start and `destroy` clear the slot and then wait for the counter to reach zero, so the observer cannot be freed under a running hint. The busy flag is neither read nor written (a hint may arrive while the owner thread is inside another observer call).
2. `api->current_thread_id(ctx) != obs->owner_thread`: `atomic_fetch_add(&obs->hints_wrong_thread, 1, relaxed)` and return. Nothing else is read or written.
3. `obs->state != Started`: return silently (hint after stop).
4. Filter: `object_id != 0`, `child_id != 0`, `window == 0`, or `event` outside [0x8000, 0x800F], [0x0016, 0x0017] and [0x8017, 0x8018]: `hints_filtered++`, return. The three ranges are the three hooks of 4.2; 0x800D and 0x800E lie inside the first range and are accepted as Other.
5. `stamp = stamp_next()`. If it is exhausted (returns 0) the hint cannot be ordered: latch `overflow = 1`, `hint_overflows++`, return (recovery is invalidate_all plus refresh, which itself then reports StampExhausted).
6. `hints_received++`; `dirty_add`: find the entry with the same `window`. Found: OR `kinds`, `last_stamp = stamp`, and for event 0x8001 set `destroy_stamp = stamp`, `destroy_event_ms = event_time_ms`; `hints_coalesced++`. Not found and `dirty_count + popped_in_flight >= 512`: `overflow = 1`, `hint_overflows++` (one slot is reserved for the key that pump holds, so a put-back never needs to overflow). Not found and room: append at index `count`, `first_stamp = last_stamp = stamp`, destroy fields as above, `retries = 0`.

Pump, `window_observer_pump(obs, graph, epoch, budget, &stats)`:
1. Argument and state checks in order: `obs`/`graph` NULL or `budget == 0`: InvalidArgument; thread: WrongThread; `busy` already set: Reentrant (nothing touched, `stats` not filled); `state != Started`: BadState. Then `busy = 1` for the whole call (cleared on every return path).
2. `query_input_desktop`: not Default sets `desktop_active=0`, `pumps_desktop_inactive++`, `refresh_requested=1`, stamps `last_refresh_attempt_ms` and `latch_ms` from one clock read, returns DesktopInactive with the graph untouched (4.10). Default sets `desktop_active=1`. The desktop is checked exactly once per call; the result applies to every window of the call.
3. `overflow` set: this call is a refresh attempt, so stamp `last_refresh_attempt_ms` now (the desktop was just checked, so it is not checked again); run the recovery of 4.11 and return its status.
4. `limit = min(budget, count)` where `count` is sampled now; at most `limit` pops happen, so keys re-added during this call (retries, put-backs, mid-query hints) are never reached in this call.
5. Per key, `dirty_pop_front` copies the entry into a local, removes it (the shift is at most 511 x 48 bytes) and sets `popped_in_flight = 1` BEFORE any api or graph call; every exit from `process_key` sets it back to 0 (so the slot reserved for the put-back is released exactly when the key is either put back, dropped or fully processed). `process_key` then runs:
   - a. `keys_processed++`.
   - b. `destroy_stamp != 0`: `note_destroyed(graph, epoch, window, destroy_stamp, destroy_event_ms, &obs_out)`, `destroys_noted++`. Ok continues to c whatever the disposition (Retired or Unchanged). QueueFull: put the local entry back at the TAIL unchanged and return GraphQueueFull. Reentrant: put back at the head, `last_graph_status = Reentrant`, return Reentrant (not GraphError). Any other status: `graph_errors++`, `last_graph_status` set, put back at the head, return GraphError.
   - c. `fresh = stamp_next()`; 0 puts the entry back and returns StampExhausted. `tick = api->tick_ms(ctx)`.
   - d. `query_one`: zero a local `interop_window_facts_t`, call `api->query_facts(ctx, window, &facts)`. Hints that fire during this call find no entry for `window` and create a new one at the tail. When the result is Exists and `facts.owner_root` is neither 0 nor `window`, the core runs the owner sub-query (4.4 "Owner-hidden computation"): a second `api->query_facts(ctx, owner_root, &owner_facts)` with the same overlay, `interop_window_graph_classify(&owner_facts, uac_mode, &verdict)` and the rule Exists -> OwnerHidden iff verdict NotExported; Gone -> OwnerHidden; Failed (or a classify status other than Ok) -> the window's query is Failed. No message is ever sent to an own-process window.
   - e. Exists with `facts.window == window`: `queries_exists++`; overwrite `stamp=fresh`, `captured_ms=tick`; set `OnInputDesktop` (always: the desktop was checked in step 2); set `AgentOwned` iff `facts.process_id == own_process_id`; set `AgentPreedit` iff AgentOwned and `window == preedit_window`; force `title_length = 0` on AgentOwned; set or clear `OwnerHidden` from step d; leave `min_width = min_height = 0`; call `interop_window_graph_observe`. Status Ok with disposition Suspended (defensive: the core always sets OnInputDesktop, so a graph Suspended cannot occur): put the key back, `desktop_active=0`, latch refresh, `pumps_desktop_inactive++`, return DesktopInactive. Ok otherwise: if `observation.reason == UacNativeHidden`, `uac_native_hidden++`; next key. QueueFull: put back at the tail, return GraphQueueFull. Reentrant: put back at the head, return Reentrant. Statuses of the drop class (InvalidFacts, StaleFacts, Cycle, AtCapacity, GenerationExhausted, LookupFailed, AppKeyInvalid, Stale): `graph_errors++`, `last_graph_status` set, the key is dropped, next key. Any other status: `graph_errors++`, `last_graph_status` set, put back at the head, return GraphError.
   - f. Exists with a different `facts.window`: counted and handled as Failed.
   - g. Gone: `queries_gone++`, `note_destroyed(graph, epoch, window, fresh, tick, ...)`, `destroys_noted++`; status handling as in b (an Active entry retires with reason Destroyed because `fresh` exceeds every stamp already admitted and the unwrapped `tick` is not older than the admission tick; a Retiring or unknown window gives Unchanged).
   - h. Failed: `queries_failed++` (also for f and for a failed owner sub-query). `retries < WindowObserverQueryRetries (1)`: `retries++`, `dirty_push_back_retry` (tail), remember "put back" so the call ends with More. Otherwise `query_retries_exhausted++` and the key is dropped (the next refresh re-offers the window). A failed query never calls the graph and never retires anything.
   - Put-back (`dirty_push_back_retry`, also used after QueueFull): look up the key again (a hint may have re-created it): found, OR the kinds, keep the larger destroy stamp and its event time, keep `retries` of the saved entry; not found, append at the tail. The reserved slot guarantees room; the defensive `overflow` latch is never reached with a correct counter.
6. After the loop: `count == 0` and nothing was put back: Ok. Otherwise More. `stats` (if non-NULL) is filled on every return except InvalidArgument, WrongThread and Reentrant.
Graph QueueFull is not a failure of the key: the coordinator drains events and calls pump again (8.3); a repeated call before a drain delivered at least one event would only hit the same full ring.

### 4.4 Full refresh

`window_observer_refresh(obs, graph, epoch, &stats)`, called when `window_observer_refresh_due` returns 1:
1. Argument and thread checks as pump step 1 (without `budget`; Reentrant if the busy flag is set; BadState if not Started); `busy = 1`.
2. `last_refresh_attempt_ms = api->monotonic_ms(ctx)`, `have_refresh_attempt = 1`. This stamp is taken BEFORE the desktop check and before anything can fail, so every attempt, including one refused because the desktop is inactive or one that fails later, counts as an attempt; a failing refresh therefore retries at the 250 ms cadence and a frozen clock with a persistently failing refresh runs at most once per latch event (worked numbers after step 8).
3. `query_input_desktop`: not Default sets `desktop_active=0`, `pumps_desktop_inactive++`, `refresh_requested=1`, `latch_ms` = the clock read of step 2 (so both values are equal), returns DesktopInactive with the graph untouched. Default sets `desktop_active=1`. The desktop is not checked again before the commit; there is no per-window desktop detection, and the core sets `OnInputDesktop` on every snapshot of the call.
4. `enum_top_level_windows(ctx, enum_buffer, 2048, &count)`: Failed or Truncated returns EnumFailed before any graph call (`refresh_requested` unchanged); `count > 2048` is treated as Failed. A complete list of any length 0..2048 is accepted; an empty list therefore retires everything at commit, which is correct for an empty desktop. The enumeration buffer is touched only by this outermost call (the busy flag keeps a nested call out).
5. `refresh_begin`. Ok continues. RefreshActive (left open by an earlier commit that returned QueueFull): `refresh_abort`, `refreshes_aborted++`, `refresh_begin` once more. Any other status: GraphError. `refreshes_started++`.
6. Pass 1, for each enumerated handle in order (no deduplication; a duplicate is re-queried and re-offered, and is Unchanged the second time; a handle of 0 is skipped): `fresh = stamp_next()`, `tick = tick_ms`, `query_one` (pump step 5d, including the owner sub-query).
   - Exists: set stamp, captured_ms and flags as in pump step 5e, then `refresh_offer`. Ok (any disposition): `uac_native_hidden` as in the pump; a Suspended disposition (defensive) aborts and returns DesktopInactive. QueueFull: `refresh_abort`, `refreshes_aborted++`, return GraphQueueFull. Reentrant: abort, return Reentrant. Drop-class statuses (InvalidFacts, StaleFacts, Cycle, AtCapacity, GenerationExhausted, LookupFailed, AppKeyInvalid, Stale): `graph_errors++`, `last_graph_status` set, the refresh continues; `refresh_offer` has already marked the window seen for this outcome, so a rejected offer never retires the window at commit. Call-level statuses (StaleEpoch, WrongThread, InvalidArgument, NoRefresh, ...): abort, GraphError.
   - Gone: the window is skipped and stays unseen (pass 2 requeries it if it is an Active window).
   - Failed: `queries_failed++`, `refresh_keep(window)` (Ok or None continue; other statuses as above), continue with the next handle. A failed query never aborts the refresh and never retires a window.
7. Pass 2, reconcile the would-be victims: `refresh_unseen(graph, epoch, windows[64], 64, &n)` lists the Active windows still unseen (a window that was hinted and admitted but is absent from the enumeration, a window whose enumeration entry was Gone, ...). For each listed window draw `fresh`, `tick` and requery it individually: Exists -> `refresh_offer` (status handling as in pass 1); Gone -> `note_destroyed(window, fresh, tick)` (`destroys_noted++`; if the destroy rule rejects the hint the window stays unseen and the commit retires it with RefreshUnseen); Failed -> `queries_failed++`, `refresh_keep(window)`. This keeps a window that is alive but not in the enumeration (a transient enumeration gap, or a hinted window the OS had not enumerated yet) and makes absence alone never the sole evidence of destruction.
8. `refresh_commit`. Ok: `refreshes_committed++`, `refresh_requested = 0`, return Ok, even when queries failed (every failed window was kept). QueueFull: the refresh stays open in the graph, return GraphQueueFull (`refresh_requested` stays). Other: best-effort `refresh_abort`, GraphError.
Total queries per call are bounded by 2048 plus 64, each with at most one owner sub-query. Graph side seen marking: `refresh_begin` clears `seen` on all 64 entries and 64 deferred records and sets `refresh_open`. After the common guards pass, `refresh_offer` marks the offered window (when nonzero) seen for EVERY outcome, including every error status and QueueFull (offered means alive for this refresh), before the plan; a new generation is born seen, and a deferral creates a seen record. A plain `observe` during an open refresh marks seen on Ok only. `refresh_keep` marks the Active entry and the deferred record of the window (Ok, None if neither exists, NoRefresh if no refresh is open). An entry created by a flush copies the `seen` bit of the deferred record it came from; a child re-deferred by a Reclassified cascade copies the bit of its entry; flush leftovers cut off by capacity stay deferred and keep their bit, so they survive the commit. `refresh_commit` retires every Active entry with `seen == false` (reason RefreshUnseen) and then drops every deferred record with `seen == false`.

Cadence worked numbers (`refresh_due`, `next_timeout_ms`). `start` at t=0 sets the latch (`latch_ms = 0`, no attempt yet): due at once, `next_timeout_ms` = 0. The first `refresh` at t=10 stamps `last_refresh_attempt_ms = 10` and commits (latch cleared). t=200: not due, timeout 60 ms. t=260: due (250 ms since 10). Persistent failure with a frozen clock (the monotonic clock returns 500 forever, the enumeration fails): the attempt stamps 500 and returns EnumFailed; `latch_ms` was last set at 0, `last_refresh_attempt_ms (500) >= latch_ms (0)`, elapsed is 0 < 250, so `refresh_due` is false and the loop sleeps; it neither spins nor starves. A desktop switch at t=900 (DesktopInactive) stamps both values 900: not due until t=1150, then due by cadence, and the committed refresh at the return of the desktop clears the latch. An overflow at t=1300 sets `latch_ms = 1300` (> the last attempt at 1150): due immediately, once; the attempt that follows stamps >= 1300 and consumes the event even if it fails.

Owner-hidden computation (portable core, `query_one`; the api table never sets `OwnerHidden`). Purpose: an owned window whose owner can never be exported (for example the owner is a tray or `Progman` window, a child, a transparent or zero-area window, an excluded agent window, or it is gone) would otherwise be announced with `parent_window` pointing at an owner that never appears, would sit in the deferred set forever and would never reach the host. The observer therefore decides the flag; the graph treats the window as not owned (3.5 `classify_facts`: a Menu stays a Menu with parent 0, popups and owned tool windows are judged with `owned = false`, an owned non-APPWINDOW window becomes Toplevel with parent 0, an ImePreedit gets parent 0). Steps, executed after `query_facts(window)` returned Exists with `owner_root != 0` and `owner_root != window`:
   1. Zero a second facts struct and call `api->query_facts(ctx, owner_root, &owner_facts)`.
   2. Exists: overlay the same core flags on `owner_facts` (same `stamp` and `captured_ms`, AgentOwned, AgentPreedit, OnInputDesktop, title forced to 0 for AgentOwned), leave its OwnerHidden clear, call `interop_window_graph_classify(&owner_facts, obs->uac_mode, &verdict)`, and set `OwnerHidden` on the window iff `verdict.kind == NotExported`. An owner whose verdict is Eligible or Suspended leaves the flag clear. A classify status other than Ok (the owner snapshot is unusable) makes the window's query Failed.
   3. Gone: set `OwnerHidden` (the owner no longer exists, so the window can never gain a parent).
   4. Failed: the window's query is Failed (kept, retried; nothing retires).
   There is no recursion: the owner is classified with OwnerHidden clear and the owner's own owner is never queried. The query counters count one result per window query with the sub-query folded in. A change of the flag between two offers changes role or parent and therefore goes through the Reclassified path (the old generation retires, a new one is announced); a flag change that leaves role and parent unchanged is Unchanged. This promotion is a deliberate refinement of the frozen section 4.4 table (which is silent about never-exportable owners); it is recorded in 9.1 and needs Codex sign-off. The exact one-line disable is: the observer never sets `OwnerHidden` (remove step 2 and 3 from `query_one`); nothing in the graph changes.

### 4.5 Observe: plan, pre-check, lookup, apply

`interop_window_graph_observe(graph, epoch, facts, &observation)`; `refresh_offer` is the same with a leading NoRefresh check and the seen rule of 4.4 (the window is marked seen for every outcome once the guards and the NoRefresh check have passed).
1. `enter`: graph NULL -> InvalidArgument; thread -> WrongThread; `busy` -> Reentrant (none of these touch anything); `epoch != config.session_epoch` -> StaleEpoch (`rejected_stale_epoch++`). `busy = true` (cleared by `defer` on every exit path). `facts`/`observation` NULL -> InvalidArgument (`refresh_offer` marks nothing then); for `refresh_offer`, `refresh_open == false` -> NoRefresh (checked after the argument checks and before validation, nothing marked). Copy `*facts` into a stack-local `f` (about 1.7 KiB of stack, no allocation); every later step, including the `app_key_lookup` callback, reads only `f`, so a callback or another thread cannot change the facts between validation and use. For `refresh_offer`, mark `f.window` seen now (Active entry and deferred record that exist; a record or entry the call creates is born seen), before the plan, so the mark survives every later failure.
2. `validate_facts(f)` fails -> InvalidFacts (`rejected_invalid_facts++`). Validation covers the full list in the header: nonzero window/root/stamp, owner/owner_root pairing, flag mask (13 bits) and implications (including OwnerHidden requires owner != 0), dpi 96..960, extents <= 16384, length bounds, UTF-8 without NUL, `reserved0` zero. The unwrapped tick of `f.captured_ms` is computed here against `tick_base` (not yet committed).
3. Locate `L = find_active(window)` and `D = find_deferred(window)`. Staleness: the reference is `L.last_stamp` if L exists, else `D.last_stamp` if D exists, else none. `f.stamp < reference` -> StaleFacts (`rejected_stale_facts++`). Equal stamps are accepted (a repeated offer is idempotent).
4. `classify_facts(f, uac_mode)` (pure; the same function the `interop_window_graph_classify` export runs). Suspended: return Ok, disposition Suspended, `suspended++`, no state change (a refresh seen mark is still set). NotExported(reason): with L, plan a retirement of L's cascade with that reason (disposition Retired); without L, plan to drop D if present (disposition Excluded, `deferred_dropped = 1` if D existed). A self-owned window (`owner_root == window`, OwnerHidden clear) whose verdict is NotExported is therefore reported by its classification, not as Cycle (classification precedes the cycle check, as in the header).
5. Identity check, BEFORE the cycle check, for an Eligible verdict. L exists and `(process_id, thread_id, process_start_time, class_hash)` differs -> the plan retires the L cascade with reason IncarnationChanged (`forced = true`, replacement "no L", a NEW key lookup). D exists and its identity tuple differs from `f` -> D is dropped in apply (`deferred_dropped++`) and the window is treated as having NO D: a fresh lookup and, if it defers again, a new `seq` (a stale record never contributes its key or age to a different incarnation).
6. Cycle test (Eligible verdict, parent != 0). `parent == window` -> cycle. Otherwise `chain_has_cycle` starts at `parent`, repeatedly follows the deferred record's `parent_window` for at most 65 steps, stops at an Active entry or at no record, and reports a cycle if it reaches `window` or exceeds 65 steps. Cascade cycle: when L exists with equal identity and the new `(role, parent)` differs (Reclassified), the new parent is also a cycle if it is in L's own descendant set (the entries that the retirement cascade would collect), because the cascade is retired and the window cannot become the child of its own descendant. On a cycle:
   - forced retirement (L exists and identity changed, step 5; or a cascade cycle): the retirement still happens, nothing is deferred or re-deferred, the call returns Ok with disposition Retired and reason IncarnationChanged (identity) or Reclassified (cascade cycle), `rejected_cycle++` (counted although the call returns Ok); D is dropped if present;
   - otherwise: `Cycle`, `rejected_cycle++`, D dropped if present (`deferred_dropped++`, the one documented mutation of a non-Ok status); L is left unchanged.
   The same persistent-shortfall rule covers AtCapacity, GenerationExhausted, LookupFailed and AppKeyInvalid below: a retirement that is required (identity change, reclassification) is never abandoned.
7. Decide the plan (all read-only; the retirement parts are already fixed by steps 4 to 6):
   - L exists, identity differs -> retire the L cascade (reason IncarnationChanged), continue as "no L" with a NEW key lookup (step 5).
   - L exists, identity equal, `role != L.role` or `parent_window != L.parent_window` -> Reclassified: retire the cascade (L reason Reclassified, descendants ParentRetired), admit a new generation that reuses `L.app_key` (no lookup); descendants are re-announced after it (4.7) because this is the only cause with an unchanged incarnation (they are alive and still owned by that HWND). Ownership by itself is not stored; it matters only through `role` and `parent_window`; a change of OwnerHidden matters the same way.
   - L exists, identity, role and parent equal -> compare x, y, width, height, `state`, dpi, title bytes, min sizes. No difference: Unchanged (also when only OwnerHidden changed). Else Update with `changed_mask` bits (Rect, State, Dpi, Title, MinSize).
   - No L, `parent == 0` or `find_active(parent)` exists -> admit (D present and identity equal: reuse `D.app_key`, no lookup; D removed in apply).
   - No L, parent absent, deferred or Retiring -> defer. D present (identity equal): overwrite D in place (keeps `seq` and `app_key`, no lookup, no event). D absent: lookup needed; `defer_insert` may evict the lowest `seq` (4.13).
8. Capacity and generations. Let `a_primary` be 1 when a new generation is wanted. If `free_slots == 0` (I3) or `generations_issued == UINT64_MAX`, the admission cannot proceed. With no retirement required the call returns AtCapacity (`rejected_at_capacity++`, diagnostic `at_capacity++`) or GenerationExhausted (`rejected_generation_exhausted++`, diagnostic `generation_exhausted++`) with no effect. With a retirement required, the plan degrades to retire-only (replacement shortfall) and the same counters increment.
9. Flush plan (only when a generation is admitted): FIFO worklist seeded with the deferred records whose `parent_window` equals the new entry's window, ascending `seq`; pop, admit if `free_slots` and generations remain (appending that record's own deferred children to the worklist tail), otherwise leave it deferred and `rejected_at_capacity += 1` and diagnostic `at_capacity += 1` (or the generation counters). For a reclassification the cascade's descendants join the worklist in ascending original generation, and any that do not fit are inserted into the deferred set (each insert may evict the lowest `seq`).
10. Event-count pre-check, evaluated after steps 8 and 9 and before the lookup:
   ```
   retire_count   = Active entries moved to Retiring by the plan (the cascade, root included)
   announce_count = a_primary + flushed children (incl. re-announced descendants)
   update_count   = 1 if an Update is needed and the newest queued event of (window, generation)
                    is not an unconsumed Announce/Update, else 0
   needed         = retire_count + announce_count + update_count
   room           = EventCapacity - ring_len
   needed > room  -> QueueFull, rejected_queue_full++, diagnostic queue_full++, no effect
   ```
   A degraded retire-only plan needs `retire_count` only; the full plan is checked first, so a transient shortage is never converted into a retirement without its replacement. Since `retire_count <= active` and `announce_count <= free_slots` and `active + free_slots <= 64`, a call needs at most 65 events; the engine relies only on the compile-time bound `MaxCallEvents (192) <= EventCapacity (256)`.
11. Lookup, only if the plan needs one and the plan is not retire-only: `lookup(app_key_context, &f, key_buf, 200, &length)` (the stack-local copy) with `length` preset to 0 and `busy` still true. Nonzero return: if a retirement is required, degrade to retire-only and `lookup_failures++`, else return LookupFailed (`lookup_failures++`). `length` outside 3..200 or `app_key_valid` false (prefix a- or p-, characters `[A-Za-z0-9_-]`): AppKeyInvalid (`app_key_invalid++`) with the same degrade-or-fail rule. A valid key is copied into the plan; the callback's buffer is never retained.
12. Apply (infallible): (a) `retire_cascade` appends the RetireIntent events and flips states; (b) the primary entry is written (`generation = ++generations_issued`, `admit_stamp = last_stamp = facts.stamp`, `admit_captured_ms = last_captured_ms = t64` (the unwrapped 64-bit tick of `facts.captured_ms`), inline title and key copies, `parent_generation` = the Active parent's generation); (c) its Announce is appended; (d) the flush list is walked, each entry written and announced; (e) leftovers are deferred; (f) a D that was consumed is cleared; (g) `stamp_high_water = max(stamp_high_water, facts.stamp)` and `tick_commit(t64)` (tick_base = max(tick_base, t64); both only because the call returns Ok); (h) the observation is filled and copied to the caller (staged in a local, copied only on Ok).
13. Counters on success: `admitted += admissions`, `retire_intents += retire_count`, `updates_queued` or `events_coalesced`, `events_queued += appended`, `deferred_added`, `deferred_evicted`, `deferred_dropped`, `reclassified`. Observation: `events_queued` is the number appended (0 for a coalesced Update), `entries_admitted = a_primary + flushed`, `entries_retired = retire_count`.

### 4.6 A child whose parent is not announced yet

Setup: nothing live. Parent P = W100 (top-level, class "Main"), child C1 = W200 and C2 = W300 (owned dialogs, `owner_root = W100`). Offer order C1, C2, P (any enumeration order is legal).
1. Offer C1 [stamp 10]: Eligible Dialog, parent W100 absent -> Deferred. Lookup runs once (first deferral) and stores key "a-main". `defer_insert`: record `seq=1`, `generation=0`, `parent_window=W100`. Observation: Deferred, reason ParentPending, `events_queued=0`. Counters: `deferred_added=1`.
2. Offer C2 [stamp 11]: same, `seq=2`, second lookup. Counters: `deferred_added=2`.
3. Offer P [stamp 12]: no L, parent 0 -> admit. Capacity, generations and room pre-checked (needed = 3). One lookup for P. Apply: P gets g1; flush worklist [C1 (seq 1), C2 (seq 2)] -> C1 g2, C2 g3, both with `parent_window=W100`, `parent_generation=1`, keys reused from the deferred records (no further lookups). Events: ev1 Announce(W100, g1), ev2 Announce(W200, g2), ev3 Announce(W300, g3). Observation: Admitted, generation 1, `events_queued=3`, `entries_admitted=3`. Counters: `admitted=3`, `events_queued=3`. Deferred set empty.
4. A grandchild G = W400 (owner W200) offered before C1's flush would sit deferred with parent W200 (not Active, deferred): when C1 is flushed its own deferred children join the worklist tail, so the order is P, C1, C2, G (breadth-first, FIFO by age within a level). A parent Announce always precedes every child Announce because a record is only written after its parent entry exists.
5. If P is later offered while it is Retiring or absent, the children stay deferred; no event is lost because deferral queues none.

### 4.7 Parent reclassification with two children

Before: Q = W50 g1 (top-level); P = W100 g3 (top-level); C1 = W200 g4 and C2 = W300 g5 (dialogs of P). `generations_issued = 5`, 4 Active, ring has been drained. P gains an owner: facts have `owner = W50`, `owner_root = W50`, no APPWINDOW, so classification gives role Dialog, parent W50 (role and parent_window both changed), stamp 50.
1. Plan: identity equal, role/parent differ -> Reclassified. Cascade of P: descendants C2 (g5), C1 (g4), then P (g3) -> `retire_count = 3`. Capacity: before = 4 slots used, 60 free; the 3 retirements free nothing; one primary admission plus 2 flushed descendants need 3 slots -> fits. Generations: P' = g6; descendants re-announced in ascending original generation: C1 -> g7, C2 -> g8. `announce_count = 3`; `needed = 6`; room check passes. No lookup (stored key of P is reused; descendants reuse theirs).
2. Apply, in this order: ev1 RetireIntent(W300, g5, reason ParentRetired), ev2 RetireIntent(W200, g4, reason ParentRetired), ev3 RetireIntent(W100, g3, reason Reclassified), ev4 Announce(W100, g6, role Dialog, parent W50/g1), ev5 Announce(W200, g7, parent W100/g6), ev6 Announce(W300, g8, parent W100/g6). Slots now: Q + 3 Retiring + 3 Active = 7.
3. Observation: Reclassified, `generation=6`, `retired_generation=3`, `reason=Reclassified`, `events_queued=6`, `entries_admitted=3`, `entries_retired=3`. Counters: `reclassified=1`, `retire_intents=3`, `admitted=3`, `events_queued=6`.
4. The old generations g3, g4, g5 stay Retiring until the coordinator calls `complete_retire` for each; `check_action` on them returns Stale at once. If P' had to be deferred (new parent not yet announced), the three would still retire, P' would become a deferred record with the reused key, and the descendants would be re-deferred with new `seq` values in ascending original generation (C1 before C2) and would return when P' is admitted.
5. Cascade cycle. If P's new owner is its own descendant (say P is offered with `owner_root = W200`, a current child of P), the new parent lies in P's cascade set, so the replacement is treated as Cycle: P retires with reason Reclassified, C1 and C2 retire with ParentRetired, nothing is deferred or re-deferred (the pair would otherwise wait for each other forever), and the call returns Ok with disposition Retired; `rejected_cycle++`. The children return by their next offers: C1 (owner P) is offered, finds P absent (Retiring) and defers; P is offered with its own parent chain and, if it is now admissible, flushes C1.
6. Shortfall variants. If exactly one slot is free when the call starts (`free_slots = 1`), the plan admits P' (g6) but cannot admit the descendants: they are inserted into the deferred set in ascending original generation (C1 then C2, new `seq` values), `rejected_at_capacity += 2`, `needed = 3 + 1 = 4`, and the call still returns Reclassified with `entries_admitted = 1`, `events_queued = 4`; each deferred descendant is admitted by its own next offer (the next refresh, at most 250 ms later, because its parent W100 is then Active) once a slot is free, so the coordinator must free slots with `complete_retire`. If `free_slots = 0`, the plan degrades to retire-only: disposition Retired, `retired_generation = 3`, `entries_retired = 3`, `needed = 3`, the descendants are re-deferred, `rejected_at_capacity += 1` for the primary, and the next offer of W100 retries the admission.

### 4.8 Destroy followed by HWND reuse: the `note_destroyed` rule

`note_destroyed(epoch, window, hint_stamp, event_time_ms)`:
1. `enter`; `window == 0` or `hint_stamp == 0` or NULL output -> InvalidArgument. Compute `e64 = unwrap_tick(tick_base, event_time_ms)` (3.5); `tick_base` is not changed yet.
2. Active entry L for `window`: retire iff `hint_stamp > L.admit_stamp` AND NOT `e64 < L.admit_captured_ms`, a plain `<` on two unwrapped 64-bit values (`admit_captured_ms` is the unwrapped tick stored when L was admitted). There is no 32-bit comparison anywhere. Both clauses are needed: the stamp clause rejects hints that reached the observer before the facts that admitted L were drawn; the tick clause rejects hints that reached the observer afterwards but describe an event older than those facts.
3. Deferred record D for `window`: dropped iff `hint_stamp > D.last_stamp` AND NOT `e64 < D.last_captured_ms` (`deferred_dropped = 1`; `last_captured_ms` is the unwrapped tick of the newest facts accepted for D). A Retiring entry is never touched.
4. Retirement uses `retire_cascade` (reason Destroyed for L, ParentRetired for descendants, descendants never re-deferred), pre-checked as in 4.5 (`needed = retire_count`); QueueFull changes nothing (including `tick_base`). Disposition Retired when anything retired, else Unchanged. On Ok `stamp_high_water` advances and `tick_base = max(tick_base, e64)`.

Honest scope of the rule (deviation from the handoff wording). The handoff asks that a destroy hint about an earlier incarnation never retires a newer one. The rule narrows this and does not literally satisfy it. A newer incarnation is retired and then readmitted when (a) the ticks are equal (they are ambiguous, so the rule treats them as destroyed) or (b) the destroy fell between the observer's `tick_ms` sample and its native read of the facts that admitted L (the event time is then not older than `captured_ms`, although the facts already describe the new window). The cost is bounded churn: at most one lost generation per such coincidence. The observer requeries the window right after a Destroy hint (4.3 step b then c), finds it alive and admits it under a new generation, so a live window is never permanently lost; the only visible effect is one surface retire plus one announce. A refresh cannot create unbounded churn because a hint is consumed by the call that applies it. This deviation needs Codex acceptance, and the claim that `GetTickCount` and WinEvent `dwmsEventTime` share a base must be checked on a native runner (8.6); this is recorded in 9.1 and 9.2.

Worked numbers (all unwrapped values are 64-bit; `tick_base` starts at 2^32 = 4294967296). L = W500 admitted as g9 with `admit_stamp = 100`, observer tick 5000: `unwrap_tick(2^32, 5000)` = 2^32 + 5000, so `admit_captured_ms = 4294972296` and `tick_base = 4294972296` after the Ok call.
- Stale destroy ignored. The old window with the same HWND was destroyed at tick 4990; the new window was created at 4995 and admitted from a query sampled at tick 5000. The OS delivered the old destroy callback late, so `note_hint` stamped it 101 (event time 4990). `note_destroyed(W500, 101, 4990)`: 101 > 100 holds; `e64 = 4294972286 < 4294972296`, so it is older than L's facts: ignored, Unchanged, no event, `tick_base` unchanged (max keeps the larger value), `destroys_noted` still counts the call. A destroy stamped 99 (arrived before the query) fails the stamp clause: ignored.
- Real destroy applied. The window is destroyed at tick 5200, callback stamped 110: 110 > 100 and `e64 = 4294972496 >= 4294972296`: L retires (RetireIntent g9, reason Destroyed); `tick_base = 4294972496`. The pump then requeries with a fresh stamp 111: Exists means the HWND was reused again, `observe` finds no Active L and admits g10 (fresh lookup); Gone means `note_destroyed(W500, 111, tick)` finds no Active entry and returns Unchanged.
- Equal ticks, conservative churn. Destroy event time 5000, stamp 101: `e64` equals `admit_captured_ms`, `<` is false, so L retires even if the destroy actually belonged to the previous incarnation. The same pump call then requeries; the new window is admitted as g10. Cost: one surface churn. The alternative, ignoring equal ticks, could leave a stale surface, which the design forbids.
- Wrap of the 32-bit counter. W501 admitted at wire tick `0xFFFFFFF0` while `tick_base = 2^32 + 0xFFFFFFE0`: d = +16, `admit_captured_ms = 2^33 - 16` and `tick_base` follows. Destroy event time `0x00000010`: d = (0x10 - 0xFFFFFFF0) mod 2^32 = 32, `e64 = 2^33 + 16 >= admit`: destroyed. Event time `0xFFFFFFE0`: d = -16, `e64 = 2^33 - 32 < admit`: ignored.
- Long-lived windows (the case that a 32-bit compare gets wrong). W600 is admitted at tick 1000 (`admit_captured_ms = 2^32 + 1000`) and then only re-offered, unchanged, by the 250 ms refresh; every Ok refresh call carries a tick and raises `tick_base`, so the base follows the clock. After 30 days (2592000000 ms, more than 2^31 = 2147483648) a destroy with event time 2592001000 unwraps to `2^32 + 2592001000 >= admit`: retired. A 32-bit `time_before` would read the 30-day difference as negative and ignore a real destroy forever. After 60 days (5184000000 ms, the 32-bit counter has wrapped once: wire value 889032704) the unwrapped value is `2^32 + 5184001000`: retired. The test of 7.1 drives exactly this with simulated ticks and an Ok call at least every 2^31 ms (the observer's cadence is every 250 ms). After a silence longer than 2^31 ms a tick can alias by a multiple of 2^32 ms; the next refresh corrects it (RefreshUnseen retires a vanished window, a requery re-admits a live one).
- Identity change without a hint: the next offer for W500 carries a different (process, thread, start time, class hash): `observe` retires g9 with IncarnationChanged and admits a new generation; one RetireIntent and one Announce, events 2.
- Deferred record. D for W700 with `last_stamp = 200`, `last_captured_ms = 2^32 + 7000`: a hint stamped 201 with event time 6990 is ignored (older than D's facts); event time 7010 drops D.
- Residual risk (documented, accepted): HWND recycled with an identical tuple, the destroy hint lost, no overflow: indistinguishable, the old generation continues. The 250 ms refresh cannot detect it either.

### 4.9 Retirement, quarantine, `list_retiring`, `complete_retire`

`retire_cascade(root_index, reason)`: (1) collect descendants by repeated scans: Active entries whose `(parent_window, parent_generation)` equals a collected pair; (2) sort the collected set plus the root by descending generation (insertion sort, at most 64 elements); (3) for each: `state = retiring`, `retire_reason` (root: the given reason; descendants: ParentRetired), append a RetireIntent event holding the last known payload (kind 3, `changed_mask = 0`, `reason`, stored title and app key); `retire_intents++`; (4) the entries keep their record, their slot and their generation; (5) descendants are re-deferred with their stored record (new `seq` in ascending original generation) ONLY when the cause is Reclassified of the SAME incarnation, because then the children are still alive and still owned by that HWND. For every other cause (parent destroyed, excluded or hidden, incarnation changed, refresh unseen, invalidate_all, a cascade cycle) the descendants retire with reason ParentRetired and are NOT re-deferred; each returns through its own next hint or refresh offer (at most 250 ms later). Under invalidate_all the deferred set is cleared as well. Active-only `find_active` no longer sees them; `check_action` answers Stale.

Coordinator sequence: drain events; for each RetireIntent (window, generation) stop capture and drain pins in the slot runtime, then call `complete_retire(epoch, window, generation, {1, 1})`. At any time `list_retiring(entries, 64, &count)` returns every quarantined pair in ascending generation (`count` is the total even if `capacity` was smaller).

`complete_retire` order of checks: `enter`; window 0, generation 0 or NULL proof -> InvalidArgument; `find_exact(window, generation)`.
- Not found (never existed, already completed, or from another incarnation; generations never repeat, so a missing pair can never alias a newer entry) -> Stale, `rejected_stale++`.
- Found Active -> NotRetiring.
- Found Retiring, `capture_stopped != 1` or `pins_drained != 1` (a value of 2 also fails) -> ProofIncomplete, `proof_incomplete++`, the entry stays quarantined and keeps its slot.
- Found Retiring with both flags exactly 1 -> the entry is zeroed (record scrubbed), `state = free`, `retire_completed++`, return Ok.
Worked example: W500 holds Retiring g9 and Active g10 (4.8). `complete_retire(W500, 9, {1,0})` -> ProofIncomplete (g9 still Retiring). `complete_retire(W500, 10, {1,1})` -> NotRetiring (g10 is Active). `complete_retire(W500, 9, {1,1})` -> Ok; slot freed. A duplicate `complete_retire(W500, 9, {1,1})` -> Stale. If W500 is later destroyed and re-admitted as g15, a late duplicate `complete_retire(W500, 9, {1,1})` is still Stale and g15 is untouched, because the match requires the exact generation. A completion with a mismatched epoch is StaleEpoch (the old graph's entries cannot be touched through a new graph).

### 4.10 Desktop switch away and back

1. Normal operation: `desktop_active = 1`, hints flow.
2. The secure desktop receives input (UAC consent in native mode, Ctrl+Alt+Del, lock). Hooks installed from the Default thread deliver nothing from the other desktop; hints for Default windows may still arrive from before the switch.
3. The next `pump` or `refresh`: `query_input_desktop` returns Other or Denied. The observer sets `desktop_active = 0`, `refresh_requested = 1`, `pumps_desktop_inactive++`, stamps `last_refresh_attempt_ms`/`latch_ms` from one clock read and returns DesktopInactive. The graph is not called, nothing is retired, no event is produced; the dirty table keeps its entries and may latch `overflow` if more than 512 distinct windows are hinted meanwhile. `next_timeout_ms` does not return 0 for a hint backlog while `desktop_active == 0`, so the loop does not spin on it; the cadence (worked numbers in 4.4) re-polls the desktop. The refresh latch is a timestamp pair, not a flag the cadence has to guard: the DesktopInactive attempt stamps `last_refresh_attempt_ms` and `latch_ms` from the same clock read, so `refresh_due` (true iff Started and (no attempt yet, or at least 250 ms since the last attempt, or latched with `last_refresh_attempt_ms < latch_ms`)) is false right after the refused attempt and becomes true again 250 ms later. A latch therefore forces at most one refresh attempt per latch event, even with a frozen clock.
4. The desktop is checked exactly once per `pump` or `refresh` call, before any query, and the result applies to every window of the call: the core sets `OnInputDesktop` on every snapshot after that single check. The observer does NOT detect a desktop switch per window and the native query does not report `OnInputDesktop`; a switch that happens during a call is seen by the next call's check. If an in-flight `query_facts` returns Failed because the desktop changed under it, the key is retried once and then dropped. A graph Suspended disposition therefore cannot occur; as a purely defensive guard, pump puts the key back and returns DesktopInactive, and refresh aborts and returns DesktopInactive. There is no per-window "Suspended" state in the graph (the graph returns the disposition Suspended only for a snapshot whose OnInputDesktop is clear, which the observer never produces).
5. Desktop returns: the next poll sees Default, `desktop_active = 1`; the pump processes the retained hints (an overflow latch triggers 4.11); the latched refresh runs and its commit retires windows that vanished (RefreshUnseen) and admits new ones. During the absence the graph still reports the old windows as live; no DesktopSwitch message exists in this slice, so the coordinator owns that protocol.

### 4.11 Hint-table overflow, `invalidate_all`, full refresh

1. 512 distinct HWNDs are pending. A hint for a 513th distinct HWND finds no entry and no room: `overflow = 1`, `hint_overflows++`, the hint is lost. Hints for already-present keys still coalesce.
2. Next `pump` (desktop Default): `overflow` set. `interop_window_graph_invalidate_all(epoch, &retired)`: `enter`; an open refresh is aborted (`refresh_aborts++`, `refresh_open = false`); every Active entry is retired in descending generation (all reason Invalidated; children first); the deferred set is cleared (`deferred_dropped += n`); pre-check `needed = active_count`, QueueFull otherwise with no effect. Retiring entries already in quarantine are unchanged.
3. QueueFull: the observer keeps `overflow` and the table, returns GraphQueueFull; after the coordinator drains, the next pump retries the whole recovery.
4. Ok: `invalidations++`, the dirty table is cleared (`count = 0`), `overflow = 0`, `refresh_requested = 1`, then the full refresh of 4.4 (steps 4 to 8: enumeration, begin, pass 1, pass 2, commit; the attempt stamp and the desktop check were done by pump) runs inside the same call and its status is returned, with the same GraphQueueFull and GraphError mapping. A failure there leaves `refresh_requested` set, so the cadence retries.
Worked numbers: 30 Active entries g1..g30, empty ring. Step 2 appends 30 RetireIntents in order g30, g29, ..., g1 (30 events). The refresh then offers the 30 live windows: the 30 Retiring entries still occupy 30 slots, so free slots = 64 - 30 = 34 and all 30 windows are re-admitted as g31..g60 (30 Announces, parents before children by offer order and deferral). Ring total 60. If the coordinator has not completed the 30 retirements and more than 34 windows exist, the remainder gets AtCapacity (graph_errors counted, refresh continues) until `complete_retire` frees slots; the coordinator must therefore drain and complete retirements promptly after an overflow. If the ring already held 220 events, step 2 needs 30 <= 36 and succeeds; the refresh then hits QueueFull on the seventh Announce (room 6 left), aborts, and returns GraphQueueFull with `refresh_requested = 1`; after draining, the next refresh re-offers everything.

### 4.12 Stop, restart, destroy; graph destroy

Observer `stop(obs)`: NULL -> InvalidArgument; wrong thread -> WrongThread; busy flag set -> Reentrant (nothing touched); Failed -> BadState; Created or Stopped -> Ok with no effect. Started: (1) release the slot first (CAS `obs` -> NULL, release), so later or queued callbacks are ignored; (2) `state = Stopped`; (3) wait until `g_hints_in_flight` is zero, then `remove_hook(hook_cloak)`, `remove_hook(hook_system)`, `remove_hook(hook_object)` in that order, each attempted even if another fails, tokens zeroed regardless (no retry); (4) clear the dirty table, `overflow`, `refresh_requested`; (5) any failed removal: `state = Failed`, return UnhookFailed; else Ok. Counters and `next_stamp` survive. Restart from Stopped is `start` (4.2); stamps continue upward, so a restart never reissues a stamp. Stop never calls the graph: nothing is retired, freed or proven.
Observer `destroy(slot)`: `slot == NULL` -> InvalidArgument; `*slot == NULL` -> Ok no-op; wrong thread -> WrongThread (nothing freed, slot unchanged); busy flag set -> Reentrant (nothing freed, slot unchanged); Started -> stop first; then defensive slot release plus a wait for `g_hints_in_flight == 0`, `free`, `*slot = NULL`; returns UnhookFailed if stop reported it (freed anyway), else Ok.
Graph `destroy(slot)`: `slot == NULL` -> InvalidArgument; `*slot == NULL` -> Ok no-op; wrong thread -> WrongThread; `busy` -> Reentrant (neither frees). Otherwise `allocator.destroy(graph)` and `*slot = NULL`. Freed: the single `graph_t` allocation with all 64 entries, 64 deferred records, 256 ring events, titles and keys. NOT proven or released: capture, frame pins, heap runs, mappings. Quarantined pairs and undelivered RetireIntent events vanish with the graph, which is why the coordinator calls `list_retiring` and drains BEFORE destroy and hands the pairs to the slot runtime; destroying the graph is never evidence that host buffers are gone. The observer holds no graph pointer, so destroy order between the two is free.

### 4.13 Capacity

- 65th window. 64 Active entries (g1..g64), no Retiring, offer W4500 (top-level, eligible): `free_slots = 0` -> AtCapacity; `rejected_at_capacity++`, diagnostic `at_capacity++`; no lookup, no event, no eviction. In `pump` the key is dropped (`graph_errors++`, `last_graph_status = AtCapacity`); in `refresh` the window stays unseen and the refresh continues. When W7 (g7) is destroyed the entry becomes Retiring and still occupies its slot, so W4500 stays blocked until `complete_retire(W7, 7, {1,1})`; the next refresh (at most 250 ms later) admits W4500 as g65.
- Deferred overflow. 64 deferred children waiting for an unannounced parent, `seq = 1..64`. A 65th child offered: first deferral lookup runs, `defer_insert` finds the set full, evicts the record with the lowest `seq` (1), `deferred_evicted++`, diagnostic `deferred_evicted++`, observation `deferred_dropped = 1`, and the new record takes `seq = 65`. Deferral consumes no exported slot and no event. The evicted window is re-offered by the next refresh and, if still orphaned, evicts the then-oldest record; per refresh the churn is bounded by the number of orphan windows.
- Capacity diagnostic, `take_capacity_diagnostic` at injected clock values: counters (`at_capacity`, `deferred_evicted`, `queue_full`, `generation_exhausted`) all zero -> None without reading the clock. t = 1000 ms, `at_capacity = 3`: first take, `have_diag` false -> Ok with `elapsed_ms = 0`, counters reset, `last_diag_ms = 1000`. t = 30000, `at_capacity = 2`: elapsed 29000 < 60000 -> None, counters kept. t = 61000: elapsed 60000 -> Ok, `elapsed_ms = 60000`, `at_capacity = 2`. A clock reading below `last_diag_ms` counts as 0 elapsed -> None and `last_diag_ms` is not moved. Gauges (`active_count`, `retiring_count`, `deferred_count`, `events_pending`) are filled on every Ok.
- Ring bound. Because every successful call fits in 192 events (<= 256), draining an empty ring always makes any single operation fit; a persistently full ring can only mean the coordinator is not draining.

## 5. Concurrency, Threading & Synchronization

### 5.1 Owner-thread rule

- The graph is bound at create to `std.Thread.getCurrentId()`; the observer is bound to `api->current_thread_id(ctx)` at create. Both bindings are immutable afterwards, which is what makes the thread check itself race-free from any thread.
- Every graph function except `create` compares the caller with `owner_thread` right after the NULL check and before any other read or write. A mismatch returns WrongThread and touches nothing, not even counters. `destroy` from another thread does not free. Every observer function except `window_observer_note_hint` does the same, then tests the busy flag (5.4); the scalar queries return `0` (`refresh_due`) or `WindowObserverWaitForever` (`next_timeout_ms`) for both a wrong thread and a set busy flag.
- The observer thread and the graph thread must be the same thread. If they differ, the first graph call from the observer returns WrongThread, the observer maps it to GraphError with `last_graph_status = WrongThread` and puts the key back; misconfiguration is detected on the first call and never corrupts state.
- The owner thread must run a message loop; out-of-context WinEvent callbacks are delivered on the thread that called the install function, only when that thread dispatches messages.

### 5.2 Why there are no locks

All mutable state of the graph and all of the observer except one counter is touched by exactly one thread. The hook procedure runs on that same thread, so it never executes in parallel with a pump or a graph call; it can only interleave at message-dispatch points. A lock would add nothing against a same-thread re-entry and would deadlock against it. The design therefore uses confinement plus explicit re-entrancy guards instead of mutual exclusion, and the type system of the ABI (opaque handles, value copies) keeps pointers into graph storage from escaping to other threads.

### 5.3 The one cross-thread entry: `window_observer_note_hint`

A call from a thread other than the owner (a test double, or a misconfigured hook) executes exactly these steps: `atomic_fetch_add(g_hints_in_flight, 1)` (seq_cst); acquire-load `g_active_observer`; read the immutable `api` and `owner_thread`; call `api->current_thread_id` (required to be trivial, non-blocking and thread-safe: `GetCurrentThreadId` natively); `atomic_fetch_add(hints_wrong_thread, 1, relaxed)`; `atomic_fetch_sub(g_hints_in_flight, 1)` (release); return. Nothing else is read or written, and the busy flag is never touched. The status snapshot reads that one field with an atomic relaxed load, so the value can lag by in-flight increments. Lifetime proof: `stop`, a failed-start rollback and `destroy` first clear `g_active_observer` (CAS to NULL) and then wait until `g_hints_in_flight` reads zero. A hint that loaded the pointer before the clear incremented the counter before the load (seq_cst), so the waiter sees it and waits; a hint that increments after the clear loads NULL and leaves. No `note_hint` can therefore touch an observer after `stop` or `destroy` has returned, on any thread. The wait is bounded by the duration of one hint, which never blocks. (A hint running on the owner thread itself cannot overlap `stop` or `destroy` on that thread, so the wait only ever covers a foreign-thread hint.)

### 5.4 Reentrancy guards

- Graph: `busy` is set to true after the `enter` checks pass and cleared by `defer` on every exit path, including error returns. It covers the whole call, hence the lookup callback (inside `observe`, `refresh_offer`), the drain sink (inside `drain`), and the clock callback (inside `take_capacity_diagnostic`, which has no epoch). While `busy` is true any graph function, including read-only ones (`get_stats`, `find_live`, `list_retiring`, `stamp_high_water`), returns Reentrant without touching anything and `destroy` does not free. Rationale: the sink runs while the ring head has not yet advanced past the event it was handed; a nested `next_event` would deliver the same event twice. The lookup callback receives a pointer to the engine's stack-local copy of the facts (copied at entry of `observe` and `refresh_offer`, so the caller's struct cannot change under the callback) and a key buffer on the engine's stack; neither may be retained.
- Drain: the engine copies `ring[head]` into a local `event_t` (1328 bytes) and hands the sink a pointer to that copy; the head advances (`head = (head + 1) mod 256`, `ring_len--`, `events_drained++`) only after the sink returns 0.
- Observer: a busy flag (`busy`, owner thread only) guards every entry point except `note_hint`. `start`, `stop`, `destroy`, `set_preedit_window`, `pump` and `refresh` set it on entry (after the argument, thread and state checks that cannot reenter) and clear it on every return path; `refresh_due`, `next_timeout_ms` and `status` only test it. Any of them that finds it set returns `WindowObserverStatusReentrant` (0, `WindowObserverWaitForever`, and a status code respectively) and touches nothing; a `destroy` frees nothing. Re-entry is real, not theoretical: a Win32 call inside an observer function (EnumWindows, a query) can dispatch a message to the owner thread, and a handler of that message could call the observer again. The guard keeps such a nested call away from the dirty table, the popped key, `popped_in_flight` and the enumeration buffer, which only the outermost `pump` or `refresh` touches. `note_hint` neither reads nor writes the flag: hook procedures are exactly the code that runs inside those message dispatches. A graph `Reentrant` status inside `pump` or `refresh` (for example from a lookup callback that calls back into the graph) puts the popped key back at the head, stops processing and returns `Reentrant` (not `GraphError`); a refresh also tries to abort its open graph refresh (a failed abort is ignored and the next refresh recovers through RefreshActive). No message is ever sent to a window of the agent's own process (4.4), which removes the main source of self-inflicted re-entry.

### 5.5 WinEvent callback safety

What `note_hint` (hence the hook procedure) may touch: the process-wide in-flight counter, the atomic slot, the immutable `api` copy, `owner_thread`, `state` (read), `next_stamp`, the dirty table (including `popped_in_flight`, read-only), `overflow`, and the hint counters; never the busy flag. What it must not touch: the graph (any function), any api entry other than `current_thread_id`, any Win32 query, enumeration or desktop check, the allocator, locks, logging, other observer functions, pointers retained after return. It performs no loop longer than 512 comparisons, so it is bounded and non-blocking.

Re-entry while the owner thread is inside a Win32 query: out-of-context hooks can be dispatched when a query pumps messages (documented as possible, not as guaranteed). The dirty table tolerates this because (1) `process_key` removes the entry and copies it to a local before the first api call, so no table index, pointer or iterator is live across any api or graph call; (2) a callback during the query sees no entry for that HWND and appends a fresh entry at the tail with a later stamp and its own destroy info; (3) the put-back after the query uses `dirty_push_back_retry`, which looks the key up again and merges, and it always finds a slot because `popped_in_flight` reserves one (a hint that arrives during the query sees `dirty_count + popped_in_flight` and overflows one key earlier instead of letting the put-back overflow); (4) `note_hint` does its only external call (`current_thread_id`) before any table write and has no callout while the table is half-updated; (5) every table update is a complete single-step edit (append or merge in place), so a nested entry always sees a consistent `count`. A destroy that arrives mid-query becomes a new entry with `destroy_stamp` greater than the fresh stamp of the query in progress; after the query's snapshot is applied, the new entry's `note_destroyed` is judged by the rule of 4.8 and either retires the entry (event time not older than the facts) or is ignored. Re-entry of the callback into itself is impossible in the observer (it dispatches no message), and the table would still be consistent by (4) and (5).

### 5.6 Memory ordering

- `g_active_observer` (`static _Atomic(window_observer_t *)`): publish with `atomic_compare_exchange_strong` (success acq_rel, failure acquire) in `start` after the observer fields are initialized; release with `atomic_compare_exchange_strong` to NULL (release) in `stop`, failed start and `destroy`; read with `atomic_load` acquire in `note_hint`. The acquire/release pair makes the fields written before publication visible to a reader on another thread.
- `g_hints_in_flight` (`static _Atomic(uint32_t)`): `atomic_fetch_add` seq_cst in `note_hint` before it loads the slot, `atomic_fetch_sub` release when it is done; the waiter in `stop`/`destroy`/rollback clears the slot with a CAS (release) and then loads the counter with acquire until it reads zero. The seq_cst increment followed by the slot load orders against the clear-then-load of the waiter (the store-buffering pattern needs the total order), which is why the increment is seq_cst.
- `hints_wrong_thread`: relaxed `atomic_fetch_add`; it carries no ordering duty (a pure statistic).
- Graph: no atomics, no fences; plain fields confined to the owner thread. Zig `usize`-indexed arrays only; no shared reads.
- Cross-compiling to Windows uses the same C11 `<stdatomic.h>` operations; a build that lacks them fails to compile rather than degrading.

### 5.7 Thread-misuse results (summary)

| Call | Wrong thread | Inside graph callback |
|---|---|---|
| any graph function except create | WrongThread, nothing touched | Reentrant, nothing touched |
| graph destroy | WrongThread, not freed | Reentrant, not freed |
| observer function other than note_hint | WrongThread (0 / WaitForever for the scalar queries), destroy does not free | Reentrant while the busy flag is set (0 / WaitForever for the scalar queries), nothing touched, destroy does not free |
| note_hint | ignored; atomic `hints_wrong_thread++` | n/a (never reads the busy flag) |

## 6. Error Handling & Failure Modes

### 6.1 Graph status codes

"State" means entries, deferred records, ring, refresh flag; "counters" means graph statistics. Unless noted, non-Ok statuses leave outputs unchanged.

| Status | Returned by | Effect on state and outputs | Counters | Caller action |
|---|---|---|---|---|
| Ok (0) | all | Plan applied; outputs fully written; `stamp_high_water` advances where documented | per call | continue |
| None (1) | next_event, drain, take_capacity_diagnostic, find_live, refresh_keep | Nothing; outputs unchanged (`delivered = 0` for drain); for refresh_keep neither an Active entry nor a deferred record exists for the window | none | benign; stop polling |
| InvalidArgument (2) | all | NULL graph/pointer, zero window/generation/budget/stamp, bad config, `uac_mode` above 2 (create, classify), NULL `windows` with capacity > 0 (refresh_unseen); no change | none | fix the call |
| InvalidFacts (3) | observe, refresh_offer, classify | Facts violate validation (including OwnerHidden without an owner); no change (classify: verdict unchanged) | `rejected_invalid_facts` | observer defect or hostile input; drop key, refresh re-offers |
| WrongThread (4) | all but create | Nothing touched, destroy does not free | none (not even counters) | call from the owner thread |
| Reentrant (5) | all but create | Nothing touched | none | remove the graph call from the callback |
| NoMemory (6) | create | `*out_graph = NULL`, nothing allocated | none | retry later or abort startup |
| StaleEpoch (7) | all with an epoch | No effect | `rejected_stale_epoch` | the caller holds an old epoch; stop using that graph |
| Stale (8) | check_action, complete_retire | No entry has that exact (window, generation) (or it is Retiring for check_action); no change | `rejected_stale` | treat the action as void / completion as already done |
| StaleFacts (9) | observe, refresh_offer | Stamp below the window's newest accepted; no change | `rejected_stale_facts` | requery with a fresh stamp |
| QueueFull (10) | observe, refresh_offer, note_destroyed, invalidate_all, refresh_commit | Ring lacks room for the whole plan; no change (refresh_offer keeps its seen mark); refresh stays open for commit | `rejected_queue_full`, diagnostic `queue_full` | drain, retry the same call |
| AtCapacity (11) | observe, refresh_offer | 64 slots in use, nothing evicted; no change (with a required retirement: Ok/Retired instead) | `rejected_at_capacity`, diagnostic `at_capacity` | complete pending retirements; next refresh re-offers |
| Cycle (12) | observe, refresh_offer | No effect on L; a deferred record for the window is dropped. A cycle met while a retirement is required (identity change, or a reclassified window whose new parent is in its own cascade set) is NOT returned: the retirement happens and the call returns Ok/Retired | `rejected_cycle` (also counted in the forced case), `deferred_dropped` | none; ownership cycle is a guest anomaly |
| LookupFailed (13) | observe, refresh_offer | Lookup returned nonzero; nothing published, no key invented; (required retirement still happens: Ok/Retired) | `lookup_failures` | fix the lookup source; next offer retries |
| AppKeyInvalid (14) | observe, refresh_offer | Key not a valid v2 key (3..200 bytes, a-/p-, `[A-Za-z0-9_-]`); same as LookupFailed | `app_key_invalid` | fix the lookup |
| GenerationExhausted (15) | observe, refresh_offer | No generation left; existing entries keep working; (required retirement happens) | `rejected_generation_exhausted`, diagnostic `generation_exhausted` | roll to a new epoch and graph |
| RefreshActive (16) | refresh_begin | The open refresh continues; no change | none | commit or abort it, or offer into it |
| NoRefresh (17) | refresh_offer, refresh_keep, refresh_unseen, commit, abort | No refresh open; no change, nothing marked (offer checks this after the argument checks and before validating facts) | none | call refresh_begin first |
| ProofIncomplete (18) | complete_retire | Entry stays Retiring | `proof_incomplete` | obtain both proofs; never substitute connection loss |
| NotRetiring (19) | complete_retire | Exact pair is Active; no change | none | the pair is live; do not release |
| SinkFailed (20) | drain | Events accepted before the failure stay consumed; the refused event stays queued; `delivered` is written | `sink_failures` | fix the sink; drain again (the refused event is redelivered first) |

### 6.2 Observer status codes

| Status | Returned by | Effect | Caller action |
|---|---|---|---|
| Ok (0) | all | Completed; for pump the table is empty on return; for refresh the graph committed | continue |
| More (1) | pump | Budget exhausted or a key was put back for retry; hints remain | call pump again (next iteration) |
| DesktopInactive (2) | pump, refresh | Desktop not Default: nothing processed, graph untouched, hints kept, `refresh_requested = 1`, `desktop_active = 0`, `pumps_desktop_inactive++`, `last_refresh_attempt_ms`/`latch_ms` stamped from one clock read; also, defensively, when the graph reports a Suspended disposition (the core always sets OnInputDesktop after its single desktop check, so this does not occur; pump puts the key back, refresh aborts first) | wait for the cadence poll |
| GraphQueueFull (3) | pump, refresh | Graph reported QueueFull; the key is put back at the tail (pump) or the refresh aborted / left open (commit); latches kept |  drain graph events, call again |
| InvalidArgument (4) | all | NULL, zero budget/stamp/process id, malformed api, `uac_mode` above 2; nothing changed (`*out = NULL` for create) | fix the call |
| WrongThread (5) | all but note_hint | Nothing touched, destroy does not free | call from the owner thread |
| NoMemory (6) | create | `*out_observer = NULL`; nothing leaked | retry or abort |
| AlreadyActive (7) | start | Another observer owns the process-wide slot; this one unchanged | stop or destroy the other |
| BadState (8) | start, stop, pump, refresh | Call illegal in the current lifecycle state; nothing changed | respect the lifecycle |
| HookFailed (9) | start | Installed hooks removed, slot released, state Failed | destroy and recreate |
| UnhookFailed (10) | stop, destroy | Every removal attempted; stop: state Failed, restart refused; destroy: freed anyway | process-exit leak of one hook; callbacks are ignored (slot NULL) |
| GraphError (11) | pump, refresh | Unexpected graph status other than QueueFull and Reentrant (`last_graph_status` has it); the key re-inserted at the head, processing stopped; refresh aborted | inspect `last_graph_status` (StaleEpoch, WrongThread, NoRefresh, ...) and fix wiring |
| EnumFailed (12) | refresh | Enumeration failed or truncated; no graph call, nothing retired | none; retried on the cadence |
| StampExhausted (13) | pump, refresh | Logical clock exhausted; key re-inserted / refresh aborted | replace the observer starting above `stamp_high_water` |
| Reentrant (14) | every function with an observer handle except note_hint | Another entry point of this observer is active (busy flag, including entry through a message dispatched inside a Win32 call); nothing touched; `destroy` frees nothing; scalar queries return 0 / WaitForever. For pump and refresh also returned when the graph reports Reentrant: the key is put back at the head, processing stops, a refresh aborts its graph refresh (failure ignored), `last_graph_status = Reentrant`, not GraphError | remove the nested call; retry from the loop |
| Unsupported (15) | create | The native table cannot run: `GetDpiForWindow` is missing or the calling thread is not `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2`; `*out_observer = NULL`; nothing allocated | fix the process or thread DPI awareness (manifest or `SetThreadDpiAwarenessContext` before create) or run on Windows 10 1607 or later |

There is no observer status for a failed query: a failed query never aborts a refresh (the window is kept with `refresh_keep`, counted in `queries_failed`) and the refresh commits and returns Ok. The pump retries a failed key once and then drops it; the next refresh re-offers the window.

Seen rule per status (graph, open refresh). `refresh_offer` marks `facts->window` (when nonzero) seen for every outcome once the common guards (graph, thread, reentrancy, epoch) have passed, a NULL facts/observation pointer has not been found and a refresh is open. A plain `observe` marks seen on Ok only. `refresh_keep` marks on Ok.

| Status of the offer | refresh_offer marks seen? | plain observe marks seen? |
|---|:---:|:---:|
| NULL graph, WrongThread, Reentrant, StaleEpoch | no | no |
| InvalidArgument (NULL facts/observation) | no | no |
| NoRefresh (no refresh open) | no (nothing to mark) | n/a |
| Ok (any disposition; a new generation is born seen, a deferral creates a seen record) | yes | yes |
| InvalidFacts, StaleFacts, Cycle, AtCapacity, GenerationExhausted, LookupFailed, AppKeyInvalid | yes | no |
| QueueFull | yes | no |

Consequence: a window whose offer failed is simply not updated (and is offered again by the next refresh); it is never retired by the commit because of that failure. Retirement at commit needs the window to be absent from every offer and every keep of the refresh.

### 6.3 Failure modes

- Allocation failure at create. Graph: one allocation; failure returns NoMemory with `*out_graph = NULL`. Observer: one allocation, same. The failing-allocator test sweeps `abi_allocator` (Zig) and a wrapped `calloc` seam (C) over the first N allocations and requires zero bytes leaked under ASan/LSan.
- Capacity 64/64. AtCapacity, nothing evicted (4.13). Quarantined entries count, so slow `complete_retire` is the usual cause; the diagnostic surfaces it once a minute.
- Deferred 64. The oldest deferral is evicted, counted, and re-offered by the refresh (4.13). Deferral never consumes exported slots.
- Lookup failure or invalid key. No entry, no event, no generation, no fabricated key; LookupFailed or AppKeyInvalid. If a retirement was required by the same offer, the retirement still happens and the call returns Ok/Retired (a surface is never left live under a stale identity). No partial publish: the lookup runs after the pre-check and before the first write.
- Queue full. Nothing mutates; the observer leaves the key queued or the refresh open and returns GraphQueueFull; the coordinator drains. The per-call bound (<= 192 events) means an empty ring always fits any single operation; a call that can never fit is impossible by construction.
- Hook failure or partial init. Installed hook removed, slot released, state Failed, HookFailed (4.2). A second observer is unaffected.
- Invalid facts. InvalidFacts, no effect, counted. The observer never edits graph state; the next hint or refresh re-offers.
- Generation exhaustion. `generations_issued == UINT64_MAX`: admissions fail (GenerationExhausted), existing entries, updates, retirements and completions keep working; the session must roll to a new epoch. Test-only constructor preloads the counter.
- Thread misuse. WrongThread / Reentrant with no effect (5.7).
- Access denied. The native query reports ImageUnknown, `image_length = 0`, `process_start_time = 0`; classification then matches no consent.exe or CoreWindow rule, the window is still exported by the remaining rules, and nothing claims UIAccess or secure-desktop proof.
- Desktop switch or denied desktop. DesktopInactive, nothing processed, latch a resync (4.10).
- Native table cannot run (one behaviour everywhere). When `window_observer_create` selects the native table it resolves `GetDpiForWindow` with `GetProcAddress` (stored in a file-static pointer) and checks that the calling thread's DPI awareness context equals `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2`. Either check failing -> create returns `WindowObserverStatusUnsupported` with `*out_observer = NULL` before anything is allocated, never a fabricated 96 and never a silent fallback. After a successful create the native table is runnable and no later call returns Unsupported. A zero return from `GetDpiForWindow` for one window makes that query Failed (no retirement). A caller-supplied api table is never checked this way.
- Owner sub-query. Owner Gone -> OwnerHidden (the window is exported as a top-level window); owner Failed or an unusable owner snapshot (classify not Ok) -> the window's query is Failed (kept, retried once by pump, kept by `refresh_keep` in a refresh); owner Exists -> OwnerHidden iff its verdict is NotExported. The owner chain is one level deep by construction (no recursion), so there is no loop risk.
- Oversized windows. The native adapter clamps width and height to `InteropWindowGraphMaxExtent` (16384) so an oversized window is exported at the maximum; the graph still rejects larger values (InvalidFacts) when a caller or double supplies them.
- Image name unreadable. `OpenProcess` fails (UIPI, protected process): the basename comes from a `CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS)` walk by process id (snapshot at most once per pump or refresh call, retaken only on a process-id miss, handle closed on every path), `process_start_time` stays 0; ImageUnknown only when both fail. A known image with start time 0 versus the same process later with a known start time is an incarnation change (one surface of churn).
- Lone-surrogate titles. The observer converts titles with `WideCharToMultiByte(CP_UTF8, 0, ...)`, so unpaired surrogates become U+FFFD and the window is not rejected; the cut for 1024 bytes happens at a code-point boundary. The graph still rejects invalid UTF-8 (InvalidFacts) as the trust boundary. Class and image names use `WC_ERR_INVALID_CHARS`; a failure sets ClassTruncated or ImageUnknown rather than failing the window.
- Enumeration truncation or failure. EnumFailed before `refresh_begin`: no refresh is opened, nothing is retired.
- Query failure during refresh. A failed window is kept with `refresh_keep` and counted (`queries_failed`); the refresh continues, commits and returns Ok, so a permanently Failed window (for example a protected window whose facts can never be read) neither starves the other windows nor blocks the commit; it is simply never updated, and it is retired only by a real Gone result or a destroy hint. Failure in pump: one retry, then drop; never retire.
- Hinted but not enumerated. A window that is alive and was admitted through a hint but is missing from the enumeration is listed by `refresh_unseen`, requeried individually and kept (Exists -> `refresh_offer`; Failed -> `refresh_keep`); only Gone retires it (through `note_destroyed`). Absence from the enumeration alone is therefore never the only evidence of destruction.
- Re-entrancy. A message dispatched inside a Win32 call that re-enters the observer gets Reentrant (busy flag) and changes nothing; a hook callback only calls `note_hint`, which is safe by construction (5.5).
- Stamp exhaustion. A hint that cannot be stamped latches overflow; pump and refresh return StampExhausted; replace the observer starting above `stamp_high_water`.
- Sink failure. SinkFailed, the refused event stays at the ring head, `sink_failures++`; no event is lost or duplicated.
- Crash or abort of the owner thread. No cleanup runs; recovery is a new epoch, a new graph and a new observer; the old graph's retired pairs are never completed, so the host's buffers are not released by the guest on this path (see 6.4).

### 6.4 Transport loss is never proof

- Loss of the connection (socket, ring, vsock, IVSHMEM mapping) is not an input of this slice: no graph function takes a "disconnected" flag and none interprets connection state. `complete_retire` accepts only a `interop_window_retire_proof_t` whose `capture_stopped` and `pins_drained` are both exactly 1.
- Exact coordinator boundary. (1) Drain all events and keep every RetireIntent; (2) for each Retiring pair obtained from the events or from `list_retiring`, stop capture and drain pins in the slot runtime; (3) call `complete_retire` only when both facts are proven for that exact generation; (4) before `interop_window_graph_destroy`, call `list_retiring` and hand every pair to the slot runtime, because destroying the graph discards the pairs and proves nothing; (5) if the transport is lost, keep the graph or hand the pairs to the slot runtime; never call `complete_retire` with flags set on the strength of the loss; (6) a new connection belongs to a new session epoch with a new graph, and host buffers of the old epoch remain accounted for by the slot runtime until its own proof exists.
- Consequence: after a transport loss quarantined entries remain Retiring and keep their slots until proven; stale or duplicate completions cannot affect any newer generation (4.9).

## 7. Verification & Testing Criteria

Scope of this section: every test, command and pass criterion for the window graph (`src/interop/window_graph.zig`, `src/interop/window_graph_abi.zig`, `include/waddle/interop_window_graph.h`) and the Win32 observation adapter (`src/interop/window_observer_windows.{h,c}`). The committed headers are authoritative; where an earlier design note differs from a header, the header wins (the differences are listed in section 9). Nothing in this section is executed on Windows.

Notation used in the tables.

- `ZG` = `src/interop/window_graph.zig`, test blocks below the line `// Test-only fixtures.`
- `ZA` = `src/interop/window_graph_abi.zig`, test blocks below the same line.
- `CG` = `tests/interop/window_graph.c` (links the real `window_graph_abi.o`).
- `CO` = `tests/interop/window_observer_windows.c` (links the real observer object and the real `window_graph_abi.o`; every Win32 call goes through the api double in `tests/interop/window_observer_api_double.h`).
- Test names are snake_case. A Zig test is `test "<name>"`. A C test is `static void test_<name>(void)` registered in a table that `main` runs in order and that prints the name before each run.
- Fixtures (all below the delimiter, all snake_case, types end in `_t`): `make_facts(profile, window)` returns a fully valid facts value and `facts_builder_t` mutates one field at a time; `lookup_ok`, `lookup_fail`, `lookup_bad_key`, `lookup_reenter`; `sink_collect`, `sink_fail_after`, `sink_reenter`; `fingerprint(graph)` returns a byte copy of every field except the counters (entries, deferred records, ring contents, ring head and length, `generations_issued`, `stamp_high_water`, `next_seq`, `refresh_open` and all seen marks); `check_invariants(graph)`; `reference_model_t`; `xorshift_t`.
- Error rule used by every `err_*` and `cap_*` row: before a failing call the test fills each output with the sentinel byte `0xA5`, takes `fingerprint`, calls, then asserts (a) the documented status, (b) outputs still equal the sentinel, (c) the fingerprint is byte-identical, (d) only the counters named in the header changed. The two documented exceptions are Cycle (drops the window's own deferred record) and SinkFailed (events accepted before the refusal stay consumed).

### 7.1 Test matrix

#### 7.1.0 Handoff bullet to test area

| Required portable test (handoff, "Tests and evidence") | Areas below | Primary files |
|:--|:--|:--|
| All 64 admissions, 65th rejection and retry, 64 deferred entries, overflow and refresh, generation growth and exhaustion, same-HWND reuse | `cap`, `gen`, `reuse` | ZG, CG |
| Every classification row and precedence, process and class exclusions, visibility, alpha and zero geometry, parent and root edge cases, all UAC settings, OwnerHidden and the classify export | `cls`, `fct`, `own` | ZG, CG, CO |
| Parent before child, deferred cycles, missing or retiring parent, deep chains, ownership change, stale parent generation | `par` | ZG, CG |
| Update coalescing, immutable event values, bounded title and app key, invalid UTF-8, exact snapshot and output on errors | `evt`, `key`, `err` | ZG, ZA, CG |
| Delayed or reordered callback, reentrancy (graph and observer busy flag), wrong thread, epoch or generation, teardown with queued callbacks, native API partial-init failure, three hooks, tick unwrap | `hint`, `ree`, `thr`, `epo`, `tdn`, `hook`, `tck` | ZG, ZA, CG, CO |
| Retirement with outstanding proof, duplicate and stale completion, reconnect and new epoch, sink failure, queue and counter exhaustion, refresh keep/unseen and failed queries | `ret`, `rcn`, `snk`, `que`, `cnt`, `rfs` | ZG, ZA, CG, CO |
| Random and stress traces, every allocator failure with zero leaked bytes | `rnd`, `alc` | ZG, ZA, CG, CO |
| C ABI layout and link checks for Linux and x86_64 Windows | `abi` | ZA (comptime), CG (`_Static_assert`, link), Make `window-graph-windows` |

#### 7.1.1 Classification (`cls`): one row per step of IMPL_DESC 4.4

All rows call `classify` directly in ZG with a table of `{facts mutation, uac_mode, expected verdict}` and again end to end through `observe` so that the expected `disposition`, `role`, `reason` and `events_queued` are asserted. Reason values are the `interop_window_reason_t` values 1 to 9. CG repeats one representative row per step through the C ABI. Each row is additionally run through the `interop_window_graph_classify` export (CG, ZA) with the same expected verdict kind, role, reason and parent, and the export is also called with no graph at all.

| Test name | Rows covered |
|:--|:--|
| `cls_s0_desktop_clear_is_suspended` | OnInputDesktop clear gives Suspended with no admission, no retirement, no update, no deferral, and `suspended` counter +1. A live entry stays Active and unchanged. Also with Visible clear, with an excluded class and with a zero area: Suspended wins. |
| `cls_s0_visible_clear_not_visible` | Visible clear gives NotExported(NotVisible); with a live entry the entry retires (reason NotVisible); beats every later step (an excluded class that is invisible reports NotVisible, not ExcludedClass). Cloaked windows reach the graph as Visible clear (the adapter clears the bit), so there is no separate cloaked row here; see `wo_cloaked_window_not_exported`. |
| `cls_s1_each_excluded_class` | One row per class: Shell_TrayWnd, Shell_SecondaryTrayWnd, Progman, WorkerW, NotifyIconOverflowWindow, TaskListThumbnailWnd, "MSCTFIME UI", IME. Each gives ExcludedClass. |
| `cls_s1_class_match_is_exact_and_case_insensitive` | `progman`, `PROGMAN`, `ime`, `msctfime ui` match; near misses do not: `Progma`, `Progman2`, `IME2`, `MSCTFIME`, `MSCTFIME  UI`, empty-prefix and 256-byte classes that start with an excluded name. Near misses export as Toplevel. |
| `cls_s1_classtruncated_matches_no_class_rule` | ClassTruncated set with the exact text `Progman` exports as Toplevel; same for `IME` and for the CoreWindow class under a shell-host image. |
| `cls_s1_corewindow_by_image` | `Windows.UI.Core.CoreWindow` with image ShellExperienceHost.exe, StartMenuExperienceHost.exe, SearchHost.exe (and upper-case variants) gives ExcludedCoreWindow. Same class with calc.exe, with an empty image plus ImageUnknown, and a non-CoreWindow class under each shell host (`ApplicationFrameWindow`) are not excluded. This is the "do not broaden to all processes or arbitrary classes" row. |
| `cls_s1_agent_owned` | AgentOwned without Preedit gives ExcludedAgent. AgentOwned plus AgentPreedit gives role ImePreedit even with `root != window`, with an owner, with consent.exe credential class, with NOACTIVATE and with TOOLWINDOW (steps 2 to 7 skipped). Preedit still faces step 8: zero width, zero height and layered alpha 0 give ZeroArea and Transparent. An AgentOwned window whose class is `IME` reports ExcludedClass (class rules run first, as in the header list). |
| `cls_s2_uac_modes_credential_dialog` | Image consent.exe (and `CONSENT.EXE`), class `Credential Dialog Xaml Host`: mode Window gives UacConsent, mode Auto gives UacConsent, mode Native gives NotExported(UacNativeHidden). Also with `root != window` (step 2 runs before the Child rule). The mode is fixed at graph creation, so each mode row uses its own graph; in Native mode the offer returns Excluded with reason UacNativeHidden, nothing is admitted and no lookup runs. |
| `cls_s2_uac_dummy_all_modes` | Class `$$$Secure UAP Dummy Window Class For Interim Dialog` on consent.exe gives ExcludedUacDummy in Window, Auto and Native. |
| `cls_s2_uac_non_matches` | Other consent.exe class falls through to Toplevel; the two UAC classes under notepad.exe do not match any UAC rule (Toplevel); the credential class with ImageUnknown does not match (Toplevel, documented consequence in section 9, risk O9). |
| `cls_s3_child_root_differs` | `root != window` gives Child for a plain window, an owned window, a window with class `#32768` and a tooltip class; Child beats steps 4 to 7. |
| `cls_s4_menu_classes_unconditional` | Class `#32768` and class `ComboLBox` give Menu with and without WS_EX_NOACTIVATE, with and without caption bits, owned or ownerless. |
| `cls_s4_menu_style_clause` | WS_POPUP and owned and caption bits not both set and NOACTIVATE clear gives Menu. Negations, one at a time: no WS_POPUP; ownerless; both caption bits set (Toplevel/Dialog result); NOACTIVATE set (Tooltip result). Only WS_BORDER set (one caption bit) still counts as no title bar and gives Menu. |
| `cls_s4_noactivate_binding_difference` | The rows where the two readings of the ambiguous header sentence differ: (a) `#32768` plus NOACTIVATE gives Menu (alternative reading: Tooltip); (b) `ComboLBox` plus NOACTIVATE gives Menu; (c) `#32768` plus both caption bits plus NOACTIVATE gives Menu; (d) WS_POPUP owned no caption plus NOACTIVATE gives Tooltip (both readings agree, guards the style clause). Chosen reading: NOACTIVATE binds only to the style clause. |
| `cls_s4_menu_tooltip_parent` | Menu and Tooltip parent is `owner_root` when owned (OwnerHidden clear) and 0 when ownerless; event `parent_window` agrees. |
| `cls_s5_tooltip_rows` | Class `tooltips_class32`; NOACTIVATE alone (ownerless, not popup); WS_EX_TOOLWINDOW plus owned; TOOLWINDOW ownerless gives Toplevel; tooltip beats Dialog (owned TOOLWINDOW is Tooltip, not Dialog). |
| `cls_s6_dialog_and_appwindow` | Owned without WS_EX_APPWINDOW gives Dialog with parent `owner_root` (not `owner`, test uses owner != owner_root); owned with APPWINDOW gives Toplevel with parent 0. |
| `cls_s7_toplevel` | Ownerless, `root == window`, none of the above gives Toplevel, parent 0. Disabled visible window exports (Enabled clear changes nothing). |
| `cls_s8_gate_zero_and_alpha` | Width 0, height 0, both (ZeroArea, tested for every role: Toplevel, Dialog, Menu, Tooltip, UacConsent, ImePreedit); Layered plus LayeredAlphaKnown plus alpha 0 gives Transparent; alpha 1 and 255 export; Layered with alpha 0 but LayeredAlphaKnown clear exports (opaque); ZeroArea is reported before Transparent when both hold; width/height 16384 export. A live window that becomes zero-area retires with reason ZeroArea. |
| `cls_state_bits_derivation` | Iconic gives Minimized, Zoomed gives Maximized, Fullscreen flag gives Fullscreen, WS_THICKFRAME gives Resizable, WS_MAXIMIZEBOX gives Maximizable, WS_MINIMIZEBOX gives Minimizable; the Activated bit is never produced; events carry the same bits. |
| `cls_ime_preedit_parent` | AgentOwned plus AgentPreedit with owner and owner_root nonzero and OwnerHidden clear gives ImePreedit with parent `owner_root` (event `parent_window` agrees); without an owner parent is 0; with OwnerHidden set parent is 0. The preedit still faces step 8 and the deferral rules (parent absent: Deferred, ParentPending). |
| `cls_precedence_pairs` | Pairwise precedence rows: step0 over 1, 1 over 2, 2 over 3, 3 over 4, 4 over 5, 5 over 6, 6 over 7, 7 over 8 using facts that satisfy both sides. |

#### 7.1.2 Facts validation (`fct`), keys (`key`), UTF-8

| Test name | Rows covered |
|:--|:--|
| `fct_scalar_rules` | window 0, root 0, stamp 0; owner != 0 with owner_root 0 and owner 0 with owner_root != 0; undefined flag bit (bit 13 and bit 31; bit 12 is OwnerHidden and valid only with owner != 0, tested in `own_validation`); AgentPreedit without AgentOwned; LayeredAlphaKnown without Layered; ImageUnknown with image_length > 0 and image_length 0 without ImageUnknown; dpi 95, 96, 960, 961, 0; width, height, min_width, min_height at 16384 (accepted) and 16385 (rejected, never clamped); reserved0 nonzero. Each rejected row returns InvalidFacts, increments `rejected_invalid_facts`, leaves state identical. |
| `fct_length_bounds` | class_length 0, 1, 256, 257; image_length 260, 261; title_length 0 (valid), 1024 (valid), 1025. |
| `fct_utf8_class_image_title` | For each of class, image and title: valid 2, 3 and 4 byte sequences accepted; overlong `C0 80`, `E0 80 80`; surrogate `ED A0 80`; above U+10FFFF `F4 90 80 80`; truncated sequence at the declared length; embedded NUL; each rejected with InvalidFacts. Invalid bytes placed after the declared length are ignored and never copied (the event title equals the declared prefix). |
| `key_codec_rule` | Lookup returns: length 0; length 2 (`a-`); 3 (`a-b`, accepted); 200 (accepted); 201 and above capacity (AppKeyInvalid); prefix `x-abc`; `p-` prefix accepted; characters `.`, space, `/`, NUL, byte 0x80; upper case, digits, `_`, `-` accepted. Nonzero return gives LookupFailed. None of the failures publishes an entry, queues an event, consumes a generation, or stores a key; counters `lookup_failures` and `app_key_invalid` move. |
| `key_lookup_called_once_and_reused` | A successful admission calls the lookup exactly once; Deferred calls it once at first deferral and the stored key is reused at later admission (call count unchanged); Reclassified reuses the stored key (call count unchanged); an Update, an Unchanged offer and a QueueFull rejection do not call it. |
| `key_title_bounds_in_events` | A 1024-byte title and a 200-byte key round-trip bit-exact into Announce, Update and RetireIntent; an empty title is carried as `title_length` 0 with the `Title` change bit on the transition. |

#### 7.1.3 Capacity, deferral, generations, reuse (`cap`, `gen`, `reuse`)

| Test name | Rows covered |
|:--|:--|
| `cap_64_admissions_then_65th` | 64 distinct windows admit (Announce each, generations 1..64). The 65th gets AtCapacity with identical fingerprint, `rejected_at_capacity` +1 and the capacity-diagnostic counter +1, and no lookup call. |
| `cap_retiring_counts_toward_capacity` | With 64 Active, retire one (Retiring): the 65th is still AtCapacity; after `complete_retire` with full proof the 65th admits with generation 65. |
| `cap_65th_retry_on_refresh` | AtCapacity offer inside a refresh stays unseen; after one window disappears and its quarantine completes, the next refresh admits the 65th. No live window is ever evicted. |
| `cap_64_deferred_then_overflow_evicts_oldest` | 64 children with a missing parent defer (no exported capacity used, even with 64 Active). The 65th deferral evicts the lowest `seq`: `deferred_evicted` +1, observation `deferred_dropped` 1, the evicted window is the first one offered, the set is deterministic. Next refresh re-offers the evicted window and it defers again, evicting the then-oldest. |
| `cap_deferral_does_not_use_exported_capacity` | 64 Active plus 64 deferred is a legal state; gauges report both. |
| `cap_flush_truncated_by_capacity` | Parent admits with one free slot and three deferred children: first child by age admits, two stay deferred, counter `at_capacity` +2, Announce order parent then child, `entries_admitted` 2. |
| `cap_flush_truncated_by_generations` | As above with the generation counter one short: leftovers stay deferred, `generation_exhausted` counted. |
| `cap_diagnostic_rate_limit` | With an injected clock: first take immediate when a counter is nonzero; second take within 59 999 ms returns None; at 60 000 ms returns Ok and counters reset; all-zero counters return None; backwards clock counts as zero elapsed; NULL clock selects the default monotonic clock (call returns a value without crashing). |
| `gen_first_is_one_and_strictly_increasing` | Generations are 1, 2, 3 across plain admission, Reclassified re-announce, incarnation change and flushed children; `stats.generations_issued` equals the last one. |
| `gen_never_reused_after_complete` | Retire, complete, re-admit the same window: new generation, larger than every earlier one; 1000-cycle loop on one HWND ends at generation 1001. |
| `gen_exhaustion` | ZG test constructor preloads `generations_issued` to `u64 max - 1`: the next admission gets `u64 max`; the one after fails GenerationExhausted (no mutation, `rejected_generation_exhausted` +1). Existing entries still update, retire and complete. A replacement that cannot be admitted for this reason still retires the old entry (forced retirement, disposition Retired). The C ABI cannot preload the counter, so exhaustion is executed in ZG only; CG asserts the status value `InteropWindowGraphStatusGenerationExhausted == 15`. |
| `reuse_incarnation_tuple_each_field` | Same HWND offered with a changed process_id, then thread_id, then process_start_time, then class (different hash): each retires the old generation with reason IncarnationChanged and admits a new one in the same call (`retired_generation` and `generation` both set, disposition Admitted). |
| `reuse_retiring_coexists_with_new` | Old generation Retiring (not completed), same HWND admitted again: both entries exist, keyed by exact (window, generation); `find_live` returns the new one; `check_action(old)` is Stale; `complete_retire(old)` leaves the new entry untouched. |
| `reuse_forced_retirement_when_replacement_blocked` | Identity changes while the graph is at capacity (replacement impossible): call returns Ok with disposition Retired, old entry Retiring, counter incremented; the next offer retries. Same for LookupFailed and AppKeyInvalid at replacement time. QueueFull at replacement time changes nothing. |

#### 7.1.4 Parents, cycles, reclassification (`par`)

| Test name | Rows covered |
|:--|:--|
| `par_parent_announce_before_child` | Child offered before parent defers; parent offer admits and flushes; drained order is parent Announce then child Announce; child `parent_generation` equals the parent's generation. |
| `par_missing_parent_defers_then_flushes_transitively` | Chain of 5 offered in reverse: all defer; root offer flushes 4 children in age order and transitively; FIFO worklist order asserted event by event. |
| `par_deep_chain_64` | Chain of 64 (limit) admits in parent-first order; child 65 gets AtCapacity; retiring the root (Destroyed) emits RetireIntent for descendants first in descending generation order (63 down to 1 positions) and the root last; the descendants are NOT re-deferred (cause is not a same-incarnation Reclassified). Worst-case call event count is at most 192 and fits an empty ring (also a comptime assert). |
| `par_deferred_cycle_two_and_three` | A owned by B and B owned by A offered in both orders: the second offer returns Cycle, the window's own deferred record is dropped (documented exception), nothing live changes. Three-window cycle likewise. Walk limit 65 steps is exercised by a 64-link deferred chain that is not a cycle (accepted). |
| `par_self_ownership` | `owner_root == window` returns Cycle with no effect and no deferred record created. |
| `par_retiring_parent_defers_child` | Parent Retiring: a new child of it defers (ParentPending); after the parent is re-admitted under a new generation, the child flushes with the new `parent_generation`. |
| `par_parent_retire_cascade` | Parent retires (destroy, refresh unseen, NotVisible, excluded, incarnation change, invalidate): every Active descendant retires first with reason ParentRetired, then the parent; the descendants are NOT re-deferred (deferred count unchanged) and return only through their own next offer, which defers and flushes when the parent is admitted again. |
| `par_cascade_redeferral_reclassified_only` | The only cause that re-defers descendants is Reclassified of the same incarnation: descendants get new `seq` in ascending original generation, inherit the seen mark, are re-announced after the new parent Announce as far as capacity allows; leftovers stay deferred and, being seen, survive a refresh commit; deferred overflow during re-deferral evicts the oldest. Re-run with each other cause: no deferred record is created. |
| `par_cascade_cycle_descendant_parent` | A window reclassified so that its new parent is in its own descendant set: the cascade retires (window Reclassified, descendants ParentRetired), nothing is deferred or re-deferred, the call returns Ok with disposition Retired, `rejected_cycle` +1; the next offers admit the descendants as deferred and the window under its new facts. |
| `par_deferred_identity_mismatch_dropped` | A deferred record D exists; an offer for the same window with a different identity tuple drops D (`deferred_dropped` +1) and is treated as having no D: a fresh lookup runs (call count +1), a new `seq` is taken on re-deferral, the stored key and age of D are not reused. With an equal identity tuple D is reused (no lookup). |
| `par_identity_before_cycle` | A live window whose identity tuple changes and whose replacement would be a Cycle (self-owned or chain cycle) still retires with IncarnationChanged and the call returns Ok/Retired (`rejected_cycle` +1); the same offer with an unchanged identity returns Cycle and leaves the entry unchanged. |
| `par_ownership_change_reclassifies` | A live dialog whose owner_root changes: Reclassified (old generation retires with reason Reclassified, new generation announced, stored key reused, no Update emitted); its own descendants get ParentRetired and re-defer (same incarnation), then flush after the new Announce. Role change Toplevel to Dialog likewise. |
| `par_stale_parent_generation` | A child record holding `parent_generation` g1 while the parent is now g2 never announces against g1: after parent reclassification the child's Announce carries g2; `parent_generation` is never lower than the generation of the parent Announce that precedes it in the stream. |
| `par_refresh_order_deterministic` | The same set of facts offered in the same order twice (fresh graphs) gives byte-identical event streams; a different order gives a different stream but the same final live set with parents first. |

#### 7.1.5 Events, errors, hints (`evt`, `err`, `hint`)

| Test name | Rows covered |
|:--|:--|
| `evt_update_coalesces_into_announce` | Announce queued, rect change, then title change: still one Announce event with the latest payload, `changed_mask` stays full (`InteropWindowChangeMask`), `events_coalesced` +2, observation `events_queued` 0 and disposition Updated. |
| `evt_update_coalesces_into_update_or_mask` | Consumed Announce, then rect change, then dpi change: one Update with mask Rect or Dpi. |
| `evt_no_coalesce_after_consume_or_across_retire` | After `next_event` consumed the Announce a new change queues a new Update. Announce(gen 1), RetireIntent(gen 1), Announce(gen 2), Update(gen 2): the Update merges into Announce(gen 2) only, never into anything for gen 1. Interleaved other windows between do not block merging for an identity. |
| `evt_values_are_immutable_copies` | A popped event is not changed by later graph activity; mutating the caller's copy does not change the queue; mutating or freeing the caller's facts buffer after `observe` does not change the queued payload or the stored record. |
| `evt_retire_intent_payload` | RetireIntent carries last known rect/state/title/key, the reason, `changed_mask` 0, kind 3; Update carries kind 2; Announce kind 1; `reserved0` 0 for all. |
| `err_every_status_leaves_state_intact` | Table-driven: for each function and each of its documented non-Ok statuses the error rule above is asserted (InvalidArgument for each NULL pointer and each zero argument, InvalidFacts, StaleFacts, StaleEpoch, QueueFull, AtCapacity, Cycle, LookupFailed, AppKeyInvalid, GenerationExhausted, RefreshActive, NoRefresh, Stale, NotRetiring, ProofIncomplete, SinkFailed). |
| `err_stale_facts_ordering` | Facts with a stamp below the window's last accepted stamp return StaleFacts and never overwrite newer fields; equal stamp is accepted; `stamp_high_water` advances only on Ok. |
| `hint_destroy_rule_table` | Live entry admitted at stamp 100, tick 5000. Hint (stamp 100, any tick): Unchanged. Hint (stamp 99): Unchanged. Hint (101, tick 4999): Unchanged (older than admission facts). Hint (101, tick 5000): Retired (equal tick is conservative). Hint (101, tick 5001): Retired. Wrap rows (unwrapped against `tick_base`, plain 64-bit `<`): admission tick `0xFFFFFFF0`, hint tick `0x00000005` retires; admission tick `0x00000005`, hint tick `0xFFFFFFF0` is Unchanged. Long-lived rows are in `tck_*` (7.1.10). Destroy hint on a Retiring-only window: Unchanged. Destroy hint cascades: descendants ParentRetired. |
| `hint_destroy_vs_deferred_record` | Deferred record last stamp 50, last tick 7000: hint (51, tick 7000) drops it (`deferred_dropped` 1); hint (50, any) or (49, any) keeps it; hint (51, tick 6999) keeps it (tick clause: the event is older than the record's newest facts); hint (51, tick 7001) drops it. |
| `hint_delayed_destroy_cannot_retire_newer_object` | Window W gen 1 retired and completed; W re-admitted gen 2 from newer facts; the old destroy hint (old stamp, old tick) arrives late: gen 2 stays Active. Then a genuine destroy (newer stamp, newer tick) retires it. |
| `hint_reordered_offers` | Offers for one window arriving as stamps 10, 12, 11: 11 is StaleFacts; final record equals the stamp 12 facts. |

#### 7.1.6 Reentrancy, threads, epochs, teardown (`ree`, `thr`, `epo`, `tdn`)

| Test name | File | Rows covered |
|:--|:--|:--|
| `ree_lookup_callback_reentry` | ZG, CG | The lookup callback calls observe, note_destroyed, drain, next_event, get_stats, find_live, take_capacity_diagnostic, refresh_begin, destroy: each returns Reentrant, destroy does not free, the outer call completes normally; afterwards the graph is not stuck busy, including after the lookup returned LookupFailed or AppKeyInvalid. |
| `ree_sink_callback_reentry` | ZG, CG | The drain sink calls next_event, observe, complete_retire, destroy: Reentrant, no effect; sink return values still honoured. |
| `ree_observer_busy_flag` | CO | From inside the double's `enum_top_level_windows` and `query_facts` (a stand-in for a message dispatched inside a Win32 call) the test calls `pump`, `refresh`, `start`, `stop`, `set_preedit_window`, `destroy`: each returns Reentrant and changes nothing (dirty table, popped key, enumeration buffer, counters identical), `refresh_due` returns 0, `next_timeout_ms` returns WaitForever, `status` returns Reentrant, destroy frees nothing; `note_hint` from the same point is accepted. The outer call completes normally and the flag is clear afterwards on every exit path (Ok, More, GraphQueueFull, EnumFailed, DesktopInactive, GraphError). |
| `ree_graph_reentrant_mapping` | CO | A lookup callback that calls back into the graph makes the graph answer Reentrant inside `observe`/`refresh_offer`; pump puts the key back at the head, stops and returns Reentrant with `last_graph_status` Reentrant and `graph_errors` +1 (never GraphError); refresh aborts its graph refresh, returns Reentrant and the next refresh starts clean (or recovers through RefreshActive when the abort failed). |
| `ree_hint_during_query` | CO | `window_observer_note_hint` called from inside the double's `query_facts`, `enum_top_level_windows`, `install_hook` and `query_input_desktop`: the key is added or coalesced, no iterator is invalidated, the in-flight key (already removed from the table) is re-added, and the next pump processes it. |
| `thr_graph_wrong_thread_every_function` | ZG (spawned `std.Thread`), CG (`run_on_second_thread`) | Each of the 19 exported functions other than `create` (which binds the calling thread) and `classify` (pure, any thread, tested in `own_classify_pure_any_thread`), called from a second thread, returns WrongThread; the full stats struct and fingerprint taken afterwards on the owner thread are identical to before (not even counters change); destroy from the wrong thread does not free; destroy from the owner thread then frees. |
| `thr_observer_wrong_thread` | CO | The double reports a different `current_thread_id` for the calling thread: start, stop, pump, refresh, set_preedit_window, status, destroy return WrongThread (refresh_due returns 0, next_timeout_ms returns WaitForever) with no state change; destroy does not free. `note_hint` from the wrong thread increments only `hints_wrong_thread` and never touches the table. One variant uses a real second thread (`run_on_second_thread`) calling `note_hint` concurrently with owner-thread pumping (run under the sanitizers; the atomic counter is the only shared write). |
| `epo_wrong_epoch_every_function` | ZG, ZA, CG | For every epoch-taking function, epochs 0, `epoch + 1`, `epoch - 1` and `u64 max` return StaleEpoch; only `rejected_stale_epoch` changes. Wrong generation: `check_action` and `complete_retire` with `generation` 0 (InvalidArgument), g-1, g+1, and another window's generation (Stale). |
| `tdn_graph_destroy_with_work_outstanding` | ZG, CG | Destroy with queued events, deferred records, a Retiring entry and an open refresh: frees everything (zero bytes leaked), slot set to NULL, `list_retiring` taken just before holds the pair, a second destroy of the NULL slot is a successful no-op. |
| `tdn_observer_stop_with_queued_hints` | CO | Stop with N queued hints and a latched overflow: table cleared, latches cleared, all three hooks removed in reverse order (cloak, system, object), later `note_hint` is ignored and counted, pump returns BadState, graph untouched (graph stats identical before and after). |
| `tdn_observer_destroy_started` | CO | Destroy of a Started observer stops first; the active slot is released before the free; a hint delivered afterwards (double replays a queued callback) is ignored; ASan clean. A second thread that calls `note_hint` in a loop while the owner thread calls `stop` and `destroy` never touches freed memory (in-flight counter; run under ASan, only meaningful with the real second thread of `run_on_second_thread`). |
| `tdn_destroy_order_independent` | CO | Observer destroyed before graph and graph before observer: both orders are clean. |

#### 7.1.7 Hooks, lifecycle, pump, refresh (`hook`, `wo_*`; all in CO)

| Test name | Rows covered |
|:--|:--|
| `hook_install_order_and_ranges` | start calls install_hook exactly three times: (0x8000, 0x800F), then (0x0016, 0x0017), then (0x8017, 0x8018); stop calls remove_hook three times in reverse order (cloak, system, object) with the returned tokens. |
| `hook_first_install_fails` | First install nonzero: HookFailed, no remove call, state Failed, active slot released (another observer can then start), only destroy allowed (start and stop return BadState). |
| `hook_second_install_fails` | Second install fails: the first token is removed exactly once, HookFailed, state Failed. A remove failure during that cleanup still ends in Failed and does not leak the slot. |
| `hook_third_install_fails` | Third (cloak) install fails: the second and first tokens are removed exactly once each, in that order, HookFailed, state Failed, slot released. A removal failure during that cleanup still ends in Failed. |
| `hook_remove_fails_on_stop` | remove_hook fails for one, two or all three hooks: every hook is still attempted, UnhookFailed, state Failed, restart refused (BadState), destroy returns UnhookFailed and frees. |
| `wo_create_validation` | NULL config; NULL out slot; api NULL on Linux; each of the eight api entries NULL one at a time; first_stamp 0; own_process_id 0; `uac_mode` 3 and `u32 max` (0, 1, 2 accepted). Each gives InvalidArgument and sets a valid out slot to NULL. |
| `wo_create_native_unsupported` | CO (double of the native checks) and the Windows cross-build: when `GetDpiForWindow` is missing, or the thread DPI awareness context is not PER_MONITOR_AWARE_V2, `window_observer_create` returns Unsupported (15) with the slot NULL and nothing allocated; both conditions give the same status; with a supplied api table the checks do not run. Executed natively in the evidence of 8.6. |
| `wo_lifecycle_states` | Created, Started, Stopped, Failed transitions; start twice BadState; stop on Created or Stopped is Ok with no effect; restart after stop keeps the logical clock increasing; second observer gets AlreadyActive and stays unchanged; after the first stops the second starts. |
| `wo_hint_filter_table` | Rejected: object_id -4, 1; child_id 1; window 0; events 0x0015, 0x0018, 0x7FFF, 0x8010, 0x8016, 0x8019, 0x0003. Accepted: 0x8000, 0x8001, 0x8002, 0x8003, 0x800C, 0x800D, 0x800E, 0x800F (parent change), 0x0016, 0x0017, 0x8017 (cloaked), 0x8018 (uncloaked). Counters `hints_filtered` and `hints_received` exact. Hints before start and after stop are ignored. |
| `wo_hint_table_coalescing_and_overflow` | Repeated hints for one HWND coalesce (one entry, kinds OR-ed, latest stamp, destroy stamp and event time kept, first-seen order kept); 512 distinct HWNDs fill the table; HWND 513 latches overflow and `hint_overflows` +1; a hint for an existing HWND in a full table still coalesces. |
| `wo_reserved_dirty_slot` | A pump holds a popped key (`popped_in_flight` 1) while 511 other distinct HWNDs are queued: a hint for a new 512th HWND from inside the double's `query_facts` latches overflow instead of filling the last slot (`dirty_count + popped >= 512`), a hint for an existing HWND still coalesces; the popped key then fails (Failed, QueueFull and Reentrant variants) and its put-back always finds a slot: the key is never lost to overflow, `overflow` is set only by the new HWND. |
| `wo_pump_order_budget_more` | Keys processed in first-seen order; budget smaller than the table returns More; budget larger returns Ok; keys arriving during the call are not processed in that call (min of budget and entries at call start); `stats` may be NULL. |
| `wo_pump_query_results` | Exists admits via observe; Gone calls `note_destroyed` with a fresh stamp and tick; Failed re-adds once at the tail and is dropped after `WindowObserverQueryRetries`, `query_retries_exhausted` +1, and never retires; a query that returns facts for a different window counts as Failed. |
| `wo_pump_destroy_then_requery` | A key with a destroy stamp calls `note_destroyed` first and then requeries; Exists afterwards re-admits with a new generation; Gone afterwards stays retired. |
| `wo_pump_graph_status_mapping` | The double forces the real graph (or a thin shim over it) into each status: InvalidFacts, StaleFacts, Cycle, AtCapacity, GenerationExhausted, LookupFailed, AppKeyInvalid, Stale are counted in `graph_errors`, the key is dropped, processing continues; QueueFull puts the key back at the TAIL with its destroy information and returns GraphQueueFull (the keys behind it are processed first on the next call); Reentrant puts the key back at the head and returns Reentrant; StaleEpoch (wrong epoch passed) and others return GraphError with `last_graph_status` exact and the key put back at the head. |
| `wo_pump_desktop_inactive_and_resync` | Desktop Other and Denied: pump and refresh return DesktopInactive, hints stay queued, graph stats identical, refresh latched; desktop returns to Default: the next loop iteration runs the full refresh and the live set converges. `next_timeout_ms` is not 0 while inactive with a backlog. |
| `wo_overflow_recovery` | Overflow latch: pump calls `invalidate_all` (RetireIntent for every Active entry, children first), clears table and latch, runs the full refresh and re-admits with new generations; QueueFull on `invalidate_all` returns GraphQueueFull and keeps the latch. |
| `wo_refresh_commit_and_unseen` | Refresh offers in enumeration order (a child enumerated before its parent still announces parent first); a window missing from the enumeration and reported Gone by its individual requery is retired at commit with RefreshUnseen (through `note_destroyed` or the commit); a window the double reports Gone in pass 1 is skipped and, if Active, requeried in pass 2. |
| `wo_refresh_fail_closed` | Enumeration Truncated and Failed: EnumFailed with zero graph calls and no refresh begun; an offer that returns QueueFull aborts (GraphQueueFull) and one that returns a call-level status aborts (GraphError); a Suspended disposition (forced by the shim) aborts with DesktopInactive; the desktop is checked once, before the enumeration; commit QueueFull returns GraphQueueFull, leaves the refresh open, and the next refresh detects RefreshActive, aborts and restarts. A failed query does NOT abort: see `rfs_*`. |
| `wo_refresh_cadence` | `refresh_due` at 249, 250 and 251 ms after the last attempt; clock going backwards counts as zero; latched refresh is due immediately; the attempt timestamp is stamped BEFORE the desktop check, so a DesktopInactive, EnumFailed or GraphError attempt counts; latch clears only on commit; `next_timeout_ms` returns 0 when due or hints pending (desktop active), else 1..250, WaitForever when not Started, off thread or busy. |
| `wo_refresh_frozen_clock_no_spin` | The double's monotonic clock is frozen at 500 and the enumeration fails persistently (and in a second run the desktop is Other): after the first attempt `refresh_due` is false and `next_timeout_ms` is not 0 until the clock advances 250 ms; a new latch event (set_preedit_window changing the window, or an overflow) makes it due exactly once more; the number of refresh attempts is at most one per latch event. |
| `wo_facts_adapter_overrides` | The observer overwrites stamp, captured_ms, AgentOwned (own_process_id match), AgentPreedit (preedit window match) and OnInputDesktop (always set after the single per-call desktop check) in every Exists result, forces `title_length` 0 on AgentOwned snapshots and ignores any OwnerHidden bit the double sets; `set_preedit_window` while Started latches a refresh; stamps are strictly increasing across `note_hint` and pump. |
| `wo_stamp_exhaustion` | first_stamp `u64 max - 1`: two stamps issue, the next draw returns StampExhausted from pump and refresh with no graph effect. |
| `wo_cloaked_window_not_exported` | The double reports a visible-but-cloaked window as Visible clear (contract of `query_facts`); the graph answers NotVisible; when the double later uncloaks it, a cloak/uncloak hint (0x8017/0x8018, delivered by the third hook with no show or hide event) plus pump admits it. |
| `wo_parent_change_hint` | A parent-change hint (0x800F) for an owned popup whose `owner_root` the double changed: pump requeries, `observe` returns Reclassified (old generation retires, new announced with the new parent) without any show or hide hint. |
| `wo_uac_modes_end_to_end` | Double world containing a consent.exe credential window and a dummy window under Window, Auto and Native graphs: exported or hidden exactly as `cls_s2_*`; in Native the observer counter `uac_native_hidden` increments per offer. |
| `wo_hwnd_reuse_end_to_end` | Double destroys W and recreates it with a new tuple; the destroy hint arrives after the create hint; the new object stays Active with a new generation, the old generation is Retiring until completed. |
| `wo_convergence_after_refresh` | Property used by the stress run: after any successful full refresh on a Default desktop with no Failed queries, the graph's Active set equals the double world's exported set. |

#### 7.1.8 Retirement, reconnect, sink, queue, counters (`ret`, `rcn`, `snk`, `que`, `cnt`)

| Test name | File | Rows covered |
|:--|:--|:--|
| `ret_quarantine_blocks_actions` | ZG, CG | After retirement `check_action` is Stale for the pair, `find_live` returns None, `list_retiring` lists it; Active pair returns Ok. |
| `ret_proof_incomplete` | ZG, ZA, CG | Proof `{0,1}`, `{1,0}`, `{0,0}`, `{2,1}`, `{1,2}`, `{0xFFFFFFFF,1}`: ProofIncomplete, entry stays Retiring and still occupies capacity, `proof_incomplete` counter moves. Exactly `{1,1}` releases it (`retire_completed` +1). |
| `ret_check_order_stale_notretiring_proof` | ZG, CG | Unknown pair with a bad proof: Stale. Active pair with a bad proof: NotRetiring. Retiring pair with a bad proof: ProofIncomplete (the header order; section 9 notes the earlier design note differed). |
| `ret_duplicate_and_stale_completion` | ZG, CG | Second completion of the same pair: Stale. Completion naming an older generation after the HWND was re-admitted: Stale and the newer entry (Active or Retiring) is untouched. Completion for a window never seen: Stale. |
| `ret_list_retiring_semantics` | ZG, CG | Ascending generation order; `count` is the total even when `capacity` is smaller; capacity 0 with NULL entries is valid; NULL entries with capacity > 0 is InvalidArgument; 64 Retiring entries list completely with capacity 64. |
| `ret_no_proof_from_connection_loss` | ZA, CG | Compile-time and link-time: `interop_window_retire_proof_t` is exactly two `uint32` fields (size 8), no exported symbol other than `complete_retire` releases quarantine, `destroy` with a Retiring entry reports nothing released. The nm check in 7.4 asserts the exported symbol list. |
| `rcn_new_epoch_graph` | ZG, CG, CO | Graph epoch 5 is destroyed after `list_retiring`; a new graph epoch 6 starts at generation 1; late `complete_retire`, `check_action`, `drain`, `observe` carrying epoch 5 return StaleEpoch on graph 6 with no effect; an old observer pumped with epoch 5 gets GraphError with `last_graph_status` StaleEpoch and no graph effect. |
| `rcn_replacement_observer_stamp_floor` | CO | A replacement observer created with `first_stamp = stamp_high_water + 1` keeps offering live windows successfully; one created below the floor sees StaleFacts for known windows (counted in `graph_errors`, keys dropped) until a stamp passes the floor: documents why the coordinator must read `stamp_high_water`. |
| `snk_sink_failure` | ZG, CG | Sink refuses on the 1st, 3rd and last event: SinkFailed, `delivered` exact, accepted events consumed, the refused event still first in the queue, `sink_failures` +1; retry with a good sink delivers the remainder in order; `max_events` 0 and NULL sink are InvalidArgument; empty queue gives None with `delivered` 0; `max_events` smaller than the queue leaves the rest. |
| `que_queue_full_precheck` | ZG, CG | Ring filled to exactly 256 pending events (64 Announce, retire all 64, complete, re-admit, retire again): every event-producing call returns QueueFull with an identical fingerprint, no lookup call, no generation consumed, `rejected_queue_full` and diagnostic `queue_full` move; `refresh_commit` QueueFull keeps the refresh open; one drained event is enough for a one-event plan and not enough for a two-event plan. After a full drain every single operation fits (192 <= 256). |
| `que_control_events_never_dropped` | ZG | Randomised fill/drain: the multiset of Announce and RetireIntent produced equals the multiset delivered; none lost, none duplicated. |
| `cnt_counters_saturate` | ZG | Every `u64` counter field of the stats struct is preloaded to `u64 max` through a test setter looping over `std.meta.fields`; an operation sequence that increments each counter (error paths included) leaves each at `u64 max`. The diagnostic counters likewise. `bump` is also unit-tested at `max - 1`, `max`. |
| `cnt_gauges_match_state` | ZG, CG | `active_count + retiring_count <= 64`, `deferred_count <= 64`, `events_pending == events_queued - events_drained` after every step of the stress run. |

#### 7.1.9 ABI, layout, link, allocation (`abi`, `alc`)

| Test name | File | Rows covered |
|:--|:--|:--|
| `abi_comptime_layout` | ZA | `@sizeOf`, `@alignOf` and `@offsetOf` of every `@cImport`ed header struct the engine uses (`facts_t`, `verdict_t`, `event_t`, `observation_t`, `live_t`, `retiring_entry_t`, `retire_proof_t`, `capacity_diagnostic_t`, `stats_t`, `config_t`; sizes 1656, 24, 1328, 56, 48, 24, 8, 56, 256, 48) equal the header values field by field (there are no hand-written Zig mirrors to drift); enum values of `status_t` equal the header's 0..20 and `verdict_kind_t` 1..3; the exported function pointer types match the header typedefs. |
| `abi_c_static_asserts_and_link` | CG | `_Static_assert` on every size and offset again from the C side (independent of Zig), plus a table taking the address of all 21 `interop_window_graph_*` functions (including `classify`, `refresh_keep`, `refresh_unseen`), which fails to link if one is missing. Observer: `_Static_assert` on `window_observer_api_t` (64), config (40, with `uac_mode` at offset 36), stats (176) and the status numbering (StampExhausted 13, Reentrant 14, Unsupported 15). |
| `abi_pointer_validation_order` | ZA, CG | For each function the common-rule order is exercised: NULL graph, wrong thread, reentrant, epoch, other argument; NULL or zero arguments never reach an unvalidated slice; `create` sets a valid out slot to NULL on every failure; `destroy` of a NULL slot is a successful no-op; `create` with bad `uac_mode` 3, nonzero reserved0, epoch 0 and NULL lookup is InvalidArgument; `classify` with NULL facts, NULL verdict and `uac_mode` 3 is InvalidArgument and leaves the verdict at the sentinel. |
| `abi_outputs_staged_until_ok` | ZA | A failing call after partial planning leaves caller outputs at the sentinel; `observe` and `note_destroyed` outputs are fully written on Ok (all 56 bytes compared against expectation, reserved0 zero). |
| `alc_create_failure_sweep` | ZG, ZA | See 7.3. |
| `alc_operations_do_not_allocate` | ZG | See 7.3. |

#### 7.1.10 OwnerHidden, tick unwrap, refresh keep/unseen (`own`, `tck`, `rfs`)

| Test name | File | Rows covered |
|:--|:--|:--|
| `own_validation` | ZG, CG | OwnerHidden with owner 0 is InvalidFacts (observe, refresh_offer and classify); with owner nonzero and owner_root nonzero it is valid; bit 13 is still undefined. |
| `own_classify_pure_any_thread` | ZG, ZA, CG | `interop_window_graph_classify` without any graph, from a second thread and from inside a lookup callback: Eligible, NotExported and Suspended verdicts with exact role, reason, parent; `reserved0` of the verdict is 0; verdict untouched (sentinel) on InvalidArgument (NULL facts, NULL verdict, `uac_mode` 3) and InvalidFacts; no allocation (failing allocator armed) and no counter or graph change; result equals what `observe` does for the same facts. |
| `own_hidden_owner_roles` | ZG, CG | OwnerHidden set: an owned non-APPWINDOW window is Toplevel with parent 0 (not Dialog); an owned TOOLWINDOW is Toplevel (not Tooltip); `#32768` stays Menu with parent 0; an owned no-caption popup is not a Menu by the style clause; an owned APPWINDOW stays Toplevel; ImePreedit parent is 0 (and `owner_root` when OwnerHidden is clear). |
| `own_promotion_end_to_end` | CO | The owner is a tray window (verdict NotExported ExcludedClass): the owned dialog is announced as Toplevel immediately (never deferred forever, never ParentPending); owner Gone: OwnerHidden, same result; owner Eligible: Dialog with the owner as parent (deferred until the owner is announced); owner Suspended or Failed: the window's query is Failed and the window is neither admitted nor retired (pump retries once, refresh keeps it). |
| `own_flag_change_reclassifies` | ZG, CO | A live Toplevel promoted by OwnerHidden whose owner becomes exportable (flag clear): role/parent change, so Reclassified (old generation retires with Reclassified, new Dialog generation announced, key reused, no Update); the reverse (owner becomes hidden) likewise; an OwnerHidden change that leaves role and parent the same (an owned APPWINDOW) is Unchanged with no event. |
| `own_subquery_counters_and_no_recursion` | CO | Owner sub-query is one extra `query_facts` call per window with a foreign `owner_root` (none when `owner_root` is 0 or the window itself); the owner's own owner is never queried; query counters count one result per window; a classify status other than Ok for the owner snapshot makes the window Failed; the double records that no query is made for AgentOwned titles. |
| `tck_unwrap_exact_and_monotone` | ZG | `tick_base` starts at 2^32; unwrap of ticks around the base (+/- 2^31 - 1); a delayed older tick unwraps below the base and leaves it unchanged; a call returning a non-Ok status never changes `tick_base`; base only moves up. |
| `tck_admission_near_wrap_30_and_60_days` | ZG | Window admitted at tick `0xFFFF0000`; simulated Ok calls every 10 days advance the base; a destroy 30 days later and (separate graph) 60 days later retires the entry in both cases; a stale destroy 10 ms before admission is ignored. The same trace with a 32-bit compare is shown to fail (documents the defect the unwrap fixes). |
| `tck_silence_beyond_2_31_documented` | ZG | After a silence above 2^31 ms the unwrap can alias; the next refresh (RefreshUnseen or requery) corrects the state; no crash or invariant violation. |
| `tck_deferred_destroy_tick_clause` | ZG | Deferred record: destroy with a newer stamp but an older unwrapped tick keeps it; with equal or newer tick drops it. |
| `rfs_seen_rule_per_status` | ZG, CG | In an open refresh `refresh_offer` marks the window seen for Ok, InvalidFacts, StaleFacts, Cycle, AtCapacity, GenerationExhausted, LookupFailed, AppKeyInvalid and QueueFull (the commit then does not retire it); it marks nothing for NoRefresh, NULL pointers, StaleEpoch, WrongThread, Reentrant; a plain `observe` marks only on Ok. Flush leftovers and Reclassified re-deferred records keep the seen mark and survive the commit. |
| `rfs_keep_and_unseen_api` | ZG, ZA, CG | `refresh_keep`: Ok for an Active entry, for a deferred record, None for neither, NoRefresh without a refresh, InvalidArgument for window 0; a Retiring entry is not marked; no event, stamp or tick changes. `refresh_unseen`: ascending generation, never Retiring or deferred, at most capacity entries with `count` the total, NULL windows only with capacity 0, NoRefresh without a refresh, outputs untouched on error. Commit semantics unchanged. |
| `rfs_hinted_not_enumerated_window` | CO | A window admitted through a hint is absent from the enumeration: pass 2 requeries it, Exists -> `refresh_offer` keeps it (no RefreshUnseen); Failed -> `refresh_keep` keeps it; Gone -> `note_destroyed` retires it with Destroyed (or RefreshUnseen if the destroy rule rejects the hint). |
| `rfs_failed_window_does_not_starve_others` | CO | One window's query is permanently Failed; 30 others change every refresh: every refresh commits and returns Ok, the others are updated each time, `queries_failed` counts the failed one, the failed window is never retired and never aborts the refresh; `refreshes_committed` increments. A destroy hint for the failed window still retires it once the destroy rule allows. |
| `rfs_commit_after_failed_queries` | CO | Refresh with failed queries commits (`refresh_requested` cleared, status Ok) and retires exactly the windows that are Gone/absent after pass 2. |
| `rfs_offer_rejection_does_not_retire` | CO | An offer rejected with StaleFacts, AtCapacity or Cycle in pass 1 does not cause that window's retirement at commit (graph_errors counted, refresh continues, next refresh re-offers). |

### 7.2 Random and stress trace design

Purpose: show that the documented state machine holds under long interleavings that hand-written rows cannot enumerate: HWND reuse, capacity pressure, reordered hints, queue pressure and cascading retirement.

Generator. `xorshift_t` is xorshift64* (shifts 12, 25, 27, multiplier `0x2545F4914F6CDD1D`) with an explicit nonzero seed; a seed is `0x9E3779B97F4A7C15 * (k + 1)` for trace number `k` (a fixed table, no wall clock, no address-derived value). Each trace has four phases of equal length, with the HWND pool size changing per phase: 24 handles (heavy reuse), 80 handles (capacity pressure above 64), 40 handles with parent chains up to depth 8, and 24 handles with queue pressure (drain only every 8th to 40th step so the ring approaches 256).

Operation mix per step (weights in percent): admit or re-offer a window from a profile catalogue (toplevel, dialog, menu, tooltip, preedit, consent in each mode, excluded class, invisible, zero area, transparent) 30; change one field of a live window (rect, title, dpi, state bit) 18; change owner or role (reclassify) 7; change one identity-tuple field (incarnation change) 3; destroy hint, genuine 7; destroy hint, delayed (stamp or tick older than admission) 5; refresh (begin, offer a random subset in random order including parents-after-children, `refresh_keep` for a random subset of the rest, optionally `refresh_unseen` followed by offer, keep or note_destroyed of the listed windows, commit or abort, 20 percent leave open) 6; flip the OwnerHidden flag of a live owned window 3; `complete_retire` correct pair with full proof 8; `complete_retire` wrong generation or incomplete proof 4; drain 1 to 40 events through a collecting sink 7; occasional sink refusal 1; `invalidate_all` 1; `check_action` 2; offers with an older stamp 3; offers with the OnInputDesktop flag clear 2; lookup failure or bad key injected on 2 percent of lookups; epoch-mismatched call 1. Stamps come from a counter that normally increases by 1 and sometimes by a random 1 to 5, and event ticks wrap through `0xFFFFFFFF` once per trace (the model keeps its own 64-bit tick base with the same unwrap rule; one phase advances the tick by 20 to 40 days between Ok calls to cross the 2^31 boundary of a 32-bit compare).

Reference model. `reference_model_t` is written independently of the engine inside the fixtures: arrays of at most 64 entries and 64 deferred records, linear scans only, no ring optimisation, and the intended role of every generated fact is known from its catalogue profile, so the model contains no classifier. It re-implements the documented rules: admission and deferral, flush order (age order, FIFO worklist, parent first), retirement order (descending generation), forced retirement, destroy-hint rule on 64-bit unwrapped ticks (entries and deferred records), identity-before-cycle order, cascade re-deferral only for same-incarnation Reclassified, seen marks (offer marks every outcome, keep, plain observe on Ok), refresh commit, coalescing, quarantine and capacity. It produces the expected result of each call and the expected event list.

Per-step comparison (after every operation, not only at the end): status; for observe-type calls `disposition`, `role`, `reason`, `generation`, `retired_generation`, `events_queued`, `entries_admitted`, `entries_retired`, `deferred_dropped`; the three gauges and `events_pending`; `find_live` for every pool window; and, at every drain, each delivered event field by field against the model's expected queue. Any difference prints seed, step index, operation and both values and fails the test.

Invariants checked by `check_invariants` after every step (independent of the model):

1. Parent before child: every Active entry with `parent_window != 0` has an Active parent whose generation equals `parent_generation` and is smaller than the child's generation. In the delivered event stream every Announce is preceded by its parent's Announce with no parent RetireIntent between them, and every parent RetireIntent is preceded by the RetireIntent of each child that was Active at that time.
2. No generation reuse: each admission's generation is larger than all earlier ones; `generations_issued` equals the number of Announces ever queued for new generations; (window, generation) pairs are unique across entries; at most one Active entry per window.
3. Capacity: Active plus Retiring at most 64, deferred at most 64, pending events at most 256; `events_pending == events_queued - events_drained`.
4. No event for a quarantined action: after a RetireIntent for a pair, no later Announce or Update names that pair, `check_action` on it is Stale until `complete_retire`, and `complete_retire` removes only that pair.
5. Failed calls change nothing: every call that returns a non-Ok status keeps `fingerprint` identical (Cycle excepted, with its own check).
6. Monotonic stamps: `stamp_high_water` never decreases; no accepted facts have a stamp lower than the entry's `last_stamp`.
7. Deferred hygiene: no deferred record names a window that is Active; no cycle in deferred parent links; no deferred record carries a generation.
8. After a committed refresh no Active entry is unseen.
9. Deterministic replay: each seed runs twice on fresh graphs; a rolling FNV-1a 64 hash over all call results and all delivered event bytes must match between the two runs. For seeds `k = 0` and `k = 1` the hash is also pinned as a literal in the test as a regression tripwire. When intended behaviour changes, the literal is regenerated by running the test once with the print flag and the change is recorded in the commit message.
10. No leak: tests run under `std.testing.allocator` (zero bytes leaked) and the allocator is a `FailingAllocator` with `fail_index = 1` so any allocation after create fails the trace (7.3).

Minimum counts. ZG `rnd_traces_match_reference_model`: 64 traces of 3000 steps (192 000 steps). ZG `rnd_stress_long`: 16 traces of 30 000 steps (480 000 steps), model comparison on, meant to run in ReleaseSafe too. ZA `rnd_abi_replay`: 8 traces of 2000 steps through the exported functions, comparing the staged outputs with a direct engine run on a twin graph (equal observation structs and equal event bytes). CG `rnd_c_stress`: 8 traces of 2000 steps through the C ABI with only structural checks (invariants 1 to 3 through the sink stream, deterministic hash replay, QueueFull handled by draining), under ASan, LSan and UBSan. CO `wo_stress_convergence`: 8 seeds of 3000 steps of random hints, destroys, reused HWNDs, Failed and Gone queries, desktop flips, overflow bursts of more than 512 distinct handles, and forced graph QueueFull; every 250 simulated milliseconds a refresh is allowed; at the end of each seed the desktop is forced to Default, queries succeed, the loop is run to quiescence, and the graph's Active set must equal the double world's exported set (`wo_convergence_after_refresh`), with all invariants 1 to 4 checked on the drained stream.

### 7.3 Allocation-failure sweep and leak proof

Allocation facts that the tests pin: the graph performs exactly one heap allocation, `allocator.create(graph_t)` of `@sizeOf(graph_t)` bytes, in `create`; `destroy` performs the one matching free; no other call allocates, resizes or frees. The portable observer core performs exactly one `calloc` (observer, dirty table and enumeration buffer in one block) and one `free`; it never allocates in `pump` or `refresh`. The native `query_facts` makes one bounded transient allocation per call (the image-path buffer; the Toolhelp snapshot is a handle, not a heap block) that is freed on every path before the call returns; the api double allocates nothing, so the portable tests see zero per-query allocations. Invalid configuration is rejected before any allocation.

`alc_create_failure_sweep` (ZG):

1. A discovery run calls `create(counting_allocator, config)` where `counting_allocator_t` (a fixture) wraps `std.testing.allocator` and counts allocation, resize and free calls and bytes. It records `n = allocations` (expected 1) and `allocated_bytes == @sizeOf(graph_t)`, then destroys and asserts `freed_bytes == allocated_bytes`.
2. The sweep runs `fail_index` from 0 to `n` inclusive. Each iteration builds `std.testing.FailingAllocator.init(std.testing.allocator, .{ .fail_index = fail_index })` and calls `create`. For `fail_index < n` the result must be `error.OutOfMemory` with no live allocation. For `fail_index == n` it must succeed, and `destroy` must return every byte.
3. The underlying `std.testing.allocator` fails the test at exit if a single byte is still allocated, so a partial create that leaks cannot pass. The sweep bound is computed, not hard-coded, so adding a second allocation later widens the sweep instead of silently skipping it.
4. A second sweep covers the same indices with an invalid config (zero epoch, NULL lookup, reserved0 nonzero, uac_mode 3): zero allocations in every iteration.

`alc_create_failure_sweep` (ZA): the module-level `abi_allocator` is replaced by a `FailingAllocator` interface for the duration of the test (restored by `defer`); `interop_window_graph_create` is called through `@call(.never_inline, ...)` for `fail_index` 0 to `n`. For `fail_index < n` the status is NoMemory and the caller's slot is NULL; at `n` it is Ok and the handle is destroyed with `interop_window_graph_destroy` after `abi_allocator` was already restored (this proves destroy frees through the allocator stored in the graph, not through the module variable).

`alc_operations_do_not_allocate` (ZG): a graph is created with a `FailingAllocator` whose `fail_index` is 1, so every allocation after create returns OutOfMemory. The full `rnd_traces_match_reference_model` set and `rnd_stress_long` run on that graph; afterwards the counting wrapper must show allocations 1, resizes 0, frees 0, and 1 free after `destroy`. Any hidden allocation in an operation either errors the trace or fails this count.

Observer allocation (CO): `wo_create_alloc_failure` forces `calloc` to fail once and expects NoMemory with the slot NULL and no leak. The committed makefile has no allocation seam, so the implementing session adds `WgObsWrap ?= -Wl,--wrap=calloc` to the observer test link lines (plain, sanitizer and coverage recipes); `tests/interop/window_observer_windows.c` then defines `__wrap_calloc` (fails when its countdown reaches zero, otherwise calls `__real_calloc`, so ASan still interposes). This requires the observer to allocate with `calloc` by name; this is a stated requirement on `window_observer_windows.c`, and the delta to `scripts/interop_window_graph.mk` is part of the deliverable (the file is in the owned list). Until it is applied the test is not built and 7.5 lists the `NoMemory` branch as not executed.

C-side proof of zero leaks. All C tests run under `-fsanitize=address,leak,undefined -fno-omit-frame-pointer` with `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1` and `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. The Zig object is not instrumented, but its `std.heap.c_allocator` memory comes from libc `malloc`/`posix_memalign`, which ASan and LSan interpose; a deliberate leak through the Zig allocator was verified during research to be reported with Zig frames and exit 134. Each C test creates and destroys its own graph and observer; process exit with detect_leaks=1 reports any block still reachable only from lost pointers. Leak-detection control (run by hand once, result recorded in HANDOFF_RETURN, not a make target): build `tests/interop/window_graph.c` with `-DWindowGraphTestLeakProbe`, which skips one destroy; the sanitizer run must abort with `Direct leak of <sizeof graph_t> byte(s)`. Disabling leak detection is never a passing configuration. UBSan covers signed overflow in the C tests and in observer arithmetic (the observer computes only unsigned elapsed-time differences with a backwards clock counted as zero; the signed tick unwrap lives in the Zig engine, where Zig's integer-overflow safety checks apply, and uses wrapping `-%` and a bitcast, never a signed subtraction of unsigned values).

### 7.4 Exact commands

Working directory for every command: `/home/dev/.codex/worktrees/seamless-window-graph/Waddle-LSW` (called W). Never run from `/home/dev/Waddle-LSW`. Environment (the makefile exports the same values itself; set them by hand only for the manual commands):

```
cd /home/dev/.codex/worktrees/seamless-window-graph/Waddle-LSW
export PATH="$HOME/.local/bin:$PATH" PYTHONDONTWRITEBYTECODE=1
export ZIG_LOCAL_CACHE_DIR="$PWD/build/window_graph/zig-local" ZIG_GLOBAL_CACHE_DIR="$PWD/build/window_graph/zig-global"
mkdir -p build/window_graph build/window_graph_logs
```

Both Zig cache variables are required: with only one set, Zig still writes to `~/.cache/zig`. `PYTHONDONTWRITEBYTECODE=1` is required because `scripts/__pycache__/*.pyc` files are tracked. `.gitignore` ignores `build/` but not `.zig-cache`, so the caches stay under `build/`.

Make targets (all from W; `make -f scripts/interop_window_graph.mk <target>`; the file is not included by GNUmakefile or `scripts/interop.mk`):

| Target | What it runs | Executes code | Key commands |
|:--|:--|:--:|:--|
| `window-graph-help` | prints targets | no | none |
| `window-graph-test` | builds `build/window_graph/window_graph_abi.o`, `window_graph_c_test`, `window_observer_test`; runs the two `zig test` modules and both C binaries | yes | below, steps 1 to 4 |
| `window-graph-sanitizers` | rebuilds both C tests with ASan, LSan, UBSan into `build/window_graph/san/` and runs them | yes | step 5 |
| `window-graph-coverage` | sed copy of `tests/av/coverage.py`, Zig modes `window_graph` and `window_graph_abi`, then gcov of the observer | yes | step 6 |
| `window-graph-windows` | cross-compile and link only | no | step 7 |
| `window-graph-all` | the four above in order | partly | all |
| `window-graph-clean` | removes `build/window_graph` only (refuses empty, `/`, `.`, `..`) | no | none |

Logging pattern for evidence (exit code kept through `tee`):

```
set -o pipefail; make -f scripts/interop_window_graph.mk window-graph-all 2>&1 | tee build/window_graph_logs/window-graph-all.log; echo "exit=${PIPESTATUS[0]}" | tee -a build/window_graph_logs/window-graph-all.log
```

Manual equivalents, in the order the targets run them:

1. Zig engine and ABI tests (Debug, safety checks on, `std.testing.allocator` leak check on):
   `zig test src/interop/window_graph.zig -Iinclude -lc`
   `zig test src/interop/window_graph_abi.zig -Iinclude -lc`
   Extra run, same tests with optimizer and safety: add `-O ReleaseSafe` to both commands. Both must report all tests passed and exit 0. `-lc` is mandatory because `std.heap.c_allocator` needs libc.
2. ABI object (the one object every C consumer links):
   `zig build-obj src/interop/window_graph_abi.zig -Iinclude -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=build/window_graph/window_graph_abi.o`
3. C ABI test: `cc -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -pthread -Iinclude tests/interop/window_graph.c build/window_graph/window_graph_abi.o -o build/window_graph/window_graph_c_test && build/window_graph/window_graph_c_test`
4. Observer test with the api double and the real graph: `cc -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -pthread -Isrc/interop -Iinclude src/interop/window_observer_windows.c tests/interop/window_observer_windows.c build/window_graph/window_graph_abi.o -o build/window_graph/window_observer_test && build/window_graph/window_observer_test` (plus `$(WgObsWrap)` once the allocation seam of 7.3 exists).
   Symbol check, run after step 2: `nm -g --defined-only build/window_graph/window_graph_abi.o | awk '$3 ~ /^interop_window_graph_/ {print $3}' | sort > build/window_graph/abi_symbols.txt; grep -o 'interop_window_graph_[a-z_]*(' include/waddle/interop_window_graph.h | tr -d '(' | sort -u > build/window_graph/header_symbols.txt; diff build/window_graph/abi_symbols.txt build/window_graph/header_symbols.txt` must print nothing: the object exports exactly the 21 header functions and nothing that could count connection loss as proof.
5. Sanitizers (compiler must be GCC; `clang-19` on the reference machine lacks the ASan runtime library and cannot link): `cc -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer -pthread -Iinclude tests/interop/window_graph.c build/window_graph/window_graph_abi.o -o build/window_graph/san/window_graph_c_test`, and the same flags for the observer test into `build/window_graph/san/window_observer_test`. Run each with `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. Exit 0 and no sanitizer report required.
6. Coverage.
   Zig modes: `sed "s/^InteropModes = (/InteropModes = ('window_graph', 'window_graph_abi', /" tests/av/coverage.py > build/window_graph/coverage_window_graph.py`, then `PYTHONPATH=tests/av python3 build/window_graph/coverage_window_graph.py window_graph` and the same with `window_graph_abi`. The sed must change exactly one line (the recipe checks `diff` shows 2 changed lines and the new text is present); `git diff --stat tests/av/coverage.py` must stay empty. The script compiles `src/interop/<mode>.zig` with `-Iinclude -lc -O ReleaseSafe`, takes production text as the bytes before the first `// Test-only fixtures.`, instruments the optimized LLVM IR, runs the instrumented test binary, and fails (exit nonzero) when line or branch is below 90 percent. Output: `build/coverage/interop/<mode>/run-<UTC>-<pid>/` (this path is fixed by the tracked script and is outside `build/window_graph/`; it is still ignored by git). Printed lines have the form `branch NN.NN% (a/b)` and `line NN.NN% (a/b)`.
   Rules the tests obey so the numbers mean something: (1) every direct call under test in fixtures goes through an `@call(.never_inline, f, .{...})` wrapper, and inputs come from runtime data (tables, PRNG, `std.mem.doNotOptimizeAway`), because direct calls with comptime-known arguments are constant-folded to zero sites, which makes the tool abort with "coverage site count must fit nonzero uint32" or report 0/N; (2) `// Test-only fixtures.` appears exactly once per file (`grep -c` must print 1) and no doc comment above it contains that string; (3) no `switch` has more than 255 distinct targets; (4) the branch metric counts conditional branches that survive in optimized IR, so an `if` converted to a `select` is visible only to line coverage; the matrix rows in 7.1 therefore exercise both sides of every decision explicitly instead of relying on the metric.
   Observer C (portable part), compiler and gcov are both GCC 14: `mkdir -p build/window_graph/coverage/observer; gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -O0 -g --coverage -fprofile-update=atomic -fprofile-abs-path -Isrc/interop -Iinclude -c src/interop/window_observer_windows.c -o build/window_graph/coverage/observer/window_observer_windows.o`; compile the test object with plain `cc`; link both with `gcc --coverage -pthread` and `build/window_graph/window_graph_abi.o`; run the binary; `cd build/window_graph/coverage/observer && gcov --json-format -b -c window_observer_windows.gcno > /dev/null`; the recipe's `python3 -c` reads `window_observer_windows.gcov.json.gz`, selects the single record ending in `src/interop/window_observer_windows.c`, counts lines with `count > 0` over all lines and branches with `count > 0` over all branch entries, prints `line a/b branch c/d` and exits nonzero below `WgCovMinPercent` (90). Use the `-b -c` JSON counts, not the text report's "Branches executed" figure, which is a different metric.
7. Windows cross-compile and link (not executed): `zig build-lib src/interop/window_graph_abi.zig -Iinclude -static -target x86_64-windows-gnu -O ReleaseSafe -fno-compiler-rt -lc -femit-bin=build/window_graph/window_graph_abi_windows.lib`; `zig cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude tests/interop/window_graph.c build/window_graph/window_graph_abi_windows.lib -o build/window_graph/window_graph_c_test.exe`; `zig cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -U_WIN32_WINNT -D_WIN32_WINNT=0x0A00 -UWINVER -DWINVER=0x0A00 -DUNICODE -D_UNICODE -Isrc/interop -Iinclude src/interop/window_observer_windows.c tests/interop/window_observer_windows.c build/window_graph/window_graph_abi_windows.lib -ldwmapi -o build/window_graph/window_observer_test.exe`; and the native observer object alone with `-c` once with `zig cc -target x86_64-windows-gnu` and once with `x86_64-w64-mingw32-gcc`, both with the same `-U_WIN32_WINNT ... -D_UNICODE` defines. Rules: the defines go on the command line, never in source (Zig predefines `_WIN32_WINNT=0x0603`, so a source `#define` is a `-Wmacro-redefined` error under `-Werror`); `WINVER` must be at least 0x0605 or `GetDpiForWindow` is not declared; `-ldwmapi` is required for `DwmGetWindowAttribute` (user32 and kernel32 link by default); `file build/window_graph/*.exe` must report `PE32+ executable (console) x86-64`. Because the Windows C flags carry no `-pthread`, `tests/interop/window_graph.c` and `window_observer_windows.c` start their second thread through one helper, `run_on_second_thread`, implemented with `pthread_create` when `_WIN32` is undefined and `CreateThread` under `_WIN32`, so the same files cross-compile.
8. Tree hygiene check after everything: `git status --short` lists only the owned paths from the handoff (and nothing under `build/`, which is ignored); `git diff --stat -- tests/av include/waddle/interop_protocol.h GNUmakefile scripts/interop.mk README.md impl/seamless-interop/IMPL_DESC.md impl/seamless-interop/TRACKER.md` is empty.

### 7.5 What is not proven, exclusions, pass criteria

Not proven by anything in this section:

1. No Win32 code ran. The `_WIN32` native section of `window_observer_windows.c` (hook procedure, `native_query_facts`, `native_enum`, desktop query, UTF conversion, DPI resolution and the PER_MONITOR_AWARE_V2 check, process image and start-time reads, the Toolhelp image fallback, the per-query image-path allocation, DWM calls) is only compiled and linked by `window-graph-windows`. Its behaviour is unverified until Codex's native runner executes it (section 8).
2. The api doubles are scripted fakes. They prove the observer's control flow, ordering, bounds and graph interaction, not that Windows delivers hints in that order, drops none, or reports the facts the double returns. They are not real-session or desktop acceptance.
3. No UIAccess, secure-desktop, elevated-process or UIPI behaviour is demonstrated. The tests prove only that an unreadable image or start time (ImageUnknown, tuple fields 0) is handled without a crash and without a UAC or CoreWindow match.
4. Coverage excludes: the `_WIN32` section of the observer (not compiled on Linux, therefore absent from gcov); all Zig test blocks and fixtures below `// Test-only fixtures.`; the C test files; compiler panic and stack-canary guard branches (excluded by `coverage.py`, a count is printed); `select`-converted decisions in the branch metric (covered by line metric and by matrix rows). The 90 percent gate applies per metric and per measured unit: `window_graph.zig`, `window_graph_abi.zig`, portable `window_observer_windows.c`. Whether the gate holds is decided by the run, not by this document.
5. Observer counter saturation (`sat_add` in C) cannot be driven without a test hook; it is checked by review of the single helper only. The graph's counter saturation is executed (`cnt_counters_saturate`).
6. `window_observer_create` NoMemory is executed only after the `--wrap=calloc` makefile delta of 7.3 exists.
7. The Zig object is not sanitizer-instrumented; Zig-side memory errors rely on Zig safety checks in Debug and ReleaseSafe and on `std.testing.allocator`.
8. Time behaviour: the 32-bit wrap and the 30/60-day unwrap are exercised with synthetic ticks; the equality of the `GetTickCount` and WinEvent `dwmsEventTime` bases is an assumption (9.1 and 9.2), as is acceptance of the destroy-rule deviation of 4.8 (Codex, 8.6).
9. Performance, soak, hook-delivery loss under load, and the cost of a 250 ms `EnumWindows` refresh are not measured here.

Pass criteria for handing this slice back (all required, none waivable):

1. `window-graph-test`, `window-graph-sanitizers`, `window-graph-coverage` and `window-graph-windows` each exit 0, logs saved under `build/window_graph_logs/` with the exit code recorded in `HANDOFF_RETURN.md`.
2. Every row of 7.1 exists as a named test in the stated file and passes; the number of tests per file is recorded.
3. Zig tests: all tests passed in Debug and ReleaseSafe, zero bytes leaked. C tests: zero sanitizer reports with `detect_leaks=1`.
4. Line and branch coverage each at least 90 percent for `window_graph.zig`, `window_graph_abi.zig` and portable `window_observer_windows.c`, with the printed numbers and run directories recorded.
5. Windows artifacts link; they are reported as built, not run.
6. The symbol check, the delimiter count and the empty `git diff --stat` checks of 7.4 pass.

## 8. Integration Requirements for Codex

This section is the contract between the delivered engine and the future guest session main loop. Codex owns the loop; the graph and observer own nothing the loop must not also track (heap runs, capture, pins, transport).

### 8.1 Ownership and thread rules

1. One guest event-loop owner thread creates the graph, creates the observer, runs the Win32 message pump, and makes every call into either object. It must outlive both. There is no cross-thread entry except `window_observer_note_hint` (called by the hook procedure on the same thread, and ignored with an atomic counter elsewhere).
2. There is one graph per nonzero `session_epoch` and one started observer per process. A reconnect that changes the epoch builds a new graph (generations restart at 1 in the new epoch) and a new observer or a restarted one.
3. Neither object touches transport, wire frames, shared memory, capture or input. The loop translates events to the frozen protocol and decides what proof it has.

### 8.2 Ordered calls: start-up

1. Choose `session_epoch` (nonzero) from the session handshake.
2. Fill `interop_window_graph_config_t` with `session_epoch`, `uac_mode` from the session configuration (`InteropWindowUacWindow`, `InteropWindowUacAuto` or `InteropWindowUacNative`), `reserved0 = 0`, `app_key_lookup` and `app_key_context` (the application catalog callback: bounded, non-blocking, returns a valid protocol-v2 key or nonzero, never calls the graph), and `now_ms` plus `now_ms_context` (NULL for the default monotonic clock). Call `interop_window_graph_create(&config, &graph)`; anything but Ok is fatal for seamless mode.
3. Call `interop_window_graph_stamp_high_water(graph, &high_water)`. Set `first_stamp = high_water + 1` (for a fresh graph this is 1; if `high_water == UINT64_MAX` roll the epoch).
4. Fill `window_observer_config_t`: `api = NULL` (native table under `_WIN32`), `api_context = NULL`, `first_stamp`, `preedit_window` (HWND of the agent's IME preedit window or 0), `own_process_id = GetCurrentProcessId()`, `uac_mode` equal to the graph's (0..2; used for the owner classification that decides OwnerHidden). The loop thread must be the thread that creates the observer and it must already run with `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2` (application manifest, or `SetThreadDpiAwarenessContext` before this call): rectangles and dpi are reported in physical pixels of a known scale and the observer never changes the awareness of a thread. Call `window_observer_create`. `Unsupported` (no `GetDpiForWindow`, or the wrong awareness) is fatal for seamless mode on that thread; fix the awareness and create again.
5. Call `window_observer_start`, which installs three out-of-context hooks (object range up to parent change, minimize, DWM cloak/uncloak). HookFailed and UnhookFailed leave the observer Failed: destroy it and report seamless mode unavailable. AlreadyActive is a programming error. A successful start latches a full refresh, so the first loop iteration enumerates the desktop.

### 8.3 Ordered calls: one loop iteration

1. Wait: `timeout = window_observer_next_timeout_ms(observer, monotonic_ms())`, combined (minimum) with the loop's other deadlines. Block in a message wait (for example `MsgWaitForMultipleObjectsEx` with `QS_ALLINPUT`) up to that timeout. `WindowObserverWaitForever` means the observer needs no wake-up.
2. Message pump: drain the thread's messages with `PeekMessageW`, `TranslateMessage`, `DispatchMessageW` (bounded count per iteration for fairness). WinEvent callbacks are delivered here and reach `window_observer_note_hint`.
3. `window_observer_pump(observer, graph, session_epoch, budget, &stats)` with a budget of at least 1 (recommended 64). Handle: Ok (idle), More (run another iteration without waiting), DesktopInactive (skip graph work; leave hints queued; the coordinator's own desktop-switch handling is separate), Reentrant (the call came from inside a nested message dispatch: drop it and retry from the loop; never treat it as a failure), GraphQueueFull (go to step 6, then repeat this step ONLY if that drain delivered at least one event; if it delivered none, the outbound path is not writable, so do not call pump or refresh again: wait for the transport's writable event, which is the only thing that can make room), GraphError (read `stats.last_graph_status`; StaleEpoch means the session was rolled, tear down; others are logged and fail-close the session), StampExhausted (replace the observer using the graph's `stamp_high_water`, or roll the epoch), BadState, InvalidArgument and WrongThread (programming errors).
4. `if (window_observer_refresh_due(observer, monotonic_ms()))` call `window_observer_refresh(observer, graph, session_epoch, &stats)`. Ok commits (also when some window queries failed: those windows were kept); DesktopInactive and EnumFailed are retried by the 250 ms cadence (`InteropWindowGraphRefreshIntervalMs`) with nothing retired; Reentrant is retried by the loop; GraphQueueFull means drain (step 6) and let the next due refresh restart it, under the same rule as step 3: only after a drain delivered at least one event, otherwise wait for the transport to become writable (a refresh that is retried against a full outbound path would only hit the same full ring).
5. Optionally `interop_window_graph_take_capacity_diagnostic(graph, &diagnostic)` once per iteration; Ok is logged (the graph already rate-limits to once per minute).
6. `interop_window_graph_drain(graph, session_epoch, sink, sink_context, max_events, &delivered)` with `max_events` of at least 1 (recommended 256). The sink copies the event and either serializes it with the frozen codec into the outbound path or appends it to a coordinator queue; it returns nonzero when its outbound capacity is exhausted (the event stays queued and drain returns SinkFailed; retry next iteration). It must not call the graph. Drain after every pump or refresh, and again immediately on GraphQueueFull. Events are delivered in order: Announce parent before child, RetireIntent child before parent.
7. Coordinator serialization: Announce and Update become the corresponding protocol messages (the `changed_mask` selects optional groups); RetireIntent makes the coordinator mark `(window, generation)` as pending retirement, stop sending actions for it and stop capture.
8. Retire proof: when the slot runtime reports that capture for the pair stopped and every frame pin drained (the proof-gated drain already tracked by the slot runtime), call `interop_window_graph_complete_retire(graph, session_epoch, window, generation, &proof)` with `proof = {1, 1}` and remove the pair from the pending set. Stale means a duplicate or already-completed pair (ignore); NotRetiring and ProofIncomplete are coordinator bugs. Every few seconds reconcile the pending set against `interop_window_graph_list_retiring` so a RetireIntent that was never delivered cannot leave a slot quarantined forever; but reconcile only pairs whose RetireIntent event was delivered to the coordinator (consumed by `drain`), or run the reconcile after a drain that emptied the ring: a Retiring pair whose RetireIntent is still queued must not be adopted as an orphan before its event is delivered.
9. Before executing any guest action (input, focus, close) for `(window, generation)`: `interop_window_graph_check_action`; not Ok means drop it.
10. When the IME preedit window changes: `window_observer_set_preedit_window(observer, hwnd)`.

The graph has no call that lists Active entries. The coordinator derives the Active set from the Announce and RetireIntent stream it has received.

### 8.4 Ordered calls: shutdown, transport reset, new epoch

1. `window_observer_stop(observer)`: the three hooks removed in reverse order (UnhookFailed is logged; the observer is then Failed and only destroyable), dirty table cleared. Nothing is retired and no proof exists.
2. If the transport is still usable, drain remaining events (steps 6 to 7 above); otherwise leave them.
3. Hand the quarantine to the slot runtime: `interop_window_graph_list_retiring(graph, entries, InteropMaxWindows, &count)`, union with the coordinator's own pending set and with every Active pair it announced. The slot runtime keeps the heap runs, mappings and pins for those pairs alive until it has its own proof. Connection loss is never proof; destroying the graph is never proof.
4. `interop_window_graph_destroy(&graph)`; must run on the owner thread and returns WrongThread or Reentrant without freeing otherwise.
5. `window_observer_destroy(&observer)`. Order relative to the graph is free (the observer never touches the graph), but the api context must outlive the observer.
6. A new session builds a new graph with a new epoch. If instead the graph is kept and only the observer is replaced, the replacement's `first_stamp` must be `stamp_high_water + 1` or its facts return StaleFacts. Roll the epoch when `GenerationExhausted` or `StampExhausted` is reported.

### 8.5 Build, link and CI wiring (Codex-owned files)

| Item | Linux (host build, CI, tests) | x86_64 Windows (guest agent) |
|:--|:--|:--|
| Graph object | `zig build-obj src/interop/window_graph_abi.zig -Iinclude -O ReleaseSafe -fPIC -fcompiler-rt -lc` | `zig build-lib src/interop/window_graph_abi.zig -Iinclude -static -target x86_64-windows-gnu -O ReleaseSafe -fno-compiler-rt -lc` |
| Observer | `src/interop/window_observer_windows.c` links with api doubles only (tests); the native table is not compiled | the same file with `-U_WIN32_WINNT -D_WIN32_WINNT=0x0A00 -UWINVER -DWINVER=0x0A00 -DUNICODE -D_UNICODE`, `-ldwmapi` at link |
| Include paths | `-Iinclude -Isrc/interop` (the header includes `waddle/interop_protocol.h`) | same |
| Make | include `scripts/interop_window_graph.mk` from the top makefile after the top makefile defines its own default goal, or port its targets; names are prefixed `window-graph-`; variables use `?=` | same targets (`window-graph-windows`) |
| Coverage | add `'window_graph', 'window_graph_abi'` to `InteropModes` in `tests/av/coverage.py` permanently and drop the sed copy | not applicable |
| CI | job running `window-graph-test`, `window-graph-sanitizers`, `window-graph-coverage`, `window-graph-windows`; needs zig 0.13, gcc 14 with gcov, clang-19 (for `coverage.py`), python3, and `x86_64-w64-mingw32-gcc` for the second native-object check (or set `WgMingwCC` to a tool that exists) | a native Windows job (below) |

The consumer links the graph object once; the observer is compiled into the guest agent for Windows. `PYTHONDONTWRITEBYTECODE=1` and both Zig cache variables must be set in CI so no tracked file is dirtied.

### 8.6 Native runner evidence required before global TODO #10 credit

Codex runs these on its Windows runner; none exists yet, and none is claimed.

1. Both toolchains (`zig cc`, mingw) build the observer and the agent with the exact flags of 7.4 and the binaries start.
2. A native fixture that creates real windows and records what the observer reported: top-level, owned dialog, owned window with WS_EX_APPWINDOW, popup menu (`#32768`), `ComboLBox`, tooltip, child windows (must not export), layered alpha 0 and a color-key layered window, zero-size window, minimized, maximized, fullscreen, a window cloaked by a virtual-desktop switch or a suspended UWP frame (must report Visible clear), lone-surrogate title, 1024-byte title.
3. HWND reuse stress: create and destroy thousands of windows of one class in one process; assert generation uniqueness and that no delayed destroy hint retires a newer window.
4. Clock check: log `GetTickCount` and the WinEvent `dwmsEventTime` of the same events; confirm they share a base and wrap (decides O6) and measure how often a destroy callback's event time equals or precedes the tick sample of the query that admitted the replacement window (the equal-tick and tick-sample-to-read churn cases of 4.8): the result decides whether Codex accepts the destroy-rule deviation of 9.1 item 24.
5. Pre-1607 behaviour is out of scope; confirm `GetDpiForWindow` resolution succeeds on the target build, that `window_observer_create` returns Unsupported (and allocates nothing) when the thread is not PER_MONITOR_AWARE_V2, and that the native table reports physical-pixel rectangles on a mixed-DPI setup when it is.
6. UAC: the real class names of the consent dialog and interim dummy window on the target Windows build, in all three modes; the behaviour of `OpenProcess` and `QueryFullProcessImageNameW` on `consent.exe` from the agent's integrity level; the desktop switch to Winlogon (DesktopInactive, then resync).
7. CoreWindow hosts: the image names and classes of ShellExperienceHost, StartMenuExperienceHost and SearchHost windows on the target build.
8. 64 live windows plus a 65th; 250 ms refresh duration with 64 and 200 top-level windows (record milliseconds); hook delivery under load; observer stop and restart without leaked hooks.
9. A soak run with leak accounting (hooks and handles).
10. The same checks driven through the real session loop with dispatch, capture and slots, not only the fixture.
11. Owner-hidden promotion: an owned dialog whose owner is a tray window, a zero-area window, a cloaked window and a destroyed window; the dialog is exported as Toplevel in each case; the owner becoming visible later reclassifies it; the Reentrant path (a handler that calls the observer from a message dispatched inside EnumWindows).
12. Image fallback: a window of a protected or elevated process whose `OpenProcess` fails reports the basename from the Toolhelp walk with start time 0; no title is read and no message is sent to windows of the agent's own process.

### 8.7 What stays with Codex

Global `TRACKER.md` and `IMPL_DESC.md` edits and the credit decision; `GNUmakefile`, `scripts/interop.mk`, workflows and the permanent `InteropModes` change; the session main loop and its message wait; the wire serialization of events through the frozen codec; the application catalog and icons behind `app_key_lookup`; the slot runtime, capture stop and pin-drain proof; DesktopSwitch and UAC protocol behaviour; Activated state, focus and input; minimum-size queries; scheduling the Windows runner; resolving the open items of section 9; review, rebase and merge.

## 9. Decision Log & Open Findings

### 9.1 Decisions (binding for this slice)

1. **NOACTIVATE binds to the style clause only.** The header sentence "WS_POPUP and owned and caption bits not both set and ex-style WS_EX_NOACTIVATE clear" is the menu style clause; the two class matches (`#32768`, `ComboLBox`) are unconditional. Reason: a real popup menu is frequently created with WS_EX_NOACTIVATE; reading the exclusion over the class matches would classify real menus as tooltips and send them to the host as the wrong role. Rows `cls_s4_noactivate_binding_difference` pin both readings' difference.
2. **Visible already means shown and not DWM-cloaked.** The adapter clears the Visible bit for cloaked windows, so the graph has no cloak flag and no cloak reason. Reason: `IsWindowVisible` stays true for windows on other virtual desktops and suspended UWP frames, which are not on screen; exporting them would create host windows for nothing. A cloak flips Visible without a show or hide event, so the observer installs a third hook for EVENT_OBJECT_CLOAKED/UNCLOAKED (item 23). This extends the literal "visible" of IMPL_DESC 4.4 and is flagged for Codex review.
3. **App keys follow the frozen codec rule.** 3 to 200 bytes, prefix `a-` or `p-`, only `[A-Za-z0-9_-]`. Reason: a key accepted by the graph but rejected by the codec would fail later at serialization, after state was published; validating at the single external call keeps the plan atomic.
4. **Forced retirement when the replacement cannot be admitted.** An identity change or reclassification retires the old entry even if the new one hits AtCapacity, GenerationExhausted, LookupFailed or AppKeyInvalid (call returns Ok with disposition Retired; the next offer retries). A transient QueueFull instead changes nothing. Reason: leaving the old generation live under a wrong identity or role is worse than a short gap.
5. **Retiring entries coexist per HWND, keyed by exact (window, generation).** `find_live` sees only Active; destroy hints and offers never touch Retiring entries; `complete_retire` names the exact pair; the new incarnation of a recycled HWND is admitted while the old one is quarantined. Reason: the old surface's capture and pins are still alive and must stay accounted for, and a late completion must not free the new entry.
6. **Destroy hint rule.** A hint retires the live entry only if its stamp is greater than the admission stamp and its unwrapped 64-bit event time is not strictly older than the unwrapped tick of the admission facts (plain `<` on 64-bit values); equal ticks count as destroyed. A deferred record is dropped by the same stamp clause plus the tick clause on its newest facts. Reason: a hint older than the admitting facts describes an earlier incarnation; equal ticks cannot be ordered, and wrongly retiring costs one re-announce while wrongly keeping leaves a stale surface. Destroyed-hint authority comes from `EVENT_OBJECT_DESTROY` only (or the query result Gone); hide, show, name, location, parent-change and cloak hints always trigger a requery. The deviation from the handoff wording is item 24.
7. **No SetProp identity markers.** Reason: they mutate foreign windows, can clash with unrelated properties, must be removed before WM_NCDESTROY, cost atoms and are blocked by UIPI for elevated windows. Continuity rests on the identity tuple plus rule 6.
8. **Desktop switch suspends, it does not retire.** With the input desktop not Default, the observer's pump and refresh process nothing (a snapshot with OnInputDesktop clear would be classified Suspended by the graph, but the observer never produces one), keeping hints. Reason: the windows on the Default desktop still exist, and retiring them on every UAC prompt would tear down all host surfaces; the observer checks the desktop once per pump or refresh call, processes nothing while it is not Default, latches a full refresh and resyncs when Default returns. The graph's Suspended disposition exists for completeness but the observer never produces a snapshot with OnInputDesktop clear, and there is no per-window desktop detection.
9. **Stamps are owned by the observer; the graph exposes `stamp_high_water`.** Reason: the observer is the only party that orders hints and queries; a replacement observer must start above the old one or the graph correctly rejects its facts as stale.
10. **Hints carry no facts.** The observer requeries every window and treats a Failed query as unknown; only an affirmative Gone or a valid destroy hint retires.
11. **One heap allocation, fixed storage, no allocation in operations.** Reason: bounded memory, trivial ownership (create/destroy), and a complete allocator-failure sweep.
12. **Reclassification is retire plus a new generation, reusing the stored app key.** Parent and role changes are never sent as Update. Reason: they need a new host surface; reusing the key avoids a second lookup that could fail halfway.
13. **Control events are never dropped.** Every mutating call checks ring room for its whole plan first (QueueFull, no mutation). Coalescing is allowed only for Announce and Update of one identity, never across a RetireIntent.
14. **Deferred records carry no generation and no capacity.** The key lookup happens at first deferral. Eviction is oldest-first and evicted windows return through the 250 ms refresh.
15. **Titles.** The observer converts UTF-16 to UTF-8 without strict validation so lone surrogates become U+FFFD, and cuts at a code-point boundary; the graph is the trust boundary and rejects invalid UTF-8 and over-length fields. Class and image names use strict conversion and fall back to ClassTruncated or ImageUnknown.
16. **`complete_retire` check order is Stale, then NotRetiring, then ProofIncomplete** (the header). Reason: a stale or unknown pair must never reveal or consume anything, whatever proof accompanies it.
17. **Time domain.** `captured_ms` and `event_time_ms` stay 32-bit wire values (the Win32 tick, wrapping every 2^32 ms), but the graph unwraps every tick against an internal 64-bit `tick_base` (starting at 2^32, advanced to the unwrapped value by every Ok call that carries a tick, never moving backwards) and stores 64-bit ticks in its records; all comparisons are plain 64-bit `<` on unwrapped values. Reason: a 32-bit signed-difference compare misreads spans above 2^31 ms (24.8 days) as negative, so a long-lived window would ignore a real destroy hint forever. Unwrapping is exact while an Ok call carrying a tick happens at least once per 2^31 ms; the 250 ms refresh cadence guarantees this by orders of magnitude, and after a longer silence the next refresh corrects any aliasing.
18. **One started observer per process.** Reason: WinEvent procedures carry no context; a process-wide atomic slot names the active observer.
19. **Iconic windows report restored size** (from window placement; coordinates in workspace space are informational); non-iconic use DWM extended frame bounds with a window-rect fallback.
20. **OwnerHidden promotion (refinement of IMPL_DESC 4.4, needs Codex sign-off).** The observer computes a new fact flag, `InteropWindowFactOwnerHidden` (bit 12, mask 0x1FFF), for a window with a foreign `owner_root`: it queries the owner (second `query_facts`), classifies it with `interop_window_graph_classify` and the config's `uac_mode`, and sets the flag iff the owner is gone or its verdict is NotExported. The graph then treats the window as not owned in steps 4 to 7 and for the ImePreedit parent: a Menu stays Menu (parent 0), popup/owned-toolwindow rules use owned = false, an owned non-APPWINDOW window becomes Toplevel with parent 0. Reason: without it a Dialog, Menu or Tooltip whose owner is never announced (tray, zero area, transparent, excluded, destroyed) would sit in the deferred set forever (the first open finding of the earlier draft), be re-offered every 250 ms and crowd the 64 records, and the user would never see the window. The 4.4 table is silent about never-exportable owners, so this is a refinement, not a literal reading. A flag change that changes role or parent goes through the Reclassified path (one retire plus one announce). The exact one-line disable: the observer never sets OwnerHidden (delete the owner sub-query result handling in `query_one`); nothing else in the graph changes, and the graph classifier and `classify` export remain correct. This is why the decision is cheap to reverse.
21. **Refresh "seen" rule.** After the guards, `refresh_offer` marks the offered window seen for EVERY outcome (including every error status and QueueFull); a plain `observe` marks on Ok only; `refresh_keep` marks without facts; `refresh_unseen` lists the still-unseen Active windows so the observer can requery each individually before the commit; a refresh with failed queries still commits because failed windows are kept. Reason: the old rule (mark on Ok only) let one persistently failing or rejected window either block the commit forever or be retired by it; neither is acceptable. Absence from the enumeration alone is never the sole evidence of destruction; only Gone, a valid destroy hint, or an unseen window that the individual requery cannot keep retires.
22. **Cascade rules.** Inside `observe`, the identity-change check (retire the live entry) comes BEFORE the cycle check, and a rejected replacement (Cycle, AtCapacity, GenerationExhausted, LookupFailed, AppKeyInvalid) still leaves the retirement done (Ok/Retired). A deferred record whose identity tuple differs from the offered facts is dropped and the window is treated as having no deferred record (fresh lookup, new age). If a reclassified window's new parent is in its own descendant set the replacement is a Cycle (cascade retired, nothing deferred). Descendants are re-deferred only for a Reclassified cause of the same incarnation (they are alive and still owned by that HWND); for every other cause (destroy, exclusion, hide, incarnation change, refresh unseen, invalidate) they retire with ParentRetired and are not re-deferred, returning through their next hint or offer. Leftovers of a capacity-cut flush stay deferred and survive the commit because they carry the seen mark.
23. **Three hooks.** Object events 0x8000..0x800F (adds parent change, so an owner change that has no show or hide is seen), minimize start/end 0x0016..0x0017, and cloaked/uncloaked 0x8017..0x8018. Reason: a DWM cloak or a parent change flips a fact the graph keys on without a show/hide event; the 250 ms refresh would otherwise be the only detector. The hint filter accepts exactly these ranges; the new hints carry no authority (requery only).
24. **Destroy-rule deviation, stated honestly.** The rule of item 6 narrows, but does not literally satisfy, the handoff wording that a hint about an earlier incarnation must never retire a newer one. It can retire and readmit a newer incarnation when the event time equals the admission tick, or when the destroy fell between the observer's `tick_ms` sample and its native read of the admitting facts. The cost is churn bounded by one lost generation per coincidence: the observer requeries after every Destroy hint and re-admits a live window under a new generation, so a window is never permanently lost. This needs Codex acceptance and the native clock check of 8.6 item 4 (measure how often the coincidence happens); if Codex rejects it the alternative is to ignore equal ticks, which can leave a stale surface, a worse failure.
25. **Image-name fallback.** When `OpenProcess` fails, the basename comes from a `CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS)` walk by process id (taken at most once per pump or refresh call, retaken on a process-id miss, handle closed on every path) with `process_start_time` left 0; ImageUnknown only when both fail. Reason: UAC and CoreWindow rules need the image basename and an elevated or protected process otherwise loses it. The known-image-with-start-time-0 versus later known start time flip is an incarnation change (one surface of churn).
26. **No messages to, and no titles from, own-process windows.** The native query never sends a message to any window of the agent's own process and reads no title for it (AgentOwned windows report an empty title, the core forces `title_length` 0; they are excluded anyway except the preedit window). Reason: a send to a window of the thread that is inside the observer would re-enter the observer or deadlock; foreign windows are never sent a message either (no `WM_GETTEXT`, no `WM_GETMINMAXINFO`).
27. **One consistent create behaviour and a busy flag.** `window_observer_create` returns Unsupported when the native table cannot run (no `GetDpiForWindow`, or the thread is not PER_MONITOR_AWARE_V2); every other observer entry point except `note_hint` returns Reentrant while another entry point is active. The native adapter clamps width and height to 16384 (documented; the graph still rejects larger).
28. **Dirty-table reservation and put-back.** One dirty slot is reserved while pump holds a popped key (`note_hint` treats `count + popped_in_flight >= 512` as full), so a put-back always fits; a graph QueueFull puts the key back at the tail, other stops at the head. Reason: a put-back that could not fit would silently lose a key.
29. **Coordinator contract.** After GraphQueueFull the loop calls pump/refresh again only after a drain delivered at least one event, otherwise it waits for the transport's writable event; list_retiring is reconciled only for pairs whose RetireIntent was delivered (or after a drain that emptied the ring) (8.3). The feature `TRACKER.md` is updated in every commit that changes progress.

### 9.2 Open findings and risks (stated as they stand)

Identifiers are stable across drafts: the first item of the earlier draft (owned windows whose owner is never exported) moved to decision 20 and its identifier is not reused, so the list starts at O2.

- **O2. Native-mode hidden UAC window.** In mode Native the consent window is counted (`uac_native_hidden`) but its handle is not exposed, so the coordinator cannot read the prompt through this API. A separate channel is needed.
- **O3. Quarantine can pin capacity.** If the coordinator never completes a retirement, up to 64 Retiring entries keep occupying all slots and every admission returns AtCapacity. Deliberate (releasing without proof is unsafe); the mitigation is a coordinator watchdog that escalates to epoch roll.
- **O4. Hint-overflow recovery churns every generation.** `invalidate_all` then full refresh retires and re-announces every window under new generations, recreating all host surfaces. Chosen because lost destroy hints cannot be told apart from live windows; a cheaper reconcile (retire only unseen) is not safe against O5.
- **O5. Identical-tuple recycled HWND with a lost destroy hint is undetectable.** If the OS drops the destroy event, the HWND is reused with the same process, thread, start time and class, and no overflow occurs, the graph sees an unchanged window.
- **O6. `GetTickCount` versus `dwmsEventTime` base is unverified.** Documentation does not state the base; the destroy rule depends on them being comparable. Needs the native clock check of 8.6.
- **O7. Minimum size is not queried** (`min_width` and `min_height` stay 0) because `WM_GETMINMAXINFO` is a cross-process send. The Update `MinSize` bit is never produced by the Win32 adapter.
- **O8. `UpdateLayeredWindow` windows are treated as opaque** because `GetLayeredWindowAttributes` fails for them; a fully transparent per-pixel window can be exported.
- **O9. Elevated and UIAccess denial behaviour is unverified, with a sharp consequence.** If the agent cannot read `consent.exe`'s image, the window is reported ImageUnknown and no UAC rule matches, so the interim dummy window and the credential window would be exported as ordinary Toplevel windows in every mode. Whether consent.exe is visible on the Default desktop at all depends on the secure-desktop policy. Needs the native runs of 8.6 item 6; no UIAccess or secure-desktop claim is made.
- **O10. No Windows execution was possible.** Every native claim is compile and link only.
- **O11. WinEvent delivery is best effort.** Hints can be dropped or delayed under load; the 250 ms refresh bounds staleness. A permanently Failed query no longer blocks refresh commits: the window is kept with `refresh_keep` and is never updated (a real Gone or a destroy hint still retires it). `EnumWindows` beyond 2048 windows makes every refresh fail closed (EnumFailed).
- **O12. `EnumWindows` does not list UWP CoreWindow top-levels on Windows 8 and later** (research, documentation), so the CoreWindow exclusion matters mainly for windows reached through hints; the frame windows that are enumerated are exported like any other.
- **O13. Observer paths without a test seam:** counter saturation in C and the NoMemory branch (until the `--wrap` delta lands), see 7.5.
- **O14. Header silence:** the behaviour of `window_observer_note_hint` when the logical clock is exhausted is not specified; the implementation must document it, and `wo_stamp_exhaustion` pins it.
- **O15. `GetDpiForWindow` and PER_MONITOR_AWARE_V2.** `GetDpiForWindow` is resolved at run time (Windows 10 1607 or later) and the owner thread must be PER_MONITOR_AWARE_V2; `window_observer_create` returns Unsupported (15) otherwise and allocates nothing, so nothing is ever exported with an invented 96 dpi. The requirement is on the guest agent's manifest or thread setup (8.2); whether the target agent can run V2 is a Codex decision.
- **O16. No Active-list call.** The graph cannot enumerate Active entries; the coordinator must track them from the event stream (8.3).
- **O17. Cloak detection relies on `DwmGetWindowAttribute(DWMWA_CLOAKED)`**; if the call fails the adapter reports Failed (never Visible), so a DWM-disabled session never exports.

### 9.3 Differences between the earlier design note and the committed headers (header wins)

1. The design note had a Cloaked fact flag and a NotExported(Cloaked) reason; the header has neither (Visible includes not-cloaked).
2. The design note listed ProofIncomplete before the entry lookup in `complete_retire`; the header orders Stale, NotRetiring, ProofIncomplete.
3. The design note made a missing `GetDpiForWindow` a create failure with no dedicated status; the amended header returns `Unsupported` (15) from create for a missing `GetDpiForWindow` or a thread that is not PER_MONITOR_AWARE_V2 (O15).
4. The design note listed an app-key check of "empty, over 200, invalid UTF-8, NUL"; the header applies the full frozen codec rule.
5. The committed makefile has no allocation seam for the observer and no test access for observer counters (7.3, 7.5).
- **O18. Application key is never re-looked-up.** The key is fetched once at first admission or deferral and reused for every later generation of the same window (reclassification, re-admission from a deferred record). If the catalog entry for a process changes (renamed application, new icon key), the surface keeps the old key until the window is retired by some other cause. Codex decides whether to re-lookup on Reclassified or periodically.
- **O19. Popup-role parents for Dialog and Toplevel.** Dialog parent is `owner_root` and Toplevel has no parent. The host maps parent to an xdg_toplevel parent or an xdg_popup depending on the role; whether a Dialog that is really a native popup (a custom drop-down using a plain owned WS_POPUP with a caption-like frame) should be an xdg_popup is a host mapping question that this slice cannot answer.
- **O20. Opaque per-pixel and click-through overlays.** A window using `UpdateLayeredWindow` (per-pixel alpha) is treated as opaque because `GetLayeredWindowAttributes` fails for it, and a `WS_EX_TRANSPARENT` click-through overlay (a screen-capture border, a game overlay) is exported like any other window: the host will show a possibly empty opaque rectangle. A later slice needs either a per-pixel alpha probe or an overlay heuristic.
- **O21. Known-versus-0 `process_start_time` flip churn.** An image whose `OpenProcess` alternates between success and failure (an elevation boundary, a sporadic access error) flips the identity tuple between a known start time and 0, which the graph treats as an incarnation change: each flip costs one surface retire plus one announce. The rule is conservative on purpose (a stale surface is worse). If flips are common on the target, Codex can pin the first-known start time per HWND in the observer.
- **O22. Refresh cost.** One refresh performs up to 2048 queries from the enumeration (each with an optional owner sub-query and, at worst, the Toolhelp walk) plus up to 64 individual requeries; a desktop with hundreds of hidden windows makes every 250 ms refresh expensive. Proposal for Codex: add an `IsWindowVisible` plus class-name pre-filter in the native `enum_top_level_windows` callback (skip windows that are not visible and whose class is a known excluded class) before the expensive per-window query; measure first (8.6 item 8).
- **O23. Parent change is a retire, not an Update.** `WindowUpdate` could carry a parent, but this design retires the window and announces a new generation on any parent or role change (decision 12). The cost is one host surface per owner change. Codex may extend the protocol update to carry a parent; until then the retire path stays.
6. The earlier committed text of this document (and the design note) listed 18 graph functions, a 13th observer status that aborted a refresh on any failed query, a pair of hooks (0x8000..0x800C and 0x0016..0x0017), a 12-bit fact mask, a 32-bit `time_before` destroy comparison, "mark seen on Ok only", a `reserved0` field in `window_observer_config_t`, and descendants that were always re-deferred. The amended headers have 21 functions (adds `classify`, `refresh_keep`, `refresh_unseen`), no failed-query status (StampExhausted 13, Reentrant 14, Unsupported 15), three hooks, a 13-bit mask with OwnerHidden, 64-bit unwrapped ticks, the offer-marks-every-outcome seen rule, `uac_mode` at config offset 36, and re-deferral only for same-incarnation Reclassified. Sections 1 to 9 above follow the amended headers.
