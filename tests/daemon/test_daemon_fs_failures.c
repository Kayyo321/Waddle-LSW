/**
 * @file test_daemon_fs_failures.c
 * @brief Fault-injected VirtIO-FS ownership and retry lifecycle tests.
 *
 * Link with --wrap=fork,lstat,unlink,kill,waitpid. The wrappers inject failures
 * only for the exact child PID recorded by the fixture after successful spawn.
 */
#include "daemon_fs.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static pid_t injected_pid;
static int injected_kill_error;
static int injected_wait_error;
static char fixture_socket_path[WaddleMaxPathLen];
static int fake_socket_ready;
static int suppress_socket_ready;

pid_t __real_fork(void);
int __real_lstat(const char *path, struct stat *status);
int __real_unlink(const char *path);

pid_t __wrap_fork(void) {
    pid_t result = __real_fork();
    if (result > 0) {
        injected_pid = result;
        if (!suppress_socket_ready) fake_socket_ready = 1;
    }
    return result;
}

int __wrap_lstat(const char *path, struct stat *status) {
    if (fake_socket_ready && strcmp(path, fixture_socket_path) == 0) {
        memset(status, 0, sizeof(*status));
        status->st_mode = S_IFSOCK | 0600;
        return 0;
    }
    return __real_lstat(path, status);
}

int __wrap_unlink(const char *path) {
    if (fake_socket_ready && strcmp(path, fixture_socket_path) == 0) {
        fake_socket_ready = 0;
        return 0;
    }
    return __real_unlink(path);
}

pid_t __real_waitpid(pid_t pid, int *status, int options);
int __real_kill(pid_t pid, int signal_number);

pid_t __wrap_waitpid(pid_t pid, int *status, int options) {
    if (pid == injected_pid && injected_wait_error != 0) {
        int error = injected_wait_error;
        if (error == EINTR) injected_wait_error = 0;
        errno = error;
        return -1;
    }
    return __real_waitpid(pid, status, options);
}

int __wrap_kill(pid_t pid, int signal_number) {
    if (pid == injected_pid && injected_kill_error != 0) {
        int error = injected_kill_error;
        if (error == EINTR) injected_kill_error = 0;
        errno = error;
        return -1;
    }
    return __real_kill(pid, signal_number);
}

/* The lifecycle fixture does not exercise the unrelated Zig path translator. */
int waddle_path_rule_valid(const char *source, const char *target) {
    (void)source;
    (void)target;
    return 0;
}

int waddle_path_rule_apply(const char *path, const char *source, const char *target,
                           unsigned char *output, size_t capacity) {
    (void)path;
    (void)source;
    (void)target;
    (void)output;
    (void)capacity;
    errno = EINVAL;
    return -1;
}

char *waddle_translate_rules(const char *path, const path_rule_t *rules, size_t count) {
    (void)path;
    (void)rules;
    (void)count;
    errno = EINVAL;
    return NULL;
}

static int run_backend_if_requested(int argc, char **argv) {
    static const char prefix[] = "--socket-path=";
    if (argc < 2 || strncmp(argv[1], prefix, sizeof(prefix) - 1) != 0) return 0;
    for (;;) pause();
}

static void initialize_process(virtiofs_process_t *proc) {
    memset(proc, 0, sizeof(*proc));
    proc->log_fd = -1;
}

static void assert_process_owned(const virtiofs_process_t *proc, pid_t pid,
                                int log_fd, const char *socket_path,
                                const char *log_path) {
    struct stat st;
    assert(proc->pid == pid);
    assert(proc->is_running == 1);
    assert(proc->log_fd == log_fd);
    assert(strcmp(proc->socket_path, socket_path) == 0);
    assert(strcmp(proc->log_path, log_path) == 0);
    assert(lstat(socket_path, &st) == 0 && S_ISSOCK(st.st_mode));
    assert(fcntl(log_fd, F_GETFD) >= 0);
}

static void start_fixture(virtiofs_process_t *proc, const char *root,
                          char *socket_path, size_t socket_capacity,
                          char *log_path, size_t log_capacity) {
    int socket_written = snprintf(socket_path, socket_capacity, "%s/vhost.sock", root);
    int log_written = snprintf(log_path, log_capacity, "%s/virtiofs.log", root);
    assert(socket_written > 0 && (size_t)socket_written < socket_capacity);
    assert(log_written > 0 && (size_t)log_written < log_capacity);
    snprintf(fixture_socket_path, sizeof(fixture_socket_path), "%s", socket_path);
    fake_socket_ready = 0;
    initialize_process(proc);
    int spawn_result = daemon_fs_spawn_for_mount(proc, socket_path, root, 1, log_path,
                                                  "/proc/self/exe", 2000);
    assert(spawn_result == 0);
    injected_pid = proc->pid;
    assert(injected_pid > 0);
}

