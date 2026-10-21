# Implementation-to-verification handoff — 2026-10-17

From: Claude Code (implementation and implementation review owner).
To: Codex (owner of ALL test writing, verification, qualification and feature closure).
Nothing in this file is a verification result. Where a statement is a source
reading or a cross-compilation, it says so.

## 1. Repository state

- Repository: `/home/dev/Waddle-LSW`; branch `feature/software-vgpu-slicing`; target `origin`.
- Start of this work: local branch was 57 commits behind `origin/feature/software-vgpu-slicing`
  (`de518b4`) with zero local-only commits; fast-forwarded with `git merge --ff-only`.
  No reset, rebase, force or history rewrite. Nothing was pushed. No PR was opened
  (the feature is not complete; see section 9).
- Production HEAD before this handoff file: `3ec4908`. The commit that adds this file is
  the exact HEAD; read it with `git rev-parse HEAD`. Working tree is clean except ignored
  `build/` artifacts. `README.md` is untouched.
- `src/`, `include/`, `tests/`, workflows and `GNUmakefile` are byte-identical to
  `de518b4` (verified with `git diff --stat de518b4..HEAD`). **No existing production source
  or test changed**, so no prior source-bound receipt is invalidated by source edits (see 8).

## 2. Milestone 2 — authoritative definition and my resolution

