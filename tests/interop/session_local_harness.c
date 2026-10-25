/**
 * @file session_local_harness.c
 * @brief Harness that runs the real host control runtime with the local command handler.
 *
 * The runtime (interop_host_control_run, linked from the production sources) is driven
 * with a fake daemon socket and a fake guest. AF_VSOCK connects are answered by a socketpair
 * through the linker wrappers below; AF_UNIX connects (the handler's stale probe) are forwarded.
 * The fake guest answers every LaunchRequest with a LaunchResult selected by --reply:
 *
 *   ok           status Ok, process id 4242
 *   status:N     status N (0..9), process id 4242
 *   error:TEXT   status GuestError (9), message TEXT
 *
 * Each LaunchRequest the guest receives is appended to --record as one JSON line,
 * {"frame": "<hex>"}, and the file <record>.ready is created once the handler is bound.
 * SIGTERM or SIGINT ends the run with a graceful local stop, so the socket is removed.
 *
 *   interop_session_local_harness --runtime-dir DIR --device NAME --reply REPLY --record PATH
 *
 * Test-only. The runtime composition used here (an adapter from session_handler_t to
 * interop_host_feature_ops_t) is a stand-in for the session application runtime.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../../src/interop/interop_mapping_init.h"
#include "../../src/interop/session_control_host.h"
#include "../../src/interop/session_handler.h"
#include "../../src/interop/session_local_handler.h"
#include "../../src/daemon/daemon_device.h"
#include "../../src/daemon/daemon_wire.h"
#include "waddle/daemon_protocol.h"
#include "waddle/interop_memory.h"
#include "waddle/interop_protocol.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/vm_sockets.h>

#define HarnessMappingBytes UINT64_C(0x80000000)
#define HarnessSessionEpoch UINT64_C(0x1020304050607080)
#define HarnessGuestCid 33u
#define HarnessGuestProcessId 4242u

extern int __real_socket(int domain, int type, int protocol);
extern int __real_connect(int fd, const struct sockaddr *address, socklen_t length);

typedef enum harness_reply_kind_t {
    HarnessReplyOk = 0,
    HarnessReplyStatus = 1,
    HarnessReplyError = 2
} harness_reply_kind_t;

typedef struct harness_guest_t {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int peer_fd; /* guest end of the vsock socketpair, or -1 */
    harness_reply_kind_t reply_kind;
    uint32_t reply_status;
    char reply_message[1025];
    char record_path[512];
    uint32_t launch_requests;
} harness_guest_t;

static harness_guest_t guest = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .condition = PTHREAD_COND_INITIALIZER,
    .peer_fd = -1,
};

static session_handler_t local_table;
static session_local_handler_t local_handler;

/* ---- Linker wrappers: the vsock control connection is a socketpair ---- */

int __wrap_socket(int domain, int type, int protocol)
{
    int descriptors[2];
    if (domain != AF_VSOCK) return __real_socket(domain, type, protocol);
    (void)protocol;
    assert((type & SOCK_STREAM) != 0);
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, descriptors) == 0);
    assert(pthread_mutex_lock(&guest.mutex) == 0);
    assert(guest.peer_fd == -1);
    guest.peer_fd = descriptors[1];
    assert(pthread_cond_broadcast(&guest.condition) == 0);
    assert(pthread_mutex_unlock(&guest.mutex) == 0);
    return descriptors[0];
}

int __wrap_connect(int fd, const struct sockaddr *address, socklen_t address_length)
{
    const struct sockaddr_vm *vsock = (const struct sockaddr_vm *)address;
    if (address->sa_family != AF_VSOCK) return __real_connect(fd, address, address_length);
    assert(address_length == sizeof(struct sockaddr_vm));
    assert(vsock->svm_cid == HarnessGuestCid && vsock->svm_port == InteropControlPort);
    return 0;
}

/* ---- Fake guest ---- */

