#include "../../src/interop/linux_session_owner.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <linux/vm_sockets.h>
#include <time.h>
#include <unistd.h>

typedef enum connector_mode_t {
    ConnectorSocketFailure,
    ConnectorImmediateRefusal,
    ConnectorImmediateSuccess,
    ConnectorPendingTimeout,
    ConnectorPendingRefusal,
    ConnectorPendingPollFailure,
    ConnectorPendingGetsockoptFailure,
    ConnectorPendingAmbiguousHup,
    ConnectorImmediateUnknown,
    ConnectorPendingCancel,
    ConnectorPendingShortResult,
    ConnectorPendingUnexpected,
    ConnectorImmediateConfiguredError,
    ConnectorPendingPollInterrupted,
    ConnectorPendingSpuriousWake
} connector_mode_t;

static connector_mode_t connector_mode;
static int connector_socket_calls;
static int connector_connect_calls;
static int connector_poll_calls;
static int connector_peer_fd = -1;
static int clock_call_count;
static int clock_fail_at;
static int clock_shift_after_first;
static int clock_invalid_nanoseconds;
static int clock_overflow_seconds;
static int clock_near_overflow;
static int clock_negative_seconds;
static int clock_large_nanoseconds;
static int poll_eintr_once;
static int spurious_poll_once;
static int immediate_connect_errno;
static int fail_preconnect_cancel_poll;
static int cancel_after_connect_write_fd = -1;
static int cancel_poll_failure_after_connect;
static int fail_next_calloc;
static int fail_next_close;
static int replace_session_lock_before_stat;
static int fstat_calls;
static int fail_fstat_call;
static int override_fstat_call;
static int override_fstat_kind;
static int fstatat_fail_once;
static int fstatat_override_kind;
static int flock_calls;
static int fail_flock_call;
static int fail_peer_create_once;
static int fail_peer_destroy_once;
static int peer_destroy_close_diagnostic_once;

extern int __real_poll(struct pollfd *fds, nfds_t count, int timeout);
extern int __real_getsockopt(int fd, int level, int option, void *value, socklen_t *length);
extern int __real_socket(int domain, int type, int protocol);
extern int __real_getpeername(int fd, struct sockaddr *address, socklen_t *address_length);
extern int __real_clock_gettime(clockid_t clock_id, struct timespec *time_value);
extern void *__real_calloc(size_t count, size_t bytes);
extern int __real_close(int fd);
extern int __real_fstatat(int dir_fd, const char *path, struct stat *status, int flags);
extern int __real_fstat(int fd, struct stat *status);
extern int __real_flock(int fd, int operation);
extern int __real_interop_linux_socket_create(int connected_fd,
    const interop_linux_socket_config_t *config, interop_linux_socket_t **out_owner);
extern int __real_interop_linux_socket_destroy(interop_linux_socket_t **owner_slot);

int __wrap_socket(int domain, int type, int protocol) {
    connector_socket_calls++;
    if (connector_mode == ConnectorSocketFailure) { errno = EMFILE; return -1; }
    assert(domain == AF_VSOCK);
    assert((type & SOCK_STREAM) != 0);
    assert((type & SOCK_NONBLOCK) != 0);
    assert((type & SOCK_CLOEXEC) != 0);
    int pair[2];
    if (socketpair(AF_UNIX, type, protocol, pair) != 0) return -1;
    connector_peer_fd = pair[1];
    return pair[0];
}

int __wrap_connect(int fd, const struct sockaddr *address, socklen_t address_length) {
    (void)fd;
    connector_connect_calls++;
    assert(address_length == sizeof(struct sockaddr_vm));
    const struct sockaddr_vm *vsock_address = (const struct sockaddr_vm *)address;
    assert(vsock_address->svm_family == AF_VSOCK);
    assert(vsock_address->svm_port == InteropControlPort);
    if (connector_mode == ConnectorImmediateRefusal || connector_mode == ConnectorPendingRefusal) {
        errno = ECONNREFUSED;
        if (connector_mode == ConnectorPendingRefusal) { errno = EINPROGRESS; return -1; }
        return -1;
    }
    if (connector_mode == ConnectorImmediateUnknown) { errno = ECONNRESET; return -1; }
    if (connector_mode == ConnectorImmediateConfiguredError) { errno = immediate_connect_errno; return -1; }
    if (connector_mode == ConnectorPendingTimeout || connector_mode == ConnectorPendingCancel ||
        connector_mode == ConnectorPendingPollFailure || connector_mode == ConnectorPendingGetsockoptFailure ||
        connector_mode == ConnectorPendingAmbiguousHup || connector_mode == ConnectorPendingShortResult ||
        connector_mode == ConnectorPendingUnexpected || connector_mode == ConnectorPendingPollInterrupted ||
        connector_mode == ConnectorPendingSpuriousWake) {
        if (connector_mode == ConnectorPendingCancel && cancel_after_connect_write_fd >= 0)
            assert(write(cancel_after_connect_write_fd, "x", 1) == 1);
        errno = EINPROGRESS;
        return -1;
    }
    if (cancel_after_connect_write_fd >= 0) assert(write(cancel_after_connect_write_fd, "x", 1) == 1);
    (void)fd;
    (void)address;
    (void)address_length;
    return 0;
}

int __wrap_getpeername(int fd, struct sockaddr *address, socklen_t *address_length) {
    (void)fd;
    if (address == NULL || address_length == NULL || *address_length < sizeof(struct sockaddr_vm)) {
        errno = EINVAL;
        return -1;
    }
    struct sockaddr_vm *vsock_address = (struct sockaddr_vm *)address;
    memset(vsock_address, 0, sizeof(*vsock_address));
    vsock_address->svm_family = AF_VSOCK;
    *address_length = sizeof(*vsock_address);
    return 0;
}

int __wrap_clock_gettime(clockid_t clock_id, struct timespec *time_value) {
    clock_call_count++;
    if (clock_fail_at > 0 && clock_call_count == clock_fail_at) { errno = EIO; return -1; }
    int result = __real_clock_gettime(clock_id, time_value);
    if (result == 0 && clock_call_count >= 2) time_value->tv_sec += clock_shift_after_first;
    if (result == 0 && clock_call_count == 1 && clock_invalid_nanoseconds) time_value->tv_nsec = -1;
    if (result == 0 && clock_call_count == 1 && clock_negative_seconds) time_value->tv_sec = -1;
    if (result == 0 && clock_call_count == 1 && clock_large_nanoseconds) time_value->tv_nsec = 1000000000L;
    if (result == 0 && clock_call_count == 1 && clock_overflow_seconds)
        time_value->tv_sec = (time_t)(UINT64_MAX / 1000u + 1u);
    if (result == 0 && clock_call_count == 1 && clock_near_overflow) {
        uint64_t value = UINT64_MAX - 50;
        time_value->tv_sec = (time_t)(value / 1000u);
        time_value->tv_nsec = (long)(value % 1000u) * 1000000L;
    }
    return result;
}

