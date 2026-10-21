# Guest WSI resize and retirement lifetime

## Scope and boundaries

This bounded production change repairs the existing synchronous guest Win32 WSI path. It does not connect the AV HWND registry to host Wayland presentation, create direct guest DMA-BUF presentation, or qualify Windows/GPU behavior in a Linux-only test environment. The original surface and swapchain numeric identities remain monotonically allocated for each ICD binding. Reuse of an HWND without an intervening observed loss remains an open integration problem requiring window-destruction events; raw IsWindow checks cannot prove a window generation.

The covered transitions are zero-area/minimized windows, observed native window loss, resize detected before acquire or presentation, resize while GPU readback runs, failed swapchain replacement, and explicit image retirement. No dependencies are added. C remains the native Win32 boundary and Zig owns bounded state and temporary pixel allocation.

## Architecture and interactions

Vulkan ICD entry points hold the existing ICD lock and invoke venus_wsi. The registry owns at most 16 surfaces and 16 swapchains, each with up to three backend image/memory pairs. A borrowed native window belongs to the application. Backend callbacks own Vulkan execution and acknowledge image destruction or transfer lost-device resources to the preexisting abandon ledger. Readback synchronously completes before the native sink receives borrowed temporary pixel storage. The native sink acquires a DC only during one call and invokes ReleaseDC on every acquired path; a native release failure is reported, not assumed to have retired an OS resource.

No protocol layout, external wire format, host receiver, or AV interface changes. No new Vulkan feature or extension is advertised. Native window extent queries distinguish a valid zero-area client rectangle from invalid/destroyed windows. The new exact-size sink supplements the existing scaled compatibility API, so callers that deliberately request scaling keep their behavior.

## Data and state transitions

A surface receives a sticky lost flag after a native extent query reports window loss. Querying that surface again never resurrects it even if the numeric HWND later becomes live. A zero-area valid window is not lost. Win32 capabilities report identical current/minimum/maximum extents, with both axes normalized to zero when either native axis is empty. Acquire/create/present return OUT_OF_DATE rather than signaling sync or allocating backing for it.

A swapchain receives a separate sticky out_of_date flag after an extent mismatch or empty client rectangle. This flag is distinct from retired: retirement prohibits new acquire but permits previously acquired images to present if the extent remains compatible. OUT_OF_DATE prohibits acquire and present even when the window later returns to the same dimensions. Neither flag frees images; explicit swapchain/device destruction remains the sole image retirement site.

A valid oldSwapchain is retired before recoverable replacement failures, including image creation, registry exhaustion, identity exhaustion, and surface extent/loss errors. Malformed create headers, invalid/stale/wrong-owner old handles, and already retired old handles are rejected without touching an unrelated chain. Backend partial-create failures destroy only successfully created new pairs. Acquired old images and all old owners stay retained until explicit destruction. Output handles and acquire indices remain unchanged on failure in the state module; existing ICD wrappers retain their documented output policy.

## Execution sequence

Create surface validates the native HWND including zero extent, reserves a monotonically increasing ID and publishes the fixed slot. Capabilities, formats and support checks call one common live-surface validation helper. Create swapchain validates the request and old identity, retires the supplied valid old chain, validates current surface extent, allocates new image pairs transactionally, then publishes the replacement slot.

Acquire validates identity and retirement, then validates the current surface and extent before invoking the backend semaphore/fence signal. Failure cannot acquire an image, change its index, or consume a backend signal call. Present validates acquisition, window and dimensions before allocating pixels; after synchronous readback it validates the window and dimensions again. The exact-size sink rechecks extent after GetDC and never stretches to a changed client rectangle. A detected mismatch is OUT_OF_DATE; observed native loss is sticky SURFACE_LOST. Temporary pixel allocations retire on every exit, and acquired DCs always receive a release attempt. Presentation success, OUT_OF_DATE and SURFACE_LOST release the image acquisition while retaining its actual backend image/memory pair. Local pixel-allocation failure before queueing preserves acquisition; device-loss failure retains unresolved owners for the existing abandon path.

## Concurrency and lifetime

