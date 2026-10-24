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
| Native image-path buffer (`native_query_image`, `_WIN32` only) | stack `WCHAR[260]` first; only on `ERROR_INSUFFICIENT_BUFFER` one `malloc` of 32768 WCHAR (64 KiB) | `native_query_image`, for one `query_facts` call | `free` before the call returns, on every path (the same function frees it; a failed allocation returns Failed for that window) | Never taken for a window that is not visible (it reads no image path) and never taken for a path up to 259 characters. Not exercised on Linux. |
| `interop_window_facts_t` | caller stack or caller storage | caller | caller | Graph copies fields it keeps; nothing borrowed survives the call. |
| Events, observation, live, stats, diagnostic outputs | caller storage | caller | caller | Graph writes a staged local and copies it only on Ok. |
| App key bytes | inline `[200]u8` in `record_t` | graph | with `graph_t` | The callback writes into a graph-provided buffer; no pointer is retained. |
| WinEvent hook tokens | OS | observer | `stop` or `destroy` | Removed in reverse install order. |

The graph never allocates after create, and neither does the portable observer core (`pump`, `refresh`, the facts structs live on the stack). The native `query_facts` allocates in exactly one bounded case, listed in the table above: one 64 KiB buffer for a single query of a visible window whose image path exceeds 259 characters, freed before the query returns. The Toolhelp snapshot is a kernel handle, not heap memory: at most one is open at a time and at most two are created per outermost `pump` or `refresh` call, and `native_call_end` closes it on every path. The observer never owns, retires or frees graph state; the graph owns no
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
and one dirty slot is reserved for its put-back), `uac_mode` (u32 from the config; the owner sub-query is folded into the query counters, no extra counter exists), `dirty[512]` plus `dirty_count` (0..512), `next_stamp` (u64, below `UINT64_MAX` while valid; `UINT64_MAX` is the exhaustion sentinel), `preedit_window` (u64), `own_process_id` (u32, nonzero), `overflow` and `refresh_requested` and
`desktop_active` (0/1 latches and gauge), `last_refresh_attempt_ms` (u64, monotonic, stamped before the desktop check) with `have_refresh_attempt`, `latch_ms` (u64, monotonic, set whenever the full-refresh latch is set) with `latch_unconsumed` (0/1: set by every latch event, cleared by the next refresh attempt; it, not a timestamp comparison, decides the latch term of `refresh_due`),
`enum_buffer[2048]` (u64 HWNDs), `stats` (176-byte `window_observer_stats_t` plus `last_graph_status`). The single
`calloc` block is `sizeof(window_observer_t)`: dirty 24,576 + enum buffer 16,384 + stats/api/scalars under 1 KiB,
about 41.5 KB. Two process-wide atomics: `static _Atomic(window_observer_t *) g_active_observer` (initially NULL, non-owning) and `static _Atomic(uint32_t) g_hints_in_flight` (initially 0, the in-flight counter of `note_hint`).