static void assert_released(const virtiofs_process_t *proc,
                            const char *socket_path, int log_fd) {
    assert(proc->pid == 0);
    assert(proc->is_running == 0);
    assert(proc->log_fd == -1);
    assert(proc->socket_path[0] == '\0');
    assert(proc->shared_dir[0] == '\0');
    assert(proc->log_path[0] == '\0');
    assert(access(socket_path, F_OK) != 0 && errno == ENOENT);
    assert(fcntl(log_fd, F_GETFD) == -1 && errno == EBADF);
}

static void assert_child_reaped(pid_t pid) {
    int status = 0;
    errno = 0;
    assert(__real_waitpid(pid, &status, WNOHANG) == -1 && errno == ECHILD);
}

static void test_socket_path_preflight(const char *root) {
    virtiofs_process_t proc;
    initialize_process(&proc);
    char socket_path[WaddleMaxPathLen];
    char log_path[WaddleMaxPathLen];
    snprintf(socket_path, sizeof(socket_path), "%s/vhost.sock", root);
    snprintf(log_path, sizeof(log_path), "%s/preflight.log", root);

    snprintf(fixture_socket_path, sizeof(fixture_socket_path), "%s", socket_path);
    fake_socket_ready = 1;
    assert(daemon_fs_spawn_for_mount(&proc, socket_path, root, 0, log_path,
                                     "/proc/self/exe", 100) == -1);
    assert(errno == EADDRINUSE && proc.pid == 0 && fake_socket_ready == 1);
    fake_socket_ready = 0;

    int file_fd = open(socket_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(file_fd >= 0);
    assert(write(file_fd, "keep", 4) == 4);
    close(file_fd);
    errno = 0;
    assert(daemon_fs_spawn_for_mount(&proc, socket_path, root, 2, log_path,
                                     "/proc/self/exe", 100) == -1);
    assert(errno == EINVAL);
    assert(access(log_path, F_OK) != 0 && errno == ENOENT);
    char bytes[5] = {0};
    file_fd = open(socket_path, O_RDONLY);
    assert(file_fd >= 0 && read(file_fd, bytes, 4) == 4);
    close(file_fd);
    assert(strcmp(bytes, "keep") == 0);

    errno = 0;
    assert(daemon_fs_spawn_for_mount(&proc, socket_path, root, 0, log_path,
                                     "/proc/self/exe", 100) == -1);
    assert(errno == EEXIST);
    assert(access(socket_path, F_OK) == 0);
    assert(access(log_path, F_OK) != 0 && errno == ENOENT);
    unlink(socket_path);

    char target_path[WaddleMaxPathLen];
    snprintf(target_path, sizeof(target_path), "%s/target", root);
    file_fd = open(target_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(file_fd >= 0);
    close(file_fd);
    assert(symlink(target_path, socket_path) == 0);
    errno = 0;
    assert(daemon_fs_spawn_for_mount(&proc, socket_path, root, 0, log_path,
                                     "/proc/self/exe", 100) == -1);
    assert(errno == EEXIST);
    struct stat st;
    assert(lstat(socket_path, &st) == 0 && S_ISLNK(st.st_mode));
    assert(access(log_path, F_OK) != 0 && errno == ENOENT);
    unlink(socket_path);
    unlink(target_path);
}

static void test_kill_failure_preserves_ownership(const char *root) {
    virtiofs_process_t proc;
    char socket_path[WaddleMaxPathLen];
    char log_path[WaddleMaxPathLen];
    start_fixture(&proc, root, socket_path, sizeof(socket_path), log_path, sizeof(log_path));
    pid_t pid = proc.pid;
    int log_fd = proc.log_fd;

    injected_kill_error = EPERM;
    errno = 0;
    assert(daemon_fs_stop(&proc, 10) == -1 && errno == EPERM);
    assert_process_owned(&proc, pid, log_fd, socket_path, log_path);
    daemon_fs_cleanup(&proc);
    assert_process_owned(&proc, pid, log_fd, socket_path, log_path);

    injected_kill_error = 0;
    daemon_fs_cleanup(&proc);
    assert_released(&proc, socket_path, log_fd);
    assert_child_reaped(pid);
    injected_pid = 0;
}

static void test_wait_failure_preserves_ownership(const char *root) {
    virtiofs_process_t proc;
    char socket_path[WaddleMaxPathLen];
    char log_path[WaddleMaxPathLen];
    start_fixture(&proc, root, socket_path, sizeof(socket_path), log_path, sizeof(log_path));
    pid_t pid = proc.pid;
    int log_fd = proc.log_fd;

    injected_wait_error = EIO;
    errno = 0;
    assert(daemon_fs_stop(&proc, 1000) == -1 && errno == EIO);
    assert_process_owned(&proc, pid, log_fd, socket_path, log_path);
    daemon_fs_cleanup(&proc);
    assert_process_owned(&proc, pid, log_fd, socket_path, log_path);

    injected_wait_error = 0;
    daemon_fs_cleanup(&proc);
    assert_released(&proc, socket_path, log_fd);
    assert_child_reaped(pid);
    injected_pid = 0;
}

static void test_eintr_is_retried(const char *root) {
    virtiofs_process_t proc;
    char socket_path[WaddleMaxPathLen];
    char log_path[WaddleMaxPathLen];
    start_fixture(&proc, root, socket_path, sizeof(socket_path), log_path, sizeof(log_path));
    int log_fd = proc.log_fd;
    injected_kill_error = EINTR;
    injected_wait_error = EINTR;
    assert(daemon_fs_stop(&proc, 1000) == 0);
    assert(proc.pid == 0 && proc.is_running == 0);
    daemon_fs_cleanup(&proc);
    assert_released(&proc, socket_path, log_fd);
    assert_child_reaped(injected_pid);
    injected_pid = 0;
}

static void test_spawn_failure_keeps_child_when_stop_fails(const char *root) {
    virtiofs_process_t proc;
    char socket_path[WaddleMaxPathLen];
    char log_path[WaddleMaxPathLen];
    int socket_written = snprintf(socket_path, sizeof(socket_path), "%s/no-ready.sock", root);
    int log_written = snprintf(log_path, sizeof(log_path), "%s/no-ready.log", root);
    assert(socket_written > 0 && (size_t)socket_written < sizeof(socket_path));
    assert(log_written > 0 && (size_t)log_written < sizeof(log_path));
    snprintf(fixture_socket_path, sizeof(fixture_socket_path), "%s", socket_path);
    initialize_process(&proc);

    suppress_socket_ready = 1;
    injected_kill_error = EPERM;
    errno = 0;
    int spawn_result = daemon_fs_spawn_for_mount(&proc, socket_path, root, 0,
                                                  log_path, "/proc/self/exe", 1);
    assert(spawn_result == -1 && errno == EPERM);
    suppress_socket_ready = 0;
    fake_socket_ready = 1;
    pid_t pid = proc.pid;
    int log_fd = proc.log_fd;
    assert(pid > 0);
    assert_process_owned(&proc, pid, log_fd, socket_path, log_path);

    injected_kill_error = 0;
    daemon_fs_cleanup(&proc);
    assert_released(&proc, socket_path, log_fd);
    assert_child_reaped(pid);
    injected_pid = 0;
}

int main(int argc, char **argv) {
    if (run_backend_if_requested(argc, argv)) return 0;
    char root_template[] = "/tmp/waddle-daemon-fs-failures-XXXXXX";
    char *root = mkdtemp(root_template);
    assert(root != NULL);

    test_socket_path_preflight(root);
    test_kill_failure_preserves_ownership(root);
    test_wait_failure_preserves_ownership(root);
    test_eintr_is_retried(root);
    test_spawn_failure_keeps_child_when_stop_fails(root);

    char path[WaddleMaxPathLen];
    snprintf(path, sizeof(path), "%s/virtiofs.log", root);
    unlink(path);
    snprintf(path, sizeof(path), "%s/preflight.log", root);
    unlink(path);
    snprintf(path, sizeof(path), "%s/no-ready.log", root);
    unlink(path);
    snprintf(path, sizeof(path), "%s/target", root);
    unlink(path);
    assert(rmdir(root) == 0);
    puts("test_daemon_fs_failures: ownership retry and path preflight checks passed");
    return 0;
}