The ICD lock serializes registry access and backend callbacks. Native HWND lifetime and application window events remain externally owned; no subclass, WinEvent hook, property, listener cookie, new thread, or persistent native allocation is introduced. Concurrent OS resize can occur after any extent snapshot. Exact drawing uses source dimensions, preventing this API from scaling stale pixels; it cannot freeze application window destruction or provide a cross-thread HWND generation guarantee. Callers must retain their window during presentation. The same-value HWND reuse gap is explicitly not solved by these state checks.

## Failure handling

State allocation failures preserve old image ownership while retiring its acquire eligibility when used as a replacement. Resize or loss before acquire does not signal semaphores/fences. Resize/loss during readback does not submit stale pixels to the native sink. Local allocation failures preserve the acquired image. Every backend readback failure, including OOM and MEMORY_MAP_FAILED, becomes DEVICE_LOST because the callback does not prove whether submission already occurred. Invalid native parameters fail before GetDC. ReleaseDC is attempted on every acquired DC even after a post-acquisition extent mismatch, StretchDIBits failure, ReleaseDC failure, or FIFO pacing failure. Non-Windows native calls fail predictably without accessing the HWND.

## Verification and acceptance

Portable Zig tests exercise normal acquire/present, replacement success/failure/quota/exhaustion, acquired-old presentation, minimized restore, sticky observed loss and out-of-date generations, resize/destroy during readback, and exact image teardown accounting. Existing ICD embedded regression expectations are updated to the Vulkan retirement contract. Native C code is compiled against local test declarations with deterministic fake Win32 API behavior to execute error/DC ownership branches on Linux; those declarations are test doubles, not vendored SDK headers. The same production source and real Windows fixture are cross-compiled against Zig's existing Windows SDK support. Native execution remains untested here.

The native fixture's injected ReleaseDC failure checks a balanced release attempt and propagated error, not OS resource retirement after a failed call. Only real Windows execution can establish GDI/USER count stability.

The ICD consumes batch wait semaphores once before individual presentation. Any local OOM after these waits or an earlier enqueued presentation/rejection becomes DEVICE_LOST and poisons the current binding. A first local OOM with no prior queue effect stops the batch before a later item can invalidate the unchanged-state guarantee; remaining pResults receive the same OOM. Every backend readback failure always becomes DEVICE_LOST because its enqueue boundary is not exposed. This conservative policy requires reinitialization under ambiguous presentation resource pressure. Presentation result aggregation applies DEVICE_LOST, SURFACE_LOST, OUT_OF_DATE, SUBOPTIMAL, SUCCESS precedence independent of list order; no unsupported extension outcome is advertised. Negative invalid-usage codes remain defensive behavior outside valid Vulkan calls.

Run Debug and ReleaseSafe WSI tests, portable C tests with AddressSanitizer/LeakSanitizer/UBSan, owned Zig ASan/LSan instrumentation and allocator checks, and coverage gates >=90% for changed production state/native code. Preserve separate coverage and sanitizer artifacts under build. Record exact commands and evidence in TRACKER.md. No cloud-only result closes roadmap D's real Windows/Wayland or shared RTX acceptance.

The whole-ICD coverage gate also applies to the queue-side WSI changes. Its portable regressions inject transport closure and malformed receiver responses at both GPU-fence creation and polling after shared presentation waits have been submitted. They require unpublished per-chain results, retained image acquisition and semaphore references, a retained pending ticket, and release of only the temporary queue-idle reference. A repeated call after sticky loss must perform no further transport or ownership mutation. First-item local allocation failure is tested with a null result array and an untouched later chain, preserving the complete retryable batch.

Defensive queue-idle lookup tests cover absent, unready, unlisted and already-idling owners, plus a live cache after an empty slot. Failure cannot retire a previously acknowledged wait; success requires both receiver fence creation and polling. These deliberately corrupted registry cases check fail-closed behavior and do not represent valid application inputs. Additional acquire and retirement tests reject missing queues, unknown or foreign device/image identities, and mismatched backing without signaling, publishing an image index, or retiring another owner's resources. A readback with an invalid wait semaphore must destroy its locally recorded staging objects, retain the original image/allocation, leave caller pixels unchanged, and never submit GPU work or export memory.

## Normative references

Reviewed 2026-10-09: [VkSwapchainCreateInfoKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainCreateInfoKHR.html) specifies old-swapchain retirement even when replacement fails and permits presentation of already acquired old images. [SetPropW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setpropw) requires explicit property removal before WM_NCDESTROY; therefore this milestone deliberately does not introduce a property scheme lacking window-event cleanup.

