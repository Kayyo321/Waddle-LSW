/**
 * @file test_daemon_config.c
 * @brief C unit tests verifying daemon configuration loading and C ABI interop.
 */

#include "daemon_config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_defaults(void) {
    daemon_config_t cfg;
    daemon_config_init_defaults(&cfg);

    assert(cfg.memory_mb == 4096);
    assert(cfg.vcpus == 4);
    assert(cfg.vsock_cid == 3);
    assert(cfg.vsock_port == 5242);
    assert(cfg.start_timeout_sec == 60);
    assert(cfg.stop_timeout_sec == 15);
    assert(strcmp(cfg.default_shell, "powershell.exe") == 0);
    assert(cfg.mount_count == 1);
    assert(strcmp(cfg.mounts[0].guest_drive, "Z:\\") == 0);
    assert(cfg.mounts[0].read_only == 0);
    assert(daemon_config_validate(&cfg) == 0);
}

static void test_parse_string(void) {
    daemon_config_t cfg;
    daemon_config_init_defaults(&cfg);

    const char *ini =
        "[subsystem]\n"
        "memory_mb = 2048\n"
        "vcpus = 2\n"
        "vsock_cid = 5\n"
        "vsock_port = 6000\n"
        "default_shell = cmd.exe\n"
        "disk_image = /tmp/test.qcow2\n"
        "\n"
        "[filesystem]\n"
        "export_1 = /var/test:Y:\\:ro\n"
        "\n"
        "[timeouts]\n"
        "start_timeout = 45\n"
        "stop_timeout = 10\n";

    assert(daemon_config_parse_string(&cfg, ini, strlen(ini)) == 0);
    assert(cfg.memory_mb == 2048);
    assert(cfg.vcpus == 2);
    assert(cfg.vsock_cid == 5);
    assert(cfg.vsock_port == 6000);
    assert(strcmp(cfg.default_shell, "cmd.exe") == 0);
    assert(strcmp(cfg.disk_image, "/tmp/test.qcow2") == 0);
    assert(cfg.mount_count == 1);
    assert(strcmp(cfg.mounts[0].host_path, "/var/test") == 0);
    assert(strcmp(cfg.mounts[0].guest_drive, "Y:\\") == 0);
    assert(cfg.mounts[0].read_only == 1);
    assert(cfg.start_timeout_sec == 45);
    assert(cfg.stop_timeout_sec == 10);
    assert(daemon_config_validate(&cfg) == 0);
}

static void test_export_parser(void) {
    waddle_daemon_fs_mount_t mount;
    memset(&mount, 0, sizeof(mount));

    assert(daemon_config_parse_export("/home/user:Z:\\:rw", &mount) == 0);
    assert(strcmp(mount.host_path, "/home/user") == 0);
    assert(strcmp(mount.guest_drive, "Z:\\") == 0);
    assert(mount.read_only == 0);

    assert(daemon_config_parse_export("/data:X:\\:ro", &mount) == 0);
    assert(strcmp(mount.host_path, "/data") == 0);
    assert(strcmp(mount.guest_drive, "X:\\") == 0);
    assert(mount.read_only == 1);

    /* Missing mode defaults to rw */
    assert(daemon_config_parse_export("/data:X:\\", &mount) == 0);
    assert(mount.read_only == 0);

    /* Invalid export formats */
    assert(daemon_config_parse_export(NULL, &mount) == -1);
    assert(daemon_config_parse_export("relative:Z:\\", &mount) == -1);
    assert(daemon_config_parse_export("/home:bad", &mount) == -1);
    assert(daemon_config_parse_export("/home:Z:\\:unknown", &mount) == -1);
}

static void test_file_loading(void) {
    daemon_config_t cfg;
    daemon_config_init_defaults(&cfg);

    /* Missing file is non-fatal: returns 0 with defaults preserved */
    assert(daemon_config_load_file(&cfg, "/nonexistent/waddle/config.ini") == 0);
    assert(cfg.memory_mb == 4096);

    /* Create temporary config file */
    char tmp_template[] = "/tmp/waddle_test_cfg_XXXXXX";
    int fd = mkstemp(tmp_template);
    assert(fd >= 0);

    const char *content =
        "[subsystem]\n"
        "memory_mb = 16384\n"
        "vcpus = 8\n";
    ssize_t written = write(fd, content, strlen(content));
    assert(written == (ssize_t)strlen(content));
    close(fd);

    assert(daemon_config_load_file(&cfg, tmp_template) == 0);
    assert(cfg.memory_mb == 16384);
    assert(cfg.vcpus == 8);

    unlink(tmp_template);
}

static void test_validation_ranges(void) {
    daemon_config_t cfg;
    daemon_config_init_defaults(&cfg);

    /* Test null check */
    assert(daemon_config_validate(NULL) == -1);

    /* Memory out of range */
    cfg.memory_mb = 128;
    assert(daemon_config_validate(&cfg) == -1);
    cfg.memory_mb = 131072;
    assert(daemon_config_validate(&cfg) == -1);
    cfg.memory_mb = 4096;

    /* VCPUs out of range */
    cfg.vcpus = 0;
    assert(daemon_config_validate(&cfg) == -1);
    cfg.vcpus = 256;
    assert(daemon_config_validate(&cfg) == -1);
    cfg.vcpus = 4;

    /* CID out of range */
    cfg.vsock_cid = 0;
    assert(daemon_config_validate(&cfg) == -1);
    cfg.vsock_cid = 2;
    assert(daemon_config_validate(&cfg) == -1);
    cfg.vsock_cid = 3;

    /* Timeout out of range */
    cfg.start_timeout_sec = 0;
    assert(daemon_config_validate(&cfg) == -1);
    cfg.start_timeout_sec = 60;

    assert(daemon_config_validate(&cfg) == 0);
}

int main(void) {
    test_defaults();
    test_parse_string();
    test_export_parser();
    test_file_loading();
    test_validation_ranges();
    printf("test_daemon_config: all daemon configuration and INI parsing tests passed\n");
    return 0;
}
