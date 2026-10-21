# Feature Tracker: Guest WSI resize and retirement lifetime

- **Contributors / Agents**: Codex Astra isolated readiness implementer; Claude Code (M2 handle-gate audit, diagnostics, fixture fixes and native verification; Sonnet 5.5 lead and workers, Opus 5.5 reviewers); Codex independent qualification lead with read-only Luna source/evidence reviewers; Codex WSI lifetime worker; independent WSI reviewer; queue coverage regression worker and reviewer
- **Time Started**: 2026-10-09T19:59:10Z
- **Time Ended**: TBD
- **Feature Branch**: feature/software-vgpu-slicing
- **Target Merge Branch**: origin
- **Current Overall Status**: Review

## Tasks & Progress

| TODO ID | Task Description | Status | Weight (%) | Progress (%) | Notes / Blockers |
|:-------:|:-----------------|:------:|:----------:|:------------:|:-----------------|
| #1 | Specify bounded lifetime and native thread contracts | Done | 15% | 100% | IMPL_DESC.md; unobserved HWND reuse and full window-event integration remain separate |
| #2 | Implement replacement retirement, resize/loss and queue ownership gates | Done | 35% | 100% | Retire on failed replacement; exact Win32 capabilities; acquire/present gates; correct acquisition release; queue result precedence and conservative loss after ambiguous failure |
| #3 | Add exact-size native presentation with balanced DC release attempts | Done | 20% | 100% | Preserves scaled compatibility API; exact path rejects extent changes before drawing |
| #4 | Run functional, coverage, native Windows/physical, integrated and sanitizer gates | In Progress | 30% | 80% | Independent Codex portable gates pass: 394 ICD cases, 90.32% branches, prescribed ASan/LSan/allocator checks; corrected diagnostics 18 cases / 6,779 checks, native diagnostics 8/8 and both fresh physical GPU workloads pass. The preserved 199-to-200 failure remains failed/unattributed. The acceptance owner approved the prospective owned-resource contract; its implementation, fresh qualification, exact-candidate CI and final review remain open. See M2_OWNERSHIP_QUALIFICATION_2026_10_21.md. |

**Total Feature Completion**: `94.0%`

Independent Codex review, 2026-10-21: correct diagnostic snapshot growth to try the documented 16 MiB capacity when doubling a valid hint would exceed that capacity. Add exact-boundary and repeated-mismatch regressions; retain the six-query bound, ownership cleanup and strict native gate. This discrete correction's commit is identified by `fix(vgpu-tests): honor the full handle snapshot capacity`; its exact hash and verification receipt are recorded in the subsequent independent review checkpoint. TODO #4 +0%; overall remains 94%; Time Ended TBD.

Native growth regression (`test(vgpu): exercise real Windows handle snapshot growth`): acquire and deterministically close 1,024 case-owned events, require a snapshot beyond the initial class-51 buffer capacity, verify every event and access mask, and require exact handle retirement. Cross-compilation alone is not execution; native results are recorded in the independent review checkpoint. TODO #4 +0% (+0% overall).

Task #4 has five equally weighted gates: portable functional tests, production coverage, native Windows qualification (cross-build plus the applicable native/physical acceptance already required below), complete ICD Debug/ReleaseSafe aggregate, and prescribed ASan/LSan. Four gates pass and contribute80% of this task. The native Windows gate contributes0% until all of its existing acceptance requirements pass; successful compilation alone cannot close it. Earlier “Windows cross-builds” wording omitted the native acceptance requirement from this accounting sentence, while the same tracker explicitly retained it under “Still required.” This clarification preserves the existing94% total and strengthens no claim or threshold. Supplemental leak-disabled runs never close the prescribed leak gate. This bounded tracker does not increase software-vGPU TODO #3, close roadmap D, or permit feature merge.

## Verification receipts

Source base: `6eea43c`; owned implementation frozen in `5be8152610619c49578f89581370d65ba0146181`. Subsequent concurrent commits do not alter these owned production paths. Toolchain: official Zig 0.13.0, GCC 14.2.0, LLVM 19.1.7. Existing pinned protocol headers generated successfully; no dependency pins or runtime dependencies changed.

