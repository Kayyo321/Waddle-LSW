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
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
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
    char parent[SessionLocalSocketPathBytes];
    char directory[SessionLocalSocketPathBytes];
    char socket[SessionLocalSocketPathBytes];
    size_t runtime_length = 0;
    int written_parent;
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
    written_parent = snprintf(parent, sizeof(parent), "%s/waddle", config->runtime_dir);
    if (written_parent < 0 || (size_t)written_parent >= sizeof(parent)) return SessionLocalInvalidArgument;
    written_directory = snprintf(directory, sizeof(directory), "%s/waddle/%s",
        config->runtime_dir, config->device_name);
    if (written_directory < 0 || (size_t)written_directory >= sizeof(directory)) return SessionLocalInvalidArgument;
    written_socket = snprintf(socket, sizeof(socket), "%s/session.sock", directory);
    if (written_socket < 0 || (size_t)written_socket >= sizeof(socket)) return SessionLocalInvalidArgument;

    memset(handler, 0, sizeof(*handler));
    memcpy(handler->parent_path, parent, (size_t)written_parent + 1u);
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

/* ---- Runtime directory and socket (contract sections 4 and 5) ------------------------------ */

/**
 * @brief Verify or create one runtime directory. Never chmods or follows a link.
 * @return 0 when the entry is usable (existing or freshly created with mode 0700), -1 otherwise.
 */
static int ensure_directory(const char *path, uint32_t expected_uid)
{
    struct stat status;
    if (lstat(path, &status) != 0) {
        if (errno != ENOENT) return -1;
        if (mkdir(path, 0700) != 0 && errno != EEXIST) return -1;
        if (lstat(path, &status) != 0) return -1;
    }
    return session_local_directory_decision(S_ISDIR(status.st_mode), S_ISLNK(status.st_mode),
        (uint32_t)status.st_uid, (uint32_t)status.st_mode, expected_uid) == SessionLocalDirectoryUsable ? 0 : -1;
}

/** @brief Fill an AF_UNIX address for the socket path; the path length was bounded at init. */
static socklen_t socket_address(const char *path, struct sockaddr_un *address)
{
    memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    memcpy(address->sun_path, path, strlen(path) + 1u);
    return (socklen_t)sizeof(*address);
}

/**
 * @brief Apply the stale-socket policy to an existing session.sock path.
 * @return 0 when the path is free or was unlinked under proof, -1 when the bind must be refused.
 */
static int prepare_socket_path(session_local_handler_t *handler)
{
    struct stat status;
    struct sockaddr_un address;
    socklen_t address_length;
    int probe;
    int probe_errno = 0;

    if (lstat(handler->socket_path, &status) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISSOCK(status.st_mode)) return -1;
    probe = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (probe < 0) return -1;
    address_length = socket_address(handler->socket_path, &address);
    if (connect(probe, (const struct sockaddr *)&address, address_length) != 0) probe_errno = errno;
    (void)close(probe);
    if (session_local_stale_decision(probe_errno, handler->lock_held_proof) != SessionLocalStaleUnlink) return -1;
    return unlink(handler->socket_path) == 0 ? 0 : -1;
}

/** @brief Create, bind (mode 0600 by umask window), listen, and register the listener in epoll. */
static int bind_listener(session_local_handler_t *handler)
{
    struct sockaddr_un address;
    socklen_t address_length = socket_address(handler->socket_path, &address);
    struct stat status;
    struct epoll_event event;
    mode_t previous_umask;
    int bind_result;
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listener < 0) return -1;

    previous_umask = umask(0177);
    bind_result = bind(listener, (const struct sockaddr *)&address, address_length);
    (void)umask(previous_umask);
    if (bind_result != 0) {
        (void)close(listener);
        return -1;
    }
    handler->socket_created = 1;
    if (lstat(handler->socket_path, &status) != 0 || !S_ISSOCK(status.st_mode) || (status.st_mode & 0777) != 0600) {
        (void)close(listener);
        return -1;
    }
    if (listen(listener, SessionLocalListenBacklog) != 0) {
        (void)close(listener);
        return -1;
    }
    handler->listen_fd = listener;
    handler->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (handler->epoll_fd < 0) return -1;
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.u64 = 0;
    if (epoll_ctl(handler->epoll_fd, EPOLL_CTL_ADD, handler->listen_fd, &event) != 0) return -1;
    return 0;
}

