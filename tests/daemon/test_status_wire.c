/** @file test_status_wire.c @brief Independent C ABI consumer for status admission. */
#include "daemon_wire.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(waddle_daemon_status_resp_t) == 8560, "status wire ABI");
_Static_assert(sizeof(waddle_daemon_fs_mount_t) == 1064, "mount wire ABI");
_Static_assert(offsetof(waddle_daemon_status_resp_t, mounts) == 48, "mount offset");

int main(void) {
    uint8_t bytes[8560] = {0};
    bytes[0] = SubsystemStateRunning;
    bytes[16] = 7;
    bytes[20] = 0x7a;
    bytes[21] = 0x14;
    bytes[33] = 0x10;
    bytes[36] = 4;
    bytes[40] = 1;
    memcpy(bytes + 48, "/tmp/export", sizeof("/tmp/export"));
    memcpy(bytes + 1072, "Z:", sizeof("Z:"));
    bytes[1104] = 1;
    waddle_daemon_status_resp_t output;
    memset(&output, 0xa5, sizeof(output));
    assert(daemon_wire_decode_status_response(bytes, sizeof(bytes), &output) == 0);
    assert(output.subsystem_state == SubsystemStateRunning && output.vsock_cid == 7);
    assert(output.vsock_port == 5242 && output.memory_mb == 4096 && output.vcpus == 4);
    assert(output.mount_count == 1 && output.reserved == 0);
    assert(strcmp(output.mounts[0].host_path, "/tmp/export") == 0);
    assert(strcmp(output.mounts[0].guest_drive, "Z:") == 0 && output.mounts[0].read_only == 1);
    waddle_daemon_fs_mount_t zero_mount = {0};
    for (unsigned i = 1; i < WaddleMaxMounts; ++i)
        assert(memcmp(&output.mounts[i], &zero_mount, sizeof(zero_mount)) == 0);
    waddle_daemon_status_resp_t saved = output;
    bytes[0] = 7;
    assert(daemon_wire_decode_status_response(bytes, sizeof(bytes), &output) == -1);
    assert(memcmp(&output, &saved, sizeof(saved)) == 0);
    puts("daemon status wire C ABI: passed");
    return 0;
}
