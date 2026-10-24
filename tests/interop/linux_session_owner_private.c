/** @file linux_session_owner_private.c
 * @brief Exercise exact production private guards without adding a public test API.
 * No owning pointer, fd, lease or lifecycle state is fabricated.
 */
#include "../../src/interop/linux_session_owner.c"
#include <assert.h>

static void test_range_and_path_guards(void) {
    _Alignas(uint64_t) uint8_t storage[32] = {0};
    pointer_range_t range;
    assert(!make_range(NULL, 1, 1, &range));
    assert(!make_range(storage, 1, 1, NULL));
    assert(!make_range(storage, 0, 1, &range));
    assert(!make_range(storage, 1, 0, &range));
    assert(!make_range(storage + 1, 8, 8, &range));
    assert(!make_range((void *)(UINTPTR_MAX - 3), 4, 1, &range));
    assert(make_range((void *)(UINTPTR_MAX - 3), 3, 1, &range));
    assert(make_range(storage, sizeof(storage), 8, &range));
    assert(!ranges_overlap((pointer_range_t){1, 2}, (pointer_range_t){2, 3}));
    assert(!ranges_overlap((pointer_range_t){2, 3}, (pointer_range_t){1, 2}));
    assert(ranges_overlap((pointer_range_t){1, 3}, (pointer_range_t){2, 4}));
    size_t length = 99;
    assert(!c_string_array(NULL, 1, &length));
    assert(!c_string_array("", 0, &length));
    assert(!c_string_array("", 1, &length));
    assert(!c_string_array("abc", 3, &length));
    assert(c_string_array("abc", 4, NULL));
    assert(c_string_array("abc", 4, &length) && length == 3);
    char output[WaddleMaxPathLen];
    assert(!path_parent("relative/daemon.sock", output, sizeof(output)));
    assert(!path_parent("", output, sizeof(output)));
    assert(!path_parent("/daemon.sock", output, sizeof(output)));
    assert(!path_parent("/tmp/other.sock", output, sizeof(output)));
    assert(!path_parent("/tmp/daemon.sock", output, 2));
    assert(path_parent("/tmp/daemon.sock", output, sizeof(output)));
    assert(strcmp(output, "/tmp") == 0);
    assert(!append_path(output, 0, "/tmp", "av"));
    assert(!append_path(output, sizeof(output), "", "av"));
    assert(!append_path(output, 2, "ab", "c"));
    assert(!append_path(output, 3, "ab", "c"));
    assert(!append_path(output, 4, "ab", "c"));
    assert(append_path(output, 5, "ab", "c") && strcmp(output, "ab/c") == 0);
    assert(append_path(output, 5, "ab/", "c") && strcmp(output, "ab/c") == 0);
    assert(!valid_directory(-1));
    assert(!valid_lock_file(-1, "missing", -1));
}

static void test_config_range_alias_guards(void) {
    device_info_t device = {0};
    memcpy(device.name, "range-guard", sizeof("range-guard"));
    memcpy(device.state_dir, "/tmp/state", sizeof("/tmp/state"));
    memcpy(device.socket_path, "/tmp/daemon.sock", sizeof("/tmp/daemon.sock"));
    device.config_valid = 1;
    device.vsock_cid = 3;
    device.vsock_port = 5242;
    static const uint8_t version[] = "1.2.3";
    const interop_linux_session_config_t original = {.device = &device,
        .agent_version = version, .agent_version_bytes = sizeof(version) - 1,
        .connection_token = 12, .drain_timeout_ms = 5};
    interop_linux_session_config_t config;
    interop_linux_session_owner_t *owner = NULL;

    union config_output_overlap_t {
        max_align_t alignment;
        uint8_t bytes[sizeof(interop_linux_session_config_t) + sizeof(void *)];
    } config_storage = {0};
    memcpy(config_storage.bytes, &original, sizeof(original));
    assert(config_validate((const interop_linux_session_config_t *)config_storage.bytes,
        (interop_linux_session_owner_t **)(void *)config_storage.bytes) == InteropLinuxSessionInvalidArgument);

    config = original;
    config.device = (const device_info_t *)&config;
    assert(config_validate(&config, &owner) == InteropLinuxSessionInvalidArgument);
    assert(owner == NULL);

    config = original;
    config.agent_version = (const uint8_t *)&config;
    config.agent_version_bytes = 5;
    assert(config_validate(&config, &owner) == InteropLinuxSessionInvalidArgument);
    assert(owner == NULL);

    config = original;
    config.agent_version = (const uint8_t *)device.name;
    config.agent_version_bytes = 5;
    assert(config_validate(&config, &owner) == InteropLinuxSessionInvalidArgument);
    assert(owner == NULL);

    memset(device.name, 0, sizeof(void *));
    assert(config_validate(&original, (interop_linux_session_owner_t **)(void *)device.name) ==
        InteropLinuxSessionInvalidArgument);

    union version_output_overlap_t {
        void *alignment;
        uint8_t bytes[16];
    } version_storage = {0};
    config = original;
    config.agent_version = version_storage.bytes;
    config.agent_version_bytes = sizeof(version_storage.bytes);
    assert(config_validate(&config,
        (interop_linux_session_owner_t **)(void *)version_storage.bytes) == InteropLinuxSessionInvalidArgument);
}

