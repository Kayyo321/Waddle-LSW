/** @file codec_abi.c @brief C ABI check for the Zig interop codec.
 * Builds frames through the public header from C, decodes them back, and
 * checks transfer reassembly, so struct layouts, enum widths and exported
 * symbols agree between C callers and the Zig implementation. Runs under the
 * sanitizer build of `make interop-sanitizers`.
 */
#include "waddle/interop_protocol.h"
#include "waddle/interop_memory.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(interop_header_t) == 16, "header ABI");
_Static_assert(sizeof(interop_transfer_t) == 152, "transfer ABI");

/** Writes a guest Hello into frame and returns its total length. */
static size_t build_hello(uint8_t *frame, size_t capacity, uint64_t epoch) {
    interop_writer_t writer;
    interop_writer_init(&writer, frame, capacity);
    assert(interop_put_uint(&writer, InteropTagCapabilities, InteropCapWindows | InteropCapClipboard) == 0);
    assert(interop_put_bytes(&writer, InteropTagAgentVersion, "0.2.0", 5) == 0);
    assert(interop_put_uint(&writer, InteropTagLayoutVersion, InteropLayoutVersion) == 0);
    assert(interop_put_uint(&writer, InteropTagSessionEpoch, epoch) == 0);
    size_t length = 0;
    assert(interop_writer_finish(&writer, InteropMsgHello, 0, InteropFromGuest, &length) == 0);
    return length;
}

int main(void) {
    static uint8_t frame[InteropMaxFrameBytes];
    size_t length = build_hello(frame, sizeof(frame), 0xABCDEF);
    interop_header_t header;
    assert(interop_header_decode(frame, length, &header) == InteropErrorNone);
    assert(header.magic == InteropMagic && header.version == InteropVersion);
    assert(header.type == InteropMsgHello && header.length + InteropHeaderBytes == length);
    assert(InteropVersion == 2 && InteropLayoutVersion == 3);
    const uint8_t *payload = frame + InteropHeaderBytes;
    assert(interop_payload_validate(header.type, InteropFromGuest, payload, header.length) == 0);
    assert(interop_payload_validate(header.type, InteropFromHost, payload, header.length) ==
           InteropErrorUnsupportedType);
    uint64_t capabilities = 0, epoch = 0;
    assert(interop_field_uint(payload, header.length, InteropTagCapabilities, 8, &capabilities) == 0);
    assert(capabilities == (InteropCapWindows | InteropCapClipboard));
    uint64_t layout = 0;
    assert(interop_field_uint(payload, header.length, InteropTagLayoutVersion, 4, &layout) == 0);
    assert(layout == InteropLayoutVersion);
    assert(interop_field_uint(payload, header.length, InteropTagSessionEpoch, 8, &epoch) == 0 && epoch == 0xABCDEF);
    const uint8_t *version = NULL;
    uint16_t version_length = 0;
    assert(interop_field_get(payload, header.length, InteropTagAgentVersion, 0, &version, &version_length) == 0);
    assert(version_length == 5 && memcmp(version, "0.2.0", 5) == 0);

    /* A window announce with a rect, point-free fields and an optional title. */
    interop_writer_t writer;
    interop_writer_init(&writer, frame, sizeof(frame));
    assert(interop_put_uint(&writer, InteropTagWindow, 0x30022) == 0);
    assert(interop_put_uint(&writer, InteropTagGeneration, 4) == 0);
    assert(interop_put_uint(&writer, InteropTagRole, InteropRoleToplevel) == 0);
    assert(interop_put_rect(&writer, InteropTagRect, -8, 0, 1280, 720) == 0);
    assert(interop_put_bytes(&writer, InteropTagTitle, "File Explorer", 13) == 0);
    assert(interop_put_bytes(&writer, InteropTagAppKey, "a-Microsoft_Windows_Explorer", 28) == 0);
    assert(interop_put_uint(&writer, InteropTagState, InteropStateResizable | InteropStateMaximizable) == 0);
    assert(interop_put_uint(&writer, InteropTagDpi, 120) == 0);
    const uint8_t minimum[8] = {64, 1, 0, 0, 200, 0, 0, 0}; /* 320 x 200. */
    assert(interop_put_bytes(&writer, InteropTagMinSize, minimum, sizeof(minimum)) == 0);
    assert(interop_writer_finish(&writer, InteropMsgWindowAnnounce, 0, InteropFromGuest, &length) == 0);
    assert(interop_header_decode(frame, length, &header) == 0);
    int32_t x = 0;
    assert(interop_field_i32(frame + InteropHeaderBytes, header.length, InteropTagAxisV120, &x) == 1);
    const uint8_t *rect = NULL;
    uint16_t rect_length = 0;
    assert(interop_field_get(frame + InteropHeaderBytes, header.length, InteropTagRect, 0, &rect, &rect_length) == 0);
    int32_t left;
    memcpy(&left, rect, sizeof(left));
    assert(rect_length == 16 && left == -8);
    assert(interop_field_get(frame + InteropHeaderBytes, header.length, InteropTagMinSize, 0, &rect, &rect_length) == 0);
    assert(rect_length == sizeof(minimum) && memcmp(rect, minimum, sizeof(minimum)) == 0);

    uint8_t key[InteropMaxAppKeyBytes];
    size_t key_length = 0;
    assert(interop_app_key_from_aumid((const uint8_t *)"A.B", 3, key, sizeof(key), &key_length) == 0);
    assert(key_length == 14 && memcmp(key, "a-A_B-4b861d8b", key_length) == 0);

    /* Point fields and a rejected out-of-order put poison the writer. */
    interop_writer_init(&writer, frame, sizeof(frame));
    assert(interop_put_point(&writer, InteropTagPoint, 10, 20) == 0);
    assert(interop_put_i32(&writer, InteropTagAxisV120, 120) == 0);
    assert(interop_put_point(&writer, InteropTagPoint, 1, 1) == InteropErrorMalformedFrame);
    assert(writer.failed && interop_put_i32(&writer, InteropTagAxisH120, 0) == InteropErrorMalformedFrame);

    /* Transfer reassembly with a sender-side digest. */
    const char content[] = "clipboard payload";
    uint8_t digest[InteropHashBytes];
    interop_sha256((const uint8_t *)content, sizeof(content) - 1, digest);
    interop_transfer_t transfer;
    memset(&transfer, 0, sizeof(transfer));
    assert(interop_transfer_begin(&transfer, 3, sizeof(content) - 1, 1024) == 0);
    assert(interop_transfer_chunk(&transfer, 0, (const uint8_t *)content, 9) == 0);
    assert(interop_transfer_chunk(&transfer, 9, (const uint8_t *)content + 9, sizeof(content) - 10) == 0);
    assert(interop_transfer_end(&transfer, digest) == 0 && transfer.state == 2);
    interop_transfer_cancel(&transfer);
    assert(transfer.id == 0 && transfer.state == 0);
    puts("interop codec ABI: header, fields, writer and transfers agree across C and Zig");
    return 0;
}