static void write_all(int fd, const void *buffer, size_t length)
{
    const uint8_t *bytes = buffer;
    size_t offset = 0;
    while (offset < length) {
        ssize_t sent = send(fd, bytes + offset, length - offset, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd descriptor = {.fd = fd, .events = POLLOUT};
            assert(poll(&descriptor, 1, 2000) > 0);
            continue;
        }
        if (sent <= 0) return;
        offset += (size_t)sent;
    }
}

static bool read_exact(int fd, void *buffer, size_t length)
{
    uint8_t *bytes = buffer;
    size_t offset = 0;
    while (offset < length) {
        ssize_t received = recv(fd, bytes + offset, length - offset, 0);
        if (received < 0 && errno == EINTR) continue;
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd descriptor = {.fd = fd, .events = POLLIN};
            if (poll(&descriptor, 1, 10000) <= 0) return false;
            continue;
        }
        if (received <= 0) return false;
        offset += (size_t)received;
    }
    return true;
}

/** @brief Read one complete frame; returns false when the peer closed or stalled. */
static bool read_frame(int fd, uint8_t *frame, size_t capacity, interop_header_t *header)
{
    if (!read_exact(fd, frame, InteropHeaderBytes)) return false;
    if (interop_header_decode(frame, InteropHeaderBytes, header) != InteropErrorNone) return false;
    if (InteropHeaderBytes + (size_t)header->length > capacity) return false;
    return read_exact(fd, frame + InteropHeaderBytes, header->length);
}

static size_t make_hello(uint8_t *frame, size_t capacity)
{
    static const uint8_t AgentVersion[] = "1.2.3";
    interop_writer_t writer;
    size_t length = 0;
    interop_writer_init(&writer, frame, capacity);
    assert(interop_put_uint(&writer, InteropTagCapabilities, InteropCapLaunch) == InteropErrorNone);
    assert(interop_put_bytes(&writer, InteropTagAgentVersion, AgentVersion, sizeof(AgentVersion) - 1) == InteropErrorNone);
    assert(interop_put_uint(&writer, InteropTagLayoutVersion, InteropLayoutVersion) == InteropErrorNone);
    assert(interop_put_uint(&writer, InteropTagSessionEpoch, HarnessSessionEpoch) == InteropErrorNone);
    assert(interop_writer_finish(&writer, InteropMsgHello, 1, InteropFromGuest, &length) == InteropErrorNone);
    return length;
}

static size_t make_pong(uint8_t *frame, size_t capacity, uint32_t request_id)
{
    interop_writer_t writer;
    size_t length = 0;
    interop_writer_init(&writer, frame, capacity);
    assert(interop_writer_finish(&writer, InteropMsgPong, request_id, InteropFromGuest, &length) == InteropErrorNone);
    return length;
}

static size_t make_launch_result(uint8_t *frame, size_t capacity, uint32_t request_id)
{
    interop_writer_t writer;
    size_t length = 0;
    uint32_t status = InteropOk;
    const char *message = NULL;
    if (guest.reply_kind == HarnessReplyStatus) status = guest.reply_status;
    if (guest.reply_kind == HarnessReplyError) {
        status = InteropGuestError;
        message = guest.reply_message;
    }
    interop_writer_init(&writer, frame, capacity);
    assert(interop_put_uint(&writer, InteropTagProcessId, HarnessGuestProcessId) == InteropErrorNone);
    assert(interop_put_uint(&writer, InteropTagStatus, status) == InteropErrorNone);
    if (message != NULL) {
        assert(interop_put_bytes(&writer, InteropTagMessage, message, (uint16_t)strlen(message)) == InteropErrorNone);
    }
    assert(interop_writer_finish(&writer, InteropMsgLaunchResult, request_id, InteropFromGuest, &length) == InteropErrorNone);
    return length;
}

