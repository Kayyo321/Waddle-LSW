#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../../src/interop/session_host.c"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

extern int session_host_test_clock_mode;
extern int session_host_test_send_eagain_once;
extern int session_host_test_recv_eof_once;
extern int session_host_test_force_poll_nval_once;
extern int session_host_test_socket_type_override;
extern int session_host_test_socket_type_fail_once;
extern int session_host_test_bad_socket_type_length;
extern int session_host_test_peer_family_override;
extern int session_host_test_getpeername_fail_once;
extern int session_host_test_peercred_fail_once;
extern int session_host_test_bad_peercred_length;
extern int session_host_test_peer_pid_override;
extern int session_host_test_peer_uid_override;
extern int session_host_test_force_cancel_poll_nval_once;
extern int session_host_test_destroy_diagnostic_once;
extern int session_host_test_destroy_diagnostic_errno;
extern int vsock_connect_count;
extern int vsock_peer_fd;
extern int session_host_test_force_poll_unrelated_once;
extern int session_host_test_fail_poll_errno_once;

static void initialize_private_config(interop_host_session_config_t *config,
                                      device_info_t *device,
                                      uint8_t *version)
{
    memset(device, 0, sizeof(*device));
    strcpy(device->name, "host-session-test");
    assert(daemon_device_get_state_dir(device->name, device->state_dir,
                                       sizeof(device->state_dir)) == 0);
    assert(daemon_device_get_socket_path(device->name, device->socket_path,
                                         sizeof(device->socket_path)) == 0);
    device->config_valid = 1;
    device->vsock_cid = 33;
    device->vsock_port = 5242;
    memset(config, 0, sizeof(*config));
    config->device = device;
    config->daemon_fd = 1;
    config->agent_version = version;
    config->agent_version_bytes = 5;
    config->connection_token = 1;
    config->deadline_ms = 1;
    config->cancel_fd = -1;
    config->guest_ready_timeout_ms = 1;
    config->drain_timeout_ms = 1;
    memcpy(version, "1.2.3", 5);
}

static void assert_invalid_config(interop_host_session_config_t *config)
{
    interop_linux_session_owner_t *owner = NULL;
    uint32_t invalidated = 0;
    assert(!config_valid(config, &owner, &invalidated));
    assert(owner == NULL);
}

