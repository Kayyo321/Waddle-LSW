/** @file command.c @brief Native C ABI/private-buffer command staging fixture. */
#include "waddle/venus_command.h"
#include <assert.h>
#include <string.h>
/** @brief Immutable pinned EnumerateInstanceVersion command, reply flag1. */
static const unsigned char VersionCommand[16] = {137, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0};
static venus_ring_status_t exchange(void *context, const venus_request_t *request,
                                    const void *input, size_t length, venus_request_t *response,
                                    void *output, size_t capacity) {
    unsigned *calls = context;
    (*calls)++;
    *response = (venus_request_t){.kind = request->kind, .direction = 1};
    if (request->kind == RequestSubmit) {
        const unsigned char *bytes = input;
        assert(length == 52 && bytes[0] == 178 && bytes[8] == 1 && bytes[16] == 1);
        assert(bytes[28] == 32 && memcmp(bytes + 36, VersionCommand, 16) == 0);
        response->argument_zero = 1;
    } else if (request->kind == RequestReply) {
        assert(!input && !length && request->argument_one == 32 && capacity == 32);
        memset(output, 0, capacity);
        ((unsigned char *)output)[0] = 137;
        response->payload_bytes = 32;
    } else {
        assert(request->kind == RequestPoll && !input && !length && !output && !capacity);
    }
    return RingOk;
}
/** @brief Exact measured x64 owner ABI, no wire/native pointer casts. */
#if UINTPTR_MAX == UINT64_MAX
_Static_assert(sizeof(venus_command_t) == 80 && _Alignof(venus_command_t) == 8,
               "x64 cursor owner layout");
_Static_assert(offsetof(venus_command_t, reply_offset) == 48 &&
               offsetof(venus_command_t, cpu_fence) == 56 &&
               offsetof(venus_command_t, command_id) == 64 &&
               offsetof(venus_command_t, state) == 68 &&
               offsetof(venus_command_t, lost) == 72, "x64 cursor and phase offsets");
#endif
/** @brief Exclusive borrowed native fixture, no heap or retained buffers. */
typedef struct chunk_fixture_t {
    size_t accepted; /**< Validated successful private bytes. */
    size_t reads; /**< Actual read attempts. */
    unsigned polls; /**< Exact CPU poll count. */
    int pause_pending; /**< One retry at byte4096, then false. */
} chunk_fixture_t;
static venus_ring_status_t chunk_exchange(void *context, const venus_request_t *request,
    const void *input, size_t length, venus_request_t *response, void *output, size_t capacity) {
    chunk_fixture_t *fixture = context;
    *response = (venus_request_t){.kind = request->kind, .direction = 1};
    if (request->kind == RequestSubmit) {
        const unsigned char *bytes = input;
        uint64_t offered = 0;
        assert(length == 52 && !output && !capacity);
        for (size_t index = 0; index < 8; index++) offered |= (uint64_t)bytes[28 + index] << (8 * index);
        assert(offered == 8200 && !request->argument_zero && !request->argument_one);
        response->argument_zero = 7;
        return RingOk;
    }
    assert(!input && !length);
    if (request->kind == RequestPoll) {
        assert(!output && !capacity && !request->argument_zero && !request->argument_one);
        fixture->polls++;
        return RingOk;
    }
    assert(request->kind == RequestReply && output && request->argument_zero == fixture->accepted);
    size_t expected = 8200 - fixture->accepted;
    if (expected > 4096) expected = 4096;
    assert(capacity == expected && request->argument_one == expected);
    fixture->reads++;
    if (fixture->accepted == 4096 && fixture->pause_pending) {
        memset(output, 0xa5, capacity);
        fixture->pause_pending = 0;
        return RingAgain;
    }
    unsigned char *bytes = output;
    for (size_t index = 0; index < capacity; index++) {
        size_t absolute = fixture->accepted + index;
        bytes[index] = absolute < 4 ? (absolute == 0 ? 137 : 0) : (unsigned char)(absolute * 7 + 3);
    }
    fixture->accepted += capacity;
    response->payload_bytes = (uint32_t)capacity;
    return RingOk;
}
static void chunk_native_check(void) {
    venus_command_t owner = {0};
    chunk_fixture_t fixture = {.pause_pending = 1};
    unsigned char tx[128], storage[8216];
    memset(storage, 0xcc, sizeof(storage));
    assert(venus_command_init(&owner, chunk_exchange, &fixture, tx, sizeof(tx), storage + 8, 8200) == RingOk);
    assert(venus_command_start(&owner, VersionCommand, sizeof(VersionCommand)) == RingOk);
    const void *view = VersionCommand;
    size_t length = sizeof(VersionCommand);
    assert(venus_command_poll(&owner) == RingAgain && owner.state == CommandReading && owner.reply_offset == 4096);
    assert(venus_command_take(&owner, &view, &length) == RingAgain && !view && !length);
    assert(venus_command_poll(&owner) == RingAgain && owner.reply_offset == 4096);
    assert(venus_command_start(&owner, VersionCommand, sizeof(VersionCommand)) == RingAgain);
    assert(venus_command_take(&owner, &view, &length) == RingAgain && !view && !length);
    assert(venus_command_poll(&owner) == RingAgain && owner.reply_offset == 8192);
    assert(venus_command_poll(&owner) == RingOk && owner.state == CommandReady && owner.reply_offset == 8200);
    assert(venus_command_take(&owner, &view, &length) == RingOk && view == storage + 8 && length == 8200);
    assert(owner.state == CommandIdle && !owner.reply_offset && !owner.cpu_fence && !owner.command_id);
    for (size_t index = 0; index < 8200; index++) {
        unsigned char expected = index < 4 ? (index == 0 ? 137 : 0) : (unsigned char)(index * 7 + 3);
        assert(storage[8 + index] == expected);
    }
    for (size_t index = 0; index < 8; index++) assert(storage[index] == 0xcc && storage[8208 + index] == 0xcc);
    assert(fixture.polls == 1 && fixture.reads == 4);
    venus_command_free(&owner);
    assert(!owner.exchange && !owner.context && !owner.tx && !owner.rx && !owner.reply_offset && !owner.cpu_fence && !owner.command_id && !owner.state && !owner.lost);
}
int main(void) {
    chunk_native_check();
    venus_command_t owner = {0};
    unsigned char tx[128], rx[32];
    unsigned calls = 0;
    assert(venus_command_init(&owner, exchange, &calls, tx, sizeof(tx), rx, sizeof(rx)) == RingOk);
    assert(venus_command_start(&owner, VersionCommand, sizeof(VersionCommand)) == RingOk);
    assert(venus_command_poll(&owner) == RingOk);
    const void *reply = NULL;
    size_t length = 0;
    assert(venus_command_take(&owner, &reply, &length) == RingOk);
    assert(reply == rx && length == sizeof(rx) && calls == 3);
    assert(venus_command_take(&owner, &reply, &length) == RingInvalid);
    assert(!reply && !length);
    venus_command_free(&owner);
    assert(!owner.exchange && !owner.context && !owner.tx && !owner.rx);
    return 0;
}
