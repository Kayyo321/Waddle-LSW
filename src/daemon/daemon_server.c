/**
 * @file daemon_server.c
 * @brief Implementation of daemon UNIX socket event loop and request dispatcher.
 */

#include "daemon_server.h"
#include "daemon_guest_ready.h"
#include "daemon_wire.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <time.h>

/** One bounded request/reply and probe per serialized same-user connection. */
typedef struct daemon_client_slot_t {
    int fd;
    uint8_t header_bytes[WaddleDaemonHeaderSize];
    size_t header_used;
    waddle_daemon_header_t header;
    _Alignas(max_align_t) uint8_t payload[WaddleDaemonMaxPayloadSize];
    size_t payload_used;
    uint8_t transmit[WaddleDaemonHeaderSize + WaddleDaemonMaxPayloadSize];
    size_t transmit_used;
    size_t transmit_offset;
    daemon_guest_ready_t ready;
    int bad;
    int shutdown_after_reply;
} daemon_client_slot_t;

static int monotonic_ms(uint64_t *out_ms) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0 || value.tv_sec < 0 ||
        value.tv_nsec < 0 || value.tv_nsec >= 1000000000L ||
        (uint64_t)value.tv_sec > (UINT64_MAX - 999) / 1000) return -1;
    *out_ms = (uint64_t)value.tv_sec * 1000 + (uint64_t)value.tv_nsec / 1000000;
    return 0;
}

static void close_client(daemon_client_slot_t *client) {
    daemon_guest_ready_cancel(&client->ready);
    int fd = client->fd;
    client->fd = -1;
    if (fd >= 0) (void)close(fd);
}

static int queue_reply(daemon_client_slot_t *client, uint16_t type,
                        uint32_t sequence, const void *payload, size_t bytes) {
    if (bytes > WaddleDaemonMaxPayloadSize || client->transmit_used != 0) {
        client->bad = 1;
        return -1;
    }
    waddle_daemon_header_t header = {.magic = WaddleDaemonMagic,
        .version = WaddleDaemonVersion, .msg_type = type,
        .sequence = sequence, .payload_len = (uint32_t)bytes};
    memcpy(client->transmit, &header, sizeof(header));
    if (bytes != 0) memcpy(client->transmit + sizeof(header), payload, bytes);
    client->transmit_used = sizeof(header) + bytes;
    client->transmit_offset = 0;
    return 0;
}

/**
 * @brief Dispatches a single parsed client command frame.
 *
 * @param[in,out] state     Supervisor state.
 * @param[in]     client_fd Connected client socket descriptor.
 * @param[in]     hdr       Received frame header.
 * @param[in]     payload   Received payload bytes.
 * @return One after successfully replying to an idle shutdown, otherwise zero.
 * @note Single event-loop thread; borrowed inputs; no retained memory.
 */