- `make vgpu-wsi-lifecycle-test`: passed 12 Zig cases in Debug and ReleaseSafe plus production C native API-double and unsupported-platform tests. Zig's testing allocator reported no outstanding allocations. Final log: `build/wsi-lifecycle/committed-test.log`.
- `python3 tests/av/coverage.py venus_wsi`: passed production line coverage 99.33% (148/149) and branch coverage 92.95% (145/156), excluding 16 compiler panic/stack-canary guards under the established repository harness. Final log: `build/wsi-lifecycle/committed-zig-coverage.log`.
- `python3 tests/vgpu/wsi_lifecycle_coverage.py`: passed production native C line coverage 100% (47/47), branch coverage 98.61% (71/72). This executes the real C source against local test API declarations, not a Windows runtime. Log: `build/wsi-lifecycle/committed-native-coverage.log`.
- `make build/vgpu_win32_present_test.exe`: passed real Windows SDK cross-link with user32/gdi32/dwmapi. The fixture adds exact-size presentation alongside 128 scaled BGRA/FIFO/IMMEDIATE/GDI/USER cycles and checks resize rejection. Native execution is untested here. Log: `build/wsi-lifecycle/windows.log`.
- `zig test src/vgpu/venus_wsi.zig -Iinclude -Isubmodules/venus_protocol/include -lc -target x86_64-windows-gnu --test-no-exec -femit-bin=build/wsi-lifecycle/wsi-test-windows.exe`: passed Windows state-fixture cross-link; unexecuted here.
- `make vgpu-icd-units`: passed all 386 Debug and 386 ReleaseSafe tests, plus the 128-cycle native instance-dispatch fixture in each mode; process exit zero verified. Includes acquired-old retirement behavior, loss-before-replacement entry point, every supported result-priority pair, mixed rejection order, and pre-/post-enqueue OOM behavior with and without shared waits. Log: `build/wsi-lifecycle/icd-units-final.log`.
- `make vgpu-wsi-lifecycle-sanitizers`: C fixture reaches LeakSanitizer's fatal ptrace restriction; the required combined gate does not pass here. `python3 tests/vgpu/state_sanitizers.py venus_wsi` separately ran all 12 current tests under owned ASan instrumentation and then hit the same LSan restriction. Zero native leaks are not verified. Logs: `build/wsi-lifecycle/sanitizers.log` and `build/wsi-lifecycle/committed-zig-sanitizers.log`.
- Supplemental current binaries rerun with `ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=1`: C ASan/UBSan fixture and owned Zig ASan/allocator suite passed. These do not substitute for LSan. Logs: `build/wsi-lifecycle/c-asan-final.log`, `build/wsi-lifecycle/committed-zig-asan.log`. Exact final Zig runner: `build/sanitizers/vgpu/venus_wsi/run-20261009T202038782277Z-5/runner`.
- Independent review reproduced the 12 Debug/ReleaseSafe cases and native coverage. It identified and the implementation corrected exact Win32 capability extents, ICD liveness bypass of failed-replacement retirement, image acquisition release on rejection, aggregate error priority, and unsafe retryable OOM after queue effects. Final independent sign-off for `5be8152` found no remaining correctness issues after reviewing both 386-test aggregate passes; recorded 2026-10-09T20:20:46Z.

### Whole-ICD coverage regression checkpoint, 2026-10-09T21:49:11Z

The subsequent complete ICD run exposed a real branch-gate failure despite the bounded WSI module gates above passing: **89.99% (4353/4837)** production branches, **98.51% (4693/4764)** lines. Preserve this failed baseline; it is not a pass rounded to 90%. Original log: `/workspace/shared/waddle-tools/final-icd-coverage.log`. Original detailed artifacts: `build/coverage/vgpu/venus_icd/run-20261009T202725706476Z-577/`.

Test/design commit: `c5ea2c72aeddfd2e4a7d5897b8542f3a1254f914`. Final `src/vgpu/venus_icd.zig` SHA-256: `31fc346c79646dc79d21ca8d115d097c2fd4394d7d0a6198e058fbaebe26831d`. The source checksum was verified after final unit execution and before committing. Production code, coverage scripts, instrumentation rules, exclusions, the 90% threshold, dependency pins and README are unchanged.

- Exact final unit command after loading the verified local SDK: `source /workspace/shared/waddle-tools/env.sh; make vgpu-icd-units > build/wsi-lifecycle/icd-coverage-regression-units-final.log 2>&1`. Exit **0**; all **392 Debug and 392 ReleaseSafe tests** pass, with the **128-cycle native instance-dispatch fixture in each mode**. Source checksum receipt: `build/wsi-lifecycle/icd-coverage-regression-final.sha256`. The earlier `icd-coverage-regression-units.log` predates strengthened assertions and is not the final receipt.
- Exact whole-target command: `source /workspace/shared/waddle-tools/env.sh; make vgpu-icd-coverage > build/wsi-lifecycle/icd-coverage-regression.log 2>&1`. Exit **0**; all prerequisite coverage gates and **392 instrumented ICD tests** pass. Final production branches: **90.32% (4369/4837)**. Final production lines: **98.53% (4694/4764)**. This exercises **16 additional production branch edges**, preserves the exact branch/line denominators, and exceeds the minimum passing branch count by **15 edges**. Instrumentation reports **14235/16384 sites**, maximum **10/32 edges**, and **2406 compiler panic/stack-canary guards excluded** under the unchanged harness.
- Detailed current coverage artifacts: `build/coverage/vgpu/venus_icd/run-20261009T214636681486Z-574/`, including `branches.json`, `branch_missed.json`, `line_missed.json`, instrumented IR, edge hits, and the executable runner. The complete target log preserves the prerequisite results as well as the final ICD totals.
- Six new regressions exercise shared-wait GPU-fence/poll failure, sticky loss without republishing results, repeated first-item allocation failure without a result array, sparse/invalid/active queue-owner handling, missing acquire queues, unknown/foreign/mismatched image retirement ownership, and invalid-wait readback rollback. The readback test positively requires acknowledged staging creation, successful command recording, then ordered command/pool/buffer/memory cleanup before asserting no GPU submission. Independent static review signed off the strengthened tests at 2026-10-09T21:44:33Z.

These are portable fixtures, not native Windows execution or real GPU proof. The prescribed LSan limitation and native Windows/Wayland/shared-GPU acceptance remain unchanged. This correction restores the full coverage gate without increasing the bounded tracker beyond 94%.

## Commit History & Progress Log

- **Commit `5be8152610619c49578f89581370d65ba0146181`**: `fix(vgpu): harden Win32 swapchain retirement and resize lifetimes`
  - **Task Impact**: #1 +100% (+15% overall), #2 +100% (+35%), #3 +100% (+20%), #4 +60% (+18%): initial checkpoint 88%.
  - **Summary**: Production WSI/ICD/native fixes, deterministic lifecycle/ownership fixtures, build gates, design and initial receipts. Scope remains the bounded compatibility path.
