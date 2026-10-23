# M2 owned-resource qualification contract, 2026-10-21

The acceptance owner explicitly approved **“Approve M2-specific ownership gate
(Recommended)”** on 2026-10-21. This prospectively replaces process-wide kernel
handle-count equality as the M2 retirement oracle with the bounded owned-resource
checks below. This is a measurement-contract change, not evidence that the old
strict test passed. The original `tcp_gpu_7adb6000` failure and all other historical
failures remain failed under their original protocols and unattributed where
identity evidence is absent. Existing raw receipts, reviews and runners remain
unchanged. The prior proposed attribution-aware classification scheme is not
adopted; this contract observes explicit Waddle owners instead.

**Current status: implemented and qualified; M2 is100% / Completed.**
See [final closure](M2_CLOSURE_2026_10_21.md) for Time Ended, applicable green
CI, fresh native evidence and independent final review. At this document's
initial commit, implementation and qualification were pending and only strict
fixture mode existed. No old count-only output satisfies the ownership contract.
Later pending/percentage statements below retain their historical checkpoint scope.
The completion gate is passing applicable CI on the integrated candidate, fresh
native/physical qualification and one independent final review, with linked evidence.

The exact approved proposal is reproduced at the end of this file. Its original
“not adopted” title and final paragraph are retained as historical source text;
the direct human approval recorded here adopts its bounded scope prospectively.
The source proposal SHA-256 is `865bc01e44e8b0cd06c8e3d2fb94e03d9d39bd846a6f6f2e438a25636fd52bb1`; the direct approval record
SHA-256 is `87cbd83f7f4845e4ecb54299e933d13b586dcc7e5ba1ddcbb13ef85ecf7712e7`. Durable source records and previously reviewed
readiness evidence are in
[`pre_ownership`](../../build/qualification_m2_astra_20261021/pre_ownership/),
whose root-owned `archive_manifest.json` binds the archived files. These are local
qualification artifacts, not newly claimed portable or native results.

Codex Astra audited the actual source at readiness commit `3736237`, following
`17dc2d0` (ordered output gate) and `e17425b` (CI wiring). The approved owner table
and implementation boundaries were technically reviewed by the coordinating
Codex lead before any ownership implementation. This document records that
review, not an independent final acceptance review.

## Audited owner inventory

