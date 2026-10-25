#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../../src/interop/session_host.h"

#include "../../src/daemon/daemon_wire.h"
#include "waddle/daemon_protocol.h"
#include "waddle/interop_protocol.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <pthread.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/vm_sockets.h>

typedef enum daemon_fixture_mode_t {
    DaemonFixtureSuccess,
    DaemonFixtureNotRunning,
    DaemonFixtureWrongCid,
    DaemonFixtureWrongGuestPort,
    DaemonFixtureGuestUnavailable,
    DaemonFixtureMalformedStatus,
    DaemonFixturePartialStatus,
    DaemonFixtureCancelStatus,
    DaemonFixtureDelayStatus,
    DaemonFixtureWrongSequence,
    DaemonFixtureBadHeader,
    DaemonFixtureWrongStatusLength,
    DaemonFixtureStatusError,
    DaemonFixtureMalformedError,
    DaemonFixtureMalformedReady,
    DaemonFixtureWrongReadyCid,
    DaemonFixtureInvalidReadyBody
} daemon_fixture_mode_t;

void session_host_private_tests(void);

static char temporary_root[WaddleMaxPathLen];
int vsock_connect_count;
int vsock_peer_fd = -1;
static int vsock_connect_errno;
int session_host_test_peer_uid_override = -1;
static int cancel_write_fd = -1;
static uint64_t host_clock_offset_ms;
static int advance_clock_after_peer_success;
static int fail_peer_destroy_once;
int session_host_test_fail_poll_errno_once;
int session_host_test_force_poll_nval_once;
int session_host_test_force_cancel_poll_nval_once;
int session_host_test_force_poll_unrelated_once;
static int fail_clock_once;
static int send_eintr_once;
int session_host_test_send_eagain_once;
static int send_epipe_once;
static int recv_eintr_once;
static int recv_eagain_once;
int session_host_test_recv_eof_once;
int session_host_test_socket_type_override;
int session_host_test_socket_type_fail_once;
int session_host_test_bad_socket_type_length;
int session_host_test_peer_family_override;
int session_host_test_getpeername_fail_once;
int session_host_test_peercred_fail_once;
int session_host_test_bad_peercred_length;
int session_host_test_peer_pid_override = -1;
int session_host_test_release_failure_once;
int session_host_test_destroy_diagnostic_once;
int session_host_test_destroy_diagnostic_errno = EIO;
int session_host_test_clock_after_acquire;
int session_host_test_clock_after_status_decode;
int session_host_test_clock_after_ready_decode;
int session_host_test_cancel_after_acquire;
int session_host_test_cancel_after_ready_decode;
int session_host_test_cancel_after_connect;
static size_t send_partial_limit;
static int fixture_close_after_status;
int session_host_test_clock_mode;

static int remove_path(const char *path, const struct stat *status,
                       int type, struct FTW *walk_state)
{
    (void)status;
    (void)type;
    (void)walk_state;
    return remove(path);
}

typedef struct daemon_fixture_arg_t {
    int fd;
    daemon_fixture_mode_t mode;
} daemon_fixture_arg_t;

extern int __real_socket(int domain, int type, int protocol);
extern int __real_connect(int fd, const struct sockaddr *address, socklen_t address_length);
extern int __real_getsockopt(int fd, int level, int option, void *value, socklen_t *length);
extern int __real_getpeername(int fd, struct sockaddr *address, socklen_t *address_length);
extern int __real_poll(struct pollfd *descriptors, nfds_t count, int timeout);
extern ssize_t __real_send(int fd, const void *bytes, size_t length, int flags);
extern ssize_t __real_recv(int fd, void *bytes, size_t length, int flags);
extern int __real_clock_gettime(clockid_t clock_id, struct timespec *now);
extern int __real_daemon_wire_decode_status_response(const uint8_t *bytes, size_t length,
                                                       waddle_daemon_status_resp_t *out);
extern int __real_daemon_wire_decode_ready_response(const uint8_t *bytes, size_t length,
                                                      waddle_daemon_guest_ready_resp_t *out);
extern interop_linux_session_result_t __real_interop_linux_session_acquire(
    const interop_linux_session_config_t *config, interop_linux_session_owner_t **owner);
extern interop_linux_session_result_t __real_interop_linux_session_connect_peer(
    interop_linux_session_owner_t *owner, uint32_t timeout_ms, int cancel_fd);
extern interop_linux_session_result_t __real_interop_linux_session_destroy_peer(
    interop_linux_session_owner_t *owner);
extern interop_linux_session_result_t __real_interop_linux_session_release(
    interop_linux_session_owner_t **owner);

int __wrap_clock_gettime(clockid_t clock_id, struct timespec *now)
{
    if (session_host_test_clock_mode != 0 && clock_id == CLOCK_MONOTONIC) {
        int mode = session_host_test_clock_mode;
        session_host_test_clock_mode = 0;
        if (mode == 1) { errno = EIO; return -1; }
        if (mode == 2) { now->tv_sec = (time_t)-1; now->tv_nsec = 0; return 0; }
        if (mode == 3) { now->tv_sec = 0; now->tv_nsec = 1000000000L; return 0; }
        if (mode == 4) {
            now->tv_sec = (time_t)(UINT64_MAX / 1000u + 1u);
            now->tv_nsec = 0;
            return 0;
        }
    }
    if (fail_clock_once && clock_id == CLOCK_MONOTONIC) {
        fail_clock_once = 0;
        errno = EIO;
        return -1;
    }
    int result = __real_clock_gettime(clock_id, now);
    if (result == 0 && clock_id == CLOCK_MONOTONIC && host_clock_offset_ms != 0) {
        uint64_t value = (uint64_t)now->tv_sec * 1000u +
            (uint64_t)now->tv_nsec / 1000000u + host_clock_offset_ms;
        now->tv_sec = (time_t)(value / 1000u);
        now->tv_nsec = (long)(value % 1000u) * 1000000L;
    }
    return result;
}

