#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

static uint32_t host_control_private_clock_failure_call;
static uint32_t host_control_private_clock_calls;

static int host_control_private_clock_gettime(clockid_t clock_id,
                                               struct timespec *time_value)
{
    if (host_control_private_clock_failure_call != 0 &&
        clock_id == CLOCK_MONOTONIC &&
        ++host_control_private_clock_calls ==
            host_control_private_clock_failure_call) {
        host_control_private_clock_failure_call = 0;
        errno = EIO;
        return -1;
    }
    return clock_gettime(clock_id, time_value);
}

#define clock_gettime host_control_private_clock_gettime
#include "../../src/interop/session_control_host.c"
#undef clock_gettime

#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include "../../src/interop/interop_mapping_init.h"
#include "../../src/interop/interop_mapping_linux.h"

void session_control_host_private_fail_heartbeat_store(void);

static void expect_run_invalid(const interop_host_control_config_t *config,
                               interop_host_control_owners_t *owners)
{
    assert(interop_host_control_run(config, owners) == HostControlInvalidArgument);
    assert(owners->session_owner == NULL && owners->mapping_owner == NULL);
}

void session_control_host_private_fail_clock_call(uint32_t call)
{
    host_control_private_clock_calls = 0;
    host_control_private_clock_failure_call = call;
}

