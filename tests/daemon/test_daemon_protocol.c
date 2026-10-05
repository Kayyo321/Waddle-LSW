/**
 * @file test_daemon_protocol.c
 * @brief Unit tests for Waddle daemon control wire protocol and data structures.
 */

#include "waddle/daemon_protocol.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void test_struct_layouts(void) {
    /* Header layout */
    assert(sizeof(waddle_daemon_header_t) == 16);
    assert(offsetof(waddle_daemon_header_t, magic) == 0);
    assert(offsetof(waddle_daemon_header_t, version) == 4);
    assert(offsetof(waddle_daemon_header_t, msg_type) == 6);
    assert(offsetof(waddle_daemon_header_t, sequence) == 8);
    assert(offsetof(waddle_daemon_header_t, payload_len) == 12);

    /* Payloads */
    assert(sizeof(waddle_daemon_start_req_t) == 8);
    assert(sizeof(waddle_daemon_result_resp_t) == 264);
    assert(sizeof(waddle_daemon_stop_req_t) == 8);
    assert(sizeof(waddle_daemon_kill_req_t) == 8);
    assert(sizeof(waddle_daemon_logs_req_t) == 16);

    /* Mount struct and 8-byte alignment */
    assert(sizeof(waddle_daemon_fs_mount_t) == 1064);
    assert((sizeof(waddle_daemon_fs_mount_t) % 8) == 0);

    /* Status response and 8-byte alignment */
    assert((sizeof(waddle_daemon_status_resp_t) % 8) == 0);
    assert((sizeof(waddle_daemon_fs_list_resp_t) % 8) == 0);
}

static void test_header_validation(void) {
    waddle_daemon_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));

    /* Null check */
    assert(waddle_daemon_header_validate(NULL) == -1);

    /* Valid header */
    hdr.magic = WaddleDaemonMagic;
    hdr.version = WaddleDaemonVersion;
    hdr.msg_type = DaemonMsgStartReq;
    hdr.sequence = 1;
    hdr.payload_len = sizeof(waddle_daemon_start_req_t);
    assert(waddle_daemon_header_validate(&hdr) == 0);

    /* Bad magic */
    hdr.magic = 0x12345678;
    assert(waddle_daemon_header_validate(&hdr) == -1);
    hdr.magic = WaddleDaemonMagic;

    /* Bad version */
    hdr.version = 99;
    assert(waddle_daemon_header_validate(&hdr) == -1);
    hdr.version = WaddleDaemonVersion;

    /* Oversize payload */
    hdr.payload_len = WaddleDaemonMaxPayloadSize + 1;
    assert(waddle_daemon_header_validate(&hdr) == -1);

    /* Maximum allowed payload */
    hdr.payload_len = WaddleDaemonMaxPayloadSize;
    assert(waddle_daemon_header_validate(&hdr) == 0);

    /* Zero payload */
    hdr.payload_len = 0;
    assert(waddle_daemon_header_validate(&hdr) == 0);
}

static void test_string_conversions(void) {
    assert(strcmp(waddle_subsystem_state_to_string(SubsystemStateStopped), "stopped") == 0);
    assert(strcmp(waddle_subsystem_state_to_string(SubsystemStateStartingVirtiofs), "starting_virtiofs") == 0);
    assert(strcmp(waddle_subsystem_state_to_string(SubsystemStateStartingQemu), "starting_qemu") == 0);
    assert(strcmp(waddle_subsystem_state_to_string(SubsystemStateWaitingGuest), "waiting_guest") == 0);
    assert(strcmp(waddle_subsystem_state_to_string(SubsystemStateRunning), "running") == 0);
    assert(strcmp(waddle_subsystem_state_to_string(SubsystemStateStopping), "stopping") == 0);
    assert(strcmp(waddle_subsystem_state_to_string(SubsystemStateFailed), "failed") == 0);
    assert(strcmp(waddle_subsystem_state_to_string((waddle_subsystem_state_t)999), "unknown") == 0);

    assert(strcmp(waddle_daemon_msg_type_to_string(DaemonMsgNone), "none") == 0);
    assert(strcmp(waddle_daemon_msg_type_to_string(DaemonMsgStartReq), "start_req") == 0);
    assert(strcmp(waddle_daemon_msg_type_to_string(DaemonMsgStartResp), "start_resp") == 0);
    assert(strcmp(waddle_daemon_msg_type_to_string(DaemonMsgStopReq), "stop_req") == 0);
    assert(strcmp(waddle_daemon_msg_type_to_string(DaemonMsgStopGracefulReq), "stop_graceful_req") == 0);
    assert(strcmp(waddle_daemon_msg_type_to_string(DaemonMsgStopResp), "stop_resp") == 0);
    assert(strcmp(waddle_daemon_msg_type_to_string(DaemonMsgStatusReq), "status_req") == 0);
    assert(strcmp(waddle_daemon_msg_type_to_string(DaemonMsgStatusResp), "status_resp") == 0);
    assert(strcmp(waddle_daemon_msg_type_to_string(DaemonMsgKillReq), "kill_req") == 0);
    assert(strcmp(waddle_daemon_msg_type_to_string(DaemonMsgKillResp), "kill_resp") == 0);
    assert(strcmp(waddle_daemon_msg_type_to_string(DaemonMsgFsListReq), "fs_list_req") == 0);
    assert(strcmp(waddle_daemon_msg_type_to_string(DaemonMsgFsListResp), "fs_list_resp") == 0);
    assert(strcmp(waddle_daemon_msg_type_to_string(DaemonMsgLogsReq), "logs_req") == 0);
    assert(strcmp(waddle_daemon_msg_type_to_string(DaemonMsgLogsResp), "logs_resp") == 0);
    assert(strcmp(waddle_daemon_msg_type_to_string(DaemonMsgErrorResp), "error_resp") == 0);
    assert(strcmp(waddle_daemon_msg_type_to_string((waddle_daemon_msg_type_t)0x1234), "unknown") == 0);
}

