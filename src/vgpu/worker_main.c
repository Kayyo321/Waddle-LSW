/** @file worker_main.c @brief Trusted isolated Linux receiver executable. */
#include "waddle/venus_service.h"
#include "waddle/venus_worker.h"
#include <signal.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
static _Atomic uint32_t worker_cancel = 0;
static void cancel_worker(int signal_number) {
    (void)signal_number;
    atomic_store_explicit(&worker_cancel, 1, memory_order_release);
}
int main(int argc, char **argv) {
    if (argc != 2 || strcmp(argv[1], "--venus-worker"))
        return 2;
    sigset_t empty;
    sigemptyset(&empty);
    struct sigaction action = {.sa_handler = cancel_worker};
    sigemptyset(&action.sa_mask);
    if (sigprocmask(SIG_SETMASK, &empty, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0 ||
        sigaction(SIGINT, &action, NULL) != 0)
        return 2;
    if (fcntl(VenusWorkerMappingFd, F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(VenusWorkerStreamFd, F_SETFD, FD_CLOEXEC) != 0)
        return 2;
    venus_service_config_t config;
    (void)venus_service_config_init(&config);
    venus_ring_status_t status =
        venus_service_run(&config, VenusWorkerMappingFd, VenusWorkerStreamFd, &worker_cancel);
    close(VenusWorkerMappingFd);
    close(VenusWorkerStreamFd);
    return status == RingClosed || status == RingCancelled ? 0 : 1;
}