static int local_on_ready(void *context, uint64_t negotiated_capabilities, interop_host_feature_sender_t *sender)
{
    session_local_handler_t *handler = context;
    (void)negotiated_capabilities;
    (void)sender;
    if (handler->bound || handler->closed) return 1;
    if (ensure_directory(handler->parent_path, handler->expected_uid) != 0) return 1;
    if (ensure_directory(handler->directory_path, handler->expected_uid) != 0) return 1;
    if (prepare_socket_path(handler) != 0) return 1;
    if (bind_listener(handler) != 0) {
        release_descriptors(handler);
        return 1;
    }
    handler->bound = 1;
    return 0;
}

/* ---- Client I/O (contract sections 5, 6 and 7) --------------------------------------------- */

/** @brief Set the epoll interest of a client: input unless stopping, output while bytes are pending. */
static void update_interest(session_local_handler_t *handler, uint32_t client_index)
{
    session_local_client_t *client = &handler->clients[client_index];
    struct epoll_event event;
    int writable = client->tx_offset < client->tx_length;
    uint32_t events = (handler->stopping ? 0u : EPOLLIN) | (writable ? EPOLLOUT : 0u);
    if (client->fd < 0 || handler->epoll_fd < 0) return;
    memset(&event, 0, sizeof(event));
    event.events = events;
    event.data.u64 = (uint64_t)client_index + 1u;
    if (epoll_ctl(handler->epoll_fd, EPOLL_CTL_MOD, client->fd, &event) == 0) client->epoll_writable = writable;
}

/** @brief Send as much pending output as the socket accepts. Closes the client on a send error. */
static void flush_client(session_local_handler_t *handler, uint32_t client_index)
{
    session_local_client_t *client = &handler->clients[client_index];
    while (client->fd >= 0 && client->tx_offset < client->tx_length) {
        ssize_t sent = send(client->fd, client->tx + client->tx_offset, client->tx_length - client->tx_offset,
            MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (sent <= 0) {
            handler->stats.closed_io++;
            client_close(handler, client_index);
            return;
        }
        client->tx_offset += (size_t)sent;
    }
    if (client->fd >= 0 && client->tx_offset == client->tx_length) {
        client->tx_offset = 0;
        client->tx_length = 0;
    }
    if (client->fd >= 0 && client->epoll_writable != (client->tx_offset < client->tx_length)) update_interest(handler, client_index);
}

/** @brief Accept every pending connection, enforcing the peer uid and the four-slot limit. */
static void accept_clients(session_local_handler_t *handler, uint64_t now_ms)
{
    for (uint32_t attempt = 0; attempt < SessionLocalMaxClients + 1u; ++attempt) {
        struct ucred credentials;
        socklen_t credentials_length = sizeof(credentials);
        uint32_t slot = SessionLocalMaxClients;
        int descriptor = accept4(handler->listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (descriptor < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno != EINTR && errno != ECONNABORTED) handler->stats.accept_errors++;
            if (errno == EINTR || errno == ECONNABORTED) continue;
            return;
        }
        if (getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &credentials_length) != 0 ||
            session_local_peer_decision((uint32_t)credentials.uid, handler->expected_uid) != SessionLocalPeerAllowed) {
            handler->stats.refused_peer++;
            (void)close(descriptor);
            continue;
        }
        if (handler->stopping) {
            handler->stats.refused_capacity++;
            (void)close(descriptor);
            continue;
        }
        for (uint32_t index = 0; index < SessionLocalMaxClients; ++index) {
            if (handler->clients[index].fd < 0) {
                slot = index;
                break;
            }
        }
        if (slot == SessionLocalMaxClients) {
            handler->stats.refused_capacity++;
            (void)close(descriptor);
            continue;
        }
        session_local_client_t *client = &handler->clients[slot];
        struct epoll_event event;
        memset(&event, 0, sizeof(event));
        event.events = EPOLLIN;
        event.data.u64 = (uint64_t)slot + 1u;
        if (epoll_ctl(handler->epoll_fd, EPOLL_CTL_ADD, descriptor, &event) != 0) {
            handler->stats.closed_io++;
            (void)close(descriptor);
            continue;
        }
        client->fd = descriptor;
        client->serial = handler->next_serial++;
        client->accepted_ms = now_ms;
        client->frame_seen = 0;
        client->epoll_writable = 0;
        client->rx_length = 0;
        client->tx_length = 0;
        client->tx_offset = 0;
        handler->stats.accepted++;
    }
}