void *__wrap_calloc(size_t count, size_t bytes) {
    if (fail_next_calloc) { fail_next_calloc = 0; errno = ENOMEM; return NULL; }
    return __real_calloc(count, bytes);
}

int __wrap_close(int fd) {
    int result = __real_close(fd);
    if (fail_next_close) { fail_next_close = 0; errno = EIO; return -1; }
    return result;
}

int __wrap_fstatat(int dir_fd, const char *path, struct stat *status, int flags) {
    if (fstatat_fail_once && strcmp(path, "session.lock") == 0) {
        fstatat_fail_once = 0;
        errno = EIO;
        return -1;
    }
    if (replace_session_lock_before_stat && strcmp(path, "session.lock") == 0) {
        replace_session_lock_before_stat = 0;
        assert(unlinkat(dir_fd, path, 0) == 0);
        int replacement = openat(dir_fd, path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        assert(replacement >= 0 && close(replacement) == 0);
    }
    int result = __real_fstatat(dir_fd, path, status, flags);
    if (result == 0 && fstatat_override_kind != 0 && strcmp(path, "session.lock") == 0) {
        if (fstatat_override_kind == 1) status->st_mode = S_IFIFO | 0600;
        else if (fstatat_override_kind == 2) status->st_dev++;
        else assert(!"unknown fstatat override");
        fstatat_override_kind = 0;
    }
    return result;
}

int __wrap_fstat(int fd, struct stat *status) {
    fstat_calls++;
    if (fail_fstat_call > 0 && fstat_calls == fail_fstat_call) {
        errno = EIO;
        return -1;
    }
    int result = __real_fstat(fd, status);
    if (result == 0 && override_fstat_call == fstat_calls) {
        /* Fault-only metadata seams exercise fail-closed checks without changing files. */
        switch (override_fstat_kind) {
        case 1: status->st_mode = (fstat_calls == 1 ? S_IFREG : S_IFDIR) | 0600; break;
        case 2: status->st_uid = geteuid() + 1; break;
        case 3: status->st_mode |= 0004; break;
        case 4: status->st_nlink = 2; break;
        default: assert(!"unknown fstat override");
        }
        override_fstat_call = 0;
    }
    return result;
}

int __wrap_flock(int fd, int operation) {
    flock_calls++;
    if (fail_flock_call > 0 && flock_calls == fail_flock_call) {
        errno = EIO;
        return -1;
    }
    return __real_flock(fd, operation);
}

int __wrap_interop_linux_socket_create(int connected_fd,
    const interop_linux_socket_config_t *config, interop_linux_socket_t **out_owner) {
    if (fail_peer_create_once) {
        fail_peer_create_once = 0;
        errno = ENOMEM;
        return InteropLinuxSocketOutOfMemory;
    }
    return __real_interop_linux_socket_create(connected_fd, config, out_owner);
}

int __wrap_interop_linux_socket_destroy(interop_linux_socket_t **owner_slot) {
    if (fail_peer_destroy_once) {
        fail_peer_destroy_once = 0;
        errno = EIO;
        return InteropLinuxSocketWrongPhase;
    }
    int result = __real_interop_linux_socket_destroy(owner_slot);
    if (result == InteropLinuxSocketSuccess && peer_destroy_close_diagnostic_once) {
        peer_destroy_close_diagnostic_once = 0;
        return InteropLinuxSocketCloseDiagnostic;
    }
    return result;
}

int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout) {
    connector_poll_calls++;
    if (count == 1 && timeout == 0) {
        if (fail_preconnect_cancel_poll) { errno = EIO; return -1; }
        if (cancel_poll_failure_after_connect && connector_connect_calls > 0) { errno = EIO; return -1; }
        return __real_poll(fds, count, timeout);
    }
    if (connector_mode == ConnectorPendingPollInterrupted && poll_eintr_once) {
        poll_eintr_once = 0;
        errno = EINTR;
        return -1;
    }
    if (connector_mode == ConnectorPendingPollInterrupted) return 0;
    if (connector_mode == ConnectorPendingSpuriousWake && spurious_poll_once) {
        spurious_poll_once = 0;
        return 1;
    }
    if (connector_mode == ConnectorPendingSpuriousWake) return 0;
    if (connector_mode == ConnectorPendingTimeout) return 0;
    if (connector_mode == ConnectorPendingPollFailure) { errno = EIO; return -1; }
    if (connector_mode == ConnectorPendingCancel && count == 2) {
        fds[1].revents = POLLIN;
        return 1;
    }
    if (connector_mode == ConnectorPendingRefusal) {
        fds[0].revents = POLLOUT;
        return 1;
    }
    if (connector_mode == ConnectorPendingShortResult || connector_mode == ConnectorPendingUnexpected) {
        fds[0].revents = connector_mode == ConnectorPendingUnexpected ? POLLIN : POLLOUT;
        return 1;
    }
    if (connector_mode == ConnectorPendingAmbiguousHup) {
        fds[0].revents = POLLHUP;
        return 1;
    }
    return __real_poll(fds, count, timeout);
}

int __wrap_getsockopt(int fd, int level, int option, void *value, socklen_t *length) {
    if (level == SOL_SOCKET && option == SO_TYPE && length != NULL && *length >= sizeof(int)) {
        *(int *)value = SOCK_STREAM;
        *length = sizeof(int);
        return 0;
    }
    if (connector_mode == ConnectorPendingShortResult) { *(int *)value = 0; *length = 1; return 0; }
    if (connector_mode == ConnectorPendingGetsockoptFailure) { errno = EIO; return -1; }
    if (connector_mode == ConnectorPendingRefusal || connector_mode == ConnectorPendingAmbiguousHup) {
        assert(level == SOL_SOCKET && option == SO_ERROR && *length >= sizeof(int));
        *(int *)value = connector_mode == ConnectorPendingRefusal ? ECONNREFUSED : 0;
        *length = sizeof(int);
        return 0;
    }
    if (connector_mode == ConnectorPendingPollInterrupted) {
        assert(level == SOL_SOCKET && option == SO_ERROR && *length >= sizeof(int));
        *(int *)value = 0;
        *length = sizeof(int);
        return 0;
    }
    return __real_getsockopt(fd, level, option, value, length);
}