- **Verification checkpoint for `5be8152`**, 2026-10-09T20:20:46Z:
  - **Task Impact**: #4 +20% (+6% overall): both integrated modes and independent final review verified; total 94%.
  - **Summary**: Repeated committed-source functional/coverage checks and owned ASan instrumentation. Full prescribed LSan remains blocked, without changing targets or suppressing leak acceptance. This documentation checkpoint changes no production source.
- **Commit `c5ea2c72aeddfd2e4a7d5897b8542f3a1254f914`**: `test(vgpu): cover queue-side WSI failure transactions`
  - **Task Impact**: #4 +0%; overall remains 94%. Restores the newly exposed whole-ICD branch gate and strengthens transaction/ownership evidence without claiming another acceptance gate.
  - **Summary**: Adds six independently reviewed portable regressions and their detailed design. Final 392-test Debug/ReleaseSafe aggregate and unchanged whole-ICD coverage target pass with the exact totals and source checksum above. No production implementation or coverage-policy changes.

## Still required

1. Run the unchanged prescribed ASan/LSan targets in a non-ptrace environment or CI. No leak suppression or disabled detection counts as acceptance.
2. Execute native Windows C/state fixtures and affected real Windows Vulkan/DXVK resize/minimize/replacement/loss acceptance on the final source, under the existing owner's GPU/VM lease.
3. Complete the separate AV-window generation to WSI/Wayland integration, including destruction events and unobserved numeric HWND reuse. No property/subclass workaround is claimed here.

### Local LSan blocker cleared, 2026-10-17

Fresh unchanged prescribed `make vgpu-wsi-lifecycle-test
vgpu-wsi-lifecycle-sanitizers vgpu-wsi-lifecycle-coverage` exits zero with leak
detection enabled, no sanitizer/allocator leaks, and unchanged 92.95% Zig /
98.61% C branch coverage. Full current ICD aggregate also passes 394 tests and
90.32% branches. All twelve WSI state tests execute successfully on native
Windows using test backends. Exact logs/hashes and evidence boundaries are in
[the current qualification checkpoint](../software-vgpu-slicing/QUALIFICATION_2026_10_17.md).
The old ptrace limitation no longer applies to this machine. Final qualification
accounting remains open while applicable physical/native acceptance is reviewed;
no premature Time Ended or product acceptance is recorded. This documentation
checkpoint changes #4 by +0% and keeps the overall tracker at 94%.

### Resumed final qualification checkpoint, 2026-10-17

Current corrected ICD `03caf9b` passes the full 394-case Debug/ReleaseSafe,
prescribed ASan/LSan/UBSan and allocator checks, with whole production coverage
90.32% branches and 98.53% lines. The former ptrace leak blocker is absent here.
Native Windows state execution passed before the host reboot; Windows applied
a pending update during resumed startup. Final physical lifecycle/Windows
requalification remains pending; this conservative checkpoint grants no new
completion or Time Ended credit. [Current feature evidence](../software-vgpu-slicing/QUALIFICATION_2026_10_17.md).


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


Current actual native checkpoint683c0ee2 passes8 device lifetimes,32 native
replacement/extent/loss cases,24 exact GPU/client presentations and existing
handle199/GDI/USER equality. Separate unchanged48-frame presentation checkpoint
7adb6000 fails after42 exact frames at handle199→200; retained as failed evidence.
Four guest binary hashes match after both runs. Final qualification remains open;
Task #4 +0%, total94%, Time Ended TBD. See current software-vGPU qualification.


At the user's stopping request, all local verification jobs finished and state
was preserved. See software-vGPU CHECKPOINT_2026_10_18.md. The unchanged48-frame
handle-count failure remains open; no assertion or deadline was weakened.
Task #4 +0%, total94%, Time Ended TBD.


### Resumed M2 native evidence, 2026-10-17 (America/New_York)

On clean `8a316fb`, the unchanged source snapshot `eb1ff4c` passes two fresh
full48-frame/eight-lifetime native runs: `tcp_gpu_41308226` and
`tcp_gpu_df62f140`. All96 exact frames,16 lifetime handle checks and both native
exits pass; before/after guest binary hashes match. Controllers/workers exit0,
owned host sessions empty and no supervisor signals are recorded. Vendor
descendant reap status9 remains explicit evidence, not a claim every child
exited normally. Source/input/log hashes and all evidence limits are in
[M2_NATIVE_RECEIPT_2026_10_17.json](../software-vgpu-slicing/M2_NATIVE_RECEIPT_2026_10_17.json).

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

Task #4 +0%; overall94%. Historical native count failure remains open.

The host-retirement wording is deliberately narrow: this pinned process-worker
configuration lets virglrenderer destroy its nested renderer with SIGKILL
(`server/render_worker.c:406–424`, connection cleanup via render_client). The
receipts record one adopted vendor child with wait status9. They identify no
signal sender; `signals=[]` covers only the supervisor's own signal journal.
Earlier “natural graph” wording must not be read as every vendor descendant
exiting gracefully or as vendor-child LeakSanitizer proof. No new leak waiver
or change to the acceptance assertions is made.


### M2 external handle-counter blocker, 2026-10-17 (America/New_York)

The standalone native system control `wsi_idle_system_only_b460c22e` reproduces
the strict count failure without creating any Vulkan instance or Waddle
session. It calls the unchanged Windows graphics/presentation initialization
helpers, records baseline167, idles65s with100ms raw count sampling, then runs
the original100×10ms equality check. At59766ms the count changes to168 and
stays there through that check. Native/control exit1 is retained as failed
diagnostic evidence; the supervisor exits0 only after collecting it. No Vulkan
or Waddle DLL is present in the captured live module inventory.

