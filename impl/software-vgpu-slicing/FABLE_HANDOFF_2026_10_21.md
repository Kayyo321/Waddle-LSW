# Recoverable Fable qualification handoff — 2026-10-21

This checkpoint is at the user's explicit request to stop safely and transfer
continuation to Fable. It supersedes earlier current-state claims, not historical
results. **M2 is completed at100%; the software-vGPU feature remains85.5% / In
Progress, Time Ended TBD. PR#6 is draft. Nothing is merged.** The objective for
Fable is to complete the remaining feature gates, independent final review and
passing final CI, then truthfully close the parent tracker at100% and make PR#6
ready for merge. A stopping-point handoff itself earns no progress.

## 1. Repository and ownership

- Checkout `/home/dev/Waddle-LSW`, branch `feature/software-vgpu-slicing`, target
  `origin`. Last source/test change `7d5c6bb5f5ee08a6250e2a3ff8c3a57a59c2fedf`;
  subsequent commits are documentation only.
- Pre-handoff HEAD `cbae0a92ef6d9142e3e5cfac2492b72898d6c1bc`, clean tree.
  The commit adding this file is the new exact checkpoint HEAD: obtain it with
  `git log -1 --format=fuller -- impl/software-vgpu-slicing/FABLE_HANDOFF_2026_10_21.md`
  and confirm current HEAD/status/remote before changing files. This avoids a
  self-referential commit hash. Final response records the actual pushed hash.
- Root and all three workers have relinquished editing and running workloads.
  `m2_astra_readiness`, `m2_source_review`, `feature_remaining_audit` all completed
  and explicitly confirmed writes stopped. No worker owns the shared index.
- Read `AGENTS.md`, `IMPL_DESC.md`, `TRACKER.md`, `CODEX_HANDOFF.md`,
  `QUALIFICATION_CONTINUATION_2026_10_21.md` and the specific contracts below.
  Read older entries chronologically: many legitimately say M2 94% or name old
  DLLs. The latest closure/canonical candidate below governs current state.
- Preserve README.md, submodule pins, `.gitmodules`, all evidence and VM state.
  **Do not gc/prune**: an OpenCL candidate clone shares repository objects.
  No force pushes, history rewrite, unrelated cleanup or process termination.
- Sole Git/index owner; use atomic commits with detailed bodies and synchronized
  progress attribution. Existing helper `build/root_todo3_continuation/commit_owned.py`
  holds `/tmp/waddle_git_index.lock` and stages only explicit owned paths.

## 2. M2 is closed; do not restart the historical-counter investigation

Closure commit `54ee9c452c0124200e76ef72eb2e85b2c6487a9e` is pushed. Read
[closure](../wsi-window-lifetime/M2_CLOSURE_2026_10_21.md) and its adjacent JSON
receipt. The M2 tracker is100% / Completed with Time Ended2026-10-21T22:36:09Z.
All19 M2 source/test/verifier files are identical to the applicable qualification
sources. Independent refresh rehashed433 artifacts (348+41+41+3) with no mismatch
and no M2 finding. Full CI39679035088 at
`8d71d8ae4c44127d9bc47626e1973af9f54f01cf` passes all three jobs and has identical
M2 code/build/workflow configuration. Explicit source/config applicability is
recorded; this is not a claim that every later whole-PR CI run has completed.

The user approved the prospective M2-specific ownership gate. GDI/USER equality
remains strict. Historical `tcp_gpu_7adb6000`199→200 and
`tcp_gpu_78129871`USER4→9 remain failed and unattributed; they were not silently
retried into acceptance or assigned an OS cause. Do not reopen M2 merely because
those historical failures remain. New production changes that affect M2 require
normal regression qualification. Parent remaining TODO#3/#5/#11 are separate.

## 3. Live VM and safe takeover

At2026-10-21T22:47:50Z the read-only guest check found console user
`waddle_test` active in session1 and **no owned candidate/debugger/guard process
survivors**. Root has no native/GPU workload or publisher running. Port55986 was
closed. All worker tasks are completed.

Preserved Windows11 VM: QEMU PID10732/startticks62029, swtpm PID10727/startticks62022.
Verify executable/starttime identities, not PID alone. WinRM127.0.0.1:55985;
QMP `build/windows11_vm/qmp.sock`; VNC:19. Auto-logon is off: after a reboot a human
console sign-in is required. Do not reset/reprovision/restore a stale backup.
Read credentials from `build/windows11_vm/credentials.json` only in memory;
never print or commit them.

