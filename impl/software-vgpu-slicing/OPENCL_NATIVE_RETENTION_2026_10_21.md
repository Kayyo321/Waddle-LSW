# OpenCL Native Retention Diagnostic — 2026-10-21

Status: **qualification failed; attribution pending**. This records a reproducible
resource delta, not acceptance or a production root cause. M2 remains94%/Review;
parent85.5%/In Progress; both Time Ended values remain TBD.

## Exact candidate and execution

Commit `7d5c6bb` changes failure diagnostics only. Fresh private GCC14 Win32
fixture `26de712fd3197970cb4fe54680d9c002c1b760e51aa755db74e4cf72b7739b28`
is bound by the300-input/two-TU build receipt
`aeb74c5204e496b20d4f448752f8b66f14e02ed766a53634d5587e88db8a0910`.
Fresh native TLS/FLS retention controls and ten malformed CLI cases pass in
`build/qualification_feature_20261021/opencl_reload_controls_native_3e985cb1`.
Full candidate DLL remains`70ca4fdbc2ca54954b7879f28f11e013c63a0a660cca718815abc24e98d5887a`;
its production/build inputs are unchanged.

Root reviews R7's1100 unique receiver slots, ports/tokens, exact five deployed
assets, current213 production files and pre-run planSHA
`73ebc365a78e04400df1ef6f6bb702816100f6823d76aae3b6641735cb8d1d2e`.
Artifact root:
`build/qualification_feature_20261021/opencl_reload_final_7d5c6bb_revision7/`.
NativePID4036/creation`01dd61aa01b562ab`, authenticated session
`92677681500fe2ff`, foreign callerTIDs3364/4816.

All three exact GPU context lifetimes, malformed buffer/event checks, foreign
caller joins, terminal shutdown/session stop and module absence pass. The
first post-cycle equality rejects:

| Snapshot | Process handles | Free TLS slots | Free FLS slots |
|:--|--:|--:|--:|
| Entry |53|1086|4078|
| Four held idle threads baseline |65|1086|4078|
| After first complete module lifetime |148|1083|4075|

Failure record identifies`stage=cycle cycle=1 predicate=!reload_equal(&baseline,&observed)`.
The measured delta is+83handles and three occupied slots of each kind. Counts
alone do not identify owners, prove per-reload growth, or permit a warm-up
waiver. Zero strict cycles were accepted. Root separately queries the guest:
native and signed batch naturally exited1 and are absent. The single host
controller/worker exits0 with an empty owned graph, no supervisor signals and
all pidfds closed. Raw streams, pre/post identities and failure are preserved;
`root_diagnostic_review.json` binds the reviewed files.

## No-vendor CRT control

`build/qualification_feature_20261021/crt_attribution_native_8fdf2f27/` contains
three fresh processes (raw CreateThread, CRT `_beginthreadex`, and CRT plus
formatted output), each with four persistent idle threads and three joined
workers. Handles remain58→70→70/70/70→58 in raw mode and
52→64→64/64/64→52 in both CRT modes. TLS/FLS remain1086/4078 throughout.
Each process naturally exits0 with no survivor; exact OS and executable hashes
match before/after. Binary`a1791f55f4915796d474df8e84cc762d7b83548f45730bd5a107b95b0dc848a1`
imports only KERNEL32/MSVCRT. Independent review rehashes280 dependencies and
all native records under`crt_native_independent_review/`.

This control does not reproduce the candidate's delta. It does not measure
heap ownership or rule out every CRT code path. The copied CONTROL.md and
build-only receipt describe pre-execution status; separate native results and
this record supersede that wording without modifying frozen inputs.

## Physical Linux matrix and overlap

The unchanged Linux physical matrix completes15cases: nine positive cases
with144 GPU lifetimes and six expected post-flush cleanup failures. ReportSHA
`9a1abd7b26a440f54e53521c2d12a4a1276ddbbffce997ac02dc31e5e47611df`.
A source/PID/start/driver-bound observer brackets the R6 native first-cycle
GPU workload with four accepted Linux GPU observations on shared`/dev/nvidia0`;
witnessSHA`d5bed9bb143f05ab6f29c7c801a2ef0810601d3b534956930566500658e05a1e`.
Root review:`build/qualification_feature_20261021/linux_opencl_overlap_revision2/root_review.json`.
This establishes functional concurrent workloads, not simultaneous instruction
issue or reload/heap acceptance. The earlier observer attached to the failed
R5 plan remains failed; its physical child independently completed the matrix.

## Remaining work

Attribute first-load retained resources with source-bound module, handle and
thread inventories; correct any owned defect and rerun strict1100-cycle
qualification. Complete all-heap DXVK/OpenCL lifetime inventories and zero-owned
retention proof, final source-applicable checks/CI and independent review.
No assertion, threshold, count gate, deadline or historical failure is waived.
