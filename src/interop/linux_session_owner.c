#include "linux_session_owner.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <linux/vm_sockets.h>
#include <waddle/interop_protocol.h>

struct interop_linux_session_owner_t {
    pthread_t owner_thread;
    int session_fd;
    int av_fd;
    int session_dir_fd;
    int av_dir_fd;
    interop_linux_socket_t *socket_owner;
    bool attempt_consumed;
    uint32_t vsock_cid;
    uint32_t guest_port;
    uint32_t drain_timeout_ms;
    uint64_t connection_token;
    uint64_t local_capabilities;
    size_t agent_version_bytes;
    uint8_t agent_version[32];
    char device_name[WaddleMaxDeviceNameLen];
    char state_dir[WaddleMaxPathLen];
    char socket_path[WaddleMaxPathLen];
};

typedef struct pointer_range_t {
    uintptr_t begin;
    uintptr_t end;
} pointer_range_t;

static bool make_range(const void *pointer, size_t bytes, size_t alignment, pointer_range_t *out) {
    if (pointer == NULL || out == NULL || bytes == 0 || alignment == 0 ||
        ((uintptr_t)pointer % alignment) != 0 || (uintptr_t)pointer > UINTPTR_MAX - bytes) return false;
    out->begin = (uintptr_t)pointer;
    out->end = out->begin + bytes;
    return true;
}

static bool ranges_overlap(pointer_range_t left, pointer_range_t right) {
    return left.begin < right.end && right.begin < left.end;
}

static bool c_string_array(const char *value, size_t capacity, size_t *length) {
    if (value == NULL || capacity == 0) return false;
    const char *end = memchr(value, '\0', capacity);
    if (end == NULL || end == value) return false;
    if (length != NULL) *length = (size_t)(end - value);
    return true;
}

static bool valid_directory(int fd) {
    struct stat status;
    if (fstat(fd, &status) != 0) return false;
    if (!S_ISDIR(status.st_mode) || status.st_uid != geteuid() || (status.st_mode & 0077) != 0) {
        errno = EACCES;
        return false;
    }
    return true;
}

static bool valid_lock_file(int dir_fd, const char *name, int fd) {
    struct stat opened;
    struct stat named;
    if (fstat(fd, &opened) != 0) return false;
    if (!S_ISREG(opened.st_mode) || opened.st_uid != geteuid() ||
        (opened.st_mode & 07777) != 0600 || opened.st_nlink != 1) {
        errno = EACCES;
        return false;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) return false;
    if (fstatat(dir_fd, name, &named, AT_SYMLINK_NOFOLLOW) != 0) return false;
    if (!S_ISREG(named.st_mode) || named.st_dev != opened.st_dev || named.st_ino != opened.st_ino) {
        errno = EACCES;
        return false;
    }
    return true;
}

static void close_owned(int *fd, bool *failed) {
    if (*fd >= 0) {
        int value = *fd;
        *fd = -1;
        if (close(value) != 0) *failed = true;
    }
}

static void unwind_acquire(interop_linux_session_owner_t *owner, bool *close_failed) {
    close_owned(&owner->av_fd, close_failed);
    close_owned(&owner->av_dir_fd, close_failed);
    close_owned(&owner->session_fd, close_failed);
    close_owned(&owner->session_dir_fd, close_failed);
}

static bool owner_valid(const interop_linux_session_owner_t *owner) {
    pointer_range_t range;
    return make_range(owner, sizeof(*owner), alignof(interop_linux_session_owner_t), &range);
}

static bool on_owner_thread(const interop_linux_session_owner_t *owner) {
    return pthread_equal(owner->owner_thread, pthread_self()) != 0;
}

static bool path_parent(const char *path, char *parent, size_t capacity) {
    size_t length = 0;
    if (!c_string_array(path, WaddleMaxPathLen, &length) || path[0] != '/' || length >= capacity) {
        errno = EINVAL;
        return false;
    }
    const char suffix[] = "/daemon.sock";
    const size_t suffix_bytes = sizeof(suffix) - 1;
    if (length <= suffix_bytes || memcmp(path + length - suffix_bytes, suffix, suffix_bytes) != 0) {
        errno = EINVAL;
        return false;
    }
    size_t parent_length = length - suffix_bytes;
    if (parent_length >= capacity) { errno = ENAMETOOLONG; return false; }
    memcpy(parent, path, parent_length);
    parent[parent_length] = '\0';
    return true;
}

