/** @file service_owner.c @brief Service acquisition and ownership fault boundary. */
#include "waddle/venus_export.h"
#include "waddle/venus_service.h"
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>

static int fault, mapped, allocated, live_receiver, attempts, dispatches;
static int receiver_cookie;
static _Alignas(64) unsigned char mapping[4096];
static int fixture_flags(int fd, int command, ...) {
    assert(command == F_GETFD);
    return fault == 18 ? 0 : fd < 0 ? -1 : FD_CLOEXEC;
}
static int fixture_stat(int fd, struct stat *metadata) {
    assert(fd == 3);
    memset(metadata, 0, sizeof(*metadata));
    metadata->st_mode = fault == 19 ? S_IFCHR : S_IFREG;
    metadata->st_size = fault == 2 ? 4095 : fault == 3 ? 1073741825 : fault == 12 ? 4097 : 4096;
    return fault == 1 ? -1 : 0;
}
static void *fixture_map(void *address, size_t bytes, int protection, int flags, int fd,
                         off_t offset) {
    assert(!address && bytes == 4096 && protection == (PROT_READ | PROT_WRITE) &&
           flags == MAP_SHARED && fd == 3 && !offset);
    if (fault == 13)
        return MAP_FAILED;
    mapped = 1;
    assert(venus_region_init(mapping, sizeof(mapping), 64) == RingOk);
    if (fault == 16)
        mapping[0] ^= 1;
    if (fault == 17)
        atomic_store(&((venus_ring_header_t *)(void *)(mapping + 64))->tail, 1);
    return mapping;
}
static int fixture_unmap(void *address, size_t bytes) {
    assert(mapped && address == mapping && bytes == sizeof(mapping));
    mapped = 0;
    return 0;
}
static ssize_t fixture_random(void *output, size_t bytes, unsigned flags) {
    assert(bytes == 8 && flags == GRND_NONBLOCK);
    attempts++;
    uint64_t identity = fault == 15 ? 0 : 1;
    memcpy(output, &identity, bytes);
    return fault == 14 ? -1 : 8;
}
static void *fixture_allocate(size_t alignment, size_t bytes) {
    assert(alignment == 64 && bytes >= 256);
    if (fault == 4)
        return NULL;
    void *output = aligned_alloc(alignment, bytes);
    assert(output);
    allocated++;
    return output;
}
static void fixture_free(void *memory) {
    if (memory) {
        assert(allocated == 1);
        allocated--;
        free(memory);
    }
}
venus_ring_status_t venus_receiver_create(venus_receiver_t **receiver, uint32_t commands,
                                          uint64_t reply) {
    assert(!*receiver && commands >= 64 && reply >= 4096);
    if (fault == 5)
        return RingCorrupt;
    *receiver = (venus_receiver_t *)(void *)&receiver_cookie;
    live_receiver = 1;
    return RingOk;
}
void venus_receiver_destroy(venus_receiver_t **receiver) {
    if (*receiver) {
        assert(live_receiver);
        live_receiver = 0;
        *receiver = NULL;
    }
}
venus_ring_status_t venus_receiver_resource_limits(venus_receiver_t *receiver, uint32_t count,
                                                   uint64_t bytes) {
    assert(receiver && count && bytes);
    return fault == 6 ? RingCorrupt : RingOk;
}
venus_ring_status_t venus_receiver_timeouts(venus_receiver_t *receiver, uint32_t cpu,
                                            uint32_t gpu) {
    assert(receiver && cpu && gpu);
    return fault == 7 ? RingCorrupt : RingOk;
}
venus_ring_status_t venus_channel_init(venus_channel_t *channel, venus_session_t *session,
                                       intptr_t stream, const _Atomic uint32_t *cancel) {
    assert(stream == 4 && !cancel);
    if (fault == 8)
        return RingInvalid;
    channel->session = session;
    channel->initialized = 1;
    return RingOk;
}
venus_ring_status_t venus_channel_deadline(venus_channel_t *channel, uint32_t timeout) {
    assert(channel->initialized && timeout);
    return fault == 9 ? RingClosed : RingOk;
}
/* This host-owner fixture exercises only legacy relative service RPC. New
 * absolute RPC symbols must link, but any accidental call fails the fixture. */