| Resource / identity | Acquisition and exclusive owner | Retirement / failure semantics | Existing evidence and proposed native observation |
| --- | --- | --- | --- |
| Surface slots: monotonic surface ID, instance ID and borrowed HWND bits | venus_wsi.create_surface; caller-owned fixed 16-slot state under ICD mutex | destroy_surface requires exact parent and no remaining chain; clears slot. Destroyed HWND marks sticky loss but is never destroyed by WSI. Failed/foreign operations preserve state. | Existing Zig registry/lifetime tests cover stale/foreign IDs, quotas, teardown, resize/loss. Qualified native snapshot must find every surface slot empty after each application lifetime; fixture ledger separately proves its HWND retirement. |
| Swapchain slots and acquired-image bits: monotonic chain ID, device, surface | create_swapchain publishes only after complete image construction; fixed 16 slots and at most 3 image/memory pairs per chain | A valid old chain becomes retired before replacement construction can fail. Failed partial construction destroys exactly created pairs. destroy_swapchain/device transfers pairs to backend retirement, then clears slot. Local pre-enqueue OOM retains acquisition for retry; resize/native loss releases acquisition; uncertain backend completion retains ownership and becomes DEVICE_LOST. | 12 WSI tests include exact backend identities, replacement rollback, quota, resize/loss, allocator failure. Native snapshot checks chain/image/acquired slots individually are empty; next-ID progression demonstrates registry activity. |
| Backend VkImage/VkDeviceMemory pairs: private registry IDs and exact bound_memory | venus_icd.wsi_create_image creates image, selects memory type, allocates and binds; only returns the complete pair | Constructor failures roll back owned members. wsi_destroy_image validates device/image/exact memory, marks retirement. retire_wsi_images waits for image views and GPU references and acknowledged destruction/free. Lost transport retains registry reservations until trusted receiver retirement and abandon. | Existing ICD tests verify transactional constructors, mismatched/foreign pairs, image/view/GPU retirement ordering. Native snapshot requires no object IDs, resource IDs, retired WSI images or bound memory owners after each successful lifetime. It must reject lost state, not count abandon as ordinary retirement. |
| Temporary BGRA/RGBA readback pixels | venus_wsi.present allocator.alloc; only synchronous caller owns slice | defer allocator.free covers backend failure, resize/loss and sink result. No callback may retain slice. Preallocation OOM leaves acquired image retryable. | std.testing.allocator, production-function ASan/LSan and >=90% coverage remain mandatory. This stack-local slice is NOT claimed as directly observed by native snapshot; native exact pixels prove execution, allocator/sanitizer gates prove this ownership boundary compositionally. |
| Readback staging buffer, memory, command pool/buffer | venus_icd.wsi_readback owns private registry IDs from successful creation; deferred cleanup in reverse dependency order | Queue idle proof precedes map/copy; failures before submission release partial constructors. Unknown completion retains real owner records; device loss and trusted abandon rules apply. | Existing staging-failure, invalid-wait rollback, readback bytes and queue-loss tests. Native snapshot requires the actual shared registry/resource owner records empty, independently of process counts. |
| Mapped shadow bytes and baseline, mapping slot/resource ID | map_memory owns two allocator slices and an exported mapping reservation, recorded in resource_state | unmap/release_shadow frees CPU slices; free_memory releases exported mapping after acknowledgment; failure retains owned reservation rather than pretending retirement. | Existing allocator/sanitizer and mapping tests; native snapshot checks pointers and mapping-resource slots, not only object live_count. |
| Queue submission tickets, wait semaphores, fences, idle_refs and inflight_count | queue-submit owns ticket ID/queue/fence and per-resource reference bitmap; shared present waits submitted once | retire_ticket decrements exact references after acknowledged queue/fence completion and destroys internally owned fence. Ambiguous submission/proof returns device loss and retains ticket/references until trusted abandon. | Existing shared-wait and corrupt/closed proof tests check retained IDs, acquired flags, semaphore refs and zero idle_refs. Native snapshot checks ticket records and reference fields are empty after each lifetime. |
| Copied descriptor-layout, pipeline-layout, pipeline, descriptor-set and command profiles | profiles.reserve_slot publishes an occupied flag in one of five independently bounded tables (32/32/64/128/64 slots); the owning resource records a one-based index | release_slot scrubs the profile and clears occupied after acknowledged native retirement or safe constructor rollback. release_command_profile also resets the corresponding graphics recording; uncertain transport retains the reservation. | Native query scans all 320 occupied flags independently of object/resource IDs, plus copied command pipeline/set references. Portable controls reserve/release every actual table and retain last-slot orphan owners with objects/resources zero. |
| Graphics recording and orphan cache/dependency tokens | command-owned recording contains pipeline token/format, active pass format and dynamic-rendering flag; parent caches retain physical/queue tokens and queue/ring-ready markers | Acknowledged command reset/retirement resets recordings; instance/device retirement clears cache. Resource view_image can independently prevent image destruction, even with missing resource ID. | Native query scans all recording fields, cached physical/queue tokens, ring/ready markers and explicit resource dependency/profile links. Orphan parent cache ownership raises inconsistency; no inferred cleanup. |
| Temporary fixed descriptor allocation/update staging and wire arrays | exclusively borrowed, reused transaction buffers under the ICD mutex; no occupied/free-slot reservation | Existing defer scrubs allocation/update snapshots and buffer/image/texel wire staging on every return. They never reserve persistent capacity or acquire native owners. | Existing allocator/state/rollback and staging-scrub assertions remain compositional evidence. Quiescence does not scan arbitrary scalar scratch bytes or claim heap-wide visibility. |
| Instance/device/queue/private Vulkan registry owners and extension-cache allocations | fixed object slots with monotonic namespace IDs; caches owned by the matching acknowledged parent | destroy_device refuses pending tickets/refs/nonqueue children; acknowledges host destruction before releasing queues/device. destroy_instance releases acknowledged children and extension-cache slices. | Existing actual dispatch tests and independent final host Ack. Native snapshot scans real slots and cached allocations under the existing mutex; final bootstrap stop/unbind remains mandatory. No per-lifetime unbind/rebind is introduced. |
| Production presentation DC: exact acquired (HWND,HDC) and monotonic local acquisition ID | venus_win32_present.c GetDC; synchronous presentation thread under ICD lock; no retained native allocation otherwise | Every successful GetDC has one ReleaseDC attempt, including post-acquisition resize/loss and draw failure. Only successful ReleaseDC proves retirement; failure propagates and must leave a live ledger owner. DwmFlush occurs after release. | Existing API doubles prove attempts/error propagation, not actual OS retirement. Add qualification-only fixed identity ledger around the actual APIs plus native deliberate leak/failed-release/masked-release controls. |
| Fixture window and pixel-inspection DC owners | the existing present and WSI lifetime fixtures CreateWindowExA/GetDC; fixture owns HWND while production only borrows it | Successful DestroyWindow/ReleaseDC retires exact identity; error paths retain a ledger entry and fail. Destroyed-window test clears its owner only after success. | Keep existing native GDI/USER equality and pixel assertions. Add local fixed fixture ledger so an unrelated retirement cannot mask one missing owned DC/window. Initialization before the lifetime loop remains unchanged and is checked separately. |
| Binding-lifetime bootstrap socket, Winsock runtime, module reference and configuration file | tcp_bootstrap_windows.c owns config file until CloseHandle, one private module reference, fixed client/socket; venus_tcp_socket owns socket/WSAStartup and temporary BCrypt algorithm | release uses CloseHandle/FreeLibrary/client release; failed config close is retained in pending_config for stop; stop requires unbind and acknowledged session retirement; abandon only after trusted host retirement. No new M2 thread/event/kernel handle acquisition found. | These predate and span the eight M2 lifetimes. Existing bootstrap/transport tests and final exact host Ack/stop/session0/module release checks remain required; the new per-lifetime check does not assert they vanish before stop. No observer added to unrelated transport/vendor paths. |
| Diagnostic snapshots | handle_diag owns bounded malloc snapshots of borrowed handle metadata; no duplicate or close of inspected handles | all snapshots freed on success/failure; capacity/query bounds unchanged | Existing 18 cases/6779 checks, sanitizer negative control, 100% included-real-source coverage and 8 actual Windows tests remain. Process totals/snapshot deltas continue as diagnostics in the new protocol. |

No unaccounted Waddle-owned kernel acquisition was found in the changed M2 production WSI paths. Process-wide Windows/loader/vendor resources remain outside the approved narrowed claim; a DLL name is not ownership evidence.

## Approved implementation boundaries (implementation in progress)

1. Preserve the approved proposal verbatim as a tracked adoption appendix with the human answer/date and source hashes, explain prospective effect, and synchronize M2/parent docs. Record unit3 commit 3736237 at +0%. Existing historical audit/review/receipt files remain untouched.

