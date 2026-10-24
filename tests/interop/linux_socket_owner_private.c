/**
 * @file linux_socket_owner_private.c
 * @brief White-box boundary tests for private Linux socket-owner helpers.
 *
 * This test translation unit includes the exact production C source so it can
 * exercise static range and RX-compaction helpers without adding a production
 * test API. It does not replace system calls, fabricate an owner, or seed
 * private lifecycle fields other than the documented poll-cookie boundary.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../../src/interop/linux_socket_owner.c"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/** @brief Fail the test process if a private-helper contract is violated. */
static void private_expect(int condition)
{
    if (!condition)
        abort();
}

/** @brief Return a valid host configuration for an authentic socketpair owner. */
static interop_linux_socket_config_t private_config(void)
{
    static const uint8_t Version[] = "1.2.3";
    interop_linux_socket_config_t config;

    memset(&config, 0, sizeof(config));
    config.peer_config.role = InteropPeerHost;
    config.peer_config.connection_token = UINT64_C(0x51C0FFEE);
    config.peer_config.accepted_at_ms = 100u;
    config.peer_config.agent_version = Version;
    config.peer_config.agent_version_bytes = sizeof(Version) - 1u;
    config.drain_timeout_ms = 1000u;
    return config;
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
    interop_linux_socket_t owner = {0};
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
    interop_linux_socket_config_t config = private_config();
    interop_linux_socket_t *owner = NULL;
    interop_linux_socket_poll_t poll_out;
    interop_linux_socket_poll_t poll_before;
    interop_linux_socket_work_t work;
    int pair[2] = {-1, -1};

    private_expect(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                              0, pair) == 0);
    private_expect(interop_linux_socket_create(pair[0], &config, &owner) ==
                   InteropLinuxSocketSuccess);

    /* Only the scalar sequence boundary is seeded; identity and resources are authentic. */
    owner->next_poll_cookie = UINT64_MAX;
    memset(&poll_out, 0xA5, sizeof(poll_out));
    private_expect(interop_linux_socket_poll_events(owner, config.peer_config.connection_token,
                                                     100u, &poll_out) == InteropLinuxSocketSuccess);
    private_expect(poll_out.poll_cookie == UINT64_MAX);
    private_expect(owner->cookies_exhausted);

    memset(&poll_out, 0x6C, sizeof(poll_out));
    memcpy(&poll_before, &poll_out, sizeof(poll_before));
    private_expect(interop_linux_socket_poll_events(owner, config.peer_config.connection_token,
                                                     100u, &poll_out) ==
                   InteropLinuxSocketPollCookieExhausted);
    private_expect(memcmp(&poll_out, &poll_before, sizeof(poll_out)) == 0);

    memset(&work, 0xA5, sizeof(work));
    private_expect(interop_linux_socket_abort(owner, config.peer_config.connection_token,
                                               &work) == InteropLinuxSocketSuccess);
    private_expect(work.state == InteropLinuxSocketTransportClosed);
    memset(&work, 0xA5, sizeof(work));
    private_expect(interop_linux_socket_get_closed_snapshot(owner,
                                                             config.peer_config.connection_token,
                                                             &work) == InteropLinuxSocketSuccess);
    private_expect(work.terminal_reason == InteropLinuxSocketTerminalLocalAbort);
    private_expect(interop_linux_socket_destroy(&owner) == InteropLinuxSocketSuccess);
    private_expect(owner == NULL);
    private_expect(fcntl(pair[0], F_GETFD) < 0 && errno == EBADF);
    private_expect(close(pair[1]) == 0);
}

/**
 * @brief Run white-box range, compaction, and poll-cookie boundary regressions.
 * @return No value; any failed assertion terminates the test process.
 * @note The authentic socketpair owner is aborted, snapshotted, destroyed,
 *       and both descriptors are closed before return.
 */
void test_linux_socket_owner_private(void)
{
    test_private_ranges();
    test_private_compact_rx();
    test_private_cookie_exhaustion();
}