interop_linux_session_result_t __wrap_interop_linux_session_connect_peer(
    interop_linux_session_owner_t *owner, uint32_t timeout_ms, int cancel_fd)
{
    interop_linux_session_result_t result = __real_interop_linux_session_connect_peer(
        owner, timeout_ms, cancel_fd);
    if (result == InteropLinuxSessionSuccess && advance_clock_after_peer_success)
        host_clock_offset_ms = 5000;
    if (result == InteropLinuxSessionSuccess && session_host_test_cancel_after_connect) {
        session_host_test_cancel_after_connect = 0;
        assert(cancel_write_fd >= 0);
        assert(write(cancel_write_fd, "x", 1) == 1);
    }
    return result;
}

interop_linux_session_result_t __wrap_interop_linux_session_acquire(
    const interop_linux_session_config_t *config, interop_linux_session_owner_t **owner)
{
    interop_linux_session_result_t result = __real_interop_linux_session_acquire(config, owner);
    if (result == InteropLinuxSessionSuccess && session_host_test_clock_after_acquire) {
        session_host_test_clock_after_acquire = 0;
        host_clock_offset_ms = 5000;
    }
    if (result == InteropLinuxSessionSuccess && session_host_test_cancel_after_acquire) {
        session_host_test_cancel_after_acquire = 0;
        assert(cancel_write_fd >= 0);
        assert(write(cancel_write_fd, "x", 1) == 1);
    }
    return result;
}

int __wrap_daemon_wire_decode_status_response(const uint8_t *bytes, size_t length,
                                               waddle_daemon_status_resp_t *out)
{
    int result = __real_daemon_wire_decode_status_response(bytes, length, out);
    if (result == 0 && session_host_test_clock_after_status_decode) {
        session_host_test_clock_after_status_decode = 0;
        host_clock_offset_ms = 5000;
    }
    return result;
}

int __wrap_daemon_wire_decode_ready_response(const uint8_t *bytes, size_t length,
                                              waddle_daemon_guest_ready_resp_t *out)
{
    int result = __real_daemon_wire_decode_ready_response(bytes, length, out);
    if (result == 0 && session_host_test_clock_after_ready_decode) {
        session_host_test_clock_after_ready_decode = 0;
        host_clock_offset_ms = 5000;
    }
    if (result == 0 && session_host_test_cancel_after_ready_decode) {
        session_host_test_cancel_after_ready_decode = 0;
        assert(cancel_write_fd >= 0);
        assert(write(cancel_write_fd, "x", 1) == 1);
    }
    return result;
}

interop_linux_session_result_t __wrap_interop_linux_session_destroy_peer(
    interop_linux_session_owner_t *owner)
{
    if (fail_peer_destroy_once) {
        fail_peer_destroy_once = 0;
        errno = EIO;
        return InteropLinuxSessionBadState;
    }
    interop_linux_session_result_t result = __real_interop_linux_session_destroy_peer(owner);
    if (result == InteropLinuxSessionSuccess && session_host_test_destroy_diagnostic_once) {
        session_host_test_destroy_diagnostic_once = 0;
        errno = session_host_test_destroy_diagnostic_errno;
        return InteropLinuxSessionCloseDiagnostic;
    }
    return result;
}

interop_linux_session_result_t __wrap_interop_linux_session_release(
    interop_linux_session_owner_t **owner)
{
    if (session_host_test_release_failure_once) {
        session_host_test_release_failure_once = 0;
        errno = EIO;
        return InteropLinuxSessionBadState;
    }
    return __real_interop_linux_session_release(owner);
}

int __wrap_getpeername(int fd, struct sockaddr *address, socklen_t *address_length)
{
    if (session_host_test_getpeername_fail_once) {
        session_host_test_getpeername_fail_once = 0;
        errno = EIO;
        return -1;
    }
    if (__real_getpeername(fd, address, address_length) == 0) {
        if (session_host_test_peer_family_override != 0)
            ((struct sockaddr *)address)->sa_family =
                (sa_family_t)session_host_test_peer_family_override;
        return 0;
    }
    if (errno != EPERM) return -1;
    if (address == NULL || address_length == NULL || *address_length < sizeof(struct sockaddr_un)) {
        errno = EINVAL;
        return -1;
    }
    memset(address, 0, sizeof(struct sockaddr_un));
    ((struct sockaddr_un *)address)->sun_family = AF_UNIX;
    if (session_host_test_peer_family_override != 0)
        ((struct sockaddr *)address)->sa_family =
            (sa_family_t)session_host_test_peer_family_override;
    *address_length = sizeof(struct sockaddr_un);
    return 0;
}

int __wrap_socket(int domain, int type, int protocol)
{
    if (domain != AF_VSOCK)
        return __real_socket(domain, type, protocol);
    assert((type & SOCK_STREAM) != 0);
    assert((type & SOCK_NONBLOCK) != 0);
    assert((type & SOCK_CLOEXEC) != 0);
    int descriptors[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                   0, descriptors) != 0)
        return -1;
    assert(vsock_peer_fd == -1);
    vsock_peer_fd = descriptors[1];
    return descriptors[0];
}

int __wrap_connect(int fd, const struct sockaddr *address, socklen_t address_length)
{
    if (address->sa_family != AF_VSOCK)
        return __real_connect(fd, address, address_length);
    ++vsock_connect_count;
    assert(address_length == sizeof(struct sockaddr_vm));
    const struct sockaddr_vm *vm_address = (const struct sockaddr_vm *)address;
    assert(vm_address->svm_family == AF_VSOCK);
    assert(vm_address->svm_cid == 33);
    assert(vm_address->svm_port == InteropControlPort);
    if (vsock_connect_errno != 0) {
        errno = vsock_connect_errno;
        return -1;
    }
    return 0;
}

