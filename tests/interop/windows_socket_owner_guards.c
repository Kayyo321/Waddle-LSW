/** @file windows_socket_owner_guards.c
 * @brief Native-input preflight and connected stream ownership checks.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "windows_socket_owner.h"
#include "windows_socket_owner_api_double.h"
#include "waddle/interop_protocol.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void guard_expect(int condition)
{
    if (!condition)
        abort();
}

static interop_windows_socket_config_t guard_config(uint64_t token)
{
    static const uint8_t Version[] = "1.2.3";
    interop_windows_socket_config_t config;
    memset(&config, 0, sizeof(config));
    config.peer_config.role = InteropPeerHost;
    config.peer_config.connection_token = token;
    config.peer_config.accepted_at_ms = 100;
    config.peer_config.agent_version = Version;
    config.peer_config.agent_version_bytes = sizeof(Version) - 1;
    config.drain_timeout_ms = 500;
    return config;
}

static void test_guard_invalid_inputs(void)
{
    interop_windows_socket_config_t config = guard_config(0xA11CE);
    interop_windows_socket_t *owner = NULL;
    int pair[2] = {-1, -1};
    guard_expect(interop_windows_socket_create(INVALID_SOCKET, &config, &owner) ==
                 InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_create(0, NULL, &owner) ==
                 InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_create(0, &config, NULL) ==
                 InteropWindowsSocketInvalidArgument);
    guard_expect(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    guard_expect(interop_windows_socket_create(pair[0], &config,
                 (interop_windows_socket_t **)(void *)((uint8_t *)&owner + 1)) ==
                 InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_create(pair[0], &config,
                 (interop_windows_socket_t **)(void *)&config) ==
                 InteropWindowsSocketInvalidArgument);
    owner = (interop_windows_socket_t *)(uintptr_t)1;
    guard_expect(interop_windows_socket_create(pair[0], &config, &owner) ==
                 InteropWindowsSocketInvalidArgument);
    owner = NULL;
    config.reserved_zero = 1;
    guard_expect(interop_windows_socket_create(pair[0], &config, &owner) ==
                 InteropWindowsSocketInvalidArgument);
    config.reserved_zero = 0;
    config.peer_config.connection_token = 0;
    guard_expect(interop_windows_socket_create(pair[0], &config, &owner) !=
                 InteropWindowsSocketSuccess);
    guard_expect(owner == NULL && fcntl(pair[0], F_GETFD) >= 0);
    close(pair[0]);
    close(pair[1]);
}

static void test_guard_connected_and_linger_checks(void)
{
    interop_windows_socket_config_t config = guard_config(0xA11CF);
    interop_windows_socket_t *owner = NULL;
    int pair[2] = {-1, -1};
    guard_expect(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    windows_socket_owner_test_set_linger_on(1);
    guard_expect(interop_windows_socket_create(pair[0], &config, &owner) ==
                 InteropWindowsSocketInvalidArgument);
    guard_expect(owner == NULL && fcntl(pair[0], F_GETFD) >= 0);
    windows_socket_owner_test_set_linger_length((int)sizeof(struct linger) - 1);
    guard_expect(interop_windows_socket_create(pair[0], &config, &owner) ==
                 InteropWindowsSocketInvalidArgument);
    close(pair[0]);
    close(pair[1]);

    guard_expect(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    windows_socket_owner_test_fail_get_peer(WSAENOTSOCK);
    guard_expect(interop_windows_socket_create(pair[0], &config, &owner) ==
                 InteropWindowsSocketInvalidArgument);
    guard_expect(owner == NULL && fcntl(pair[0], F_GETFD) >= 0);
    windows_socket_owner_test_fail_linger(WSAENETDOWN);
    guard_expect(interop_windows_socket_create(pair[0], &config, &owner) ==
                 InteropWindowsSocketInvalidArgument);
    guard_expect(owner == NULL && fcntl(pair[0], F_GETFD) >= 0);
    close(pair[0]);
    close(pair[1]);
}

static void test_guard_socket_metadata_and_fionbio_failures(void)
{
    interop_windows_socket_config_t config = guard_config(0xA11D2);
    interop_windows_socket_t *owner = NULL;
    int pair[2] = {-1, -1};
    guard_expect(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);

    windows_socket_owner_test_set_socket_type(SOCK_DGRAM, (int)sizeof(int));
    guard_expect(interop_windows_socket_create(pair[0], &config, &owner) ==
                 InteropWindowsSocketInvalidArgument);
    windows_socket_owner_test_set_socket_type(SOCK_STREAM, 1);
    guard_expect(interop_windows_socket_create(pair[0], &config, &owner) ==
                 InteropWindowsSocketInvalidArgument);
    windows_socket_owner_test_set_peer_length(1);
    guard_expect(interop_windows_socket_create(pair[0], &config, &owner) ==
                 InteropWindowsSocketInvalidArgument);
    windows_socket_owner_test_set_peer_length(4096);
    guard_expect(interop_windows_socket_create(pair[0], &config, &owner) ==
                 InteropWindowsSocketInvalidArgument);
    windows_socket_owner_test_set_inheritable(1);
    guard_expect(interop_windows_socket_create(pair[0], &config, &owner) ==
                 InteropWindowsSocketInvalidArgument);
    windows_socket_owner_test_fail_get_handle(WSAENOTSOCK);
    guard_expect(interop_windows_socket_create(pair[0], &config, &owner) ==
                 InteropWindowsSocketInvalidArgument);
    windows_socket_owner_test_fail_ioctl(WSAEINVAL);
    guard_expect(interop_windows_socket_create(pair[0], &config, &owner) ==
                 InteropWindowsSocketOsError);
    windows_socket_owner_test_fail_ioctl(WSAENOBUFS);
    guard_expect(interop_windows_socket_create(pair[0], &config, &owner) ==
                 InteropWindowsSocketOutOfMemory);
    config.peer_config.agent_version = NULL;
    config.peer_config.agent_version_bytes = 0;
    guard_expect(interop_windows_socket_create(pair[0], &config, &owner) ==
                 InteropWindowsSocketInvalidArgument);
    guard_expect(owner == NULL && fcntl(pair[0], F_GETFD) >= 0);
    guard_expect(close(pair[0]) == 0 && close(pair[1]) == 0);
}

static void test_guard_success_sets_nonblocking_and_owns_once(void)
{
    interop_windows_socket_config_t config = guard_config(0xA11D0);
    interop_windows_socket_t *owner = NULL;
    interop_windows_socket_work_t work;
    int pair[2] = {-1, -1};
    interop_windows_socket_t *empty = NULL;

    guard_expect(interop_windows_socket_destroy(NULL) == InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_destroy(&empty) == InteropWindowsSocketSuccess);
    guard_expect(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    guard_expect(interop_windows_socket_create(pair[0], &config, &owner) ==
                 InteropWindowsSocketSuccess);
    guard_expect(owner != NULL && (fcntl(pair[0], F_GETFL) & O_NONBLOCK) != 0);
    guard_expect(interop_windows_socket_abort(owner, config.peer_config.connection_token,
                                              &work) == InteropWindowsSocketSuccess);
    guard_expect(work.state == InteropWindowsSocketTransportClosed);
    guard_expect(interop_windows_socket_destroy(&owner) == InteropWindowsSocketSuccess);
    guard_expect(owner == NULL && fcntl(pair[0], F_GETFD) < 0 && errno == EBADF);
    close(pair[1]);
}

typedef struct thread_guard_context_t {
    interop_windows_socket_t *owner;
    interop_windows_socket_t **owner_slot;
    int result;
    interop_windows_socket_poll_t poll;
} thread_guard_context_t;

static void *wrong_thread_poll(void *opaque)
{
    thread_guard_context_t *context = opaque;
    memset(&context->poll, 0xA5, sizeof(context->poll));
    context->result = interop_windows_socket_poll_events(
        context->owner, 0xA11D1, 100, &context->poll);
    return NULL;
}

static void *wrong_thread_destroy(void *opaque)
{
    thread_guard_context_t *context = opaque;
    context->result = interop_windows_socket_destroy(context->owner_slot);
    return NULL;
}

static void test_guard_thread_token_and_time(void)
{
    interop_windows_socket_config_t config = guard_config(0xA11D1);
    interop_windows_socket_t *owner = NULL;
    interop_windows_socket_poll_t poll;
    interop_windows_socket_poll_t before;
    thread_guard_context_t context;
    pthread_t thread;
    int pair[2] = {-1, -1};
    guard_expect(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    guard_expect(interop_windows_socket_create(pair[0], &config, &owner) ==
                 InteropWindowsSocketSuccess);
    memset(&poll, 0x5A, sizeof(poll));
    memcpy(&before, &poll, sizeof(before));
    guard_expect(interop_windows_socket_poll_events(owner, 0, 100, &poll) ==
                 InteropWindowsSocketWrongToken);
    guard_expect(memcmp(&poll, &before, sizeof(poll)) == 0);
    guard_expect(interop_windows_socket_poll_events(owner, 0xA11D1, 99, &poll) ==
                 InteropWindowsSocketTimeWentBackwards);
    guard_expect(memcmp(&poll, &before, sizeof(poll)) == 0);
    memset(&context, 0, sizeof(context));
    context.owner = owner;
    guard_expect(pthread_create(&thread, NULL, wrong_thread_poll, &context) == 0);
    guard_expect(pthread_join(thread, NULL) == 0);
    guard_expect(context.result == InteropWindowsSocketWrongThread);
    memset(&poll, 0xA5, sizeof(poll));
    memcpy(&before, &poll, sizeof(before));
    guard_expect(memcmp(&context.poll, &before, sizeof(before)) == 0);
    context.owner_slot = &owner;
    guard_expect(pthread_create(&thread, NULL, wrong_thread_destroy, &context) == 0);
    guard_expect(pthread_join(thread, NULL) == 0);
    guard_expect(context.result == InteropWindowsSocketWrongThread && owner != NULL);
    guard_expect(interop_windows_socket_destroy(&owner) == InteropWindowsSocketSuccess);
    close(pair[1]);
}

static void test_guard_operation_output_preflights(void)
{
    interop_windows_socket_config_t config = guard_config(0xA11D3);
    interop_windows_socket_t *owner = NULL;
    interop_windows_socket_poll_t poll;
    interop_windows_socket_poll_t before_poll;
    interop_windows_socket_work_t work;
    interop_windows_socket_work_t before_work;
    interop_peer_event_t event;
    interop_peer_event_t before_event;
    _Alignas(interop_peer_event_t) uint8_t event_bytes[sizeof(interop_peer_event_t) + 1];
    _Alignas(interop_windows_socket_work_t) uint8_t frame[InteropWindowsSocketFrameCapacity];
    int pair[2] = {-1, -1};

    guard_expect(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    guard_expect(interop_windows_socket_create(pair[0], &config, &owner) ==
                 InteropWindowsSocketSuccess);
    memset(&poll, 0x39, sizeof(poll));
    memcpy(&before_poll, &poll, sizeof(poll));
    guard_expect(interop_windows_socket_poll_events(NULL, config.peer_config.connection_token,
                                                    100, &poll) == InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_poll_events(owner, config.peer_config.connection_token,
                                                    100, NULL) == InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_poll_events(owner, config.peer_config.connection_token,
                                                    100, (interop_windows_socket_poll_t *)
                                                    ((uint8_t *)&poll + 1)) ==
                 InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_poll_events(owner, config.peer_config.connection_token,
                 100, (interop_windows_socket_poll_t *)(void *)owner) ==
                 InteropWindowsSocketInvalidArgument);
    guard_expect(memcmp(&poll, &before_poll, sizeof(poll)) == 0);

    memset(&work, 0x5C, sizeof(work));
    memcpy(&before_work, &work, sizeof(work));
    guard_expect(interop_windows_socket_get_closed_snapshot(NULL,
                 config.peer_config.connection_token, &work) == InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_get_closed_snapshot(owner,
                 config.peer_config.connection_token, NULL) == InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_get_closed_snapshot(owner,
                 config.peer_config.connection_token,
                 (interop_windows_socket_work_t *)(void *)((uint8_t *)&work + 1)) ==
                 InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_get_closed_snapshot(owner,
                 config.peer_config.connection_token,
                 (interop_windows_socket_work_t *)(void *)owner) ==
                 InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_get_closed_snapshot(owner,
                 config.peer_config.connection_token, &work) == InteropWindowsSocketWrongPhase);
    guard_expect(interop_windows_socket_drive(NULL, config.peer_config.connection_token,
                 1, 100, 0, 0, 0, 0, frame, sizeof(frame), &work) ==
                 InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_drive(owner, config.peer_config.connection_token,
                 1, 100, 0, 0, 0, 0, frame, sizeof(frame), NULL) ==
                 InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_drive(owner, config.peer_config.connection_token,
                 1, 100, 0, 0, 0, 0, NULL, sizeof(frame), &work) ==
                 InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_drive(owner, config.peer_config.connection_token,
                 1, 100, 0, 0, 0, 0, frame, sizeof(frame),
                 (interop_windows_socket_work_t *)(void *)owner) ==
                 InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_drive(owner, config.peer_config.connection_token,
                 1, 100, 0, 0, 0, 0, (uint8_t *)(void *)owner, sizeof(frame), &work) ==
                 InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_drive(owner, config.peer_config.connection_token,
                 1, 100, 0, 0, 0, 0, frame, sizeof(frame),
                 (interop_windows_socket_work_t *)(void *)frame) ==
                 InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_drive(owner, config.peer_config.connection_token,
                 1, 100, 0, 0, 0, 0, frame, sizeof(frame),
                 (interop_windows_socket_work_t *)(void *)((uint8_t *)&work + 1)) ==
                 InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_drive(owner, config.peer_config.connection_token,
                 1, 100, 0, 0, 0, 0, (uint8_t *)(uintptr_t)(UINTPTR_MAX - 7u),
                 InteropWindowsSocketFrameCapacity, &work) == InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_drive(owner, config.peer_config.connection_token,
                 1, 100, 0, 0, 0, 0, frame, sizeof(frame),
                 (interop_windows_socket_work_t *)(uintptr_t)(UINTPTR_MAX - 7u)) ==
                 InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_drive(owner, config.peer_config.connection_token,
                 1, 100, 0, 0, 0, 0, frame, sizeof(frame) - 1, &work) ==
                 InteropWindowsSocketInvalidArgument);
    guard_expect(memcmp(&work, &before_work, sizeof(work)) == 0);
    guard_expect(interop_windows_socket_drive(owner, config.peer_config.connection_token,
                 1, 100, UINT32_MAX, 0, 0, 0, frame, sizeof(frame), &work) ==
                 InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_poll_events(owner, config.peer_config.connection_token,
                                                    100, &poll) == InteropWindowsSocketSuccess);
    guard_expect(interop_windows_socket_drive(owner, config.peer_config.connection_token + 1,
                 poll.poll_cookie, 100, 0, 0, 0, 0, frame, sizeof(frame), &work) ==
                 InteropWindowsSocketWrongToken);
    guard_expect(interop_windows_socket_drive(owner, config.peer_config.connection_token,
                 poll.poll_cookie, 99, 0, 0, 0, 0, frame, sizeof(frame), &work) ==
                 InteropWindowsSocketTimeWentBackwards);
    guard_expect(interop_windows_socket_drive(owner, config.peer_config.connection_token,
                 poll.poll_cookie + 1, 100, 0, 0, 0, 0, frame, sizeof(frame), &work) ==
                 InteropWindowsSocketStalePoll);
    guard_expect(interop_windows_socket_drive(owner, config.peer_config.connection_token,
                 poll.poll_cookie, 100, 0, InteropWindowsSocketMaxRxBudgetBytes + 1, 0, 0,
                 frame, sizeof(frame), &work) == InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_drive(owner, config.peer_config.connection_token,
                 poll.poll_cookie, 100, 0, 0, InteropWindowsSocketMaxTxBudgetBytes + 1, 0,
                 frame, sizeof(frame), &work) == InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_drive(owner, config.peer_config.connection_token,
                 poll.poll_cookie, 100, 0, 0, 0,
                 InteropWindowsSocketMaxSyscallBudget + 1, frame, sizeof(frame), &work) ==
                 InteropWindowsSocketInvalidArgument);
    guard_expect(memcmp(&work, &before_work, sizeof(work)) == 0);
    guard_expect(interop_windows_socket_drive(owner, config.peer_config.connection_token,
                 poll.poll_cookie, 100, 0, 0, 0, 0, frame, sizeof(frame), &work) ==
                 InteropWindowsSocketSuccess);

    memset(&event, 0xA6, sizeof(event));
    memcpy(&before_event, &event, sizeof(event));
    guard_expect(interop_windows_socket_mapping_result(NULL,
                 config.peer_config.connection_token, 100, 1,
                 InteropLayoutVersion, 1, &event) == InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_mapping_result(owner,
                 config.peer_config.connection_token, 100, 1,
                 InteropLayoutVersion, 1, NULL) == InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_mapping_result(owner,
                 config.peer_config.connection_token, 100, 1,
                 InteropLayoutVersion, 1, (interop_peer_event_t *)(void *)owner) ==
                 InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_mapping_result(owner,
                 config.peer_config.connection_token, 100, 1,
                 InteropLayoutVersion, 1, (interop_peer_event_t *)(void *)(event_bytes + 1)) ==
                 InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_mapping_result(owner,
                 config.peer_config.connection_token, 100, 1,
                 InteropLayoutVersion, 1, &event) == InteropWindowsSocketWrongPhase);
    guard_expect(interop_windows_socket_begin_drain(NULL, config.peer_config.connection_token,
                 100, &event) == InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_begin_drain(owner, config.peer_config.connection_token,
                 100, NULL) == InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_begin_drain(owner, config.peer_config.connection_token,
                 100, (interop_peer_event_t *)(void *)owner) == InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_begin_drain(owner, config.peer_config.connection_token,
                 100, (interop_peer_event_t *)(void *)(event_bytes + 1)) ==
                 InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_begin_drain(owner, 0, 100, &event) ==
                 InteropWindowsSocketWrongToken);
    guard_expect(memcmp(&event, &before_event, sizeof(event)) == 0);

    guard_expect(interop_windows_socket_abort(owner, config.peer_config.connection_token,
                                              &work) == InteropWindowsSocketSuccess);
    guard_expect(interop_windows_socket_abort(NULL, config.peer_config.connection_token,
                                              &work) == InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_abort(owner, config.peer_config.connection_token,
                                              NULL) == InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_abort(owner, config.peer_config.connection_token,
                 (interop_windows_socket_work_t *)(void *)owner) ==
                 InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_get_closed_snapshot(owner,
                 config.peer_config.connection_token, &work) == InteropWindowsSocketSuccess);
    guard_expect(work.close_wsa_error == 0);
    guard_expect(interop_windows_socket_destroy(&owner) == InteropWindowsSocketSuccess);
    guard_expect(owner == NULL && close(pair[1]) == 0);
}

static void test_guard_guest_mapping_preflight(void)
{
    interop_windows_socket_config_t config = guard_config(0xA11D4);
    interop_windows_socket_t *owner = NULL;
    interop_peer_event_t event;
    interop_windows_socket_poll_t poll;
    interop_windows_socket_work_t work;
    uint8_t frame[InteropWindowsSocketFrameCapacity];
    int pair[2] = {-1, -1};
    config.peer_config.role = InteropPeerGuest;
    guard_expect(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    guard_expect(interop_windows_socket_create(pair[0], &config, &owner) ==
                 InteropWindowsSocketSuccess);

    guard_expect(interop_windows_socket_mapping_result(owner,
                 config.peer_config.connection_token, 100, 0, InteropLayoutVersion,
                 2, &event) == InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_mapping_result(owner,
                 config.peer_config.connection_token, 100, 0, InteropLayoutVersion,
                 1, &event) == InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_mapping_result(owner,
                 config.peer_config.connection_token, 100, 1, InteropLayoutVersion + 1,
                 1, &event) == InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_mapping_result(owner,
                 config.peer_config.connection_token, 100, 1, InteropLayoutVersion,
                 0, &event) == InteropWindowsSocketMappingFailed);
    guard_expect(interop_windows_socket_mapping_result(owner,
                 config.peer_config.connection_token, 100, 1, InteropLayoutVersion,
                 1, &event) == InteropWindowsSocketSuccess);
    guard_expect(interop_windows_socket_mapping_result(owner,
                 config.peer_config.connection_token, 100, 1, InteropLayoutVersion,
                 1, &event) == InteropWindowsSocketWrongPhase);
    guard_expect(interop_windows_socket_abort(owner, config.peer_config.connection_token,
                                              &(interop_windows_socket_work_t){0}) ==
                 InteropWindowsSocketSuccess);
    guard_expect(interop_windows_socket_mapping_result(owner,
                 config.peer_config.connection_token, 100, 1, InteropLayoutVersion,
                 1, &event) == InteropWindowsSocketClosed);
    guard_expect(interop_windows_socket_begin_drain(owner,
                 config.peer_config.connection_token, 100, &event) ==
                 InteropWindowsSocketClosed);
    guard_expect(interop_windows_socket_poll_events(owner,
                 config.peer_config.connection_token, 100, &poll) ==
                 InteropWindowsSocketSuccess);
    guard_expect(interop_windows_socket_drive(owner, config.peer_config.connection_token,
                 poll.poll_cookie, 100, 0, 0, 0, 0, frame, sizeof(frame), &work) ==
                 InteropWindowsSocketSuccess);
    guard_expect(work.status == InteropWindowsSocketWorkTransportClosed &&
                 work.event.kind == InteropPeerEventNone);
    guard_expect(interop_windows_socket_destroy((interop_windows_socket_t **)(void *)
                 ((uint8_t *)&owner + 1)) == InteropWindowsSocketInvalidArgument);
    guard_expect(interop_windows_socket_destroy(&owner) == InteropWindowsSocketSuccess);
    guard_expect(close(pair[1]) == 0);
}

void interop_windows_socket_test_guards(void)
{
    test_guard_invalid_inputs();
    test_guard_connected_and_linger_checks();
    test_guard_socket_metadata_and_fionbio_failures();
    test_guard_success_sets_nonblocking_and_owns_once();
    test_guard_thread_token_and_time();
    test_guard_operation_output_preflights();
    test_guard_guest_mapping_preflight();
}