/**
 * @brief Accept a LocalStop: queue its reply, stop reading from every client, and start the flush bound.
 * @param[in] client_index Client that sent the stop; its reply is the only one flushed for the stop.
 * @param[in] request_id Nonzero identifier to echo.
 * @param[in] now_ms Monotonic time of the frame.
 */
static void begin_stop(session_local_handler_t *handler, uint32_t client_index, uint32_t request_id, uint64_t now_ms)
{
    session_local_client_t *client = &handler->clients[client_index];
    handler->stats.stop_requests++;
    handler->stopping = 1;
    handler->stop_client_index = client_index;
    handler->stop_client_serial = client->serial;
    handler->stop_deadline_ms = now_ms + SessionLocalStopFlushMs;
    (void)reply_stop_ok(handler, client_index, client->serial, request_id);
    for (uint32_t index = 0; index < SessionLocalMaxClients; ++index) {
        if (handler->clients[index].fd >= 0) update_interest(handler, index);
    }
}

/**
 * @brief Whether the stop may now end the run: the LocalStopResult is fully written, the client is
 *        gone, or the flush bound expired (counted).
 * @return Nonzero when SessionHandlerRequestStop may be returned.
 */
static int stop_ready(session_local_handler_t *handler, uint64_t now_ms)
{
    const session_local_client_t *client = &handler->clients[handler->stop_client_index];
    if (!handler->stopping) return 0;
    if (client->fd < 0 || client->serial != handler->stop_client_serial) return 1;
    if (client->tx_offset == client->tx_length) return 1;
    if (now_ms >= handler->stop_deadline_ms) {
        handler->stats.stop_deadline_expired++;
        return 1;
    }
    return 0;
}

/**
 * @brief Dispatch every complete frame in a client receive buffer, in order.
 * @return 1 when the client is still open, 0 when it was closed.
 */
static int process_frames(session_local_handler_t *handler, uint32_t client_index, uint64_t now_ms,
    interop_host_feature_sender_t *sender)
{
    session_local_client_t *client = &handler->clients[client_index];
    while (client->fd >= 0 && client->rx_length != 0 && !handler->stopping) {
        interop_header_t header;
        size_t frame_length = 0;
        const uint8_t *payload;
        int scan = interop_local_frame_scan(client->rx, client->rx_length, &header, &frame_length);
        if (scan == InteropLocalScanNeedMore) break;
        if (scan == InteropLocalScanTooLarge) {
            handler->stats.closed_oversize++;
            client_close(handler, client_index);
            return 0;
        }
        if (scan != InteropLocalScanReady) {
            handler->stats.closed_malformed++;
            client_close(handler, client_index);
            return 0;
        }
        client->frame_seen = 1;
        if (header.type != InteropMsgLocalLaunch && header.type != InteropMsgLocalStop) {
            handler->stats.closed_unknown_type++;
            client_close(handler, client_index);
            return 0;
        }
        if (header.request_id == 0) {
            handler->stats.closed_zero_request++;
            client_close(handler, client_index);
            return 0;
        }
        payload = client->rx + InteropHeaderBytes;
        if (interop_payload_validate(header.type, InteropFromLocal, payload, header.length) != InteropErrorNone) {
            handler->stats.closed_malformed++;
            client_close(handler, client_index);
            return 0;
        }
        if (header.type == InteropMsgLocalLaunch) {
            forward_launch(handler, sender, client_index, client->serial, header.request_id, payload,
                header.length, now_ms);
        } else {
            begin_stop(handler, client_index, header.request_id, now_ms);
            return client->fd >= 0;
        }
        memmove(client->rx, client->rx + frame_length, client->rx_length - frame_length);
        client->rx_length -= frame_length;
    }
    return client->fd >= 0;
}

