# Independent Codex review: M2 qualification remains blocked

Reviewed candidate: `8810544b114ee69f0b3990d51b987caebc774c70` on `feature/software-vgpu-slicing`, starting from a clean tree. Claude's final handoff and absence of child workers were verified before edits. Codex took over the preserved VM/GPU lease explicitly; two Luna reviewers at high effort performed read-only source and evidence reviews. Codex independently read the WSI state machine, native sink and ICD presentation/retirement callbacks, reviewed the fixture changes, ran the gates and checked the raw results.

**M2 remains 94% / Review; Time Ended remains TBD.** Software-vGPU remains 85.5%. No assertion, baseline, budget, workload, coverage threshold or task weight changed. Nothing was pushed, the PR was not marked ready, and nothing was merged. The machine-readable [independent receipt](M2_CODEX_RECEIPT_2026_10_21.json) binds the new evidence and sources.

## Changes made during independent review

- `c650401`: correct the diagnostics' buffer-growth policy. A required-size hint of 8 MiB plus one byte previously failed despite fitting the documented 16 MiB cap. Growth now saturates at that cap and rejects an oversized hint or another mismatch after the cap was actually tried. New boundary regressions reject the reviewed implementation and pass after correction under ASan, LSan and UBSan. This affects diagnostic availability, not the native verdict.
- `ff66d04`: add a real-Windows snapshot-growth case. It creates 1,024 private events, requires a decoded table larger than fits the initial 32 KiB buffer, verifies every event and its access mask, closes every acquired handle on every exit, frees the private snapshots and requires exact handle-count retirement.
- `bc89bb0`: correct console sign-in provenance and duration comparisons. At the user's explicit request, Codex entered the saved test-account credential through QMP; auto-logon stayed off. The original triangle receipt digest is retained in its transparent metadata amendment, and original bytes are preserved under `build/qualification_20261021_codex/before_signin_correction/`. No raw run artifact or source binding was changed.

Production `src/`, `include/`, dependency pointers, `.gitmodules`, `patches/` and the root README remain unchanged against `52a1a2970c2a182c155c718432a9dee520b0ca72`. The newer diagnostics source is covered by fresh execution below; the 30 earlier native receipts retain their original source binding.

## Independent verification

All commands ran from `/home/dev/Waddle-LSW`. Separate logs, command/exit records and tool sources are under `build/qualification_20261021_codex/`; this directory is evidence storage, not a replacement for tracked tests. The full portable run began at the candidate HEAD. Production sources stayed unchanged; its final diagnostics step used the capacity correction. The separate red/green regression and native runs bind that correction explicitly. The subsequent native-growth change affects only its standalone Windows test; later edits correct documentation only.

| Check | Observed result | Scope |
|:--|:--|:--|
| `make vgpu-wsi-lifecycle-test` | Exit 0; 12 Zig cases in Debug and ReleaseSafe, C API-double and unsupported-platform fixtures pass | Portable state/API behavior and allocator checks |
| `make vgpu-wsi-lifecycle-sanitizers` | Exit 0; 39 owned Zig functions instrumented, prescribed ASan/LSan/UBSan clean | Portable ownership; leak detection enabled |
| `make vgpu-wsi-lifecycle-coverage` | Exit 0; Zig branches 145/156 = 92.95%, native C branches 71/72 = 98.61% | Real production source over portable fixtures |
| `make vgpu-icd-units` | Exit 0; 394 cases in each of Debug/ReleaseSafe, 128-cycle native dispatch fixture in each | Portable integrated ICD behavior |
| `make vgpu-icd-coverage` | Exit 0; branches 4369/4837 = 90.32%, lines 4696/4766 = 98.53% | Unchanged coverage policy; exceeds 90% |
| `make vgpu-handle-diag-test vgpu-handle-diag-sanitizers` | Exit 0; 18 cases / 6,779 checks, source pin passes; deliberate-leak control exits 23 with an LSan report | Real diagnostics over scripted NT/Win32 APIs |
| Independent gcov of the real diagnostics include | 108/108 lines and 100/100 branch outcomes hit | Isolated host build; no exclusions or threshold change |
| New capacity regression against candidate diagnostics | Expected exit 1 at the half-cap-plus-one boundary | Proves the strengthened test detects the defect |
| Strict Windows cross-compilation | Exit 0 for corrected GPU fixture and expanded native diagnostics executable | Compilation, separately from execution |
| Native Windows diagnostics | Exit 0; 8/8 cases including snapshot growth, exact cleanup, no surviving owned test process | Actual Windows 11 build 26300 via WinRM; not an interactive-console test |
| Fresh `triangle_present` | Exit 0; 48 exact frames, 24 FIFO and 24 IMMEDIATE, 8 lifetimes | Interactive medium-integrity Windows through Waddle to the physical RTX 5080 |
| Fresh `wsi_lifetime` | Exit 0; 32 exact labeled cases, 8 lifetimes, including destroyed HWND and replacement retirement | Same actual guest/host GPU route |

Fresh GPU run folders:

- `build/windows11_vm/tcp_gpu_8cbf89ac`: baseline 199; 8/8 checks equal on sample 1; last check 55,395 ms after baseline.
- `build/windows11_vm/tcp_gpu_72dc797c`: baseline 199; 8/8 checks equal on sample 1; last check 38,203 ms after baseline.

