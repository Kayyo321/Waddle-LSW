/**
 * @file windows_socket_owner_private.c
 * @brief White-box boundary tests for private Windows socket-owner helpers.
 *
 * This test translation unit includes the exact production C source so it can
 * exercise static range and RX-compaction helpers without adding a production
 * test API. It does not replace system calls, fabricate an owner, or seed
 * private lifecycle fields other than the documented poll-cookie boundary.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../../src/interop/windows_socket_owner.c"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

extern void windows_socket_owner_test_set_get_state_phase(int phase);

/** @brief Fail the test process if a private-helper contract is violated. */
static void private_expect_at(int condition, int line)
{
    if (!condition) {
        fprintf(stderr, "windows_socket_owner_private: assertion at line %d\n", line);
        abort();
    }
}
#define private_expect(condition) private_expect_at((condition), __LINE__)

/** @brief Return a valid host configuration for an authentic socketpair owner. */
static interop_windows_socket_config_t private_config(void)
{
    static const uint8_t Version[] = "1.2.3";
    interop_windows_socket_config_t config;

    memset(&config, 0, sizeof(config));
    config.peer_config.role = InteropPeerHost;
    config.peer_config.connection_token = UINT64_C(0x51C0FFEE);
    config.peer_config.accepted_at_ms = 100u;
    config.peer_config.agent_version = Version;
    config.peer_config.agent_version_bytes = sizeof(Version) - 1u;
    config.drain_timeout_ms = 1000u;
    return config;
}

/** @brief Encode one valid Hello for the host peer's pending-frame path. */
static size_t private_make_hello(uint8_t *frame, size_t capacity)
{
    static const uint8_t Version[] = "1.2.3";
    interop_writer_t writer;
    size_t frame_length = 0;

    interop_writer_init(&writer, frame, capacity);
    private_expect(interop_put_uint(&writer, InteropTagCapabilities,
                                   InteropCapWindows | InteropCapInput) == InteropErrorNone);
    private_expect(interop_put_bytes(&writer, InteropTagAgentVersion, Version,
                                    sizeof(Version) - 1u) == InteropErrorNone);
    private_expect(interop_put_uint(&writer, InteropTagLayoutVersion,
                                   InteropLayoutVersion) == InteropErrorNone);
    private_expect(interop_put_uint(&writer, InteropTagSessionEpoch,
                                   UINT64_C(0x1020304050607080)) == InteropErrorNone);
    private_expect(interop_writer_finish(&writer, InteropMsgHello, 1u,
                    InteropFromGuest, &frame_length) == InteropErrorNone);
    return frame_length;
}

/** @brief Exercise private address-range validation at real boundaries. */
static void test_private_ranges(void)
{
    _Alignas(uint64_t) uint8_t bytes[32] = {0};
    uintptr_t wrapping_address = UINTPTR_MAX - 3u;

    private_expect(!range_valid(NULL, 0u, 1u));
    private_expect(!range_valid(bytes, 0u, 0u));
    private_expect(range_valid(bytes, 0u, 1u));
    private_expect(range_valid(bytes, sizeof(bytes), 1u));
    private_expect(range_valid(bytes, sizeof(uint64_t), _Alignof(uint64_t)));
    private_expect(!range_valid(bytes + 1u, sizeof(uint64_t), _Alignof(uint64_t)));
    private_expect(!range_valid((const void *)wrapping_address, 4u, 1u));
    private_expect(range_valid((const void *)wrapping_address, 3u, 1u));

    private_expect(!ranges_overlap(bytes, 0u, bytes, sizeof(bytes)));
    private_expect(!ranges_overlap(bytes, sizeof(bytes), bytes, 0u));
    private_expect(!ranges_overlap(bytes, 8u, bytes + 8u, 8u));
    private_expect(ranges_overlap(bytes, 9u, bytes + 8u, 8u));
    private_expect(ranges_overlap(bytes + 8u, 8u, bytes, 9u));
}

/** @brief Verify bounded RX compaction for live and exhausted suffixes. */
static void test_private_compact_rx(void)
{
    interop_windows_socket_t owner = {0};
    static const uint8_t Live[] = {0x11u, 0x22u, 0x33u, 0x44u};

    owner.rx_start = 7u;
    owner.rx_length = sizeof(Live);
    memcpy(owner.rx_carry + owner.rx_start, Live, sizeof(Live));
    compact_rx(&owner);
    private_expect(owner.rx_start == 0u);
    private_expect(owner.rx_length == sizeof(Live));
    private_expect(memcmp(owner.rx_carry, Live, sizeof(Live)) == 0);

    owner.rx_start = sizeof(owner.rx_carry);
    owner.rx_length = 0u;
    compact_rx(&owner);
    private_expect(owner.rx_start == 0u);
    private_expect(owner.rx_length == 0u);
}

