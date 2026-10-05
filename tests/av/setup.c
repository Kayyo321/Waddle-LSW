/** @file setup.c @brief Privileged helper reuse decisions with no kernel mutation. */
#include "av_layout.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
/** @brief Renamed setup entry; borrowed args, synchronous, no ownership transfer. */
int av_setup_entry(int argc, char **argv);
static unsigned waits;
static int wait_failure;
static uid_t effective_owner;
static int compatible = 1;
/** @brief Mock current privilege; no arguments/allocation, single test thread. */
uid_t __wrap_geteuid(void) { return effective_owner; }
/** @brief Mock opening only the expected device; borrowed path, returns fake owned FD. */
int __wrap_open(const char *path, int flags, ...) {
    assert(!strcmp(path, "/dev/kvmfr0"));
    assert((flags & (O_RDWR | O_CLOEXEC | O_NOFOLLOW)) == (O_RDWR | O_CLOEXEC | O_NOFOLLOW));
    return 123;
}
/** @brief Mock size ioctl; borrowed output gets eight bytes; no retained memory. */
int __wrap_ioctl(int fd, unsigned long request, ...) {
    (void)request;
    assert(fd == 123);
    va_list arguments;
    va_start(arguments, request);
    uint64_t *output = va_arg(arguments, uint64_t *);
    va_end(arguments);
    *output = compatible ? AvMappingBytes : 1;
    return 0;
}
/** @brief Mock bounded size seek; fake FD only, no external state. */
off_t __wrap_lseek(int fd, off_t offset, int whence) {
    assert(fd == 123 && offset == 0 && whence == SEEK_END);
    return AvMappingBytes;
}
/** @brief Mock fake FD release; ownership ends on success, single test thread. */
int __wrap_close(int fd) { assert(fd == 123); return 0; }
/** @brief Mock parent side of helper spawn; no actual subprocess is created. */
pid_t __wrap_fork(void) { return 456; }
/** @brief Mock owned child reaping; borrowed status set or error, no process mutation. */
pid_t __wrap_waitpid(pid_t pid, int *status, int options) {
    assert(pid == 456 && status && options == 0);
    ++waits;
    *status = 0;
    if (wait_failure) { errno = ECHILD; return -1; }
    return pid;
}
int main(void) {
    char *arguments[] = {"waddle-av-setup", "--load-module", NULL};
    assert(unsetenv("SUDO_UID") == 0);
    assert(av_setup_entry(2, arguments) == 1 && !waits);
    assert(setenv("SUDO_UID", "invalid", 1) == 0);
    assert(av_setup_entry(2, arguments) == 1 && !waits);
    assert(setenv("SUDO_UID", "1001", 1) == 0);
    assert(av_setup_entry(2, arguments) == 0 && waits == 1);
    wait_failure = 1;
    assert(av_setup_entry(2, arguments) == 1 && waits == 2);
    wait_failure = 0;
    compatible = 0;
    assert(av_setup_entry(2, arguments) == 1 && waits == 2);
    effective_owner = 1001;
    assert(av_setup_entry(2, arguments) == 1 && waits == 2);
    return 0;
}