uint64_t venus_channel_time_ms(void) {
    abort();
}
venus_ring_status_t venus_channel_deadline_until(venus_channel_t *channel, uint64_t deadline_ms) {
    (void)channel;
    (void)deadline_ms;
    abort();
}
venus_ring_status_t venus_channel_handshake(venus_channel_t *channel) {
    if (fault == 10)
        return RingTimeout;
    channel->session->state = SessionReady;
    return RingOk;
}
void venus_channel_free(venus_channel_t *channel) { memset(channel, 0, sizeof(*channel)); }
venus_ring_status_t venus_channel_wait(void *context) {
    (void)context;
    return RingOk;
}
static venus_ring_status_t fixture_rpc(venus_rpc_t *rpc, venus_channel_t *channel, void *bytes,
                                       uint32_t size) {
    return fault == 11 ? RingInvalid : venus_rpc_init(rpc, channel, bytes, size);
}
venus_ring_status_t venus_dispatch_serve(venus_rpc_t *rpc, venus_receiver_t *receiver,
                                         uint32_t timeout) {
    assert(rpc->channel && receiver && timeout);
    return dispatches++ ? RingClosed : RingOk;
}
static int export_live, export_inits, export_frees, export_callbacks, take_fault;
venus_ring_status_t venus_export_init(venus_export_t *owner, venus_receiver_t *receiver, int fd,
                                      int32_t pid, uint64_t context) {
    assert(!owner->receiver && receiver && fd == 5 && pid == 23 && context == 17);
    export_inits++;
    if (fault == 20)
        return RingInvalid;
    owner->receiver = receiver;
    export_live = 1;
    return RingOk;
}
void venus_export_free(venus_export_t *owner) {
    if (owner->receiver) {
        assert(export_live && live_receiver);
        export_live = 0;
        export_frees++;
    }
    memset(owner, 0, sizeof(*owner));
}
venus_ring_status_t venus_export_submit(venus_export_t *owner, const void *bytes, size_t length,
                                        uint32_t timeline, uint64_t fence) {
    assert(export_live && owner->receiver && bytes && length == 1216 && timeline == 1 &&
           fence == 1);
    export_callbacks++;
    return RingAgain;
}
venus_ring_status_t venus_export_take(venus_export_t *owner, uint64_t frame,
                                      venus_release_t *release) {
    assert(export_live && owner->receiver && frame == 1 && release);
    export_callbacks++;
    if (take_fault == 1)
        return RingAgain;
    *release = (venus_release_t){
        .context = 17, .frame = 1, .status = take_fault == 2 ? RingCorrupt : RingOk};
    return RingOk;
}
int venus_export_resource_busy(const venus_export_t *owner, uint32_t id) {
    assert(export_live && owner->receiver && id == 2);
    export_callbacks++;
    return 1;
}
venus_ring_status_t venus_export_pump(venus_export_t *owner) {
    assert(export_live && owner->receiver);
    export_callbacks++;
    return RingOk;
}
venus_ring_status_t
venus_dispatch_serve_presented(venus_rpc_t *rpc, venus_receiver_t *receiver,
                               const venus_dispatch_presentation_t *presentation,
                               uint32_t timeout) {
    if (presentation) {
        unsigned char frame[1216] = {0}, bytes[32];
        assert(presentation->submit(presentation->context, frame, sizeof(frame), 1, 1) ==
               RingAgain);
        assert(presentation->resource_busy(presentation->context, 2));
        assert(presentation->pump(presentation->context) == RingOk);
        venus_ring_status_t status =
            presentation->take(presentation->context, 1, bytes, sizeof(bytes));
        assert(status == (take_fault == 1 ? RingAgain : take_fault == 2 ? RingCorrupt : RingOk));
        if (status == RingOk) {
            venus_release_t release;
            assert(venus_release_decode(&release, bytes, sizeof(bytes)) == RingOk);
            assert(release.context == 17 && release.frame == 1 && release.status == RingOk);
        }
    }
    return venus_dispatch_serve(rpc, receiver, timeout);
}
#define fcntl fixture_flags
#define fstat fixture_stat
#define mmap fixture_map
#define munmap fixture_unmap
#define getrandom fixture_random
#define aligned_alloc fixture_allocate
#define free fixture_free
#define venus_rpc_init fixture_rpc
#include "venus_service.c"