The records do not contain an explicit "milestone 2". The repository's own wording is
`impl/HARDWARE_TEST_HANDOFF.md` ("the two new milestones") and
`impl/CLOUD_PUBLICATION_CHECKPOINT.md` ("Both implementation milestones"): milestone 1 =
ordinary-window desktop input (`impl/desktop-input`, `impl/input-lease-recovery`);
milestone 2 = guest WSI resize/loss/retirement lifetime (`impl/wsi-window-lifetime`).
I resolved milestone 2 as the WSI lifetime milestone, not TODO #2. Its production code was
already complete at `5be8152` (published as `51ee17b`); its tracker is 94% with only
verification left (Task #4: full LeakSanitizer gate, previously blocked by the cloud
executor's ptrace restriction). **That blocker does not exist on this machine:**
`/proc/sys/kernel/yama/ptrace_scope` is 0 and a trivial `-fsanitize=address,leak` leak
was reported by LSan here. The WSI scope boundary stays: unobserved same-HWND reuse and
direct guest-swapchain-to-Wayland presentation are explicitly out of scope there.
If you read "milestone 2" differently, the OpenCL work below is the only new production
scope and is independent of that choice.

## 3. What I implemented (all unverified at runtime)

| Commit | Content |
|:--|:--|
| `96505a9` | `IMPL_DESC.md` section 5: OpenCL remoting contract and policy resolution |
| `f6d0708` `79dc7f9` `6dbf933` `36e077c` | `chore(deps)` submodules: clvk (+5 nested gitlinks), llvm_project, clspv_spirv_headers, clspv_spirv_tools |
| `93bf9d7` | `scripts/verify_clvk.sh`, tracked `scripts/clvk/OpenCL.def` export list |
| `1886207` | `scripts/build_clvk_windows.py` + `make vgpu-opencl-pin|vgpu-opencl-build` (patch-free MinGW cross-build) |
| `42c7ce2` | `IMPL_DESC.md` 5.2/5.5 synchronised with the built driver |
| `3ec4908` | `TRACKER.md`: #5 = 40% (implemented, unverified), new #12 = 100% (pins), overall 85.5% |

### 3.1 OpenCL (TODO #5) decisions

- "rusticocl" is historical and does not authorise Rust; Mesa Rusticl is excluded (needs rustc).
  The roadmap's clvk/clspv proposal is now adopted by this branch as unmodified pinned
  third-party C++ submodules (same posture as DXVK). I judged this not to be a language-policy
  exception because AGENTS.md restricts Waddle-authored C++ and permits third-party
  submodules; **if you or the user disagree, that is the one decision to escalate.**
  All Waddle-authored code is Python build glue; no C/Zig links into `OpenCL.dll`.
- Route: app → `OpenCL.dll` (clvk + in-process clspv) → `vulkan-1.dll` (pinned loader) → existing
  guest ICD → existing Venus transport → host. **No wire, ring, host-receiver or ICD change.**
- Static audit (source reading only; see TRACKER "2026-10-17" for details): clvk initialises
  on core Vulkan 1.3 with zero device extensions; legacy per-feature structs it chains are
  skipped by the ICD and read as zero, so clvk disables fp16/int8, 8/16-bit storage and buffer
  device address. Honest but narrower OpenCL capability. Optional unimplemented enhancement:
  project those aliased structs from the Vulkan 1.1/1.2 feature state the ICD already holds
  (needs allow-list entries in `venus_features_native.zig`/`venus_device_native.zig` and tests).
- Build facts: driver completed here; receipt `build/clvk_client.json`; DLL
  `build/clvk_client/clvk_e924a5bb0388_*/out/OpenCL.dll`, SHA-256
  `96e5b8dfb739adc0e7d372b3f33f29585b4e96bbabfc23793299ec52f66bde38`, 187,246,283 bytes
  unstripped (about 105 MB stripped — matters for any WinRM/base64 transfer), 121 exports,
  imports only `vulkan-1.dll`, ADVAPI32, KERNEL32, msvcrt, ntdll, ole32, SHELL32.
  Build time with 6 jobs (worker-reported): about 16 minutes for the native tools and 15 minutes for the Windows pass.

## 4. Remaining production gaps / unresolved decisions

1. **OpenCL has never executed.** Expect first-run defects. Likely places: loader discovery of the
   ICD from `OpenCL.dll` (use the same loader/ICD manifest/bootstrap route as the DXVK fixture),
   clvk device init against the real negotiated ICD, clspv-generated SPIR-V acceptance by the
   ICD/host (push constants, specialization-constant work-group sizes, descriptor limits), and
   teardown order (clvk may delay Vulkan teardown to static destruction; the host receiver
   requires natural retirement). Any required upstream change needs a tracked patch under
   `patches/clvk/` applied to the private copy and **explicit human approval first**.
2. **`scripts/clvk/OpenCL.def` must track clvk `src/api.cpp`.** The driver fails closed on drift.
   A first hand-derived list missed `clBuildProgram`, `clEnqueueCopyImage`,
   `clSetProgramSpecializationConstant`; fixed.
3. **CI cost I introduced:** `scripts/ci_submodules.sh` (used by `cli_linux.yml`) ends with
   `git submodule update --init --recursive --depth 1`, so it will now also fetch llvm-project
   (shallow) and the clvk nested checkouts. Not changed; decide after observing a CI run
   (options: `update = none` for the OpenCL submodules, or explicit exclusions). No workflow
   builds OpenCL.
4. **DXVK native-heap gate (#3/#11) remains open** and its code is the test fixture
   (`tests/vgpu/dxvk_integration.c`), so it is yours. My analysis of the existing evidence
   is in section 7. No production-side defect was found: the guest ICD creates no threads
   (`grep` over `src/vgpu`, `src/guest`: only test code and the unrelated CLI session).
5. Optional ICD enhancement above (aliased feature structs) — not required to run a kernel.
6. Not in scope and untouched: D3D12, Photoshop/Premiere, installed WDDM kernel driver,
   integrated Wayland swapchain presentation, unobserved HWND reuse.

## 5. Changed interfaces, formats, ownership, dependencies

- Wire formats: **none changed.** ICD exports: none changed. Public headers: none changed.
- New dependencies (all public HTTPS, immutable SHAs, shallow):
  `submodules/clvk` `8bd3672174764dce25ba220773e10baa752f0373`;
  its nested `external/{clspv 7ee3a40e…, OpenCL-Headers 386ca390…, SPIRV-Headers 2b7475f6…,
  SPIRV-Tools ba9a9e32…, SPIRV-LLVM-Translator c4a6b423…}`;
  `submodules/llvm_project` `71a85ef89fc1997205104b72447e0dc2c4ad83da`;
  `submodules/clspv_spirv_headers` `b824a462d4256d720bebb40e78b9eb8f78bbb305`;
  `submodules/clspv_spirv_tools` `fb7471844504abb16b732dc4fc0837119a32ec24`.
  Licences: clvk/clspv/SPIRV-Tools Apache-2.0; LLVM Apache-2.0 with LLVM exceptions;
  SPIRV-LLVM-Translator NCSA. Nested clvk submodules need
  `git -C submodules/clvk submodule update --init --depth 1 external/<name>`
  (or `git submodule update --init --recursive`).
- New build outputs (ignored): `build/clvk_client.json`, `build/clvk_client/…`.
- Ownership: clvk owns its VkInstance/VkDevice and all OpenCL objects; the app releases
  OpenCL handles in order; process unload order should be OpenCL.dll before vulkan-1.dll and the
  bootstrap DLL. The third-party DLL is not covered by sanitizers.

## 6. Tests to write and existing commands to run

New tests (none written by me):
1. Windows guest OpenCL fixture (C, next to `tests/vgpu/dxvk_integration.c` pattern): platform/device
   discovery honesty (must report the shared RTX 5080 via the ICD, not a CPU fallback), then real kernels
   with exact CPU-reference comparison: integer vector add; `__local` reduction with `barrier`;
   atomics; largest supported work-group; a deliberate compile error returning
   `CL_BUILD_PROGRAM_FAILURE` with a log; buffer map/read/write/copy; event ordering; repeated
   create/execute/release lifetimes; natural whole-graph retirement; native handle/heap baseline
   across lifetimes; concurrent Linux GPU workload. `clinfo`-style discovery alone is not acceptance.
2. Unit/pure test for `scripts/build_clvk_windows.py` export-list consistency and
   `scripts/verify_clvk.sh` failure paths (pin mismatch, dirty tree).
3. Whatever coverage the optional ICD enhancement needs, if you choose to implement it.

Existing gates to run (they apply to the unchanged sources; none were run by me): see
`impl/HARDWARE_TEST_HANDOFF.md` section 4 and `impl/REVIEW_CHECKLIST.md`. Key targets:
`make vgpu-icd-test vgpu-icd-sanitizers vgpu-windows`, `make vgpu-wsi-lifecycle-test
vgpu-wsi-lifecycle-sanitizers` (LSan now usable locally), `python3 tests/av/coverage.py venus_wsi`,
`python3 tests/vgpu/wsi_lifecycle_coverage.py`, whole-ICD coverage (≥90% branch required;
last recorded 90.32%/98.53%), `make vgpu-dxvk-client-build`, the TCP/DXVK/WSI supervisors under
`build/windows11_vm/`, and `make vgpu-opencl-pin vgpu-opencl-build`.

## 7. DXVK native-heap evidence analysis (for the fixture owner)

Latest complete three-lifetime audit with the joined compiler is
`build/windows11_vm/dxvk_heap_3ea6429b112fad64` (source before `e8961c5`; the current fixture
that holds the compiler module through device-thread retirement has NOT been audited). Steady-state
growth between lifetime 2 and 3 (`diff_unloaded_2_unloaded_3.txt`, net about +6 KB per lifetime):

- 14,560 B (5 allocations per lifetime): `ntdll!LdrpHandleTlsData` ← `LdrpLoadDll` ← `LoadLibraryExW`
  from the fixture's `d3dcompiler_47.dll` load. Per-thread static-TLS blocks for a TLS-bearing DLL
  loaded while other threads exist; consistent with module/thread teardown ordering (a thread that
  outlives `FreeLibrary` never runs the DLL's detach cleanup).
- 2,048 B: `ucrtbase!malloc` inside a TLS callback during `LdrpInitializeThread` (a DLL TLS callback
  allocating on thread creation; freed only if that thread exits while the DLL is still loaded).
- 344 B: `ucrtbase!setlocale` from the compiler worker thread; 304 B `bcryptprimitives` PRNG; 646 B
  loader DLL-path cache (OS caches, to be attributed by exact stack, not excluded).

I wrote a system-only reproduction (no project DLLs) that cycles the same load/compile/free patterns
in-process and counts busy heap bytes: `build/codex_handoff/heap_repro.c` (compiled with
`x86_64-w64-mingw32-gcc`, **never run**). It separates "OS behaviour" from "fixture ordering".
`build/codex_handoff/vm.py` is a credential-reading WinRM helper (prints nothing secret).

## 8. Evidence invalidated by my changes

None by source change (no `src/`, `include/`, `tests/`, workflow or Makefile edit). Receipts that bind a
**whole-tree or submodule list hash** (for example any `SOURCE_SHA256SUMS` covering `.gitmodules`) will
differ; four gitlinks and `.gitmodules` entries were added. The documentation tracker totals changed
(85.5%). `CODEX_HANDOFF.md` aside, the older HANDOFF/TRACKER narrative is unchanged and historical.

## 9. VM, host and artifacts (no secrets)

- I **restarted the existing Windows 11 VM** (it was not running) from its preserved artifacts only —
  never `provision.py`. Backup copies first: `build/vm_backup_20261017/{windows.qcow2,vars.fd,tpm/}`.
  Preserved originals: `build/windows11_vm/{windows.qcow2,vars.fd,tpm/}`. VM is **running now**
  (`build/windows11_vm/qemu.pid`, swtpm `tpm.pid`). Release: I hold no lease; you own VM/GPU.
- Launch equivalent used (no install media): swtpm socket on `tpm/`, then
  `qemu-system-x86_64 -name waddle_windows11_test -machine q35,accel=kvm -cpu host -smp 4 -m 8192`
  with OVMF_CODE_4M.ms.fd (readonly) + `vars.fd` pflash, `windows.qcow2` as `if=ide`, tpm-tis, e1000 user
  net with `hostfwd=tcp:127.0.0.1:55985-:5985`, `-display none -vnc 127.0.0.1:19`,
  `-qmp unix:build/windows11_vm/qmp.sock,server=on,wait=off`, `-pidfile`, `-daemonize`.
- WinRM `127.0.0.1:55985`; credentials JSON `build/windows11_vm/credentials.json` (mode 600, not read into logs).
- Prepared immutable guest snapshots and all prior evidence remain under `build/windows11_vm/` and
  `build/wddm_dxvk_checkpoint/`. The VM had been stopped before I started it, so guest processes from the earlier boot (for
  example the preserved native PIDs 4296/1404 named in HANDOFF.md) are presumably gone; this is inferred, not checked.
- Host: Linux 7.1, RTX 5080 (about 1 GB used at idle), GNU/MinGW-w64 14, Zig 0.13.0, CMake, Ninja.
  The GPU was never touched by me.

## 10. Compilation performed vs. not performed

Performed (compilation/build only):
- Scratch (outside the repo): shallow clones of clvk/clspv/LLVM; a native LLVM+libclc build and a
  MinGW cross-build by a Haiku worker; `heap_repro.c` compiled once and not executed; a three-line C
  LeakSanitizer smoke program compiled and run once to learn whether ptrace-restricted LSan applies here.
- In the repo: `make vgpu-opencl-build` completed (native tools + MinGW pass; `scripts/verify_clvk.sh`
  ran as part of it) and `objdump` inspection of the produced DLL. The driver's correctness is established only by that one run. A one-shot WinRM read of the guest OS version confirmed the VM
  answers.

Not performed: every unit, integration, stress, sanitizer, coverage, benchmark, Windows-native and
GPU acceptance run; no existing test was modified or run; no CI run; no push.

## 11. Workers

Three Haiku-labelled workers (the harness reports the model as `claude-haiku-5-5` from their own
context, which I cannot independently verify; the Agent tool only accepts the alias `haiku` and gives
no model-ID control): an OpenCL repository inventory (low effort), a clvk-versus-ICD Vulkan gap audit
(medium) and the clvk MinGW build spike (high). All have completed and none is running. I verified their
output: the inventory against `.gitmodules`/`git submodule status`; the gap audit's critical claim by reading
`venus_device_native.zig` `copy_chain`/`preflight`; the build result by re-running the build through my own
driver without their patch, which also exposed three missing exports in their list.


Resumed M2 verification: see M2_NATIVE_RECEIPT_2026_10_17.json and the latest
qualification section. Two unchanged native full48-frame repetitions pass;
the earlier199→200 failure remains unexplained and unqualified. Current-source
tracing identifies balanced RPC garbage-collection handle churn in a successful
diagnostic, which cannot retrospectively attribute the failed run. No M2
closure, Time Ended, feature push or PR readiness is authorized by these results.


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
[M2_SYSTEM_HANDLE_RECEIPT_2026_10_17.json](M2_SYSTEM_HANDLE_RECEIPT_2026_10_17.json).

No production fix is justified by these OS-owned callback frames. Closing
borrowed RPC handles, prewarming an elevated baseline, reducing frames or
widening the equality budget would conceal the issue and is not done. M2's
strict native count gate is externally blocked by the current Windows runtime
behavior; M2 remains94%/Review, Time Ended TBD. The original tracked assertion
and all coverage/leak thresholds remain unchanged. Healthy OpenCL native-heap
qualification proceeds independently; it cannot close this M2 gate.


M2 accounting clarification: the WSI tracker retains five equal verification
gates; its Windows gate requires both compilation and the already-required
native/physical acceptance. The prior cross-build-only label omitted that
requirement from the accounting sentence. Four gates pass; native qualification
remains blocked by the strict count failure, so #4 stays80% and M2 stays94%.
No new completion credit, waiver or feature progress. Evidence commit db40b19
records the system-only reproduction; software-vGPU #3/#5/#11 +0%, total85.5%.


### OpenCL three-cycle native heap capture, resumed 2026-10-17

The unchanged approved DLL and native fixture completed three native cycles in
`dxvk_opencl_heap_edf760f119a1747c`: 24 primary lifetimes plus six temporary
foreign contexts, all four exact kernels, malformed inputs, build errors,
joined concurrent callers, explicit shutdown, module absence and loader
environment restoration. Native exit0, exact guest inputs and five signed
Microsoft tools were verified before/after, and the unique originally absent
UST image key was restored. The context/event mismatch oracle runs in two
serial primary lifetimes per cycle; the other six exercise concurrent exact
results and range/pointer/overlap checks. No broader oracle scope is claimed.

UMDH unloaded2→unloaded3 has zero requested/overhead delta. Baseline53 process
handles become142,148,154 after the three unloads. Heap stability does not
prove zero owned leaks or clear the six-handle increase per later cycle.
First-cycle retained stacks include Windows graphics/loader calls reached
through clvk initialization; caller frames alone do not establish allocator
ownership. No LLVM compiler leak or system-cache exemption is inferred.
Native heap and final GPU acceptance remain false pending exact attribution.

Controller and Waddle worker exit0 with no supervisor-directed signals and
empty owned sessions. An adopted vendor renderer child has raw wait status9;
pinned virgl teardown contains its own SIGKILL path, but these receipts do not
identify the signal sender or prove graceful vendor-child exit/LSan coverage.

Two failed private-supervisor attempts are preserved separately:76683d1e59a91796
never launched the native fixture because of a malformed private CMD; its
unique UST key was restored through an independently recorded fresh session.
The corrected staging attempt acb28b83e56ef2e0 reached baseline, then a wrong
Python helper reference aborted supervision; no OpenCL cycle ran, the native
fixture naturally timed out with exit1, and UST was restored. Neither failure
is reused as positive evidence. Ten private supervisor regression tests pass.

The separate CI-only head1e91c4833a285c7978afa0d50eaa4035f983cce7 now passes all
four workflows, including software-vGPU run39085524347 (OpenCL admission,
Linux transport and native Windows transport). This is CI evidence for that
source/configuration, not native heap or current feature-HEAD acceptance.

[OPENCL_HEAP_RECEIPT_2026_10_17.json](OPENCL_HEAP_RECEIPT_2026_10_17.json) binds
raw captures, differences, identities and checksums. #3/#5/#11 +0%; total85.5%.
M2 remains94%/Review, feature In Progress, Time Ended TBD. Real cancellation,
simultaneous Linux-GPU/OpenCL execution and DXVK heap ownership remain open;
no feature push, PR readiness or merge follows from this evidence.


Offline repeat-cycle review accounts for verified per-cycle OpenCL/Vulkan
rebasing. Unloaded1→2 requested bytes decrease0xF78, overhead increases0x8,
net decrease0xF70; unloaded2→3 has zero positive stack groups and zero net
bytes/count. One same-size Windows stack allocation changes address, so the
captures are not byte-identical. These are allocation deltas, not ownership
acceptance. The executed supervisor and ten-test source are archived with
hashes equal to the successful run's original manifest before later private
polling changes. See the OpenCL receipt. Evidence commit80cf470 updated
#3/#5/#11 +0%; this preservation/review also adds0%, total85.5%.


The separate24-primary-lifetime diagnostic ca77f1b6a9915205 completes native0,
all bounded trace queries0, explicit trace-disable0 and UST restoration. It
reproduces six retained OpenCL runtime handles per reload; exact creator
chains and the pending isolated runtime/toolchain comparison are documented
in [CLVK_RUNTIME_LIFETIME_REVIEW.md](CLVK_RUNTIME_LIFETIME_REVIEW.md) and
[OPENCL_HANDLE_RECEIPT_2026_10_17.json](OPENCL_HANDLE_RECEIPT_2026_10_17.json).
This is diagnostic evidence, not heap acceptance. The preceding8ce239cc4db15dd2
attempt failed before any OpenCL cycle because of a private prelaunch token-file
race; native baseline timed out naturally with exit1 and UST was restored.
The race correction retains strict token admission and passes11 private tests.
Evidence commit031c665 preserves the successful prior supervisor source and
repeat-cycle heap review; #3/#5/#11 +0%, total85.5%. No production correction,
threshold change, feature push, Time Ended or PR readiness is claimed.


### Runtime reload regression: both toolchains fail, 2026-10-17

Tracked Windows compatibility tests now cover exact handle/TLS-slot return,
module/export absence, joined threads, once initialization, shared mutex and
stream teardown. Both strict cross-builds pass; native POSIX loses6handles and
1TLS slot on first unload, while candidate Win32 keeps handles balanced but
loses2TLS slots. The Win32 handle-only stress stalls after543 reloads; its failed
collector and exact owned forced retirement are preserved. The candidate build
configuration is withdrawn, not qualified. Original19 admission tests pass.
See [CLVK_RUNTIME_LIFETIME_REVIEW.md](CLVK_RUNTIME_LIFETIME_REVIEW.md) and
[CLVK_RUNTIME_RECEIPT_2026_10_17.json](CLVK_RUNTIME_RECEIPT_2026_10_17.json).
Evidence commit0a2eec0 records the preceding trace isolation. New regression
coverage adds0% to #3/#5/#11; total85.5%. M2 remains94%/Review and Time Ended TBD.
No new upstream SDK patch, feature push, PR readiness or assertion waiver.


## Resumed qualification blocker checkpoint, 2026-10-17 America/New_York

The earlier user-requested stop was superseded by the subsequent request to
continue toward M2. This resumed work is now recorded at concrete qualification
blockers, not feature completion or a new user-requested pause.

- Branch `feature/software-vgpu-slicing`; implementation/evidence head
  `07923b46c9abcfe8d9883bb9aa040236073ee12e`. M2 remains94%/Review;
  software-vGPU85.5%/In Progress; Time Ended TBD.
- M2's unchanged global native handle-count gate fails independently in a
  Windows-only control without Vulkan or Waddle. Two fresh unmodified full48
  frame positives do not waive the retained199→200 failure.
- The approved OpenCL DLL still points to exact hash
  `a8fd9d7a4f979ca4534e26d789025cf9f7c7affd84b46726eeee28f40d83c54d`.
  Three native cycles complete24 primary lifetimes; repeat heap bytes are
  stable but six runtime handles per reload require attribution/closure.
- Exact tracing and a new isolated native regression identify current POSIX
  runtime loss of6handles/1TLS slot per unload. Candidate Win32 balances
  handles but loses2TLS slots and stalls after543 handle-balanced reloads.
  Both final TLS regressions exit1. The original19 admission tests pass;
  cross-compilation/collection are not native acceptance.
- The pending Win32 build switch was preserved as a rejected private patch
  and withdrawn. No new upstream patch or production runtime change is applied.
  SDK lifetime corrections need a separately reviewed private-copy proposal
  and explicit approval under CODEX_HANDOFF.md section4. Do not destroy TLS
  state early, call speculative LLVMshutdown, close borrowed handles, relax
  assertions or reinterpret a short positive as final qualification.
- Owned stalled process5888/creation01dd5ea9f4e6eede was identity-checked and
  forcibly retired after two frozen-progress/increasing-CPU observations.
  Guest exit marker is-1; this is failed stress evidence. All exact owned
  native jobs are now absent. Unique UST keys were restored to original absence.
- Task-only binary/PDB publishers257264/257274 were identity/socket checked
  and stopped with pidfds. Their loopback ports are closed. Restart only the
  documented scripts if more transfers are needed; update PDB server record
  through its script before preparing new immutable audits.
- Original VM10895/start19105 remains running. No host packages were installed
  or upgraded; the optional matching SDK was only extracted in build/. README
  matches the original task-start bytes. Artifacts and unrelated processes remain.
- Separate CI-only head1e91c4833a285c7978afa0d50eaa4035f983cce7 passes all four
  workflows. No feature push, PR readiness update or merge was performed.

Canonical evidence: [runtime review](CLVK_RUNTIME_LIFETIME_REVIEW.md),
[runtime receipt](CLVK_RUNTIME_RECEIPT_2026_10_17.json),
[OpenCL heap receipt](OPENCL_HEAP_RECEIPT_2026_10_17.json),
[OpenCL handle receipt](OPENCL_HANDLE_RECEIPT_2026_10_17.json), and
[M2 system control](M2_SYSTEM_HANDLE_RECEIPT_2026_10_17.json).
The durable runner/README/DLL identity checkpoint is
`build/qualification_20261017/runtime_blocker_checkpoint.json`.
Commit07923b4 adds the regression and synchronized evidence with0% task impact.
No qualification credit or thresholds changed. Remaining actual-DLL heap,
transport-loss cancellation and Linux-GPU/OpenCL overlap acceptance stays open.


Final recursive inventory retains five uninitialized nested Looking Glass
checkouts (`repos/LGMP`, `PureSpice`, `cimgui`, `nanosvg`, `wayland-protocols`).
The initial overbroad assertion that every recursive checkout was initialized
failed and is not acceptance evidence. No checkout/pin was changed to hide it.
Required feature top-level checkouts are initialized at their pins; the
inventory has no advanced/conflicted entries. Raw inventory and corrected
non-acceptance report remain in `build/qualification_20261017/runtime_final_git_state.json`.


## Update 2026-10-21: M2 review handoff

The M2 section above predates the work of 2026-10-21. The current M2 state, evidence and the open acceptance decision are in [../wsi-window-lifetime/M2_REVIEW_HANDOFF_2026_10_21.md](../wsi-window-lifetime/M2_REVIEW_HANDOFF_2026_10_21.md). OpenCL remains as described in [CHECKPOINT_2026_10_21.md](CHECKPOINT_2026_10_21.md).


## Independent M2 review continuation, 2026-10-21

Codex completed the independent review of candidate `8810544`, corrected a diagnostic capacity defect, added and executed a native growth regression, and reran all prescribed portable M2 gates plus both physical GPU workloads. See [the independent review](../wsi-window-lifetime/M2_CODEX_REVIEW_2026_10_21.md) and [receipt](../wsi-window-lifetime/M2_CODEX_RECEIPT_2026_10_21.json) for exact bindings and limits. The historical 199-to-200 failure is still unattributed; M2 remains 94%, feature progress 85.5%, Time Ended TBD. The documented attribution-aware oracle is an unapproved proposal. No PR-readiness change, push or merge occurred. The OpenCL work is not accepted by this M2 review.

### Runtime heap-attribution fixture unit (2026-10-21)

- **Commit containing `test(vgpu): expose bounded runtime heap attribution stages`**: +0% to #5/all tasks; feature **85.5% / In Progress**, Time Ended TBD. Adds optional K={0,4,16} pre-existing-thread variants and exact UMDH baseline/loaded/unloaded/threads-retired stages, with no threshold or timeout relaxation. Three real-source compile groups pass normally and optimized (six positive and three rejected configurations each). See [CLVK_RUNTIME_PROBE_V2.md](CLVK_RUNTIME_PROBE_V2.md).
- The frozen admitted probe's native2048 stress **fails at381**: heap decreases1048bytes/2entries while handles/TLS/FLS balance; natural exit1, no owned survivor. Evidence `build/qualification_feature_20261021/native_probe_cda4fe0f/` remains failed/unattributed. New audited variants and full OpenCL native acceptance remain pending.
