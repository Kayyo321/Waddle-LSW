/**
 * @file test_daemon_qemu.c
 * @brief Unit tests for QEMU process manager and QMP JSON-RPC client.
 */

#include "daemon_config.h"
#include "daemon_device.h"
#include "daemon_qemu.h"
#include "daemon_qmp.h"
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/** @brief Test-only CUPS lstat result; -1 delegates to the real filesystem. */
static int cups_test_mode = -1;

/** @brief Linker-provided real lstat; arguments are borrowed for this call. */
int __real_lstat(const char *path, struct stat *status);

/** @brief Override only the frozen CUPS path to prove both device branches. */
int __wrap_lstat(const char *path, struct stat *status) {
    if (cups_test_mode >= 0 && strcmp(path, "/run/cups/cups.sock") == 0) {
        if (cups_test_mode == 0) { errno = ENOENT; return -1; }
        memset(status, 0, sizeof(*status));
        status->st_mode = (mode_t)cups_test_mode;
        return 0;
    }
    return __real_lstat(path, status);
}

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

static void test_av_args(void) {
    daemon_config_t config;
    daemon_config_init_defaults(&config);
    config.av_enabled = 1;
    strcpy(config.av_shm_path, "/dev/shm/waddle-av-test");
    strcpy(config.av_gpu_bdf, "0000:0e:00.0");
    strcpy(config.av_gpu_mdev_uuid, "12345678-1234-5678-9abc-123456789abc");
    char *argv[QemuMaxArgs];
    int count = qemu_build_args(&config, "/tmp/qmp", NULL, NULL, argv, QemuMaxArgs);
    assert(count > 0);
    int ivshmem = 0, memory = 0, audio = 0, endpoint = 0, gpu = 0;
    int pipewire_backend = 0;
    int hda_count = 0;
    for (int i = 0; i < count; ++i) {
        if (!strcmp(argv[i], "ivshmem-plain,memdev=waddle_av")) ivshmem = 1;
        if (strstr(argv[i], "size=2147483648,mem-path=/dev/shm/waddle-av-test,share=on")) memory = 1;
        if (!strcmp(argv[i], "pipewire,id=waddle_audio,out.name=waddle-discard.default")) { audio = 1; pipewire_backend = 1; }
        if (!strcmp(argv[i], "hda-duplex,audiodev=waddle_audio")) { endpoint = 1; hda_count++; }
        if (!strcmp(argv[i], "ich9-intel-hda")) hda_count++;
        if (!strcmp(argv[i], "vfio-pci,sysfsdev=/sys/bus/mdev/devices/12345678-1234-5678-9abc-123456789abc,display=off")) gpu = 1;
        assert(!strstr(argv[i], "vfio-pci,host="));
    }
    assert(ivshmem && memory && audio && pipewire_backend && endpoint && gpu);
    assert(hda_count == 2);
    qemu_free_args(argv, (size_t)count);
    config.av_gpu_bdf[0] = '\0';
    count = qemu_build_args(&config, "/tmp/qmp", NULL, NULL, argv, QemuMaxArgs);
    assert(count > 0); /* UUID alone is a complete assignment. */
    qemu_free_args(argv, (size_t)count);
    config.av_gpu_mdev_uuid[0] = '\0';
    strcpy(config.av_gpu_bdf, "0000:0e:00.0");
    assert(qemu_build_args(&config, "/tmp/qmp", NULL, NULL, argv, QemuMaxArgs) == -1);
    assert(argv[0] == NULL);
    config.av_gpu_bdf[0] = '\0';
    count = qemu_build_args(&config, "/tmp/qmp", NULL, NULL, argv, QemuMaxArgs);
    assert(count > 0);
    int refresh = 0;
    for (int i = 0; i < count; ++i)
        if (!strcmp(argv[i], "VGA,refresh_rate=144,xres=1920,yres=1080,vgamem_mb=64")) refresh = 1;
    assert(refresh);
    qemu_free_args(argv, (size_t)count);
    strcpy(config.av_shm_path, "/tmp/x,share=off");
    assert(qemu_build_args(&config, "/tmp/qmp", NULL, NULL, argv, QemuMaxArgs) == -1);
    assert(argv[0] == NULL);
}

