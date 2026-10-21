# Independent qualification continuation, 2026-10-21

Status: **not ready for merge**. M2 remains94% / Review; parent85.5% /
In Progress; Time Ended TBD. This checkpoint supersedes same-day earlier
current-state claims, while preserving their historical results. No merge.
README, dependency pins and upstream sources remain unchanged.

## New production OpenCL image

The full reviewed private Win32 runtime build and static admission passed at
52f8999 (`root_tls/full_driver_build_v2/`, below the qualification root).
Its186,373,276-byte DLL SHA256 is
`5f604c43f2671dcf001f860935c72e7df3a8cf90122ccec24276894e019c6ed1`.
The exact image ran in the existing signed-in Windows11 guest against the real
RTX5080: eight primary context lifetimes, exact add/local-barrier/atomic/max-group
kernels, buffer/event/build-error and malformed-range/context/pointer/overlap
oracles, plus joined concurrent independent contexts. Natural native exit0,
matching sessionAck, controller/worker0, empty owned process graph and module
retirement passed. Fresh fixture SHA256
`7c13a69a6d128b24a51a0905ed8ccdd44b33525efec803263c282ca0ac445067`.
Evidence: `build/windows11_vm/tcp_gpu_1cab18c8/`, session7fae26884eb14282;
`build/qualification_feature_20261021/opencl_functional_plan/fixture_build_receipt.json`,
`opencl_functional_package_24f766b/`, `opencl_functional_launch_24f766b/`.

These are actual native GPU results, not heap/TLS/FLS, transport-cancellation
or simultaneous Linux/OpenCL acceptance. The later audit-header/source-identity
correction requires fresh affected fixtures/admission. The new full build at
01702c5 completed all 2,738 targets, link and admission with exit zero:
`root_tls/full_driver_build_v3/{invocation.json,build.log,exit.json}`. Current
canonical `build/clvk_client.json` SHA256 is
`7c8c386350c347b45956822e216316f53269092674488c4c217e7f58077c1433`;
its DLL SHA256 is
`70ca4fdbc2ca54954b7879f28f11e013c63a0a660cca718815abc24e98d5887a`
(186,373,276 bytes). Current source hashes match. This image subsequently passes the scoped native cancellation result below;
ordinary/full reload/heap acceptance remains separate. Historical functional
results are not relabeled as this candidate.

## Fresh M2 failure retained

The full new `m2_owned_v1` triangle workload failed naturally at the unchanged
GDI/USER gate: GDI7->7, USER4->9 after18 successful exact GPU frames (two
complete lifetimes, third workload frame checks passed before the count check).
Initial and the first two lifetime ownership records were healthy/zero; there
is no third boundary record. The approved ownership amendment applies to
kernel handles and explicitly retains GDI/USER equality. This failure is not
waived, reclassified or replaced by an automatic retry.

Evidence: `build/windows11_vm/tcp_gpu_78129871/`, session7cb88514dc9a91cf;
`build/qualification_feature_20261021/m2_owned_candidate_24f766b/`,
`m2_owned_launch_24f766b/`, `m2_owned_runs_24f766b/results.json`. Exact hostAck,
controller/worker natural0, empty graph, retirement and VM preservation passed.
A concurrent root-owned OpenCL compiler build supplied recorded CPU load; no
VM affinity or unrelated process was changed.

Source review found that the fixture never pumps Windows messages. Windows
ghosting/deferred UI activity is a hypothesis, not established allocation
attribution or a production defect. A bounded failure-only HWND identity
collector passed independent source review and was committed in8038ad9.
Root portable ASan/LSan/UBSan checks passed (18 groups / 5,314 checks), with
100% included-source lines,98.39% GUI branches and96.71% owner branches.
Root native control `m2_gui_native_df4d92e7/` passed with natural exit zero,
real hidden top-level, child and message-only identities, exact owned cleanup
and no survivors. Windows CI ran these controls successfully at8038ad9.
These controls do not qualify the full WSI workload. Both full loaded-host M2 workloads now pass; the exact evidence is in
[M2_OWNED_NATIVE_2026_10_21.md](../wsi-window-lifetime/M2_OWNED_NATIVE_2026_10_21.md).
M2 remains94% pending final CI/review; no completion credit is awarded yet.
All original comparisons, sampling, timing and UI behavior remain mandatory.

## Linux physical GPU matrix

The complete37-module ICD seam ASan/LSan gate passes at the current sources:
`build/icd_complete_seam_safety_37/run-20261021T193122717034Z-341004/completion.json`.
It proves662 owned functions across31 modules and399 test-mode cases; all37
module obligations pass with actual instrumentation checks.