Lease `build/qualification_20261020/vm_lease.json` records idle user-requested
handoff and relinquishment. Fable must verify current identities, check no other
owner intervened, then take the sole VM/GPU lease before any native preparation.
Existing runners expect the previous owner string: update deliberately and
review their lease assertions; do not bypass ownership checks. Read-only audit
and host snapshot are in
`build/qualification_feature_20261021/recoverable_handoff_20261021/`
(`host_state.json`, `guest_state.raw.json`). This checkpoint adds no active work.

## 4. Current OpenCL blocker is reproducible and distinct from M2

Read [native retention evidence](OPENCL_NATIVE_RETENTION_2026_10_21.md),
[reload contract](OPENCL_RELOAD_CONTRACT.md), [cancellation](OPENCL_CANCELLATION_CONTRACT.md),
`CLVK_LIFETIME_REVIEW.md`, `CLVK_BUFFER_REVIEW.md` and current runtime review.

Canonical `build/clvk_client.json` SHA256
`7c8c386350c347b45956822e216316f53269092674488c4c217e7f58077c1433`;
DLL `build/clvk_client/clvk_b4b22fcea0aa_b_4v5q51/out/OpenCL.dll` SHA256
`70ca4fdbc2ca54954b7879f28f11e013c63a0a660cca718815abc24e98d5887a`
(186373276bytes). Full2738-target build/static admission pass. It includes the
approved private GCC14 Win32 TLS/EH correction; no upstream/pin alteration.
Current fixture `build/qualification_feature_20261021/opencl_cancel_fixture_99edfab5/opencl.exe`
SHA256 `26de712fd3197970cb4fe54680d9c002c1b760e51aa755db74e4cf72b7739b28`;
300-input/two-TU build receipt SHA256
`aeb74c5204e496b20d4f448752f8b66f14e02ed766a53634d5587e88db8a0910`.

Strict full reload R7:
`build/qualification_feature_20261021/opencl_reload_final_7d5c6bb_revision7/`.
First3 exact GPU contexts, malformed/buffer/event checks, joined threads, shutdown,
session stop, named4-module absence pass; first-cycle count equality then fails:

```
entry=[53,1086,4078] baseline=[65,1086,4078] observed=[148,1083,4075]
module_exact=1 modules_absent=1
```

That is **+83handles and3 occupied TLS plus3 occupied FLS slots; zero strict reload
cycles accepted**. Native PID4036/birth01dd61aa01b562ab/session92677681500fe2ff
naturally exits1; native wrapper absent, host/controller worker retired, graph
empty and pidfds closed. No one-per-context ownership inference is justified.
The full1100 DLL lives /3300 GPU contexts (persistent4 raw idle +2 foreign preload
threads) is still required. Failure-only29-predicate diagnostics preserve original
acceptance/short circuit; root San3 passes and branch coverage93.07%.

The no-vendor raw/CRT/CRT+stdio controls show no corresponding retained slots or
handles. Exact full-DLL **load-only** control
`full_dll_load_native_fab15a15/` under the qualification root completes3 loads/frees:
baseline70/1086/4078, each loaded127/1085/4075, each unloaded77/1086/4078,
cleanup65/1086/4078 versus entry58/1086/4078. All extra modules absent after unload,
exit0, **+7handles still unattributed**, no retained TLS/FLS. Session0/static
loading is not interactive GPU acceptance and does not explain active retention.
Use these results to narrow attribution; do not waive or warm away first-load
ownership. No hypothesis is an established cause yet.

## 5. Frozen next-step tools, preserved outside volatile /tmp

Local evidence root is `build/qualification_feature_20261021/`.
New complete copies are under `recoverable_handoff_20261021/`; originals in /tmp
are retained. `frozen_packages.json` SHA256
`d958087d7b14d69884eab9d90c05bbff1f690120098c9d73461ca26b12fd411e`
binds78 copied regular files. Root verified all1221 census bindings and35 R14
receipt inputs in `root_freeze_review.json`. These are local ignored artifacts,
not uploaded Git contents. Preserve before host/tmp cleanup. Absolute original
paths in receipts remain intentional; if relocation is necessary, make a new
reviewed receipt, never edit the original frozen evidence.

### OpenCL census R2 — next bounded diagnostic

`recoverable_handoff_20261021/opencl_census_r2/` copies
`/tmp/waddle_opencl_failure_attribution_plan_revision2/`.
Receipt SHA256 `7a25dc3961477d0d77b4dd00931cb42c49d6269f8260d225a2892bcf755dc797`;
bindings `eb6fe8402da9b3b6a8099e00ce4b60250436a80d77410bfed9c0bc034face153`;
runner `1eb72e9bd1135c49dca0842991db32f5e7abd8e09fb77002257a4db63c68a09c`.
Read HANDOFF/CONTRACT/root_invocation. 17 test groups pass normal/-O; read-only
preflight passes. Actual preparation/native/VM/GPU **NOT RUN**. Re-review and
rehash before running its exact ordinary8-context diagnostic under sole lease.
It performs read-only target module/handle/thread/TLS/FLS census, scoped6-binary
loopback publication and existing full owner-graph checks. It grants no acceptance.
UTF16 `%04x` code-unit paths decode UTF16BE; TLS descriptor pointers must be
read through; FLS upper halves are included. Original R1 runner had concrete
packaging/quoting/API bugs and must not be used. R2 corrects them but native
execution remains unverified. Diagnose actual retained owners before a fix.

