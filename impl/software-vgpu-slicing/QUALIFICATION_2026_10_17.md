# Current qualification update, 2026-10-17

Qualification remains in progress, feature completion 85.5%, Time Ended TBD.
README is unchanged. Earlier sections below are historical checkpoints; their
unchanged-source, unexecuted-OpenCL and growing-handle statements describe their
recorded checkpoints, not this current update.

Current tested production ICD is `df5f5b9` (the03caf9b loader-chain correction
plus removal of three never-emitted declarations). Current DXVK stream/environment
fixture is8f2732e; native WSI fixture iseb1ff4c; OpenCL malformed-input fixture
is64910b4. Git records the exact current feature HEAD. Required GPU/heap gates
below are still open; no feature readiness or completion is claimed.

- Full current ICD: 394 Debug and 394 ReleaseSafe tests; native C dispatch
  128 cycles; prescribed sanitizer/allocator gates pass. Whole production
  coverage 4,369/4,837 branches (90.32%), 4,696/4,766 lines (98.53%). Native
  Windows394-case Debug/ReleaseSafe and12-case WSI Debug/ReleaseSafe execution
  also pass after the Windows update and dead-declaration cleanup. Current logs:
  `icd_current_qualification.log`, `native_cpu_cleanup_run.log`; earlier
  `icd_debug_chain_final.log` and `native_icd_chain_execution.log` are historical.
- Nineteen additional module groups pass unit/sanitizer/coverage commands:
  capabilities, dmabuf, instance-wire, query-wire, values, extensions-wire,
  objects, command, guest, present, wddm, frame, export, surface, request,
  context, service, runtime, worker. Every executed production coverage gate
  remains at least 90% for both lines and branches, with unchanged tooling.
  Exact commands, isolated Zig cache environment, exit codes and per-module
  logs: `build/qualification_20261017/remaining_cpu_results.json`; extracted
  gate summaries: `remaining_cpu_coverage_summary.json`. All nineteen exit zero.
- `make vgpu-tcp-server-test vgpu-tcp-server-sanitizers
  vgpu-tcp-server-coverage vgpu-tcp-bridge-test vgpu-tcp-bridge-sanitizers
  vgpu-tcp-bootstrap-coverage vgpu-tcp-bootstrap-sanitizers` exits zero. These
  are actual sockets/process ownership with synthetic frontend/SDK boundaries,
  not GPU proof. Bootstrap lines 133/133, branches 174/182 (95.60%); bridge
  lines 167/167, branches 216/234 (92.31%). All configured leak detection stays
  enabled. Log: `tcp_ownership_gates.log`.
- `make vgpu-libc-test vgpu-libc-sanitizers vgpu-receiver-test
  vgpu-receiver-sanitizers vgpu-receiver-coverage vgpu-native-loader-test
  vgpu-native-loader-sanitizers vgpu-present-integration
  vgpu-present-integration-sanitizers` passes its checks with leak detection
  enabled. This includes actual pinned Linux loader discovery/lifetimes and
  synthetic presentation protocol integration, not a physical compositor run.
  Log: `linux_receiver_loader_integration.log`.
- Source freeze `build/wddm_dxvk_checkpoint/dxvk_source_ac95c458` binds the
  corrected ICD and joined-caller DXVK fixture. Fresh physical GPU runs after
  Windows startup pass: sanitized positive `dxvk_gpu_7bbd3a2f`, normal postflush
  `dxvk_gpu_4f1be74b`, sanitized postflush `dxvk_gpu_84281cd7`. Positive requires
  all 4,096 rendered/presented pixels and 64 compute words, natural whole-session
  retirement and zero controller sanitizer diagnostics. Fault cases require
  the existing recorded-submission/device-removal assertions, exact trusted
  retirement proof and no SIGKILL. These are expected failure-cleanup tests,
  not positive workload receipts. `dxvk_current_results.json` binds logs and
  exit codes; each checkpoint stores its full acceptance/process journal.

## Native heap status

The original growing-handle audit remains failed and preserved. Exact native
creator stacks resolve to static winpthreads attached to the fixture's calling
thread during DXVK global logger construction. Commit `0c8d144` moves the entire
transaction to a joined CRT thread and retains all vendor DLLs through TLS/FLS
destruction before parent unload. It does not alter native baselines or free
borrowed/vendor blocks. The corrected three-life historical audit
`dxvk_heap_710ae4228123a45f` has 183/183/183 handles with exact GPU outputs and
natural host retirement. Raw UMDH baseline-to-final delta remains +99,489 bytes;
this is not zero-total-process-heap evidence and heap_acceptance remains false.

