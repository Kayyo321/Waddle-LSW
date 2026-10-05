/**
 * @file daemon_server.c
 * @brief Implementation of daemon UNIX socket event loop and request dispatcher.
 */

#include "daemon_server.h"
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

/**
 * @brief Sets a file descriptor to non-blocking mode.
 *
 * @param[in] fd File descriptor.
 * @return 0 on success, or -1 on fcntl failure.
 */
static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
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
                                  int client_fd,
                                  const waddle_daemon_header_t *hdr,
                                  const uint8_t *payload) {
    switch (hdr->msg_type) {
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
        (void)waddle_daemon_send_msg(client_fd, DaemonMsgStartResp, hdr->sequence, &resp, sizeof(resp));
        break;
    }

    case DaemonMsgStopReq: {
        const waddle_daemon_stop_req_t *req = (const waddle_daemon_stop_req_t *)payload;
        uint32_t force = (hdr->payload_len >= sizeof(waddle_daemon_stop_req_t)) ? req->force : 0;
        uint32_t timeout = (hdr->payload_len >= sizeof(waddle_daemon_stop_req_t)) ? req->timeout_sec : 0;

        int r = daemon_state_stop_subsystem(state, force, timeout);
        waddle_daemon_result_resp_t resp;
        memset(&resp, 0, sizeof(resp));
        resp.status_code = (r == 0) ? 0 : ((errno != 0) ? (uint32_t)errno : 1);
        resp.subsystem_state = (uint32_t)state->state;
        if (r != 0 && state->last_error[0] != '\0') {
            snprintf(resp.error_msg, sizeof(resp.error_msg), "%s", state->last_error);
        }
        (void)waddle_daemon_send_msg(client_fd, DaemonMsgStopResp, hdr->sequence, &resp, sizeof(resp));
        break;
    }

    case DaemonMsgShutdownReq: {
        waddle_daemon_result_resp_t resp = {0};
        daemon_state_reap_children(state);
        resp.subsystem_state = (uint32_t)state->state;
        if (hdr->payload_len != 0) {
            resp.status_code = EINVAL;
        } else if ((state->state != SubsystemStateStopped && state->state != SubsystemStateFailed) ||
                   state->qemu.pid != 0 || state->virtiofs.pid != 0) {
            resp.status_code = EBUSY;
        }
        int sent = waddle_daemon_send_msg(client_fd, DaemonMsgShutdownResp,
                                         hdr->sequence, &resp, sizeof(resp));
        return resp.status_code == 0 && sent == 0;
    }

    case DaemonMsgStatusReq: {
        waddle_daemon_status_resp_t resp;
        daemon_state_get_status(state, &resp);
        (void)waddle_daemon_send_msg(client_fd, DaemonMsgStatusResp, hdr->sequence, &resp, sizeof(resp));
        break;
    }

    case DaemonMsgKillReq: {
        (void)daemon_state_kill_subsystem(state);
        waddle_daemon_result_resp_t resp;
        memset(&resp, 0, sizeof(resp));
        resp.status_code = 0;
        resp.subsystem_state = SubsystemStateStopped;
        (void)waddle_daemon_send_msg(client_fd, DaemonMsgKillResp, hdr->sequence, &resp, sizeof(resp));
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
        (void)waddle_daemon_send_msg(client_fd, DaemonMsgFsListResp, hdr->sequence, &resp, sizeof(resp));
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

        (void)waddle_daemon_send_msg(client_fd, DaemonMsgLogsResp, hdr->sequence, log_buf, bytes_read);
        break;
    }

    default: {
        waddle_daemon_result_resp_t resp;
        memset(&resp, 0, sizeof(resp));
        resp.status_code = ENOTSUP;
        resp.subsystem_state = (uint32_t)state->state;
        snprintf(resp.error_msg, sizeof(resp.error_msg), "Unsupported message type: %u", (unsigned)hdr->msg_type);
        (void)waddle_daemon_send_msg(client_fd, DaemonMsgErrorResp, hdr->sequence, &resp, sizeof(resp));
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

    int client_fds[DaemonMaxClients];
    for (size_t i = 0; i < DaemonMaxClients; i++) {
        client_fds[i] = -1;
    }

    int shutdown_requested = 0;
    /* Main non-blocking event loop */
    while (!shutdown_requested && (stop_flag == NULL || *stop_flag == 0)) {
        daemon_state_reap_children(&state);

        struct pollfd pfd[1 + DaemonMaxClients];
        pfd[0].fd = server_fd;
        pfd[0].events = POLLIN;
        pfd[0].revents = 0;
        nfds_t nfds = 1;

        for (size_t i = 0; i < DaemonMaxClients; i++) {
            if (client_fds[i] >= 0) {
                pfd[nfds].fd = client_fds[i];
                pfd[nfds].events = POLLIN;
                pfd[nfds].revents = 0;
                nfds++;
            }
        }

        int pr = poll(pfd, nfds, 250);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* Accept new client */
        if (pfd[0].revents & POLLIN) {
            int new_fd = accept(server_fd, NULL, NULL);
            if (new_fd >= 0) {
                set_nonblock(new_fd);
                int placed = 0;
                for (size_t i = 0; i < DaemonMaxClients; i++) {
                    if (client_fds[i] < 0) {
                        client_fds[i] = new_fd;
                        placed = 1;
                        break;
                    }
                }
                if (!placed) {
                    /* Server full */
                    waddle_daemon_result_resp_t err_resp;
                    memset(&err_resp, 0, sizeof(err_resp));
                    err_resp.status_code = EMFILE;
                    snprintf(err_resp.error_msg, sizeof(err_resp.error_msg), "Too many client connections");
                    (void)waddle_daemon_send_msg(new_fd, DaemonMsgErrorResp, 0, &err_resp, sizeof(err_resp));
                    close(new_fd);
                }
            }
        }

        /* Check client sockets */
        nfds_t cur_idx = 1;
        for (size_t i = 0; i < DaemonMaxClients; i++) {
            if (client_fds[i] < 0) continue;
            if (cur_idx >= nfds) break;
            /* Newly accepted clients are not in this poll snapshot. */
            if (pfd[cur_idx].fd != client_fds[i]) continue;

            if (pfd[cur_idx].revents & (POLLIN | POLLHUP | POLLERR)) {
                waddle_daemon_header_t hdr;
                uint8_t payload[WaddleDaemonMaxPayloadSize];
                memset(&hdr, 0, sizeof(hdr));

                int r = waddle_daemon_recv_msg(client_fds[i], &hdr, payload, sizeof(payload), 50);
                if (r == 0) {
                    if (handle_client_message(&state, client_fds[i], &hdr, payload)) {
                        shutdown_requested = 1;
                        break;
                    }
                } else if (r == -2 || (r == -1 && errno != EAGAIN && errno != EWOULDBLOCK && errno != ETIMEDOUT)) {
                    /* EOF or client disconnect */
                    close(client_fds[i]);
                    client_fds[i] = -1;
                }
            }
            cur_idx++;
        }
    }

    /* Shutdown & Teardown */
    for (size_t i = 0; i < DaemonMaxClients; i++) {
        if (client_fds[i] >= 0) {
            close(client_fds[i]);
            client_fds[i] = -1;
        }
    }

    close(server_fd);
    daemon_state_cleanup(&state);
    return 0;
}
