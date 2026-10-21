# CLVK Runtime Probe V2: Native Unload/Reload Regression Contract

## Purpose and boundary

This is a native Windows test harness contract for the isolated MinGW C++ runtime DLL probe. It detects per-load process-handle, TLS-index, FLS-index, and process-heap growth while a DLL is repeatedly loaded and unloaded. It also exercises DLL-owned C++ state from the process entry thread, DLL-created `std::thread` instances, four pre-existing idle process threads, and raw Win32 threads created by the harness.

The probe does not change production runtime code, build-driver policy, dependency pins, or the OpenCL integration fixture. It provides diagnostic/acceptance evidence only when run on the intended native Windows image with the exact candidate DLL and recorded source/binary hashes. Cross-compilation validates ABI and compiler compatibility only; it is not native evidence. The old POSIX and unshimmed Win32 DLLs remain controls and are expected to fail their existing TLS oracle.

## Files and ABI

`tests/vgpu/clvk_runtime_windows.c` is the process harness. `tests/vgpu/clvk_runtime_windows.cpp` is compiled as a DLL and owns every C++ object it creates. The exports use C linkage and fixed-width/scalar arguments only. No C++ object, pointer to DLL state, heap allocation, TLS index, FLS index, or DLL-owned handle crosses the ABI.

The DLL exports API version 2:

- `runtime_lifetime_probe(uint32_t api_version)` runs the bounded C++ lifecycle checks, including a deliberately concurrent first use of `std::call_once`, an initialized scalar `thread_local`, exception catch, identity checks, joined thread retirement, and a detached thread whose duplicated native handle is waited and closed inside the DLL.
- `runtime_foreign_thread_probe(uint32_t api_version, uint32_t slot, uintptr_t ready_event, uintptr_t release_event)` runs from a raw harness-created thread. The two event handles are borrowed from the harness and remain valid until all foreign threads have returned and been joined. The DLL validates the API version and slot, initializes and updates scalar `thread_local`, records the current `std::thread::id` under a DLL-owned mutex, checks uniqueness against the other concurrently live slots, signals the borrowed ready event only after all four distinct slots have been published, and waits no longer than five seconds for the borrowed release event. It returns a documented status code; it never closes either event.

API version 1 is rejected with a nonzero status. The second export is required for v2; missing exports fail the harness while preserving safe cleanup.

Status values are stable for this fixture version. The primary export returns
`0` success, `1` API version mismatch, `2` concurrent `call_once` failure,
`3` static registry/iostream failure, `4` joined-thread retirement failure,
`5` scalar `thread_local` or joined-thread exception failure, `6` thread
identity mismatch, `7` caller exception failure, and `8` detached-thread
retirement failure. The foreign export returns `0` success, `20` invalid API,
slot, or event argument, `21` foreign-thread `thread_local` mismatch, `22`
duplicate slot or thread identity, `23` ready-event failure, `24` release-event
timeout/failure, and `25` foreign-thread exception failure. All failures are
ordinary return codes; exceptions do not cross the C ABI.

## Lifetime sequence and ownership

1. The harness validates its single UTF-8 DLL path into a fixed-size UTF-16 stack buffer.
2. It holds a process-owned reference to `msvcrt.dll` for the full measurement interval so unloading the candidate cannot destroy the CRT heap whose retained allocations are being measured.
3. It creates one manual-reset ready event and one manual-reset release event, then creates exactly four idle threads. Each thread increments a shared ready count, signals the ready event when all four are waiting, and remains alive until the harness signals release. The harness joins and closes every thread handle and closes both events during all cleanup paths.
4. It records baseline free TLS slots and FLS slots by allocating indices into fixed bounded stack arrays until exhaustion, then freeing every successfully allocated index in reverse order. No index is retained. Both counts are measured from the running image; historical values such as 1086 free TLS slots are diagnostic only, never hard-coded expectations. Each measured baseline must be exactly restored after every load/unload cycle. The FLS probe has 4097 stack slots and must observe exhaustion before filling that array; if it cannot prove exhaustion within the bound, baseline measurement fails rather than silently truncating the count.
5. It records the baseline process handle count after all persistent harness-owned handles and the four idle threads exist.
6. For each configured cycle, it checks the pre-load handle count against the baseline, performs candidate load and unload on watchdog-observed native threads, resolves both v2 exports, and invokes the primary lifecycle export on its own watchdog-observed thread. It starts four raw `CreateThread` workers that concurrently call the second export, waits for the one ready signal emitted after all four IDs have been registered, releases the workers, and joins them before unloading the DLL. The DLL remains loaded until all calls and workers have completed.
7. It unloads the candidate only after all test-created DLL callers have returned. It clears every function pointer before any later use, verifies the candidate module and both exports are absent, then samples handle, TLS, and FLS balances.
8. Before the first load, it records an aggregate process-heap tuple. Cycle 1 is an explicit heap warm-up and cycle 2 establishes the post-warm-up heap reference. The first-load before/after delta is emitted as diagnostic evidence only. From cycle 3 onward, the heap oracle requires exact equality with the cycle-2 reference for both busy bytes and busy-entry count. Each observation retries for at most 100 intervals of 10 ms. Heap acceptance is based on the aggregate `(busy bytes, busy entries)` tuple over every process heap while `msvcrt.dll` remains pinned.
9. After all cycles and the final heap observation, it releases the idle threads, joins them, closes their handles and events, and finally releases the held `msvcrt.dll` reference. It reports success only if every mandatory assertion passed.