Its recurring 880-byte allocation (0x370, not the unrelated 0x880 GDI allocation)
has the LocalAlloc/MSCTF/IMM32/USER32 stack. The OS-only joined GUI-thread
control `system_window_audit_bfd98078` reproduces the same stack sequence and
one allocation per cycle without Vulkan/DXVK/Waddle runtime modules. Independent
Luna high-effort read-only review and exact-image LLVM symbolization connect
other large private ancestry frames to DXVK discovery and Windows loader,
DirectXDatabaseHelper, RPC and Configuration Manager internals. No unbalanced
project allocator was established. Process environment state (4,062 bytes) and
CRT temporary stderr buffering (4,096 bytes) also survive snapshots. These are
explicit attribution items; totals from unlike controls are not subtracted and
unknown ownership is not treated as a leak pass. Fresh current-ICD audit
`dxvk_heap_56dca3b969766ebc` is in progress with signed tools rechecked.

## OpenCL status

No OpenCL kernel acceptance has passed. `dxvk_gpu_14ce8bed` fails at instance
creation; `dxvk_gpu_17aa5f54` times out before discovery; `dxvk_gpu_005527db`
reaches platform discovery/three adapters but fails the old one-adapter fixture.
The last run also proves twenty retained clvk-owned ICD objects; its scoped
forced cleanup grants no natural-retirement credit. Artifacts remain preserved.

The user explicitly approved the private-copy correction in
[the versioned lifetime review](CLVK_LIFETIME_REVIEW.md). Commit `b4481b9` adds
terminal ownership preflight/shutdown and exact patch/ABI/export cache binding;
independent review finds an executor candidate could be stranded bound.
`fac8c24` fixes selection ordering while preserving strict free/idle shutdown.
Sixteen admission tests pass, including actual patched owner accounting under
ASan/LSan/UBSan and the exact patched pool with a controlled executor boundary.
The previous implementation fails its reuse negative control. The first owned
cross-build was intentionally interrupted without publishing a receipt; partial
inputs/logs are retained. `clvk_pool_cross_build.log` records the corrected build
in progress. The new fixture requires private shutdown, rejects live owners,
checks terminal discovery/idempotence and unloads only after caller TLS/FLS exit.
Strict Windows cross-build passes; actual new-DLL execution remains required.

## Environment and remaining qualification

The host rebooted during the interruption. Existing VM disk, firmware and TPM
files were preserved; the VM restarted with the existing QEMU configuration.
Windows applied its pending update without intervention. Fresh OS inventory is
`resumed_windows_inventory.json` (Windows 11 Pro 10.0.26300, UBR 9711, interactive
explorer session 1). Later native/physical receipts must bind this environment;
earlier results are not silently reused across the OS change. Native heap, full
physical WSI matrix, new OpenCL DLL kernels/retirement/heaps, simultaneous Linux
GPU workload and final source/native CI remain open. No PR, merge, feature
completion or Time Ended is recorded.

---

# Verification checkpoint, 2026-10-17

Qualification remains **in progress**. No PR or completion claim follows from
this checkpoint. README, dependency pins, production code and thresholds are
unchanged during this verification phase.

Entry: `feature/software-vgpu-slicing`, clean tracked tree at
`7214c8e7007e7d9aa8236b90b90a98eb9f89255e`. Claude/workers relinquished editing
by direct user confirmation. Milestone 2 means the bounded WSI lifetime milestone
identified by CODEX_HANDOFF.md and HARDWARE_TEST_HANDOFF.md.

## Local and cross-build evidence

Commands executed from the entry source; subsequent commits add only tests,
build targets and documentation. Zig 0.13.0; prescribed leak detection remains on.

