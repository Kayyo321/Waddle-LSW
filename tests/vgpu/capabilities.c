/** @file capabilities.c @brief Native C ABI/policy execution for private snapshots. */
#include "waddle/venus_capabilities.h"
#include <assert.h>
#include <string.h>
_Static_assert(offsetof(venus_capabilities_t, vk_extension_mask1) == 20, "Mask ABI");
_Static_assert(offsetof(venus_capabilities_t, allow_vk_wait_syncs) == 148, "Flag ABI");
static void put_word(unsigned char *bytes, size_t offset, uint32_t value) {
    for (unsigned shift = 0; shift < 4; shift++)
        bytes[offset + shift] = (unsigned char)(value >> (8 * shift));
}
static void good_frame(unsigned char *bytes) {
    memset(bytes, 0, VenusCapabilitiesBytes);
    put_word(bytes, 0, 1);
    put_word(bytes, 4, VenusPinnedXmlVersion);
    put_word(bytes, 8, 1);
    put_word(bytes, 12, 3);
    put_word(bytes, 16, 1);
    put_word(bytes, 20, 1);
    put_word(bytes, 68, 3);
    put_word(bytes, 152, 1);
}
static void zero_output(const venus_capabilities_t *value) {
    const unsigned char *bytes = (const unsigned char *)(const void *)value;
    for (size_t index = 0; index < sizeof(*value); index++)
        assert(bytes[index] == 0);
}
int main(void) {
    unsigned char bytes[VenusCapabilitiesBytes + 1];
    venus_capabilities_t value;
    good_frame(bytes);
    assert(venus_capabilities_decode(&value, bytes, VenusCapabilitiesBytes) == RingOk);
    assert(venus_capabilities_compatible(&value) == RingOk);
    assert(value.vk_xml_version == VenusPinnedXmlVersion);
    assert(value.wire_format_version == 1 && value.vk_ext_command_serialization_spec_version == 1 &&
           value.vk_mesa_venus_protocol_spec_version == 3);
    assert(value.supports_blob_id_0 == 1 && value.allow_vk_wait_syncs == 0 &&
           value.supports_multiple_timelines == 1 && value.use_guest_vram == 0);
    assert(venus_capabilities_extension(&value, 384) == 1);
    assert(venus_capabilities_extension(&value, 385) == 1);
    assert(venus_capabilities_decode(NULL, bytes, VenusCapabilitiesBytes) == RingInvalid);
    assert(venus_capabilities_decode(&value, NULL, VenusCapabilitiesBytes) == RingInvalid);
    zero_output(&value);
    const size_t BadLengths[] = {0, 159, 161, SIZE_MAX};
    for (size_t index = 0; index < sizeof(BadLengths) / sizeof(BadLengths[0]); index++) {
        assert(venus_capabilities_decode(&value, bytes, BadLengths[index]) == RingCorrupt);
        zero_output(&value);
    }
    const size_t FlagOffsets[] = {16, 148, 152, 156};
    for (size_t index = 0; index < sizeof(FlagOffsets) / sizeof(FlagOffsets[0]); index++) {
        good_frame(bytes);
        put_word(bytes, FlagOffsets[index], 2);
        assert(venus_capabilities_decode(&value, bytes, VenusCapabilitiesBytes) == RingCorrupt);
        zero_output(&value);
    }
    good_frame(bytes);
    put_word(bytes, 0, 2); /* Well-shaped future version, unsupported profile. */
    assert(venus_capabilities_decode(&value, bytes, VenusCapabilitiesBytes) == RingOk);
    const venus_capabilities_t Before = value;
    assert(venus_capabilities_compatible(&value) == RingInvalid);
    assert(memcmp(&Before, &value, sizeof(value)) == 0);
    assert(venus_capabilities_compatible(NULL) == RingInvalid);
    assert(venus_capabilities_extension(NULL, 1) == 0);
    for (uint32_t number = 1; number < 1024; number++) {
        memset(value.vk_extension_mask1, 0, sizeof(value.vk_extension_mask1));
        value.vk_extension_mask1[0] = 1;
        assert(venus_capabilities_extension(&value, number) == 0);
        value.vk_extension_mask1[number / 32] |= 1u << (number & 31);
        assert(venus_capabilities_extension(&value, number) == 1);
    }
    assert(venus_capabilities_extension(&value, 0) == 0);
    assert(venus_capabilities_extension(&value, 1024) == 0);
    assert(venus_capabilities_extension(&value, UINT32_MAX) == 0);
    value.vk_extension_mask1[0] = 0;
    assert(venus_capabilities_extension(&value, 1023) == 0);
    return 0;
}
