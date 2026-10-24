#ifndef WaddleInteropIvshmemDriverH
/** @brief Guard the unguarded pinned SDK include; no runtime ownership. */
#define WaddleInteropIvshmemDriverH
/**
 * @file interop_ivshmem_driver.h
 * @brief Include the pinned IVSHMEM ABI through one production/test SDK boundary.
 * @ownership This header declares SDK types and one device-interface GUID only.
 * @thread_safety No runtime state or calls are provided.
 */
#if defined(InteropMappingWindowsTestApi)
#include InteropMappingWindowsTestApi
#else
#include <windows.h>
#include <setupapi.h>
#endif

/* The vendor header is intentionally included only through this guarded wrapper. */
#include "ivshmem.h"

#endif
