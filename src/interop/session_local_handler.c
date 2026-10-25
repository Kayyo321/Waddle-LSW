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
#include <sys/epoll.h>
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

/* ---- Reply construction -------------------------------------------------------------------- */

/** @brief Find the live correlation entry for a guest identifier, or NULL. */
static session_local_pending_t *pending_find(session_local_handler_t *handler, uint32_t guest_request_id)
{
    for (uint32_t index = 0; index < SessionLocalMaxPending; ++index) {
        session_local_pending_t *entry = &handler->pending[index];
        if (entry->in_use && entry->guest_request_id == guest_request_id) return entry;
    }
    return NULL;
}

/** @brief Find a free correlation slot, or NULL when the table is full. */
static session_local_pending_t *pending_free_slot(session_local_handler_t *handler)
{
    for (uint32_t index = 0; index < SessionLocalMaxPending; ++index) {
        if (!handler->pending[index].in_use) return &handler->pending[index];
    }
    return NULL;
}

/** @brief Count live launches owned by one client occupancy (slot index plus serial). */
static uint32_t pending_outstanding(const session_local_handler_t *handler, uint32_t client_index, uint64_t serial)
{
    uint32_t count = 0;
    for (uint32_t index = 0; index < SessionLocalMaxPending; ++index) {
        const session_local_pending_t *entry = &handler->pending[index];
        if (entry->in_use && entry->client_index == client_index && entry->client_serial == serial) ++count;
    }
    return count;
}

/**
 * @brief Obtain the transmit space of one attached client, compacting sent bytes first.
 * @return 1 with frame and room set, or 0 when the client is no longer the occupant (counted as dropped).
 */
static int reply_target(session_local_handler_t *handler, uint32_t client_index, uint64_t serial,
    uint8_t **frame, size_t *room)
{
    session_local_client_t *client = &handler->clients[client_index];
    if (client->fd < 0 || client->serial != serial) {
        handler->stats.replies_dropped++;
        return 0;
    }
    if (client->tx_offset != 0) {
        memmove(client->tx, client->tx + client->tx_offset, client->tx_length - client->tx_offset);
        client->tx_length -= client->tx_offset;
        client->tx_offset = 0;
    }
    *frame = client->tx + client->tx_length;
    *room = SessionLocalTxBytes - client->tx_length;
    return 1;
}

