#ifndef WaddleVenusDriverH
#define WaddleVenusDriverH
/** @file venus_driver.h
 * @brief Guard the pinned signed-driver header, which has no upstream guard.
 * Vendor declarations stay in the submodule and are used only at the native
 * Windows ABI boundary. This wrapper owns no memory or mutable state.
 */
#include <windows.h>
#include "ivshmem.h"
#endif
