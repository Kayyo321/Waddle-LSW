/**
 * @file linux_socket_owner_guards.c
 * @brief Public-API preflight and descriptor-transfer regressions.
 *
 * This helper exercises only the frozen socket-owner ABI with real local
 * socket descriptors. It does not inspect opaque owner storage or simulate
 * peer internals. The main socketpair fixture calls the documented entry
 * point below so the guard checks share its linked production implementation.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waddle/interop_linux_socket.h"
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

/** @brief Assert a test condition without relying on NDEBUG behavior. */
static void guard_expect(int condition)
{
    if (!condition)
        abort();
}

/** @brief Build a valid host configuration for one connected test socket. */
static interop_linux_socket_config_t guard_config(uint64_t token)
{
    static const uint8_t Version[] = "1.2.3";
    interop_linux_socket_config_t config;

    memset(&config, 0, sizeof(config));
    config.peer_config.role = InteropPeerHost;
    config.peer_config.connection_token = token;
    config.peer_config.accepted_at_ms = 100u;
    config.peer_config.agent_version = Version;
    config.peer_config.agent_version_bytes = sizeof(Version) - 1u;
    config.drain_timeout_ms = 1000u;
    return config;
}

/** @brief Create an AF_UNIX stream pair with exactly the requested flags. */
static int guard_socket_pair(int flags, int pair[2])
{
    return socketpair(AF_UNIX, SOCK_STREAM | flags, 0, pair);
}

/** @brief Verify failed create retains a valid caller-owned descriptor. */
static void guard_rejected_fd(int fd, const interop_linux_socket_config_t *config,
                              int expected_result)
{
    interop_linux_socket_t *owner = NULL;
    int result = interop_linux_socket_create(fd, config, &owner);

    guard_expect(result == expected_result);
    guard_expect(owner == NULL);
    guard_expect(fcntl(fd, F_GETFD) >= 0);
}

/** @brief Destroy a valid owner and close the peer endpoint. */
static void guard_cleanup_owner(interop_linux_socket_t **owner, int peer_fd)
{
    guard_expect(interop_linux_socket_destroy(owner) == InteropLinuxSocketSuccess);
    guard_expect(*owner == NULL);
    if (peer_fd >= 0)
        guard_expect(close(peer_fd) == 0);
}

typedef struct guard_foreign_call_t {
    interop_linux_socket_t *owner;
    uint64_t token;
    uint64_t poll_cookie;
    interop_linux_socket_poll_t poll_out;
    interop_linux_socket_work_t snapshot_out;
    interop_linux_socket_work_t drive_out;
    interop_linux_socket_work_t abort_out;
    interop_peer_event_t mapping_out;
    interop_peer_event_t drain_out;
    uint8_t frame_out[InteropLinuxSocketFrameCapacity];
    int poll_result;
    int snapshot_result;
    int drive_result;
    int mapping_result;
    int drain_result;
    int abort_result;
    int destroy_result;
    interop_linux_socket_t *slot;
} guard_foreign_call_t;

/** @brief Check that every byte of a caller output retains its sentinel. */
static void guard_expect_bytes(const void *bytes, size_t length, uint8_t expected)
{
    const uint8_t *value = bytes;
    size_t index;

    for (index = 0; index < length; ++index)
        guard_expect(value[index] == expected);
}

/** @brief Attempt owner operations from a non-owner pthread. */
static void *guard_foreign_thread(void *opaque)
{
    guard_foreign_call_t *call = opaque;

    memset(&call->poll_out, 0xA5, sizeof(call->poll_out));
    memset(&call->snapshot_out, 0xA5, sizeof(call->snapshot_out));
    memset(&call->drive_out, 0xA5, sizeof(call->drive_out));
    memset(&call->abort_out, 0xA5, sizeof(call->abort_out));
    memset(&call->mapping_out, 0xA5, sizeof(call->mapping_out));
    memset(&call->drain_out, 0xA5, sizeof(call->drain_out));
    memset(call->frame_out, 0xA5, sizeof(call->frame_out));
    call->poll_result = interop_linux_socket_poll_events(call->owner, call->token, 100u,
                                                          &call->poll_out);
    call->snapshot_result = interop_linux_socket_get_closed_snapshot(call->owner, call->token,
                                                                       &call->snapshot_out);
    call->drive_result = interop_linux_socket_drive(call->owner, call->token, call->poll_cookie,
                                                     100u, 0u, 0u, 0u, 0u, call->frame_out,
                                                     sizeof(call->frame_out), &call->drive_out);
    call->mapping_result = interop_linux_socket_mapping_result(call->owner, call->token, 100u,
                                                                1u, InteropLayoutVersion, 1u,
                                                                &call->mapping_out);
    call->drain_result = interop_linux_socket_begin_drain(call->owner, call->token, 100u,
                                                           &call->drain_out);
    call->abort_result = interop_linux_socket_abort(call->owner, call->token, &call->abort_out);
    call->slot = call->owner;
    call->destroy_result = interop_linux_socket_destroy(&call->slot);
    return NULL;
}

