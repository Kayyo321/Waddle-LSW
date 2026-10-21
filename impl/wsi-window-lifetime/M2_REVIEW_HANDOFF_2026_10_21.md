# READY FOR INDEPENDENT CODEX REVIEW: Milestone 2 (`impl/wsi-window-lifetime`)

> Codex subsequently completed independent review, corrected the snapshot capacity, added native growth coverage and replayed both GPU workloads. Qualification remains blocked at 94%; see [M2_CODEX_REVIEW_2026_10_21.md](M2_CODEX_REVIEW_2026_10_21.md). This original handoff remains bound to candidate `8810544`.

From: Claude Code (implementation owner for this closure session, 2026-10-21). To: Codex (owner of independent review, verification, qualification and the decision to advance M2 to 100%).
Nothing in this file is an acceptance. It says the implementer's part of M2 is done, evidenced on the final sources and ready for you to check, and it names the one decision that is not the implementer's.

- Repository `/home/dev/Waddle-LSW`, branch `feature/software-vgpu-slicing`, base `52a1a2970c2a182c155c718432a9dee520b0ca72`. The exact review-candidate HEAD was `8810544b114ee69f0b3990d51b987caebc774c70`; later Codex corrections and new verification are recorded separately in `M2_CODEX_REVIEW_2026_10_21.md`. The working tree is clean, nothing was pushed, Draft PR 6 is untouched, `README.md`, `.gitmodules`, every submodule and `patches/` are unchanged since the base.
- **M2 stays 94% / Review, Time Ended TBD; the PR is not marked ready; nothing is merged.** Task #4's native gate keeps 0% of its weight until you decide (section 6). No claim of 100% is made.
- This handoff covers **M2 only**. The OpenCL runtime-unload correction (software-vGPU TODO #5) is a separate feature-accounting item, was not touched by this resumption, and remains **NOT READY** (see `../software-vgpu-slicing/CHECKPOINT_2026_10_21.md`).

## 1. What was wrong, and what changed

The earlier stop (checkpoint `7392d93`) left three M2 gaps. All are closed.

| Gap at the stop | Resolution | Evidence |
|:--|:--|:--|
| No instrumented 48-frame native run; the failure diagnostics had only been proved in isolation | 20 native `triangle_present` runs on the committed fixture with the diagnostics compiled in | section 3, receipt `ca52536` |
| Native receipts bound the old fixture hashes | two new receipts (30 run entries) bind the committed sources, the executed fixture hash (`77a33415...`) and the unchanged frozen guest inputs | section 3 |
| The `wsi_lifetime` workload had never executed with the changed destroyed-HWND ownership path (`6faf4ed`) | 10 native `wsi_lifetime` runs, all pass | section 3 |

Commits on the branch since the base (all docs or test-fixture changes; no `src/` or `include/` production file changed):

| Commit | Change | M2 #4 |
|:--|:--|:--|
| `7b6c9d5` | fixture: one `ReleaseDC` attempt per `GetDC` on failure | +0% |
| `6faf4ed` | fixture: clear the destroyed-HWND owner before the post-destroy checks | +0% |
| `faf9d19` | failure-only handle attribution diagnostics in the native fixture, host test, source pin of the acceptance rule | +0% |
| `44d69c2` | native Windows test of the diagnostics | +0% |
| `56b89a9` | independent-review fixes, CI step for the host tests | +0% |
| `cdd7690` | audit note, IMPL_DESC section, tracker checkpoint, diagnostics receipt | +0% |
| `7392d93` | checkpoint at the user stop (mostly OpenCL evidence) | +0% |
| `ca52536` | receipts of the 30 native runs | +0% |
| `d5f2361`, `0faf4cd`, and the review-fix commit(s) after them | docs sync (IMPL_DESC, tracker, audit update, checkpoint pointer, this handoff) | +0% |

Unchanged on purpose: the assertion in `handle_baseline()` (exact `GetProcessHandleCount` equality to the baseline, 100 samples x `Sleep(10)`, baseline taken once before the lifetimes), the workload (48 frames / 8 lifetimes; 8 lifetimes and 32 cases for `wsi_lifetime`), the budgets and the thresholds. The host test `tests/vgpu/tcp_gpu_handle_diag_host.c` pins the exact source text of that rule, so a literal-text edit of it fails that test (a guard against accidental edits only: macros or loop edits elsewhere are not caught, and the `git diff` is the real proof). None of the forbidden remedies was used: no borrowed handle closed, no prewarmed or elevated baseline, no reduced workload, no widened budget, no assertion replaced by logging, no redefined acceptance.

## 2. Final-source portable gates

HEAD `7392d93` (the `ca52536` and docs commits change no code), incremental builds, logs with `### EXIT=` and SHA-256 under `build/qualification_20261021/gates/`. Toolchain from PATH: Zig 0.13.0, GCC 14.2.0 (Debian), clang-19.1.7; `ptrace_scope` 0 so LeakSanitizer is live.

| Gate | Result |
|:--|:--|
| `make vgpu-wsi-lifecycle-test` | exit 0; Zig 12/12 (Debug and ReleaseSafe), native C API-double and unsupported-platform tests |
| `make vgpu-wsi-lifecycle-sanitizers` | exit 0; ASan/LSan/UBSan with `detect_leaks=1`, 39 owned functions instrumented, zero leaks |
| `make vgpu-wsi-lifecycle-coverage` | exit 0; Zig branches 92.95% (145/156), lines 99.33% (148/149); native C branches 98.61% (71/72), lines 100% (47/47) |
| `make vgpu-icd-units` | exit 0; 394/394 in Debug and in ReleaseSafe, 128-cycle dispatch fixture in each |
| `make vgpu-icd-coverage` | exit 0; branches 90.32% (4369/4837), lines 98.53% (4696/4766) |
| `make vgpu-handle-diag-test vgpu-handle-diag-sanitizers` | exit 0; 18 cases / 6724 checks plain and under ASan+LSan+UBSan; LeakSanitizer self-control exits 23 |
| Windows cross-builds (`build/vgpu_win32_present_test.exe`, `build/vgpu_handle_diag_native.exe`, `build/vgpu_tcp_gpu_windows_draft.exe`) | up to date in the gate run; then freshly compiled at HEAD with the same `zig cc -target x86_64-windows-gnu ... -Werror` recipes into scratch paths, exit 0 (the executed binaries stay untouched) |

Also: `clang-19 -Werror -O2` and `gcc -O2 -Werror` build the diagnostics host test and it passes (no clang sanitizer runtime is installed, so that adds no sanitizer evidence). The numbers equal the baseline recorded on the unchanged base HEAD (`build/qualification_20261020/baseline/SUMMARY.md`). **CI has not run for these commits** (nothing was pushed; CI installs Ubuntu's win32 toolchain, not the reviewed GCC 14.2).

## 3. Native evidence on the committed fixture

Setup: the existing Windows 11 VM (guest build 26300) booted from its preserved disk after a second host reboot (never provisioned or reset; `qemu-img check` clean before boot; a backup taken earlier in the session is in `build/vm_backup_20261021`); Codex signed in at the console through QMP using the saved test-account credential at the user’s explicit request (auto-logon stayed off); each run launched on the interactive desktop through Win+R key events at medium integrity; the physical RTX 5080 through the host Waddle worker. The byte-identical copies of the 2026-10-17 supervisor drove each run with the frozen guest ICD, bootstrap, loader, worker, runtime and controller (hash-equal to the two qualified 2026-10-17 runs). Only the guest fixture executable differs: instrumented build of the committed sources, sha256 `77a334150077b0702c0a4144124b760871ec2e1bb41a7967cc774139773bb66d`, 317,440 bytes (preserved as `gpu.exe` in every run folder and in `build/qualification_20261021/executed_fixture_77a33415.exe`).

| Workload | Runs | Passed | Baseline | Lifetime checks equal to the baseline | Last check after baseline | Per lifetime |
|:--|--:|--:|--:|:--|:--|:--|
| `triangle_present` | 20 | 20 | 199 every run | 160/160, each on the first sample | 55.06-55.80 s (median 55.34) | 6.82-7.39 s |
| `wsi_lifetime` | 10 | 10 | 199 every run | 80/80, each on the first sample | 38.12-38.78 s (median 38.44) | 4.72-5.11 s |

Receipts: `../software-vgpu-slicing/M2_INSTRUMENTED_TRIANGLE_PRESENT_RECEIPT_2026_10_21.json` (sha256 `a3700a8dd8bce76ebf7f5618b799480f83530b6d17dae14c223c12a73f6fd91a`) and `../software-vgpu-slicing/M2_INSTRUMENTED_WSI_LIFETIME_RECEIPT_2026_10_21.json` (sha256 `350526b39772e5d427928898b0dbca1042d28dd49840cf53de42329ef6658f68`). They bind the runner hashes, supervisor copies, fixture sources and executable, frozen input hashes, VM identity, per-run guest output analysis, per-lifetime `handle_diag` timeline, host retirement fields and the evidence-manifest hash of every run folder. Cumulative results files: `build/qualification_20261020/m2_instrumented_results.json` (sha256 `7aee8c92730f82d9ddfe12ee48a2597fe195cb60bd51b6385380d3028c56daf1`) and `m2_wsi_lifetime_results.json` (sha256 `c6dbc95195013acfd40c0129d70f8069e2f3a3722e70ba43343ae4a5cf9cd971`). Raw run folders (about 945 MB): `build/windows11_vm/tcp_gpu_<id>/`.

Every pass requires the supervisor's own assertions plus an independent runner check: guest exit 0, the exact output shape (8 exact-output lines and 48 frame lines with 24 FIFO + 24 IMMEDIATE for `triangle_present`; 8 exact-output lines and 8 of each of the four labeled WSI cases for `wsi_lifetime`), no legacy handle-failure line, guest binary hashes verified after the run, and no live controller, worker or guest process afterwards. Host retirement fields were `worker_retired=1 status=0 worker_exit_status=0`; the vendor descendant keeps wait status 9 (the pinned virgl teardown's own SIGKILL path), as in the earlier receipts, and no claim is made that every vendor child exited normally.

Disclosures: run 1 of each workload is a pilot invocation, the rest a second invocation with continue-past-failure enabled (none failed). A light offline harness of an evidence-tooling worker (nice 19, under `unshare -rn`, no VM/GPU/network contact) overlapped triangle runs 18-20; those runs show all-pass pre-checks and the same timings. The `wsi_lifetime` driver `run_m2_instrumented_wsi.py` is a workload-generalised copy of the triangle runner (diff `run_m2_instrumented_wsi.diff`, sha256 `5e7c2cf6870e31f62e78cc10cfe93e5b01cf2f64cd2623a11a4d4fad51368bfc`; 32/32 offline mock scenarios, a 24/24 old-versus-new equivalence check on `triangle_present`, 85 real guest status files compared); the supervisor's own `wsi_lifetime` assertions are the unchanged 2026-10-17 ones. Both runners live in the git-ignored `build/qualification_20261020/m2_runner/`; their hashes are in the receipts.

## 4. What these runs do not show

- The preserved `tcp_gpu_7adb6000` failure (199 to 200 after 42 frames) did not recur, and it is **neither replaced nor attributed**: no artifact records which handle appeared. The audit's rule stands: repeated passes do not waive it. The only direct evidence about the oracle's sensitivity is the 2026-10-17 system-only control (no Vulkan, no Waddle, +1 at 59.8 s) and the diagnostic traces of balanced RPC garbage-collection churn.
- Duration is **not discriminated** by these runs. The 66-68 s and 82 s figures of the earlier runs are controller-ready to host-result times; on that same clock the new `triangle_present` runs took 65.1-68.6 s (no faster than the earlier passes at 66.4-68.5 s), and the earlier `wsi_lifetime` run `tcp_gpu_683c0ee2` took 71.1 s (new runs 48.1-48.9 s) and still passed. Only the failed run (81.8 s) was clearly slower, and whether slowness explains it is untested. A slowed-host diagnostic was considered; one attempt to restrict the VM process's CPU affinity was blocked by the harness permission policy and not pursued, and the user did not approve it. It remains open for whoever holds the lease.
- Not exercised: WOW64, the diagnostics' class-51 buffer growth path, any run of this fixture on a loaded host, a repeat of the system-only control on this boot.
- Not added: a native right-to-left (`WS_EX_LAYOUTRTL`) regression in `tests/vgpu/win32_present.c`. The audit's inferred `StretchDIBits` defect did not reproduce in a native probe (positive scanline counts on every layout), so no production change was made; the regression test remains an optional follow-up.
- Statistics: zero failures in 20 `triangle_present` runs bound the per-run failure probability below about 14% (one-sided 95%), and zero in 10 `wsi_lifetime` runs below about 26%; they cannot show the rate is zero. The earlier history on the old fixture (six passes and one failure of the same workload) is consistent with a rate of that order.

## 5. Verify it yourself

```bash
git rev-parse HEAD && git status --porcelain                     # clean
git log --oneline 52a1a2970c2a182c155c718432a9dee520b0ca72..HEAD
git diff --stat 52a1a2970c2a182c155c718432a9dee520b0ca72..HEAD -- README.md .gitmodules submodules patches src include   # empty
git diff 52a1a2970c2a182c155c718432a9dee520b0ca72..HEAD -- tests/vgpu/tcp_gpu_windows.c tests/vgpu/tcp_gpu_present_windows.inc tests/vgpu/tcp_gpu_wsi_lifetime_windows.inc scripts/vgpu_tcp.mk
make vgpu-wsi-lifecycle-test vgpu-wsi-lifecycle-sanitizers vgpu-wsi-lifecycle-coverage
make vgpu-icd-units vgpu-icd-coverage vgpu-handle-diag-test vgpu-handle-diag-sanitizers
```

Receipts (read-only; recomputes every bound hash, 1,020 run-folder files in this session, zero problems):

```bash
python3 -I - <<'EOF'
import hashlib, json, pathlib
root, problems, files = pathlib.Path.cwd(), [], 0
for name in ('TRIANGLE_PRESENT', 'WSI_LIFETIME'):
    receipt = json.loads((root / f'impl/software-vgpu-slicing/M2_INSTRUMENTED_{name}_RECEIPT_2026_10_21.json').read_text())
    if hashlib.sha256((root / receipt['results_file']['path']).read_bytes()).hexdigest() != receipt['results_file']['sha256']:
        problems.append(name)
    for run in receipt['runs']:
        manifest = root / run['evidence_manifest']['path']
        if hashlib.sha256(manifest.read_bytes()).hexdigest() != run['evidence_manifest']['sha256']:
            problems.append(str(manifest))
        for relative, info in json.loads(manifest.read_text())['files'].items():
            files += 1
            path = manifest.parent / relative
            if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != info['sha256']:
                problems.append(str(path))
print('files re-hashed', files, 'problems', len(problems))
EOF
```

Native re-run (needs the VM/GPU lease and a signed-in console; do not use QMP, WinRM or the GPU from another session while it runs, keep the host otherwise idle):
`make build/vgpu_tcp_gpu_windows_draft.exe` (a rebuild changes the PE timestamp and therefore the hash; compare behaviour, not hashes), start `build/qualification_20261017/public_binaries_resume.py` (loopback publisher on 127.0.0.1:55986), then
`python3 -B build/qualification_20261020/m2_runner/run_m2_instrumented.py --dry-run` followed by `--repetitions N`, or `run_m2_instrumented_wsi.py --workload wsi_lifetime --dry-run` / `--repetitions N`. Never `python -O` (the acceptance is assert-based; the runner refuses).

Suggested review focus: (1) that the acceptance rule and workload are byte-for-byte the originals in the fixture diff, including the success path of the changed destroyed-HWND code; (2) that the diagnostics are passive on success (the host test pins this; the 30 native runs support it); (3) that the receipts claim nothing beyond section 4; (4) the runner generalisation if you rely on its `wsi_lifetime` verdicts; (5) the decision in section 6.

## 6. The decision that is not the implementer's

Agents may not redefine acceptance. The audit (`M2_HANDLE_GATE_AUDIT_2026_10_21.md`, with the update after the resumption) lays out the options without endorsement:

- **A. Keep the oracle as written.** M2 stays blocked on this Windows image; future failures are attributable. The fresh 30/30 passes do not waive `tcp_gpu_7adb6000` (the audit's rule).
- **A'. Count the native gate despite the unexplained failure.** This is a waiver in effect and needs an explicit, recorded owner decision; it is named separately so it is not mistaken for A. Not proposed.
- **B. A recorded criteria amendment, not a waiver,** to an attribution-aware strict oracle (handle-table snapshots with tracing from process start; fail on any Waddle-attributed, directly-creatable-type or unattributed delta). Strictly stronger for Waddle-owned handles and catches offset swaps; costs a new oracle and its tests.
- **C. Change the platform under test.** Not proposed.

My assessment, which is not a decision: no Waddle-owned code path explains the failure (static proof from the import closure plus every rare path provably not run), the oracle is process-global and can hide an offsetting real leak, and the fresh runs give no reason to suspect a Waddle defect. They also do not discriminate any cause of the historical failure. If you need that failure attributed, only B (or a protocol that can reproduce it) can supply it; A' is a waiver decision, not an evidence outcome.

## 7. Live state at this handoff (verify, do not trust)

- Git: as in the header. Nothing pushed; `build/qualification_20261020/cand_clone` (OpenCL candidate) is a `git clone --shared` of this repository: **do not gc or prune this repository** while it is needed.
- VM/GPU lease: held by this session, recorded in `build/qualification_20261020/vm_lease.json`. The Windows 11 VM is **running** (QEMU pid and swtpm pid in `build/windows11_vm/qemu.pid` / `tpm.pid` and the lease; WinRM 127.0.0.1:55985, VNC 127.0.0.1:19) with the console signed in. No guest test and no GPU job is running. Release or take over the lease explicitly; a fresh boot needs console sign-in again.
- The loopback artifact publisher this session started for the native runs (`build/qualification_20261017/public_binaries_resume.py`, port 55986) was identity-checked (own pid file, command line, working directory, listener) and **stopped**; port 55986 is closed. Restart it only for new native runs. Thirty guest folders `C:\tcp_gpu_<id>` and thirty host folders under `build/windows11_vm/` are intentionally preserved.
- No build, test, runner, controller, worker or GPU job of this session is running; the only live processes it owns are the VM's QEMU and swtpm.
- The host rebooted twice during the session; `/tmp` scratch is volatile, key tools and evidence are under `build/qualification_20261020/` and `build/qualification_20261021/`.

## 8. Independent review of this package

Independent Opus review of HEAD `0faf4cd` (read-only, adversarial): **PASS WITH FIXES**. It recomputed all 30 runs from the raw folders (all 240 checks equal on the first sample, 1,020 manifest-bound files re-hash cleanly, executed `gpu.exe` hash consistent, no hidden or extra run), found no acceptance change on the success path of the fixture diff, ran the host gates (all pass), and found the runner generalisation no looser than the supervisor. Its blocking findings were documentation only and are fixed in the review-fix commit: option A had been reframed (restored; the waiver option A' is named separately), the duration comparison mixed metrics (restated like-for-like), and this section was a placeholder (this text). Should-fix items applied: pin wording, commit attribution and contributors header, pointer from `CODEX_HANDOFF.md`, consistent slowed-host disclosure, supersession notes. Left open and disclosed: the diagnostics' gcov 100% and mutation results are manual (no make target); audit line numbers refer to base HEAD.
