/**
 * @file session_control_host.c
 * @brief Real-file, daemon-stream, and socket-owner integration tests for host control runtime.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../../src/interop/interop_mapping_init.h"
#include "../../src/interop/session_control_host.h"
#include "../../src/daemon/daemon_wire.h"
#include "waddle/daemon_protocol.h"
#include "waddle/interop_memory.h"
#include "waddle/interop_protocol.h"
#include "../../src/interop/interop_native_atomic.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/vm_sockets.h>

#define TestMappingBytes UINT64_C(0x80000000)
#define TestSessionEpoch UINT64_C(0x1020304050607080)
#define TestRunMs 900u

void session_control_host_private_tests(void);
void session_control_host_private_fail_clock_call(uint32_t call);
void session_control_host_private_fail_heartbeat_store(void);
#ifdef WaddleSessionNoRuntimeMain
void session_control_host_private_fail_clock_call(uint32_t call)
{
    (void)call;
}
#endif
void session_main_private_tests(void);
int waddle_session_main(int argc, char **argv);
static void *guest_peer(void *unused);
static void make_mapping_file(const char *path);
static void run_guest_script(int fd);
static size_t make_launch_result(uint8_t *frame, size_t capacity, uint32_t request_id);

static pthread_mutex_t peer_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t peer_condition = PTHREAD_COND_INITIALIZER;
static int vhost_peer_fd = -1;
static uint32_t vhost_connect_count;
static bool peer_bad_epoch;
static bool peer_send_feature;
static bool peer_send_error;
static bool peer_send_goodbye;
static bool arm_peer_send_goodbye;
static bool arm_peer_send_error;
static bool peer_disconnect_after_hello;
static uint32_t peer_delay_hello_ms;
static const char *peer_agent_version = "1.2.3";
static bool fail_munmap_once;
static bool test_fail_mmap_once;
static bool fail_mapping_view_once;
static bool fail_session_destroy_once;
static bool fail_session_release_once;
static bool fail_session_destroy_status_once;
static bool fail_session_release_status_once;
static bool fail_mapping_destroy_status_once;
static bool session_destroy_diagnostic_once;
static bool session_release_diagnostic_once;
static bool mapping_close_diagnostic_once;
static atomic_uint peer_saw_hello_ack;
static atomic_uint peer_saw_goodbye;
static atomic_uint peer_saw_error;
static bool fail_runtime_poll_once;
static uint32_t fail_runtime_poll_eintr_count;
static bool arm_runtime_poll_eintr_after_admission;
static bool arm_long_poll_timeout_after_success;
static bool arm_clock_advance_after_poll_events;
static bool fail_runtime_poll_refresh;
static bool arm_poll_after_admission;
static bool arm_refresh_poll_after_admission;
static bool arm_close_stop_fd;
static bool fail_stop_read_eagain;
static bool fail_stop_read_eio;
static uint32_t fail_stop_read_eintr_count;
static bool fail_stop_read_short;
static bool fail_stop_read_wrong_signal;
static int runtime_stop_fd = -1;
static bool arm_bad_mapping_layout_event;
static bool arm_nonzero_ready_capabilities;
static bool arm_closed_transport_status;
static bool arm_admission_failure_after_success;
static bool arm_mapping_create_failure_after_success;
static bool arm_poll_events_failure_after_success;
static bool arm_mapping_result_failure_after_success;
static bool arm_drain_failure_after_success;
static bool arm_socket_drive_failure_after_success;
static uint32_t arm_clock_anomaly_after_admission;
static uint32_t clock_anomaly_mode;
static bool fail_heartbeat_store_status_once;
static bool arm_heartbeat_mismatch_and_signal;
static bool arm_daemon_extra_byte;
static bool arm_daemon_peek_eagain_once;
static bool arm_daemon_peek_eio_once;
static bool runtime_thread_known;
static pthread_t runtime_thread;
static const interop_host_feature_ops_t *active_feature_ops;
/* The runtime entry used by run_control_fixture; session_app tests substitute their own runner. */
static interop_host_control_result_t fixture_run_default(
    const interop_host_control_config_t *config,
    interop_host_control_owners_t *owners,
    interop_host_control_detail_t *detail)
{
    return interop_host_control_run_detailed(config, owners, detail);
}
static interop_host_control_result_t (*fixture_control_runner)(
    const interop_host_control_config_t *, interop_host_control_owners_t *,
    interop_host_control_detail_t *) = fixture_run_default;
static interop_host_control_detail_t *fixture_detail_out;
static uint64_t peer_hello_capabilities = InteropCapWindows | InteropCapInput;
static uint64_t peer_expected_ack_capabilities;
static bool peer_script_enabled;
static uint32_t peer_script_announces;
static bool peer_script_lease_revoked;
static bool peer_script_echo_launch;
static bool peer_script_close_after;
static bool peer_script_suppress_pong;
static uint32_t peer_received_window_close;
static uint32_t peer_received_launch_requests;
static uint32_t peer_received_other_features;
static uint32_t peer_last_launch_request_id;
static uint8_t peer_received_close_bytes[64];
static size_t peer_received_close_length;

static int remove_entry(const char *path, const struct stat *status,
                        int type, struct FTW *walk_state)
{
    (void)status;
    (void)type;
    (void)walk_state;
    return remove(path);
}

extern int __real_socket(int domain, int type, int protocol);
extern int __real_connect(int fd, const struct sockaddr *address,
                          socklen_t address_length);
extern int __real_poll(struct pollfd *descriptors, nfds_t count, int timeout);
extern interop_host_session_result_t __real_interop_host_session_admit(
    const interop_host_session_config_t *config,
    interop_linux_session_owner_t **out_owner,
    uint32_t *out_control_channel_invalidated);
extern int __real_getsockopt(int fd, int level, int option, void *value,
                             socklen_t *length);
extern int __real_getpeername(int fd, struct sockaddr *address,
                              socklen_t *address_length);
extern int __real_clock_gettime(clockid_t clock_id, struct timespec *time_value);
extern interop_atomic_status_t __real_interop_heartbeat_store_next(
    interop_shm_header_t *header, uintptr_t mapping_base,
    uint64_t mapping_length, uint32_t writer, uint64_t value);
extern interop_atomic_status_t __real_interop_heartbeat_load(
    const interop_shm_header_t *header, uintptr_t mapping_base,
    uint64_t mapping_length, interop_heartbeat_kind_t kind, uint64_t *value);
extern ssize_t __real_send(int fd, const void *buffer, size_t length, int flags);
extern ssize_t __real_recv(int fd, void *buffer, size_t length, int flags);
extern ssize_t __real_read(int fd, void *buffer, size_t length);
extern int __real_munmap(void *address, size_t length);
extern void *__real_mmap(void *address, size_t length, int protection,
                         int flags, int fd, off_t offset);
extern interop_mapping_linux_status_t __real_interop_mapping_linux_view(
    interop_mapping_linux_t *owner, interop_mapping_linux_view_t *view_out);
extern interop_linux_session_result_t __real_interop_linux_session_destroy_peer(
    interop_linux_session_owner_t *owner);
extern interop_linux_session_result_t __real_interop_linux_session_release(
    interop_linux_session_owner_t **owner_slot);
extern int __real_interop_linux_socket_drive(
    interop_linux_socket_t *owner, uint64_t token, uint64_t poll_cookie,
    uint64_t now_ms, uint32_t revents, size_t rx_byte_budget,
    size_t tx_byte_budget, uint32_t syscall_budget, uint8_t *frame_out,
    size_t frame_capacity, interop_linux_socket_work_t *out_work);
extern int __real_interop_linux_socket_poll_events(
    interop_linux_socket_t *owner, uint64_t token, uint64_t now_ms,
    interop_linux_socket_poll_t *out_poll);
extern int __real_interop_linux_socket_mapping_result(
    interop_linux_socket_t *owner, uint64_t token, uint64_t now_ms,
    uint64_t epoch, uint32_t layout_version, uint32_t succeeded,
    interop_peer_event_t *out_event);
extern int __real_interop_linux_socket_begin_drain(
    interop_linux_socket_t *owner, uint64_t token, uint64_t now_ms,
    interop_peer_event_t *out_event);
extern interop_mapping_linux_status_t __real_interop_mapping_linux_create(
    interop_mapping_linux_t **out_owner);
extern interop_mapping_linux_status_t __real_interop_mapping_linux_destroy(
    interop_mapping_linux_t **owner_inout);

interop_linux_session_result_t __wrap_interop_linux_session_destroy_peer(
    interop_linux_session_owner_t *owner)
{
    if (fail_session_destroy_once) {
        fail_session_destroy_once = false;
        return InteropLinuxSessionBadState;
    }
    interop_linux_session_result_t result =
        __real_interop_linux_session_destroy_peer(owner);
    if (result == InteropLinuxSessionSuccess && session_destroy_diagnostic_once) {
        session_destroy_diagnostic_once = false;
        return InteropLinuxSessionCloseDiagnostic;
    }
    if (result == InteropLinuxSessionSuccess && fail_session_destroy_status_once) {
        fail_session_destroy_status_once = false;
        return InteropLinuxSessionWrongThread;
    }
    return result;
}

interop_linux_session_result_t __wrap_interop_linux_session_release(
    interop_linux_session_owner_t **owner_slot)
{
    if (fail_session_release_once) {
        fail_session_release_once = false;
        return InteropLinuxSessionBadState;
    }
    interop_linux_session_result_t result =
        __real_interop_linux_session_release(owner_slot);
    if (result == InteropLinuxSessionSuccess && session_release_diagnostic_once) {
        session_release_diagnostic_once = false;
        return InteropLinuxSessionCloseDiagnostic;
    }
    if (result == InteropLinuxSessionSuccess && fail_session_release_status_once) {
        fail_session_release_status_once = false;
        return InteropLinuxSessionWrongThread;
    }
    return result;
}

interop_mapping_linux_status_t __wrap_interop_mapping_linux_destroy(
    interop_mapping_linux_t **owner_inout)
{
    interop_mapping_linux_status_t result =
        __real_interop_mapping_linux_destroy(owner_inout);
    if (result == InteropMappingLinuxOk && fail_mapping_destroy_status_once) {
        fail_mapping_destroy_status_once = false;
        return InteropMappingLinuxWrongThread;
    }
    return result;
}

extern interop_mapping_linux_status_t __real_interop_mapping_linux_close(
    interop_mapping_linux_t *owner, uint32_t local_users_quiesced);

interop_mapping_linux_status_t __wrap_interop_mapping_linux_close(
    interop_mapping_linux_t *owner, uint32_t local_users_quiesced)
{
    interop_mapping_linux_status_t result = __real_interop_mapping_linux_close(
        owner, local_users_quiesced);
    if (result == InteropMappingLinuxOk && mapping_close_diagnostic_once) {
        mapping_close_diagnostic_once = false;
        return InteropMappingLinuxCloseDiagnostic;
    }
    return result;
}

int __wrap_interop_linux_socket_drive(
    interop_linux_socket_t *owner, uint64_t token, uint64_t poll_cookie,
    uint64_t now_ms, uint32_t revents, size_t rx_byte_budget,
    size_t tx_byte_budget, uint32_t syscall_budget, uint8_t *frame_out,
    size_t frame_capacity, interop_linux_socket_work_t *out_work)
{
    int result = __real_interop_linux_socket_drive(owner, token, poll_cookie,
        now_ms, revents, rx_byte_budget, tx_byte_budget, syscall_budget,
        frame_out, frame_capacity, out_work);
    if (result != InteropLinuxSocketSuccess || out_work == NULL) return result;
    if (arm_bad_mapping_layout_event &&
        out_work->event.kind == InteropPeerEventValidateMapping) {
        arm_bad_mapping_layout_event = false;
        out_work->event.layout_version = InteropLayoutVersion + 1;
    } else if (arm_nonzero_ready_capabilities &&
        out_work->event.kind == InteropPeerEventReady) {
        arm_nonzero_ready_capabilities = false;
        out_work->event.capabilities = 1;
    } else if (arm_closed_transport_status) {
        arm_closed_transport_status = false;
        out_work->state = InteropLinuxSocketTransportClosed;
    }
    if (arm_socket_drive_failure_after_success) {
        arm_socket_drive_failure_after_success = false;
        return InteropLinuxSocketPeerFailure;
    }
    return result;
}