static char temporary_root[] = "/tmp/waddle-session-owner-XXXXXX";
static char state_root[WaddleMaxPathLen];
static char runtime_root[WaddleMaxPathLen];

static int remove_temporary_path(const char *path, const struct stat *status, int kind, struct FTW *walk) {
    (void)status;
    (void)kind;
    (void)walk;
    return remove(path);
}

static void make_directory(const char *path) {
    assert(mkdir(path, 0700) == 0);
}

static void configure_environment(void) {
    char *root = mkdtemp(temporary_root);
    assert(root != NULL);
    assert(snprintf(state_root, sizeof(state_root), "%s/state", root) > 0);
    assert(snprintf(runtime_root, sizeof(runtime_root), "%s/run", root) > 0);
    make_directory(state_root);
    make_directory(runtime_root);
    assert(setenv("XDG_STATE_HOME", state_root, 1) == 0);
    assert(setenv("XDG_RUNTIME_DIR", runtime_root, 1) == 0);
}

static void make_profile_dirs(const char *name) {
    char path[WaddleMaxPathLen];
    assert(daemon_device_get_state_dir(name, path, sizeof(path)) == 0);
    char parent[WaddleMaxPathLen];
    assert(snprintf(parent, sizeof(parent), "%s/waddle", state_root) > 0);
    if (mkdir(parent, 0700) != 0) assert(errno == EEXIST);
    assert(snprintf(parent, sizeof(parent), "%s/waddle/devices", state_root) > 0);
    if (mkdir(parent, 0700) != 0) assert(errno == EEXIST);
    if (mkdir(path, 0700) != 0) assert(errno == EEXIST);
}

static void make_runtime_dirs(const char *name) {
    char path[WaddleMaxPathLen];
    assert(snprintf(path, sizeof(path), "%s/waddle", runtime_root) > 0);
    if (mkdir(path, 0700) != 0) assert(errno == EEXIST);
    assert(snprintf(path, sizeof(path), "%s/waddle/%s", runtime_root, name) > 0);
    if (mkdir(path, 0700) != 0) assert(errno == EEXIST);
}

static void copy_runtime_parent(const char *socket_path, char *output, size_t capacity) {
    assert(strlen(socket_path) < capacity);
    memcpy(output, socket_path, strlen(socket_path) + 1);
    char *slash = strrchr(output, '/');
    assert(slash != NULL && slash != output);
    *slash = '\0';
}

static void make_config(const char *name, uint32_t port, interop_linux_session_config_t *config,
                        device_info_t *device) {
    memset(device, 0, sizeof(*device));
    memcpy(device->name, name, strlen(name) + 1);
    assert(daemon_device_get_state_dir(name, device->state_dir, sizeof(device->state_dir)) == 0);
    assert(daemon_device_get_socket_path(name, device->socket_path, sizeof(device->socket_path)) == 0);
    device->config_valid = 1;
    device->vsock_cid = 7;
    device->vsock_port = port;
    static const uint8_t version[] = "1.2.3-alpha.1+build.7";
    memset(config, 0, sizeof(*config));
    config->device = device;
    config->agent_version = version;
    config->agent_version_bytes = sizeof(version) - 1;
    config->connection_token = 42;
    config->local_capabilities = 0;
    config->drain_timeout_ms = 1000;
}

static void test_port_conflict_precedes_filesystem(void) {
    interop_linux_session_config_t config;
    device_info_t device;
    make_config("port-conflict", InteropControlPort, &config, &device);
    interop_linux_session_owner_t *owner = NULL;
    interop_linux_session_result_t result = interop_linux_session_acquire(&config, &owner);
    assert(result == InteropLinuxSessionGuestPortConflict);
    assert(owner == NULL);
    char state[WaddleMaxPathLen];
    assert(daemon_device_get_state_dir(device.name, state, sizeof(state)) == 0);
    struct stat info;
    assert(stat(state, &info) != 0 && errno == ENOENT);
}

static void test_profile_validation_is_side_effect_free(void) {
    interop_linux_session_config_t config;
    device_info_t device;
    make_config("invalid-profile", 5100, &config, &device);
    interop_linux_session_owner_t *owner = NULL;
    config.local_capabilities = UINT64_MAX;
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionInvalidArgument);
    assert(owner == NULL);
    config.local_capabilities = 0;
    static const uint8_t bad_version[] = "01.2.3";
    config.agent_version = bad_version;
    config.agent_version_bytes = sizeof(bad_version) - 1;
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionInvalidArgument);
    config.agent_version = (const uint8_t *)"1.2.3";
    config.agent_version_bytes = 5;
    device.config_valid = 0;
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionInvalidArgument);
    device.config_valid = 1;
    device.socket_path[0] = '/';
    device.socket_path[1] = '\0';
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionInvalidArgument);
    device.socket_path[0] = '\0';
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionInvalidArgument);
    memset(device.socket_path, 'x', sizeof(device.socket_path));
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionInvalidArgument);
    char state[WaddleMaxPathLen];
    assert(daemon_device_get_state_dir(device.name, state, sizeof(state)) == 0);
    struct stat info;
    assert(stat(state, &info) != 0 && errno == ENOENT);
}

static void test_profile_identity_and_root_resolution_fail_closed(void) {
    interop_linux_session_config_t config;
    device_info_t device;
    make_config("profile-identity", 5116, &config, &device);
    interop_linux_session_owner_t *owner = NULL;
    char saved_path[WaddleMaxPathLen];
    memcpy(saved_path, device.state_dir, sizeof(saved_path));
    device.state_dir[strlen(device.state_dir) - 1] ^= 1;
    errno = 0;
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionInvalidArgument);
    assert(owner == NULL && errno == EINVAL);
    memcpy(device.state_dir, saved_path, sizeof(saved_path));
    memcpy(saved_path, device.socket_path, sizeof(saved_path));
    device.socket_path[strlen(device.socket_path) - 1] ^= 1;
    errno = 0;
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionInvalidArgument);
    assert(owner == NULL && errno == EINVAL);
    memcpy(device.socket_path, saved_path, sizeof(saved_path));

    assert(setenv("XDG_STATE_HOME", "relative-root", 1) == 0);
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionOsError);
    assert(owner == NULL && errno == EINVAL);
    assert(setenv("XDG_STATE_HOME", state_root, 1) == 0);
    assert(setenv("XDG_RUNTIME_DIR", "relative-root", 1) == 0);
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionOsError);
    assert(owner == NULL && errno == EINVAL);
    assert(setenv("XDG_RUNTIME_DIR", runtime_root, 1) == 0);
}