| Command | Result and evidence class |
| --- | --- |
| `make vgpu-wsi-lifecycle-test vgpu-wsi-lifecycle-sanitizers vgpu-wsi-lifecycle-coverage` | Exit 0. Twelve Debug/ReleaseSafe Zig cases, C API doubles/unsupported platform, prescribed C ASan/LSan/UBSan and owned Zig instrumentation/allocator pass. WSI lines 148/149 (99.33%), branches 145/156 (92.95%); C sink lines 47/47, branches 71/72 (98.61%). Portable evidence. |
| `make vgpu-icd-test vgpu-icd-sanitizers vgpu-icd-units vgpu-icd-coverage` | Exit 0. Full current 394-case aggregate; native dispatch fixture 128 cycles. Whole-ICD lines 4694/4764 (98.53%), branches 4369/4837 (90.32%). Owned sanitizer/allocator runs pass. Mocks/portable execution, not physical acceptance. |
| `make vgpu-test vgpu-sanitizers vgpu-coverage` | Exit 0. Includes concurrency/process stress and protocol/mapping/lifecycle checks. All executed coverage gates pass; zero prescribed sanitizer findings. |
| `WADDLE_RECORDER_SANITIZERS=1 python3 -B tests/av/coverage_recorder_test.py -v` | Exit 0, all eight cases including fatal-UBSan negative control. |
| `make av-test av-input-test av-sanitizers av-coverage` | Initial/retry cached Zig imports fail `FileNotFound`, preserved in av.log/av_retry.log. Same unchanged command with isolated ZIG_GLOBAL_CACHE_DIR/ZIG_LOCAL_CACHE_DIR passes, including 100000 concurrent video/PCM frames and all coverage gates. No source or acceptance change to work around cache failure. |
| `make vgpu-windows build/vgpu_win32_present_test.exe vgpu-dxvk-client-build` | Exit 0; Windows cross-build only. DXVK cache revalidated, not rebuilt. |
| `make vgpu-opencl-pin vgpu-opencl-build` | Exit 0; pins and existing DLL cache verified, not rebuilt or executed. |
| `make vgpu-opencl-test vgpu-opencl-fixture` | Ten build-tool methods and strict Windows C11 fixture cross-link pass. Kernel execution remains pending. |

Full log and executable/source hash inventory:
`build/qualification_20261017/checkpoint_sha256.json`, SHA-256
`4635d7fa9466420a2945cb697c1072d2ac2b8b5b06ed347a6a28b375c654d540`.
This immutable checkpoint predates later diagnostics; it does not bind later
appended logs. The coverage/sanitizer tools preserve uniquely named detailed
artifacts, recorded inside the corresponding logs.

| Log under build/qualification_20261017 | SHA-256 |
| --- | --- |
| wsi.log | bb2f8d99cbfd271e2b81124686be686da44f132d4c3c4da69278bec91795bb33 |
| icd.log | 4354b57c1654ba95adf8df3f264b85a64e88ae430abafdd39fd250f944a40367 |
| transport.log | 39042b9cd7748b572df3c5e728b1a8a8c4aaa14f948dc0ffabb34b8d11f8c115 |
| av_fresh_cache.log | b5f52d8fd69b2898dc1836cc251c723fb037d20c739522bb7c3098d23beba54f |
| recorder.log | bb03151166e713b71fd77805ec8a28a71576c4a7ce2ebb576a1daa69e562bc14 |
| wsi_windows_native.log | 4d407f9428f98c0d9f19a3a91cf73b8636a64be2128c7455609b5cd158bcf60a |

## Native Windows state fixture

Cross-linked `zig test src/vgpu/venus_wsi.zig -Iinclude
-Isubmodules/venus_protocol/include -lc -target x86_64-windows-gnu
--test-no-exec -femit-bin=build/qualification_20261017/wsi_windows.exe`.
Transferred through authenticated WinRM, checked SHA-256 before running:
`6e1eaf4ecdc6ac3f8f046351fa10ba340aa8fcb3255204ea2e60d28e44d9a338`.
Native Windows execution exits zero, all twelve cases pass. This fixture uses
state/backend doubles: it is native CPU evidence, not actual HWND/GPU evidence.

## Fresh physical DXVK audit

Frozen source/runtime: `build/wddm_dxvk_checkpoint/dxvk_source_2c89e944`,
source commit `7347352` (its DXVK fixture equals current tracked fixture; this
includes compiler-retention commit e8961c5). Rebuilt isolated Windows/runtime
artifacts; unchanged renderer reused only after exact hash/pin comparison.

Fresh three-lifetime audit:
`build/windows11_vm/dxvk_heap_dea02b8aca67b7f1`. Every lifetime produces all 4096
exact rendered and presented pixels and 64 exact compute words, with medium
integrity and natural authenticated controller/worker/whole-session retirement.
Signed Microsoft UMDH inputs revalidated before and after; image-specific UST
registry key restored to its original absence. Deployed binaries rehashed after
execution. Prior artifacts and VM state retained. The VM initially required
interactive sign-in; no workload was launched by the first keyboard attempt.
The existing private credentials were used without logging or changing them.