The count-change snapshot records three removed ALPC handles and four added
Thread/Event handles. Read-only external tracing, tied to PID/creation identity
and the current matched system DLL/PDBs, resolves creation frames to
`RPCRT4!ThreadSelf`, `RpcpSetThreadpoolCallbackInstance`,
`PerformGarbageCollection`, and the Windows threadpool timer. Trace histories
may reuse handle values, so do not infer object identity from a value alone or
attribute every historical allocation from these records. This demonstrates
that the absolute process-count oracle can fail independently of Waddle GPU
ownership; it does not retrospectively identify the exact handle in7adb6000.

Three further current-source48-frame diagnostics (400a228b,ed3d992a,6290e08c)
all return original exit0; they remain diagnostic-only. Together with the two
unmodified native positives, they establish repeatable functional output,
not a waiver of the preserved199→200 failure. The exact controls, source and
binary hashes, native logs and module/trace receipts are bound in
[M2_SYSTEM_HANDLE_RECEIPT_2026_10_17.json](../software-vgpu-slicing/M2_SYSTEM_HANDLE_RECEIPT_2026_10_17.json).

No production fix is justified by these OS-owned callback frames. Closing
borrowed RPC handles, prewarming an elevated baseline, reducing frames or
widening the equality budget would conceal the issue and is not done. M2's
strict native count gate is externally blocked by the current Windows runtime
behavior; M2 remains94%/Review, Time Ended TBD. The original tracked assertion
and all coverage/leak thresholds remain unchanged. Healthy OpenCL native-heap
qualification proceeds independently; it cannot close this M2 gate.

Task #4 +0%; total94%; no Time Ended or native acceptance waiver.


Accounting clarification (this documentation commit): #4 +0%; four of five
verification gates pass. The Windows gate includes its already-required native
acceptance and remains unpassed due the unchanged global handle oracle. The
prior cross-build-only wording did not explain the retained80% task value.
No task weights, totals, assertions, thresholds or scope boundaries change.


## 2026-10-21 checkpoint (user-requested stopping point)

Independent ownership audit of the strict native handle gate, passive failure-attribution diagnostics, two fixture defect fixes
and a native proof of the diagnostics. The strict gate, baseline, frame/lifetime counts, retry budget and every threshold are
unchanged. **Task #4 +0%; overall remains 94%; Time Ended TBD; not ready for closure.**

- Audit verdict: no Waddle-owned code path explains the preserved 199->200 failure; the process-global count is sensitive to OS-owned RPC
  garbage-collection churn and can hide an offsetting Waddle leak. No production fix is justified. The acceptance-owner decision (keep as
  written / recorded criteria amendment to an attribution-aware strict oracle / change platform) is recorded in
  [M2_HANDLE_GATE_AUDIT_2026_10_21.md](M2_HANDLE_GATE_AUDIT_2026_10_21.md). The native `StretchDIBits` right-to-left probe did not reproduce the audit's
  inferred defect (positive scanline counts everywhere; sink succeeds); no change was made.
- Native evidence: the diagnostics test passes 7/7 in the Windows 11 guest over WinRM and on the interactive desktop
  (class-51 snapshot equals `GetProcessHandleCount`, exact type names and access masks, passive: 50 captures leave the count unchanged).
  The existing native presentation fixture passes on the interactive desktop. **No instrumented 48-frame workload was run at that point (superseded: executed later the same day, see the section below)**; the repeat runner is
  prepared (`build/qualification_20261020/m2_runner/`, dry-run only) and will need the rebuilt `gpu.exe`.
- The committed fixture sources change the fixture hashes; M2 receipts that pin the old hashes (`M2_NATIVE_RECEIPT_2026_10_17.json`,
  `NATIVE_RECEIPTS_2026_10_18.json`, `M2_SYSTEM_HANDLE_RECEIPT_2026_10_17.json`) describe the old fixture only.
- Machine-readable receipt: [../software-vgpu-slicing/M2_DIAGNOSTICS_RECEIPT_2026_10_21.json](../software-vgpu-slicing/M2_DIAGNOSTICS_RECEIPT_2026_10_21.json).

### Commit history (2026-10-21)

- **Commit `7b6c9d5`**: `fix(vgpu-tests): release each native probe DC exactly once on ReleaseDC failure`
  - **Task Impact**: +0% to TODO: #4 (+0% overall). Fixture ownership defect found by the audit; unchanged success path.
- **Commit `6faf4ed`**: `fix(vgpu-tests): clear the destroyed HWND owner before post-destroy checks`
  - **Task Impact**: +0% to TODO: #4 (+0% overall). Fixture ownership defect found by the audit.
- **Commit `faf9d19`**: `test(vgpu): add failure-only handle attribution diagnostics to the native fixture`
  - **Task Impact**: +0% to TODO: #4 (+0% overall). Passive diagnostics, host test (18 cases, 6724 checks, ASan/LSan/UBSan, LSan self-control) and acceptance-rule source pin.
- **Commit `44d69c2`**: `test(vgpu): add native Windows test for the handle attribution diagnostics`
  - **Task Impact**: +0% to TODO: #4 (+0% overall). Native 7/7 PASS in the guest.
- **Commit `56b89a9`**: `test(vgpu): apply independent-review fixes to the handle diagnostics`
  - **Task Impact**: +0% to TODO: #4 (+0% overall). Review fixes, CI step for the host tests; native test re-run 7/7.
- **Commit `cdd7690`**: `docs(wsi): record the M2 handle-gate audit, diagnostics and the open acceptance decision`
  - **Task Impact**: +0% to TODO: #4 (+0% overall). Audit note, IMPL_DESC diagnostics section, tracker checkpoint and the diagnostics receipt.
