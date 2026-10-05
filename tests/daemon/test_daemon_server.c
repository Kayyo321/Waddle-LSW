/**
 * @file test_daemon_server.c
 * @brief Unit tests for daemon supervisor state machine, lockfile, and server event loop.
 */

#include "daemon_server.h"
#include "daemon_state.h"
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
#include <sys/wait.h>
#include <unistd.h>

static void test_lockfile_concurrency(void) {
    char runtime_dir[128];
    snprintf(runtime_dir, sizeof(runtime_dir), "/tmp/waddle_test_lock_%d", (int)getpid());

    daemon_state_t s1;
    assert(daemon_state_init(&s1, runtime_dir) == 0);
    assert(daemon_state_acquire_lock(&s1) == 0);

    /* Second instance in a separate process must fail to acquire the lock */
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        daemon_state_t s2;
        if (daemon_state_init(&s2, runtime_dir) != 0) {
            _exit(1);
        }
        int r = daemon_state_acquire_lock(&s2);
        if (r == -1 && errno == EADDRINUSE) {
            _exit(42); /* Expected collision */
        }
        _exit(2);
    }

    int status = 0;
    waitpid(child, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 42);

    /* After releasing s1, a new process can acquire */
    daemon_state_release_lock(&s1);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        daemon_state_t s3;
        if (daemon_state_init(&s3, runtime_dir) != 0) {
            _exit(1);
        }
        if (daemon_state_acquire_lock(&s3) == 0) {
            daemon_state_release_lock(&s3);
            _exit(43); /* Expected success */
        }
        _exit(3);
    }

    waitpid(child, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 43);

    daemon_state_cleanup(&s1);
    rmdir(runtime_dir);
}

typedef struct server_test_ctx_t {
    char runtime_dir[128];
    volatile sig_atomic_t stop_flag;
    pthread_t thread;
} server_test_ctx_t;

static void *server_thread_func(void *arg) {
    server_test_ctx_t *ctx = (server_test_ctx_t *)arg;
    (void)daemon_server_run(ctx->runtime_dir, &ctx->stop_flag);
    return NULL;
}

static void test_server_ipc(void) {
    server_test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    snprintf(ctx.runtime_dir, sizeof(ctx.runtime_dir), "/tmp/waddle_test_srv_%d", (int)getpid());

    assert(pthread_create(&ctx.thread, NULL, server_thread_func, &ctx) == 0);

    /* Wait for daemon.sock to appear */
    char sock_path[160];
    snprintf(sock_path, sizeof(sock_path), "%s/daemon.sock", ctx.runtime_dir);

    int client_fd = -1;
    int tries = 0;
    while (tries < 40) {
        client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (client_fd >= 0) {
            struct sockaddr_un addr;
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            snprintf(addr.sun_path, sizeof(addr.sun_path), "%.107s", sock_path);
            if (connect(client_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                break;
            }
            close(client_fd);
            client_fd = -1;
        }
        usleep(50000);
        tries++;
    }
    assert(client_fd >= 0);

    /* 1. Status Request */
    assert(waddle_daemon_send_msg(client_fd, DaemonMsgStatusReq, 101, NULL, 0) == 0);

    waddle_daemon_header_t hdr;
    waddle_daemon_status_resp_t status_resp;
    assert(waddle_daemon_recv_msg(client_fd, &hdr, &status_resp, sizeof(status_resp), 2000) == 0);
    assert(hdr.msg_type == DaemonMsgStatusResp);
    assert(hdr.sequence == 101);
    assert(status_resp.subsystem_state == SubsystemStateStopped);
    assert(status_resp.daemon_pid == (uint32_t)getpid());
    assert(status_resp.memory_mb == 4096);
    assert(status_resp.vcpus == 4);

    /* 2. Filesystem List Request */
    assert(waddle_daemon_send_msg(client_fd, DaemonMsgFsListReq, 102, NULL, 0) == 0);
    waddle_daemon_fs_list_resp_t fs_resp;
    assert(waddle_daemon_recv_msg(client_fd, &hdr, &fs_resp, sizeof(fs_resp), 2000) == 0);
    assert(hdr.msg_type == DaemonMsgFsListResp);
    assert(hdr.sequence == 102);
    assert(fs_resp.mount_count >= 1);
    assert(strcmp(fs_resp.mounts[0].guest_drive, "Z:\\") == 0);

    /* 3. Kill Request */
    assert(waddle_daemon_send_msg(client_fd, DaemonMsgKillReq, 103, NULL, 0) == 0);
    waddle_daemon_result_resp_t kill_resp;
    assert(waddle_daemon_recv_msg(client_fd, &hdr, &kill_resp, sizeof(kill_resp), 2000) == 0);
    assert(hdr.msg_type == DaemonMsgKillResp);
    assert(hdr.sequence == 103);
    assert(kill_resp.status_code == 0);
    assert(kill_resp.subsystem_state == SubsystemStateStopped);

    close(client_fd);

    /* Stop server */
    ctx.stop_flag = 1;
    pthread_join(ctx.thread, NULL);
}

int main(void) {
    test_lockfile_concurrency();
    test_server_ipc();
    printf("test_daemon_server: all daemon lockfile, state machine, and IPC server tests passed\n");
    return 0;
}