/** @brief Exercise the authentic owner's final poll-cookie values and cleanup. */
static void test_private_cookie_exhaustion(void)
{
    interop_windows_socket_config_t config = private_config();
    interop_windows_socket_t *owner = NULL;
    interop_windows_socket_poll_t poll_out;
    interop_windows_socket_poll_t poll_before;
    interop_windows_socket_work_t work;
    int pair[2] = {-1, -1};

    private_expect(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                              0, pair) == 0);
    private_expect(interop_windows_socket_create(pair[0], &config, &owner) ==
                   InteropWindowsSocketSuccess);

    /* Only the scalar sequence boundary is seeded; identity and resources are authentic. */
    owner->next_poll_cookie = UINT64_MAX;
    memset(&poll_out, 0xA5, sizeof(poll_out));
    private_expect(interop_windows_socket_poll_events(owner, config.peer_config.connection_token,
                                                     100u, &poll_out) == InteropWindowsSocketSuccess);
    private_expect(poll_out.poll_cookie == UINT64_MAX);
    private_expect(owner->cookies_exhausted);

    owner->cookies_exhausted = false;
    owner->next_poll_cookie = 0u;
    private_expect(interop_windows_socket_poll_events(owner, config.peer_config.connection_token,
                                                      100u, &poll_out) ==
                   InteropWindowsSocketPollCookieExhausted);
    private_expect(owner->cookies_exhausted);

    memset(&poll_out, 0x6C, sizeof(poll_out));
    memcpy(&poll_before, &poll_out, sizeof(poll_before));
    private_expect(interop_windows_socket_poll_events(owner, config.peer_config.connection_token,
                                                     100u, &poll_out) ==
                   InteropWindowsSocketPollCookieExhausted);
    private_expect(memcmp(&poll_out, &poll_before, sizeof(poll_out)) == 0);

    memset(&work, 0xA5, sizeof(work));
    private_expect(interop_windows_socket_abort(owner, config.peer_config.connection_token,
                                               &work) == InteropWindowsSocketSuccess);
    private_expect(work.state == InteropWindowsSocketTransportClosed);
    memset(&work, 0xA5, sizeof(work));
    {
        interop_windows_socket_work_t before;
        memcpy(&before, &work, sizeof(before));
        private_expect(interop_windows_socket_get_closed_snapshot(owner,
                        config.peer_config.connection_token + 1u, &work) ==
                       InteropWindowsSocketWrongToken);
        private_expect(memcmp(&work, &before, sizeof(work)) == 0);
    }
    private_expect(interop_windows_socket_get_closed_snapshot(owner,
                                                             config.peer_config.connection_token,
                                                             &work) == InteropWindowsSocketSuccess);
    private_expect(work.terminal_reason == InteropWindowsSocketTerminalLocalAbort);
    private_expect(interop_windows_socket_destroy(&owner) == InteropWindowsSocketSuccess);
    private_expect(owner == NULL);
    private_expect(fcntl(pair[0], F_GETFD) < 0 && errno == EBADF);
    private_expect(close(pair[1]) == 0);
}

/** @brief Defer then finish native-close notification without replaying events. */
static void test_private_deferred_transport_notification(void)
{
    interop_windows_socket_config_t config = private_config();
    interop_windows_socket_t *owner = NULL;
    interop_windows_socket_poll_t poll_out;
    interop_windows_socket_work_t work;
    int pair[2] = {-1, -1};

    private_expect(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                              0, pair) == 0);
    private_expect(interop_windows_socket_create(pair[0], &config, &owner) ==
                   InteropWindowsSocketSuccess);
    memset(&work, 0, sizeof(work));
    work.internal_steps = InteropWindowsSocketMaxInternalStepsPerDrive;
    close_transport(owner, InteropWindowsSocketTerminalLocalAbort, 0, &work);
    private_expect(owner->state == InteropWindowsSocketTransportClosed);
    private_expect(owner->notify_deferred && !owner->transport_notified);
    close_transport(owner, InteropWindowsSocketTerminalDestroyed, 0, &work);
    private_expect(owner->terminal_reason == InteropWindowsSocketTerminalLocalAbort);
    private_expect(interop_windows_socket_poll_events(owner, config.peer_config.connection_token,
                                                      100, &poll_out) ==
                   InteropWindowsSocketSuccess);
    private_expect(!owner->notify_deferred && owner->transport_notified);
    private_expect(interop_windows_socket_drive(owner, config.peer_config.connection_token,
                                                poll_out.poll_cookie, 100, 0, 0, 0, 0,
                                                (uint8_t[InteropWindowsSocketFrameCapacity]){0},
                                                InteropWindowsSocketFrameCapacity, &work) ==
                   InteropWindowsSocketSuccess);
    private_expect(work.event.kind == InteropPeerEventNone);
    private_expect(interop_windows_socket_destroy(&owner) == InteropWindowsSocketSuccess);
    private_expect(owner == NULL && close(pair[1]) == 0);
}