void session_host_private_tests(void)
{
    host_session_range_t range;
    int value = 0;
    assert(make_range(&value, sizeof(value), alignof(int), &range));
    assert(range.begin == (uintptr_t)&value);
    assert(range.end == (uintptr_t)&value + sizeof(value));
    assert(!make_range(NULL, sizeof(value), alignof(int), &range));
    assert(!make_range(&value, 0, alignof(int), &range));
    assert(!make_range(&value, sizeof(value), 0, &range));
    assert(!make_range(&value, sizeof(value), alignof(int), NULL));
    assert(!make_range((const void *)((uintptr_t)&value + 1u), sizeof(value),
                       alignof(int), &range));
    assert(!make_range((const void *)(UINTPTR_MAX - 3u), 8, 4, &range));
    assert(monotonic_ms(NULL) == -1 && errno == EINVAL);

    interop_host_session_config_t config;
    device_info_t device;
    _Alignas(interop_linux_session_owner_t *) uint8_t version[40];
    initialize_private_config(&config, &device, version);
    interop_linux_session_owner_t *owner_slot = NULL;
    uint32_t invalidated_slot = 0;
    assert(config_valid(&config, &owner_slot, &invalidated_slot));
    assert(!config_valid((interop_host_session_config_t *)((uint8_t *)&config + 1),
                         &owner_slot, &invalidated_slot));
    assert(!config_valid(&config,
                         (interop_linux_session_owner_t **)((uint8_t *)&owner_slot + 1),
                         &invalidated_slot));
    assert(!config_valid(&config, &owner_slot,
                         (uint32_t *)((uint8_t *)&invalidated_slot + 1)));
    config.device = NULL; assert_invalid_config(&config); initialize_private_config(&config, &device, version);
    config.agent_version = NULL; assert_invalid_config(&config); initialize_private_config(&config, &device, version);

    assert(!config_valid(&config, (interop_linux_session_owner_t **)&config,
                         &invalidated_slot));
    assert(!config_valid(&config, &owner_slot, (uint32_t *)&config.reserved_zero));
    union owner_invalidated_union_t {
        interop_linux_session_owner_t *owner;
        uint32_t invalidated;
    } owner_invalidated;
    owner_invalidated.owner = NULL;
    assert(!config_valid(&config, &owner_invalidated.owner,
                         &owner_invalidated.invalidated));
    config.device = (const device_info_t *)&config;
    assert_invalid_config(&config);
    initialize_private_config(&config, &device, version);
    config.agent_version = (const uint8_t *)&config;
    assert_invalid_config(&config);
    initialize_private_config(&config, &device, version);
    config.agent_version = (const uint8_t *)&device;
    assert_invalid_config(&config);
    initialize_private_config(&config, &device, version);
    assert(!config_valid(&config, (interop_linux_session_owner_t **)&device,
                         &invalidated_slot));
    assert(!config_valid(&config, (interop_linux_session_owner_t **)version,
                         &invalidated_slot));
    assert(!config_valid(&config, &owner_slot, (uint32_t *)&device.vsock_port));
    assert(!config_valid(&config, &owner_slot, (uint32_t *)version));

#define RejectField(statement) do { statement; assert_invalid_config(&config); initialize_private_config(&config, &device, version); } while (0)
    RejectField(config.reserved_zero = 1);
    RejectField(config.daemon_fd = -1);
    RejectField(config.cancel_fd = -2);
    RejectField(config.cancel_fd = config.daemon_fd);
    RejectField(config.connection_token = 0);
    RejectField(config.local_capabilities = UINT64_MAX);
    RejectField(config.guest_ready_timeout_ms = 0);
    RejectField(config.guest_ready_timeout_ms = HostSessionMaxReadyTimeoutMs + 1);
    RejectField(config.drain_timeout_ms = 0);
    RejectField(config.drain_timeout_ms = InteropLinuxSocketMaxDrainTimeoutMs + 1);
    RejectField(config.agent_version_bytes = 4);
    RejectField(config.agent_version_bytes = 33);
    RejectField(config.deadline_ms = 0);
    RejectField(device.config_valid = 0);
    RejectField(device.vsock_cid = 2);
    RejectField(device.vsock_cid = UINT32_MAX);
    RejectField(device.vsock_port = 0);
    RejectField(device.vsock_port = InteropControlPort);
    memset(device.name, 'x', sizeof(device.name)); assert_invalid_config(&config); initialize_private_config(&config, &device, version);
    memset(device.state_dir, 'x', sizeof(device.state_dir)); assert_invalid_config(&config); initialize_private_config(&config, &device, version);
    memset(device.socket_path, 'x', sizeof(device.socket_path)); assert_invalid_config(&config); initialize_private_config(&config, &device, version);
    strcpy(device.name, "bad/name"); assert_invalid_config(&config); initialize_private_config(&config, &device, version);
#undef RejectField

    host_session_range_t left = {.begin = 10, .end = 20};
    host_session_range_t overlapping = {.begin = 19, .end = 21};
    host_session_range_t touching = {.begin = 20, .end = 25};
    assert(ranges_overlap(left, overlapping));
    assert(!ranges_overlap(left, touching));
    assert(!ranges_overlap(touching, left));

    const char terminated[] = "ready";
    const char unterminated[3] = {'b', 'a', 'd'};
    assert(valid_string(terminated, sizeof(terminated)));
    assert(!valid_string(terminated, 0));
    assert(!valid_string(NULL, sizeof(terminated)));
    assert(!valid_string(unterminated, sizeof(unterminated)));
    assert(valid_capabilities(0));
    assert(valid_capabilities(InteropCapabilityMask));
    assert(!valid_capabilities(UINT64_MAX));

    host_session_sequence = UINT32_MAX;
    assert(next_sequence() == UINT32_MAX);
    assert(next_sequence() == 1);

    uint64_t now = 0;
    assert(monotonic_ms(&now) == 0);
    assert(now != 0);
    errno = 0;
    assert(check_deadline(now + 10000u, &now) == 0);
    uint64_t expired_deadline = now;
    assert(check_deadline(expired_deadline, &now) == 1);
    assert(errno == ETIMEDOUT);
    assert(check_cancel(-1) == 0);
    int cancel_pipe[2];
    assert(pipe(cancel_pipe) == 0);
    assert(write(cancel_pipe[1], "x", 1) == 1);
    assert(check_cancel(cancel_pipe[0]) == 1);
    assert(errno == ECANCELED);
    assert(close(cancel_pipe[0]) == 0);
    assert(check_cancel(cancel_pipe[0]) == -1);
    assert(errno == EBADF);
    assert(close(cancel_pipe[1]) == 0);
    assert(pipe(cancel_pipe) == 0);
    session_host_test_fail_poll_errno_once = EIO;
    assert(check_cancel(cancel_pipe[0]) == -1 && errno == EIO);
    assert(monotonic_ms(&now) == 0);
    session_host_test_force_poll_unrelated_once = 1;
    assert(wait_io(cancel_pipe[0], POLLIN, -1, now + 2u) == -1);
    assert(errno == ETIMEDOUT);
    assert(close(cancel_pipe[0]) == 0);
    assert(close(cancel_pipe[1]) == 0);

    session_host_test_clock_mode = 1;
    assert(monotonic_ms(&now) == -1 && errno == EIO);
    session_host_test_clock_mode = 2;
    assert(monotonic_ms(&now) == -1 && errno == EOVERFLOW);
    session_host_test_clock_mode = 3;
    assert(monotonic_ms(&now) == -1 && errno == EOVERFLOW);
    session_host_test_clock_mode = 4;
    assert(monotonic_ms(&now) == -1 && errno == EOVERFLOW);
    session_host_test_clock_mode = 0;
    errno = 0;
    assert(monotonic_ms(&now) == 0);
    assert(wait_io(cancel_pipe[0], POLLIN, -1, now + 10000u) == -1);
    assert(errno == EBADF);

    int io_pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, io_pair) == 0);
    uint8_t message[] = {1, 2, 3, 4};
    size_t moved = SIZE_MAX;
    assert(monotonic_ms(&now) == 0);
    session_host_test_send_eagain_once = 1;
    assert(transfer_bytes(io_pair[0], message, sizeof(message), true, -1,
                          now + 1000u, &moved) == 0);
    assert(moved == sizeof(message));
    uint8_t received[sizeof(message)];
    assert(read(io_pair[1], received, sizeof(received)) == (ssize_t)sizeof(received));
    assert(memcmp(message, received, sizeof(message)) == 0);
    assert(write(io_pair[1], message, sizeof(message)) == (ssize_t)sizeof(message));
    assert(monotonic_ms(&now) == 0);
    assert(transfer_bytes(io_pair[0], received, sizeof(received), false, -1,
                          now + 1000u, NULL) == 0);
    assert(memcmp(message, received, sizeof(message)) == 0);
    assert(close(io_pair[1]) == 0);
    assert(monotonic_ms(&now) == 0);
    assert(transfer_bytes(io_pair[0], received, sizeof(received), false, -1,
                          now + 1000u, &moved) == -1);
    assert(errno == ECONNRESET && moved == 0);
    assert(close(io_pair[0]) == 0);
    assert(monotonic_ms(&now) == 0);
    assert(transfer_bytes(-1, message, sizeof(message), true, -1, now,
                          &moved) == -1);
    assert(errno == ETIMEDOUT && moved == 0);

    assert(pipe(cancel_pipe) == 0);
    assert(write(cancel_pipe[1], "x", 1) == 1);
    assert(monotonic_ms(&now) == 0);
    moved = SIZE_MAX;
    assert(transfer_bytes(-1, message, sizeof(message), true, cancel_pipe[0],
                          now + 1000u, &moved) == -1);
    assert(errno == ECANCELED && moved == 0);
    assert(close(cancel_pipe[0]) == 0);
    assert(close(cancel_pipe[1]) == 0);

    assert(pipe(cancel_pipe) == 0);
    session_host_test_force_poll_nval_once = 1;
    assert(monotonic_ms(&now) == 0);
    assert(wait_io(cancel_pipe[0], POLLIN, cancel_pipe[1], now + 1000u) == -1);
    assert(errno == EBADF);
    assert(close(cancel_pipe[0]) == 0);
    assert(close(cancel_pipe[1]) == 0);
    assert(pipe(cancel_pipe) == 0);
    session_host_test_force_poll_nval_once = 1;
    assert(wait_io(cancel_pipe[0], POLLIN, -1, UINT64_MAX) == -1);
    assert(errno == EBADF);
    assert(close(cancel_pipe[0]) == 0);
    assert(close(cancel_pipe[1]) == 0);
    assert(pipe(cancel_pipe) == 0);
    session_host_test_force_cancel_poll_nval_once = 1;
    assert(monotonic_ms(&now) == 0);
    assert(wait_io(cancel_pipe[0], POLLIN, cancel_pipe[1], now + 1000u) == -1);
    assert(errno == EBADF);
    assert(close(cancel_pipe[0]) == 0);
    assert(close(cancel_pipe[1]) == 0);

    host_session_response_t response;
    uint32_t invalidated = 0;
    uint8_t too_long[HostSessionGuestReadyRequestBytes + 1] = {0};
    assert(exchange_request(-1, -1, UINT64_MAX, DaemonMsgGuestReadyReq,
                            DaemonMsgGuestReadyResp, too_long, sizeof(too_long),
                            0, 1, &response, &invalidated) == -1);
    assert(errno == EINVAL);
    assert(exchange_request(-1, -1, UINT64_MAX, DaemonMsgGuestReadyReq,
                            DaemonMsgGuestReadyResp, NULL, 0,
                            sizeof(response.payload) + 1u, 1, &response,
                            &invalidated) == -1);
    assert(errno == EINVAL);
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, io_pair) == 0);
    session_host_test_peer_uid_override = (int)geteuid();
    assert(validate_control_fd(io_pair[0]) == 0);
    session_host_test_socket_type_fail_once = 1;
    assert(validate_control_fd(io_pair[0]) == -1 && errno == EIO);
    session_host_test_socket_type_override = SOCK_DGRAM;
    assert(validate_control_fd(io_pair[0]) == -1 && errno == EPROTOTYPE);
    session_host_test_socket_type_override = SOCK_STREAM;
    session_host_test_bad_socket_type_length = 1;
    assert(validate_control_fd(io_pair[0]) == -1 && errno == EPROTOTYPE);
    session_host_test_socket_type_override = 0;
    session_host_test_getpeername_fail_once = 1;
    assert(validate_control_fd(io_pair[0]) == -1 && errno == EIO);
    session_host_test_peer_family_override = AF_INET;
    assert(validate_control_fd(io_pair[0]) == -1 && errno == EAFNOSUPPORT);
    session_host_test_peer_family_override = 0;
    session_host_test_peercred_fail_once = 1;
    assert(validate_control_fd(io_pair[0]) == -1 && errno == EIO);
    session_host_test_bad_peercred_length = 1;
    assert(validate_control_fd(io_pair[0]) == -1 && errno == EACCES);
    session_host_test_peer_pid_override = 0;
    assert(validate_control_fd(io_pair[0]) == -1 && errno == EACCES);
    session_host_test_peer_pid_override = -1;
    assert(close(io_pair[0]) == 0);
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, io_pair) == 0);
    assert(close(io_pair[1]) == 0);
    assert(monotonic_ms(&now) == 0);
    moved = SIZE_MAX;
    assert(transfer_bytes(io_pair[0], message, sizeof(message), true, -1,
                          now + 1000u, &moved) == -1);
    assert(errno == EPIPE && moved == 0);
    assert(close(io_pair[0]) == 0);
    session_host_test_peer_uid_override = -1;

    uint8_t encoded[HostSessionFrameHeaderBytes];
    encode_header(encoded, DaemonMsgStatusReq, UINT32_C(0x12345678), 0);
    waddle_daemon_header_t decoded;
    assert(daemon_wire_decode_header(encoded, sizeof(encoded), &decoded) == 0);
    assert(decoded.msg_type == DaemonMsgStatusReq);
    assert(decoded.sequence == UINT32_C(0x12345678));
    assert(decoded.payload_len == 0);

    interop_linux_session_owner_t *no_owner = NULL;
    interop_linux_session_owner_t *no_output = NULL;
    assert(cleanup_failure(&no_owner, &no_output, HostSessionTimedOut, 0) ==
           HostSessionTimedOut);
    assert(no_owner == NULL && no_output == NULL);

