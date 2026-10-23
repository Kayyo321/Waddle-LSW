# OpenCL (clvk) runtime-unload lifetime: root cause and approved correction path, 2026-10-21

Status: root cause established; isolated native proof of the mechanism obtained; the correction is
**approved by the user in principle (v2, hardened, re-review required)** and is **not integrated**.
Nothing here is native OpenCL qualification, heap acceptance or physical-GPU acceptance. Feature progress
stays 85.5% and the OpenCL runtime-unload defect remains open until the v2 correction passes the
gates listed in [CHECKPOINT_2026_10_21.md](CHECKPOINT_2026_10_21.md).

Provenance. The body below is the report of an independent read-only Opus analysis (offline: installed
toolchain binaries, DWARF line tables, headers, a PE call-site scanner over the approved DLL). Its static
attributions are **claims until natively confirmed**. The lead's native confirmations on the Windows 11 guest
(build 26300, interactive desktop, medium integrity, 2026-10-21) are in the table below and in
[RUNTIME_CANDIDATE_RECEIPT_2026_10_21.json](RUNTIME_CANDIDATE_RECEIPT_2026_10_21.json). Two independent
reviews of the proposed correction are in
[CLVK_RUNTIME_PROPOSAL_REVIEWS_2026_10_21.md](CLVK_RUNTIME_PROPOSAL_REVIEWS_2026_10_21.md); they found the mechanism
sound but the v1 proposal **not yet zero-leak** (an unfreed libstdc++ emergency-exception pool of 0x16000 bytes
per load, unmeasured loader retention of the larger per-thread TLS block, fail-open build gates).

## Lead's native confirmation (isolated probe, unchanged oracle)

| Variant (isolated probe DLL) | Exit | Handles per cycle | Free TLS slots |
|:--|:-:|:--|:--|
| approved POSIX model (control) | 1 | 47 -> 53 | 1086 -> 1085 |
| Win32 model, no shim (control) | 1 | 47 -> 47 | 1086 -> 1084 |
| Win32 model, no shim, extended probe (control) | 1 | 47 -> 48 (duplicated thread handle) | 1086 -> 1084 |
| Win32 model + v1 shim | 0 | 53 -> 53, 8 cycles | 1086 every cycle |
| Win32 model + v1 shim, 2048-cycle stress | 0 (natural) | 2048 cycle lines, 0 nonconforming | 1086 every cycle |
| Win32 model + v1 shim, extended probe (thread_local init, caught exception, thread::id) 8 and 2048 cycles | 0 | 47 -> 47 | 1086 every cycle |

Harness: the receipt-bound original `vgpu_clvk_runtime_test.exe` / `vgpu_clvk_runtime_stress.exe`
(734b6bf9... / 0fda46d3...). The 2048-cycle run is far beyond the 543-reload stall horizon of the Win32 control.
**Limits:** the oracle does not see heap (the harness is UCRT while the DLL uses msvcrt.dll, which probably unloads with the
DLL), FLS, pre-existing threads or loader TLS retention, so it cannot see the 0x16000-byte per-load emergency pool leak
that review found. Those oracles are the first work item of v2.

A full candidate `OpenCL.dll` (Win32 model + v1 shim) was also **built, never executed**: `make vgpu-opencl-build`
exit 0 in an isolated clone (31 min), 121 exports, no winpthreads import, exactly one paired TlsAlloc site, TLS template
608 bytes against 8 bytes in the approved DLL. See the receipt.

---

# OpenCL (clvk) Windows runtime-unload lifetime defect: root cause and correction

Analyst report for the lead. Repository `/home/dev/Waddle-LSW`, branch
`feature/software-vgpu-slicing`, HEAD `52a1a2970c2a182c155c718432a9dee520b0ca72`.
Approved DLL `a8fd9d7a4f979ca4534e26d789025cf9f7c7affd84b46726eeee28f40d83c54d`.

Limits of this work: offline only. No VM, QEMU, GPU or guest command was run. No
tracked repository file, submodule, `patches/clvk/` file or README was modified, no
commit was made, no host package was installed. Everything was built in
`.../scratchpad/clvk_runtime/` (called `$S` below). Wine is not installed and was not
used. Nothing here is native acceptance evidence.