static void test_release_slot_inside_owner_is_rejected_without_mutation(void) {
    device_info_t device = {0};
    memcpy(device.name, "release-alias", sizeof("release-alias"));
    assert(daemon_device_get_state_dir(device.name, device.state_dir, sizeof(device.state_dir)) == 0);
    assert(daemon_device_get_socket_path(device.name, device.socket_path, sizeof(device.socket_path)) == 0);
    device.config_valid = 1;
    device.vsock_cid = 3;
    device.vsock_port = 5242;
    static const uint8_t version[] = "1.2.3";
    const interop_linux_session_config_t config = {.device = &device,
        .agent_version = version, .agent_version_bytes = sizeof(version) - 1,
        .connection_token = 7, .drain_timeout_ms = 100};
    interop_linux_session_owner_t *owner = NULL;
    assert(interop_linux_session_acquire(&config, &owner) == InteropLinuxSessionSuccess);
    interop_linux_socket_t **alias = &owner->socket_owner;
    assert(interop_linux_session_release((interop_linux_session_owner_t **)alias) ==
        InteropLinuxSessionInvalidArgument);
    assert(owner != NULL && owner->session_fd >= 0 && owner->av_fd >= 0 && owner->socket_owner == NULL);
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionSuccess);
}

static void test_config_validation_guards(void) {
    device_info_t device = {0};
    memcpy(device.name, "guard", 6);
    memcpy(device.state_dir, "/tmp/state", 11);
    memcpy(device.socket_path, "/tmp/daemon.sock", 17);
    device.config_valid = 1;
    device.vsock_cid = 3;
    device.vsock_port = 5242;
    static const uint8_t Version[] = "1.2.3";
    const interop_linux_session_config_t original = {.device = &device,
        .agent_version = Version, .agent_version_bytes = 5, .connection_token = 1,
        .drain_timeout_ms = 1};
    interop_linux_session_config_t config = original;
    interop_linux_session_owner_t *owner = NULL;
    assert(config_validate(&config, &owner) == InteropLinuxSessionSuccess);
    assert(config_validate(NULL, &owner) == InteropLinuxSessionInvalidArgument);
    assert(config_validate(&config, NULL) == InteropLinuxSessionInvalidArgument);
    assert(config_validate((void *)((uintptr_t)&config + 1), &owner) == InteropLinuxSessionInvalidArgument);
    assert(config_validate(&config, (void *)((uintptr_t)&config + 1)) == InteropLinuxSessionInvalidArgument);
    assert(config_validate(&config, (void *)&config) == InteropLinuxSessionInvalidArgument);
    owner = (void *)&config;
    assert(config_validate(&config, &owner) == InteropLinuxSessionInvalidArgument);
    owner = NULL;
    for (unsigned scenario = 0; scenario < 24; scenario++) {
        config = original;
        device_info_t changed = device;
        config.device = &changed;
        switch (scenario) {
        case 0: config.device = NULL; break;
        case 1: config.agent_version = NULL; break;
        case 2: config.device = (void *)&config; break;
        case 3: config.agent_version = (void *)&config; break;
        case 4: config.agent_version = (void *)&changed; break;
        case 5: config.reserved_zero = 1; break;
        case 6: config.connection_token = 0; break;
        case 7: config.drain_timeout_ms = 0; break;
        case 8: config.drain_timeout_ms = InteropLinuxSocketMaxDrainTimeoutMs + 1; break;
        case 9: config.local_capabilities = UINT64_MAX; break;
        case 10: config.agent_version_bytes = 4; break;
        case 11: config.agent_version_bytes = 33; break;
        case 12: config.agent_version = (const uint8_t *)"a.b.c"; break;
        case 13: changed.config_valid = 0; break;
        case 14: changed.vsock_cid = 2; break;
        case 15: changed.vsock_cid = UINT32_MAX; break;
        case 16: changed.vsock_port = 0; break;
        case 17: changed.name[0] = 0; break;
        case 18: memset(changed.name, 'a', sizeof(changed.name)); break;
        case 19: changed.state_dir[0] = 0; break;
        case 20: memset(changed.state_dir, 'a', sizeof(changed.state_dir)); break;
        case 21: changed.socket_path[0] = 0; break;
        case 22: memset(changed.socket_path, 'a', sizeof(changed.socket_path)); break;
        case 23: memcpy(changed.name, "../x", 5); break;
        }
        assert(config_validate(&config, &owner) == InteropLinuxSessionInvalidArgument);
        assert(owner == NULL);
    }
    assert(interop_linux_session_acquire(NULL, &owner) == InteropLinuxSessionInvalidArgument);
    assert(interop_linux_session_acquire(&original, NULL) == InteropLinuxSessionInvalidArgument);
    assert(interop_linux_session_socket_owner(NULL) == NULL && errno == EINVAL);
    assert(interop_linux_session_destroy_peer(NULL) == InteropLinuxSessionInvalidArgument);
    assert(interop_linux_session_connect_peer(NULL, 100, -1) == InteropLinuxSessionInvalidArgument);
    assert(interop_linux_session_release(NULL) == InteropLinuxSessionInvalidArgument);
    assert(interop_linux_session_release(&owner) == InteropLinuxSessionInvalidArgument);
}

void linux_session_private_guard_tests(void) {
    test_range_and_path_guards();
    test_config_range_alias_guards();
    test_config_validation_guards();
    test_release_slot_inside_owner_is_rejected_without_mutation();
}
