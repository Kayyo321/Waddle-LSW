/**
 * @file session_local_harness.c
 * @brief End-to-end harness for `waddle open`: the real `waddle_session_main` with a fake daemon and guest.
 *
 * The harness does not compose its own runtime. It writes the device profile (an [av] section
 * pointing at a real, initialised 2 GiB mapping), serves the daemon protocol on the real device
 * socket <XDG_RUNTIME_DIR>/waddle/<device>/daemon.sock, and then calls `waddle_session_main` (the
 * supervised command, compiled in with WaddleSessionNoMain). That path performs the real admission,
 * the real supervisor, the real builtin registry and the real launch handler. Only two things are
 * fake: AF_VSOCK connects are answered by a socketpair whose far end is a fake guest (one per
 * connection, through the linker wrappers below), and the daemon is a thread per connection.
 *
 * The fake guest answers every LaunchRequest with a LaunchResult selected by --reply:
 *   ok           status Ok, process id 4242
 *   status:N     status N (0..9), process id 4242
 *   error:TEXT   status GuestError (9), message TEXT
 *
 * Output files, all derived from --record PATH:
 *   PATH            one JSON line per LaunchRequest the guest received: {"frame": "<hex>"}.
 *   PATH.ready      present while the launch socket is bound (created 100 ms after bind, removed when
 *                   the socket disappears), so a client never connects before listen().
 *   PATH.connections one line per VSOCK connection the runtime opened (attempt count).
 *
 * --drop-first-guest closes the first guest connection right after its HelloAck, which is a
 * transport loss before any launch. The supervisor must reconnect and serve the second attempt.
 * --drop-first-after-launch closes the first guest after it answered one launch, so the second
 * attempt is a second consecutive guest session that serves its own launches.
 *
 * Signals: SIGINT and SIGTERM are blocked before any thread is created, so the runtime's signalfd
 * is the only consumer. SIGTERM therefore ends the run through the real graceful stop, the handler
 * removes session.sock, and the process exits 0. A LocalStop from a client does the same thing.
 *
 *   interop_session_local_harness --runtime-dir DIR --device NAME --reply REPLY --record PATH
 *       [--drop-first-guest | --drop-first-after-launch]
 *
 * Test-only. Built with -DWaddleSessionNoMain and linked with --wrap=socket,--wrap=connect.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../../src/daemon/daemon_device.h"
#include "../../src/daemon/daemon_wire.h"
#include "../../src/interop/interop_mapping_init.h"
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <linux/vm_sockets.h>

#define HarnessMappingBytes UINT64_C(0x80000000)
#define HarnessSessionEpoch UINT64_C(0x1020304050607080)
#define HarnessGuestCid 33u
#define HarnessGuestVsockPort 5242u
#define HarnessGuestProcessId 4242u

extern int __real_socket(int domain, int type, int protocol);
extern int __real_connect(int fd, const struct sockaddr *address, socklen_t length);
/* Defined in session_main.c (compiled with WaddleSessionNoMain). */
int waddle_session_main(int argc, char **argv);

typedef enum harness_reply_kind_t {
    HarnessReplyOk = 0,
    HarnessReplyStatus = 1,
    HarnessReplyError = 2
} harness_reply_kind_t;

typedef struct harness_guest_t {
    harness_reply_kind_t reply_kind;
    uint32_t reply_status;
    char reply_message[1025];
    char record_path[512];
    char connections_path[520];
    char ready_path[528];
    char socket_path[256]; /* runtime/waddle/<device>/session.sock */
    char daemon_socket[256];
    int drop_first_guest;
    int drop_first_after_launch; /* the first guest drops right after answering one launch */
    uint32_t connections; /* guarded by connection_lock */
    pthread_mutex_t connection_lock;
} harness_guest_t;

static harness_guest_t guest = {
    .connection_lock = PTHREAD_MUTEX_INITIALIZER,
};

/* ---- Linker wrappers: each vsock control connection is a socketpair with a fake guest ---- */

typedef struct harness_guest_arg_t {
    int fd;
    uint32_t index;
} harness_guest_arg_t;

static void *guest_thread(void *opaque);

int __wrap_socket(int domain, int type, int protocol)
{
    int descriptors[2];
    harness_guest_arg_t *argument;
    pthread_t thread;
    if (domain != AF_VSOCK) return __real_socket(domain, type, protocol);
    (void)protocol;
    assert((type & SOCK_STREAM) != 0);
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, descriptors) == 0);
    argument = malloc(sizeof(*argument));
    assert(argument != NULL);
    assert(pthread_mutex_lock(&guest.connection_lock) == 0);
    argument->index = guest.connections++;
    assert(pthread_mutex_unlock(&guest.connection_lock) == 0);
    argument->fd = descriptors[1];
    {
        FILE *connections = fopen(guest.connections_path, "a");
        assert(connections != NULL);
        fprintf(connections, "connection %u\n", (unsigned)argument->index);
        assert(fclose(connections) == 0);
    }
    assert(pthread_create(&thread, NULL, guest_thread, argument) == 0);
    assert(pthread_detach(thread) == 0);
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