2. The qualification-only C DC observer wraps the actual GetDC/ReleaseDC calls when an explicit build macro is enabled. Fixed storage, no allocations/threads/native resources, monotonic IDs, exact HWND/HDC matching, sticky overflow/invalid-release/failure state. Release success retires that exact acquisition; failed release does not. A full/faulted/exhausted ledger must reject before another native acquisition, or immediately release the newly acquired exact DC while retaining a sticky failure; it must never silently lose a successful acquisition. Ledger access is serialized by the existing ICD lock for production calls and the sole fixture UI thread for controls; no observer thread is introduced. Query is read-only. Ordinary builds retain their current APIs/code path and exports. The qualified DLL exposes only read-only observer snapshots; fault injection exists only in the standalone control executable, never the qualified DLL.

3. A qualification-only ICD read-only snapshot runs under lock_icd, scanning existing fixed owner arrays. It reports protocol/version/size, WSI next ID, object next ID, live surfaces/chains/pairs/acquired images, object/resource IDs, mappings/owned cache allocations, tickets/references, first retained identity and command/lost state. It verifies slots as well as live_count (count/slot inconsistency fails). It neither clears state nor unbinds, rebinds, retries or retires anything. Export exists only for explicitly qualified builds. Existing allocator/backend identity tests supply the temporary-slice and exact release proof; this is a quiescence query, not a general tracing/debug framework. Normal production DLL remains uninstrumented.

4. The planned explicit fixture mode argument m2_owned_v1 is valid only for triangle_present and wsi_lifetime. The existing 8-argument invocation remains strict count equality with unchanged handle_baseline implementation and budget. New mode requires both read-only probe exports from the exact loaded private ICD, rejects absent/version/size mismatches and nonempty initial owners, and checks per-lifetime ownership after device/instance destruction before printing that lifetime PASS. The new mode samples total handles and captures differential snapshots diagnostically; count mismatch alone does not abort. Required GDI/USER, frames/cases, output pixels, final stop/session0/host Ack remain unchanged. No workload restart or new warmup.

5. Each lifetime emits one structured fixed-shape m2_owned_v1 record bound to the existing session, workload, lifetime number, monotonically advancing owner ID high-water marks and DC acquire/retire statistics. All observed owner categories must be zero with healthy command/transport and no observer faults; actual DC activity must be nonzero. Native fixture HWND/DC owners are separately included. Missing, duplicate, out-of-order or contradictory evidence fails the tracked verifier. An explicit --protocol selector distinguishes m2_owned_v1 qualification from legacy strict-count shape verification; old count-only output cannot satisfy the ownership protocol. Final verifier report names the compositional limits and required external evidence, not blanket acceptance.

## Controls and verification

- Portable ledger tests: exact identity, duplicate/foreign retirement, retained failed release, capacity and ID overflow, unrelated release cannot cancel a live owner; sanitizer/coverage gates with >=90% lines and branches and no exclusions.
- ICD query tests use actual fixture creation/teardown and queued-reference/loss paths; live registry/WSI owner and inconsistent count/mapping/ticket evidence must reject. Null/wrong ABI input and missing probe reject. Existing full coverage threshold unchanged.
- Actual Windows standalone controls link the real production C sink with the same observer. Positive case acquires/releases a real DC. Negative cases deliberately omit its actual release while reporting success to the sink, simulate cleanup failure while retaining the exact DC, and retire an unrelated acquired DC while the leaked sink acquisition remains. The ledger must reject each before controlled cleanup. Process-count equality is measured in the negative case to demonstrate rejection despite an unchanged scalar total; if the native platform cannot establish this bounded control, it reports failure/inconclusive, not invented evidence or a new prewarm. Only DCs acquired by the control are released during cleanup. No arbitrary borrowed CloseHandle.
- Portable verifier controls exhaust missing/duplicate/order/malformed/nonzero-owner/stale-version/protocol-confusion cases and contradictory failures. Existing strict regression controls remain intact.
- Cross-build qualified DLL, standalone Windows controls and GPU fixture; inspect exports to prove default DLL has no observer/fault entrypoints and qualified DLL has no fault entrypoints. Cross-build is NOT execution.
- CI executes portable gates and actual Windows standalone controls with bounded timeout, raw stdout/stderr and true exit code preserved. Root alone executes physical GPU workloads, loaded-host case if permitted, and final Windows diagnostic controls.

## Implementation sequencing and evidence limits

The atomic units are contract/owner documentation; actual C-boundary observer and
portable/native controls; conditional read-only ICD quiescence query and qualified
build; and explicit fixture/verifier protocol with CI. Each completed unit receives
its own reviewed commit and +0% tracker attribution until qualification completes.
Exact ABI fields, output grammar and executable commands are documented alongside
their implementation before a final candidate is presented. This initial contract
does not claim those interfaces already exist.

The minimal ICD query is required because final unbind checks ownership only after
all eight lifetimes and does not inspect the separate WSI slots. It must scan actual
slots and counts, retained mappings, tickets and references under the existing lock.
It may not clear state, retry cleanup, unbind or change an ownership transition.
Initial state, ABI/version/size, owner-ID progression and absent/stale probe exports
must all have fail-closed controls. The ordinary DLL has no qualification exports;
the qualified DLL has no fault-injection entrypoints. Only standalone controls may
inject failure. Physical/native execution belongs to the coordinating lead.

No generic NT tracing, OS-module allowlist, prewarming, inflated baseline, widened
retry budget, borrowed-handle closure, reduced workload, weakened sanitizer/coverage
floor, or unrelated vendor change is authorized. Temporary pixel allocation ownership
is proved by the existing allocator/sanitizer boundary and is explicitly not visible
to the native quiescence query. GDI/USER aggregate counts remain required supplemental
checks and cannot substitute for the exact identity ledger.