int __wrap_getsockopt(int fd, int level, int option, void *value, socklen_t *length)
{
    if (level == SOL_SOCKET && option == SO_TYPE) {
        if (session_host_test_socket_type_fail_once) {
            session_host_test_socket_type_fail_once = 0;
            errno = EIO;
            return -1;
        }
        if (session_host_test_socket_type_override != 0) {
            if (length == NULL || *length < sizeof(int)) { errno = EINVAL; return -1; }
            *(int *)value = session_host_test_socket_type_override;
            *length = session_host_test_bad_socket_type_length ? sizeof(int) - 1 : sizeof(int);
            session_host_test_bad_socket_type_length = 0;
            return 0;
        }
        if (__real_getsockopt(fd, level, option, value, length) == 0) return 0;
        if (errno != EPERM) return -1;
        if (length == NULL || *length < sizeof(int)) { errno = EINVAL; return -1; }
        *(int *)value = SOCK_STREAM;
        *length = session_host_test_bad_socket_type_length ? sizeof(int) - 1 : sizeof(int);
        session_host_test_bad_socket_type_length = 0;
        return 0;
    }
    if (level == SOL_SOCKET && option == SO_PEERCRED) {
        if (session_host_test_peercred_fail_once) {
            session_host_test_peercred_fail_once = 0;
            errno = EIO;
            return -1;
        }
        if (session_host_test_peer_uid_override < 0) {
            if (__real_getsockopt(fd, level, option, value, length) == 0) return 0;
            if (errno != EPERM) return -1;
        }
        if (length == NULL || *length < sizeof(struct ucred)) {
            errno = EINVAL;
            return -1;
        }
        struct ucred *credentials = value;
        *credentials = (struct ucred){.pid = session_host_test_peer_pid_override != -1 ?
                session_host_test_peer_pid_override : getpid(),
            .uid = (uid_t)session_host_test_peer_uid_override, .gid = getegid()};
        *length = session_host_test_bad_peercred_length ?
            sizeof(*credentials) - 1 : sizeof(*credentials);
        session_host_test_bad_peercred_length = 0;
        return 0;
    }
    return __real_getsockopt(fd, level, option, value, length);
}

ssize_t __wrap_send(int fd, const void *bytes, size_t length, int flags)
{
    if (send_eintr_once) { send_eintr_once = 0; errno = EINTR; return -1; }
    if (session_host_test_send_eagain_once) {
        session_host_test_send_eagain_once = 0;
        errno = EAGAIN;
        return -1;
    }
    if (send_epipe_once) { send_epipe_once = 0; errno = EPIPE; return -1; }
    if (send_partial_limit != 0 && length > send_partial_limit)
        length = send_partial_limit;
    ssize_t result = __real_send(fd, bytes, length, flags);
    if (result < 0 && errno == EPERM) result = write(fd, bytes, length);
    if (result > 0) send_partial_limit = 0;
    return result;
}

ssize_t __wrap_recv(int fd, void *bytes, size_t length, int flags)
{
    if (recv_eintr_once) { recv_eintr_once = 0; errno = EINTR; return -1; }
    if (recv_eagain_once) { recv_eagain_once = 0; errno = EAGAIN; return -1; }
    if (session_host_test_recv_eof_once) {
        session_host_test_recv_eof_once = 0;
        return 0;
    }
    ssize_t result = __real_recv(fd, bytes, length, flags);
    if (result < 0 && errno == EPERM) result = read(fd, bytes, length);
    return result;
}

int __wrap_poll(struct pollfd *descriptors, nfds_t count, int timeout)
{
    if (session_host_test_fail_poll_errno_once != 0) {
        errno = session_host_test_fail_poll_errno_once;
        session_host_test_fail_poll_errno_once = 0;
        return -1;
    }
    if (session_host_test_force_poll_nval_once) {
        session_host_test_force_poll_nval_once = 0;
        for (nfds_t index = 0; index < count; ++index) descriptors[index].revents = 0;
        descriptors[0].revents = POLLNVAL;
        return 1;
    }
    if (session_host_test_force_cancel_poll_nval_once) {
        session_host_test_force_cancel_poll_nval_once = 0;
        for (nfds_t index = 0; index < count; ++index) descriptors[index].revents = 0;
        assert(count == 2);
        descriptors[1].revents = POLLNVAL;
        return 1;
    }
    if (session_host_test_force_poll_unrelated_once) {
        session_host_test_force_poll_unrelated_once = 0;
        for (nfds_t index = 0; index < count; ++index) descriptors[index].revents = 0;
        descriptors[0].revents = POLLPRI;
        return 1;
    }
    return __real_poll(descriptors, count, timeout);
}

static void read_exact(int fd, void *output, size_t length)
{
    uint8_t *bytes = output;
    size_t offset = 0;
    while (offset < length) {
        ssize_t amount = read(fd, bytes + offset, length - offset);
        if (amount < 0 && errno == EINTR) continue;
        assert(amount > 0);
        offset += (size_t)amount;
    }
}

static void write_exact(int fd, const void *input, size_t length)
{
    const uint8_t *bytes = input;
    size_t offset = 0;
    while (offset < length) {
        ssize_t amount = write(fd, bytes + offset, length - offset);
        if (amount < 0 && errno == EINTR) continue;
        assert(amount > 0);
        offset += (size_t)amount;
    }
}

static void encode_response_header(uint8_t bytes[WaddleDaemonHeaderSize],
                                   uint16_t message_type, uint32_t sequence,
                                   uint32_t payload_length)
{
    waddle_daemon_header_t header = {
        .magic = WaddleDaemonMagic,
        .version = WaddleDaemonVersion,
        .msg_type = message_type,
        .sequence = sequence,
        .payload_len = payload_length
    };
    memcpy(bytes, &header, sizeof(header));
}