### CDB guard R14 — offline ready, not native calibrated

`recoverable_handoff_20261021/cdb_guard_r14/` copies
`/tmp/waddle_cdb_suspend_guard_r14/`; receipt SHA256
`95d4e19166209d49ab30bdbb3d7da8ff12ca3437a58add21888090fa52310a68`.
16 Python groups pass normal/-O; C plain/optimized pass, ASan+UBSan pass with
LSan disabled. Worker LSan=1 fails under ptrace; **root leak-enabled R14 rerun is
still required**, not a passing leak claim. Core coverage195/195lines,
170/188 branch outcomes (90.43%),35/35calls. Strict private GCC14 Win32 cross-build
passes;4168-file compiler closure manifests preserved. Cross-build is not native.
Windows worker and C# no-follow marker reader require native controls first.

This wraps root R12 bulk capture helper:
`root_tls/pageheap_capture_revision12_bulk/pageheap_capture.py` SHA256
`0e9fe010a004f3d3906fed173352b049df49a0feb911e50ab3f30165231bff21`;
receipt `7bd9412fb60eb0495236088a9fe2f0d148b7fb2d9cf1d4098e6d4e67b0b8e500`.
R12 exactly recovers R10 by inverse patch except bulk trace transport (33 normal/-O
groups pass). **R11 stale bulk overlay is invalid and must not be run. R13 wrapper
had tuple-key JSON and newline signal bugs; use reviewed R14, not R13.**

Existing CDB limit15s and complete stage60s remain; R14 reserves cleanup6s.
Only held exact original thread identities with debugger-owned +1 and proven
exact CDB exit may be reconciled, once; preserve baseline suspended counts.
Fresh/unknown identities or uncertain CDB retirement are failures, never broad
resume. Timeout remains failed qualification even when cleanup succeeds.
Read the receipt/API and actual code, not just this description.

### Native guard calibration fixture — partial, finish before use

`recoverable_handoff_20261021/guard_native_control_partial/` copies
`/tmp/waddle_guard_native_control/`. CHECKPOINT SHA256
`359820bc1402f9632d7a071d149490027dbdd70b1ffdf80378b025100ac3d4bd`.
Source `4273e5455364b5f7eee73245733a1fca21a2a840d91a58f5bc42732f4d20b4c1`;
PE `d0382c1017b335084c7894e11cf99a3fa2f77e278c8391805792ec72c96e1a34`;
214-input cross-build receipt
`4d44d8c523123031fefb47624a27b17062dd67c57fabfd7f3f4f6184261711d6`.
Only C target/cross-build exists: **runner/fake tests/native tests unwritten**.
Root's two draft defects (named-event collisions and undoing an unexpected
successful SuspendThread increment) were corrected but not natively exercised.
Complete the adapter against frozen R14, review, then test normal CDB detach,
real15s timeout with cleanup-only recovery, baseline suspended thread preservation,
fresh-thread uncertainty/abort and natural exact-target retirement. No force
kill of target, arbitrary ResumeThread or CPU-affinity-policy workaround.

## 6. Native heap gates and safety history

Read `NATIVE_CSR_HEAP_CONTRACT.md`, DXVK qualification docs and current tracker.
UMDH failed a known12345-byte DLL leak and is insufficient alone. Full
GetProcessHeaps ordered API arrays are required: PageHeap PEB list is incomplete.
Classify full DPH wrappers/backings plus CSR busy allocations, retain all raw
records and exact source/PE/PDB or reviewed PE/COFF/DWARF binding. CSR metadata
is not user busy data. No blanket module-label or subtraction waiver. Incomplete
heap coverage is a failed gate. Calibrated CSR held12380/free0 and seven-heap
negative controls exist, but do not prove candidate heap cleanliness.

DXVK run `build/windows11_vm/dxvk_heap_ec42ed546a2a23a7/` completed one real GPU
life but unloaded1 CDB stack printing timed out at15s (no END) and correctly
failed. Old CDB termination left4 original threads suspended+1. Exact PID/birth/
TID/start evidence and source-bound cleanup resumed only those4, naturally
retiring the failed target; newer2 threads were untouched and IFEO restored.
`debugger_owned_suspend_recovery/` and `root_failed_run_retirement.json` retain
this history. **Do not run old unguarded R10-pvs capture again.** R14/native
calibration above is the concrete unfinished prerequisite for resuming heap runs.

