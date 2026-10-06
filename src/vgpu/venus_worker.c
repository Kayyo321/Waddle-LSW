/** @file venus_worker.c @brief Collision-safe exec and bounded group cleanup. */
#include "waddle/venus_worker.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
static int signal_group(venus_worker_t *worker, int signal_number) {
    return kill(-worker->process_id, signal_number) == 0 || errno == ESRCH;
}
static int clock_ms(uint64_t *value) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 ||
        (uint64_t)now.tv_sec > (UINT64_MAX - 999) / 1000)
        return 0;
    *value = (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
    return 1;
}
venus_ring_status_t venus_worker_create(venus_worker_t *worker, const char *path, int mapping_fd,
                                        int stream_fd) {
    struct stat metadata;
    int type = 0;
    socklen_t type_bytes = sizeof(type);
    int flags = fcntl(stream_fd, F_GETFL);
    if (!worker || worker->process_id || worker->exited || !path || path[0] != '/' ||
        fstat(mapping_fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) || flags < 0 ||
        !(flags & O_NONBLOCK) ||
        getsockopt(stream_fd, SOL_SOCKET, SO_TYPE, &type, &type_bytes) != 0 || type != SOCK_STREAM)
        return RingInvalid;
    int mapping_copy = fcntl(mapping_fd, F_DUPFD_CLOEXEC, 64);
    if (mapping_copy < 0)
        return RingCorrupt;
    int stream_copy = fcntl(stream_fd, F_DUPFD_CLOEXEC, 64);
    if (stream_copy < 0) {
        close(mapping_copy);
        return RingCorrupt;
    }
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    int actions_ready = 0, attributes_ready = 0;
    venus_ring_status_t result = RingCorrupt;
    if (posix_spawn_file_actions_init(&actions) != 0)
        goto cleanup;
    actions_ready = 1;
    if (posix_spawnattr_init(&attributes) != 0)
        goto cleanup;
    attributes_ready = 1;
    if (posix_spawn_file_actions_adddup2(&actions, mapping_copy, VenusWorkerMappingFd) != 0 ||
        posix_spawn_file_actions_adddup2(&actions, stream_copy, VenusWorkerStreamFd) != 0 ||
        posix_spawn_file_actions_addclosefrom_np(&actions, 5) != 0 ||
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP) != 0 ||
        posix_spawnattr_setpgroup(&attributes, 0) != 0)
        goto cleanup;
    char *arguments[] = {(char *)path, "--venus-worker", NULL};
    pid_t process = 0;
    if (posix_spawn(&process, path, &actions, &attributes, arguments, environ) != 0)
        goto cleanup;
    *worker = (venus_worker_t){.process_id = process};
    result = RingOk;
cleanup:
    if (attributes_ready)
        posix_spawnattr_destroy(&attributes);
    if (actions_ready)
        posix_spawn_file_actions_destroy(&actions);
    close(stream_copy);
    close(mapping_copy);
    return result;
}
venus_ring_status_t venus_worker_poll(venus_worker_t *worker) {
    if (!worker)
        return RingInvalid;
    if (!worker->process_id)
        return worker->exited ? RingClosed : RingInvalid;
    siginfo_t information = {0};
    if (waitid(P_PID, (id_t)worker->process_id, &information, WEXITED | WNOWAIT | WNOHANG) != 0) {
        if (errno == EINTR)
            return RingAgain;
        if (errno == ECHILD) {
            worker->process_id = 0;
            worker->exited = 1;
            worker->exit_status = -1;
        }
        return RingCorrupt;
    }
    if (!information.si_pid)
        return RingAgain;
    if (!signal_group(worker, SIGKILL))
        return RingCorrupt;
    int status = 0;
    pid_t reaped = waitpid(worker->process_id, &status, WNOHANG);
    if (reaped == 0 || (reaped < 0 && errno == EINTR))
        return RingAgain;
    if (reaped < 0) {
        if (errno == ECHILD) {
            worker->process_id = 0;
            worker->exited = 1;
            worker->exit_status = -1;
        }
        return RingCorrupt;
    }
    worker->process_id = 0;
    worker->exited = 1;
    worker->exit_status = status;
    return RingClosed;
}
venus_ring_status_t venus_worker_destroy(venus_worker_t *worker, uint32_t timeout_ms) {
    if (!worker || !worker->process_id) {
        if (worker)
            memset(worker, 0, sizeof(*worker));
        return RingOk;
    }
    if (!timeout_ms || timeout_ms > 60000)
        return RingInvalid;
    uint64_t start, now;
    if (!worker->terminating) {
        if (!signal_group(worker, SIGTERM))
            return RingCorrupt;
        worker->terminating = 1;
    }
    if (!clock_ms(&start) || start > UINT64_MAX - timeout_ms) {
        if (signal_group(worker, SIGKILL))
            worker->terminating = 2;
        return RingCorrupt;
    }
    uint64_t grace = timeout_ms / 2;
    if (grace > 100)
        grace = 100;
    if (worker->terminating == 2)
        grace = 0;
    for (;;) {
        venus_ring_status_t result = venus_worker_poll(worker);
        if (result == RingClosed) {
            memset(worker, 0, sizeof(*worker));
            return RingOk;
        }
        if (result != RingAgain)
            return result;
        if (!clock_ms(&now) || now < start)
            return RingCorrupt;
        if (worker->terminating != 2 && now - start >= grace) {
            if (!signal_group(worker, SIGKILL))
                return RingCorrupt;
            worker->terminating = 2;
        }
        if (now - start >= timeout_ms)
            return RingTimeout;
        const struct timespec Pause = {0, 1000000};
        (void)nanosleep(&Pause, NULL);
    }
}