Dirty table rules. The table is an insertion-ordered array; index 0 is the oldest first-seen key. `dirty_add`
scans for the window (at most 512 compares): found -> `kinds |=`, `last_stamp` = newest, if Destroy then
`destroy_stamp` and `destroy_event_ms` = latest, `hints_coalesced` +1; absent and count < 512 -> append, order
preserved; absent and full -> `overflow = 1`, `hint_overflows` +1, no entry (the lost key is recovered by
one full refresh in `pump`, see 4.11). "Full" counts the reserved slot: while pump holds a popped key (`popped_in_flight`
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
removed in reverse order, retaining each token whose removal fails. A separate atomic
`g_lifecycle_observer` reservation blocks a second start from installation until
all owned hooks are removed and state is Stopped. `g_active_observer` is only the
callback publication pointer: clearing it does not release the lifecycle reservation.
The hint filter accepts exactly these three hook ranges.

State machine: Created/Stopped may start; success enters Started. A failed install
returns HookFailed after rollback: complete rollback enters Stopped and releases
the reservation; failed rollback enters Failed with tokens/reservation retained.
Started or Failed may stop/destroy. Stop clears callback publication, waits for
in-flight hints, attempts every retained hook in reverse order and clears only
successful removals. Any failed removal returns UnhookFailed with state Failed,
tokens and reservation retained. A later stop/destroy retries only those tokens.
After every token is removed, state becomes Stopped and reservation is released.
Destroy frees/NULLs its slot only after successful teardown; on failure it resets
busy and leaves owner/slot intact. Created/Stopped stop is idempotent. Failed
start/pump/refresh remain BadState; Failed stop/destroy are retry operations.

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
1. A NULL, misaligned or wrapping output slot returns InvalidArgument unchanged. Before any write, reject overlap with the raw nonwrapping config extent (even if config is misaligned), leaving both regions unchanged. Only after establishing a valid disjoint slot write `*out_graph = NULL`; later configuration/allocation failures leave it NULL.
2. `config == NULL`, `session_epoch == 0`, `uac_mode > InteropWindowUacNative`, `reserved0 != 0` or `app_key_lookup == NULL` returns InvalidArgument. `now_ms == NULL` selects the default monotonic clock: a `std.time.Timer` started at create, read in milliseconds. If the timer cannot be started the clock reads 0 forever, which counts as zero elapsed time everywhere (no tick estimate drift, and the capacity diagnostic samples once and is then rate-limited by the frozen clock).
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

1. Validate nonnull observer, creating thread and busy/state gates. Only Created
   or Stopped may start; Started/Failed reject without changing ownership.
2. Atomically reserve g_lifecycle_observer from NULL to this owner. AlreadyActive
   means another lifecycle is installing/started/stopping/retaining failed hooks.
   Then publish g_active_observer; an inconsistent occupied callback slot causes
   reservation rollback and AlreadyActive without touching the other owner.
3. Clear bounded hint/refresh state. Install object, system and cloak hooks in
   order, immediately storing every nonzero returned token in observer->hooks.
   A hint during installation sees a non-Started owner and is ignored; the first
   full refresh covers it. The caller holds busy throughout installation.
4. On any failed install, clear callback publication and wait for in-flight hints.
   Attempt all owned hook removals in reverse order. Clear only successful tokens.
   If any token remains, enter Failed and retain lifecycle reservation; otherwise
   enter Stopped and release it. Return HookFailed for the acquisition failure.
   The caller may stop/destroy to retry cleanup; no token is forgotten.
5. On complete installation enter Started, set the full-refresh latch and reset
   attempt cadence. Return Ok. No second observer can start while this lifecycle
   owns the reservation, even when callback publication is temporarily NULL.

### 4.3 Steady state: hint to pump

Hint path, `window_observer_note_hint(event, window, object_id, child_id, event_thread_id, event_time_ms)`, run by the hook procedure on the owner thread:
1. `atomic_fetch_add(&g_hints_in_flight, 1)` (seq_cst), then `obs = atomic_load(&g_active_observer, acquire)`; NULL: decrement and return, no counter exists to bump. Every later return path decrements the in-flight counter (release); `stop`, a failed start and `destroy` clear the slot and then wait for the counter to reach zero, so the observer cannot be freed under a running hint. The busy flag is neither read nor written (a hint may arrive while the owner thread is inside another observer call).
2. `api->current_thread_id(ctx) != obs->owner_thread`: `atomic_fetch_add(&obs->hints_wrong_thread, 1, relaxed)` and return. Nothing else is read or written.
3. `obs->state != Started`: return silently (hint after stop).
4. Filter: `object_id != 0`, `child_id != 0`, `window == 0`, or `event` outside [0x8000, 0x800F], [0x0016, 0x0017] and [0x8017, 0x8018]: `hints_filtered++`, return. The three ranges are the three hooks of 4.2; 0x800D and 0x800E lie inside the first range and are accepted as Other.
5. `stamp = stamp_next()`. If it is exhausted (returns 0) the hint cannot be ordered: latch `overflow = 1`, `hint_overflows++`, return (the recovery of 4.11 is a full refresh, which itself then reports StampExhausted).
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
   - d. `query_one`: zero a local `interop_window_facts_t`, call `api->query_facts(ctx, window, &facts)`. Hints that fire during this call find no entry for `window` and create a new one at the tail. When the result is Exists and `facts.owner_root` is neither 0 nor `window`, and the window's own Visible flag is set, the core runs the owner sub-query (4.4 "Owner-hidden computation"; a window with Visible clear is classified NotVisible before OwnerHidden is consulted, so it skips the sub-query): a second `api->query_facts(ctx, owner_root, &owner_facts)` with the same overlay, `interop_window_graph_classify(&owner_facts, uac_mode, &verdict)` and the rule Exists -> OwnerHidden iff verdict NotExported; Gone -> OwnerHidden; Failed (or a classify status other than Ok) -> the window's query is Failed. No message is ever sent to an own-process window.
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
4. `enum_top_level_windows(ctx, enum_buffer, 2048, &count)`: Failed returns EnumFailed before any graph call (`refresh_requested` unchanged); `count > 2048` is treated as Failed. Truncated (more than `WindowObserverMaxEnumerated` = 2048 top-level windows, a hard limit) does NOT stop the refresh: steps 5 to 7 run on the partial list (listed windows are offered, so admissions and updates flow; the unseen Active windows are requeried one by one, so Gone notes and updates flow), and step 8 is replaced by `refresh_abort` (`refreshes_aborted++`) and a return of EnumFailed; the refresh is never committed, nothing is retired by absence, and `refresh_requested` is not cleared, so the cadence repeats it. A desktop that stays above 2048 windows therefore never commits: windows past the 2048th in z-order are admitted only by hints or offers of their own and retire only through a destroy hint or a Gone result. A complete list of any length 0..2048 is accepted; an empty list therefore retires everything at commit, which is correct for an empty desktop. The enumeration buffer is touched only by this outermost call (the busy flag keeps a nested call out).
5. `refresh_begin`. Ok continues. RefreshActive (left open by an earlier commit that returned QueueFull): `refresh_abort`, `refreshes_aborted++`, `refresh_begin` once more. Any other status: GraphError. `refreshes_started++`.
6. Pass 1, for each enumerated handle in order (no deduplication; a duplicate is re-queried and re-offered, and is Unchanged the second time; a handle of 0 is skipped): `fresh = stamp_next()`, `tick = tick_ms`, `query_one` (pump step 5d, including the owner sub-query).
   - Exists: set stamp, captured_ms and flags as in pump step 5e, then `refresh_offer`. Ok (any disposition): `uac_native_hidden` as in the pump; a Suspended disposition (defensive) aborts and returns DesktopInactive. QueueFull: `refresh_abort`, `refreshes_aborted++`, return GraphQueueFull. Reentrant: abort, return Reentrant. Drop-class statuses (InvalidFacts, StaleFacts, Cycle, AtCapacity, GenerationExhausted, LookupFailed, AppKeyInvalid, Stale): `graph_errors++`, `last_graph_status` set, the refresh continues; `refresh_offer` has already marked the window seen for this outcome, so a rejected offer never retires the window at commit. Call-level statuses (StaleEpoch, WrongThread, InvalidArgument, NoRefresh, ...): abort, GraphError.
   - Gone: the window is skipped and stays unseen (pass 2 requeries it if it is an Active window).
   - Failed: `queries_failed++`, `refresh_keep(window)` (Ok or None continue; other statuses as above), continue with the next handle. A failed query never aborts the refresh and never retires a window.
7. Pass 2, reconcile the would-be victims: `refresh_unseen(graph, epoch, windows[64], 64, &n)` lists the Active windows still unseen (a window that was hinted and admitted but is absent from the enumeration, a window whose enumeration entry was Gone, ...). For each listed window draw `fresh`, `tick` and requery it individually: Exists -> `refresh_offer` (status handling as in pass 1); Gone -> `note_destroyed(window, fresh, tick)` (`destroys_noted++`; if the destroy rule rejects the hint the window stays unseen and the commit retires it with RefreshUnseen); Failed -> `queries_failed++`, `refresh_keep(window)`. This keeps a window that is alive but not in the enumeration (a transient enumeration gap, or a hinted window the OS had not enumerated yet) and makes absence alone never the sole evidence of destruction.
8. `refresh_commit`. Ok: `refreshes_committed++`, `refresh_requested = 0`, return Ok, even when queries failed (every failed window was kept). QueueFull: the refresh stays open in the graph, return GraphQueueFull (`refresh_requested` stays). Other: best-effort `refresh_abort`, GraphError.
Total queries per call are bounded by 2048 plus 64, each with at most one owner sub-query (none for a window that is not visible). Graph side seen marking: `refresh_begin` clears `seen` on all 64 entries and 64 deferred records and sets `refresh_open`. After the common guards pass, `refresh_offer` marks the offered window (when nonzero) seen for EVERY outcome, including every error status and QueueFull (offered means alive for this refresh), before the plan; a new generation is born seen, and a deferral creates a seen record. A plain `observe` during an open refresh marks seen on Ok only. `refresh_keep` marks the Active entry and the deferred record of the window (Ok, None if neither exists, NoRefresh if no refresh is open). An entry created by a flush copies the `seen` bit of the deferred record it came from; a child re-deferred by a Reclassified cascade copies the bit of its entry; flush leftovers cut off by capacity stay deferred and keep their bit, so they survive the commit. `refresh_commit` retires every Active entry with `seen == false` (reason RefreshUnseen) and then drops every deferred record with `seen == false`.

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
5. Cascade cycle. The new-parent chain is followed through deferred records to its terminal Active entry (at most 65 steps). If P's new owner is its own descendant (say P is offered with `owner_root = W200`, a current child of P), or the chain from the new parent runs through deferred records and ends at an Active entry that is in P's cascade set, the new parent lies in P's cascade set, so the replacement is forced to the retire-only outcome and treated as Cycle (no deferral): P retires with reason Reclassified, C1 and C2 retire with ParentRetired, nothing is deferred or re-deferred (the pair would otherwise wait for each other forever), and the call returns Ok with disposition Retired; `rejected_cycle++` is counted. A chain that ends at an Active entry outside the cascade still defers normally. The children return by their next offers: C1 (owner P) is offered, finds P absent (Retiring) and defers; P is offered with its own parent chain and, if it is now admissible, flushes C1.
6. Shortfall variants. If exactly one slot is free when the call starts (`free_slots = 1`), the plan admits P' (g6) but cannot admit the descendants: they are inserted into the deferred set in ascending original generation (C1 then C2, new `seq` values), `rejected_at_capacity += 2`, `needed = 3 + 1 = 4`, and the call still returns Reclassified with `entries_admitted = 1`, `events_queued = 4`; each deferred descendant is admitted by its own next offer (the next refresh, at most 250 ms later, because its parent W100 is then Active) once a slot is free, so the coordinator must free slots with `complete_retire`. If `free_slots = 0`, the plan degrades to retire-only: disposition Retired, `retired_generation = 3`, `entries_retired = 3`, `needed = 3`, the descendants are re-deferred, `rejected_at_capacity += 1` for the primary, and the next offer of W100 retries the admission.

### 4.8 Destroy followed by HWND reuse: the `note_destroyed` rule

`note_destroyed(epoch, window, hint_stamp, event_time_ms)`:
1. `enter`; `window == 0` or `hint_stamp == 0` or NULL output -> InvalidArgument. Read the graph clock `now_ms` once, estimate the current tick as `est = tick_base + (now_ms - tick_clock_at_base)` (elapsed counts as 0 when the clock is absent, stuck or goes backwards), and compute `e64 = est + (int64)(int32)(event_time_ms - (u32)est)` (the unwrap of 3.5 against the estimate, not against the stale base); `tick_base` is not changed yet.
2. Active entry L for `window`: retire iff `hint_stamp > L.admit_stamp` AND NOT `e64 < L.admit_captured_ms`, a plain `<` on two unwrapped 64-bit values (`admit_captured_ms` is the unwrapped tick stored when L was admitted). There is no 32-bit comparison anywhere. Both clauses are needed: the stamp clause rejects hints that reached the observer before the facts that admitted L were drawn; the tick clause rejects hints that reached the observer afterwards but describe an event older than those facts.
3. Deferred record D for `window`: dropped iff `hint_stamp > D.last_stamp` AND NOT `e64 < D.last_captured_ms` (`deferred_dropped = 1`; `last_captured_ms` is the unwrapped tick of the newest facts accepted for D). A Retiring entry is never touched.
4. Retirement uses `retire_cascade` (reason Destroyed for L, ParentRetired for descendants, descendants never re-deferred), pre-checked as in 4.5 (`needed = retire_count`); QueueFull changes nothing (including `tick_base`). Disposition Retired when anything retired, else Unchanged. On Ok `stamp_high_water` advances, `tick_base = max(tick_base, e64)` and the anchor `tick_clock_at_base` is re-set to the `now_ms` read at the start of the call; a call that does not return Ok changes neither field.

Honest scope of the rule (deviation from the handoff wording). The handoff asks that a destroy hint about an earlier incarnation never retires a newer one. The rule narrows this and does not literally satisfy it. A newer incarnation is retired and then readmitted when (a) the ticks are equal (they are ambiguous, so the rule treats them as destroyed) or (b) the destroy fell between the observer's `tick_ms` sample and its native read of the facts that admitted L (the event time is then not older than `captured_ms`, although the facts already describe the new window). The cost is bounded churn: at most one lost generation per such coincidence. The observer requeries the window right after a Destroy hint (4.3 step b then c), finds it alive and admits it under a new generation, so a live window is never permanently lost; the only visible effect is one surface retire plus one announce. A refresh cannot create unbounded churn because a hint is consumed by the call that applies it. This deviation needs Codex acceptance, and the claim that `GetTickCount` and WinEvent `dwmsEventTime` share a base must be checked on a native runner (8.6); this is recorded in 9.1 and 9.2.

Worked numbers (all unwrapped values are 64-bit; `tick_base` starts at 2^32 = 4294967296). L = W500 admitted as g9 with `admit_stamp = 100`, observer tick 5000: `unwrap_tick(2^32, 5000)` = 2^32 + 5000, so `admit_captured_ms = 4294972296` and `tick_base = 4294972296` after the Ok call.
- Stale destroy ignored. The old window with the same HWND was destroyed at tick 4990; the new window was created at 4995 and admitted from a query sampled at tick 5000. The OS delivered the old destroy callback late, so `note_hint` stamped it 101 (event time 4990). `note_destroyed(W500, 101, 4990)`: 101 > 100 holds; `e64 = 4294972286 < 4294972296`, so it is older than L's facts: ignored, Unchanged, no event, `tick_base` unchanged (max keeps the larger value), `destroys_noted` still counts the call. A destroy stamped 99 (arrived before the query) fails the stamp clause: ignored.
- Real destroy applied. The window is destroyed at tick 5200, callback stamped 110: 110 > 100 and `e64 = 4294972496 >= 4294972296`: L retires (RetireIntent g9, reason Destroyed); `tick_base = 4294972496`. The pump then requeries with a fresh stamp 111: Exists means the HWND was reused again, `observe` finds no Active L and admits g10 (fresh lookup); Gone means `note_destroyed(W500, 111, tick)` finds no Active entry and returns Unchanged.
- Equal ticks, conservative churn. Destroy event time 5000, stamp 101: `e64` equals `admit_captured_ms`, `<` is false, so L retires even if the destroy actually belonged to the previous incarnation. The same pump call then requeries; the new window is admitted as g10. Cost: one surface churn. The alternative, ignoring equal ticks, could leave a stale surface, which the design forbids.
- Wrap of the 32-bit counter. W501 admitted at wire tick `0xFFFFFFF0` while `tick_base = 2^32 + 0xFFFFFFE0`: d = +16, `admit_captured_ms = 2^33 - 16` and `tick_base` follows. Destroy event time `0x00000010`: d = (0x10 - 0xFFFFFFF0) mod 2^32 = 32, `e64 = 2^33 + 16 >= admit`: destroyed. Event time `0xFFFFFFE0`: d = -16, `e64 = 2^33 - 32 < admit`: ignored.
- Long-lived windows (the case that a 32-bit compare gets wrong). W600 is admitted at tick 1000 (`admit_captured_ms = 2^32 + 1000`) and then only re-offered, unchanged, by the 250 ms refresh; every Ok refresh call carries a tick, raises `tick_base` and re-anchors the estimate, so the base follows the clock. After 30 days (2592000000 ms, more than 2^31 = 2147483648) a destroy with event time 2592001000 unwraps to `2^32 + 2592001000 >= admit`: retired. A 32-bit `time_before` would read the 30-day difference as negative and ignore a real destroy forever. After 60 days (5184000000 ms, the 32-bit counter has wrapped once: wire value 889032704) the unwrapped value is `2^32 + 5184001000`: retired. The ZG tests `destroy hints still work 30 and 60 simulated days after the admission` and `after a silence longer than 2^31 ms of the graph clock, current-tick offers repair the base and a genuine destroy hint retires` drive this with simulated ticks and an injected clock. Silence longer than 2^31 ms: because every tick-carrying call first adds the graph-clock time elapsed since the anchor to the base (`est`), a destroy hint or Gone note after a long silence (a locked guest, a long DesktopInactive stretch) is unwrapped against the estimated current tick and keeps working (ZG `the very first tick-carrying call after a long silence is unwrapped by the clock estimate`). Aliasing by a multiple of 2^32 ms remains possible only when the graph clock did not advance during the silence (a frozen or absent clock, ZG `with a clock that does not advance, a silence longer than half the range still aliases`), or drifts from the tick domain by more than 2^31 ms; the next refresh then corrects the outcome (RefreshUnseen retires a vanished window, a requery re-admits a live one).
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
3. The next `pump` or `refresh`: `query_input_desktop` returns Other or Denied. The observer sets `desktop_active = 0`, `refresh_requested = 1`, `pumps_desktop_inactive++`, stamps `last_refresh_attempt_ms`/`latch_ms` from one clock read and returns DesktopInactive. The graph is not called, nothing is retired, no event is produced; the dirty table keeps its entries and may latch `overflow` if more than 512 distinct windows are hinted meanwhile. `next_timeout_ms` does not return 0 for a hint backlog while `desktop_active == 0`, so the loop does not spin on it; the cadence (worked numbers in 4.4) re-polls the desktop. The refresh latch is a timestamp pair plus the `latch_unconsumed` flag: the DesktopInactive attempt stamps `last_refresh_attempt_ms` and `latch_ms` from the same clock read, so `refresh_due` (true iff Started and (no attempt yet, or at least 250 ms since the last attempt, or latched with a latch event not yet consumed by an attempt, tracked by the flag `latch_unconsumed`)) is false right after the refused attempt and becomes true again 250 ms later. A latch therefore forces at most one refresh attempt per latch event, even with a frozen clock.
4. The desktop is checked exactly once per `pump` or `refresh` call, before any query, and the result applies to every window of the call: the core sets `OnInputDesktop` on every snapshot after that single check. The observer does NOT detect a desktop switch per window and the native query does not report `OnInputDesktop`; a switch that happens during a call is seen by the next call's check. If an in-flight `query_facts` returns Failed because the desktop changed under it, the key is retried once and then dropped. A graph Suspended disposition therefore cannot occur; as a purely defensive guard, pump puts the key back and returns DesktopInactive, and refresh aborts and returns DesktopInactive. There is no per-window "Suspended" state in the graph (the graph returns the disposition Suspended only for a snapshot whose OnInputDesktop is clear, which the observer never produces).
5. Desktop returns: the next poll sees Default, `desktop_active = 1`; the pump processes the retained hints (an overflow latch triggers 4.11); the latched refresh runs and its commit retires windows that vanished (RefreshUnseen) and admits new ones. During the absence the graph still reports the old windows as live; no DesktopSwitch message exists in this slice, so the coordinator owns that protocol.

### 4.11 Hint-table overflow and its recovery by a full refresh

1. 512 distinct HWNDs are pending. A hint for a 513th distinct HWND finds no entry and no room: `overflow = 1`, `hint_overflows++`, the hint is lost. Hints for already-present keys still coalesce. Every WinEvent with `idObject == OBJID_WINDOW` and `CHILDID_SELF` is accepted, and Windows raises those for CHILD windows too (create, destroy, show, hide, location change, name change of every control), so closing, opening or resizing one window with hundreds of child controls can overflow the table with hints that concern no top-level window. The portable core cannot tell a child from a top-level window without a Win32 call and the hook procedure may call none, so the hints are not filtered (O24).
2. Next `pump` (desktop Default): `overflow` set. The observer does NOT call `interop_window_graph_invalidate_all`. It increments `invalidations` (the counter now counts overflow recoveries; the name is kept for ABI stability), clears the dirty table (`count = 0`; the destroy information in it is discarded) and `overflow`, sets `refresh_requested = 1`, reads the clock once for `latch_ms` and the attempt stamp (so the recovery refresh consumes its own latch event, `latch_unconsumed = 0`) and runs the full refresh of 4.4 (steps 4 to 8, with the Truncated rule) inside the same call.
3. What the refresh does to the exported windows: every enumerated window is queried and offered; an unchanged window produces no event and no new generation (the identity tuple and the facts equal the stored ones); a changed window gets its normal Update or Reclassified; a window that is no longer enumerated is requeried individually by pass 2 and retired only when the query says Gone (through `note_destroyed` with a fresh stamp and tick, or at the commit with RefreshUnseen when the destroy rule rejects the hint); a Failed query keeps the window. A flood of hints for unrelated windows therefore costs one refresh and zero generation changes (CO `test_flood_child_hints_no_churn`: 10 exported windows, 600 child DESTROY hints, `generations_issued` stays 10, no event).
4. Status of the pump call. The pump returns the status of the recovery refresh: Ok only if it committed and no hint is queued and no overflow is latched on return; More if hints arrived or overflow latched again while the refresh ran (pump again); otherwise the refresh's failure status: EnumFailed (failed or truncated enumeration), GraphQueueFull, GraphError, StampExhausted, DesktopInactive (defensive) or Reentrant. The table is already empty in all failure cases and `refresh_requested` stays 1, so the cadence retries the refresh (`refresh_due` is true 250 ms after the attempt). The coordinator must therefore treat every status that refresh can return as valid from pump (8.3 step 3).
5. Residual risk (also O5). The old recovery retired every window with `invalidate_all`, which also cleared a recycled HWND whose destroy hint had been lost. The new recovery does not: a window destroyed and recreated with the identical HWND and identity tuple (process, thread, start time, class hash) whose destroy hint was in the discarded table (or lost by the OS) is indistinguishable from the old incarnation, stays on its old generation and keeps its old surface (CO `test_overflow_recovery_gone_and_recycled` pins this). `interop_window_graph_invalidate_all` remains in the ABI for the coordinator to use when it needs a hard resync (for example after a prolonged DesktopInactive or a suspected lost-hint storm); the observer never calls it.
Worked numbers: 10 Active windows g1..g10, empty ring, 600 child DESTROY hints. The first 512 fill the table, hints 513..600 are overflow (`hint_overflows = 88`). One pump: 10 enumerated windows queried and offered (10 Unchanged), `refreshes_committed` +1, events 0, generations still 10. If instead the ring is full and the enumeration contains a new window, the offer of the new window returns QueueFull, the refresh aborts, pump returns GraphQueueFull with the table empty and `refresh_requested = 1`; after the coordinator drains, the next due refresh admits the window (CO `test_dirty_overflow_recovery`).

### 4.12 Stop, restart, destroy; graph destroy

Observer stop validates pointer, owner thread and busy first. Created/Stopped
return Ok unchanged. Started/Failed clear callback publication and wait for all
previously published in-flight hints, while retaining lifecycle reservation.
Every retained hook is removed in reverse order; successful tokens become0 and
failed tokens remain owned. Clear only scalar hint/refresh work, never graph
pins. Failure enters Failed and returns UnhookFailed. Success enters Stopped and
releases reservation. Counters/stamps survive; restart performs a fresh refresh.
Observer destroy performs the same retryable teardown for Started/Failed. Any
failure leaves the owner/slot intact and resets busy for retry. Only success frees
and NULLs the owner. Neither operation calls graph retirement or produces proof.

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

A call from a thread other than the owner (a test double, or a misconfigured hook) executes exactly these steps: `atomic_fetch_add(g_hints_in_flight, 1)` (seq_cst); acquire-load `g_active_observer`; read the immutable `api` and `owner_thread`; call `api->current_thread_id` (required to be trivial, non-blocking and thread-safe: `GetCurrentThreadId` natively); `atomic_fetch_add(hints_wrong_thread, 1, relaxed)`; `atomic_fetch_sub(g_hints_in_flight, 1)` (release); return. Nothing else is read or written, and the busy flag is never touched. The status snapshot reads that one field with an atomic relaxed load, so the value can lag by in-flight increments. Lifetime proof: `stop`, a failed-start rollback and `destroy` first clear `g_active_observer` (CAS to NULL) and then wait until `g_hints_in_flight` reads zero. A hint that loaded the pointer before the clear incremented the counter before the load (seq_cst), so the waiter sees it and waits; a hint that increments after the clear loads NULL and leaves. No `note_hint` can therefore touch an observer after `stop` or `destroy` has returned, on any thread. The wait yields the processor on every iteration (`SwitchToThread` on Windows, `sched_yield` elsewhere), so a preempted foreign-thread hint on a single-CPU guest is scheduled instead of burning the waiter's quantum; it is bounded by the duration of one hint, which never blocks. CO `test_release_slot_waits_for_inflight_hint` runs this contended branch (7.1.4). (A hint running on the owner thread itself cannot overlap `stop` or `destroy` on that thread, so the wait only ever covers a foreign-thread hint.)

### 5.4 Reentrancy guards

- Graph: `busy` is set to true after the `enter` checks pass and cleared by `defer` on every exit path, including error returns. It covers the whole call, hence the lookup callback (inside `observe`, `refresh_offer`), the drain sink (inside `drain`), and the clock callback (read once per call by every tick-carrying call, `observe`, `refresh_offer` and `note_destroyed`, for the tick estimate of 3.5/4.8, and by `take_capacity_diagnostic`, which has no epoch). While `busy` is true any graph function, including read-only ones (`get_stats`, `find_live`, `list_retiring`, `stamp_high_water`), returns Reentrant without touching anything and `destroy` does not free. Rationale: the sink runs while the ring head has not yet advanced past the event it was handed; a nested `next_event` would deliver the same event twice. The lookup callback receives a pointer to the engine's stack-local copy of the facts (copied at entry of `observe` and `refresh_offer`, so the caller's struct cannot change under the callback) and a key buffer on the engine's stack; neither may be retained.
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
| Ok (0) | all | Completed; for pump the table is empty and no overflow is latched on return; for refresh the graph committed | continue |
| More (1) | pump | Budget exhausted, a key was put back for retry, or hints arrived (or overflow latched again) while an overflow recovery refresh ran; hints remain | call pump again (next iteration) |
| DesktopInactive (2) | pump, refresh | Desktop not Default: nothing processed, graph untouched, hints kept, `refresh_requested = 1`, `desktop_active = 0`, `pumps_desktop_inactive++`, `last_refresh_attempt_ms`/`latch_ms` stamped from one clock read; also, defensively, when the graph reports a Suspended disposition (the core always sets OnInputDesktop after its single desktop check, so this does not occur; pump puts the key back, refresh aborts first) | wait for the cadence poll |
| GraphQueueFull (3) | pump, refresh | Graph reported QueueFull; the key is put back at the tail (pump) or the refresh aborted / left open (commit); latches kept |  drain graph events, call again |
| InvalidArgument (4) | all | NULL, zero budget/stamp/process id, malformed api, `uac_mode` above 2; nothing changed (`*out = NULL` for create) | fix the call |
| WrongThread (5) | all but note_hint | Nothing touched, destroy does not free | call from the owner thread |
| NoMemory (6) | create | `*out_observer = NULL`; nothing leaked | retry or abort |
| AlreadyActive (7) | start | Another observer owns the process-wide slot; this one unchanged | stop or destroy the other |
| BadState (8) | start, stop, pump, refresh | Call illegal in the current lifecycle state; nothing changed | respect the lifecycle |
| HookFailed (9) | start | Rollback attempted; failed tokens/reservation retained in Failed, otherwise Stopped | stop/destroy retries cleanup; never discard owner |
| UnhookFailed (10) | stop, destroy | Failed tokens/reservation and owner retained in Failed; successful tokens cleared | retry stop/destroy; callback publication stays NULL |
| GraphError (11) | pump, refresh | Unexpected graph status other than QueueFull and Reentrant (`last_graph_status` has it); the key re-inserted at the head, processing stopped; refresh aborted | inspect `last_graph_status` (StaleEpoch, WrongThread, NoRefresh, ...) and fix wiring |
| EnumFailed (12) | refresh, and pump when it runs the overflow recovery | Enumeration failed: no graph call, nothing begun, nothing retired. Enumeration truncated (above 2048 windows): the refresh ran its offers and unseen requery and was aborted, never committed; nothing retired by absence. In both cases the refresh latch is kept | none; retried on the cadence |
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

