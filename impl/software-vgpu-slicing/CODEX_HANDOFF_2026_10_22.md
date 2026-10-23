# Codex handoff: finish software vGPU slicing (PR #6), 2026-10-22

This file hands the remaining software-vgpu-slicing qualification from the Claude
continuation session to Codex. It lists what is proven, what is still open, and the
exact steps to reach 100% with every CI check green. Read it fully before acting.

## 0. Read first

1. `AGENTS.md` at the repository root. It governs naming, commits, coverage,
   zero-leak policy and the README invariant.
2. This file, then `TRACKER.md` and `IMPL_DESC.md` in this folder.
3. `OPENCL_RETENTION_ATTRIBUTION_2026_10_22.md` sections 13 to 17. They explain
   the reload contract amendment 4, the R10 pass, the OpenCL heap failure and the
   `llvm_shutdown` fix.
4. `OPENCL_RELOAD_CONTRACT.md` and `FABLE_HANDOFF_2026_10_21.md` for older context.
   Older entries legitimately name superseded DLLs and percentages. The newest
   entry governs.

## 1. Hard rules (carried over verbatim from the user's handoff)

- Read credentials from `build/windows11_vm/credentials.json` only in memory; never
  print or commit them.
- Do not gc or prune. An OpenCL candidate clone shares repository objects.
- No force pushes, history rewrite, unrelated cleanup or process termination.
- Preserve README.md, submodule pins, `.gitmodules`, all evidence and VM state.
- Do not reset, reprovision or restore a VM backup.
- Take the sole VM/GPU lease, and do not bypass ownership checks.
- Do not run the old unguarded R10-pvs capture again. R11 and R13 guard revisions
  are invalid; the current guard is R22.
- No force kill of the target, arbitrary ResumeThread or CPU-affinity workaround.
- Obtain human approval for any further upstream private-copy changes (clvk, LLVM,
  clspv, loader, DXVK sources).
- Push to PR #6; do not merge. Never fabricate 100%.
- Do not weaken gates, waive unexplained ownership failures or fabricate results.
- Auto-logon is off. After a guest reboot a human must sign in at the console.
  Never enter passwords yourself.

Note on names: "R11" and "R12" in this file mean OpenCL reload run revisions 11 and
12 (section 5). They are unrelated to the invalid CDB guard revision R11 and to the
R12 bulk PageHeap capture script.

## 2. Repository and PR state at handoff

- Checkout `/home/dev/Waddle-LSW`, branch `feature/software-vgpu-slicing`, target
  branch `origin`. PR: https://github.com/Kayyo321/Waddle-LSW/pull/6 (open, draft).
- Last source/test change: `9f40010` (probe `llvm_shutdown` stand-in). Later
  commits are documentation only. The commit adding this file is the handoff
  checkpoint; find it with
  `git log -1 --format=%H -- impl/software-vgpu-slicing/CODEX_HANDOFF_2026_10_22.md`.
- Branch protection on `origin` requires the `cli` and `guest` checks. The
  workflows also run `windows`, `transport`, `windows_transport` and
  `opencl_admission`. All of them must be green on the exact final head.
- Commit helper: `build/root_todo3_continuation/commit_owned.py` holds
  `/tmp/waddle_git_index.lock` and stages only explicit paths. Every commit needs a
  detailed body, an `Affects TODO:` line, and a matching row in the TRACKER commit
  log. Commit trailers used so far end with a `Co-Authored-By:` line.

## 3. Live VM, GPU and lease

- Preserved Windows 11 VM. QEMU PID 7564 (start ticks 25543), swtpm PID 7558 (start
  ticks 25429), both started 2026-10-22 10:48 local. Verify executable and start
  time, not PID alone. WinRM 127.0.0.1:55985; QMP `build/windows11_vm/qmp.sock`.
- Lease file: `build/qualification_20261020/vm_lease.json`. A runnable lease has
  `runner_state` starting with `idle; ` and no active, uncertain or running state.
- Retained guest process: fixture PID 7984 from a failed guard R16 heap run is left
  suspended in the guest. It was not killed because process termination is
  forbidden. It holds no IFEO key; the IFEO key was restored exactly. The user has
  not yet decided what to do with it. Do not terminate it without their approval.

### Taking the lease

At handoff the lease is released to idle with `handoff_next_owner` naming Codex
(section 5 records the final state). Every frozen runner, including heap r4, goes
through `build/qualification_feature_20261021/lease_owner_wrapper_r1/lease_owner_wrapper.py`
(sha256 `94a367b8…463bd8d`). It requires the lease `owner` to equal exactly
`Claude Opus 5.5 Fable-handoff continuation (sole VM/GPU runner)` and
`previous_owner` to equal the frozen owner.

Codex therefore cannot run heap r4 under its own owner name with wrapper r1
unchanged. The correct route:

1. Verify the VM identities above and that no other owner intervened since handoff.
2. Write `lease_owner_wrapper_r2/` as a new revision directory. It must accept a
   lease whose `owner` is Codex's exact owner string and whose ownership chain
   records the Claude continuation owner as the prior holder. Change nothing else.
   Add inverse-patch tests in the style of r1.
3. Wrapper r1 was user-approved on 2026-10-21. Get the user's approval for r2 before
   using it, because it relaxes an ownership equality.
4. Make heap r4's `run_heap.py` a new revision (r5) that pins wrapper r2 by hash.
   Do not edit r4 in place; revision directories are evidence.
5. Take the lease: set `owner` to Codex's string, `previous_owner` to the Claude
   continuation owner, and `runner_state` to `idle; taken over by Codex …`.

Never edit the lease to impersonate the Claude owner string.

## 4. Proven so far

| Area | Evidence | Result |
|:--|:--|:--|
| Windows physical matrix revision 2 | `build/qualification_feature_20261021/windows_physical_r2` | 12/12 pass on the patched loader |
| M2 revision 4 | `m2_owned_runs_2e411c4_revision4` | triangle_present and wsi_lifetime pass |
| DXVK guarded three-life heap | `build/windows11_vm/dxvk_heap_ab2efe3cbec4fc1f` | zero OWNED blocks |
| OpenCL reload R10 (old DLL) | attribution section 15 | 1,100 cycles, exact handle/TLS/FLS equality |
| OpenCL heap r3 (old DLL) | `opencl_heap_1e336986` | FAIL, 27 OWNED blocks per load |
| `llvm_shutdown` fix | commits `23e9275`, `99d333a`, `9f40010` | unit tests 6/6 plain and sanitized, 100% coverage |
| Rebuilt OpenCL.dll `8552b3a1` | census r6 `opencl_census_523ca7d6` | zero handle/module/TLS/FLS deltas over 6 lives |
| Native controls r8 | `opencl_reload_controls_native_cdefffa2` | pass |
| CDB guard R22 | `cdb_guard_r22`, controls r5/r6/r8/r9 | pass |

Rebuilt artifacts (ignored build outputs, preserve them):

- OpenCL.dll: `build/clvk_client/clvk_eb5cfe741d16_oa6jyccs/out/OpenCL.dll`, sha256
  `8552b3a12dda4010c3fcac7958229fdef84e6369e42c99f81334b3a5a991a410`. Receipt
  `build/clvk_client.json`, sha256 `61f2ce53…88ed5aa60`.
- Fixture: `opencl_cancel_fixture_903957f1/opencl.exe`, sha256 `d519743e…64eed587`.
- Census r6 and heap r4 bindings, sha256
  `ce9daeaa5457db25ed5d3a83842d6919446df5f15669f81504adb00d13938fc9`.

## 5. Results at handoff

### 5.1 OpenCL reload R11 on the rebuilt DLL

**R11 was interrupted and is not evidence.** It started 2026-10-22T22:16Z under the
Claude Code session's shell. When that Claude Code process exited at about
2026-10-22T23:22Z, the host executor died with it. The last complete record is
cycle 346 of 1,100; the guest was in cycle 347. All cycles up to 346 kept exact
handle equality (104 handles), but a partial run proves nothing for the
1,100-cycle gate.

Retirement (user approved a graceful stop of PID 588 only):