## Implemented C-boundary observer and controls (unit B)

`m2_owner_ledger.h/.c` and `m2_ownership_probe.h/.c` now implement the exact native
identity boundary. `src/vgpu/venus_win32_present.c` selects those wrappers only
when `WaddleM2OwnershipProbe` is defined; otherwise the Win32 source preprocesses
identically to its pre-observer form. The ordinary native object has no probe
references. The qualified observer object is built without
`WaddleM2OwnershipFaults`, and its only explicit Windows export directive is the
read-only `venus_m2_dc_snapshot`. Its three externally linkable C definitions are
exactly acquire, release and snapshot; the native build gate rejects any extra
(including test-fault) definitions. The qualified ICD DLL and per-lifetime fixture
protocol are not yet implemented by this unit.

The caller exclusively owns a 32-entry fixed ledger. The ICD lock serializes
production sink calls; native and portable controls use a sole UI thread. A
successful reservation allocates a never-reused nonzero 64-bit ID before GetDC.
Full capacity, an exhausted counter, invalid parent/class or an earlier sticky
failure reject before another native acquisition. Failed GetDC cancels only its
reserved empty slot. A successful native result remains in that reserved slot,
even if it duplicates an existing identity. In that exceptional duplicate case,
the wrapper attempts actual ReleaseDC for only the new acquisition's exact ID;
a failed release remains recorded and acquisition stays blocked. No success is
left without a slot.

Release first resolves exactly one matching resource class, HWND and HDC.
Unknown/ambiguous identities never reach ReleaseDC. The actual API result
controls retirement: zero preserves the exact ID/native bits and records a sticky
release failure; nonzero clears that one entry and increments retirement. Cleanup
may retire proven owners despite a sticky error, but cannot make the qualification
snapshot clean by erasing the error. Window identities use the same ledger type
for the forthcoming fixture instrumentation; the production observer never owns
or destroys HWND. No allocation, thread, kernel handle or general OS snapshot is
introduced by the ledger itself.

The snapshot ABI is exactly 72 bytes, compile-time asserted, with these fields in
order: u32 `version=1`, u32 `bytes=72`; u64 `next_id`, `acquired`, `retired`,
`first_live_id`, `first_live_parent`, `first_live_value`; u32 `live`, `pending`,
`failures`, `reserved=0`. `venus_m2_dc_snapshot(version, output, bytes)` requires
version 1, a nonnull distinct writable output and exact size; mismatch returns
zero without touching output. A successful query scans entries, counts pending
reservations and live identities and detects inconsistent counters/empty slots.
It transfers no owner and performs no native call. A clean decision requires the
valid ABI, no live/pending identity, no failure, zero first-live fields/reserved
field, balanced successful acquire/retire counts and `next_id > acquired`.
The final fixture must additionally require nonzero per-lifetime activity and
monotonic high-water marks; an initial empty snapshot is not lifetime evidence.

Four portable ledger groups (2,720 checks) exercise 128 repeated cycles, exact
identity, unrelated release, failed/foreign/duplicate retirement, acquisition
cancellation, capacity/counter exhaustion, malformed state and ABI. Three
portable observer groups (759 checks) call the actual C sink and observer over
Win32 API doubles, including the native ReleaseDC return path, omitted release,
injected failure, actual API-double cleanup failure, foreign release, capacity
rejection before GetDC and duplicate native results. These assertions remain
active under optimization. ASan/LSan/UBSan reports zero owned leaks/errors.

`make vgpu-m2-ownership-coverage` first validates malformed coverage evidence in
four groups, then measures each actual ledger/observer source independently at
>=90% lines and directional branch outcomes, with no exclusions. All standalone
control code compiled into that observer is included in the denominator. The
latest local result is ledger 85/85 lines and 143/144 outcomes (99.31%); observer
35/35 lines and 26/28 outcomes (92.86%). Unique retained directories under
`build/coverage/vgpu/m2_ownership/` contain binaries, objects, gcno/gcda, compressed
gcov JSON and source/raw-report hashes. Empty/missing/duplicate/unexpected source
records, zero denominators, malformed counters or either individual floor fail.
Existing default WSI tests, allocator/sanitizers and coverage also pass unchanged:
12 Zig cases; 145/156 Zig branches (92.95%) and 71/72 C branches (98.61%).

`make vgpu-m2-ownership-native-build` cross-builds
`build/vgpu_m2_ownership_native.exe` and the separate fault-free observer object.
It does not execute Windows. The standalone executable has four ordered cases:
`positive`, `owned-leak-equal-total`, `injected-release-failure`, and
`unrelated-release-masks-total`. Every case invokes the actual production C sink
on a real owned HWND. Negative cases retain the sink's actual acquired DC; one
returns success despite omitting release, one injects the failed-release outcome,
and one additionally releases a separate DC actually acquired by that control.
The exact-owner decision must reject before cleanup even when measured total
kernel counts are equal. There is no warmup/retry/baseline adjustment; an unequal
negative-control total fails the control and must be reported. The injected
failed-release outcome is not claimed as a naturally occurring Windows API
failure. All cleanup uses actual ReleaseDC on only those acquired identities and
requires success, balanced identities and preserved GDI/USER counts before the
owned window is destroyed. Failed cleanup remains a failing test.