- Allocation failure at create. Graph: one allocation; failure returns NoMemory with `*out_graph = NULL`. Observer: one allocation, same. The Zig tests sweep a `FailingAllocator` over the single allocation of create (ZG, ZA); the observer test `test_create_no_memory` fails the one `calloc` through the linker wrapper `__wrap_calloc` (Linux only) and requires NoMemory, a NULL slot and, under ASan/LSan, zero bytes leaked.
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
- Enumeration failure or truncation. Failed: EnumFailed before `refresh_begin`, no refresh is opened, nothing is retired. Truncated (more than 2048 top-level windows, `WindowObserverMaxEnumerated` is a hard limit): the refresh is opened and applied to the partial list (offers, unseen requery) and then aborted instead of committed, EnumFailed is returned and the latch is kept; nothing is retired by absence, so windows past the 2048th are never admitted by the enumeration and retire only through hints or a Gone result (O11).
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

Scope of this section: what tests, commands and gates exist for the window graph (`src/interop/window_graph.zig`, `src/interop/window_graph_abi.zig`, `include/waddle/interop_window_graph.h`) and the Win32 observation adapter (`src/interop/window_observer_windows.{h,c}`), with their exact names and counts. This section states ONLY what exists in the tree; everything that was planned and is absent is listed in 7.5 as not provided. Nothing in this section is executed on Windows. The committed headers are authoritative; where an earlier design note differs from a header, the header wins (section 9).

Notation.

- `ZG` = `src/interop/window_graph.zig` (136 `test "..."` blocks, run by `zig test src/interop/window_graph.zig`).
- `ZA` = `src/interop/window_graph_abi.zig` (14 own `test "..."` blocks; `zig test src/interop/window_graph_abi.zig` also compiles and runs the 136 imported engine tests, so it reports 150).
- `CG` = `tests/interop/window_graph.c` (26 tests; links the real `window_graph_abi.o`).
- `CO` = `tests/interop/window_observer_windows.c` (63 tests; links the real observer object and the real `window_graph_abi.o`; every Win32 call goes through the api double in `tests/interop/window_observer_api_double.h`).
- A Zig test is `test "<name>"`; a C test is a `static int test_<name>(void)` in a table that `main` runs in order, printing `ok <name>`; the first failing check prints file, line and expression and stops the run.
- Test blocks and fixtures of both Zig modules sit below the line `// Test-only fixtures.`, which appears exactly once per file and in no doc comment.

### 7.1 Tests that exist

Totals: ZG 136, ZA 14 (150 in the ABI module run), CG 26, CO 63. The names below are copied from the sources; there is no separate matrix id scheme.

#### 7.1.1 ZG: the engine (136 tests)

*Classifier, validation, codec helpers, constants*

- `classification matrix covers every step of the header table`
- `every excluded class matches exactly, case-insensitively and nothing else`
- `classification parents follow owner_root, never the raw owner`
- `classification never reads past an unvalidated class or image length`
- `valid facts: defaults, maxima and boundaries`
- `validate_facts rejects each violated rule and nothing else`
- `validate_facts checks text only over the stated length`
- `utf8_valid_no_nul accepts well-formed text and rejects every malformed class`
- `app_key_valid matches the frozen codec rule and the 3..200 length rule`
- `class_hash_of follows FNV-1a 64 and distinguishes case`
- `ascii_eql_ignore_case folds only ASCII letters over the full length`
- `unwrap_tick resolves a raw tick to the 64-bit value nearest the base`
- `derive_state maps facts to protocol bits and never sets Activated`
- `identity_equal compares process, thread, start time and class hash`
- `record_init returns an all-zero record`
- `bump increments and saturates without wrapping`
- `status values and constants equal the committed header`
- `app_key_valid agrees with the codec rule on random keys up to 200 bytes`

*Create, call guards (thread, busy, epoch)*

- `create validates every configuration field and builds a zeroed graph`
- `create copies the configuration by value`
- `create under allocator failure leaves no graph and leaks nothing`
- `create takes the default monotonic clock when no now_ms is given`
- `enter applies thread, busy and epoch rules in order`
- `a foreign thread gets WrongThread and touches nothing`

*Event ring, coalescing, next_event, drain*

- `ring wraps around over many more pushes than its capacity`
- `a full ring refuses a new event and changes nothing`
- `announce and update merge keep the announce kind and OR the change mask`
- `update and update merge OR their masks and count only appended updates`
- `events never merge across a retire intent`
- `retire intents are always appended and an older update stays unmerged`
- `events of another generation or window do not merge`
- `next_event reports None on an empty queue and leaves the output alone`
- `drain delivers events oldest first up to max_events`
- `drain on an empty queue reports None and delivered 0`
- `drain stops at a refused event and redelivers it first next time`
- `drain rejects bad arguments after the common rules and changes nothing`
- `graph calls made from inside the sink are Reentrant and nothing is delivered twice`

*Lookups, find_live, list_retiring, complete_retire, stats, capacity diagnostic*

- `check_action is Ok only for the exact Active pair`
- `lookup helpers distinguish Active, Retiring and deferred records`
- `find_live describes only Active entries`
- `list_retiring orders by generation, reports the total and truncates`
- `complete_retire releases only the exact Retiring generation`
- `get_stats fills counters and recomputed gauges`
- `stamp high water is read through the guard and the plain getter`
- `capacity diagnostic is None without reading the clock when every counter is zero`
- `capacity diagnostic follows the sixty second rate limit`
- `capacity diagnostic takes its first sample even at clock zero and guards the clock callback`

*observe: admission, update, deferral, parents, identity, reclassification, capacity, QueueFull, generations*