static void *daemon_thread(void *opaque)
{
    daemon_fixture_arg_t *argument = opaque;
    for (uint32_t request_index = 0; request_index < 2; ++request_index) {
        uint8_t request_header_bytes[WaddleDaemonHeaderSize];
        waddle_daemon_header_t request_header;
        read_exact(argument->fd, request_header_bytes, sizeof(request_header_bytes));
        assert(daemon_wire_decode_header(request_header_bytes,
            sizeof(request_header_bytes), &request_header) == 0);
        if (request_index == 0) {
            assert(request_header.msg_type == DaemonMsgStatusReq);
            assert(request_header.payload_len == 0);
            waddle_daemon_status_resp_t status;
            memset(&status, 0, sizeof(status));
            status.subsystem_state = argument->mode == DaemonFixtureNotRunning ?
                SubsystemStateStopped : SubsystemStateRunning;
            status.daemon_pid = (uint32_t)getpid();
            status.qemu_pid = (uint32_t)getpid();
            status.vsock_cid = argument->mode == DaemonFixtureWrongCid ? 34 : 33;
            status.vsock_port = argument->mode == DaemonFixtureWrongGuestPort ? 5243 : 5242;
            status.memory_mb = 4096;
            status.vcpus = 4;
            if (argument->mode == DaemonFixtureMalformedStatus) {
                status.reserved = 1;
            }
            uint8_t response_header[WaddleDaemonHeaderSize];
            uint16_t response_type = argument->mode == DaemonFixtureStatusError ||
                argument->mode == DaemonFixtureMalformedError ?
                DaemonMsgErrorResp : DaemonMsgStatusResp;
            uint32_t response_sequence = request_header.sequence +
                (argument->mode == DaemonFixtureWrongSequence ? 1u : 0u);
            uint32_t response_bytes = response_type == DaemonMsgErrorResp ? 264u :
                (uint32_t)sizeof(status);
            if (argument->mode == DaemonFixtureWrongStatusLength)
                response_bytes -= 1;
            encode_response_header(response_header, response_type,
                                   response_sequence, response_bytes);
            if (argument->mode == DaemonFixtureBadHeader)
                response_header[0] ^= 0xff;
            if (argument->mode == DaemonFixtureDelayStatus) {
                struct timespec delay = {.tv_sec = 0, .tv_nsec = 50000000L};
                assert(nanosleep(&delay, NULL) == 0);
            }
            write_exact(argument->fd, response_header, sizeof(response_header));
            if (argument->mode == DaemonFixturePartialStatus ||
                argument->mode == DaemonFixtureCancelStatus) {
                write_exact(argument->fd, &status, sizeof(status) / 2);
                if (argument->mode == DaemonFixtureCancelStatus) {
                    assert(cancel_write_fd >= 0);
                    assert(write(cancel_write_fd, "x", 1) == 1);
                }
                assert(close(argument->fd) == 0);
                argument->fd = -1;
                return NULL;
            }
            if (response_type == DaemonMsgErrorResp) {
                uint8_t error_payload[264] = {0};
                uint32_t error_code = argument->mode == DaemonFixtureMalformedError ?
                    0 : ENOTSUP;
                memcpy(error_payload, &error_code, sizeof(error_code));
                write_exact(argument->fd, error_payload, sizeof(error_payload));
            } else {
                write_exact(argument->fd, &status, sizeof(status));
            }
            if (argument->mode == DaemonFixtureNotRunning ||
                argument->mode == DaemonFixtureWrongCid ||
                argument->mode == DaemonFixtureWrongGuestPort ||
                argument->mode == DaemonFixtureMalformedStatus ||
                argument->mode == DaemonFixtureWrongSequence ||
                argument->mode == DaemonFixtureBadHeader ||
                argument->mode == DaemonFixtureWrongStatusLength ||
                argument->mode == DaemonFixtureStatusError ||
                argument->mode == DaemonFixtureMalformedError ||
                argument->mode == DaemonFixtureDelayStatus) {
                assert(close(argument->fd) == 0);
                argument->fd = -1;
                return NULL;
            }
            if (fixture_close_after_status) {
                assert(close(argument->fd) == 0);
                argument->fd = -1;
                return NULL;
            }
        } else {
            assert(request_header.msg_type == DaemonMsgGuestReadyReq);
            assert(request_header.payload_len == sizeof(uint32_t));
            uint8_t timeout_bytes[sizeof(uint32_t)];
            read_exact(argument->fd, timeout_bytes, sizeof(timeout_bytes));
            uint32_t timeout_ms = 0;
            assert(daemon_wire_decode_ready_request(timeout_bytes,
                sizeof(timeout_bytes), &timeout_ms) == 0);
            assert(timeout_ms > 0 && timeout_ms <= 120000);
            waddle_daemon_guest_ready_resp_t ready = {
                .status = argument->mode == DaemonFixtureGuestUnavailable ? -ETIMEDOUT :
                    argument->mode == DaemonFixtureInvalidReadyBody ? 1 : 0,
                .cid = argument->mode == DaemonFixtureWrongReadyCid ? 34 : 33
            };
            uint8_t response_header[WaddleDaemonHeaderSize];
            encode_response_header(response_header,
                                   argument->mode == DaemonFixtureMalformedReady ?
                                       DaemonMsgErrorResp : DaemonMsgGuestReadyResp,
                                   request_header.sequence, sizeof(ready));
            write_exact(argument->fd, response_header, sizeof(response_header));
            write_exact(argument->fd, &ready, sizeof(ready));
        }
    }
    assert(close(argument->fd) == 0);
    argument->fd = -1;
    return NULL;
}

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void initialize_device(device_info_t *device)
{
    memset(device, 0, sizeof(*device));
    strcpy(device->name, "host-session-test");
    assert(daemon_device_get_state_dir(device->name, device->state_dir,
                                       sizeof(device->state_dir)) == 0);
    assert(daemon_device_get_socket_path(device->name, device->socket_path,
                                         sizeof(device->socket_path)) == 0);
    strcpy(device->config_path, "/tmp/host-session-test.ini");
    strcpy(device->disk_image, "/tmp/host-session-test.qcow2");
    device->vsock_cid = 33;
    device->vsock_port = 5242;
    device->memory_mb = 4096;
    device->vcpus = 4;
    device->config_valid = 1;
}

static void initialize_config(interop_host_session_config_t *config,
                              device_info_t *device, int daemon_fd,
                              uint8_t version[8], int cancel_fd)
{
    memset(config, 0, sizeof(*config));
    config->device = device;
    config->daemon_fd = daemon_fd;
    config->agent_version = version;
    config->agent_version_bytes = 5;
    config->connection_token = 0x12345678;
    config->local_capabilities = 0;
    config->deadline_ms = monotonic_ms() + 3000;
    config->cancel_fd = cancel_fd;
    config->guest_ready_timeout_ms = 500;
    config->drain_timeout_ms = 1000;
    memcpy(version, "1.2.3", 5);
}