static int handle_client_message(daemon_state_t *state,
                                  daemon_client_slot_t *client,
                                  const waddle_daemon_header_t *hdr,
                                  const uint8_t *payload, uint64_t now_ms) {
    switch (hdr->msg_type) {
    case DaemonMsgGuestReadyReq: {
        uint32_t timeout_ms = 0;
        if (daemon_wire_decode_ready_request(payload, hdr->payload_len, &timeout_ms) != 0 ||
            daemon_guest_ready_start(&client->ready, state->config.vsock_cid,
                state->config.vsock_port, timeout_ms, now_ms, hdr->sequence) != 0) {
            waddle_daemon_result_resp_t response = {.status_code = EINVAL,
                .subsystem_state = (uint32_t)state->state};
            (void)queue_reply(client, DaemonMsgErrorResp, hdr->sequence, &response, sizeof(response));
        }
        break;
    }

    case DaemonMsgStartReq: {
        const waddle_daemon_start_req_t *req = (const waddle_daemon_start_req_t *)payload;
        uint32_t flags = (hdr->payload_len >= sizeof(waddle_daemon_start_req_t)) ? req->flags : 0;
        uint32_t timeout = (hdr->payload_len >= sizeof(waddle_daemon_start_req_t)) ? req->timeout_sec : 0;

        int r = daemon_state_start_subsystem(state, flags, timeout);
        waddle_daemon_result_resp_t resp;
        memset(&resp, 0, sizeof(resp));
        resp.status_code = (r == 0) ? 0 : ((errno != 0) ? (uint32_t)errno : 1);
        resp.subsystem_state = (uint32_t)state->state;
        if (r != 0 && state->last_error[0] != '\0') {
            snprintf(resp.error_msg, sizeof(resp.error_msg), "%s", state->last_error);
        }
        (void)queue_reply(client, DaemonMsgStartResp, hdr->sequence, &resp, sizeof(resp));
        break;
    }

    case DaemonMsgStopGracefulReq:
    case DaemonMsgStopReq: {
        const waddle_daemon_stop_req_t *req = (const waddle_daemon_stop_req_t *)payload;
        uint32_t force = (hdr->payload_len >= sizeof(waddle_daemon_stop_req_t)) ? req->force : 0;
        uint32_t timeout = (hdr->payload_len >= sizeof(waddle_daemon_stop_req_t)) ? req->timeout_sec : 0;

        int r;
        if (hdr->msg_type == DaemonMsgStopGracefulReq &&
            (hdr->payload_len != sizeof(waddle_daemon_stop_req_t) || req->force != 0)) {
            errno = EINVAL;
            r = -1;
        } else {
            if (hdr->msg_type == DaemonMsgStopGracefulReq) force = WaddleStopGracefulOnly;
            r = daemon_state_stop_subsystem(state, force, timeout);
        }
        waddle_daemon_result_resp_t resp;
        memset(&resp, 0, sizeof(resp));
        resp.status_code = (r == 0) ? 0 : ((errno != 0) ? (uint32_t)errno : 1);
        resp.subsystem_state = (uint32_t)state->state;
        if (r != 0 && state->last_error[0] != '\0') {
            snprintf(resp.error_msg, sizeof(resp.error_msg), "%s", state->last_error);
        }
        (void)queue_reply(client, DaemonMsgStopResp, hdr->sequence, &resp, sizeof(resp));
        break;
    }

    case DaemonMsgShutdownReq: {
        waddle_daemon_result_resp_t resp = {0};
        daemon_state_reap_children(state);
        resp.subsystem_state = (uint32_t)state->state;
        if (hdr->payload_len != 0) {
            resp.status_code = EINVAL;
        } else if ((state->state != SubsystemStateStopped && state->state != SubsystemStateFailed) ||
                   state->qemu.pid != 0 || state->virtiofs.pid != 0 ||
                   state->virtiofs_owned_count != 0) {
            resp.status_code = EBUSY;
        }
        int sent = queue_reply(client, DaemonMsgShutdownResp,
                                         hdr->sequence, &resp, sizeof(resp));
        return resp.status_code == 0 && sent == 0;
    }

    case DaemonMsgStatusReq: {
        waddle_daemon_status_resp_t resp;
        daemon_state_get_status(state, &resp);
        (void)queue_reply(client, DaemonMsgStatusResp, hdr->sequence, &resp, sizeof(resp));
        break;
    }

    case DaemonMsgKillReq: {
        int r = daemon_state_kill_subsystem(state);
        waddle_daemon_result_resp_t resp;
        memset(&resp, 0, sizeof(resp));
        resp.status_code = (r == 0) ? 0 : ((errno != 0) ? (uint32_t)errno : 1);
        resp.subsystem_state = (uint32_t)state->state;
        if (r != 0 && state->last_error[0] != '\0') {
            snprintf(resp.error_msg, sizeof(resp.error_msg), "%s", state->last_error);
        }
        (void)queue_reply(client, DaemonMsgKillResp, hdr->sequence, &resp, sizeof(resp));
        break;
    }

    case DaemonMsgFsListReq: {
        waddle_daemon_fs_list_resp_t resp;
        memset(&resp, 0, sizeof(resp));
        resp.mount_count = state->config.mount_count;
        size_t count = (state->config.mount_count < WaddleMaxMounts) ? state->config.mount_count : WaddleMaxMounts;
        for (size_t i = 0; i < count; i++) {
            memcpy(&resp.mounts[i], &state->config.mounts[i], sizeof(waddle_daemon_fs_mount_t));
        }
        (void)queue_reply(client, DaemonMsgFsListResp, hdr->sequence, &resp, sizeof(resp));
        break;
    }

    case DaemonMsgLogsReq: {
        /* Read up to 8 KiB from daemon log file */
        char log_buf[8192];
        memset(log_buf, 0, sizeof(log_buf));
        uint32_t bytes_read = 0;

        int log_fd = open(state->daemon_log_path, O_RDONLY);
        if (log_fd >= 0) {
            off_t sz = lseek(log_fd, 0, SEEK_END);
            off_t start = (sz > (off_t)sizeof(log_buf)) ? (sz - (off_t)sizeof(log_buf)) : 0;
            lseek(log_fd, start, SEEK_SET);
            ssize_t n = read(log_fd, log_buf, sizeof(log_buf));
            if (n > 0) bytes_read = (uint32_t)n;
            close(log_fd);
        }

        (void)queue_reply(client, DaemonMsgLogsResp, hdr->sequence, log_buf, bytes_read);
        break;
    }

    default: {
        waddle_daemon_result_resp_t resp;
        memset(&resp, 0, sizeof(resp));
        resp.status_code = ENOTSUP;
        resp.subsystem_state = (uint32_t)state->state;
        snprintf(resp.error_msg, sizeof(resp.error_msg), "Unsupported message type: %u", (unsigned)hdr->msg_type);
        (void)queue_reply(client, DaemonMsgErrorResp, hdr->sequence, &resp, sizeof(resp));
        break;
    }
    }
    return 0;
}