/** @brief Exercise the pure phase guard with an authentic resource owner. */
static void test_private_phase_and_terminal_event_guards(void)
{
    static const uint32_t ValidPhases[] = {
        InteropPeerAwaitHello,
        InteropPeerAwaitMapping,
        InteropPeerAwaitHelloAckSent,
        InteropPeerInitializingMapping,
        InteropPeerAwaitHelloSent,
        InteropPeerAwaitHelloAck,
        InteropPeerReady,
        InteropPeerDraining
    };
    interop_windows_socket_config_t config = private_config();
    interop_windows_socket_t *owner = NULL;
    interop_peer_event_t event;
    int pair[2] = {-1, -1};
    size_t index;

    private_expect(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                              0, pair) == 0);
    private_expect(interop_windows_socket_create(pair[0], &config, &owner) ==
                   InteropWindowsSocketSuccess);
    clear_event(&event);
    event.kind = InteropPeerEventDraining;
    event.phase = InteropPeerDraining;
    save_terminal_event(owner, &event);
    private_expect(owner->terminal_event.kind == InteropPeerEventDraining);
    event.kind = InteropPeerEventNone;
    save_terminal_event(owner, &event);
    private_expect(owner->terminal_event.kind == InteropPeerEventDraining);

    for (index = 0; index < sizeof(ValidPhases) / sizeof(ValidPhases[0]); ++index)
        private_expect(observe_phase(owner, ValidPhases[index], 100 + index, NULL));
    private_expect(!observe_phase(owner, UINT32_MAX, 200, NULL));
    private_expect(interop_windows_socket_destroy(&owner) == InteropWindowsSocketSuccess);
    private_expect(owner == NULL && close(pair[1]) == 0);
}

