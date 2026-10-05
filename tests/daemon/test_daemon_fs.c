/**
 * @file test_daemon_fs.c
 * @brief Unit tests for VirtIO-FS lifecycle supervisor and path translation bridge.
 */

#include "daemon_config.h"
#include "daemon_fs.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_build_args(void) {
    char *argv[VirtiofsMaxArgs];
    memset(argv, 0, sizeof(argv));

    int argc = daemon_fs_build_args("/tmp/vfs.sock", "/home/dev", "none", "auto", argv, VirtiofsMaxArgs);
    assert(argc == 5);
    assert(strcmp(argv[0], "virtiofsd") == 0);
    assert(strcmp(argv[1], "--socket-path=/tmp/vfs.sock") == 0);
    assert(strcmp(argv[2], "--shared-dir=/home/dev") == 0);
    assert(strcmp(argv[3], "--sandbox=none") == 0);
    assert(strcmp(argv[4], "--cache=auto") == 0);
    assert(argv[argc] == NULL);

    daemon_fs_free_args(argv, (size_t)argc);
}

static void test_mounts_to_rules(void) {
    waddle_daemon_fs_mount_t mounts[2];
    memset(mounts, 0, sizeof(mounts));

    strncpy(mounts[0].host_path, "/home/dev", sizeof(mounts[0].host_path) - 1);
    strncpy(mounts[0].guest_drive, "Z:\\", sizeof(mounts[0].guest_drive) - 1);
    mounts[0].read_only = 0;

    strncpy(mounts[1].host_path, "/mnt/storage", sizeof(mounts[1].host_path) - 1);
    strncpy(mounts[1].guest_drive, "X:\\", sizeof(mounts[1].guest_drive) - 1);
    mounts[1].read_only = 1;

    path_rule_t rules[4];
    int count = daemon_fs_mounts_to_rules(mounts, 2, rules, 4);
    assert(count == 2);
    assert(strcmp(rules[0].source, "/home/dev") == 0);
    assert(strcmp(rules[0].target, "Z:\\") == 0);
    assert(strcmp(rules[1].source, "/mnt/storage") == 0);
    assert(strcmp(rules[1].target, "X:\\") == 0);
}

static void test_path_translation(void) {
    daemon_config_t cfg;
    daemon_config_init_defaults(&cfg);

    /* Default config maps $HOME to Z:\ */
    /* Add explicit mounts for testing */
    cfg.mount_count = 2;
    strncpy(cfg.mounts[0].host_path, "/home/dev", sizeof(cfg.mounts[0].host_path) - 1);
    strncpy(cfg.mounts[0].guest_drive, "Z:\\", sizeof(cfg.mounts[0].guest_drive) - 1);
    strncpy(cfg.mounts[1].host_path, "/mnt/work", sizeof(cfg.mounts[1].host_path) - 1);
    strncpy(cfg.mounts[1].guest_drive, "W:\\", sizeof(cfg.mounts[1].guest_drive) - 1);

    /* Test mapping primary mount */
    char *translated = daemon_fs_translate_path("/home/dev/projects/waddle", &cfg);
    assert(translated != NULL);
    assert(strcmp(translated, "Z:\\projects\\waddle") == 0);
    free(translated);

    /* Test mapping secondary mount */
    translated = daemon_fs_translate_path("/mnt/work/data.bin", &cfg);
    assert(translated != NULL);
    assert(strcmp(translated, "W:\\data.bin") == 0);
    free(translated);

    /* Test relative path */
    translated = daemon_fs_translate_path("relative/path", &cfg);
    assert(translated != NULL);
    assert(strcmp(translated, "relative") != -1);
    free(translated);
}

static void test_invalid_spawn(void) {
    virtiofs_process_t proc;
    memset(&proc, 0, sizeof(proc));

    /* Nonexistent shared directory must fail */
    assert(daemon_fs_spawn(&proc, "/tmp/sock", "/nonexistent/path/waddle_test",
                           "/tmp/log", NULL, 100) == -1);
    assert(errno == ENOENT);
}

static void test_daemon_fs_find_binary(void) {
    char bin_path[WaddleMaxPathLen];
    int res = daemon_fs_find_binary(bin_path, sizeof(bin_path));
    assert(res == 0);
    assert(strstr(bin_path, "virtiofsd") != NULL);
    assert(access(bin_path, X_OK) == 0);
}

int main(void) {
    test_daemon_fs_find_binary();
    test_build_args();
    test_mounts_to_rules();
    test_path_translation();
    test_invalid_spawn();
    printf("test_daemon_fs: all VirtIO-FS argument builder and path translation tests passed\n");
    return 0;
}