static interop_host_session_result_t run_fixture(daemon_fixture_mode_t mode,
        bool with_cancel, interop_linux_session_owner_t **out_owner,
        uint32_t *out_invalidated)
{
    int control[2];
    int cancel_pipe[2] = {-1, -1};
    pthread_t server_thread;
    daemon_fixture_arg_t argument;
    device_info_t device;
    interop_host_session_config_t config;
    uint8_t version[8] = {0};
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, control) == 0);
    if (with_cancel) assert(pipe2(cancel_pipe, O_CLOEXEC | O_NONBLOCK) == 0);
    initialize_device(&device);
    initialize_config(&config, &device, control[0], version,
                      with_cancel ? cancel_pipe[0] : -1);
    if (mode == DaemonFixtureDelayStatus)
        config.deadline_ms = monotonic_ms() + 10;
    argument.fd = control[1];
    argument.mode = mode;
    cancel_write_fd = with_cancel ? cancel_pipe[1] : -1;
    session_host_test_peer_uid_override = (int)geteuid();
    assert(pthread_create(&server_thread, NULL, daemon_thread, &argument) == 0);
    interop_host_session_result_t result = interop_host_session_admit(
        &config, out_owner, out_invalidated);
    assert(pthread_join(server_thread, NULL) == 0);
    close(control[0]);
    if (argument.fd >= 0) close(argument.fd);
    if (cancel_pipe[0] >= 0) close(cancel_pipe[0]);
    if (cancel_pipe[1] >= 0) close(cancel_pipe[1]);
    cancel_write_fd = -1;
    return result;
}

static void test_success_and_exactly_one_interop_attempt(void)
{
    interop_linux_session_owner_t *owner = NULL;
    uint32_t invalidated = 99;
    vsock_connect_count = 0;
    vsock_connect_errno = 0;
    assert(run_fixture(DaemonFixtureSuccess, false, &owner, &invalidated) ==
           HostSessionSuccess);
    assert(owner != NULL);
    assert(invalidated == 0);
    assert(vsock_connect_count == 1);
    assert(interop_linux_session_socket_owner(owner) != NULL);
    assert(interop_linux_session_destroy_peer(owner) == InteropLinuxSessionSuccess);
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);
    assert(owner == NULL);
    assert(vsock_peer_fd >= 0);
    assert(close(vsock_peer_fd) == 0);
    vsock_peer_fd = -1;
}

static void test_daemon_state_and_profile_mismatches_do_not_connect(void)
{
    interop_linux_session_owner_t *owner = NULL;
    uint32_t invalidated = 99;
    vsock_connect_count = 0;
    assert(run_fixture(DaemonFixtureNotRunning, false, &owner, &invalidated) ==
           HostSessionNotRunning);
    assert(owner == NULL && invalidated == 0 && vsock_connect_count == 0);
    assert(run_fixture(DaemonFixtureWrongCid, false, &owner, &invalidated) ==
           HostSessionDaemonError);
    assert(owner == NULL && invalidated == 0 && vsock_connect_count == 0);
    assert(run_fixture(DaemonFixtureWrongGuestPort, false, &owner, &invalidated) ==
           HostSessionDaemonError);
    assert(owner == NULL && invalidated == 0 && vsock_connect_count == 0);
}

static void test_guest_readiness_and_malformed_status(void)
{
    interop_linux_session_owner_t *owner = NULL;
    uint32_t invalidated = 99;
    vsock_connect_count = 0;
    assert(run_fixture(DaemonFixtureGuestUnavailable, false, &owner, &invalidated) ==
           HostSessionGuestUnavailable);
    assert(owner == NULL && invalidated == 0 && vsock_connect_count == 0);
    assert(run_fixture(DaemonFixtureMalformedStatus, false, &owner, &invalidated) ==
           HostSessionDaemonError);
    assert(owner == NULL && invalidated == 1 && vsock_connect_count == 0);
    assert(run_fixture(DaemonFixtureWrongReadyCid, false, &owner, &invalidated) ==
           HostSessionDaemonError);
    assert(owner == NULL && invalidated == 0 && vsock_connect_count == 0);
    assert(run_fixture(DaemonFixtureMalformedReady, false, &owner, &invalidated) ==
           HostSessionDaemonError);
    assert(owner == NULL && invalidated == 1 && vsock_connect_count == 0);
    assert(run_fixture(DaemonFixtureInvalidReadyBody, false, &owner, &invalidated) ==
           HostSessionDaemonError);
    assert(owner == NULL && invalidated == 1 && vsock_connect_count == 0);
}

static void test_bad_sequence_and_remote_daemon_error(void)
{
    interop_linux_session_owner_t *owner = NULL;
    uint32_t invalidated = 0;
    vsock_connect_count = 0;
    assert(run_fixture(DaemonFixtureWrongSequence, false, &owner, &invalidated) ==
           HostSessionDaemonError);
    assert(owner == NULL && invalidated == 1 && vsock_connect_count == 0);
    assert(run_fixture(DaemonFixtureBadHeader, false, &owner, &invalidated) ==
           HostSessionDaemonError);
    assert(owner == NULL && invalidated == 1 && vsock_connect_count == 0);
    assert(run_fixture(DaemonFixtureWrongStatusLength, false, &owner, &invalidated) ==
           HostSessionDaemonError);
    assert(owner == NULL && invalidated == 1 && vsock_connect_count == 0);
    errno = 0;
    assert(run_fixture(DaemonFixtureStatusError, false, &owner, &invalidated) ==
           HostSessionDaemonError);
    assert(owner == NULL && invalidated == 0 && errno == ENOTSUP && vsock_connect_count == 0);
    assert(run_fixture(DaemonFixtureMalformedError, false, &owner, &invalidated) ==
           HostSessionDaemonError);
    assert(owner == NULL && invalidated == 1 && vsock_connect_count == 0);
}

static void test_partial_daemon_response_invalidates_channel(void)
{
    interop_linux_session_owner_t *owner = NULL;
    uint32_t invalidated = 99;
    vsock_connect_count = 0;
    assert(run_fixture(DaemonFixturePartialStatus, false, &owner, &invalidated) ==
           HostSessionDaemonError);
    assert(owner == NULL && invalidated == 1 && vsock_connect_count == 0);
}

static void test_cancel_and_deadline_during_local_exchange(void)
{
    interop_linux_session_owner_t *owner = NULL;
    uint32_t invalidated = 0;
    vsock_connect_count = 0;
    assert(run_fixture(DaemonFixtureCancelStatus, true, &owner, &invalidated) ==
           HostSessionCancelled);
    assert(owner == NULL && invalidated == 1 && vsock_connect_count == 0);
    invalidated = 0;
    assert(run_fixture(DaemonFixtureDelayStatus, false, &owner, &invalidated) ==
           HostSessionTimedOut);
    assert(owner == NULL && invalidated == 1 && vsock_connect_count == 0);
}