Current DXVK source snapshot `build/wddm_dxvk_checkpoint/dxvk_source_8283757e`
binds212 src/include files;52 host-controller inputs; base host checkpoint
`build/windows11_vm/dxvk_gpu_47779959/checkpoint.json`. Use actual current-source
proof and reviewed helper revision6, not old generic audit assumptions.
After validated guard, complete required DXVK native heap lives and OpenCL
three-life heap qualification, first load included. Old
`/tmp/waddle_opencl_final_heap_plan` is frozen but binds stale fixture65846/fb37
and R10: rebind to current26de/aeb74 and R12/R14 with explicit inverse/applicability
proof before execution. Each3 ordinary OpenCL lives has8 context lifetimes;
all host graph/pidfd and native/cmd absence proofs remain mandatory.

## 7. Results that can be reused only with source/config applicability

- ICD399-test aggregate and90.42%branch/98.56%line gate; complete37-module San3
  seam `build/icd_complete_seam_safety_37/run-20261021T212739473584Z-544235/` passes.
- Windows final physical12 cases (six positives48 GPU lives, six expected failures),
  `build/wddm_dxvk_checkpoint/final_vulkan_f1d49c85/results.json`. Natural retirement;
  no heap credit.
- Linux physical15-case matrix144 positive GPU lives/six expected cleanups:
  `build/icd_physical_opencl_overlap_37/run-1792619474903833290/run-20261021T215114928154Z-650463/physical_report.json`.
  Observer `linux_opencl_overlap_revision2/witness.json` proves functional overlap
  with exact native GPU context interval; not simultaneous instruction issue and
  not reload/heap acceptance. Earlier wrong-plan observer failure preserved.
- Actual complete-input OpenCL cancellation `tcp_gpu_d62ddee8`,
  session2112ca43451ab2a1: real opcode18 gate, enqueue0/read-5, intentional scoped
  host SIGTERM, SDK release before shutdown/abandon, natural native0, all retired.
- Native TLS193-case98.85%branch and FLS270-case100%branch certificates/control
  runs pass; isolated2048-runtime reload passes. None substitutes full DLL reload.
- Full8d71d8a Software vGPU CI39679035088 passes. Later7d/fdf/d050 CI was still
  running at last snapshot with no known failed job; **query current exact HEAD**.
  Do not call pending CI passed or cancel/retry away a real failure.

## 8. Ordered completion plan and final acceptance

1. Verify checkout/remote/VM identities, clean state and all prior workers stopped.
   Take sole lease. Rehash preserved tools, inspect actual code and receipts.
2. Use bounded R2 census to attribute active-GPU retained handles/TLS/FLS. Compare
   no-vendor and load-only controls. Fix Waddle defects with regression tests.
   If the cause requires another upstream private-copy change, present a concrete
   reviewed correction for human approval under CODEX_HANDOFF section4; prior
   shutdown/buffer/runtime approvals are not blanket authorization for new work.
3. Finish/test/review native guard fixture and R14 integration, run leak-enabled
   root sanitizer outside ptrace, then native guard controls before heap acceptance.
4. Complete DXVK and OpenCL first-load/three-life all-heap ownership qualification,
   cancellation/disconnect/error/concurrency regressions and full1100 reload /
   3300 GPU-context run. Preserve failures and attribute every retained owned
   resource; do not alter thresholds/assertions/count policy to turn failure green.
5. Run applicable repository Linux/native Windows/physical GPU builds, allocator,
   ASan/LSan/UBSan and minimum90% protocol/memory coverage gates; rerun affected
   checks after each production fix. Distinguish mocks, cross-build, native and GPU.
   Reuse existing costly results only with recorded exact source/config proof.
6. Commit discrete units with synchronized descriptions/tracker/task attribution;
   push to existing [PR#6](https://github.com/Kayyo321/Waddle-LSW/pull/6). Obtain
   independent final source/evidence review and all required final CI green.
   Resolve actual failures rather than suppress them. No merge.
7. Only when every required task/gate passes, set each parent tracker task100%,
   overall100% / Completed and concrete Time Ended; reconcile descriptions,
   handoff and PR evidence links. Record independent review and exact CI SHA.
   Mark PR ready for review/merge as appropriate, **do not merge**. Final reply
   may then say: "Ready to move onto other areas of the project."

Current checkpoint changes progress by +0% for all tasks. Parent85.5% and TBD
are intentional until those steps succeed. If a genuine external blocker needs
human input, record the precise evidence and missing action; never fabricate100%.