interop_mapping_linux_status_t __wrap_interop_mapping_linux_view(
    interop_mapping_linux_t *owner, interop_mapping_linux_view_t *view_out)
{
    if (fail_mapping_view_once) {
        fail_mapping_view_once = false;
        return InteropMappingLinuxBusy;
    }
    return __real_interop_mapping_linux_view(owner, view_out);
}

void *__wrap_mmap(void *address, size_t length, int protection,
                  int flags, int fd, off_t offset)
{
    if (test_fail_mmap_once) {
        test_fail_mmap_once = false;
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return __real_mmap(address, length, protection, flags, fd, offset);
}

int __wrap_munmap(void *address, size_t length)
{
    if (fail_munmap_once) {
        fail_munmap_once = false;
        errno = EIO;
        return -1;
    }
    return __real_munmap(address, length);
}

int __wrap_socket(int domain, int type, int protocol)
{
    if (domain != AF_VSOCK) return __real_socket(domain, type, protocol);
    (void)protocol;
    assert((type & SOCK_STREAM) != 0);
    int descriptors[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                      0, descriptors) == 0);
    assert(pthread_mutex_lock(&peer_mutex) == 0);
    assert(vhost_peer_fd == -1);
    vhost_peer_fd = descriptors[1];
    assert(pthread_cond_broadcast(&peer_condition) == 0);
    assert(pthread_mutex_unlock(&peer_mutex) == 0);
    return descriptors[0];
}

int __wrap_connect(int fd, const struct sockaddr *address,
                   socklen_t address_length)
{
    if (address->sa_family != AF_VSOCK)
        return __real_connect(fd, address, address_length);
    assert(address_length == sizeof(struct sockaddr_vm));
    const struct sockaddr_vm *vsock = (const struct sockaddr_vm *)address;
    assert(vsock->svm_cid == 33 && vsock->svm_port == InteropControlPort);
    runtime_thread = pthread_self();
    runtime_thread_known = true;
    ++vhost_connect_count;
    return 0;
}

int __wrap_poll(struct pollfd *descriptors, nfds_t count, int timeout)
{
    if (fail_runtime_poll_eintr_count != 0 && runtime_thread_known &&
        pthread_equal(runtime_thread, pthread_self())) {
        --fail_runtime_poll_eintr_count;
        errno = EINTR;
        return -1;
    }
    if (fail_runtime_poll_once && runtime_thread_known &&
        pthread_equal(runtime_thread, pthread_self())) {
        fail_runtime_poll_once = false;
        errno = EIO;
        return -1;
    }
    if (fail_runtime_poll_refresh && runtime_thread_known &&
        pthread_equal(runtime_thread, pthread_self()) && timeout == 0) {
        fail_runtime_poll_refresh = false;
        errno = EIO;
        return -1;
    }
    return __real_poll(descriptors, count, timeout);
}

int __wrap_clock_gettime(clockid_t clock_id, struct timespec *time_value)
{
    int result = __real_clock_gettime(clock_id, time_value);
    if (result == 0 && clock_anomaly_mode != 0 && runtime_thread_known &&
        pthread_equal(runtime_thread, pthread_self()) &&
        clock_id == CLOCK_MONOTONIC) {
        uint32_t mode = clock_anomaly_mode;
        clock_anomaly_mode = 0;
        if (mode == 1) time_value->tv_nsec = 1000000000L;
        else if (mode == 2) time_value->tv_sec = (time_t)INT64_MAX;
        else if (mode == 3) {
            errno = EIO;
            return -1;
        }
        else if (mode == 4) time_value->tv_nsec = -1;
        else if (mode == 5) time_value->tv_sec = -1;
        else if (mode == 6) {
            time_value->tv_sec = (time_t)(UINT64_MAX / UINT64_C(1000));
            time_value->tv_nsec = 0;
        } else if (mode == 7) ++time_value->tv_sec;
    }
    return result;
}

interop_atomic_status_t __wrap_interop_heartbeat_store_next(
    interop_shm_header_t *header, uintptr_t mapping_base,
    uint64_t mapping_length, uint32_t writer, uint64_t value)
{
    interop_atomic_status_t result = __real_interop_heartbeat_store_next(
        header, mapping_base, mapping_length, writer, value);
    if (result == InteropAtomicStatusOk && fail_heartbeat_store_status_once) {
        fail_heartbeat_store_status_once = false;
        return InteropAtomicStatusWrongState;
    }
    return result;
}

interop_atomic_status_t __wrap_interop_heartbeat_load(
    const interop_shm_header_t *header, uintptr_t mapping_base,
    uint64_t mapping_length, interop_heartbeat_kind_t kind, uint64_t *value)
{
    interop_atomic_status_t result = __real_interop_heartbeat_load(header,
        mapping_base, mapping_length, kind, value);
    if (result == InteropAtomicStatusOk && arm_heartbeat_mismatch_and_signal &&
        runtime_thread_known && pthread_equal(runtime_thread, pthread_self()) &&
        atomic_load(&peer_saw_hello_ack) != 0 && value != NULL) {
        arm_heartbeat_mismatch_and_signal = false;
        *value = UINT64_MAX;
        assert(pthread_kill(pthread_self(), SIGTERM) == 0);
    }
    return result;
}

void session_control_host_private_fail_heartbeat_store(void)
{
    fail_heartbeat_store_status_once = true;
}

interop_host_session_result_t __wrap_interop_host_session_admit(
    const interop_host_session_config_t *config,
    interop_linux_session_owner_t **out_owner,
    uint32_t *out_control_channel_invalidated)
{
    interop_host_session_result_t result = __real_interop_host_session_admit(
        config, out_owner, out_control_channel_invalidated);
    if (result == HostSessionSuccess && arm_admission_failure_after_success) {
        arm_admission_failure_after_success = false;
        return HostSessionBusy;
    }
    if (result == HostSessionSuccess && arm_clock_anomaly_after_admission != 0) {
        clock_anomaly_mode = arm_clock_anomaly_after_admission;
        arm_clock_anomaly_after_admission = 0;
    }
    if (result == HostSessionSuccess && arm_poll_after_admission)
        fail_runtime_poll_once = true;
    if (result == HostSessionSuccess && arm_runtime_poll_eintr_after_admission) {
        fail_runtime_poll_eintr_count = 1;
        arm_runtime_poll_eintr_after_admission = false;
    }
    if (result == HostSessionSuccess && arm_refresh_poll_after_admission) {
        fail_runtime_poll_refresh = true;
        arm_refresh_poll_after_admission = false;
    }
    return result;
}

interop_mapping_linux_status_t __wrap_interop_mapping_linux_create(
    interop_mapping_linux_t **out_owner)
{
    interop_mapping_linux_status_t result =
        __real_interop_mapping_linux_create(out_owner);
    if (result == InteropMappingLinuxOk && arm_mapping_create_failure_after_success) {
        arm_mapping_create_failure_after_success = false;
        return InteropMappingLinuxBusy;
    }
    return result;
}

int __wrap_interop_linux_socket_poll_events(
    interop_linux_socket_t *owner, uint64_t token, uint64_t now_ms,
    interop_linux_socket_poll_t *out_poll)
{
    int result = __real_interop_linux_socket_poll_events(owner, token, now_ms,
        out_poll);
    if (result == InteropLinuxSocketSuccess && arm_poll_events_failure_after_success) {
        arm_poll_events_failure_after_success = false;
        return InteropLinuxSocketPeerFailure;
    }
    if (result == InteropLinuxSocketSuccess && arm_long_poll_timeout_after_success) {
        arm_long_poll_timeout_after_success = false;
        out_poll->timeout_ms = 101u;
    }
    if (result == InteropLinuxSocketSuccess && arm_clock_advance_after_poll_events) {
        arm_clock_advance_after_poll_events = false;
        clock_anomaly_mode = 7;
    }
    return result;
}

int __wrap_interop_linux_socket_mapping_result(
    interop_linux_socket_t *owner, uint64_t token, uint64_t now_ms,
    uint64_t epoch, uint32_t layout_version, uint32_t succeeded,
    interop_peer_event_t *out_event)
{
    int result = __real_interop_linux_socket_mapping_result(owner, token,
        now_ms, epoch, layout_version, succeeded, out_event);
    if (result == InteropLinuxSocketSuccess && arm_mapping_result_failure_after_success) {
        arm_mapping_result_failure_after_success = false;
        return InteropLinuxSocketPeerFailure;
    }
    return result;
}

int __wrap_interop_linux_socket_begin_drain(
    interop_linux_socket_t *owner, uint64_t token, uint64_t now_ms,
    interop_peer_event_t *out_event)
{
    int result = __real_interop_linux_socket_begin_drain(owner, token,
        now_ms, out_event);
    if (result == InteropLinuxSocketSuccess && arm_drain_failure_after_success) {
        arm_drain_failure_after_success = false;
        return InteropLinuxSocketPeerFailure;
    }
    return result;
}

int __wrap_getsockopt(int fd, int level, int option, void *value,
                      socklen_t *length)
{
    socklen_t requested = length == NULL ? 0 : *length;
    int result = __real_getsockopt(fd, level, option, value, length);
    if (result == 0 || errno != EPERM) return result;
    if (level != SOL_SOCKET || length == NULL || value == NULL) return result;
    if (option == SO_TYPE && requested >= sizeof(int)) {
        *(int *)value = SOCK_STREAM;
        *length = sizeof(int);
        return 0;
    }
#ifdef SO_PEERCRED
    if (option == SO_PEERCRED && requested >= sizeof(struct ucred)) {
        struct ucred credentials = {
            .pid = getpid(), .uid = geteuid(), .gid = getegid(),
        };
        memcpy(value, &credentials, sizeof(credentials));
        *length = sizeof(credentials);
        return 0;
    }
#endif
    return result;
}

int __wrap_getpeername(int fd, struct sockaddr *address,
                       socklen_t *address_length)
{
    socklen_t requested = address_length == NULL ? 0 : *address_length;
    int result = __real_getpeername(fd, address, address_length);
    if (result == 0 || errno != EPERM) return result;
    if (address == NULL || address_length == NULL ||
        requested < sizeof(struct sockaddr_un)) {
        errno = EINVAL;
        return -1;
    }
    memset(address, 0, sizeof(struct sockaddr_un));
    address->sa_family = AF_UNIX;
    *address_length = sizeof(struct sockaddr_un);
    return 0;
}

ssize_t __wrap_send(int fd, const void *buffer, size_t length, int flags)
{
    ssize_t result = __real_send(fd, buffer, length, flags);
    if (result < 0 && errno == EPERM) result = write(fd, buffer, length);
    return result;
}

ssize_t __wrap_recv(int fd, void *buffer, size_t length, int flags)
{
    ssize_t result = __real_recv(fd, buffer, length, flags);
    if (result >= 0 && runtime_thread_known &&
        pthread_equal(runtime_thread, pthread_self()) && (flags & MSG_PEEK) != 0) {
        if (arm_daemon_peek_eagain_once) {
            arm_daemon_peek_eagain_once = false;
            errno = EAGAIN;
            return -1;
        }
        if (arm_daemon_peek_eio_once) {
            arm_daemon_peek_eio_once = false;
            errno = EIO;
            return -1;
        }
    }
    if (result < 0 && errno == EPERM) result = read(fd, buffer, length);
    return result;
}