- `first admission announces the full payload with the looked-up key`
- `the lookup receives a stack copy of the facts, never the caller's struct`
- `each fact group maps to its change bit in a queued Update`
- `updates coalesce into a pending Announce or Update and OR their masks`
- `an identical offer is Unchanged, queues nothing and still advances stamp and tick`
- `an older tick advances the record but never moves the tick base backwards`
- `an Update that cannot be queued returns QueueFull unless a pending event absorbs it`
- `a child whose parent is absent is deferred, then flushed in age order behind its parent`
- `re-offering a deferred window updates its record in place without a new lookup`
- `a deferred window offered with an Active parent is admitted from the stored key`
- `a three-deep chain is announced root first whatever the arrival order`
- `flushed children are admitted breadth first in deferral age order`
- `owner cycles through deferred records return Cycle and drop only the offered window's record`
- `a live window offered with an owner cycle stays unchanged`
- `identity change retires the old generation and its descendants and admits a fresh generation`
- `identity change of a thread, start time or class also forces a replacement`
- `identity change keeps the retirement when the replacement cannot be admitted`
- `identity change whose replacement must wait for its parent is deferred with a fresh key`
- `reclassification retires the cascade descending and re-announces it behind the new parent`
- `reclassifying a window under its own descendant retires the cascade and defers nothing`
- `reclassification toward an unannounced parent defers the window and re-defers its descendants`
- `reclassification with one free slot admits the window and leaves its descendants deferred`
- `reclassification with a cut flush keeps the unadmitted descendant deferred`
- `reclassification with no free slot degrades to retire-only and retries on the next offer`
- `a role change without a parent change also reclassifies`
- `retirement for every non-reclassify cause leaves the descendants retired and not deferred`
- `a window that is not exported is Excluded and drops its deferred record`
- `an off-desktop offer is Suspended and changes no entry, record or event`
- `64 exported entries refuse a 65th window, evict nothing and free a slot only after completion`
- `a flush cut by capacity leaves the youngest children deferred and counts them`
- `a 65th deferred record evicts the oldest deferral`
- `lookup failure, invalid keys and re-entry publish nothing`
- `QueueFull changes nothing and the same offer succeeds after a drain`
- `QueueFull on a forced retirement counts nothing else and keeps the entry live`
- `generations grow by one, run out cleanly and leave existing entries working`
- `a flush cut by the last generations counts them as generation exhaustion`
- `reusing an HWND always yields a fresh generation and a late completion cannot touch it`

*note_destroyed and tick handling (wrap, 30/60 days, clock estimate, silence beyond 2^31 ms)*

- `stale epochs, invalid facts and stale facts are rejected before any state change`
- `a genuine destroy hint retires the live entry and advances the high-water marks`
- `a destroy hint older than the admitting facts is ignored`
- `a destroy hint for a deferred record needs both a newer stamp and a non-older tick`
- `tick wrap: a destroy across the 32-bit wrap retires, one just before the admission does not`
- `destroy hints still work 30 and 60 simulated days after the admission`
- `with a clock that does not advance, a silence longer than half the range still aliases`
- `tick_estimate adds the clock time elapsed since the anchor, never goes below the base and saturates`
- `the estimate anchor follows ticks and clock: newer ticks re-anchor, delayed ticks keep the estimate`
- `a rejected call reads the clock but changes neither tick_base nor its anchor`
- `a graph without a now_ms callback unwraps with its default timer and keeps working`
- `after a silence longer than 2^31 ms of the graph clock, current-tick offers repair the base and a genuine destroy hint retires`
- `the very first tick-carrying call after a long silence is unwrapped by the clock estimate`
- `estimate unwrap tolerates a clock that runs ahead of the ticks by less than half the range`

*Cascade rule through deferred chains*

- `a reclassification whose new parent chain re-enters the cascade through a deferred leftover is retire-only`
- `the cascade rule follows a longer deferred chain and a plain chain to a non-cascade terminal still defers`

*OwnerHidden, preedit parent, classify_public*

- `OwnerHidden promotes an owned window and a later flag change reclassifies it`
- `an IME preedit window follows its owner and loses the parent when the owner is hidden`
- `classify_public reports the pure verdict in the C struct`
- `classify_public rejects bad arguments and invalid facts and leaves the verdict alone`
- `OwnerHidden is valid only together with an owner`

*Internal helpers (cascade, records, deferral, seen marks, ring merge)*

- `collect_cascade gathers Active descendants by exact parent pair, descending by generation`
- `retire_cascade keeps records, marks Retiring and queues children before the root`
- `event_from_record copies every payload field and leaves the rest zero`
- `record_from_facts copies identity and geometry and only the stated title bytes`
- `defer_insert uses a saturating age and evicts the lowest index among equal ages`
- `drop_deferred empties the slot and counts a drop, not an eviction`
- `mark_seen marks the Active entry and the deferred record but never a Retiring entry`
- `bump_by adds and saturates`
- `ring_merge_target returns the newest Announce or Update and stops at a RetireIntent`
- `seen marks follow entries through re-deferral, promotion and flush`
- `a deferred record of another incarnation is dropped and its key is never reused`

*Seeded random traces and observe_locked*

- `random operation traces keep every graph invariant and the event stream ordered`
- `observe_locked runs the plan on the facts it is given without applying the call guards`

*Refresh (begin, offer, keep, unseen, commit, abort), invalidate_all, guards of the refresh calls*

- `refresh_begin opens the refresh, clears every mark and reports an already open refresh`
- `offering every live window and committing retires nothing`
- `a commit retires every unseen entry children first with reason RefreshUnseen`
- `a seen child of an unseen parent retires with it as ParentRetired and is never re-deferred`
- `a commit drops unseen deferred records and keeps the seen ones`
- `offers that fail still mark the window seen so the commit keeps it`
- `an offer rejected with QueueFull is not applied but marks the window seen`
- `a flush leftover cut off by capacity survives the commit only when it is offered again`
- `refresh_keep and refresh_unseen: statuses, ordering, truncation and null rules`
- `a commit that does not fit the ring changes nothing and succeeds after draining`
- `refresh_abort closes the refresh without retiring or dropping anything`
- `an offer without an open refresh returns NoRefresh after the argument checks and marks nothing`
- `refresh_offer hands the lookup its own stack copy of the facts`
- `a reclassification inside a refresh keeps the new generation seen and its re-deferred children unseen`
- `invalidate_all retires every Active entry children first with reason Invalidated`
- `every refresh function rejects a stale epoch and changes only the rejection counter`
- `every refresh function from a foreign thread returns WrongThread and touches nothing`
- `every refresh function called while the graph is busy returns Reentrant and touches nothing`

#### 7.1.2 ZA: the exported C ABI (14 tests)

- `create rejects bad slot and config pointers and nulls every valid slot`
- `create maps allocation failure to NoMemory and frees everything it took`
- `destroy handles null, empty, repeated, misaligned and self-containing slots`
- `null and misaligned graph handles are InvalidArgument for every export`
- `a foreign thread gets WrongThread from every graph export and touches nothing`
- `calls from inside the lookup and sink callbacks return Reentrant and destroy keeps the graph`
- `a stale epoch is decided before argument errors and changes only its counter`
- `pointer faults are rejected before the engine for every export with pointer arguments`
- `a pointer fault on a foreign thread is WrongThread and a stale epoch is StaleEpoch`
- `classify validates its arguments and facts`
- `zero-capacity arrays ignore their pointer and truncation reports the full count`
- `drain reports SinkFailed with the delivered count and keeps the refused event`
- `capacity diagnostic, invalidate_all and lookup failures through the exports`
- `lifecycle through the exports: admit, events, retire, complete, refresh, drain`

#### 7.1.3 CG: the C ABI consumer (26 tests, registration names)

`runtime_sizes`, `create_destroy`, `invalid_arguments`, `lifecycle`, `update_coalescing`, `child_before_parent`, `capacity`, `deferral_overflow`, `classification`, `observe_errors`, `queue_full`, `drain_sink`, `retirement`, `complete_retire_matrix`, `note_destroyed`, `tick_wrap`, `identity_and_reclassify`, `refresh`, `invalidate_all`, `capacity_diagnostic`, `stats_and_find`, `wrong_thread`, `reentrancy`, `epoch_isolation`, `generation_monotonic`, `random_trace`

#### 7.1.4 CO: the observer through the api double and the real graph (63 tests)

| Test | What it checks (its comment) |
|:--|:--|
| `test_create_validation` | create: every bad config field, a NULL slot, a malformed api table, and the success state. |
| `test_create_config_overlaps_slot` | R11: a config whose storage overlaps the out slot is copied before the slot is written. |
| `test_create_no_memory` | create returns NoMemory and a NULL handle when its single allocation fails; nothing leaks (LSan). |
| `test_lifecycle_start_stop_restart` | start, stop, restart: hook ranges and order, removal order, state, stamps and latch. |
| `test_lifecycle_second_observer` | A second observer cannot start while another is Started; it can after the first stops. |
| `test_lifecycle_hook_failure_rollback` | A failing install at the 1st, 2nd or 3rd hook removes the installed ones in reverse and fails the state. |
| `test_lifecycle_unhook_failure` | A failing remove_hook at any position still attempts all three, fails the state and refuses restart. |
| `test_lifecycle_destroy_started` | Destroying a Started observer with queued hints removes its hooks and frees everything. |
| `test_lifecycle_wrong_thread` | Every entry point returns WrongThread (or the scalar default) off the owner thread and touches nothing. |
| `test_hint_filter_table` | Hint filter: every accept/reject row changes exactly the counters it must. |
| `test_hint_wrong_thread` | Hints from another thread only bump the atomic wrong-thread counter, whatever they carry. |
| `test_release_slot_waits_for_inflight_hint` | R7: the code that clears the process-wide slot waits (yielding) for a hint that is inside note_hint. The helper thread parks inside current_thread_id, i.e. after note_hint registered itself and loaded the slot, for 200 ms. destroy runs meanwhile on the owner thread; it must not return before the park ends, and the hint then touches the still-live observer (a use-after-free here would be an AddressSanitizer failure). This is the only test that runs the contended branch of release_slot. |
| `test_dirty_coalesce_order_budget` | Coalescing, first-seen order, budget, and the re-add of a key hinted while it is being queried. |
| `test_dirty_overflow_recovery` | Table overflow latches at the 513th distinct window and recovers through a full refresh (no invalidate_all). |
| `test_flood_child_hints_no_churn` | R1: a flood of DESTROY hints for child windows (unknown to the graph and already gone) must not touch the exported windows, below the table capacity (processed key by key) and above it (one refresh). |
| `test_overflow_recovery_gone_and_recycled` | Overflow recovery still retires a window that is really gone, and documents the one undetected case. |
| `test_overflow_pump_statuses` | R5: the statuses of pump's overflow path. EnumFailed is returned (latch kept, nothing retired); Ok only when the table is empty on return, More when a hint arrived during the recovery refresh. |
| `test_dirty_reserved_slot` | The slot reserved for a popped key: a put-back at a full table never overflows; a new window does. |
| `test_pump_admit_update_hide` | Admit, update, hide and re-show a window; check the graph events field by field. |
| `test_pump_cloak` | A cloak clears Visible in the double; the window is retired and re-announced on uncloak. |
| `test_pump_destroy_and_reuse` | Destroy hints: a stale one (older incarnation) is ignored, a real one applies, HWND reuse re-admits. |
| `test_pump_gone_vs_failed` | Gone retires through note_destroyed; Failed never retires, retries once, then drops the key. |
| `test_pump_queue_full` | A full graph ring puts the key back at the tail; a drain lets the next pump finish. |
| `test_pump_queue_full_destroy_information` | A QueueFull put-back keeps (or correctly clears) the destroy information of the key. |
| `test_pump_desktop_inactive` | A desktop that is not Default processes nothing, keeps hints, latches a refresh and does not spin. |
| `test_pump_busy_reentrant` | Re-entry from inside query_facts: Reentrant everywhere, nothing freed, note_hint still works. |
| `test_pump_graph_reentrant` | A graph Reentrant status maps to the observer's Reentrant status, with the key kept at the head. |
| `test_pump_stamp_exhausted` | Stamp exhaustion: a refused key stays queued; a refresh aborts and leaves no open graph refresh. |
| `test_refresh_admits_in_order` | A full refresh admits everything in enumeration order and commits. |
| `test_refresh_defers_child_before_parent` | A child enumerated before its owner is deferred and flushed when the owner is admitted. |
| `test_refresh_failed_window_kept` | A Failed window is kept while the healthy window behind it is admitted, and the refresh commits. |
| `test_refresh_unenumerated_survives` | A window known through hints but missing from the enumeration survives ten refreshes. |
| `test_refresh_vanished_retired` | A vanished window is retired at the refresh: as Destroyed when the hint rule accepts, else RefreshUnseen. |
| `test_refresh_enum_failure` | A Failed enumeration returns EnumFailed before any graph call; a Truncated one is applied but never committed. |
| `test_refresh_truncated_degrades` | R6: a Truncated enumeration is not a dead end. The listed windows are offered (admissions and updates flow), unlisted Active windows are requeried one by one (updates and Gone notes flow), and the refresh is aborted instead of committed, so absence from the partial list retires nothing. |
| `test_refresh_cadence_latch` | 250 ms cadence, backwards clock, and the latch-once rule with a frozen clock. |
| `test_refresh_queue_full_abort` | QueueFull while offering aborts the refresh (nothing retired); a later refresh succeeds. |
| `test_refresh_commit_queue_full` | QueueFull on the commit leaves the graph refresh open; the next refresh aborts it and starts over. |
| `test_refresh_replacement_observer` | A replacement observer that starts above stamp_high_water causes no retirements; one below it is dropped. |
| `test_owner_hidden_promotion` | A hidden owner promotes its dialog to Toplevel; when the owner becomes visible the dialog is Reclassified. |
| `test_owner_hidden_gone_and_failed` | A Gone owner sets OwnerHidden (not counted as Gone); a Failed owner fails the dialog's query. |
| `test_hidden_window_skips_owner_query` | R8: a window that is not visible is classified NotVisible first, so its owner is never queried. |
| `test_owner_hidden_uac` | The observer classifies the owner with the configured uac_mode; Native hides the consent owner. |
| `test_preedit_parenting` | The agent's preedit window is exported as ImePreedit under its owner with an empty title. |
| `test_preedit_from_config` | The preedit window can also be passed in the create config. |
| `test_own_process_excluded` | Own-process windows other than the preedit are excluded and a refresh never exports them. |
| `test_teardown_with_hints` | Destroying with queued hints and an open overflow latch frees everything. |
| `test_stats_accounting` | Exact counter accounting across hints, pumps, failures and graph errors. |
| `test_entry_arguments_and_threads` | NULL / zero arguments on every entry point, and WrongThread on every entry point in two states. |
| `test_start_zero_token` | A hook that installs "successfully" with a zero token is a failed install. |
| `test_hint_during_install` | A hint that races hook installation (state Created) is ignored without any accounting. |
| `test_hint_destroy_coalesces` | A Destroy hint coalescing into a queued entry records the destroy stamp and tick on that entry. |
| `test_facts_overlay_corners` | Overlay and owner-resolution corner cases of the facts assembly. |
| `test_putback_merge_queue_full` | QueueFull put-back merges a hint that arrived during the query into the retried entry. |
| `test_putback_merge_fatal` | A call-level graph error puts the key back at the head and merges the hint that arrived meanwhile. |
| `test_putback_merge_retry` | A failed query's retry entry merges a hint that arrived during that very query. |
| `test_pump_destroy_status_paths` | Destroy-application statuses: fatal, reentrant, Gone plus fatal; plus a Drop status from observe. |
| `test_refresh_enumeration_oddities` | Enumeration oddities: zero handles, vanished windows, failed unknown windows, an absurd count. |
| `test_refresh_graph_refresh_lost` | A graph refresh that vanishes mid-call makes offer, unseen listing and the quiet abort all report errors. |
| `test_refresh_unseen_stamp_exhausted` | Stamp exhaustion on the requery of an unseen window aborts the refresh. |
| `test_cadence_corners` | refresh_due / next_timeout_ms across no-attempt, backwards clock, backlog, blocked desktop and overflow. |
| `test_fuzz_convergence` | Convergence fuzz, general: create, destroy with owned-window cascade, HWND reuse with a new process id, visibility, owner and class changes, zero area, transient Failed, desktop flips, overflow bursts on every second seed, lost hints (8%), omitted enumeration entries, enumeration failure, lookup failure (2%), sink refusal (5%). 40 seeds x 3000 steps. Oracles: parent Announce before child, child retire before parent, generations only grow, app key equals the identity-derived key, and after a final quiescence the Active set, roles, parents and process ids equal the independent model. |
| `test_fuzz_convergence_stressed` | Convergence fuzz, stressed: every seed has overflow bursts, lazy draining (GraphQueueFull) and truncated enumerations. |