- **Commit `7392d93`**: `docs(vgpu): checkpoint at user stop with OpenCL runtime-unload root cause and approved path`
  - **Task Impact**: +0% to TODO: #4 (+0% overall; M2 untouched). OpenCL runtime-unload evidence and checkpoint; recorded here only so every commit of the branch is attributed.
- **Commit `ca52536`**: `docs(wsi): record source-bound native receipts for the instrumented M2 handle-gate runs`
  - **Task Impact**: +0% to TODO: #4 (+0% overall). Machine-readable receipts of the 30 native runs below.
- **Commit `d5f2361`**: `docs(wsi): record the source-bound native verification in the M2 IMPL_DESC, tracker and audit`
  - **Task Impact**: +0% to TODO: #4 (+0% overall). Documentation sync of the native evidence.
- **Commit `0faf4cd`**: `docs(wsi): add the M2 review handoff and point the checkpoint and feature tracker at it`
  - **Task Impact**: +0% to TODO: #4 (+0% overall). Review handoff and pointers.
- Later documentation commit(s) (review fixes, e.g. the corrected duration comparison): +0%; see `git log`.

## 2026-10-21 native verification on the committed fixture (work resumed at the user's request)

The stopping-point section above listed three M2 gaps: no instrumented native run, receipts bound to the old fixture hashes, and the `wsi_lifetime` path of the changed fixture never executed.
They are closed below. The strict gate, baseline point, retry budget, frame and lifetime counts, thresholds and every task weight are unchanged.
**Task #4 +0%; overall remains 94%; Time Ended TBD; the PR is not marked ready; nothing merged.** Review readiness is not acceptance: whether these passes satisfy the native gate while
`tcp_gpu_7adb6000` stays unwaived is the acceptance-owner decision recorded in [M2_HANDLE_GATE_AUDIT_2026_10_21.md](M2_HANDLE_GATE_AUDIT_2026_10_21.md); the amended [IMPL_DESC.md](IMPL_DESC.md) holds the full specification of this evidence.

- **Portable gates on the final sources** (HEAD `7392d93`, incremental builds, logs and SHA-256 under `build/qualification_20261021/gates/`, every exit 0): `make vgpu-wsi-lifecycle-test` (Zig 12/12 in Debug and ReleaseSafe, native C API-double and
  unsupported-platform tests); `make vgpu-wsi-lifecycle-sanitizers` (ASan/LSan/UBSan, 39 owned functions instrumented, zero leaks); `make vgpu-wsi-lifecycle-coverage` (Zig branches 92.95% = 145/156, lines 99.33% = 148/149; native C
  branches 98.61% = 71/72, lines 100% = 47/47); `make vgpu-icd-units` (394/394 in Debug and ReleaseSafe, 128-cycle dispatch fixture in each); `make vgpu-icd-coverage` (branches 90.32% = 4369/4837, lines 98.53% = 4696/4766);
  `make vgpu-handle-diag-test vgpu-handle-diag-sanitizers` (18 cases / 6724 checks, LeakSanitizer self-control exits 23); the three Windows cross-build targets were up to date in that run and were then freshly compiled at HEAD with the same
  `zig cc -target x86_64-windows-gnu ... -Werror` recipes into scratch paths (exit 0; the executed binaries stay untouched). These equal the baseline recorded on the unchanged base HEAD.
  `clang-19 -Werror` and `gcc -O2 -Werror` also build the diagnostics host test and it passes (no clang sanitizer runtime is installed here, so that check carries no sanitizer evidence). CI has not run for these commits.
- **Native `triangle_present`:** 20 of 20 pass on the committed fixture (instrumented build sha256 `77a334150077b070...`). Baseline 199 in every run; all 160 lifetime checks equal on the first sample; last check 55.06-55.80 s after the baseline.
  Receipt: [M2_INSTRUMENTED_TRIANGLE_PRESENT_RECEIPT_2026_10_21.json](../software-vgpu-slicing/M2_INSTRUMENTED_TRIANGLE_PRESENT_RECEIPT_2026_10_21.json).
- **Native `wsi_lifetime`:** 10 of 10 pass (8 lifetimes and 32 labeled cases each, exact GPU-readback comparisons as in the 2026-10-17 run). Baseline 199 in every run; all 80 lifetime checks equal on the first sample; last check 38.12-38.78 s after the baseline.
  Receipt: [M2_INSTRUMENTED_WSI_LIFETIME_RECEIPT_2026_10_21.json](../software-vgpu-slicing/M2_INSTRUMENTED_WSI_LIFETIME_RECEIPT_2026_10_21.json). This is the first native execution of the changed destroyed-HWND ownership path.
- **What these do not show:** the preserved 199 to 200 failure is neither replaced nor attributed, and no artifact can attribute it retrospectively; repeated passes do not waive it. Duration is not discriminated: on the like-for-like controller-ready to host-result clock the new triangle runs (65.1-68.6 s) were no faster than the earlier passes (66.4-68.5 s) and the earlier `wsi_lifetime` run 683c0ee2 (71.1 s) passed; only the failed run (81.8 s) was clearly slower. A slowed-host diagnostic was considered; a CPU-affinity change on the VM process was blocked by the harness permission policy and not pursued. WOW64, the class-51 growth path and any loaded-host run are unexercised.
- **Disclosures:** the VM was restarted from its preserved disk after a second host reboot (qemu-img check clean first; Codex signed in at the console at the user’s explicit request); a light offline worker harness (nice 19, no VM/GPU/network contact) overlapped runs 18-20 of the triangle campaign, which recorded all-pass pre-checks and the same timings.

