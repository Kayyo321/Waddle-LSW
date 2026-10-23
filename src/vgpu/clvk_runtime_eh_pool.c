/**
 * @file clvk_runtime_eh_pool.c
 * @brief Retire LLVM's managed statics and the static GCC 14.2 exception arena
 *        after C++ destructors.
 *
 * The link-map gate requires this initializer between .CRT$XCA and .CRT$XCZ,
 * before the C++ constructor dispatcher. DLL-local atexit is LIFO: registering
 * first makes this callback run last. llvm::llvm_shutdown() then destroys every
 * ManagedStatic (cl::opt registry, timer globals, debug counters, statistic and
 * timer mutexes) that clspv and LLVM created during this module image's life;
 * without it each OpenCL.dll load retained about 108 KB (native heap run
 * opencl_heap_1e336986). __freeres runs last: it invalidates the arena freelist
 * and must never run while an exception or throwing destructor can remain.
 * No DllMain, TLS callback, upstream change or early explicit release is used.
 */
#include <stdlib.h>

#ifdef WaddleRuntimeEhPoolTest
/** Test boundary; substitutes registration, retirement and fail-fast only. */
extern int waddle_test_runtime_atexit(void (*callback)(void));
extern void waddle_test_runtime_freeres(void);
extern void waddle_test_runtime_llvm_shutdown(void);
extern void waddle_test_runtime_fastfail(void) __attribute__((noreturn));
#define RuntimeEhAtexit waddle_test_runtime_atexit
#define RuntimeEhFreeres waddle_test_runtime_freeres
#define RuntimeLlvmShutdown waddle_test_runtime_llvm_shutdown
#define RuntimeEhFail() waddle_test_runtime_fastfail()
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
/** GCC 14.2 ABI; definition must be libstdc++.a(eh_alloc.o), verified at link.
 * @ownership Releases the DLL-local emergency arena exactly once.
 * @thread_safety Only after all exception users and C++ destructors retire. */
extern void runtime_eh_freeres(void) __asm__("_ZN9__gnu_cxx9__freeresEv");
/** LLVM ABI; definition must be libLLVMSupport.a(ManagedStatic.cpp.obj), verified at link.
 * @ownership Destroys every LLVM ManagedStatic of this module image exactly once.
 * @pre ManagedStatic deleters must not use already-destroyed C++ local statics,
 *      including LLVM streams and getManagedStaticMutex(). Default compiler
 *      options were reviewed; -print-debug-counter enables a deleter that uses
 *      the destroyed dbgs() stream and is not qualified for this unload order.
 * @thread_safety Only after every LLVM user and C++ destructor of the image retired. */
extern void runtime_llvm_shutdown(void) __asm__("_ZN4llvm13llvm_shutdownEv");
#define RuntimeEhAtexit atexit
#define RuntimeEhFreeres runtime_eh_freeres
#define RuntimeLlvmShutdown runtime_llvm_shutdown
#define RuntimeEhFail() __fastfail(FAST_FAIL_FATAL_APP_EXIT)
#endif

/** Loader-serialized state; owns no memory and is reset by a fresh DLL image. */
typedef enum runtime_eh_state_t {
   RuntimeEhUninitialized = 0,
   RuntimeEhRegistered = 1,
   RuntimeEhReleased = 2
} runtime_eh_state_t;

/** Only initializer and final CRT callback access this state. */
static runtime_eh_state_t runtime_eh_state;

/**
 * @brief Final DLL-local atexit callback; never call as an explicit shutdown API.
 * @return No value; fail-fast on unregistered or repeated invocation.
 * @ownership Destroys LLVM's managed statics, then consumes the GCC arena, after
 *            all static destructors, each exactly once and in that order.
 * @pre ManagedStatic deletion must not access destroyed streams/local statics or
 *      recreate a ManagedStatic. Forwarded LLVM diagnostic/statistics options
 *      require separate review; -print-debug-counter violates this precondition.
 * @thread_safety Loader/CRT serialized; every worker and exception must retire.
 */
void
waddle_clvk_eh_pool_release(void)
{
   if (runtime_eh_state != RuntimeEhRegistered)
      RuntimeEhFail();
   runtime_eh_state = RuntimeEhReleased;
   RuntimeLlvmShutdown();
   RuntimeEhFreeres();
}

/**
 * @brief Register the last destructor before any C++ constructors execute.
 * @return No value; registration failure or duplicate initialization fails fast.
 * @ownership CRT owns the callback registration; no caller-owned allocation.
 * @thread_safety Loader serialized, once per module image; not an exported API.
 */
void
waddle_clvk_eh_pool_register(void)
{
   if (runtime_eh_state != RuntimeEhUninitialized)
      RuntimeEhFail();
   if (RuntimeEhAtexit(waddle_clvk_eh_pool_release) != 0)
      RuntimeEhFail();
   runtime_eh_state = RuntimeEhRegistered;
}

/** First C initializer; exact section/order/owner must pass final-image gates.
 * @ownership Immutable module pointer, no resource. Not a DLL export.
 * @thread_safety Invoked once by the serialized CRT before C++ construction. */
__attribute__((used, section(".CRT$XCAWDL")))
void (*const waddle_clvk_eh_pool_initializer)(void) = waddle_clvk_eh_pool_register;