static void test_socket_transmission(void) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    /* Test 1: Send StartReq and receive */
    waddle_daemon_start_req_t req;
    memset(&req, 0, sizeof(req));
    req.flags = DaemonStartFlagWaitGuest | DaemonStartFlagHeadless;
    req.timeout_sec = 45;

    assert(waddle_daemon_send_msg(sv[0], DaemonMsgStartReq, 42, &req, sizeof(req)) == 0);

    waddle_daemon_header_t hdr;
    waddle_daemon_start_req_t recv_req;
    memset(&hdr, 0, sizeof(hdr));
    memset(&recv_req, 0, sizeof(recv_req));

    assert(waddle_daemon_recv_msg(sv[1], &hdr, &recv_req, sizeof(recv_req), 5000) == 0);
    assert(hdr.magic == WaddleDaemonMagic);
    assert(hdr.version == WaddleDaemonVersion);
    assert(hdr.msg_type == DaemonMsgStartReq);
    assert(hdr.sequence == 42);
    assert(hdr.payload_len == sizeof(req));
    assert(recv_req.flags == (DaemonStartFlagWaitGuest | DaemonStartFlagHeadless));
    assert(recv_req.timeout_sec == 45);

    /* Test 2: Send StatusResp with mounts */
    waddle_daemon_status_resp_t status;
    memset(&status, 0, sizeof(status));
    status.subsystem_state = SubsystemStateRunning;
    status.daemon_pid = 1234;
    status.qemu_pid = 5678;
    status.virtiofsd_pid = 9101;
    status.vsock_cid = 3;
    status.vsock_port = 5242;
    status.uptime_sec = 3600;
    status.memory_mb = 4096;
    status.vcpus = 4;
    status.mount_count = 2;
    strncpy(status.mounts[0].host_path, "/home/user", sizeof(status.mounts[0].host_path) - 1);
    strncpy(status.mounts[0].guest_drive, "Z:\\", sizeof(status.mounts[0].guest_drive) - 1);
    status.mounts[0].read_only = 0;
    strncpy(status.mounts[1].host_path, "/mnt/data", sizeof(status.mounts[1].host_path) - 1);
    strncpy(status.mounts[1].guest_drive, "X:\\", sizeof(status.mounts[1].guest_drive) - 1);
    status.mounts[1].read_only = 1;

    assert(waddle_daemon_send_msg(sv[1], DaemonMsgStatusResp, 43, &status, sizeof(status)) == 0);

    waddle_daemon_status_resp_t recv_status;
    memset(&hdr, 0, sizeof(hdr));
    memset(&recv_status, 0, sizeof(recv_status));

    assert(waddle_daemon_recv_msg(sv[0], &hdr, &recv_status, sizeof(recv_status), 5000) == 0);
    assert(hdr.msg_type == DaemonMsgStatusResp);
    assert(hdr.sequence == 43);
    assert(recv_status.subsystem_state == SubsystemStateRunning);
    assert(recv_status.daemon_pid == 1234);
    assert(recv_status.qemu_pid == 5678);
    assert(recv_status.virtiofsd_pid == 9101);
    assert(recv_status.mount_count == 2);
    assert(strcmp(recv_status.mounts[0].host_path, "/home/user") == 0);
    assert(strcmp(recv_status.mounts[0].guest_drive, "Z:\\") == 0);
    assert(recv_status.mounts[0].read_only == 0);
    assert(strcmp(recv_status.mounts[1].host_path, "/mnt/data") == 0);
    assert(strcmp(recv_status.mounts[1].guest_drive, "X:\\") == 0);
    assert(recv_status.mounts[1].read_only == 1);

    /* Test 3: Buffer too small */
    assert(waddle_daemon_send_msg(sv[0], DaemonMsgStatusResp, 44, &status, sizeof(status)) == 0);
    uint8_t tiny_buf[16];
    assert(waddle_daemon_recv_msg(sv[1], &hdr, tiny_buf, sizeof(tiny_buf), 5000) == -1);
    assert(errno == ENOBUFS);

    close(sv[0]);
    close(sv[1]);

    /* Test 4: Clean EOF handling on fresh socketpair */
    int sv2[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv2) == 0);
    close(sv2[0]);
    assert(waddle_daemon_recv_msg(sv2[1], &hdr, &recv_status, sizeof(recv_status), 100) == -2);
    close(sv2[1]);
}

int main(void) {
    test_struct_layouts();
    test_header_validation();
    test_string_conversions();
    test_socket_transmission();
    printf("test_daemon_protocol: all wire protocol and data structure tests passed\n");
    return 0;
}
