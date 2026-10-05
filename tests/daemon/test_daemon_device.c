/**
 * @file test_daemon_device.c
 * @brief Unit tests for daemon device management, naming validation, registry listing, and init.
 */

#include "daemon_device.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>

static void test_device_name_validation(void) {
    /* Valid names */
    assert(daemon_device_validate_name("win11") == 0);
    assert(daemon_device_validate_name("test-device") == 0);
    assert(daemon_device_validate_name("dev_1") == 0);
    assert(daemon_device_validate_name("A1-B2_C3") == 0);

    /* Invalid names */
    assert(daemon_device_validate_name(NULL) == -1);
    assert(errno == EINVAL);

    assert(daemon_device_validate_name("") == -1);
    assert(errno == EINVAL);

    assert(daemon_device_validate_name("has space") == -1);
    assert(errno == EINVAL);

    assert(daemon_device_validate_name("bad/slash") == -1);
    assert(errno == EINVAL);

    assert(daemon_device_validate_name("invalid$char") == -1);
    assert(errno == EINVAL);

    assert(daemon_device_validate_name("\xff") == -1);
    char boundary[64];
    memset(boundary, 'a', sizeof(boundary) - 1);
    boundary[63] = '\0';
    assert(daemon_device_validate_name(boundary) == 0);
    char long_name[WaddleMaxDeviceNameLen + 10];
    memset(long_name, 'a', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    assert(daemon_device_validate_name(long_name) == -1);
    assert(errno == EINVAL);
}

static void test_cid_allocation(void) {
    device_list_t list;
    memset(&list, 0, sizeof(list));

    /* Empty list yields BaseVsockCid (3) */
    assert(daemon_device_allocate_cid(&list) == BaseVsockCid);

    /* Single device with CID 3 */
    list.count = 1;
    list.devices[0].vsock_cid = 3;
    assert(daemon_device_allocate_cid(&list) == 4);

    /* Two devices with CIDs 3 and 7 */
    list.count = 2;
    list.devices[1].vsock_cid = 7;
    assert(daemon_device_allocate_cid(&list) == 8);
    list.devices[1].vsock_cid = UINT32_MAX - 1;
    assert(daemon_device_allocate_cid(&list) == 4);
    list.devices[1].vsock_cid = UINT32_MAX;
    assert(daemon_device_allocate_cid(&list) == 0);
    assert(errno == EINVAL);
}

static void test_device_init_and_registry(void) {
    char temp_dir[] = "/tmp/waddle_test_device_XXXXXX";
    char *res = mkdtemp(temp_dir);
    assert(res != NULL);

    char config_home[512];
    snprintf(config_home, sizeof(config_home), "%s/config", temp_dir);
    char state_home[512];
    snprintf(state_home, sizeof(state_home), "%s/state", temp_dir);

    setenv("XDG_CONFIG_HOME", config_home, 1);
    setenv("XDG_STATE_HOME", state_home, 1);

    char selected[WaddleMaxDeviceNameLen];
    assert(daemon_device_default_get(selected, sizeof(selected)) == 0);
    assert(selected[0] == '\0');

    /* Initially zero devices */
    device_list_t list;
    assert(daemon_device_list(&list) == 0);
    assert(list.count == 0);
    assert(access(config_home, F_OK) != 0);
    assert(access(state_home, F_OK) != 0);
    char socket_path[WaddleMaxPathLen];
    assert(daemon_device_get_socket_path("alpha", socket_path, sizeof(socket_path)) == 0);
    assert(access(socket_path, F_OK) != 0);
    setenv("XDG_CONFIG_HOME", "relative", 1);
    assert(daemon_device_list(&list) == -1);
    assert(errno == EINVAL);
    setenv("XDG_CONFIG_HOME", config_home, 1);

    /* Initialize first device */
    device_info_t dev1;
    assert(daemon_device_init("alpha", NULL, &dev1) == 0);
    assert(strcmp(dev1.name, "alpha") == 0);
    assert(dev1.vsock_cid == BaseVsockCid);
    assert(access(dev1.config_path, R_OK) == 0);
    assert(access(dev1.disk_image, R_OK) == 0);

    /* Duplicate device must fail with EEXIST */
    assert(daemon_device_init("alpha", NULL, NULL) == -1);
    assert(errno == EEXIST);

    /* Find dev1 */
    device_info_t found;
    assert(daemon_device_find("alpha", &found) == 0);
    assert(strcmp(found.name, "alpha") == 0);
    assert(found.vsock_cid == BaseVsockCid);

    /* Find nonexistent device must fail with ENOENT */
    assert(daemon_device_find("nonexistent", &found) == -1);
    assert(errno == ENOENT);

    /* Initialize second device */
    device_info_t dev2;
    assert(daemon_device_init("beta", NULL, &dev2) == 0);
    assert(strcmp(dev2.name, "beta") == 0);
    assert(dev2.vsock_cid == BaseVsockCid + 1);

    /* List devices */
    assert(daemon_device_list(&list) == 2);
    assert(list.count == 2);

    int has_alpha = 0;
    int has_beta = 0;
    for (size_t i = 0; i < list.count; i++) {
        if (strcmp(list.devices[i].name, "alpha") == 0) has_alpha = 1;
        if (strcmp(list.devices[i].name, "beta") == 0) has_beta = 1;
    }
    assert(has_alpha == 1);
    assert(has_beta == 1);
    assert(strcmp(list.devices[0].name, "alpha") == 0);
    assert(strcmp(list.devices[1].name, "beta") == 0);
    assert(list.devices[0].config_valid == 1);

    /* Fail closed on an explicit missing base and on a missing utility. */
    assert(daemon_device_init("missing", "/nonexistent/waddle-base.qcow2", NULL) == -1);
    char *original_path = strdup(getenv("PATH"));
    assert(original_path != NULL);
    setenv("PATH", "/nonexistent", 1);
    assert(daemon_device_init("tool-failure", NULL, NULL) == -1);
    assert(errno == ENOENT);
    setenv("PATH", original_path, 1);
    free(original_path);
    original_path = NULL;
    assert(daemon_device_list(&list) == 2);

    /* Concurrent writers must serialize CID reservation. */
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) _exit(daemon_device_init("gamma", NULL, NULL) == 0 ? 0 : 1);
    assert(daemon_device_init("delta", NULL, NULL) == 0);
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    device_info_t gamma, delta;
    assert(daemon_device_find("gamma", &gamma) == 0);
    assert(daemon_device_find("delta", &delta) == 0);
    assert(gamma.vsock_cid != delta.vsock_cid);

    /* Default updates do not boot; invalid targets preserve the selection. */
    assert(daemon_device_default_set("beta") == 0);
    assert(daemon_device_default_get(selected, sizeof(selected)) == 0);
    assert(strcmp(selected, "beta") == 0);
    assert(daemon_device_default_set("absent") == -1 && errno == ENOENT);
    assert(daemon_device_default_get(selected, sizeof(selected)) == 0);
    assert(strcmp(selected, "beta") == 0);
    assert(daemon_device_default_get(selected, 2) == -1 && errno == ENAMETOOLONG);
    assert(daemon_device_default_set(NULL) == 0);
    assert(daemon_device_default_set(NULL) == 0);
    assert(daemon_device_default_get(selected, sizeof(selected)) == 0 && selected[0] == '\0');

    /* Symlink profiles are unhealthy; never consume outside metadata. */
    char link_path[1024];
    char config_dir[1024];
    assert(daemon_device_get_config_dir(config_dir, sizeof(config_dir)) == 0);
    assert(snprintf(link_path, sizeof(link_path), "%s/linked.ini", config_dir) < (int)sizeof(link_path));
    assert(symlink(dev1.config_path, link_path) == 0);
    assert(daemon_device_find("linked", &found) == 0);
    assert(found.config_valid == 0);
    assert(daemon_device_init("blocked", NULL, NULL) == -1);
    assert(errno == EINVAL);
    assert(unlink(link_path) == 0);

    /* Never truncate at the 32-entry registry limit. */
    for (int i = 0; i < 29; i++) {
        assert(snprintf(link_path, sizeof(link_path), "%s/capacity_%02d.ini", config_dir, i) < (int)sizeof(link_path));
        int fd = open(link_path, O_CREAT | O_EXCL | O_WRONLY, 0600);
        assert(fd >= 0);
        assert(close(fd) == 0);
    }
    assert(daemon_device_list(&list) == -1);
    assert(errno == ENOSPC);

    /* Cleanup temp files */
    unlink(dev1.disk_image);
    unlink(dev1.config_path);
    unlink(dev2.disk_image);
    unlink(dev2.config_path);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", temp_dir);
    int ret = system(cmd);
    (void)ret;

    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_STATE_HOME");
}

int main(void) {
    test_device_name_validation();
    test_cid_allocation();
    test_device_init_and_registry();
    printf("test_daemon_device: all device registry and initialization tests passed\n");
    return 0;
}