The [vkQueuePresentKHR reference](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueuePresentKHR.html), reviewed 2026-10-09, defines result precedence, unchanged resource/synchronization guarantees for retryable OOM, DEVICE_LOST when that cannot be guaranteed, semaphore execution on presentation rejection, and release of image acquisition. The [Win32 WSI capability rules](https://docs.vulkan.org/spec/latest/chapters/VK_KHR_surface/wsi.html) require exact minimum/current/maximum extents and paired zero axes.


Native lifetime verification fixture (qualification addition): the separate
`wsi_lifetime` workload repeats eight actual device lifetimes. Each owns its
popup HWND, surface, chains, binary semaphores and temporary DCs, with exact
GDI/USER equality and the existing exact process-handle baseline. It renders to
an acquired image before a valid failed extent-mismatch replacement, requires
that replacement to retire the old chain while preserving that acquired image
for presentation, and compares all4096 client pixels against GPU readback.
It checks paired zero capabilities, sticky OUT_OF_DATE with unchanged index,
fresh replacement and exact rendering after restore;80x64 resize capabilities,
old acquire rejection and64x64 replacement/rendering; actual HWND destruction,
SURFACE_LOST before replacement, and retired-chain OUT_OF_DATE precedence after
a valid lost-surface replacement attempt. It claims no unobserved HWND reuse.
All owners are released before starting the next case. The original48-frame
presentation workload, retry bounds and native assertions are unchanged.
Strict integrated Windows cross-build passes (`native_wsi_lifetime_build.log`);
this is compilation only, and actual native/physical execution remains required.

Task #4 +0%; overall remains94%, Time Ended TBD.


Current qualification accounting: portable functional, production coverage,
complete ICD Debug/ReleaseSafe and prescribed ASan/LSan gates pass. The fifth
equal gate is native Windows qualification: compilation plus the applicable
physical/native acceptance already required above. Native WSI transition
matrix and two unmodified full48-frame runs pass, but the prior strict count
failure remains open. A no-Vulkan/no-Waddle system control fails167→168 through
Windows RPC garbage collection. Do not replace the exact assertion with a
count waiver. See ../software-vgpu-slicing/M2_SYSTEM_HANDLE_RECEIPT_2026_10_17.json.
The Windows gate remains unpassed; task #4 stays80%, overall94%, Time Ended TBD.


## Native handle-gate attribution diagnostics (2026-10-21)

This section specifies the passive failure-attribution code added to the native fixture. It adds no acceptance criterion and removes none.

**Scope.** Inside: reporting which kernel handles were added or removed when the exact process handle-count check fails; timing of the
check; reporting a `GetProcessHandleCount` failure. Outside: changing the baseline, the 100 x 10 ms retry rule, the equality test, the
8 lifetimes (2 in smoke mode), the workload, the failure result, or closing/duplicating any handle.

**Architecture.** `tests/vgpu/tcp_gpu_handle_diag_windows.inc` is textually included by `tests/vgpu/tcp_gpu_windows.c` after `native_handles()` and before
`handle_baseline()`. It needs the includer's `symbol()` helper (pinned by the native test's make recipe to be byte-identical). Host test:
`tests/vgpu/tcp_gpu_handle_diag_host.c` over `tests/vgpu/win32_fake/handle_diag_fake.h`. Native test: `tests/vgpu/tcp_gpu_handle_diag_native.c`.

**Data structures.** `handle_check_t {samples, minimum, maximum, last}` passive statistics of the compared samples. `handle_diag_entry_t {handle_value,
type_index, granted_access}`; `handle_diag_set_t {entries (owned malloc array), count, unavailable[96]}`; `handle_diag_t {frequency, start, query_process,
query_object (borrowed ntdll exports), baseline (owned snapshot)}`. Wire mirrors (x64, `_Static_assert`ed): `PROCESS_HANDLE_SNAPSHOT_INFORMATION` header
(2 x `ULONG_PTR`) followed by 40-byte `PROCESS_HANDLE_TABLE_ENTRY_INFO` entries. Information classes: `ProcessHandleInformation = 51`,
`ObjectTypeInformation = 2`. Capacities: initial buffer 32 KiB, maximum 16 MiB, 6 attempts, 1 KiB type buffer, 48-character type name.