static void test_cancel_and_deadline_after_lease_acquire(void)
{
    for (int cancel_case = 0; cancel_case < 2; ++cancel_case) {
        int control[2];
        int cancel_pipe[2];
        device_info_t device;
        interop_host_session_config_t config;
        interop_linux_session_owner_t *owner = NULL;
        uint8_t version[8] = {0};
        uint32_t invalidated = 5;
        assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, control) == 0);
        assert(pipe2(cancel_pipe, O_CLOEXEC | O_NONBLOCK) == 0);
        initialize_device(&device);
        initialize_config(&config, &device, control[0], version, cancel_pipe[0]);
        cancel_write_fd = cancel_pipe[1];
        session_host_test_peer_uid_override = (int)geteuid();
        if (cancel_case == 0) session_host_test_cancel_after_acquire = 1;
        else session_host_test_clock_after_acquire = 1;
        interop_host_session_result_t result = interop_host_session_admit(
            &config, &owner, &invalidated);
        assert(owner == NULL && invalidated == 0);
        assert(result == (cancel_case == 0 ? HostSessionCancelled : HostSessionTimedOut));
        host_clock_offset_ms = 0;
        session_host_test_peer_uid_override = -1;
        cancel_write_fd = -1;
        close(cancel_pipe[0]); close(cancel_pipe[1]);
        close(control[0]); close(control[1]);
    }
}

static void test_deadline_and_cancel_after_readiness_decode(void)
{
    interop_linux_session_owner_t *owner = NULL;
    uint32_t invalidated = 0;
    vsock_connect_count = 0;
    fixture_close_after_status = 1;
    session_host_test_clock_after_status_decode = 1;
    assert(run_fixture(DaemonFixtureSuccess, false, &owner, &invalidated) ==
           HostSessionTimedOut);
    host_clock_offset_ms = 0;
    fixture_close_after_status = 0;
    assert(owner == NULL && invalidated == 0 && vsock_connect_count == 0);

    invalidated = 0;
    session_host_test_clock_after_ready_decode = 1;
    assert(run_fixture(DaemonFixtureSuccess, false, &owner, &invalidated) ==
           HostSessionTimedOut);
    host_clock_offset_ms = 0;
    assert(owner == NULL && invalidated == 0 && vsock_connect_count == 0);

    invalidated = 0;
    session_host_test_cancel_after_ready_decode = 1;
    assert(run_fixture(DaemonFixtureSuccess, true, &owner, &invalidated) ==
           HostSessionCancelled);
    assert(owner == NULL && invalidated == 0 && vsock_connect_count == 0);

    invalidated = 0;
    session_host_test_cancel_after_connect = 1;
    assert(run_fixture(DaemonFixtureSuccess, true, &owner, &invalidated) ==
           HostSessionCancelled);
    assert(owner == NULL && invalidated == 0 && vsock_connect_count == 1);
    assert(vsock_peer_fd >= 0 && close(vsock_peer_fd) == 0);
    vsock_peer_fd = -1;
}

static void test_pre_cancel_and_expired_deadline(void)
{
    int control[2];
    int cancel_pipe[2];
    device_info_t device;
    interop_host_session_config_t config;
    interop_linux_session_owner_t *owner = NULL;
    uint8_t version[8] = {0};
    uint32_t invalidated = 7;
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, control) == 0);
    assert(pipe2(cancel_pipe, O_CLOEXEC | O_NONBLOCK) == 0);
    initialize_device(&device);
    initialize_config(&config, &device, control[0], version, cancel_pipe[0]);
    assert(write(cancel_pipe[1], "x", 1) == 1);
    assert(interop_host_session_admit(&config, &owner, &invalidated) == HostSessionCancelled);
    assert(owner == NULL && invalidated == 0);
    close(cancel_pipe[0]); close(cancel_pipe[1]);
    close(control[0]); close(control[1]);

    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, control) == 0);
    initialize_config(&config, &device, control[0], version, -1);
    config.deadline_ms = monotonic_ms();
    invalidated = 7;
    assert(interop_host_session_admit(&config, &owner, &invalidated) == HostSessionTimedOut);
    assert(owner == NULL && invalidated == 0);
    close(control[0]); close(control[1]);
}

