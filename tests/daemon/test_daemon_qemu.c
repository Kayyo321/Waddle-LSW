/**
 * @file test_daemon_qemu.c
 * @brief Unit tests for QEMU process manager and QMP JSON-RPC client.
 */

#include "daemon_config.h"
#include "daemon_qemu.h"
#include "daemon_qmp.h"
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void test_qemu_build_args(void) {
    daemon_config_t cfg;
    daemon_config_init_defaults(&cfg);
    cfg.vcpus = 6;
    cfg.memory_mb = 8192;
    cfg.vsock_cid = 4;
    strncpy(cfg.disk_image, "/var/vm/windows.qcow2", sizeof(cfg.disk_image) - 1);

    char *argv[QemuMaxArgs];
    memset(argv, 0, sizeof(argv));

    int argc = qemu_build_args(&cfg, "/run/qmp.sock", "/run/virtiofsd.sock", "/run/qemu.pid", argv, QemuMaxArgs);
    assert(argc > 15);
    assert(argv[argc] == NULL);

    int found_smp = 0;
    int found_mem = 0;
    int found_vsock = 0;
    int found_qmp = 0;
    int found_fs = 0;
    int found_drive = 0;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-smp") == 0 && i + 1 < argc && strcmp(argv[i + 1], "6") == 0) {
            found_smp = 1;
        }
        if (strcmp(argv[i], "-m") == 0 && i + 1 < argc && strcmp(argv[i + 1], "8192M") == 0) {
            found_mem = 1;
        }
        if (strstr(argv[i], "guest-cid=4") != NULL) {
            found_vsock = 1;
        }
        if (strstr(argv[i], "/run/qmp.sock") != NULL) {
            found_qmp = 1;
        }
        if (strstr(argv[i], "tag=waddle_fs") != NULL) {
            found_fs = 1;
        }
        if (strstr(argv[i], "file=/var/vm/windows.qcow2") != NULL) {
            found_drive = 1;
        }
    }

    assert(found_smp == 1);
    assert(found_mem == 1);
    assert(found_vsock == 1);
    assert(found_qmp == 1);
    assert(found_fs == 1);
    assert(found_drive == 1);

    qemu_free_args(argv, (size_t)argc);
}

typedef struct mock_qmp_server_t {
    int listen_fd;
    char sock_path[108];
    pthread_t thread;
    volatile int stop;
} mock_qmp_server_t;

static void *mock_qmp_thread(void *arg) {
    mock_qmp_server_t *srv = (mock_qmp_server_t *)arg;

    struct pollfd pfd = {srv->listen_fd, POLLIN, 0};
    if (poll(&pfd, 1, 2000) <= 0) {
        return NULL;
    }

    int client_fd = accept(srv->listen_fd, NULL, NULL);
    if (client_fd < 0) {
        return NULL;
    }

    /* Send initial greeting */
    const char *greeting = "{\"QMP\": {\"version\": {\"qemu\": {\"major\": 8}}, \"capabilities\": []}}\r\n";
    (void)write(client_fd, greeting, strlen(greeting));

    char buf[1024];
    while (!srv->stop) {
        pfd.fd = client_fd;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, 500);
        if (pr <= 0) continue;

        ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = '\0';

        if (strstr(buf, "\"qmp_capabilities\"") != NULL) {
            const char *resp = "{\"return\": {}}\r\n";
            (void)write(client_fd, resp, strlen(resp));
        } else if (strstr(buf, "\"query-status\"") != NULL) {
            /* Also emit an asynchronous event first to verify parser skips events */
            const char *event = "{\"event\": \"NIC_RX_FILTER_CHANGED\"}\r\n";
            (void)write(client_fd, event, strlen(event));

            const char *resp = "{\"return\": {\"status\": \"running\", \"running\": true}}\r\n";
            (void)write(client_fd, resp, strlen(resp));
        } else if (strstr(buf, "\"system_powerdown\"") != NULL) {
            const char *resp = "{\"return\": {}}\r\n";
            (void)write(client_fd, resp, strlen(resp));
        } else if (strstr(buf, "\"quit\"") != NULL) {
            const char *resp = "{\"return\": {}}\r\n";
            (void)write(client_fd, resp, strlen(resp));
            break;
        }
    }

    close(client_fd);
    return NULL;
}

static void test_qmp_client(void) {
    mock_qmp_server_t srv;
    memset(&srv, 0, sizeof(srv));

    snprintf(srv.sock_path, sizeof(srv.sock_path), "/tmp/waddle_qmp_test_%d.sock", (int)getpid());
    unlink(srv.sock_path);

    srv.listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(srv.listen_fd >= 0);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", srv.sock_path);

    assert(bind(srv.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(srv.listen_fd, 1) == 0);

    assert(pthread_create(&srv.thread, NULL, mock_qmp_thread, &srv) == 0);

    /* Test QMP client connection */
    qmp_client_t client;
    assert(qmp_connect(&client, srv.sock_path, 2000) == 0);
    assert(client.socket_fd >= 0);

    /* Capabilities handshake */
    assert(qmp_negotiate_capabilities(&client) == 0);
    assert(client.capabilities_negotiated == true);

    /* Status query */
    int is_running = 0;
    assert(qmp_query_status(&client, &is_running) == 0);
    assert(is_running == 1);

    /* System powerdown */
    assert(qmp_system_powerdown(&client) == 0);

    /* Quit */
    assert(qmp_quit(&client) == 0);

    qmp_close(&client);

    srv.stop = 1;
    pthread_join(srv.thread, NULL);
    close(srv.listen_fd);
    unlink(srv.sock_path);
}

static void test_qemu_process_lifecycle(void) {
    daemon_config_t cfg;
    daemon_config_init_defaults(&cfg);

    char log_path[128];
    snprintf(log_path, sizeof(log_path), "/tmp/waddle_qemu_log_%d.log", (int)getpid());
    unlink(log_path);

    qemu_process_t proc;
    memset(&proc, 0, sizeof(proc));

    /* Spawn /bin/sleep as mock hypervisor binary */
    assert(qemu_spawn(&proc, &cfg, "/tmp/dummy_qmp.sock", NULL, log_path, "/bin/sleep") == 0);
    assert(proc.pid > 0);
    assert(proc.is_running == 1);

    /* Poll status - should still be running */
    assert(qemu_poll_status(&proc) == 1);

    /* Force kill */
    assert(qemu_kill(&proc) == 0);
    assert(proc.is_running == 0);
    assert(proc.pid == 0);

    /* Poll status - should be 0 */
    assert(qemu_poll_status(&proc) == 0);

    qemu_cleanup(&proc);
    unlink(log_path);
}

int main(void) {
    test_qemu_build_args();
    test_qmp_client();
    test_qemu_process_lifecycle();
    printf("test_daemon_qemu: all QEMU argument builder, process lifecycle, and QMP client tests passed\n");
    return 0;
}
