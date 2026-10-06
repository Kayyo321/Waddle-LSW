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
int main(void) {
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
