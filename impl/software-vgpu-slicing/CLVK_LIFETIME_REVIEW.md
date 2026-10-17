# Windows clvk retirement review, 2026-10-17

Status: confirmed ownership defect; user explicitly approved the documented
private-copy correction on 2026-10-17. The tracked patch and private export
contract are implemented. Upstream submodule trees and pins remain unchanged.
Cross-build and native execution are still qualification gates.

## Evidence

Pinned clvk `8bd3672174764dce25ba220773e10baa752f0373` allocates its
process-global state lazily from `clGetPlatformIDs` (`src/api.cpp:191-200`,
`src/init.cpp:326-340`). Its only destruction registration is guarded by
`#ifndef WIN32`. The exact target compiler
`x86_64-w64-mingw32-g++-posix -dM -E -x c++ /dev/null` defines both WIN32 and
_WIN32 to 1. There is no Windows global-state retirement entry point.
`clUnloadCompiler` and `clUnloadPlatformCompiler` return success without state
destruction (`src/api.cpp:2559-2575`). Releasing public CL contexts does not
release the internal platform, Vulkan devices, queues or instance.

Native attempt `build/windows11_vm/dxvk_gpu_005527db` loads the compiler DLL in
3422 ms before bootstrap start, then successfully discovers one CL platform
and initializes three host adapters. Its fixture incorrectly required exactly
one adapter and therefore fails before any kernel. On cleanup the ICD reports
20 retained owners: instance 1, physical devices 2/3/4, devices 5/14/19 and their
queues (use the original stderr for exact identities). No result credit is
claimed. The supervisor uses only the exact authenticated session
`b54d7accb652a628`, proves the owned worker graph empty and supplies the trusted
retirement receipt. This is forced failure cleanup, not natural acceptance.
The original logs, executable hashes and process journal remain preserved.

A separate GPT-6 Luna high-effort source audit reached the same lifetime
finding; target preprocessor execution and native retained-owner logs supply
independent evidence beyond that opinion.

## Proposed correction for approval

Apply a tracked patch under `patches/clvk/` only to the build driver's private
copy. Keep the submodule and upstream pin unchanged. Add a private, explicit
terminal shutdown export callable before `FreeLibrary` and outside the Windows
loader lock. Document that the embedding owner must stop new CL calls, finish
all queues, release all CL handles and callbacks, and join application callers
before shutdown. The entry point must make the transition idempotent, prevent
subsequent lazy reinitialization, terminate/join the internal executor pool,
delete platform/device owners and finally destroy VkInstance. It must reject
unsafe active ownership rather than silently abandon it. The implementation
review must establish how the quiescence precondition is checked before it can
be accepted; this document does not assume the current API tracks every owner.

Do not simply enable an atexit/DllMain deletion on Windows: destruction joins
executor threads (`src/queue.hpp:307-316`), which cannot safely run while the
DLL unload path holds the loader lock. Do not change standard
`clUnloadPlatformCompiler` semantics to secretly destroy live device state.

The build receipt/cache key must include the exact patch bytes and private
export contract. The existing standard export drift gate remains in force;
private exports need a separate exact allowlist. Add patch-application, export,
cache invalidation and unsafe-lifetime regressions. The native fixture must
call shutdown only after its exact-result/error tests release all owners, then
prove normal bootstrap stop and module retirement on a completed calling thread.
Rerun eight-context exact kernels, native heap/handles, sanitizers, failure
cleanup and concurrent Linux/guest GPU cases. A successful private extension
fixture does not prove arbitrary unmodified applications unload safely; that
embedding contract must remain explicit in feature scope and acceptance.

## Independent work

The fixture's adapter selection can be corrected without touching clvk. Current
ICD/DXVK source still requires fresh normal/sanitizer/fault/native heap/physical
runs. Feature completion and Time Ended remain unchanged pending all gates.

## Implemented contract after approval

`0001-explicit-quiescent-windows-shutdown.patch` changes only the copied clvk
source. A single atomic allocation count is incremented in every `refcounted`
constructor and decremented in its base destructor, after derived destruction
and member cleanup. It counts allocations, not retained reference counts.
Before shutdown the count must equal the actual platform root-device count;
every root device must independently have reference count one. This also rejects
objects detached from context ownership. Executor states must all be free and
individually idle under their locks. Only after these nonmutating preflights
passes may shutdown join the executor pool and delete platform/devices/instance
in their existing owner order. Failed checks retain all resources.

The initialization and shutdown paths share a mutex. The successful terminal
state is sticky for the current DLL load; the once flag is never reset. Version
1 returns 0 for success/repeats (including before lazy initialization), 1 for
busy owners/executors, 2 for unsupported ABI, and 3 when the configured global
destruction opt-out prevents cleanup. The opt-out is rejected rather than
overridden. Four discovery/getter entry points reject terminal state with
CL_INVALID_PLATFORM before logging/dereferencing. Other commands must not be
called after shutdown; stale opaque handles are not made valid by this extension.
The embedding owner excludes concurrent CL API callers and callbacks throughout
shutdown, and invokes it outside the loader lock and callback/executor threads.

The build driver checks exact patch preimages before applying, bounds Git
discovery to the private copy, includes patch/ABI/export hashes in its cache key,
requires exact PE exports, revalidates source pins and build contract before
publishing, and archives the prior receipt. Fifteen admission tests pass,
including malformed preimage/no partial mutation, repeated patch rejection,
submodule-write rejection, unexpected PE export rejection, and actual patched
refcount owners under ASan/LSan/UBSan with 16,000 threaded owner lifetimes. This
portable vendor-boundary test uses a logging double; it is not native GPU or
shutdown execution evidence.

## Pool selection correction found during independent review

The existing `get_executor` marked a free candidate bound before testing idle.
An executor can be returned while its last command group is still unwinding;
if another queue skipped it in that interval, its map state stayed bound with
no queue owner. This could make the strict shutdown preflight busy forever.
The private patch now changes the state only after a usable idle candidate is
found. All free/idle shutdown assertions remain in force. The exact patched
pool is compiled with a controlled executor boundary; a deterministic sequence
returns an executor before it is idle, forces a skip, then verifies reuse and
strict final quiescence. The previous private-copy implementation fails that
regression at `reused == first` (SIGABRT); the correction passes under
ASan/LSan/UBSan. Sixteen admission tests now pass.

The owned first cross-build was interrupted after this source defect was found;
its log and partial tree remain preserved, and it published no new receipt. A
fresh build with the corrected patch is required. No other process was stopped.