**Heap acceptance remains false.** Native handle counts after unload are
201, 205, 209; this growth has not been independently attributed. Raw captures,
original and symbol-resolved differences are retained. The system-only compiler
reproduction executes all three ordering modes for eight cycles; it reaches a
stable busy-byte total after startup, which does not explain or clear the DXVK
handle growth. Raw byte totals never substitute for allocation-stack acceptance.
A separate diagnostic-only handle-tracing run is being prepared/performed;
it cannot receive qualification credit or silently replace the original audit.

## Open work

- Attribute and eliminate owned native DXVK handle/heap survivors; rerun normal,
  sanitizer/fault and native heap gates as affected.
- Execute the prepared OpenCL fixture through the real guest/host path, fix
  defects, prove natural retirement, native heap/handle stability and overlapping
  Linux GPU work. Prepared run: `build/windows11_vm/dxvk_gpu_869aaa04`, no execution
  or acceptance claimed at this checkpoint.
- Complete remaining applicable native/physical WSI and shared-GPU cases and
  exact-source CI/deployment verification before closure. Cross-builds and native
  state mocks do not substitute for physical presentation or signed-driver use.
- Upstream clvk changes, if needed, require the handoff's explicit user approval
  and a tracked private-copy patch. No submodule was edited.

WSI tracker stays open pending final qualification accounting; software-vGPU
stays 85.5%, Time Ended TBD. No assertions, coverage thresholds, suppressions,
production exclusions or workload acceptance budgets were weakened.


Loader guard production coverage is now an independent >=90% line-and-branch
Make/CI gate. It compiles the unchanged production prefix and counts only its
seven exact functions, never the injected tests. Fresh mock ASan/LSan/UBSan,
coverage and strict Windows cross-build pass in
`build/qualification_20261017/environment_guard_complete_gates.log`:
84/84 lines and74/74 branches (100%). Additional regressions cover malformed
GetEnvironmentVariable lengths/NUL, every absent API member, invalid active
restore with retained snapshot, and retry. Actual Windows round-trip passes in
`loader_environment_native_42ea77cc/record.json`, with matching executable hashes
before and after. This fixture-only change does not establish native heap or GPU
acceptance; the guard production prefix is unchanged from ef77895.


Native heap follow-up: configure both borrowed CRT diagnostic streams unbuffered
in wmain before any use. The prior audit's stderr-triggered4096-byte UCRT stdio
buffer survives cycle snapshots. Explicit _IONBF configuration avoids acquiring
that buffer during measured work; no borrowed stream is closed, no snapshot or
baseline is moved, and no leak is suppressed. Strict DXVK fixture cross-build
passes (`dxvk_unbuffered_build.log`); fresh native GPU/heap evidence is required.


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


The native OpenCL fixture now checks exact-end read/write/copy/map/unmap results
and rejects zero-length, offset/span past-end and SIZE_MAX-wrap inputs for all
four1D APIs, null read/write host pointers and same-buffer/sibling alias overlap.
Two serial validation scopes additionally own a second context, buffer, queue and
event to require CL_INVALID_CONTEXT for foreign buffer and event inputs. Those
scopes add temporary validation contexts to the unchanged eight primary kernel
lifetimes; concurrent primary callers remain disjoint. Rejections require null
event output, and map rejection requires null pointer plus exact error. Every
new temporary owner is released and every unexpected mapping is tracked for
cleanup. Stage/status diagnostics identify the failing oracle. Strict Windows
cross-build passes in `opencl_buffer_oracles_build.log`; these are unexecuted
native assertions until the fresh actual DLL/GPU run, not acceptance evidence.


The user-approved private clvk buffer correction is recorded in
CLVK_BUFFER_REVIEW.md. Patch0002 rejects malformed logical ranges by subtraction,
checks contexts/events before effects, and resolves same-root sibling aliases
before copy overlap admission. Patch0003 releases a map command when its build
fails before queue ownership; no queue-owned command is freed twice. Every patch
digest participates in driver cache/publication admission, and pinned upstream
checkouts remain unchanged. Thirteen actual API/helper bodies pass controlled
ASan/LSan/UBSan regressions; both original-source range and missing-delete
negative controls fail as required. All19 admission tests pass. Full final DLL,
actual native eight-primary-context execution, simultaneous GPU and heap gates
remain required. The two-patch cross-build passed; the final three-patch build
is in progress. No completion credit or Time Ended change is claimed.