static void *guest_thread(void *opaque)
{
    harness_guest_arg_t *argument = opaque;
    int fd = argument->fd;
    uint32_t index = argument->index;
    uint8_t frame[InteropMaxFrameBytes];
    interop_header_t header;
    size_t length;
    free(argument);
    length = make_hello(frame, sizeof(frame));
    write_all(fd, frame, length);
    while (read_frame(fd, frame, sizeof(frame), &header)) {
        if (header.type == InteropMsgHelloAck) {
            if (guest.drop_first_guest && index == 0) break;
            continue;
        }
        if (header.type == InteropMsgPing) {
            length = make_pong(frame, sizeof(frame), header.request_id);
            write_all(fd, frame, length);
        } else if (header.type == InteropMsgLaunchRequest) {
            append_record(frame, InteropHeaderBytes + header.length);
            length = make_launch_result(frame, sizeof(frame), header.request_id);
            write_all(fd, frame, length);
            if (guest.drop_first_after_launch && index == 0) break;
        } else if (header.type == InteropMsgGoodbye) {
            break;
        }
    }
    close(fd);
    return NULL;
}

/* ---- Fake daemon wire ---- */

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



static void *daemon_thread(void *opaque)
{
    int fd = *(int *)opaque;
    free(opaque);
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

/* ---- Fake daemon listener: one thread per accepted connection ---- */

static void *daemon_accept_thread(void *opaque)
{
    int listener = *(int *)opaque;
    for (;;) {
        int client = accept(listener, NULL, NULL);
        pthread_t thread;
        int *argument;
        if (client < 0) {
            if (errno == EINTR) continue;
            break;
        }
        argument = malloc(sizeof(*argument));
        assert(argument != NULL);
        *argument = client;
        assert(pthread_create(&thread, NULL, daemon_thread, argument) == 0);
        assert(pthread_detach(thread) == 0);
    }
    return NULL;
}

/* Creates <record>.ready while the launch socket is bound, after a short delay past bind+listen. */
static void *ready_marker_thread(void *unused)
{
    bool marked = false;
    (void)unused;
    for (;;) {
        bool present = access(guest.socket_path, F_OK) == 0;
        if (present && !marked) {
            struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000L};
            nanosleep(&pause, NULL);
            FILE *marker = fopen(guest.ready_path, "w");
            assert(marker != NULL);
            assert(fclose(marker) == 0);
            marked = true;
        } else if (!present && marked) {
            (void)unlink(guest.ready_path);
            marked = false;
        }
        {
            struct timespec pause = {.tv_sec = 0, .tv_nsec = 20000000L};
            nanosleep(&pause, NULL);
        }
    }
    return NULL;
}

static int make_directory_if_missing(const char *path, mode_t mode)
{
    if (mkdir(path, mode) == 0 || errno == EEXIST) return 0;
    return -1;
}

/* Writes the [av] and [subsystem] values the runtime needs; later keys override the lab profile. */
static int append_profile(const char *config_root, const char *device, const char *mapping_path)
{
    char directory[512];
    char path[640];
    FILE *profile;
    if (snprintf(directory, sizeof(directory), "%s/waddle", config_root) < 0) return -1;
    if (make_directory_if_missing(directory, 0700) != 0) return -1;
    if (snprintf(directory, sizeof(directory), "%s/waddle/devices", config_root) < 0) return -1;
    if (make_directory_if_missing(directory, 0700) != 0) return -1;
    if (snprintf(path, sizeof(path), "%s/%s.ini", directory, device) < 0) return -1;
    profile = fopen(path, "a");
    if (profile == NULL) return -1;
    fprintf(profile, "\n[subsystem]\nvsock_cid = %u\nvsock_port = %u\n\n[av]\nenabled = 1\nshm_path = %s\n",
        (unsigned)HarnessGuestCid, (unsigned)HarnessGuestVsockPort, mapping_path);
    return fclose(profile) == 0 ? 0 : -1;
}