static bool append_path(char *output, size_t capacity, const char *base, const char *leaf) {
    size_t base_length = strlen(base);
    size_t leaf_length = strlen(leaf);
    bool has_slash = base_length != 0 && base[base_length - 1] == '/';
    size_t separator_bytes = has_slash ? 0 : 1;
    if (capacity == 0 || base_length == 0 || base_length >= capacity ||
        separator_bytes > capacity - 1 - base_length ||
        leaf_length > capacity - 1 - base_length - separator_bytes) {
        errno = ENAMETOOLONG;
        return false;
    }
    memcpy(output, base, base_length);
    if (has_slash) {
        memcpy(output + base_length, leaf, leaf_length + 1);
    } else {
        output[base_length] = '/';
        memcpy(output + base_length + 1, leaf, leaf_length + 1);
    }
    return true;
}

static bool valid_semver(const uint8_t *version, size_t length) {
    uint8_t frame[64] = {0};
    interop_writer_t writer;
    interop_writer_init(&writer, frame, sizeof(frame));
    return interop_put_bytes(&writer, InteropTagAgentVersion, version,
                             (uint16_t)length) == InteropErrorNone;
}

static interop_linux_session_result_t config_validate(
    const interop_linux_session_config_t *config,
    interop_linux_session_owner_t **out_owner) {
    pointer_range_t config_range, output_range, device_range, version_range;
    if (!make_range(config, sizeof(*config), alignof(interop_linux_session_config_t), &config_range) ||
        !make_range(out_owner, sizeof(*out_owner), alignof(interop_linux_session_owner_t *), &output_range) ||
        ranges_overlap(config_range, output_range) ||
        !make_range(config->device, sizeof(*config->device), alignof(device_info_t), &device_range) ||
        !make_range(config->agent_version, config->agent_version_bytes, 1, &version_range) ||
        ranges_overlap(config_range, device_range) || ranges_overlap(config_range, version_range) ||
        ranges_overlap(device_range, version_range) || ranges_overlap(output_range, device_range) ||
        ranges_overlap(output_range, version_range) || *out_owner != NULL) return InteropLinuxSessionInvalidArgument;

    const device_info_t *device = config->device;
    size_t name_length = 0, state_length = 0, socket_length = 0;
    if (config->reserved_zero != 0 || config->connection_token == 0 ||
        config->drain_timeout_ms == 0 || config->drain_timeout_ms > InteropLinuxSocketMaxDrainTimeoutMs ||
        (config->local_capabilities & ~InteropCapabilityMask) != 0 ||
        config->agent_version_bytes < 5 || config->agent_version_bytes > 32 ||
        !valid_semver(config->agent_version, config->agent_version_bytes) ||
        device->config_valid != 1 || device->vsock_cid < 3 || device->vsock_cid == UINT32_MAX ||
        device->vsock_port == 0 ||
        !c_string_array(device->name, sizeof(device->name), &name_length) ||
        !c_string_array(device->state_dir, sizeof(device->state_dir), &state_length) ||
        !c_string_array(device->socket_path, sizeof(device->socket_path), &socket_length) ||
        daemon_device_validate_name(device->name) != 0) return InteropLinuxSessionInvalidArgument;
    (void)name_length;
    (void)state_length;
    (void)socket_length;
    if (device->vsock_port == InteropControlPort) return InteropLinuxSessionGuestPortConflict;
    return InteropLinuxSessionSuccess;
}