/** @brief Cover private status, notification, and event-gate branches. */
static void test_private_helper_branch_boundaries(void)
{
    interop_windows_socket_config_t config = private_config();
    interop_windows_socket_t *owner = NULL;
    interop_windows_socket_work_t work;
    interop_peer_event_t event;
    uint8_t hello[256];
    size_t hello_length;
    size_t consumed = 0;
    int pair[2] = {-1, -1};

    private_expect(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                              0, pair) == 0);
    private_expect(interop_windows_socket_create(pair[0], &config, &owner) ==
                   InteropWindowsSocketSuccess);

    /* A framed Hello reaches the peer assembler before outward dispatch. */
    hello_length = private_make_hello(hello, sizeof(hello));
    clear_event(&event);
    private_expect(interop_peer_feed(owner->peer, config.peer_config.connection_token,
                                     hello, hello_length, &consumed, &event) == InteropPeerSuccess);
    private_expect(consumed == hello_length);
    memset(&work, 0, sizeof(work));
    {
        bool frame_consumed = false;
        private_expect(service_carry(owner, &work,
                    (uint8_t[InteropWindowsSocketFrameCapacity]){0}, 100u,
                    &frame_consumed) == InteropWindowsSocketSuccess);
        private_expect(frame_consumed && work.event.kind == InteropPeerEventValidateMapping);
    }
    owner->state = InteropWindowsSocketActive;
    owner->mapping_callback_pending = false;

    memset(&work, 0, sizeof(work));
    private_expect(valid_work_output(owner, &work));
    private_expect(!valid_work_output(owner, NULL));
    private_expect(!valid_work_output(owner,
                    (interop_windows_socket_work_t *)(void *)owner));

    owner->peer_notify_failed = true;
    private_expect(close_work_status(owner) == InteropWindowsSocketWorkPeerFailure);
    owner->peer_notify_failed = false;
    owner->state = InteropWindowsSocketTransportClosed;
    private_expect(close_work_status(owner) == InteropWindowsSocketWorkTransportClosed);
    owner->state = InteropWindowsSocketActive;
    owner->transport_notified = false;
    notify_transport_lost(owner, NULL);
    private_expect(!owner->transport_notified);
    owner->state = InteropWindowsSocketClosePendingState;
    private_expect(close_work_status(owner) == InteropWindowsSocketWorkClosePending);
    owner->state = InteropWindowsSocketCloseIndeterminateState;
    private_expect(close_work_status(owner) == InteropWindowsSocketWorkCloseIndeterminate);
    owner->state = InteropWindowsSocketActive;
    private_expect(close_work_status(owner) == InteropWindowsSocketWorkIdle);

    clear_event(&event);
    event.kind = InteropPeerEventNone;
    event.phase = InteropPeerReady;
    save_terminal_event(owner, &event);
    private_expect(owner->terminal_event.kind == InteropPeerEventNone);
    private_expect(observe_phase(owner, InteropPeerReady, 100u, &event));
    event.kind = InteropPeerEventDraining;
    event.phase = InteropPeerReady;
    save_terminal_event(owner, &event);
    private_expect(owner->terminal_event.kind == InteropPeerEventDraining);
    private_expect(observe_phase(owner, InteropPeerReady, 100u, &event));
    event.kind = InteropPeerEventDraining;
    event.phase = InteropPeerDraining;
    private_expect(observe_phase(owner, InteropPeerDraining, 101u, &event));
    private_expect(owner->terminal_event.kind == InteropPeerEventDraining);

    /* A full budget defers notification; an unconstrained call completes it. */
    owner->state = InteropWindowsSocketTransportClosed;
    owner->closed = true;
    owner->peer_notify_failed = true;
    notify_transport_lost(owner, NULL);
    private_expect(!owner->transport_notified);
    owner->peer_notify_failed = false;
    {
        interop_peer_t *saved_peer = owner->peer;
        owner->peer = NULL;
        notify_transport_lost(owner, NULL);
        owner->peer = saved_peer;
    }
    private_expect(!owner->transport_notified);
    work.internal_steps = InteropWindowsSocketMaxInternalStepsPerDrive;
    notify_transport_lost(owner, &work);
    private_expect(owner->notify_deferred && !owner->transport_notified);
    notify_transport_lost(owner, NULL);
    private_expect(!owner->notify_deferred && owner->transport_notified);
    notify_transport_lost(owner, NULL);
    private_expect(owner->transport_notified);

    /* Exercise the event copy suppression while genuinely draining. */
    owner->state = InteropWindowsSocketDraining;
    memset(&work, 0, sizeof(work));
    private_expect(accept_peer_event(owner, &event, &work, 102u));
    private_expect(work.event.kind == InteropPeerEventNone);
    event.kind = InteropPeerEventDraining;
    event.phase = InteropPeerReady;
    private_expect(accept_peer_event(owner, &event, &work, 102u));
    private_expect(work.event.kind == InteropPeerEventDraining);
    event.kind = InteropPeerEventReady;
    event.phase = InteropPeerReady;
    private_expect(accept_peer_event(owner, &event, &work, 103u));
    private_expect(work.event.kind == InteropPeerEventReady);

    /* Exhausted helper budget with staged RX must stop before peer feed. */
    owner->rx_length = 1u;
    work.internal_steps = InteropWindowsSocketMaxInternalStepsPerDrive - 1u;
    {
        bool frame_consumed = false;
        private_expect(service_carry(owner, &work,
                    (uint8_t[InteropWindowsSocketFrameCapacity]){0}, 104u,
                    &frame_consumed) == InternalWorkExhausted);
        private_expect(!frame_consumed);
    }
    owner->rx_length = 0u;
    owner->rx_start = 0u;

    /* Closed drive snapshots still consume cookies and report stable status. */
    owner->closed = true;
    owner->state = InteropWindowsSocketCloseIndeterminateState;
    owner->cookie_current = true;
    owner->current_poll_cookie = 77u;
    memset(&work, 0, sizeof(work));
    private_expect(interop_windows_socket_drive(owner, config.peer_config.connection_token,
                77u, 104u, 0u, 0u, 0u, 0u,
                (uint8_t[InteropWindowsSocketFrameCapacity]){0},
                InteropWindowsSocketFrameCapacity, &work) == InteropWindowsSocketSuccess);
    private_expect(work.status == InteropWindowsSocketWorkCloseIndeterminate);
    owner->state = InteropWindowsSocketTransportClosed;
    owner->peer_notify_failed = true;
    owner->cookie_current = true;
    owner->current_poll_cookie = 78u;
    private_expect(interop_windows_socket_drive(owner, config.peer_config.connection_token,
                78u, 105u, 0u, 0u, 0u, 0u,
                (uint8_t[InteropWindowsSocketFrameCapacity]){0},
                InteropWindowsSocketFrameCapacity, &work) == InteropWindowsSocketSuccess);
    private_expect(work.status == InteropWindowsSocketWorkPeerFailure);
    owner->peer_notify_failed = false;

    /* Restore the live transport after scalar-only notification probes. */
    owner->closed = false;
    owner->state = InteropWindowsSocketActive;
    /* Terminalize the authentic socket and preserve its primary OS error. */
    memset(&work, 0, sizeof(work));
    close_transport(owner, InteropWindowsSocketTerminalSocketError, WSAECONNRESET, &work);
    private_expect(owner->state == InteropWindowsSocketTransportClosed);
    private_expect(owner->terminal_reason == InteropWindowsSocketTerminalSocketError);
    private_expect(owner->terminal_wsa_error == WSAECONNRESET);
    {
        int extra_pair[2] = {-1, -1};
        private_expect(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                                  0, extra_pair) == 0);
        owner->socket_value = extra_pair[0];
        owner->closed = false;
        close_transport(owner, InteropWindowsSocketTerminalDestroyed, 0u, &work);
        private_expect(owner->terminal_reason == InteropWindowsSocketTerminalSocketError);
        private_expect(owner->terminal_wsa_error == WSAECONNRESET);
        owner->socket_value = INVALID_SOCKET;
        owner->closed = true;
        owner->state = InteropWindowsSocketTransportClosed;
        private_expect(close(extra_pair[1]) == 0);
    }
    private_expect(interop_windows_socket_destroy(&owner) == InteropWindowsSocketSuccess);
    private_expect(owner == NULL && close(pair[1]) == 0);
}

