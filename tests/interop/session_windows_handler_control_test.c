/**
 * @file session_windows_handler_control_test.c
 * @brief Control-runtime integration test of the session windows handler (src/interop/session_windows_handler.c).
 *
 * Every case is one complete run of interop_host_control_run (src/interop/session_control_host.c). The guest is an
 * in-process peer on a socketpair that replaces the AF_VSOCK connection. It speaks encoded wire frames built with the
 * codec C API and decodes every frame the host sends. The Wayland layer is the recording double of
 * session_windows_handler_fake.h, injected as the display of the handler.
 *
 * Composition: a single-handler adapter fills interop_host_feature_ops_t from the handler's session_handler_t table
 * (session_windows_handler_descriptor). The adapter drops a frame whose type the handler does not own (owns_type), counts
 * the callbacks that matter here (on_ready, on_closing) and forwards every other callback unchanged.
 *
 * Threading: the runtime runs on the test's main thread. Compositor-side actions (queued xdg events, presses, display
 * faults) run on that thread from the frame hooks of a case, so the double and the handler are never touched from the
 * guest thread. The guest thread only writes and reads its socket and fills its own log. The wake descriptor reported by
 * the handler is a real pipe: a case writes one byte to wake the runtime, and the poll wrapper consumes the byte after
 * the kernel reports it readable, as a display file descriptor would be consumed by the Wayland library.
 *
 * Cases:
 *   1 admission and announce      Hello with Windows, ack with Windows, on_ready, WindowAnnounce, xdg requests of 4.3.
 *   2 configure reaches guest     decoration, toplevel and surface configures; one WindowConfigure with the scaled rect.
 *   3 move validation             unrecorded serial, released press (refused); a live press (one xdg move).
 *   4 FrameReady                  FrameRelease with the same window, generation, slot and frame sequence.
 *   5 retire, then late close     decoration, toplevel, xdg_surface, wl_surface destroyed in order; no WindowClose sent.
 *   6 display error mid-run       read failure of the display; run ends HostControlFeatureFailed and drains.
 *   7 guest goodbye               guest Goodbye; the run ends HostControlPeerFailed (see the report of the runtime).
 *   8 no Windows negotiated       extra: a WindowAnnounce without Windows is refused as UnsupportedType before the handler.
 *
 * Each case prints one line and the test exits 0 only when every case passed. Protocol errors of the double, leaked
 * proxies and reducers, and a missing or repeated on_closing fail the case.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "session_windows_handler_fake.h"

#include "daemon_device.h"
#include "daemon_wire.h"
#include "interop_mapping_init.h"
#include "interop_native_atomic.h"
#include "session_control_host.h"
#include "session_handler.h"
#include "session_windows_handler.h"
#include "waddle/daemon_protocol.h"
#include "waddle/interop_memory.h"
#include "waddle/interop_protocol.h"
#include "window_state.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <poll.h>
#include <pthread.h>
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
#include <time.h>
#include <unistd.h>
#include <linux/vm_sockets.h>

#define Check(condition) do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            return 1; \
        } \
    } while (0)

#define TestMappingBytes UINT64_C(0x80000000)
#define TestSessionEpoch UINT64_C(0x1020304050607080)
#define TestDeviceName "dev0"
#define TestAgentVersion "1.2.3"
#define TestRunMs 1200u
#define TestNow UINT64_C(1000)
#define TestFrameSlots 8u
#define TestReceivedFrames 16u
#define TestReceivedBytes 256u
#define TestTextBytes 128u
#define TestSignatureBytes 4096u
#define TestDestroySlots 16u

/* ---- guest peer: scripted frames, received log ----------------------------------------------------------- */

typedef struct received_frame_t {
    uint16_t type;
    uint32_t request_id;
    size_t length;
    uint8_t bytes[TestReceivedBytes];
} received_frame_t;

static pthread_mutex_t peer_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t peer_condition = PTHREAD_COND_INITIALIZER;
static int vhost_peer_fd = -1;
static uint32_t vhost_connect_count;
static uint64_t peer_hello_capabilities;
static uint64_t peer_expected_ack_capabilities;
static bool peer_saw_hello_ack;
static bool peer_saw_goodbye;
static bool peer_saw_error;
static uint32_t peer_error_code;
static uint32_t peer_error_refused_type;
static uint32_t guest_frame_count;
static uint8_t guest_frames[TestFrameSlots][InteropMaxFrameBytes];
static size_t guest_frame_lengths[TestFrameSlots];
static received_frame_t received[TestReceivedFrames];
static uint32_t received_count;
static bool received_overflow;

/* Real libc entry points behind the wrappers of this file. */
extern int __real_socket(int domain, int type, int protocol);
extern int __real_connect(int fd, const struct sockaddr *address, socklen_t address_length);
extern int __real_poll(struct pollfd *descriptors, nfds_t count, int timeout);
extern int __real_getsockopt(int fd, int level, int option, void *value, socklen_t *length);
extern int __real_getpeername(int fd, struct sockaddr *address, socklen_t *address_length);
extern ssize_t __real_send(int fd, const void *buffer, size_t length, int flags);
extern ssize_t __real_recv(int fd, void *buffer, size_t length, int flags);

/* Wake pipe: its read end is the handler's display descriptor (wl_display_get_fd returns it). */
static int wake_read_fd = -1;
static int wake_write_fd = -1;

int __wrap_socket(int domain, int type, int protocol)
{
    if (domain != AF_VSOCK) return __real_socket(domain, type, protocol);
    (void)protocol;
    assert((type & SOCK_STREAM) != 0);
    int descriptors[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, descriptors) == 0);
    assert(pthread_mutex_lock(&peer_mutex) == 0);
    assert(vhost_peer_fd == -1);
    vhost_peer_fd = descriptors[1];
    assert(pthread_cond_broadcast(&peer_condition) == 0);
    assert(pthread_mutex_unlock(&peer_mutex) == 0);
    return descriptors[0];
}

int __wrap_connect(int fd, const struct sockaddr *address, socklen_t address_length)
{
    if (address->sa_family != AF_VSOCK) return __real_connect(fd, address, address_length);
    assert(address_length == sizeof(struct sockaddr_vm));
    const struct sockaddr_vm *vsock = (const struct sockaddr_vm *)address;
    assert(vsock->svm_cid == 33 && vsock->svm_port == InteropControlPort);
    ++vhost_connect_count;
    return 0;
}

