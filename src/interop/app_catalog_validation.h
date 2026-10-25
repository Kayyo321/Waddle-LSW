/** @file app_catalog_validation.h @brief Internal C ABI for Zig catalog validators. */
#ifndef AppCatalogValidationHeaderGuard
#define AppCatalogValidationHeaderGuard

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Validate a borrowed UTF-8 slice.
 * @param[in] data Borrowed bytes, nullable only for allowed empty input.
 * @param[in] length Number of readable bytes for this call.
 * @param[in] maximum Maximum accepted byte length, at most 4096.
 * @param[in] allow_empty Whether zero length is valid.
 * @return True when valid; false for malformed UTF-8, NUL, null/nonempty, or bounds.
 * @note Implemented in Zig; rejects lengths whose pointer-end arithmetic
 * overflows, retains no pointer, allocates no memory, and is thread-safe for
 * independent calls. Nonempty input must be non-NULL and caller guarantees
 * `data[0..length]` is readable.
 */
bool app_catalog_utf8_valid(const uint8_t *data, size_t length, size_t maximum, bool allow_empty);

/** @brief Validate borrowed package-family syntax.
 * @param[in] data Borrowed bytes, nullable only for empty input.
 * @param[in] length Number of readable bytes for this call.
 * @return True for empty or valid family syntax (maximum 50 bytes); false otherwise.
 * @note Implemented in Zig; rejects pointer-end overflow, retains no pointer,
 * allocates no memory, and is thread-safe. Nonempty input must be non-NULL and
 * caller guarantees `data[0..length]` is readable.
 */
bool app_catalog_package_family_valid(const uint8_t *data, size_t length);

/** @brief Validate borrowed canonical uppercase UTF-16LE path bytes.
 * @param[in] data Borrowed bytes, nullable only when length is zero.
 * @param[in] length Number of readable bytes for this call.
 * @param[in] maximum_units Maximum allowed UTF-16 code units, at most 32767.
 * @return True for an absolute canonical drive/UNC byte form; false otherwise.
 * @note Implemented in Zig; this checks byte syntax and rejects pointer-end
 * overflow. It rejects ASCII lowercase code units; Unicode uppercase identity
 * is proven only by Windows invariant case mapping from the target handle.
 * No pointer is retained or allocation made. Nonempty input must be non-NULL
 * and caller guarantees the input bytes are readable. Independent calls may
 * run concurrently.
 */
bool app_catalog_path_identity_valid(const uint8_t *data, size_t length, size_t maximum_units);

#endif
