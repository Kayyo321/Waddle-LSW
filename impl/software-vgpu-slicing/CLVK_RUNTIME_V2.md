# OpenCL runtime lifetime correction v2

Status: implementation in progress; not integrated into OpenCL.dll and no new native acceptance. Feature remains 85.5%, Time Ended TBD. This Waddle-authored correction follows the recorded Win32-runtime-shim approach; clvk/LLVM/clspv sources, pins and approved private patches remain untouched. No host package installation or README edit.

## Scope and language

Replace the complete external ABI of libgcc14.2 Win32 emutls.o and gthr-win32-thread.o with a Waddle C object before those archives are linked. The ABI, COFF TLS sections, Windows thread routines and TEB accessor require a C object compatible with the pinned GCC runtime; no C++ runtime implementation or managed language is introduced. The bounded internal arena is not an untrusted wire parser: its controls are compiler-emitted, and the final PE/toolchain/inventory gates plus defensive runtime checks validate them. Zig remains responsible for protocol and guest memory validation. C++ appears only in the existing runtime-compatibility probe because it must exercise std::thread, exceptions and libstdc++ initialization.

## TLS state and ownership

Each module owns one fixed slot-layout table guarded by SRWLOCK_INIT (no kernel allocation). Compiler-emitted emutls control location words publish a slot and byte offset with release ordering; readers acquire the word. Size/alignment metadata is immutable after publication. Per-thread data is in the DLL's loader-managed implicit TLS template, with a 16-byte aligned arena and an initialization bitmask. Each thread initializes its own assigned slot from its borrowed compiler template, or loader-provided zero bytes, once. Neither compiler control/template nor the loader record is freed by Waddle.

The production candidate has 20 symbol entries aliasing 11 distinct controls, 88 bytes of payload, max alignment 8 and a conservative first-touch-order bound 157 bytes; the probe's earlier11-object inventory is not production evidence. Default arena 192 bytes and 32 slots provide a bounded fit for current pins, with compile-time capacities and a final-image gate. A changed inventory must pass the actual capacity gate; no automatic overflow fallback or heap allocation is permitted. Native TLS heap retention remains unproven and must be measured with pre-existing-thread controls before acceptance; shrinking the template does not prove zero retained bytes.

Windows reads the TEB TLS vector at x64 offset0x58, checks vector/block and aligned record location, and validates the record's complete range against the module TLS template before dereferencing it. The TLS index is supplied by the Windows loader after a fail-closed PE AddressOfIndex check; the module has no independent length for the platform-owned vector and claims no arbitrary corrupted-loader-memory safety. Unsupported ABI/placement/native mutant results fail qualification. Malformed size/alignment/location/capacity or growth after assignment terminates via __fastfail; no loader-lock teardown or recovery is attempted.

Thread creation owns its start block until the suspended child is resumed; the child copies/frees it before user code. The creation handle belongs to std::thread until join/detach; the per-thread record stores only a borrowed copy for identity comparison. Failure before resumption retires the exact created thread/handle/start block, or fail-fast if cleanup cannot be established. Foreign thread identities are stable tagged record addresses, unique among simultaneously live threads; they are opaque comparison values and must not be closed or waited on.

## Exception arena retirement

`src/vgpu/clvk_runtime_eh_pool.c` registers a final callback from.CRT$XCAWDL before C++ constructors. Registration failure/duplicate initialization fail fast. The callback invokes __gnu_cxx::__freeres exactly once after every static destructor; it must not run early because the GCC routine leaves exception-pool freelist metadata stale. Link-map/PE gates must prove CRT ordering, DLL-local atexit, the exact libstdc++ eh_alloc symbol, and absence of thread-atexit mechanisms. No DllMain or early TLS destruction is introduced. The actual full DLL, not merely the probe, must satisfy this layout and native heap evidence.

## Required verification

Each host test group and fail-fast scenario starts with clean process state, preventing prior slot assignments from falsely killing a boundary mutant. Barrier-controlled first use exercises publication and per-thread isolation; exact-fit/exhaustion, malformed locations, alignment/size changes, allocation/thread failures, joins and foreign identities have explicit controls. Run plain, ASan/LSan/UBSan and TSan tests plus aggregate>=90% line/branch and every production function; the Windows-only accessor needs strict cross-build, final-image checks and real native offset/section mutants. No fake-mode coverage claim covers native loader behavior.

The strengthened native probe measures handles, TLS, FLS, heap busy bytes/entries, existing-thread TLS, foreign and owned threads, and module absence. Preserve oldPOSIX/Win32 controls. Final actual OpenCL.dll requires fresh exact GPU kernels/malformed API/ownership tests, native heap/UMDH unloaded2→3,1100+ reloads, transport-loss cancellation and simultaneous Linux GPU workload. All exact-head CI must pass before final tracker/Time Ended/PR readiness. See existing checkpoint and v2 audit/probe specifications for evidence limits.

## Portable implementation receipt