/** @brief Cover guest queued-output polling and a shortened drain wait. */
static void test_private_guest_poll_and_drain_deadline(void)
{
    interop_windows_socket_config_t config = private_config();
    interop_windows_socket_t *owner = NULL;
    interop_windows_socket_poll_t poll_out;
    interop_peer_event_t event;
    int pair[2] = {-1, -1};

    config.peer_config.role = InteropPeerGuest;
    config.peer_config.connection_token = UINT64_C(0x51C0FFEF);
    private_expect(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                              0, pair) == 0);
    private_expect(interop_windows_socket_create(pair[0], &config, &owner) ==
                   InteropWindowsSocketSuccess);
    private_expect(interop_windows_socket_mapping_result(owner,
                   config.peer_config.connection_token, 100u, 0x1020304050607080u,
                   InteropLayoutVersion, 1u, &event) == InteropWindowsSocketSuccess);
    private_expect(interop_windows_socket_poll_events(owner,
                   config.peer_config.connection_token, 100u, &poll_out) ==
                   InteropWindowsSocketSuccess);
    private_expect((poll_out.events & (uint32_t)POLLWRNORM) != 0u);

    /* Force the peer-state adapter to cover queued output while mapping waits. */
    windows_socket_owner_test_set_get_state_phase(InteropPeerInitializingMapping);
    private_expect(interop_windows_socket_poll_events(owner,
                   config.peer_config.connection_token, 101u, &poll_out) ==
                   InteropWindowsSocketSuccess);
    private_expect((poll_out.events & (uint32_t)POLLWRNORM) == 0u);
    private_expect(interop_windows_socket_destroy(&owner) == InteropWindowsSocketSuccess);
    private_expect(owner == NULL && close(pair[1]) == 0);
}

/**
 * @brief Run white-box range, compaction, and poll-cookie boundary regressions.
 * @return No value; any failed assertion terminates the test process.
 * @note The authentic socketpair owner is aborted, snapshotted, destroyed,
 *       and both descriptors are closed before return.
 */
void test_windows_socket_owner_private(void)
{
    test_private_ranges();
    test_private_compact_rx();
    test_private_cookie_exhaustion();
    test_private_deferred_transport_notification();
    test_private_phase_and_terminal_event_guards();
    test_private_helper_branch_boundaries();
    test_private_guest_poll_and_drain_deadline();
}