Behaviours the CO tests pin that are easy to miss:

- Overflow recovery is a full refresh and never retires a live window: `test_dirty_overflow_recovery`, `test_flood_child_hints_no_churn` (10 exported windows, 300 and then 600 child DESTROY hints, `generations_issued` stays 10 and no event is queued), `test_overflow_recovery_gone_and_recycled` (a really gone window is retired through the unseen requery; a window destroyed and recreated with an identical identity tuple whose destroy hint was discarded stays live: the documented residual risk), `test_overflow_pump_statuses` (pump returns EnumFailed from the recovery with the latch kept, and More when a hint arrives during the recovery).
- Truncated enumeration: `test_refresh_truncated_degrades` (2048 entries, Truncated: admission without a hint, update and Gone note through the unseen requery, refresh aborted, never committed) and `test_refresh_enum_failure`.
- `test_release_slot_waits_for_inflight_hint`: the only test of the contended in-flight wait of 5.3. A helper thread is parked for 200 ms inside `current_thread_id` (that is, inside `note_hint`, after it registered itself and loaded the slot) while the owner thread calls `destroy`; the test requires that `destroy` returns no earlier than the end of the park. Mutation check done by hand during development: with the wait loop removed the test fails and AddressSanitizer reports a heap-use-after-free in `note_hint_active`.
- `test_create_no_memory`: real only when built with the makefile's `WgObsWrap` (Linux, GNU-compatible linker); otherwise it prints a skip line and passes. See 7.3.
- `test_create_config_overlaps_slot`: a config whose storage overlaps the out slot is copied before the slot is written.
- `test_hidden_window_skips_owner_query`: a window with Visible clear causes no owner sub-query (and a failing owner does not fail it).

### 7.2 Random and stress traces that exist

1. Engine random traces (ZG `random operation traces keep every graph invariant and the event stream ordered`). Six seeded traces, fixed seeds `0x5EED_0001` to `0x5EED_0006`, 20000 + 20000 + 40000 + 40000 + 30000 + 60000 = 210000 steps in total (the test asserts the total), with different HWND pools (12, 220 and 90 windows), drain thresholds and with or without refresh operations. After every step `check_invariants` runs (entry and record consistency, parent before child with matching generation, capacity bounds, deferred hygiene, generation bounds) and the delivered event stream is checked for parent-first and child-retire-first order. The test also asserts that the generator reached the interesting paths (reclassifications, updates, coalescing, evictions, AtCapacity, Cycle, QueueFull, lookup failures, stale facts, refresh offers, keeps, commits, commit retirements, commit QueueFull, aborts, invalidations, unseen listings). It uses `std.testing.allocator`.
2. C ABI random trace (CG `random_trace`): `TraceOps` = 24000 operations through the C ABI with structural checks.
3. Observer convergence fuzzer (CO `test_fuzz_convergence`, `test_fuzz_convergence_stressed`): real observer, real graph and the api double. A model world of 40 fake windows (create, destroy with owned-window cascade, HWND reuse with a fresh process id, visibility, owner change, excluded class, zero area), transient Failed queries, desktop flips, 8% lost hints, 10% omitted enumeration entries, enumeration failure, 2% lookup failure, 5% sink refusal. `test_fuzz_convergence`: 40 seeds x 3000 steps = 120000 steps, overflow bursts of 600 handles on every second seed. `test_fuzz_convergence_stressed`: 8 seeds x 3000 steps = 24000 steps, bursts on every seed, lazy draining (GraphQueueFull), Truncated enumerations. Oracles in the graph event sink: no fabricated application key (the key is derived from the process id), parent Announce before child, child RetireIntent before parent, Announce generations only grow, no double Announce, no Update or retire for a non-live generation. After the random steps all faults are switched off, pumps, refreshes and retirement completions run, and the graph is compared with an independently computed model: the exact Active set, role, parent and process id of every window. A mutation that disables the owner sub-query is caught by the convergence check; the counts printed by the test (generations, overflow recoveries) show the runs are not vacuous (about 5400 generations and 400 overflow recoveries in the 40-seed test).

What the traces are not: there is no separate reference-model implementation of the engine and no per-step comparison of every call result against one. The engine traces check structural invariants; the observer fuzzer checks stream oracles plus end-state convergence against a consumer-side model. There is no deterministic replay hash and no pinned literal (the seeds are fixed, so a run is reproducible, but a behaviour change is not tripped by a literal).

### 7.3 Allocation-failure evidence and leak proof

Allocation facts: the graph performs exactly one heap allocation, `allocator.create(graph_t)`, in `create`, and the matching free in `destroy`; no other call allocates. The portable observer core performs exactly one `calloc` (observer, dirty table and enumeration buffer in one block) and one `free`; `pump` and `refresh` do not allocate. The native `query_facts` allocates only in one case: a visible window whose image path does not fit the 260-WCHAR stack buffer gets one 64 KiB heap buffer for that single call, freed on every path before the call returns (2.3). The Toolhelp snapshot is a kernel handle, scoped per pump or refresh call and closed on every path.

What exists:

- ZG `create under allocator failure leaves no graph and leaks nothing` sweeps a `FailingAllocator` over `fail_index` 0, 1 and requires exactly one allocation, equal allocated and freed bytes, and no live bytes on failure. ZA `create maps allocation failure to NoMemory and frees everything it took` does the same through `interop_window_graph_create` with the module allocator replaced for the test.
- CO `test_create_no_memory` (Linux, GNU-compatible linker only): the makefile passes `WgObsWrap` (`-DWgObsWrapCalloc=1 -Wl,--wrap=calloc`) to the plain, sanitizer and coverage builds of the observer test; the test defines `__wrap_calloc`, which fails exactly the next call when armed and otherwise forwards to `__real_calloc` (so AddressSanitizer still interposes). The test requires NoMemory, a NULL slot, that the armed failure was consumed by `create`, that exactly one wrapped call happened, and that the same config then succeeds. The Windows builds do not use the seam and the test is a skip there.
- C-side leak proof: the C tests run under `-fsanitize=address,leak,undefined -fno-omit-frame-pointer` with `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1` and `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. The Zig object is not instrumented, but its `std.heap.c_allocator` memory comes from libc `malloc`, which ASan and LSan interpose. Each C test creates and destroys its own graph and observer.
- Zig-side leak proof: `std.testing.allocator` in the Zig tests fails a test that leaves a byte allocated.

Not provided: a test that every operation after `create` performs zero allocations (the engine traces run with the testing allocator, but no counting or failing wrapper asserts `allocations == 1` after a trace); a deliberate-leak control that proves the harness detects a leak (the planned `-DWindowGraphTestLeakProbe`); an allocation sweep of the native `query_facts` (not runnable here).

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
| `window-graph-test` | builds `build/window_graph/window_graph_abi.o`, `window_graph_c_test`, `window_observer_test`; runs the two `zig test` modules in Debug and again with `-O ReleaseSafe`, and both C binaries | yes | below, steps 1 to 4 |
| `window-graph-symbols` | `nm` check of the ABI object: the global text symbols (type T) are exactly the 21 names in `WgAbiSymbols` and exactly the functions the public header declares | yes | step 4 |
| `window-graph-sanitizers` | rebuilds both C tests with ASan, LSan, UBSan into `build/window_graph/san/` and runs them | yes | step 5 |
| `window-graph-coverage` | sed copy of `tests/av/coverage.py`, Zig modes `window_graph` and `window_graph_abi`, then gcov of the observer | yes | step 6 |
| `window-graph-windows` | cross-compile and link only | no | step 7 |
| `window-graph-all` | `window-graph-test`, `window-graph-symbols`, `window-graph-sanitizers`, `window-graph-coverage`, `window-graph-windows` in order | partly | all |
| `window-graph-clean` | removes `build/window_graph` only (refuses empty, `/`, `.`, `..`) | no | none |

Logging pattern for evidence (exit code kept through `tee`):

```
set -o pipefail; make -f scripts/interop_window_graph.mk window-graph-all 2>&1 | tee build/window_graph_logs/window-graph-all.log; echo "exit=${PIPESTATUS[0]}" | tee -a build/window_graph_logs/window-graph-all.log
```

Manual equivalents, in the order the targets run them:

1. Zig engine and ABI tests (Debug, safety checks on, `std.testing.allocator` leak check on):
   `zig test src/interop/window_graph.zig -Iinclude -lc`
   `zig test src/interop/window_graph_abi.zig -Iinclude -lc`
   The target runs both commands a second time with `-O ReleaseSafe` appended. All four runs must report all tests passed and exit 0. `-lc` is mandatory because `std.heap.c_allocator` needs libc.
2. ABI object (the one object every C consumer links):
   `zig build-obj src/interop/window_graph_abi.zig -Iinclude -O ReleaseSafe -fPIC -fcompiler-rt -lc -femit-bin=build/window_graph/window_graph_abi.o`
3. C ABI test: `cc -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -pthread -Iinclude tests/interop/window_graph.c build/window_graph/window_graph_abi.o -o build/window_graph/window_graph_c_test && build/window_graph/window_graph_c_test`
4. Observer test with the api double and the real graph: `cc -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -pthread -Isrc/interop -Iinclude src/interop/window_observer_windows.c tests/interop/window_observer_windows.c build/window_graph/window_graph_abi.o -o build/window_graph/window_observer_test && build/window_graph/window_observer_test` ; the makefile adds `$(WgObsWrap)` (`-DWgObsWrapCalloc=1 -Wl,--wrap=calloc`) to the compile and link of the plain, sanitizer and coverage builds (7.3); override `WgObsWrapDef` and `WgObsWrapLd` to empty to build without it.
   Symbol check (`make -f scripts/interop_window_graph.mk window-graph-symbols`, run after step 2): `nm -g --defined-only build/window_graph/window_graph_abi.o | awk '$2=="T" {print $3}' | sort` is diffed against the 21 names in `WgAbiSymbols` and against `grep -o 'interop_window_graph_[a-z_]*(' include/waddle/interop_window_graph.h | tr -d '(' | sort -u`; both diffs must be empty and the expected list must have exactly 21 lines. The weak compiler-rt symbols (type W) of `-fcompiler-rt` are not T and are ignored. The object therefore exports exactly the 21 header functions and nothing that could count connection loss as proof.
5. Sanitizers (compiler must be GCC; `clang-19` on the reference machine lacks the ASan runtime library and cannot link): `cc -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -O1 -g -fsanitize=address,leak,undefined -fno-omit-frame-pointer -pthread -Iinclude tests/interop/window_graph.c build/window_graph/window_graph_abi.o -o build/window_graph/san/window_graph_c_test`, and the same flags for the observer test into `build/window_graph/san/window_observer_test`. Run each with `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. Exit 0 and no sanitizer report required.
6. Coverage.
   Zig modes: `sed "s/^InteropModes = (/InteropModes = ('window_graph', 'window_graph_abi', /" tests/av/coverage.py > build/window_graph/coverage_window_graph.py`, then `PYTHONPATH=tests/av python3 build/window_graph/coverage_window_graph.py window_graph` and the same with `window_graph_abi`. The sed must change exactly one line (the recipe checks `diff` shows 2 changed lines and the new text is present); `git diff --stat tests/av/coverage.py` must stay empty. The script compiles `src/interop/<mode>.zig` with `-Iinclude -lc -O ReleaseSafe`, takes production text as the bytes before the first `// Test-only fixtures.`, instruments the optimized LLVM IR, runs the instrumented test binary, and fails (exit nonzero) when line or branch is below 90 percent. Output: `build/coverage/interop/<mode>/run-<UTC>-<pid>/` (this path is fixed by the tracked script and is outside `build/window_graph/`; it is still ignored by git). Printed lines have the form `branch NN.NN% (a/b)` and `line NN.NN% (a/b)`.
   Rules the tests obey so the numbers mean something: (1) every direct call under test in fixtures goes through an `@call(.never_inline, f, .{...})` wrapper, and inputs come from runtime data (tables, PRNG, `std.mem.doNotOptimizeAway`), because direct calls with comptime-known arguments are constant-folded to zero sites, which makes the tool abort with "coverage site count must fit nonzero uint32" or report 0/N; (2) `// Test-only fixtures.` appears exactly once per file (`grep -c` must print 1) and no doc comment above it contains that string; (3) no `switch` has more than 255 distinct targets; (4) the branch metric counts conditional branches that survive in optimized IR, so an `if` converted to a `select` is visible only to line coverage; the matrix rows in 7.1 therefore exercise both sides of every decision explicitly instead of relying on the metric.
   Observer C (portable part), compiler and gcov are both GCC 14: `mkdir -p build/window_graph/coverage/observer; gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -O0 -g --coverage -fprofile-update=atomic -fprofile-abs-path -Isrc/interop -Iinclude -c src/interop/window_observer_windows.c -o build/window_graph/coverage/observer/window_observer_windows.o`; compile the test object with plain `cc`; link both with `gcc --coverage -pthread` and `build/window_graph/window_graph_abi.o`; run the binary; `cd build/window_graph/coverage/observer && gcov --json-format -b -c window_observer_windows.gcno > /dev/null`; the recipe's `python3 -c` reads `window_observer_windows.gcov.json.gz`, selects the single record ending in `src/interop/window_observer_windows.c`, counts lines with `count > 0` over all lines and branches with `count > 0` over all branch entries, prints `line a/b branch c/d` and exits nonzero below `WgCovMinPercent` (90). Use the `-b -c` JSON counts, not the text report's "Branches executed" figure, which is a different metric.