/* The runtime's poll: after the kernel reports the wake pipe readable, consume its bytes so the next poll blocks. */
int __wrap_poll(struct pollfd *descriptors, nfds_t count, int timeout)
{
    int result = __real_poll(descriptors, count, timeout);
    if (result > 0 && wake_read_fd >= 0) {
        for (nfds_t index = 0; index < count; ++index) {
            if (descriptors[index].fd != wake_read_fd || (descriptors[index].revents & POLLIN) == 0) continue;
            char bytes[64];
            while (read(wake_read_fd, bytes, sizeof bytes) > 0) { }
        }
    }
    return result;
}

/* Sandbox fallbacks identical to the control runtime fixture: a refused socket call is retried as a file call. */
int __wrap_getsockopt(int fd, int level, int option, void *value, socklen_t *length)
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
        struct ucred credentials = {.pid = getpid(), .uid = geteuid(), .gid = getegid()};
        memcpy(value, &credentials, sizeof(credentials));
        *length = sizeof(credentials);
        return 0;
    }
#endif
    return result;
}

int __wrap_getpeername(int fd, struct sockaddr *address, socklen_t *address_length)
{
    socklen_t requested = address_length == NULL ? 0 : *address_length;
    int result = __real_getpeername(fd, address, address_length);
    if (result == 0 || errno != EPERM) return result;
    if (address == NULL || address_length == NULL || requested < sizeof(struct sockaddr_un)) {
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
    if (result < 0 && errno == EPERM) result = read(fd, buffer, length);
    return result;
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

/** @brief Write a frame; false when the host has closed its end (the run is ending). */
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

static uint16_t read_u16_le(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/** @brief Read one complete frame. False on EOF, error or a frame larger than the capacity. */
static bool try_read_interop_frame(int fd, uint8_t *frame, size_t capacity, size_t *out_length)
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
    const uint8_t *agent_version = (const uint8_t *)TestAgentVersion;
    interop_writer_t writer;
    size_t frame_length = 0;
    interop_writer_init(&writer, frame, capacity);
    assert(interop_put_uint(&writer, InteropTagCapabilities, peer_hello_capabilities) == InteropErrorNone);
    assert(interop_put_bytes(&writer, InteropTagAgentVersion, agent_version, strlen(TestAgentVersion)) == InteropErrorNone);
    assert(interop_put_uint(&writer, InteropTagLayoutVersion, InteropLayoutVersion) == InteropErrorNone);
    assert(interop_put_uint(&writer, InteropTagSessionEpoch, TestSessionEpoch) == InteropErrorNone);
    assert(interop_writer_finish(&writer, InteropMsgHello, 1, InteropFromGuest, &frame_length) == InteropErrorNone);
    return frame_length;
}

static size_t make_pong(uint8_t *frame, size_t capacity, uint32_t request_id)
{
    interop_writer_t writer;
    size_t frame_length = 0;
    interop_writer_init(&writer, frame, capacity);
    assert(interop_writer_finish(&writer, InteropMsgPong, request_id, InteropFromGuest, &frame_length) == InteropErrorNone);
    return frame_length;
}

/** @brief Record a host frame the guest keeps for the case check. Overflow is a case failure. */
static void record_received(uint16_t type, uint32_t request_id, const uint8_t *frame, size_t length)
{
    if (received_count == TestReceivedFrames || length > TestReceivedBytes) {
        received_overflow = true;
        return;
    }
    received_frame_t *slot = &received[received_count++];
    slot->type = type;
    slot->request_id = request_id;
    slot->length = length;
    memcpy(slot->bytes, frame, length);
}

/** @brief Guest thread: HelloAck, then the case's scripted frames, then Ping/Pong until Goodbye or EOF. */
static void *guest_peer(void *unused)
{
    (void)unused;
    assert(pthread_mutex_lock(&peer_mutex) == 0);
    while (vhost_peer_fd < 0) assert(pthread_cond_wait(&peer_condition, &peer_mutex) == 0);
    int fd = vhost_peer_fd;
    assert(pthread_mutex_unlock(&peer_mutex) == 0);

    uint8_t frame[InteropMaxFrameBytes];
    size_t frame_length = make_hello(frame, sizeof(frame));
    if (write_all_peer_may_close(fd, frame, frame_length)) {
        for (;;) {
            if (!try_read_interop_frame(fd, frame, sizeof(frame), &frame_length)) break;
            uint16_t type = read_u16_le(frame + 6);
            const uint8_t *payload = frame + InteropHeaderBytes;
            size_t payload_length = frame_length - InteropHeaderBytes;
            if (type == InteropMsgHelloAck) {
                uint64_t capabilities = UINT64_MAX;
                assert(interop_field_uint(payload, payload_length, InteropTagCapabilities, 8, &capabilities) == 0);
                assert(capabilities == peer_expected_ack_capabilities);
                peer_saw_hello_ack = true;
                for (uint32_t index = 0; index < guest_frame_count; ++index) {
                    if (!write_all_peer_may_close(fd, guest_frames[index], guest_frame_lengths[index])) break;
                }
            } else if (type == InteropMsgPing) {
                frame_length = make_pong(frame, sizeof(frame), read_u32_le(frame + 12));
                if (!write_all_peer_may_close(fd, frame, frame_length)) break;
            } else if (type == InteropMsgGoodbye) {
                peer_saw_goodbye = true;
                break;
            } else if (type == InteropMsgError) {
                uint64_t code = 0;
                uint64_t refused = 0;
                peer_saw_error = true;
                if (interop_field_uint(payload, payload_length, InteropTagErrorCode, 4, &code) == 0) peer_error_code = (uint32_t)code;
                if (interop_field_uint(payload, payload_length, InteropTagRefusedType, 2, &refused) == 0) peer_error_refused_type = (uint32_t)refused;
            } else {
                record_received(type, read_u32_le(frame + 12), frame, frame_length);
            }
        }
    }
    close(fd);
    assert(pthread_mutex_lock(&peer_mutex) == 0);
    vhost_peer_fd = -1;
    assert(pthread_mutex_unlock(&peer_mutex) == 0);
    return NULL;
}

/* ---- guest frame script (built on the main thread before a run) ------------------------------------------ */

static void script_reset(void)
{
    guest_frame_count = 0;
}

static void script_commit(size_t length)
{
    assert(length != 0 && guest_frame_count < TestFrameSlots);
    guest_frame_lengths[guest_frame_count++] = length;
}

/** @brief WindowAnnounce of a toplevel at guest rect 10,20,640,480 with state 0, DPI 96. */
static void script_announce(uint64_t window, uint64_t generation, const char *title, const char *app_key)
{
    interop_writer_t writer;
    size_t length = 0;
    interop_writer_init(&writer, guest_frames[guest_frame_count], InteropMaxFrameBytes);
    interop_put_uint(&writer, InteropTagWindow, window);
    interop_put_uint(&writer, InteropTagGeneration, generation);
    interop_put_uint(&writer, InteropTagRole, InteropRoleToplevel);
    interop_put_rect(&writer, InteropTagRect, 10, 20, 640, 480);
    if (title != NULL) interop_put_bytes(&writer, InteropTagTitle, title, (uint16_t)strlen(title));
    interop_put_bytes(&writer, InteropTagAppKey, app_key, (uint16_t)strlen(app_key));
    interop_put_uint(&writer, InteropTagState, 0);
    interop_put_uint(&writer, InteropTagDpi, 96);
    assert(interop_writer_finish(&writer, InteropMsgWindowAnnounce, 0, InteropFromGuest, &length) == InteropErrorNone);
    script_commit(length);
}

static void script_retire(uint64_t window, uint64_t generation)
{
    interop_writer_t writer;
    size_t length = 0;
    interop_writer_init(&writer, guest_frames[guest_frame_count], InteropMaxFrameBytes);
    interop_put_uint(&writer, InteropTagWindow, window);
    interop_put_uint(&writer, InteropTagGeneration, generation);
    assert(interop_writer_finish(&writer, InteropMsgWindowRetire, 0, InteropFromGuest, &length) == InteropErrorNone);
    script_commit(length);
}

static void script_frame_ready(uint64_t window, uint64_t generation, uint32_t slot, uint64_t sequence)
{
    interop_writer_t writer;
    size_t length = 0;
    interop_writer_init(&writer, guest_frames[guest_frame_count], InteropMaxFrameBytes);
    interop_put_uint(&writer, InteropTagWindow, window);
    interop_put_uint(&writer, InteropTagGeneration, generation);
    interop_put_uint(&writer, InteropTagSlot, slot);
    interop_put_uint(&writer, InteropTagFrameSequence, sequence);
    assert(interop_writer_finish(&writer, InteropMsgFrameReady, 0, InteropFromGuest, &length) == InteropErrorNone);
    script_commit(length);
}

static void script_move(uint64_t window, uint64_t generation, uint32_t serial)
{
    interop_writer_t writer;
    size_t length = 0;
    interop_writer_init(&writer, guest_frames[guest_frame_count], InteropMaxFrameBytes);
    interop_put_uint(&writer, InteropTagWindow, window);
    interop_put_uint(&writer, InteropTagGeneration, generation);
    interop_put_uint(&writer, InteropTagSerial, serial);
    assert(interop_writer_finish(&writer, InteropMsgWindowMoveRequest, 0, InteropFromGuest, &length) == InteropErrorNone);
    script_commit(length);
}

static void script_goodbye(void)
{
    interop_writer_t writer;
    size_t length = 0;
    interop_writer_init(&writer, guest_frames[guest_frame_count], InteropMaxFrameBytes);
    interop_put_uint(&writer, InteropTagStatus, InteropCancelled);
    assert(interop_writer_finish(&writer, InteropMsgGoodbye, 0, InteropFromGuest, &length) == InteropErrorNone);
    script_commit(length);
}

/* ---- decoding of host frames (host-to-guest direction, request identifier zero) -------------------------- */

typedef struct outbound_t {
    uint32_t type;
    uint32_t request_id;
    uint64_t window;
    uint64_t generation;
    int has_rect;
    int32_t rect[4];
    int has_state;
    uint32_t state;
    int has_slot;
    uint32_t slot;
    int has_sequence;
    uint64_t sequence;
} outbound_t;

/** @brief Validate and decode one host frame. Returns 0 on success. */
static int decode_host_frame(const uint8_t *frame, size_t length, outbound_t *out)
{
    memset(out, 0, sizeof *out);
    interop_header_t header;
    if (interop_header_decode(frame, length, &header) != InteropErrorNone) return 1;
    if (header.length != length - InteropHeaderBytes) return 1;
    const uint8_t *payload = frame + InteropHeaderBytes;
    if (interop_payload_validate(header.type, InteropFromHost, payload, header.length) != InteropErrorNone) return 1;
    out->type = header.type;
    out->request_id = header.request_id;
    if (interop_field_uint(payload, header.length, InteropTagWindow, 8u, &out->window) != 0) return 1;
    if (interop_field_uint(payload, header.length, InteropTagGeneration, 8u, &out->generation) != 0) return 1;
    const uint8_t *rect = NULL;
    uint16_t rect_length = 0;
    int found = interop_field_get(payload, header.length, InteropTagRect, 0u, &rect, &rect_length);
    if (found < 0 || (found == 0 && rect_length != 16u)) return 1;
    if (found == 0) {
        out->has_rect = 1;
        for (unsigned field = 0; field < 4; ++field) {
            out->rect[field] = (int32_t)read_u32_le(rect + 4 * field);
        }
    }
    uint64_t wide = 0;
    if (interop_field_uint(payload, header.length, InteropTagState, 4u, &wide) == 0) {
        out->has_state = 1;
        out->state = (uint32_t)wide;
    }
    if (interop_field_uint(payload, header.length, InteropTagSlot, 4u, &wide) == 0) {
        out->has_slot = 1;
        out->slot = (uint32_t)wide;
    }
    if (interop_field_uint(payload, header.length, InteropTagFrameSequence, 8u, &wide) == 0) {
        out->has_sequence = 1;
        out->sequence = wide;
    }
    return 0;
}

/** @brief Decode received frame `index` of the guest log. */
static int decode_received(uint32_t index, outbound_t *out)
{
    if (index >= received_count) return 1;
    return decode_host_frame(received[index].bytes, received[index].length, out);
}

/* ---- single-handler composition -------------------------------------------------------------------------- */

/** @brief Per-frame hook of a case; runs on the runtime thread around each admitted frame. */
typedef void (*frame_hook_t)(bool after, uint32_t ordinal);

static session_windows_handler_t handler;
static session_handler_t table;
static struct wl_display *display;
static uint64_t test_now;
static frame_hook_t frame_hook;
static uint32_t admitted_frames;
static uint32_t ready_calls;
static uint64_t ready_negotiated;
static uint32_t closing_calls;
static uint32_t closing_reason;
static uint32_t closing_destroy_count;
static const char *closing_destroys[TestDestroySlots];
static bool hook_failed;

static uint64_t test_clock(void)
{
    return test_now;
}

/** @brief on_ready of the composition: counted, then the handler's own callback. */
static int composition_on_ready(void *context, uint64_t negotiated, interop_host_feature_sender_t *sender)
{
    session_handler_t *composed = context;
    ++ready_calls;
    ready_negotiated = negotiated;
    return composed->on_ready(composed->context, negotiated, sender);
}

/** @brief on_frame of the composition: a type the handler does not own is dropped and fails the run. */
static int composition_on_frame(void *context, uint32_t message_type, uint32_t request_id, const uint8_t *frame, size_t length,
    interop_host_feature_sender_t *sender)
{
    session_handler_t *composed = context;
    if (!composed->owns_type(composed->context, message_type)) return 1;
    uint32_t ordinal = admitted_frames++;
    if (frame_hook != NULL) frame_hook(false, ordinal);
    int result = composed->on_frame(composed->context, message_type, request_id, frame, length, sender);
    if (frame_hook != NULL && result == 0) frame_hook(true, ordinal);
    return result;
}

static int composition_poll_fds(void *context, struct pollfd *fds, size_t capacity, size_t *count)
{
    session_handler_t *composed = context;
    return composed->poll_fds(composed->context, fds, capacity, count);
}

static int composition_on_fds(void *context, const struct pollfd *fds, size_t count, uint64_t now_ms,
    interop_host_feature_sender_t *sender)
{
    session_handler_t *composed = context;
    return composed->on_fds(composed->context, fds, count, now_ms, sender);
}

static int composition_on_tick(void *context, uint64_t now_ms, interop_host_feature_sender_t *sender)
{
    session_handler_t *composed = context;
    return composed->on_tick(composed->context, now_ms, sender);
}

static uint64_t composition_next_deadline_ms(void *context, uint64_t now_ms)
{
    session_handler_t *composed = context;
    return composed->next_deadline_ms(composed->context, now_ms);
}

/** @brief on_closing of the composition: counted once per run, the destroys of the handler's teardown are recorded. */
static void composition_on_closing(void *context, uint32_t reason, interop_host_feature_sender_t *sender)
{
    session_handler_t *composed = context;
    ++closing_calls;
    closing_reason = reason;
    unsigned begin = fake_log_count;
    composed->on_closing(composed->context, reason, sender);
    closing_destroy_count = 0;
    for (unsigned index = begin; index < fake_log_count && closing_destroy_count < TestDestroySlots; ++index) {
        closing_destroys[closing_destroy_count++] = fake_log[index];
    }
}

/** @brief Fill the runtime's feature table from the single handler's table. */
static void composition_fill(interop_host_feature_ops_t *ops, session_handler_t *composed)
{
    assert(composed->owns_type != NULL && composed->on_ready != NULL && composed->on_frame != NULL);
    assert(composed->poll_fds != NULL && composed->on_fds != NULL && composed->on_tick != NULL);
    assert(composed->next_deadline_ms != NULL && composed->on_closing != NULL);
    memset(ops, 0, sizeof *ops);
    ops->context = composed;
    ops->local_capabilities = composed->capabilities;
    ops->on_ready = composition_on_ready;
    ops->on_frame = composition_on_frame;
    ops->poll_fds = composition_poll_fds;
    ops->on_fds = composition_on_fds;
    ops->on_tick = composition_on_tick;
    ops->next_deadline_ms = composition_next_deadline_ms;
    ops->on_closing = composition_on_closing;
}

/* ---- compositor-side helpers (runtime thread only) ------------------------------------------------------- */

static void wake_runtime(void)
{
    assert(wake_write_fd >= 0);
    assert(write(wake_write_fd, "w", 1) == 1);
}

static host_wayland_record_t *record_of(uint64_t window, uint64_t generation)
{
    for (uint32_t index = 0; index < HostWindowMapMaxRealized; ++index) {
        host_wayland_record_t *rec = &handler.windows.records[index];
        if (rec->generation != 0 && rec->window == window && rec->generation == generation) return rec;
    }
    return NULL;
}

static unsigned index_of(const void *object)
{
    return object ? fake_index(object) : FakeNone;
}

static struct wl_seat *session_seat(void)
{
    return host_wayland_core_seat_at(&handler.core, 0);
}

/** @brief Record a left-button press of window (1, 1) at the test clock, on the session seat, lease zero. */
static host_wayland_status_t press_window_one(uint32_t protocol_serial, uint32_t wayland_serial)
{
    host_wayland_press_input_t input;
    memset(&input, 0, sizeof input);
    input.protocol_serial = protocol_serial;
    input.wayland_serial = wayland_serial;
    input.seat = session_seat();
    input.window = 1;
    input.generation = 1;
    input.lease_epoch = 0;
    input.kind = HostWaylandPressPointer;
    input.button = SessionWindowsButtonLeft;
    input.now_ms = TestNow;
    return session_windows_record_press(&handler, &input);
}

/** @brief Text argument of the first recorded request of iface/opcode at or after `from`. */
static bool request_text_copy(const char *iface, uint32_t opcode, unsigned from, char *out, size_t capacity)
{
    int index = fake_find(iface, opcode, from);
    if (index < 0) return false;
    snprintf(out, capacity, "%s", fake_requests[index].text);
    return true;
}

/* ---- hook state recorded for the case checks -------------------------------------------------------------- */

static unsigned hook_mark;
static char recorded_signature[TestSignatureBytes];
static char recorded_app_id[TestTextBytes];
static char recorded_title[TestTextBytes];
static unsigned recorded_seat;
static unsigned recorded_toplevel;
static unsigned recorded_surface;
static unsigned recorded_decoration;
static host_wayland_status_t recorded_press[2];
static const char *retire_destroys[TestDestroySlots];
static uint32_t retire_destroy_count;
static unsigned retire_dispatch_mark;

/** @brief Case 1: the window requests of the announce, from the frame's start to the commit and the initial observation. */
static void hook_admission(bool after, uint32_t ordinal)
{
    if (ordinal != 0) return;
    if (!after) {
        hook_mark = fake_request_count;
        return;
    }
    fake_signature(recorded_signature, sizeof recorded_signature, hook_mark);
    if (!request_text_copy("xdg_toplevel", XDG_TOPLEVEL_SET_APP_ID, hook_mark, recorded_app_id, sizeof recorded_app_id)) hook_failed = true;
    if (!request_text_copy("xdg_toplevel", XDG_TOPLEVEL_SET_TITLE, hook_mark, recorded_title, sizeof recorded_title)) hook_failed = true;
}

/** @brief Case 2: decoration, then the toplevel and surface configures of the compositor, as one dispatch. */
static void hook_configure(bool after, uint32_t ordinal)
{
    if (!after || ordinal != 0) return;
    host_wayland_record_t *rec = record_of(1, 1);
    if (rec == NULL || rec->toplevel == NULL || rec->xdg_surface == NULL || rec->decoration == NULL) {
        hook_failed = true;
        return;
    }
    unsigned toplevel = index_of(rec->toplevel);
    unsigned surface = index_of(rec->xdg_surface);
    unsigned decoration = index_of(rec->decoration);
    fake_queue_decoration_configure(decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    uint32_t states[] = {XDG_TOPLEVEL_STATE_ACTIVATED};
    fake_queue_toplevel_configure(toplevel, 400, 300, states, 1);
    fake_queue_surface_configure(surface, 1);
    wake_runtime();
}

/** @brief Case 3: presses are recorded between the guest's move requests (ordinals 0 to 3, the moves are 1 to 3). */
static void hook_moves(bool after, uint32_t ordinal)
{
    if (!after) return;
    if (ordinal == 0) {
        host_wayland_record_t *rec = record_of(1, 1);
        if (rec == NULL || rec->toplevel == NULL || session_seat() == NULL) {
            hook_failed = true;
            return;
        }
        recorded_toplevel = index_of(rec->toplevel);
        recorded_seat = fake_index(session_seat());
    } else if (ordinal == 1) {
        /* Recorded, then released before its move arrives: the guest's move with serial 11 must be refused. */
        recorded_press[0] = press_window_one(11, 101);
        session_windows_release_press(&handler, 11);
    } else if (ordinal == 2) {
        recorded_press[1] = press_window_one(12, 102);
    }
}

/** @brief Case 5: the destroys of the retire, then a WindowClose for the retired toplevel queued after it. */
static void hook_retire(bool after, uint32_t ordinal)
{
    if (!after) return;
    if (ordinal == 0) {
        host_wayland_record_t *rec = record_of(1, 1);
        if (rec == NULL || rec->toplevel == NULL) {
            hook_failed = true;
            return;
        }
        recorded_toplevel = index_of(rec->toplevel);
        hook_mark = fake_log_count;
    } else if (ordinal == 1) {
        retire_destroy_count = 0;
        for (unsigned index = hook_mark; index < fake_log_count && retire_destroy_count < TestDestroySlots; ++index) {
            retire_destroys[retire_destroy_count++] = fake_log[index];
        }
        fake_queue_toplevel_close(recorded_toplevel);
        retire_dispatch_mark = fake_dispatch_calls;
        wake_runtime();
    }
}

/** @brief Case 6: the display read fails on the next dispatch. */
static void hook_display_fault(bool after, uint32_t ordinal)
{
    if (!after || ordinal != 0) return;
    fake_read_mode = FakeFault;
    wake_runtime();
}

/* ---- one complete runtime run ---------------------------------------------------------------------------- */

typedef struct scenario_t {
    const char *name;
    uint64_t hello_capabilities;
    uint64_t ack_capabilities;
    uint32_t scale_numerator;
    uint32_t scale_denominator;
    frame_hook_t hook;
} scenario_t;

typedef struct daemon_fixture_t {
    int fd;
} daemon_fixture_t;

static void write_daemon_response_header(int fd, uint16_t message_type, uint32_t sequence, uint32_t payload_length)
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
    size_t offset = 0;
    while (offset < sizeof(bytes)) {
        ssize_t result = recv(fd, bytes + offset, sizeof(bytes) - offset, 0);
        assert(result > 0 || (result < 0 && errno == EINTR));
        if (result > 0) offset += (size_t)result;
    }
    assert(daemon_wire_decode_header(bytes, sizeof(bytes), header) == 0);
}

/** @brief Daemon side of the admission: status, GuestReady, then silence until the runtime closes the stream. */
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
    write_daemon_response_header(fixture->fd, DaemonMsgStatusResp, request.sequence, sizeof(status));
    write_all(fixture->fd, &status, sizeof(status));

    read_daemon_request(fixture->fd, &request);
    assert(request.msg_type == DaemonMsgGuestReadyReq && request.payload_len == sizeof(uint32_t));
    uint8_t timeout_bytes[sizeof(uint32_t)];
    size_t offset = 0;
    while (offset < sizeof(timeout_bytes)) {
        ssize_t result = recv(fixture->fd, timeout_bytes + offset, sizeof(timeout_bytes) - offset, 0);
        assert(result > 0 || (result < 0 && errno == EINTR));
        if (result > 0) offset += (size_t)result;
    }
    waddle_daemon_guest_ready_resp_t ready = {.status = 0, .cid = 33};
    write_daemon_response_header(fixture->fd, DaemonMsgGuestReadyResp, request.sequence, sizeof(ready));
    write_all(fixture->fd, &ready, sizeof(ready));

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

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void setup_paths(char root[64])
{
    strcpy(root, "/tmp/session-windows-control-XXXXXX");
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
    void *mapping = mmap(NULL, (size_t)TestMappingBytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NORESERVE, fd, 0);
    assert(mapping != MAP_FAILED);
    assert(interop_mapping_initialize((uintptr_t)mapping, TestMappingBytes, TestSessionEpoch, 1) == InteropMappingInitStatusOk);
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

static int remove_entry(const char *path, const struct stat *status, int type, struct FTW *walk_state)
{
    (void)status;
    (void)type;
    (void)walk_state;
    return remove(path);
}

/** @brief Reset every adapter and recording table; the double and the handler are reset by the caller. */
static void reset_run_state(void)
{
    admitted_frames = 0;
    ready_calls = 0;
    ready_negotiated = 0;
    closing_calls = 0;
    closing_reason = 0;
    closing_destroy_count = 0;
    hook_failed = false;
    hook_mark = 0;
    recorded_seat = FakeNone;
    recorded_toplevel = FakeNone;
    recorded_surface = FakeNone;
    recorded_decoration = FakeNone;
    recorded_signature[0] = '\0';
    recorded_app_id[0] = '\0';
    recorded_title[0] = '\0';
    retire_destroy_count = 0;
    received_count = 0;
    received_overflow = false;
    peer_saw_hello_ack = false;
    peer_saw_goodbye = false;
    peer_saw_error = false;
    peer_error_code = 0;
    peer_error_refused_type = 0;
    vhost_connect_count = 0;
    for (unsigned index = 0; index < 2; ++index) recorded_press[index] = HostWaylandOk;
}

/**
 * @brief Run one control session end to end: fresh double and handler, the guest script already in place, one call of
 * interop_host_control_run, and every thread, descriptor and environment entry released before return.
 */
static interop_host_control_result_t run_scenario(const scenario_t *scenario, uint32_t run_ms)
{
    reset_run_state();
    frame_hook = scenario->hook;
    peer_hello_capabilities = scenario->hello_capabilities;
    peer_expected_ack_capabilities = scenario->ack_capabilities;

    /* Wayland side: the double is the display; the handler does not connect. */
    fake_begin();
    memset(&handler, 0, sizeof handler);
    memset(&table, 0, sizeof table);
    test_now = TestNow;
    display = (struct wl_display *)fake_object(fake_new("wl_display", 1));
    session_windows_config_t config;
    memset(&config, 0, sizeof config);
    config.device = TestDeviceName;
    config.display = display;
    config.scale_numerator = scenario->scale_numerator;
    config.scale_denominator = scenario->scale_denominator;
    config.now_ms = test_clock;
    assert(session_windows_handler_init(&handler, &config) == 0);
    table = session_windows_handler_descriptor(&handler);
    fake_queue_announce(1, "wl_compositor", 6);
    fake_queue_announce(2, "xdg_wm_base", 5);
    fake_queue_announce(3, "wl_shm", 1);
    fake_queue_announce(4, "zxdg_decoration_manager_v1", 1);
    fake_queue_announce(5, "wl_seat", 7);
    int wake[2];
    assert(pipe2(wake, O_NONBLOCK | O_CLOEXEC) == 0);
    wake_read_fd = wake[0];
    wake_write_fd = wake[1];
    fake_fd_value = wake[0];

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

    daemon_fixture_t daemon = {.fd = daemon_pair[1]};
    pthread_t daemon_thread;
    pthread_t guest_thread;
    assert(pthread_create(&daemon_thread, NULL, daemon_server, &daemon) == 0);
    assert(pthread_create(&guest_thread, NULL, guest_peer, NULL) == 0);

    device_info_t device;
    memset(&device, 0, sizeof(device));
    strcpy(device.name, "session-windows-control");
    assert(daemon_device_get_state_dir(device.name, device.state_dir, sizeof(device.state_dir)) == 0);
    assert(daemon_device_get_socket_path(device.name, device.socket_path, sizeof(device.socket_path)) == 0);
    strcpy(device.config_path, "/tmp/session-windows-control.ini");
    strcpy(device.disk_image, "/tmp/session-windows-control.qcow2");
    device.vsock_cid = 33;
    device.vsock_port = 5242;
    device.memory_mb = 4096;
    device.vcpus = 4;
    device.config_valid = 1;
    static const uint8_t AgentVersion[] = TestAgentVersion;
    interop_host_feature_ops_t ops;
    composition_fill(&ops, &table);
    interop_host_session_config_t admission = {
        .device = &device,
        .daemon_fd = daemon_pair[0],
        .agent_version = AgentVersion,
        .agent_version_bytes = sizeof(AgentVersion) - 1,
        .connection_token = UINT64_C(0x1122334455667788),
        .local_capabilities = ops.local_capabilities,
        .deadline_ms = monotonic_ms() + 5000,
        .cancel_fd = stop_fd,
        .guest_ready_timeout_ms = 1000,
        .drain_timeout_ms = 1000,
        .reserved_zero = 0,
    };
    interop_host_control_config_t config_run = {
        .admission = &admission,
        .mapping_fd = mapping_fd,
        .backing_kind = InteropMappingLinuxBackingRegularPrivateFile,
        .stop_fd = stop_fd,
        .run_for_ms = run_ms,
        .reserved_zero = 0,
        .feature_ops = &ops,
    };
    interop_host_control_owners_t owners = {0};
    interop_host_control_result_t result = interop_host_control_run(&config_run, &owners);
    assert(result != HostControlCleanupRequired);
    assert(owners.mapping_owner == NULL && owners.session_owner == NULL);
    assert(vhost_connect_count == 1);

    assert(pthread_join(guest_thread, NULL) == 0);
    close(daemon_pair[0]);
    assert(pthread_join(daemon_thread, NULL) == 0);
    assert(pthread_sigmask(SIG_SETMASK, &prior_signal_mask, NULL) == 0);
    close(stop_fd);
    close(mapping_fd);
    close(wake_read_fd);
    close(wake_write_fd);
    wake_read_fd = -1;
    wake_write_fd = -1;
    frame_hook = NULL;
    assert(unsetenv("XDG_STATE_HOME") == 0);
    assert(unsetenv("XDG_RUNTIME_DIR") == 0);
    assert(nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0);
    return result;
}

/* ---- shared checks --------------------------------------------------------------------------------------- */

static const char *const WindowTeardown[] = {"zxdg_toplevel_decoration_v1", "xdg_toplevel", "xdg_surface", "wl_surface"};
static const char *const CoreTeardown[] = {"wl_seat", "zxdg_decoration_manager_v1", "wl_shm", "xdg_wm_base", "wl_compositor", "wl_registry"};

/** @brief The whole of the handler's teardown, windows first then the core, as on_closing runs it (one window). */
static int check_full_teardown(void)
{
    Check(closing_destroy_count == 10);
    for (unsigned index = 0; index < 4; ++index) Check(strcmp(closing_destroys[index], WindowTeardown[index]) == 0);
    for (unsigned index = 0; index < 6; ++index) Check(strcmp(closing_destroys[4 + index], CoreTeardown[index]) == 0);
    return 0;
}

/** @brief Every proxy and reducer is gone, the display is borrowed and never disconnected, and the double is clean. */
static int check_session_end(void)
{
    Check(!handler.wayland_ready);
    Check(fake_live_count() == 1);
    Check(fake_live_pointer(display));
    Check(fake_creates - fake_destroys == 1);
    Check(fake_disconnects == 0);
    for (uint32_t index = 0; index < SessionWindowsMaxReducers; ++index) Check(!handler.reducers[index].used);
    Check(fake_protocol_errors == 0);
    Check(fake_dead_requests == 0);
    Check(fake_double_destroys == 0);
    Check(fake_order_violations == 0);
    Check(fake_invalid_serials == 0);
    Check(fake_invalid_grabs == 0);
    Check(fake_sender_mismatch == 0);
    Check(!hook_failed);
    Check(!received_overflow);
    return 0;
}

/* ---- the eight cases ------------------------------------------------------------------------------------- */

static int case_admission_announce(char *detail, size_t capacity)
{
    static const char Expected[] = "wl_compositor:0 xdg_wm_base:2 xdg_surface:1 xdg_toplevel:3 xdg_toplevel:2 "
        "zxdg_decoration_manager_v1:1 zxdg_toplevel_decoration_v1:1 wl_surface:6 xdg_toplevel:10 xdg_toplevel:12";
    script_reset();
    script_announce(1, 1, "Title", "a-app");
    scenario_t scenario = {"admission and announce", InteropCapWindows, InteropCapWindows, 0, 0, hook_admission};
    Check(run_scenario(&scenario, TestRunMs) == HostControlSuccess);
    Check(ready_calls == 1 && ready_negotiated == InteropCapWindows);
    Check(admitted_frames == 1);
    if (strcmp(recorded_signature, Expected) != 0) {
        fprintf(stderr, "signature mismatch\n  expected: %s\n  actual:   %s\n", Expected, recorded_signature);
        return 1;
    }
    Check(strcmp(recorded_app_id, "waddle.dev0.a-app") == 0);
    Check(strcmp(recorded_title, "Title") == 0);
    session_windows_counters_t counters;
    session_windows_handler_counters(&handler, &counters);
    Check(counters.announces_realized == 1 && counters.announces_refused == 0);
    Check(peer_saw_hello_ack && peer_saw_goodbye);
    Check(received_count == 0);
    Check(closing_calls == 1 && closing_reason == (uint32_t)HostControlSuccess);
    Check(check_full_teardown() == 0);
    Check(check_session_end() == 0);
    unsigned requests = 1;
    for (const char *cursor = recorded_signature; *cursor != '\0'; ++cursor) {
        if (*cursor == ' ') ++requests;
    }
    snprintf(detail, capacity, "xdg_requests=%u app_id=%s title=%s", requests, recorded_app_id, recorded_title);
    return 0;
}

static int case_configure(char *detail, size_t capacity)
{
    script_reset();
    script_announce(1, 1, NULL, "a-app");
    scenario_t scenario = {"configure reaches guest", InteropCapWindows, InteropCapWindows, 2, 1, hook_configure};
    Check(run_scenario(&scenario, TestRunMs) == HostControlSuccess);
    Check(ready_calls == 1 && ready_negotiated == InteropCapWindows);
    Check(received_count == 1);
    outbound_t out;
    Check(decode_received(0, &out) == 0);
    Check(out.type == InteropMsgWindowConfigure && out.request_id == 0);
    Check(out.window == 1 && out.generation == 1);
    Check(out.has_rect && out.rect[0] == 0 && out.rect[1] == 0 && out.rect[2] == 800 && out.rect[3] == 600);
    Check(out.has_state && out.state == WaddleWindowStateActivated);
    session_windows_counters_t counters;
    session_windows_handler_counters(&handler, &counters);
    Check(counters.configures_sent == 1 && counters.closes_sent == 0);
    Check(closing_calls == 1 && closing_reason == (uint32_t)HostControlSuccess);
    Check(check_full_teardown() == 0);
    Check(check_session_end() == 0);
    snprintf(detail, capacity, "window=1 generation=1 rect=0,0,800,600 state=0x%x frames=%u", out.state, received_count);
    return 0;
}

static int case_move_validation(char *detail, size_t capacity)
{
    script_reset();
    script_announce(1, 1, NULL, "a-app");
    script_move(1, 1, 77);
    script_move(1, 1, 11);
    script_move(1, 1, 12);
    scenario_t scenario = {"move validation", InteropCapWindows, InteropCapWindows, 0, 0, hook_moves};
    Check(run_scenario(&scenario, TestRunMs) == HostControlSuccess);
    Check(recorded_press[0] == HostWaylandOk && recorded_press[1] == HostWaylandOk);
    Check(admitted_frames == 4);
    session_windows_counters_t counters;
    session_windows_handler_counters(&handler, &counters);
    Check(counters.moves_applied == 1 && counters.moves_ignored == 2);
    Check(fake_count_requests("xdg_toplevel", XDG_TOPLEVEL_MOVE) == 1);
    int move = fake_find("xdg_toplevel", XDG_TOPLEVEL_MOVE, 0);
    Check(move >= 0);
    Check(fake_requests[move].proxy == recorded_toplevel);
    Check(fake_requests[move].a0 == (long long)recorded_seat);
    Check(fake_requests[move].a1 == 102);
    Check(received_count == 0);
    Check(closing_calls == 1 && closing_reason == (uint32_t)HostControlSuccess);
    Check(check_full_teardown() == 0);
    Check(check_session_end() == 0);
    snprintf(detail, capacity, "refused=2 applied=1 wayland_serial=102 xdg_move_requests=1");
    return 0;
}

static int case_frame_ready(char *detail, size_t capacity)
{
    script_reset();
    script_announce(1, 1, NULL, "a-app");
    script_frame_ready(1, 1, 3, 7);
    scenario_t scenario = {"FrameReady releases the slot", InteropCapWindows, InteropCapWindows, 0, 0, NULL};
    Check(run_scenario(&scenario, TestRunMs) == HostControlSuccess);
    Check(received_count == 1);
    outbound_t out;
    Check(decode_received(0, &out) == 0);
    Check(out.type == InteropMsgFrameRelease && out.request_id == 0);
    Check(out.window == 1 && out.generation == 1);
    Check(out.has_slot && out.slot == 3);
    Check(out.has_sequence && out.sequence == 7);
    session_windows_counters_t counters;
    session_windows_handler_counters(&handler, &counters);
    Check(counters.frames_dropped_no_buffer_layer == 1);
    Check(closing_calls == 1 && closing_reason == (uint32_t)HostControlSuccess);
    Check(check_full_teardown() == 0);
    Check(check_session_end() == 0);
    snprintf(detail, capacity, "window=1 generation=1 slot=3 sequence=7 frames=%u", received_count);
    return 0;
}

static int case_retire_then_close(char *detail, size_t capacity)
{
    static const char *const RetireOrder[] = {"zxdg_toplevel_decoration_v1", "xdg_toplevel", "xdg_surface", "wl_surface"};
    script_reset();
    script_announce(1, 1, NULL, "a-app");
    script_retire(1, 1);
    scenario_t scenario = {"retire, then late close", InteropCapWindows, InteropCapWindows, 0, 0, hook_retire};
    Check(run_scenario(&scenario, TestRunMs) == HostControlSuccess);
    Check(retire_destroy_count == 4);
    for (unsigned index = 0; index < 4; ++index) Check(strcmp(retire_destroys[index], RetireOrder[index]) == 0);
    session_windows_counters_t counters;
    session_windows_handler_counters(&handler, &counters);
    Check(counters.retires_applied == 1 && counters.closes_sent == 0);
    Check(received_count == 0);
    /* The late close was queued before a dispatch of the display ran, so the double dropped it as a dead object. */
    Check(fake_dispatch_calls > retire_dispatch_mark);
    /* The retire destroyed the window, so the teardown on close has only the core left. */
    Check(closing_destroy_count == 6);
    for (unsigned index = 0; index < 6; ++index) Check(strcmp(closing_destroys[index], CoreTeardown[index]) == 0);
    Check(closing_calls == 1 && closing_reason == (uint32_t)HostControlSuccess);
    Check(check_session_end() == 0);
    snprintf(detail, capacity, "destroys=decoration,toplevel,xdg_surface,wl_surface closes_sent=0 frames=0");
    return 0;
}

static int case_display_error(char *detail, size_t capacity)
{
    script_reset();
    script_announce(1, 1, NULL, "a-app");
    scenario_t scenario = {"display error mid-run", InteropCapWindows, InteropCapWindows, 0, 0, hook_display_fault};
    Check(run_scenario(&scenario, TestRunMs) == HostControlFeatureFailed);
    Check(handler.failed == 1);
    Check(closing_calls == 1 && closing_reason == (uint32_t)HostControlFeatureFailed);
    Check(check_full_teardown() == 0);
    Check(fake_display_error != 0);
    Check(check_session_end() == 0);
    snprintf(detail, capacity, "result=HostControlFeatureFailed(%d) closing=1 reason=FeatureFailed", (int)HostControlFeatureFailed);
    return 0;
}

static int case_goodbye(char *detail, size_t capacity)
{
    script_reset();
    script_announce(1, 1, NULL, "a-app");
    script_goodbye();
    scenario_t scenario = {"guest goodbye", InteropCapWindows, InteropCapWindows, 0, 0, NULL};
    Check(run_scenario(&scenario, TestRunMs) == HostControlPeerFailed);
    Check(closing_calls == 1 && closing_reason == (uint32_t)HostControlPeerFailed);
    Check(admitted_frames == 1);
    Check(check_full_teardown() == 0);
    Check(check_session_end() == 0);
    snprintf(detail, capacity, "result=HostControlPeerFailed(%d) closing=1 goodbye_from_guest=1", (int)HostControlPeerFailed);
    return 0;
}

static int case_without_windows(char *detail, size_t capacity)
{
    script_reset();
    script_announce(1, 1, NULL, "a-app");
    /* Hello offers Input only: the intersection with the host's Windows offer is empty. */
    scenario_t scenario = {"no Windows negotiated", InteropCapInput, 0, 0, 0, NULL};
    Check(run_scenario(&scenario, TestRunMs) == HostControlUnexpectedFeature);
    Check(ready_calls == 1 && ready_negotiated == 0);
    Check(admitted_frames == 0);
    Check(peer_saw_error && peer_error_code == InteropErrorUnsupportedType);
    Check(peer_error_refused_type == InteropMsgWindowAnnounce);
    Check(closing_calls == 1 && closing_reason == (uint32_t)HostControlUnexpectedFeature);
    Check(closing_destroy_count == 0);
    Check(check_session_end() == 0);
    snprintf(detail, capacity, "result=HostControlUnexpectedFeature(%d) error=UnsupportedType refused=WindowAnnounce wayland=none",
        (int)HostControlUnexpectedFeature);
    return 0;
}

typedef int (*case_fn_t)(char *detail, size_t capacity);

static unsigned run_case(unsigned number, const char *name, case_fn_t fn)
{
    char detail[256];
    memset(detail, 0, sizeof detail);
    int result = fn(detail, sizeof detail);
    if (result != 0) {
        printf("FAIL %u %s\n", number, name);
        return 1;
    }
    printf("PASS %u %s: %s\n", number, name, detail);
    return 0;
}

int main(void)
{
    assert(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
    unsigned failed = 0;
    failed += run_case(1, "admission and announce", case_admission_announce);
    failed += run_case(2, "configure reaches guest", case_configure);
    failed += run_case(3, "move validation", case_move_validation);
    failed += run_case(4, "FrameReady releases the slot", case_frame_ready);
    failed += run_case(5, "retire, then late close", case_retire_then_close);
    failed += run_case(6, "display error mid-run", case_display_error);
    failed += run_case(7, "guest goodbye", case_goodbye);
    failed += run_case(8, "no Windows negotiated", case_without_windows);
    return failed == 0 ? 0 : 1;
}