interop_linux_session_result_t interop_linux_session_acquire(
    const interop_linux_session_config_t *config,
    interop_linux_session_owner_t **out_owner) {
    if (config == NULL || out_owner == NULL) return InteropLinuxSessionInvalidArgument;
    interop_linux_session_result_t result = config_validate(config, out_owner);
    if (result != InteropLinuxSessionSuccess) return result;

    char expected_state[WaddleMaxPathLen] = {0};
    char expected_socket[WaddleMaxPathLen] = {0};
    if (daemon_device_get_state_dir(config->device->name, expected_state, sizeof(expected_state)) != 0 ||
        daemon_device_get_socket_path(config->device->name, expected_socket, sizeof(expected_socket)) != 0) {
        return InteropLinuxSessionOsError;
    }
    if (strcmp(config->device->state_dir, expected_state) != 0 ||
        strcmp(config->device->socket_path, expected_socket) != 0) {
        errno = EINVAL;
        return InteropLinuxSessionInvalidArgument;
    }

    interop_linux_session_owner_t *owner = calloc(1, sizeof(*owner));
    if (owner == NULL) return InteropLinuxSessionOutOfMemory;
    owner->session_fd = owner->av_fd = owner->session_dir_fd = owner->av_dir_fd = -1;
    owner->owner_thread = pthread_self();
    owner->vsock_cid = config->device->vsock_cid;
    owner->guest_port = config->device->vsock_port;
    owner->drain_timeout_ms = config->drain_timeout_ms;
    owner->connection_token = config->connection_token;
    owner->local_capabilities = config->local_capabilities;
    owner->agent_version_bytes = config->agent_version_bytes;
    memcpy(owner->agent_version, config->agent_version, config->agent_version_bytes);
    memcpy(owner->device_name, config->device->name, strlen(config->device->name) + 1);
    memcpy(owner->state_dir, expected_state, strlen(expected_state) + 1);
    memcpy(owner->socket_path, expected_socket, strlen(expected_socket) + 1);

    char runtime_dir[WaddleMaxPathLen];
    char av_dir[WaddleMaxPathLen];
    if (!path_parent(owner->socket_path, runtime_dir, sizeof(runtime_dir)) ||
        !append_path(av_dir, sizeof(av_dir), owner->state_dir, "av")) {
        result = InteropLinuxSessionOsError;
        goto fail;
    }

    owner->session_dir_fd = daemon_device_open_directory(runtime_dir, 1);
    if (owner->session_dir_fd < 0 || !valid_directory(owner->session_dir_fd)) {
        result = InteropLinuxSessionOsError;
        goto fail;
    }
    owner->session_fd = openat(owner->session_dir_fd, "session.lock",
                               O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (owner->session_fd < 0) { result = InteropLinuxSessionOsError; goto fail; }
    if (!valid_lock_file(owner->session_dir_fd, "session.lock", owner->session_fd)) {
        result = (errno == EWOULDBLOCK || errno == EAGAIN) ? InteropLinuxSessionBusy : InteropLinuxSessionOsError;
        goto fail;
    }

    owner->av_dir_fd = daemon_device_open_directory(av_dir, 1);
    if (owner->av_dir_fd < 0 || !valid_directory(owner->av_dir_fd)) {
        result = InteropLinuxSessionOsError;
        goto fail;
    }
    owner->av_fd = openat(owner->av_dir_fd, "command.lock",
                          O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (owner->av_fd < 0) { result = InteropLinuxSessionOsError; goto fail; }
    if (!valid_lock_file(owner->av_dir_fd, "command.lock", owner->av_fd)) {
        result = (errno == EWOULDBLOCK || errno == EAGAIN) ? InteropLinuxSessionAvBusy : InteropLinuxSessionOsError;
        goto fail;
    }
    *out_owner = owner;
    return InteropLinuxSessionSuccess;

fail: {
        int saved_errno = errno;
        bool close_failed = false;
        unwind_acquire(owner, &close_failed);
        free(owner);
        errno = saved_errno;
        return close_failed ? InteropLinuxSessionOsError : result;
    }
}

static uint64_t monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return UINT64_MAX;
    if (now.tv_sec < 0 || now.tv_nsec < 0 || now.tv_nsec >= 1000000000L ||
        (uint64_t)now.tv_sec > (UINT64_MAX - (uint64_t)now.tv_nsec / 1000000u) / 1000u) {
        errno = EOVERFLOW;
        return UINT64_MAX;
    }
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static bool definite_refusal(int error_number) {
    return error_number == ECONNREFUSED || error_number == ENETUNREACH ||
           error_number == EHOSTUNREACH || error_number == ENODEV;
}

static interop_linux_session_result_t connect_failure(
    interop_linux_session_owner_t *owner, int error_number, bool consumed) {
    owner->attempt_consumed = consumed;
    errno = error_number;
    if (error_number == ECANCELED) return InteropLinuxSessionCancelled;
    if (error_number == ETIMEDOUT) return InteropLinuxSessionTimedOut;
    return InteropLinuxSessionConnectError;
}

interop_linux_session_result_t interop_linux_session_connect_peer(
    interop_linux_session_owner_t *owner,
    uint32_t connect_timeout_ms,
    int cancel_fd) {
    pointer_range_t owner_range;
    if (!owner_valid(owner) || !make_range(owner, sizeof(*owner), alignof(interop_linux_session_owner_t), &owner_range) ||
        connect_timeout_ms == 0 || connect_timeout_ms > 2000 || cancel_fd < -1) return InteropLinuxSessionInvalidArgument;
    if (!on_owner_thread(owner)) return InteropLinuxSessionWrongThread;
    if (owner->socket_owner != NULL) return InteropLinuxSessionPeerAlreadyCreated;
    if (owner->attempt_consumed) return InteropLinuxSessionAttemptConsumed;

    if (cancel_fd >= 0) {
        struct pollfd cancel = {.fd = cancel_fd, .events = POLLIN | POLLERR | POLLHUP};
        int cancel_ready = poll(&cancel, 1, 0);
        if (cancel_ready > 0 && cancel.revents != 0) {
            errno = ECANCELED;
            return InteropLinuxSessionCancelled;
        }
        if (cancel_ready < 0 && errno != EINTR) return InteropLinuxSessionOsError;
    }

    errno = 0;
    uint64_t started = monotonic_ms();
    if (started == UINT64_MAX || started > UINT64_MAX - connect_timeout_ms) return InteropLinuxSessionClockError;
    uint64_t deadline = started + connect_timeout_ms;
    int fd = socket(AF_VSOCK, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return InteropLinuxSessionConnectError;

    struct sockaddr_vm address;
    memset(&address, 0, sizeof(address));
    address.svm_family = AF_VSOCK;
    address.svm_cid = owner->vsock_cid;
    address.svm_port = InteropControlPort;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
        owner->attempt_consumed = true;
    } else if (errno != EINPROGRESS && errno != EALREADY && errno != EINTR) {
        int saved = errno;
        bool consumed = !definite_refusal(saved);
        (void)close(fd);
        return connect_failure(owner, saved, consumed);
    } else {
        owner->attempt_consumed = true;
        for (;;) {
            errno = 0;
            uint64_t now = monotonic_ms();
            if (now == UINT64_MAX) {
                int saved = errno == 0 ? EIO : errno;
                (void)close(fd);
                errno = saved;
                return InteropLinuxSessionClockError;
            }
            if (cancel_fd >= 0) {
                struct pollfd cancel = {.fd = cancel_fd, .events = POLLIN | POLLERR | POLLHUP};
                int cancel_ready = poll(&cancel, 1, 0);
                if (cancel_ready > 0 && cancel.revents != 0) {
                    (void)close(fd);
                    errno = ECANCELED;
                    return InteropLinuxSessionCancelled;
                }
                if (cancel_ready < 0 && errno != EINTR) {
                    int saved = errno; (void)close(fd); errno = saved;
                    return connect_failure(owner, saved, true);
                }
            }
            if (now >= deadline) {
                (void)close(fd);
                errno = ETIMEDOUT;
                return InteropLinuxSessionTimedOut;
            }
            uint64_t remaining = deadline - now;
            int timeout = remaining > INT32_MAX ? INT32_MAX : (int)remaining;
            struct pollfd fds[2] = {{.fd = fd, .events = POLLOUT | POLLERR | POLLHUP},
                                    {.fd = cancel_fd, .events = POLLIN | POLLERR | POLLHUP}};
            int ready = poll(fds, cancel_fd >= 0 ? 2 : 1, timeout);
            if (ready < 0) {
                if (errno == EINTR) continue;
                int saved = errno; (void)close(fd);
                return connect_failure(owner, saved, true);
            }
            if (ready == 0) continue;
            if (cancel_fd >= 0 && fds[1].revents != 0) {
                (void)close(fd); errno = ECANCELED;
                return InteropLinuxSessionCancelled;
            }
            if (fds[0].revents == 0) continue;
            int socket_error = 0;
            socklen_t error_length = sizeof(socket_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) != 0) {
                int saved = errno; (void)close(fd);
                return connect_failure(owner, saved, true);
            }
            if (error_length != sizeof(socket_error)) {
                (void)close(fd);
                return connect_failure(owner, EPROTO, true);
            }
            if (socket_error != 0) {
                (void)close(fd);
                return connect_failure(owner, socket_error, !definite_refusal(socket_error));
            }
            if ((fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 ||
                (fds[0].revents & POLLOUT) == 0) {
                (void)close(fd);
                return connect_failure(owner, ECONNRESET, true);
            }
            owner->attempt_consumed = true;
            break;
        }
    }

    errno = 0;
    uint64_t accepted_at_ms = monotonic_ms();
    if (accepted_at_ms == UINT64_MAX || accepted_at_ms < started) {
        int saved = errno == 0 ? EIO : errno;
        (void)close(fd);
        owner->attempt_consumed = true;
        errno = saved;
        return InteropLinuxSessionClockError;
    }
    if (accepted_at_ms >= deadline) {
        (void)close(fd);
        return connect_failure(owner, ETIMEDOUT, true);
    }
    if (cancel_fd >= 0) {
        struct pollfd cancel = {.fd = cancel_fd, .events = POLLIN | POLLERR | POLLHUP};
        int cancel_ready = poll(&cancel, 1, 0);
        if (cancel_ready > 0 && cancel.revents != 0) {
            (void)close(fd);
            return connect_failure(owner, ECANCELED, true);
        }
        if (cancel_ready < 0) {
            int saved = errno;
            (void)close(fd);
            return connect_failure(owner, saved, true);
        }
    }
    interop_linux_socket_config_t socket_config;
    memset(&socket_config, 0, sizeof(socket_config));
    socket_config.peer_config.role = InteropPeerHost;
    socket_config.peer_config.connection_token = owner->connection_token;
    socket_config.peer_config.accepted_at_ms = accepted_at_ms;
    socket_config.peer_config.agent_version = owner->agent_version;
    socket_config.peer_config.agent_version_bytes = owner->agent_version_bytes;
    socket_config.peer_config.local_capabilities = owner->local_capabilities;
    socket_config.drain_timeout_ms = owner->drain_timeout_ms;
    int peer_result = interop_linux_socket_create(fd, &socket_config, &owner->socket_owner);
    if (peer_result != InteropLinuxSocketSuccess) {
        int saved = errno;
        (void)close(fd);
        owner->attempt_consumed = true;
        errno = saved;
        return InteropLinuxSessionPeerCreateError;
    }
    owner->attempt_consumed = true;
    return InteropLinuxSessionSuccess;
}

interop_linux_socket_t *interop_linux_session_socket_owner(
    const interop_linux_session_owner_t *owner) {
    if (!owner_valid(owner)) { errno = EINVAL; return NULL; }
    if (!on_owner_thread(owner)) { errno = EPERM; return NULL; }
    return owner->socket_owner;
}

interop_linux_session_result_t interop_linux_session_destroy_peer(
    interop_linux_session_owner_t *owner) {
    if (!owner_valid(owner)) return InteropLinuxSessionInvalidArgument;
    if (!on_owner_thread(owner)) return InteropLinuxSessionWrongThread;
    if (owner->socket_owner == NULL) return InteropLinuxSessionSuccess;
    int result = interop_linux_socket_destroy(&owner->socket_owner);
    if (result == InteropLinuxSocketSuccess || result == InteropLinuxSocketCloseDiagnostic) {
        return result == InteropLinuxSocketSuccess ? InteropLinuxSessionSuccess : InteropLinuxSessionCloseDiagnostic;
    }
    return InteropLinuxSessionBadState;
}

interop_linux_session_result_t interop_linux_session_release(
    interop_linux_session_owner_t **owner_slot) {
    pointer_range_t slot_range;
    if (!make_range(owner_slot, sizeof(*owner_slot), alignof(interop_linux_session_owner_t *), &slot_range))
        return InteropLinuxSessionInvalidArgument;
    /* Stage pointer bytes before rejecting a slot that aliases opaque owner storage. */
    interop_linux_session_owner_t *owner;
    memcpy(&owner, owner_slot, sizeof(owner));
    if (owner == NULL) return InteropLinuxSessionInvalidArgument;
    pointer_range_t owner_range;
    if (!owner_valid(owner) || !make_range(owner, sizeof(*owner), alignof(interop_linux_session_owner_t), &owner_range) ||
        ranges_overlap(slot_range, owner_range)) return InteropLinuxSessionInvalidArgument;
    if (!on_owner_thread(owner)) return InteropLinuxSessionWrongThread;
    if (owner->socket_owner != NULL) return InteropLinuxSessionSocketOwnerNotDestroyed;

    *owner_slot = NULL;
    bool close_failed = false;
    close_owned(&owner->av_fd, &close_failed);
    close_owned(&owner->av_dir_fd, &close_failed);
    close_owned(&owner->session_fd, &close_failed);
    close_owned(&owner->session_dir_fd, &close_failed);
    memset(owner, 0, sizeof(*owner));
    free(owner);
    return close_failed ? InteropLinuxSessionCloseDiagnostic : InteropLinuxSessionSuccess;
}