Observation, not mine: during this session `tests/vgpu/tcp_gpu_present_windows.inc`,
`tests/vgpu/tcp_gpu_windows.c` and `tests/vgpu/tcp_gpu_wsi_lifetime_windows.inc` became
modified in the working tree (mtimes 21:57 and 22:05 EDT). Later, the untracked files
`tests/vgpu/tcp_gpu_handle_diag_windows.inc` and
`tests/vgpu/win32_fake/handle_diag_fake.h` appeared too. I did not write any of them.
Presumably they are the lead's concurrent edits.

## 0. Evidence method

The installed toolchain contains no runtime sources, so "source" proof uses the exact
installed binaries:

* `libwinpthread.a` (mingw-w64 12.0.0-5) ships DWARF line tables
  (`/build/./mingw-w64-libraries/winpthreads/src/*.c`). `objdump -d -l -r` maps every call
  to a `file:line` of the exact Debian build. The line numbers below are those DWARF
  lines, not GitHub v12 lines.
* `libmingw32.a` and `dllcrt2.o` (12.0.0-5) also carry DWARF (`crt/crtdll.c`,
  `crt/tlssup.c`, `crt/tlsthrd.c`).
* libgcc (`14-posix/libgcc_eh.a(emutls.o)`, `14-win32/libgcc_eh.a(emutls.o)`,
  `14-win32/libgcc_eh.a(gthr-win32-thread.o)`) has no debug info, so it is cited by
  disassembly offset. With `-static-libgcc`, emutls and gthr-win32-thread come from
  **libgcc_eh.a**, not libgcc.a. The link maps confirm this.
* Header behavior comes from the installed headers, cited with their line numbers.
* For the real DLL I wrote a read-only PE call-site scanner
  (`$S/tools/iat_xref.py`, `$S/tools/direct_xref.py`). It runs over the approved
  OpenCL.dll's COFF symbols (627,977 symbols) and finds which function references each
  TlsAlloc, CreateSemaphoreA, CreateEventA and DuplicateHandle import slot.
* I rebuilt the probe DLLs exactly as `scripts/vgpu_dxvk.mk` does. Their `.text` is
  byte-identical to the receipt-bound `build/vgpu_clvk_runtime_{posix,win32}.dll`.
  `.data`/`.rdata` differ only in non-code bytes.

## 1. Root cause per resource

### 1.1 POSIX thread model with static whole-archive winpthreads (approved DLL and probe)

Measured: 53→59 handles and 1086→1085 free TLS slots on the first unload. The handle
trace has four type-24, one event and one thread reference.

| # | Resource | Who allocates (exact) | Owner | Why nothing frees it at FreeLibrary |
|:-:|:--|:--|:--|:--|
| A1, A2 | 2 semaphores | `cond.c:214` and `cond.c:218` `CreateSemaphoreA` (`sema_q`, `sema_b`) in `pthread_cond_init`, called from `rwlock.c:196` `pthread_rwlock_init`, from `rwlock.c:166` `rwlock_static_init`, from `rwlock.c:429` (first `pthread_rwlock_wrlock` of a `PTHREAD_RWLOCK_INITIALIZER` lock). The caller is libstdc++ `std::shared_mutex` → `llvm::sys::SmartRWMutex` (`llvm/include/llvm/Support/RWMutex.h:101`, `std::shared_mutex impl`; `LLVM_USE_RW_MUTEX_IMPL` is set only for `__APPLE__`, `RWMutex.h:22-24`) → `PassRegistry::Lock` (`PassRegistry.h:39`) inside the function-local static (`PassRegistry.cpp:23-24`). The probe uses the namespace-scope `pass_registry_mutex` instead. | The static `std::shared_mutex` object | `14-posix/include/c++/shared_mutex:161-166`: on the `PTHREAD_RWLOCK_INITIALIZER` branch the destructor is `= default`, so `pthread_rwlock_destroy` is never called. This is harmless on glibc but leaks the lazily created cond on winpthreads. The approved DLL has **zero** call sites of `pthread_rwlock_destroy`. `llvm_shutdown()` cannot help because the static is not a ManagedStatic and the leak is in the header's destructor. |
| B1, B2 | 2 semaphores | Same `cond.c:214/218` path, but through winpthreads' own static `_pthread_key_lock` (an rwlock with the static initializer in thread.o `.data`), write-locked by `pthread_key_create` (`thread.c:805`). That is called by libgcc `emutls_init` (14-posix `libgcc_eh.a(emutls.o)` `.text+0x70`; in the approved DLL it is the only `pthread_key_create` call site besides libstdc++'s at-thread-exit `key_init`). emutls_init runs on the first `__emutls_get_address`, which comes from `std::call_once` (`__once_callable` is emutls because `c++config.h:1394` has `_GLIBCXX_HAVE_TLS`) in clvk `init.cpp:339` and in the probe's `call_once`. | winpthreads library-global | No code destroys `_pthread_key_lock`. libgcc emutls never deletes its key: emutls.o imports `pthread_key_create` but not `pthread_key_delete`. |
| C | 1 event | `thread.c:1038` `CreateEventA` (`t->evStart`) in `__pthread_self_lite` (`thread.c:1019-1053`) for a thread that winpthreads did not create. It is reached through `pthread_getspecific` (`thread.c:897-906`) from `__emutls_get_address`. In the real DLL the trace shows it on the unloading thread during `ios_base::Init::~Init` → flush → `uncaught_exception` → `__cxa_get_globals` → emutls, run from CRT onexit (`crt/crtdll.c:130` `_execute_onexit_table`). | That thread's `_pthread_v`, stored in winpthreads' TLS slot | It is closed only on `DLL_THREAD_DETACH` (`thread.c:455-493` closes `h`/`evStart`). The unloading thread outlives the DLL. `DLL_PROCESS_DETACH` (`thread.c:433-440`) only removes a vectored exception handler. |
| D | 1 thread handle | `thread.c:1043` `DuplicateHandle(GetCurrentThread)` → `t->h`, same function | Same | Same |
| T | 1 TLS index | `thread.c:962` `TlsAlloc` in `pthread_tls_init` (run once from `_pthread_once_raw`); abort on failure at `thread.c:965` | winpthreads `_pthread_tls` | `libwinpthread.a(thread.o)` does not import `TlsFree` at all. The approved OpenCL.dll imports TlsAlloc but **not TlsFree**, and its only TlsAlloc call sites are `pthread_tls_init` and its inlined copy in `_pthread_once_raw`. |

