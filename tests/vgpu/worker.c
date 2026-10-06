/** @file worker.c @brief Real exec/group shutdown and injected ownership faults. */
#include "waddle/venus_worker.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fault, duplicates, dup_actions, clock_calls;
static int copied[2];
static int fixture_fcntl(int fd, int command, ...) {
    if (command != F_DUPFD_CLOEXEC)
        return fcntl(fd, command);
    va_list arguments;
    va_start(arguments, command);
    int minimum = va_arg(arguments, int);
    va_end(arguments);
    duplicates++;
    if (fault == duplicates) {
        errno = EMFILE;
        return -1;
    }
    int result = fcntl(fd, command, minimum);
    assert(duplicates <= 2);
    copied[duplicates - 1] = result;
    return result;
}
static int fixture_actions(posix_spawn_file_actions_t *actions) {
    return fault == 3 ? ENOMEM : posix_spawn_file_actions_init(actions);
}
static int fixture_attributes(posix_spawnattr_t *attributes) {
    return fault == 4 ? ENOMEM : posix_spawnattr_init(attributes);
}
static int fixture_dup(posix_spawn_file_actions_t *actions, int source, int target) {
    dup_actions++;
    return fault == 4 + dup_actions ? ENOMEM
                                    : posix_spawn_file_actions_adddup2(actions, source, target);
}
static int fixture_closefrom(posix_spawn_file_actions_t *actions, int start) {
    return fault == 7 ? ENOMEM : posix_spawn_file_actions_addclosefrom_np(actions, start);
}
static int fixture_flags(posix_spawnattr_t *attributes, short flags) {
    return fault == 8 ? EINVAL : posix_spawnattr_setflags(attributes, flags);
}
static int fixture_group(posix_spawnattr_t *attributes, pid_t group) {
    return fault == 9 ? EINVAL : posix_spawnattr_setpgroup(attributes, group);
}
static int fixture_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                         const posix_spawnattr_t *attributes, char *const argv[],
                         char *const env[]) {
    return fault == 10 ? EIO : posix_spawn(pid, path, actions, attributes, argv, env);
}
static int fixture_waitid(idtype_t type, id_t id, siginfo_t *info, int options) {
    if (fault >= 11 && fault <= 13) {
        errno = fault == 11 ? EINTR : fault == 12 ? ECHILD : EIO;
        return -1;
    }
    return waitid(type, id, info, options);
}
static pid_t fixture_waitpid(pid_t pid, int *status, int options) {
    if (fault == 14)
        return 0;
    if (fault == 21) {
        errno = EINTR;
        return -1;
    }
    if (fault == 22) {
        assert(waitpid(pid, status, options) == pid);
        errno = ECHILD;
        return -1;
    }
    if (fault == 23) {
        errno = EIO;
        return -1;
    }
    return waitpid(pid, status, options);
}
static int fixture_kill(pid_t pid, int signal_number) {
    if (fault == 16) {
        errno = ESRCH;
        return -1;
    }
    if (fault == 15) {
        errno = EPERM;
        return -1;
    }
    return kill(pid, signal_number);
}
static int fixture_clock(clockid_t clock, struct timespec *now) {
    clock_calls++;
    if (fault == 17)
        return -1;
    int result = clock_gettime(clock, now);
    if (fault == 18 && clock_calls >= 2)
        now->tv_sec--;
    if (fault == 19)
        now->tv_sec = (time_t)(UINT64_MAX / 1000 + 1);
    if (fault == 24)
        now->tv_sec = -1;
    if (fault == 20 && clock_calls >= 2)
        now->tv_sec++;
    return result;
}
#define fcntl fixture_fcntl
#define posix_spawn_file_actions_init fixture_actions
#define posix_spawnattr_init fixture_attributes
#define posix_spawn_file_actions_adddup2 fixture_dup
#define posix_spawn_file_actions_addclosefrom_np fixture_closefrom
#define posix_spawnattr_setflags fixture_flags
#define posix_spawnattr_setpgroup fixture_group
#define posix_spawn fixture_spawn
#define waitid fixture_waitid
#define waitpid fixture_waitpid
#define kill fixture_kill
#define clock_gettime fixture_clock
#include "venus_worker.c"
#undef fcntl
#undef waitid
#undef waitpid
#undef kill
#undef clock_gettime