static void test_argument_validation_and_clock_failure(void)
{
    int control[2];
    device_info_t device;
    interop_host_session_config_t config;
    interop_linux_session_owner_t *owner = NULL;
    uint8_t version[40] = {0};
    uint32_t invalidated = 17;
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, control) == 0);
    initialize_device(&device);
    initialize_config(&config, &device, control[0], version, -1);
    assert(interop_host_session_admit(NULL, &owner, &invalidated) == HostSessionInvalidArgument);
    assert(interop_host_session_admit(&config, NULL, &invalidated) == HostSessionInvalidArgument);
    assert(interop_host_session_admit(&config, &owner, NULL) == HostSessionInvalidArgument);

    config.reserved_zero = 1;
    assert(interop_host_session_admit(&config, &owner, &invalidated) == HostSessionInvalidArgument);
    config.reserved_zero = 0;
    config.daemon_fd = -1;
    assert(interop_host_session_admit(&config, &owner, &invalidated) == HostSessionInvalidArgument);
    config.daemon_fd = control[0];
    config.cancel_fd = -2;
    assert(interop_host_session_admit(&config, &owner, &invalidated) == HostSessionInvalidArgument);
    config.cancel_fd = control[0];
    assert(interop_host_session_admit(&config, &owner, &invalidated) == HostSessionInvalidArgument);
    config.cancel_fd = -1;
    config.connection_token = 0;
    assert(interop_host_session_admit(&config, &owner, &invalidated) == HostSessionInvalidArgument);
    config.connection_token = 1;
    config.local_capabilities = UINT64_MAX;
    assert(interop_host_session_admit(&config, &owner, &invalidated) == HostSessionInvalidArgument);
    config.local_capabilities = 0;
    config.guest_ready_timeout_ms = 0;
    assert(interop_host_session_admit(&config, &owner, &invalidated) == HostSessionInvalidArgument);
    config.guest_ready_timeout_ms = 500;
    config.drain_timeout_ms = 0;
    assert(interop_host_session_admit(&config, &owner, &invalidated) == HostSessionInvalidArgument);
    config.drain_timeout_ms = 1000;
    config.agent_version_bytes = 4;
    assert(interop_host_session_admit(&config, &owner, &invalidated) == HostSessionInvalidArgument);
    config.agent_version_bytes = 33;
    assert(interop_host_session_admit(&config, &owner, &invalidated) == HostSessionInvalidArgument);
    config.agent_version_bytes = 5;
    config.deadline_ms = 0;
    assert(interop_host_session_admit(&config, &owner, &invalidated) == HostSessionInvalidArgument);
    config.deadline_ms = monotonic_ms() + 3000;
    device.config_valid = 0;
    assert(interop_host_session_admit(&config, &owner, &invalidated) == HostSessionInvalidArgument);
    device.config_valid = 1;
    device.vsock_cid = 2;
    assert(interop_host_session_admit(&config, &owner, &invalidated) == HostSessionInvalidArgument);
    device.vsock_cid = UINT32_MAX;
    assert(interop_host_session_admit(&config, &owner, &invalidated) == HostSessionInvalidArgument);
    device.vsock_cid = 33;
    device.vsock_port = 0;
    assert(interop_host_session_admit(&config, &owner, &invalidated) == HostSessionInvalidArgument);
    device.vsock_port = InteropControlPort;
    assert(interop_host_session_admit(&config, &owner, &invalidated) == HostSessionInvalidArgument);
    device.vsock_port = 5242;
    strcpy(device.name, "bad/name");
    assert(interop_host_session_admit(&config, &owner, &invalidated) == HostSessionInvalidArgument);
    strcpy(device.name, "host-session-test");
    config.agent_version = version;
    config.agent_version_bytes = SIZE_MAX;
    assert(interop_host_session_admit(&config, &owner, &invalidated) == HostSessionInvalidArgument);
    assert(owner == NULL);
    close(control[0]);
    close(control[1]);

    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, control) == 0);
    initialize_config(&config, &device, control[0], version, -1);
    fail_clock_once = 1;
    invalidated = 0;
    assert(interop_host_session_admit(&config, &owner, &invalidated) == HostSessionClockError);
    assert(owner == NULL && invalidated == 0);
    close(control[0]);
    close(control[1]);
}

static void test_real_owner_preexisting_output_rejected(void)
{
    int control[2];
    device_info_t device;
    interop_host_session_config_t config;
    interop_linux_session_owner_t *owner = NULL;
    uint8_t version[8] = {0};
    uint32_t invalidated = 19;
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, control) == 0);
    initialize_device(&device);
    initialize_config(&config, &device, control[0], version, -1);
    interop_linux_session_config_t lease_config = {
        .device = &device,
        .agent_version = version,
        .agent_version_bytes = 5,
        .connection_token = config.connection_token,
        .local_capabilities = config.local_capabilities,
        .drain_timeout_ms = config.drain_timeout_ms,
        .reserved_zero = 0
    };
    assert(interop_linux_session_acquire(&lease_config, &owner) ==
           InteropLinuxSessionSuccess);
    assert(owner != NULL);
    assert(interop_host_session_admit(&config, &owner, &invalidated) ==
           HostSessionInvalidArgument);
    assert(owner != NULL && invalidated == 19);
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);
    assert(owner == NULL);
    close(control[0]);
    close(control[1]);
}

static void test_held_session_lease_rejects_second_admission(void)
{
    int control[2];
    device_info_t device;
    interop_host_session_config_t config;
    interop_linux_session_owner_t *held_owner = NULL;
    interop_linux_session_owner_t *out_owner = NULL;
    uint8_t version[8] = {0};
    uint32_t invalidated = 9;
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, control) == 0);
    initialize_device(&device);
    initialize_config(&config, &device, control[0], version, -1);
    interop_linux_session_config_t lease_config = {
        .device = &device,
        .agent_version = version,
        .agent_version_bytes = 5,
        .connection_token = config.connection_token,
        .local_capabilities = config.local_capabilities,
        .drain_timeout_ms = config.drain_timeout_ms,
        .reserved_zero = 0
    };
    assert(interop_linux_session_acquire(&lease_config, &held_owner) ==
           InteropLinuxSessionSuccess);
    session_host_test_peer_uid_override = (int)geteuid();
    assert(interop_host_session_admit(&config, &out_owner, &invalidated) ==
           HostSessionBusy);
    session_host_test_peer_uid_override = -1;
    assert(held_owner != NULL && out_owner == NULL && invalidated == 0);
    assert(interop_linux_session_release(&held_owner) == InteropLinuxSessionSuccess);
    assert(held_owner == NULL);
    close(control[0]);
    close(control[1]);
}

static void test_peer_credentials_rejected_before_control_io(void)
{
    int control[2];
    device_info_t device;
    interop_host_session_config_t config;
    interop_linux_session_owner_t *owner = NULL;
    uint8_t version[8] = {0};
    uint32_t invalidated = 9;
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, control) == 0);
    initialize_device(&device);
    initialize_config(&config, &device, control[0], version, -1);
    session_host_test_peer_uid_override = (int)geteuid() + 1;
    vsock_connect_count = 0;
    assert(interop_host_session_admit(&config, &owner, &invalidated) ==
           HostSessionDaemonError);
    struct pollfd descriptor = {.fd = control[1], .events = POLLIN};
    assert(poll(&descriptor, 1, 0) == 0);
    assert(owner == NULL && invalidated == 0 && vsock_connect_count == 0);
    session_host_test_peer_uid_override = -1;
    close(control[0]); close(control[1]);
}

static void test_vsock_refusal_is_never_retried(void)
{
    interop_linux_session_owner_t *owner = NULL;
    uint32_t invalidated = 0;
    vsock_connect_count = 0;
    vsock_connect_errno = ECONNREFUSED;
    assert(run_fixture(DaemonFixtureSuccess, false, &owner, &invalidated) ==
           HostSessionConnectError);
    assert(owner == NULL && invalidated == 0 && vsock_connect_count == 1);
    assert(vsock_peer_fd >= 0 && close(vsock_peer_fd) == 0);
    vsock_peer_fd = -1;
    vsock_connect_errno = 0;
}