These four Windows cases are **NOT RUN** in the isolated implementation worktree.
Root-owned native execution and later CI must preserve their real output and exit
status. Cross-compilation and portable API doubles grant no native acceptance.
The source-bound unit receipt is
[`ownership_observer/receipt.json`](../../build/qualification_m2_astra_20261021/ownership_observer/receipt.json).
M2 remains 94%, parent 85.5%, Time Ended TBD.

## Exact approved proposal (historical source text)

```text
# Proposed bounded M2 qualification amendment — not adopted

Decision requested: replace process-wide kernel-handle count equality as a mandatory M2 acceptance oracle with qualification of resources actually owned by the bounded WSI implementation. This deliberately narrows what the M2 native gate claims; it does not attribute or pass any historical failure. Zero owned leaks, the 90% coverage floor and existing functional assertions remain required.

## Evidence and limits

Codex independently read the additional raw guest statuses: tcp_gpu_520be84f exits 1 with 199→198 after 42 frames / six completed lifetimes; tcp_gpu_0da46e11 exits 1 with 199→200 after 24 frames / three completed lifetimes. The latter retains repeated count-200 snapshots. The resolved trace for fe13e873 includes RPCRT4 creation frames for the Thread/Event pair. These support an OS-churn limitation of process totals. They do not identify the original 7adb6000 handle, prove all additional handles OS-owned, or exclude masked owned leaks. Fable's claim of 30 identity-checked passing baselines is incorrect: successful checks did not snapshot identities at every retirement.

The original 7adb6000 failed result and every earlier failed result remain preserved and failed under their original protocols. No later run can recover the missing identity. No speculative production correction is authorized by these facts.

## Proposed scope and required evidence

M2 covers synchronous Win32 WSI resize/loss/replacement and image retirement, including queue ownership transitions. It does not qualify every Windows/Vulkan-loader/vendor cache or the separate OpenCL/DXVK feature gates.

1. Before changing the oracle, record a complete resource-owner table for the actual changed M2 paths: bounded registry slots/IDs, swapchain backend image-memory pairs, temporary readback pixels, queued references/waits and borrowed HWND lifetime, and every GetDC/ReleaseDC pair. Audit all normal, error, resize, destroyed-window, replacement and device-loss paths. Any unaccounted Waddle-owned kernel resource blocks the amendment's implementation and needs a concrete ownership check.
2. Keep or add explicit ownership assertions with identities and balanced acquisition/retirement, rather than merely a total. Retain allocator/ASan/LSan checks for owned allocations, backend create/destroy accounting, native GDI/USER checks, pixel comparisons, sticky-loss and replacement behavior, ordered eight-lifetime workloads, both present modes and host retirement. Do not close borrowed OS handles or treat an OS DLL name/direct API caller alone as proof of ownership.
3. Validate the changed ownership gate with deliberate owned-resource leak and cleanup-failure negative controls, including an offsetting unrelated release where relevant. Missing/incomplete ownership evidence fails. At least one control must exercise actual Windows WSI resource acquisition/retirement, not only a mock.
4. Continue recording process-wide handle counts and snapshot deltas for diagnosis. A global-count mismatch alone no longer determines M2 pass/fail once the explicit owner checks are established. Do not remove the old strict mode; preserve it for diagnostics/comparison, and never relabel its historical failures as passes.
5. Finish Astra's isolated output-validation/CI/diagnostic-coverage changes; integrate only after review. Run all applicable production coverage, allocator and sanitizer gates at the final source. Perform a bounded fresh native/physical qualification of both complete workloads and the native diagnostics; include one controlled loaded-host run if runner ownership permits it, without modifying VM affinity or unrelated processes.
6. Require passing applicable CI on the exact integrated candidate, one independent final review, synchronized trackers/handoff and actual evidence links before M2 100% and Time Ended. Do not mark the entire software-vGPU feature complete.

## Boundaries

This is a measurement-contract amendment, not a waiver claiming the old scalar test passed. It reduces the process-wide claim and substitutes explicit M2-owned-resource checks. Generic Windows RPC timer reconstruction, an OS-wide symbol/tracing engine, quiet-host-only acceptance, raised baselines, broader retry budgets and automatic classification of all OS-created handles are outside this proposal. If the owner table or native negative controls cannot establish the narrower required claims, stop with that concrete deficiency; do not expand indefinitely or award acceptance.

No code or acceptance rule has been changed by this proposal. Astra's current work retains the existing strict gate. Adoption requires explicit acceptance-owner approval because M2_CODEX_REVIEW_2026_10_21.md preserves exact equality and expressly reserves an acceptance-owner decision for replacing it.
```


## Read-only actual ICD ownership query (unit C, 2026-10-21)

Commit `b385806` completed the C observer unit. This unit adds a conditional
`venus_icd_m2_snapshot` export to the actual ICD and a separate qualified DLL at
`build/m2_qualification/waddle_vulkan_experimental.dll`. The ordinary DLL stays at
its original path with exactly its original 11 exports. The qualified DLL has
exactly those exports plus `venus_icd_m2_snapshot` and `venus_m2_dc_snapshot`.
`WaddleM2OwnershipProbe` enables the ICD query and the real C sink wrappers;
`WaddleM2OwnershipProbeExport` exports only the DC snapshot. No qualified input
object is compiled with `WaddleM2OwnershipFaults`. The actual PE export tables
and object definitions are checked by `vgpu-m2-qualification-build`; an ordinary
DLL substituted for the candidate and injected fault definitions fail the gate.
Cross-build success proves no Windows execution or physical acceptance.

