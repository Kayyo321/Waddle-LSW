# Independent reviews of the clvk runtime correction proposal v1, 2026-10-21

Three read-only reports, kept verbatim, plus the lead's disposition. They review
`clvk_runtime_proposal/proposal-v1-runtime-tls-win32.diff` (sha256 da538045da187c526cf3fc60588265230fc6378a9310992a9ecd7369849bf898).
Reviewer statements are claims unless they cite a command and an exit code; the lead's native results are in
[RUNTIME_CANDIDATE_RECEIPT_2026_10_21.json](RUNTIME_CANDIDATE_RECEIPT_2026_10_21.json).

## Lead's disposition

- Mechanism (full replacement of libgcc_eh `emutls.o` and `gthr-win32-thread.o` members by a Waddle C object using the
  module's own implicit TLS) judged sound by the low-level review: linking, ABI parity, concurrency, thread-start parity.
- Not yet a zero-leak fix. Required before shipping (all part of v2, none written yet): release of the libstdc++ emergency
  pool (design below); a heap oracle that sees it (HeapWalk with msvcrt.dll pinned, reference after cycle 2); FLS slot
  oracle; pre-existing idle threads to expose loader TLS retention; a foreign-thread (raw CreateThread) exercise; fail-fast
  checks in the TLS accessor (NULL vector/block, 16-byte alignment); a sound worst-case arena bound; hardened, tested,
  fail-closed driver gates (TLS directory parse, map bound to the DLL, FlsAlloc/GetProcAddress/thunk-call detection,
  compiler-version assertion and libgcc/libstdc++ hashes in the cache key, pre-flight on the probe before the multi-hour
  build, idempotent Makefile gate, tests for main()); no unconditional POSIX removal without an explicit rollback; split the
  1408-line change into atomic commits with IMPL_DESC/TRACKER entries; CI toolchain pinned to the reviewed GCC.
- Native proof still required on the real DLL: exact handles, TLS and FLS per cycle, UMDH zero growth unloaded2->3, HeapWalk,
  and a 1100+ reload loop; the approved DLL a8fd9d7a... is the control.

## 1. Low-level adversarial review (Opus)

### Independent review: runtime TLS shim + Win32 thread model for OpenCL.dll

Reviewer: independent adversarial review (offline, read-only on the repo).
Subject: `scratchpad/clvk_runtime/proposal-runtime-tls-win32.diff`
(sha256 `da538045…f898`; `git apply --check` passes on the worktree; identical to `repo_copy` HEAD diff).
Toolchain examined: Debian GCC 14.2.0-19+27+b1 **win32** (private extraction
`build/qualification_20261017/mingw_win32_private_1402`, not dpkg-installed; only `14-posix`
is installed system-wide), mingw-w64 12.0.0-5 CRT (byte-identical to `/usr/x86_64-w64-mingw32/lib`),
binutils 2.44. Approved DLL `a8fd9d7a…` (`build/clvk_client/clvk_325c776620d5_4l2awxam/out/OpenCL.dll`).
Scratch work: `scratchpad/clvk_review_opus/` (`inv/` scanners, `mut/` mutation runs, `fool/` gate bypasses).
No Windows execution; nothing here is native evidence. I formed these conclusions before reading
the author's report; where I agree with it I say so, where I add or disagree I flag it.

## Verdict summary

| # | Topic | Verdict |
|:-:|:--|:--|
| 1 | Linking strategy (member suppression) | **SOUND** |
| 2 | ABI parity of emutls / thread-exit semantics | **SOUND** (latent `__cxa_thread_atexit` hazard must be gated) |
| 3 | Implicit-TLS mechanics on x64 | **UNPROVEN** (layout/codegen verified; loader retention and alignment need native proof) |
| 4 | Concurrency / memory model / tests | **SOUND** logic; Windows-specific accessor is untested before native runs |
| 5 | gthr create/join/self parity | **SOUND** (one documented identity difference; detach caveat is libgcc parity) |
| 6 | Thread-model side effects / remaining per-load leaks | **UNSOUND** vs. the stated zero-leak requirement (definite 90,112 B heap leak per load, both models) |
| 7 | Failure modes and gates | fail-fast **SOUND**; post-link gates **UNSOUND** (demonstrably fail open) |
| 8 | Bottom line | Right mechanism for TLS indices + handles; **not sufficient / not ready** as a zero-leak correction |

## 1. Linking strategy — SOUND

* Archive index of `14-win32/libgcc_eh.a` (`nm -s`): `emutls.o` defines only `__emutls_get_address`,
  `__emutls_register_common` (+ COMDAT `.refptr._CRT_MT`); `gthr-win32-thread.o` defines only
  `__gthr_win32_create/join/self` (+ COMDAT `.refptr._CRT_MT`, `.refptr.free`). No weak symbols.
  References: emutls.o → abort, calloc, realloc, malloc, free, memcpy, memset, `_CRT_MT`,
  `__mingwthr_key_dtor`, Initialize/Enter/LeaveCriticalSection, TlsAlloc/Get/SetValue, Sleep;
  gthr-win32-thread.o → `_CRT_MT`, `__mingwthr_key_dtor`, CreateThread, DuplicateHandle, ExitThread,
  ResumeThread, TlsAlloc/Get/SetValue, GetThreadId, WaitForSingleObject, GetExitCodeThread, malloc/free.
* Referencers (script `inv/inventory.py`): `__emutls_get_address` ← libstdc++/libsupc++ `eh_globals.o`,
  libstdc++ `mutex.o` (`__once_call/__once_callable` controls); `__gthr_win32_create/join` ← libstdc++
  `thread.o`; `__gthr_win32_self` ← no archive member, only user objects via inline `this_thread::get_id()`
  (`include/c++/bits/std_thread.h:366`, `gthr-default.h:455-458`); `__emutls_register_common` ← nothing.
* The shim (identical bytes to `build/probe/clvk_runtime_tls.o` when I rebuilt it) defines all five
  globals; `probe_win32_tls.map` loads it as `LOAD clvk_runtime_tls.o` and lists neither member;
  symbols resolve at 0x1db2d1920…0x1db2d1c30 in the shim's `.text`.
* The only other extraction triggers are the COMDAT `.refptr.*` index entries; GCC always emits the
  COMDAT definition in the referencing object, so they are never undefined. If either member were
  ever extracted, its strong definitions would collide with the shim → hard link error (no
  `--allow-multiple-definition` anywhere in LLVM/clvk CMake). Mixing is not possible silently.
* Caveat: `verify_runtime_link` is substring-based (member names); it should instead assert the
  *defining input* of each of the five symbols in the map.

## 2. ABI parity — SOUND

* Layout `{word size; word align; union{pointer offset; void*ptr} loc; void *templ}` = 4×8 B, matches
  the shim's `emutls_object_t`. `loc` is private to the module (GCC code only passes `&__emutls_v.x`),
  so the shim's own encoding is safe. `__emutls_register_common` is instruction-for-instruction the
  libgcc rule set (shim.o+0x1c0). `_CRT_MT` = 2 (`libmingw32.a(tlsmcrt.o)` .data), so libgcc always took
  the threaded path; the shim's per-thread-always behaviour is equivalent.
* Init: libgcc copies `templ` or memsets per thread on first access (emutls_alloc); shim copies `templ`
  on first per-thread access and relies on the loader's zeroed template copy otherwise — equivalent
  because slots are never reused within a thread and blocks are always fresh template copies.
* Differences are stricter, not looser: align > 16 or size growth after first use → `__fastfail`
  (libgcc over-allocates / silently overflows).
* Inventory of the approved DLL (`inv/emutls_objs.py`): 11 controls, sizes 4–16, align ≤ 8,
  5 with templates (e.g. `threadIndex`), address-order layout 96 B, worst-case any-order 157 B (≤ 512).
* Thread exit: libgcc frees arrays via `__mingwthr_key_dtor`; the shim relies on the loader freeing the
  per-thread block (after all DLL_THREAD_DETACH callbacks). Better: mingw's runner
  (`tlsthrd.c:105-119`, tlsthrd.o+0x00..0x69) calls each key dtor **without TlsSetValue(NULL)**, so with
  libgcc emutls a later access during DllMain/onexit on the unloading thread reads a freed array
  (author's §1.2 hazard; Wine/ReactOS and mingw's tls_atexit design run TLS callbacks before DllMain on
  detach, so I consider it likely real). The shim removes that hazard.
* `__cxa_thread_atexit`: not linked in the approved DLL (no thread_local destructors today). If one is
  ever added, libstdc++ `atexit_thread.o` is linked: it pins the DLL per registration
  (`GetModuleHandleExW` at `__cxa_thread_atexit`+0x67, `FreeLibrary` in `run`+0x12), uses a TlsAlloc key,
  and `std::atexit(run)` re-reads the stale (never-cleared) key value after mingw's runner already ran
  it → double destructor/delete at detach. Latent; must be gated (fail build if `__cxa_thread_atexit`
  or `__at_thread_exit` is linked).

## 3. Implicit-TLS mechanics — UNPROVEN

Verified offline (own parser `inv/pe_tls.py`, map, disassembly):
* GNU ld PE script (identical `.tls` block in every i386pep.* variant except .xr/.xu):
  `.tls BLOCK(__section_alignment__) { ___tls_start__; .tls$AAA; .tls; .tls$; SORT(.tls$*); .tls$ZZZ }`.
  Map: `_tls_start` (tlssup.o `.tls`, 8 B) at 0x1db3c0000 (page aligned), fill 0x18,
  `.tls$WDL` 0x240 B at +0x20 (input section 2**5), `.tls$ZZZ`/`_tls_end` at +0x260.
* TLS directory of `probe_win32_tls.dll`: Start=0x1db3c0000 (=`_tls_start`), End=+0x260 (608 B raw,
  file-backed in a 0x400 raw section), SizeOfZeroFill=0, Characteristics=0, AddressOfIndex=`_tls_index`
  (.bss, 4 B), callbacks `[__dyn_tls_init, __dyn_tls_dtor]`. Approved DLL: 8 B template, 3 callbacks.
* Codegen (shim.o+0x6a..0x91): `.refptr._tls_index` load, `mov %gs:0x58`, `add (%rax,%rdx,8)`,
  `+(.tls$WDL − _tls_start)` — the same sequence MSVC uses for `__declspec(thread)`; the asm is
  non-volatile but only the per-module block pointer is cached, which is stable for a thread's life.
* Alignment: record offset in the block = 0x20; the loader copies the template to the start of a
  heap block, x64 heap blocks are 16-aligned → record 16-aligned. >16 is not guaranteed
  (Characteristics=0), so `RuntimeTlsMaxAlign=16` is correct. This is reasoning, not a measurement,
  and nothing enforces `offset % 16 == 0` at link or run time.
* Dynamic load of TLS DLLs and blocks for pre-existing threads: supported since Vista (LdrpHandleTlsData
  with kernel-assisted per-thread install); new threads get blocks in LdrpInitializeThread; blocks are
  valid through DLL_*_DETACH callbacks for the calling thread.
* **Unload retention is the open risk.** `CODEX_HANDOFF.md:146-149` shows +5 allocations / 14,560 B per
  lifetime from `ntdll!LdrpHandleTlsData ← LdrpLoadDll` for d3dcompiler_47 — i.e. blocks allocated for
  threads alive at load survived unload and accumulated. The approved DLL already has this exposure
  (8 B template → ~32 B/thread/load incl. header); the proposal makes it 608 B (~640 B/thread/load,
  ~20×). Measure with an OS control (plain mingw C DLL, 8 B vs 608 B template) and K∈{0,4,16} idle
  threads alive across cycles. If real, shrink the record (arena sized from the build inventory,
  e.g. 160 B, initialized flags as a bitmask) — it cannot be eliminated by any mingw DLL.

## 4. Concurrency and tests — SOUND logic

* Publication: slot bytes and location written under an SRWLOCK, location published with
  `__ATOMIC_RELEASE`, fast path `__ATOMIC_ACQUIRE` (x86 plain movs, compiler ordering preserved);
  locked re-check; `initialized[]` is per record; nothing inside the lock can re-enter or take the
  loader lock; fail-fast releases the lock then `int 0x29`.
* Ran the author's tests myself (`mut/run.sh`): plain, ASan+LSan+UBSan, TSan all PASS; coverage script
  100% lines / 100% branches (fake build only).
* Mutation testing (`mut/mutate.py`, 18 mutants): killed by plain tests — slot off-by-one, bytes not
  advanced, wrong offset shift, initialized flag global, no align rounding, wrong align mask,
  record not 16-aligned (Linux-layout dependent), 64 B max-align, no size-regrowth check, handle stored
  after routine, no locked re-check, self-join closes, register_common templ rule, no template copy,
  flag-before-copy, start block not freed, untagged foreign identity.
  Killed only by TSan (20/20 runs): relaxed publication, handle written after ResumeThread.
  Killed only by UBSan: slot-capacity `>=`→`>`. **Survives everything: TEB offset 0x58→0x60** —
  the Windows accessor is not compiled in the double; the coverage gate excludes it.

## 5. gthr parity — SOUND

* Original (`gthr-win32-thread.o`): `malloc(0x18){func,arg,h}`, `CreateThread(…, CREATE_SUSPENDED)`
  (+0x9b/+0xa3), `ResumeThread` result ignored (+0xb5), starter stores the block in its TLS key and
  calls `ExitThread` (+0x0..0x29); key dtor is `free` only (+0x105/+0x114). `_beginthreadex` is not used
  → shim's CreateThread is parity; with msvcrt.dll as a DLL its per-thread data is created lazily and
  freed in msvcrt's DLL_THREAD_DETACH, so no CRT difference.
* join (+0x170..0x1fb): self → return 1 without close; Wait/GetExitCode failure → close + GetLastError;
  exit code zero-extended — shim identical. After join the thread has fully exited, so the borrowed
  handle is never used after close; the shim never passes the borrowed handle to any API.
* self: original duplicates a handle for foreign threads and never closes it (leak, +0x2b0/+0x304);
  shim returns `record|1`, unique among live threads, stable, resource-free. libstdc++ compares
  `thread::id` by value only (`std_thread.h:339`); `__gthr_win32_equal` (GetThreadId-based) is unused by
  headers. LLVM: `llvm::get_threadid`=GetCurrentThreadId (`Threading.inc:66`), `llvm::thread` uses
  `_beginthreadex` (`:31`); `Parallel.cpp:76` compares std ids correctly with the shim.
* Detach: clvk detaches build threads (`clvk/src/program.cpp:1903`); their id becomes a closed handle
  value that the OS may reuse for a later `std::thread` → duplicate live ids possible. Same in libgcc.
* "Never a valid handle value" is slightly overstated (kernel ignores the low 2 tag bits); in practice
  the value (heap address >> 2) is far beyond any handle-table index.

## 6. Thread-model side effects — UNSOUND vs. requirement

* **Definite per-load heap leak, both models (not in the author's report):** libstdc++ `eh_alloc.o`
  static ctor `_GLOBAL__sub_I__ZN9__gnu_cxx9__freeresEv` mallocs the emergency EH arena
  (`mov $0x16000,%ebx; call malloc`, +0x16a/+0x172) and registers `__tcf_0`, which only calls
  `__gthr_win32_mutex_destroy`; only `__gnu_cxx::__freeres()` frees the arena. Approved DLL:
  same code at 0x20c248cba/0x20c248cc2, `__freeres` at 0x20c248b20 has **0** callers
  (`inv/direct_calls.py`). → 90,112 B leaked per load whenever msvcrt.dll stays loaded.
* **Heap oracle confounder:** the harness EXE imports only UCRT (`api-ms-win-crt-*`); the probe DLL
  imports KERNEL32+msvcrt only, so msvcrt.dll is very likely loaded/unloaded with it each cycle. If
  msvcrt's CRT heap is private it is destroyed with it, erasing such leaks. The real-fixture UMDH
  receipt (`OPENCL_HEAP_RECEIPT_2026_10_17.json`: unloaded_1→2 −0xF78 B while handles grow +6/cycle)
  is inconsistent with this deterministic leak, so that heap evidence cannot qualify anything.
* Never-deleted static CS: `guard.o` `init()` calls `__gthr_win32_mutex_init_function(&fake_mutex)`
  (+0x2f) and guard.o has no destroy reference. Debug-info heap / dangling ntdll lock-list entry per
  load on Win10/11 is UNPROVEN (AppVerifier "Locks" + UMDH decide). eh_alloc/locale/shared_ptr mutexes
  are destroyed by static dtors. emutls' CS disappears with the shim.
* New Win32-model hazard: no `__GTHREAD_MUTEX_INIT` (`gthr-default.h:370-371`), so std::mutex,
  recursive_mutex and shared_mutex become dynamically initialised with DeleteCriticalSection dtors.
  Cross-TU static init/destruction order bugs in LLVM/clvk become use-before-init / use-after-delete
  (EnterCriticalSection on a deleted CS can block forever under the loader lock). The full Win32
  OpenCL.dll has never been built or run.
* No persistent registrations from the toolchain: x64 SEH `crtbegin.o` `__gcc_register_frame` only
  atexit-registers a `ret`; onexit table is freed by `_execute_onexit_table`; no RtlAddFunctionTable
  (`.pdata` present). VEH: only `llvm::CrashRecoveryContext::Enable/Disable` (paired). Pre-existing,
  model-independent: `llvm::RegisterHandler` (SetUnhandledExceptionFilter, SetConsoleCtrlHandler,
  LoadLibraryExA dbghelp, InitializeCriticalSection) would dangle after unload if any path reaches it.
* call_once is a Sleep(0) spin (`gthr-win32.o __gthr_win32_once`): permanent spin if the callable
  throws, possible livelock under priority inversion (author noted the first).

## 7. Failure modes and gates

* `__fastfail(FAST_FAIL_FATAL_APP_EXIT)` is inlined as `mov $7,%ecx; int $0x29` (shim.o+0x1ab..+0x1b7):
  no import, valid on Windows 8+ (targets are 10/11), same code UCRT abort uses — appropriate.
* Gate bypasses demonstrated (`fool/`, author's own `audit_runtime_tls` + `verify_runtime_link`):
  all PASS both gates while leaking slots: TlsAlloc via import thunk (`jmp TlsAlloc`), via
  `.refptr.__imp_TlsAlloc`, via GetProcAddress, and FlsAlloc. A **stripped** DLL with 40 TLS objects
  passes the inventory gate (0 objects); the unstripped one fails closed. The arena bound uses address
  order (96 B) instead of the order-independent worst case (157 B). No check of the TLS directory
  (record inside [Start,End), offset%16, AddressOfIndex). Fix: use `-Wl,--cref`/map cross references
  for every input referencing `TlsAlloc`/`__imp_TlsAlloc`/`FlsAlloc`/`GetProcAddress`; require a symbol
  table and that every `__emutls_get_address` call site's control object is inventoried; parse the
  TLS directory; check defining inputs of the five symbols.

## 8. Bottom line

The shim is the minimal mechanism that removes both TLS-index leaks and all winpthreads handles
without loader-lock cleanup; an emutls-only replacement is insufficient (clvk's std::thread hits the gthr
TlsAlloc every load); pinning, private GCC/winpthreads patches and a helper process are heavier or
violate module-absence. But the proposal does not meet "zero owned leaks of memory": the eh arena
leaks 90,112 B/load in both models, loader TLS retention may grow 20×, and the gates can be fooled.

### Defects (severity order) and fixes

1. HIGH — eh_alloc emergency arena leaks 0x16000 B per load (both models). Free it as the last
   teardown action (e.g. Waddle static object with highest init priority whose destructor calls
   `__gnu_cxx::__freeres()`), or obtain human sign-off; prove with the heap oracle below.
2. HIGH — heap oracle blind/confounded (msvcrt.dll lifetime coupled to the DLL; UMDH receipt contradicts
   defect 1). Pin msvcrt.dll (and ADVAPI32/ole32/SHELL32/combase for the real DLL) before baseline; add
   a HeapWalk busy-bytes/blocks oracle over all heaps; calibrate: approved DLL must show +0x16000/cycle.
3. MED-HIGH (unproven) — loader-retained implicit-TLS blocks ×20 (608 B template). OS control DLL +
   K idle threads; shrink the record if retention is real.
4. MED — gates fail open (thunk, refptr, GetProcAddress, FlsAlloc, stripped image, no TLS-dir check,
   non-worst-case bound).
5. MED — Windows accessor untested (mutant survives); add cheap fail-fast checks in
   `runtime_tls_current_record` (vector/block non-NULL, record 16-aligned) and make
   `optional-probe-coverage.diff` mandatory plus: pre-existing CreateThread thread alive across unload,
   raw CreateThread thread after load, alignas(16) thread_local address check, concurrent foreign-id
   uniqueness, exception on each thread, detach.
6. MED — Win32-model static init/destruction order with non-trivial mutex dtors; never built for the
   real DLL.
7. LOW-MED — guard.o CS never deleted (AppVerifier Locks/UMDH).
8. LOW-MED — LLVM signal handlers/dbghelp registration dangling after unload if reached.
9. LOW — latent `__cxa_thread_atexit` pinning/double-run; gate it.
10. LOW — receipt binds driver binaries only (`--version` prints "14-win32"); hash libgcc_eh.a,
    libgcc.a, libstdc++.a, libmingw32.a and assert their member/symbol inventory; CI is GCC 13.
11. LOW — doc/UB nits: "needs 88" (layout 96, worst 157); `extern char _tls_start` vs mingw's
    `char *_tls_start` and cross-object pointer subtraction (use uintptr_t or
    `_tls_used.StartAddressOfRawData`); "owns no lock that outlives" overclaims at DLL level;
    IMPL_DESC/TRACKER entries absent from the diff (AGENTS §3).

### Native proof required before shipping (all on the real image, medium integrity)

1. Probe harness, msvcrt.dll pinned, K=4 idle threads created before baseline and alive throughout:
   2048 cycles natural exit 0; per cycle exact handles, exact free TlsAlloc slots (1086), exact free
   FLS slots, exact HeapWalk busy bytes/blocks after a warm-up cycle, module/export absent.
2. Controls on the same image: POSIX DLL (+6 handles, −1 TLS), unshimmed Win32 DLL (−2 TLS), approved
   DLL heap +0x16000/cycle, OS control DLLs (8 B and 608 B templates) for LdrpHandleTlsData.
3. Semantics (extended probe): template values on pre-existing / std::thread / raw threads,
   `(addr & 15) == 0`, unique concurrent foreign ids, throw/catch on each, threads exiting after unload
   without fault.
4. Application Verifier (Basics: Heaps, Handles, Locks, TLS, Memory) on probe and real DLL: 0 stops
   over ≥3 cycles.
5. Real Win32 OpenCL.dll: build receipt gates; full existing qualification; per-cycle TLS oracle;
   ≥1100 reload cycles natural exit; UMDH (msvcrt pinned) zero positive groups after defect 1 is fixed;
   `SetUnhandledExceptionFilter(NULL)` after unload returns a pointer outside the old image.
6. Fault injection build: forced arena overflow terminates via 0xC0000409/code 7 with no hang.

## 2. Build driver, test quality, AGENTS.md compliance and CI review (Sonnet)

### Review: runtime-TLS shim + Win32 thread model (build driver, test quality, AGENTS.md compliance, CI, evidence)

Reviewer: build-driver / test-quality / AGENTS.md-compliance (Sonnet). Repo HEAD 52a1a2970c2a182c155c718432a9dee520b0ca72.
Under review: `clvk_runtime/proposal-runtime-tls-win32.diff` (sha256 da538045da187c526cf3fc60588265230fc6378a9310992a9ecd7369849bf898,
8 files, +1408/-20) and `clvk_runtime/optional-probe-coverage.diff` (sha256 5d5391cc10806b7521143e694fd6c2b6f4887be3f04dc1bfb85074d49de30a68).

Scope reminder: low-level correctness of `src/vgpu/clvk_runtime_tls.c` is another reviewer's job. Everything below that touches the shim is
about its tests, its gates and its documentation.

All work was done in an overlay under `scratchpad/clvk_review_sonnet/` (`overlay/` = `git archive HEAD` + the diff applied; `submodules`
symlinked read-only). Nothing in `/home/dev/Waddle-LSW` was edited, added, committed, or built in; `git apply --check` only. The real
approved DLL and receipt were only read. No `make vgpu-opencl-build`. No VM/QEMU/GPU use. Line numbers below are post-image lines of the
overlay files (identical to the diff's new-file lines).

---------------------------------------------------------------------------------------------------------------------------------
## 0. Results at a glance

| Item | Result |
|:--|:--|
| `git apply --check` of main diff vs HEAD and vs the current working tree | OK / OK (also `--whitespace=error` OK; no TODO, no trailing whitespace, no non-ASCII in added lines) |
| optional diff on top of the proposal | applies; 25 tests OK; fixture builds; probe inventory becomes 4 objects / 40 B / align 8 |
| `python3 -B tests/vgpu/clvk_build.py`, private win32 SDK on PATH | exit 0, 25 tests OK in ~3 s (baseline HEAD: 19 tests OK; so 6 new = 3 `windows_toolchain_tests_t` + 3 `runtime_tls_tests_t`) |
| same, stock host PATH (no private win32 SDK) | exit 1, 25 run, 4 errors in 3 tests (`FileNotFoundError`/missing Win32 compiler). The proposal makes the standard host test run depend on decision item 5 |
| `make vgpu-clvk-runtime-tls-test` | exit 0 (`CLVK_RUNTIME_TLS_TEST PASS`) |
| `make vgpu-clvk-runtime-tls-sanitizers` | exit 0 (ASan+LSan+UBSan pass; TSan pass) |
| `make vgpu-clvk-runtime-tls-coverage` | exit 0, lines 93/93, branches 46/46 (fake-mode compile only, see 5.5) |
| `make vgpu-opencl-runtime-fixture` | exit 0 (posix dll, win32 dll, 2 zig exes, win32_tls dll + map gate rc 0) |
| `python3 -B scripts/clvk_buffer_coverage.py` (existing CI step, helper module reused) | exit 0, 161/161 lines, 200/212 (94.34%) branches |
| flakiness of the C test | 300 plain + 60 ASan/UBSan + 60 TSan runs, 0 failures |
| new audit run on the real approved DLL a8fd9d7a... (187 MB, read-only) | rejected in 3.6 s: unpaired TlsAlloc sites `_pthread_once_raw.constprop.0.isra.0`, `pthread_tls_init`; emutls inventory measured 11 objects / 96 B / align 8 (matches the approval request) |
| new audit on old POSIX probe / old Win32 probe / shim probe | rejected / rejected (`__emutls_get_address`, `__gthr_win32_create`, `__gthr_win32_self`) / accepted |
| `main()` end-to-end with ONLY cmake/ninja/native-LLVM faked (real verify_clvk.sh, tool capture, preflight, shim compile, toolchain, inspect_dll, map gate, audit, receipt) | works; second run = "Verified OpenCL cache"; shim object dropped from link => fails closed, no receipt; touching the shim source changes the cache key |
| mutation checks (build driver gates) | 30 mutants: 18 killed, 12 SURVIVED |
| mutation checks (shim vs C test + sanitizers + coverage) | 27 mutants: 23 killed (3 only by sanitizers, 1 by hang), 4 SURVIVED |
| skip-one-test coverage experiments | 7: 3 caught by the 90% gate, 4 not |

Production default after the diff (answer to "exactly what"): the driver is switched, not parameterised. `python3 scripts/build_clvk_windows.py`
and `make vgpu-opencl-build` ALWAYS use `x86_64-w64-mingw32-{gcc,g++}-win32` (thread model verified as exactly one `Thread model: win32`
line from `-v`), `-mthreads` at compile and link, `-static-libgcc -static-libstdc++`, no `-lwinpthread` whole-archive, the shim compiled
with gcc-win32 and put into `CMAKE_SHARED_LINKER_FLAGS_INIT` as a raw object for EVERY shared library, a linker map for every shared
library link, and the post-link gates `inspect_dll` (unchanged), `verify_runtime_link`, `audit_runtime_tls`. There is no env var, flag or
second code path for the POSIX configuration. The approved POSIX DLL stays on disk but cannot be rebuilt or re-verified with the new
driver; a successful Win32 build overwrites `build/clvk_client.json` (the old receipt is only kept as `<new folder>/prior_receipt.json`).
Rollback therefore needs a revert plus `cp prior_receipt.json build/clvk_client.json`, otherwise a revert finds a cache-key mismatch and
rebuilds for hours. The approval request's sentence "The approved DLL and its receipt remain in build/clvk_client/" is only half true.

---------------------------------------------------------------------------------------------------------------------------------
## 1. AGENTS.md compliance (every added/changed line, 8 files + optional diff)

Method: tokenised all 969 added-line identifiers of the main diff (and the optional diff) and classified them
(`tools/ident_scan.py`, output in `logs/ident_scan_main.txt`), then read every added line.

### 1.1 camelCase: zero violations
All `camel` hits are exempt or false positives. Justified exemptions (names Waddle does not define):

* Python stdlib `unittest` API used in `tests/vgpu/clvk_build.py` (and already used throughout the existing file): `assertEqual`,
  `assertNotEqual`, `assertTrue`, `assertIn`, `assertNotIn`, `assertIsNotNone`, `assertRaisesRegex`, `assertLessEqual`,
  `assertGreaterEqual`, `subTest`, `TestCase`.
* False positives from string literals / format codes: `nThread` (from `'\nThread model'`), `tT`/`dT`/`fZ` (strftime `%Y%m%dT%H%M%S%fZ`).
* Win32 SDK / mingw-w64 / NT ABI, reproduced name-for-name by `tests/vgpu/clvk_runtime_tls_fake/windows.h` and used by the shim:
  functions `AcquireSRWLockExclusive`, `ReleaseSRWLockExclusive`, `CreateThread`, `ResumeThread`, `TerminateThread`,
  `WaitForSingleObject`, `GetExitCodeThread`, `CloseHandle`, `GetThreadId`, `GetCurrentThreadId`, `GetLastError`, `__fastfail`;
  types `HANDLE`, `DWORD`, `ULONG`, `LPVOID`, `SRWLOCK`, `LPTHREAD_START_ROUTINE`, `WINAPI`; macros `SRWLOCK_INIT`, `INFINITE`,
  `WAIT_OBJECT_0`, `WAIT_FAILED`, `CREATE_SUSPENDED`, `ERROR_NOT_ENOUGH_MEMORY`, `FAST_FAIL_FATAL_APP_EXIT`, `WIN32_LEAN_AND_MEAN`;
  CRT symbols `_tls_index`, `_tls_start`; TEB field name `ThreadLocalStoragePointer` (comment only).
* libgcc/libstdc++ ABI that the shim must define: `__emutls_get_address`, `__emutls_register_common`, `__gthr_win32_create`,
  `__gthr_win32_join`, `__gthr_win32_self`; referenced `__gthr_win32_key_create`/`__gthr_win32_key_delete`; the `__emutls_v.` prefix
  and `.tls$WDL` section name (the latter is Waddle-chosen but must follow the linker's `.tls$xxx` ordering rule).
* POSIX/glibc/GCC builtins in the Linux double: `pthread_*`, `PTHREAD_MUTEX_INITIALIZER`, `WIFEXITED`, `WEXITSTATUS`, `fork`, `waitpid`,
  `__ATOMIC_*`, `__thread`, `_Static_assert`, `_Alignas`, `_GNU_SOURCE`.
* CMake variables (`CMAKE_C_COMPILER`, `CMAKE_SHARED_LINKER_FLAGS_INIT`, ...), linker/compiler flags, `ASAN_OPTIONS`/`LSAN_OPTIONS`/`UBSAN_OPTIONS`/`TSAN_OPTIONS`.
* Optional diff: `std::thread::id`, `std::uncaught_exceptions`, `std::runtime_error` (C++ stdlib).

### 1.2 Types without `_t`: zero violations among Waddle-defined types
Waddle types: `emutls_object_t`, `runtime_tls_record_t`, `gthread_start_t`, `runtime_tls_constants_t`, `test_constants_t`,
`struct fake_thread_t`, Python classes `windows_toolchain_tests_t`, `runtime_tls_tests_t`. The Win32 typedef names in the fake header
(`HANDLE`, `DWORD`, `ULONG`, `LPVOID`, `SRWLOCK`, `LPTHREAD_START_ROUTINE`) are exempt ABI mirrors.

### 1.3 Violations found (12 groups). Worst first.

| # | Sev | Where (file:line) | Violation | Exact fix |
|:-:|:-:|:--|:--|:--|
| P1 | HIGH | whole diff; `impl/software-vgpu-slicing/IMPL_DESC.md:5955-5983` (toolchain/linkage description: posix threads, whole-archive static winpthread); `CLVK_RUNTIME_RECEIPT_2026_10_17.json` (`current_production_configuration`: "unchanged POSIX/static winpthread"); `TRACKER.md` row #5 ("switch withdrawn") | AGENTS.md 1.2/1.3/3.1/3.2/4.2: no IMPL_DESC.md/TRACKER.md change in the change set; the existing documents state the opposite of what the new driver builds; one 1408-line, 8-file, 5-concern diff cannot be an atomic commit | Split into >=5 commits: (1) `feat(clvk-runtime): shim + fake + unit test` (src, test .c, fake windows.h, the three `vgpu-clvk-runtime-tls-*` targets); (2) `test(clvk-runtime): 90% coverage gate`; (3) `feat(build): win32 thread model + shim link + post-link gates` (driver + clvk_build.py); (4) `chore(ci): win32 toolchain + shim gates`; (5) `docs(vgpu): IMPL_DESC 5.x + TRACKER #5 + receipt json`. Each with `Affects TODO: #5 (+n%)` and a TRACKER commit-log row. Rewrite IMPL_DESC.md sections for architecture, memory layout (arena 512 B/32 slots/16 B align, `location` encoding), concurrency (SRW lock + acquire/release), failure modes (`__fastfail`), the documented `std::this_thread::get_id()` behaviour difference, verification criteria |
| D1 | MED | `tests/vgpu/clvk_runtime_tls_fake/windows.h:45-73` (14 declarations) | Public header functions have one-line comments only: no `@param[in/out]`, nullability, ownership, return/error codes, thread safety (AGENTS 1.6) | Expand each to the structured block the shim uses, e.g. `@param[in,out] lock Non-null, caller-owned; @return void; @thread_safety Serialises concurrent callers; @note fake only` |
| D2 | MED | `windows.h:17,31,32,33,75,76` | `WINAPI`, `DWORD`, `ULONG`, `LPVOID`, `#define malloc(size)`, `#define free(pointer)` undocumented; the last two silently re-define libc names for every later include | Add `/** ... */` for each; document that they rename libc allocation for the shim TU only, and keep `<stdlib.h>` included before the macros (already true) |
| D3 | MED | `src/vgpu/clvk_runtime_tls.c:126,136` (+ feature switch used at 36,125); `:130-134` | Macro `RuntimeTlsSymbol` (2 definitions) and the public build switch `WaddleRuntimeTlsFakeWindows` have no Doxygen; the fake `runtime_tls_current_record` has none (only the Windows variant at 148-153) | `/** @def RuntimeTlsSymbol ... renames the five ABI entry points to waddle_test_* in the double; identity otherwise */`; `/** @def WaddleRuntimeTlsFakeWindows ... selects the Linux double; never defined for Windows builds */`; add a `@brief/@return/@thread_safety` block on the fake accessor |
| D4 | LOW-MED | `clvk_runtime_tls.c:190-196` (`runtime_tls_assign`), `:230-238` (`__emutls_get_address`), `:285-290` (`gthread_entry`), `:345-352` (`__gthr_win32_join`), `:261-269` (`__emutls_register_common`) | Error semantics are only in the file header: no per-function statement that align>16, non-power-of-two align, size>512, >32 objects, arena overflow, or size growth after assignment terminate via `__fastfail`; `gthread_entry` lacks `@thread_safety`; `join` lacks nullability of `thread`; `register_common` "thread safety" is a precondition (constructors only), not a guarantee | Add `@note Terminates the process via __fastfail when ...` to both emutls functions and `@pre thread != NULL`, `@thread_safety` lines |
| D5 | LOW | `tests/vgpu/clvk_runtime_tls_test.c:27-29` (enum members), `:64` (`Check`), `:76-78,81-101,118-208` (externally linked fake Win32 definitions) | No per-member / per-macro / per-definition documentation (the header one-liners are the only docs) | One-line `/** */` per member; `@def Check`; reference the header block for the fakes |
| N1 | LOW | `clvk_runtime_tls_test.c:226` (13 uses) | `static const unsigned char template_bytes[16]` is a constant but snake_case (AGENTS 1.5: constants PascalCase) | rename to `TemplateBytes` (all 13 uses) |
| N2 | LOW | `scripts/build_clvk_windows.py:338-347,367-425`; `clvk_runtime_tls.c:104`; `clvk_runtime_tls_test.c:129,159,181,192,437-447` | Unnamed compile-time values: PE magic/offsets (`0x3c`, `0x20b`, `24`, `112`, `8`, `0x20000000`, `0x7fffffff`), x86 opcodes (`0x48 0x4c 0x8b 0xff 0x15 0x25 0xc7 0x05`), `_Alignas(16)` instead of `RuntimeTlsMaxAlign`, fake error codes `1455/5/6/87` | Module-level PascalCase constants (`PeOptionalHeaderMagicPe32Plus`, `SectionMemExecute`, `OpcodeCallIndirectRip`, ...); `_Alignas(RuntimeTlsMaxAlign)`; `ErrorFakeCommitLimit` etc. |
| M1 | LOW | `clvk_runtime_tls.c:297,328,338,369`; test `:200-208` | AGENTS 1.8 "defensive NULLing of freed pointers ... mandatory in C": `free(parameter)`, `free(start)`, `CloseHandle(thread)` are not followed by NULLing (all are dead locals/params after the call) | `parameter = NULL;` etc. or document the exemption for end-of-scope locals |
| T1 | LOW | `scripts/vgpu_dxvk.mk:103-115` | Make target prefix `vgpu-clvk-runtime-tls-*` while the same subsystem's neighbours are `vgpu-opencl-*` (`vgpu-opencl-runtime-fixture`, `vgpu-opencl-buffer-coverage`, `vgpu-opencl-test`); the `-test/-sanitizers/-coverage` triple itself matches `vgpu-loader-environment-*` | rename to `vgpu-opencl-runtime-tls-{test,sanitizers,coverage}` (also in the workflow) |
| L1 | NOTE | `src/vgpu/clvk_runtime_tls.c` | Language policy: C is the default and the file must be a GNU-ld-linked COFF object with `.tls$WDL`, inline asm and `__fastfail`, which supports C. But it is a fixed-capacity, hand-bounds-checked arena/slot allocator, the class AGENTS 1.4 reserves for Zig; no written justification exists anywhere in the change set | Record the C-vs-Zig decision in IMPL_DESC.md. No C++ was added in tests. The optional diff edits the existing C++ libstdc++ probe, which is justified (it exercises libstdc++ runtime state) |
| X1 | LOW | `build_clvk_windows.py:256-266` (`write_toolchain` docstring merges `shim` into the `export_file` sentence); `scripts/clvk_runtime_tls_coverage.py:2-8` (docstring says "every ... production function, >=90% of its lines and branches" but code is aggregate, line 81); optional diff `clvk_runtime_windows.cpp` return doc "(5, 6, 7 name the last three)" and literals 7/99/8 | Ambiguous documentation / unnamed constants | Rewrite: "shim is the private Windows header shim directory"; "aggregate >=90% line and >=90% branch over the file, every function present"; document 5 = thread_local initial value wrong, 6 = thread_local isolation or thread-id identity wrong, 7 = exception not caught, and name the literals 7/99/8 |

Not violations: README untouched, no submodule change, no new dependency, file names snake_case, PascalCase function-like test macros
(`Check`, `RuntimeTlsSymbol`) follow existing repo precedent (`tests/vgpu/clvk_buffer_api.cpp:208`, `opencl_integration.c` `Require`).
Count: 12 groups; 0 camelCase, 0 `_t` violations; worst = P1 (process/documentation sync), then D1-D3.

---------------------------------------------------------------------------------------------------------------------------------
## 2. Build driver (`scripts/build_clvk_windows.py`, read in full in the overlay)

### 2.1 Structure after the diff
`Compilers`/`Tools` (44-46) swap posix for win32; constants 50-62; `verify_compiler_thread_models` (102-119); `build_settings`
(122-131); `compute_cache_key` (134-136); `compile_runtime_tls` (243-253); `write_toolchain` (256-283); `verify_runtime_link`
(314-327); `read_image`/`image_offset`/`audit_runtime_tls` (330-431); `main` wiring (434-520: inputs 443-451, shim compile and toolchain
474-476, gates 498-500, contract re-check 503-507, receipt 509-513).

### 2.2 Logic correctness (checked with real tools)
* CMake semantics verified with a real CMake+Ninja mini-project using the generated toolchain file (`cmake_probe/`): the
  `*_INIT` variables are honoured from a toolchain file; the link line is `... OpenCL.def clvk_runtime_tls.o -Wl,-Map=... -static-libgcc
  -static-libstdc++ -mthreads -shared -o libOpenCL.dll ... objects`; the map is produced at the stated path; `inspect_dll`,
  `verify_runtime_link` and `audit_runtime_tls` all accept that DLL (121 exports, 4 emutls objects, site `__gthr_win32_key_create`).
* Archive order is irrelevant for the shim (explicit object defines the symbols before `libgcc_eh.a` is scanned), the docstring claim
  is right.
* `verify_runtime_link` (real maps): feeding real maps with the shim token injected, libgcc map is rejected for
  `libgcc_eh.a(emutls.o)` + `libgcc_eh.a(gthr-win32-thread.o)`, POSIX/winpthread map for `libgcc_eh.a(emutls.o)` + `libwinpthread.a(`;
  performance fine (110 MiB map: 0.8 s, 282 MiB RSS).
* `main()` smoke (`tools/smoke_main.py`, only cmake/ninja/native faked): works end to end, cache hit on second call, fail-closed
  with no receipt when the shim is not on the link line, cache key changes on a shim edit.

### 2.3 Error handling (does every subprocess failure fail closed with a clear message?)
All fail closed (non-zero exit, no receipt published), but only some with a clear message:
`verify_compiler_thread_models` -> `CalledProcessError` traceback if `-v` fails (114); `verify_runtime_link` -> bare `FileNotFoundError`
if the map is not produced (321); `audit_runtime_tls` -> `CalledProcessError` from nm, `struct.error`/`ValueError` tracebacks for
truncated or corrupt PE input (no explicit bounds checks anywhere; fuzzed, see 2.6). Fix: wrap in `SystemExit('OpenCL link map missing: <path>
(linker flag not applied?)')` etc.

### 2.4 Cache key / receipt (can a stale DLL be reused?)
Covered: shim source sha256 (`runtime_tls_sha256`, 450), driver sha256 (so every code/flag change), export list, pins, private patches,
ABI header, `settings` (thread model, `-mthreads`, runtime list, capacity triple), `tools` (first `--version` line of cmake, ninja, the two
win32 compilers, dlltool, objdump, nm), `compiler_thread_models` (observed model + sha256 of the two compiler DRIVER executables).
NOT covered: `cc1`/`cc1plus`/`collect2`/`ld`/`as`/`ar`/`ranlib`/`windres`, and above all `libgcc.a`, `libgcc_eh.a`, `libstdc++.a`, the
mingw-w64 CRT and headers, although the shim is ABI-coupled to exactly those archives. `--version` for these packages is
`(GCC) 14-win32` (no minor version, no date). Consequence: a runtime-library-only package update with unchanged driver binaries is a
cache HIT and reuses a DLL whose gates ran against a different libgcc; the cache-hit path never re-runs the gates. Also nothing asserts the
reviewed version: GCC 13 (CI) or GCC 15 would be accepted silently as long as it says `Thread model: win32`.
Fix: hash `gcc -print-file-name=libgcc.a|libgcc_eh.a|libstdc++.a|libmingw32.a`, record `-dumpmachine`, and fail unless the toolchain
equals the reviewed Debian package version (14.2.0-19+27+b1) or an explicit reviewed allowlist.

Side effect found: `native_key` (453) hashes `tools`, which now lists the win32 instead of posix compilers, so the host-only LLVM
TableGen/libclc cache changes from `native_d84f8e2c6bb3` to `native_6a92f958ac4e` (computed from the approved receipt vs the new
driver). The expensive, MinGW-independent native LLVM tool build is rebuilt for no reason. Fix: derive `native_key` from cmake, ninja
and the host compiler only.

### 2.5 Determinism / path handling
* `link_map_sha256` hashes a map that embeds the random `mkdtemp` folder and absolute toolchain paths, so it is provenance only, not a
  reproducibility check. DLL/object timestamps are unchanged from before.
* `write_toolchain` interpolates `export_file`, `runtime_object`, `link_map` unquoted into a CMake string and linker flags: a checkout
  path with spaces, `;`, `"` or `$` would break the link (same weakness as before for `export_file`, now three paths).
* The map is requested for EVERY shared-library link (`CMAKE_SHARED_LINKER_FLAGS_INIT`); the last link wins. `verify_runtime_link`
  never checks that the map belongs to `OpenCL.dll`. The map contains `OUTPUT(libOpenCL.dll pei-x86-64)` (verified) so a one-line check
  is available. Also the shim object is linked into every shared library, and `OpenCL.def` is passed to every shared library (pre-existing).

### 2.6 Gates under adversarial input (experiments, `exp/`, `fuzz/`)

**(a) Link-map gate (Python).** Cannot pass vacuously: empty map -> "does not contain the runtime TLS object"; missing map ->
`FileNotFoundError`; the member scan matches real ld output (verified on real maps). Weak spots: only the substring
`clvk_runtime_tls.o` proves the shim was on the command line (not that it resolved the symbols); wrong-DLL map (2.5).

**(b) Link-map gate (Makefile fixture) is fail-once and vacuous** (`scripts/vgpu_dxvk.mk:96-98`):
1. Mutate the rule so the shim is not linked (`$^` -> `$<`): run 1 fails (exit 2, correct). Run 2 with nothing changed prints
   "'build/vgpu_clvk_runtime_win32_tls.dll' is up to date." and exits 0; `make vgpu-opencl-runtime-fixture` also exits 0, while the map
   still contains 26 forbidden-member lines. The gate is a trailing recipe line, the invalid DLL stays on disk, and no
   `.DELETE_ON_ERROR` is set anywhere in the makefiles.
2. `! grep -E ... map` passes for a missing map file and for an empty map (rc 0 verified); there is no positive check that the shim is in
   the map; the pattern lacks `libpthread\.a\(` that the Python list has.
Fix: `.DELETE_ON_ERROR:` (or `|| { rm -f $@ $@.map; exit 1; }`), `grep -q 'vgpu_clvk_runtime_win32_tls\.o' $@.map && ! grep -E '...|libpthread\.a\(' $@.map`,
or better call the Python gates (`verify_runtime_link` + `audit_runtime_tls`) on the fixture DLL so there is one implementation.

**(c) Emulated-TLS inventory** (382-391). Works on real binaries: synthetic DLLs with 33 objects, a 600 B object and `alignas(32)` are all
rejected with the right message; the approved DLL measures exactly 11/96/8. Problems:
* No automated test has a violating input: removing the whole check (M13), an off-by-one in the slot limit (M14), and dropping the
  alignment padding term (M15) all SURVIVE the 25 tests.
* It is not a sound bound: the audit lays objects out in ADDRESS order, the shim assigns offsets in FIRST-TOUCH order. Counterexample
  executed against the real shim code in the double (`exp/inv_counter.c`): 32 objects with (size,align) pairs
  (1,4)x2 (1,16)x2 (2,1) (8,1)x2 (8,16) (12,1)x4 (12,4) (12,8) (15,1)x4 (15,2) (15,16)x2 (16,1)x2 (17,1)x2 (17,4) (17,8) (17,16)
  (24,1) (24,4) (24,16)x2: audit total (align-descending address order) 499 B <= 512, a first-touch order needs 548 B and the shim
  `__fastfail`s (child exits 77). Practically irrelevant at today's 11/96/8, but "the build gate prevents shipping a DLL that could
  overflow" is false as stated. Fix: bound by sum(size + max(align,1) - 1) <= 512.
* On a stripped DLL it silently reports 0 objects (it then fails later with site `?` only because TlsAlloc happens to be imported).

**(d) TlsAlloc call-site audit** (392-429). Decides by: find the `TlsAlloc` import's IAT slot by name, scan every executable section at
every byte offset for `ff 15|ff 25 disp32` and `48|4c 8b|ff modrm(rip) disp32` resolving to that slot, attribute to the preceding
`t/T` symbol from `nm`, drop owner `TlsAlloc` (the kernel32 import thunk), allow only `__gthr_win32_key_create`. Verified blind spots,
each producing PASS with `tls_alloc_sites: []` (fixtures `exp/leak_*.c`):

| fixture | result |
|:--|:--|
| declared via `<windows.h>` (dllimport, `call *__imp_TlsAlloc`) | detected: `leak_index` |
| declared without dllimport (`call/jmp TlsAlloc` thunk, e8/e9 rel32) | **PASS (missed)** |
| address of `TlsAlloc` stored in a pointer (data relocation to the thunk) | **PASS (missed)** |
| `FlsAlloc` (fiber-local index, the API rpmalloc uses; not compiled in this build) | **PASS (not covered by design)** |
| `GetProcAddress("TlsAlloc")` | PASS (static scan cannot see it) |

* The allow-list is a SITE list: any caller of `__gthr_win32_key_create` is accepted, so "paired with TlsFree" is an assumption,
  not a check. In the probe the only caller of the allowed site is libstdc++'s anonymous-namespace `key_init`, whose address is taken
  only in `std::__at_thread_exit` and `std::notify_all_at_thread_exit` (neither called by the probe), and whose function-local static
  `key_s` is destroyed by `__tcf_0`, which calls `__gthr_win32_key_delete` (confirmed in the disassembly). That supports the
  author's pairing claim for the probe only, and "the probe cannot reach it" is by reading, not by an executed check.
* Inlined copies: not an issue for libgcc (no LTO bytecode), would produce false positives (fail closed). Multiple `TlsAlloc` import
  descriptors: only the LAST slot is kept (`slot = ...` at 405).
* Pinned LLVM/clvk sources contain `TlsAlloc`/`FlsAlloc` only in `jitprofiling.c` and rpmalloc (both off by default), so the blind spots
  are not exercised by known production code today.
Fix: also collect references to the thunk symbol (rel32 targets and 8-byte data words equal to ImageBase+RVA), put `FlsAlloc` and
`TlsAllocEx`-style APIs on the list, assert callers of `__gthr_win32_key_create` are in an allow-list.

**(e) PE parser robustness.** Fuzz of a known-bad DLL (170 cases: truncations, header fields, 150 random bit flips): 155 SystemExit,
8 `CalledProcessError` (nm), 5 `struct.error`, and 2 vacuous PASSES: clearing `IMAGE_SCN_MEM_EXECUTE` on every section, or setting
`.text` raw size to 0, makes the audit report `tls_alloc_sites: []` for a DLL that leaks. Never silent crash; two silent
misreports. Fix: require `symbols` non-empty, require `__emutls_get_address`/known text symbols present, require that at least the
`TlsAlloc` thunk reference itself was found whenever `slot` is set (a free vacuity canary), and bound-check reads.

### 2.7 Other defects
* All gates run after the multi-hour cross build in a fresh `mkdtemp` tree (469): any gate failure costs a full rebuild. The shim
  compile (474) also happens only after `build_native`. Suggest a pre-flight before `build_native`: compile the shim and link a tiny
  stub through the generated toolchain, then run `verify_runtime_link` + `audit_runtime_tls` (my `smoke_main.py` stub is a working
  template).
* `ForbiddenImports` (49) still catches `libwinpthread` DLL imports; the map gate catches static winpthreads members.
* The TEB accessor (`mov %gs:0x58`, `_tls_index`, `.tls$WDL` placement) is exercised by no Linux test and inspected by no gate; the
  `.tls$WDL`-inside-TLS-directory property is not checked anywhere (see 3.3).

### 2.8 Defects ranked
1. HIGH Makefile fixture gate fail-once/vacuous (2.6b).
2. HIGH `main()` wiring and cache-key coverage untested: 12 surviving mutants incl. gate calls removed, wrong map path, shim hash or
   compiler models dropped from the key (3.2).
3. HIGH TlsAlloc audit blind spots + "paired" unenforced + two vacuous-pass modes (2.6d, 2.6e).
4. MED Inventory gate has no failing test and is not a sound bound (2.6c).
5. MED Weak toolchain binding; no version assertion; CI GCC 13 (2.4, section 4).
6. MED Late failure cost; `native_key` churn (2.7, 2.4).
7. MED-LOW Map not bound to the DLL; accessor/section placement unverifiable on Linux (2.5, 3.3).
8. LOW Error-message quality, docstring defects, unquoted paths, rollback nuance, non-reproducible `link_map_sha256`.

---------------------------------------------------------------------------------------------------------------------------------
## 3. Test quality

### 3.1 Per-test verdict (Python, `tests/vgpu/clvk_build.py`)

| test (line) | can it fail for a real regression? |
|:--|:--|
| `test_win32_compilers_are_preflighted` (191) | yes for posix compiler names (M06). Partly a change detector: expectation is a copy of the constant |
| `test_actual_models_are_win32_and_reject_wrong_or_missing_diagnostics` (202) | yes: real compilers + mocked wrong/missing/duplicate diagnostics (M07, M08 killed). The "compiler missing" branch (113) is untested |
| `test_generated_toolchain_and_settings_invalidate_old_cache` (212) | toolchain text: yes (exact string, M22-M24 killed) but tautological (same format string). The "invalidate old cache" half only proves that two different dicts hash differently; it does not prove `main()` puts the shim hash or compiler models into the key (M20/M21 survive) |
| `test_driver_constants_match_source` (260) | yes, good (M25 killed) |
| `test_link_map_rejects_each_forbidden_member` (268) | yes for synthetic lines (M01-M05 killed); real map text never used for the negative case in the suite (I verified manually) |
| `test_actual_probe_links_replace_libgcc_tls_owners` (283) | best test: real compile+link with and without shim (M09, M10, M12, M16, M26, M30 killed). Weak: asserts `tls_alloc_sites` is a SUBSET of the allow-list (also true for `[]`), so dropping the REX-mov decoding (M11) survives; no assertion on the exact site set or on inventory limits; the real-map negative case is not fed to `verify_runtime_link` |

### 3.2 Mutation results, build-driver gates (30 mutants, command `python3 -B tests/vgpu/clvk_build.py`; `logs/mut_driver.txt`)

KILLED (18): M01 map member match neutered; M02 shim-presence check removed; M03/M04/M05 forbid list loses emutls / gthr-win32-thread /
winpthread; M06 compilers back to posix; M07 thread-model check neutered; M08 tolerates extra thread-model lines; M09 allow-list widened;
M10 audit skips all code sections; M12 audit clears sites; M16 inventory never sees emutls; M22 settings drop thread model; M23 toolchain
drops shim object; M24 CXX compiler back to posix; M25 arena constant drift; M26 wrong PE magic; M30 wrong import-directory offset.

SURVIVED (12):
| id | mutation | why it matters |
|:--|:--|:--|
| M17 | `main()` no longer calls `verify_runtime_link` | no test executes `main()`; the gate can be unwired |
| M18 | `main()` skips `audit_runtime_tls` | same |
| M19 | `main()` passes a different map path to `write_toolchain` than it verifies | wrong-map wiring |
| M20 | `runtime_tls_sha256` removed from the cache key | a shim edit would reuse a stale DLL (the smoke run proves the real code is right today) |
| M21 | `compiler_thread_models` removed from the key | compiler swap would reuse a stale DLL |
| M27 | receipt drops the compiler sha256 | binding claim unverified |
| M13 | whole inventory limit check removed | headline gate has no failing test |
| M14 | slot limit off by one | same |
| M15 | alignment padding dropped from the total | same |
| M11 | REX-prefixed `mov` decoding removed | encoding never exercised |
| M28 | shim compiled without `-Werror -mthreads` | flags untested |
| M29 | shim compiled with gcc-posix | harmless for a thread-model-independent object, but untested |

A small `main()` smoke test with faked cmake/ninja (`tools/smoke_main.py`, ~90 lines, uses the real stubs of this review) would kill
M17-M21 and M27; three tiny negative DLL fixtures (33 objects / 600 B / align 32; I built them with `g++ -shared`) would kill M13-M15.

### 3.3 Mutation results, shim vs C test, sanitizers and coverage (27 mutants; `logs/mut_shim.txt`)
KILLED (23): S01 max-align check removed; S02 pow2 check removed; S03 slot-capacity off by one (**only ASan**, plain test and coverage pass);
S05 template copy removed; S06 initialised flag never set; S07 fake record not thread-local; S08 release store -> relaxed (**only TSan**);
S09 acquire load -> relaxed (**only TSan**); S10 size-growth check removed; S11 register_common template guard; S12 join does not close
handle; S13 start block not freed; S14 self-join not detected; S15 create failure leaks start; S16 resume failure without terminate (killed
by hang/timeout: the make target has no timeout); S17 foreign identity tag removed; S18 gthread handle not recorded; S19 alignment rounding
removed; S20/S21 wrong error codes; S22 join value not stored; S23 shared (non-TLS) record; S24 no recheck under lock.

SURVIVED (4):
* S04 `offset > Arena - size + 1` and S04b `+ 8` (arena boundary accepts an overflow): by the time `test_fail_fast_paths` runs, the
  process-global layout already holds 11 slots / 112 bytes (instrumented: `used_slots=11 used_bytes=112`), so `scenario_arena_overflow`
  trips on its FIRST object (112 > 512-504) and never reaches the intended second one; `scenario_slot_exhaustion` trips at the 22nd, not
  the 33rd, object. Exact-fit-accepted and one-byte-over-rejected are never asserted. (S03 only dies under ASan for the same reason.)
  Fix: run each fail-fast scenario in a child that first resets/avoids shared layout state (e.g. fork before any other test, or expose a
  test reset), and add exact-fit and exact-overflow cases.
* S25 `RuntimeTlsTebVectorOffset` 0x58 -> 0x60 and S26 section name `.tls$WDL` -> `.data$WDL`: both are inside the Windows-only branch.
  `clvk_build.py` compiles that branch (and I confirmed it emits `mov %gs:0x58,%rax`), but nothing executes or inspects it. A wrong TEB
  offset or a section placed outside the TLS range would pass every Linux gate; only the native guest run can fail. Fix: add a driver
  test that (i) greps the shim object disassembly for `%gs:0x58`, (ii) parses the linked probe's TLS directory
  (StartAddressOfRawData/EndAddressOfRawData) and asserts the `.tls$WDL` symbol lies inside it with size 576.

### 3.4 Coverage script (`scripts/clvk_runtime_tls_coverage.py`)
* Enforces BOTH thresholds with `or` (81) and strict `<` (exactly 90.00% passes, "at least 90" is respected). Nonzero denominators:
  `assert lines and branches` (70), exactly one gcov file entry, exact function-name set (67). Not usable with zero denominators.
* Verified by removing tests: `test_register_common` skipped -> lines 97.85%, branches 89.13% -> FAIL (branch threshold effective
  independently of lines); `test_gthreads` skipped -> 83.87/82.61 FAIL; `test_fail_fast_paths` skipped -> 92.47/86.96 FAIL.
* But it is an aggregate over 93 lines / 46 branches of the FAKE-mode compile: skipping `test_layout_and_initialization` (100/97.83),
  `test_concurrent_first_use_and_isolation` (100/100), `test_assignment_is_idempotent` (100/97.83) or `test_self_join_keeps_handle`
  (98.92/97.83) is not detected, and the Windows-only accessor is outside the denominator (docstring discloses it; the CI summary
  "100%" must not be read as shim coverage). The gate is itself untested (no test lowers the threshold or feeds a bad report).
* Per-function execution is not enforced, unlike `scripts/clvk_buffer_coverage.py` (`execution_count > 0` for every function).

### 3.5 C unit test (`tests/vgpu/clvk_runtime_tls_test.c`)
Strong on initialisation, isolation, ownership and error returns (23/27 mutants killed, 0 failures in 420 repeated runs). Weaknesses: boundary state
sharing (S04/S03 above); races start without a barrier so first-use contention is incidental (TSan still caught S08/S09); no test that
two different DLL loads cannot share state (inherent); the fake allocator counters do not count the fake threads' `calloc`
(LSan covers them).

### 3.6 Optional diff (`optional-probe-coverage.diff`)
Applies cleanly on top of the proposal; `clvk_build.py` 25/25 and `make vgpu-opencl-runtime-fixture` still pass; the shim probe then has
4 emutls objects / 40 B / align 8 and the old Win32 and POSIX probes are still rejected by the audit. Compliance: no camelCase, no new
types; items in X1 (ambiguous return-code doc, literals 7/99/8 unnamed, include order). Value: it adds a `thread_local` with initialiser, a
caught exception and `std::thread::id` identity to the NATIVE probe, i.e. the pieces the shim replaces; on Linux CI it is only compiled
and audited, never executed, so it adds no Linux-side regression power. Recommended for acceptance, with the constants named.

---------------------------------------------------------------------------------------------------------------------------------
## 4. CI workflow (`.github/workflows/software_vgpu.yml`)

* Package set: `g++-mingw-w64-x86-64-win32` (pulls `gcc-mingw-w64-x86-64-win32`) provides `x86_64-w64-mingw32-{gcc,g++}-win32` on
  ubuntu-24.04; `gcc-mingw-w64-x86-64-posix` stays for the PE-parser fixture; `binutils-mingw-w64-x86-64` provides nm/objdump/dlltool.
  YAML parses; the job and timeout (15 min) are unchanged; nothing is `continue-on-error` or skipped.
* The new step runs only `make vgpu-clvk-runtime-tls-sanitizers vgpu-clvk-runtime-tls-coverage` (the sanitizer target already runs the
  test); `mkdir -p build` is redundant (the `| build` rule creates it). I verified the targets work on a bare PATH (no pkg-config,
  no zig): `GNUmakefile` parse-time `$(shell pkg-config ...)` is recursive/lazy. `-fsanitize=thread` appears nowhere else in the repo's
  makefiles/workflows, so TSan on the runner image is unproven (existing jobs use ASan/LSan/UBSan only).
* Compiler binding vs GCC 13: ubuntu-24.04's package is GCC 13-win32. The driver accepts any compiler whose `-v` says
  `Thread model: win32`; there is no GCC 14.2 assertion and no skip. The tests that run on CI are GCC-14-specific:
  `test_actual_probe_links_replace_libgcc_tls_owners` compiles `clvk_runtime_windows.cpp` (needs `std::thread`/`std::shared_mutex`
  on the win32 model) and expects the no-shim probe to show `__gthr_win32_create` as a TlsAlloc site. I cannot run GCC 13 here. My
  recollection (unverified) is that GCC 13's win32-model libstdc++ lacks `std::thread`, in which case the existing, currently green
  `opencl_admission` job turns red; if instead it compiles, the shim is exercised against a libgcc it was not reviewed for and the
  gates (names/sites only) cannot detect semantic drift. Either way CI assurance != the host toolchain.
  Fix: run this job in `debian:trixie-slim` like the `transport` job, install the pinned `14.2.0-19+27+b1` packages, and make the
  driver compare the toolchain with the reviewed version.
* Not invoked in CI at all: `make vgpu-opencl-runtime-fixture` (needs zig) and therefore the Makefile map gate; `main()` and the
  production gates. The Python tests are the only CI exercise of `verify_runtime_link`/`audit_runtime_tls`.
* The previously withdrawn Win32 candidate carried the same workflow edit but was never pushed, so there is no prior CI evidence.

---------------------------------------------------------------------------------------------------------------------------------
## 5. Evidence discipline

### 5.1 Statements in `approval_request.md` I verified
Diff hash/size/applicability; "Not changed: clvk, LLVM, clspv, submodule pins, patches 0001-0003, README.md, shutdown ABI" (none touched);
25/25 tests; ASan/LSan/UBSan/TSan + 100%/100% (scope caveat in 3.4); no replaced member and no winpthreads in the probe map;
CreateSemaphoreA/CreateEventA/DuplicateHandle not imported by the shim probe (`objdump -p`); only TlsAlloc site is
`__gthr_win32_key_create`; gates reject the approved DLL, old POSIX probe, old Win32 probe and accept the corrected probe; the 11/96/8
inventory; the 1086-free-index arithmetic and the earlier native 1086->1084 / 543-reload stall are in the repo's own
`CLVK_RUNTIME_LIFETIME_REVIEW.md` (not re-run, no VM).

### 5.2 Claims (not verified facts)
1. "Static analysis attributes every one of them" (4 semaphores, 1 event, 1 duplicated handle, 1 TLS index): disassembly inference; no
   per-resource native confirmation.
2. 544th load aborts in libgcc's spin-once and the detach flush spins on `Sleep(0)`: the request itself says inferred.
3. "OpenCL.dll owns no TLS index ... that outlives a thread": a property of the shim, not of the DLL. The allowed
   `__gthr_win32_key_create` can still allocate an index (libstdc++ `key_init`, used by `__at_thread_exit`), freed only by the
   `__tcf_0` static destructor at unload, i.e. it outlives every thread if that facility is ever used.
4. "The Windows loader allocates the record for every thread, zero-fills it from the template, and releases ... when the module unloads or a
   thread exits": OS behaviour, UMDH check pending (the request flags it).
5. "Never a hang" on `__fastfail`: reasoning; the double uses `exit(77)`.
6. "The build gate prevents shipping a DLL that could overflow" and "headroom 5x / 3x": the latter is the POSIX DLL's measurement; the
   Win32 DLL's inventory is unmeasured; the former is false as stated (2.6c).
7. "Exact compiler versions and hashes are bound into the receipt and cache key": driver executables and first version line only (2.4).
8. "Post-link gates fail closed": mostly, with the blind spots in 2.6d/e and the Makefile gate in 2.6b.
9. Win32-model behaviours (CRITICAL_SECTION mutexes, `call_once` spinning on a throwing callable, never-deleted guard mutex, clvk init
   throws only on OOM): code reading, no native evidence.
10. "about 600 B per thread": consistent (`.tls$WDL` is 0x240 = 576 B in the object) but the loader's retention of that heap is open.
11. "Rollback: revert; approved DLL and receipt remain in build/clvk_client/": see 0 (receipt is overwritten).
12. "The cache key changes with the thread model and the shim hash, so builds cannot mix": true in code, untested (M20/M21).
13. "CI uses GCC 13, so the version skew must be watched": nobody has run the job.

### 5.3 What must be re-run because the build changes
* `make vgpu-opencl-build` (first full Win32 build ever; native LLVM tools rebuilt because `native_key` changes), then the receipt must
  show `runtime_tls` within limits, no `libwinpthread` import, `link_map_sha256`.
* Every native result bound to a8fd9d7a... (exact kernels, concurrent callers, shutdown, module/heap/handle retirement, 24-lifetime and
  three-cycle UMDH captures): the repo's own review says prior DLL execution cannot qualify a rebuilt DLL.
* The emutls inventory and TlsAlloc site list for the NEW DLL (measured, not projected).
* The ubuntu-24.04 `opencl_admission` job (never run with this change) and the host decision for the Win32 compiler (the stock PATH now
  fails 4 tests).
* All compile/link-time unknowns of the first production Win32 build: LLVM/clvk/clspv under the Win32 model (no winpthreads), any stray
  `-lpthread` from CMake's `Threads` detection (would trip `ForbiddenImports`/map gate hours in), and the shim's duplicate-symbol
  behaviour against the real libgcc members.

---------------------------------------------------------------------------------------------------------------------------------
## 6. Minimum changes I would require before approving (ordered)
1. Make the fixture gate fail-closed on rerun and non-vacuous (`.DELETE_ON_ERROR`, positive shim check, or call the Python gates).
2. Add a `main()` smoke test with faked cmake/ninja that asserts: gate order, `runtime_tls_sha256` and `compiler_thread_models` in the
   key, map path used == map path verified, failure when the shim is missing.
3. Add failing-input tests for the inventory (33 objects, 600 B, align 32) and the exact TlsAlloc site set of the shim probe
   (`== ['__gthr_win32_key_create']`).
4. Harden `audit_runtime_tls`: vacuity canaries (symbols present, thunk reference found, executable bytes scanned), thunk/pointer
   references, `FlsAlloc`, caller allow-list for `__gthr_win32_key_create`, conservative inventory bound, all `TlsAlloc` slots.
5. Bind the map to the DLL (`OUTPUT(libOpenCL.dll`), add a `.tls$WDL`-in-TLS-directory test and a `%gs:0x58` disassembly assertion.
6. Hash the runtime archives into the cache key, assert the reviewed toolchain version, exclude MinGW from `native_key`, add a pre-flight
   before `build_native`.
7. Run the CI job on the same GCC as the host (trixie container with pinned packages) or prove the GCC 13 run.
8. Fix the fail-fast scenarios' state sharing (S03/S04), name the constants, complete the Doxygen (D1-D5), split the commits and update
   IMPL_DESC.md/TRACKER.md/receipt json (P1).

---------------------------------------------------------------------------------------------------------------------------------
## 7. Artifacts (all under `scratchpad/clvk_review_sonnet/`)
`overlay/` (HEAD + diff, own git baseline), `logs/` (test logs, `mut_driver.txt`, `mut_shim.txt`, `mut_cov.txt`, `ident_scan_main.txt`,
`audit_approved.log`, `make_*.log`), `tools/` (`mutate.py`, `spec_driver.py`, `spec_shim.py`, `spec_cov.py`, `smoke_main.py`,
`ident_scan.py`, `drv.py`), `probe/` (shim/libgcc/posix probe DLLs and maps), `exp/` (leak fixtures, emutls inventory fixtures, counterexample
`inv_counter.c`), `cmake_probe/` (real CMake+Ninja check of the toolchain file), `fuzz/fuzz_audit.py`, `smoke_root*/` (main() smoke).
Reproduce: `export PATH=/home/dev/Waddle-LSW/build/qualification_20261017/mingw_win32_private_1402/extracted/usr/bin:$PATH` then, in
`overlay/`, `python3 -B tests/vgpu/clvk_build.py`, `make vgpu-clvk-runtime-tls-test vgpu-clvk-runtime-tls-sanitizers vgpu-clvk-runtime-tls-coverage`,
`make vgpu-opencl-runtime-fixture`, and `OVERLAY=$PWD python3 -B ../tools/mutate.py ../tools/spec_driver.py`.

## 3. v2 engineering notes: emergency pool and teardown order (Opus, partial, stopped at the user request)

### clvk runtime correction v2: PARTIAL report (stopped at the coordinator's request)

Repository `/home/dev/Waddle-LSW`, branch `feature/software-vgpu-slicing`, HEAD `52a1a29`.
This was offline work only. I did not touch the VM, QEMU, WinRM, guest or GPU. I ran no git
write commands. I edited no submodule, `patches/clvk/*`, README or driver/probe file.
Scratch directory: `.../scratchpad/clvk_v2/`.

## 0. Working-tree state (consistent)

I did **not** modify the four inherited v1 files, and I did **not** create any new file in
the repository. So there is no partial code to back out, and `build/.../clvk_v2/partial/`
is not needed.

| File | sha256 (unchanged v1) |
|:--|:--|
| `src/vgpu/clvk_runtime_tls.c` | `eadd210d8374cfafc6be2e0f5e1ac2ad97658b43bcec853a0f0f3f1a79724950` |
| `tests/vgpu/clvk_runtime_tls_test.c` | `63e9025428c9c823d6d29b351d138c0d82614558b4f13c5593ee60288efe9496` |
| `tests/vgpu/clvk_runtime_tls_fake/windows.h` | `2dcee8f240d7c444f1579bd8315bf2d68060a8feb59c4393313e70769f41c537` |
| `scripts/clvk_runtime_tls_coverage.py` | `a53e43bb24ce33858a257b550067ba53057a6792caa50315347b871eca8bcb29` |

Consistency check, run from `/home/dev/Waddle-LSW`:

```
cc -std=c11 -Wall -Wextra -Wpedantic -Werror -O0 -g -pthread -DWaddleRuntimeTlsFakeWindows \
   -Itests/vgpu tests/vgpu/clvk_runtime_tls_test.c -o $SCRATCH/clvk_v2/tests/v1_plain   # rc 0
timeout 60 $SCRATCH/clvk_v2/tests/v1_plain     # "CLVK_RUNTIME_TLS_TEST PASS", rc 0
```

`src/vgpu/clvk_runtime_eh_pool.c` and `tests/vgpu/clvk_runtime_eh_pool_test.c` do not exist yet.

## 1. Deliverable status

| Deliverable | Status |
|:--|:--|
| **A. eh-pool release** | Analysis is done for the allocation, `__freeres` semantics, the libstdc++ static-ctor inventory and the CRT attach/detach order. The mechanism is chosen and justified (§3). Not done: the C file, Linux fake-CRT tests, the Windows probe snippet, and the lazy-allocation audit of ios_init, locale_init and guard. |
| **B. Shim hardening** | Not started. The v1 code is unchanged. |
| **C. Tests, coverage and mutation** | Only the v1 plain test was re-run (PASS). No new tests, no coverage run, no mutants. |
| **D. Native plan** | Draft only (§5). It still needs the other worker's strengthened probe sources. They were unmodified at the time I checked: mtimes were 2026-10-17. |

## 2. Libstdc++ emergency pool: verified facts

Toolchain: Win32 `libstdc++.a`, sha256 `a5767e5b...b473`, private 14.2 extraction. I extracted
the archive members into `clvk_v2/ar_win32*` and disassembled them with relocations.

### 2.1 How the pool is allocated (`eh_alloc.o`)

* The static constructor is `_GLOBAL__sub_I__ZN9__gnu_cxx9__freeresEv`. It sits in plain
  `.ctors`, which means default priority.
* It inits the mutex: `__gthr_win32_mutex_init_function(&emergency_pool)` when `_CRT_MT != 0`.
  The mutex is a CRITICAL_SECTION of 0x28 bytes at offset +0x0.
* It reads `getenv("GLIBCXX_TUNABLES")` and parses `glibcxx.eh_pool.obj_count` and `obj_size`.
* The defaults are obj_count = 256 (capped at 4096) and obj_size = 6 words.
* `arena_size = (obj_size + 38) * obj_count * 8 = 0x16000` (90,112 B). The code is at +0x110..+0x13a and +0x15f.
* It calls `malloc(0x16000)` at +0x16a/+0x172. Layout of the pool object:

  | Offset | Field |
  |:--|:--|
  | +0x28 | `first_free_entry` = arena, with header {size, next = 0} |
  | +0x30 | `arena` |
  | +0x38 | `arena_size` |

* If malloc fails, it sets `arena_size = 0`.
* Finally it registers `atexit(__tcf_0)` as a tail jump at +0x15a.

### 2.2 What the destructor and `__freeres` do

* `__tcf_0` only calls `__gthr_win32_mutex_destroy(&emergency_pool)`, and only when `_CRT_MT` is
  set. It **never frees the arena.** That gives the 0x16000-byte leak per load.
* `__gnu_cxx::__freeres()` (`_ZN9__gnu_cxx9__freeresEv`, global `T`, `.text.unlikely`) does only
  `if (arena) { free(arena); arena = 0; }`.
* It does **not** reset `first_free_entry` (+0x28) or `arena_size` (+0x38).

### 2.3 Behaviour after `__freeres`

* `__cxa_allocate_exception` still tries `malloc` first.
* Only when malloc fails does it call `pool::allocate`. That walks the dangling
  `first_free_entry`, which is a use-after-free.
* `in_pool(p)` becomes true only for `p < 0x16000`, which is never a valid heap block in practice.
* So the release is safe **only** as the last action of the DLL, after every destructor that
  could throw under OOM. This is the requirement §3 satisfies.

### 2.4 Inventory of libstdc++ static constructors

13 of the 189 members have `.ctors`. Tool: `clvk_v2/ctor_calls.py`.

| Member | What its constructor does |
|:--|:--|
| `eh_alloc` | The **only** one that allocates from the heap. |
| `globals_io` | `.ctors.65445`, init_priority 90: `ios_base::Init()` plus `atexit(~Init)`. |
| `future`, `ios_errcat`, `system_error`, `memory_resource` | `atexit` only. |
| `{,w,cxx11-,cxx11-w}locale-inst` | Set `locale::id` guard variables only. |
| `bitmap_allocator`, `parallel_settings`, `tzdb` (`.ctors.65437`, priority 98) | Not linked in the probe. |

The probe map's `.ctors` list:

* plain `.ctors`: user object, cxx11-wlocale-inst, locale-inst, system_error, wlocale-inst,
  eh_alloc, cxx11-locale-inst, ios_errcat;
* `.ctors.65445`: globals_io;
* `.ctors.65535`: `crtend.o` (**priority 0**), whose `register_frame_ctor` calls
  `atexit(__gcc_deregister_frame)`, which is a bare `ret`.

Still to do: lazy allocations on first use in ios_init, locale_init and guard. guard.o's never-deleted
static CRITICAL_SECTION was already noted by the Opus review. I have also not yet dumped the
approved DLL's own `.ctors` list.

## 3. Teardown-order conclusion (from DWARF-annotated installed CRT)

Sources disassembled from the installed binaries:

* mingw-w64 12.0.0-5: `dllcrt2.o` (`crt/crtdll.c`), `libmingw32.a` (`gccmain.o`, `tlssup.o`,
  `tls_atexit.o`, `cxa_atexit.o`) and `libmsvcrt.a(onexit_table.o)`. These are byte-identical to
  `/usr/x86_64-w64-mingw32/lib`.
* GCC `crtbegin.o` and `crtend.o`.

### 3.1 DLL_PROCESS_ATTACH order

1. Loader TLS callbacks run: `__dyn_tls_init` sets `_CRT_MT = 2` and calls `__mingw_TLScallback(1)`.
2. `DllMainCRTStartup` calls `_pei386_runtime_relocator`.
3. `_CRT_INIT(1)` runs:
   * crtdll.c:100 runs `_initterm(__xi_a, __xi_z)`. That includes `pcinit = pre_c_init` in
     `.CRT$XIAA`, which runs `_initialize_onexit_table(&atexit_table)`.
   * crtdll.c:104 runs `_initterm(__xc_a, __xc_z)`.
   * crtdll.c:113 calls `__dyn_tls_init_callback(h, DLL_THREAD_ATTACH)`, which runs `.CRT$XD*`.
4. `DllEntryPoint(1)` runs.
5. crtdll.c:185 calls `__main()`, which runs `__do_global_ctors`:
   * gccmain.c:42-44 walks `.ctors` **backwards**, so priorities run in ascending order: crtend
     (priority 0), then globals_io (priority 90), then default-priority entries in reverse link order.
   * gccmain.c:47 then registers `atexit(__do_global_dtors)`.
6. `DllMain(1)` runs.

### 3.2 DLL_PROCESS_DETACH order (FreeLibrary)

1. The loader runs the TLS callbacks **first**. `__dyn_tls_dtor` calls `__mingw_TLScallback(0)`.
   If `tls_atexit.o` were linked, its `.CRT$XLB` callback would run `__cxa_atexit` destructors here.
2. crtdll.c:186 calls `DllMain(0)`.
3. :195 calls `DllEntryPoint(0)`.
4. :196 calls `_CRT_INIT(0)`. At crtdll.c:130 that runs `_execute_onexit_table(&atexit_table)`,
   which does (onexit_table.c:59-71):
   1. lock;
   2. take the array and reset the table;
   3. unlock;
   4. call the entries **LIFO**, skipping NULL;
   5. `free(array)`.

   A registration made while this table is executing goes into a fresh table. That table never
   runs, and its 256-byte calloc is leaked.

### 3.3 Where static destructors are registered

`atexit` in a DLL is `dllcrt2.o`'s (crtdll.c:206), which calls `_register_onexit_function(&atexit_table, f)`.
g++-win32 14.2 emits `U atexit`, not `__cxa_atexit`, for both namespace-scope and function-local
statics. I verified this with `crt/cxa_probe.cpp`.

### 3.4 Candidate release points

| Candidate | Verdict |
|:--|:--|
| `.dtors` / `__attribute__((destructor(N)))` | Runs **first** at detach, because `__do_global_dtors` is the last registration of the constructor phase. Wrong. |
| `constructor(101)` + atexit, or a C++ `init_priority(101)` static | Registers **after** globals_io (priority 90: `~Init` flushes streams) and crtend (priority 0). So it runs before them and is not last. Priorities 0–100 are reserved (`-Wprio-ctor-dtor`), and even `constructor(0)` ties with crtend's `.ctors.65535`. |
| **Chosen:** a C initializer pointer in `.CRT$XC?`, between cinitexe's `__xc_a` (`.CRT$XCA`) and `__xc_z` (`.CRT$XCZ`) | See below. |

How the chosen mechanism works:

* The initializer runs in `_CRT_INIT` (crtdll.c:104). That is after `pre_c_init` and **before
  any `.ctors`**.
* It calls the DLL-local `atexit(runtime_eh_pool_release)`. That makes it entry 0 of
  `atexit_table`, so it executes **last** in `_execute_onexit_table`. That point is after every
  C++ static destructor, `~Init`, `__tcf_0` and `.dtors`, and the msvcrt heap is still valid there.
* The release calls `__gnu_cxx::__freeres` through a C asm label
  `__asm__("_ZN9__gnu_cxx9__freeresEv")`.
* It needs no DllMain or TLS callback, and it destroys nothing early.
* Pure C is sufficient, so no C++ translation unit is justified.
* Probe map today: `.CRT$XC*` holds only XCA and XCZ, and `.CRT$XI*` holds only XIA, XIAA
  (`pcinit`) and XIZ. Nothing else registers earlier.

### 3.5 Required build gates

Fail closed unless the map shows all of the following:

* `.CRT$XC*` is exactly {cinitexe XCA, the eh-pool object, cinitexe XCZ}.
* `.CRT$XI*` is exactly {XIA, dllcrt2 XIAA, XIZ}.
* The `atexit` definition comes from `dllcrt2.o`.
* None of these are linked: `tls_atexit.o`, `__mingw_cxa_atexit`, `__cxa_atexit`, `__dso_handle`,
  `__cxa_thread_atexit` (libstdc++ `atexit_thread.o`, which pins via GetModuleHandleExW and uses
  `__gthr_win32_key_create`), `__cxa_thread_atexit_impl`.
* `_ZN9__gnu_cxx9__freeresEv` is defined by `libstdc++.a(eh_alloc.o)`.

### 3.6 Planned implementation (not written)

`src/vgpu/clvk_runtime_eh_pool.c` would contain:

* `static void runtime_eh_pool_release(void)`, which calls `__freeres` exactly once.
* `static void runtime_eh_pool_register(void)`, which calls `__fastfail` if `atexit` fails, so
  the process never runs with a silently unregistered release.
* A `__attribute__((section(".CRT$XCAWDL"), used))` initializer pointer.
* A fake-CRT Linux test of the register/release ordering through an injected atexit table,
  checking: registered first, executed last, exactly once, fail-fast on a failed registration.

## 4. Not done (B, C and the rest of A)

* **B**, shim hardening: TEB/vector/block/alignment fail-fast; worst-case arena bound
  `sum(size + max(align, 1) - 1)` with a Python reference; macro-configurable capacities with
  256 B/16-slot defaults (approved inventory: 11 controls, align ≤ 8, 96 B in address order,
  157 B worst case per the Opus review); boundary scenarios that do not inherit slots; gthr
  parity; Doxygen; `TemplateBytes`; the C-vs-Zig rationale. All still to do.
* **C**: coverage, sanitizers and at least 12 mutants. Still to do.

## 5. Native plan (DRAFT, not final)

* **Variants**, built from the strengthened probe once the other worker lands it, using the
  strict flags of `vgpu_dxvk.mk`:
  * POSIX, approved style (`-lwinpthread` whole-archive);
  * Win32 without the shim;
  * Win32 + shim v2 + eh-pool object, with both objects on the link line before the libraries;
* **Common setup and pass criteria**:
  * msvcrt pinned, 4 idle threads alive from before the baseline;
  * handles, free TLS and free FLS must equal the baseline in every cycle;
  * HeapWalk busy bytes and blocks over all `GetProcessHeaps`, referenced after cycle 2, must be
    exact from cycle 3;
  * 2048-cycle stress must end with a natural exit 0.
* **Predictions**:
  * POSIX: at least +0x16000 B per cycle plus winpthreads/emutls blocks, +6 handles, −1 TLS slot.
  * Win32 without shim: −2 TLS slots per cycle and +0x16000 B per cycle.
  * Shim without eh-pool: exactly +0x16000 B (90,112) per cycle.
  * Shim + eh-pool: 0.
* **Loader TLS-retention control**: OS-control DLLs with an 8 B template versus the shim record
  size, K ∈ {0, 4, 16} idle threads. Shrink the record if the per-cycle delta scales with
  K × template.

Exact command lines and HeapWalk code are still to be written.