## Heap measurement contract

The heap oracle first queries the process heap count with `GetProcessHeaps(0, NULL)`, rejects zero or a count larger than its fixed-capacity stack array (128 heap handles), and then fills the array with a second `GetProcessHeaps` call that must return the exact same count. This prevents silent truncation if the process owns more heaps than the bound. For each heap it calls `HeapLock`, walks entries with `HeapWalk`, counts only entries marked `PROCESS_HEAP_ENTRY_BUSY`, and sums their `cbData` using checked 64-bit arithmetic. It always calls `HeapUnlock` after a successful lock, including `HeapWalk` and overflow failures. `ERROR_NO_MORE_ITEMS` is the only successful end condition for a walk; any other API error is reported and fails closed. A pre-load snapshot and cycle-1 snapshot report first-load deltas but do not assert them away or treat them as proof that one-time allocations were freed.

The measurement path performs no explicit dynamic allocation and does not print, load modules, create threads/events, or modify process environment while heaps are locked or being walked. Output is emitted after the full snapshot and all heap locks have been released. The fixed arrays, counters, heap-entry record, and module list are stack-owned. Busy bytes mean the sum of `PROCESS_HEAP_ENTRY.cbData`, not allocator metadata, reserved virtual address space, or UMDH stack attribution.

A heap observation is a pair of unsigned 64-bit values: busy bytes and busy entries. The post-cycle-2 pair is the reference. Cycles 1 and 2 are reported but are not treated as steady-state equality evidence. Every cycle from 3 through the configured count must match both reference values within its bounded 100 × 10 ms observation window. Failure to enumerate, lock, walk, unlock, or settle is a hard failure, never an omitted measurement.

## Synchronization and cleanup

The four pre-existing idle threads use two harness-owned manual-reset events and an interlocked readiness counter. They remain alive continuously from before baseline measurement through the final candidate unload. The four per-cycle foreign workers share a ready event and release event, both owned by the harness; the candidate's DLL-owned registry mutex protects slot publication and ID comparison. The harness releases and joins every worker before `FreeLibrary`.

Every successful `CreateThread`, `CreateEventW`, `LoadLibraryW`, `TlsAlloc`, and `FlsAlloc` has one explicit release path. The counting routines free all successful index allocations even after exhaustion or an error. If a DLL call or worker setup fails, the harness signals any created release event, joins every started worker, clears borrowed function pointers, unloads the module only after no call can execute inside it, then stops/joins idle threads and closes owned handles. Candidate load, unload, and the primary DLL call each run under a 10-second watchdog; foreign and internal C++ waits use 5-second bounds; thread joins use a 10-second bound. Internal C++ workers duplicate a real self-handle and publish it before returning, allowing a bounded wait for native termination before `std::thread::join`. A watchdog expiry or uncertain live-thread state prints and flushes a failure diagnostic, then terminates the test process with a nonzero exit code before any unsafe DLL unload or CRT teardown. It never retries a timed-out join with an unbounded wait. The harness never closes a pseudo-handle or reuses a borrowed handle after its documented lifetime.

## Exact acceptance assertions

