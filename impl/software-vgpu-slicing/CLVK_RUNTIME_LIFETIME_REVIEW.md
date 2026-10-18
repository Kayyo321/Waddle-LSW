# OpenCL MinGW runtime lifetime investigation

Status: native defect isolated; correction comparison pending. No upstream
source correction, new DLL, acceptance waiver or completed qualification.
The approved shutdown/buffer patches and all pinned submodules stay unchanged.

## Reproduction and ownership evidence

The unmodified final DLL hash is
`a8fd9d7a4f979ca4534e26d789025cf9f7c7affd84b46726eeee28f40d83c54d`.
Healthy UMDH audit `edf760f119a1747c` completes24 primary lifetimes and all
original checks with native exit0. It retains process handles53→142→148→154.
The separate traced diagnostic `ca77f1b6a9915205` reproduces the same counts
and24 primary lifetimes, native exit0. Tracing enables/disables successfully
only on the PID/creation-validated owned process; all bounded snapshots return0.
Both unique UST keys return to original absence. Tracing alters diagnostic
configuration, so its execution is not a replacement acceptance run.

At unloaded2 there are eight newly occupied handle values and two removed.
Six new current handles have latest open records through the exact OpenCL
image: four type24 values (0xf0,0x17c,0x1d4,0x1fc), event0x2e4(type21), and
thread reference0x32c(type9). All six remain at unloaded3 with identical full
trace histories and types. Six more values at unloaded3 have the same OpenCL
creation chains. Separate RPC event/thread churn explains the other two adds;
it does not explain away the six OpenCL runtime handles. Numeric values alone
are not object identities, and older trace PCs require their original mappings.

The exact-image COFF symbols and embedded DWARF identify these chains:

- First semaphore pair: pthread condition/rwlock initialization, writer lock,
  `llvm::PassRegistry::registerPass`, static initialization and CRT attach.
- Second pair: pthread key creation, libgcc emulated TLS, `std::call_once`, and
  clvk global initialization.
- Event/thread reference: lazy pthread current-thread state during emulated
  TLS lookup from `std::uncaught_exception`, stream flush, iostream finalization
  and CRT onexit/DLL detach. This is a duplicated current-thread reference;
  the trace does not establish creation of another running thread.

No collector closes or duplicates target handles. It closes only its own
OpenProcess reference. Full trace history, stage-specific module bases, exact
DLL hashes, raw UMDH and creator resolutions are retained in
[OPENCL_HANDLE_RECEIPT_2026_10_17.json](OPENCL_HANDLE_RECEIPT_2026_10_17.json).

## Source findings and rejected speculative correction

The actual pinned LLVM `llvm/lib/IR/PassRegistry.cpp:23` uses an ordinary
function-local static registry; it is not the ManagedStatic implementation
found in older LLVM releases. Its lock uses `std::shared_mutex`. The installed
GCC14 POSIX `shared_mutex` header selects `PTHREAD_RWLOCK_INITIALIZER` and a
default destructor, omitting `pthread_rwlock_destroy` on that branch. The
runtime creates condition semaphores lazily through winpthreads. Calling
`llvm_shutdown()` does not destroy this ordinary static and is not an evidenced
fix for the observed pair. No such patch has been applied.

Official MinGW v12 source describes lazy pthread thread state that owns an
event and a duplicated current-thread handle, consistent with the detach
stacks. Its process-detach pool cleanup is disabled, while its thread-detach
path has explicit closes. That branch is supporting design evidence, not a
byte-identical source receipt for Debian12.0.0-5. The native binary traces and
installed headers are the exact local evidence. Do not enable generic cleanup
inside DllMain or destroy TLS globals early while later CRT callbacks use them.

## Bounded correction experiment

First reproduce the failure with an isolated C++ runtime DLL and a C Windows
loader test. Exercise static shared mutex initialization, once/TLS, a joined
thread and stream teardown; require the original exact handle baseline and
module absence across repeated unloads. This omits graphics and OpenCL to
isolate runtime ownership. Preserve the POSIX result even if it fails.

Then compile the identical source with the matching provided GCC14.2 Win32
thread model and static libgcc/libstdc++, without forced winpthreads. The three
Debian packages14.2.0-19+27+b1 were downloaded and hash-matched against APT
metadata, then extracted under
`build/qualification_20261017/mingw_win32_private_1402`; no host package was
installed or upgraded. Compiler support, strict cross-compilation, import
inspection and native lifetime assertions must pass before selecting this
configuration. No change to LLVM/CLVK or base SDK sources is authorized by this
experiment. Any required upstream source correction still needs the separate
explicit approval required by CODEX_HANDOFF.md section4.

If successful, specify and test the repository build configuration change,
rebuild the final DLL, rerun all affected admission/sanitizer/coverage checks,
exact native GPU kernels, concurrent callers, shutdown, module/heap/handle
retirement and overlap/cancellation gates. Prior DLL execution cannot qualify
a rebuilt DLL. Keep native heap and final GPU acceptance false until complete.

M2 remains94%/Review because its independent absolute Windows count gate can
fail in a system-only control. This runtime defect is additional work and
cannot waive that preserved failure. Feature progress stays85.5%, Time Ended TBD.