The TLS implementation unit passes the plain, ASan/LSan/UBSan, TSan and coverage Make targets. Every one of the 10 fake-mode production functions executes; line coverage is 118/120 (98.33%) and branch outcomes 85/92 (92.39%), without exclusions. Four fail-closed coverage-validator groups pass normally and under Python optimization. The source-bound raw receipt is `build/qualification_feature_20261021/root_tls/make_receipt.json`; actual gcov JSON and input hashes are in unique directories under `build/coverage/vgpu/clvk_runtime_tls/`. The private GCC 14.2 Win32 compiler also accepts the real Windows object with strict warnings. These are portable/API-double and cross-build results only. No new DLL/native acceptance is claimed.

The emergency-arena unit has six clean-process controls: LIFO ordering after three simulated destructors, registration failure, duplicate initialization, unregistered release, double release, and initialization after retirement. The test refuses `NDEBUG`; the production callback remains private and is not exposed as early shutdown. Plain and ASan/LSan/UBSan gates pass; actual included callback code covers 13/13 lines and 6/6 branch outcomes. The common coverage validator retains the 90% floor and exact function inventory for either source. The initial coverage run correctly failed (10/13 lines) because the fake fatal boundary used `_Exit` without flushing host gcov; it now uses host `exit`, with only fake CRT registrations, so fatal-path coverage and LSan are retained. Production still uses `__fastfail`. Raw failed and corrected runs remain preserved. Strict GCC 14.2 Win32 object compilation and combined probe link succeed, but strict map admission rejects a retained `std::__at_thread_exit` helper. This is unresolved, no gate is waived and no new candidate is accepted.

## Unsupported thread-exit/equality link fences

Exact GCC archive review found no inbound calls/address references to the two thread-exit roots in the current probe. `condition_variable.o` was nevertheless retained for ordinary synchronization required by shared_mutex. Ordinary `--gc-sections`, including a second attempt with `--exclude-all-symbols`, did not remove those roots. The Waddle linker supplement `scripts/clvk/runtime_sections.ld` removes only the exact text/unwind/once COMDAT sections for `std::__at_thread_exit` and `std::notify_all_at_thread_exit`; ordinary condition-variable functionality remains. No SDK source/archive is edited.

The first actual negative link test exposed that GNU PE ld silently permits a reference to a discarded function. **Discard alone is unsafe and not admitted.** Mandatory undefined `--wrap` fences for both exact mangled roots now reject external calls and address-taking before a DLL can be admitted. An additional undefined `--wrap=__gthr_win32_equal` rejects use of the comparator incompatible with foreign resource-free identities. The exact reviewed gthr archive has no internal comparator calls; std::thread::id uses scalar comparisons, as the native probe exercises. Detached-created-thread handle identity behavior is inherited from this GCC runtime, not a new foreign-thread guarantee. These unsupported low-level APIs must not be invoked; no general replacement ABI beyond the documented five entry points is claimed. Package/archive identity, ordinary synchronization, final PE checks and actual negative link tests are mandatory.

`vgpu-opencl-runtime-link-test` compiles a positive DLL plus six separate negative objects; each negative must compile successfully, then fail the actual link for the expected unresolved wrapper (direct call and address-taking for all three APIs). The final positive PE contains no mapped thread-exit root definitions; discarded section-zero COFF names are metadata only. Root confirmed strict map ABI/CRT ownership on that DLL after adding correct support for GNU long-section continuation rows. The map gate ignores only the discarded inventory and linker wildcard declarations; any forbidden mapped output row and any defined PE function still fail. Nineteen synthetic admission groups pass optimized. Failed and corrected real controls remain under `build/qualification_runtime_link/`, including the initial discarded-reference bypass. No native acceptance is inferred from the link controls.

### Normal futures without thread-exit callbacks (2026-10-21)

The first full production link exposed an additional dead COMDAT in the exact
reviewed GCC archive: `future.o` is needed by LLVM's ordinary future_error,
promise.set_value and future.wait paths, but also contains
`std::__future_base::_State_baseV2::_Make_ready::_M_set()`, a tail-call to the
unsupported thread-exit callback API. Source scans and exact object relocations
show no normal runtime caller of the thread-exit helper. The original failed
link/map remain in `root_tls/full_driver_build/`.

The Waddle linker supplement discards only that helper's text/pdata/xdata and
adds an undefined-reference wrap for the helper itself in both production and
probe links. This preserves the fence against GNU PE discard-only call/address
bypasses. Final map/PE admission rejects a retained helper in mangled or
demangled form. No dependency source/archive, pin or forbidden API allowance
changes. Actual ordinary promise.set_value/future.wait/get linking passes; all
eight direct/address controls reject, including set_value_at_thread_exit call
and member-pointer use. Both Python modes pass. Root independently reran all34
build-driver tests outside ptrace in both modes: PASS, including actual
ASan/LSan-backed boundaries. Worker ptrace-related sanitizer failures remain
preserved. Receipt `build/qualification_feature_20261021/root_tls/future_link_root/receipt.json`;
link/audit evidence `root_tls/future_link_revision/`. Full new production link,
static admission and native qualification remain pending.

Task impact: +0% to #5/all tasks; parent85.5%, M2 94%, Time Ended TBD.
