/** @file vkr_cs.h @brief Bounded test encoder adapter around pinned generator
 * declarations. Vendor decoder/handle stubs remain compile-only and are never
 * invoked. This fixture is an independent serializer oracle, not a receiver or
 * guest ICD.
 */
#ifndef WaddleFixtureVkrCsH
/** @brief Test include guard, no storage/ownership. */
#define WaddleFixtureVkrCsH
#include <assert.h>
#include <stdint.h>
#include <string.h>
/** @brief External generator tag aliases; no runtime storage. */
#define vn_cs_encoder fixture_encoder_t
#define vkr_cs_encoder fixture_encoder_t
/** @brief Rename the vendor compile-only writer before installing the bounded
 * oracle writer. */
#define vkr_cs_encoder_write fixture_unused_encoder_write
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "../../../submodules/venus_protocol/tests/vkr_cs.h"
#pragma GCC diagnostic pop
#undef vkr_cs_encoder_write
/** @brief Private test-only encoder, borrows output until synchronous encoding
 * ends. */
struct fixture_encoder_t {
    unsigned char *bytes; /**< Actual exclusive private bytes[capacity]. */
    size_t capacity;      /**< Accessible output extent. */
    size_t used;          /**< Initialized serialized prefix. */
};
/** @brief Copy native generated data into bounded test wire storage.
 * @param[in,out] encoder Nonnull exclusive borrowed encoder/buffer.
 * @param[in] bytes Wire stride, at least data_bytes; multiple of4.
 * @param[in] data Nonnull borrowed native data[data_bytes].
 * @param[in] data_bytes Actual initialized source extent.
 * @note Allocation-free, sole test thread; assert all extents before copying.
 */
static inline void vkr_cs_encoder_write(struct fixture_encoder_t *encoder, size_t bytes,
                                        const void *data, size_t data_bytes) {
    assert(data && data_bytes <= bytes && bytes <= encoder->capacity - encoder->used);
    memcpy(encoder->bytes + encoder->used, data, data_bytes);
    memset(encoder->bytes + encoder->used + data_bytes, 0, bytes - data_bytes);
    encoder->used += bytes;
}
#endif