static void append_record(const uint8_t *frame, size_t length)
{
    static const char Digits[] = "0123456789abcdef";
    FILE *record = fopen(guest.record_path, "a");
    assert(record != NULL);
    fputs("{\"frame\": \"", record);
    for (size_t index = 0; index < length; ++index) {
        fputc(Digits[frame[index] >> 4], record);
        fputc(Digits[frame[index] & 0x0F], record);
    }
    fputs("\"}\n", record);
    assert(fclose(record) == 0);
}

static void *guest_thread(void *unused)
{
    uint8_t frame[InteropMaxFrameBytes];
    interop_header_t header;
    size_t length;
    int fd;
    (void)unused;
    assert(pthread_mutex_lock(&guest.mutex) == 0);
    while (guest.peer_fd < 0) assert(pthread_cond_wait(&guest.condition, &guest.mutex) == 0);
    fd = guest.peer_fd;
    assert(pthread_mutex_unlock(&guest.mutex) == 0);

    length = make_hello(frame, sizeof(frame));
    write_all(fd, frame, length);
    while (read_frame(fd, frame, sizeof(frame), &header)) {
        if (header.type == InteropMsgHelloAck) continue;
        if (header.type == InteropMsgPing) {
            length = make_pong(frame, sizeof(frame), header.request_id);
            write_all(fd, frame, length);
        } else if (header.type == InteropMsgLaunchRequest) {
            ++guest.launch_requests;
            append_record(frame, InteropHeaderBytes + header.length);
            length = make_launch_result(frame, sizeof(frame), header.request_id);
            write_all(fd, frame, length);
        } else if (header.type == InteropMsgGoodbye) {
            break;
        }
    }
    close(fd);
    assert(pthread_mutex_lock(&guest.mutex) == 0);
    guest.peer_fd = -1;
    assert(pthread_mutex_unlock(&guest.mutex) == 0);
    return NULL;
}

/* ---- Fake daemon ---- */

static void write_daemon_header(int fd, uint16_t message_type, uint32_t sequence, uint32_t payload_length)
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

static void read_daemon_header(int fd, waddle_daemon_header_t *header)
{
    uint8_t bytes[WaddleDaemonHeaderSize];
    assert(read_exact(fd, bytes, sizeof(bytes)));
    assert(daemon_wire_decode_header(bytes, sizeof(bytes), header) == 0);
}

static void *daemon_thread(void *opaque)
{
    int fd = *(int *)opaque;
    waddle_daemon_header_t request;
    waddle_daemon_status_resp_t status;
    waddle_daemon_guest_ready_resp_t ready = {.status = 0, .cid = HarnessGuestCid};
    uint8_t timeout[sizeof(uint32_t)];
    uint8_t byte;
    memset(&status, 0, sizeof(status));
    status.subsystem_state = SubsystemStateRunning;
    status.daemon_pid = (uint32_t)getpid();
    status.qemu_pid = (uint32_t)getpid();
    status.vsock_cid = HarnessGuestCid;
    status.vsock_port = 5242;
    status.memory_mb = 4096;
    status.vcpus = 4;
    read_daemon_header(fd, &request);
    assert(request.msg_type == DaemonMsgStatusReq && request.payload_len == 0);
    write_daemon_header(fd, DaemonMsgStatusResp, request.sequence, sizeof(status));
    write_all(fd, &status, sizeof(status));
    read_daemon_header(fd, &request);
    assert(request.msg_type == DaemonMsgGuestReadyReq && request.payload_len == sizeof(uint32_t));
    assert(read_exact(fd, timeout, sizeof(timeout)));
    write_daemon_header(fd, DaemonMsgGuestReadyResp, request.sequence, sizeof(ready));
    write_all(fd, &ready, sizeof(ready));
    for (;;) {
        ssize_t result = recv(fd, &byte, 1, 0);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd descriptor = {.fd = fd, .events = POLLIN};
            if (poll(&descriptor, 1, 60000) <= 0) break;
            continue;
        }
        break;
    }
    close(fd);
    return NULL;
}