- Compile-time lifetime count is between 3 and 4096 inclusive; the stress binary uses 2048.
- The process records the image's free TLS baseline dynamically. Every post-unload TLS observation equals that same measured baseline; no historical slot count is a portable acceptance constant.
- The FLS exhaustion count is recorded before cycles and restored exactly after every cycle. All measurement-owned TLS/FLS indices are freed on every path.
- Each cycle starts and ends with exactly the same process handle count. The existing fixed wait remains 100 × 10 ms; neither baseline nor budget may be widened to make a failure pass.
- The candidate DLL is absent after unload, and neither v2 export can be resolved from a residual module.
- Four pre-existing idle threads stay alive across all load cycles; four separate raw Win32 threads call the second export and are joined before unload on every cycle.
- The primary export returns zero. It preserves the earlier `call_once`, shared-mutex, joined-thread, iostream, and exception checks; four internal C++ threads wait at a ready/start barrier before contending on first `call_once`. The joined and detached workers are both observed terminated before the export returns. The detached worker duplicates a real handle to itself with `GetCurrentThread` from inside that thread; the parent waits on and closes that handle, independent of MinGW `native_handle_type`. Each foreign export returns zero, records a distinct ID while all four worker threads are simultaneously alive, sees its own initial `thread_local` value, and completes after release.
- Heap cycle 1 is warm-up; cycle 2 is the comparison reference; every cycle 3+ exactly matches that reference in busy bytes and busy entries within the fixed 100 × 10 ms bound.
- The terminal `RUNTIME_LIFETIME_PROBE PASS` line appears only after all DLL callers and worker threads are joined, the module is unloaded, the idle threads are released and joined, all harness-owned events/handles are closed, and the pinned CRT reference is released. `CYCLES_COMPLETED` is provisional and is not a pass. Watchdog failure exits the test process nonzero; no external signal, shortened stress count, or weakened assertion is acceptance evidence.

## Status and limits

This document specifies the prospective v2 native probe only. At authoring time no native execution of the revised harness or rebuilt real `OpenCL.dll` has occurred. A successful cross-build is not evidence that TLS/FLS indices return, heaps balance, Windows loader TLS storage retires, or native handles return. The warm-up and post-cycle-2 reference can hide a leak that occurs only on the first module load; the first-load delta is diagnostic, not a zero-leak proof. Native acceptance must separately compare the approved leaking DLL and final candidate using UMDH captures before load, while loaded, and after unload, with `msvcrt.dll` pinned, and attribute positive groups to the actual DLL. It must also include a first-load leak control and pair the aggregate HeapWalk oracle with final real-DLL native qualification. It does not establish actual WDDM, Adobe application, D3D12, physical hypervisor cross-VM, or production OpenCL acceptance.

### Heap-enumeration boundary

The process-heap tuple is measured only with test-owned DLL calls quiescent. `GetProcessHeaps` returns the active heap count, including when it exceeds the supplied capacity; count growth between sizing and filling is rejected. Enumeration and subsequent per-heap walks do not form an atomic process-wide snapshot, and an unexpected concurrent heap destroy can invalidate a borrowed handle. The tuple is a diagnostic control, not a general proof that unobserved background activity is absent. First-load and full-DLL owned heap attribution still require the separate UMDH/negative-control evidence.

### Native heap control and corrected Win32 end condition

`--heap-controls` loads no candidate DLL: it creates one private heap, captures the process tuple, retains a 12,345-byte owned allocation, requires the tuple to detect it, frees it, requires exact restoration, and destroys the heap before reporting success. The first exploratory native v2 run failed before any candidate load with error 259, exposing the fixture's incorrect use of `ERROR_NO_MORE_FILES`. Microsoft documents `ERROR_NO_MORE_ITEMS` for successful `HeapWalk` completion; the corrected implementation accepts only that code, preserving all other error failures. The original failed run remains in `build/qualification_feature_20261021/native_probe_758487be/`. No candidate acceptance is inferred from correcting the fixture.

The Win32 end-code contract is specified in [Microsoft HeapWalk documentation](https://learn.microsoft.com/en-us/windows/win32/api/heapapi/nf-heapapi-heapwalk). The corrected native heap control passes in `build/qualification_feature_20261021/native_probe_4f58c01e/` (zero exit, no owned survivors). An exploratory eight-cycle run of the combined Win32 shim/EH probe in `native_probe_3301f3fc/` ends naturally with exact handles 70, free TLS 1086 and FLS 4078 in every cycle, and exact heap tuples from cycle 3. It also exposes an unattributed first-load increase of 8,016 busy bytes / 6 entries; the reference does not prove those bytes retired. This run is explicitly **not acceptance**: strict admission still rejects retained thread-exit helpers, the foreign gthread-equality boundary remains under review, and first-load attribution is pending. The actual full OpenCL.dll was not executed by these experiments. Source/binary hashes and failed initial baseline run are retained.