/** @brief Check create-time validation and descriptor ownership rollback. */
static void test_create_guards(void)
{
    interop_linux_socket_config_t config = guard_config(0xA11CEu);
    interop_linux_socket_t *owner = NULL;
    int pair[2] = {-1, -1};
    int fd;
    int flags;
    int descriptor_flags;

    guard_expect(interop_linux_socket_create(-1, &config, &owner) == InteropLinuxSocketInvalidArgument);
    guard_expect(owner == NULL);
    guard_expect(interop_linux_socket_create(0, NULL, &owner) == InteropLinuxSocketInvalidArgument);
    guard_expect(owner == NULL);
    guard_expect(interop_linux_socket_create(-1, &config, NULL) == InteropLinuxSocketInvalidArgument);

    guard_expect(guard_socket_pair(0, pair) == 0);
    guard_rejected_fd(pair[0], &config, InteropLinuxSocketInvalidArgument); /* blocking and no CLOEXEC */
    flags = fcntl(pair[0], F_GETFL);
    descriptor_flags = fcntl(pair[0], F_GETFD);
    guard_expect(flags >= 0 && (flags & O_NONBLOCK) == 0);
    guard_expect(descriptor_flags >= 0 && (descriptor_flags & FD_CLOEXEC) == 0);
    guard_expect(close(pair[0]) == 0);
    guard_expect(close(pair[1]) == 0);

    guard_expect(guard_socket_pair(SOCK_NONBLOCK, pair) == 0);
    guard_rejected_fd(pair[0], &config, InteropLinuxSocketInvalidArgument); /* missing CLOEXEC */
    guard_expect((fcntl(pair[0], F_GETFL) & O_NONBLOCK) != 0);
    guard_expect((fcntl(pair[0], F_GETFD) & FD_CLOEXEC) == 0);
    guard_expect(close(pair[0]) == 0);
    guard_expect(close(pair[1]) == 0);

    guard_expect(guard_socket_pair(SOCK_CLOEXEC, pair) == 0);
    guard_rejected_fd(pair[0], &config, InteropLinuxSocketInvalidArgument); /* missing NONBLOCK */
    guard_expect((fcntl(pair[0], F_GETFL) & O_NONBLOCK) == 0);
    guard_expect((fcntl(pair[0], F_GETFD) & FD_CLOEXEC) != 0);
    guard_expect(close(pair[0]) == 0);
    guard_expect(close(pair[1]) == 0);

    fd = open("/dev/null", O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    guard_expect(fd >= 0);
    guard_rejected_fd(fd, &config, InteropLinuxSocketInvalidArgument); /* not a socket */
    guard_expect(close(fd) == 0);

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    guard_expect(fd >= 0);
    guard_rejected_fd(fd, &config, InteropLinuxSocketInvalidArgument); /* unconnected stream */
    guard_expect(close(fd) == 0);

    guard_expect(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
    guard_rejected_fd(pair[0], &config, InteropLinuxSocketInvalidArgument); /* connected, wrong type */
    guard_expect(close(pair[0]) == 0);
    guard_expect(close(pair[1]) == 0);

    guard_expect(guard_socket_pair(SOCK_NONBLOCK | SOCK_CLOEXEC, pair) == 0);
    config.peer_config.connection_token = 0;
    guard_rejected_fd(pair[0], &config, InteropLinuxSocketInvalidArgument);
    config = guard_config(0xA11CEu);
    config.drain_timeout_ms = 0;
    guard_rejected_fd(pair[0], &config, InteropLinuxSocketInvalidArgument);
    config = guard_config(0xA11CEu);
    config.reserved_zero = 1u;
    guard_rejected_fd(pair[0], &config, InteropLinuxSocketInvalidArgument);
    config = guard_config(0xA11CEu);
    config.peer_config.role = 99u;
    guard_rejected_fd(pair[0], &config, InteropLinuxSocketInvalidArgument);
    config = guard_config(0xA11CEu);
    config.peer_config.agent_version_bytes = 4u;
    guard_rejected_fd(pair[0], &config, InteropLinuxSocketInvalidArgument);
    config = guard_config(0xA11CEu);
    config.peer_config.agent_version = NULL;
    config.peer_config.agent_version_bytes = 0u;
    guard_rejected_fd(pair[0], &config, InteropLinuxSocketInvalidArgument);
    config = guard_config(0xA11CEu);
    config.peer_config.agent_version = (const uint8_t *)(uintptr_t)(UINTPTR_MAX - 7u);
    config.peer_config.agent_version_bytes = 8u;
    guard_rejected_fd(pair[0], &config, InteropLinuxSocketInvalidArgument);
    config = guard_config(0xA11CEu);
    config.drain_timeout_ms = InteropLinuxSocketMaxDrainTimeoutMs + 1u;
    guard_rejected_fd(pair[0], &config, InteropLinuxSocketInvalidArgument);
    guard_expect(close(pair[0]) == 0);
    guard_expect(close(pair[1]) == 0);

    guard_expect(guard_socket_pair(SOCK_NONBLOCK | SOCK_CLOEXEC, pair) == 0);
    config = guard_config(0xA11CEu);
    owner = (interop_linux_socket_t *)(uintptr_t)1u;
    guard_expect(interop_linux_socket_create(pair[0], &config, &owner) ==
                 InteropLinuxSocketInvalidArgument);
    guard_expect(owner == (interop_linux_socket_t *)(uintptr_t)1u);
    owner = NULL;
    {
        interop_linux_socket_t *overlap_slot = NULL;
        config.peer_config.agent_version = (const uint8_t *)(const void *)&overlap_slot;
        config.peer_config.agent_version_bytes = sizeof(overlap_slot);
        guard_expect(interop_linux_socket_create(pair[0], &config, &overlap_slot) ==
                     InteropLinuxSocketInvalidArgument);
        guard_expect(overlap_slot == NULL);
    }
    config = guard_config(0xA11CEu);
    {
        _Alignas(interop_linux_socket_config_t) unsigned char storage[sizeof(config) + _Alignof(interop_linux_socket_config_t)] = {0};
        interop_linux_socket_config_t *misaligned =
            (interop_linux_socket_config_t *)(void *)(storage + 1u);
        _Alignas(interop_linux_socket_t *) unsigned char slot_storage[sizeof(owner) + _Alignof(interop_linux_socket_t *)] = {0};
        interop_linux_socket_t **misaligned_slot =
            (interop_linux_socket_t **)(void *)(slot_storage + 1u);
        memcpy(misaligned, &config, sizeof(config));
        guard_expect(interop_linux_socket_create(pair[0], misaligned, &owner) == InteropLinuxSocketInvalidArgument);
        guard_expect(owner == NULL);
        guard_expect(interop_linux_socket_create(pair[0], &config, misaligned_slot) == InteropLinuxSocketInvalidArgument);
        guard_expect(owner == NULL);
    }
    {
        interop_linux_socket_config_t before;
        memcpy(&before, &config, sizeof(before));
        interop_linux_socket_t **overlap_slot = (interop_linux_socket_t **)(void *)&config.peer_config.local_capabilities;
        guard_expect(config.peer_config.local_capabilities == 0u);
        guard_expect(interop_linux_socket_create(pair[0], &config, overlap_slot) == InteropLinuxSocketInvalidArgument);
        guard_expect(memcmp(&config, &before, sizeof(config)) == 0);
        guard_expect(owner == NULL);
    }
    guard_expect(interop_linux_socket_create(
                     pair[0],
                     (const interop_linux_socket_config_t *)(uintptr_t)(UINTPTR_MAX - sizeof(config) + 1u),
                     &owner) == InteropLinuxSocketInvalidArgument);
    guard_expect(owner == NULL);
    guard_expect(interop_linux_socket_create(
                     pair[0], &config,
                     (interop_linux_socket_t **)(uintptr_t)(UINTPTR_MAX - sizeof(owner) + 1u)) ==
                 InteropLinuxSocketInvalidArgument);
    guard_expect(owner == NULL);
    guard_expect(interop_linux_socket_create(pair[0], &config, &owner) == InteropLinuxSocketSuccess);
    guard_expect(owner != NULL);
    guard_expect(fcntl(pair[0], F_GETFD) >= 0); /* ownership transfer keeps the fd open */
    guard_cleanup_owner(&owner, pair[1]);
    guard_expect(fcntl(pair[0], F_GETFD) < 0 && errno == EBADF); /* destroy closes once */
}

/** @brief Exercise local preflight failures without consuming a valid cookie. */
static void test_owner_guard_calls(void)
{
    int pair[2] = {-1, -1};
    interop_linux_socket_config_t config = guard_config(0xB0B0u);
    interop_linux_socket_t *owner = NULL;
    interop_linux_socket_poll_t poll_out;
    interop_linux_socket_poll_t poll_before;
    interop_linux_socket_work_t work;
    interop_linux_socket_work_t work_before;
    interop_peer_event_t event;
    interop_peer_event_t event_before;
    uint8_t frame[InteropLinuxSocketFrameCapacity];
    uint8_t misaligned_frame[InteropLinuxSocketFrameCapacity + 1u];
    uint8_t frame_before[32];
    size_t i;
    uint64_t cookie;
    int result;

    guard_expect(guard_socket_pair(SOCK_NONBLOCK | SOCK_CLOEXEC, pair) == 0);
    guard_expect(interop_linux_socket_create(pair[0], &config, &owner) == InteropLinuxSocketSuccess);

    guard_expect(interop_linux_socket_poll_events(NULL, 0xB0B0u, 100u, &poll_out) ==
                 InteropLinuxSocketInvalidArgument);
    guard_expect(interop_linux_socket_get_closed_snapshot(NULL, 0xB0B0u, &work) ==
                 InteropLinuxSocketInvalidArgument);
    guard_expect(interop_linux_socket_drive(NULL, 0xB0B0u, 1u, 100u, 0u,
                                            0u, 0u, 0u, frame, sizeof(frame), &work) ==
                 InteropLinuxSocketInvalidArgument);
    guard_expect(interop_linux_socket_mapping_result(NULL, 0xB0B0u, 100u, 1u,
                                                      InteropLayoutVersion, 1u, &event) ==
                 InteropLinuxSocketInvalidArgument);
    guard_expect(interop_linux_socket_begin_drain(NULL, 0xB0B0u, 100u, &event) ==
                 InteropLinuxSocketInvalidArgument);
    guard_expect(interop_linux_socket_abort(NULL, 0xB0B0u, &work) ==
                 InteropLinuxSocketInvalidArgument);

    memset(frame, 0xA5, sizeof(frame));
    memcpy(frame_before, frame, sizeof(frame_before));
    memset(&poll_out, 0xA5, sizeof(poll_out));
    memcpy(&poll_before, &poll_out, sizeof(poll_before));
    guard_expect(interop_linux_socket_poll_events(owner, 0, 100u, &poll_out) == InteropLinuxSocketWrongToken);
    guard_expect(memcmp(&poll_out, &poll_before, sizeof(poll_out)) == 0);
    guard_expect(memcmp(frame, frame_before, sizeof(frame_before)) == 0);

    guard_expect(interop_linux_socket_poll_events(owner, 0xB0B0u, 100u, &poll_out) == InteropLinuxSocketSuccess);
    cookie = poll_out.poll_cookie;
    guard_expect(cookie != 0u);
    memset(&work, 0xA5, sizeof(work));
    {
        uint8_t work_before_bytes[sizeof(work)];
        memcpy(work_before_bytes, &work, sizeof(work));
        guard_expect(interop_linux_socket_get_closed_snapshot(owner, 0xB0B0u, &work) ==
                     InteropLinuxSocketWrongPhase);
        guard_expect(memcmp(&work, work_before_bytes, sizeof(work)) == 0);
    }
    guard_expect(interop_linux_socket_get_closed_snapshot(owner, 0xB0B0u, NULL) ==
                 InteropLinuxSocketInvalidArgument);

    /* Aligned ranges that overflow uintptr_t must fail before any read/write. */
    guard_expect(interop_linux_socket_poll_events(
                     owner, 0xB0B0u, 100u,
                     (interop_linux_socket_poll_t *)(uintptr_t)(UINTPTR_MAX - sizeof(poll_out) + 1u)) ==
                 InteropLinuxSocketInvalidArgument);
    guard_expect(interop_linux_socket_get_closed_snapshot(
                     owner, 0xB0B0u,
                     (interop_linux_socket_work_t *)(uintptr_t)(UINTPTR_MAX - sizeof(work) + 1u)) ==
                 InteropLinuxSocketInvalidArgument);
    guard_expect(interop_linux_socket_drive(
                     owner, 0xB0B0u, cookie, 100u, 0u, 0u, 0u, 0u,
                     (uint8_t *)(uintptr_t)(UINTPTR_MAX - sizeof(frame) + 1u), sizeof(frame), &work) ==
                 InteropLinuxSocketInvalidArgument);
    guard_expect(interop_linux_socket_drive(
                     owner, 0xB0B0u, cookie, 100u, 0u, 0u, 0u, 0u, frame, sizeof(frame),
                     (interop_linux_socket_work_t *)(uintptr_t)(UINTPTR_MAX - sizeof(work) + 1u)) ==
                 InteropLinuxSocketInvalidArgument);
    guard_expect(interop_linux_socket_mapping_result(
                     owner, 0xB0B0u, 100u, 1u, InteropLayoutVersion, 1u,
                     (interop_peer_event_t *)(uintptr_t)(UINTPTR_MAX - sizeof(event) + 1u)) ==
                 InteropLinuxSocketInvalidArgument);
    guard_expect(interop_linux_socket_begin_drain(
                     owner, 0xB0B0u, 100u,
                     (interop_peer_event_t *)(uintptr_t)(UINTPTR_MAX - sizeof(event) + 1u)) ==
                 InteropLinuxSocketInvalidArgument);
    guard_expect(interop_linux_socket_abort(
                     owner, 0xB0B0u,
                     (interop_linux_socket_work_t *)(uintptr_t)(UINTPTR_MAX - sizeof(work) + 1u)) ==
                 InteropLinuxSocketInvalidArgument);
    {
        interop_linux_socket_t **wrapped_slot =
            (interop_linux_socket_t **)(uintptr_t)(UINTPTR_MAX - sizeof(owner) + 1u);
        guard_expect(interop_linux_socket_destroy(wrapped_slot) == InteropLinuxSocketInvalidArgument);
    }
    guard_expect(interop_linux_socket_drive(
                     owner, 0xB0B0u, cookie, 100u, 0u, 0u, 0u, 0u,
                     (uint8_t *)(uintptr_t)(UINTPTR_MAX - sizeof(frame) + 1u), sizeof(frame), &work) ==
                 InteropLinuxSocketInvalidArgument);

    /* Every typed output rejects misalignment before changing state or bytes. */
    {
        _Alignas(interop_linux_socket_poll_t) uint8_t poll_storage[sizeof(poll_out) + _Alignof(interop_linux_socket_poll_t)];
        _Alignas(interop_linux_socket_work_t) uint8_t work_storage[sizeof(work) + _Alignof(interop_linux_socket_work_t)];
        _Alignas(interop_peer_event_t) uint8_t event_storage[sizeof(event) + _Alignof(interop_peer_event_t)];
        interop_linux_socket_poll_t *bad_poll = (interop_linux_socket_poll_t *)(void *)(poll_storage + 1u);
        interop_linux_socket_work_t *bad_work = (interop_linux_socket_work_t *)(void *)(work_storage + 1u);
        interop_peer_event_t *bad_event = (interop_peer_event_t *)(void *)(event_storage + 1u);
        memset(poll_storage + 1u, 0xA5, sizeof(poll_out));
        memset(work_storage + 1u, 0xA5, sizeof(work));
        memset(event_storage + 1u, 0xA5, sizeof(event));
        guard_expect(interop_linux_socket_poll_events(owner, 0xB0B0u, 100u, bad_poll) ==
                     InteropLinuxSocketInvalidArgument);
        guard_expect_bytes(poll_storage + 1u, sizeof(poll_out), 0xA5);
        guard_expect(interop_linux_socket_get_closed_snapshot(owner, 0xB0B0u, bad_work) ==
                     InteropLinuxSocketInvalidArgument);
        guard_expect_bytes(work_storage + 1u, sizeof(work), 0xA5);
        guard_expect(interop_linux_socket_drive(owner, 0xB0B0u, cookie, 100u, 0u,
                                                0u, 0u, 0u, frame, sizeof(frame), bad_work) ==
                     InteropLinuxSocketInvalidArgument);
        guard_expect_bytes(work_storage + 1u, sizeof(work), 0xA5);
        guard_expect(interop_linux_socket_mapping_result(owner, 0xB0B0u, 100u, 1u,
                                                          InteropLayoutVersion, 1u, bad_event) ==
                     InteropLinuxSocketInvalidArgument);
        guard_expect_bytes(event_storage + 1u, sizeof(event), 0xA5);
        guard_expect(interop_linux_socket_begin_drain(owner, 0xB0B0u, 100u, bad_event) ==
                     InteropLinuxSocketInvalidArgument);
        guard_expect_bytes(event_storage + 1u, sizeof(event), 0xA5);
        guard_expect(interop_linux_socket_abort(owner, 0xB0B0u, bad_work) ==
                     InteropLinuxSocketInvalidArgument);
        guard_expect_bytes(work_storage + 1u, sizeof(work), 0xA5);
    }

    /* Public output spans may not alias the opaque owner allocation. */
    guard_expect(interop_linux_socket_get_closed_snapshot(
                     owner, 0xB0B0u, (interop_linux_socket_work_t *)(void *)owner) ==
                 InteropLinuxSocketInvalidArgument);
    guard_expect(interop_linux_socket_drive(
                     owner, 0xB0B0u, cookie, 100u, 0u, 0u, 0u, 0u, frame, sizeof(frame),
                     (interop_linux_socket_work_t *)(void *)owner) == InteropLinuxSocketInvalidArgument);
    guard_expect(interop_linux_socket_drive(
                     owner, 0xB0B0u, cookie, 100u, 0u, 0u, 0u, 0u,
                     (uint8_t *)(void *)owner, sizeof(frame), &work) == InteropLinuxSocketInvalidArgument);
    guard_expect(interop_linux_socket_mapping_result(
                     owner, 0xB0B0u, 100u, 1u, InteropLayoutVersion, 1u,
                     (interop_peer_event_t *)(void *)owner) == InteropLinuxSocketInvalidArgument);
    guard_expect(interop_linux_socket_begin_drain(
                     owner, 0xB0B0u, 100u, (interop_peer_event_t *)(void *)owner) ==
                 InteropLinuxSocketInvalidArgument);
    guard_expect(interop_linux_socket_abort(
                     owner, 0xB0B0u, (interop_linux_socket_work_t *)(void *)owner) ==
                 InteropLinuxSocketInvalidArgument);
    memset(&work, 0x4B, sizeof(work));
    memcpy(&work_before, &work, sizeof(work_before));
    guard_expect(interop_linux_socket_drive(owner, 0xB0B1u, cookie, 99u, 0u,
                                            0u, 0u, 0u, frame, sizeof(frame), &work) ==
                 InteropLinuxSocketWrongToken);
    guard_expect(memcmp(&work, &work_before, sizeof(work)) == 0);
    guard_expect(memcmp(frame, frame_before, sizeof(frame_before)) == 0);
    memcpy(&poll_before, &poll_out, sizeof(poll_before));
    guard_expect(interop_linux_socket_poll_events(owner, 0xB0B0u, 99u, &poll_out) == InteropLinuxSocketTimeWentBackwards);
    guard_expect(memcmp(&poll_out, &poll_before, sizeof(poll_out)) == 0);

    memset(&work, 0x5A, sizeof(work));
    memcpy(&work_before, &work, sizeof(work_before));
    guard_expect(interop_linux_socket_drive(owner, 0xB0B0u, cookie, 99u, 0u,
                                            0u, 0u, 0u, frame, sizeof(frame), &work) ==
                 InteropLinuxSocketTimeWentBackwards);
    guard_expect(memcmp(&work, &work_before, sizeof(work)) == 0);
    guard_expect(memcmp(frame, frame_before, sizeof(frame_before)) == 0);

    guard_expect(interop_linux_socket_drive(owner, 0xB0B0u, cookie, 100u, 0u,
                                            0u, 0u, 0u, frame, sizeof(frame), &work) ==
                 InteropLinuxSocketSuccess);
    guard_expect(work.syscalls == 0u && work.rx_bytes == 0u && work.tx_bytes == 0u);
    guard_expect(memcmp(frame, frame_before, sizeof(frame_before)) == 0);
    memcpy(&work_before, &work, sizeof(work_before));
    guard_expect(interop_linux_socket_drive(owner, 0xB0B0u, cookie, 100u, 0u,
                                            0u, 0u, 0u, frame, sizeof(frame), &work) ==
                 InteropLinuxSocketStalePoll);
    guard_expect(memcmp(&work, &work_before, sizeof(work)) == 0);

    guard_expect(interop_linux_socket_poll_events(owner, 0xB0B0u, 100u, &poll_out) == InteropLinuxSocketSuccess);
    cookie = poll_out.poll_cookie;
    memset(&work, 0x3C, sizeof(work));
    memcpy(&work_before, &work, sizeof(work_before));
    memcpy(frame, frame_before, sizeof(frame_before));
    guard_expect(interop_linux_socket_drive(owner, 0xB0B0u, cookie, 100u, UINT32_C(0x80000000),
                                            0u, 0u, 0u, frame, sizeof(frame), &work) ==
                 InteropLinuxSocketInvalidArgument);
    guard_expect(memcmp(&work, &work_before, sizeof(work)) == 0);
    guard_expect(memcmp(frame, frame_before, sizeof(frame_before)) == 0);

    guard_expect(interop_linux_socket_drive(owner, 0xB0B0u, cookie, 100u, 0u,
                                            InteropLinuxSocketMaxRxBudgetBytes + 1u, 0u, 0u,
                                            frame, sizeof(frame), &work) == InteropLinuxSocketInvalidArgument);
    guard_expect(memcmp(&work, &work_before, sizeof(work)) == 0);
    guard_expect(interop_linux_socket_drive(owner, 0xB0B0u, cookie, 100u, 0u,
                                            0u, InteropLinuxSocketMaxTxBudgetBytes + 1u, 0u,
                                            frame, sizeof(frame), &work) == InteropLinuxSocketInvalidArgument);
    guard_expect(interop_linux_socket_drive(owner, 0xB0B0u, cookie, 100u, 0u,
                                            0u, 0u, InteropLinuxSocketMaxSyscallBudget + 1u,
                                            frame, sizeof(frame), &work) == InteropLinuxSocketInvalidArgument);
    guard_expect(interop_linux_socket_drive(owner, 0xB0B0u, cookie, 100u, 0u,
                                            0u, 0u, 0u, frame, sizeof(frame) - 1u, &work) ==
                 InteropLinuxSocketInvalidArgument);
    guard_expect(memcmp(&work, &work_before, sizeof(work)) == 0);
    guard_expect(interop_linux_socket_drive(owner, 0xB0B0u, cookie, 100u, 0u,
                                            0u, 0u, 0u, NULL, sizeof(frame), &work) ==
                 InteropLinuxSocketInvalidArgument);
    guard_expect(interop_linux_socket_drive(owner, 0xB0B0u, cookie, 100u, 0u,
                                            0u, 0u, 0u, frame, sizeof(frame), NULL) ==
                 InteropLinuxSocketInvalidArgument);
    {
        _Alignas(interop_linux_socket_work_t) uint8_t work_storage[sizeof(work) + _Alignof(interop_linux_socket_work_t)] = {0};
        guard_expect(interop_linux_socket_drive(
                         owner, 0xB0B0u, cookie, 100u, 0u, 0u, 0u, 0u, frame, sizeof(frame),
                         (interop_linux_socket_work_t *)(void *)(work_storage + 1u)) ==
                     InteropLinuxSocketInvalidArgument);
    }

    memset(&event, 0xE1, sizeof(event));
    memcpy(&event_before, &event, sizeof(event_before));
    guard_expect(interop_linux_socket_mapping_result(owner, 0xB0B0u, 100u, 1u,
                                                      InteropLayoutVersion, 2u, &event) ==
                 InteropLinuxSocketInvalidArgument);
    guard_expect(memcmp(&event, &event_before, sizeof(event)) == 0);
    guard_expect(interop_linux_socket_mapping_result(owner, 0xB0B0u, 99u, 1u,
                                                      InteropLayoutVersion, 1u, &event) ==
                 InteropLinuxSocketTimeWentBackwards);
    guard_expect(memcmp(&event, &event_before, sizeof(event)) == 0);
    guard_expect(interop_linux_socket_mapping_result(owner, 0xB0B0u, 100u, 1u,
                                                      InteropLayoutVersion, 1u, &event) ==
                 InteropLinuxSocketWrongPhase);
    guard_expect(memcmp(&event, &event_before, sizeof(event)) == 0);
    guard_expect(interop_linux_socket_mapping_result(owner, 0xB0B0u, 100u, 1u,
                                                      InteropLayoutVersion, 1u, NULL) ==
                 InteropLinuxSocketInvalidArgument);
    guard_expect(interop_linux_socket_poll_events(owner, 0xB0B0u, 100u, NULL) ==
                 InteropLinuxSocketInvalidArgument);
    memset(&event, 0x73, sizeof(event));
    memcpy(&event_before, &event, sizeof(event_before));
    guard_expect(interop_linux_socket_mapping_result(owner, 0xB0B1u, 100u, 1u,
                                                      InteropLayoutVersion, 1u, &event) ==
                 InteropLinuxSocketWrongToken);
    guard_expect(memcmp(&event, &event_before, sizeof(event)) == 0);

    guard_expect(interop_linux_socket_begin_drain(owner, 0xB0B0u, 99u, &event) ==
                 InteropLinuxSocketTimeWentBackwards);
    guard_expect(memcmp(&event, &event_before, sizeof(event)) == 0);
    guard_expect(interop_linux_socket_begin_drain(owner, 0xB0B1u, 100u, &event) ==
                 InteropLinuxSocketWrongToken);
    guard_expect(memcmp(&event, &event_before, sizeof(event)) == 0);
    guard_expect(interop_linux_socket_begin_drain(owner, 0xB0B0u, 100u, NULL) ==
                 InteropLinuxSocketInvalidArgument);

    /* Outputs may not alias each other or owner storage. */
    {
        union {
            interop_linux_socket_work_t work_value;
            uint8_t bytes[InteropLinuxSocketFrameCapacity];
        } overlap;
        memset(&overlap, 0x6D, sizeof(overlap));
        uint8_t overlap_before[32];
        memcpy(overlap_before, overlap.bytes, sizeof(overlap_before));
        result = interop_linux_socket_drive(owner, 0xB0B0u, cookie, 100u, 0u,
                                            0u, 0u, 0u, overlap.bytes, sizeof(overlap.bytes),
                                            &overlap.work_value);
        guard_expect(result == InteropLinuxSocketInvalidArgument);
        guard_expect(memcmp(overlap.bytes, overlap_before, sizeof(overlap_before)) == 0);
    }
    memset(&poll_out, 0xA5, sizeof(poll_out));
    memcpy(&poll_before, &poll_out, sizeof(poll_before));
    result = interop_linux_socket_poll_events(owner, 0xB0B0u, 100u,
                                              (interop_linux_socket_poll_t *)(void *)owner);
    guard_expect(result == InteropLinuxSocketInvalidArgument);
    guard_expect(memcmp(&poll_out, &poll_before, sizeof(poll_out)) == 0);
    guard_expect(interop_linux_socket_destroy(NULL) == InteropLinuxSocketInvalidArgument);
    {
        interop_linux_socket_t *null_owner = NULL;
        guard_expect(interop_linux_socket_destroy(&null_owner) == InteropLinuxSocketSuccess);
        guard_expect(null_owner == NULL);
    }

    /* Every drive rejection above leaves the same poll cookie available. */
    guard_expect(interop_linux_socket_drive(owner, 0xB0B0u, cookie, 100u, 0u,
                                            0u, 0u, 0u, frame, sizeof(frame), &work) ==
                 InteropLinuxSocketSuccess);
    for (i = 0; i < sizeof(frame); ++i)
        guard_expect(frame[i] == 0xA5);
    /* Byte buffers have alignment1; an offset byte address is valid. */
    memset(misaligned_frame, 0xA5, sizeof(misaligned_frame));
    guard_expect(interop_linux_socket_poll_events(owner, 0xB0B0u, 100u, &poll_out) == InteropLinuxSocketSuccess);
    guard_expect(interop_linux_socket_drive(owner, 0xB0B0u, poll_out.poll_cookie, 100u,
                    0u, 0u, 0u, 0u, misaligned_frame + 1u, sizeof(frame), &work) == InteropLinuxSocketSuccess);
    for (i = 0; i < sizeof(misaligned_frame); ++i)
        guard_expect(misaligned_frame[i] == 0xA5);
    guard_cleanup_owner(&owner, pair[1]);
}

/** @brief Reject wrong cookies while preserving the still-current snapshot. */
static void test_current_cookie_rejections(void)
{
    const uint64_t Token = 0xC001u;
    int pair[2] = {-1, -1};
    interop_linux_socket_config_t config = guard_config(Token);
    interop_linux_socket_t *owner = NULL;
    interop_linux_socket_poll_t poll_out;
    interop_linux_socket_work_t work;
    uint8_t frame[InteropLinuxSocketFrameCapacity];
    uint64_t bad_cookie[2];
    size_t index;

    guard_expect(guard_socket_pair(SOCK_NONBLOCK | SOCK_CLOEXEC, pair) == 0);
    guard_expect(interop_linux_socket_create(pair[0], &config, &owner) == InteropLinuxSocketSuccess);
    guard_expect(interop_linux_socket_poll_events(owner, Token, 100u, &poll_out) ==
                 InteropLinuxSocketSuccess);
    bad_cookie[0] = 0u;
    bad_cookie[1] = poll_out.poll_cookie + 1u;
    for (index = 0; index < 2u; ++index) {
        memset(&work, 0xA5, sizeof(work));
        memset(frame, 0x6C, sizeof(frame));
        guard_expect(interop_linux_socket_drive(owner, Token, bad_cookie[index], 101u,
                         0u, 0u, 0u, 0u, frame, sizeof(frame), &work) == InteropLinuxSocketStalePoll);
        guard_expect_bytes(&work, sizeof(work), 0xA5);
        guard_expect_bytes(frame, sizeof(frame), 0x6C);
    }
    /* Earlier time and original cookie prove neither rejection committed them.
     * Positive I/O budgets without readiness must still issue no send/recv. */
    guard_expect(interop_linux_socket_drive(owner, Token, poll_out.poll_cookie, 100u,
                     0u, 1u, 1u, 1u, frame, sizeof(frame), &work) == InteropLinuxSocketSuccess);
    guard_expect(work.syscalls == 0u && work.event.kind == InteropPeerEventNone);
    guard_expect_bytes(frame, sizeof(frame), 0x6C);
    guard_cleanup_owner(&owner, pair[1]);
}

/** @brief Reject Guest layout evidence without committing time or a cookie. */
static void test_guest_mapping_layout_rejection(void)
{
    const uint64_t Token = 0xC002u;
    int pair[2] = {-1, -1};
    interop_linux_socket_config_t config = guard_config(Token);
    interop_linux_socket_t *owner = NULL;
    interop_linux_socket_poll_t poll_out;
    interop_linux_socket_work_t work;
    interop_peer_event_t event;
    uint8_t frame[InteropLinuxSocketFrameCapacity];

    config.peer_config.role = InteropPeerGuest;
    guard_expect(guard_socket_pair(SOCK_NONBLOCK | SOCK_CLOEXEC, pair) == 0);
    guard_expect(interop_linux_socket_create(pair[0], &config, &owner) == InteropLinuxSocketSuccess);
    guard_expect(interop_linux_socket_poll_events(owner, Token, 100u, &poll_out) ==
                 InteropLinuxSocketSuccess);
    memset(&event, 0x5B, sizeof(event));
    guard_expect(interop_linux_socket_mapping_result(owner, Token, 101u, 77u,
                     InteropLayoutVersion + 1u, 1u, &event) == InteropLinuxSocketInvalidArgument);
    guard_expect_bytes(&event, sizeof(event), 0x5B);
    memset(frame, 0xA5, sizeof(frame));
    guard_expect(interop_linux_socket_drive(owner, Token, poll_out.poll_cookie, 100u,
                     0u, 0u, 0u, 0u, frame, sizeof(frame), &work) == InteropLinuxSocketSuccess);
    guard_expect(work.state == InteropLinuxSocketMappingWait && work.syscalls == 0u);
    guard_expect_bytes(frame, sizeof(frame), 0xA5);
    guard_expect(interop_linux_socket_mapping_result(owner, Token, 100u, 77u,
                     InteropLayoutVersion, 1u, &event) == InteropLinuxSocketSuccess);
    guard_cleanup_owner(&owner, pair[1]);
}

/** @brief Verify authentic-owner thread affinity and preserve both outputs. */
static void test_owner_thread_guard(void)
{
    int pair[2] = {-1, -1};
    interop_linux_socket_config_t config = guard_config(0xC0DEu);
    interop_linux_socket_t *owner = NULL;
    guard_foreign_call_t call;
    pthread_t thread;

    guard_expect(guard_socket_pair(SOCK_NONBLOCK | SOCK_CLOEXEC, pair) == 0);
    guard_expect(interop_linux_socket_create(pair[0], &config, &owner) == InteropLinuxSocketSuccess);
    memset(&call, 0, sizeof(call));
    call.owner = owner;
    call.token = 0xC0DEu;
    {
        interop_linux_socket_poll_t poll;
        guard_expect(interop_linux_socket_poll_events(owner, call.token, 100u, &poll) ==
                     InteropLinuxSocketSuccess);
        call.poll_cookie = poll.poll_cookie;
    }
    guard_expect(pthread_create(&thread, NULL, guard_foreign_thread, &call) == 0);
    guard_expect(pthread_join(thread, NULL) == 0);
    guard_expect(call.poll_result == InteropLinuxSocketWrongThread);
    guard_expect(call.destroy_result == InteropLinuxSocketWrongThread);
    guard_expect(call.slot == owner);
    guard_expect(call.snapshot_result == InteropLinuxSocketWrongThread);
    guard_expect(call.drive_result == InteropLinuxSocketWrongThread);
    guard_expect(call.mapping_result == InteropLinuxSocketWrongThread);
    guard_expect(call.drain_result == InteropLinuxSocketWrongThread);
    guard_expect(call.abort_result == InteropLinuxSocketWrongThread);
    guard_expect_bytes(&call.poll_out, sizeof(call.poll_out), 0xA5);
    guard_expect_bytes(&call.snapshot_out, sizeof(call.snapshot_out), 0xA5);
    guard_expect_bytes(&call.drive_out, sizeof(call.drive_out), 0xA5);
    guard_expect_bytes(&call.abort_out, sizeof(call.abort_out), 0xA5);
    guard_expect_bytes(&call.mapping_out, sizeof(call.mapping_out), 0xA5);
    guard_expect_bytes(&call.drain_out, sizeof(call.drain_out), 0xA5);
    guard_expect_bytes(call.frame_out, sizeof(call.frame_out), 0xA5);
    guard_cleanup_owner(&owner, pair[1]);
}

/** @brief Exercise abort, stable closed snapshots, and terminal callbacks. */
static void test_closed_owner_guards(void)
{
    const uint64_t Token = 0xD1E0u;
    int pair[2] = {-1, -1};
    interop_linux_socket_config_t config = guard_config(Token);
    interop_linux_socket_t *owner = NULL;
    interop_linux_socket_poll_t poll;
    interop_linux_socket_work_t work;
    interop_linux_socket_work_t before_work;
    interop_peer_event_t event;
    interop_peer_event_t before_event;
    uint8_t frame[InteropLinuxSocketFrameCapacity];
    uint8_t frame_before[sizeof(frame)];
    uint64_t cookie;

    guard_expect(guard_socket_pair(SOCK_NONBLOCK | SOCK_CLOEXEC, pair) == 0);
    guard_expect(interop_linux_socket_create(pair[0], &config, &owner) == InteropLinuxSocketSuccess);

    memset(&work, 0xA5, sizeof(work));
    memcpy(&before_work, &work, sizeof(work));
    guard_expect(interop_linux_socket_get_closed_snapshot(owner, Token + 1u, &work) ==
                 InteropLinuxSocketWrongToken);
    guard_expect(memcmp(&work, &before_work, sizeof(work)) == 0);
    guard_expect(interop_linux_socket_get_closed_snapshot(owner, Token, &work) ==
                 InteropLinuxSocketWrongPhase);
    guard_expect(memcmp(&work, &before_work, sizeof(work)) == 0);
    guard_expect(interop_linux_socket_get_closed_snapshot(owner, Token, NULL) ==
                 InteropLinuxSocketInvalidArgument);

    memset(&event, 0x5B, sizeof(event));
    memcpy(&before_event, &event, sizeof(event));
    guard_expect(interop_linux_socket_abort(owner, Token + 1u, &work) ==
                 InteropLinuxSocketWrongToken);
    guard_expect(memcmp(&work, &before_work, sizeof(work)) == 0);
    guard_expect(interop_linux_socket_abort(owner, Token, NULL) ==
                 InteropLinuxSocketInvalidArgument);
    guard_expect(interop_linux_socket_mapping_result(NULL, Token, 100u, 1u,
                                                      InteropLayoutVersion, 1u, &event) ==
                 InteropLinuxSocketInvalidArgument);
    guard_expect(interop_linux_socket_begin_drain(NULL, Token, 100u, &event) ==
                 InteropLinuxSocketInvalidArgument);
    guard_expect(interop_linux_socket_abort(NULL, Token, &work) ==
                 InteropLinuxSocketInvalidArgument);
    guard_expect(memcmp(&event, &before_event, sizeof(event)) == 0);

    memset(&work, 0xC3, sizeof(work));
    guard_expect(interop_linux_socket_abort(owner, Token, &work) == InteropLinuxSocketSuccess);
    guard_expect(work.state == InteropLinuxSocketTransportClosed);
    guard_expect(work.status == InteropLinuxSocketWorkTransportClosed);
    guard_expect(work.terminal_reason == InteropLinuxSocketTerminalLocalAbort);
    guard_expect(work.event.kind == InteropPeerEventNone);
    guard_expect(fcntl(pair[0], F_GETFD) < 0 && errno == EBADF);

    memset(&work, 0xA5, sizeof(work));
    guard_expect(interop_linux_socket_get_closed_snapshot(owner, Token, &work) ==
                 InteropLinuxSocketSuccess);
    guard_expect(work.state == InteropLinuxSocketTransportClosed);
    guard_expect(work.status == InteropLinuxSocketWorkTransportClosed);
    guard_expect(work.terminal_reason == InteropLinuxSocketTerminalLocalAbort);
    guard_expect(work.event.kind == InteropPeerEventNone);
    memcpy(&before_work, &work, sizeof(work));

    memset(&poll, 0xA5, sizeof(poll));
    guard_expect(interop_linux_socket_poll_events(owner, Token + 1u, 100u, &poll) ==
                 InteropLinuxSocketWrongToken);
    guard_expect(interop_linux_socket_poll_events(owner, Token, 99u, &poll) ==
                 InteropLinuxSocketTimeWentBackwards);
    guard_expect(interop_linux_socket_poll_events(owner, Token, 100u, &poll) ==
                 InteropLinuxSocketSuccess);
    guard_expect(poll.fd == -1 && poll.events == 0u && poll.timeout_ms == 0u);
    guard_expect(poll.immediate_work == 0u && poll.poll_cookie != 0u);
    cookie = poll.poll_cookie;

    memset(frame, 0xA5, sizeof(frame));
    memcpy(frame_before, frame, sizeof(frame));
    memset(&work, 0xA5, sizeof(work));
    guard_expect(interop_linux_socket_drive(owner, Token, cookie, 100u, 0u,
                                            0u, 0u, 0u, frame, sizeof(frame), &work) ==
                 InteropLinuxSocketSuccess);
    guard_expect(work.state == InteropLinuxSocketTransportClosed);
    guard_expect(work.status == InteropLinuxSocketWorkTransportClosed);
    guard_expect(work.event.kind == InteropPeerEventNone);
    guard_expect(memcmp(frame, frame_before, sizeof(frame)) == 0);
    guard_expect(memcmp(&work, &before_work, sizeof(work)) == 0);

    guard_expect(interop_linux_socket_mapping_result(owner, Token, 99u, 1u,
                                                      InteropLayoutVersion, 1u, &event) ==
                 InteropLinuxSocketClosed);
    guard_expect(memcmp(&event, &before_event, sizeof(event)) == 0);
    guard_expect(interop_linux_socket_begin_drain(owner, Token, 100u, &event) ==
                 InteropLinuxSocketClosed);
    guard_expect(memcmp(&event, &before_event, sizeof(event)) == 0);
    guard_expect(interop_linux_socket_abort(owner, Token, &work) == InteropLinuxSocketSuccess);
    guard_expect(work.state == InteropLinuxSocketTransportClosed);
    guard_expect(work.terminal_reason == InteropLinuxSocketTerminalLocalAbort);
    guard_cleanup_owner(&owner, pair[1]);
}

/**
 * @brief Run public socket-owner guard regressions using real local sockets.
 * @return No value; any failed assertion terminates the test process.
 * @note Creates and closes all descriptors and destroys all successfully
 *       created owners. Calls from the main test thread only, except for the
 *       intentional foreign-thread affinity probe. Does not inspect opaque
 *       owner storage or claim VSOCK/native mapping coverage.
 */
void interop_linux_socket_test_guards(void)
{
    test_create_guards();
    test_owner_guard_calls();
    test_current_cookie_rejections();
    test_guest_mapping_layout_rejection();
    test_owner_thread_guard();
    test_closed_owner_guards();
}