ssize_t __wrap_read(int fd, void *buffer, size_t length)
{
    if (fd == runtime_stop_fd && runtime_thread_known &&
        pthread_equal(runtime_thread, pthread_self())) {
        if (fail_stop_read_eintr_count != 0) {
            --fail_stop_read_eintr_count;
            errno = EINTR;
            return -1;
        }
        if (fail_stop_read_eagain) {
            fail_stop_read_eagain = false;
            errno = EAGAIN;
            return -1;
        }
        if (fail_stop_read_eio) {
            fail_stop_read_eio = false;
            return __real_read(fd, buffer, length) < 0 ? -1 :
                (errno = EIO, -1);
        }
        if (fail_stop_read_short || fail_stop_read_wrong_signal) {
            ssize_t result = __real_read(fd, buffer, length);
            if (result == (ssize_t)sizeof(struct signalfd_siginfo)) {
                if (fail_stop_read_short) {
                    fail_stop_read_short = false;
                    return result - 1;
                }
                fail_stop_read_wrong_signal = false;
                ((struct signalfd_siginfo *)buffer)->ssi_signo = SIGUSR1;
            }
            return result;
        }
    }
    return __real_read(fd, buffer, length);
}

static void write_all(int fd, const void *buffer, size_t length)
{
    const uint8_t *bytes = buffer;
    size_t offset = 0;
    while (offset < length) {
        ssize_t result = send(fd, bytes + offset, length - offset, MSG_NOSIGNAL);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd descriptor = {.fd = fd, .events = POLLOUT};
            assert(poll(&descriptor, 1, 2000) > 0);
            continue;
        }
        assert(result > 0);
        offset += (size_t)result;
    }
}

static bool write_all_peer_may_close(int fd, const void *buffer, size_t length)
{
    const uint8_t *bytes = buffer;
    size_t offset = 0;
    while (offset < length) {
        ssize_t result = send(fd, bytes + offset, length - offset, MSG_NOSIGNAL);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd descriptor = {.fd = fd, .events = POLLOUT};
            int ready = poll(&descriptor, 1, 1000);
            if (ready > 0) continue;
            if (ready < 0 && errno == EINTR) continue;
            return false;
        }
        if (result < 0 && (errno == EPIPE || errno == ECONNRESET)) return false;
        if (result <= 0) return false;
        offset += (size_t)result;
    }
    return true;
}

static void read_all(int fd, void *buffer, size_t length)
{
    uint8_t *bytes = buffer;
    size_t offset = 0;
    while (offset < length) {
        ssize_t result = recv(fd, bytes + offset, length - offset, 0);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd descriptor = {.fd = fd, .events = POLLIN};
            assert(poll(&descriptor, 1, 5000) > 0);
            continue;
        }
        assert(result > 0);
        offset += (size_t)result;
    }
}

static uint16_t read_u16_le(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static bool try_read_interop_frame(int fd, uint8_t *frame, size_t capacity,
                                   size_t *out_length)
{
    assert(capacity >= InteropHeaderBytes);
    size_t offset = 0;
    while (offset < InteropHeaderBytes) {
        ssize_t result = recv(fd, frame + offset, InteropHeaderBytes - offset, 0);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd descriptor = {.fd = fd, .events = POLLIN};
            if (poll(&descriptor, 1, 5000) > 0) continue;
            return false;
        }
        if (result <= 0) return false;
        offset += (size_t)result;
    }
    size_t frame_length = InteropHeaderBytes + read_u32_le(frame + 8);
    if (frame_length > capacity) return false;
    offset = InteropHeaderBytes;
    while (offset < frame_length) {
        ssize_t result = recv(fd, frame + offset, frame_length - offset, 0);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd descriptor = {.fd = fd, .events = POLLIN};
            if (poll(&descriptor, 1, 5000) > 0) continue;
            return false;
        }
        if (result <= 0) return false;
        offset += (size_t)result;
    }
    *out_length = frame_length;
    return true;
}

static size_t make_hello(uint8_t *frame, size_t capacity)
{
    const uint8_t *agent_version = (const uint8_t *)peer_agent_version;
    size_t agent_version_bytes = strlen(peer_agent_version);
    interop_writer_t writer;
    size_t frame_length = 0;
    interop_writer_init(&writer, frame, capacity);
    assert(interop_put_uint(&writer, InteropTagCapabilities,
        peer_hello_capabilities) == InteropErrorNone);
    assert(interop_put_bytes(&writer, InteropTagAgentVersion, agent_version,
        agent_version_bytes) == InteropErrorNone);
    assert(interop_put_uint(&writer, InteropTagLayoutVersion,
        InteropLayoutVersion) == InteropErrorNone);
    assert(interop_put_uint(&writer, InteropTagSessionEpoch,
        peer_bad_epoch ? TestSessionEpoch + 1 : TestSessionEpoch) == InteropErrorNone);
    assert(interop_writer_finish(&writer, InteropMsgHello, 1, InteropFromGuest,
        &frame_length) == InteropErrorNone);
    return frame_length;
}

static size_t make_feature(uint8_t *frame, size_t capacity)
{
    static const uint8_t AppKey[] = "a-Microsoft_Windows_Explorer";
    interop_writer_t writer;
    size_t frame_length = 0;
    interop_writer_init(&writer, frame, capacity);
    assert(interop_put_uint(&writer, InteropTagWindow, 1) == InteropErrorNone);
    assert(interop_put_uint(&writer, InteropTagGeneration, 1) == InteropErrorNone);
    assert(interop_put_uint(&writer, InteropTagRole, 1) == InteropErrorNone);
    assert(interop_put_rect(&writer, InteropTagRect, 0, 0, 640, 480) == InteropErrorNone);
    assert(interop_put_bytes(&writer, InteropTagAppKey, AppKey,
        sizeof(AppKey) - 1) == InteropErrorNone);
    assert(interop_put_uint(&writer, InteropTagState, 0) == InteropErrorNone);
    assert(interop_put_uint(&writer, InteropTagDpi, 96) == InteropErrorNone);
    assert(interop_writer_finish(&writer, InteropMsgWindowAnnounce, 0,
        InteropFromGuest, &frame_length) == InteropErrorNone);
    return frame_length;
}

static size_t make_error(uint8_t *frame, size_t capacity)
{
    interop_writer_t writer;
    size_t frame_length = 0;
    interop_writer_init(&writer, frame, capacity);
    assert(interop_put_uint(&writer, InteropTagErrorCode, 1) == InteropErrorNone);
    assert(interop_writer_finish(&writer, InteropMsgError, 2, InteropFromGuest,
        &frame_length) == InteropErrorNone);
    return frame_length;
}

static size_t make_pong(uint8_t *frame, size_t capacity, uint32_t request_id)
{
    interop_writer_t writer;
    size_t frame_length = 0;
    interop_writer_init(&writer, frame, capacity);
    assert(interop_writer_finish(&writer, InteropMsgPong, request_id,
        InteropFromGuest, &frame_length) == InteropErrorNone);
    return frame_length;
}

static size_t make_guest_goodbye(uint8_t *frame, size_t capacity)
{
    interop_writer_t writer;
    size_t frame_length = 0;
    interop_writer_init(&writer, frame, capacity);
    assert(interop_put_uint(&writer, InteropTagStatus, InteropCancelled) ==
        InteropErrorNone);
    assert(interop_writer_finish(&writer, InteropMsgGoodbye, 0,
        InteropFromGuest, &frame_length) == InteropErrorNone);
    return frame_length;
}

typedef struct daemon_fixture_t {
    int fd;
    bool close_after_ready;
    bool send_unsolicited;
} daemon_fixture_t;

typedef struct stop_fixture_t {
    pthread_t target;
    int fd;
    bool close_fd;
    uint32_t delay_ms;
} stop_fixture_t;

typedef struct heartbeat_fixture_t {
    int fd;
    uint32_t delay_ms;
} heartbeat_fixture_t;

static interop_host_control_result_t run_control_fixture(
    bool bad_epoch, bool send_feature, bool close_daemon,
    uint32_t cancel_after_ms, bool disconnect_after_hello,
    bool fail_mmap_once, bool fail_poll_once, uint32_t fail_clock_call,
    bool fail_cleanup_once, uint32_t delay_hello_ms, bool fail_mapping_view,
    bool tamper_heartbeat, int run_ms, uint64_t *out_heartbeat);

static void write_daemon_response_header(int fd, uint16_t message_type,
                                         uint32_t sequence,
                                         uint32_t payload_length)
{
    waddle_daemon_header_t header = {
        .magic = WaddleDaemonMagic,
        .version = WaddleDaemonVersion,
        .msg_type = message_type,
        .sequence = sequence,
        .payload_len = payload_length,
    };
    write_all(fd, &header, sizeof(header));
}

static void read_daemon_request(int fd, waddle_daemon_header_t *header)
{
    uint8_t bytes[WaddleDaemonHeaderSize];
    read_all(fd, bytes, sizeof(bytes));
    assert(daemon_wire_decode_header(bytes, sizeof(bytes), header) == 0);
}

static void *daemon_server(void *opaque)
{
    daemon_fixture_t *fixture = opaque;
    waddle_daemon_header_t request;
    read_daemon_request(fixture->fd, &request);
    assert(request.msg_type == DaemonMsgStatusReq && request.payload_len == 0);
    waddle_daemon_status_resp_t status;
    memset(&status, 0, sizeof(status));
    status.subsystem_state = SubsystemStateRunning;
    status.daemon_pid = (uint32_t)getpid();
    status.qemu_pid = (uint32_t)getpid();
    status.vsock_cid = 33;
    status.vsock_port = 5242;
    status.memory_mb = 4096;
    status.vcpus = 4;
    write_daemon_response_header(fixture->fd, DaemonMsgStatusResp,
        request.sequence, sizeof(status));
    write_all(fixture->fd, &status, sizeof(status));

    read_daemon_request(fixture->fd, &request);
    assert(request.msg_type == DaemonMsgGuestReadyReq);
    assert(request.payload_len == sizeof(uint32_t));
    uint8_t timeout_bytes[sizeof(uint32_t)];
    read_all(fixture->fd, timeout_bytes, sizeof(timeout_bytes));
    waddle_daemon_guest_ready_resp_t ready = {.status = 0, .cid = 33};
    write_daemon_response_header(fixture->fd, DaemonMsgGuestReadyResp,
        request.sequence, sizeof(ready));
    write_all(fixture->fd, &ready, sizeof(ready));
    if (fixture->close_after_ready) {
        close(fixture->fd);
        return NULL;
    }
    if (fixture->send_unsolicited) {
        uint8_t byte = 1;
        write_all(fixture->fd, &byte, sizeof(byte));
        close(fixture->fd);
        return NULL;
    }

    uint8_t byte;
    for (;;) {
        ssize_t result = recv(fixture->fd, &byte, sizeof(byte), 0);
        if (result < 0 && errno == EINTR) continue;
        if (result == 0) break;
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd descriptor = {.fd = fixture->fd, .events = POLLIN};
            if (poll(&descriptor, 1, 5000) <= 0) break;
            continue;
        }
        assert(result > 0);
        assert(false && "runtime must not send unrelated daemon commands");
    }
    close(fixture->fd);
    return NULL;
}

typedef struct daemon_listener_fixture_t {
    int listener_fd;
    daemon_fixture_t daemon;
} daemon_listener_fixture_t;

typedef struct main_signal_fixture_t {
    pthread_t target;
    uint32_t delay_ms;
} main_signal_fixture_t;

static void *signal_main_thread(void *opaque)
{
    main_signal_fixture_t *fixture = opaque;
    struct timespec retry = {.tv_nsec = 10000000L};
    uint32_t attempts = 0;
    while (atomic_load(&peer_saw_hello_ack) == 0 && attempts < 1000) {
        (void)nanosleep(&retry, NULL);
        ++attempts;
    }
    assert(atomic_load(&peer_saw_hello_ack) == 1);
    struct timespec delay = {
        .tv_sec = (time_t)(fixture->delay_ms / 1000u),
        .tv_nsec = (long)(fixture->delay_ms % 1000u) * 1000000L,
    };
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) { }
    assert(pthread_kill(fixture->target, SIGTERM) == 0);
    return NULL;
}

static void *accept_daemon_server(void *opaque)
{
    daemon_listener_fixture_t *fixture = opaque;
    fixture->daemon.fd = accept4(fixture->listener_fd, NULL, NULL, SOCK_CLOEXEC);
    assert(fixture->daemon.fd >= 0);
    return daemon_server(&fixture->daemon);
}