static int find_arg(char *const argv[], int argc, const char *needle) {
    for (int i = 0; i < argc; ++i) if (strcmp(argv[i], needle) == 0) return i;
    return -1;
}

static int count_arg(char *const argv[], int argc, const char *needle) {
    int count = 0;
    for (int i = 0; i < argc; ++i) if (strcmp(argv[i], needle) == 0) count++;
    return count;
}

static void test_seamless_qemu_args(void) {
    daemon_config_t config;
    daemon_config_init_defaults(&config);
    config.network_cups_forward = 0; /* The fixed host socket is environment-dependent. */
    char *argv[QemuMaxArgs];
    memset(argv, 0, sizeof(argv));

    int count = qemu_build_args(&config, "/tmp/qmp", NULL, NULL, argv, QemuMaxArgs);
    assert(count > 0 && argv[count] == NULL);
    int netdev = find_arg(argv, count, "-netdev");
    int nic = find_arg(argv, count, "virtio-net-pci,netdev=waddle_net");
    int xhci = find_arg(argv, count, "qemu-xhci,id=waddle_xhci");
    int audio = find_arg(argv, count, "pipewire,id=waddle_audio,out.name=waddle-discard.default");
    assert(netdev >= 0 && strcmp(argv[netdev + 1], "user,id=waddle_net") == 0);
    assert(nic > netdev && xhci > nic && audio > xhci);
    assert(find_arg(argv, count, "-nic") < 0);
    assert(count_arg(argv, count, "ich9-intel-hda") == 1);
    assert(count_arg(argv, count, "hda-duplex,audiodev=waddle_audio") == 1);
    qemu_free_args(argv, (size_t)count);

    config.network_mode = NetworkModeNone;
    config.usb_controller = 0;
    config.audio_backend = AudioBackendNone;
    config.audio_microphone = 0; /* Silent backend retains HDA; mic has no capture. */
    count = qemu_build_args(&config, "/tmp/qmp", NULL, NULL, argv, QemuMaxArgs);
    assert(count > 0 && argv[count] == NULL);
    int no_network = find_arg(argv, count, "-nic");
    int silent_audio = find_arg(argv, count, "none,id=waddle_audio");
    assert(no_network >= 0 && strcmp(argv[no_network + 1], "none") == 0);
    assert(find_arg(argv, count, "-netdev") < 0);
    assert(find_arg(argv, count, "qemu-xhci,id=waddle_xhci") < 0);
    assert(silent_audio >= 0);
    assert(count_arg(argv, count, "ich9-intel-hda") == 1);
    assert(count_arg(argv, count, "hda-duplex,audiodev=waddle_audio") == 1);
    qemu_free_args(argv, (size_t)count);

    config.network_mode = NetworkModeUser;
    config.usb_controller = 1;
    config.audio_backend = AudioBackendPipewire;
    config.audio_microphone = 0;
    count = qemu_build_args(&config, "/tmp/qmp", NULL, NULL, argv, QemuMaxArgs);
    assert(count > 0 && find_arg(argv, count, "pipewire,id=waddle_audio,out.name=waddle-discard.default,in.voices=0") >= 0);
    qemu_free_args(argv, (size_t)count);

    config.network_cups_forward = 1;
    const int cups_modes[] = { 0, S_IFREG | 0600, S_IFLNK | 0777, S_IFSOCK | 0600 };
    for (size_t index = 0; index < sizeof(cups_modes) / sizeof(cups_modes[0]); ++index) {
        cups_test_mode = cups_modes[index];
        int cups_socket = S_ISSOCK(cups_test_mode);
        count = qemu_build_args(&config, "/tmp/qmp", NULL, NULL, argv, QemuMaxArgs);
        assert(count > 0);
        int cups_chardev = find_arg(argv, count, "socket,id=waddle_cups,path=/run/cups/cups.sock");
        int netdev_index = find_arg(argv, count, "-netdev");
        assert((cups_chardev >= 0) == cups_socket);
        assert((strstr(argv[netdev_index + 1], "guestfwd=tcp:10.0.2.100:631-chardev:waddle_cups") != NULL) == cups_socket);
        qemu_free_args(argv, (size_t)count);
    }
    cups_test_mode = -1;

    config.audio_microphone = 1;
    count = qemu_build_args_for_device(&config, "alpha", "/tmp/qmp", NULL, NULL, argv, QemuMaxArgs);
    assert(count > 0 && find_arg(argv, count, "pipewire,id=waddle_audio,out.name=waddle-discard.alpha") >= 0);
    assert(find_arg(argv, count, "pipewire,id=waddle_audio,out.name=waddle-discard.beta") < 0);
    qemu_free_args(argv, (size_t)count);
    count = qemu_build_args_for_device(&config, "beta", "/tmp/qmp", NULL, NULL, argv, QemuMaxArgs);
    assert(count > 0 && find_arg(argv, count, "pipewire,id=waddle_audio,out.name=waddle-discard.beta") >= 0);
    qemu_free_args(argv, (size_t)count);

    char too_long_name[WaddleMaxDeviceNameLen + 1];
    memset(too_long_name, 'a', sizeof(too_long_name) - 1);
    too_long_name[sizeof(too_long_name) - 1] = '\0';
    const char *invalid_names[] = { NULL, "", "../escape", "name with space", too_long_name };
    for (size_t index = 0; index < sizeof(invalid_names) / sizeof(invalid_names[0]); ++index) {
        memset(argv, 0, sizeof(argv));
        assert(qemu_build_args_for_device(&config, invalid_names[index], "/tmp/qmp", NULL,
                                          NULL, argv, QemuMaxArgs) == -1);
        assert(errno == EINVAL && argv[0] == NULL);
    }
}