**Sequence.** (1) `main()` takes the unchanged baseline sample. (2) `handle_diag_begin()` records the QPC origin, resolves `NtQueryInformationProcess` and
`NtQueryObject` from the already-loaded ntdll (`GetModuleHandleA` + `GetProcAddress`, no new link dependency) and copies the process handle table. (3) Each
lifetime runs the unchanged workload and teardown; then the unchanged `handle_baseline()` runs, additionally recording each compared sample. (4) On success one
`handle_diag:` timeline line is printed after the existing PASS line. (5) On failure the legacy line `Native GPU handle baseline=%lu actual=%lu` is printed first
(unchanged), then `handle_diag_report()` prints samples/min/max/last and elapsed time, takes a second private snapshot, and lists every handle present in one
snapshot and absent in the other with value, type index, type name and access, then a summary line. (6) `handle_diag_free()` releases the baseline snapshot on every exit path.

**Concurrency.** Single fixture thread; no shared state beyond the caller-owned `handle_diag_t`. The queries read kernel state and open, duplicate and close nothing.

**Failure handling.** Any unavailable step prints `handle_diag: snapshot unavailable (<reason>)` and continues; no diagnostic path changes the exit status. A handle closing
between snapshot and type lookup only yields type `unknown`. Object names are never queried (name queries can block on pipes).

**Verification.** Host: 18 cases / 6724 checks, plain and ASan+LSan+UBSan, LeakSanitizer self-control exits 23, gcov 100% lines and branches of the `.inc` and mutation testing (38 mutants, 35 killed; both measured manually, no make target), exact-text
pin of the acceptance rule (a literal-edit guard only; the git diff is the real proof). Native (Windows 11 build 26300 guest, WinRM and interactive): 7/7 cases (layout, passivity, added types
Event/Semaphore/Mutant/Timer/File/Section/Job/Thread with exact names, removed handle, report shape, unknown type, timing). **Evidence limit (updated later on 2026-10-21):**
the diagnostics have since run inside 20 full 48-frame `triangle_present` and 10 `wsi_lifetime` native workloads (next subsection); earlier native receipts
(`M2_NATIVE_RECEIPT_2026_10_17.json`, `NATIVE_RECEIPTS_2026_10_18.json`, `M2_SYSTEM_HANDLE_RECEIPT_2026_10_17.json`) describe the old fixture hashes only. Over those 30 runs every compared
sample was the first one and the slowest retirement check was 1 ms (no first-use or long-run side effect was observed); the class-51 growth path and WOW64 were not exercised. Audit, decisions and the
`StretchDIBits` probe: [M2_HANDLE_GATE_AUDIT_2026_10_21.md](M2_HANDLE_GATE_AUDIT_2026_10_21.md).

### Source-bound native evidence on the committed fixture (2026-10-21)

**Scope.** Inside: native repetitions of the unchanged strict process-handle gate with the diagnostics compiled in. Outside: any change of the rule, the baseline point, the retry budget,
the workload, the frozen guest inputs or the supervisor; any acceptance credit; any attribution of the preserved `tcp_gpu_7adb6000` failure.

**What ran.** The byte-identical copies of the 2026-10-17 supervisor (`tcp_gpu_actual.py`, `tcp_gpu_owned_session.py`, `type_command_immediate.py`; hashes in the receipts) drove, per repetition, the frozen
guest ICD, bootstrap, loader, host worker, runtime and controller (hash-equal to the two qualified 2026-10-17 runs); only the guest fixture executable differs: `make build/vgpu_tcp_gpu_windows_draft.exe`
from the committed sources (sha256 `77a334150077b0702c0a4144124b760871ec2e1bb41a7967cc774139773bb66d`, 317,440 bytes, 8 `handle_diag:` strings, the legacy failure text unchanged). Environment: the existing
Windows 11 VM booted from its preserved disk (never provisioned or reset), the interactive console session launched through Win+R key events (Codex signed in through QMP using the saved test-account credential at the user’s explicit request; auto-logon stayed off), medium
integrity, the physical RTX 5080 through the host Waddle worker. A repetition passes only if the supervisor's own assertions pass and the runner's independent check agrees: guest exit 0, the exact
output shape of the workload, no legacy handle-failure line, guest binary hashes verified after the run, and no live controller, worker or guest process afterwards.

