# Approval request: unload-neutral C++ runtime for OpenCL.dll (draft for the lead)

## What I'm asking you to approve

1. **Thread model.** Build OpenCL.dll with GCC 14.2's Win32 thread model
   (`x86_64-w64-mingw32-{gcc,g++}-win32 -mthreads -static-libgcc -static-libstdc++`) and
   without winpthreads. This is the same switch withdrawn on 2026-10-17
   (`build/qualification_20261017/rejected_win32_configuration.patch`). It was withdrawn
   only because it leaked 2 TLS slots per unload and stalled after 543 reloads; this
   request removes both causes.
2. **Runtime member replacement.** Link a new Waddle-authored C file,
   `src/vgpu/clvk_runtime_tls.c`, into OpenCL.dll. It replaces two libgcc_eh archive
   members, `emutls.o` and `gthr-win32-thread.o`. Each of those allocates a Windows TLS
   index with TlsAlloc and never frees it, so every load/unload cycle loses two indices.
   The replacement keeps the same ABI but stores per-thread data in OpenCL.dll's own
   loader-managed implicit TLS. As a result:
   * OpenCL.dll owns no TLS index, kernel handle or lock that outlives a thread.
   * It needs no DllMain, TLS-callback or atexit cleanup.
3. **Accept one documented behavior difference.** `std::this_thread::get_id()` on a thread
   that OpenCL.dll did not create returns an opaque identity value instead of a newly
   duplicated thread handle. libgcc leaked that handle. libstdc++ only compares these
   ids by value.
4. **Build-driver gates.** Add fail-closed checks to the build driver, with matching
   tests and CI:
   * the link map must show neither replaced member and no winpthreads;
   * the emulated-TLS inventory must fit 32 objects / 512 B / alignment 16 (today:
     11 / 96 B / 8);
   * TlsAlloc may only be called by `__gthr_win32_key_create`, which pairs with TlsFree.
5. **Decide how the Win32 compiler is provided** (pick one):
   * (a) install the Debian `g++-mingw-w64-x86-64-win32` 14.2.0-19+27+b1 package on the
     host; or
   * (b) keep using the hash-verified private extraction under
     `build/qualification_20261017/mingw_win32_private_1402` on PATH.

   CI installs Ubuntu 24.04's package, which is GCC 13, so CI and the host differ.

Not changed: clvk, LLVM, clspv, any submodule pin, `patches/clvk/0001`–`0003`,
README.md, and the private shutdown ABI.

## Why

Native evidence shows the approved OpenCL.dll (`a8fd9d7a…`) loses 6 kernel handles and
1 TLS slot on every unload. Static analysis of the installed libraries attributes every
one of them:

* **4 semaphores:** winpthreads condition variables created lazily inside two
  `PTHREAD_RWLOCK_INITIALIZER` locks that are never destroyed.
  * One is LLVM PassRegistry's `std::shared_mutex`. libstdc++'s destructor for it is
    defaulted and never calls `pthread_rwlock_destroy`.
  * The other is winpthreads' internal key lock, first taken when libgcc's emulated TLS
    creates its key.
* **1 event and 1 duplicated thread handle:** winpthreads' lazy per-thread state for the
  unloading thread. It is freed only when that thread exits, which happens after the DLL
  is gone.
* **1 TLS index:** winpthreads' own index. Nothing in the DLL ever calls TlsFree.

The Win32 build removes all winpthreads state but leaks the two libgcc indices. At 1086
free indices that allows 543 loads. The 544th load aborts inside libgcc's spin-once.
The detach-time iostream flush then re-enters the same never-completed once and spins
on `Sleep(0)` forever. This matches the observed stall (frozen output, one thread,
rising CPU) but is inferred, not captured natively.

None of these resources can be released safely at FreeLibrary:

* They are private statics inside third-party archives.
* CRT static destructors still use them inside DLL_PROCESS_DETACH, under the loader lock.

Not creating them at all is the only fix that needs no loader-lock cleanup and no early
TLS destruction.

## Exact change

`proposal-runtime-tls-win32.diff` (sha256
`da538045da187c526cf3fc60588265230fc6378a9310992a9ecd7369849bf898`; 8 files,
+1408/−20; `git apply --check` passes against HEAD 52a1a29 and the current working tree):

* `src/vgpu/clvk_runtime_tls.c` (new): the replacement runtime
* `tests/vgpu/clvk_runtime_tls_test.c` and `tests/vgpu/clvk_runtime_tls_fake/windows.h`
  (new): Linux-only Win32 test double