static void test_symlink_lock_is_rejected_without_following(void) {
    const char *name = "symlink-lock";
    interop_linux_session_config_t config;
    device_info_t device;
    make_config(name, 5101, &config, &device);
    char runtime_device[WaddleMaxPathLen];
    assert(snprintf(runtime_device, sizeof(runtime_device), "%s/waddle/%s", runtime_root, name) > 0);
    char runtime_waddle[WaddleMaxPathLen];
    assert(snprintf(runtime_waddle, sizeof(runtime_waddle), "%s/waddle", runtime_root) > 0);
    assert(mkdir(runtime_waddle, 0700) == 0);
    assert(mkdir(runtime_device, 0700) == 0);
    char target[WaddleMaxPathLen];
    char lock_path[WaddleMaxPathLen];
    assert(snprintf(target, sizeof(target), "%s/target", temporary_root) > 0);
    int target_fd = open(target, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    assert(target_fd >= 0 && close(target_fd) == 0);
    assert(snprintf(lock_path, sizeof(lock_path), "%s/session.lock", runtime_device) > 0);
    assert(symlink(target, lock_path) == 0);
    interop_linux_session_owner_t *owner = NULL;
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionOsError);
    assert(owner == NULL && errno == ELOOP);
    struct stat link_info;
    assert(lstat(lock_path, &link_info) == 0 && S_ISLNK(link_info.st_mode));
    assert(unlink(lock_path) == 0 && unlink(target) == 0);
}

static void test_nonprivate_directory_and_bad_lock_file_are_rejected(void) {
    interop_linux_session_config_t config;
    device_info_t device;
    const char *name = "bad-lock-file";
    make_config(name, 5105, &config, &device);
    char runtime_waddle[WaddleMaxPathLen];
    char runtime_device[WaddleMaxPathLen];
    char lock_path[WaddleMaxPathLen];
    assert(snprintf(runtime_waddle, sizeof(runtime_waddle), "%s/waddle", runtime_root) > 0);
    if (mkdir(runtime_waddle, 0700) != 0) assert(errno == EEXIST);
    assert(snprintf(runtime_device, sizeof(runtime_device), "%s/%s", runtime_waddle, name) > 0);
    assert(mkdir(runtime_device, 0700) == 0);
    assert(snprintf(lock_path, sizeof(lock_path), "%s/session.lock", runtime_device) > 0);
    int fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    assert(fd >= 0 && close(fd) == 0);
    interop_linux_session_owner_t *owner = NULL;
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionOsError);
    assert(owner == NULL && errno == EACCES);
    assert(unlink(lock_path) == 0);
    fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    assert(fd >= 0 && close(fd) == 0);
    char hard_link[WaddleMaxPathLen];
    assert(snprintf(hard_link, sizeof(hard_link), "%s/session-link", temporary_root) > 0);
    assert(link(lock_path, hard_link) == 0);
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionOsError);
    assert(owner == NULL && errno == EACCES);
    assert(unlink(hard_link) == 0 && unlink(lock_path) == 0);
    assert(chmod(runtime_device, 0755) == 0);
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionOsError);
    assert(owner == NULL && errno == EACCES);
    assert(chmod(runtime_device, 0700) == 0);
    assert(rmdir(runtime_device) == 0);
}

static void test_directory_and_lock_syscall_failures_unwind_leases(void) {
    const char *name = "lock-syscall-faults";
    make_profile_dirs(name);
    make_runtime_dirs(name);
    interop_linux_session_config_t config;
    device_info_t device;
    make_config(name, 5114, &config, &device);

    for (int fail_call = 1; fail_call <= 4; fail_call++) {
        interop_linux_session_owner_t *owner = NULL;
        fstat_calls = 0;
        fail_fstat_call = fail_call;
        assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionOsError);
        assert(owner == NULL);
        fail_fstat_call = 0;
        assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);
        assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);
    }

    for (int kind = 1; kind <= 3; kind++) {
        interop_linux_session_owner_t *owner = NULL;
        fstat_calls = 0;
        override_fstat_call = 1;
        override_fstat_kind = kind;
        assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionOsError);
        assert(owner == NULL && override_fstat_call == 0);
    }
    for (int kind = 1; kind <= 4; kind++) {
        interop_linux_session_owner_t *owner = NULL;
        fstat_calls = 0;
        override_fstat_call = 2;
        override_fstat_kind = kind;
        assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionOsError);
        assert(owner == NULL && override_fstat_call == 0);
    }
    for (int kind = 1; kind <= 4; kind++) {
        interop_linux_session_owner_t *owner = NULL;
        fstat_calls = 0;
        override_fstat_call = 4;
        override_fstat_kind = kind;
        assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionOsError);
        assert(owner == NULL && override_fstat_call == 0);
    }

    interop_linux_session_owner_t *owner = NULL;
    fstatat_fail_once = 1;
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionOsError);
    assert(owner == NULL && fstatat_fail_once == 0);
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);

    for (int kind = 1; kind <= 2; kind++) {
        owner = NULL;
        fstatat_override_kind = kind;
        assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionOsError);
        assert(owner == NULL && fstatat_override_kind == 0 && errno == EACCES);
        assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);
        assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);
    }
    owner = NULL;
    fstatat_fail_once = 1;
    fail_next_close = 1;
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionOsError);
    assert(owner == NULL && fstatat_fail_once == 0 && fail_next_close == 0);
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);

    for (int fail_call = 1; fail_call <= 2; fail_call++) {
        owner = NULL;
        flock_calls = 0;
        fail_flock_call = fail_call;
        assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionOsError);
        assert(owner == NULL);
        fail_flock_call = 0;
        assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);
        assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);
    }

    char runtime_parent[WaddleMaxPathLen];
    copy_runtime_parent(device.socket_path, runtime_parent, sizeof(runtime_parent));
    assert(chmod(runtime_parent, 0755) == 0);
    owner = NULL;
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionOsError);
    assert(owner == NULL && errno == EACCES);
    assert(chmod(runtime_parent, 0700) == 0);

    char av_dir[WaddleMaxPathLen];
    assert(snprintf(av_dir, sizeof(av_dir), "%s/av", device.state_dir) > 0);
    if (mkdir(av_dir, 0700) != 0) assert(errno == EEXIST);
    assert(chmod(av_dir, 0755) == 0);
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionOsError);
    assert(owner == NULL && errno == EACCES);
    assert(chmod(av_dir, 0700) == 0);
}

