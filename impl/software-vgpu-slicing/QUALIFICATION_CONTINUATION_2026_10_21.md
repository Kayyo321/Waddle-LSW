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
correction requires fresh affected fixtures/admission; a full new build is in
progress at01702c5 in `root_tls/full_driver_build_v3/`. Historical results are
not relabeled as the final candidate.

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
collector is being independently reviewed before fresh native diagnostics.
All original comparisons, sampling, timing and UI behavior remain mandatory.

## Remaining gates

Current complete37-module ICD seam ASan/LSan passes; physical Linux15-case
matrix is running. Exact-head CI is rerunning after5260170 and01702c5.
The first-load malloc12345 negative control proves UMDH alone misses an owned
allocation; full native heap acceptance remains open until a calibrated
alternative inventory proves ownership/retirement. Full OpenCL TLS/FLS/reload,
actual in-flight cancellation, host-GPU overlap, final three-lifetime DXVK heap,
Windows physical matrix, M2 native qualification and final review remain open.

All raw failure folders are preserved. The existing VM and its state are
preserved; root remains sole native/GPU runner. No gc/prune: the candidate
clone shares repository objects. +0% credit to all tasks at this checkpoint.
