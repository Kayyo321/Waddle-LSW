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

### Optional loader-TLS heap attribution matrix (2026-10-21)

`WaddleRuntimeIdleThreadCount` selects 0..16 pre-existing idle threads (default4);
per-cycle foreign callers remain exactly4. The fixed16-element stack arrays do
not allocate, and the zero-thread case skips only the readiness wait. All
handle/TLS/FLS/heap assertions and their budgets are unchanged.
`WaddleRuntimeHeapAudit` requires a valid fresh supervisor directory in
`WADDLE_DXVK_HEAP_AUDIT`; absence/malformation fails before module loading.
It allows at most8 cycles and uses the existing closed-handle, exact PID/creation,
60-second acknowledgement contract. Captures occur at baseline, loaded_N after
all candidate callers retire, unloaded_N after confirmed module absence, and
threads_retired after the pre-existing threads/events close while msvcrt remains
pinned. Stages do not perform cleanup or alter the measurement reference.
These variants enable UMDH attribution of first-load and loader-TLS storage with
K={0,4,16}; compiling them is not proof that any allocation retired.

Three real-source build regression groups cover all six K/audit combinations,
reject out-of-range thread counts and reject oversized audited runs, including
normal versus optimized Python execution. Raw cross-build receipts are retained
under `build/qualification_runtime_probe/20261021T182818_pim4vi1u/` and
`20261021T182821_9f8o8qit/`. Native execution of these variants remains pending.

The separately frozen admitted probe's2048-cycle native stress run
`build/qualification_feature_20261021/native_probe_cda4fe0f/` **failed naturally
at cycle381**, exit1, no owned survivors. Handles70, free TLS1086 and FLS4078
were exact throughout; heap busy bytes fell from177275 to176227 and busy
entries239 to237. This is an unclassified decrease1048bytes/2entries, not a
passing stress run or zero-owned-leak proof. It is preserved without widening
the strict equality oracle. The source/executable hashes bind that earlier
fixture, not the newly added audit variants. First-load UMDH and stress-delta
attribution remain required before final runtime acceptance.

### Separate strict stress after positively observed startup retirement

`clvk_runtime_quiescent_windows.c` includes the unchanged base fixture with only
its entry point and process-count call sites aliased. Windows declarations are
included before the alias; the existing stage helper is included after undoing
it. The first count call occurs after original stream-buffer initialization,
msvcrt pinning, idle-thread setup and TLS/FLS baseline probes, but before the
original count/heap baselines and before any candidate load. This avoids a
stdio/setvbuf ordering change and warms the index-probe storage before capture.
The one-shot gate uses existing exact closed-marker/60-second handshake at
startup_quiescence; on failure the original main takes its ordinary cleanup.
All subsequent count calls forward directly to the original Win32 API. No
original cycle assertion, limit, reference calculation or watchdog is changed.

The supervisor first requires both exact stack-tracked startup allocations,
912bytes at TppWorkerThread+0xd03 and136bytes at
TppAllocThreadData+0x41/TppWorkerThread+0xd8. It binds the current native ntdll
image hash651ac9041af8e6b5ebd331d53163244c0a27d36d1433f0f15cac1f28c5f2e82c
to the preserved matching PDB before resolving those frames. Both allocation
addresses must then disappear from complete raw UMDH records, with no new
matching startup groups, in two consecutive captures within the existing
60-second stage deadline. Missing initial owners, missing stacks, a loaded
candidate before the gate, changed process identity, unretired startup owners
or deadline failure rejects the run. No fixed sleep/count allowance substitutes
for that proof. Only then may the supervisor acknowledge and let the original
strict2048-cycle fixture start. Each configured cycle and full natural teardown
must still pass. First-load attribution and full-DLL qualification remain
separate gates; the original failed381-cycle run remains a failure.

Source-bound strict cross-build and an independent read-only source/receipt
review pass (`root_tls/probe_quiescent_stress/inputs.json`). This section records
the prospective native protocol; no successful2048 native run is claimed here.
The three prior K={0,4,16} audits collected all8stages, exited0 and restored UST;
their raw inventories independently reproduce the OS startup -912/-136 groups.
UMDH omits allocations without collected stacks, so it supplies attribution,
not a replacement for the unchanged all-process-heaps HeapWalk oracle.

## First-load observability negative control (2026-10-21)

`tests/vgpu/clvk_runtime_first_load_control.cpp` is an explicit-opt-in negative
DLL fixture, never a production or leak-free candidate. It includes the actual
runtime workload unchanged and intentionally retains one 12,345-byte malloc
allocation from a static C++ constructor on every load. The volatile owner
prevents removal; OS process teardown is its only retirement site. The normal
strict probe must reject it; no allocator suppression or relaxed threshold is
permitted.

Strict cross-build receipt: `build/qualification_feature_20261021/root_tls/
probe_first_load_control/receipt.json`. Actual console execution is preserved in
`build/qualification_feature_20261021/runtime_first_load_negative_05771710/`: it
exits naturally with expected failure1 at cycle3, no survivors. HeapWalk rises
203,504/273 to215,849/274 between cycles2/3, exactly12,345bytes/one entry. All
seven UMDH captures miss that allocation and contain zero runtime DLL frames,
including while the DLL is loaded. Therefore absence of UMDH runtime stacks is
not calibrated evidence of zero owned malloc leaks. The older raw reports stay
preserved; allocator acceptance is explicitly open pending another method that
identifies this control and proves normal ownership retirement.

The separate startup-gated unchanged2048-cycle probe finishes naturally with
exit0 in `runtime_quiescent_stress_9fcc8655/`, exact per-cycle heap/handle/TLS/FLS
balances and no survivors. Startup OS pool retirement was observed before any
DLL load; no original assertion or budget changed. This isolated runtime result
does not qualify the full OpenCL DLL or first-load owned allocations.
