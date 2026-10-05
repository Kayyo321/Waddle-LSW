/** @file vm_terminal.c @brief Linux PTY frontend acceptance against real Viosock. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
static pid_t frontend;
static void check(int valid, const char *message) {
    if (valid) { return; }
    fprintf(stderr, "VM terminal: %s (errno %d)\n", message, errno);
    if (frontend > 0) { kill(frontend, SIGKILL); waitpid(frontend, NULL, 0); }
    exit(1);
}
static long long now(void) {
    struct timespec value;
    check(clock_gettime(CLOCK_MONOTONIC, &value) == 0, "clock");
    return (long long)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}
static void expect(int master, char *bytes, size_t capacity, size_t *used, const char *marker) {
    long long deadline = now() + 20000;
    while (strstr(bytes, marker) == NULL && now() < deadline) {
        struct pollfd ready = {master, POLLIN, 0};
        int result = poll(&ready, 1, 100);
        if (result < 0 && errno == EINTR) { continue; }
        check(result >= 0, "PTY poll");
        if (result == 0) { continue; }
        check(*used + 1 < capacity, "bounded terminal capture");
        ssize_t count = read(master, bytes + *used, capacity - *used - 1);
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) { continue; }
        check(count > 0, "PTY read");
        *used += (size_t)count; bytes[*used] = 0;
    }
    if (strstr(bytes, marker) == NULL) { fprintf(stderr, "Captured console: %s\n", bytes); }
    check(strstr(bytes, marker) != NULL, marker);
}
/** @brief Run input/resize/Ctrl-C and termios restoration against an installed guest.
 * @param[in] argc Must equal four. @param[in] argv Borrowed nonnull host binary,
 * VSOCK CID, and POSIX export source arguments retained until exec.
 * @return Zero verified, one failure, two invalid arguments.
 * @note Single main caller; owns/closes PTY descriptors and reaps its frontend.
 */
int main(int argc, char **argv) {
    if (argc != 4) { return 2; }
    char mapping[4096], executable[4096];
    int length = snprintf(mapping, sizeof(mapping), "%s=X:\\", argv[3]);
    check(length > 0 && (size_t)length < sizeof(mapping), "mapping bound");
    length = snprintf(executable, sizeof(executable), "%s/windows_guest_test.exe", argv[3]);
    check(length > 0 && (size_t)length < sizeof(executable), "executable bound");
    int master, slave;
    struct winsize size = {24, 80, 0, 0};
    check(openpty(&master, &slave, NULL, NULL, &size) == 0, "open PTY");
    struct termios before, after;
    check(tcgetattr(slave, &before) == 0, "initial termios");
    frontend = fork(); check(frontend >= 0, "fork frontend");
    if (frontend == 0) {
        close(master);
        if (setsid() < 0 || ioctl(slave, TIOCSCTTY, 0) < 0 ||
            dup2(slave, 0) < 0 || dup2(slave, 1) < 0 || dup2(slave, 2) < 0) { _exit(127); }
        if (slave > 2) { close(slave); }
        char *arguments[] = {argv[1], "exec", "--tty", "--vsock-cid", argv[2],
            "--timeout", "60", "--path-map", mapping, "--cwd", argv[3], "--",
            executable, "--child", "interactive", NULL};
        execv(argv[1], arguments); _exit(127);
    }
    check(fcntl(master, F_SETFL, O_NONBLOCK) == 0, "nonblocking PTY");
    char bytes[32768] = {0}; size_t used = 0;
    expect(master, bytes, sizeof(bytes), &used, "READY");
    size.ws_row = 39; size.ws_col = 101;
    check(ioctl(master, TIOCSWINSZ, &size) == 0 && kill(frontend, SIGWINCH) == 0, "resize signal");
    check(write(master, "go\r", 3) == 3, "interactive input");
    expect(master, bytes, sizeof(bytes), &used, "SIZE 39 101");
    expect(master, bytes, sizeof(bytes), &used, "MERGED");
    check(write(master, "\003", 1) == 1, "Ctrl-C input");
    int status = 0; pid_t finished = 0; long long deadline = now() + 20000;
    while (finished == 0 && now() < deadline) {
        finished = waitpid(frontend, &status, WNOHANG);
        if (finished < 0 && errno == EINTR) { finished = 0; continue; }
        check(finished >= 0, "wait frontend");
        if (finished == 0) { struct pollfd delay = {master, POLLIN, 0}; poll(&delay, 1, 10); }
    }
    check(finished == frontend, "interrupt completion deadline"); frontend = 0;
    check(WIFEXITED(status) && WEXITSTATUS(status) == 130, "guest interrupt exit 130");
    check(tcgetattr(slave, &after) == 0 && before.c_iflag == after.c_iflag &&
          before.c_oflag == after.c_oflag && before.c_cflag == after.c_cflag &&
          before.c_lflag == after.c_lflag && memcmp(before.c_cc, after.c_cc, NCCS) == 0,
          "terminal restoration");
    close(master); close(slave);
    puts("VM terminal: input, 39x101 resize, merged stderr, Ctrl-C, restoration passed");
    return 0;
}
