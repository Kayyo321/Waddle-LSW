# M2 current-source loaded-host native qualification, 2026-10-21

Result: both complete `m2_owned_v1` workloads pass. M2 remains94% / Review
until applicable exact-candidate CI and final review complete. Parent feature
acceptance remains separate. No merge and no Time Ended recorded.

## Frozen inputs and execution

Source8038ad9; subsequent3e3d205 changes documentation only. Package
`build/qualification_feature_20261021/m2_owned_candidate_8038ad9/`, manifest
SHA256206f1474f3a67dd8a4af5e27f57ea6e5606019a4ca0001d258813c34f7ab7ebb.
The package records a forced fresh qualified ICD, native fixture and bootstrap
cross-build; unchanged accepted worker/renderer reuse has a42-input source and
recipe equality audit. Each actual native run freshly compiles its controller.
The ordinary ICD retains its11-export boundary.

Root ran the existing signed-in Windows11 VM against the real RTX5080 while
six exclusively owned CPU hash workers ran. No affinity, unrelated process or
VM state changed. Exact PID/start identities, per-second CPU progress, work
counters and natural retirement are preserved in
`build/qualification_feature_20261021/m2_owned_launch_8038ad9_revision2/`.
All six load workers exited0 without stderr. The unmodified full runner,
package and per-run manifests remain preserved.

| Workload | Native folder | Session | Full work | Ownership boundaries |
|:--|:--|:--|:--|:--|
| triangle_present | build/windows11_vm/tcp_gpu_8fd00fda | 9c062c8b9394af1f | 8 lifetimes,48 exact4096 RGB/BGRA frames, FIFO and IMMEDIATE | initial+8 healthy, zero retained M2 owners |
| wsi_lifetime | build/windows11_vm/tcp_gpu_94f4964b | 61a41d29dc325204 | 8 lifetimes,32 failed-replacement/zero-area/resize/destroyed-HWND cases | initial+8 healthy, zero retained M2 owners |

Both native processes exit0 naturally, provide the matching retirement Ack,
and have current guest artifact hashes verified. Controller and worker exit0;
exact owned host graphs are empty with no supervisor signals, observer or
journal errors. Pre/post guest console and stale-process checks pass. The VM
lease returns idle with the original QEMU/swtpm identities preserved. Root
independently rehashed82 manifest-bound run files and checked all verifier,
host, console and CPU-worker receipts. Root analysis:
`build/qualification_feature_20261021/m2_loaded_root_review.json`; complete
runner result: `m2_owned_runs_8038ad9_revision2/results.json` below that same root.

## Handle diagnostics and unchanged acceptance

Both processes start at199 total handles. Triangle's lifetime counts are
199,199,199,199,199,200,200,200; WSI's are199,199,199,199,199,199,199,200.
Differential snapshots preserve the observed additions/removals. These totals
are diagnostic under the human-approved ownership amendment; they do not
identify or pass the original historical failure. Explicit ICD, sink DC,
fixture DC and fixture HWND checks pass at every boundary. All original exact
pixels/cases and strict GDI/USER equality remain unchanged and pass.

The historical strict199-to200 failure, earlier fresh USER4-to9 failure and
all other raw failures remain failed and preserved. This new result does not
attribute those records, subtract counts, raise baselines, lengthen budgets,
add a warm-up or retry an assertion. The failure-only HWND collector does not
emit a failure report in these two passing runs.

## Other current evidence and limits

Native Windows CI at8038ad9 passes the actual handle-growth/ownership and GUI
retained-top/child/message controls plus their exact owned cleanup. Root GUI
control `m2_gui_native_df4d92e7/` passes separately. Portable399-case ICD tests,
90.42% branch coverage, native API-double coverage and prescribed sanitizers
remain applicable to unchanged M2 production inputs. The current37-module
seam has actual instrumentation and zero sanitizer findings.

The remaining Linux CI job and final independent review are still required.
No OpenCL heap/reload/cancellation/overlap or DXVK heap acceptance is inferred
from these M2 runs. This evidence checkpoint changes task progress by+0%.
