/**
 * @file test_daemon_server.c
 * @brief Unit tests for daemon supervisor state machine, lockfile, and server event loop.
 */

#include "daemon_server.h"
#include "daemon_state.h"
#include "daemon_device.h"
#include <fcntl.h>
#include <sys/stat.h>
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

/** @brief Verify final config reload and nested lock lifetime without starting a VM. */
static void test_named_startup_handoff(void) {
    char root[] = "/tmp/waddle_handoff_XXXXXX";
    assert(mkdtemp(root) != NULL);
    const char *keys[] = {"XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_RUNTIME_DIR"};
    char *saved[3] = {NULL, NULL, NULL};
    for (size_t i = 0; i < 3; i++) {
        if (getenv(keys[i]) != NULL) { saved[i] = strdup(getenv(keys[i])); assert(saved[i]); }
        assert(setenv(keys[i], root, 1) == 0);
    }
    int registry_fd = daemon_device_registry_lock(1);
    assert(registry_fd >= 0);
    char directory[1024], profile[1100];
    assert(daemon_device_get_config_dir(directory, sizeof(directory)) == 0);
    assert(mkdir(directory, 0700) == 0);
    assert(snprintf(profile, sizeof(profile), "%s/alpha.ini", directory) < (int)sizeof(profile));
    FILE *file = fopen(profile, "w");
    assert(file != NULL);
    assert(fputs("[subsystem]\nvsock_cid=33\ndisk_image=/tmp/test.qcow2\nvcpus=2\n", file) >= 0);
    assert(fclose(file) == 0);
    assert(chmod(profile, 0600) == 0);
    close(registry_fd);

    /* Independent nested readers must not release the outer OFD lock. */
    registry_fd = daemon_device_registry_lock(0);
    assert(registry_fd >= 0);
    device_info_t info;
    assert(daemon_device_find("alpha", &info) == 0);
    char lock_path[1100];
    assert(snprintf(lock_path, sizeof(lock_path), "%s/waddle/registry.lock", root) < (int)sizeof(lock_path));
    int probe = open(lock_path, O_RDWR | O_CLOEXEC);
    assert(probe >= 0);
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    assert(fcntl(probe, F_OFD_SETLK, &lock) == -1 && errno == EAGAIN);
    close(registry_fd);
    assert(fcntl(probe, F_OFD_SETLK, &lock) == 0);
    close(probe);

    char socket_path[1024];
    assert(daemon_device_get_socket_path("alpha", socket_path, sizeof(socket_path)) == 0);
    *strrchr(socket_path, '/') = '\0';
    daemon_state_t state;
    assert(daemon_state_init(&state, socket_path) == 0);
    assert(state.config.vcpus == 2);
    file = fopen(profile, "a");
    assert(file != NULL && fputs("vcpus=7\n", file) >= 0 && fclose(file) == 0);
    assert(daemon_state_acquire_lock(&state) == 0);
    assert(state.config.vcpus == 7);
    daemon_state_cleanup(&state);
    assert(daemon_state_init(&state, socket_path) == 0);
    assert(unlink(profile) == 0);
    assert(daemon_state_acquire_lock(&state) == -1 && errno == ENOENT);
    assert(state.lock_fd == -1);
    daemon_state_cleanup(&state);
    for (size_t i = 0; i < 3; i++) {
        if (saved[i]) { assert(setenv(keys[i], saved[i], 1) == 0); free(saved[i]); saved[i] = NULL; }
        else assert(unsetenv(keys[i]) == 0);
    }
    char cleanup[1200];
    assert(snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root) < (int)sizeof(cleanup));
    assert(system(cleanup) == 0);
}

static void test_lockfile_concurrency(void) {
    char runtime_dir[128];
    snprintf(runtime_dir, sizeof(runtime_dir), "/tmp/waddle_test_lock_%d", (int)getpid());

    daemon_state_t s1;
    assert(daemon_state_init(&s1, runtime_dir) == 0);
    assert(daemon_state_acquire_lock(&s1) == 0);
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(listener >= 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    assert(strlen(s1.daemon_sock_path) < sizeof(address.sun_path));
    memcpy(address.sun_path, s1.daemon_sock_path, strlen(s1.daemon_sock_path) + 1);
    assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);

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
            daemon_state_cleanup(&s2);
            _exit(42); /* Expected collision */
        }
        _exit(2);
    }

    int status = 0;
    waitpid(child, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 42);
    assert(access(s1.daemon_sock_path, F_OK) == 0);
    close(listener);
    assert(unlink(s1.daemon_sock_path) == 0);

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

    assert(setenv("WADDLE_MOCK_GUEST_SOCK", "/dev/null", 1) == 0);
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

    /* Running mock state refuses shutdown even with no child PID. */
    waddle_daemon_start_req_t start_req = {0};
    waddle_daemon_result_resp_t result;
    assert(waddle_daemon_send_msg(client_fd, DaemonMsgStartReq, 120, &start_req, sizeof(start_req)) == 0);
    assert(waddle_daemon_recv_msg(client_fd, &hdr, &result, sizeof(result), 2000) == 0);
    assert(result.status_code == 0 && result.subsystem_state == SubsystemStateRunning);
    assert(waddle_daemon_send_msg(client_fd, DaemonMsgShutdownReq, 121, NULL, 0) == 0);
    assert(waddle_daemon_recv_msg(client_fd, &hdr, &result, sizeof(result), 2000) == 0);
    assert(hdr.msg_type == DaemonMsgShutdownResp && hdr.sequence == 121);
    assert(result.status_code == EBUSY);

    /* 3. Kill Request */
    assert(waddle_daemon_send_msg(client_fd, DaemonMsgKillReq, 103, NULL, 0) == 0);
    waddle_daemon_result_resp_t kill_resp;
    assert(waddle_daemon_recv_msg(client_fd, &hdr, &kill_resp, sizeof(kill_resp), 2000) == 0);
    assert(hdr.msg_type == DaemonMsgKillResp);
    assert(hdr.sequence == 103);
    assert(kill_resp.status_code == 0);
    assert(kill_resp.subsystem_state == SubsystemStateStopped);

    /* Malformed shutdown stays connected; idle shutdown replies before exiting. */
    uint8_t unexpected = 1;
    assert(waddle_daemon_send_msg(client_fd, DaemonMsgShutdownReq, 122, &unexpected, 1) == 0);
    assert(waddle_daemon_recv_msg(client_fd, &hdr, &result, sizeof(result), 2000) == 0);
    assert(result.status_code == EINVAL);
    assert(waddle_daemon_send_msg(client_fd, DaemonMsgShutdownReq, 123, NULL, 0) == 0);
    assert(waddle_daemon_recv_msg(client_fd, &hdr, &result, sizeof(result), 2000) == 0);
    assert(hdr.msg_type == DaemonMsgShutdownResp && hdr.sequence == 123 && result.status_code == 0);
    close(client_fd);
    pthread_join(ctx.thread, NULL);
    assert(access(sock_path, F_OK) != 0);
    unsetenv("WADDLE_MOCK_GUEST_SOCK");
}

int main(void) {
    test_named_startup_handoff();
    test_lockfile_concurrency();
    test_server_ipc();
    printf("test_daemon_server: all daemon lockfile, state machine, and IPC server tests passed\n");
    return 0;
}