static void test_lock_leaf_open_failures_unwind(void) {
    const char *name = "lock-leaf-open-failures";
    make_profile_dirs(name);
    make_runtime_dirs(name);
    interop_linux_session_config_t config;
    device_info_t device;
    make_config(name, 5115, &config, &device);
    char runtime_parent[WaddleMaxPathLen];
    copy_runtime_parent(device.socket_path, runtime_parent, sizeof(runtime_parent));
    char session_leaf[WaddleMaxPathLen];
    assert(snprintf(session_leaf, sizeof(session_leaf), "%s/session.lock", runtime_parent) > 0);
    assert(mkdir(session_leaf, 0700) == 0);
    interop_linux_session_owner_t *owner = NULL;
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionOsError);
    assert(owner == NULL && errno == EISDIR);
    assert(rmdir(session_leaf) == 0);

    char av_dir[WaddleMaxPathLen];
    char av_leaf[WaddleMaxPathLen];
    assert(snprintf(av_dir, sizeof(av_dir), "%s/av", device.state_dir) > 0);
    assert(mkdir(av_dir, 0700) == 0);
    assert(snprintf(av_leaf, sizeof(av_leaf), "%s/command.lock", av_dir) > 0);
    assert(mkdir(av_leaf, 0700) == 0);
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionOsError);
    assert(owner == NULL && errno == EISDIR);
    assert(rmdir(av_leaf) == 0);
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);
}

static void test_lock_name_identity_is_rechecked_after_flock(void) {
    const char *name = "lock-name-race";
    make_profile_dirs(name);
    interop_linux_session_config_t config;
    device_info_t device;
    make_config(name, 5113, &config, &device);
    interop_linux_session_owner_t *owner = NULL;
    replace_session_lock_before_stat = 1;
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionOsError);
    assert(owner == NULL && errno == EACCES && replace_session_lock_before_stat == 0);
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);
}

static void test_profile_path_boundaries_fail_before_publishing(void) {
    interop_linux_session_config_t config;
    device_info_t device;
    make_config("long-profile", 5106, &config, &device);
    interop_linux_session_owner_t *owner = NULL;
    char long_root[WaddleMaxPathLen];
    size_t suffix_length = strlen(device.state_dir) - strlen(state_root);
    size_t root_length = WaddleMaxPathLen - 2 - suffix_length;
    memset(long_root, 'x', sizeof(long_root));
    long_root[0] = '/';
    long_root[root_length] = '\0';
    assert(setenv("XDG_STATE_HOME", long_root, 1) == 0);
    assert(daemon_device_get_state_dir(device.name, device.state_dir, sizeof(device.state_dir)) == 0);
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionOsError);
    assert(owner == NULL && errno == ENAMETOOLONG);
    assert(setenv("XDG_STATE_HOME", state_root, 1) == 0);
}

static void test_av_directory_symlink_fails_after_session_unwind(void) {
    const char *name = "av-directory-link";
    make_profile_dirs(name);
    interop_linux_session_config_t config;
    device_info_t device;
    make_config(name, 5112, &config, &device);
    char av_dir[WaddleMaxPathLen];
    char target[WaddleMaxPathLen];
    assert(snprintf(av_dir, sizeof(av_dir), "%s/av", device.state_dir) > 0);
    assert(snprintf(target, sizeof(target), "%s/av-target", temporary_root) > 0);
    assert(mkdir(target, 0700) == 0);
    assert(symlink(target, av_dir) == 0);
    interop_linux_session_owner_t *owner = NULL;
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionOsError);
    assert(owner == NULL && errno == ENOTDIR);
    assert(unlink(av_dir) == 0);
    assert(rmdir(target) == 0);
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);
}

static void test_allocation_failure_preserves_output(void) {
    make_profile_dirs("allocation-failure");
    interop_linux_session_config_t config;
    device_info_t device;
    make_config("allocation-failure", 5109, &config, &device);
    interop_linux_session_owner_t *owner = NULL;
    fail_next_calloc = 1;
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionOutOfMemory);
    assert(owner == NULL && fail_next_calloc == 0);
}

static void test_lock_order_contention_and_release(void) {
    const char *name = "lease-contention";
    make_profile_dirs(name);
    interop_linux_session_config_t config;
    device_info_t device;
    make_config(name, 5003, &config, &device);
    interop_linux_session_owner_t *owner = NULL;
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);
    assert(owner != NULL);
    assert(interop_linux_session_socket_owner(owner) == NULL);

    interop_linux_session_owner_t *contender = NULL;
    assert(interop_linux_session_acquire(&config, &contender) == InteropLinuxSessionBusy);
    assert(contender == NULL);
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);
    assert(owner == NULL);

    char av_dir[WaddleMaxPathLen];
    char lock_path[WaddleMaxPathLen];
    assert(snprintf(av_dir, sizeof(av_dir), "%s/av", device.state_dir) > 0);
    if (mkdir(av_dir, 0700) != 0) assert(errno == EEXIST);
    assert(snprintf(lock_path, sizeof(lock_path), "%s/command.lock", av_dir) > 0);
    int av_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    assert(av_fd >= 0 && flock(av_fd, LOCK_EX | LOCK_NB) == 0);
    assert(interop_linux_session_acquire(&config, &contender) == InteropLinuxSessionAvBusy);
    assert(contender == NULL);
    assert(flock(av_fd, LOCK_UN) == 0);
    assert(close(av_fd) == 0);
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);

    char session_lock[WaddleMaxPathLen];
    char socket_path[WaddleMaxPathLen];
    assert(snprintf(socket_path, sizeof(socket_path), "%s", device.socket_path) > 0);
    char *slash = strrchr(socket_path, '/');
    assert(slash != NULL);
    *slash = '\0';
    assert(snprintf(session_lock, sizeof(session_lock), "%s/session.lock", socket_path) > 0);
    struct stat info;
    assert(stat(session_lock, &info) == 0 && (info.st_mode & 07777) == 0600 && info.st_nlink == 1);
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);
    assert(owner == NULL);
}

static void reset_connector(connector_mode_t mode) {
    connector_mode = mode;
    connector_socket_calls = connector_connect_calls = connector_poll_calls = 0;
    clock_call_count = clock_shift_after_first = cancel_poll_failure_after_connect = 0;
    clock_fail_at = 0;
    clock_invalid_nanoseconds = clock_overflow_seconds = clock_near_overflow = 0;
    clock_negative_seconds = clock_large_nanoseconds = 0;
    poll_eintr_once = spurious_poll_once = immediate_connect_errno = fstatat_override_kind = 0;
    cancel_after_connect_write_fd = -1;
    clock_invalid_nanoseconds = clock_overflow_seconds = fail_preconnect_cancel_poll = 0;
    if (connector_peer_fd >= 0) { close(connector_peer_fd); connector_peer_fd = -1; }
}