The subsequent coherent-memory physical RTX5080 matrix passes:
`build/icd_physical_coherent_owned_safety_37/run-20261021T194847362262Z-365988/physical_report.json`.
Nine positive runs cover144 lifetimes (direct, shared normal and shared ASan,
for triangle, compute and compute-push). Six expected status-one post-flush
failure runs prove cleanup. Complete/source-preservation checks pass. This is
Linux native physical execution, not Windows or OpenCL overlap acceptance.
The earlier run194256680532Z-349996 was invalidated by a concurrent Make recipe
edit; its raw files remain preserved and receive no credit.

## Native allocator measurement controls

UMDH alone missed the known owned malloc12345 first-load negative control.
The bounded full-PageHeap inventory now detects the exact live malloc23457 and
HeapAlloc34561 controls and proves their disappearance after real frees. It
also detects the deliberately retained12345 allocation after DLL unload,
bound to the actual loaded module range and exact constructor return RVA.
Native run `pageheap_negative_73f47037/` completed naturally, with no survivors
and the original per-image registry state restored. Its source-bound
`calibration_observation.json` records these controls. A subsequent nine-stage
critical-section init/delete/reinitialize calibration
`pageheap_negative_41846803/` also completed naturally with empty structural
inventory errors and registry restoration; ownership analysis is pending.

These controls are measurement evidence, not final candidate heap acceptance.
Six of seven exposed API heap handles have an exact verifier-code mapping;
the remaining heap and all retained-block ownership still require proof.
Four retained48-byte critical-section debug blocks have a possible bounded
NTDLL cache owner; stack causation alone does not establish that owner. No
allocation allowance, warmed-up candidate baseline or generic subtraction is
permitted. Tool syntax/preflight failures remain preserved.

## Remaining gates

Exact-head Windows transport and OpenCL admission CI pass at8038ad9; the full
Linux transport job is still running. Full OpenCL TLS/FLS/1100-reload stress,
actual in-flight cancellation, simultaneous Linux GPU execution, calibrated
first-load native heap qualification, final three-lifetime DXVK heap,
Windows physical matrix, M2 native qualification and final review remain open.
Astra's new reload fixture is still being implemented in its isolated worktree.

All raw failure folders are preserved. The existing VM and its state are
preserved; root remains sole native/GPU runner. No gc/prune: the candidate
clone shares repository objects. +0% credit to all tasks at this checkpoint.

## Actual in-flight cancellation exposed a fixture teardown defect

`build/windows11_vm/tcp_gpu_7359408c/` proves authenticated opcode18, the actual
joined enqueue0/read-5 result and exact owned host reaping, then exits3 because
the fixture cleared ICD dispatch before clvk's buffer destructor. The raw run
remains failed. The source correction preserves dispatch through reverse CL
release and terminal SDK shutdown, abandoning only afterwards. All four
extracted helper lines/branches and sanitizer controls pass; independent source
review reports no further defect. A fresh native run is still required. See
[the corrected contract](OPENCL_CANCELLATION_CONTRACT.md). Current production
DLL inputs/identity are unchanged. This correction awards+0% acceptance credit.

## Complete cancellation orchestration and fresh native pass (2026-10-21)

The extended source-extracted regression executes the exact `run_transport_loss`
and its four helpers, including all SDK acquisition variants, thread setup/join
errors, Busy ownership checks, real call-result mapping and every cleanup edge.
Root normal/optimized runs pass O0/O2 and ASan/LSan/UBSan with108/108 executable
lines and92/92 directional branch outcomes, all five functions executed. No
coverage exclusion or threshold change. Receipts are
`build/coverage/vgpu/opencl_cancellation_order/run-3rf7398o/receipt.json` and
`run-p00vgs7j/receipt.json`; root logs are under
`build/qualification_feature_20261021/opencl_cancellation_full_root/`.

Fresh actual native run `build/windows11_vm/tcp_gpu_d5dbcc4f/`, authenticated
session4996ced9f2449702, passes the witnessed transport-loss scope. The actual
submit gate triggers intentional cancellation of only its exact host graph;
the joined API returns enqueue0/read-5. Exact receiver reaping precedes the
trusted receipt, reverse CL release, terminal SDK shutdown and ICD abandonment.
The native fixture exits0 naturally; module absence and exact native/batch
console absence pass. The host graph is empty; reviewed supervisor finally
closes its pidfds and the supervisor exits0. Cancellation signals and nonzero
host status are expected fault actions, not normal-exit evidence. Full DLL
SHA256 is70ca4fdbc2ca54954b7879f28f11e013c63a0a660cca718815abc24e98d5887a.

