/** @file worker_main.c @brief Trusted isolated Linux receiver executable. */
#include "waddle/venus_frame.h"
#include "waddle/venus_service.h"
#include "waddle/venus_worker.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(__SANITIZE_ADDRESS__)
/** @brief Preserve deep allocation callers when Vulkan modules unload at shutdown.
 * @return Nonnull borrowed static option string; process lifetime, never free.
 * @details Sanitizer diagnostic only: use DWARF stack unwinding on allocation.
 * Leak detection, abort policy and resource lifetime remain controlled by the
 * existing ASAN_OPTIONS and normal worker teardown. Thread-safe; no allocation.
 * Explicit ASAN_OPTIONS overrides this default when that flag is present.
 */
__attribute__((no_sanitize_address))
const char *__asan_default_options(void) {
    return "fast_unwind_on_malloc=0";
}
#endif
static _Atomic uint32_t worker_cancel = 0;
static void cancel_worker(int signal_number) {
    (void)signal_number;
    atomic_store_explicit(&worker_cancel, 1, memory_order_release);
}
int main(int argc, char **argv) {
    int presented = argc == 4 && !strcmp(argv[1], "--venus-worker-presented");
    if (!presented && (argc != 2 || strcmp(argv[1], "--venus-worker")))
        return 2;
    uint64_t context = 0, controller = 0;
    if (presented &&
        (venus_frame_context_decode(&context, argv[2], strnlen(argv[2], 17)) != RingOk ||
         venus_frame_context_decode(&controller, argv[3], strnlen(argv[3], 17)) != RingOk ||
         controller > INT32_MAX || controller != (uint64_t)getppid()))
        return 2;
    sigset_t empty;
    sigemptyset(&empty);
    struct sigaction action = {.sa_handler = cancel_worker};
    sigemptyset(&action.sa_mask);
    if (sigprocmask(SIG_SETMASK, &empty, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0 ||
        sigaction(SIGINT, &action, NULL) != 0)
        return 2;
    if (fcntl(VenusWorkerMappingFd, F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(VenusWorkerStreamFd, F_SETFD, FD_CLOEXEC) != 0 ||
        (presented && fcntl(VenusWorkerFrameFd, F_SETFD, FD_CLOEXEC) != 0))
        return 2;
    venus_service_config_t config;
    (void)venus_service_config_init(&config);
    /* Complete bounded device-extension replies use up to274460 bytes.
     * This trusted executable requests the exact power-of-two mapping;
     * timed ICD binding independently checks its last/outside byte ranges. */
    config.reply_bytes = 524288;
    venus_ring_status_t status = venus_service_run_presented(
        &config, VenusWorkerMappingFd, VenusWorkerStreamFd, presented ? VenusWorkerFrameFd : -1,
        (int32_t)controller, context, &worker_cancel);
    const char *diagnostics = getenv("WADDLE_TCP_DIAGNOSTICS");
    if (diagnostics && !strcmp(diagnostics, "1"))
        fprintf(stderr, "Venus worker service terminated status=%d\n", (int)status);
    if (presented)
        close(VenusWorkerFrameFd);
    close(VenusWorkerMappingFd);
    close(VenusWorkerStreamFd);
    return status == RingClosed || status == RingCancelled ? 0 : 1;
}