static void test_connector_retry_consumption_and_peer_handoff(void) {
    const char *name = "connector-outcomes";
    make_profile_dirs(name);
    interop_linux_session_config_t config;
    device_info_t device;
    make_config(name, 5004, &config, &device);
    interop_linux_session_owner_t *owner = NULL;
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);

    reset_connector(ConnectorSocketFailure);
    assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionConnectError);
    assert(connector_socket_calls == 1 && connector_connect_calls == 0);
    reset_connector(ConnectorImmediateRefusal);
    interop_linux_session_result_t refusal_result = interop_linux_session_connect_peer(owner, 100, -1);
    assert(refusal_result == InteropLinuxSessionConnectError);
    assert(connector_connect_calls == 1);
    reset_connector(ConnectorPendingRefusal);
    assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionConnectError);
    assert(connector_connect_calls == 1 && connector_poll_calls >= 1);
    reset_connector(ConnectorImmediateUnknown);
    assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionConnectError);
    assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionAttemptConsumed);
    assert(connector_connect_calls == 1);
    /* A definite refusal on a fresh owner remains retryable. */
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);
    make_config(name, 5004, &config, &device);
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);
    reset_connector(ConnectorImmediateSuccess);
    interop_linux_session_result_t result = interop_linux_session_connect_peer(owner, 100, -1);
    assert(result == InteropLinuxSessionSuccess);
    assert(interop_linux_session_socket_owner(owner) != NULL);
    assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionPeerAlreadyCreated);
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSocketOwnerNotDestroyed);
    assert(interop_linux_session_destroy_peer(owner) == InteropLinuxSessionSuccess);
    assert(connector_peer_fd >= 0 && close(connector_peer_fd) == 0);
    connector_peer_fd = -1;
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);
}

static void test_preconnect_cancel_and_postconnect_consumption(void) {
    const char *name = "connector-cancel";
    make_profile_dirs(name);
    interop_linux_session_config_t config;
    device_info_t device;
    make_config(name, 5005, &config, &device);
    interop_linux_session_owner_t *owner = NULL;
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);
    int pipe_fds[2];
    assert(pipe(pipe_fds) == 0);
    assert(write(pipe_fds[1], "x", 1) == 1);
    reset_connector(ConnectorImmediateSuccess);
    assert(interop_linux_session_connect_peer(owner, 100, pipe_fds[0]) == InteropLinuxSessionCancelled);
    assert(connector_socket_calls == 0 && connector_connect_calls == 0);
    assert(read(pipe_fds[0], &(char){0}, 1) == 1);
    assert(close(pipe_fds[0]) == 0 && close(pipe_fds[1]) == 0);

    assert(pipe(pipe_fds) == 0);
    reset_connector(ConnectorPendingCancel);
    cancel_after_connect_write_fd = pipe_fds[1];
    assert(interop_linux_session_connect_peer(owner, 100, pipe_fds[0]) == InteropLinuxSessionCancelled);
    assert(close(pipe_fds[0]) == 0 && close(pipe_fds[1]) == 0);
    assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionAttemptConsumed);
    assert(connector_connect_calls == 1);
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);
}

static void *wrong_thread_probe(void *argument) {
    interop_linux_session_owner_t *owner = argument;
    assert(interop_linux_session_socket_owner(owner) == NULL);
    assert(errno == EPERM);
    return (void *)(uintptr_t)interop_linux_session_destroy_peer(owner);
}

static void *wrong_thread_lifecycle_probe(void *argument) {
    interop_linux_session_owner_t *owner = argument;
    assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionWrongThread);
    assert(interop_linux_session_destroy_peer(owner) == InteropLinuxSessionWrongThread);
    interop_linux_session_owner_t *local_slot = owner;
    assert(interop_linux_session_release(&local_slot) == InteropLinuxSessionWrongThread);
    assert(local_slot == owner);
    return NULL;
}

static void test_wrong_thread_is_rejected(void) {
    const char *name = "wrong-thread";
    make_profile_dirs(name);
    interop_linux_session_config_t config;
    device_info_t device;
    make_config(name, 5102, &config, &device);
    interop_linux_session_owner_t *owner = NULL;
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, wrong_thread_probe, owner) == 0);
    void *result = NULL;
    assert(pthread_join(thread, &result) == 0);
    assert((interop_linux_session_result_t)(uintptr_t)result == InteropLinuxSessionWrongThread);
    assert(pthread_create(&thread, NULL, wrong_thread_lifecycle_probe, owner) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);
}

static void test_connect_api_argument_boundaries_preserve_owner(void) {
    const char *name = "connect-argument-boundaries";
    make_profile_dirs(name);
    interop_linux_session_config_t config;
    device_info_t device;
    make_config(name, 5119, &config, &device);
    interop_linux_session_owner_t *owner = NULL;
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);
    assert(interop_linux_session_connect_peer(NULL, 100, -1) == InteropLinuxSessionInvalidArgument);
    assert(interop_linux_session_connect_peer((void *)((uintptr_t)owner + 1), 100, -1) ==
        InteropLinuxSessionInvalidArgument);
    assert(interop_linux_session_connect_peer(
        (void *)(UINTPTR_MAX - (UINTPTR_MAX % _Alignof(void *))), 100, -1) ==
        InteropLinuxSessionInvalidArgument);
    assert(interop_linux_session_connect_peer(owner, 0, -1) == InteropLinuxSessionInvalidArgument);
    assert(interop_linux_session_connect_peer(owner, 2001, -1) == InteropLinuxSessionInvalidArgument);
    assert(interop_linux_session_connect_peer(owner, 100, -2) == InteropLinuxSessionInvalidArgument);
    reset_connector(ConnectorImmediateSuccess);
    assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionSuccess);
    assert(interop_linux_session_destroy_peer(owner) == InteropLinuxSessionSuccess);
    assert(connector_peer_fd >= 0 && close(connector_peer_fd) == 0);
    connector_peer_fd = -1;
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);
}