static void write_text_file(const char *path, const char *contents)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    assert(fd >= 0);
    size_t length = strlen(contents);
    size_t offset = 0;
    while (offset < length) {
        ssize_t result = write(fd, contents + offset, length - offset);
        if (result < 0 && errno == EINTR) continue;
        assert(result > 0);
        offset += (size_t)result;
    }
    assert(close(fd) == 0);
}

static void run_main_entry_fixture(bool cancel_by_signal, bool break_daemon,
                                   bool fatal_cleanup)
{
    char root[] = "/tmp/session-main-runtime-XXXXXX";
    assert(mkdtemp(root) != NULL);
    char config_home[96];
    char state_home[96];
    char runtime_home[96];
    char config_parent[128];
    char config_dir[160];
    char state_parent[128];
    char state_devices[160];
    char state_device[192];
    char state_av[224];
    char runtime_parent[128];
    char runtime_device[160];
    char profile_path[224];
    char mapping_path[224];
    char socket_path[224];
    assert(snprintf(config_home, sizeof(config_home), "%s/config", root) > 0);
    assert(snprintf(state_home, sizeof(state_home), "%s/state", root) > 0);
    assert(snprintf(runtime_home, sizeof(runtime_home), "%s/runtime", root) > 0);
    assert(snprintf(config_parent, sizeof(config_parent), "%s/waddle", config_home) > 0);
    assert(snprintf(config_dir, sizeof(config_dir), "%s/devices", config_parent) > 0);
    assert(snprintf(state_parent, sizeof(state_parent), "%s/waddle", state_home) > 0);
    assert(snprintf(state_devices, sizeof(state_devices), "%s/devices", state_parent) > 0);
    assert(snprintf(state_device, sizeof(state_device), "%s/entry-test", state_devices) > 0);
    assert(snprintf(state_av, sizeof(state_av), "%s/av", state_device) > 0);
    assert(snprintf(runtime_parent, sizeof(runtime_parent), "%s/waddle", runtime_home) > 0);
    assert(snprintf(runtime_device, sizeof(runtime_device), "%s/entry-test", runtime_parent) > 0);
    assert(snprintf(profile_path, sizeof(profile_path), "%s/entry-test.ini", config_dir) > 0);
    assert(snprintf(mapping_path, sizeof(mapping_path), "%s/mapping", root) > 0);
    assert(snprintf(socket_path, sizeof(socket_path), "%s/daemon.sock", runtime_device) > 0);

    assert(mkdir(config_home, 0700) == 0);
    assert(mkdir(state_home, 0700) == 0);
    assert(mkdir(runtime_home, 0700) == 0);
    assert(mkdir(config_parent, 0700) == 0);
    assert(mkdir(config_dir, 0700) == 0);
    assert(mkdir(state_parent, 0700) == 0);
    assert(mkdir(state_devices, 0700) == 0);
    assert(mkdir(state_device, 0700) == 0);
    assert(mkdir(state_av, 0700) == 0);
    assert(mkdir(runtime_parent, 0700) == 0);
    assert(mkdir(runtime_device, 0700) == 0);
    assert(setenv("XDG_CONFIG_HOME", config_home, 1) == 0);
    assert(setenv("XDG_STATE_HOME", state_home, 1) == 0);
    assert(setenv("XDG_RUNTIME_DIR", runtime_home, 1) == 0);

    char profile[512];
    int profile_length = snprintf(profile, sizeof(profile),
        "[subsystem]\n"
        "disk_image = /tmp/session-main-entry.qcow2\n"
        "vsock_cid = 33\n"
        "vsock_port = 5242\n"
        "[av]\n"
        "enabled = 1\n"
        "shm_path = %s\n", mapping_path);
    assert(profile_length > 0 && (size_t)profile_length < sizeof(profile));
    write_text_file(profile_path, profile);
    make_mapping_file(mapping_path);

    int listener_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(listener_fd >= 0);
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    assert(strlen(socket_path) < sizeof(address.sun_path));
    memcpy(address.sun_path, socket_path, strlen(socket_path) + 1);
    if (bind(listener_fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        perror("session main daemon bind");
        assert(false);
    }
    assert(listen(listener_fd, 1) == 0);
    daemon_listener_fixture_t daemon = {
        .listener_fd = listener_fd,
        .daemon = {.fd = -1, .close_after_ready = break_daemon},
    };

    peer_bad_epoch = false;
    peer_send_feature = false;
    peer_send_error = false;
    peer_disconnect_after_hello = false;
    peer_delay_hello_ms = 0;
    peer_agent_version = "0.0.0";
    peer_saw_hello_ack = 0;
    peer_saw_goodbye = 0;
    peer_saw_error = 0;
    vhost_connect_count = 0;
    runtime_thread_known = false;
    fail_runtime_poll_once = false;
    fail_runtime_poll_refresh = false;
    arm_poll_after_admission = false;
    arm_refresh_poll_after_admission = false;
    pthread_t daemon_thread;
    pthread_t guest_thread;
    pthread_t signal_thread;
    bool signal_thread_started = false;
    assert(pthread_create(&daemon_thread, NULL, accept_daemon_server, &daemon) == 0);
    assert(pthread_create(&guest_thread, NULL, guest_peer, NULL) == 0);
    main_signal_fixture_t signal_fixture = {
        .target = pthread_self(),
        .delay_ms = 200,
    };
    if (cancel_by_signal) {
        assert(pthread_create(&signal_thread, NULL, signal_main_thread,
            &signal_fixture) == 0);
        signal_thread_started = true;
    }
    fail_munmap_once = fatal_cleanup;
    fail_session_destroy_once = fatal_cleanup;
    char duration[16];
    assert(snprintf(duration, sizeof(duration), "%u",
        cancel_by_signal ? 30000u : 1500u) > 0);
    char *arguments[] = {
        (char *)"waddle-session", (char *)"--control-only", (char *)"--device",
        (char *)"entry-test", (char *)"--run-for-ms", duration,
    };
    assert(waddle_session_main(6, arguments) ==
        (cancel_by_signal || break_daemon ? 1 : 0));
    assert(vhost_connect_count == 1);
    assert(pthread_join(guest_thread, NULL) == 0);
    if (signal_thread_started) assert(pthread_join(signal_thread, NULL) == 0);
    assert(pthread_join(daemon_thread, NULL) == 0);
    if (break_daemon) {
        assert(peer_saw_hello_ack <= 1);
        if (peer_saw_hello_ack == 1) assert(peer_saw_goodbye == 1);
    } else {
        assert(peer_saw_hello_ack == 1 && peer_saw_goodbye == 1);
    }
    assert(close(listener_fd) == 0);
    peer_agent_version = "1.2.3";
    assert(unsetenv("XDG_CONFIG_HOME") == 0);
    assert(unsetenv("XDG_STATE_HOME") == 0);
    assert(unsetenv("XDG_RUNTIME_DIR") == 0);
    assert(nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0);
}

static void run_fatal_cleanup_child(void)
{
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        run_main_entry_fixture(false, false, true);
        _exit(0);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 1);
}

void session_control_host_main_entry_tests(void)
{
    assert(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
    (void)&run_control_fixture;
    run_main_entry_fixture(false, false, false);
    run_main_entry_fixture(true, false, false);
    run_main_entry_fixture(false, true, false);
    run_fatal_cleanup_child();
}

static void *stop_after_delay(void *opaque)
{
    stop_fixture_t *fixture = opaque;
    struct timespec delay = {
        .tv_sec = (time_t)(fixture->delay_ms / 1000u),
        .tv_nsec = (long)(fixture->delay_ms % 1000u) * 1000000L,
    };
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) { }
    if (fixture->close_fd) {
        assert(close(fixture->fd) == 0);
        return NULL;
    }
    assert(pthread_kill(fixture->target, SIGTERM) == 0);
    return NULL;
}

static void *tamper_host_heartbeat(void *opaque)
{
    heartbeat_fixture_t *fixture = opaque;
    struct timespec delay = {
        .tv_sec = (time_t)(fixture->delay_ms / 1000u),
        .tv_nsec = (long)(fixture->delay_ms % 1000u) * 1000000L,
    };
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) { }
    void *mapping = mmap(NULL, (size_t)TestMappingBytes, PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_NORESERVE, fixture->fd, 0);
    assert(mapping != MAP_FAILED);
    interop_shm_header_t *header = mapping;
    atomic_store_explicit(&header->host_heartbeat, 99, memory_order_relaxed);
    assert(munmap(mapping, (size_t)TestMappingBytes) == 0);
    return NULL;
}

static size_t make_launch_result(uint8_t *frame, size_t capacity, uint32_t request_id)
{
    interop_writer_t writer;
    size_t frame_length = 0;
    interop_writer_init(&writer, frame, capacity);
    assert(interop_put_uint(&writer, InteropTagProcessId, 4242) == InteropErrorNone);
    assert(interop_put_uint(&writer, InteropTagStatus, InteropOk) == InteropErrorNone);
    assert(interop_writer_finish(&writer, InteropMsgLaunchResult, request_id,
        InteropFromGuest, &frame_length) == InteropErrorNone);
    return frame_length;
}

static size_t make_lease_revoked(uint8_t *frame, size_t capacity)
{
    interop_writer_t writer;
    size_t frame_length = 0;
    interop_writer_init(&writer, frame, capacity);
    assert(interop_put_uint(&writer, InteropTagLeaseEpoch, 3) == InteropErrorNone);
    assert(interop_writer_finish(&writer, InteropMsgLeaseRevoked, 0,
        InteropFromGuest, &frame_length) == InteropErrorNone);
    return frame_length;
}

/* Scripted guest traffic sent immediately after HelloAck, before the guest reads again. */
static void run_guest_script(int fd)
{
    uint8_t frame[InteropMaxFrameBytes];
    size_t frame_length;
    uint32_t index;

    for (index = 0; index < peer_script_announces; ++index) {
        frame_length = make_feature(frame, sizeof(frame));
        write_all(fd, frame, frame_length);
    }
    if (peer_script_lease_revoked) {
        frame_length = make_lease_revoked(frame, sizeof(frame));
        write_all(fd, frame, frame_length);
    }
}

