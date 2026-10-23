/** @file instance_wire.c @brief Independent pinned instance encoder and native decoder ABI. */
#include "vn_cs.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "vn_protocol_driver_instance.h"
#pragma GCC diagnostic pop
#include "waddle/venus_instance_wire.h"
#include <assert.h>
#include <string.h>
/** @brief Generate independent pinned create packet into test output.
 * @param[in] info Nonnull immutable valid core native input, borrowed for call.
 * @param[in] host_id Reserved nonzero test identity, never dereferenced.
 * @param[out] bytes Nonnull exclusive output[capacity], at least4096 bytes.
 * @param[in] capacity Actual accessible output extent.
 * @return Initialized prefix length, zero for invalid local input.
 * @note Test-only, synchronous, no allocation; vendor decoder helpers are unused.
 */
size_t venus_instance_test_encode(const venus_vk_instance_info_t *info, uint64_t host_id,
                                  void *bytes, size_t capacity) {
    if (!info || !host_id || !bytes || capacity < 4096)
        return 0;
    struct instance_encoder_t encoder = {.bytes = bytes, .capacity = capacity};
    VkInstance instance = (VkInstance)(uintptr_t)host_id;
    vn_encode_vkCreateInstance(&encoder, 1, info, NULL, &instance);
    return encoder.used;
}
#ifndef VgpuInstanceOracle
int main(void) {
    unsigned char bytes[4096], expected[4096];
    venus_vk_instance_info_t info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    size_t written = 0;
    assert(venus_instance_wire_create(&info, 42, bytes, sizeof(bytes), &written) == RingOk);
    size_t count = venus_instance_test_encode(&info, 42, expected, sizeof(expected));
    assert(written == count && !memcmp(bytes, expected, count));
    VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                     .pApplicationName = "app",
                                     .pEngineName = "engine",
                                     .apiVersion = VK_API_VERSION_1_1};
    info.pApplicationInfo = &application;
    for (uint32_t minor = 0; minor <= 3; ++minor) {
        application.apiVersion = VK_MAKE_API_VERSION(0, 1, minor, 4095);
        assert(venus_instance_wire_create(&info, 42, bytes, sizeof(bytes), &written) == RingOk);
        count = venus_instance_test_encode(&info, 42, expected, sizeof(expected));
        assert(written == count && !memcmp(bytes, expected, count));
    }
    application.apiVersion = VK_MAKE_API_VERSION(0, 1, 4, 0);
    memset(bytes, 0xa5, sizeof(bytes));
    written = 99;
    assert(venus_instance_wire_create(&info, 42, bytes, sizeof(bytes), &written) == RingInvalid);
    assert(written == 99);
    for (size_t index = 0; index < sizeof(bytes); ++index)
        assert(bytes[index] == 0xa5);
    unsigned char reply[24] = {0};
    reply[8] = 1;
    reply[16] = 42;
    venus_vk_result_t result = VK_ERROR_UNKNOWN;
    assert(venus_instance_wire_create_reply(&result, reply, sizeof(reply), 42) == RingOk);
    assert(result == VK_SUCCESS);
    assert(venus_instance_wire_destroy(42, bytes, sizeof(bytes), &written) == RingOk);
    assert(written == 24 && bytes[0] == 1 && bytes[4] == 1 && bytes[8] == 42);
    return 0;
}
#endif