static int mapping_fd, sockets[2];
static void fixture_open(unsigned char mode) {
    duplicates = dup_actions = clock_calls = 0;
    copied[0] = copied[1] = -1;
    mapping_fd = memfd_create("waddle_worker_test", MFD_CLOEXEC);
    assert(mapping_fd >= 0 && ftruncate(mapping_fd, 4096) == 0);
    assert(pwrite(mapping_fd, &mode, 1, 0) == 1);
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets) == 0);
}
static void fixture_close(void) {
    assert(fcntl(mapping_fd, F_GETFD) >= 0 && fcntl(sockets[1], F_GETFD) >= 0);
    for (int index = 0; index < 2; index++)
        if (copied[index] >= 0)
            assert(fcntl(copied[index], F_GETFD) < 0 && errno == EBADF);
    close(mapping_fd);
    close(sockets[0]);
    close(sockets[1]);
}
static void ready(void) {
    struct pollfd descriptor = {.fd = sockets[0], .events = POLLIN};
    assert(poll(&descriptor, 1, 2000) == 1);
    unsigned char value;
    assert(read(sockets[0], &value, 1) == 1 && value == 'R');
}
static int child_main(void) {
    assert(getpgrp() == getpid());
    assert(fcntl(64, F_GETFD) < 0 && errno == EBADF);
    unsigned char mode;
    assert(pread(VenusWorkerMappingFd, &mode, 1, 0) == 1);
    if (mode) {
        assert(signal(SIGTERM, SIG_IGN) != SIG_ERR);
        pid_t descendant = fork();
        assert(descendant >= 0);
        if (!descendant)
            for (;;)
                pause();
    }
    assert(write(VenusWorkerStreamFd, "R", 1) == 1);
    if (mode)
        for (;;)
            pause();
    return 0;
}
static void launch(venus_worker_t *worker, unsigned char mode) {
    fault = 0;
    fixture_open(mode);
    assert(venus_worker_create(worker, "/proc/self/exe", mapping_fd, sockets[1]) == RingOk);
    ready();
}
int main(int argc, char **argv) {
    alarm(30);
    if (argc == 2 && !strcmp(argv[1], "--venus-worker"))
        return child_main();
    venus_worker_t worker = {0};
    assert(venus_worker_poll(NULL) == RingInvalid);
    assert(venus_worker_poll(&worker) == RingInvalid);
    assert(venus_worker_destroy(NULL, 1) == RingOk);
    fixture_open(0);
    assert(venus_worker_create(NULL, "/proc/self/exe", mapping_fd, sockets[1]) == RingInvalid);
    assert(venus_worker_create(&worker, NULL, mapping_fd, sockets[1]) == RingInvalid);
    assert(venus_worker_create(&worker, "relative", mapping_fd, sockets[1]) == RingInvalid);
    assert(venus_worker_create(&worker, "/proc/self/exe", -1, sockets[1]) == RingInvalid);
    assert(venus_worker_create(&worker, "/proc/self/exe", mapping_fd, -1) == RingInvalid);
    assert(venus_worker_create(&worker, "/proc/self/exe", sockets[0], sockets[1]) == RingInvalid);
    assert(fcntl(sockets[1], F_SETFL, 0) == 0);
    assert(venus_worker_create(&worker, "/proc/self/exe", mapping_fd, sockets[1]) == RingInvalid);
    int datagrams[2];
    assert(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0, datagrams) == 0);
    assert(venus_worker_create(&worker, "/proc/self/exe", mapping_fd, datagrams[1]) == RingInvalid);
    close(datagrams[0]);
    close(datagrams[1]);
    fixture_close();
    for (int stage = 1; stage <= 10; stage++) {
        fixture_open(0);
        fault = stage;
        assert(venus_worker_create(&worker, "/proc/self/exe", mapping_fd, sockets[1]) ==
               RingCorrupt);
        assert(worker.process_id == 0);
        fixture_close();
    }
    launch(&worker, 0);
    siginfo_t observed;
    assert(waitid(P_PID, (id_t)worker.process_id, &observed, WEXITED | WNOWAIT) == 0);
    fault = 14;
    assert(venus_worker_poll(&worker) == RingAgain && worker.process_id > 0);
    fault = 21;
    assert(venus_worker_poll(&worker) == RingAgain && worker.process_id > 0);
    fault = 23;
    assert(venus_worker_poll(&worker) == RingCorrupt && worker.process_id > 0);
    fault = 15;
    assert(venus_worker_poll(&worker) == RingCorrupt && worker.process_id > 0);
    fault = 16;
    assert(venus_worker_poll(&worker) == RingClosed && WIFEXITED(worker.exit_status));
    assert(venus_worker_poll(&worker) == RingClosed);
    assert(venus_worker_create(&worker, "/proc/self/exe", mapping_fd, sockets[1]) == RingInvalid);
    assert(venus_worker_destroy(&worker, 1000) == RingOk);
    fixture_close();
    for (int stage = 0; stage <= 24; stage++) {
        if (stage && (stage < 11 || stage == 12 || stage == 14 || stage == 16 || stage == 21 ||
                      stage == 22 || stage == 23))
            continue;
        launch(&worker, 1);
        assert(venus_worker_poll(&worker) == RingAgain);
        assert(venus_worker_create(&worker, "/proc/self/exe", mapping_fd, sockets[1]) ==
               RingInvalid);
        assert(venus_worker_destroy(&worker, 0) == RingInvalid);
        assert(venus_worker_destroy(&worker, 60001) == RingInvalid);
        fault = stage;
        if (stage == 11)
            assert(venus_worker_poll(&worker) == RingAgain);
        else if (stage == 13)
            assert(venus_worker_poll(&worker) == RingCorrupt);
        else if (stage) {
            venus_ring_status_t status = venus_worker_destroy(&worker, 1000);
            assert(status == (stage == 20 ? RingTimeout : RingCorrupt));
            assert(worker.process_id > 0);
        }
        fault = 0;
        assert(venus_worker_destroy(&worker, 1000) == RingOk);
        assert(venus_worker_destroy(&worker, 1000) == RingOk);
        /* Parent source fd remains open; close it before checking descendant EOF. */
        close(sockets[1]);
        sockets[1] = dup(mapping_fd);
        struct pollfd descriptor = {.fd = sockets[0], .events = POLLIN};
        assert(poll(&descriptor, 1, 2000) == 1);
        unsigned char value;
        assert(read(sockets[0], &value, 1) == 0);
        fixture_close();
    }
    launch(&worker, 0);
    assert(waitid(P_PID, (id_t)worker.process_id, &observed, WEXITED | WNOWAIT) == 0);
    fault = 22;
    assert(venus_worker_poll(&worker) == RingCorrupt && !worker.process_id);
    fault = 0;
    assert(venus_worker_destroy(&worker, 1000) == RingOk);
    fixture_close();
    launch(&worker, 0);
    int status;
    assert(waitpid(worker.process_id, &status, 0) == worker.process_id);
    fault = 12;
    assert(venus_worker_poll(&worker) == RingCorrupt && !worker.process_id);
    fault = 0;
    assert(venus_worker_destroy(&worker, 1000) == RingOk);
    fixture_close();
    puts("Isolated process ownership, exec faults and descendant cleanup passed");
    return 0;
}