static void *guest_peer(void *unused)
{
    (void)unused;
    assert(pthread_mutex_lock(&peer_mutex) == 0);
    while (vhost_peer_fd < 0)
        assert(pthread_cond_wait(&peer_condition, &peer_mutex) == 0);
    int fd = vhost_peer_fd;
    assert(pthread_mutex_unlock(&peer_mutex) == 0);
    if (peer_delay_hello_ms != 0) {
        struct timespec delay = {
            .tv_sec = (time_t)(peer_delay_hello_ms / 1000u),
            .tv_nsec = (long)(peer_delay_hello_ms % 1000u) * 1000000L,
        };
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR) { }
    }
    uint8_t frame[InteropMaxFrameBytes];
    size_t frame_length = make_hello(frame, sizeof(frame));
    if (!write_all_peer_may_close(fd, frame, frame_length)) {
        close(fd);
        assert(pthread_mutex_lock(&peer_mutex) == 0);
        vhost_peer_fd = -1;
        assert(pthread_mutex_unlock(&peer_mutex) == 0);
        return NULL;
    }
    if (peer_disconnect_after_hello) {
        close(fd);
        assert(pthread_mutex_lock(&peer_mutex) == 0);
        vhost_peer_fd = -1;
        assert(pthread_mutex_unlock(&peer_mutex) == 0);
        return NULL;
    }
    for (;;) {
        if (!try_read_interop_frame(fd, frame, sizeof(frame), &frame_length)) break;
        uint16_t type = read_u16_le(frame + 6);
        if (type == InteropMsgHelloAck) {
            uint64_t capabilities = UINT64_MAX;
            assert(interop_field_uint(frame + InteropHeaderBytes,
                frame_length - InteropHeaderBytes, InteropTagCapabilities, 8,
                &capabilities) == 0);
            assert(capabilities == peer_expected_ack_capabilities);
            peer_saw_hello_ack = 1;
            if (peer_script_enabled) run_guest_script(fd);
            if (peer_script_close_after) break;
            if (peer_send_feature) {
                frame_length = make_feature(frame, sizeof(frame));
                write_all(fd, frame, frame_length);
            }
            if (peer_send_error) {
                frame_length = make_error(frame, sizeof(frame));
                write_all(fd, frame, frame_length);
            }
        } else if (type == InteropMsgPing) {
            if (peer_script_suppress_pong) continue;
            if (peer_send_goodbye) {
                peer_send_goodbye = false;
                frame_length = make_guest_goodbye(frame, sizeof(frame));
            } else {
                frame_length = make_pong(frame, sizeof(frame), read_u32_le(frame + 12));
            }
            write_all(fd, frame, frame_length);
        } else if (peer_script_enabled && type == InteropMsgLaunchRequest) {
            ++peer_received_launch_requests;
            peer_last_launch_request_id = read_u32_le(frame + 12);
            if (peer_script_echo_launch) {
                frame_length = make_launch_result(frame, sizeof(frame), peer_last_launch_request_id);
                write_all(fd, frame, frame_length);
            }
        } else if (peer_script_enabled && type == InteropMsgWindowClose) {
            ++peer_received_window_close;
            assert(frame_length <= sizeof(peer_received_close_bytes));
            memcpy(peer_received_close_bytes, frame, frame_length);
            peer_received_close_length = frame_length;
        } else if (peer_script_enabled && type >= 0x0100 && type < 0x7F00) {
            ++peer_received_other_features;
        } else if (type == InteropMsgGoodbye) {
            peer_saw_goodbye = 1;
            break;
        } else if (type == InteropMsgError) {
            peer_saw_error = 1;
            if (peer_bad_epoch) break;
            continue;
        } else {
            assert(false && "unexpected host control frame");
        }
    }
    close(fd);
    assert(pthread_mutex_lock(&peer_mutex) == 0);
    vhost_peer_fd = -1;
    assert(pthread_mutex_unlock(&peer_mutex) == 0);
    return NULL;
}

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void setup_paths(char root[64])
{
    strcpy(root, "/tmp/session-control-host-XXXXXX");
    assert(mkdtemp(root) != NULL);
    char state_dir[96];
    char runtime_dir[96];
    assert(snprintf(state_dir, sizeof(state_dir), "%s/state", root) > 0);
    assert(snprintf(runtime_dir, sizeof(runtime_dir), "%s/runtime", root) > 0);
    assert(mkdir(state_dir, 0700) == 0);
    assert(mkdir(runtime_dir, 0700) == 0);
    assert(setenv("XDG_STATE_HOME", state_dir, 1) == 0);
    assert(setenv("XDG_RUNTIME_DIR", runtime_dir, 1) == 0);
}

static void make_mapping_file(const char *path)
{
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    assert(fd >= 0);
    assert(ftruncate(fd, (off_t)TestMappingBytes) == 0);
    void *mapping = mmap(NULL, (size_t)TestMappingBytes, PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_NORESERVE, fd, 0);
    assert(mapping != MAP_FAILED);
    assert(interop_mapping_initialize((uintptr_t)mapping, TestMappingBytes,
        TestSessionEpoch, 1) == InteropMappingInitStatusOk);
    assert(munmap(mapping, (size_t)TestMappingBytes) == 0);
    assert(close(fd) == 0);
}

static int create_stop_signal_fd(sigset_t *prior_mask)
{
    sigset_t blocked;
    sigemptyset(&blocked);
    assert(sigaddset(&blocked, SIGINT) == 0);
    assert(sigaddset(&blocked, SIGTERM) == 0);
    assert(pthread_sigmask(SIG_BLOCK, &blocked, prior_mask) == 0);
    int fd = signalfd(-1, &blocked, SFD_NONBLOCK | SFD_CLOEXEC);
    if (fd < 0) assert(pthread_sigmask(SIG_SETMASK, prior_mask, NULL) == 0);
    return fd;
}

static interop_host_control_result_t run_control_fixture(
    bool bad_epoch, bool send_feature, bool close_daemon,
    uint32_t cancel_after_ms, bool disconnect_after_hello,
    bool fail_mmap_once, bool fail_poll_once, uint32_t fail_clock_call,
    bool fail_cleanup_once, uint32_t delay_hello_ms, bool fail_mapping_view,
    bool tamper_heartbeat,
    int run_ms, uint64_t *out_heartbeat)
{
    char root[64];
    setup_paths(root);
    char mapping_path[96];
    assert(snprintf(mapping_path, sizeof(mapping_path), "%s/mapping", root) > 0);
    make_mapping_file(mapping_path);
    int mapping_fd = open(mapping_path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    assert(mapping_fd >= 0);
    int daemon_pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, daemon_pair) == 0);
    sigset_t prior_signal_mask;
    int stop_fd = create_stop_signal_fd(&prior_signal_mask);
    assert(stop_fd >= 0);
    pthread_t daemon_thread;
    pthread_t guest_thread;
    pthread_t stop_thread;
    pthread_t heartbeat_thread;
    bool stop_thread_started = false;
    bool heartbeat_thread_started = false;
    stop_fixture_t stop = {
        .target = pthread_self(), .fd = stop_fd,
        .close_fd = arm_close_stop_fd, .delay_ms = cancel_after_ms,
    };
    arm_close_stop_fd = false;
    daemon_fixture_t daemon = {
        .fd = daemon_pair[1],
        .close_after_ready = close_daemon,
        .send_unsolicited = arm_daemon_extra_byte,
    };
    arm_daemon_extra_byte = false;
    arm_daemon_peek_eagain_once = false;
    arm_daemon_peek_eio_once = false;
    peer_bad_epoch = bad_epoch;
    peer_send_feature = send_feature;
    peer_send_goodbye = arm_peer_send_goodbye;
    arm_peer_send_goodbye = false;
    peer_send_error = arm_peer_send_error;
    arm_peer_send_error = false;
    peer_disconnect_after_hello = disconnect_after_hello;
    peer_delay_hello_ms = delay_hello_ms;
    peer_agent_version = "1.2.3";
    peer_saw_hello_ack = 0;
    peer_saw_goodbye = 0;
    peer_saw_error = 0;
    vhost_connect_count = 0;
    runtime_thread_known = false;
    fail_runtime_poll_once = false;
    arm_poll_after_admission = fail_poll_once;
    assert(pthread_create(&daemon_thread, NULL, daemon_server, &daemon) == 0);
    assert(pthread_create(&guest_thread, NULL, guest_peer, NULL) == 0);
    if (cancel_after_ms != 0) {
        assert(pthread_create(&stop_thread, NULL, stop_after_delay, &stop) == 0);
        stop_thread_started = true;
    }
    heartbeat_fixture_t heartbeat_fixture = {
        .fd = mapping_fd,
        .delay_ms = 700,
    };
    if (tamper_heartbeat) {
        assert(pthread_create(&heartbeat_thread, NULL, tamper_host_heartbeat,
            &heartbeat_fixture) == 0);
        heartbeat_thread_started = true;
    }
    device_info_t device;
    memset(&device, 0, sizeof(device));
    strcpy(device.name, "session-control-test");
    assert(daemon_device_get_state_dir(device.name, device.state_dir,
        sizeof(device.state_dir)) == 0);
    assert(daemon_device_get_socket_path(device.name, device.socket_path,
        sizeof(device.socket_path)) == 0);
    strcpy(device.config_path, "/tmp/session-control-test.ini");
    strcpy(device.disk_image, "/tmp/session-control-test.qcow2");
    device.vsock_cid = 33;
    device.vsock_port = 5242;
    device.memory_mb = 4096;
    device.vcpus = 4;
    device.config_valid = 1;
    static const uint8_t AgentVersion[] = "1.2.3";
    interop_host_session_config_t admission = {
        .device = &device,
        .daemon_fd = daemon_pair[0],
        .agent_version = AgentVersion,
        .agent_version_bytes = sizeof(AgentVersion) - 1,
        .connection_token = UINT64_C(0x1122334455667788),
        .local_capabilities = active_feature_ops != NULL ? active_feature_ops->local_capabilities : 0,
        .deadline_ms = monotonic_ms() + 5000,
        .cancel_fd = stop_fd,
        .guest_ready_timeout_ms = 1000,
        .drain_timeout_ms = 1000,
        .reserved_zero = 0,
    };
    interop_host_control_config_t config = {
        .admission = &admission,
        .mapping_fd = mapping_fd,
        .backing_kind = InteropMappingLinuxBackingRegularPrivateFile,
        .stop_fd = stop_fd,
        .run_for_ms = (uint32_t)run_ms,
        .reserved_zero = 0,
        .feature_ops = active_feature_ops,
    };
    interop_host_control_owners_t owners = {0};
    runtime_stop_fd = stop_fd;
    test_fail_mmap_once = fail_mmap_once;
    fail_mapping_view_once = fail_mapping_view;
    fail_munmap_once = fail_cleanup_once;
    fail_session_destroy_once = false;
    fail_session_release_once = false;
    session_control_host_private_fail_clock_call(fail_clock_call);
    interop_host_control_result_t result = fixture_control_runner(&config,
        &owners, fixture_detail_out);
    runtime_stop_fd = -1;
    arm_poll_after_admission = false;
    if (fail_cleanup_once) {
        assert(result == HostControlCleanupRequired);
        assert(owners.mapping_owner != NULL && owners.session_owner != NULL);
        fail_session_destroy_once = true;
        assert(interop_host_control_retry_cleanup(&owners) ==
               HostControlCleanupRequired);
        assert(owners.mapping_owner == NULL && owners.session_owner != NULL);
        fail_session_release_once = true;
        assert(interop_host_control_retry_cleanup(&owners) ==
               HostControlCleanupRequired);
        assert(owners.mapping_owner == NULL && owners.session_owner != NULL);
        assert(interop_host_control_retry_cleanup(&owners) == HostControlSuccess);
    } else if (result == HostControlCleanupRequired) {
        /* Fault wrappers returned an inconsistent diagnostic after the real release;
         * retry authentic remaining owners and prove the no-owner terminal state. */
        assert(interop_host_control_retry_cleanup(&owners) == HostControlSuccess);
    }
    assert(owners.mapping_owner == NULL && owners.session_owner == NULL);
    assert(vhost_connect_count == 1);
    assert(pthread_join(guest_thread, NULL) == 0);
    if (stop_thread_started) assert(pthread_join(stop_thread, NULL) == 0);
    if (heartbeat_thread_started) assert(pthread_join(heartbeat_thread, NULL) == 0);
    close(daemon_pair[0]);
    assert(pthread_join(daemon_thread, NULL) == 0);
    if (fail_stop_read_eio) fail_stop_read_eio = false;
    if (!stop.close_fd) {
        struct signalfd_siginfo pending_signal;
        (void)read(stop_fd, &pending_signal, sizeof(pending_signal));
        close(stop_fd);
    }
    assert(pthread_sigmask(SIG_SETMASK, &prior_signal_mask, NULL) == 0);

    if (out_heartbeat != NULL) {
        void *mapping = mmap(NULL, (size_t)TestMappingBytes, PROT_READ,
            MAP_SHARED | MAP_NORESERVE, mapping_fd, 0);
        assert(mapping != MAP_FAILED);
        interop_shm_header_t *header = mapping;
        *out_heartbeat = atomic_load_explicit(&header->host_heartbeat,
                                               memory_order_relaxed);
        assert(munmap(mapping, (size_t)TestMappingBytes) == 0);
    }
    close(mapping_fd);
    assert(unsetenv("XDG_STATE_HOME") == 0);
    assert(unsetenv("XDG_RUNTIME_DIR") == 0);
    assert(nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0);
    return result;
}


#ifndef WaddleSessionNoRuntimeMain
/* ---- Feature ops: frame builders shared by the guest script and host callbacks ---- */

static size_t build_window_close(uint8_t *frame, size_t capacity, uint64_t window,
                                 uint64_t generation, uint32_t request_id)
{
    interop_writer_t writer;
    size_t frame_length = 0;
    interop_writer_init(&writer, frame, capacity);
    assert(interop_put_uint(&writer, InteropTagWindow, window) == InteropErrorNone);
    assert(interop_put_uint(&writer, InteropTagGeneration, generation) == InteropErrorNone);
    assert(interop_writer_finish(&writer, InteropMsgWindowClose, request_id,
        InteropFromHost, &frame_length) == InteropErrorNone);
    return frame_length;
}