#define CHECK_RESULT(input, expected) assert(map_lease_result(input) == expected)
    CHECK_RESULT(InteropLinuxSessionSuccess, HostSessionSuccess);
    CHECK_RESULT(InteropLinuxSessionInvalidArgument, HostSessionInvalidArgument);
    CHECK_RESULT(InteropLinuxSessionOutOfMemory, HostSessionOutOfMemory);
    CHECK_RESULT(InteropLinuxSessionBusy, HostSessionBusy);
    CHECK_RESULT(InteropLinuxSessionAvBusy, HostSessionAvBusy);
    CHECK_RESULT(InteropLinuxSessionCancelled, HostSessionCancelled);
    CHECK_RESULT(InteropLinuxSessionTimedOut, HostSessionTimedOut);
    CHECK_RESULT(InteropLinuxSessionConnectError, HostSessionConnectError);
    CHECK_RESULT(InteropLinuxSessionAttemptConsumed, HostSessionAttemptConsumed);
    CHECK_RESULT(InteropLinuxSessionPeerCreateError, HostSessionPeerCreateError);
    CHECK_RESULT(InteropLinuxSessionCloseDiagnostic, HostSessionCloseDiagnostic);
    CHECK_RESULT(InteropLinuxSessionClockError, HostSessionClockError);
    CHECK_RESULT(InteropLinuxSessionOsError, HostSessionOsError);
    CHECK_RESULT(InteropLinuxSessionGuestPortConflict, HostSessionOsError);
    CHECK_RESULT(InteropLinuxSessionWrongThread, HostSessionOsError);
    CHECK_RESULT(InteropLinuxSessionBadState, HostSessionOsError);
#undef CHECK_RESULT

    interop_linux_session_config_t owner_config = {
        .device = &device,
        .agent_version = version,
        .agent_version_bytes = 5,
        .connection_token = 3,
        .local_capabilities = 0,
        .drain_timeout_ms = 1000,
        .reserved_zero = 0
    };
    interop_linux_session_owner_t *owner = NULL;
    interop_linux_session_owner_t *out_owner = NULL;
    assert(interop_linux_session_acquire(&owner_config, &owner) ==
           InteropLinuxSessionSuccess);
    assert(interop_linux_session_connect_peer(owner, 1000, -1) ==
           InteropLinuxSessionSuccess);
    assert(vsock_connect_count == 1 && vsock_peer_fd >= 0);
    session_host_test_destroy_diagnostic_once = 1;
    session_host_test_destroy_diagnostic_errno = 0;
    assert(cleanup_failure(&owner, &out_owner, HostSessionNotRunning, 0) ==
           HostSessionNotRunning);
    session_host_test_destroy_diagnostic_errno = EIO;
    assert(owner == NULL && out_owner == NULL && errno == EIO);
    assert(close(vsock_peer_fd) == 0);
    vsock_peer_fd = -1;
}