**Results** ([triangle_present receipt](../software-vgpu-slicing/M2_INSTRUMENTED_TRIANGLE_PRESENT_RECEIPT_2026_10_21.json), [wsi_lifetime receipt](../software-vgpu-slicing/M2_INSTRUMENTED_WSI_LIFETIME_RECEIPT_2026_10_21.json)).

| Workload | Runs | Passed | Baseline | Lifetime checks equal to the baseline | Last retirement check after the baseline | Per lifetime |
|:--|--:|--:|--:|:--|:--|:--|
| `triangle_present` (48 frames, 24 FIFO + 24 IMMEDIATE, 8 lifetimes) | 20 | 20 | 199 in every run | 160 of 160, each on the first sample | 55.06-55.80 s (median 55.34 s) | 6.82-7.39 s |
| `wsi_lifetime` (8 lifetimes, 32 labeled cases) | 10 | 10 | 199 in every run | 80 of 80, each on the first sample | 38.12-38.78 s (median 38.44 s) | 4.72-5.11 s |

**Meaning and limits.** These are repetitions of the unchanged oracle and grant no credit. They do not replace or attribute the preserved 199 to 200 failure and, as recorded in the audit, repeated passes
do not waive it. Duration is not discriminated by these runs: on the like-for-like controller-ready to host-result clock the new `triangle_present` runs took 65.1-68.6 s versus 66.4-68.5 s for the earlier passes (the failed `tcp_gpu_7adb6000` took 81.8 s), and the earlier `wsi_lifetime` run `tcp_gpu_683c0ee2` took 71.1 s (new runs 48.1-48.9 s) and passed. Only the failed run was clearly slower; whether slowness explains it is an untested hypothesis. A slowed-host diagnostic was considered; one attempt to restrict the VM process's CPU affinity was blocked by the harness permission policy and was not pursued (no user approval was given for it).

`wsi_lifetime` exercises the changed destroyed-HWND ownership path of `tcp_gpu_wsi_lifetime_windows.inc`; the earlier native pass (`tcp_gpu_683c0ee2`) used the old fixture.

**Reproduction (this host, same lease rules).** `make build/vgpu_tcp_gpu_windows_draft.exe`; start the loopback publisher `build/qualification_20261017/public_binaries_resume.py` (serves `build/windows11_vm/repair_media` on
127.0.0.1:55986); then `python3 -B build/qualification_20261020/m2_runner/run_m2_instrumented.py --repetitions N` or `python3 -B .../run_m2_instrumented_wsi.py --workload wsi_lifetime --repetitions N` (never `python -O`).
The runners are git-ignored evidence tooling; their hashes are in the receipts. They refuse to start unless the VM, lease, console session and frozen inputs check out.

## Independent Codex review: snapshot capacity correction (2026-10-21)

The diagnostics' allocation ceiling is 16 MiB, not a ceiling of 8 MiB on the kernel's required-size hint. On a length mismatch, a hint above 16 MiB fails without allocation; otherwise growth doubles the greater of the current capacity and hint, saturating at 16 MiB. A mismatch after actually trying that maximum fails immediately. The existing six-query attempt bound remains. Host regressions cover hints at 8 MiB, 8 MiB plus one byte, exactly 16 MiB, 16 MiB plus one byte and ULONG_MAX, and repeated mismatch at the maximum. Every successful or failed path releases private buffers. No acceptance assertion, baseline point, workload, production source or prescribed threshold changes. The 30 earlier native receipts remain evidence of the earlier diagnostic source; fresh execution is required to bind this correction.

The native diagnostics test additionally creates 1,024 private manual-reset events, captures the real class-51 handle table, and requires more entries than fit in the initial 32 KiB wire buffer. It checks that every acquired event occurs in the grown snapshot with EVENT_ALL_ACCESS. Cleanup visits all 1,024 owner slots on success and on every failure, frees both snapshots, and requires the original exact process handle count. This exercises real kernel buffer growth without creating hundreds of thousands of objects merely to reach the allocation ceiling; the ceiling boundaries remain host-injected tests. This case adds no warm-up or acceptance change to the GPU fixture.


