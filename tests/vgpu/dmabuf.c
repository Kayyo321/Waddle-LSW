/** @file dmabuf.c @brief Native C/Zig image metadata ABI fixture. */
#include "waddle/venus_dmabuf.h"
#include <assert.h>
#include <string.h>
int main(void) {
    venus_dmabuf_layout_t layout = {.width = 64,
                                    .height = 32,
                                    .fourcc = 0x34325241,
                                    .plane_count = 1,
                                    .planes = {{.stride = 256, .size = 8192, .extent = 8192}}};
    assert(venus_dmabuf_layout_validate(&layout) == RingOk);
    layout.planes[0].offset = 1;
    assert(venus_dmabuf_layout_validate(&layout) == RingInvalid);
    unsigned char table[16] = {0x41, 0x52, 0x32, 0x34};
    unsigned char indices[2] = {0};
    memset(table + 4, 0xff, 4);
    assert(venus_dmabuf_feedback_match(table, sizeof(table), indices, sizeof(indices), 0x34325241,
                                       0) == RingOk);
    indices[0] = 1;
    assert(venus_dmabuf_feedback_match(table, sizeof(table), indices, sizeof(indices), 0x34325241,
                                       0) == RingCorrupt);
    return 0;
}