7. Windows cross-compile and link (not executed): `zig build-lib src/interop/window_graph_abi.zig -Iinclude -static -target x86_64-windows-gnu -O ReleaseSafe -fno-compiler-rt -lc -femit-bin=build/window_graph/window_graph_abi_windows.lib`; `zig cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude tests/interop/window_graph.c build/window_graph/window_graph_abi_windows.lib -o build/window_graph/window_graph_c_test.exe`; `zig cc -target x86_64-windows-gnu -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -U_WIN32_WINNT -D_WIN32_WINNT=0x0A00 -UWINVER -DWINVER=0x0A00 -DUNICODE -D_UNICODE -Isrc/interop -Iinclude src/interop/window_observer_windows.c tests/interop/window_observer_windows.c build/window_graph/window_graph_abi_windows.lib -ldwmapi -o build/window_graph/window_observer_test.exe`; and the native observer object alone with `-c` once with `zig cc -target x86_64-windows-gnu` and once with `x86_64-w64-mingw32-gcc`, both with the same `-U_WIN32_WINNT ... -D_UNICODE` defines. Rules: the defines go on the command line, never in source (Zig predefines `_WIN32_WINNT=0x0603`, so a source `#define` is a `-Wmacro-redefined` error under `-Werror`); `WINVER` must be at least 0x0605 or `GetDpiForWindow` is not declared; `-ldwmapi` is required for `DwmGetWindowAttribute` (user32 and kernel32 link by default); `file build/window_graph/*.exe` must report `PE32+ executable (console) x86-64`. Because the Windows C flags carry no `-pthread`, `tests/interop/window_graph.c` and `window_observer_windows.c` start their second thread through one helper, `run_on_second_thread`, implemented with `pthread_create` when `_WIN32` is undefined and `CreateThread` under `_WIN32`, so the same files cross-compile.
8. Tree hygiene check after everything: `git status --short` lists only the owned paths from the handoff (and nothing under `build/`, which is ignored); `git diff --stat -- tests/av include/waddle/interop_protocol.h GNUmakefile scripts/interop.mk README.md impl/seamless-interop/IMPL_DESC.md impl/seamless-interop/TRACKER.md` is empty.

### 7.5 What is not proven, what is not provided, pass criteria

Not proven by anything in this section:

1. No Win32 code ran. The `_WIN32` native section of `window_observer_windows.c` (hook procedure, `native_query_facts` including the reduced query of invisible windows, `native_enum`, desktop query, UTF conversion, DPI resolution and the PER_MONITOR_AWARE_V2 check, process image and start-time reads, the Toolhelp image fallback, the stack/heap image-path buffer, DWM calls) is only compiled and linked by `window-graph-windows`. Its behaviour is unverified until Codex's native runner executes it (section 8).
2. The api doubles are scripted fakes. They prove the observer's control flow, ordering, bounds and graph interaction, not that Windows delivers hints in that order, drops none, or reports the facts the double returns. They are not real-session or desktop acceptance.
3. No UIAccess, secure-desktop, elevated-process or UIPI behaviour is demonstrated.
4. Coverage excludes: the `_WIN32` section of the observer (not compiled on Linux, therefore absent from gcov); all Zig test blocks and fixtures below `// Test-only fixtures.`; the C test files; compiler panic and stack-canary guard branches (excluded by `coverage.py`, a count is printed). The gate is 90 percent line and branch per measured unit. Measured on the current tree: `window_graph.zig` line 99.87 percent (764/765), branch 98.25 percent (505/514); `window_graph_abi.zig` line 99.30 percent (141/142), branch 96.55 percent (56/58); portable `window_observer_windows.c` line 614/633 (97.0 percent), branch 350/374 (93.6 percent). Whether the gate holds is decided by the run, not by this document.
5. Observer counter saturation (`bump` in C) cannot be driven without a test hook; it is checked by review of the single helper only. The graph's counters saturate in the engine (`bump increments and saturates without wrapping`, `bump_by adds and saturates`).
6. The Zig object is not sanitizer-instrumented; Zig-side memory errors rely on Zig safety checks (Debug and ReleaseSafe runs) and on `std.testing.allocator`.
7. Time behaviour: the 32-bit wrap, the 30/60-day unwrap and a silence beyond 2^31 ms are exercised with synthetic ticks and an injected clock; the equality of the `GetTickCount` and WinEvent `dwmsEventTime` bases is an assumption (9.1 and 9.2), as is acceptance of the destroy-rule deviation of 4.8 (Codex, 8.6).
8. Performance, soak, hook-delivery loss under load, and the cost of a 250 ms `EnumWindows` refresh are not measured here.

Not provided (planned in an earlier draft of this section; absent from the tree):

- A separate reference model of the engine (`reference_model_t`) with per-step comparison of every call result; a deterministic replay hash with pinned literals; the ZA replay trace and the C stress trace with a hash (only the CG `random_trace` exists).
- The minimum step counts of the earlier draft (192000 + 480000 engine steps, 8 x 3000 observer steps with a model): the real counts are those of 7.2.
- A matrix id scheme (`cls_*`, `fct_*`, `cap_*`, `wo_*`, ...): tests carry prose names (ZG, ZA) or `test_*` names (CG, CO); 7.1 is the mapping.
- `alc_operations_do_not_allocate` and the leak-detection control (`-DWindowGraphTestLeakProbe`) of 7.3.
- A test that drives observer counter saturation.
- Any Windows execution, native runner or soak evidence (section 8.6).

Pass criteria for handing this slice back (all required, none waivable):

1. `window-graph-test`, `window-graph-symbols`, `window-graph-sanitizers`, `window-graph-coverage` and `window-graph-windows` each exit 0 (`window-graph-all` runs them in that order), logs saved under `build/window_graph_logs/` with the exit code recorded in `HANDOFF_RETURN.md`.
2. The test counts of 7.1 hold (ZG 136, ZA run 150, CG 26, CO 63) or the change is recorded; every test passes.
3. Zig tests pass in Debug and ReleaseSafe with zero bytes leaked. C tests: zero sanitizer reports with `detect_leaks=1`.
4. Line and branch coverage each at least 90 percent for `window_graph.zig`, `window_graph_abi.zig` and portable `window_observer_windows.c`, with the printed numbers recorded.
5. Windows artifacts link; they are reported as built, not run.
6. The delimiter count (`grep -c '// Test-only fixtures.'` prints 1 per Zig file) and the empty `git diff --stat` checks of 7.4 pass.

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
5. Call `window_observer_start`, which installs three out-of-context hooks (object range up to parent change, minimize, DWM cloak/uncloak). HookFailed/UnhookFailed require cleanup inspection: retry stop/destroy until every retained token is removed; a failed destroy preserves the owner and reservation. Report mode unavailable while cleanup is pending. AlreadyActive is a programming error. A successful start latches a full refresh, so the first loop iteration enumerates the desktop.

### 8.3 Ordered calls: one loop iteration