static void test_qemu_argument_failure_cleanup_and_path_bounds(void) {
    daemon_config_t config;
    daemon_config_init_defaults(&config);
    char *argv[QemuMaxArgs];
    memset(argv, 0, sizeof(argv));
    assert(qemu_build_args(&config, "/tmp/qmp", NULL, NULL, argv, 32) == -1);
    assert(errno == E2BIG && argv[0] == NULL);
    for (size_t i = 0; i < QemuMaxArgs; ++i) assert(argv[i] == NULL);

    char too_long_path[WaddleMaxPathLen + 1];
    memset(too_long_path, 'x', sizeof(too_long_path) - 1);
    too_long_path[sizeof(too_long_path) - 1] = '\0';
    assert(qemu_build_args(&config, too_long_path, NULL, NULL, argv, QemuMaxArgs) == -1);
    assert(errno == EINVAL && argv[0] == NULL);
    assert(qemu_build_args(&config, "relative-qmp.sock", NULL, NULL, argv, QemuMaxArgs) == -1);
    assert(errno == EINVAL && argv[0] == NULL);

    config.av_enabled = 1;
    config.av_uefi = 1;
    strcpy(config.av_shm_path, "/dev/shm/waddle-av-capacity-test");
    strcpy(config.av_gpu_bdf, "0000:0e:00.0");
    strcpy(config.av_gpu_mdev_uuid, "12345678-1234-5678-9abc-123456789abc");
    config.network_cups_forward = 1;
    config.audio_backend = AudioBackendPipewire;
    int count = qemu_build_args_for_device(&config, "capacity", "/tmp/qmp",
                                          "/tmp/virtiofsd.sock", "/tmp/qemu.pid",
                                          argv, QemuMaxArgs);
    assert(count > 0 && argv[count] == NULL);
    /* Current maximum options plus seven more four-argument mount groups fit. */
    assert((size_t)count + (7U * 4U) < QemuMaxArgs);
    qemu_free_args(argv, (size_t)count);
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

static void test_qemu_find_binary(void) {
    char bin_path[WaddleMaxPathLen];
    int res = qemu_find_binary(bin_path, sizeof(bin_path));
    assert(res == 0);
    assert(strstr(bin_path, "qemu-system-x86_64") != NULL);
    assert(access(bin_path, X_OK) == 0);
}

int main(void) {
    test_qemu_find_binary();
    test_qemu_build_args();
    test_av_args();
    test_seamless_qemu_args();
    test_qemu_argument_failure_cleanup_and_path_bounds();
    test_qmp_client();
    test_qemu_process_lifecycle();
    printf("test_daemon_qemu: all QEMU argument builder, process lifecycle, and QMP client tests passed\n");
    return 0;
}