### Independent review provenance amendment (2026-10-21)

`docs(wsi): correct console sign-in provenance and duration clocks` corrects the historical handoff, receipt disclosure and implementation description: Codex entered the saved test-account credential via QMP at the user’s explicit request; auto-logon remained off. The receipt now compares controller-relative durations only with the same clock. Original receipt bytes are preserved under `build/qualification_20261021_codex/before_signin_correction/`; its prior digest is recorded in the amended receipt. No raw run artifact, native source binding, assertion or result changes. TODO #4 +0% (+0% overall). The exact original review candidate remains `8810544b114ee69f0b3990d51b987caebc774c70`.


## Independent Codex review checkpoint, 2026-10-21

Full review: [M2_CODEX_REVIEW_2026_10_21.md](M2_CODEX_REVIEW_2026_10_21.md). Source-bound results: [M2_CODEX_RECEIPT_2026_10_21.json](M2_CODEX_RECEIPT_2026_10_21.json). The review candidate was `8810544b114ee69f0b3990d51b987caebc774c70`; the independent correction and native test are separate atomic commits below.

- **Commit `8810544b114ee69f0b3990d51b987caebc774c70`**: `docs(wsi): apply independent-review fixes to the M2 handoff, trackers and audit`.
  - **Task Impact**: TODO #4 +0% (+0% overall); fixes the earlier handoff's review documentation without granting acceptance.
- **Commit `c650401`**: `fix(vgpu-tests): honor the full handle snapshot capacity`.
  - **Task Impact**: TODO #4 +0% (+0% overall); corrects the premature half-cap refusal. Regression rejects the candidate implementation and passes with ASan/LSan/UBSan after correction.
- **Commit `ff66d04`**: `test(vgpu): exercise real Windows handle snapshot growth`.
  - **Task Impact**: TODO #4 +0% (+0% overall); 1,024 owned-event growth regression cross-compiles and executes natively as part of 8/8 Windows diagnostics cases, with exact retirement.
- **Commit `bc89bb0`**: `docs(wsi): correct console sign-in provenance and duration clocks`.
  - **Task Impact**: TODO #4 +0% (+0% overall); transparently amends metadata, preserving original receipt digest and all raw native artifacts.
- **Commit containing this checkpoint**, `docs(wsi): record independent Codex review and qualification blocker`:
  - **Task Impact**: TODO #4 +0% (+0% overall); records independent test/coverage/native receipts, synchronized tracker/handoff state and the concrete unapproved alternative oracle. Its exact object identity is the Git commit containing this entry.

All six prescribed portable target groups pass independently. WSI branches are 92.95% Zig and 98.61% native C; the full ICD has 394 Debug/ReleaseSafe cases and 90.32% branches. Diagnostics have 18 cases / 6,779 checks under ASan/LSan/UBSan, with the intentional-leak self-control detected. Native Windows diagnostics pass 8/8. Fresh corrected-fixture `triangle_present` and `wsi_lifetime` runs pass on the actual RTX 5080 route; all 16 exact handle checks equal 199 on sample 1. Both the 1,020 historical and 68 new manifest-bound files re-hash successfully; an independent verifier additionally requires lifetime markers 1 through 8 in order.

The historical `tcp_gpu_7adb6000` 199-to-200 failure remains unattributed and unwaived. CI has not run for these commits. The unchanged native qualification gate remains open; the proposed fail-closed attribution protocol is documented for an explicit owner decision, not implemented or adopted. **TODO #4 remains 80%, overall remains 94%, Time Ended TBD.** Software-vGPU remains 85.5%, with its separate OpenCL and other feature gates open. No PR-readiness update, push or merge occurred. The preserved VM is running; all owned native/GPU jobs and the temporary publisher have retired.


### Isolated M2 readiness: ordered output gate, 2026-10-21

- Implementer: Codex Astra readiness worker, isolated branch
  `feature/m2-astra-readiness`, base `ca00ad7396ad42e6cdacc6fbe7b15f4fd53cd3a4`.
  Shared native jobs, VM/GPU state, runners and historical receipts stayed read-only.
- Commit subject: `test(vgpu): reject unordered M2 qualification output`
  (exact hash is recorded by the lead in the subsequent commit checkpoint).
  **Task impact: +0% to TODO #4 (+0% overall); M2 remains 94% / Review,
  Time Ended TBD; software-vGPU remains 85.5%.**
- Added `scripts/vgpu_m2_output.py` and `tests/vgpu/m2_output_test.py`, exposed by
  `make vgpu-m2-output-test`. Future final qualification must invoke the tracked
  verifier for every collected run in addition to the native supervisor/host
  checks. It enforces ordered lifetimes 1..8 and each workload's ordered events,
  rejects contradictory failures and malformed evidence, and records hashes.
- `TMPDIR=/tmp/waddle_m2_astra_readiness/tmp make vgpu-m2-output-test`:
  **PASS**, exit 0, nine regression groups including normal and optimized CLI.
  Log: `/tmp/waddle_m2_astra_readiness/logs/unit1_output_test.log`.
- Read-only compatibility controls: existing `tcp_gpu_8cbf89ac` triangle and
  `tcp_gpu_72dc797c` WSI output shapes **PASS**; original failed
  `tcp_gpu_7adb6000` **REJECTED** as required. No native jobs ran and these checks
  add no acceptance credit or new historical attribution. Input hashes and
  results: `/tmp/waddle_m2_astra_readiness/logs/unit1_historical_readonly.log`.
- CI wiring and final native/physical verification of the integrated candidate
  remain pending. The strict native handle oracle remains unchanged.