The independent verification of the capacity correction and native growth regression is recorded in [M2_CODEX_REVIEW_2026_10_21.md](M2_CODEX_REVIEW_2026_10_21.md) and its [machine-readable receipt](M2_CODEX_RECEIPT_2026_10_21.json). All prescribed portable gates, eight native Windows diagnostics cases and both fresh physical GPU workloads pass. The historical count failure remains open; the alternative attribution protocol described there is an unapproved proposal and changes no current acceptance criterion. M2 remains 94%, Time Ended TBD.


## Prospective ordered guest-output qualification gate (2026-10-21)

`scripts/vgpu_m2_output.py` is the tracked, read-only final guest-output gate for
new full `triangle_present` and `wsi_lifetime` qualifications. Every final native
repetition must pass this command in addition to the existing supervisor and
independent host/source/binary/process-owner verification:

```sh
python3 -B scripts/vgpu_m2_output.py --status /absolute/run/guest_status.json --workload triangle_present --device 'NVIDIA GeForce RTX 5080'
python3 -B scripts/vgpu_m2_output.py --status /absolute/run/guest_status.json --workload wsi_lifetime --device 'NVIDIA GeForce RTX 5080'
```

Use the corresponding workload command on each run's final collected status.
Save its JSON stdout and exit code in new qualification evidence, bind the
reported status/verifier SHA-256 hashes in the final review receipt, and require
exit zero. A previous ignored replay driver's pass alone cannot close this
check. The command never starts a guest, accesses the VM, changes a receipt,
modifies historical runners, or accepts M2. Synthetic regression success and
read-only checks of historical outputs do not qualify a new binary.

**Input and ownership.** The input is the supervisor's UTF-8 JSON object with
string `exit`, `stdout` and `stderr` fields. Duplicate JSON keys, invalid JSON,
non-string or missing output, NUL output, and any exit other than the literal
string `0` fail. The caller supplies the exact workload and device name. The
verifier reads the file once, hashes those same bytes, and owns only ordinary
Python parsing/storage objects until process exit. It has no subprocess,
network, device, persistent state, output-file or native resource ownership.
Parallel invocations have independent local state. Python is existing test and
qualification tooling; no runtime dependency or production parser is added.

**Sequence.** The verifier requires exactly one nonzero authenticated session
header for the selected workload/API 1.0 and one positive handle baseline before
workload events. It then requires lifetimes 1 through 8 in order, with the exact
requested device on every lifetime line. Before each triangle lifetime line,
exactly FIFO frames 1, 2, 3 then IMMEDIATE frames 1, 2, 3 must appear. Before each
WSI lifetime line, the four existing case labels must appear exactly once in
the fixture's order (failed replacement, zero area/restore, resize, destroyed
HWND). The final retirement-Ack PASS must occur exactly once and be the last
nonempty stdout line. CRLF, LF and CR line endings are supported. Unknown
ordinary informational lines are allowed between required events. Recognizable
but malformed/embedded acceptance markers, missing/extra events, wrong labels,
reordered frames, wrong workload, duplicate lifetime numbers and acceptance
markers on stderr fail. Known fixture failure markers in either stream fail,
including markers embedded in loader output. Native snapshot availability and
timing remain passive diagnostics and do not become new native acceptance
criteria; no count rule, baseline, retry budget, workload or threshold changes.

**Errors and verification.** Exit 0 means only the guest-output shape passed;
exit 1 means input/read/validation failure; invalid command arguments exit 2.
The JSON result explicitly limits its scope to guest output. Validation uses
explicit exceptions, not Python assertions, so `python -O` does not bypass it.
`make vgpu-m2-output-test` runs nine test groups: exact valid workloads, every
required event's deletion/duplication/adjacent swap, count-preserving lifetime
and frame mutations, cross-workload contamination, malformed markers,
contradictory failures in both streams, schema/header/final-line errors, passive
diagnostics, and the real CLI under normal/optimized Python with malformed and
duplicate-key JSON. Temporary test files have a scoped owner and are removed
on every test exit. These are output-verifier tests, separate from production
memory sanitizer and coverage gates.

Historical review already verified all actual 32 outputs had the correct
ordered lifetime sequence. This prospective gate closes future false acceptance
in count-only replay checks; it does not invalidate or rewrite those receipts,
attribute the historical failure, or award the remaining M2 qualification weight.