static int usage(void)
{
    fprintf(stderr, "usage: interop_session_local_harness --runtime-dir DIR --device NAME "
        "--reply ok|status:N|error:TEXT --record PATH [--drop-first-guest]\n");
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
    char config_root[512];
    char runtime_device[512];
    char command[1024];
    sigset_t blocked;
    int mapping_fd;
    int listener;
    pthread_t thread;
    void *mapping;
    int status_code;
    static const uint8_t AgentVersion[] = "1.2.3";
    (void)AgentVersion;

    for (int index = 1; index < argc;) {
        if (strcmp(argv[index], "--drop-first-guest") == 0) {
            guest.drop_first_guest = 1;
            index += 1;
            continue;
        }
        if (strcmp(argv[index], "--drop-first-after-launch") == 0) {
            guest.drop_first_after_launch = 1;
            index += 1;
            continue;
        }
        if (index + 1 >= argc) return usage();
        if (strcmp(argv[index], "--runtime-dir") == 0) runtime_dir = argv[index + 1];
        else if (strcmp(argv[index], "--device") == 0) device_name = argv[index + 1];
        else if (strcmp(argv[index], "--reply") == 0) reply = argv[index + 1];
        else if (strcmp(argv[index], "--record") == 0) record = argv[index + 1];
        else return usage();
        index += 2;
    }
    if (runtime_dir == NULL || device_name == NULL || reply == NULL || record == NULL) return usage();
    if (daemon_device_validate_name(device_name) != 0) return usage();
    /* The daemon, the session lease and the launch socket must share one runtime directory. */
    {
        const char *environment_runtime = getenv("XDG_RUNTIME_DIR");
        const char *config_home = getenv("XDG_CONFIG_HOME");
        if (environment_runtime == NULL || strcmp(environment_runtime, runtime_dir) != 0 ||
                config_home == NULL || config_home[0] != '/') {
            fprintf(stderr, "session-local-harness: XDG_RUNTIME_DIR must equal --runtime-dir and XDG_CONFIG_HOME must be absolute\n");
            return 2;
        }
        assert(snprintf(config_root, sizeof(config_root), "%s", config_home) > 0);
    }
    if (strlen(record) + 16 >= sizeof(guest.record_path)) return usage();
    if (parse_reply(reply) != 0) return usage();
    assert(snprintf(guest.record_path, sizeof(guest.record_path), "%s", record) > 0);
    assert(snprintf(guest.connections_path, sizeof(guest.connections_path), "%s.connections", record) > 0);
    assert(snprintf(guest.ready_path, sizeof(guest.ready_path), "%s.ready", record) > 0);
    (void)unlink(guest.record_path);
    (void)unlink(guest.connections_path);
    (void)unlink(guest.ready_path);

    /* Block SIGINT and SIGTERM before any thread exists, so every thread inherits the block. */
    assert(sigemptyset(&blocked) == 0);
    assert(sigaddset(&blocked, SIGINT) == 0 && sigaddset(&blocked, SIGTERM) == 0);
    assert(pthread_sigmask(SIG_BLOCK, &blocked, NULL) == 0);

    memcpy(mapping_root, "/tmp/interop-session-local-XXXXXX", sizeof("/tmp/interop-session-local-XXXXXX"));
    if (mkdtemp(mapping_root) == NULL) return 1;
    assert(snprintf(mapping_path, sizeof(mapping_path), "%s/mapping", mapping_root) > 0);
    mapping_fd = open(mapping_path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    assert(mapping_fd >= 0);
    assert(ftruncate(mapping_fd, (off_t)HarnessMappingBytes) == 0);
    mapping = mmap(NULL, (size_t)HarnessMappingBytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NORESERVE, mapping_fd, 0);
    assert(mapping != MAP_FAILED);
    assert(interop_mapping_initialize((uintptr_t)mapping, HarnessMappingBytes, HarnessSessionEpoch, 1) ==
        InteropMappingInitStatusOk);
    assert(munmap(mapping, (size_t)HarnessMappingBytes) == 0);
    assert(close(mapping_fd) == 0);

    if (append_profile(config_root, device_name, mapping_path) != 0) return 1;
    assert(snprintf(runtime_device, sizeof(runtime_device), "%s/waddle", runtime_dir) > 0);
    if (make_directory_if_missing(runtime_device, 0700) != 0) return 1;
    assert(snprintf(runtime_device, sizeof(runtime_device), "%s/waddle/%s", runtime_dir, device_name) > 0);
    if (make_directory_if_missing(runtime_device, 0700) != 0) return 1;
    assert(chmod(runtime_device, 0700) == 0);
    assert(snprintf(guest.socket_path, sizeof(guest.socket_path), "%s/session.sock", runtime_device) > 0);
    assert(daemon_device_get_socket_path(device_name, guest.daemon_socket, sizeof(guest.daemon_socket)) == 0);
    (void)unlink(guest.daemon_socket);

    listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(listener >= 0);
    {
        struct sockaddr_un address;
        memset(&address, 0, sizeof(address));
        address.sun_family = AF_UNIX;
        assert(strlen(guest.daemon_socket) < sizeof(address.sun_path));
        strcpy(address.sun_path, guest.daemon_socket);
        assert(bind(listener, (const struct sockaddr *)&address, sizeof(address)) == 0);
        assert(listen(listener, 8) == 0);
    }
    assert(pthread_create(&thread, NULL, daemon_accept_thread, &listener) == 0);
    assert(pthread_detach(thread) == 0);
    assert(pthread_create(&thread, NULL, ready_marker_thread, NULL) == 0);
    assert(pthread_detach(thread) == 0);

    {
        char *arguments[] = {(char *)"waddle-session", (char *)"--supervised", (char *)"--device",
            (char *)device_name, (char *)"--max-retries", (char *)"3", (char *)"--run-for-ms", (char *)"120000"};
        status_code = waddle_session_main((int)(sizeof(arguments) / sizeof(arguments[0])), arguments);
    }
    if (status_code != 0) fprintf(stderr, "session-local-harness: run ended with %d\n", status_code);

    (void)unlink(guest.daemon_socket);
    (void)unlink(guest.ready_path);
    close(listener);
    assert(snprintf(command, sizeof(command), "rm -rf '%s'", mapping_root) > 0);
    assert(system(command) == 0);
    return status_code;
}