static size_t build_launch_request(uint8_t *frame, size_t capacity, uint32_t request_id)
{
    static const uint8_t AppKey[] = "a-Microsoft_Windows_Explorer";
    interop_writer_t writer;
    size_t frame_length = 0;
    interop_writer_init(&writer, frame, capacity);
    assert(interop_put_bytes(&writer, InteropTagAppKey, AppKey, sizeof(AppKey) - 1) == InteropErrorNone);
    assert(interop_put_uint(&writer, InteropTagLaunchKind, InteropLaunchApp) == InteropErrorNone);
    assert(interop_writer_finish(&writer, InteropMsgLaunchRequest, request_id,
        InteropFromHost, &frame_length) == InteropErrorNone);
    return frame_length;
}

static size_t build_keyboard_layout(uint8_t *frame, size_t capacity)
{
    static const uint8_t Layout[] = "00000409";
    interop_writer_t writer;
    size_t frame_length = 0;
    interop_writer_init(&writer, frame, capacity);
    assert(interop_put_bytes(&writer, InteropTagLayout, Layout, sizeof(Layout) - 1) == InteropErrorNone);
    assert(interop_writer_finish(&writer, InteropMsgKeyboardLayout, 0,
        InteropFromHost, &frame_length) == InteropErrorNone);
    return frame_length;
}

/* ---- Host feature probe: records every callback and the sender results it observes ---- */

typedef struct feature_probe_t {
    bool skip_ready;
    bool skip_frame;
    bool skip_tick;
    bool use_pipe;
    bool use_deadline;
    bool negative_probe;
    bool reentry;
    bool release_twice;
    bool send_window_close;
    bool send_on_closing;
    bool tick_writes_pipe;
    uint32_t fds_mode;
    uint32_t poll_fail_after;
    uint32_t poll_calls;
    int ready_status;
    int tick_status;
    int frame_status;
    int fds_status;
    uint32_t announce_fail_at;
    uint32_t launch_target;
    uint64_t negotiated;
    uint32_t ready_calls;
    uint32_t frame_calls;
    uint32_t announce_frames;
    uint32_t launch_results;
    uint32_t launch_matched;
    uint32_t release_ok;
    uint32_t release_unknown;
    uint32_t tick_calls;
    uint32_t deadline_calls;
    uint32_t launch_sent;
    uint32_t launch_queue_full;
    uint32_t launch_refused;
    uint32_t window_close_sent;
    uint32_t request_id;
    uint32_t fd_calls;
    uint32_t fd_bytes;
    uint32_t closing_calls;
    uint32_t closing_reason;
    interop_host_feature_result_t closing_send;
    interop_host_feature_result_t closing_release;
    interop_host_control_result_t reentry_run;
    interop_host_control_result_t reentry_cleanup;
    interop_host_feature_result_t negative[12];
    uint8_t close_bytes[64];
    size_t close_length;
    int pipe_read;
    int pipe_write;
} feature_probe_t;

static int probe_on_ready(void *context, uint64_t negotiated, interop_host_feature_sender_t *sender)
{
    feature_probe_t *probe = context;
    uint8_t frame[InteropMaxFrameBytes];
    size_t length;
    uint32_t request_id = 0;

    ++probe->ready_calls;
    probe->negotiated = negotiated;
    if (probe->reentry) {
        probe->reentry_run = interop_host_control_run(NULL, NULL);
        probe->reentry_cleanup = interop_host_control_retry_cleanup(NULL);
    }
    if (probe->negative_probe) {
        length = build_window_close(frame, sizeof(frame), 7, 1, 0);
        probe->negative[0] = interop_host_feature_send(NULL, frame, length);
        probe->negative[1] = interop_host_feature_send(sender, NULL, 16);
        probe->negative[2] = interop_host_feature_send(sender, frame, length - 1);
        length = build_launch_request(frame, sizeof(frame), 0);
        probe->negative[3] = interop_host_feature_send(sender, frame, length);
        probe->negative[4] = interop_host_feature_send_request(sender, frame, length, NULL);
        length = build_launch_request(frame, sizeof(frame), 5);
        probe->negative[5] = interop_host_feature_send_request(sender, frame, length, &request_id);
        length = build_window_close(frame, sizeof(frame), 7, 1, 0);
        probe->negative[6] = interop_host_feature_send_request(sender, frame, length, &request_id);
        probe->negative[7] = interop_host_feature_send(sender, frame, length - 1);
        length = build_window_close(frame, sizeof(frame), 7, 1, 9);
        probe->negative[8] = interop_host_feature_send(sender, frame, length);
        length = build_keyboard_layout(frame, sizeof(frame));
        probe->negative[9] = interop_host_feature_send(sender, frame, length);
        probe->negative[10] = interop_host_feature_release_request(NULL, 1);
        probe->negative[11] = interop_host_feature_release_request(sender, 4242);
    }
    return probe->ready_status;
}

static int probe_on_frame(void *context, uint32_t message_type, uint32_t request_id,
                          const uint8_t *frame, size_t length, interop_host_feature_sender_t *sender)
{
    feature_probe_t *probe = context;
    (void)frame;
    (void)length;
    ++probe->frame_calls;
    if (message_type == InteropMsgWindowAnnounce) {
        ++probe->announce_frames;
        if (probe->announce_fail_at != 0 && probe->announce_frames == probe->announce_fail_at)
            return 2;
    } else if (message_type == InteropMsgLaunchResult) {
        ++probe->launch_results;
        if (request_id == probe->request_id) ++probe->launch_matched;
        if (interop_host_feature_release_request(sender, request_id) == HostFeatureSuccess)
            ++probe->release_ok;
        if (probe->release_twice &&
            interop_host_feature_release_request(sender, request_id) == HostFeatureUnknownRequestId)
            ++probe->release_unknown;
    }
    return probe->frame_status;
}

static int probe_on_tick(void *context, uint64_t now_ms, interop_host_feature_sender_t *sender)
{
    feature_probe_t *probe = context;
    uint8_t frame[InteropMaxFrameBytes];
    size_t length;
    uint32_t request_id = 0;
    interop_host_feature_result_t result;

    (void)now_ms;
    ++probe->tick_calls;
    if (probe->tick_writes_pipe && probe->tick_calls == 1) {
        assert(write(probe->pipe_write, "x", 1) == 1);
    }
    if (probe->send_window_close && probe->window_close_sent == 0) {
        length = build_window_close(frame, sizeof(frame), 0x55, 2, 0);
        assert(length <= sizeof(probe->close_bytes));
        memcpy(probe->close_bytes, frame, length);
        probe->close_length = length;
        if (interop_host_feature_send(sender, frame, length) == HostFeatureSuccess)
            ++probe->window_close_sent;
    }
    if (probe->launch_target != 0 && probe->launch_sent < probe->launch_target) {
        length = build_launch_request(frame, sizeof(frame), 0);
        result = interop_host_feature_send_request(sender, frame, length, &request_id);
        if (result == HostFeatureSuccess) {
            ++probe->launch_sent;
            probe->request_id = request_id;
        } else if (result == HostFeatureQueueFull) {
            ++probe->launch_queue_full;
        } else {
            ++probe->launch_refused;
        }
    }
    return probe->tick_status;
}

static int probe_poll_fds(void *context, struct pollfd *fds, size_t capacity, size_t *count)
{
    feature_probe_t *probe = context;
    if (probe->poll_fail_after != 0 && ++probe->poll_calls > probe->poll_fail_after) return 1;
    if (probe->fds_mode == 1) return 1;
    if (probe->fds_mode == 2) {
        *count = capacity + 1;
        return 0;
    }
    if (!probe->use_pipe) {
        *count = 0;
        return 0;
    }
    fds[0].fd = probe->pipe_read;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    *count = 1;
    return 0;
}

static int probe_on_fds(void *context, const struct pollfd *fds, size_t count,
                        uint64_t now_ms, interop_host_feature_sender_t *sender)
{
    feature_probe_t *probe = context;
    uint8_t buffer[16];
    size_t index;
    (void)now_ms;
    (void)sender;
    ++probe->fd_calls;
    for (index = 0; index < count; ++index) {
        if ((fds[index].revents & POLLIN) != 0) {
            ssize_t received = read(fds[index].fd, buffer, sizeof(buffer));
            if (received > 0) probe->fd_bytes += (uint32_t)received;
        }
    }
    return probe->fds_status;
}

static uint64_t probe_next_deadline(void *context, uint64_t now_ms)
{
    feature_probe_t *probe = context;
    ++probe->deadline_calls;
    return now_ms + 15;
}

static void probe_on_closing(void *context, uint32_t reason, interop_host_feature_sender_t *sender)
{
    feature_probe_t *probe = context;
    uint8_t frame[InteropMaxFrameBytes];
    size_t length;

    ++probe->closing_calls;
    probe->closing_reason = reason;
    if (probe->send_on_closing) {
        length = build_window_close(frame, sizeof(frame), 0x66, 3, 0);
        probe->closing_send = interop_host_feature_send(sender, frame, length);
        probe->closing_release = interop_host_feature_release_request(sender, 9999);
    }
}

static interop_host_control_detail_t feature_case_detail;

/* Run one control session with the probe's callbacks; minimal variants omit callbacks. */
static interop_host_control_result_t run_feature_case(feature_probe_t *probe,
    uint64_t local_capabilities, uint64_t hello_capabilities, uint64_t ack_capabilities,
    int run_ms, uint32_t cancel_after_ms, uint64_t *heartbeat)
{
    interop_host_feature_ops_t ops;
    interop_host_control_result_t result;

    peer_received_window_close = 0;
    peer_received_launch_requests = 0;
    peer_received_other_features = 0;
    peer_last_launch_request_id = 0;
    peer_received_close_length = 0;
    memset(&ops, 0, sizeof(ops));
    ops.context = probe;
    ops.local_capabilities = local_capabilities;
    ops.on_ready = probe->skip_ready ? NULL : probe_on_ready;
    ops.on_frame = probe->skip_frame ? NULL : probe_on_frame;
    ops.on_tick = probe->skip_tick ? NULL : probe_on_tick;
    ops.poll_fds = probe->use_pipe || probe->fds_mode != 0 || probe->poll_fail_after != 0
        ? probe_poll_fds : NULL;
    ops.on_fds = probe->use_pipe || probe->fds_mode != 0 ? probe_on_fds : NULL;
    ops.next_deadline_ms = probe->use_deadline ? probe_next_deadline : NULL;
    ops.on_closing = probe_on_closing;
    peer_hello_capabilities = hello_capabilities;
    peer_expected_ack_capabilities = ack_capabilities;
    peer_script_enabled = true;
    active_feature_ops = &ops;
    memset(&feature_case_detail, 0, sizeof(feature_case_detail));
    fixture_detail_out = &feature_case_detail;
    result = run_control_fixture(false, false, false, cancel_after_ms, false, false, false, 0,
        false, 0, false, false, run_ms, heartbeat);
    fixture_detail_out = NULL;
    active_feature_ops = NULL;
    peer_script_enabled = false;
    peer_script_announces = 0;
    peer_script_lease_revoked = false;
    peer_script_echo_launch = false;
    peer_script_close_after = false;
    peer_script_suppress_pong = false;
    peer_hello_capabilities = InteropCapWindows | InteropCapInput;
    peer_expected_ack_capabilities = 0;
    return result;
}

static void reset_probe_pipe(feature_probe_t *probe)
{
    int pipe_fds[2];
    assert(pipe2(pipe_fds, O_NONBLOCK | O_CLOEXEC) == 0);
    probe->pipe_read = pipe_fds[0];
    probe->pipe_write = pipe_fds[1];
}

static void close_probe_pipe(feature_probe_t *probe)
{
    assert(close(probe->pipe_read) == 0);
    assert(close(probe->pipe_write) == 0);
}