### Isolated M2 CI readiness wiring, 2026-10-21

- **Commit `17dc2d0`**: `test(vgpu): reject unordered M2 qualification output`.
  **Task impact: +0% to TODO #4 (+0% overall)**. The lead independently reviewed
  the six-file unit, reran nine regression groups and checked preserved positive
  and failed outputs before committing. Historical receipts remain unchanged.
- Current unit subject: `ci(vgpu): enforce M2 output and native diagnostic gates`
  (exact hash recorded by the lead in the following checkpoint). Add the
  portable verifier gate, strict diagnostic/full-GPU cross-build, and native
  Windows diagnostic execution with exact eight-case output, real exit status,
  60-second own-child timeout and preserved raw output/exit artifacts.
  **Task impact: +0% to TODO #4 (+0% overall); M2 remains 94% / Review,
  Time Ended TBD; software-vGPU remains 85.5%.**
- YAML and embedded Python compilation/source-helper pin: **PASS**. Controlled
  collector cases **PASS** for informational stderr, actual exit 7, missing case
  despite exit 0, and timeout with preserved partial evidence. Log:
  `/tmp/waddle_m2_astra_readiness/logs/unit2_ci_config_test.log`.
- Actual Windows CI, PowerShell parsing and native/physical execution of this
  integrated candidate: **NOT RUN** in the isolated worktree. No VM/GPU access
  or new native acceptance is claimed. Portable gate results follow in this
  unit's evidence record.


Fresh isolated unit-2 verification (all commands from
`/home/dev/.codex/worktrees/m2-astra-readiness/Waddle-LSW`, with both Zig caches,
TMPDIR and logs under `/tmp/waddle_m2_astra_readiness/`, Python bytecode disabled):

| Command | Observed result |
|:--|:--|
| `make vgpu-m2-output-test` | PASS, exit 0; nine regression groups |
| `make vgpu-wsi-lifecycle-test` | PASS, exit 0; 12 Zig cases in Debug/ReleaseSafe plus C API-double/unsupported fixtures |
| `make vgpu-wsi-lifecycle-sanitizers` | PASS, exit 0; 39 owned Zig functions instrumented; ASan/LSan/UBSan and allocator checks clean |
| `make vgpu-wsi-lifecycle-coverage` | PASS, exit 0; Zig branches 145/156 (92.95%); C branches 71/72 (98.61%) |
| `make vgpu-icd-units` | PASS, exit 0; 394 cases each in Debug/ReleaseSafe and 128-cycle dispatch fixture each |
| `make vgpu-icd-coverage` | PASS, exit 0; ICD branches 4369/4837 (90.32%), lines 4696/4766 (98.53%) |
| `make vgpu-handle-diag-test vgpu-handle-diag-sanitizers` | PASS, exit 0; 18 cases/6,779 checks, source pin and intentional LSan leak exit-23 control |
| `make vgpu-handle-diag-native-build build/vgpu_tcp_gpu_windows_draft.exe build/vgpu_win32_present_test.exe` | PASS, exit 0; strict x64 Windows compilation/linkage only; all three executables unexecuted here |

Exact commands, environment, UTC start/end times, exits and log SHA-256 hashes:
`/tmp/waddle_m2_astra_readiness/unit2_gate_results.json`. Source/config bindings
and configuration-control results are in the sibling `unit2_receipt.json`.
No production sources, existing native workloads, strict acceptance rule,
coverage exclusions/thresholds, historical evidence or dependency pins changed.


### Isolated diagnostic coverage gate, 2026-10-21

- **Commit `e17425b`**: `ci(vgpu): enforce M2 output and native diagnostic gates`.
  **Task impact: +0% to TODO #4 (+0% overall)**. Lead review independently
  rehashed all nine bound logs and source/config records before committing.
- Current unit subject: `test(vgpu): gate real handle diagnostic coverage`
  (exact hash recorded by the lead in the next checkpoint). Add the reproducible
  real-include gcov gate, five fail-closed evidence controls, make target and CI
  artifact retention. No production or acceptance rule changes in this unit.
- `TMPDIR=/tmp/waddle_m2_astra_readiness/tmp PYTHONDONTWRITEBYTECODE=1 make
  vgpu-handle-diag-coverage`: **PASS**, exit 0; five evidence-control groups,
  existing 18 diagnostic cases/6,779 checks and source pin; real-include coverage
  **108/108 lines (100%) and 100/100 branch outcomes (100%)**, no exclusions.
  Log: `/tmp/waddle_m2_astra_readiness/logs/unit3_coverage.log`.
  Raw report/summary directory:
  `build/coverage/vgpu/handle_diag/run-20261021T172335548961Z-178883/`.
- **Task impact: +0% to TODO #4 (+0% overall)**. M2 remains **94% / Review**,
  Time Ended **TBD**; software-vGPU remains **85.5%**. Hosted CI and the final
  native/physical qualification are **NOT RUN** by this isolated worker.


### Approved ownership qualification contract, 2026-10-21

- **Commit `3736237`**: `test(vgpu): gate real handle diagnostic coverage`.
  **Task impact: +0% to TODO #4 (+0% overall)**. The reproduced real-source gate
  reports 108/108 lines and 100/100 directional branch outcomes; it retains all
  thresholds and raw evidence. Prior reviewed receipts are archived under
  `build/qualification_m2_astra_20261021/pre_ownership/` with a manifest.
