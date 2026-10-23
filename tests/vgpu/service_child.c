/** @file service_child.c @brief Real isolated service with deterministic test policy. */
#include "waddle/venus_service.h"
#include "waddle/venus_worker.h"
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc != 2 || strcmp(argv[1], "--venus-worker"))
        return 2;
    if (fcntl(VenusWorkerMappingFd, F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(VenusWorkerStreamFd, F_SETFD, FD_CLOEXEC) != 0)
        return 2;
    venus_service_config_t config;
    (void)venus_service_config_init(&config);
    config.command_bytes = config.reply_bytes = 4096;
    config.resource_count = 1;
    config.resource_bytes = 4096;
    venus_ring_status_t status =
        venus_service_run(&config, VenusWorkerMappingFd, VenusWorkerStreamFd, NULL);
    close(VenusWorkerMappingFd);
    close(VenusWorkerStreamFd);
    return status == RingClosed ? 0 : 1;
}