The ICD ABI is version 1 and exactly 144 bytes on the supported x86_64 boundary.
Offsets 0/4/8/12 contain u32 version/bytes/flags/command_state; offset 16 is i32
lost, 20 is u32 binding. Offsets 24/32/40 contain u64 object_next_id/wsi_next_id/
first_live_id. Starting at offset 48, twelve successive u64 fields are objects,
resources, surfaces, swapchains, images, acquired_images, backings, mappings,
caches, tickets, references and reservations. There is no pointer/ownership
transfer, variable-sized tail or implicit cleanup. A null output, wrong version,
short/long/zero size returns zero without writing any output byte. A valid query
returns one even when its snapshot is unhealthy; the caller must validate the
complete result, not just the function return.

The query takes the existing nested ICD mutex and scans actual private arrays.
It validates the bound slot pointer/capacity/context/namespace/next ID, counts
actual object IDs independently of live_count, rejects dirty empty slots and
mismatched resource identities, and reports all fixed surface/swapchain/image
entries (including orphan or partial pairs and acquired bits beyond active count).
It independently counts bound memory, retired WSI images, internal fence fields,
mapped shadow/baseline pointers, exported mapping IDs, instance/device/template
cache owners and extension-cache allocations. Submission tickets include orphan
sequence/fence/reference metadata; references include inflight/idle counts and
all resource/ticket bitmaps and explicit image/index/framebuffer/profile links.
All 320 occupied copied-profile/command slots, all 64 graphics recordings, cached
physical/queue tokens and cache ring/ready markers are scanned independently.
Parentless cache ownership raises inconsistency, including nonzero physical count
with a zero instance handle. Mapping/ring reservation bits are scanned directly.
The first available retained identity is diagnostic; absence of that identity
never cancels a nonzero owner field. Completed GPU fence sequence history is not
an owner and is not reset. The query never allocates, calls transport/native APIs,
releases an owner, retries, unbinds, abandons or changes sticky lost state.

An unbound query reports flag 1 and cannot qualify (including after abandonment).
Registry inconsistency is flag 2, owner inconsistency flag 4. The future native
per-lifetime predicate must require bound=1, flags=0, CommandIdle=0, RingOk=0,
nonzero monotonic identity marks and zero in all thirteen retained-identity/owner
fields. The marks must also advance during the expected workload. This query
reports the real shared registries, including retained readback staging and queue
owners; temporary synchronous pixel slices remain proved compositionally by the
existing allocator/ASan/LSan and backend identity tests, not by native heap-size
claims. A healthy query is necessary but cannot alone satisfy the M2 protocol.

Five new Zig groups exercise malformed ABI/output preservation, nested locking,
unbound and failed command state, actual swapchain create/destroy and complete
acknowledged device/instance retirement, actual readback success/loss, failed GPU
wait proof with retained tickets/references/acquisitions, 43 independently
corrupted/retained-owner fields, real reserve/release across
all five metadata tables, and 22 additional last-slot orphan reservation/token
controls. Repeated queries preserve state and callback
counts. The reused synthetic graph explicitly supplies the instance cache needed
by actual destroy_instance; no query-side cleanup makes a test pass. The full
suite grows from 394 to 399; both existing exact-count sanitizer pins are updated
to that observed count without changing any floor or assertion.

Validation: 399/399 Debug and ReleaseSafe tests; full portable ICD ASan/LSan
access instrumentation (1,603 emitted owned functions) with zero reported leaks;
actual default/qualified export and object boundaries; malformed export-table,
wrong-DLL and injected-fault rejection controls, including optimized Python.
The unchanged full ICD coverage gate passes 4,419/4,887 branches (90.42%) and
4,777/4,847 lines (98.56%); the two query helpers cover 50/50 measured branch
outcomes and 81/81 measured lines. Source-bound raw artifacts are recorded in
`build/qualification_m2_astra_20261021/ownership_icd_revision2/receipt.json` along with source,
binary and log hashes. Native execution of this qualified DLL remains NOT RUN.
Unit D must still implement the explicit per-lifetime native protocol, verifier
requirements and CI wiring before root's fresh physical qualification.

## Root-executed real C-boundary controls (unit B evidence follow-up)

The lead executed the exact unit B standalone binary in the signed-in Windows
console at medium integrity: all four cases PASS, real exit 0, empty stderr and
no survivors. SHA-256 is
`fee79e5a5ef4a7133769c7ba605a5b9a0692bde380084bd3baacdf085e128b4e`.
Positive case measured 154→154 with live=0. Owned leak, injected release failure
and unrelated release masking each measured 158→158 while live=1 was rejected;
the injected failure additionally retained failure bit 8. Actual owned cleanup
succeeded. These are actual Windows C-boundary controls, not physical GPU or
complete M2 qualification. The prior deployment `8351a349` failed before execution
because the lead's hash substitution changed an algorithm token; that failed
attempt remains preserved and is not a passing run.

Read-only evidence:
`/home/dev/Waddle-LSW/build/qualification_feature_20261021/m2_owner_native_2f66c87a/guest_status.json`,
its `source_receipt.json`, binary and frozen runner. The isolated unit C binding
record independently hashes all six evidence files and checks all 17 unit B
source hashes against commit `b385806`. Existing unit B receipts remain unchanged
and retain their original build-only meaning. This follow-up grants +0%: M2 stays
94% / Review, parent 85.5%, Time Ended TBD, and historical strict failures remain
failed/unattributed.


## Pre-commit owner-inventory correction (unit C revision 2)

Independent lead/Luna review found that the first unit C snapshot omitted the
separate copied-profile reservation flags and graphics recordings. A reserved
profile could coexist with zero object/resource counts and be falsely reported
clean. This was a qualification-observer defect; no native qualification ran and
no production resource-lifetime fix is inferred. The old receipt, source files,
binaries and failure logs remain preserved. A new regression first reproduced
false acceptance through actual profiles.reserve_slot before changing the query;
that 398-pass/1-fail run is retained under ownership_icd_revision2.