__attribute__((unused)) static void session_control_host_feature_tests(void)
{
    static const uint64_t WindowsInput = InteropCapWindows | InteropCapInput;
    static const uint64_t WindowsInputLaunch = InteropCapWindows | InteropCapInput | InteropCapLaunch;
    feature_probe_t probe;
    interop_host_control_result_t result;
    uint64_t heartbeat = 0;

    /* 1. Exchange in both directions, every sender refusal, release semantics and exact bytes. */
    memset(&probe, 0, sizeof(probe));
    probe.negative_probe = true;
    probe.send_window_close = true;
    probe.release_twice = true;
    probe.launch_target = 1;
    probe.send_on_closing = true;
    peer_script_announces = 3;
    peer_script_echo_launch = true;
    result = run_feature_case(&probe, WindowsInputLaunch, WindowsInputLaunch,
        WindowsInputLaunch, 700, 0, &heartbeat);
    assert(result == HostControlSuccess);
    assert(probe.ready_calls == 1 && probe.negotiated == WindowsInputLaunch);
    assert(probe.announce_frames == 3);
    assert(probe.launch_results == 1 && probe.launch_matched == 1);
    assert(probe.release_ok == 1 && probe.release_unknown == 1);
    assert(probe.window_close_sent == 1 && probe.launch_sent == 1);
    assert(probe.negative[0] == HostFeatureInvalidArgument);
    assert(probe.negative[1] == HostFeatureInvalidArgument);
    assert(probe.negative[2] == HostFeatureInvalidFrame);
    assert(probe.negative[3] == HostFeatureUnsupportedFrame);
    assert(probe.negative[4] == HostFeatureInvalidArgument);
    assert(probe.negative[5] == HostFeatureInvalidFrame);
    assert(probe.negative[6] == HostFeatureUnsupportedFrame);
    assert(probe.negative[7] == HostFeatureInvalidFrame);
    assert(probe.negative[8] == HostFeatureInvalidFrame);
    assert(probe.negative[9] == HostFeatureNotNegotiated);
    assert(probe.negative[10] == HostFeatureInvalidArgument);
    assert(probe.negative[11] == HostFeatureUnknownRequestId);
    assert(probe.closing_calls == 1 && probe.closing_reason == HostControlSuccess);
    assert(feature_case_detail.reached_ready == 1 && feature_case_detail.ready_at_ms != 0);
    assert(feature_case_detail.ended_at_ms >= feature_case_detail.ready_at_ms);
    assert(feature_case_detail.guest_error == 0 && feature_case_detail.admission_result == 0);
    assert(probe.closing_send == HostFeatureDraining);
    assert(probe.closing_release == HostFeatureUnknownRequestId);
    assert(peer_received_window_close == 1);
    assert(peer_received_close_length == probe.close_length);
    assert(memcmp(peer_received_close_bytes, probe.close_bytes, probe.close_length) == 0);
    assert(peer_received_launch_requests == 1 && peer_last_launch_request_id == probe.request_id);
    assert(peer_saw_hello_ack == 1 && peer_saw_goodbye == 1);

    /* 2. Failure of an inbound callback drains with FeatureFailed and closes exactly once. */
    memset(&probe, 0, sizeof(probe));
    probe.announce_fail_at = 1;
    peer_script_announces = 2;
    result = run_feature_case(&probe, WindowsInput, WindowsInput, WindowsInput, 2000, 0, NULL);
    assert(result == HostControlFeatureFailed);
    assert(probe.frame_calls == 1 && probe.closing_calls == 1);
    assert(probe.closing_reason == HostControlFeatureFailed);

    /* 3. on_ready failure is the same failure path before any traffic. */
    memset(&probe, 0, sizeof(probe));
    probe.ready_status = 2;
    result = run_feature_case(&probe, WindowsInput, WindowsInput, WindowsInput, 2000, 0, NULL);
    assert(result == HostControlFeatureFailed);
    assert(probe.ready_calls == 1 && probe.closing_calls == 1);
    assert(probe.closing_reason == HostControlFeatureFailed);

    /* 4. on_tick failure drains; tick cadence stays bounded before the failure. */
    memset(&probe, 0, sizeof(probe));
    probe.tick_status = 2;
    result = run_feature_case(&probe, WindowsInput, WindowsInput, WindowsInput, 2000, 0, NULL);
    assert(result == HostControlFeatureFailed);
    assert(probe.tick_calls == 1 && probe.closing_calls == 1);

    /* 5. Extra descriptor readiness reaches on_fds; deadline and ticks keep firing. */
    memset(&probe, 0, sizeof(probe));
    probe.use_pipe = true;
    probe.use_deadline = true;
    probe.tick_writes_pipe = true;
    reset_probe_pipe(&probe);
    result = run_feature_case(&probe, WindowsInput, WindowsInput, WindowsInput, 400, 0, NULL);
    assert(result == HostControlSuccess);
    assert(probe.fd_calls >= 1 && probe.fd_bytes == 1);
    assert(probe.deadline_calls >= 3 && probe.tick_calls >= 5);
    assert(probe.closing_calls == 1);
    close_probe_pipe(&probe);

    /* 6. on_fds failure drains with FeatureFailed. */
    memset(&probe, 0, sizeof(probe));
    probe.use_pipe = true;
    probe.tick_writes_pipe = true;
    probe.fds_status = 2;
    reset_probe_pipe(&probe);
    result = run_feature_case(&probe, WindowsInput, WindowsInput, WindowsInput, 2000, 0, NULL);
    assert(result == HostControlFeatureFailed);
    assert(probe.fd_calls == 1 && probe.closing_calls == 1);
    close_probe_pipe(&probe);

    /* 7. poll_fds failure and an over-capacity descriptor count both fail the run. */
    memset(&probe, 0, sizeof(probe));
    probe.fds_mode = 1;
    result = run_feature_case(&probe, WindowsInput, WindowsInput, WindowsInput, 2000, 0, NULL);
    assert(result == HostControlFeatureFailed);
    assert(probe.closing_calls == 1);
    /* 7b. A poll_fds failure after successful cycles still sends Goodbye and closes once. */
    memset(&probe, 0, sizeof(probe));
    probe.use_pipe = true;
    probe.poll_fail_after = 2;
    reset_probe_pipe(&probe);
    result = run_feature_case(&probe, WindowsInput, WindowsInput, WindowsInput, 2000, 0, NULL);
    assert(result == HostControlFeatureFailed);
    assert(probe.closing_calls == 1);
    assert(peer_saw_goodbye == 1);
    close_probe_pipe(&probe);
    memset(&probe, 0, sizeof(probe));
    probe.fds_mode = 2;
    result = run_feature_case(&probe, WindowsInput, WindowsInput, WindowsInput, 2000, 0, NULL);
    assert(result == HostControlFeatureFailed);
    assert(probe.closing_calls == 1);

    /* 8. No run bound: the run lasts until cancellation, and closing sends are refused. */
    memset(&probe, 0, sizeof(probe));
    probe.send_on_closing = true;
    result = run_feature_case(&probe, WindowsInput, WindowsInput, WindowsInput, 0, 300, NULL);
    assert(result == HostControlCancelled);
    assert(probe.closing_calls == 1 && probe.closing_reason == HostControlCancelled);
    assert(probe.closing_send == HostFeatureDraining);
    assert(probe.closing_release == HostFeatureUnknownRequestId);

    /* 9. Inbound feature not negotiated: UnexpectedFeature through the peer's refusal. */
    memset(&probe, 0, sizeof(probe));
    peer_script_lease_revoked = true;
    result = run_feature_case(&probe, InteropCapWindows, WindowsInput, InteropCapWindows,
        2000, 0, NULL);
    assert(result == HostControlUnexpectedFeature);
    assert(probe.frame_calls == 0 && probe.closing_reason == HostControlUnexpectedFeature);
    assert(feature_case_detail.guest_error == 1 && feature_case_detail.reached_ready == 1);

    /* 10. Minimal ops (no on_ready, on_frame, on_tick): an admitted frame is unexpected. */
    memset(&probe, 0, sizeof(probe));
    probe.skip_ready = true;
    probe.skip_frame = true;
    probe.skip_tick = true;
    peer_script_announces = 1;
    result = run_feature_case(&probe, WindowsInput, WindowsInput, WindowsInput, 2000, 0, NULL);
    assert(result == HostControlUnexpectedFeature);
    assert(probe.ready_calls == 0 && probe.tick_calls == 0 && probe.closing_calls == 1);

    /* 11. Minimal ops with a quiet guest finish at the run bound with no feature traffic. */
    memset(&probe, 0, sizeof(probe));
    probe.skip_ready = true;
    probe.skip_frame = true;
    probe.skip_tick = true;
    result = run_feature_case(&probe, WindowsInput, WindowsInput, WindowsInput, 200, 0, NULL);
    assert(result == HostControlSuccess && probe.closing_calls == 1);

    /* 12. Re-entering run or retry_cleanup from a callback is refused. */
    memset(&probe, 0, sizeof(probe));
    probe.reentry = true;
    result = run_feature_case(&probe, WindowsInput, WindowsInput, WindowsInput, 300, 0, NULL);
    assert(result == HostControlSuccess);
    assert(probe.reentry_run == HostControlInvalidArgument);
    assert(probe.reentry_cleanup == HostControlInvalidArgument);

    /* 13. Guest disconnect after Ready ends the run with PeerFailed and one closing call. */
    memset(&probe, 0, sizeof(probe));
    peer_script_announces = 1;
    peer_script_close_after = true;
    result = run_feature_case(&probe, WindowsInput, WindowsInput, WindowsInput, 2000, 0, NULL);
    assert(result == HostControlPeerFailed);
    assert(probe.closing_calls == 1 && probe.closing_reason == HostControlPeerFailed);

    /* 14. Stress: 2000 identifiers each way, interleaved with heartbeats and ticks. */
    memset(&probe, 0, sizeof(probe));
    probe.launch_target = 2000;
    probe.release_twice = false;
    peer_script_announces = 2000;
    peer_script_echo_launch = true;
    result = run_feature_case(&probe, WindowsInputLaunch, WindowsInputLaunch,
        WindowsInputLaunch, 3000, 0, &heartbeat);
    assert(result == HostControlSuccess);
    assert(probe.launch_sent == 2000 && probe.launch_results == 2000);
    assert(probe.release_ok == 2000);
    assert(probe.announce_frames == 2000);
    assert(peer_received_launch_requests == 2000);
    assert(heartbeat >= 3);
    assert(probe.closing_calls == 1);

    /* 15. A graceful stop request from a feature callback ends the run as Cancelled, not failed. */
    memset(&probe, 0, sizeof(probe));
    probe.tick_status = InteropHostFeatureStopRequested;
    result = run_feature_case(&probe, WindowsInput, WindowsInput, WindowsInput, 2000, 0, NULL);
    assert(result == HostControlCancelled);
    assert(probe.tick_calls == 1 && probe.closing_calls == 1);
    assert(probe.closing_reason == HostControlCancelled);

    /* 15b. An admission refusal reports the session-layer result through the detail output. */
    {
        interop_host_control_detail_t refused;
        interop_host_control_result_t refusal;
        memset(&refused, 0, sizeof(refused));
        fixture_detail_out = &refused;
        arm_admission_failure_after_success = true;
        refusal = run_control_fixture(false, false, false, 0, false, false, false, 0,
            false, 0, false, false, TestRunMs, NULL);
        fixture_detail_out = NULL;
        assert(refusal == HostControlAdmissionFailed);
        assert(refused.admission_result == HostSessionBusy && refused.reached_ready == 0);

        /* 15c. A failed monotonic read in cleanup records ended_at_ms as zero. */
        memset(&refused, 0, sizeof(refused));
        fixture_detail_out = &refused;
        arm_admission_failure_after_success = true;
        refusal = run_control_fixture(false, false, false, 0, false, false, false, 1,
            false, 0, false, false, TestRunMs, NULL);
        fixture_detail_out = NULL;
        assert(refusal == HostControlAdmissionFailed);
        assert(refused.admission_result == HostSessionBusy && refused.ended_at_ms == 0);
    }

    /* 16. A stop request from on_ready is the same graceful path before any traffic. */
    memset(&probe, 0, sizeof(probe));
    probe.ready_status = InteropHostFeatureStopRequested;
    result = run_feature_case(&probe, WindowsInput, WindowsInput, WindowsInput, 2000, 0, NULL);
    assert(result == HostControlCancelled);
    assert(probe.ready_calls == 1 && probe.closing_calls == 1);
    assert(probe.closing_reason == HostControlCancelled);

    /* 17. Any other nonzero status from a callback still fails the run. */
    memset(&probe, 0, sizeof(probe));
    probe.tick_status = 2;
    result = run_feature_case(&probe, WindowsInput, WindowsInput, WindowsInput, 2000, 0, NULL);
    assert(result == HostControlFeatureFailed);
}

