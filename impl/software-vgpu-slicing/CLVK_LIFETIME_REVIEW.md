# Windows clvk retirement review, 2026-10-17

Status: confirmed ownership defect; proposed private-copy correction is awaiting
the explicit approval required by CODEX_HANDOFF.md section 4, item 1. No
upstream source, submodule pointer, export list or pinned DLL has been changed.

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
