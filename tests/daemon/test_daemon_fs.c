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
#include <sys/stat.h>
#include <unistd.h>

static char test_root[WaddleMaxPathLen];
static char shared_root[WaddleMaxPathLen];
static char nested_root[WaddleMaxPathLen];
static char other_root[WaddleMaxPathLen];

static void setup_path_tree(void) {
    snprintf(test_root, sizeof(test_root), "/tmp/waddle-paths-%ld-XXXXXX", (long)getpid());
    assert(mkdtemp(test_root) != NULL);
    assert(snprintf(shared_root, sizeof(shared_root), "%s/shared", test_root) < (int)sizeof(shared_root));
    assert(snprintf(nested_root, sizeof(nested_root), "%s/shared/nested", test_root) < (int)sizeof(nested_root));
    assert(snprintf(other_root, sizeof(other_root), "%s/other", test_root) < (int)sizeof(other_root));
    assert(mkdir(shared_root, 0700) == 0);
    assert(mkdir(nested_root, 0700) == 0);
    assert(mkdir(other_root, 0700) == 0);
}

static void remove_path_tree(void) {
    char path[WaddleMaxPathLen];
    assert(snprintf(path, sizeof(path), "%s/other", test_root) < (int)sizeof(path)); assert(rmdir(path) == 0);
    assert(snprintf(path, sizeof(path), "%s/shared/nested", test_root) < (int)sizeof(path)); assert(rmdir(path) == 0);
    assert(snprintf(path, sizeof(path), "%s/shared", test_root) < (int)sizeof(path)); assert(rmdir(path) == 0);
    assert(rmdir(test_root) == 0);
}

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
    argc = daemon_fs_build_args_for_mount("/tmp/vfs.sock", "/home/dev", "none",
                                          "auto", 1, argv, VirtiofsMaxArgs);
    assert(argc == 6 && strcmp(argv[5], "--readonly") == 0 && argv[6] == NULL);
    daemon_fs_free_args(argv, (size_t)argc);
    assert(daemon_fs_build_args_for_mount("/tmp/vfs.sock", "/home/dev", NULL,
                                           NULL, 2, argv, VirtiofsMaxArgs) == -1);
    assert(errno == EINVAL && argv[0] == NULL);
    char unterminated[WaddleMaxPathLen];
    memset(unterminated, 'x', sizeof(unterminated));
    unterminated[0] = '/';
    assert(daemon_fs_build_args_for_mount(unterminated, "/home/dev", NULL,
                                           NULL, 0, argv, VirtiofsMaxArgs) == -1);
    assert(errno == EINVAL && argv[0] == NULL);
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
    memcpy(cfg.mounts[0].host_path, shared_root, strlen(shared_root) + 1);
    strncpy(cfg.mounts[0].guest_drive, "Z:\\", sizeof(cfg.mounts[0].guest_drive) - 1);
    memcpy(cfg.mounts[1].host_path, other_root, strlen(other_root) + 1);
    strncpy(cfg.mounts[1].guest_drive, "W:\\", sizeof(cfg.mounts[1].guest_drive) - 1);

    /* Test mapping primary mount */
    char input[WaddleMaxPathLen];
    assert(snprintf(input, sizeof(input), "%s/projects/waddle", shared_root) < (int)sizeof(input));
    char *translated = daemon_fs_translate_path(input, &cfg);
    assert(translated != NULL);
    assert(strcmp(translated, "Z:\\projects\\waddle") == 0);
    free(translated);

    /* Test mapping secondary mount */
    assert(snprintf(input, sizeof(input), "%s/data.bin", other_root) < (int)sizeof(input));
    translated = daemon_fs_translate_path(input, &cfg);
    assert(translated != NULL);
    assert(strcmp(translated, "W:\\data.bin") == 0);
    free(translated);

    /* Test relative path */
    translated = daemon_fs_translate_path("relative/path", &cfg);
    assert(translated != NULL);
    assert(strcmp(translated, "relative/path") == 0);
    free(translated);
}

static void test_export_boundaries(void) {
    daemon_config_t cfg;
    daemon_config_init_defaults(&cfg);
    cfg.mount_count = 0;
    errno = 0;
    assert(daemon_fs_translate_path("/unshared/file", &cfg) == NULL);
    assert(errno == EXDEV);
    char *translated = daemon_fs_translate_path("C:\\native.exe", &cfg);
    assert(translated != NULL && strcmp(translated, "C:\\native.exe") == 0);
    free(translated);
    translated = NULL;

    cfg.mount_count = 2;
    strcpy(cfg.mounts[0].host_path, shared_root);
    strcpy(cfg.mounts[0].guest_drive, "X:\\");
    strcpy(cfg.mounts[1].host_path, nested_root);
    strcpy(cfg.mounts[1].guest_drive, "Y:\\");
    char input[WaddleMaxPathLen];
    assert(snprintf(input, sizeof(input), "%s-sibling/file", shared_root) < (int)sizeof(input));
    assert(daemon_fs_translate_path(input, &cfg) == NULL && errno == EXDEV);
    assert(daemon_fs_translate_path("/outside/file", &cfg) == NULL && errno == EXDEV);
    assert(snprintf(input, sizeof(input), "%s/日本語", nested_root) < (int)sizeof(input));
    translated = daemon_fs_translate_path(input, &cfg);
    assert(translated != NULL && strcmp(translated, "Y:\\日本語") == 0);
    free(translated);
    translated = NULL;

    strcpy(cfg.mounts[0].host_path, test_root);
    assert(snprintf(input, sizeof(input), "%s/new/leaf", test_root) < (int)sizeof(input));
    translated = daemon_fs_translate_path(input, &cfg);
    assert(translated != NULL && strcmp(translated, "X:\\new\\leaf") == 0);
    free(translated); translated = NULL;

    path_rule_t rules[2] = {{"sentinel", "sentinel"}, {NULL, NULL}};
    assert(daemon_fs_mounts_to_rules(cfg.mounts, 2, rules, 1) == -1 && errno == E2BIG);
    assert(strcmp(rules[0].source, "sentinel") == 0);
    memset(cfg.mounts[1].host_path, 'x', sizeof(cfg.mounts[1].host_path));
    assert(daemon_fs_mounts_to_rules(cfg.mounts, 2, rules, 2) == -1 && errno == EINVAL);
    assert(strcmp(rules[0].source, "sentinel") == 0);
    cfg.mount_count = WaddleMaxMounts + 1;
    assert(daemon_fs_translate_path("/x", &cfg) == NULL && errno == EINVAL);
}

static void test_invalid_spawn(void) {
    virtiofs_process_t proc;
    memset(&proc, 0, sizeof(proc));
    proc.log_fd = -1;

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
    setup_path_tree();
    test_daemon_fs_find_binary();
    test_build_args();
    test_mounts_to_rules();
    test_path_translation();
    test_export_boundaries();
    test_invalid_spawn();
    remove_path_tree();
    printf("test_daemon_fs: all VirtIO-FS argument builder and path translation tests passed\n");
    return 0;
}