The six handles and one slot are exactly the creators that the static inventory shows
in both the probe and the real DLL. Each POSIX load also retains heap the same way:

* `rwlock.c:180` and `cond.c:205` (calloc)
* `mutex.c:62` (`mutex_impl_init` malloc) for every used `std::mutex`, because
  `std_mutex.h:64-65` (`__GTHREAD_MUTEX_INIT`) never calls `pthread_mutex_destroy`
* `thread.c:341` (`_pthread_v`) and `thread.c:849` (key arrays)
* the emutls arrays

UMDH reported zero growth between unloaded2 and unloaded3, which contradicts this. See §7.

### 1.2 Win32 thread model (`-mthreads`, static libgcc/libstdc++; candidate and probe)

Measured: 47→47 handles and 1086→1084 free TLS slots.

| # | Resource | Who allocates (exact) | Why it is never freed |
|:-:|:--|:--|:--|
| T1 | TLS index | `14-win32/libgcc_eh.a(emutls.o)` `__emutls_get_address`+0x296 `TlsAlloc`. This is the inlined `emutls_init`/`__gthread_key_create` (`gthr-default.h:674-693`), which registers `emutls_destroy` through `__mingwthr_key_dtor` (+0x2b4). | emutls.o imports no TlsFree. The key has process lifetime by design. |
| T2 | TLS index | `14-win32/libgcc_eh.a(gthr-win32-thread.o)` `__gthr_win32_create`+0xfa `TlsAlloc` (`__gthr_win32_tls`, once). The same index is also reached from `__gthr_win32_self`+0x2ea. It runs on every `std::thread` start. | The member imports no TlsFree. |
| (latent) | Thread handle | `__gthr_win32_self`+0x2b0 `DuplicateHandle` for a thread not created by gthreads (`std::this_thread::get_id()`), registered with the `free` destructor only (+0x304) | The handle is never closed, not even at thread exit. The 8-cycle probe does not exercise this, but the optional extended probe does (§6.1). |

Inventory of the rebuilt Win32 probe: the TlsAlloc call sites are `__emutls_get_address`,
`__gthr_win32_create` and `__gthr_win32_self` (all without TlsFree), plus
`__gthr_win32_key_create`, which pairs with `__gthr_win32_key_delete` → TlsFree. The probe
executes `call_once` (emutls) and `std::thread` (gthr create), which gives exactly 2
slots per load and matches 1086→1084. Handles balance because CRITICAL_SECTION (the
mutex type, `gthr-default.h:370-371`) and CONDITION_VARIABLE hold no per-object handle,
and the CreateThread handle is closed by join.