static void test_timeout_consumes_in_progress_attempt(void) {
    const char *name = "connect-timeout";
    make_profile_dirs(name);
    interop_linux_session_config_t config;
    device_info_t device;
    make_config(name, 5103, &config, &device);
    interop_linux_session_owner_t *owner = NULL;
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);
    reset_connector(ConnectorPendingTimeout);
    assert(interop_linux_session_connect_peer(owner, 1, -1) == InteropLinuxSessionTimedOut);
    assert(connector_connect_calls == 1);
    assert(interop_linux_session_connect_peer(owner, 1, -1) == InteropLinuxSessionAttemptConsumed);
    assert(connector_socket_calls == 1);
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);
}

static void test_pending_syscall_errors_consume_attempt(void) {
    const char *name = "connect-syscall-errors";
    make_profile_dirs(name);
    interop_linux_session_config_t config;
    device_info_t device;
    make_config(name, 5107, &config, &device);
    interop_linux_session_owner_t *owner = NULL;
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);
    reset_connector(ConnectorPendingPollFailure);
    assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionConnectError);
    assert(connector_connect_calls == 1);
    assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionAttemptConsumed);
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);

    make_config("connect-getopt-error", 5108, &config, &device);
    make_profile_dirs(device.name);
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);
    reset_connector(ConnectorPendingGetsockoptFailure);
    assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionConnectError);
    assert(connector_connect_calls == 1);
    assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionAttemptConsumed);
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);

    make_config("connect-ambiguous-hup", 5111, &config, &device);
    make_profile_dirs(device.name);
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);
    reset_connector(ConnectorPendingAmbiguousHup);
    assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionConnectError);
    assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionAttemptConsumed);
    assert(connector_connect_calls == 1);
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);
}

static void test_clock_failures_preserve_retry_boundary(void) {
    const char *name = "clock-failure";
    make_profile_dirs(name);
    interop_linux_session_config_t config;
    device_info_t device;
    make_config(name, 5104, &config, &device);
    interop_linux_session_owner_t *owner = NULL;
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);
    reset_connector(ConnectorImmediateSuccess);
    clock_call_count = 0;
    clock_fail_at = 1;
    assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionClockError);
    assert(connector_socket_calls == 0);
    clock_call_count = 0;
    clock_fail_at = 2;
    assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionClockError);
    clock_fail_at = 0;
    assert(connector_connect_calls == 1);
    int socket_calls = connector_socket_calls;
    assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionAttemptConsumed);
    assert(connector_socket_calls == socket_calls);
    clock_fail_at = 0;
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);
}

static void test_malformed_clock_values_and_preconnect_poll_error(void) {
    const char *name = "clock-malformed-values";
    make_profile_dirs(name);
    interop_linux_session_config_t config;
    device_info_t device;
    make_config(name, 5117, &config, &device);
    interop_linux_session_owner_t *owner = NULL;
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);
    reset_connector(ConnectorImmediateSuccess);
    clock_invalid_nanoseconds = 1;
    assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionClockError);
    assert(connector_socket_calls == 0);
    reset_connector(ConnectorImmediateSuccess);
    clock_overflow_seconds = 1;
    assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionClockError);
    assert(connector_socket_calls == 0);
    reset_connector(ConnectorImmediateSuccess);
    int pipe_fds[2];
    assert(pipe(pipe_fds) == 0);
    fail_preconnect_cancel_poll = 1;
    assert(interop_linux_session_connect_peer(owner, 100, pipe_fds[0]) == InteropLinuxSessionOsError);
    assert(connector_socket_calls == 0 && connector_connect_calls == 0);
    fail_preconnect_cancel_poll = 0;
    assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionSuccess);
    assert(interop_linux_session_destroy_peer(owner) == InteropLinuxSessionSuccess);
    assert(connector_peer_fd >= 0 && close(connector_peer_fd) == 0);
    connector_peer_fd = -1;
    assert(close(pipe_fds[0]) == 0 && close(pipe_fds[1]) == 0);
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);
}

static void test_definite_refusal_errno_set_remains_retryable(void) {
    static const int errors[] = {ENETUNREACH, EHOSTUNREACH, ENODEV};
    for (size_t index = 0; index < sizeof(errors) / sizeof(errors[0]); index++) {
        char name[WaddleMaxDeviceNameLen];
        assert(snprintf(name, sizeof(name), "refusal-%zu", index) < (int)sizeof(name));
        make_profile_dirs(name);
        interop_linux_session_config_t config;
        device_info_t device;
        make_config(name, (uint32_t)(5120 + index), &config, &device);
        interop_linux_session_owner_t *owner = NULL;
        assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);
        reset_connector(ConnectorImmediateConfiguredError);
        immediate_connect_errno = errors[index];
        assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionConnectError);
        assert(interop_linux_session_connect_peer(owner, 0, -1) == InteropLinuxSessionInvalidArgument);
        connector_mode = ConnectorImmediateSuccess;
        assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionSuccess);
        assert(interop_linux_session_destroy_peer(owner) == InteropLinuxSessionSuccess);
        assert(connector_peer_fd >= 0 && close(connector_peer_fd) == 0);
        connector_peer_fd = -1;
        assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);
    }
}

static void test_pending_connect_poll_clock_and_spurious_edges(void) {
    const char *name = "pending-connect-edges";
    make_profile_dirs(name);
    interop_linux_session_config_t config;
    device_info_t device;
    make_config(name, 5123, &config, &device);
    interop_linux_session_owner_t *owner = NULL;
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);
    int pipe_fds[2];
    assert(pipe(pipe_fds) == 0);

    reset_connector(ConnectorPendingTimeout);
    clock_fail_at = 2;
    assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionClockError);
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);

    make_config(name, 5123, &config, &device);
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);
    reset_connector(ConnectorPendingPollInterrupted);
    poll_eintr_once = 1;
    interop_linux_session_result_t poll_result = interop_linux_session_connect_peer(owner, 100, -1);
    assert(poll_result == InteropLinuxSessionTimedOut);
    assert(poll_eintr_once == 0);
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);

    make_config(name, 5123, &config, &device);
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);
    reset_connector(ConnectorPendingSpuriousWake);
    spurious_poll_once = 1;
    assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionTimedOut);
    assert(spurious_poll_once == 0);
    assert(close(pipe_fds[0]) == 0 && close(pipe_fds[1]) == 0);
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);
}

static void test_clock_range_boundaries(void) {
    const char *name = "clock-range-boundaries";
    make_profile_dirs(name);
    interop_linux_session_config_t config;
    device_info_t device;
    make_config(name, 5124, &config, &device);
    interop_linux_session_owner_t *owner = NULL;
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);
    reset_connector(ConnectorImmediateSuccess);
    clock_negative_seconds = 1;
    assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionClockError);
    assert(connector_socket_calls == 0);
    reset_connector(ConnectorImmediateSuccess);
    clock_large_nanoseconds = 1;
    assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionClockError);
    assert(connector_socket_calls == 0);
    reset_connector(ConnectorImmediateSuccess);
    clock_near_overflow = 1;
    assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionClockError);
    assert(connector_socket_calls == 0);
    reset_connector(ConnectorImmediateSuccess);
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);
}

