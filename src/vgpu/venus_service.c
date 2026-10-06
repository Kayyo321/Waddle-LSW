/** @file venus_service.c @brief Symmetric isolated receiver run ownership. */
#include "waddle/venus_service.h"
#include "venus_receiver_bounds.h"
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>

venus_ring_status_t venus_service_config_init(venus_service_config_t *config) {
    if (!config)
        return RingInvalid;
    *config = (venus_service_config_t){.command_bytes = 65536,
                                       .reply_bytes = 65536,
                                       .resource_count = VenusReceiverMaxResources,
                                       .resource_bytes = VenusReceiverDefaultResourceBytes,
                                       .cpu_timeout_ms = 5000,
                                       .gpu_timeout_ms = 5000,
                                       .operation_timeout_ms = 5000};
    return RingOk;
}
static int valid_timeout(uint32_t timeout) { return timeout && timeout <= 60000; }
venus_ring_status_t venus_service_run(const venus_service_config_t *config, int mapping_fd,
                                      int stream_fd, const _Atomic uint32_t *cancel) {
    if (!config || !venus_receiver_limits(config->command_bytes, config->reply_bytes) ||
        !config->resource_count || config->resource_count > VenusReceiverMaxResources ||
        config->resource_bytes < 4096 || config->resource_bytes > 1073741824 ||
        !valid_timeout(config->cpu_timeout_ms) || !valid_timeout(config->gpu_timeout_ms) ||
        !valid_timeout(config->operation_timeout_ms))
        return RingInvalid;
    int mapping_flags = fcntl(mapping_fd, F_GETFD), stream_flags = fcntl(stream_fd, F_GETFD);
    if (mapping_flags < 0 || stream_flags < 0 || !(mapping_flags & FD_CLOEXEC) ||
        !(stream_flags & FD_CLOEXEC))
        return RingInvalid;
    struct stat metadata;
    if (fstat(mapping_fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_size < 4096 || metadata.st_size > 1073741824 ||
        ((uint64_t)metadata.st_size & ((uint64_t)metadata.st_size - 1)))
        return RingInvalid;
    size_t mapping_bytes = (size_t)metadata.st_size;
    void *mapping = mmap(NULL, mapping_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, mapping_fd, 0);
    if (mapping == MAP_FAILED)
        return RingCorrupt;
    venus_ring_status_t status = RingCorrupt;
    venus_region_view_t attached = {0};
    venus_session_t session = {0};
    venus_channel_t channel = {0};
    venus_rpc_t rpc = {0};
    venus_receiver_t *receiver = NULL;
    void *private_bytes = NULL;
    if (venus_region_attach(&attached, mapping, mapping_bytes) != RingOk)
        goto cleanup;
    uint64_t identity = 0;
    for (unsigned attempt = 0; attempt < 4 && !identity; attempt++)
        if (getrandom(&identity, sizeof(identity), GRND_NONBLOCK) != sizeof(identity))
            goto cleanup;
    if (!identity ||
        venus_session_init(&session, SessionHost, mapping, mapping_bytes, identity) != RingOk)
        goto cleanup;
    uint32_t transfer_bytes = config->command_bytes < 256 ? 256 : config->command_bytes;
    private_bytes = aligned_alloc(64, transfer_bytes);
    if (!private_bytes ||
        venus_receiver_create(&receiver, config->command_bytes, config->reply_bytes) != RingOk)
        goto cleanup;
    if (venus_receiver_resource_limits(receiver, config->resource_count, config->resource_bytes) !=
            RingOk ||
        venus_receiver_timeouts(receiver, config->cpu_timeout_ms, config->gpu_timeout_ms) != RingOk)
        goto cleanup;
    status = venus_channel_init(&channel, &session, stream_fd, cancel);
    if (status != RingOk)
        goto cleanup;
    status = venus_channel_deadline(&channel, config->operation_timeout_ms);
    if (status != RingOk)
        goto cleanup;
    status = venus_channel_handshake(&channel);
    if (status != RingOk)
        goto cleanup;
    status = venus_rpc_init(&rpc, &channel, private_bytes, transfer_bytes);
    if (status != RingOk)
        goto cleanup;
    do {
        status = venus_dispatch_serve(&rpc, receiver, config->operation_timeout_ms);
    } while (status == RingOk);
cleanup:
    venus_session_close(&session, StopDisconnect);
    venus_ring_close(&attached.commands);
    venus_ring_close(&attached.replies);
    venus_rpc_free(&rpc);
    venus_channel_free(&channel);
    venus_receiver_destroy(&receiver);
    venus_region_detach(&session.region);
    venus_region_detach(&attached);
    free(private_bytes);
    private_bytes = NULL;
    munmap(mapping, mapping_bytes);
    return status;
}
