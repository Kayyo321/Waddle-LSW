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
(186,373,276 bytes). Current source hashes match. This image has not yet run
natively. Historical functional results are not relabeled as this candidate.

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
These controls do not qualify the full WSI workload. Fresh full M2 qualification
is running with six bounded root-owned CPU workers; no result is credited yet.
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