OpenCL fixture transaction ownership now also restores the exact four-key loader
environment through the already-tested bounded guard. The parent snapshots before
its worker starts and restores only after callers join, private global shutdown
finishes, TLS/FLS retires, DLL references unload and module absence is verified.
Any restore failure marks the cycle failed while retaining/retrying its snapshot;
retry success cannot erase that failure. A retained vendor module likewise fails
and holds the snapshot rather than emitting an unloaded marker.

Optional native UMDH mode reuses the existing audit-stage protocol: capture one
baseline before the first transaction, then unloaded_N after each complete cycle,
with1..8 cycles and no moved baseline or altered deadline. Its compatibility
messages/environment keys retain the existing DXVK audit names; they do not
identify a DXVK workload. Every OpenCL cycle still runs all eight primary contexts
plus temporary validation scopes. Both diagnostic streams are unbuffered before
first use. Strict cross-build passes (`opencl_environment_audit_build.log`);
actual native restoration, UMDH and concurrent GPU evidence remain required.


Private buffer memory admission has its own exact production coverage gate:
`make vgpu-opencl-buffer-coverage`, also required by the OpenCL admission CI job.
The first measurement was152/161 lines (94.41%) and165/212 directional outcomes
(77.83%);97.17% merely reached a condition and did not meet the branch gate.
Added cases exercise host map access restrictions, nullable error outputs,
allocated invalid object identity/type, nonfirst foreign events, independent
logical/backing-span boundaries and valid controls without changing production.
The final gate counts161/161 lines (100%) and200/212 directional outcomes
(94.34%), including both untaken compiler throw edges with no exclusions.
All13 definitions execute and have100% individual line coverage. The generated
header contains exact freshly patched API/helper bytes; object/command boundaries
remain CPU doubles. Immutable input/tool hashes, raw gcov and commands are retained
under `build/clvk_buffer_coverage/run-20261018T010722656693Z-201377`.
All19 ASan/LSan/UBSan admission tests pass again in
`clvk_buffer_coverage_sanitizers.log`. Initial lower coverage and script diagnostics
are preserved; this is not actual Windows/DLL/GPU or native heap proof.


Current Linux physical memory profile: c839945 already restricts public mapped
memory to actual HOST_VISIBLE|HOST_COHERENT types and merges coherent dirty data.
The legacy strict noncoherent triangle probe cannot select a public type under
that contract and its failed run remains failed/preserved. A separate
triangle_coherent mode rejects every public host-visible noncoherent type,
selects a compatible actual coherent type and preserves mapped poison, flush,
invalidate, exact pixels, fences and teardown. The legacy mode/default target and
assertions are unchanged. Graphics queue selection is required in both modes.

The full current37-source owned-access proof passes on RTX5080 using the new
coherent profile:9 positive16-lifetime cases (144 lifetimes) and6 exact exit1
postflush cleanup cases, across direct, normal shared-loader and ASan shared-loader
paths. Prescribed ASan/LSan/UBSan remains enabled with zero diagnostics; every
case revalidates all frozen inputs and proves owned session/process/FD cleanup.
The receipt does not claim Windows or vendor-driver access instrumentation.
Command: `python3 -B scripts/icd_physical_sanitizers.py
build/icd_physical_coherent_owned_safety_37 --seam-record
build/icd_complete_seam_safety_37/latest.json --memory-profile coherent
--expected-device 'NVIDIA GeForce RTX 5080'`, exit0.
Receipt: `build/icd_physical_coherent_owned_safety_37/
run-20261018T004547289903Z-148584/physical_report.json`.
Log: `physical_coherent_owned37.log`. Native Windows WSI, fresh DXVK heap,
OpenCL final DLL/native/concurrent GPU/heap gates remain open.


Current native Windows results (not final global qualification):
- `tcp_gpu_683c0ee2` passes all8 actual WSI device lifetimes and32 labeled native
  cases. Every case retains the original process-handle baseline199 and exact
  GDI/USER equality, with24 GPU-rendered64x64 presentations compared across all
 4096 client RGB pixels to independent GPU readback. Replacement/acquired-image,
  zero-area, resize and observed HWND loss behaviors pass. Medium integrity,
  exact host-session retirement and four unchanged deployed binaries are proved.
- The unchanged48-frame presentation workload `tcp_gpu_7adb6000` fails after42
  exact frames at lifetime7 retirement: baseline199, actual200. Native exit1;
  host session and worker exit0 naturally with no remaining owned process. Four
  deployed input hashes still match. It is failed evidence and not replaced by
  the WSI result or earlier diagnostic-only runs. Current-source handle tracing
  is prepared; prior different-fixture traces cannot attribute this new delta.
