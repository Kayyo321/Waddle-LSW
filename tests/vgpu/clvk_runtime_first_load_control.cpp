/**
 * @file clvk_runtime_first_load_control.cpp
 * @brief Deliberately retained malloc control for first-load heap observability.
 *
 * This separate negative fixture must never qualify as a leak-free candidate.
 * Every load intentionally retains12345 malloc bytes through unload; the test
 * process/OS heap teardown is its only retirement site. Native instrumentation
 * must identify that allocation before absence of runtime frames is meaningful.
 * Existing runtime probe ABI/work is included unchanged. No upstream is edited.
 */
#ifndef WaddleRuntimeFirstLoadLeakControl
#error This deliberate leak fixture requires explicit negative-control opt-in.
#endif
#include <cstdlib>

namespace {
/** @brief Exact nonzero allocation size required in the calibration receipt. */
constexpr size_t RetainedBytes = 12345;
/** @brief Process-lifetime deliberate negative-control owner, never production. */
static void *volatile retained_allocation = nullptr;
/** @brief Allocate one intentionally retained owner per DLL load. */
struct retained_allocation_t {
    retained_allocation_t()
    {
        retained_allocation = std::malloc(RetainedBytes);
        if (!retained_allocation) std::abort();
    }
};
static retained_allocation_t retained_owner;
}

#include "clvk_runtime_windows.cpp"
