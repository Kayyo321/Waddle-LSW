/**
 * @file session_local_handler_test.c
 * @brief Unit, socket, and fault tests for the session.sock command handler.
 *
 * This file includes the handler source so private rules are testable directly. The
 * feature sender entry points are supplied here as scripted fakes; the production runtime
 * provides the real ones in session_control_host.c.
 */
#include "../../src/interop/session_local_handler.c"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define Check(condition) assert((condition) && #condition)

static void test_init_rejects_bad_input(void)
{
    session_local_handler_t handler;
    session_local_config_t config = {
        .runtime_dir = "/run/user/1000",
        .device_name = "alpha",
        .lock_held_proof = 1,
        .reserved_zero = 0,
    };
    memset(&handler, 0xAB, sizeof(handler));
    Check(session_local_handler_init(NULL, &config) == SessionLocalInvalidArgument);
    Check(session_local_handler_init(&handler, NULL) == SessionLocalInvalidArgument);

    config.reserved_zero = 1;
    Check(session_local_handler_init(&handler, &config) == SessionLocalRefused);
    config.reserved_zero = 0;

    config.runtime_dir = "run/user/1000";
    Check(session_local_handler_init(&handler, &config) == SessionLocalInvalidArgument);
    config.runtime_dir = NULL;
    Check(session_local_handler_init(&handler, &config) == SessionLocalInvalidArgument);
    config.runtime_dir = "/run/user/1000";

    config.device_name = NULL;
    Check(session_local_handler_init(&handler, &config) == SessionLocalInvalidArgument);
    config.device_name = "";
    Check(session_local_handler_init(&handler, &config) == SessionLocalInvalidArgument);
    config.device_name = "a/b";
    Check(session_local_handler_init(&handler, &config) == SessionLocalInvalidArgument);
    config.device_name = "a b";
    Check(session_local_handler_init(&handler, &config) == SessionLocalInvalidArgument);
    config.device_name = "a.b";
    Check(session_local_handler_init(&handler, &config) == SessionLocalInvalidArgument);
    char longest[SessionLocalDeviceNameMax + 2];
    memset(longest, 'x', sizeof(longest) - 1);
    longest[sizeof(longest) - 1] = '\0';
    config.device_name = longest;
    Check(session_local_handler_init(&handler, &config) == SessionLocalInvalidArgument);
    longest[SessionLocalDeviceNameMax] = '\0';
    Check(session_local_handler_init(&handler, &config) == SessionLocalSuccess);
    config.device_name = "alpha";

    /* A runtime directory this long pushes the socket path past sun_path. */
    char deep[256];
    memset(deep, 'd', sizeof(deep) - 1);
    deep[0] = '/';
    deep[sizeof(deep) - 1] = '\0';
    config.runtime_dir = deep;
    Check(session_local_handler_init(&handler, &config) == SessionLocalInvalidArgument);

    unsigned char before[sizeof(handler)];
    memset(&handler, 0x5C, sizeof(handler));
    memcpy(before, &handler, sizeof(handler));
    config.runtime_dir = "/run/user/1000";
    config.device_name = "a-name-of-sixty-four-characters-is-one-byte-over-the-limit-for-devices-x";
    Check(session_local_handler_init(&handler, &config) == SessionLocalInvalidArgument);
    Check(memcmp(before, &handler, sizeof(handler)) == 0);
}

static void test_init_fills_paths_and_defaults(void)
{
    session_local_handler_t *handler = calloc(1, sizeof(*handler));
    Check(handler != NULL);
    session_local_config_t config = {
        .runtime_dir = "/run/user/1000",
        .device_name = "alpha-1_x",
        .lock_held_proof = 7,
        .reserved_zero = 0,
    };
    Check(session_local_handler_init(handler, &config) == SessionLocalSuccess);
    Check(strcmp(handler->directory_path, "/run/user/1000/waddle/alpha-1_x") == 0);
    Check(strcmp(handler->socket_path, "/run/user/1000/waddle/alpha-1_x/session.sock") == 0);
    Check(handler->lock_held_proof == 7);
    Check(handler->expected_uid == (uint32_t)geteuid());
    Check(handler->listen_fd == -1 && handler->epoll_fd == -1);
    Check(handler->socket_created == 0 && handler->bound == 0 && handler->stopping == 0 && handler->closed == 0);
    Check(handler->next_serial == 1);
    for (uint32_t index = 0; index < SessionLocalMaxClients; ++index) {
        Check(handler->clients[index].fd == -1);
        Check(handler->clients[index].serial == 0);
    }
    for (uint32_t index = 0; index < SessionLocalMaxPending; ++index) Check(handler->pending[index].in_use == 0);
    session_local_stats_t stats;
    session_local_handler_stats(handler, &stats);
    Check(stats.accepted == 0 && stats.launches_forwarded == 0 && stats.accept_errors == 0);
    session_local_handler_stats(NULL, &stats);
    session_local_handler_stats(handler, NULL);
    session_local_handler_destroy(NULL);
    free(handler);
}

