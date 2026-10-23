/** @file request.c @brief Portable C/Zig ABI and independent wire-policy
 * fixture. */
#include "waddle/venus_request.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void write_word(unsigned char *bytes, size_t offset, uint32_t value) {
    for (size_t index = 0; index < 4; index++)
        bytes[offset + index] = (unsigned char)(value >> (index * 8));
}

static void reject_field(unsigned char *bytes, size_t offset, uint32_t value) {
    unsigned char saved[VenusRequestHeaderBytes];
    memcpy(saved, bytes, sizeof(saved));
    write_word(bytes, offset, value);
    venus_request_t decoded;
    memset(&decoded, 0x5a, sizeof(decoded));
    assert(venus_request_decode(&decoded, bytes, sizeof(saved)) == RingCorrupt);
    const venus_request_t Empty = {0};
    assert(memcmp(&decoded, &Empty, sizeof(decoded)) == 0);
    memcpy(bytes, saved, sizeof(saved));
}

int main(void) {
    unsigned char bytes[VenusRequestHeaderBytes];
    const unsigned char Golden[VenusRequestHeaderBytes] = {
        0x31, 0x51, 0x56, 0x57, 1,    0,    0,    0,   RequestCapabilities, 0, 0, 0, 0, 0, 0, 0,
        0x80, 0x70, 0x60, 0x50, 0x40, 0x30, 0x20, 0x10};
    venus_request_t value = {.kind = RequestCapabilities, .sequence = UINT64_C(0x1020304050607080)};
    venus_request_t decoded;
    assert(venus_request_encode(&value, bytes, sizeof(bytes)) == RingOk);
    assert(memcmp(bytes, Golden, sizeof(bytes)) == 0);
    assert(venus_request_decode(&decoded, Golden, sizeof(Golden)) == RingOk);
    assert(memcmp(&decoded, &value, sizeof(value)) == 0);
    for (size_t index = 0; index < sizeof(bytes); index++) {
        if (index >= 16 && index < 24)
            continue;
        bytes[index] ^= 0x80;
        assert(venus_request_decode(&decoded, bytes, sizeof(bytes)) == RingCorrupt);
        bytes[index] ^= 0x80;
    }
    assert(venus_request_encode(NULL, bytes, sizeof(bytes)) == RingInvalid);
    assert(venus_request_encode(&value, NULL, sizeof(bytes)) == RingInvalid);
    assert(venus_request_decode(NULL, bytes, sizeof(bytes)) == RingInvalid);
    assert(venus_request_decode(&decoded, NULL, sizeof(bytes)) == RingInvalid);
    assert(venus_request_decode(&decoded, bytes, sizeof(bytes) - 1) == RingCorrupt);
    memset(bytes, 0x5a, sizeof(bytes));
    value.kind = 0;
    assert(venus_request_encode(&value, bytes, sizeof(bytes)) == RingInvalid);
    for (size_t index = 0; index < sizeof(bytes); index++)
        assert(bytes[index] == 0x5a);

    value = (venus_request_t){.kind = RequestWrite, .sequence = 1, .resource_id = 2,
        .flags = RequestWriteScatter, .payload_bytes = 17, .argument_one = 17};
    assert(venus_request_encode(&value, bytes, sizeof(bytes)) == RingOk);
    assert(venus_request_decode(&decoded, bytes, sizeof(bytes)) == RingOk);
    value.payload_bytes = value.argument_one = 4096;
    assert(venus_request_encode(&value, bytes, sizeof(bytes)) == RingOk);
    value.payload_bytes = value.argument_one = 4097;
    assert(venus_request_encode(&value, bytes, sizeof(bytes)) == RingInvalid);
    value.payload_bytes = value.argument_one = 16;
    assert(venus_request_encode(&value, bytes, sizeof(bytes)) == RingInvalid);
    value.payload_bytes = value.argument_one = 17;
    value.argument_zero = 1;
    assert(venus_request_encode(&value, bytes, sizeof(bytes)) == RingInvalid);
    value.argument_zero = 0;
    value.flags = 3;
    assert(venus_request_encode(&value, bytes, sizeof(bytes)) == RingInvalid);
    for (uint32_t kind = RequestCapabilities; kind <= RequestPresentPoll; kind++) {
        value = (venus_request_t){.kind = kind, .sequence = 1};
        if (kind == RequestSubmit)
            value.payload_bytes = 8;
        if (kind == RequestNegotiate)
            value.payload_bytes = 160;
        if (kind == RequestReply || kind == RequestRead || kind == RequestWrite)
            value.argument_one = 4;
        if (kind >= RequestCreate && kind <= RequestWrite)
            value.resource_id = 2;
        if (kind == RequestCreate) {
            value.flags = 1;
            value.argument_one = 4096;
        }
        if (kind == RequestWrite)
            value.payload_bytes = 4;
        if (kind == RequestGpuFence || kind == RequestGpuPoll) {
            value.argument_zero = 1;
            if (kind == RequestGpuPoll)
                value.argument_one = 1;
        }
        if (kind == RequestPresent) {
            value.payload_bytes = 1216;
            value.argument_zero = 1;
            value.argument_one = 1;
        }
        if (kind == RequestPresentPoll)
            value.argument_zero = UINT64_MAX;
        assert(venus_request_encode(&value, bytes, sizeof(bytes)) == RingOk);
        assert(venus_request_decode(&decoded, bytes, sizeof(bytes)) == RingOk);
        assert(memcmp(&decoded, &value, sizeof(value)) == 0);
        reject_field(bytes, 28, RequestAgain);
        reject_field(bytes, 36, 8);
        if (kind == RequestSubmit) {
            reject_field(bytes, 24, 4);
            reject_field(bytes, 24, 9);
            reject_field(bytes, 24, VenusRequestMaxPayload + 4);
        }
        if (kind >= RequestCreate && kind <= RequestWrite) {
            reject_field(bytes, 32, 1);
            reject_field(bytes, 32, 66);
        }
        if (kind == RequestCreate) {
            reject_field(bytes, 36, 0);
            reject_field(bytes, 36, 3);
            reject_field(bytes, 48, 0);
            reject_field(bytes, 48, 4097);
            reject_field(bytes, 48, 1073745920);
            // Cross-device device storage requires Share, even with nonzero blob.
            write_word(bytes, 40, 7);
            reject_field(bytes, 36, 4);
            value.argument_zero = 7;
            value.flags = 6;
            assert(venus_request_encode(&value, bytes, sizeof(bytes)) == RingOk);
        }
        if (kind == RequestReply || kind == RequestRead || kind == RequestWrite) {
            reject_field(bytes, 48, 0);
            reject_field(bytes, 48, VenusRequestMaxPayload + 1);
            value.argument_zero = UINT64_MAX - 3;
            assert(venus_request_encode(&value, bytes, sizeof(bytes)) == RingInvalid);
            value.argument_zero--;
            assert(venus_request_encode(&value, bytes, sizeof(bytes)) == RingOk);
        }
        if (kind == RequestGpuFence || kind == RequestGpuPoll) {
            reject_field(bytes, 40, 0);
            reject_field(bytes, 40, 64);
            reject_field(bytes, 44, 1); /* Reject u64 timeline before C narrowing. */
            reject_field(bytes, 24, 4);
            if (kind == RequestGpuFence)
                reject_field(bytes, 48, 1);
            else
                reject_field(bytes, 48, 0);
            value.argument_zero = 63;
            if (kind == RequestGpuPoll)
                value.argument_one = UINT64_MAX;
            assert(venus_request_encode(&value, bytes, sizeof(bytes)) == RingOk);
        }
        if (kind == RequestPresent) {
            reject_field(bytes, 24, 1215);
            reject_field(bytes, 24, 1217);
            reject_field(bytes, 40, 0);
            reject_field(bytes, 40, 64);
            reject_field(bytes, 44, 1);
            reject_field(bytes, 48, 0);
        }
        if (kind == RequestPresentPoll) {
            write_word(bytes, 40, 0);
            write_word(bytes, 44, 0);
            reject_field(bytes, 40, 0);
            write_word(bytes, 40, UINT32_MAX);
            write_word(bytes, 44, UINT32_MAX);
            reject_field(bytes, 48, 1);
            reject_field(bytes, 24, 32);
        }
        for (uint32_t status = RequestSuccess; status <= RequestLimit; status++) {
            value =
                (venus_request_t){.kind = kind, .direction = 1, .sequence = 1, .status = status};
            if (status == RequestSuccess) {
                if (kind == RequestCapabilities)
                    value.payload_bytes = 160;
                if (kind == RequestPresentPoll)
                    value.payload_bytes = 32;
                if (kind == RequestSubmit || kind == RequestGpuFence)
                    value.argument_zero = 1;
                if (kind == RequestRead || kind == RequestReply)
                    value.payload_bytes = 4;
            }
            assert(venus_request_encode(&value, bytes, sizeof(bytes)) == RingOk);
            assert(venus_request_decode(&decoded, bytes, sizeof(bytes)) == RingOk);
            assert(memcmp(&decoded, &value, sizeof(value)) == 0);
            reject_field(bytes, 28, 8);
            reject_field(bytes, 32, 2);
            reject_field(bytes, 36, 1);
            reject_field(bytes, 48, 1);
            if (status != RequestSuccess) {
                reject_field(bytes, 24, 1);
                reject_field(bytes, 40, 1);
            } else if (kind == RequestCapabilities) {
                reject_field(bytes, 24, 159);
                reject_field(bytes, 24, 161);
            } else if (kind == RequestPresentPoll) {
                reject_field(bytes, 24, 31);
                reject_field(bytes, 24, 33);
                reject_field(bytes, 40, 1);
            } else if (kind == RequestSubmit || kind == RequestGpuFence) {
                reject_field(bytes, 40, 0);
                reject_field(bytes, 24, 8);
            } else if (kind == RequestRead || kind == RequestReply) {
                reject_field(bytes, 24, 0);
                reject_field(bytes, 24, VenusRequestMaxPayload + 1);
            } else {
                reject_field(bytes, 24, 4);
            }
        }
    }
    puts("Portable receiver envelope ABI and wire policy passed");
    return 0;
}