#endif

#ifndef WaddleSessionNoRuntimeMain
int main(void)
{
    assert(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
    session_control_host_private_tests();
    session_control_host_feature_tests();
    session_main_private_tests();
    run_main_entry_fixture(false, false, false);
    run_main_entry_fixture(true, false, false);
    run_main_entry_fixture(false, true, false);
    run_fatal_cleanup_child();
    uint64_t heartbeat = 0;
    assert(run_control_fixture(false, false, false, 0, false, false, false, 0, false, 0, false, false,
        TestRunMs, &heartbeat) ==
           HostControlSuccess);
    assert(heartbeat >= 1);
    assert(peer_saw_hello_ack == 1 && peer_saw_goodbye == 1);
    mapping_close_diagnostic_once = true;
    session_destroy_diagnostic_once = true;
    session_release_diagnostic_once = true;
    assert(run_control_fixture(false, false, false, 0, false, false, false, 0,
        false, 0, false, false, TestRunMs, NULL) == HostControlSuccess);
    fail_mapping_destroy_status_once = true;
    assert(run_control_fixture(false, false, false, 0, false, false, false, 0,
        false, 0, false, false, TestRunMs, NULL) == HostControlCleanupRequired);
    fail_session_destroy_status_once = true;
    assert(run_control_fixture(false, false, false, 0, false, false, false, 0,
        false, 0, false, false, TestRunMs, NULL) == HostControlCleanupRequired);
    fail_session_release_status_once = true;
    assert(run_control_fixture(false, false, false, 0, false, false, false, 0,
        false, 0, false, false, TestRunMs, NULL) == HostControlCleanupRequired);
    arm_admission_failure_after_success = true;
    assert(run_control_fixture(false, false, false, 0, false, false, false, 0,
        false, 0, false, false, TestRunMs, NULL) == HostControlAdmissionFailed);
    arm_mapping_create_failure_after_success = true;
    assert(run_control_fixture(false, false, false, 0, false, false, false, 0,
        false, 0, false, false, TestRunMs, NULL) == HostControlMappingFailed);
    arm_poll_events_failure_after_success = true;
    assert(run_control_fixture(false, false, false, 0, false, false, false, 0,
        false, 0, false, false, TestRunMs, NULL) == HostControlPeerFailed);
    arm_mapping_result_failure_after_success = true;
    assert(run_control_fixture(false, false, false, 0, false, false, false, 0,
        false, 0, false, false, TestRunMs, NULL) == HostControlPeerFailed);
    arm_drain_failure_after_success = true;
    assert(run_control_fixture(false, true, false, 0, false, false, false, 0,
        false, 0, false, false, TestRunMs, NULL) == HostControlUnexpectedFeature);
    for (uint32_t anomaly = 1; anomaly <= 6; ++anomaly) {
        arm_clock_anomaly_after_admission = anomaly;
        assert(run_control_fixture(false, false, false, 0, false, false, false, 0,
            false, 0, false, false, TestRunMs, NULL) == HostControlOsError);
    }
    arm_heartbeat_mismatch_and_signal = true;
    assert(run_control_fixture(false, false, false, 0, false, false, false, 0,
        false, 0, false, false, TestRunMs, NULL) == HostControlMappingFailed);
    arm_heartbeat_mismatch_and_signal = true;
    arm_drain_failure_after_success = true;
    assert(run_control_fixture(false, false, false, 0, false, false, false, 0,
        false, 0, false, false, TestRunMs, NULL) == HostControlMappingFailed);
    arm_runtime_poll_eintr_after_admission = true;
    interop_host_control_result_t poll_eintr_result = run_control_fixture(
        false, false, false, 0, false, false, false, 0,
        false, 0, false, false, TestRunMs, NULL);
    if (poll_eintr_result != HostControlSuccess)
        fprintf(stderr, "poll EINTR result=%u\\n", (unsigned)poll_eintr_result);
    assert(poll_eintr_result == HostControlSuccess);
    arm_socket_drive_failure_after_success = true;
    assert(run_control_fixture(false, false, false, 0, false, false, false, 0,
        false, 0, false, false, TestRunMs, NULL) == HostControlPeerFailed);
    arm_long_poll_timeout_after_success = true;
    assert(run_control_fixture(false, false, false, 0, false, false, false, 0,
        false, 0, false, false, TestRunMs, NULL) == HostControlSuccess);
    arm_clock_advance_after_poll_events = true;
    assert(run_control_fixture(false, false, false, 0, false, false, false, 0,
        false, 0, false, false, TestRunMs, NULL) == HostControlSuccess);
    arm_daemon_extra_byte = true;
    assert(run_control_fixture(false, false, false, 0, false, false, false, 0,
        false, 0, false, false, TestRunMs, NULL) == HostControlDaemonLost);
    arm_daemon_extra_byte = true;
    arm_daemon_peek_eagain_once = true;
    assert(run_control_fixture(false, false, false, 0, false, false, false, 0,
        false, 0, false, false, TestRunMs, NULL) == HostControlDaemonLost);
    arm_daemon_extra_byte = true;
    arm_daemon_peek_eio_once = true;
    assert(run_control_fixture(false, false, false, 0, false, false, false, 0,
        false, 0, false, false, TestRunMs, NULL) == HostControlDaemonLost);
    arm_peer_send_goodbye = true;
    assert(run_control_fixture(false, false, false, 0, false, false, false, 0,
        false, 0, false, false, 6000, NULL) == HostControlPeerFailed);
    assert(run_control_fixture(true, false, false, 0, false, false, false, 0, false, 0, false, false,
        500, NULL) == HostControlMappingFailed);
    assert(peer_saw_hello_ack == 0 && peer_saw_error == 1);
    arm_mapping_result_failure_after_success = true;
    assert(run_control_fixture(true, false, false, 0, false, false, false, 0,
        false, 0, false, false, 500, NULL) == HostControlPeerFailed);
    arm_bad_mapping_layout_event = true;
    assert(run_control_fixture(false, false, false, 0, false, false, false, 0,
        false, 0, false, false, 500, NULL) == HostControlMappingFailed);
    arm_bad_mapping_layout_event = true;
    arm_mapping_result_failure_after_success = true;
    assert(run_control_fixture(false, false, false, 0, false, false, false, 0,
        false, 0, false, false, 500, NULL) == HostControlPeerFailed);
    assert(run_control_fixture(false, false, false, 0, false, false, false, 0,
        false, 0, true, false, 500, NULL) == HostControlMappingFailed);
    assert(peer_saw_hello_ack == 0 && peer_saw_error == 1);
    assert(run_control_fixture(false, true, false, 0, false, false, false, 0, false, 0, false, false,
        500, NULL) ==
           HostControlUnexpectedFeature);
    /* The refused unnegotiated frame is answered by Error(UnsupportedType), not Goodbye. */
    assert(peer_saw_hello_ack == 1 && peer_saw_error == 1 && peer_saw_goodbye == 0);
    arm_nonzero_ready_capabilities = true;
    assert(run_control_fixture(false, false, false, 0, false, false, false, 0,
        false, 0, false, false, 500, NULL) == HostControlPeerFailed);
    arm_closed_transport_status = true;
    assert(run_control_fixture(false, false, false, 0, false, false, false, 0,
        false, 0, false, false, 500, NULL) == HostControlPeerFailed);
    arm_peer_send_error = true;
    assert(run_control_fixture(false, false, false, 0, false, false, false, 0,
        false, 0, false, false, 500, NULL) == HostControlPeerFailed);
    assert(run_control_fixture(false, false, false, 150, false, false, false, 0, false, 0, false, false,
        1000, NULL) ==
           HostControlCancelled);
    assert(peer_saw_hello_ack == 1 && peer_saw_goodbye == 1);
    fail_stop_read_eagain = true;
    assert(run_control_fixture(false, false, false, 150, false, false, false, 0,
        false, 0, false, false, 1000, NULL) == HostControlCancelled);
    fail_stop_read_eintr_count = 1;
    assert(run_control_fixture(false, false, false, 150, false, false, false, 0,
        false, 0, false, false, 1000, NULL) == HostControlCancelled);
    fail_stop_read_eintr_count = 8;
    assert(run_control_fixture(false, false, false, 150, false, false, false, 0,
        false, 0, false, false, 1000, NULL) == HostControlOsError);
    fail_stop_read_short = true;
    assert(run_control_fixture(false, false, false, 150, false, false, false, 0,
        false, 0, false, false, 1000, NULL) == HostControlOsError);
    fail_stop_read_wrong_signal = true;
    assert(run_control_fixture(false, false, false, 150, false, false, false, 0,
        false, 0, false, false, 1000, NULL) == HostControlOsError);
    fail_stop_read_eio = true;
    assert(run_control_fixture(false, false, false, 150, false, false, false, 0,
        false, 0, false, false, 1000, NULL) == HostControlOsError);
    arm_close_stop_fd = true;
    assert(run_control_fixture(false, false, false, 150, false, false, false, 0,
        false, 0, false, false, 1000, NULL) == HostControlOsError);
    arm_refresh_poll_after_admission = true;
    interop_host_control_result_t refresh_failure = run_control_fixture(
        false, false, false, 150, false, false, false, 0,
        false, 0, false, false, 1000, NULL);
    assert(refresh_failure == HostControlCancelled);
    assert(run_control_fixture(false, false, true, 0, false, false, false, 0, false, 0, false, false,
        1000, NULL) ==
           HostControlDaemonLost);
    assert(run_control_fixture(false, false, false, 0, true, false, false, 0, false, 0, false, false,
        500, NULL) == HostControlPeerFailed);
    assert(run_control_fixture(false, false, false, 0, true, true, false, 0, false, 0, false, false,
        500, NULL) == HostControlMappingFailed);
    assert(run_control_fixture(false, false, false, 0, true, false, true, 0, false, 0, false, false,
        500, NULL) == HostControlOsError);
    assert(run_control_fixture(false, false, false, 0, false, false, false, 0, true, 0, false, false,
        TestRunMs, NULL) ==
           HostControlCleanupRequired);
    for (uint32_t fail_call = 1; fail_call <= 4; ++fail_call) {
        assert(run_control_fixture(false, false, false, 0, true, false, false,
            fail_call, false, 0, false, false, 500, NULL) == HostControlOsError);
    }
    assert(run_control_fixture(false, false, false, 0, false, false, false, 0,
        false, 1500, false, false, 250, NULL) == HostControlTimedOut);
    assert(peer_saw_hello_ack == 0);
    arm_drain_failure_after_success = true;
    assert(run_control_fixture(false, false, false, 0, false, false, false, 0,
        false, 1500, false, false, 250, NULL) == HostControlTimedOut);
    arm_drain_failure_after_success = true;
    assert(run_control_fixture(false, false, false, 0, false, false, false, 0,
        false, 0, false, false, 100, NULL) == HostControlPeerFailed);
    uint64_t tampered_heartbeat = 0;
    assert(run_control_fixture(false, false, false, 0, false, false, false, 0,
        false, 0, false, true, 1500, &tampered_heartbeat) == HostControlMappingFailed);
    assert(tampered_heartbeat == 99);
    assert(peer_saw_hello_ack == 1 && peer_saw_goodbye == 1);
    puts("session_control_host integration tests passed");
    return 0;
}
#endif
