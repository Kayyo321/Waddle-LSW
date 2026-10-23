/** @file vn_cs.h @brief Bounded test writer around immutable guest encoder
 * declarations. */
#ifndef WaddleFixtureVnCsH
/** @brief Test include guard; no storage or ownership. */
#define WaddleFixtureVnCsH
#include <assert.h>
#include <stdint.h>
#include <string.h>
/** @brief External generator tag alias; no runtime storage. */
#define vn_cs_encoder instance_encoder_t
#define vn_cs_encoder_write fixture_unused_write
#define vn_cs_encoder_reserve fixture_unused_reserve
#define vn_cs_encoder_get_len fixture_unused_length
#define vn_cs_handle_load_id fixture_unused_load
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "../../../submodules/venus_protocol/tests/vn_cs.h"
#pragma GCC diagnostic pop
#undef vn_cs_encoder_write
#undef vn_cs_encoder_reserve
#undef vn_cs_encoder_get_len
#undef vn_cs_handle_load_id
/** @brief Test-only writer borrows exclusive byte output for synchronous
 * generation. */
struct instance_encoder_t {
  unsigned char *bytes; /**< Nonnull private output[capacity]. */
  size_t capacity;      /**< Accessible bounded extent. */
  size_t used;          /**< Initialized prefix extent. */
};
/** @brief Read initialized length from the bounded test writer.
 * @param[in] encoder Nonnull borrowed owner, retained by the synchronous test
 * call.
 * @return Initialized prefix byte count; no error path for valid private owner.
 * @note Allocation-free, sole test thread; no ownership transfer or retained
 * pointer.
 */
static inline size_t
vn_cs_encoder_get_len(const struct instance_encoder_t *encoder) {
  return encoder->used;
}
/** @brief Native external reserve hook; no allocation, sole test thread.
 * @param[in] encoder Nonnull borrowed bounded output owner.
 * @param[in] bytes Required remaining output extent.
 * @return True if extent fits; false without mutation otherwise.
 */
static inline bool vn_cs_encoder_reserve(struct instance_encoder_t *encoder,
                                         size_t bytes) {
  return bytes <= encoder->capacity - encoder->used;
}
/** @brief Native external writer hook, copies initialized bytes and zero
 * padding.
 * @param[in,out] encoder Nonnull exclusive borrowed owner and output.
 * @param[in] bytes Wire stride, at least data_bytes and within remaining
 * capacity.
 * @param[in] data Nonnull borrowed data[data_bytes].
 * @param[in] data_bytes Actual initialized source extent.
 * @note Allocation-free, assert bounds before writes, sole test thread.
 */
static inline void vn_cs_encoder_write(struct instance_encoder_t *encoder,
                                       size_t bytes, const void *data,
                                       size_t data_bytes) {
  assert(data && data_bytes <= bytes && vn_cs_encoder_reserve(encoder, bytes));
  memcpy(encoder->bytes + encoder->used, data, data_bytes);
  memset(encoder->bytes + encoder->used + data_bytes, 0, bytes - data_bytes);
  encoder->used += bytes;
}
/** @brief Native external ID hook; no pointer dereference or ownership
 * transfer.
 * @param[in] handle Nonnull address of test handle word, borrowed for call.
 * @param[in] kind Imported Vulkan object kind, must be INSTANCE or
 * PHYSICAL_DEVICE, DEVICE, QUEUE, FENCE, SEMAPHORE, BUFFER, DEVICE_MEMORY, COMMAND_POOL, COMMAND_BUFFER, RENDER_PASS, FRAMEBUFFER, IMAGE, IMAGE_VIEW, SHADER_MODULE,
 * PIPELINE, PIPELINE_CACHE, PIPELINE_LAYOUT, DESCRIPTOR_SET_LAYOUT, DESCRIPTOR_POOL, DESCRIPTOR_SET, SAMPLER, BUFFER_VIEW, QUERY_POOL or EVENT.
 * @return Preassigned test host ID encoded in the native handle word.
 * @note Test-only; never used to validate application handles in the real ICD.
 */
static inline vn_object_id vn_cs_handle_load_id(const void **handle,
                                                VkObjectType kind) {
  assert(kind == VK_OBJECT_TYPE_INSTANCE ||
         kind == VK_OBJECT_TYPE_PHYSICAL_DEVICE || kind == VK_OBJECT_TYPE_DEVICE ||
         kind == VK_OBJECT_TYPE_QUEUE || kind == VK_OBJECT_TYPE_FENCE || kind == VK_OBJECT_TYPE_SEMAPHORE || kind == VK_OBJECT_TYPE_BUFFER || kind == VK_OBJECT_TYPE_DEVICE_MEMORY || kind == VK_OBJECT_TYPE_COMMAND_POOL || kind == VK_OBJECT_TYPE_COMMAND_BUFFER ||
         kind == VK_OBJECT_TYPE_RENDER_PASS || kind == VK_OBJECT_TYPE_FRAMEBUFFER ||
         kind == VK_OBJECT_TYPE_IMAGE || kind == VK_OBJECT_TYPE_IMAGE_VIEW ||
         kind == VK_OBJECT_TYPE_SHADER_MODULE || kind == VK_OBJECT_TYPE_PIPELINE ||
         kind == VK_OBJECT_TYPE_PIPELINE_CACHE ||
         kind == VK_OBJECT_TYPE_PIPELINE_LAYOUT || kind == VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT ||
         kind == VK_OBJECT_TYPE_DESCRIPTOR_POOL || kind == VK_OBJECT_TYPE_DESCRIPTOR_SET ||
         kind == VK_OBJECT_TYPE_SAMPLER || kind == VK_OBJECT_TYPE_BUFFER_VIEW ||
         kind == VK_OBJECT_TYPE_QUERY_POOL || kind == VK_OBJECT_TYPE_EVENT);
  uintptr_t id;
  memcpy(&id, handle, sizeof(id));
  return id;
}
#endif
