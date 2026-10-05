/**
 * @file test_daemon_client.c
 * @brief Unit and integration tests for the daemon client library and CLI lifecycle commands.
 */

#include "daemon_client.h"
#include "daemon_server.h"
#include "waddle/daemon_protocol.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void test_client_path_helpers(void) {
    char buf[WaddleMaxPathLen];
    assert(waddle_client_default_runtime_dir(buf, sizeof(buf)) == 0);
    assert(strlen(buf) > 0);

    assert(waddle_client_default_socket_path(buf, sizeof(buf)) == 0);
    assert(strstr(buf, "daemon.sock") != NULL);

    assert(waddle_client_default_log_dir(buf, sizeof(buf)) == 0);
    assert(strstr(buf, "logs") != NULL);

    /* Error handling on invalid parameters */
    assert(waddle_client_default_runtime_dir(NULL, 100) == -1);
    assert(waddle_client_default_runtime_dir(buf, 0) == -1);
    assert(waddle_client_default_socket_path(NULL, 100) == -1);
    assert(waddle_client_default_socket_path(buf, 5) == -1);
    assert(waddle_client_default_log_dir(NULL, 100) == -1);
    assert(waddle_client_default_log_dir(buf, 5) == -1);
}

static void test_client_offline(void) {
    const char *bogus_sock = "/tmp/waddle_nonexistent_socket_test.sock";
    assert(waddle_client_connect(bogus_sock) == -1);
    assert(waddle_client_is_alive(bogus_sock) == 0);

    /* Offline status command should succeed with stopped state */
    assert(waddle_client_cmd_status(bogus_sock, 0) == 0);
    assert(waddle_client_cmd_status(bogus_sock, 1) == 0);

    assert(waddle_client_shutdown(-1, NULL) == -1 && errno == EINVAL);

    /* Offline stop should report already stopped */
    assert(waddle_client_cmd_stop(bogus_sock, 0, 5) == 0);
}

typedef struct client_test_server_ctx_t {
    char runtime_dir[128];
    char sock_path[160];
    volatile sig_atomic_t stop_flag;
    pthread_t thread;
} client_test_server_ctx_t;

static void *server_thread_func(void *arg) {
    client_test_server_ctx_t *ctx = (client_test_server_ctx_t *)arg;
    (void)daemon_server_run(ctx->runtime_dir, &ctx->stop_flag);
    return NULL;
}

static void test_client_with_server(void) {
    client_test_server_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    snprintf(ctx.runtime_dir, sizeof(ctx.runtime_dir), "/tmp/waddle_test_cli_%d", (int)getpid());
    snprintf(ctx.sock_path, sizeof(ctx.sock_path), "%s/daemon.sock", ctx.runtime_dir);

    assert(pthread_create(&ctx.thread, NULL, server_thread_func, &ctx) == 0);

    /* Wait for daemon.sock to appear */
    int client_fd = -1;
    int tries = 0;
    while (tries < 40) {
        client_fd = waddle_client_connect(ctx.sock_path);
        if (client_fd >= 0) {
            break;
        }
        usleep(50000);
        tries++;
    }
    assert(client_fd >= 0);

    /* Verify is_alive */
    assert(waddle_client_is_alive(ctx.sock_path) == 1);

    /* Query status */
    waddle_daemon_status_resp_t status;
    memset(&status, 0, sizeof(status));
    assert(waddle_client_status(client_fd, &status) == 0);
    assert(status.subsystem_state == SubsystemStateStopped);

    /* Query filesystem mounts */
    waddle_daemon_fs_list_resp_t fs_list;
    memset(&fs_list, 0, sizeof(fs_list));
    assert(waddle_client_fs_list(client_fd, &fs_list) == 0);

    /* Stop command while stopped */
    waddle_daemon_result_resp_t stop_resp;
    memset(&stop_resp, 0, sizeof(stop_resp));
    assert(waddle_client_stop(client_fd, 0, 5, &stop_resp) == 0);
    assert(stop_resp.status_code == 0);
    assert(waddle_client_stop(client_fd, WaddleStopGracefulOnly, 5, &stop_resp) == 0);
    assert(stop_resp.status_code == 0);
    /* The additive request requires the complete fixed stop payload. */
    assert(waddle_daemon_send_msg(client_fd, DaemonMsgStopGracefulReq, 999, NULL, 0) == 0);
    waddle_daemon_header_t invalid_header;
    assert(waddle_daemon_recv_msg(client_fd, &invalid_header, &stop_resp, sizeof(stop_resp), 5000) == 0);
    assert(invalid_header.msg_type == DaemonMsgStopResp && stop_resp.status_code == EINVAL);
    waddle_daemon_stop_req_t malformed = {.force = 1, .timeout_sec = 5};
    assert(waddle_daemon_send_msg(client_fd, DaemonMsgStopGracefulReq, 1000, &malformed, sizeof(malformed)) == 0);
    assert(waddle_daemon_recv_msg(client_fd, &invalid_header, &stop_resp, sizeof(stop_resp), 5000) == 0);
    assert(stop_resp.status_code == EINVAL);

    /* Kill command */
    waddle_daemon_result_resp_t kill_resp;
    memset(&kill_resp, 0, sizeof(kill_resp));
    assert(waddle_client_kill(client_fd, &kill_resp) == 0);
    assert(kill_resp.status_code == 0);

    /* High-level commands */
    assert(waddle_client_cmd_status(ctx.sock_path, 0) == 0);
    assert(waddle_client_cmd_status(ctx.sock_path, 1) == 0);
    assert(waddle_client_cmd_fs(ctx.sock_path) == 0);
    assert(waddle_client_cmd_stop(ctx.sock_path, 0, 5) == 0);

    close(client_fd);

    /* Shut down server */
    ctx.stop_flag = 1;
    pthread_join(ctx.thread, NULL);

    unlink(ctx.sock_path);
    char lock_path[160];
    snprintf(lock_path, sizeof(lock_path), "%s/waddle.lock", ctx.runtime_dir);
    unlink(lock_path);
    rmdir(ctx.runtime_dir);
}

int main(void) {
    test_client_path_helpers();
    test_client_offline();
    test_client_with_server();
    printf("All test_daemon_client tests passed successfully!\n");
    return 0;
}
