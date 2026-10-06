/** @file frame_codec.c @brief Native C/Zig presentation frame ABI fixture. */
#include "waddle/venus_frame.h"
#include <assert.h>
int main(void) {
    venus_frame_t frame = {.context = 1,
                           .frame = 2,
                           .layout = {.width = 32,
                                      .height = 16,
                                      .fourcc = 0x34325241,
                                      .plane_count = 1,
                                      .planes = {{.stride = 128, .size = 2048, .extent = 4096}}},
                           .resource_ids = {2},
                           .damage_count = 1,
                           .damage = {{.width = 32, .height = 16}}};
    unsigned char bytes[VenusFrameBytes];
    venus_frame_t decoded;
    assert(venus_frame_encode(&frame, bytes, sizeof(bytes)) == RingOk);
    assert(venus_frame_decode(&decoded, bytes, sizeof(bytes)) == RingOk);
    assert(decoded.context == 1 && decoded.frame == 2 && decoded.layout.width == 32);
    bytes[44] = 1;
    assert(venus_frame_decode(&decoded, bytes, sizeof(bytes)) == RingCorrupt);
    assert(!decoded.context);
    return 0;
}