static void test_peer_create_and_destroy_failures_are_retryable(void) {
    const char *name = "peer-create-destroy-faults";
    make_profile_dirs(name);
    interop_linux_session_config_t config;
    device_info_t device;
    make_config(name, 5118, &config, &device);
    interop_linux_session_owner_t *owner = NULL;
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);
    reset_connector(ConnectorImmediateSuccess);
    fail_peer_create_once = 1;
    assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionPeerCreateError);
    assert(owner != NULL && connector_peer_fd >= 0);
    assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionAttemptConsumed);
    assert(close(connector_peer_fd) == 0);
    connector_peer_fd = -1;

    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);
    make_config(name, 5118, &config, &device);
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);
    reset_connector(ConnectorImmediateSuccess);
    assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionSuccess);
    fail_peer_destroy_once = 1;
    assert(interop_linux_session_destroy_peer(owner) == InteropLinuxSessionBadState);
    assert(interop_linux_session_socket_owner(owner) != NULL);
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSocketOwnerNotDestroyed);
    assert(owner != NULL);
    assert(interop_linux_session_destroy_peer(owner) == InteropLinuxSessionSuccess);
    assert(connector_peer_fd >= 0 && close(connector_peer_fd) == 0);
    connector_peer_fd = -1;
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);
}

static void test_peer_destroy_close_diagnostic_releases_slot(void) {
    const char *name = "peer-destroy-close-diagnostic";
    make_profile_dirs(name);
    interop_linux_session_config_t config;
    device_info_t device;
    make_config(name, 5125, &config, &device);
    interop_linux_session_owner_t *owner = NULL;
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);
    reset_connector(ConnectorImmediateSuccess);
    assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionSuccess);
    peer_destroy_close_diagnostic_once = 1;
    assert(interop_linux_session_destroy_peer(owner) == InteropLinuxSessionCloseDiagnostic);
    assert(owner != NULL && interop_linux_session_socket_owner(owner) == NULL);
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);
    assert(connector_peer_fd >= 0 && close(connector_peer_fd) == 0);
    connector_peer_fd = -1;
}

static void test_reviewed_postconnect_boundaries(void) {
    make_profile_dirs("review-boundaries");
    for (unsigned scenario = 0; scenario < 6; scenario++) {
        interop_linux_session_config_t config;
        device_info_t device;
        make_config("review-boundaries", 5113, &config, &device);
        interop_linux_session_owner_t *owner = NULL;
        assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);
        reset_connector(scenario == 0 ? ConnectorPendingShortResult :
            scenario == 1 ? ConnectorPendingUnexpected : ConnectorImmediateSuccess);
        int pipe_fds[2];
        assert(pipe(pipe_fds) == 0);
        if (scenario == 2) clock_shift_after_first = 1;
        if (scenario == 3) clock_shift_after_first = -1;
        if (scenario == 4) cancel_after_connect_write_fd = pipe_fds[1];
        if (scenario == 5) cancel_poll_failure_after_connect = 1;
        int result = interop_linux_session_connect_peer(owner, 100, scenario >= 4 ? pipe_fds[0] : -1);
        assert(result == (scenario == 2 ? InteropLinuxSessionTimedOut :
            scenario == 3 ? InteropLinuxSessionClockError :
            scenario == 4 ? InteropLinuxSessionCancelled : InteropLinuxSessionConnectError));
        assert(interop_linux_session_socket_owner(owner) == NULL);
        assert(interop_linux_session_connect_peer(owner, 100, -1) == InteropLinuxSessionAttemptConsumed);
        assert(connector_socket_calls == 1 && connector_connect_calls == 1);
        clock_shift_after_first = 0;
        assert(close(pipe_fds[0]) == 0 && close(pipe_fds[1]) == 0);
        assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);
    }
}

static void test_release_reports_close_diagnostic_after_relinquishing(void) {
    make_profile_dirs("close-diagnostic");
    interop_linux_session_config_t config;
    device_info_t device;
    make_config("close-diagnostic", 5110, &config, &device);
    interop_linux_session_owner_t *owner = NULL;
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);
    fail_next_close = 1;
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionCloseDiagnostic);
    assert(owner == NULL && fail_next_close == 0);
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionInvalidArgument);
}

extern void linux_session_private_guard_tests(void);

int main(void) {
    configure_environment();
    test_port_conflict_precedes_filesystem();
    test_profile_validation_is_side_effect_free();
    test_profile_identity_and_root_resolution_fail_closed();
    test_symlink_lock_is_rejected_without_following();
    test_nonprivate_directory_and_bad_lock_file_are_rejected();
    test_directory_and_lock_syscall_failures_unwind_leases();
    test_lock_leaf_open_failures_unwind();
    test_lock_name_identity_is_rechecked_after_flock();
    test_profile_path_boundaries_fail_before_publishing();
    test_av_directory_symlink_fails_after_session_unwind();
    test_allocation_failure_preserves_output();
    test_lock_order_contention_and_release();
    test_connector_retry_consumption_and_peer_handoff();
    test_preconnect_cancel_and_postconnect_consumption();
    test_wrong_thread_is_rejected();
    test_connect_api_argument_boundaries_preserve_owner();
    test_timeout_consumes_in_progress_attempt();
    test_pending_syscall_errors_consume_attempt();
    test_clock_failures_preserve_retry_boundary();
    test_malformed_clock_values_and_preconnect_poll_error();
    test_definite_refusal_errno_set_remains_retryable();
    test_pending_connect_poll_clock_and_spurious_edges();
    test_clock_range_boundaries();
    test_peer_create_and_destroy_failures_are_retryable();
    test_peer_destroy_close_diagnostic_releases_slot();
    test_reviewed_postconnect_boundaries();
    test_release_reports_close_diagnostic_after_relinquishing();
    linux_session_private_guard_tests();
    assert(unsetenv("XDG_STATE_HOME") == 0);
    assert(unsetenv("XDG_RUNTIME_DIR") == 0);
    assert(nftw(temporary_root, remove_temporary_path, 16, FTW_DEPTH | FTW_PHYS) == 0);
    puts("linux_session_owner tests passed");
    return 0;
}