/* ---- Runtime composition: session_handler_t as interop_host_feature_ops_t ---- */

/** @brief Map a handler result to the runtime: the graceful stop becomes a SIGTERM to the pump thread. */
static int map_handler_result(int result)
{
    if (result == SessionHandlerRequestStop) {
        /* SIGTERM is blocked on the pump thread and read through the stop signalfd, so this is
         * exactly what the runtime sees for an external stop: HostControlCancelled and drain. */
        assert(pthread_kill(pthread_self(), SIGTERM) == 0);
        return 0;
    }
    return result;
}

static int adapter_on_ready(void *context, uint64_t negotiated, interop_host_feature_sender_t *sender)
{
    session_handler_t *table = context;
    session_local_handler_t *local = table->context;
    int result;
    /* The runtime sets the singleton lock proof after admission, before the handler binds. */
    local->lock_held_proof = 1;
    result = map_handler_result(table->on_ready(table->context, negotiated, sender));
    if (result == 0 && guest.record_path[0] != '\0') {
        char marker[sizeof(guest.record_path) + 8];
        FILE *ready = NULL;
        assert(snprintf(marker, sizeof(marker), "%s.ready", guest.record_path) > 0);
        ready = fopen(marker, "w");
        assert(ready != NULL);
        assert(fclose(ready) == 0);
    }
    return result;
}

static int adapter_on_frame(void *context, uint32_t message_type, uint32_t request_id,
    const uint8_t *frame, size_t length, interop_host_feature_sender_t *sender)
{
    session_handler_t *table = context;
    if (!table->owns_type(table->context, message_type)) return 1;
    return map_handler_result(table->on_frame(table->context, message_type, request_id, frame, length, sender));
}

static int adapter_poll_fds(void *context, struct pollfd *fds, size_t capacity, size_t *count)
{
    session_handler_t *table = context;
    return table->poll_fds(table->context, fds, capacity, count);
}

static int adapter_on_fds(void *context, const struct pollfd *fds, size_t count, uint64_t now_ms,
    interop_host_feature_sender_t *sender)
{
    session_handler_t *table = context;
    return map_handler_result(table->on_fds(table->context, fds, count, now_ms, sender));
}

static int adapter_on_tick(void *context, uint64_t now_ms, interop_host_feature_sender_t *sender)
{
    session_handler_t *table = context;
    return map_handler_result(table->on_tick(table->context, now_ms, sender));
}

static uint64_t adapter_next_deadline(void *context, uint64_t now_ms)
{
    session_handler_t *table = context;
    return table->next_deadline_ms(table->context, now_ms);
}

static void adapter_on_closing(void *context, uint32_t reason, interop_host_feature_sender_t *sender)
{
    session_handler_t *table = context;
    table->on_closing(table->context, reason, sender);
}

/* ---- Main ---- */

static int create_stop_signal_fd(sigset_t *prior_mask)
{
    sigset_t blocked;
    int fd;
    sigemptyset(&blocked);
    assert(sigaddset(&blocked, SIGINT) == 0);
    assert(sigaddset(&blocked, SIGTERM) == 0);
    assert(pthread_sigmask(SIG_BLOCK, &blocked, prior_mask) == 0);
    fd = signalfd(-1, &blocked, SFD_NONBLOCK | SFD_CLOEXEC);
    assert(fd >= 0);
    return fd;
}

static int parse_reply(const char *value)
{
    if (strcmp(value, "ok") == 0) {
        guest.reply_kind = HarnessReplyOk;
        return 0;
    }
    if (strncmp(value, "status:", 7) == 0) {
        char *end = NULL;
        unsigned long status = strtoul(value + 7, &end, 10);
        if (end == value + 7 || *end != '\0' || status > InteropGuestError) return 1;
        guest.reply_kind = HarnessReplyStatus;
        guest.reply_status = (uint32_t)status;
        return 0;
    }
    if (strncmp(value, "error:", 6) == 0) {
        size_t length = strlen(value + 6);
        if (length >= sizeof(guest.reply_message)) return 1;
        guest.reply_kind = HarnessReplyError;
        memcpy(guest.reply_message, value + 6, length + 1u);
        return 0;
    }
    return 1;
}