- Current atomic unit: `docs(wsi): adopt bounded M2 ownership qualification`
  (exact hash recorded in the next checkpoint). The direct human answer
  **“Approve M2-specific ownership gate (Recommended)”** adopts the prospective
  contract and complete [owner inventory](M2_OWNERSHIP_QUALIFICATION_2026_10_21.md).
  Conditional native identity observation and read-only ICD/WSI quiescence checks
  are authorized; no ownership code is implemented by this documentation unit.
- **Task impact: +0% to TODO #4 (+0% overall)**. M2 remains **94% / Review**, task
  #4 **80%**, Time Ended **TBD**. Historical failures stay failed/unattributed.
  The remaining blocker is implementation and fresh qualification under the
  approved contract, including controls, exact-candidate CI and independent final
  review; approval itself provides no acceptance evidence.


### Exact native ownership observer readiness, 2026-10-21

- **Commit `fbd94e6`**: `docs(wsi): adopt bounded M2 ownership qualification`.
  **Task impact: +0% to TODO #4 (+0% overall)**. Records direct approval and the
  complete owner map before implementation.
- Current atomic unit: `test(wsi): observe exact native drawing-context owners`
  (exact hash recorded in the next checkpoint). Adds reservation-before-GetDC,
  exact successful-release retirement, sticky failed owners, fail-closed versioned
  snapshots, native leak/masked-release controls and portable safety/coverage gates.
  The default C path is unchanged; test faults are absent from the qualified object.
- **PASS**: four ledger groups / 2,720 checks, three observer groups / 759 checks;
  ASan/LSan/UBSan; four coverage-evidence groups (also under Python optimization);
  ledger 85/85 lines and 143/144 outcomes, observer 35/35 lines and 26/28 outcomes;
  existing WSI functional/safety/coverage gates; strict x64 Windows cross-build.
  **NOT RUN**: four actual Windows owner controls, physical workload qualification,
  CI. See `build/qualification_m2_astra_20261021/ownership_observer/receipt.json`.
- **Task impact: +0% to TODO #4 (+0% overall)**. M2 remains **94% / Review**,
  task #4 **80%**, Time Ended **TBD**. ICD quiescence and native per-lifetime
  protocol remain to implement; historical failures remain failed/unattributed.


### M2 actual-owner query readiness, 2026-10-21

- **Commit `b385806`**: `test(wsi): observe exact native drawing-context owners`.
  **Task impact: +0% to TODO #4 (+0% overall)**. The lead subsequently executed
  its source-bound native controls: all four PASS; exact owned leaks rejected
  despite equal process totals. Earlier deployment failure remains preserved.
- Current atomic unit: `test(wsi): expose read-only qualified ICD ownership` (exact
  hash recorded in the next checkpoint). Adds the fixed 144-byte actual-array
  query, five meaningful Zig groups, separate qualified DLL and actual export/
  symbol gate. Invalid queries do not mutate output; valid queries never retire
  owners or clear failures. The two existing exact test pins increase 394→399.
- **PASS**: 399 Debug/ReleaseSafe tests, owned ICD ASan/LSan access instrumentation,
  qualified Windows cross-build and strict export/fault/wrong-DLL controls.
  Corrected coverage: ICD 90.42% branches / 98.56% lines; query 50/50 branch
  outcomes and 81/81 lines. Exact source/artifact bindings are in
  `build/qualification_m2_astra_20261021/ownership_icd_revision2/receipt.json`.
  **NOT RUN**: qualified DLL native/physical execution and remote CI.
- **Task impact: +0% to TODO #4 (+0% overall)**. M2 remains **94% / Review**,
  M2 task #4 **80%**, parent **85.5%**, Time Ended **TBD**. Per-lifetime protocol,
  CI wiring, fresh physical qualification and independent final review remain.
  Every historical strict failure remains failed/unattributed.

- Pre-commit independent review reopened this unit for a real observation gap:
  copied-profile occupancy/recordings could survive with zero object counts.
  Revision 2 adds all 320 occupied profile/command slots, all recording fields,
  orphan cache/dependency tokens and 22 retained-owner controls. The initial
  false-clean regression failure and old receipt remain preserved; current
  validation is source-bound in `ownership_icd_revision2/receipt.json`. +0%.

### Explicit native ownership protocol readiness, 2026-10-21

- **Commit `b56f7b7`**: `test(wsi): expose read-only qualified ICD ownership`.
  **Task impact: +0% to TODO #4 (+0% overall)**. Includes the independently
  corrected copied-profile/command/recording owner inventory and preserved
  false-clean failure control, 399 tests and exact qualified export boundary.
- Current atomic unit: `test(wsi): enforce explicit native ownership qualification`
  (exact hash recorded in the next checkpoint). Adds the opt-in exact-module,
  session-bound, per-lifetime protocol and fixture HWND/DC ownership, mandatory
  verifier selection, actual C serialization/malformed-evidence regressions,
  portable safety/coverage gates and bounded real Windows control CI execution.
  Legacy eight-argument strict mode and historical failure meanings stay intact.
- **PASS**: 15 verifier groups, 3 collector groups, 4 actual-controller API-double
  groups / 686 checks; owner/controller sanitizers and coverage; controller
  103/103 lines, 141/146 directional branches (96.58%); default/qualified export
  and integrated Windows cross-build gates. Source-bound aggregate logs/receipts:
  `build/qualification_m2_astra_20261021/ownership_protocol/receipt.json`.
  **NOT RUN**: this candidate's native/physical workloads and remote CI.
- **Task impact: +0% to TODO #4 (+0% overall)**. M2 remains **94% / Review**,
  task #4 **80%**, parent **85.5%**, Time Ended **TBD**. Require root-owned fresh
  complete workloads, native controls/diagnostics, loaded-host qualification,
  matching independent host retirement, exact integrated CI and final review.