static void test_retryable_transfer_and_poll_faults(void)
{
    interop_linux_session_owner_t *owner = NULL;
    uint32_t invalidated = 0;
    vsock_connect_count = 0;
    send_eintr_once = 1;
    send_partial_limit = 5;
    recv_eintr_once = 1;
    assert(run_fixture(DaemonFixtureSuccess, false, &owner, &invalidated) ==
           HostSessionSuccess);
    assert(owner != NULL && invalidated == 0 && vsock_connect_count == 1);
    assert(interop_linux_session_destroy_peer(owner) == InteropLinuxSessionSuccess);
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);
    assert(vsock_peer_fd >= 0 && close(vsock_peer_fd) == 0);
    vsock_peer_fd = -1;

    owner = NULL;
    invalidated = 0;
    recv_eagain_once = 1;
    session_host_test_fail_poll_errno_once = EINTR;
    assert(run_fixture(DaemonFixtureSuccess, false, &owner, &invalidated) ==
           HostSessionSuccess);
    assert(owner != NULL && invalidated == 0);
    assert(interop_linux_session_destroy_peer(owner) == InteropLinuxSessionSuccess);
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);
    assert(vsock_peer_fd >= 0 && close(vsock_peer_fd) == 0);
    vsock_peer_fd = -1;

    owner = NULL;
    invalidated = 0;
    recv_eagain_once = 1;
    session_host_test_fail_poll_errno_once = EIO;
    fixture_close_after_status = 1;
    assert(run_fixture(DaemonFixtureSuccess, false, &owner, &invalidated) ==
           HostSessionDaemonError);
    fixture_close_after_status = 0;
    assert(owner == NULL && invalidated == 1 && vsock_connect_count == 2);

    owner = NULL;
    invalidated = 0;
    recv_eagain_once = 1;
    session_host_test_force_poll_nval_once = 1;
    fixture_close_after_status = 1;
    assert(run_fixture(DaemonFixtureSuccess, false, &owner, &invalidated) ==
           HostSessionDaemonError);
    fixture_close_after_status = 0;
    assert(owner == NULL && invalidated == 1 && vsock_connect_count == 2);
}

static void test_post_connect_deadline_preserves_unreleased_owner(void)
{
    interop_linux_session_owner_t *owner = NULL;
    uint32_t invalidated = 0;
    vsock_connect_count = 0;
    advance_clock_after_peer_success = 1;
    fail_peer_destroy_once = 1;
    assert(run_fixture(DaemonFixtureSuccess, false, &owner, &invalidated) ==
           HostSessionCleanupRequired);
    advance_clock_after_peer_success = 0;
    host_clock_offset_ms = 0;
    assert(owner != NULL && invalidated == 0 && vsock_connect_count == 1);
    assert(interop_linux_session_destroy_peer(owner) == InteropLinuxSessionSuccess);
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);
    assert(owner == NULL);
    assert(vsock_peer_fd >= 0 && close(vsock_peer_fd) == 0);
    vsock_peer_fd = -1;
}

static void test_cleanup_release_failure_preserves_real_owner(void)
{
    interop_linux_session_owner_t *owner = NULL;
    uint32_t invalidated = 0;
    session_host_test_release_failure_once = 1;
    assert(run_fixture(DaemonFixtureNotRunning, false, &owner, &invalidated) ==
           HostSessionCleanupRequired);
    assert(owner != NULL && invalidated == 0);
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);
    assert(owner == NULL);
}

static void test_cleanup_close_diagnostic_releases_real_owner(void)
{
    interop_linux_session_owner_t *owner = NULL;
    uint32_t invalidated = 0;
    vsock_connect_count = 0;
    advance_clock_after_peer_success = 1;
    session_host_test_destroy_diagnostic_once = 1;
    assert(run_fixture(DaemonFixtureSuccess, false, &owner, &invalidated) ==
           HostSessionTimedOut);
    advance_clock_after_peer_success = 0;
    host_clock_offset_ms = 0;
    assert(owner == NULL && invalidated == 0 && vsock_connect_count == 1);
    assert(vsock_peer_fd >= 0 && close(vsock_peer_fd) == 0);
    vsock_peer_fd = -1;
}


int main(void)
{
    snprintf(temporary_root, sizeof(temporary_root), "/tmp/waddle-session-host-XXXXXX");
    assert(mkdtemp(temporary_root) != NULL);
    char state_root[WaddleMaxPathLen];
    char runtime_root[WaddleMaxPathLen];
    assert(snprintf(state_root, sizeof(state_root), "%s/state", temporary_root) > 0);
    assert(snprintf(runtime_root, sizeof(runtime_root), "%s/runtime", temporary_root) > 0);
    assert(mkdir(state_root, 0700) == 0);
    assert(mkdir(runtime_root, 0700) == 0);
    assert(setenv("XDG_STATE_HOME", state_root, 1) == 0);
    assert(setenv("XDG_RUNTIME_DIR", runtime_root, 1) == 0);

    session_host_private_tests();
    test_success_and_exactly_one_interop_attempt();
    test_daemon_state_and_profile_mismatches_do_not_connect();
    test_guest_readiness_and_malformed_status();
    test_bad_sequence_and_remote_daemon_error();
    test_partial_daemon_response_invalidates_channel();
    test_cancel_and_deadline_during_local_exchange();
    test_cancel_and_deadline_after_lease_acquire();
    test_deadline_and_cancel_after_readiness_decode();
    test_pre_cancel_and_expired_deadline();
    test_argument_validation_and_clock_failure();
    test_real_owner_preexisting_output_rejected();
    test_held_session_lease_rejects_second_admission();
    test_peer_credentials_rejected_before_control_io();
    test_vsock_refusal_is_never_retried();
    test_retryable_transfer_and_poll_faults();
    test_post_connect_deadline_preserves_unreleased_owner();
    test_cleanup_release_failure_preserves_real_owner();
    test_cleanup_close_diagnostic_releases_real_owner();

    assert(unsetenv("XDG_STATE_HOME") == 0);
    assert(unsetenv("XDG_RUNTIME_DIR") == 0);
    assert(nftw(temporary_root, remove_path, 16, FTW_DEPTH | FTW_PHYS) == 0);
    printf("session_host tests passed\n");
    return 0;
}