/** @brief Close one client slot and mark it free. Idempotent; never frees memory. */
static void client_close(session_local_handler_t *handler, uint32_t client_index)
{
    session_local_client_t *client = &handler->clients[client_index];
    if (client->fd < 0) return;
    if (handler->epoll_fd >= 0) (void)epoll_ctl(handler->epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
    (void)close(client->fd);
    client->fd = -1;
    client->serial = 0;
    client->rx_length = 0;
    client->tx_length = 0;
    client->tx_offset = 0;
}

/**
 * @brief Commit a reply built into client space, or close the client when building failed.
 * @return Nonzero when the frame was appended.
 */
static int reply_commit(session_local_handler_t *handler, uint32_t client_index, uint64_t serial,
    int built, size_t length)
{
    session_local_client_t *client = &handler->clients[client_index];
    if (!built) {
        handler->stats.replies_dropped++;
        handler->stats.closed_io++;
        client_close(handler, client_index);
        return 0;
    }
    if (client->serial != serial) return 0;
    client->tx_length += length;
    return 1;
}

/** @brief Queue LaunchResult (guest direction) to a client; process_id zero means none. */
static int reply_launch_result(session_local_handler_t *handler, uint32_t client_index, uint64_t serial,
    uint32_t request_id, uint32_t status, uint32_t process_id, const uint8_t *message, uint16_t message_length)
{
    uint8_t *frame = NULL;
    size_t room = 0;
    interop_writer_t writer;
    size_t length = 0;
    int built;
    if (!reply_target(handler, client_index, serial, &frame, &room)) return 0;
    interop_writer_init(&writer, frame, room);
    built = interop_put_uint(&writer, InteropTagProcessId, process_id) == InteropErrorNone &&
        interop_put_uint(&writer, InteropTagStatus, status) == InteropErrorNone &&
        (message == NULL || interop_put_bytes(&writer, InteropTagMessage, message, message_length) == InteropErrorNone) &&
        interop_writer_finish(&writer, InteropMsgLaunchResult, request_id, InteropFromGuest, &length) == InteropErrorNone;
    return reply_commit(handler, client_index, serial, built, length);
}

/** @brief Queue Error (host direction) with a message to a client. */
static int reply_error(session_local_handler_t *handler, uint32_t client_index, uint64_t serial,
    uint32_t request_id, uint32_t error_code, const char *message)
{
    uint8_t *frame = NULL;
    size_t room = 0;
    interop_writer_t writer;
    size_t length = 0;
    int built;
    if (!reply_target(handler, client_index, serial, &frame, &room)) return 0;
    interop_writer_init(&writer, frame, room);
    built = (message == NULL || interop_put_bytes(&writer, InteropTagMessage, message, (uint16_t)strlen(message)) == InteropErrorNone) &&
        interop_put_uint(&writer, InteropTagErrorCode, error_code) == InteropErrorNone &&
        interop_writer_finish(&writer, InteropMsgError, request_id, InteropFromHost, &length) == InteropErrorNone;
    return reply_commit(handler, client_index, serial, built, length);
}

/** @brief Queue LocalStopResult with status Ok (local direction) and the echoed request id. */
static int reply_stop_ok(session_local_handler_t *handler, uint32_t client_index, uint64_t serial, uint32_t request_id)
{
    uint8_t *frame = NULL;
    size_t room = 0;
    interop_writer_t writer;
    size_t length = 0;
    int built;
    if (!reply_target(handler, client_index, serial, &frame, &room)) return 0;
    interop_writer_init(&writer, frame, room);
    built = interop_put_uint(&writer, InteropTagStatus, InteropOk) == InteropErrorNone &&
        interop_writer_finish(&writer, InteropMsgLocalStopResult, request_id, InteropFromLocal, &length) == InteropErrorNone;
    return reply_commit(handler, client_index, serial, built, length);
}

/* ---- Launch forwarding and correlation ----------------------------------------------------- */

/**
 * @brief Build LaunchRequest (type 0x0700, host direction, identifier zero) from a validated LocalLaunch.
 * @param[in] payload Validated LocalLaunch payload, borrowed.
 * @param[in] payload_length Payload bytes.
 * @param[out] out_length Frame size on success.
 * @return 0 on success, -1 when a field cannot be copied (the caller replies Error).
 */
static int build_launch_request(session_local_handler_t *handler, const uint8_t *payload,
    size_t payload_length, size_t *out_length)
{
    /* Ascending tag order: app_key 0x08, target_path 0x2F, arguments 0x30, working_dir 0x31,
     * launch_kind 0x36, activation_token 0x37. */
    static const uint16_t copied_tags[] = {
        InteropTagAppKey, InteropTagTargetPath, InteropTagArguments, InteropTagWorkingDir,
    };
    interop_writer_t writer;
    const uint8_t *value = NULL;
    uint16_t value_length = 0;
    uint64_t launch_kind = 0;
    interop_writer_init(&writer, handler->scratch, sizeof(handler->scratch));
    for (size_t index = 0; index < sizeof(copied_tags) / sizeof(copied_tags[0]); ++index) {
        int found = interop_field_get(payload, payload_length, copied_tags[index], 0, &value, &value_length);
        if (found < 0) return -1;
        if (found == 0 && interop_put_bytes(&writer, copied_tags[index], value, value_length) != InteropErrorNone) return -1;
    }
    if (interop_field_uint(payload, payload_length, InteropTagLaunchKind, 1, &launch_kind) != 0) return -1;
    if (interop_put_uint(&writer, InteropTagLaunchKind, launch_kind) != InteropErrorNone) return -1;
    int found = interop_field_get(payload, payload_length, InteropTagActivationToken, 0, &value, &value_length);
    if (found < 0) return -1;
    if (found == 0 && interop_put_bytes(&writer, InteropTagActivationToken, value, value_length) != InteropErrorNone) return -1;
    return interop_writer_finish(&writer, InteropMsgLaunchRequest, 0, InteropFromHost, out_length) == InteropErrorNone ? 0 : -1;
}

/** @brief Map a non-accepted send outcome to the client reply (contract section 8.1). */
static void reply_refused_launch(session_local_handler_t *handler, uint32_t client_index, uint64_t serial,
    uint32_t local_request_id, session_local_send_outcome_t outcome)
{
    switch (outcome) {
    case SessionLocalOutcomeBusy:
        handler->stats.launches_busy++;
        (void)reply_launch_result(handler, client_index, serial, local_request_id, InteropBusy, 0, NULL, 0);
        break;
    case SessionLocalOutcomeUnsupported:
        handler->stats.launches_unsupported++;
        (void)reply_launch_result(handler, client_index, serial, local_request_id, InteropUnsupported, 0, NULL, 0);
        break;
    case SessionLocalOutcomeCancelled:
        handler->stats.launches_cancelled++;
        (void)reply_launch_result(handler, client_index, serial, local_request_id, InteropCancelled, 0, NULL, 0);
        break;
    default:
        handler->stats.launch_errors++;
        (void)reply_error(handler, client_index, serial, local_request_id, InteropErrorUnexpected,
            "launch not forwarded");
        break;
    }
}

/**
 * @brief Forward one validated LocalLaunch and create its correlation entry on success.
 * @param[in,out] sender Valid sender from the running callback.
 * @param[in] client_index Slot of the requesting client.
 * @param[in] serial Occupancy serial of that client.
 * @param[in] local_request_id Nonzero identifier from the client frame.
 * @param[in] payload Validated LocalLaunch payload.
 * @param[in] payload_length Payload bytes.
 * @param[in] now_ms Monotonic time of the send, used for the launch deadline.
 * @thread_safety Pump thread only.
 */
static void forward_launch(session_local_handler_t *handler, interop_host_feature_sender_t *sender,
    uint32_t client_index, uint64_t serial, uint32_t local_request_id, const uint8_t *payload,
    size_t payload_length, uint64_t now_ms)
{
    session_local_pending_t *slot;
    size_t frame_length = 0;
    uint32_t guest_request_id = 0;
    int result;
    const uint8_t *token = NULL;
    uint16_t token_length = 0;

    if (handler->stopping) {
        handler->stats.launches_cancelled++;
        (void)reply_launch_result(handler, client_index, serial, local_request_id, InteropCancelled, 0, NULL, 0);
        return;
    }
    if (pending_outstanding(handler, client_index, serial) >= SessionLocalMaxOutstanding) {
        handler->stats.launches_busy++;
        (void)reply_launch_result(handler, client_index, serial, local_request_id, InteropBusy, 0, NULL, 0);
        return;
    }
    slot = pending_free_slot(handler);
    if (slot == NULL) {
        handler->stats.launches_busy++;
        (void)reply_launch_result(handler, client_index, serial, local_request_id, InteropBusy, 0, NULL, 0);
        return;
    }
    if (build_launch_request(handler, payload, payload_length, &frame_length) != 0) {
        handler->stats.launch_errors++;
        (void)reply_error(handler, client_index, serial, local_request_id, InteropErrorUnexpected,
            "launch not forwarded");
        return;
    }
    result = interop_host_feature_send_request(sender, handler->scratch, frame_length, &guest_request_id);
    if (session_local_send_outcome(result) != SessionLocalOutcomeAccepted) {
        reply_refused_launch(handler, client_index, serial, local_request_id, session_local_send_outcome(result));
        return;
    }
    memset(slot, 0, sizeof(*slot));
    slot->in_use = 1;
    slot->guest_request_id = guest_request_id;
    slot->local_request_id = local_request_id;
    slot->client_index = client_index;
    slot->client_serial = serial;
    slot->deadline_ms = now_ms + SessionLocalLaunchTimeoutMs;
    if (interop_field_get(payload, payload_length, InteropTagActivationToken, 0, &token, &token_length) == 0) {
        memcpy(slot->token, token, token_length);
        slot->token[token_length] = '\0';
        slot->token_length = token_length;
    }
    handler->stats.launches_forwarded++;
}

/**
 * @brief Handle one guest LaunchResult (contract section 8.3).
 * @return 0 when handled or ignored, 1 for a malformed frame (the run must fail).
 */
static int handle_launch_result(session_local_handler_t *handler, interop_host_feature_sender_t *sender,
    uint32_t request_id, const uint8_t *frame, size_t length)
{
    const uint8_t *payload = frame + InteropHeaderBytes;
    size_t payload_length = length - InteropHeaderBytes;
    session_local_pending_t *entry = pending_find(handler, request_id);
    uint64_t status = 0;
    uint64_t process_id = 0;
    const uint8_t *message = NULL;
    uint16_t message_length = 0;
    int message_found;

    if (entry == NULL) {
        handler->stats.ignored_results++;
        return 0;
    }
    if (interop_payload_validate(InteropMsgLaunchResult, InteropFromGuest, payload, payload_length) != InteropErrorNone) {
        return 1;
    }
    if (interop_field_uint(payload, payload_length, InteropTagStatus, 4, &status) != 0) return 1;
    if (interop_field_uint(payload, payload_length, InteropTagProcessId, 4, &process_id) != 0) return 1;
    message_found = interop_field_get(payload, payload_length, InteropTagMessage, 0, &message, &message_length);
    if (message_found < 0) return 1;

    if (status == InteropOk && entry->token_length != 0) {
        int stored = interop_activation_put(&handler->tokens, entry->token, entry->token_length,
            (uint32_t)process_id, handler->last_now_ms);
        if (stored == InteropActivationPutOk) handler->stats.token_stored++;
        else handler->stats.token_refused++;
    }
    if (reply_launch_result(handler, entry->client_index, entry->client_serial, entry->local_request_id,
            (uint32_t)status, (uint32_t)process_id, message_found == 0 ? message : NULL, message_length)) {
        handler->stats.results_delivered++;
    }
    (void)interop_host_feature_release_request(sender, entry->guest_request_id);
    memset(entry, 0, sizeof(*entry));
    return 0;
}