- `dxvk_gpu_d0fc65d0` passes actual final-three-patch OpenCL execution on uniquely
  selected NVIDIA RTX5080 (maximum group1024): all8 primary kernel lifetimes,
  temporary context/alias/event rejection scopes, deliberate compile error with
  log, both joined concurrent callers, terminal shutdown and complete native
  module retirement. The authenticated host graph retires naturally with no
  signals and exact status0. All six deployed artifacts and all fixture inputs
  still match after execution. This is functional native GPU proof; native heap
  and simultaneous Linux GPU acceptance remain open.
- Fresh post-stdio DXVK UMDH audit `dxvk_heap_d79f3c51503adc22` verifies3 actual
  GPU lifetimes/native exit0 with exact before/after tool, image and PDB hashes
  and originally absent image flags restored. The old4096-byte stderr bucket is
  absent. Remaining positive allocations require attribution, so heap_acceptance
  stays false. Four additional system-only native UMDH controls each complete
  three joined lifetimes (window/factory, compiler, environment, network); their
  successful execution alone is not a leak waiver. Comparisons remain pending.

Exact commands/logs: `native_wsi_lifetime_run.log`,
`native_wsi_lifetime_inputs_after.log`, `clvk_buffer_map_final_cross_build.log`,
`opencl_buffer_final_native_run.log`, `stdio_heap_run.log`,
`system_heap_controls_run.log`, under build/qualification_20261017.
All fourCI workflows atddc7926 passed. FreshCI is dispatched at1e91c48 on the
separate verification-only ref; the feature PR is not updated or marked ready.


User-requested stopping checkpoint (2026-10-18T01:25:19Z): see
[CHECKPOINT_2026_10_18.md](CHECKPOINT_2026_10_18.md) for completed evidence,
remaining gates, exact prepared-but-unexecuted tooling and safe resumption.
Normal and sanitized-host OpenCL functional execution both pass; native heap,
simultaneous Linux GPU and unchanged48-frame presentation remain unqualified.
Local jobs finished, our HTTP publishers stopped, VM preserved running. No PR
readiness/closure or Time Ended change.


### Resumed M2 native evidence, 2026-10-17 (America/New_York)

On clean `8a316fb`, the unchanged source snapshot `eb1ff4c` passes two fresh
full48-frame/eight-lifetime native runs: `tcp_gpu_41308226` and
`tcp_gpu_df62f140`. All96 exact frames,16 lifetime handle checks and both native
exits pass; before/after guest binary hashes match. Controllers/workers exit0,
owned host sessions empty and no supervisor signals are recorded. Vendor
descendant reap status9 remains explicit evidence, not a claim every child
exited normally. Source/input/log hashes and all evidence limits are in
[M2_NATIVE_RECEIPT_2026_10_17.json](M2_NATIVE_RECEIPT_2026_10_17.json).

The current-source diagnostic `tcp_gpu_fe13e873` also reaches48 frames and
original exit0; it is diagnostic-only. At sequence8 two ALPC handles close and
a thread/event pair appears, keeping count199. Exact current OS DLL hashes and
Microsoft public PDBs identify the additions as `RPCRT4!ThreadSelf` from
`PerformGarbageCollection`. Initial Info-stream-age-only symbol admission
failed and is retained. The final admission follows Microsoft's GUID and
DBI/image-age validation (InfoAge>=ImageAge, DBIAge==ImageAge, no legacy-zero
exception), verified against every current copied system image. This
background churn does not attribute the prior failed199→200 run.

`tcp_gpu_7adb6000` remains failed and unexplained. Bounded further diagnostics
are separate from acceptance; no baseline, assertion, frame count, retry budget
or source workload is changed. M2 remains94%, software-vGPU85.5%, Time Ended
TBD. This checkpoint grants no progress or PR readiness. OpenCL, DXVK heap and
simultaneous physical GPU gates remain separate pending feature work.

The host-retirement wording is deliberately narrow: this pinned process-worker
configuration lets virglrenderer destroy its nested renderer with SIGKILL
(`server/render_worker.c:406–424`, connection cleanup via render_client). The
receipts record one adopted vendor child with wait status9. They identify no
signal sender; `signals=[]` covers only the supervisor's own signal journal.
Earlier “natural graph” wording must not be read as every vendor descendant
exiting gracefully or as vendor-child LeakSanitizer proof. No new leak waiver
or change to the acceptance assertions is made.