Each replay used the frozen production input set and unchanged supervisor with a rebuilt diagnostic fixture. The manifests bind 68 new files; the receipt records the executed binary hash and compile inputs. Both Waddle-owned controller/worker retirements succeeded with status 0, empty owned sessions and no survivors or supervisor signals. The adopted vendor descendant still has raw wait status 9. This is not evidence of a normal exit or a leak-clean exit for every vendor descendant.

The first native-diagnostics collection attempt failed because PowerShell treated informational stderr as a terminating `NativeCommandError`. It has no acceptance credit and is preserved under `native_collection_attempt_1/`. The corrected collector drains both streams independently, obtains the actual process exit code and refuses an overlapping native review process. The subsequent native execution passed; no test failure was suppressed.

## Recomputed historical evidence

Both Luna and Codex separately recomputed the 30 candidate-source native runs. All 1,020 manifest-bound files and 30 manifests match, as do the result-file and runner hashes. Candidate fixture compile-input hashes match the exact candidate Git objects. Every actual output contains lifetime numbers 1 through 8 in order; all 240 retirement checks equal 199 on sample 1. The 20 triangle runs contain 960 exact frame lines, evenly split FIFO/IMMEDIATE; the 10 WSI runs contain 320 exact labeled cases. All runs have guest exit 0 and successful owned host retirement.

The replay drivers count eight exact-output lines without themselves requiring lifetime numbers 1 through 8 in order. That is a possible future false acceptance for malformed output, not an observed problem in these runs. Codex's independent verifier requires the exact ordered sequence for all 32 actual outputs. The drivers were not changed; their hashes remain applicable. The historical default build-executable path was rebuilt for the new runs; its original executed bytes remain preserved in every historical run folder and `build/qualification_20261021/executed_fixture_77a33415.exe`.

## Remaining blocker and concrete owner decision

`build/windows11_vm/tcp_gpu_7adb6000` still records a strict native failure: 199 to 200 handles after 42 frames, guest exit 1. It recorded no handle identity or creation trace. New passing runs cannot identify that handle retrospectively. The separate system-only failure establishes that OS activity can change the global count; it does not attribute this particular failure. No production defect causing that delta was established, and no borrowed handle was closed to make a count match.

The current criteria explicitly retain exact count equality and prohibit counting repeated passes as a waiver. **Under those criteria M2 cannot receive its remaining 6%.** Keeping the criteria leaves qualification blocked. An owner-approved change would require a new implementation and fresh verification; this review does not approve or apply one.

The concrete alternative for approval is an attribution-aware, fail-closed handle oracle:

1. Preserve workloads, baseline point, retry budget, exact pixel checks, memory/allocator gates and all original failure artifacts. Keep reporting the process-wide count.
2. Enable handle tracing before component initialization and capture handle identities and trace generations from process start, at baseline and at every retirement boundary. Validate this instrumentation natively before relying on its undocumented NT layouts. Unsupported tracing, truncated history, unresolved addresses or incomplete snapshots fail qualification.
3. Check every added/removed/reused identity even when the total count is unchanged. Fail on Waddle-attributed traces, changes to directly creatable resource classes, or any unexplained delta. OS-only churn is permissible only with complete, exact-build trace evidence accounting for the whole difference. No type-only allowlist, prewarming, elevated baseline or manual borrowed-handle cleanup.
4. Add negative controls for an intentional owned leak, an owned leak masked by an unrelated close, reused handle values, unresolved traces, overflow and missing tracing; add an OS-only positive control and native cancellation/error cleanup tests. Then rerun coverage, sanitizers and both physical GPU workloads with source-bound receipts and CI.
5. Record the amended acceptance contract and its prospective applicability explicitly. Retain the historical failure as an unaccepted result of the original protocol; never relabel it as attributed or passed.

This is an alternative protocol, not a completed fix. It needs the acceptance owner's explicit approval because it changes what the native gate measures. No evidence here authorizes that change.

## Limits and preserved state

CI has not run for the review commits, and nothing was pushed. WOW64, a loaded-host run, a repeated system-only control on this boot and a native RTL regression remain unexecuted. The report makes no new platform or RTL correctness claim. Failure-only delta printing is quadratic for very large handle tables; a private read-only benchmark took about 2.17 s for 100,000 entries with output discarded and a fast type-query stub. This diagnostic scalability observation is separate from the unchanged acceptance rule and the approximately 199-entry runs here.

All prescribed portable ASan/LSan/allocator checks passed without owned leaks. No new whole-process Windows heap acceptance is claimed. The OpenCL runtime-unload correction, DXVK heap ownership, transport-loss cancellation and simultaneous Linux-GPU/OpenCL acceptance remain separate open software-vGPU gates. Five unrelated nested Looking Glass submodules remain uninitialized; recursive initialization was inventoried, not claimed as passed.

QEMU 10732 and swtpm 10727 remain running on the preserved VM, with the signed-in explorer session present. All owned native/GPU jobs retired; the task-only publisher stopped and loopback port 55986 is closed. The lease records Codex as idle owner, with explicit handoff required before another runner. No VM reset, provisioning, garbage collection, pruning, unrelated process termination, README edit, push, PR-readiness update or merge occurred.

The only pending qualification decision is whether to retain the current strict-count protocol or authorize implementation of the concrete alternative above. Current progress and PR state remain unchanged either way until all applicable new gates actually pass.