int daemon_server_run(const char *custom_runtime_dir, volatile sig_atomic_t *stop_flag) {
    daemon_state_t state;
    if (daemon_state_init(&state, custom_runtime_dir) != 0) {
        return 1;
    }

    if (daemon_state_acquire_lock(&state) != 0) {
        daemon_state_cleanup(&state);
        return 2; /* Another daemon already active */
    }

    /* Unlink old socket */
    unlink(state.daemon_sock_path);

    int server_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (server_fd < 0) {
        daemon_state_cleanup(&state);
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(state.daemon_sock_path) >= sizeof(addr.sun_path)) {
        close(server_fd);
        daemon_state_cleanup(&state);
        errno = ENAMETOOLONG;
        return 1;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%.107s", state.daemon_sock_path);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(server_fd);
        daemon_state_cleanup(&state);
        return 1;
    }

    if (chmod(state.daemon_sock_path, 0700) != 0) {
        close(server_fd);
        daemon_state_cleanup(&state);
        return 1;
    }

    if (listen(server_fd, 8) != 0) {
        close(server_fd);
        daemon_state_cleanup(&state);
        return 1;
    }

    daemon_client_slot_t *clients = calloc(DaemonMaxClients, sizeof(*clients));
    if (clients == NULL) {
        close(server_fd);
        daemon_state_cleanup(&state);
        return 1;
    }
    for (size_t index = 0; index < DaemonMaxClients; index++) {
        clients[index].fd = -1;
        daemon_guest_ready_init(&clients[index].ready);
    }
    int shutdown_requested = 0;
    int shutdown_pending = 0;
    while (!shutdown_requested && (stop_flag == NULL || *stop_flag == 0)) {
        daemon_state_reap_children(&state);
        uint64_t now_ms;
        if (monotonic_ms(&now_ms) != 0) {
            /* A timer failure cancels client work; it must not tear down the VM. */
            for (size_t index = 0; index < DaemonMaxClients; index++) close_client(&clients[index]);
            shutdown_pending = 0;
            (void)poll(NULL, 0, 250);
            continue;
        }
        struct pollfd descriptors[1 + 2 * DaemonMaxClients];
        descriptors[0] = (struct pollfd){.fd = shutdown_pending ? -1 : server_fd, .events = POLLIN};
        int timeout_ms = 250;
        for (size_t index = 0; index < DaemonMaxClients; index++) {
            daemon_client_slot_t *client = &clients[index];
            if (client->fd >= 0 && client->ready.active) {
                waddle_daemon_guest_ready_resp_t response;
                if (daemon_guest_ready_step(&client->ready, state.state == SubsystemStateRunning,
                                             now_ms, 0, &response))
                    (void)queue_reply(client, DaemonMsgGuestReadyResp, client->ready.sequence,
                                      &response, sizeof(response));
                if (client->ready.active) {
                    uint64_t next_ms = client->ready.probe_fd >= 0 ?
                        client->ready.probe_deadline_ms : client->ready.retry_at_ms;
                    if (next_ms > client->ready.deadline_ms) next_ms = client->ready.deadline_ms;
                    uint64_t remaining = next_ms > now_ms ? next_ms - now_ms : 0;
                    if (remaining < (uint64_t)timeout_ms) timeout_ms = (int)remaining;
                }
            }
            short events = client->transmit_used != 0 ? POLLOUT :
                !client->ready.active && !shutdown_pending ? POLLIN : 0;
            descriptors[1 + 2 * index] = (struct pollfd){.fd = client->fd,
                .events = events | POLLRDHUP};
            descriptors[2 + 2 * index] = (struct pollfd){.fd = client->ready.probe_fd, .events = POLLOUT};
        }
        int polled = poll(descriptors, 1 + 2 * DaemonMaxClients, timeout_ms);
        if (polled < 0) { if (errno == EINTR) continue; break; }
        if (monotonic_ms(&now_ms) != 0) {
            for (size_t index = 0; index < DaemonMaxClients; index++) close_client(&clients[index]);
            shutdown_pending = 0;
            continue;
        }
        if (descriptors[0].revents & POLLIN) {
            int fd = accept4(server_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd >= 0) {
                struct ucred credentials;
                socklen_t bytes = sizeof(credentials);
                int permitted = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &bytes) == 0 &&
                    bytes == sizeof(credentials) && credentials.uid == geteuid();
                size_t index = 0;
                while (index < DaemonMaxClients && clients[index].fd >= 0) index++;
                if (!permitted || index == DaemonMaxClients) (void)close(fd);
                else {
                    memset(&clients[index], 0, sizeof(clients[index]));
                    clients[index].fd = fd;
                    daemon_guest_ready_init(&clients[index].ready);
                }
            }
        }
        for (size_t index = 0; index < DaemonMaxClients; index++) {
            daemon_client_slot_t *client = &clients[index];
            if (client->fd < 0) continue;
            struct pollfd *descriptor = &descriptors[1 + 2 * index];
            if (descriptor->fd != client->fd) continue;
            if (descriptor->revents & (POLLHUP | POLLERR | POLLNVAL | POLLRDHUP)) {
                if (client->shutdown_after_reply) shutdown_pending = 0;
                close_client(client);
                continue;
            }
            if (client->ready.active) {
                waddle_daemon_guest_ready_resp_t response;
                if (daemon_guest_ready_step(&client->ready, state.state == SubsystemStateRunning,
                    now_ms, descriptors[2 + 2 * index].revents, &response))
                    (void)queue_reply(client, DaemonMsgGuestReadyResp, client->ready.sequence,
                                      &response, sizeof(response));
            }
            if ((descriptor->revents & POLLOUT) && client->transmit_used != 0) {
                ssize_t sent = send(client->fd, client->transmit + client->transmit_offset,
                    client->transmit_used - client->transmit_offset, MSG_DONTWAIT | MSG_NOSIGNAL);
                if (sent > 0) {
                    client->transmit_offset += (size_t)sent;
                    if (client->transmit_offset == client->transmit_used) {
                        client->transmit_offset = client->transmit_used = 0;
                        if (client->shutdown_after_reply) { shutdown_requested = 1; break; }
                    }
                } else if (sent == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                    client->bad = 1;
            }
            if ((descriptor->revents & POLLIN) && !shutdown_pending &&
                !client->ready.active && client->transmit_used == 0) {
                /* Consume at most this frame, retaining every short-read offset. */
                for (unsigned part = 0; part < 2; part++) {
                    int header_pending = client->header_used < WaddleDaemonHeaderSize;
                    size_t remaining = header_pending ? WaddleDaemonHeaderSize - client->header_used :
                        client->header.payload_len - client->payload_used;
                    if (remaining != 0) {
                        uint8_t *destination = header_pending ? client->header_bytes + client->header_used :
                            client->payload + client->payload_used;
                        ssize_t received = recv(client->fd, destination, remaining, MSG_DONTWAIT);
                        if (received <= 0) {
                            if (received == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                                client->bad = 1;
                            break;
                        }
                        if (header_pending) client->header_used += (size_t)received;
                        else client->payload_used += (size_t)received;
                    }
                    if (client->header_used < WaddleDaemonHeaderSize) break;
                    if (header_pending && daemon_wire_decode_header(client->header_bytes,
                        WaddleDaemonHeaderSize, &client->header) != 0) { client->bad = 1; break; }
                    if (client->payload_used == client->header.payload_len) {
                        if (handle_client_message(&state, client, &client->header, client->payload, now_ms)) {
                            client->shutdown_after_reply = 1;
                            shutdown_pending = 1;
                        }
                        client->header_used = client->payload_used = 0;
                        break;
                    }
                }
            }
            if (client->bad) {
                if (client->shutdown_after_reply) shutdown_pending = 0;
                close_client(client);
            }
        }
    }
    for (size_t index = 0; index < DaemonMaxClients; index++) close_client(&clients[index]);
    free(clients);

    close(server_fd);
    daemon_state_cleanup(&state);
    return 0;
}