There is also a latent ordering hazard here (not proven either way):
`__mingw_TLScallback(DLL_PROCESS_DETACH)` (`crt/tlsthrd.c:133-146`, through the
`.CRT$XLD` callback `crt/tlssup.c:138-169`) runs `emutls_destroy` for the unloading
thread. That frees its emutls array but does not clear the TLS value. If Windows runs
TLS callbacks before the DLL entry point on detach, the later onexit destructors
(`crtdll.c:130`) read freed memory.

### 1.3 Why loader-lock-safe cleanup is not a viable fix

* Every leaked object is owned by a **file-static of a third-party archive**: `_pthread_tls`,
  `_pthread_key_lock`, `emutls_key` and `__gthr_win32_tls`. A public API does not expose
  them, so a Waddle shim cannot release them without patching or replacing those
  libraries.
* They are still used after the last point we control: the CRT onexit destructors run
  *inside* `DLL_PROCESS_DETACH` under the loader lock (`crtdll.c:186,195,196` call
  DllMain, then DllEntryPoint, then `_CRT_INIT`, which runs `_execute_onexit_table`).
  Releasing earlier is "early TLS destruction". Releasing later means extra work under
  the loader lock whose position relative to TLS callbacks cannot be controlled from
  the DLL.
* `TlsFree` and `CloseHandle` on objects the DLL owns are loader-lock-safe calls in
  themselves (Microsoft's TLS DLL sample does exactly this). The objection is the
  ordering and library patching, not the API calls.
* Conclusion: the correct fix is to **never create** process-lifetime runtime resources
  in a DLL that is unloaded, not to clean them up later.

## 2. The 543-reload stall (Win32 handle-only stress)

**Proven** from the installed binaries:

1. Each Win32 load leaks exactly two TlsAlloc indices (the two members above), and the
   measured baseline was 1086 free slots. 2 × 543 = 1086, so the 544th load starts with
   zero free indices.
2. emutls's once is a spin-once (`gthr-default.h:639-668`, inlined in emutls.o):
   `lock add started` → the first caller calls TlsAlloc (+0x296). On
   `TLS_OUT_OF_INDEXES` it calls GetLastError (≠0) → `abort()` (+0x337) **before**
   `done = 1` (+0x2bd). Every later emutls access in the same DLL instance takes the
   non-first path: `while (!done) Sleep(0);` (+0x320..+0x328), which spins forever on
   one CPU.
3. A later emutls access is guaranteed during process exit. The harness EXEs are UCRT
   (`api-ms-win-crt-*`) while the DLL uses msvcrt, and DLL_PROCESS_DETACH runs the
   DLL's onexit table (`crtdll.c:130`). That table contains libstdc++
   `ios_base::Init::~Init` → flush (`cerr` is unitbuf) → `uncaught_exception` →
   `__cxa_get_globals`. In the Win32 `libstdc++.a`, `eh_globals.o` references
   `__emutls_get_address`.

**Inference** (not captured natively): msvcrt's `abort()` ended in `_exit(3)` →
ExitProcess. That kills the other threads, then LdrShutdownProcess runs the probe's
detach under the loader lock, which hits step 3, so the once-spin never ends. This
matches all three observations: frozen output (abort before the CYCLE line), one live
thread, and rising CPU (Sleep(0) spin).

Alternatives considered and judged worse fits:

* A `__fastfail`/WER path would have terminated the process.
* An `abort` MessageBox would idle rather than burn CPU.
* Heap corruption from the hazard in §1.2 would not land exactly on 543.

To confirm natively, capture `~*k` with cdb or procdump from a deliberately exhausted
Win32 run (the extended probe makes this reproducible at cycle 1 if TLS is pre-exhausted
after preloading msvcrt.dll).

The POSIX build would exhaust after about 1086 loads. Its failure path is different:
`thread.c:965` calls abort while `_pthread_once_raw` holds its once mutex. That would
most likely end in a blocked self-deadlock rather than a spin (inference). In practice
the POSIX build fails the handle oracle at cycle 1 anyway.

## 3. Candidate corrections, ranked

| Rank | Correction | Layer touched | Human approval | Main risks | How the existing regression shows success |
|:-:|:--|:--|:--|:--|:--|
| **1 (recommended)** | Win32 thread model (removes winpthreads and therefore all six handles) **plus** a Waddle-authored C object `src/vgpu/clvk_runtime_tls.c` linked ahead of libgcc_eh. It defines the complete symbol set of `emutls.o` and `gthr-win32-thread.o`, backed by the DLL's own **loader-managed implicit TLS** (`.tls$WDL` record), so OpenCL.dll calls TlsAlloc nowhere it does not pair with TlsFree. No DllMain, TLS-callback or atexit code. No heap per thread. | Build config, plus one Waddle C object, plus driver post-link gates. **No** clvk/LLVM/clspv source or pin change; no `patches/clvk/0004`. | **Yes.** It replaces toolchain runtime members inside the shipped DLL and changes the thread model, which was previously rejected. Treat it with section-4 rigor even though it is not an upstream patch. | ABI coupling to GCC 14.2 internals; fail-fast if the emutls arena overflows; about 600 B more implicit-TLS template per thread; Win32-model behavior differences (§5.4); the full OpenCL.dll Win32 build has never been attempted. | `vgpu_clvk_runtime_test.exe` against `vgpu_clvk_runtime_win32_tls.dll`: exit 0, every cycle exact handles, `free_tls` equal to baseline, module and export absent within 100×10 ms. Stress: 2048 cycles end naturally with exit 0. |
| 2 | Win32 model plus a **private copy of libgcc** emutls/gthr with TlsFree at "last" detach | GCC source submodule plus patch | Yes, section 4 | Loader-lock cleanup, order dependence on TLS callback vs entry point, vendoring GCC | Same oracle, but correctness depends on loader order |
| 3 | POSIX model plus a **private winpthreads** (mingw-w64 v12 submodule) with process-detach `TlsFree(_pthread_tls)`, `_pthread_key_lock` destroy and closing the calling thread's `_pthread_v`; plus a libstdc++ `shared_mutex`/`mutex` destroy override or an LLVM `RWMutex` patch; plus an emutls key delete | Three third-party layers | Yes, section 4 | Many loader-lock actions; winpthreads state is still used by onexit destructors after its TLS callback; other `std::shared_mutex` users leak; heap from `std::mutex` remains | Same oracle |
| 4 | **Pin OpenCL.dll** (`GetModuleHandleExW(..._PIN)`) | One clvk/shim call | Human architecture decision | The leak becomes O(1) per process. The module never unloads, so the module-absence oracle fails by design. Conflicts with 0001's sticky terminal shutdown: a re-LoadLibrary returns the terminal instance, so `CL_INVALID_PLATFORM` until 0001 allows re-arming. | The acceptance criteria would have to change. |
| 5 | **Helper process** per OpenCL lifetime or session | Architecture | Human decision | Process exit reclaims everything. Needs API remoting or a different embedding contract; latency and IPC. | A different test model |
| Rejected | `llvm_shutdown()` | — | — | Does not touch the function-local static, winpthreads or emutls | — |
| Rejected | Dynamic `libwinpthread-1.dll` | Build | — | The emutls key destructor would dangle into the unloaded OpenCL.dll and crash at a later thread exit; the key table grows per reload; the import is forbidden; the header leak (A) remains | — |
| Rejected | Native TLS or `-f...` flags | Build | — | GCC 14.2 mingw has no native TLS: `-ftls-model=local-exec` still emits `__emutls_v`/`__emutls_get_address`, and the prebuilt libstdc++.a uses emutls | — |

## 4. Recommended correction (concrete)

### 4.1 Mechanism

* **Thread model.** Use the GCC 14.2 `x86_64-w64-mingw32-{gcc,g++}-win32` compilers with
  `-mthreads -static-libgcc -static-libstdc++` and no winpthreads. This is the rejected
  `build/qualification_20261017/rejected_win32_configuration.patch`, which applies
  cleanly to HEAD. It removes every winpthreads creator: A, B, C, D and T.
* **Emulated TLS.** `__emutls_get_address` and `__emutls_register_common` follow the exact
  libgcc ABI (`struct __emutls_object` = `{size, align, loc, templ}`, verified against
  the control objects in OpenCL.dll). Storage lives in a per-thread
  `runtime_tls_record_t` placed in section `.tls$WDL`, which falls between mingw's
  `_tls_start` (`.tls`) and `_tls_end` (`.tls$ZZZ`) in GNU ld's PE script.
  * The record is located the same way MSVC `__declspec(thread)` code is:
    `gs:[0x58]` → `ThreadLocalStoragePointer[_tls_index]` + offset from `_tls_start`.
  * Slots are laid out process-wide on first use under a statically initialized
    SRWLOCK and published with a release store. Templates are copied on a thread's
    first access.
  * The loader zero-fills, allocates and frees the record for every thread, and frees
    the implicit index at unload. The approved DLL **already** has an implicit TLS
    directory (8-byte template; callbacks `__dyn_tls_init`, `__dyn_tls_dtor`,
    `__dyn_tls_pthread`), so this adds no new kind of resource.
  * On arena or slot overflow it calls `__fastfail`, which is immediate termination
    with no detach code and therefore no hang.
* **gthreads.**
  * `__gthr_win32_create` starts a CREATE_SUSPENDED thread. The thread copies and frees
    the malloc start block **before** user code, then stores the borrowed creation
    handle in its record.
  * `__gthr_win32_join` matches libgcc's disassembled semantics, including returning 1
    without closing on a self-join.
  * `__gthr_win32_self` returns the creation handle for gthreads. For other threads it
    returns `record | 1`: unique among live threads, stable, never a valid handle,
    owning nothing. libstdc++ compares `thread::id` only by value
    (`std_thread.h:333-339`) and never passes a foreign id to `GetThreadId`, join or
    detach.
* **Build driver post-link gates (fail closed).**
  * The link map must contain `clvk_runtime_tls.o` and none of
    `libgcc_eh.a(emutls.o)`, `libgcc_eh.a(gthr-win32-thread.o)`, `libwinpthread.a(` or
    `libpthread.a(`.
  * The emutls inventory from COFF symbols and raw control objects must be ≤ 32
    objects, ≤ 512 B and alignment ≤ 16. The current OpenCL.dll has 11 objects, 96 B and
    alignment 8.
  * Every TlsAlloc call site must be in `{__gthr_win32_key_create}`, the libstdc++
    keyed facilities whose static destructor calls TlsFree.
  * Results go into the receipt and cache key (`runtime_tls_sha256`, capacity, map hash).

### 4.2 Diff (not applied; `git apply --check` passes against HEAD and the working tree)

* `$S/proposal-runtime-tls-win32.diff` (sha256 `da538045…f898`, 8 files, +1408/-20):
  * `src/vgpu/clvk_runtime_tls.c` (new, Doxygen on all symbols)
  * `tests/vgpu/clvk_runtime_tls_test.c` and `tests/vgpu/clvk_runtime_tls_fake/windows.h`
    (Linux-only Win32 double)
  * `scripts/clvk_runtime_tls_coverage.py`
  * `scripts/build_clvk_windows.py`: the rejected Win32 switch, plus
    `compile_runtime_tls`, `verify_runtime_link`, `read_image`, `image_offset` and
    `audit_runtime_tls`
  * `scripts/vgpu_dxvk.mk`: new `build/vgpu_clvk_runtime_win32_tls.dll` with a map gate
    added to `vgpu-opencl-runtime-fixture`, and `vgpu-clvk-runtime-tls-{test,sanitizers,coverage}`
  * `tests/vgpu/clvk_build.py`: the rejected patch's toolchain tests updated, plus three
    runtime TLS tests
  * `.github/workflows/software_vgpu.yml`: win32 package and the shim gates
* `$S/optional-probe-coverage.diff`: extends `clvk_runtime_windows.cpp` with an
  initialized `thread_local` on two threads, a caught exception and `thread::id`
  identity. With it the baseline Win32 DLL also leaks a duplicated thread handle per
  load (the latent row in §1.2), which makes the oracle discriminate more sharply.
  Compiles strict for all three variants.
* The plain source files are also under `$S/proposal/`.

### 4.3 Proof obtained without Windows

* **Linux unit tests** of the actual shim source:
  * Plain, ASan+LSan+UBSan and TSan all report `CLVK_RUNTIME_TLS_TEST PASS`, exit 0,
    zero leaks, repeated 5 times.
  * The gcov gate gives **100% lines (93/93) and 100% branches (46/46)** of the
    fake-mode production code.
  * Covered: layout, alignment, template and zero init, per-thread isolation, racing
    first use, register_common rules, six fail-fast paths in forked children,
    create/join/self including malloc, CreateThread, ResumeThread, Wait and ExitCode
    failures, and fake handle/allocation leak counters.
* **Strict cross-compile.** The shim builds with `gcc-win32 -std=c11 -Wall -Wextra
  -Wpedantic -Werror`. GCC 14 misreports mingw's `__readgsqword` as an array-bounds
  error, so the shim uses one `movq %gs:0x58` asm, which is identical codegen.
* **Linked probe `vgpu_clvk_runtime_win32_tls.dll`** (sha256 `1ed28683…bef5`):
  * The map shows neither libgcc member and no winpthreads.
  * Imports are KERNEL32 and msvcrt only. CreateSemaphoreA, CreateEventA and
    DuplicateHandle are **not imported**.
  * The only TlsAlloc call site is `__gthr_win32_key_create`. Its single caller is
    `std::(anon)::key_init` (at-thread-exit), which has no direct caller in the probe.
    It pairs with `__tcf_0` → `__gthr_win32_key_delete` → TlsFree.
  * `__emutls_get_address`, `__once_proxy`, `__cxa_get_globals` and
    `__gthr_win32_create`/`join` resolve to the shim.
  * The TLS directory template is 608 B, with `runtime_tls_template` at
    `_tls_start + 0x20` inside [start, end).
  * The disassembly shows `mov %gs:0x58` → `[vector + _tls_index*8]` + 0x20.
* **Driver gates checked against real artifacts.**
  * The approved OpenCL.dll fails closed with "unpaired TlsAlloc call sites:
    `_pthread_once_raw…`, `pthread_tls_init`".
  * The POSIX and Win32 probes fail closed.
  * The shim probe passes.
* **Full admission suite** in a read-only overlay (symlinked submodules): **25/25 OK**.
  That is the 19 original tests, 3 rejected-toolchain tests and 3 new ones, run with
  the private SDK on PATH. Afterwards the submodules and the working tree outside the
  tcp_gpu files are unchanged.
* **Not done:** a full OpenCL.dll Win32 build. It would need several GB of tmpfs RAM
  and long CPU contention while the lead may be running timed VM jobs.

### 4.4 Residual risks the native runs must cover

* **Win32-model semantics.**
  * Mutexes are CRITICAL_SECTIONs, so recursive misuse no longer deadlocks.
  * `call_once` is a spin-once: if the callable throws (clvk `init_global_state` under
    OOM), later callers spin; on POSIX they block.
  * The `guard.cc` static recursive mutex is never deleted. Check UMDH for CS
    debug-info heap.
  * A never-destroyed CS that is contended could own an event. The real-DLL handle
    oracle catches that.
* **OS loader behavior.** Check UMDH for the implicit-TLS heap (`ntdll!LdrpHandleTlsData`
  and `LdrpAllocateTlsEntry` stacks). The per-thread block grows from 8 to 608 B. The
  DXVK review shows ntdll can retain per-thread TLS vectors when other threads exist at
  load time.
* **Toolchain coupling.** CI's ubuntu-24.04 `g++-mingw-w64-x86-64-win32` is GCC 13, not
  the local 14.2. The admission test links real libgcc members, so CI would reveal
  drift, but the production cache key and receipt bind 14.2 only.
* **Provisioning.** The host has the win32 compiler only as the private extraction.
  Production builds need that PATH or an approved package install.

## 5. Native proof the lead must run (in the VM, medium integrity, owned processes)

Build on the host after applying the diff on a branch:

```
export PATH=$PWD/build/qualification_20261017/mingw_win32_private_1402/extracted/usr/bin:$PATH
make vgpu-clvk-runtime-tls-test vgpu-clvk-runtime-tls-sanitizers vgpu-clvk-runtime-tls-coverage
python3 -B tests/vgpu/clvk_build.py            # expect Ran 25 ... OK
make vgpu-opencl-runtime-fixture               # posix, win32, win32_tls DLLs, test/stress EXEs; map gate
sha256sum build/vgpu_clvk_runtime_*.dll build/vgpu_clvk_runtime_{test,stress}.exe
```

### Stage 1: isolated probe (minutes)

Copy the hash-checked artifacts to the guest.

1. Run `vgpu_clvk_runtime_test.exe <dir>\vgpu_clvk_runtime_win32_tls.dll`. **Pass when:**
   * native exit 0 and the line `RUNTIME_LIFETIME_PROBE PASS cycles=8`;
   * every `CYCLE` line has `probe_result=0 exact_handles=1 module_absent=1
     symbol_absent=1 tls_balanced=1`;
   * `free_tls` equals BASELINE (expected 1086 on this image) in every cycle;
   * the handle count returns to baseline within the unchanged 100×10 ms budget.
2. Run `vgpu_clvk_runtime_stress.exe <dir>\vgpu_clvk_runtime_win32_tls.dll`. **Pass when:**
   * `PASS cycles=2048` with **natural** exit 0 and no forced retirement;
   * every cycle has `tls_balanced=1`. 2048 cycles is beyond both exhaustion horizons
     (543 for Win32, about 1086 for POSIX), and TLS is checked exactly on every cycle.
3. Controls on the same image, unchanged oracle:
   * `posix.dll` must still exit 1 at cycle 1 with handles 53→59 and TLS 1086→1085.
   * `win32.dll` must exit 1 at cycle 1 with free_tls 1084.
   * If `optional-probe-coverage.diff` is adopted, the extended `win32.dll` must also
     show a +1 handle while `win32_tls` still passes.

### Stage 2: real OpenCL.dll

1. `make vgpu-opencl-build` (same PATH). **Pass when** the new receipt has:
   * `settings.windows_thread_model=win32`;
   * `runtime_tls = {emutls_objects ≤32, emutls_bytes ≤512, emutls_max_align ≤16,
     tls_alloc_sites ⊆ [__gthr_win32_key_create]}`;
   * imports without `libwinpthread`;
   * the exact 121 exports.
2. Rerun the full approved native OpenCL qualification with the new DLL: exact kernels,
   malformed inputs, concurrent callers, `waddle_clvk_shutdown`, module retirement, and
   the 3-cycle UMDH plus handle-trace audit. **Pass when:**
   * native exit 0;
   * the post-unload process handle count is constant across cycles (today it is
     142→148→154; it must become equal) with no retained handle whose creator stack is
     in the OpenCL image;
   * UMDH unloaded2→3 has zero positive groups;
   * there are no `LdrpHandleTlsData` or CS-debug-info growth groups.
3. **New and required.** The real fixture has never measured TLS capacity. Add the
   probe's `count_free_tls_slots` before the first load and after each unload (test
   owner change). Require exact equality. Optionally add a light reload loop of at least
   1100 cycles (load, `clGetPlatformIDs`, `waddle_clvk_shutdown`, FreeLibrary) with the
   same handle and TLS oracle.