static void test_directory_decision(void)
{
    Check(session_local_directory_decision(1, 0, 1000, 0700, 1000) == SessionLocalDirectoryUsable);
    Check(session_local_directory_decision(1, 0, 1000, 040700, 1000) == SessionLocalDirectoryUsable);
    Check(session_local_directory_decision(0, 0, 1000, 0700, 1000) == SessionLocalDirectoryRefused);
    Check(session_local_directory_decision(1, 1, 1000, 0700, 1000) == SessionLocalDirectoryRefused);
    Check(session_local_directory_decision(1, 0, 0, 0700, 1000) == SessionLocalDirectoryRefused);
    Check(session_local_directory_decision(1, 0, 1000, 0755, 1000) == SessionLocalDirectoryRefused);
    Check(session_local_directory_decision(1, 0, 1000, 0500, 1000) == SessionLocalDirectoryRefused);
    Check(session_local_directory_decision(1, 0, 1000, 0710, 1000) == SessionLocalDirectoryRefused);
}

static void test_stale_and_peer_decisions(void)
{
    Check(session_local_stale_decision(ECONNREFUSED, 1) == SessionLocalStaleUnlink);
    Check(session_local_stale_decision(ECONNREFUSED, 0) == SessionLocalStaleRefuse);
    Check(session_local_stale_decision(0, 1) == SessionLocalStaleRefuse);
    Check(session_local_stale_decision(EAGAIN, 1) == SessionLocalStaleRefuse);
    Check(session_local_stale_decision(EINPROGRESS, 1) == SessionLocalStaleRefuse);
    Check(session_local_stale_decision(EACCES, 1) == SessionLocalStaleRefuse);

    Check(session_local_peer_decision(1000, 1000) == SessionLocalPeerAllowed);
    Check(session_local_peer_decision(0, 1000) == SessionLocalPeerRefused);
    Check(session_local_peer_decision(1001, 1000) == SessionLocalPeerRefused);
}

static void test_send_outcome_mapping(void)
{
    Check(session_local_send_outcome(HostFeatureSuccess) == SessionLocalOutcomeAccepted);
    Check(session_local_send_outcome(HostFeatureQueueFull) == SessionLocalOutcomeBusy);
    Check(session_local_send_outcome(HostFeatureRequestIdsFull) == SessionLocalOutcomeBusy);
    Check(session_local_send_outcome(HostFeatureNotNegotiated) == SessionLocalOutcomeUnsupported);
    Check(session_local_send_outcome(HostFeatureDraining) == SessionLocalOutcomeCancelled);
    Check(session_local_send_outcome(HostFeatureNotReady) == SessionLocalOutcomeError);
    Check(session_local_send_outcome(HostFeatureInvalidFrame) == SessionLocalOutcomeError);
    Check(session_local_send_outcome(HostFeatureUnsupportedFrame) == SessionLocalOutcomeError);
    Check(session_local_send_outcome(HostFeatureOutOfMemory) == SessionLocalOutcomeError);
    Check(session_local_send_outcome(HostFeatureClosed) == SessionLocalOutcomeError);
    Check(session_local_send_outcome(HostFeatureTransportFailed) == SessionLocalOutcomeError);
    Check(session_local_send_outcome(99) == SessionLocalOutcomeError);
}

int main(void)
{
    test_init_rejects_bad_input();
    test_init_fills_paths_and_defaults();
    test_directory_decision();
    test_stale_and_peer_decisions();
    test_send_outcome_mapping();
    printf("session_local_handler_test: pure rules passed\n");
    return 0;
}