* `scripts/clvk_runtime_tls_coverage.py` (new): gate requiring at least 90% line and
  branch coverage
* `scripts/build_clvk_windows.py`: Win32 compilers, compiling and linking the shim, and
  the post-link gates; the shim's hash goes into the cache key and receipt
* `scripts/vgpu_dxvk.mk`: `build/vgpu_clvk_runtime_win32_tls.dll` (with the map gate)
  added to `vgpu-opencl-runtime-fixture`, plus the `vgpu-clvk-runtime-tls-*` targets
* `tests/vgpu/clvk_build.py`: toolchain tests and 3 new runtime TLS tests
* `.github/workflows/software_vgpu.yml`: installs the win32 package and runs the shim
  gates

Optional: `optional-probe-coverage.diff` strengthens the isolated probe. It adds a
thread_local, an exception and thread-id identity checks.

## Evidence so far (offline only; not acceptance)

* Linux unit tests of the actual shim source pass under ASan, LSan, UBSan and TSan, with
  100% line coverage and 100% branch coverage.
* Strict Win32 cross-compile succeeds. In the linked probe DLL:
  * no replaced libgcc member and no winpthreads appear in the map;
  * CreateSemaphoreA, CreateEventA and DuplicateHandle are not imported;
  * the only TlsAlloc call site is the paired `__gthr_win32_key_create`, and the probe
    cannot reach it.
* The admission suite passes 25 of 25.
* The new driver gates reject the approved DLL and both old probe variants and accept
  the corrected probe.
* Not yet attempted: a full Win32 build of OpenCL.dll, and any native Windows run.

## Risks

* **ABI coupling to GCC 14.2 runtime internals** (the emulated-TLS control layout and the
  gthr-win32 thread functions). Mitigations:
  * the exact compiler versions and hashes are bound into the receipt and cache key;
  * an admission test links the real libgcc members both with and without the shim;
  * the post-link gates fail closed.

  CI uses GCC 13, so the version skew must be watched.
* **Emulated-TLS arena overflow** would end the process via `__fastfail`: an immediate
  exit, never a hang. The build gate prevents shipping a DLL that could overflow. The
  current headroom is about 5× on bytes and about 3× on object count.
* **Win32 thread model behavior:**
  * mutexes are CRITICAL_SECTIONs (recursive misuse no longer deadlocks);
  * `std::call_once` spins rather than blocks if its callable throws (clvk initialization
    could only throw on out-of-memory);
  * libstdc++'s guard mutex for local statics is never deleted (check heap).
* **Memory and OS behavior:** about 600 B more implicit TLS per thread while OpenCL.dll is
  loaded. Whether the Windows loader retains implicit-TLS heap (`LdrpHandleTlsData`) must
  be checked in UMDH.
* **Full build untested:** the complete clvk + LLVM + clspv Win32 build has never been
  done. Compile or link failures are possible and must be fixed before anything is
  qualified.

## What must pass before acceptance (no waivers, no budget changes)

1. **Host:**
   * `make vgpu-clvk-runtime-tls-test vgpu-clvk-runtime-tls-sanitizers vgpu-clvk-runtime-tls-coverage`
   * `python3 -B tests/vgpu/clvk_build.py` (25 tests OK)
   * `make vgpu-opencl-runtime-fixture`
   * `make vgpu-opencl-build`, with the receipt showing `runtime_tls` within limits and
     no `libwinpthread` import
2. **Guest, isolated probe:**
   * `vgpu_clvk_runtime_test.exe …\vgpu_clvk_runtime_win32_tls.dll` exits 0 after 8
     cycles with exact handles, free TLS equal to baseline, and module and export absent
     within 100×10 ms.
   * `vgpu_clvk_runtime_stress.exe` with the same DLL completes all 2048 cycles and
     exits 0 on its own, with no forced retirement.
   * The old POSIX and Win32 DLLs still fail on the same image. This proves the test
     still detects the leaks.
3. **Guest, real OpenCL.dll:** the full existing qualification passes. That means native
   exit 0, a constant post-unload handle count with no OpenCL-image creators, zero UMDH
   growth from the second to the third unload, and **new** exact free-TLS equality
   checked around every unload.

## Rollback

Revert the commit or commits.

* The approved DLL `a8fd9d7a…` and its receipt remain in `build/clvk_client/`.
* The cache key changes with the thread model and the shim hash, so builds cannot mix.
* Nothing in the submodules or `patches/clvk/` changes, so there is nothing else to undo.