- Guest `opencl.exe` PID 588 was blocked waiting for a host retirement record. It
  exited on its own at 23:25:59Z with exit code 1 ("OpenCL reload fatal: outer
  caller did not join within its deadline"), before any action was taken. No
  process was terminated and no `retired.txt` was written. PID 7984 is untouched.
- Evidence: `build/qualification_feature_20261021/opencl_reload_final_r11_interruption/`
  (`retirement_result.json`, the unused `retire_r11_session.py`, and
  `vm_lease_before_idle.json`). The run folder `opencl_reload_final_r11/` is
  preserved as-is.
- The lease was set to `idle; R11 … INTERRUPTED …` at 23:27:23Z with an
  `interrupted_run` field, after re-verifying the QEMU and swtpm identities.

The reload contract needs one uninterrupted process lifetime, so R11 could not be
resumed.

### 5.1a OpenCL reload R12, the rerun that replaces R11

- **Runner:** `build/qualification_feature_20261021/opencl_reload_runner_r12/` is a
  copy of the R11 runner. Its executor, supervisor, admission and guest CLI files
  are byte-identical to R11. Only the composite generator changed:
  `make_composite_r12.py` writes `composite_admission_r12.json` (sha256
  `d2b0a1db86eead36f1e05692e3285ac9cf515e120874d17aa049f5912fd92c8c`). It
  re-pins the attribution document, which changed in `8fb0b93` after the R11
  composite was written. It also adds a lineage line recording the interruption.
  The new runner folder was needed because the generator writes into its own
  folder and refuses to overwrite.
- **Inputs:** OpenCL.dll `8552b3a1`, fixture `d519743e`, census r6, controls r8,
  same VM identity. `test_r11.py` and the executor self-test pass from the R12 copy.
- **Run folder:** `build/qualification_feature_20261021/opencl_reload_final_r12/`
  with plan sha256 prefix `8156317fdd36d8b5`, `root_plan_review.json` and
  `root_exact_invocation.json`.
- **Launch:** 2026-10-22T23:29:45Z with `setsid nohup`, in its own process session,
  so an agent exit cannot kill it. When the executor ends, the wrapper writes
  `executor_exit.txt` (`exit=<code> utc=<time>`). Expect it near 2026-10-23T03:30Z.
- **Pass criteria:** `executor_exit.txt` says `exit=0`; `native_stdout.raw` holds
  1,100 `"event":"cycle"` records and one `"event":"complete"`;
  `native_result.json` passes; the lease returns to `idle; `.
- **If it fails:** do a read-only review first, as for R11. Never kill the guest
  fixture. Attribute the failure in a new attribution section before any rerun.

RESULT_R12_PLACEHOLDER

### 5.2 Final independent review of PR #6

An independent read-only reviewer covered commits `952583e` through `22e86e3`: the
teardown shim, the link-map audit gate, the probe stand-in, tests, tracker
arithmetic and naming. It ran `clvk_runtime_audit_test.py` (21 OK) and
`opencl_reload_output_test.py` (13 OK). It found **no confirmed defects**.

**Conditional finding, decide before closing #5.** The teardown callback in
`src/vgpu/clvk_runtime_eh_pool.c` (around line 69) calls `llvm::llvm_shutdown()`
after all C++ static destructors have run. That includes LLVM's function-local
statics: the `errs()`/`dbgs()` streams and the `std::recursive_mutex` inside
`getManagedStaticMutex()`. Under GCC 14's win32 thread model that mutex is a
CRITICAL_SECTION that is already deleted by then. LLVM itself assumes the opposite
order (see the comment at `DebugCounter.cpp:92`).

- **Default options are safe.** The reviewer traced every ManagedStatic deleter that
  OpenCL constructs: TimerGlobals, CommandLineParser, SubCommand and cl::opt,
  StatLock and DebugCounter. None touches a dead static or re-creates a
  ManagedStatic. StatisticInfo is never built in a release build. The Parallel.cpp
  executor is linked but unreachable from OpenCL.
- **Failure case.** clspv's `ParseOptions` (`Compiler.cpp:917-918`) forwards the
  application's `clBuildProgram` options to `cl::ParseCommandLineOptions`. An
  application that passes `-print-debug-counter` would make `~DebugCounterOwner`
  print to the destroyed `dbgs()` stream during unload. That is undefined
  behaviour, likely a pure-virtual abort. This path did not exist before `23e9275`,
  because the ManagedStatic was never destroyed.
- **Recommended handling.** At minimum, document the ordering assumption in
  `IMPL_DESC.md` and the shim's Doxygen, including the debug-counter case. The
  stronger fix is to strip debug-counter and statistics options before they reach
  clspv. Doing that inside clspv or clvk is an upstream private-copy change and
  needs user approval. Any change to OpenCL.dll requires a rebuild and a full
  requalification: census, controls, a 1,100-cycle reload and a heap run.

**Nits, each a small atomic commit:**

1. The shim comment near line 36, the IMPL_DESC admission section and attribution
   section 17 say `llvm_shutdown` must come from
   `libLLVMSupport.a(ManagedStatic.cpp.obj)`. `_source_matches` in
   `scripts/clvk_runtime_audit.py` (around line 756) only checks the member name, in
   any archive. The existing `eh_alloc.o` gate is equally loose. Either bind the
   archive name in the auditor or reword the three documents.
2. `tests/vgpu/clvk_build.py` (around line 568) captures the probe's keyword
   arguments but never asserts `llvm_runtime is False`. No test asserts that the
   production and cached call sites in `scripts/build_clvk_windows.py` (lines 527
   and 888) keep it True. Add both assertions.
3. `tests/vgpu/clvk_runtime_audit_test.py` lacks a case for an archive-only spoof,
   that is, `ManagedStatic.cpp.obj` inside a foreign `.a`. Add it with nit 1.
4. TRACKER row #5 still says the fix "needs user-approved private-copy changes, a
   rebuild…". The fix was approved and implemented. Update the note.
5. The stand-in in `tests/vgpu/clvk_runtime_windows.cpp` (around line 140) is not
   `noexcept`, unlike its neighbours. Mangling is unaffected; add it for
   consistency.

The reviewer also confirmed: the policy gate cannot drop or add the required
symbol; the cache key binds the shim sources and the auditor; the real map has
exactly one `llvm::llvm_shutdown()` definition owned by `ManagedStatic.cpp.obj`;
TRACKER arithmetic gives 93.0%; no camelCase was introduced; README.md is
untouched. Commit `22e86e3` is not yet in the TRACKER commit log because it is the
commit that wrote the log. Add its row in the next tracker commit.

Nits 1 to 3 change only the auditor and tests, not OpenCL.dll. Check whether the
build cache key includes the auditor. If it does, a rebuild may be triggered;
confirm the rebuilt DLL hash still equals `8552b3a1` before reusing R12 and heap
evidence.

## 6. Remaining steps to 100%, in order

Tracker weights: #1 15%, #2 20%, #3 25%, #4 15%, #5 20%, #6 5%; other rows weigh
0%. At handoff #3 is 96% and #5 is 70%, total 93.0%. Raise a row only with
passing evidence, and attribute every change to its commit.

### Step 1. OpenCL heap run on the rebuilt DLL

Prerequisites: lease idle and held by Codex (section 3); R12 passed (section 5.1a).
Run from the repository root, through the r5 runner that pins wrapper r2:

```
python3 -B build/qualification_feature_20261021/opencl_heap_runner_r5/run_heap.py --expected-bindings-sha256 ce9daeaa5457db25ed5d3a83842d6919446df5f15669f81504adb00d13938fc9 root-run
```

The r4 runner is `opencl_heap_runner_r4/run_heap.py` (sha256 `0cb0e445…3fbb`), its
`runner.py` sha256 `d276f589…bc22662`, guard R22
`cdb_guard_r22/guarded_capture.py` sha256 `525fc268…2bd75`. Its preflight and
`test_heap_r4.py` pass. Expect roughly an hour. A run that ends with "source-frame
attribution is incomplete" is the expected hand-off to the offline review, not a
failure.

Then review offline, from `build/qualification_feature_20261021`:

```
python3 -B heap_residual_review.py <run_folder> opencl ntdll_c93f4614_publics.json kernelbase_pinned_publics.json
```

Pass condition: verdict PASS, zero OWNED blocks at every unloaded stage. Expected
residue that is not owned:

- One 48-byte critical-section debug record per load from the C++ runtime's guard
  mutex, allocated under the verifier only. It classifies as OS.
- Freed debug-info records held in ntdll's reuse cache of fewer than 10 entries.

If the IFEO key is not restored at the end, first run `heap_run_liveness.py <folder>
<record.json>` (read only). Only when it shows no target, CDB, batch or guard worker
alive, run `heap_run_retire.py <folder> <record.json>`. Never kill a process.

If OWNED blocks remain: attribute them by stack in a new attribution section, fix
Waddle code with regression tests, and ask the user before any upstream private-copy
change. Then rebuild, rerun census, controls, a fresh 1,100-cycle reload and a heap
run, exactly as for `8552b3a1`.

### Step 2. Address the final review findings

Section 5 lists them. Fix each with an atomic commit and tests, or record why it
needs no change.

### Step 3. Documentation and tracker

1. Add attribution section 18 to `OPENCL_RETENTION_ATTRIBUTION_2026_10_22.md`:
   the R11 interruption, R12 and the heap run with their folders, hashes and verdicts.
2. Update `IMPL_DESC.md` OpenCL qualification text to match.
3. TRACKER: #5 to 100% and #11 to 100% when the heap run passes. #3 to 100% when
   exact-head CI and the final review pass. Set the notes, the total, and a commit
   log row for each commit.
4. Commit atomically and push to PR #6.

### Step 4. Exact-head CI

After the last content commit, check `gh run list --branch feature/software-vgpu-slicing`
and `gh pr checks 6` for the exact head SHA. All jobs listed in section 2 must pass.
Fix failures with new commits; never re-run a failure into a pass without a cause.

### Step 5. Close out

1. TRACKER header: status `Completed`, `Time Ended` set to a real ISO 8601 UTC
   timestamp, total `100.0%`. Commit and push. Confirm CI is green on that head.
2. Mark PR #6 ready for review with `gh pr ready 6`. Do not merge.
3. Report to the user: "Ready to move onto other areas of the project."

## 7. Decisions still owed by the user

- Ratify reload contract amendment 4: loader patch 0002 and OS pre-initialization
  steps 8 to 10.
- Confirm that DXVK residue from DXVK's own statically linked runtime counts as
  CLIENT, not OWNED.
- Decide what to do with suspended guest fixture PID 7984.
- Approve lease-owner wrapper r2 (section 3).

Ask for these early. Do not mark the feature 100% while amendment 4 is unratified
unless the user says otherwise.