4. Prior executions of `a8fd9d7a…` do not qualify the rebuilt DLL. Rerun every affected
   admission, sanitizer, coverage, kernel, concurrency, shutdown and heap gate.

## 6. Answers to specific questions

* **Is `llvm_shutdown()` a fix?** No. See A1/A2 and §3.
* **Can the 543 stall happen with the correction?** Not by the same mechanism. The shim
  has no once-function and never calls TlsAlloc. The only TlsAlloc left in the DLL is
  `__gthr_win32_key_create`, which pairs with TlsFree and is not reached by the probe.
  The shim's only failure path is `__fastfail` on arena overflow, which terminates
  without running detach code, so it cannot spin. Native proof is still pending
  (§5, stage 1, step 2).
* **Does the correction need a `patches/clvk/0004`?** No. No third-party source changes.

## 7. Open items for the lead

1. **UMDH contradiction.** Source predicts per-load POSIX heap retention (winpthreads
   impl blocks, `_pthread_v`, emutls arrays), but the receipt reports zero unloaded2→3
   growth. Before relying on heap evidence, check whether the audited heaps include
   msvcrt's malloc heap and whether stack-group "rebasing" merged these allocations.
2. The real-DLL TLS oracle is missing (§5, stage 2, item 3).
3. Decide on toolchain provisioning (§4.4) and the CI GCC 13 skew.
4. AGENTS workflow: commits, tracker updates and the IMPL_DESC/TRACKER entry for the
   new Waddle C file and any submodule-free runtime replacement belong to the lead.

## 8. Artifacts (`$S` = `.../scratchpad/clvk_runtime`)

* `proposal-runtime-tls-win32.diff`: recommended correction
* `optional-probe-coverage.diff`: optional stronger probe
* `approval_request.md`: packet for the human
* `proposal/`: plain sources of the new files
* `repo_copy/`: scratch git copy with the diff applied (`build/` has the linked
  `vgpu_clvk_runtime_win32_tls.dll`, its `.map` and test/stress EXEs)
* The 25-test run used a symlinked overlay (since deleted). To recreate it, symlink `submodules/`, `patches/`, `include/` and the unchanged scripts and tests next to the copies in `repo_copy`.
* `build/probe/`: posix, win32 and win32_tls probe DLLs with `.map` and `.nm`
* `build/cov/`: gcov output
* `dis/`: annotated disassembly with DWARF lines of every cited winpthreads and
  mingw-crt object, plus libgcc emutls/gthr
* `tools/`: read-only PE scanners (`iat_xref.py`, `direct_xref.py`,
  `emutls_inventory.py`, `tls_directory.py`)
* `opencl_nm.txt`, `opencl_p.txt`: COFF symbols and headers of the approved DLL