Revision 2 keeps the fixed version-1 144-byte ABI and broadens its existing
reservations/references fields as specified above. Repeated observations never
release slots or clear faults. The fifth test group observes exact reserve/release
of all five tables, then independently rejects 22 orphan flag/token cases at the
last array positions with zero objects/resources. A successful complete readback
must also return reservations and references to their pre-call values. The actual
suite is now 399 tests; both exact-count pins are 399. New receipt and candidate
artifacts reside only in `ownership_icd_revision2/`; the earlier
`ownership_icd/receipt.json` is superseded for readiness and remains unmodified.
M2 remains 94%, parent 85.5%, +0%, Time Ended TBD.

## Explicit native ownership protocol (unit D, 2026-10-21)

Commit `b56f7b7` completes the corrected actual-owner query. Unit D consumes that
query in the existing physical fixture, with no production source change. The
original eight-argument invocation retains its original strict handle-baseline
function, 100 samples/10 ms, workloads, assertions and cleanup. The additional
mode is selected only by exactly two extra arguments, `m2_owned_v1` followed by
the absolute qualified DLL path; it is valid only for full `triangle_present`
and `wsi_lifetime`, never smoke or compute. The candidate DLL must already be
loaded/bound by the bootstrap configuration and selected by the manifest.
The new mode performs no new graphics initialization or baseline adjustment.

After authentication and pinned-loader discovery, the fixture canonicalizes the
supplied absolute path with GetFullPathNameA and requires the already loaded
GetModuleHandleA result to report that same path through GetModuleFileNameA.
Paths are printable ASCII and shorter than MAX_PATH (260 including terminator);
relative/noncanonical, missing or truncated paths fail. Both exact snapshot
exports must resolve, and GetModuleHandleExA with FROM_ADDRESS and
UNCHANGED_REFCOUNT must identify that same module for both actual addresses.
Forwarded exports in another module fail. These are borrowed module references;
no observer LoadLibrary/FreeLibrary, file open or handle close is added. Root's
separate binary/source and loaded-module receipt still binds the exact artifact.
The snapshot ABI is not a substitute for an independent binary hash.
Final cleanup additionally requires that exact qualified module path to be absent
after the existing loader/bootstrap reference retirement.

A fixed fixture controller owns two independent 32-slot ledgers: CreateWindowExA
HWNDs and pixel-read GetDC results. Only the two existing presentation includes
select these wrappers. Each reserves an exact ID before acquiring; full/faulted
state rejects before another native call. Failed acquisitions cancel only their
empty reservation. A duplicate successful native acquisition remains recorded;
only that new exact acquisition is eligible for an actual cleanup attempt.
Unknown/ambiguous retirements never reach the API. Actual DestroyWindow/ReleaseDC
success retires one identity; failure retains it and a sticky fault. Existing
cleanup may retry its own owner as before, but never clears the observation
fault. The DLL's independent sink-DC ledger and borrowed HWND boundary are
unchanged. Legacy mode wrappers simply invoke the original APIs. The controller
allocates no memory, thread, mutex or OS handle and runs on the sole UI thread.

The initial boundary follows the existing independent window initialization and
original process-count baseline. Each later boundary follows actual workload
success, device idle, device destruction and instance destruction, and precedes
that lifetime's PASS and final bootstrap Stop. Every query must return exactly
one with the exact fixed version/size. The current bootstrap session must equal
the initial nonzero session. The ICD must be bound, idle, healthy and have zero
flags, first-live identity and every owner field. Both nonzero high-water marks
must share the original nonzero namespace. On each of lifetimes 1..8 both marks
must strictly advance in that namespace. All three native owner snapshots must
be valid, balanced and empty, including pending slots, first-live fields, sticky
failures and reserved ABI space. Successful acquisition counters and next IDs
must advance for each lifetime, and the next-ID increase must be at least the
acquisition increase. The initial snapshot cannot replace later observations.
A query never releases owners, clears state, retries or unbinds. Missing or
unclean evidence fails the fixture before a corresponding lifetime PASS.

Every complete observation is one stdout line beginning `M2_OWNED_V1 ` followed
by JSON with exactly these keys: `session` (16 lowercase hex digits), `workload`,
`lifetime` (0 initially, then 1..8), `module_hex` (lowercase two-digit hex encoding
of each byte of the actual loaded path), `handles` (positive u32 diagnostic),
`icd`, `sink_dc`, `fixture_dc`, `fixture_window`. There are no extra/missing keys.
The ICD array has 21 integer words in the declaration order documented in unit C:
version, bytes, flags, command_state, lost, binding, object_next_id, wsi_next_id,
first_live_id, objects, resources, surfaces, swapchains, images, acquired_images,
backings, mappings, caches, tickets, references, reservations. Each owner array
has 12 words in the unit B declaration order: version, bytes, next_id, acquired,
retired, first_live_id, first_live_parent, first_live_value, live, pending,
failures, reserved. Word widths match those ABIs; negative/oversized integers,
bools, floats and duplicate JSON keys fail verification. The signed lost word
must be zero; any negative code fails as unhealthy evidence.

The read-only output verifier now requires `--protocol strict_count_v1` or
`--protocol m2_owned_v1`. Ownership mode additionally requires `--icd-path` with
the exact loaded Windows path. It repeats all field/predicate/monotonicity checks
and requires the initial record after baseline, then each ownership record after
its exact workload events and immediately before its lifetime event in the
acceptance-event sequence. Ordinary diagnostic lines can remain between events.
Missing, duplicate, reordered, stderr or malformed ownership records fail.
Records bind the same authenticated session/workload/module for the whole run.
The initial count must equal the reported baseline; subsequent process counts
may change and are reported without being used as ownership evidence. Count-only
historical output is rejected by the ownership mode; ownership output is rejected
by the strict mode. All known contradictory fixture failures still fail both.