void session_control_host_private_tests(void)
{
    assert(interop_host_control_retry_cleanup(NULL) == HostControlInvalidArgument);
    assert(interop_host_control_run(NULL, NULL) == HostControlInvalidArgument);


    _Alignas(interop_host_control_config_t) uint8_t config_storage[
        sizeof(interop_host_control_config_t) + _Alignof(interop_host_control_config_t)];
    _Alignas(interop_host_control_owners_t) uint8_t owners_storage[
        sizeof(interop_host_control_owners_t) + _Alignof(interop_host_control_owners_t)];
    interop_host_control_config_t *config =
        (interop_host_control_config_t *)config_storage;
    interop_host_control_owners_t *owners =
        (interop_host_control_owners_t *)owners_storage;
    memset(config, 0, sizeof(*config));
    memset(owners, 0, sizeof(*owners));
    assert(interop_host_control_run(
        (const interop_host_control_config_t *)(config_storage + 1), owners) ==
        HostControlInvalidArgument);
    assert(interop_host_control_run(config,
        (interop_host_control_owners_t *)(owners_storage + 1)) ==
        HostControlInvalidArgument);
    assert(interop_host_control_run(config,
        (interop_host_control_owners_t *)config) == HostControlInvalidArgument);

    _Alignas(max_align_t) uint8_t range_storage[
        sizeof(*config) + sizeof(*owners) + 2 * _Alignof(max_align_t)];
    memset(range_storage, 0, sizeof(range_storage));
    interop_host_control_config_t *range_config =
        (interop_host_control_config_t *)range_storage;
    interop_host_control_owners_t *range_owners =
        (interop_host_control_owners_t *)(range_storage + sizeof(*config) -
            _Alignof(interop_host_control_owners_t));
    assert((uintptr_t)range_owners % _Alignof(interop_host_control_owners_t) == 0);
    assert(interop_host_control_run(range_config, range_owners) ==
        HostControlInvalidArgument);
    interop_host_control_owners_t *owners_first =
        (interop_host_control_owners_t *)range_storage;
    interop_host_control_config_t *config_after_owner =
        (interop_host_control_config_t *)(range_storage + sizeof(*owners) -
            _Alignof(interop_host_control_config_t));
    assert((uintptr_t)config_after_owner % _Alignof(interop_host_control_config_t) == 0);
    assert(interop_host_control_run(config_after_owner, owners_first) ==
        HostControlInvalidArgument);
    assert(interop_host_control_run(NULL, owners) == HostControlInvalidArgument);
    assert(interop_host_control_run(config, NULL) == HostControlInvalidArgument);

    interop_host_session_config_t admission;
    memset(&admission, 0, sizeof(admission));
    config->admission = (const interop_host_session_config_t *)((uintptr_t)&admission + 1u);
    assert(interop_host_control_run(config, owners) == HostControlInvalidArgument);
    config->admission = (const interop_host_session_config_t *)(uintptr_t)1u;
    assert(interop_host_control_run(config, owners) == HostControlInvalidArgument);

    config->admission = &admission;
    admission.device = NULL;
    admission.agent_version = NULL;
    admission.agent_version_bytes = 0;
    admission.local_capabilities = 1;
    admission.cancel_fd = -1;
    config->mapping_fd = -1;
    config->backing_kind = (interop_mapping_linux_backing_t)99;
    config->stop_fd = -1;
    config->run_for_ms = 0;
    config->reserved_zero = 1;
    assert(!host_control_config_valid(config, owners));

    device_info_t device;
    memset(&device, 0, sizeof(device));
    strcpy(device.name, "control-private-test");
    device.config_valid = 1;
    device.vsock_cid = 33;
    device.vsock_port = 5242;
    device.memory_mb = 4096;
    device.vcpus = 4;
    uint8_t version[] = "1.2.3";
    int stop_pipe[2];
    int daemon_pair[2];
    assert(pipe2(stop_pipe, O_NONBLOCK | O_CLOEXEC) == 0);
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, daemon_pair) == 0);
    memset(&admission, 0, sizeof(admission));
    admission.device = &device;
    admission.daemon_fd = daemon_pair[0];
    admission.agent_version = version;
    admission.agent_version_bytes = sizeof(version) - 1;
    admission.connection_token = 1;
    admission.local_capabilities = 0;
    admission.deadline_ms = 1;
    admission.cancel_fd = stop_pipe[0];
    admission.guest_ready_timeout_ms = 10;
    admission.drain_timeout_ms = 10;
    memset(config, 0, sizeof(*config));
    config->admission = &admission;
    config->mapping_fd = daemon_pair[1];
    config->backing_kind = InteropMappingLinuxBackingRegularPrivateFile;
    config->stop_fd = stop_pipe[0];
    config->run_for_ms = 1;
    memset(owners, 0, sizeof(*owners));
    assert(host_control_config_valid(config, owners));
    int closed_stop_fd = fcntl(stop_pipe[0], F_DUPFD_CLOEXEC, 10);
    assert(closed_stop_fd >= 0 && close(closed_stop_fd) == 0);
    config->stop_fd = closed_stop_fd;
    admission.cancel_fd = closed_stop_fd;
    expect_run_invalid(config, owners);
    config->stop_fd = stop_pipe[0];
    admission.cancel_fd = stop_pipe[0];
    config->run_for_ms = 0;
    expect_run_invalid(config, owners);
    config->run_for_ms = 1;
    config->backing_kind = InteropMappingLinuxBackingKvmfrCharacter;
    assert(host_control_config_valid(config, owners));
    config->backing_kind = InteropMappingLinuxBackingRegularPrivateFile;
    assert(!host_control_config_valid(NULL, owners));
    assert(!host_control_config_valid(config, NULL));
    const interop_host_session_config_t *valid_admission = config->admission;
    config->admission = NULL;
    assert(!host_control_config_valid(config, owners));
    config->admission = valid_admission;
    config->admission = (const interop_host_session_config_t *)((uintptr_t)config + 1u);
    expect_run_invalid(config, owners);
    config->admission = valid_admission;
    int saved_mapping_fd = config->mapping_fd;
    config->mapping_fd = -1;
    assert(!host_control_config_valid(config, owners));
    config->mapping_fd = saved_mapping_fd;
    interop_mapping_linux_backing_t saved_backing = config->backing_kind;
    config->backing_kind = (interop_mapping_linux_backing_t)99;
    assert(!host_control_config_valid(config, owners));
    config->backing_kind = saved_backing;
    config->backing_kind = InteropMappingLinuxBackingKvmfrCharacter;
    assert(host_control_config_valid(config, owners));
    config->backing_kind = saved_backing;
    int saved_stop_fd = config->stop_fd;
    config->stop_fd = -1;
    assert(!host_control_config_valid(config, owners));
    config->stop_fd = saved_stop_fd;
    config->run_for_ms = 0;
    assert(!host_control_config_valid(config, owners));
    config->run_for_ms = 120001;
    assert(!host_control_config_valid(config, owners));
    config->run_for_ms = 1;
    config->reserved_zero = 1;
    assert(!host_control_config_valid(config, owners));
    config->reserved_zero = 0;
    admission.local_capabilities = 1;
    assert(!host_control_config_valid(config, owners));
    admission.local_capabilities = 0;
    admission.cancel_fd = daemon_pair[0];
    assert(!host_control_config_valid(config, owners));
    admission.cancel_fd = stop_pipe[0];
    config->stop_fd = config->mapping_fd;
    assert(!host_control_config_valid(config, owners));
    config->stop_fd = daemon_pair[0];
    assert(!host_control_config_valid(config, owners));
    config->stop_fd = stop_pipe[0];
    owners->session_owner = (interop_linux_session_owner_t *)(uintptr_t)1u;
    assert(!host_control_config_valid(config, owners));
    owners->session_owner = NULL;
    owners->mapping_owner = (interop_mapping_linux_t *)(uintptr_t)1u;
    assert(!host_control_config_valid(config, owners));
    owners->mapping_owner = NULL;
    admission.local_capabilities = 1;
    assert(!host_control_config_valid(config, owners));
    admission.local_capabilities = 0;
    admission.cancel_fd = daemon_pair[0];
    assert(!host_control_config_valid(config, owners));
    admission.cancel_fd = stop_pipe[0];
    config->stop_fd = daemon_pair[1];
    admission.cancel_fd = daemon_pair[1];
    assert(!host_control_config_valid(config, owners));
    config->stop_fd = daemon_pair[0];
    admission.cancel_fd = daemon_pair[0];
    assert(!host_control_config_valid(config, owners));
    config->stop_fd = stop_pipe[0];
    admission.cancel_fd = stop_pipe[0];
    assert(interop_host_control_run(config, owners) == HostControlAdmissionFailed);
    assert(owners->session_owner == NULL && owners->mapping_owner == NULL);
    admission.local_capabilities = 1;
    assert(!host_control_config_valid(config, owners));
    admission.local_capabilities = 0;
    admission.cancel_fd = -1;
    assert(!host_control_config_valid(config, owners));
    admission.cancel_fd = stop_pipe[0];
    config->backing_kind = (interop_mapping_linux_backing_t)99;
    assert(!host_control_config_valid(config, owners));
    config->backing_kind = InteropMappingLinuxBackingRegularPrivateFile;
    config->run_for_ms = 120001;
    assert(!host_control_config_valid(config, owners));
    config->run_for_ms = 1;
    config->reserved_zero = 1;
    assert(!host_control_config_valid(config, owners));
    config->reserved_zero = 0;
    config->stop_fd = daemon_pair[0];
    assert(!host_control_config_valid(config, owners));
    config->stop_fd = config->mapping_fd;
    assert(!host_control_config_valid(config, owners));

    config->stop_fd = stop_pipe[0];
    interop_host_session_config_t *saved_admission =
        (interop_host_session_config_t *)config->admission;
    config->admission = NULL;
    expect_run_invalid(config, owners);
    config->admission = saved_admission;
    expect_run_invalid((const interop_host_control_config_t *)(uintptr_t)
        (UINTPTR_MAX & ~(uintptr_t)(_Alignof(interop_host_control_config_t) - 1u)),
        owners);
    assert(interop_host_control_run(config, (interop_host_control_owners_t *)(uintptr_t)
        (UINTPTR_MAX & ~(uintptr_t)(_Alignof(interop_host_control_owners_t) - 1u))) ==
        HostControlInvalidArgument);
    assert(interop_host_control_run(config,
        (interop_host_control_owners_t *)config) == HostControlInvalidArgument);

    const interop_host_session_config_t *good_admission = config->admission;
    config->admission = (const interop_host_session_config_t *)
        ((uintptr_t)config + sizeof(*config) -
            _Alignof(interop_host_session_config_t));
    expect_run_invalid(config, owners);
    _Alignas(max_align_t) uint8_t nested_range_storage[
        sizeof(*owners) + sizeof(admission) + 2 * _Alignof(max_align_t)];
    memset(nested_range_storage, 0, sizeof(nested_range_storage));
    interop_host_control_owners_t *nested_owners =
        (interop_host_control_owners_t *)nested_range_storage;
    interop_host_session_config_t *nested_admission =
        (interop_host_session_config_t *)(nested_range_storage + sizeof(*owners) -
            _Alignof(interop_host_session_config_t));
    assert((uintptr_t)nested_admission %
        _Alignof(interop_host_session_config_t) == 0);
    config->admission = nested_admission;
    expect_run_invalid(config, nested_owners);
    config->admission = good_admission;
    config->admission = (const interop_host_session_config_t *)(uintptr_t)
        (UINTPTR_MAX & ~(uintptr_t)(_Alignof(interop_host_session_config_t) - 1u));
    expect_run_invalid(config, owners);
    config->admission = (const interop_host_session_config_t *)
        ((uintptr_t)&admission + 1u);
    expect_run_invalid(config, owners);
    config->admission = (const interop_host_session_config_t *)config;
    expect_run_invalid(config, owners);
    config->admission = (const interop_host_session_config_t *)owners;
    expect_run_invalid(config, owners);
    config->admission = (const interop_host_session_config_t *)config;
    expect_run_invalid(config, owners);
    config->admission = good_admission;
    assert(interop_host_control_run(config, owners) == HostControlAdmissionFailed);

    device_info_t *good_device = (device_info_t *)admission.device;
    const uint8_t *good_version = admission.agent_version;
    admission.device = NULL;
    expect_run_invalid(config, owners);
    admission.device = (const device_info_t *)((uintptr_t)good_device + 1u);
    expect_run_invalid(config, owners);
    admission.device = (const device_info_t *)(uintptr_t)
        (UINTPTR_MAX & ~(uintptr_t)(_Alignof(device_info_t) - 1u));
    expect_run_invalid(config, owners);
    admission.device = (const device_info_t *)config;
    expect_run_invalid(config, owners);
    admission.device = (const device_info_t *)owners;
    expect_run_invalid(config, owners);
    admission.device = (const device_info_t *)((uintptr_t)owners +
        _Alignof(device_info_t));
    expect_run_invalid(config, owners);
    admission.device = (const device_info_t *)config;
    expect_run_invalid(config, owners);
    admission.device = (const device_info_t *)((uintptr_t)config +
        _Alignof(device_info_t));
    expect_run_invalid(config, owners);
    admission.device = (const device_info_t *)&admission;
    expect_run_invalid(config, owners);
    admission.device = (const device_info_t *)((uintptr_t)&admission +
        _Alignof(device_info_t));
    expect_run_invalid(config, owners);
    admission.device = &device;
    admission.agent_version = NULL;
    expect_run_invalid(config, owners);
    admission.agent_version = (const uint8_t *)(uintptr_t)(UINTPTR_MAX - 1u);
    expect_run_invalid(config, owners);
    admission.agent_version = (const uint8_t *)config;
    expect_run_invalid(config, owners);
    admission.agent_version = (const uint8_t *)owners;
    expect_run_invalid(config, owners);
    admission.agent_version = (const uint8_t *)((uintptr_t)owners + 1u);
    expect_run_invalid(config, owners);
    admission.agent_version = (const uint8_t *)&admission;
    expect_run_invalid(config, owners);
    admission.agent_version = (const uint8_t *)((uintptr_t)&admission + 1u);
    expect_run_invalid(config, owners);
    admission.agent_version = (const uint8_t *)&device;
    expect_run_invalid(config, owners);
    admission.agent_version = (const uint8_t *)((uintptr_t)&device + 1u);
    expect_run_invalid(config, owners);
    admission.agent_version_bytes = 2;
    admission.agent_version = (const uint8_t *)(UINTPTR_MAX - 1u);
    expect_run_invalid(config, owners);
    admission.agent_version = (const uint8_t *)&device;
    admission.agent_version_bytes = 1;
    expect_run_invalid(config, owners);
    admission.agent_version = (const uint8_t *)config;
    expect_run_invalid(config, owners);
    admission.agent_version = (const uint8_t *)((uintptr_t)config + 1u);
    expect_run_invalid(config, owners);
    admission.agent_version = (const uint8_t *)owners;
    expect_run_invalid(config, owners);
    admission.agent_version = (const uint8_t *)((uintptr_t)owners + 1u);
    expect_run_invalid(config, owners);
    admission.agent_version = (const uint8_t *)&admission;
    expect_run_invalid(config, owners);
    admission.agent_version = (const uint8_t *)((uintptr_t)&admission + 1u);
    expect_run_invalid(config, owners);
    admission.agent_version_bytes = sizeof(version) - 1;
    admission.agent_version = good_version;

    admission.connection_token = 0;
    expect_run_invalid(config, owners);
    admission.connection_token = 1;

    int nonblocking_pipe[2];
    int close_on_exec_pipe[2];
    assert(pipe2(nonblocking_pipe, O_CLOEXEC) == 0);
    config->stop_fd = nonblocking_pipe[0];
    admission.cancel_fd = nonblocking_pipe[0];
    expect_run_invalid(config, owners);
    close(nonblocking_pipe[0]);
    close(nonblocking_pipe[1]);
    assert(pipe2(close_on_exec_pipe, O_NONBLOCK) == 0);
    config->stop_fd = close_on_exec_pipe[0];
    admission.cancel_fd = close_on_exec_pipe[0];
    expect_run_invalid(config, owners);
    close(close_on_exec_pipe[0]);
    close(close_on_exec_pipe[1]);
    config->stop_fd = stop_pipe[0];
    admission.cancel_fd = stop_pipe[0];
    close(daemon_pair[0]);
    close(daemon_pair[1]);
    close(stop_pipe[0]);
    close(stop_pipe[1]);

    interop_mapping_linux_view_t view;
    memset(&view, 0, sizeof(view));
    uint64_t heartbeat = 0;
    uint64_t next = 0;
    assert(host_control_heartbeat(&view, &heartbeat, &next, 0) ==
           HostControlMappingFailed);

    char mapping_path[96];
    int mapping_path_bytes = snprintf(mapping_path, sizeof(mapping_path),
        "/tmp/session-control-heartbeat-%ld", (long)getpid());
    assert(mapping_path_bytes > 0 && (size_t)mapping_path_bytes < sizeof(mapping_path));
    int heartbeat_fd = open(mapping_path,
        O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    assert(heartbeat_fd >= 0);
    assert(ftruncate(heartbeat_fd, (off_t)UINT64_C(0x80000000)) == 0);
    void *heartbeat_mapping = mmap(NULL, (size_t)UINT64_C(0x80000000),
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NORESERVE, heartbeat_fd, 0);
    assert(heartbeat_mapping != MAP_FAILED);
    assert(interop_mapping_initialize((uintptr_t)heartbeat_mapping,
        UINT64_C(0x80000000), UINT64_C(0x123456789abcdef0), 1) ==
        InteropMappingInitStatusOk);
    assert(munmap(heartbeat_mapping, (size_t)UINT64_C(0x80000000)) == 0);
    interop_mapping_linux_t *mapping_owner = NULL;
    assert(interop_mapping_linux_create(&mapping_owner) == InteropMappingLinuxOk);
    assert(interop_mapping_linux_open(mapping_owner, heartbeat_fd,
        InteropMappingLinuxBackingRegularPrivateFile) == InteropMappingLinuxOk);
    interop_shm_header_snapshot_t heartbeat_snapshot;
    assert(interop_mapping_linux_validate_header(mapping_owner,
        UINT64_C(0x123456789abcdef0), &heartbeat_snapshot) ==
        InteropMappingLinuxOk);
    assert(interop_mapping_linux_view(mapping_owner, &view) == InteropMappingLinuxOk);
    assert(interop_mapping_initialize((uintptr_t)view.mapping_base,
        view.mapping_length, UINT64_C(0x123456789abcdef0), 1) ==
        InteropMappingInitStatusOk);
    heartbeat = 0;
    next = 100;
    assert(host_control_heartbeat(&view, &heartbeat, &next, 50) ==
        HostControlSuccess);
    assert(heartbeat == 0 && next == 100);
    assert(host_control_heartbeat(&view, &heartbeat, &next, 100) ==
        HostControlSuccess);
    assert(heartbeat == 1 && next == 600);
    assert(host_control_heartbeat(&view, &heartbeat, &next,
        UINT64_MAX - 100) == HostControlSuccess);
    assert(heartbeat == 2 && next == UINT64_MAX);
    next = 0;
    session_control_host_private_fail_heartbeat_store();
    assert(host_control_heartbeat(&view, &heartbeat, &next,
        UINT64_MAX - 50) == HostControlMappingFailed);
    interop_shm_header_t *header = (interop_shm_header_t *)view.mapping_base;
    atomic_store_explicit(&header->host_heartbeat, UINT64_MAX,
        memory_order_relaxed);
    heartbeat = UINT64_MAX;
    assert(host_control_heartbeat(&view, &heartbeat, &next, UINT64_MAX) ==
        HostControlMappingFailed);
    atomic_store_explicit(&header->host_heartbeat, 5, memory_order_relaxed);
    heartbeat = 4;
    assert(host_control_heartbeat(&view, &heartbeat, &next, UINT64_MAX) ==
        HostControlMappingFailed);
    assert(interop_mapping_linux_close(mapping_owner, 1) == InteropMappingLinuxOk);
    assert(interop_mapping_linux_destroy(&mapping_owner) == InteropMappingLinuxOk);
    assert(close(heartbeat_fd) == 0);
    assert(unlink(mapping_path) == 0);
    assert(host_control_cleanup_owners(owners) == HostControlSuccess);
    assert(host_control_monotonic_ms() != UINT64_MAX);
}
