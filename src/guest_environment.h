/** @file guest_environment.h @brief Owned Win32 Unicode and environment conversion. */
#ifndef WaddleGuestEnvironmentH
/** @brief Include guard; compile-time marker with no storage or ownership. */
#define WaddleGuestEnvironmentH
#include "guest_codec.h"
#include <windows.h>
/** @brief Convert validated borrowed UTF-8 bytes to owned UTF-16; thread-safe.
 * @param[in] source Nonnull readable length-byte UTF-8 buffer, no embedded NUL.
 * @param[in] length Byte length excluding terminator; bounded to 1 MiB.
 * @return malloc-owned NUL-terminated string; free with free(). NULL sets Win32
 * last error to conversion/allocation failure; no ownership transfer of input. */
wchar_t *guest_utf16(const uint8_t *source, size_t length);
/** @brief Merge explicit overrides into a copy of inherited environment; thread-safe.
 * @param[in] spawn Nonnull borrowed validated view, retained through this call.
 * @return malloc-owned sorted double-NUL UTF-16 block; free with free(). NULL sets
 * last error. Last case-insensitive override wins. Process environment is untouched. */
wchar_t *guest_environment(const guest_spawn_t *spawn);
#endif