Root binds the complete raw run in
`build/qualification_feature_20261021/opencl_cancellation_full_root/native_root_review.json`.
The earlier invalid-device run remains failed. The older fixture builder's
single multi-TU dependency invocation omitted one preprocessing closure; its
manual logical source/tool bindings were reviewed, but this receipt is not
complete dependency-closure evidence. Final qualification requires the fresh
per-TU-union builder and rerun after fixture integration. This result awards no
heap, full reload, overlap or final feature completion credit.

## Full reload integration, unresolved native exhaustion calibration

Astra revision3 is integrated with independently reviewed receipt, inverse
preservation and root portable/sanitizer/coverage pass. A fresh per-TU-union
Windows cross-build binds297 dependencies. Native TLS/FLS controls fail closed
on status8 at1086 TLS acquisitions; FLS itself and full1100-cycle stress are
not run. Status8 cannot alone distinguish exhausted slots from allocation
failure. See [the exact contract and preserved evidence](OPENCL_RELOAD_CONTRACT.md).
This supersedes the earlier statement that Astra implementation is still open;
actual native calibration/qualification remains open. +0% progress.

## Exact-target TLS native controls pass

[The new certificate](OPENCL_SLOT_CERTIFICATE.md) proves actual saturation by
bitmap and token identity rather than error8 alone. Root safety/coverage and
native Windows controls pass:1086 free,1085 with an exact held slot,1086 after
release. Forced early error8 is rejected; all temporary owners retire. Actual
PEB build26300 differs from DLL file version26100; the wrong-target native
result is retained as failed. FLS and full-DLL qualification remain open.
+0% progress; parent85.5%, M2 94%, Time Ended TBD.

### Final Windows physical matrix and independent M2 review (2026-10-21)

Atomic unit `docs(vgpu): record final Windows physical qualification` records
12actual host/guest cases:48exact positiveGPUlifetimes and6expectedpostflush
cleanup failures, natural controller/worker0 and complete owned retirement.
[Evidence and applicability](WINDOWS_PHYSICAL_FINAL_2026_10_21.md).
Independent M2 review recomputes348package entries and both41-file runs with
zero mismatches and no source defects; frozen M2 paths remain8038ad9. Its
prospective ownership contract passes, while historical kernel/USER failures
remain failed/unattributed. Exact current CI remains pending. Review artifacts
are preserved under `build/qualification_feature_20261021/final_scope_reviews`.
All tasks +0%; total85.5%, M2 94%, Time Ended TBD.

### Native FLS certificate and combined fixture controls (2026-10-21)

Atomic unit `test(opencl): gate exact native FLS exhaustion and retirement`
integrates Astra's fixed-storage certificate and native reload branch with
required portable/sanitizer/coverage/CI gates. Root270-case ASan/LSan/UBSan and
100%line/branch gates pass. Native four-case calibration passes4078→4077→4078,
with independent raw-marker review; the final300-input fixture passes both
slot controls and ten malformed CLI regressions. [Evidence](OPENCL_FLS_CERTIFICATE.md).
All tasks +0%; total85.5%, M2 94%, Time Ended TBD. Full candidate stress/heap/
overlap and final CI/review remain open. Commit `a845098` records CSR inventory
and native calibration, all tasks +0%; `7db0909` consolidates documentation,
all tasks +0%.

### Bind actual API heap arrays during native audits (2026-10-21)

Atomic `test(vgpu): bind complete native API heap snapshots` preserves each
validated GetProcessHeaps array and emits ordered handles, correcting the
verification assumption that PageHeap's PEB list is complete. Native retained/
freed CSR control and exact API rows pass; current San3 and94.53%branch gates
pass. [Contract, preserved failure and inverse proof](NATIVE_CSR_HEAP_CONTRACT.md).
All tasks +0%; total85.5%, M2 94%, Time Ended TBD. Commit `f873b18` records
Astra FLS certificate/gates and native controls; all tasks +0%, overall +0%.

### Complete-input OpenCL fault rerun (2026-10-21)

Atomic `docs(opencl): record complete-input cancellation qualification` binds
fresh native tcp_gpu_d62ddee8/session2112ca43451ab2a1 to the300-input/two-TU
fixture and current fullDLL70ca4fdb. Intentional authenticated host cancellation,
API completion and exact guest/host/module retirement pass;41rawfiles rehashed.
[Evidence/applicability](OPENCL_CANCELLATION_CONTRACT.md). Full reload/heap/overlap
and exact final CI/review remain required. All tasks +0%; total85.5%, M2 94%,
Time Ended TBD. Commit `4688cf8` records complete API-heap snapshots, all tasks
+0%, overall +0%.