int main(void) {
    venus_service_config_t config;
    assert(venus_service_config_init(NULL) == RingInvalid);
    assert(venus_service_config_init(&config) == RingOk);
    /* Full Vulkan inline update plus Venus command/reply-stream headers must fit. */
    assert(config.command_bytes >= 65536 + 48 + 36);
    assert(venus_service_run(NULL, 3, 4, NULL) == RingInvalid);
    assert(venus_service_run(&config, -1, 4, NULL) == RingInvalid);
    assert(venus_service_run(&config, 3, -1, NULL) == RingInvalid);
    assert(venus_service_run_presented(&config, 3, 4, -2, 0, 0, NULL) == RingInvalid);
    assert(venus_service_run_presented(&config, 3, 4, -1, 23, 0, NULL) == RingInvalid);
    assert(venus_service_run_presented(&config, 3, 4, -1, 0, 17, NULL) == RingInvalid);
    assert(venus_service_run_presented(&config, 3, 4, 5, 0, 17, NULL) == RingInvalid);
    assert(venus_service_run_presented(&config, 3, 4, 5, 23, 0, NULL) == RingInvalid);
    assert(venus_service_run_presented(&config, 3, 4, 3, 23, 17, NULL) == RingInvalid);
    assert(venus_service_run_presented(&config, 3, 4, 4, 23, 17, NULL) == RingInvalid);
    fault = 20;
    assert(venus_service_run_presented(&config, 3, 4, 5, 23, 17, NULL) == RingInvalid);
    assert(export_inits == 1 && !export_live && !export_frees && !mapped && !allocated &&
           !live_receiver);
    fault = 0;
    for (take_fault = 0; take_fault < 3; take_fault++) {
        dispatches = 0;
        assert(venus_service_run_presented(&config, 3, 4, 5, 23, 17, NULL) == RingClosed);
        assert(!export_live && !mapped && !allocated && !live_receiver);
    }
    assert(export_inits == 4 && export_frees == 3 && export_callbacks == 24);
    for (int mode = 0; mode < 12; mode++) {
        venus_service_config_t bad = config;
        switch (mode) {
        case 0:
            bad.command_bytes = 0;
            break;
        case 1:
            bad.reply_bytes = 0;
            break;
        case 2:
            bad.resource_count = 0;
            break;
        case 3:
            bad.resource_count = 65;
            break;
        case 4:
            bad.resource_bytes = 4095;
            break;
        case 5:
            bad.resource_bytes = UINT64_MAX;
            break;
        case 6:
            bad.cpu_timeout_ms = 0;
            break;
        case 7:
            bad.cpu_timeout_ms = 60001;
            break;
        case 8:
            bad.gpu_timeout_ms = 0;
            break;
        case 9:
            bad.gpu_timeout_ms = 60001;
            break;
        case 10:
            bad.operation_timeout_ms = 0;
            break;
        default:
            bad.operation_timeout_ms = 60001;
        }
        assert(venus_service_run(&bad, 3, 4, NULL) == RingInvalid);
    }
    for (int stage = 0; stage <= 19; stage++) {
        fault = stage;
        attempts = dispatches = 0;
        venus_ring_status_t status = venus_service_run(&config, 3, 4, NULL);
        assert(status == (stage == 0 ? RingClosed
                          : stage == 1 || stage == 2 || stage == 3 || stage == 8 || stage == 11 ||
                                  stage == 12 || stage == 18 || stage == 19
                              ? RingInvalid
                          : stage == 9  ? RingClosed
                          : stage == 10 ? RingTimeout
                                        : RingCorrupt));
        assert(!mapped && !allocated && !live_receiver);
        if (stage == 15)
            assert(attempts == 4);
    }
    fault = 0;
    config.command_bytes = 64;
    assert(venus_service_run(&config, 3, 4, NULL) == RingClosed);
    assert(!mapped && !allocated && !live_receiver);
    return 0;
}