Counts are recorded at every boundary. A changed count also prints the existing
bounded NT snapshot added/removed delta without a retirement-failed label. An
unavailable diagnostic snapshot is reported and remains passive. Equal counts
do not trigger an identity snapshot and are not claimed as identity evidence.
No OS object-type allowlist, symbol attribution, prewarming, baseline inflation
or borrowed-handle close is introduced. The diagnostic comparison retains its
bounded quadratic algorithm, now used only on changed totals or legacy failure.
All original strict failures remain failed/unattributed.

### Operational qualification commands and external checks

Root's existing native supervisor must invoke the frozen fixture as follows
(the first seven arguments and workload remain its existing exact run inputs):

```text
vgpu_tcp_gpu_windows_draft.exe <bootstrap> <pinned-loader> <manifest> <config> <fresh-retirement-receipt> <triangle_present|wsi_lifetime> "NVIDIA GeForce RTX 5080" m2_owned_v1 "C:\exact\candidate\waddle_vulkan_experimental.dll"
```

It must preserve real exit/both streams, source/binary identities, medium
integrity, exact current transport session, matching host retirement Ack and no
survivors. Select the qualified DLL in both bootstrap configuration and manifest;
passing an ordinary DLL or a different/unbound query module must fail. Do not
edit historical ignored supervisors or receipts; freeze a new invocation for
this protocol. On every resulting status, require the tracked final gate:

```sh
python3 -B scripts/vgpu_m2_output.py --status /absolute/new-run/guest_status.json --workload triangle_present --device 'NVIDIA GeForce RTX 5080' --protocol m2_owned_v1 --icd-path 'C:\exact\candidate\waddle_vulkan_experimental.dll'
python3 -B scripts/vgpu_m2_output.py --status /absolute/new-run/guest_status.json --workload wsi_lifetime --device 'NVIDIA GeForce RTX 5080' --protocol m2_owned_v1 --icd-path 'C:\exact\candidate\waddle_vulkan_experimental.dll'
```

Save gate JSON/exit and bind its status/verifier hashes. These commands only read
collected evidence; they neither launch a native job nor accept M2 themselves.
Complete both eight-lifetime workloads (48 exact frames or 32 ordered WSI cases),
the real Windows eight-case diagnostics and four exact-owner controls, and one
controlled loaded-host run when root's runner ownership permits, without VM
configuration changes. Native/physical execution of this Unit D candidate is
**NOT RUN** in the implementer's isolated checkout. No source-only test awards
physical acceptance, remote CI success, M2 100% or a Time Ended.

### Automated gate coverage and limits

The actual included native controller passes four API-double groups / 686 checks
under optimized C and ASan/LSan/UBSan. Its measured source covers 103/103 lines
and 141/146 directional branch outcomes (96.58%), no exclusions. Malformed
coverage records/floors fail. Tests cover exact module/export binding, legacy
pass-through, failed/cancelled/full/duplicate acquisition, unknown and failed
retirement, every ICD owner field, stale sessions/namespaces/IDs, incomplete
queries and repeated boundary ordering. Synthetic C serialization for both
workloads is independently consumed by the Python verifier. Fifteen verifier
groups cover both protocols, every ABI field, missing/duplicate/reordered records,
contradictory failures, exact CLI behavior and optimized Python. API doubles and
synthetic count drift are explicitly not native evidence.

`vgpu-m2-output-test` also runs three native-collector regression groups. The
tracked `tests/vgpu/m2_native_controls.py` runs the separately built four-case
Windows executable with a 60-second timeout, preserves real exit and both
streams, retains partial timeout logs, and kills/reaps only its own direct child.
It rejects failed exit even if every PASS line exists. Informational stderr is
not treated as a native failure, but contradictory failure or acceptance events
there fail. Output directories must be fresh. The exact four ordered INFO/PASS
pairs and final summary are mandatory; leak/failure/masked controls must expose
live identity 1 and rejection despite equal measured process totals. Binary,
collector and collected-status hashes accompany the result. Stub tests validate
failure/timeout/shape behavior and claim no Windows execution.

CI retains every existing job and pinned action/checksum. The portable job adds
owner/controller tests, sanitizers and >=90% coverage, the actual ICD access
sanitizer gate, qualified/default DLL export checks and full fixture cross-build.
Its Windows job builds and actually executes the four real controls with the
tracked collector; logs/hashes and coverage are preserved as artifacts. The full
fixture cross-build is never labeled native/physical acceptance. Applicable CI
on the exact integrated candidate and independent final review remain required.
Source-bound local receipts are in
`build/qualification_m2_astra_20261021/ownership_protocol/receipt.json`.
M2 remains 94% / Review, parent 85.5%, +0%, Time Ended TBD.


The first aggregate diagnostic run correctly rejected the old unconditional
call-site source pin after explicit mode dispatch was introduced. Its failed log
is preserved as `ownership_protocol/legacy_diagnostics.log`. The revised test
pins the complete explicit owner/strict dispatch and mode selector while retaining
the exact original handle_baseline function, unique strict call, counts, sleeps,
baseline ordering and failure path. Subsequent validation uses a separate log;
no failed evidence is overwritten or waived. Four deliberate source mutations
(sample count, equality, explicit selector and strict branch) must still fail.
