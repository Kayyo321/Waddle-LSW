# User-requested stopping point, 2026-10-21 (Claude Code implementation owner)

> **Update, later the same day (work resumed at the user's request):** the **M2** gaps listed below (no instrumented native run, receipts bound to the old fixture hashes, `wsi_lifetime` path unexecuted) are closed;
> M2 is handed over as READY FOR INDEPENDENT CODEX REVIEW in [../wsi-window-lifetime/M2_REVIEW_HANDOFF_2026_10_21.md](../wsi-window-lifetime/M2_REVIEW_HANDOFF_2026_10_21.md) (that document also holds the current VM/process state; the
> "Live state" section below describes the moment of the stop only). The **OpenCL runtime-unload** work list below is unchanged and remains NOT READY. No percentage, Time Ended, PR or merge state changed.

**Status at the stop: NOT READY for independent Codex qualification.** Work was stopped at the user's request ("find a stopping point now").
This is a recoverable checkpoint, not closure. Nothing here marks M2 or the feature complete, records a final Time Ended, marks the PR
ready or merges. README.md is untouched.

## Numbers (unchanged)

| Item | Value | Why unchanged |
|:--|:--|:--|
| M2 (`impl/wsi-window-lifetime`) | 94% / Review, Time Ended TBD | The native Windows gate (one of five equal #4 gates) is still unpassed; the audit found no Waddle-owned defect, so the remaining step is an acceptance-owner decision, not code |
| Software-vGPU feature | 85.5% / In Progress | The OpenCL runtime-unload defect is root-caused and has an approved correction path, but no correction is integrated or verified; DXVK heap, cancellation and Linux-GPU/OpenCL overlap gates are untouched |

## Commits made this session (all on `feature/software-vgpu-slicing`; base HEAD `52a1a2970c2a182c155c718432a9dee520b0ca72`)

| Commit | Change | Task impact |
|:--|:--|:--|
| `7b6c9d5` | fixture: one `ReleaseDC` attempt per `GetDC` on failure | M2 #4 +0% |
| `6faf4ed` | fixture: clear destroyed-HWND owner before post-destroy checks | M2 #4 +0% |
| `faf9d19` | failure-only handle attribution diagnostics in the native fixture + host test + acceptance-rule source pin | M2 #4 +0% |
| `44d69c2` | native Windows test of the diagnostics (cross-compile rule) | M2 #4 +0% |
| `56b89a9` | independent-review fixes, CI step for the host tests | M2 #4 +0% |
| docs commit(s) after this checkpoint file's parent | audit, root-cause, reviews, proposal evidence, receipts, tracker/IMPL_DESC sync (this file included) | +0% to both trackers |

## M2 findings (details: `impl/wsi-window-lifetime/M2_HANDLE_GATE_AUDIT_2026_10_21.md`)

- No Waddle-owned path explains `tcp_gpu_7adb6000` (199->200). The same fixture sources and the same five guest binaries passed elsewhere; the ICD/bootstrap create no
  threads/events/timers/files after the baseline; the rare retain/poison paths provably did not run. The process-global count is sensitive to OS-owned RPC idle-GC churn
  (system-only control: -3 ALPC, +4 Thread/Event, net +1 at 59.8 s) and could hide an offsetting Waddle leak. No production fix was made or justified. None of the forbidden remedies was used.
- Native `StretchDIBits` right-to-left probe (interactive and WinRM): positive scanline counts everywhere, sink succeeds -> the audit's inferred defect is not reproduced.
- Diagnostics are implemented and natively proven (7/7 in the guest). **They had not yet run inside a full 48-frame workload at the stop (superseded: 30 native runs later the same day).**
- **Decision needed from the acceptance owner** (agents cannot redefine acceptance): A keep as written (M2 blocked on this image), B a recorded criteria amendment to an attribution-aware strict oracle, C change the platform. See the audit note.

## OpenCL runtime-unload findings (details: `CLVK_RUNTIME_ROOT_CAUSE_2026_10_21.md`, `CLVK_RUNTIME_PROPOSAL_REVIEWS_2026_10_21.md`, `RUNTIME_CANDIDATE_RECEIPT_2026_10_21.json`)

- Root cause (static attribution, natively consistent): POSIX/static winpthreads loses 6 handles + 1 TLS slot per unload (4 semaphores from winpthreads condition variables
  inside never-destroyed `PTHREAD_RWLOCK_INITIALIZER` locks, 1 event + 1 thread handle of the unloading thread, 1 winpthreads TlsAlloc); the Win32 model loses 2 libgcc TLS indices per unload (+ a thread handle per foreign thread using `std::this_thread::get_id`)
  and stalls after 543 reloads. `llvm_shutdown()` is not a fix. **New (review finding, not yet natively measured):** libstdc++'s emergency-exception pool mallocs 0x16000 bytes per load and is never freed in either model.
- **Human approval given in this session:** the shim approach (Win32 model + Waddle C runtime shim replacing two libgcc_eh members + release of the emergency pool + hardened fail-closed gates), integrated as reviewed atomic commits, then real-DLL rebuild and native qualification. Not approved: clvk/LLVM/clspv/submodule/`patches/clvk` changes, host package installs.
- Isolated native proof of the mechanism (v1 shim, unchanged oracle, interactive guest): 8 and 2048 reloads, handles exact, free TLS 1086 every cycle, natural exit 0; the POSIX and Win32 controls fail exactly as before; the extended probe shows the Win32 control's +1 handle.
  Candidate real `OpenCL.dll` (Win32 + v1 shim) **built** (31 min, 121 exports, no winpthreads, one paired TlsAlloc, 608-byte TLS template vs 8) and **never executed**.
- Two independent reviews: mechanism sound; v1 **not zero-leak** and its gates fail open. v1 is kept as evidence in `impl/software-vgpu-slicing/clvk_runtime_proposal/` (not applied, not wired into the build).

## Remaining work, in order

1. **Strengthen the isolated regression (test-only, authorized):** pin `msvcrt.dll`; HeapWalk busy-bytes/entries oracle (cycle 1 warm-up, reference after cycle 2, exact from cycle 3, 100 x 10 ms); FLS free-slot oracle; 4 pre-existing idle threads; foreign `CreateThread` exercise via a second export; probe checks for initialized `thread_local`, caught exception, `std::thread::id`, detached thread. Predict and confirm that the approved POSIX build shows >= 0x16000 bytes per cycle. The worker for this item was stopped before it edited any tracked file; nothing partial exists, and the list above is the specification.
2. **v2 shim** (`src/vgpu/clvk_runtime_tls.c` + new `src/vgpu/clvk_runtime_eh_pool.c`): emergency-pool release (design from the stopped analyst: a C initializer in `.CRT$XC*` that calls `atexit`, so it is table entry 0 and runs after every static destructor while the heap is valid; `.dtors`, `constructor(101)` and `init_priority(101)` are too early; fail-closed link-map checks that `tls_atexit.o`/`__cxa_atexit`/`__cxa_thread_atexit` are not linked and `atexit` resolves to `dllcrt2.o`), accessor fail-fast (NULL vector/block, 16-byte alignment), worst-case arena bound, configurable capacities, documentation per AGENTS.md, mutation-resistant tests (>= 90% line/branch), then native kill of the TEB-offset and `.tls$WDL` mutants.
3. **Hardened driver** (`scripts/build_clvk_windows.py`, `scripts/vgpu_dxvk.mk`, `tests/vgpu/clvk_build.py`, CI): the review's defect list (TLS-directory parse, map bound to the DLL, FlsAlloc/GetProcAddress/thunk-call detection, compiler-version assertion and libgcc/libstdc++ hashes in the cache key, pre-flight on the probe before the long build, idempotent fixture gate, tests for `main()`, native-tool key independent of the thread model, rollback path), atomic commits with IMPL_DESC/TRACKER entries. Re-review before native.
4. **Native on the isolated probe** with the strengthened regression (v2 vs controls; loader TLS retention control with idle threads), then **real DLL**: `make vgpu-opencl-build`, full native qualification, UMDH zero growth unloaded2->3, a new per-unload TLS/FLS oracle, 1100+ reload loop. Prior executions of `a8fd9d7a...` do not qualify a rebuilt DLL.
5. **M2:** run the instrumented unchanged 48-frame workload N times with `build/qualification_20261020/m2_runner/run_m2_instrumented.py` (read its README first), then the acceptance-owner decision above. Remaining independent gates (DXVK heap attribution, cancellation, Linux-GPU/OpenCL overlap) are unchanged from `CHECKPOINT_2026_10_18.md`.

## Live state at the stop

- **Git:** branch `feature/software-vgpu-slicing`; working tree clean after the docs commit; no push; Draft PR 6 untouched; submodules untouched (pins unchanged; five nested Looking Glass checkouts remain uninitialized as before).
  `build/qualification_20261020/cand_clone` is a `git clone --shared` of this repository (alternates into `.git/objects`): **do not gc/prune this repository while the clone is needed**.
- **Workers:** all subagents completed or were stopped; the one that held an unfinished edit (isolated-regression coder) had not touched tracked files. One orphaned hung test process of a finished reviewer (`vgpu_clvk_runtime_tls_test` in that reviewer's scratch overlay) was identity-checked and retired. No build, test, publisher or HTTP server of this session is running. The loopback artifact server used for guest transfers is started and stopped per use by `build/qualification_20261020/guest_tools.py` (port 55986).
- **VM/GPU lease:** the existing Windows 11 VM was started by this session from preserved artifacts only (never provisioned or reset) after the host rebooted: QEMU pid in `build/windows11_vm/qemu.pid` (39354), swtpm `build/windows11_vm/tpm.pid` (39348), WinRM `127.0.0.1:55985`, VNC `127.0.0.1:19`. Before boot the disk, firmware vars and TPM state were copied to `build/vm_backup_20261021` (sha256-identical to the originals). The VM is **left running**; the user signed in `waddle_test` at the console (auto-logon is off; the assistant never enters passwords, so a fresh boot needs the user to sign in again before any interactive run). No guest test process is running. Guest test folders `C:\waddle_*_20261021` remain. No GPU job is running; the lease is relinquished.
- **Scratch:** `/tmp` scratch is RAM-backed and volatile; key reports, diffs and tools were copied to `build/qualification_20261020/scratch_preserved/` and `build/qualification_20261020/clvk_candidate/`. Guest helper: `build/qualification_20261020/guest_tools.py`.
- **Native results in this session** run on the interactive desktop (Win+R via QMP key events, no credentials typed) unless marked WinRM; WinRM sessions are non-interactive and cannot run window/GetPixel fixtures.
- **Not verified / not done:** any instrumented 48-frame native run; any real-DLL execution; heap (HeapWalk/UMDH) evidence for the candidate; loader TLS retention of the 608-byte template; the eh-pool leak natively; CI behaviour on GCC 13 (CI installs Ubuntu's win32 toolchain, which is not the reviewed 14.2).

## Evidence index

`M2_DIAGNOSTICS_RECEIPT_2026_10_21.json`, `RUNTIME_CANDIDATE_RECEIPT_2026_10_21.json`, `impl/wsi-window-lifetime/M2_HANDLE_GATE_AUDIT_2026_10_21.md`, `CLVK_RUNTIME_ROOT_CAUSE_2026_10_21.md`,
`CLVK_RUNTIME_PROPOSAL_REVIEWS_2026_10_21.md`, `clvk_runtime_proposal/`, and under `build/qualification_20261020/`: `baseline/` (portable gates on unchanged HEAD), `vm/`, `m2_native/`, `clvk_candidate/`, `cand_logs/`, `cand_clone/`, `m2_runner/`, `m2_fixture/`, `scratch_preserved/`, `vm_lease.json`.
