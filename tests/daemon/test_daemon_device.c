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

    /* Initially zero devices */
    device_list_t list;
    assert(daemon_device_list(&list) == 0);
    assert(list.count == 0);

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