/** @brief Read once from a readable client; end of stream, errors, and overflow close it. */
static void read_client(session_local_handler_t *handler, uint32_t client_index, uint64_t now_ms,
    interop_host_feature_sender_t *sender)
{
    session_local_client_t *client = &handler->clients[client_index];
    size_t room = SessionLocalRxBytes - client->rx_length;
    ssize_t received;
    if (room == 0) {
        handler->stats.closed_oversize++;
        client_close(handler, client_index);
        return;
    }
    received = recv(client->fd, client->rx + client->rx_length, room, 0);
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
    if (received <= 0) {
        handler->stats.closed_io++;
        client_close(handler, client_index);
        return;
    }
    client->rx_length += (size_t)received;
    (void)process_frames(handler, client_index, now_ms, sender);
}

/* ---- Callbacks ----------------------------------------------------------------------------- */

static int local_owns_type(void *context, uint32_t message_type)
{
    (void)context;
    return message_type == InteropMsgLaunchResult;
}

static int local_on_frame(void *context, uint32_t message_type, uint32_t request_id,
    const uint8_t *frame, size_t length, interop_host_feature_sender_t *sender)
{
    session_local_handler_t *handler = context;
    if (message_type != InteropMsgLaunchResult) return 1;
    return handle_launch_result(handler, sender, request_id, frame, length);
}

static int local_poll_fds(void *context, struct pollfd *fds, size_t capacity, size_t *count)
{
    session_local_handler_t *handler = context;
    if (fds == NULL || count == NULL || capacity < 1) return 1;
    *count = 0;
    if (!handler->bound || handler->epoll_fd < 0) return 0;
    fds[0].fd = handler->epoll_fd;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    *count = 1;
    return 0;
}

static int local_on_fds(void *context, const struct pollfd *fds, size_t count, uint64_t now_ms,
    interop_host_feature_sender_t *sender)
{
    session_local_handler_t *handler = context;
    struct epoll_event events[SessionLocalMaxClients + 1u];
    int ready;

    handler->last_now_ms = now_ms;
    if (!handler->bound || handler->closed || count == 0 || fds[0].revents == 0) return 0;
    ready = epoll_wait(handler->epoll_fd, events, (int)(sizeof(events) / sizeof(events[0])), 0);
    if (ready < 0) return errno == EINTR ? 0 : 1;
    for (int index = 0; index < ready; ++index) {
        uint64_t tag = events[index].data.u64;
        if (tag == 0) {
            accept_clients(handler, now_ms);
            continue;
        }
        uint32_t client_index = (uint32_t)(tag - 1u);
        if (client_index >= SessionLocalMaxClients || handler->clients[client_index].fd < 0) continue;
        if (events[index].events & EPOLLOUT) flush_client(handler, client_index);
        if (handler->clients[client_index].fd >= 0 && (events[index].events & (EPOLLIN | EPOLLHUP | EPOLLERR)) &&
            !handler->stopping) {
            read_client(handler, client_index, now_ms, sender);
        } else if (handler->clients[client_index].fd >= 0 && (events[index].events & (EPOLLHUP | EPOLLERR))) {
            handler->stats.closed_io++;
            client_close(handler, client_index);
        }
    }
    for (uint32_t index = 0; index < SessionLocalMaxClients; ++index) {
        if (handler->clients[index].fd >= 0) flush_client(handler, index);
    }
    return stop_ready(handler, now_ms) ? SessionHandlerRequestStop : 0;
}

static int local_on_tick(void *context, uint64_t now_ms, interop_host_feature_sender_t *sender)
{
    session_local_handler_t *handler = context;
    (void)sender;
    handler->last_now_ms = now_ms;
    if (!handler->bound || handler->closed) return 0;
    for (uint32_t index = 0; index < SessionLocalMaxClients; ++index) {
        if (handler->clients[index].fd >= 0) flush_client(handler, index);
    }
    return stop_ready(handler, now_ms) ? SessionHandlerRequestStop : 0;
}

void session_local_handler_table(session_local_handler_t *handler, session_handler_t *out_table)
{
    memset(out_table, 0, sizeof(*out_table));
    out_table->name = "session_local";
    out_table->context = handler;
    out_table->capabilities = InteropCapLaunch;
    out_table->owns_type = local_owns_type;
    out_table->on_ready = local_on_ready;
    out_table->on_frame = local_on_frame;
    out_table->poll_fds = local_poll_fds;
    out_table->on_fds = local_on_fds;
    out_table->on_tick = local_on_tick;
}