static int usage(void)
{
    fprintf(stderr, "usage: interop_session_local_harness --runtime-dir DIR --device NAME "
        "--reply ok|status:N|error:TEXT --record PATH\n");
    return 2;
}

int main(int argc, char **argv)
{
    const char *runtime_dir = NULL;
    const char *device_name = NULL;
    const char *reply = NULL;
    const char *record = NULL;
    char mapping_root[64];
    char mapping_path[128];
    int daemon_pair[2];
    int mapping_fd;
    pthread_t guest_handle;
    pthread_t daemon_handle;
    sigset_t prior_mask;
    int stop_fd;
    session_local_config_t config;
    device_info_t device;
    interop_host_session_config_t admission;
    interop_host_feature_ops_t ops;
    interop_host_control_config_t control;
    interop_host_control_owners_t owners;
    interop_host_control_result_t result;
    interop_mapping_init_status_t mapping_status;
    void *mapping;
    int status_code = 1;
    int mapping_ready = 0;
    int threads_started = 0;
    static const uint8_t AgentVersion[] = "1.2.3";

    for (int index = 1; index + 1 < argc; index += 2) {
        if (strcmp(argv[index], "--runtime-dir") == 0) runtime_dir = argv[index + 1];
        else if (strcmp(argv[index], "--device") == 0) device_name = argv[index + 1];
        else if (strcmp(argv[index], "--reply") == 0) reply = argv[index + 1];
        else if (strcmp(argv[index], "--record") == 0) record = argv[index + 1];
        else return usage();
    }
    if (runtime_dir == NULL || device_name == NULL || reply == NULL || record == NULL || argc % 2 == 0) return usage();
    /* The daemon and session lease paths come from the environment; the socket from --runtime-dir.
     * They must name the same directory or admission would lock a different session.lock. */
    {
        const char *environment_runtime = getenv("XDG_RUNTIME_DIR");
        if (environment_runtime == NULL || strcmp(environment_runtime, runtime_dir) != 0) {
            fprintf(stderr, "session-local-harness: XDG_RUNTIME_DIR must equal --runtime-dir\n");
            return 2;
        }
    }
    if (strlen(record) >= sizeof(guest.record_path)) return usage();
    if (parse_reply(reply) != 0) return usage();
    memcpy(guest.record_path, record, strlen(record) + 1u);
    /* The record starts empty for each run; the ready marker is removed so a stale one is not seen. */
    (void)unlink(guest.record_path);
    {
        char marker[sizeof(guest.record_path) + 8];
        assert(snprintf(marker, sizeof(marker), "%s.ready", guest.record_path) > 0);
        (void)unlink(marker);
    }

    memset(&config, 0, sizeof(config));
    config.runtime_dir = runtime_dir;
    config.device_name = device_name;
    if (session_local_handler_init(&local_handler, &config) != SessionLocalSuccess) return 1;
    session_local_handler_table(&local_handler, &local_table);

    memcpy(mapping_root, "/tmp/interop-session-local-XXXXXX", sizeof("/tmp/interop-session-local-XXXXXX"));
    if (mkdtemp(mapping_root) == NULL) return 1;
    assert(snprintf(mapping_path, sizeof(mapping_path), "%s/mapping", mapping_root) > 0);
    mapping_fd = open(mapping_path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    assert(mapping_fd >= 0);
    assert(ftruncate(mapping_fd, (off_t)HarnessMappingBytes) == 0);
    mapping = mmap(NULL, (size_t)HarnessMappingBytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NORESERVE, mapping_fd, 0);
    assert(mapping != MAP_FAILED);
    mapping_status = interop_mapping_initialize((uintptr_t)mapping, HarnessMappingBytes, HarnessSessionEpoch, 1);
    assert(mapping_status == InteropMappingInitStatusOk);
    assert(munmap(mapping, (size_t)HarnessMappingBytes) == 0);
    mapping_ready = 1;

    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, daemon_pair) == 0);
    stop_fd = create_stop_signal_fd(&prior_mask);

    memset(&device, 0, sizeof(device));
    assert(strlen(device_name) < sizeof(device.name));
    memcpy(device.name, device_name, strlen(device_name) + 1u);
    assert(daemon_device_get_state_dir(device.name, device.state_dir, sizeof(device.state_dir)) == 0);
    assert(daemon_device_get_socket_path(device.name, device.socket_path, sizeof(device.socket_path)) == 0);
    memcpy(device.config_path, "/tmp/session-local-harness.ini", sizeof("/tmp/session-local-harness.ini"));
    memcpy(device.disk_image, "/tmp/session-local-harness.qcow2", sizeof("/tmp/session-local-harness.qcow2"));
    device.vsock_cid = HarnessGuestCid;
    device.vsock_port = 5242;
    device.memory_mb = 4096;
    device.vcpus = 4;
    device.config_valid = 1;

    memset(&ops, 0, sizeof(ops));
    ops.context = &local_table;
    ops.local_capabilities = local_table.capabilities;
    ops.on_ready = adapter_on_ready;
    ops.on_frame = adapter_on_frame;
    ops.poll_fds = adapter_poll_fds;
    ops.on_fds = adapter_on_fds;
    ops.on_tick = adapter_on_tick;
    ops.next_deadline_ms = adapter_next_deadline;
    ops.on_closing = adapter_on_closing;

    memset(&admission, 0, sizeof(admission));
    admission.device = &device;
    admission.daemon_fd = daemon_pair[0];
    admission.agent_version = AgentVersion;
    admission.agent_version_bytes = sizeof(AgentVersion) - 1;
    admission.connection_token = UINT64_C(0x1122334455667788);
    admission.local_capabilities = ops.local_capabilities;
    admission.cancel_fd = stop_fd;
    admission.guest_ready_timeout_ms = 5000;
    admission.drain_timeout_ms = 5000;
    {
        struct timespec now;
        assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        admission.deadline_ms = (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u + 30000u;
    }

    memset(&control, 0, sizeof(control));
    control.admission = &admission;
    control.mapping_fd = mapping_fd;
    control.backing_kind = InteropMappingLinuxBackingRegularPrivateFile;
    control.stop_fd = stop_fd;
    control.run_for_ms = 0;
    control.feature_ops = &ops;

    assert(pthread_create(&daemon_handle, NULL, daemon_thread, &daemon_pair[1]) == 0);
    assert(pthread_create(&guest_handle, NULL, guest_thread, NULL) == 0);
    threads_started = 1;

    memset(&owners, 0, sizeof(owners));
    result = interop_host_control_run(&control, &owners);
    if (result == HostControlCleanupRequired) {
        result = interop_host_control_retry_cleanup(&owners);
    }
    if (result == HostControlCancelled) status_code = 0;
    if (status_code != 0) fprintf(stderr, "session-local-harness: run ended with %d\n", (int)result);

    /* Closing the runtime end lets the fake daemon observe end of stream before it is joined. */
    close(daemon_pair[0]);
    if (threads_started) {
        assert(pthread_join(guest_handle, NULL) == 0);
        assert(pthread_join(daemon_handle, NULL) == 0);
    }
    close(mapping_fd);
    close(stop_fd);
    assert(pthread_sigmask(SIG_SETMASK, &prior_mask, NULL) == 0);
    (void)mapping_ready;
    {
        char command[256];
        assert(snprintf(command, sizeof(command), "rm -rf '%s'", mapping_root) > 0);
        assert(system(command) == 0);
    }
    return status_code;
}
