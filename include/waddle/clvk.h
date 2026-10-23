/** @file clvk.h
 * @brief Private explicit retirement ABI for the approved clvk build copy.
 * This extension is not a standard OpenCL command. Borrowed root platform and
 * device identities become invalid after success. No CL call may overlap the
 * shutdown call or follow successful shutdown except a fresh discovery probe
 * (which returns CL_INVALID_PLATFORM). Reload the DLL for a new lifecycle.
 */
#ifndef WaddleClvkHeader
#define WaddleClvkHeader
#include <stdint.h>
/** @brief Version accepted by the private shutdown entry; no ownership. */
enum { ClvkShutdownAbi = 1 };
/** @brief Stable shutdown status values; failures preserve every owner. */
typedef enum clvk_shutdown_status_t {
    /** Terminal success, including repeated calls; no owners remain. */
    ClvkShutdownOk = 0,
    /** A counted object, retained device reference, or executor is still live. */
    ClvkShutdownBusy = 1,
    /** Unsupported ABI; no initialization or mutation occurs. */
    ClvkShutdownVersion = 2,
    /** CLVK_DESTROY_GLOBAL_STATE disables cleanup; configuration must be fixed. */
    ClvkShutdownDisabled = 3
} clvk_shutdown_status_t;
/** @brief Dynamically loaded private C ABI; DLL must outlive the call.
 * @param[in] abi_version ClvkShutdownAbi; scalar, never a pointer.
 * @return A clvk_shutdown_status_t value represented as signed 32-bit integer.
 * @note Sole embedding owner calls outside loader lock and callbacks, after
 * stopping API callers, finishing queues, releasing all CL objects and joining
 * application callers. Busy may be retried while executor teardown settles.
 * Success joins internal executors and releases devices, instance and logging.
 * Never call this ABI through an unverified third-party OpenCL module.
 */
typedef int32_t (*clvk_shutdown_t)(uint32_t abi_version);
#endif