1. Wait: `timeout = window_observer_next_timeout_ms(observer, monotonic_ms())`, combined (minimum) with the loop's other deadlines. Block in a message wait (for example `MsgWaitForMultipleObjectsEx` with `QS_ALLINPUT`) up to that timeout. `WindowObserverWaitForever` means the observer needs no wake-up.
2. Message pump: drain the thread's messages with `PeekMessageW`, `TranslateMessage`, `DispatchMessageW` (bounded count per iteration for fairness). WinEvent callbacks are delivered here and reach `window_observer_note_hint`.
3. `window_observer_pump(observer, graph, session_epoch, budget, &stats)` with a budget of at least 1 (recommended 64). When the dirty table overflowed, pump runs a full refresh in place of hint processing (4.11) and can therefore return every status of `window_observer_refresh`, in particular EnumFailed (enumeration failed or truncated; the refresh latch is kept and the cadence retries it, treat it exactly like the EnumFailed of step 4); Ok always means the table is empty and no overflow is latched, hints that arrive during the recovery turn the result into More. Handle: Ok (idle), More (run another iteration without waiting), DesktopInactive (skip graph work; leave hints queued; the coordinator's own desktop-switch handling is separate), Reentrant (the call came from inside a nested message dispatch: drop it and retry from the loop; never treat it as a failure), GraphQueueFull (go to step 6, then repeat this step ONLY if that drain delivered at least one event; if it delivered none, the outbound path is not writable, so do not call pump or refresh again: wait for the transport's writable event, which is the only thing that can make room), GraphError (read `stats.last_graph_status`; StaleEpoch means the session was rolled, tear down; others are logged and fail-close the session), StampExhausted (replace the observer using the graph's `stamp_high_water`, or roll the epoch), BadState, InvalidArgument and WrongThread (programming errors).
4. `if (window_observer_refresh_due(observer, monotonic_ms()))` call `window_observer_refresh(observer, graph, session_epoch, &stats)`. Ok commits (also when some window queries failed: those windows were kept); DesktopInactive and EnumFailed (a failed enumeration retires nothing; a truncated one, above 2048 windows, applied its offers and Gone notes but was never committed) are retried by the 250 ms cadence (`InteropWindowGraphRefreshIntervalMs`); Reentrant is retried by the loop; GraphQueueFull means drain (step 6) and let the next due refresh restart it, under the same rule as step 3: only after a drain delivered at least one event, otherwise wait for the transport to become writable (a refresh that is retried against a full outbound path would only hit the same full ring).
5. Optionally `interop_window_graph_take_capacity_diagnostic(graph, &diagnostic)` once per iteration; Ok is logged (the graph already rate-limits to once per minute).
6. `interop_window_graph_drain(graph, session_epoch, sink, sink_context, max_events, &delivered)` with `max_events` of at least 1 (recommended 256). The sink copies the event and either serializes it with the frozen codec into the outbound path or appends it to a coordinator queue; it returns nonzero when its outbound capacity is exhausted (the event stays queued and drain returns SinkFailed; retry next iteration). It must not call the graph. Drain after every pump or refresh, and again immediately on GraphQueueFull. Events are delivered in order: Announce parent before child, RetireIntent child before parent.
7. Coordinator serialization: Announce and Update become the corresponding protocol messages (the `changed_mask` selects optional groups); RetireIntent makes the coordinator mark `(window, generation)` as pending retirement, stop sending actions for it and stop capture.
8. Retire proof: when the slot runtime reports that capture for the pair stopped and every frame pin drained (the proof-gated drain already tracked by the slot runtime), call `interop_window_graph_complete_retire(graph, session_epoch, window, generation, &proof)` with `proof = {1, 1}` and remove the pair from the pending set. Stale means a duplicate or already-completed pair (ignore); NotRetiring and ProofIncomplete are coordinator bugs. Every few seconds reconcile the pending set against `interop_window_graph_list_retiring` so a RetireIntent that was never delivered cannot leave a slot quarantined forever; but reconcile only pairs whose RetireIntent event was delivered to the coordinator (consumed by `drain`), or run the reconcile after a drain that emptied the ring: a Retiring pair whose RetireIntent is still queued must not be adopted as an orphan before its event is delivered.
9. Before executing any guest action (input, focus, close) for `(window, generation)`: `interop_window_graph_check_action`; not Ok means drop it.
10. When the IME preedit window changes: `window_observer_set_preedit_window(observer, hwnd)`.

The graph has no call that lists Active entries. The coordinator derives the Active set from the Announce and RetireIntent stream it has received.

### 8.4 Ordered calls: shutdown, transport reset, new epoch

1. `window_observer_stop(observer)`: the retained hooks are attempted in reverse order; UnhookFailed retains tokens/owner/reservation for stop/destroy retry, and the dirty table is cleared. Nothing is retired and no proof exists.
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
13. Needs native confirmation (assumptions the code makes that no document guarantees): (a) `GetWindow(hwnd, GW_OWNER)` returns NULL with `GetLastError() == 0` for an unowned window on every Windows build (the code treats NULL with a nonzero last error as a failed read, so a build that leaves a stale nonzero last error would make every unowned window Failed and nothing would be exported); (b) a window with Visible clear reports a usable identity tuple (class, process, thread, start time) from the reduced query of 9.1 item 33 and is classified NotVisible; (c) the 260-WCHAR first try of `QueryFullProcessImageNameW` returns `ERROR_INSUFFICIENT_BUFFER` for a longer path and the 32768-WCHAR retry succeeds.

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
11. **One heap allocation per object, fixed storage, no allocation in graph or portable-observer operations.** The one bounded exception is the native query's 64 KiB image-path buffer for a visible window with a path longer than 259 characters (2.3). Reason: bounded memory, trivial ownership (create/destroy), and an allocator-failure check at create.
12. **Reclassification is retire plus a new generation, reusing the stored app key.** Parent and role changes are never sent as Update. Reason: they need a new host surface; reusing the key avoids a second lookup that could fail halfway.
13. **Control events are never dropped.** Every mutating call checks ring room for its whole plan first (QueueFull, no mutation). Coalescing is allowed only for Announce and Update of one identity, never across a RetireIntent.
14. **Deferred records carry no generation and no capacity.** The key lookup happens at first deferral. Eviction is oldest-first and evicted windows return through the 250 ms refresh.
15. **Titles.** The observer converts UTF-16 to UTF-8 without strict validation so lone surrogates become U+FFFD, and cuts at a code-point boundary; the graph is the trust boundary and rejects invalid UTF-8 and over-length fields. Class and image names use strict conversion and fall back to ClassTruncated or ImageUnknown.
16. **`complete_retire` check order is Stale, then NotRetiring, then ProofIncomplete** (the header). Reason: a stale or unknown pair must never reveal or consume anything, whatever proof accompanies it.
17. **Time domain.** `captured_ms` and `event_time_ms` stay 32-bit wire values (the Win32 tick, wrapping every 2^32 ms), but the graph unwraps every tick against an internal 64-bit `tick_base` (starting at 2^32, advanced to the unwrapped value by every Ok call that carries a tick, never moving backwards) and stores 64-bit ticks in its records; all comparisons are plain 64-bit `<` on unwrapped values. Reason: a 32-bit signed-difference compare misreads spans above 2^31 ms (24.8 days) as negative, so a long-lived window would ignore a real destroy hint forever. Before unwrapping, every tick-carrying call estimates the current tick as `tick_base + (now_ms - tick_clock_at_base)` from the graph clock (elapsed 0 if the clock is absent, stuck or backwards) and reads `now_ms` once per `observe`, `refresh_offer` and `note_destroyed`; an Ok call re-anchors the pair. Unwrapping is exact while the graph clock does not drift from the tick domain by more than 2^31 ms between two Ok tick-carrying calls, so silences longer than 2^31 ms no longer make genuine destroy hints and Gone notes be ignored (the earlier text that a refresh repairs the base was wrong and is superseded). Aliasing by a multiple of 2^32 ms remains only if the clock did not advance during the silence; the next refresh then corrects the outcome.
18. **One started observer per process.** Reason: WinEvent procedures carry no context; a process-wide atomic slot names the active observer.
19. **Iconic windows report restored size** (from window placement; coordinates in workspace space are informational); non-iconic use DWM extended frame bounds with a window-rect fallback.
20. **OwnerHidden promotion (refinement of IMPL_DESC 4.4, needs Codex sign-off).** The observer computes a new fact flag, `InteropWindowFactOwnerHidden` (bit 12, mask 0x1FFF), for a window with a foreign `owner_root`: it queries the owner (second `query_facts`), classifies it with `interop_window_graph_classify` and the config's `uac_mode`, and sets the flag iff the owner is gone or its verdict is NotExported. The graph then treats the window as not owned in steps 4 to 7 and for the ImePreedit parent: a Menu stays Menu (parent 0), popup/owned-toolwindow rules use owned = false, an owned non-APPWINDOW window becomes Toplevel with parent 0. Reason: without it a Dialog, Menu or Tooltip whose owner is never announced (tray, zero area, transparent, excluded, destroyed) would sit in the deferred set forever (the first open finding of the earlier draft), be re-offered every 250 ms and crowd the 64 records, and the user would never see the window. The 4.4 table is silent about never-exportable owners, so this is a refinement, not a literal reading. A flag change that changes role or parent goes through the Reclassified path (one retire plus one announce). The exact one-line disable: the observer never sets OwnerHidden (delete the owner sub-query result handling in `query_one`); nothing else in the graph changes, and the graph classifier and `classify` export remain correct. This is why the decision is cheap to reverse.
21. **Refresh "seen" rule.** After the guards, `refresh_offer` marks the offered window seen for EVERY outcome (including every error status and QueueFull); a plain `observe` marks on Ok only; `refresh_keep` marks without facts; `refresh_unseen` lists the still-unseen Active windows so the observer can requery each individually before the commit; a refresh with failed queries still commits because failed windows are kept. Reason: the old rule (mark on Ok only) let one persistently failing or rejected window either block the commit forever or be retired by it; neither is acceptable. Absence from the enumeration alone is never the sole evidence of destruction; only Gone, a valid destroy hint, or an unseen window that the individual requery cannot keep retires.
22. **Cascade rules.** Inside `observe`, the identity-change check (retire the live entry) comes BEFORE the cycle check, and a rejected replacement (Cycle, AtCapacity, GenerationExhausted, LookupFailed, AppKeyInvalid) still leaves the retirement done (Ok/Retired). A deferred record whose identity tuple differs from the offered facts is dropped and the window is treated as having no deferred record (fresh lookup, new age). If a reclassified window's new parent is in its own descendant set, directly or through a chain of deferred records that ends at an Active entry of the cascade, the replacement is a Cycle (cascade retired, nothing deferred). Descendants are re-deferred only for a Reclassified cause of the same incarnation (they are alive and still owned by that HWND); for every other cause (destroy, exclusion, hide, incarnation change, refresh unseen, invalidate) they retire with ParentRetired and are not re-deferred, returning through their next hint or offer. Leftovers of a capacity-cut flush stay deferred and survive the commit because they carry the seen mark.
23. **Three hooks.** Object events 0x8000..0x800F (adds parent change, so an owner change that has no show or hide is seen), minimize start/end 0x0016..0x0017, and cloaked/uncloaked 0x8017..0x8018. Reason: a DWM cloak or a parent change flips a fact the graph keys on without a show/hide event; the 250 ms refresh would otherwise be the only detector. The hint filter accepts exactly these ranges; the new hints carry no authority (requery only).
24. **Destroy-rule deviation, stated honestly.** The rule of item 6 narrows, but does not literally satisfy, the handoff wording that a hint about an earlier incarnation must never retire a newer one. It can retire and readmit a newer incarnation when the event time equals the admission tick, or when the destroy fell between the observer's `tick_ms` sample and its native read of the admitting facts. The cost is churn bounded by one lost generation per coincidence: the observer requeries after every Destroy hint and re-admits a live window under a new generation, so a window is never permanently lost. This needs Codex acceptance and the native clock check of 8.6 item 4 (measure how often the coincidence happens); if Codex rejects it the alternative is to ignore equal ticks, which can leave a stale surface, a worse failure.
25. **Image-name fallback.** When `OpenProcess` fails, the basename comes from a `CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS)` walk by process id (taken at most once per pump or refresh call, retaken on a process-id miss, handle closed on every path) with `process_start_time` left 0; ImageUnknown only when both fail. Reason: UAC and CoreWindow rules need the image basename and an elevated or protected process otherwise loses it. The known-image-with-start-time-0 versus later known start time flip is an incarnation change (one surface of churn).
26. **No messages to, and no titles from, own-process windows.** The native query never sends a message to any window of the agent's own process and reads no title for it (AgentOwned windows report an empty title, the core forces `title_length` 0; they are excluded anyway except the preedit window). Reason: a send to a window of the thread that is inside the observer would re-enter the observer or deadlock; foreign windows are never sent a message either (no `WM_GETTEXT`, no `WM_GETMINMAXINFO`).
27. **One consistent create behaviour and a busy flag.** `window_observer_create` returns Unsupported when the native table cannot run (no `GetDpiForWindow`, or the thread is not PER_MONITOR_AWARE_V2); every other observer entry point except `note_hint` returns Reentrant while another entry point is active. The native adapter clamps width and height to 16384 (documented; the graph still rejects larger).
28. **Dirty-table reservation and put-back.** One dirty slot is reserved while pump holds a popped key (`note_hint` treats `count + popped_in_flight >= 512` as full), so a put-back always fits; a graph QueueFull puts the key back at the tail, other stops at the head. Reason: a put-back that could not fit would silently lose a key.
29. **Coordinator contract.** After GraphQueueFull the loop calls pump/refresh again only after a drain delivered at least one event, otherwise it waits for the transport's writable event; list_retiring is reconciled only for pairs whose RetireIntent was delivered (or after a drain that emptied the ring) (8.3). The feature `TRACKER.md` is updated in every commit that changes progress.
30. **Overflow recovery is a full refresh, never `invalidate_all`.** A hint-table overflow (which child-window events make easy to cause) clears the table and runs one refresh in `pump` (4.11); exported windows that still exist and are unchanged see no event and keep their generation. `invalidate_all` stays in the ABI for the coordinator. Reason: a flood of child hints must not recreate every host surface. Cost: the residual risk of O5 now also covers a destroy hint discarded by the overflow.
31. **A Truncated enumeration is applied and aborted, not rejected.** `WindowObserverMaxEnumerated` (2048) is a hard limit. Above it a refresh offers the first 2048 windows, requeries the unseen Active windows individually and aborts instead of committing; EnumFailed is returned (4.4, O11). Reason: failing before `refresh_begin` made admissions and Gone notes impossible forever on such a desktop. Raising the buffer was not chosen: it only moves the limit.
32. **The in-flight wait yields.** The loop that waits for foreign-thread hints in `release_slot` calls `SwitchToThread` / `sched_yield` each iteration (5.3).
33. **Reduced native query for windows that are not visible.** A window with Visible clear (not `IsWindowVisible`, or DWM-cloaked) is classified NotVisible before any other fact matters, so `native_query_facts` skips its rect and fullscreen reads, layered alpha, title, image path and Toolhelp fallback and reports `image_length = 0` with ImageUnknown; it still reads the relations, styles, class, dpi, process id, thread id and process start time because the identity tuple (process, thread, start time, class hash) decides IncarnationChanged for an Active window that becomes hidden. The core also skips the owner sub-query for such a window. Gone and Failed are decided exactly as for visible windows. Reason: the 250 ms refresh queries every enumerated window, hidden ones included. Unverified natively (8.6 item 13).

### 9.2 Open findings and risks (stated as they stand)

Identifiers are stable across drafts: the first item of the earlier draft (owned windows whose owner is never exported) moved to decision 20 and its identifier is not reused, so the list starts at O2.

- **O2. Native-mode hidden UAC window.** In mode Native the consent window is counted (`uac_native_hidden`) but its handle is not exposed, so the coordinator cannot read the prompt through this API. A separate channel is needed.
- **O3. Quarantine can pin capacity.** If the coordinator never completes a retirement, up to 64 Retiring entries keep occupying all slots and every admission returns AtCapacity. Deliberate (releasing without proof is unsafe); the mitigation is a coordinator watchdog that escalates to epoch roll.
- **O4. Hint-overflow recovery (changed).** The recovery is now one full refresh (4.11), not `invalidate_all`, so an overflow no longer retires and re-announces the exported windows. The cost moved to O5 and O24. `invalidate_all` stays in the ABI for the coordinator.
- **O5. Identical-tuple recycled HWND with a lost destroy hint is undetectable.** If the OS drops the destroy event, or the destroy hint is discarded together with the dirty table by an overflow recovery, and the HWND is reused with the same process, thread, start time and class, the graph sees an unchanged window and keeps the old generation. The 250 ms refresh cannot detect it either. After an overflow this is reachable without any OS loss.
- **O6. `GetTickCount` versus `dwmsEventTime` base is unverified.** Documentation does not state the base; the destroy rule depends on them being comparable. Needs the native clock check of 8.6.
- **O7. Minimum size is not queried** (`min_width` and `min_height` stay 0) because `WM_GETMINMAXINFO` is a cross-process send. The Update `MinSize` bit is never produced by the Win32 adapter.
- **O8. `UpdateLayeredWindow` windows are treated as opaque** because `GetLayeredWindowAttributes` fails for them; a fully transparent per-pixel window can be exported.
- **O9. Elevated and UIAccess denial behaviour is unverified, with a sharp consequence.** If the agent cannot read `consent.exe`'s image, the window is reported ImageUnknown and no UAC rule matches, so the interim dummy window and the credential window would be exported as ordinary Toplevel windows in every mode. Whether consent.exe is visible on the Default desktop at all depends on the secure-desktop policy. Needs the native runs of 8.6 item 6; no UIAccess or secure-desktop claim is made.
- **O10. No Windows execution was possible.** Every native claim is compile and link only.
- **O11. WinEvent delivery is best effort.** Hints can be dropped or delayed under load; the 250 ms refresh bounds staleness. A permanently Failed query no longer blocks refresh commits: the window is kept with `refresh_keep` and is never updated (a real Gone or a destroy hint still retires it). With more than 2048 top-level windows `EnumWindows` is Truncated: every refresh offers the first 2048, requeries the unseen Active windows and aborts (EnumFailed, never committed), so windows past the 2048th are admitted only by their own hints and retire only through a destroy hint or a Gone result (4.4, decision 31).
- **O12. `EnumWindows` does not list UWP CoreWindow top-levels on Windows 8 and later** (research, documentation), so the CoreWindow exclusion matters mainly for windows reached through hints; the frame windows that are enumerated are exported like any other.
- **O13. Observer paths without a test seam:** counter saturation in C (7.5). The NoMemory branch has a seam on Linux (`WgObsWrap`, 7.3) and none on Windows.
- **O14. Header silence:** the behaviour of `window_observer_note_hint` when the logical clock is exhausted is not specified; the implementation must document it, and CO `test_pump_stamp_exhausted` pins it.
- **O15. `GetDpiForWindow` and PER_MONITOR_AWARE_V2.** `GetDpiForWindow` is resolved at run time (Windows 10 1607 or later) and the owner thread must be PER_MONITOR_AWARE_V2; `window_observer_create` returns Unsupported (15) otherwise and allocates nothing, so nothing is ever exported with an invented 96 dpi. The requirement is on the guest agent's manifest or thread setup (8.2); whether the target agent can run V2 is a Codex decision.
- **O16. No Active-list call.** The graph cannot enumerate Active entries; the coordinator must track them from the event stream (8.3).
- **O17. Cloak detection relies on `DwmGetWindowAttribute(DWMWA_CLOAKED)`**; if the call fails the adapter reports Failed (never Visible), so a DWM-disabled session never exports.

### 9.3 Differences between the earlier design note and the committed headers (header wins)

1. The design note had a Cloaked fact flag and a NotExported(Cloaked) reason; the header has neither (Visible includes not-cloaked).
2. The design note listed ProofIncomplete before the entry lookup in `complete_retire`; the header orders Stale, NotRetiring, ProofIncomplete.
3. The design note made a missing `GetDpiForWindow` a create failure with no dedicated status; the amended header returns `Unsupported` (15) from create for a missing `GetDpiForWindow` or a thread that is not PER_MONITOR_AWARE_V2 (O15).
4. The design note listed an app-key check of "empty, over 200, invalid UTF-8, NUL"; the header applies the full frozen codec rule.
5. The earlier note's makefile had no allocation seam for the observer; the current makefile has the Linux-only `WgObsWrap` seam (7.3). There is still no test access for observer counters (7.5).
- **O18. Application key is never re-looked-up.** The key is fetched once at first admission or deferral and reused for every later generation of the same window (reclassification, re-admission from a deferred record). If the catalog entry for a process changes (renamed application, new icon key), the surface keeps the old key until the window is retired by some other cause. Codex decides whether to re-lookup on Reclassified or periodically.
- **O19. Popup-role parents for Dialog and Toplevel.** Dialog parent is `owner_root` and Toplevel has no parent. The host maps parent to an xdg_toplevel parent or an xdg_popup depending on the role; whether a Dialog that is really a native popup (a custom drop-down using a plain owned WS_POPUP with a caption-like frame) should be an xdg_popup is a host mapping question that this slice cannot answer.
- **O20. Opaque per-pixel and click-through overlays.** A window using `UpdateLayeredWindow` (per-pixel alpha) is treated as opaque because `GetLayeredWindowAttributes` fails for it, and a `WS_EX_TRANSPARENT` click-through overlay (a screen-capture border, a game overlay) is exported like any other window: the host will show a possibly empty opaque rectangle. A later slice needs either a per-pixel alpha probe or an overlay heuristic.
- **O21. Known-versus-0 `process_start_time` flip churn.** An image whose `OpenProcess` alternates between success and failure (an elevation boundary, a sporadic access error) flips the identity tuple between a known start time and 0, which the graph treats as an incarnation change: each flip costs one surface retire plus one announce. The rule is conservative on purpose (a stale surface is worse). If flips are common on the target, Codex can pin the first-known start time per HWND in the observer.
- **O22. Refresh cost.** One refresh performs up to 2048 queries from the enumeration plus up to 64 individual requeries every 250 ms. A window that is not visible is queried with the reduced native query (decision 33: no title, rect, image path, allocation or Toolhelp walk, and no owner sub-query), but still costs `OpenProcess` plus `GetProcessTimes` and the relation and style reads. Measure on a native runner (8.6 item 8) before adding an enumeration pre-filter.
- **O23. Parent change is a retire, not an Update.** `WindowUpdate` could carry a parent, but this design retires the window and announces a new generation on any parent or role change (decision 12). The cost is one host surface per owner change. Codex may extend the protocol update to carry a parent; until then the retire path stays.
- **O24. Child-window hint flood.** Every `OBJID_WINDOW`/`CHILDID_SELF` WinEvent of a child window is accepted into the 512-key table because the portable core cannot recognise a child without a Win32 call and the hook procedure may call none. Below 512 distinct handles each child key costs one pump query (a child is classified Child or is Gone); above it the overflow recovery is one full refresh (4.11) with no export churn. A native-only pre-filter (`GetAncestor(hwnd, GA_ROOT) != hwnd` for non-DESTROY hints, which sends no message) would cut the pump cost; it is not implemented because it cannot be tested here and a DESTROY of a child cannot be tested natively (the handle is already invalid).
- **O25. Design-review m1 and m5 residuals (documented, not narrowed).** m1: there is no post-query tick sample, so the equal-tick and sample-to-read churn of 4.8 and decision 24 remain declared, not narrowed; acceptance is Codex's. m5: an IncarnationChanged admission still runs the flush over deferred records that wait on that HWND; those records may belong to children of the previous incarnation whose destroy hint was lost, and can be flushed as ghost Announce/RetireIntent pairs, bounded by the next commit (the refresh drops unseen deferred records). The engine was not changed for either.
- **O26. `GetWindow(GW_OWNER)` last-error assumption.** `native_fill_relations` treats a NULL owner with a nonzero `GetLastError()` as a failed read (after `SetLastError(0)`). If any Windows build leaves a nonzero last error for an unowned window, every unowned window would become Failed and nothing would be exported. Needs native confirmation (8.6 item 13).
6. The earlier committed text of this document (and the design note) listed 18 graph functions, a 13th observer status that aborted a refresh on any failed query, a pair of hooks (0x8000..0x800C and 0x0016..0x0017), a 12-bit fact mask, a 32-bit `time_before` destroy comparison, "mark seen on Ok only", a `reserved0` field in `window_observer_config_t`, and descendants that were always re-deferred. The amended headers have 21 functions (adds `classify`, `refresh_keep`, `refresh_unseen`), no failed-query status (StampExhausted 13, Reentrant 14, Unsupported 15), three hooks, a 13-bit mask with OwnerHidden, 64-bit unwrapped ticks, the offer-marks-every-outcome seen rule, `uac_mode` at config offset 36, and re-deferral only for same-incarnation Reclassified. Sections 1 to 9 above follow the amended headers.


### 9.3 Codex review correction: create output precedence (2026-10-24)

Review of returned3714cf2 found create clearing its output through a config alias
before rejecting the overlap. The corrected C boundary checks raw config/output
extent overlap before forming or writing the typed output slot. Aliased regions,
including a misaligned config extent overlapping a valid aligned slot, return
InvalidArgument with all caller bytes unchanged. A valid disjoint slot keeps the
existing NULL-on-later-failure rule. Runtime C and Zig regressions verify both
aligned callback-context overlap and misaligned overlap. Debug/ReleaseSafe150
Zig tests and27 real C ABI tests pass in private /tmp outputs; native Windows
execution and leak-enabled lead qualification remain separate pending gates.


### 9.4 Codex review correction: retryable observer hooks (2026-10-24)

The returned3714cf2 released its singleton callback pointer before hook removal,
allowing a second start during teardown, and discarded failed unhook tokens.
Codex requires the separate lifecycle reservation and retryable teardown described
in sections3/4/6 above. Deterministic tests interleave a second start from the
first owner's actual remove-hook callback, fail stop and destroy, and fail rollback
after partial installation. The second start remains AlreadyActive through cleanup;
failed destroy keeps its caller slot; retries remove only retained tokens and can
then free/NULL the owner. This supersedes every historical leak-until-exit policy.
Public numeric enums/ABI structs and wire/layout versions are unchanged. Strict
portable tests65/65 and Windows object cross-build pass. Worker leak-disabled
ASan runs are supplemental only; lead detect_leaks1 qualification remains required.

Microsoft documents UnhookWinEvent as fallible and owner-thread-bound
(https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-unhookwinevent).
The retry policy follows that boundary and never treats failure as detachment.
OUTOFCONTEXT callbacks require a message loop and run on the installing thread,
with reentrancy possible (https://learn.microsoft.com/en-us/windows/desktop/api/winuser/nf-winuser-setwineventhook).
Native behavior and installed-agent acceptance remain separate qualification.

### 9.5 Codex namespace conformance

Owned Zig module aliases use `std_t`, `c_t`, `graph_t`, and `testing_t` to
satisfy the repository type naming rule. External module properties, including
`std_t.testing`, and C pointer sentinel syntax `[*c]` remain unchanged. This is
an identifier-only transformation: `namespace_review_receipt.json` records the
exact two source hashes and token proof. It grants no behavioral or native
qualification credit.

### 9.6 Codex required correction: invalidate cached descendants on discontinuity

This section supersedes the stale-deferred-child residual in O25/m5 and the
old plan/apply steps that allow it. Implementation is pending at this contract
commit. No classification or native lifetime proof is added.

A deferred record has no parent incarnation proof. On accepted destruction,
exclusion or identity replacement, it must not be promoted using only a recycled
numeric parent HWND. Before mutation, form a fixed64-slot deletion mask: seed
all used deferred records whose parent_window names any invalidated root, then
select their transitive deferred descendants. Each selected slot is visited at
most once; no allocation, native calls or unbounded recursion. Include every
HWND in a retiring Active cascade among the roots, not just the primary HWND:
otherwise a deferred descendant of an Active child could escape invalidation.
Dropping the primary deferred record remains explicit and counted once.

Apply this mask on Active IncarnationChanged replacement, identity replacement
of an existing Deferred record, accepted note_destroyed (Active or Deferred),
NotExported observation of a known Active/Deferred parent, and every doomed
Active entry in refresh_commit. A seen deferred child of a doomed parent is
also invalidated. An unknown NotExported parent is conservative evidence that
it cannot serve cached descendants; invalidate those descendants too. Suppress
replacement flush for invalidated descendants, including multi-level chains.
Ordinary first admission and same-incarnation Reclassified keep their existing
parent-first flush and re-deferral behavior. Rejected stale destroy hints,
Suspended, failed queries, refresh_keep, Unchanged and Update do not invalidate.

Plan the complete deletion mask and event count read-only. QueueFull preserves
entries, deferred arrays, generations and events apart from the established
rejection diagnostic. Apply deletion only after ring-room preflight and lookup
outcome. An accepted identity change still retires the old cascade and drops
its invalidated cached descendants if replacement lookup/capacity/generation
fails, as the existing retire-only outcome requires. No drop emits a fabricated
Announce/RetireIntent. Use existing deferred-drop accounting, count each slot
once and preserve surviving records' seen/age/order.

Tests must cover Active and Deferred replacement, direct/chained descendants,
accepted versus stale destroy, exclusion, seen-child/unseen-parent refresh,
QueueFull retry and lookup failure. Freshly reobserved children may be admitted
against the replacement; stored facts from the invalidated subtree may not.
Same-incarnation Reclassified regressions must continue to pass. The independent
review is archived in incarnation_review.md. O5 delivered-destroy loss remains
open separately; no full-refresh continuity proof is accepted for identical
identity tuples after discarded destroy evidence.
