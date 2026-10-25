/**
 * @file session_local_handler.c
 * @brief Implementation of the session.sock command handler (see session_local_handler.h).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "session_local_handler.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Largest runtime directory string accepted by init; far above any real XDG value. */
enum { SessionLocalRuntimeDirMax = 4096 };

/** @brief Device name grammar: one or more of [A-Za-z0-9_-], at most SessionLocalDeviceNameMax bytes. */
static int device_name_valid(const char *name)
{
    size_t length = 0;
    if (name == NULL) return 0;
    for (; name[length] != '\0'; ++length) {
        char byte = name[length];
        int alpha = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
        int digit = byte >= '0' && byte <= '9';
        if (!alpha && !digit && byte != '_' && byte != '-') return 0;
        if (length >= SessionLocalDeviceNameMax) return 0;
    }
    return length != 0;
}

int session_local_handler_init(session_local_handler_t *handler, const session_local_config_t *config)
{
    char directory[SessionLocalSocketPathBytes];
    char socket[SessionLocalSocketPathBytes];
    size_t runtime_length = 0;
    int written_directory;
    int written_socket;

    if (handler == NULL || config == NULL) return SessionLocalInvalidArgument;
    if (config->reserved_zero != 0) return SessionLocalRefused;
    if (config->runtime_dir == NULL || config->runtime_dir[0] != '/') return SessionLocalInvalidArgument;
    while (config->runtime_dir[runtime_length] != '\0') {
        if (++runtime_length > SessionLocalRuntimeDirMax) return SessionLocalInvalidArgument;
    }
    if (!device_name_valid(config->device_name)) return SessionLocalInvalidArgument;

    /* Paths are composed before any handler byte is written, so failure leaves the handler unchanged. */
    written_directory = snprintf(directory, sizeof(directory), "%s/waddle/%s",
        config->runtime_dir, config->device_name);
    if (written_directory < 0 || (size_t)written_directory >= sizeof(directory)) return SessionLocalInvalidArgument;
    written_socket = snprintf(socket, sizeof(socket), "%s/session.sock", directory);
    if (written_socket < 0 || (size_t)written_socket >= sizeof(socket)) return SessionLocalInvalidArgument;

    memset(handler, 0, sizeof(*handler));
    memcpy(handler->directory_path, directory, (size_t)written_directory + 1u);
    memcpy(handler->socket_path, socket, (size_t)written_socket + 1u);
    handler->lock_held_proof = config->lock_held_proof;
    handler->expected_uid = (uint32_t)geteuid();
    handler->listen_fd = -1;
    handler->epoll_fd = -1;
    handler->next_serial = 1;
    for (uint32_t index = 0; index < SessionLocalMaxClients; ++index) handler->clients[index].fd = -1;
    if (interop_activation_store_init(&handler->tokens) != 0) return SessionLocalInvalidArgument;
    return SessionLocalSuccess;
}

void session_local_handler_stats(const session_local_handler_t *handler, session_local_stats_t *out_stats)
{
    if (handler == NULL || out_stats == NULL) return;
    *out_stats = handler->stats;
}

session_local_directory_decision_t session_local_directory_decision(int is_directory, int is_symlink,
    uint32_t owner_uid, uint32_t mode, uint32_t expected_uid)
{
    if (!is_directory || is_symlink) return SessionLocalDirectoryRefused;
    if (owner_uid != expected_uid) return SessionLocalDirectoryRefused;
    if ((mode & 07777u) != 0700u) return SessionLocalDirectoryRefused;
    return SessionLocalDirectoryUsable;
}

session_local_stale_decision_t session_local_stale_decision(int probe_errno, uint32_t lock_held_proof)
{
    if (probe_errno == ECONNREFUSED && lock_held_proof != 0) return SessionLocalStaleUnlink;
    return SessionLocalStaleRefuse;
}

session_local_peer_decision_t session_local_peer_decision(uint32_t peer_uid, uint32_t expected_uid)
{
    return peer_uid == expected_uid ? SessionLocalPeerAllowed : SessionLocalPeerRefused;
}

session_local_send_outcome_t session_local_send_outcome(int send_result)
{
    switch (send_result) {
    case HostFeatureSuccess:
        return SessionLocalOutcomeAccepted;
    case HostFeatureQueueFull:
    case HostFeatureRequestIdsFull:
        return SessionLocalOutcomeBusy;
    case HostFeatureNotNegotiated:
        return SessionLocalOutcomeUnsupported;
    case HostFeatureDraining:
        return SessionLocalOutcomeCancelled;
    default:
        return SessionLocalOutcomeError;
    }
}

/** @brief Close every descriptor the handler owns and remove the socket file it created. */
static void release_descriptors(session_local_handler_t *handler)
{
    for (uint32_t index = 0; index < SessionLocalMaxClients; ++index) {
        session_local_client_t *client = &handler->clients[index];
        if (client->fd >= 0) (void)close(client->fd);
        client->fd = -1;
        client->serial = 0;
    }
    if (handler->listen_fd >= 0) (void)close(handler->listen_fd);
    handler->listen_fd = -1;
    if (handler->epoll_fd >= 0) (void)close(handler->epoll_fd);
    handler->epoll_fd = -1;
    if (handler->socket_created) (void)unlink(handler->socket_path);
    handler->socket_created = 0;
    handler->bound = 0;
}

void session_local_handler_destroy(session_local_handler_t *handler)
{
    if (handler == NULL) return;
    release_descriptors(handler);
    handler->closed = 1;
}
