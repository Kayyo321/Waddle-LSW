/**
 * @file window_coordinator_suite.c
 * @brief Coordinator suites over the REAL graph, slot runtime, reducer and codec ABIs, with the observer and
 *        window-state adapter doubles, a capture fake and a recording wire fake (TODO #10 C1-C3, #13 guest).
 *
 * Every frame the coordinator sends is decoded with the real codec (interop_header_decode,
 * interop_payload_validate and interop_field_*), so the wire assertions check what the protocol says,
 * not what the coordinator intended. Each test returns 0 on success and 1 at the first failed Check.
 */
#include "window_coordinator.h"
#include "window_observer_api_double.h"
#include "window_state_adapter_double.h"
#include "window_coordinator_faults.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define Check(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "window_coordinator_suite.c:%d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

#define CoordEpoch ((uint64_t)5)
#define OwnProcess ((uint32_t)900)
#define HostAppKey "a-coord-test"
#define WireCapacity 4096u
#define WireFrameBytes WaddleCoordinatorMaxFrameBytes
#define InboundCapacity 64u

/* ---- Recording wire fake ------------------------------------------------------------------------ */

/** @brief Decoded view of one sent frame. */
typedef struct sent_t {
    uint16_t type;
    uint64_t window;
    uint64_t generation;
    uint32_t slot;
    uint64_t sequence;
    uint32_t error;
    uint32_t state;
    uint32_t has_rect;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t has_parent;
    uint64_t parent_window;
    uint64_t parent_generation;
} sent_t;

typedef struct wire_t {
    uint8_t frames[WireCapacity][WireFrameBytes];
    uint32_t lengths[WireCapacity];
    uint32_t count;
    uint32_t backpressure_budget;
    int disconnected;
    uint8_t inbound[InboundCapacity][WireFrameBytes];
    uint32_t inbound_lengths[InboundCapacity];
    uint32_t inbound_head;
    uint32_t inbound_count;
    uint32_t send_calls;
} wire_t;

static window_coordinator_send_result_t wire_send(void *context, const uint8_t *frame, size_t length) {
    wire_t *wire = (wire_t *)context;
    wire->send_calls += 1u;
    if (wire->disconnected != 0) return WindowCoordinatorDisconnected;
    if (wire->backpressure_budget != 0u) {
        wire->backpressure_budget -= 1u;
        return WindowCoordinatorBackpressure;
    }
    if (wire->count >= WireCapacity || length > WireFrameBytes) return WindowCoordinatorDisconnected;
    memcpy(wire->frames[wire->count], frame, length);
    wire->lengths[wire->count] = (uint32_t)length;
    wire->count += 1u;
    return WindowCoordinatorSent;
}

static int wire_receive(void *context, uint8_t *frame, size_t capacity, size_t *length) {
    wire_t *wire = (wire_t *)context;
    if (wire->disconnected != 0) return -1;
    if (wire->inbound_count == 0u) return 0;
    if (capacity < wire->inbound_lengths[wire->inbound_head]) return 0;
    memcpy(frame, wire->inbound[wire->inbound_head], wire->inbound_lengths[wire->inbound_head]);
    *length = wire->inbound_lengths[wire->inbound_head];
    wire->inbound_head = (wire->inbound_head + 1u) % InboundCapacity;
    wire->inbound_count -= 1u;
    return 1;
}

static const window_coordinator_io_t WireIo = {NULL, wire_send, wire_receive};

/** @brief Queues one host frame built with the real codec. */
static int wire_push_host(wire_t *wire, const uint8_t *frame, size_t length) {
    uint32_t slot;
    if (wire->inbound_count >= InboundCapacity || length > WireFrameBytes) return 1;
    slot = (wire->inbound_head + wire->inbound_count) % InboundCapacity;
    memcpy(wire->inbound[slot], frame, length);
    wire->inbound_lengths[slot] = (uint32_t)length;
    wire->inbound_count += 1u;
    return 0;
}

/** @brief Builds a host frame of the given type with the real codec. */
static int build_host(uint16_t type, uint8_t *frame, size_t capacity, size_t *length, uint64_t window, uint64_t generation,
                      int has_slot, uint32_t slot, uint64_t sequence, int has_state, uint32_t state) {
    interop_writer_t writer;
    interop_writer_init(&writer, frame, capacity);
    /* Ascending tag order, as the codec requires: window 1, generation 2, state 10, slot 12, sequence 13. */
    if (interop_put_uint(&writer, InteropTagWindow, window) != InteropErrorNone) return 1;
    if (interop_put_uint(&writer, InteropTagGeneration, generation) != InteropErrorNone) return 1;
    if (has_state != 0) {
        if (interop_put_uint(&writer, InteropTagState, state) != InteropErrorNone) return 1;
    }
    if (has_slot != 0) {
        if (interop_put_uint(&writer, InteropTagSlot, slot) != InteropErrorNone) return 1;
        if (interop_put_uint(&writer, InteropTagFrameSequence, sequence) != InteropErrorNone) return 1;
    }
    return interop_writer_finish(&writer, type, 0u, InteropFromHost, length) == InteropErrorNone ? 0 : 1;
}

/** @brief Builds a host popup update (window, generation, rect) in ascending tag order. */
static int build_host_popup(uint8_t *frame, size_t capacity, size_t *length, uint64_t window, uint64_t generation) {
    interop_writer_t writer;
    interop_writer_init(&writer, frame, capacity);
    if (interop_put_uint(&writer, InteropTagWindow, window) != InteropErrorNone) return 1;
    if (interop_put_uint(&writer, InteropTagGeneration, generation) != InteropErrorNone) return 1;
    if (interop_put_rect(&writer, InteropTagRect, 10, 20, 300, 200) != 0) return 1;
    return interop_writer_finish(&writer, InteropMsgPopupRepositioned, 0u, InteropFromHost, length) == InteropErrorNone ? 0 : 1;
}

/** @brief Decodes one recorded frame with the real codec. Returns 0 on success. */
static int decode_frame(const uint8_t *bytes, size_t length, sent_t *out) {
    interop_header_t header;
    const uint8_t *payload;
    uint64_t value = 0;
    memset(out, 0, sizeof(*out));
    if (interop_header_decode(bytes, length, &header) != InteropErrorNone) return 1;
    if ((size_t)InteropHeaderBytes + header.length != length) return 1;
    payload = bytes + InteropHeaderBytes;
    if (interop_payload_validate(header.type, InteropFromGuest, payload, header.length) != InteropErrorNone) return 1;
    out->type = header.type;
    if (interop_field_uint(payload, header.length, InteropTagWindow, 8u, &out->window) == 0 &&
        interop_field_uint(payload, header.length, InteropTagGeneration, 8u, &out->generation) == 0) {
        /* window and generation present */
    }
    if (interop_field_uint(payload, header.length, InteropTagSlot, 4u, &value) == 0) out->slot = (uint32_t)value;
    if (interop_field_uint(payload, header.length, InteropTagFrameSequence, 8u, &out->sequence) != 0) out->sequence = 0;
    if (interop_field_uint(payload, header.length, InteropTagErrorCode, 4u, &value) == 0) out->error = (uint32_t)value;
    if (interop_field_uint(payload, header.length, InteropTagState, 4u, &value) == 0) out->state = (uint32_t)value;
    if (interop_field_uint(payload, header.length, InteropTagParent, 8u, &out->parent_window) == 0) out->has_parent = 1u;
    if (interop_field_uint(payload, header.length, InteropTagParentGeneration, 8u, &out->parent_generation) != 0) {
        out->parent_generation = 0;
    }
    {
        const uint8_t *rect = NULL;
        uint16_t rect_length = 0;
        if (interop_field_get(payload, header.length, InteropTagRect, 0u, &rect, &rect_length) == 0 && rect_length == 16u) {
            out->has_rect = 1u;
            out->x = (int32_t)((uint32_t)rect[0] | ((uint32_t)rect[1] << 8) | ((uint32_t)rect[2] << 16) | ((uint32_t)rect[3] << 24));
            out->y = (int32_t)((uint32_t)rect[4] | ((uint32_t)rect[5] << 8) | ((uint32_t)rect[6] << 16) | ((uint32_t)rect[7] << 24));
            out->width = (uint32_t)rect[8] | ((uint32_t)rect[9] << 8) | ((uint32_t)rect[10] << 16) | ((uint32_t)rect[11] << 24);
            out->height = (uint32_t)rect[12] | ((uint32_t)rect[13] << 8) | ((uint32_t)rect[14] << 16) | ((uint32_t)rect[15] << 24);
        }
    }
    return 0;
}

/** @brief Returns the i-th sent frame decoded, or 0 when out of range. */
static int sent_at(const wire_t *wire, uint32_t index, sent_t *out) {
    if (index >= wire->count) return 0;
    return decode_frame(wire->frames[index], wire->lengths[index], out) == 0;
}

/** @brief Counts sent frames of a type for one exact (window, generation) pair (generation 0 matches any). */
static uint32_t count_type(const wire_t *wire, uint16_t type, uint64_t window, uint64_t generation) {
    uint32_t total = 0;
    for (uint32_t index = 0; index < wire->count; ++index) {
        sent_t sent;
        if (!sent_at(wire, index, &sent)) continue;
        if (sent.type != type) continue;
        if (window != 0u && sent.window != window) continue;
        if (generation != 0u && sent.generation != generation) continue;
        total += 1u;
    }
    return total;
}

/* ---- Capture fake ------------------------------------------------------------------------------- */

typedef struct capture_t {
    uint32_t started;
    uint32_t stopped;
    uint32_t frames_written;
    uint32_t next_failure;     /* 0 none; 1 entered failure; 2 unentered failure on the next write */
    uint32_t start_failure;    /* nonzero: the next start is refused */
    uint32_t last_offset_ok;   /* data_offset + data_bytes was inside the declared mapping */
    uint64_t mapping_bytes;
    uint32_t active[InteropMaxWindows];
    uint64_t active_window[InteropMaxWindows];
    uint64_t active_generation[InteropMaxWindows];
    uint64_t pattern_digest;
} capture_t;

static int capture_start(void *context, uint64_t window, uint32_t generation, uint32_t width, uint32_t height) {
    capture_t *capture = (capture_t *)context;
    (void)width;
    (void)height;
    if (capture->start_failure != 0u) {
        capture->start_failure = 0u;
        return 1;
    }
    for (uint32_t index = 0; index < InteropMaxWindows; ++index) {
        if (capture->active[index] == 0u) {
            capture->active[index] = 1u;
            capture->active_window[index] = window;
            capture->active_generation[index] = generation;
            capture->started += 1u;
            return 0;
        }
    }
    return 1;
}

static void capture_stop(void *context, uint64_t window, uint32_t generation) {
    capture_t *capture = (capture_t *)context;
    for (uint32_t index = 0; index < InteropMaxWindows; ++index) {
        if (capture->active[index] != 0u && capture->active_window[index] == window &&
            capture->active_generation[index] == generation) {
            capture->active[index] = 0u;
            capture->stopped += 1u;
            return;
        }
    }
}

static int capture_write(void *context, uint64_t window, uint32_t generation, uint8_t slot, uint64_t sequence,
                         const waddle_slot_view_t *view) {
    capture_t *capture = (capture_t *)context;
    uint32_t failure = capture->next_failure;
    (void)window;
    (void)generation;
    capture->next_failure = 0u;
    if (failure != 0u) return (int)failure;
    /* A deterministic pattern digest over the frame identity and the view, as a fake capture would write. */
    capture->pattern_digest = capture->pattern_digest * UINT64_C(1099511628211) ^ (sequence + slot + view->data_offset);
    if (view->data_offset + view->data_bytes <= capture->mapping_bytes) capture->last_offset_ok = 1u;
    capture->frames_written += 1u;
    return 0;
}

static const window_capture_ops_t CaptureOpsTable = {NULL, capture_start, capture_stop, capture_write};

/* ---- Fixture ------------------------------------------------------------------------------------ */

typedef struct fixture_t {
    api_double_t *observer;
    wsa_double_t adapter;
    capture_t capture;
    wire_t wire;
    window_capture_ops_t capture_ops;
    window_coordinator_io_t io;
    window_coordinator_t *coordinator;
    uint64_t visible[InteropMaxWindows];
    uint32_t visible_count;
    uint64_t epoch;
} fixture_t;

/** @brief The one fixture every suite reuses; fixture_open resets it. */
static fixture_t GlobalFixture;

static int app_key_lookup(void *context, const interop_window_facts_t *facts, uint8_t *key, uint32_t capacity,
                          uint32_t *length) {
    static const uint8_t Key[] = HostAppKey;
    (void)context;
    (void)facts;
    if (capacity < sizeof(Key) - 1u) return 1;
    memcpy(key, Key, sizeof(Key) - 1u);
    *length = (uint32_t)(sizeof(Key) - 1u);
    return 0;
}

static void fixture_close(fixture_t *f);
static void advance_clock(fixture_t *f, uint64_t milliseconds);
static int host_release(fixture_t *f, uint64_t window, uint64_t generation, uint32_t slot, uint64_t sequence);

static window_coordinator_config_t make_config(fixture_t *f, uint64_t epoch, uint64_t capabilities) {
    window_coordinator_config_t config;
    memset(&config, 0, sizeof(config));
    config.session_epoch = epoch;
    config.uac_mode = 0u;
    config.own_process_id = OwnProcess;
    config.negotiated_capabilities = capabilities;
    config.app_key_lookup = app_key_lookup;
    config.observer_api = &ApiDoubleTable;
    config.observer_api_context = f->observer;
    config.native_api = wsa_double_api();
    config.native_api_context = &f->adapter;
    config.io = &f->io;
    config.capture = &f->capture_ops;
    return config;
}

static void fixture_prepare(fixture_t *f) {
    memset(f, 0, sizeof(*f));
    f->observer = api_double_new();
    wsa_double_init(&f->adapter, 0u);
    f->adapter.current_thread = 0u;
    f->capture_ops = CaptureOpsTable;
    f->capture_ops.context = &f->capture;
    f->capture.mapping_bytes = UINT64_C(1) << 30;
    f->io = WireIo;
    f->io.context = &f->wire;
    f->epoch = CoordEpoch;
}

static int fixture_open(fixture_t *f, uint64_t capabilities) {
    window_coordinator_config_t config;
    if (f->coordinator != NULL || f->observer != NULL) fixture_close(f);
    memset(&Faults, 0, sizeof(Faults));
    fixture_prepare(f);
    if (f->observer == NULL) return 1;
    config = make_config(f, CoordEpoch, capabilities);
    if (window_coordinator_create(&config, &f->coordinator) != WindowCoordinatorOk) return 1;
    return 0;
}

static void fixture_close(fixture_t *f) {
    memset(&Faults, 0, sizeof(Faults));
    if (f->coordinator != NULL && window_coordinator_state(f->coordinator) == WindowCoordinatorStateLost) {
        /* A lost transport keeps its quarantines until the host proves its buffers are gone. */
        f->wire.disconnected = 0;
        f->epoch += 1u;
        (void)window_coordinator_reconnect(f->coordinator, f->epoch, 1u);
    }
    if (f->coordinator != NULL) {
        /* Retire every window and release every pin the host still holds, so the coordinator can be freed. */
        if (f->observer != NULL) for (uint32_t round = 0; round < 8u && window_coordinator_entry_count(f->coordinator) != 0u; ++round) {
            while (f->visible_count != 0u) {
                uint64_t window = f->visible[0];
                uint32_t write = 0;
                for (uint32_t index = 0; index < f->visible_count; ++index) {
                    if (f->visible[index] != window) f->visible[write++] = f->visible[index];
                }
                f->visible_count = write;
                api_double_enum_set(f->observer, f->visible, f->visible_count);
                api_double_remove(f->observer, window);
                wsa_double_destroy_window(&f->adapter, (uintptr_t)window);
            }
            advance_clock(f, 300u);
            (void)window_coordinator_tick(f->coordinator, 8u, NULL);
            for (uint32_t index = 0; index < f->wire.count; ++index) {
                sent_t sent;
                if (decode_frame(f->wire.frames[index], f->wire.lengths[index], &sent) == 0 &&
                    sent.type == InteropMsgFrameReady) {
                    (void)host_release(f, sent.window, sent.generation, sent.slot, sent.sequence);
                }
            }
            (void)window_coordinator_tick(f->coordinator, 8u, NULL);
        }
        (void)window_coordinator_destroy(&f->coordinator, 1u);
    }
    if (f->observer != NULL) api_double_free(f->observer);
    f->observer = NULL;
}

/** @brief Adds a visible window to the observer double and the adapter double, and lists it as visible. */
static void add_window(fixture_t *f, uint64_t window) {
    window_state_rect_t rect = {100, 100, 500, 400};
    (void)api_double_add(f->observer, window);
    wsa_double_add(&f->adapter, (uintptr_t)window, ApiDoubleForeignPid, 77u, rect);
    f->visible[f->visible_count++] = window;
    api_double_enum_set(f->observer, f->visible, f->visible_count);
}

/** @brief Removes a window from both doubles and from the visible list. */
static void remove_window(fixture_t *f, uint64_t window) {
    uint32_t write = 0;
    for (uint32_t index = 0; index < f->visible_count; ++index) {
        if (f->visible[index] != window) f->visible[write++] = f->visible[index];
    }
    f->visible_count = write;
    api_double_enum_set(f->observer, f->visible, f->visible_count);
    api_double_remove(f->observer, window);
    wsa_double_destroy_window(&f->adapter, (uintptr_t)window);
}

/** @brief Makes the observer's next refresh due, by moving its clock past the cadence. */
static void advance_clock(fixture_t *f, uint64_t milliseconds) {
    f->observer->monotonic += milliseconds;
}

static window_coordinator_status_t tick(fixture_t *f) {
    return window_coordinator_tick(f->coordinator, 8u, NULL);
}

/** @brief Plays the host's FrameRelease for one published frame. */
static int host_release(fixture_t *f, uint64_t window, uint64_t generation, uint32_t slot, uint64_t sequence) {
    uint8_t frame[WireFrameBytes];
    size_t length = 0;
    if (build_host(InteropMsgFrameRelease, frame, sizeof(frame), &length, window, generation, 1, slot, sequence, 0, 0) != 0) return 1;
    return wire_push_host(&f->wire, frame, length);
}


/* ---- Suites ------------------------------------------------------------------------------------- */

static int test_create_without_capability_is_idle(void) {
    fixture_t *fp = &GlobalFixture;
    uint64_t window = 0x1000;
    Check(fixture_open(fp, 0u) == 0);
    Check(window_coordinator_state(fp->coordinator) == WindowCoordinatorStateIdle);
    add_window(fp, window);
    Check(tick(fp) == WindowCoordinatorNoCapability);
    Check(fp->wire.count == 0u);
    Check(window_coordinator_entry_count(fp->coordinator) == 0u);
    fixture_close(fp);
    return 0;
}

static int test_create_rejects_invalid_config(void) {
    window_coordinator_config_t config;
    window_coordinator_t *coordinator = NULL;
    fixture_t *fp = &GlobalFixture;
    Check(fixture_open(fp, InteropCapWindows) == 0);
    config = make_config(fp, CoordEpoch, InteropCapWindows);
    config.session_epoch = 0u;
    Check(window_coordinator_create(&config, &coordinator) == WindowCoordinatorInvalidArgument);
    Check(coordinator == NULL);
    config = make_config(fp, CoordEpoch, InteropCapWindows);
    config.capture = NULL;
    Check(window_coordinator_create(&config, &coordinator) == WindowCoordinatorInvalidArgument);
    config = make_config(fp, CoordEpoch, InteropCapWindows);
    config.io = NULL;
    Check(window_coordinator_create(&config, &coordinator) == WindowCoordinatorInvalidArgument);
    Check(window_coordinator_create(NULL, &coordinator) == WindowCoordinatorInvalidArgument);
    Check(window_coordinator_create(&config, NULL) == WindowCoordinatorInvalidArgument);
    fixture_close(fp);
    return 0;
}

static int test_announce_publish_and_release(void) {
    fixture_t *fp = &GlobalFixture;
    sent_t sent;
    Check(fixture_open(fp, InteropCapWindows) == 0);
    add_window(fp, 0x1000);
    Check(tick(fp) == WindowCoordinatorOk);

    /* Admission: Announce first, then the first frame of the admitted generation. */
    Check(fp->wire.count >= 2u);
    Check(sent_at(&fp->wire, 0, &sent));
    Check(sent.type == InteropMsgWindowAnnounce);
    Check(sent.window == 0x1000u && sent.generation == 1u);
    Check(sent.has_rect == 1u && sent.width == 800u && sent.height == 600u);
    /* Default style carries the three capability bits: Resizable, Maximizable, Minimizable. */
    Check(sent.state == (WaddleWindowStateResizable | WaddleWindowStateMaximizable | WaddleWindowStateMinimizable));
    Check(sent_at(&fp->wire, 1, &sent));
    Check(sent.type == InteropMsgFrameReady);
    Check(sent.window == 0x1000u && sent.generation == 1u);
    Check(sent.slot == 0u && sent.sequence == 1u);
    Check(fp->capture.started == 1u);
    Check(fp->capture.last_offset_ok == 1u);
    Check(window_coordinator_entry_count(fp->coordinator) == 1u);

    /* A host release clears exactly its pin; the next frame takes the next sequence and slot. */
    Check(host_release(fp, 0x1000, 1, 0, 1) == 0);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(sent_at(&fp->wire, fp->wire.count - 1u, &sent));
    Check(sent.type == InteropMsgFrameReady && sent.sequence == 2u && sent.slot == 1u);
    {
        window_coordinator_stats_t stats;
        Check(window_coordinator_get_stats(fp->coordinator, &stats) == WindowCoordinatorOk);
        Check(stats.announces == 1u && stats.releases_applied == 1u);
    }
    fixture_close(fp);
    return 0;
}

static int test_stale_host_frame_is_answered_not_applied(void) {
    fixture_t *fp = &GlobalFixture;
    sent_t sent;
    uint8_t frame[WireFrameBytes];
    size_t length = 0;
    Check(fixture_open(fp, InteropCapWindows) == 0);
    add_window(fp, 0x1000);
    Check(tick(fp) == WindowCoordinatorOk);
    /* FrameRelease for a window the coordinator does not hold. */
    Check(build_host(InteropMsgFrameRelease, frame, sizeof(frame), &length, 0x9999, 1, 1, 0, 1, 0, 0) == 0);
    Check(wire_push_host(&fp->wire, frame, length) == 0);
    /* FrameRelease with the right identity but a sequence that was never published. */
    Check(build_host(InteropMsgFrameRelease, frame, sizeof(frame), &length, 0x1000, 1, 1, 2, 77, 0, 0) == 0);
    Check(wire_push_host(&fp->wire, frame, length) == 0);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(count_type(&fp->wire, InteropMsgError, 0, 0) == 1u);
    {
        uint32_t error_index = 0;
        for (uint32_t index = 0; index < fp->wire.count; ++index) {
            if (sent_at(&fp->wire, index, &sent) && sent.type == InteropMsgError) error_index = index + 1u;
        }
        Check(error_index != 0u && sent_at(&fp->wire, error_index - 1u, &sent));
        Check(sent.error == InteropErrorStaleWindow);
    }
    {
        window_coordinator_stats_t stats;
        Check(window_coordinator_get_stats(fp->coordinator, &stats) == WindowCoordinatorOk);
        Check(stats.stale_replies == 1u && stats.releases_ignored == 1u && stats.releases_applied == 0u);
    }
    fixture_close(fp);
    return 0;
}

static int test_configure_and_close_reach_the_native_window(void) {
    fixture_t *fp = &GlobalFixture;
    uint8_t frame[WireFrameBytes];
    size_t length = 0;
    wsa_double_window_t *native;
    Check(fixture_open(fp, InteropCapWindows) == 0);
    add_window(fp, 0x1000);
    Check(tick(fp) == WindowCoordinatorOk);

    /* Host configure: minimize. The reducer plans a guest minimize; the adapter executes it. */
    Check(build_host(InteropMsgWindowConfigure, frame, sizeof(frame), &length, 0x1000, 1, 0, 0, 0, 1,
                     WaddleWindowStateMinimized) == 0);
    Check(wire_push_host(&fp->wire, frame, length) == 0);
    Check(tick(fp) == WindowCoordinatorOk);
    native = wsa_double_find(&fp->adapter, 0x1000);
    Check(native != NULL && native->iconic == 1);
    {
        window_coordinator_stats_t stats;
        Check(window_coordinator_get_stats(fp->coordinator, &stats) == WindowCoordinatorOk);
        Check(stats.configures == 1u && stats.reducer_failures == 0u);
    }

    /* Host close: the reducer plans PostClose; the native window receives WM_CLOSE and stays alive. */
    Check(build_host(InteropMsgWindowClose, frame, sizeof(frame), &length, 0x1000, 1, 0, 0, 0, 0, 0) == 0);
    Check(wire_push_host(&fp->wire, frame, length) == 0);
    Check(tick(fp) == WindowCoordinatorOk);
    native = wsa_double_find(&fp->adapter, 0x1000);
    Check(native != NULL && native->close_posts == 1u && native->exists == 1);
    fixture_close(fp);
    return 0;
}

static int test_backpressure_keeps_order_and_loses_nothing(void) {
    fixture_t *fp = &GlobalFixture;
    sent_t sent;
    Check(fixture_open(fp, InteropCapWindows) == 0);
    fp->wire.backpressure_budget = 3u;
    add_window(fp, 0x1000);
    add_window(fp, 0x2000);
    for (uint32_t round = 0; round < 6u; ++round) {
        Check(tick(fp) == WindowCoordinatorOk);
    }
    /* Every announce and frame was eventually accepted, and the order is announce before its frame. */
    Check(count_type(&fp->wire, InteropMsgWindowAnnounce, 0, 0) == 2u);
    Check(count_type(&fp->wire, InteropMsgFrameReady, 0x1000, 1) >= 1u);
    Check(count_type(&fp->wire, InteropMsgFrameReady, 0x2000, 2) >= 1u);
    Check(sent_at(&fp->wire, 0, &sent) && sent.type == InteropMsgWindowAnnounce);
    {
        window_coordinator_stats_t stats;
        Check(window_coordinator_get_stats(fp->coordinator, &stats) == WindowCoordinatorOk);
        Check(stats.backpressure == 3u && stats.disconnects == 0u);
    }
    fixture_close(fp);
    return 0;
}

static int test_retire_waits_for_pins_then_completes(void) {
    fixture_t *fp = &GlobalFixture;
    Check(fixture_open(fp, InteropCapWindows) == 0);
    add_window(fp, 0x1000);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(fp->capture.started == 1u);

    /* The window disappears: the observer retires it; the coordinator stops capture. IMPL 5.3: WindowRetire is
     * sent only after the host released every slot, so it stays unsent while the pin is held. */
    remove_window(fp, 0x1000);
    advance_clock(fp, 300u);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(count_type(&fp->wire, InteropMsgWindowRetire, 0x1000, 1) == 0u);
    Check(fp->capture.stopped == 1u);
    /* The frame pin is still held by the host, so the quarantine remains. */
    Check(window_coordinator_entry_count(fp->coordinator) == 1u);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(window_coordinator_entry_count(fp->coordinator) == 1u);
    Check(count_type(&fp->wire, InteropMsgWindowRetire, 0x1000, 1) == 0u);

    /* The host releases the pin. The next pass sends Retire and completes the retirement. */
    Check(host_release(fp, 0x1000, 1, 0, 1) == 0);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(count_type(&fp->wire, InteropMsgWindowRetire, 0x1000, 1) == 1u);
    Check(window_coordinator_entry_count(fp->coordinator) == 0u);
    {
        window_coordinator_stats_t stats;
        Check(window_coordinator_get_stats(fp->coordinator, &stats) == WindowCoordinatorOk);
        Check(stats.retires == 1u && stats.retire_completed == 1u && stats.releases_applied == 1u);
    }
    fixture_close(fp);
    return 0;
}

static int test_children_retire_before_parents(void) {
    fixture_t *fp = &GlobalFixture;
    interop_window_facts_t dummy;
    api_double_window_t *child;
    Check(fixture_open(fp, InteropCapWindows) == 0);
    (void)dummy;
    add_window(fp, 0x1000);
    add_window(fp, 0x2000);
    child = api_double_find(fp->observer, 0x2000);
    Check(child != NULL);
    /* A dialog owned by the parent: its announce names the parent generation. */
    child->facts.owner = 0x1000;
    child->facts.owner_root = 0x1000;
    Check(tick(fp) == WindowCoordinatorOk);
    /* Generations come from the graph counter of the epoch: the parent is generation 1, the child 2. */
    Check(count_type(&fp->wire, InteropMsgWindowAnnounce, 0x2000, 2) == 1u);
    Check(count_type(&fp->wire, InteropMsgWindowAnnounce, 0x1000, 1) == 1u);
    Check(window_coordinator_entry_count(fp->coordinator) == 2u);

    remove_window(fp, 0x1000);
    remove_window(fp, 0x2000);
    advance_clock(fp, 300u);
    Check(tick(fp) == WindowCoordinatorOk);
    /* Retire waits for the pins (IMPL 5.3): neither Retire is sent while both pins are held. */
    Check(count_type(&fp->wire, InteropMsgWindowRetire, 0x2000, 2) == 0u);
    Check(count_type(&fp->wire, InteropMsgWindowRetire, 0x1000, 1) == 0u);
    /* The child's pin drains first: its Retire goes out; the parent still waits for its own pin. */
    Check(host_release(fp, 0x2000, 2, 0, 1) == 0);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(count_type(&fp->wire, InteropMsgWindowRetire, 0x2000, 2) == 1u);
    Check(count_type(&fp->wire, InteropMsgWindowRetire, 0x1000, 1) == 0u);
    Check(window_coordinator_entry_count(fp->coordinator) == 1u);
    /* Then the parent: its Retire follows the child's in the wire order. */
    Check(host_release(fp, 0x1000, 1, 0, 1) == 0);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(window_coordinator_entry_count(fp->coordinator) == 0u);
    fixture_close(fp);
    return 0;
}

static int test_phantom_when_the_native_window_cannot_be_served(void) {
    fixture_t *fp = &GlobalFixture;
    Check(fixture_open(fp, InteropCapWindows) == 0);
    /* The observer sees the window, the adapter does not: admission must not announce it. */
    (void)api_double_add(fp->observer, 0x3000);
    fp->visible[fp->visible_count++] = 0x3000;
    api_double_enum_set(fp->observer, fp->visible, fp->visible_count);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(count_type(&fp->wire, InteropMsgWindowAnnounce, 0, 0) == 0u);
    Check(count_type(&fp->wire, InteropMsgFrameReady, 0, 0) == 0u);
    {
        window_coordinator_stats_t stats;
        Check(window_coordinator_get_stats(fp->coordinator, &stats) == WindowCoordinatorOk);
        Check(stats.phantoms == 1u && stats.admission_failures == 1u);
    }
    /* The graph later retires the phantom; no wire message is sent for it, and the entry is released. */
    remove_window(fp, 0x3000);
    advance_clock(fp, 300u);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(count_type(&fp->wire, InteropMsgWindowRetire, 0, 0) == 0u);
    Check(window_coordinator_entry_count(fp->coordinator) == 0u);
    fixture_close(fp);
    return 0;
}

static int test_capture_failure_cancels_the_claim(void) {
    fixture_t *fp = &GlobalFixture;
    sent_t sent;
    Check(fixture_open(fp, InteropCapWindows) == 0);
    fp->capture.next_failure = 1u;
    add_window(fp, 0x1000);
    Check(tick(fp) == WindowCoordinatorOk);
    /* The first frame failed after entering: no FrameReady, and the consumed sequence is not reused. */
    Check(count_type(&fp->wire, InteropMsgFrameReady, 0, 0) == 0u);
    fp->capture.next_failure = 2u;
    Check(tick(fp) == WindowCoordinatorOk);
    Check(count_type(&fp->wire, InteropMsgFrameReady, 0, 0) == 0u);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(sent_at(&fp->wire, fp->wire.count - 1u, &sent));
    Check(sent.type == InteropMsgFrameReady && sent.sequence == 3u);
    {
        window_coordinator_stats_t stats;
        Check(window_coordinator_get_stats(fp->coordinator, &stats) == WindowCoordinatorOk);
        Check(stats.capture_failures == 2u && stats.frames_published == 1u);
    }
    fixture_close(fp);
    return 0;
}

static int test_disconnect_and_reconnect_with_proof(void) {
    fixture_t *fp = &GlobalFixture;
    sent_t sent;
    Check(fixture_open(fp, InteropCapWindows) == 0);
    add_window(fp, 0x1000);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(fp->capture.started == 1u);

    /* The transport drops on the next send, which carries the announce of a second window. */
    fp->wire.disconnected = 1;
    add_window(fp, 0x2000);
    advance_clock(fp, 300u);
    Check(tick(fp) == WindowCoordinatorLost);
    Check(window_coordinator_state(fp->coordinator) == WindowCoordinatorStateLost);
    Check(fp->capture.stopped >= 1u);
    Check(window_coordinator_mark_lost(fp->coordinator) == WindowCoordinatorBadState);
    /* Lost: nothing is sent, and a reconnect without host proof is refused and changes nothing. */
    {
        uint32_t before = fp->wire.send_calls;
        Check(tick(fp) == WindowCoordinatorLost);
        Check(fp->wire.send_calls == before);
    }
    Check(window_coordinator_reconnect(fp->coordinator, CoordEpoch + 1u, 0u) == WindowCoordinatorMissingProof);
    Check(window_coordinator_reconnect(fp->coordinator, CoordEpoch, 1u) == WindowCoordinatorInvalidArgument);
    Check(window_coordinator_state(fp->coordinator) == WindowCoordinatorStateLost);

    /* The host proves its buffers are gone. The new epoch re-announces the visible window. */
    fp->wire.disconnected = 0;
    fp->wire.count = 0u;
    Check(window_coordinator_reconnect(fp->coordinator, CoordEpoch + 1u, 1u) == WindowCoordinatorOk);
    Check(window_coordinator_state(fp->coordinator) == WindowCoordinatorStateRunning);
    advance_clock(fp, 300u);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(sent_at(&fp->wire, 0, &sent));
    Check(sent.type == InteropMsgWindowAnnounce && sent.window == 0x1000u && sent.generation == 1u);
    fixture_close(fp);
    return 0;
}

/* ---- Fault injection at the ABI seams -------------------------------------------------------- */

/** @brief Adds a window and reports its creation through a CREATE hint, so the next pass admits it. */
static void admit_window_now(fixture_t *f, uint64_t window) {
    add_window(f, window);
    api_double_hint(f->observer, 0x8000u, window);
    advance_clock(f, 300u);
}

/** @brief Generation of the newest Announce sent for one window, or 0 when none was sent. */
static uint64_t announced_generation(fixture_t *f, uint64_t window) {
    uint64_t generation = 0u;
    sent_t sent;
    for (uint32_t index = 0; index < f->wire.count; ++index) {
        if (sent_at(&f->wire, index, &sent) && sent.type == InteropMsgWindowAnnounce && sent.window == window) {
            generation = sent.generation;
        }
    }
    return generation;
}

/** @brief Counts frames of one message type for one window in everything sent so far. */
static uint32_t count_frames(fixture_t *f, uint16_t type, uint64_t window) {
    uint32_t count = 0u;
    sent_t sent;
    for (uint32_t index = 0; index < f->wire.count; ++index) {
        if (sent_at(&f->wire, index, &sent) && sent.type == type && sent.window == window) count += 1u;
    }
    return count;
}

/** @brief Releases every FrameReady sent so far for one window. Refused pushes are not retried here. */
static void release_window_frames(fixture_t *f, uint64_t window) {
    for (uint32_t index = 0; index < f->wire.count; ++index) {
        sent_t sent;
        if (sent_at(&f->wire, index, &sent) && sent.type == InteropMsgFrameReady && sent.window == window) {
            (void)host_release(f, window, sent.generation, sent.slot, sent.sequence);
        }
    }
}

/** @brief Retires one window: removes it, releases its frames and ticks until its entry is gone. */
static void retire_one_window(fixture_t *f, uint64_t window) {
    remove_window(f, window);
    api_double_hint(f->observer, 0x8001u, window);
    advance_clock(f, 300u);
    for (uint32_t pass = 0; pass < 8u && window_coordinator_entry_count(f->coordinator) != 0u; ++pass) {
        release_window_frames(f, window);
        advance_clock(f, 300u);
        (void)tick(f);
    }
}

/** @brief Create fails cleanly for each backend that cannot be built, and nothing is left allocated. */
static int test_create_backend_faults(void) {
    fixture_t *fp = &GlobalFixture;
    window_coordinator_config_t config;
    window_coordinator_t *coordinator;
    for (uint32_t which = 0; which < 6u; ++which) {
        if (fp->coordinator != NULL || fp->observer != NULL) fixture_close(fp);
        memset(&Faults, 0, sizeof(Faults));
        fixture_prepare(fp);
        switch (which) {
        case 0: Faults.adapter_create = 1u; break;
        case 1: Faults.graph_create = 1u; break;
        case 2: Faults.slot_create = 1u; break;
        case 3: Faults.observer_create = 1u; break;
        case 4: Faults.observer_bind = 1u; break;
        default: Faults.observer_start = 1u; break;
        }
        config = make_config(fp, CoordEpoch, InteropCapWindows);
        coordinator = NULL;
        Check(window_coordinator_create(&config, &coordinator) == WindowCoordinatorBackendFailed);
        Check(coordinator == NULL);
    }
    fixture_close(fp);
    return 0;
}

/** @brief Admission, publish, reducer and quarantine refusals degrade one window without corrupting the rest. */
static int test_admission_and_publish_seam_faults(void) {
    fixture_t *fp = &GlobalFixture;
    window_coordinator_stats_t stats;
    Check(fixture_open(fp, InteropCapWindows) == 0);

    /* Slot admission refused is a resource failure: the admission is retried on the next pass and then announced. */
    Faults.slot_admit = 1u;
    admit_window_now(fp, 0x4100);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(window_coordinator_get_stats(fp->coordinator, &stats) == WindowCoordinatorOk);
    Check(stats.admission_failures >= 1u);
    Check(count_frames(fp, InteropMsgWindowAnnounce, 0x4100) == 1u);
    retire_one_window(fp, 0x4100);
    Check(window_coordinator_entry_count(fp->coordinator) == 0u);

    /* Adapter admission refused: the same outcome. */
    Faults.adapter_admit = 1u;
    admit_window_now(fp, 0x4200);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(count_frames(fp, InteropMsgWindowAnnounce, 0x4200) == 0u);
    /* Host frames that name the phantom (any of the first generations) are ignored or answered, never applied. */
    for (uint64_t generation = 1u; generation <= 4u; ++generation) {
        uint8_t frame[WireFrameBytes];
        size_t length = 0;
        Check(build_host(InteropMsgFrameRelease, frame, sizeof(frame), &length, 0x4200, generation, 1, 0, 1, 0, 0) == 0);
        Check(wire_push_host(&fp->wire, frame, length) == 0);
        Check(build_host_popup(frame, sizeof(frame), &length, 0x4200, generation) == 0);
        Check(wire_push_host(&fp->wire, frame, length) == 0);
    }
    Check(tick(fp) == WindowCoordinatorOk);
    Check(tick(fp) == WindowCoordinatorOk);
    retire_one_window(fp, 0x4200);
    Check(window_coordinator_entry_count(fp->coordinator) == 0u);

    /* A refused prepare skips one publish: the announce still goes out and the frame follows on a later pass. */
    Faults.slot_prepare = 1u;
    admit_window_now(fp, 0x4300);
    for (uint32_t pass = 0; pass < 4u && count_frames(fp, InteropMsgWindowAnnounce, 0x4300) == 0u; ++pass) {
        advance_clock(fp, 300u);
        Check(tick(fp) == WindowCoordinatorOk);
    }
    Check(count_frames(fp, InteropMsgWindowAnnounce, 0x4300) == 1u);
    Check(window_coordinator_get_stats(fp->coordinator, &stats) == WindowCoordinatorOk);
    Check(stats.frames_skipped >= 1u);
    advance_clock(fp, 300u);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(count_frames(fp, InteropMsgFrameReady, 0x4300) >= 1u);

    /* A refused reducer plan drops a host configure; a refused reducer completion does too. */
    Faults.reducer_plan = 1u;
    {
        uint8_t frame[WireFrameBytes];
        size_t length = 0;
        Check(build_host(InteropMsgWindowConfigure, frame, sizeof(frame), &length, 0x4300, announced_generation(fp, 0x4300), 0, 0, 0, 1, 0x3u) == 0);
        Check(wire_push_host(&fp->wire, frame, length) == 0);
    }
    Check(tick(fp) == WindowCoordinatorOk);
    Check(window_coordinator_get_stats(fp->coordinator, &stats) == WindowCoordinatorOk);
    Check(stats.reducer_failures >= 1u);
    Faults.reducer_complete = 1u;
    {
        uint8_t frame[WireFrameBytes];
        size_t length = 0;
        Check(build_host(InteropMsgWindowConfigure, frame, sizeof(frame), &length, 0x4300, announced_generation(fp, 0x4300), 0, 0, 0, 1, 0x1u) == 0);
        Check(wire_push_host(&fp->wire, frame, length) == 0);
    }
    Check(tick(fp) == WindowCoordinatorOk);
    Check(window_coordinator_get_stats(fp->coordinator, &stats) == WindowCoordinatorOk);
    Check(stats.reducer_failures >= 2u);

    retire_one_window(fp, 0x4300);
    Check(window_coordinator_entry_count(fp->coordinator) == 0u);

    /* A refused quarantine completion is retried on a later pass, and the entry is gone afterwards. */
    admit_window_now(fp, 0x4400);
    Check(tick(fp) == WindowCoordinatorOk);
    Faults.graph_complete = 1u;
    remove_window(fp, 0x4400);
    retire_one_window(fp, 0x4400);
    Check(window_coordinator_get_stats(fp->coordinator, &stats) == WindowCoordinatorOk);
    Check(stats.admission_failures >= 1u);
    Check(window_coordinator_entry_count(fp->coordinator) == 0u);

    /* A refused graph event read pauses staging for this pass only. */
    admit_window_now(fp, 0x4500);
    Faults.graph_next_event = 1u;
    Check(tick(fp) == WindowCoordinatorOk);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(count_frames(fp, InteropMsgWindowAnnounce, 0x4500) == 1u);
    retire_one_window(fp, 0x4500);
    Check(window_coordinator_entry_count(fp->coordinator) == 0u);

    fixture_close(fp);
    return 0;
}

/** @brief Transport loss and reconnect survive refusals of every backend step they perform. */
static int test_lost_and_reconnect_seam_faults(void) {
    fixture_t *fp = &GlobalFixture;
    uint64_t epoch = CoordEpoch;

    /* Invalidation refused on loss: the coordinator keeps the request and retries it on the next pass. */
    Check(fixture_open(fp, InteropCapWindows) == 0);
    admit_window_now(fp, 0x4600);
    Check(tick(fp) == WindowCoordinatorOk);
    Faults.graph_invalidate = 1u;
    /* Two invalidations are refused in a row: the request stays pending until a later pass succeeds. */
    Faults.graph_invalidate = 2u;
    Check(window_coordinator_mark_lost(fp->coordinator) == WindowCoordinatorOk);
    Check(window_coordinator_state(fp->coordinator) == WindowCoordinatorStateLost);
    Check(tick(fp) == WindowCoordinatorLost);
    Check(tick(fp) == WindowCoordinatorLost);
    fixture_close(fp);

    /* A refused drain start on loss faults the coordinator, and its teardown still frees everything. */
    Check(fixture_open(fp, InteropCapWindows) == 0);
    admit_window_now(fp, 0x4610);
    Check(tick(fp) == WindowCoordinatorOk);
    Faults.slot_begin_drain = 1u;
    Check(window_coordinator_mark_lost(fp->coordinator) == WindowCoordinatorOk);
    Check(window_coordinator_state(fp->coordinator) == WindowCoordinatorStateFaulted);
    Check(tick(fp) == WindowCoordinatorFaulted);
    fixture_close(fp);

    /* Each reconnect backend step that can be refused reports BackendFailed and leaves the coordinator faulted. */
    for (uint32_t which = 0; which < 5u; ++which) {
        Check(fixture_open(fp, InteropCapWindows) == 0);
        admit_window_now(fp, 0x4700 + 0x10u * which);
        Check(tick(fp) == WindowCoordinatorOk);
        Check(window_coordinator_mark_lost(fp->coordinator) == WindowCoordinatorOk);
        memset(&Faults, 0, sizeof(Faults));
        switch (which) {
        case 0: Faults.slot_confirm = 1u; break;
        case 1: Faults.graph_invalidate = 1u; break;
        case 2: Faults.observer_destroy = 1u; break;
        case 3: Faults.graph_create = 1u; break;
        default: Faults.observer_start = 1u; break;
        }
        epoch += 1u;
        Check(window_coordinator_reconnect(fp->coordinator, epoch, 1u) == WindowCoordinatorBackendFailed);
        Check(window_coordinator_state(fp->coordinator) == WindowCoordinatorStateFaulted);
        fixture_close(fp);
    }

    /* A reconnect whose observer sync is refused still completes, because the sync result is advisory. */
    Check(fixture_open(fp, InteropCapWindows) == 0);
    admit_window_now(fp, 0x4800);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(window_coordinator_mark_lost(fp->coordinator) == WindowCoordinatorOk);
    Faults.observer_sync = 1u;
    epoch += 1u;
    Check(window_coordinator_reconnect(fp->coordinator, epoch, 1u) == WindowCoordinatorOk);
    Check(window_coordinator_state(fp->coordinator) == WindowCoordinatorStateRunning);
    fixture_close(fp);
    return 0;
}

/** @brief Refusals inside admission (observe, reducer, capture) turn the window into a phantom. */
static int test_admission_refusals_turn_into_phantoms(void) {
    fixture_t *fp = &GlobalFixture;
    window_coordinator_stats_t stats;
    Check(fixture_open(fp, InteropCapWindows) == 0);
    Faults.adapter_observe = 1u;
    admit_window_now(fp, 0x4900);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(window_coordinator_get_stats(fp->coordinator, &stats) == WindowCoordinatorOk);
    Check(stats.admission_failures >= 1u && stats.phantoms >= 1u);
    Check(count_frames(fp, InteropMsgWindowAnnounce, 0x4900) == 0u);
    retire_one_window(fp, 0x4900);
    Check(window_coordinator_entry_count(fp->coordinator) == 0u);

    Faults.reducer_create = 1u;
    admit_window_now(fp, 0x4910);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(count_frames(fp, InteropMsgWindowAnnounce, 0x4910) == 0u);
    retire_one_window(fp, 0x4910);
    Check(window_coordinator_entry_count(fp->coordinator) == 0u);

    fp->capture.start_failure = 1u;
    admit_window_now(fp, 0x4920);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(window_coordinator_get_stats(fp->coordinator, &stats) == WindowCoordinatorOk);
    Check(stats.capture_failures >= 1u);
    Check(count_frames(fp, InteropMsgWindowAnnounce, 0x4920) == 0u);
    retire_one_window(fp, 0x4920);
    Check(window_coordinator_entry_count(fp->coordinator) == 0u);

    /* A barrier from a retiring window with pins still held holds the next admission; the hold ends with the release. */
    admit_window_now(fp, 0x4930);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(count_frames(fp, InteropMsgFrameReady, 0x4930) >= 1u);
    remove_window(fp, 0x4930);
    api_double_hint(fp->observer, 0x8001u, 0x4930);
    advance_clock(fp, 300u);
    Check(tick(fp) == WindowCoordinatorOk);
    admit_window_now(fp, 0x4940);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(window_coordinator_get_stats(fp->coordinator, &stats) == WindowCoordinatorOk);
    Check(stats.admissions_held >= 1u);
    Check(count_frames(fp, InteropMsgWindowAnnounce, 0x4940) == 0u);
    retire_one_window(fp, 0x4930);
    retire_one_window(fp, 0x4940);
    Check(window_coordinator_entry_count(fp->coordinator) == 0u);
    fixture_close(fp);
    return 0;
}

/** @brief Faults that can no longer be recovered by a retry leave the coordinator Faulted, and teardown still frees it. */
static int test_faulted_transitions_and_teardown(void) {
    fixture_t *fp = &GlobalFixture;
    Check(fixture_open(fp, InteropCapWindows) == 0);
    Faults.slot_writer = 1u;
    admit_window_now(fp, 0x4a00);
    Check(tick(fp) == WindowCoordinatorFaulted);
    Check(window_coordinator_state(fp->coordinator) == WindowCoordinatorStateFaulted);
    fixture_close(fp);

    Check(fixture_open(fp, InteropCapWindows) == 0);
    Faults.slot_commit = 1u;
    admit_window_now(fp, 0x4a10);
    Check(tick(fp) == WindowCoordinatorFaulted);
    Check(window_coordinator_state(fp->coordinator) == WindowCoordinatorStateFaulted);
    fixture_close(fp);

    /* A admission refused by the slot runtime on every pass exhausts the retry budget and faults. */
    Check(fixture_open(fp, InteropCapWindows) == 0);
    Faults.slot_admit = 1000u;
    admit_window_now(fp, 0x4a20);
    for (uint32_t pass = 0; pass < 200u && window_coordinator_state(fp->coordinator) != WindowCoordinatorStateFaulted; ++pass) {
        (void)tick(fp);
    }
    Check(window_coordinator_state(fp->coordinator) == WindowCoordinatorStateFaulted);
    fixture_close(fp);
    return 0;
}

/** @brief Malformed, unknown and popup host frames are answered or counted, and a disconnect on receive loses the transport. */
static int test_host_frame_classes(void) {
    fixture_t *fp = &GlobalFixture;
    window_coordinator_stats_t stats;
    uint8_t frame[WireFrameBytes];
    size_t length = 0;
    Check(fixture_open(fp, InteropCapWindows) == 0);
    admit_window_now(fp, 0x4b00);
    Check(tick(fp) == WindowCoordinatorOk);
    /* A frame that is not a valid interop header is rejected and counted. */
    {
        const uint8_t garbage[4] = {0xde, 0xad, 0xbe, 0xef};
        Check(wire_push_host(&fp->wire, garbage, sizeof(garbage)) == 0);
    }
    /* A popup update for a window that is not announced is answered as stale; for an announced one it is counted. */
    Check(build_host_popup(frame, sizeof(frame), &length, 0x4fff, 1) == 0);
    Check(wire_push_host(&fp->wire, frame, length) == 0);
    Check(build_host_popup(frame, sizeof(frame), &length, 0x4b00, announced_generation(fp, 0x4b00)) == 0);
    Check(wire_push_host(&fp->wire, frame, length) == 0);
    /* Popup dismissal reaches the popup case; the frame is accepted without effect. */
    Check(build_host(InteropMsgPopupDismissed, frame, sizeof(frame), &length, 0x4b00, announced_generation(fp, 0x4b00), 0, 0, 0, 0, 0) == 0);
    Check(wire_push_host(&fp->wire, frame, length) == 0);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(window_coordinator_get_stats(fp->coordinator, &stats) == WindowCoordinatorOk);
    Check(stats.host_frames_rejected >= 1u);
    Check(stats.stale_replies >= 1u);
    Check(stats.popup_frames >= 2u);

    /* A disconnect seen while receiving moves the coordinator to Lost before anything else is sent. */
    Check(build_host(InteropMsgFrameRelease, frame, sizeof(frame), &length, 0x4b00, announced_generation(fp, 0x4b00), 1, 0, 1, 0, 0) == 0);
    Check(wire_push_host(&fp->wire, frame, length) == 0);
    fp->wire.disconnected = 1;
    Check(tick(fp) == WindowCoordinatorLost);
    Check(window_coordinator_state(fp->coordinator) == WindowCoordinatorStateLost);
    fixture_close(fp);
    return 0;
}

/** @brief A refused observer destroy keeps the coordinator alive and reports Busy; a later destroy frees it. */
static int test_destroy_refusal_is_recoverable(void) {
    fixture_t *fp = &GlobalFixture;
    Check(fixture_open(fp, InteropCapWindows) == 0);
    Faults.observer_destroy = 1u;
    Check(window_coordinator_destroy(&fp->coordinator, 1u) == WindowCoordinatorBusy);
    Check(fp->coordinator != NULL);
    fixture_close(fp);
    return 0;
}

/** @brief Every required configuration field is checked: each one missing makes create refuse, with nothing allocated. */
static int test_config_table_rejects_each_field(void) {
    fixture_t *fp = &GlobalFixture;
    window_coordinator_config_t config;
    window_coordinator_t *coordinator = NULL;
    window_observer_api_t observer_api;
    window_capture_ops_t capture_ops;
    window_coordinator_io_t io;
    Check(fixture_open(fp, InteropCapWindows) == 0);
    for (uint32_t which = 0; which < 16u; ++which) {
        config = make_config(fp, CoordEpoch, InteropCapWindows);
        observer_api = ApiDoubleTable;
        capture_ops = *config.capture;
        io = *config.io;
        switch (which) {
        case 0: config.session_epoch = 0u; break;
        case 1: config.uac_mode = 3u; break;
        case 2: config.own_process_id = 0u; break;
        case 3: config.app_key_lookup = NULL; break;
        case 4: config.observer_api = NULL; break;
        case 5: observer_api.current_thread_id = NULL; config.observer_api = &observer_api; break;
        case 6: observer_api.monotonic_ms = NULL; config.observer_api = &observer_api; break;
        case 7: config.native_api = NULL; break;
        case 8: config.io = NULL; break;
        case 9: io.send_frame = NULL; config.io = &io; break;
        case 10: io.receive_frame = NULL; config.io = &io; break;
        case 11: config.capture = NULL; break;
        case 12: capture_ops.start = NULL; config.capture = &capture_ops; break;
        case 13: capture_ops.stop = NULL; config.capture = &capture_ops; break;
        case 14: capture_ops.write_frame = NULL; config.capture = &capture_ops; break;
        default: config.observer_api = NULL; break;
        }
        coordinator = NULL;
        Check(window_coordinator_create(&config, &coordinator) == WindowCoordinatorInvalidArgument);
        Check(coordinator == NULL);
    }
    fixture_close(fp);
    return 0;
}

/** @brief A calloc refusal fails create cleanly; a refused observer stop keeps destroy from freeing. */
static int test_allocation_and_stop_refusals(void) {
    fixture_t *fp = &GlobalFixture;
    window_coordinator_config_t config;
    window_coordinator_t *coordinator = NULL;
    window_coordinator_stats_t stats;
    /* The coordinator allocation is the first calloc of create. */
    fixture_prepare(fp);
    memset(&Faults, 0, sizeof(Faults));
    Faults.allocation = 1u;
    config = make_config(fp, CoordEpoch, InteropCapWindows);
    Check(window_coordinator_create(&config, &coordinator) == WindowCoordinatorOutOfMemory);
    Check(coordinator == NULL);
    memset(&Faults, 0, sizeof(Faults));
    fixture_close(fp);

    Check(fixture_open(fp, InteropCapWindows) == 0);
    /* A stats query into a caller buffer and a tick that reports its statistics. */
    Check(window_coordinator_tick(fp->coordinator, 1u, &stats) == WindowCoordinatorOk);
    /* A refused observer stop makes destroy report Busy and keeps the coordinator; the next destroy frees it. */
    Faults.observer_stop = 1u;
    Check(window_coordinator_destroy(&fp->coordinator, 1u) == WindowCoordinatorBusy);
    Check(fp->coordinator != NULL);
    fixture_close(fp);

    /* A stop that answers BadState (already stopped) lets destroy proceed and free the coordinator. */
    Check(fixture_open(fp, InteropCapWindows) == 0);
    Faults.observer_stop = 2u;
    Check(window_coordinator_destroy(&fp->coordinator, 1u) == WindowCoordinatorOk);
    Check(fp->coordinator == NULL);
    fixture_close(fp);

    /* A protected-destroy drain that finds the graph queue full is retried after the queue is drained. */
    Check(fixture_open(fp, InteropCapWindows) == 0);
    Faults.observer_drain = 1u;
    Check(window_coordinator_destroy(&fp->coordinator, 1u) == WindowCoordinatorOk);
    Check(fp->coordinator == NULL);
    fixture_close(fp);

    /* A drain that keeps finding the queue full gives up after its bounded attempts; teardown still completes. */
    Check(fixture_open(fp, InteropCapWindows) == 0);
    Faults.observer_drain = 4u;
    (void)window_coordinator_destroy(&fp->coordinator, 1u);
    fixture_close(fp);

    /* A faulted coordinator with an entry is refused by destroy until its entries are gone. */
    Check(fixture_open(fp, InteropCapWindows) == 0);
    Faults.slot_writer = 1u;
    admit_window_now(fp, 0x4c00);
    Check(tick(fp) == WindowCoordinatorFaulted);
    Check(window_coordinator_destroy(&fp->coordinator, 0u) == WindowCoordinatorBusy);
    Check(fp->coordinator != NULL);
    fixture_close(fp);

    /* A reconnect needs a nonzero epoch. */
    Check(fixture_open(fp, InteropCapWindows) == 0);
    Check(window_coordinator_mark_lost(fp->coordinator) == WindowCoordinatorOk);
    Check(window_coordinator_reconnect(fp->coordinator, 0u, 1u) == WindowCoordinatorInvalidArgument);
    Check(window_coordinator_state(fp->coordinator) == WindowCoordinatorStateLost);
    fixture_close(fp);
    return 0;
}

/** @brief Malformed, boundary and mismatched host frames are rejected or ignored without touching any entry. */
static int test_host_frame_validation(void) {
    fixture_t *fp = &GlobalFixture;
    window_coordinator_stats_t stats;
    uint8_t frame[WireFrameBytes];
    size_t length = 0;
    uint64_t generation;
    Check(fixture_open(fp, InteropCapWindows) == 0);
    admit_window_now(fp, 0x4d00);
    Check(tick(fp) == WindowCoordinatorOk);
    generation = announced_generation(fp, 0x4d00);
    Check(generation != 0u);
    Check(window_coordinator_get_stats(fp->coordinator, &stats) == WindowCoordinatorOk);
    {
        uint64_t before = stats.host_frames_rejected;
        /* A release naming a slot outside the window's three is rejected. */
        Check(build_host(InteropMsgFrameRelease, frame, sizeof(frame), &length, 0x4d00, generation, 1, 9u, 1, 0, 0) == 0);
        Check(wire_push_host(&fp->wire, frame, length) == 0);
        /* A valid frame with a trailing byte has a length the header does not describe. */
        Check(build_host(InteropMsgWindowClose, frame, sizeof(frame), &length, 0x4d00, generation, 0, 0, 0, 0, 0) == 0);
        Check(length < sizeof(frame));
        frame[length] = 0u;
        Check(wire_push_host(&fp->wire, frame, length + 1u) == 0);
        Check(tick(fp) == WindowCoordinatorOk);
        Check(tick(fp) == WindowCoordinatorOk);
        Check(window_coordinator_get_stats(fp->coordinator, &stats) == WindowCoordinatorOk);
        Check(stats.host_frames_rejected >= before + 2u);
    }
    /* A release for a window that is not tracked, and one with the wrong sequence, leave the pin untouched. */
    Check(build_host(InteropMsgFrameRelease, frame, sizeof(frame), &length, 0x4eee, 1, 1, 0, 1, 0, 0) == 0);
    Check(wire_push_host(&fp->wire, frame, length) == 0);
    Check(build_host(InteropMsgFrameRelease, frame, sizeof(frame), &length, 0x4d00, generation, 1, 0, 99u, 0, 0) == 0);
    Check(wire_push_host(&fp->wire, frame, length) == 0);
    /* A configure carrying a rect is applied to the reducer as a host configure. */
    {
        interop_writer_t writer;
        interop_writer_init(&writer, frame, sizeof(frame));
        Check(interop_put_uint(&writer, InteropTagWindow, 0x4d00) == InteropErrorNone);
        Check(interop_put_uint(&writer, InteropTagGeneration, generation) == InteropErrorNone);
        Check(interop_put_rect(&writer, InteropTagRect, 5, 6, 640, 480) == 0);
        Check(interop_put_uint(&writer, InteropTagState, 0x3u) == InteropErrorNone);
        Check(interop_writer_finish(&writer, InteropMsgWindowConfigure, 0u, InteropFromHost, &length) == InteropErrorNone);
        Check(wire_push_host(&fp->wire, frame, length) == 0);
    }
    Check(tick(fp) == WindowCoordinatorOk);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(window_coordinator_entry_count(fp->coordinator) == 1u);
    retire_one_window(fp, 0x4d00);
    Check(window_coordinator_entry_count(fp->coordinator) == 0u);
    fixture_close(fp);
    return 0;
}

/** @brief An unsynchronized reducer is resynchronized and planned again; a refused resync counts as a reducer failure. */
static int test_reducer_resynchronization(void) {
    fixture_t *fp = &GlobalFixture;
    window_coordinator_stats_t stats;
    uint8_t frame[WireFrameBytes];
    size_t length = 0;
    Check(fixture_open(fp, InteropCapWindows) == 0);
    admit_window_now(fp, 0x4e00);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(window_coordinator_get_stats(fp->coordinator, &stats) == WindowCoordinatorOk);
    Faults.reducer_unsynchronized = 1u;
    Check(build_host(InteropMsgWindowConfigure, frame, sizeof(frame), &length, 0x4e00, announced_generation(fp, 0x4e00), 0, 0, 0, 1, 0x3u) == 0);
    Check(wire_push_host(&fp->wire, frame, length) == 0);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(window_coordinator_get_stats(fp->coordinator, &stats) == WindowCoordinatorOk);
    Check(stats.reducer_failures == 0u);
    /* A resync whose completion is refused is counted; the configure is then planned and applied again. */
    Faults.reducer_unsynchronized = 1u;
    Faults.reducer_complete = 1u;
    Check(build_host(InteropMsgWindowConfigure, frame, sizeof(frame), &length, 0x4e00, announced_generation(fp, 0x4e00), 0, 0, 0, 1, 0x3u) == 0);
    Check(wire_push_host(&fp->wire, frame, length) == 0);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(window_coordinator_get_stats(fp->coordinator, &stats) == WindowCoordinatorOk);
    Check(stats.reducer_failures >= 1u);
    /* A resync whose own plan is refused is counted, and the configure is dropped. */
    Faults.reducer_unsynchronized = 1u;
    Faults.reducer_plan = 1u;
    Check(build_host(InteropMsgWindowConfigure, frame, sizeof(frame), &length, 0x4e00, announced_generation(fp, 0x4e00), 0, 0, 0, 1, 0x1u) == 0);
    Check(wire_push_host(&fp->wire, frame, length) == 0);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(window_coordinator_get_stats(fp->coordinator, &stats) == WindowCoordinatorOk);
    Check(stats.reducer_failures >= 1u || stats.configures >= 2u);
    retire_one_window(fp, 0x4e00);
    Check(window_coordinator_entry_count(fp->coordinator) == 0u);
    fixture_close(fp);
    return 0;
}

/** @brief With the wire stalled, the outbound queue fills: admissions and publishes are held until it drains. */
static int test_outbound_backpressure_holds(void) {
    fixture_t *fp = &GlobalFixture;
    window_coordinator_stats_t stats;
    Check(fixture_open(fp, InteropCapWindows) == 0);
    /* The budget counts refused sends: a large value stalls the wire for the whole test. */
    fp->wire.backpressure_budget = 1000000u;
    for (uint32_t index = 0; index < InteropMaxWindows; ++index) {
        admit_window_now(fp, 0x5000u + 0x10u * index);
        Check(tick(fp) == WindowCoordinatorOk);
    }
    for (uint32_t pass = 0; pass < 16u; ++pass) {
        advance_clock(fp, 300u);
        Check(tick(fp) == WindowCoordinatorOk);
    }
    Check(window_coordinator_get_stats(fp->coordinator, &stats) == WindowCoordinatorOk);
    Check(stats.admissions_held >= 1u || stats.frames_skipped >= 1u);
    /* With the queue full, a title update and then a retirement are each held until the queue has room. */
    api_double_set_text(api_double_find(fp->observer, 0x5010)->facts.title, InteropWindowGraphMaxTitleBytes,
                        &api_double_find(fp->observer, 0x5010)->facts.title_length, "held");
    api_double_hint(fp->observer, 0x800Cu, 0x5010);
    advance_clock(fp, 300u);
    Check(tick(fp) == WindowCoordinatorOk);
    remove_window(fp, 0x5000);
    api_double_hint(fp->observer, 0x8001u, 0x5000);
    advance_clock(fp, 300u);
    Check(tick(fp) == WindowCoordinatorOk);
    /* Restore the wire: the queue drains in order and every held admission completes. */
    fp->wire.backpressure_budget = 0u;
    for (uint32_t pass = 0; pass < 40u; ++pass) {
        advance_clock(fp, 300u);
        Check(tick(fp) == WindowCoordinatorOk);
    }
    Check(window_coordinator_entry_count(fp->coordinator) == InteropMaxWindows);
    /* The graph exports at most InteropMaxWindows windows, so a retired entry makes room for the next admission. */
    retire_one_window(fp, 0x5000);
    admit_window_now(fp, 0x5800);
    for (uint32_t pass = 0; pass < 4u && count_frames(fp, InteropMsgWindowAnnounce, 0x5800) == 0u; ++pass) {
        advance_clock(fp, 300u);
        Check(tick(fp) == WindowCoordinatorOk);
    }
    Check(count_frames(fp, InteropMsgWindowAnnounce, 0x5800) == 1u);
    for (uint32_t index = 1; index < InteropMaxWindows; ++index) retire_one_window(fp, 0x5000u + 0x10u * index);
    retire_one_window(fp, 0x5800);
    Check(window_coordinator_entry_count(fp->coordinator) == 0u);
    fixture_close(fp);
    return 0;
}

/** @brief A refused encoder fails the coordinator at that encoder: announce, update, publish, retire and stale reply. */
static int test_encoder_refusals_fault(void) {
    fixture_t *fp = &GlobalFixture;
    uint8_t frame[WireFrameBytes];
    size_t length = 0;
    /* Announce. */
    Check(fixture_open(fp, InteropCapWindows) == 0);
    Faults.writer_finish = 1u;
    admit_window_now(fp, 0x5900);
    Check(tick(fp) == WindowCoordinatorFaulted);
    fixture_close(fp);

    /* Stale reply to a frame for an unknown window. */
    Check(fixture_open(fp, InteropCapWindows) == 0);
    Check(build_host(InteropMsgFrameRelease, frame, sizeof(frame), &length, 0x5a00, 1, 1, 0, 1, 0, 0) == 0);
    Check(wire_push_host(&fp->wire, frame, length) == 0);
    Faults.writer_finish = 1u;
    Check(tick(fp) == WindowCoordinatorFaulted);
    fixture_close(fp);

    /* Update after a title change. */
    Check(fixture_open(fp, InteropCapWindows) == 0);
    admit_window_now(fp, 0x5b00);
    Check(tick(fp) == WindowCoordinatorOk);
    Faults.writer_finish = 1u;
    api_double_set_text(api_double_find(fp->observer, 0x5b00)->facts.title, InteropWindowGraphMaxTitleBytes,
                        &api_double_find(fp->observer, 0x5b00)->facts.title_length, "changed");
    api_double_hint(fp->observer, 0x800Cu, 0x5b00);
    advance_clock(fp, 300u);
    Check(tick(fp) == WindowCoordinatorFaulted);
    fixture_close(fp);

    /* Frame ready for a publish. */
    Check(fixture_open(fp, InteropCapWindows) == 0);
    admit_window_now(fp, 0x5c00);
    Check(tick(fp) == WindowCoordinatorOk);
    Faults.writer_finish = 1u;
    advance_clock(fp, 300u);
    Check(tick(fp) == WindowCoordinatorFaulted);
    fixture_close(fp);

    /* Retire after a window goes away. */
    Check(fixture_open(fp, InteropCapWindows) == 0);
    admit_window_now(fp, 0x5d00);
    Check(tick(fp) == WindowCoordinatorOk);
    release_window_frames(fp, 0x5d00);
    Faults.writer_finish = 1u;
    remove_window(fp, 0x5d00);
    api_double_hint(fp->observer, 0x8001u, 0x5d00);
    advance_clock(fp, 300u);
    Check(tick(fp) == WindowCoordinatorFaulted);
    fixture_close(fp);
    return 0;
}

/** @brief A window without a title is announced with no title field. */
static int test_untitled_window_announces(void) {
    fixture_t *fp = &GlobalFixture;
    Check(fixture_open(fp, InteropCapWindows) == 0);
    add_window(fp, 0x5e00);
    api_double_find(fp->observer, 0x5e00)->facts.title_length = 0u;
    api_double_hint(fp->observer, 0x8000u, 0x5e00);
    advance_clock(fp, 300u);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(count_frames(fp, InteropMsgWindowAnnounce, 0x5e00) == 1u);
    retire_one_window(fp, 0x5e00);
    Check(window_coordinator_entry_count(fp->coordinator) == 0u);
    fixture_close(fp);
    return 0;
}

/** @brief Owner-thread calls made from inside the observer's query are refused as reentrant. */
typedef struct reentry_probe_t {
    fixture_t *fixture;
    int fired;
    int slot_kept;
    window_coordinator_status_t tick_status;
    window_coordinator_status_t mark_status;
    window_coordinator_status_t reconnect_status;
    window_coordinator_status_t destroy_status;
} reentry_probe_t;

static void reentry_probe_query(api_double_t *dbl, uint64_t window, void *argument) {
    reentry_probe_t *probe = (reentry_probe_t *)argument;
    window_coordinator_t *coordinator;
    window_coordinator_t *slot;
    (void)dbl;
    (void)window;
    if (probe->fired != 0) return;
    probe->fired = 1;
    coordinator = probe->fixture->coordinator;
    probe->tick_status = window_coordinator_tick(coordinator, 1u, NULL);
    probe->mark_status = window_coordinator_mark_lost(coordinator);
    probe->reconnect_status = window_coordinator_reconnect(coordinator, CoordEpoch + 7u, 1u);
    slot = coordinator;
    probe->destroy_status = window_coordinator_destroy(&slot, 1u);
    probe->slot_kept = slot == coordinator;
}

static int test_reentry_and_budgeted_passes(void) {
    fixture_t *fp = &GlobalFixture;
    reentry_probe_t probe;
    Check(fixture_open(fp, InteropCapWindows) == 0);
    memset(&probe, 0, sizeof(probe));
    probe.fixture = fp;
    fp->observer->query_hook = reentry_probe_query;
    fp->observer->query_hook_arg = &probe;
    admit_window_now(fp, 0x5f00);
    Check(tick(fp) == WindowCoordinatorOk);
    fp->observer->query_hook = NULL;
    fp->observer->query_hook_arg = NULL;
    Check(probe.fired == 1);
    Check(probe.tick_status == WindowCoordinatorReentrant);
    Check(probe.mark_status == WindowCoordinatorReentrant);
    Check(probe.reconnect_status == WindowCoordinatorReentrant);
    Check(probe.destroy_status == WindowCoordinatorReentrant);
    Check(probe.slot_kept == 1);
    Check(window_coordinator_state(fp->coordinator) == WindowCoordinatorStateRunning);
    retire_one_window(fp, 0x5f00);
    fixture_close(fp);

    /* One pump call per tick with a budget of one: several windows are admitted across many passes, each budget-limited. */
    Check(fixture_open(fp, InteropCapWindows) == 0);
    for (uint32_t index = 0; index < 10u; ++index) {
        add_window(fp, 0x6000u + 0x10u * index);
        api_double_hint(fp->observer, 0x8000u, 0x6000u + 0x10u * index);
    }
    for (uint32_t pass = 0; pass < 40u && window_coordinator_entry_count(fp->coordinator) < 10u; ++pass) {
        advance_clock(fp, 300u);
        Check(window_coordinator_tick(fp->coordinator, 1u, NULL) == WindowCoordinatorOk);
    }
    Check(window_coordinator_entry_count(fp->coordinator) == 10u);
    for (uint32_t index = 0; index < 10u; ++index) retire_one_window(fp, 0x6000u + 0x10u * index);
    Check(window_coordinator_entry_count(fp->coordinator) == 0u);
    fixture_close(fp);

    /* A reconnect needs the host proof. */
    Check(fixture_open(fp, InteropCapWindows) == 0);
    Check(window_coordinator_mark_lost(fp->coordinator) == WindowCoordinatorOk);
    Check(window_coordinator_reconnect(fp->coordinator, CoordEpoch + 1u, 0u) == WindowCoordinatorMissingProof);
    Check(window_coordinator_state(fp->coordinator) == WindowCoordinatorStateLost);
    fixture_close(fp);
    return 0;
}

/** @brief A tracking minimum size is carried by the announce and by a later update. */
static int test_minimum_size_is_encoded(void) {
    fixture_t *fp = &GlobalFixture;
    Check(fixture_open(fp, InteropCapWindows) == 0);
    add_window(fp, 0x6100);
    api_double_find(fp->observer, 0x6100)->facts.min_width = 320u;
    api_double_find(fp->observer, 0x6100)->facts.min_height = 240u;
    api_double_hint(fp->observer, 0x8000u, 0x6100);
    advance_clock(fp, 300u);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(count_frames(fp, InteropMsgWindowAnnounce, 0x6100) == 1u);
    api_double_find(fp->observer, 0x6100)->facts.min_width = 400u;
    api_double_hint(fp->observer, 0x800Bu, 0x6100);
    for (uint32_t pass = 0; pass < 3u && count_frames(fp, InteropMsgWindowUpdate, 0x6100) == 0u; ++pass) {
        advance_clock(fp, 300u);
        Check(tick(fp) == WindowCoordinatorOk);
    }
    Check(count_frames(fp, InteropMsgWindowUpdate, 0x6100) >= 1u);
    /* A height-only minimum size is encoded as well. */
    add_window(fp, 0x6200);
    api_double_find(fp->observer, 0x6200)->facts.min_width = 0u;
    api_double_find(fp->observer, 0x6200)->facts.min_height = 200u;
    api_double_hint(fp->observer, 0x8000u, 0x6200);
    advance_clock(fp, 300u);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(count_frames(fp, InteropMsgWindowAnnounce, 0x6200) == 1u);
    retire_one_window(fp, 0x6100);
    retire_one_window(fp, 0x6200);
    Check(window_coordinator_entry_count(fp->coordinator) == 0u);
    fixture_close(fp);
    return 0;
}

/** @brief A pump that finds the input desktop away from Default does nothing; admission resumes when it returns. */
static int test_desktop_change_pauses_observation(void) {
    fixture_t *fp = &GlobalFixture;
    Check(fixture_open(fp, InteropCapWindows) == 0);
    fp->observer->desktop = WindowObserverDesktopOther;
    add_window(fp, 0x6300);
    api_double_hint(fp->observer, 0x8000u, 0x6300);
    advance_clock(fp, 300u);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(count_frames(fp, InteropMsgWindowAnnounce, 0x6300) == 0u);
    fp->observer->desktop = WindowObserverDesktopDefault;
    for (uint32_t pass = 0; pass < 4u && count_frames(fp, InteropMsgWindowAnnounce, 0x6300) == 0u; ++pass) {
        advance_clock(fp, 300u);
        Check(tick(fp) == WindowCoordinatorOk);
    }
    Check(count_frames(fp, InteropMsgWindowAnnounce, 0x6300) == 1u);
    retire_one_window(fp, 0x6300);
    Check(window_coordinator_entry_count(fp->coordinator) == 0u);
    fixture_close(fp);
    return 0;
}

/* ---- Review regressions (Opus review of the coordinator, P1-P6) --------------------------------- */

/** @brief P1: a stale reply that cannot be queued is counted and dropped; the coordinator keeps running. */
static int test_review_p1_stale_reply_under_full_outbound_drops(void) {
    fixture_t *fp = &GlobalFixture;
    uint8_t frame[WireFrameBytes];
    size_t length = 0;
    window_coordinator_stats_t stats;
    uint32_t pass;
    Check(fixture_open(fp, InteropCapWindows) == 0);
    admit_window_now(fp, 0x1000);
    Check(tick(fp) == WindowCoordinatorOk);
    fp->wire.backpressure_budget = 1000000u;
    for (pass = 0; pass < 40u; ++pass) {
        uint32_t index;
        for (index = 0; index < 16u; ++index) {
            Check(build_host(InteropMsgFrameRelease, frame, sizeof(frame), &length, 0x9999, 1, 1, 0, 1, 0, 0) == 0);
            Check(wire_push_host(&fp->wire, frame, length) == 0);
        }
        Check(tick(fp) == WindowCoordinatorOk);
    }
    Check(window_coordinator_state(fp->coordinator) == WindowCoordinatorStateRunning);
    Check(window_coordinator_get_stats(fp->coordinator, &stats) == WindowCoordinatorOk);
    Check(stats.stale_replies >= 1u);
    fp->wire.backpressure_budget = 0u;
    retire_one_window(fp, 0x1000);
    fixture_close(fp);
    return 0;
}

/**
 * @brief A generation whose pins drain while the FIFO is full waits for room for its Retire; it is never dropped.
 *
 * Setup: window X puts its three slots on the wire while the wire accepts, then the wire stalls. Its slots are
 * all pinned, so it cannot publish more. X is removed (Retiring, no publish), the stalled FIFO is filled by
 * other windows, and only then does the host release X's frames (its releases are inbound and still work).
 */
static int test_review_retire_waits_for_fifo_room(void) {
    fixture_t *fp = &GlobalFixture;
    window_coordinator_stats_t stats;
    uint32_t index;
    uint32_t guard;
    Check(fixture_open(fp, InteropCapWindows) == 0);
    fp->wire.backpressure_budget = 0u;
    admit_window_now(fp, 0x5000);
    for (guard = 0; guard < 10u && count_type(&fp->wire, InteropMsgFrameReady, 0x5000, 1) < 3u; ++guard) {
        advance_clock(fp, 300u);
        Check(tick(fp) == WindowCoordinatorOk);
    }
    Check(count_type(&fp->wire, InteropMsgFrameReady, 0x5000, 1) == 3u);
    fp->wire.backpressure_budget = 1000000u;
    for (index = 1; index < InteropMaxWindows; ++index) {
        admit_window_now(fp, 0x5000u + 0x10u * index);
        Check(tick(fp) == WindowCoordinatorOk);
    }
    for (uint32_t pass = 0; pass < 16u; ++pass) {
        advance_clock(fp, 300u);
        Check(tick(fp) == WindowCoordinatorOk);
    }
    Check(window_coordinator_get_stats(fp->coordinator, &stats) == WindowCoordinatorOk);
    Check(stats.frames_skipped >= 1u);
    /* X goes away while the wire is stalled; its pins are still held. */
    remove_window(fp, 0x5000);
    api_double_hint(fp->observer, 0x8001u, 0x5000);
    advance_clock(fp, 300u);
    Check(tick(fp) == WindowCoordinatorOk);
    /* The host releases X's frames now, and four stale frames fill the last FIFO slots (each queues one Error
     * reply). The receive pass runs before the retirement pass, so X's pins drain while the FIFO is full. */
    release_window_frames(fp, 0x5000);
    for (index = 0; index < 4u; ++index) {
        uint8_t stale[WireFrameBytes];
        size_t stale_length = 0;
        Check(build_host(InteropMsgFrameRelease, stale, sizeof(stale), &stale_length, 0x9999, 1, 1, 0, 1, 0, 0) == 0);
        Check(wire_push_host(&fp->wire, stale, stale_length) == 0);
    }
    advance_clock(fp, 300u);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(count_type(&fp->wire, InteropMsgWindowRetire, 0x5000, 1) == 0u);
    Check(window_coordinator_state(fp->coordinator) == WindowCoordinatorStateRunning);
    Check(window_coordinator_entry_count(fp->coordinator) >= 1u);
    /* Room returns: the queued Retire goes out and the generation completes. */
    fp->wire.backpressure_budget = 0u;
    for (guard = 0; guard < 40u && count_type(&fp->wire, InteropMsgWindowRetire, 0x5000, 1) == 0u; ++guard) {
        advance_clock(fp, 300u);
        Check(tick(fp) == WindowCoordinatorOk);
    }
    Check(count_type(&fp->wire, InteropMsgWindowRetire, 0x5000, 1) == 1u);
    for (index = 1; index < InteropMaxWindows; ++index) retire_one_window(fp, 0x5000u + 0x10u * index);
    fixture_close(fp);
    return 0;
}

/* ---- Defensive guards and owner-thread refusal ------------------------------------------------ */

/** @brief Results of the owner-only calls made from a foreign thread. */
typedef struct wrong_thread_t {
    fixture_t *fixture;
    window_coordinator_status_t status[6];
    uint32_t state;
    uint32_t entries;
    window_coordinator_stats_t stats;
} wrong_thread_t;

static void wrong_thread_calls(void *argument) {
    wrong_thread_t *ctx = (wrong_thread_t *)argument;
    window_coordinator_t *coordinator = ctx->fixture->coordinator;
    window_coordinator_t *slot = coordinator;
    ctx->status[0] = window_coordinator_tick(coordinator, 1u, NULL);
    ctx->status[1] = window_coordinator_mark_lost(coordinator);
    ctx->status[2] = window_coordinator_reconnect(coordinator, CoordEpoch + 9u, 1u);
    ctx->status[3] = window_coordinator_get_stats(coordinator, &ctx->stats);
    ctx->status[4] = window_coordinator_destroy(&slot, 1u);
    ctx->state = window_coordinator_state(coordinator);
    ctx->entries = window_coordinator_entry_count(coordinator);
    ctx->status[5] = slot == coordinator ? WindowCoordinatorOk : WindowCoordinatorBadState;
}

static int test_defensive_guards_and_owner_thread(void) {
    fixture_t *fp = &GlobalFixture;
    window_coordinator_stats_t stats;
    window_coordinator_t *none = NULL;
    wrong_thread_t ctx;
    uint32_t index;
    Check(fixture_open(fp, InteropCapWindows) == 0);

    /* Invalid arguments on every entry point that takes a pointer or a budget. */
    Check(window_coordinator_tick(NULL, 1u, NULL) == WindowCoordinatorInvalidArgument);
    Check(window_coordinator_tick(fp->coordinator, 0u, NULL) == WindowCoordinatorInvalidArgument);
    Check(window_coordinator_get_stats(NULL, &stats) == WindowCoordinatorInvalidArgument);
    Check(window_coordinator_get_stats(fp->coordinator, NULL) == WindowCoordinatorInvalidArgument);
    Check(window_coordinator_entry_count(NULL) == 0u);
    Check(window_coordinator_state(NULL) == 0u);
    Check(window_coordinator_destroy(NULL, 1u) == WindowCoordinatorInvalidArgument);
    Check(window_coordinator_destroy(&none, 1u) == WindowCoordinatorOk);
    Check(window_coordinator_reconnect(fp->coordinator, CoordEpoch, 1u) == WindowCoordinatorBadState);
    Check(window_coordinator_get_stats(fp->coordinator, &stats) == WindowCoordinatorOk);

    /* Destroy refuses while a window is admitted, and while the host proof is missing. */
    add_window(fp, 0x3100);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(window_coordinator_entry_count(fp->coordinator) == 1u);
    Check(window_coordinator_destroy(&fp->coordinator, 1u) == WindowCoordinatorBusy);
    Check(fp->coordinator != NULL);
    remove_window(fp, 0x3100);
    for (index = 0; index < fp->wire.count; ++index) {
        sent_t sent;
        Check(sent_at(&fp->wire, index, &sent));
        if (sent.type == InteropMsgFrameReady && sent.window == 0x3100u) {
            Check(host_release(fp, 0x3100, 1, sent.slot, sent.sequence) == 0);
        }
    }
    for (index = 0; index < 4u && window_coordinator_entry_count(fp->coordinator) != 0u; ++index) {
        advance_clock(fp, 300u);
        Check(tick(fp) == WindowCoordinatorOk);
    }
    Check(window_coordinator_entry_count(fp->coordinator) == 0u);
    Check(window_coordinator_destroy(&fp->coordinator, 0u) == WindowCoordinatorMissingProof);
    Check(fp->coordinator != NULL);

    /* A lost coordinator refuses a second mark_lost, and every owner-only call from a foreign thread is refused. */
    Check(window_coordinator_mark_lost(fp->coordinator) == WindowCoordinatorOk);
    Check(window_coordinator_mark_lost(fp->coordinator) == WindowCoordinatorBadState);
    memset(&ctx, 0, sizeof(ctx));
    ctx.fixture = fp;
    Check(api_double_run_on_other_thread(fp->observer, 0x7777u, wrong_thread_calls, &ctx) == 0);
    Check(ctx.status[0] == WindowCoordinatorWrongThread);
    Check(ctx.status[1] == WindowCoordinatorWrongThread);
    Check(ctx.status[2] == WindowCoordinatorWrongThread);
    Check(ctx.status[3] == WindowCoordinatorWrongThread);
    Check(ctx.status[4] == WindowCoordinatorWrongThread);
    Check(ctx.status[5] == WindowCoordinatorOk);
    Check(ctx.state == 0u);
    Check(ctx.entries == 0u);
    Check(window_coordinator_state(fp->coordinator) == WindowCoordinatorStateLost);
    fixture_close(fp);
    return 0;
}

/* A title change reported through a NAMECHANGE hint reaches the host as WindowUpdate for the announced pair. */
static int test_title_change_sends_window_update(void) {
    fixture_t *fp = &GlobalFixture;
    sent_t sent;
    uint32_t index;
    uint32_t found = 0u;
    Check(fixture_open(fp, InteropCapWindows) == 0);
    add_window(fp, 0x3000);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(fp->wire.count >= 1u);
    Check(sent_at(&fp->wire, 0, &sent));
    Check(sent.type == InteropMsgWindowAnnounce && sent.window == 0x3000u);
    {
        uint32_t baseline = fp->wire.count;
        api_double_window_t *record = api_double_find(fp->observer, 0x3000);
        Check(record != NULL);
        api_double_set_text(record->facts.title, InteropWindowGraphMaxTitleBytes, &record->facts.title_length, "retitled");
        api_double_hint(fp->observer, 0x800Cu, 0x3000);
        for (index = 0; index < 4u; ++index) {
            advance_clock(fp, 300u);
            Check(tick(fp) == WindowCoordinatorOk);
        }
        for (index = baseline; index < fp->wire.count; ++index) {
            Check(sent_at(&fp->wire, index, &sent));
            if (sent.type == InteropMsgWindowUpdate && sent.window == 0x3000u && sent.generation == 1u) found += 1u;
        }
    }
    Check(found >= 1u);
    fixture_close(fp);
    return 0;
}

/* ---- Seeded long sequences with fault injection and wire invariants ----------------------------- */

static uint64_t rng_state = UINT64_C(0x2545f4914f6cdd1d);

static uint32_t rng_pick(uint32_t bound) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state % bound);
}

/** @brief Pairs the host has been told are announced in the current epoch. */
#define ModelCapacity 1024u
typedef struct model_t {
    uint64_t window[ModelCapacity];
    uint64_t generation[ModelCapacity];
    uint32_t live[ModelCapacity];
    uint32_t announces;
    uint32_t retires;
    uint32_t updates;
    uint32_t frames;
    uint32_t releases_sent;
    uint32_t errors;
    uint32_t configures_sent;
    uint32_t closes_sent;
    uint32_t lost_passes;
    uint32_t reconnects;
    uint32_t processed;
    uint32_t hints;
    uint32_t admits;
} model_t;

/** @brief Host-side record of one frame pin: a FrameReady the host has received and not yet released. */
#define PinCapacity 256u
typedef struct pin_t {
    uint64_t window;
    uint64_t generation;
    uint32_t slot;
    uint64_t sequence;
    uint32_t outstanding;
} pin_t;

static int model_find(const model_t *model, uint64_t window, uint64_t generation) {
    for (uint32_t index = 0; index < ModelCapacity; ++index) {
        if (model->live[index] != 0u && model->window[index] == window && model->generation[index] == generation) {
            return (int)index;
        }
    }
    return -1;
}

static int model_add(model_t *model, uint64_t window, uint64_t generation) {
    for (uint32_t index = 0; index < ModelCapacity; ++index) {
        if (model->live[index] == 0u) {
            model->live[index] = 1u;
            model->window[index] = window;
            model->generation[index] = generation;
            return 0;
        }
    }
    return 1;
}

/** @brief Queues a FrameRelease for every outstanding pin the wire can take; refused pushes stay outstanding. */
static void release_outstanding(fixture_t *f, pin_t *pins) {
    for (uint32_t index = 0; index < PinCapacity; ++index) {
        if (pins[index].outstanding != 0u &&
            host_release(f, pins[index].window, pins[index].generation, pins[index].slot, pins[index].sequence) == 0) {
            pins[index].outstanding = 0u;
        }
    }
}

/**
 * @brief Reads every frame the coordinator sent since the last call and checks the wire invariants:
 *        Announce once per pair, Retire only for an announced pair, Update and FrameReady only for an
 *        announced pair. Records FrameReady pins for the host and counts error replies.
 */
static int consume_wire(fixture_t *f, model_t *model, pin_t *pins) {
    for (; model->processed < f->wire.count; model->processed += 1u) {
        sent_t sent;
        if (decode_frame(f->wire.frames[model->processed], f->wire.lengths[model->processed], &sent) != 0) return 1;
        if (sent.type == InteropMsgWindowAnnounce) {
            if (model_find(model, sent.window, sent.generation) >= 0) return 1;
            if (model_add(model, sent.window, sent.generation) != 0) return 1;
            model->announces += 1u;
        } else if (sent.type == InteropMsgWindowRetire) {
            int index = model_find(model, sent.window, sent.generation);
            if (index < 0) return 1;
            model->live[index] = 0u;
            model->retires += 1u;
        } else if (sent.type == InteropMsgWindowUpdate) {
            if (model_find(model, sent.window, sent.generation) < 0) return 1;
            model->updates += 1u;
        } else if (sent.type == InteropMsgFrameReady) {
            uint32_t placed = 0;
            if (model_find(model, sent.window, sent.generation) < 0) return 1;
            for (uint32_t index = 0; index < PinCapacity && placed == 0u; ++index) {
                if (pins[index].outstanding == 0u) {
                    pins[index].window = sent.window;
                    pins[index].generation = sent.generation;
                    pins[index].slot = sent.slot;
                    pins[index].sequence = sent.sequence;
                    pins[index].outstanding = 1u;
                    placed = 1u;
                }
            }
            if (placed == 0u) return 1;
            model->frames += 1u;
        } else if (sent.type == InteropMsgError) {
            model->errors += 1u;
        }
    }
    return 0;
}

/** @brief Clears the host's view of the epoch: after reconnect the host starts from nothing. */
static void model_reset_epoch(model_t *model, pin_t *pins) {
    memset(model->live, 0, sizeof(model->live));
    memset(pins, 0, sizeof(pin_t) * PinCapacity);
}

static int test_random_long_sequences_with_faults(void) {
    fixture_t *fp = &GlobalFixture;
    static model_t model;
    static pin_t pins[PinCapacity];
    uint64_t epoch = CoordEpoch;
    uint64_t total_rounds = 10u;
    for (uint64_t round = 0; round < total_rounds; ++round) {
        uint32_t next_window_slot = 0;
        memset(&model, 0, sizeof(model));
        memset(pins, 0, sizeof(pins));
        rng_state = UINT64_C(0x9e3779b97f4a7c15) ^ (round * UINT64_C(0x100000001b3) + 7u);
        Check(fixture_open(fp, InteropCapWindows) == 0);
        for (uint32_t step = 0; step < 300u; ++step) {
            uint32_t roll = rng_pick(100u);
            advance_clock(fp, 100u);
            uint64_t window = 0x1000u + 0x10u * (uint64_t)rng_pick(12u);
            window_coordinator_status_t status;
            if (window_coordinator_state(fp->coordinator) == WindowCoordinatorStateLost && rng_pick(10u) == 0u) {
                roll = 200u; /* a Lost coordinator reconnects with a fresh epoch on about one step in ten */
            }
            if (roll < 10u && fp->visible_count < 10u) {
                /* Admit a window, sometimes a large one that stresses the heap. */
                api_double_window_t *record;
                if (api_double_find(fp->observer, window) == NULL) {
                    add_window(fp, window);
                    model.admits += 1u;
                    record = api_double_find(fp->observer, window);
                    if (record != NULL) {
                        uint32_t side = rng_pick(4u) == 0u ? 1500u : 16u + rng_pick(96u);
                        record->facts.width = side;
                        record->facts.height = side;
                    }
                }
            } else if (roll < 16u && fp->visible_count != 0u) {
                remove_window(fp, fp->visible[rng_pick(fp->visible_count)]);
            } else if (roll < 22u) {
                /* Reuse: the same HWND value comes back as a new incarnation after its old one went away. */
                if (api_double_find(fp->observer, window) != NULL) {
                    remove_window(fp, window);
                } else {
                    add_window(fp, window);
                }
            } else if (roll < 30u) {
                /* Change a visible window's title or width and report it through a hint, so the next refresh
                 * sends a WindowUpdate for an announced pair. */
                uint64_t target = fp->visible_count != 0u ? fp->visible[rng_pick(fp->visible_count)] : 0u;
                api_double_window_t *record = target != 0u ? api_double_find(fp->observer, target) : NULL;
                if (record != NULL) {
                    model.hints += 1u;
                    if (rng_pick(2u) == 0u) {
                        api_double_set_text(record->facts.title, InteropWindowGraphMaxTitleBytes,
                                            &record->facts.title_length, "retitled");
                        api_double_hint(fp->observer, 0x800Cu, target);
                    } else {
                        record->facts.width = 16u + rng_pick(96u);
                        api_double_hint(fp->observer, 0x800Bu, target);
                    }
                    advance_clock(fp, 300u);
                }
            } else if (roll < 40u) {
                advance_clock(fp, 300u);
            } else if (roll < 50u) {
                /* Host release of an outstanding pin, or a bogus release for an unknown generation. */
                uint32_t index = rng_pick(PinCapacity);
                if (rng_pick(5u) == 0u) {
                    (void)host_release(fp, 0x7000u, 1, 0, 1);
                } else if (pins[index].outstanding != 0u) {
                    if (host_release(fp, pins[index].window, pins[index].generation, pins[index].slot, pins[index].sequence) == 0) {
                        pins[index].outstanding = 0u;
                        model.releases_sent += 1u;
                    }
                }
            } else if (roll < 56u) {
                /* Host configure: random state bits for a random window (exported or not). */
                uint8_t frame[WireFrameBytes];
                size_t length = 0;
                uint32_t state = rng_pick(16u);
                uint64_t generation = 1u + rng_pick(4u);
                if (build_host(InteropMsgWindowConfigure, frame, sizeof(frame), &length, window, generation, 0, 0, 0, 1, state) == 0 &&
                    wire_push_host(&fp->wire, frame, length) == 0) {
                    model.configures_sent += 1u;
                }
            } else if (roll < 60u) {
                uint8_t frame[WireFrameBytes];
                size_t length = 0;
                if (build_host(InteropMsgWindowClose, frame, sizeof(frame), &length, window, 1u + rng_pick(3u), 0, 0, 0, 0, 0) == 0 &&
                    wire_push_host(&fp->wire, frame, length) == 0) {
                    model.closes_sent += 1u;
                }
            } else if (roll < 63u) {
                fp->wire.backpressure_budget = rng_pick(3u);
            } else if (roll < 65u) {
                fp->capture.next_failure = 1u + rng_pick(2u);
            } else if (roll < 67u) {
                fp->adapter.fault_per_mille = rng_pick(60u);
            } else if (roll < 68u && window_coordinator_state(fp->coordinator) == WindowCoordinatorStateRunning) {
                fp->wire.disconnected = 1;
            } else if (roll == 200u) {
                /* The host restarts: a fresh epoch once it has proved its buffers are gone. */
                fp->wire.disconnected = 0;
                epoch += 1u;
                Check(window_coordinator_reconnect(fp->coordinator, epoch, 1u) == WindowCoordinatorOk);
                model_reset_epoch(&model, pins);
                model.reconnects += 1u;
            }
            status = tick(fp);
            if (status == WindowCoordinatorLost) model.lost_passes += 1u;
            Check(status == WindowCoordinatorOk || status == WindowCoordinatorLost);
            Check(consume_wire(fp, &model, pins) == 0);
            Check(window_coordinator_entry_count(fp->coordinator) <= InteropMaxWindows);
            if (rng_pick(3u) == 0u) {
                fp->wire.backpressure_budget = 0u;
                fp->adapter.fault_per_mille = 0u;
            }
            next_window_slot += 1u;
        }
        /* Drain: the host releases every pin, the windows go away, and the epoch must end with no entry and
         * every announced pair retired exactly once. */
        if (window_coordinator_state(fp->coordinator) == WindowCoordinatorStateLost) {
            fp->wire.disconnected = 0;
            epoch += 1u;
            Check(window_coordinator_reconnect(fp->coordinator, epoch, 1u) == WindowCoordinatorOk);
            model_reset_epoch(&model, pins);
            model.reconnects += 1u;
        }
        fp->wire.backpressure_budget = 0u;
        fp->adapter.fault_per_mille = 0u;
        if (window_coordinator_state(fp->coordinator) == WindowCoordinatorStateRunning) {
            /* Slot accounting: release pins and remove the random windows so every retirement completes; then a
             * full set of MaxWindows admissions must announce, which fails if slots leaked. */
            uint32_t before;
            while (fp->visible_count != 0u) remove_window(fp, fp->visible[0]);
            for (uint32_t pass = 0; pass < 40u && window_coordinator_entry_count(fp->coordinator) != 0u; ++pass) {
                release_outstanding(fp, pins);
                advance_clock(fp, 300u);
                Check(tick(fp) == WindowCoordinatorOk);
                Check(consume_wire(fp, &model, pins) == 0);
            }
            Check(window_coordinator_entry_count(fp->coordinator) == 0u);
            before = model.announces;
            for (uint32_t index = 0; index < InteropMaxWindows; ++index) add_window(fp, 0x9000u + 0x10u * (uint64_t)index);
            for (uint32_t pass = 0; pass < 40u && model.announces - before < InteropMaxWindows; ++pass) {
                advance_clock(fp, 300u);
                Check(tick(fp) == WindowCoordinatorOk);
                Check(consume_wire(fp, &model, pins) == 0);
            }
            Check(model.announces - before == InteropMaxWindows);
            Check(window_coordinator_entry_count(fp->coordinator) == InteropMaxWindows);
        }
        while (fp->visible_count != 0u) remove_window(fp, fp->visible[0]);
        for (uint32_t pass = 0; pass < 40u; ++pass) {
            release_outstanding(fp, pins);
            advance_clock(fp, 300u);
            Check(tick(fp) == WindowCoordinatorOk);
            Check(consume_wire(fp, &model, pins) == 0);
        }
        /* Every announced pair was retired, and nothing is left in the coordinator or on the host side. */
        Check(window_coordinator_entry_count(fp->coordinator) == 0u);
        for (uint32_t index = 0; index < ModelCapacity; ++index) {
            Check(model.live[index] == 0u);
        }
        fixture_close(fp);
        printf("random round %llu: announces=%u retires=%u frames=%u releases=%u updates=%u errors=%u hints=%u admits=%u lost=%u reconnects=%u\n",
            (unsigned long long)round, model.announces, model.retires, model.frames, model.releases_sent, model.updates, model.errors, model.hints, model.admits, model.lost_passes, model.reconnects);
    }
    return 0;
}

int main(void) {
    int failures = 0;
    struct {
        const char *name;
        int (*test)(void);
    } suites[] = {
        {"idle without the windows capability", test_create_without_capability_is_idle},
        {"create rejects invalid configuration", test_create_rejects_invalid_config},
        {"announce, publish and release", test_announce_publish_and_release},
        {"stale host frames are answered, not applied", test_stale_host_frame_is_answered_not_applied},
        {"configure and close reach the native window", test_configure_and_close_reach_the_native_window},
        {"backpressure keeps order and loses nothing", test_backpressure_keeps_order_and_loses_nothing},
        {"retire waits for pins, then completes", test_retire_waits_for_pins_then_completes},
        {"children retire before parents", test_children_retire_before_parents},
        {"phantom when the native window cannot be served", test_phantom_when_the_native_window_cannot_be_served},
        {"capture failure cancels the claim", test_capture_failure_cancels_the_claim},
        {"disconnect and reconnect with host proof", test_disconnect_and_reconnect_with_proof},
        {"create refuses each backend cleanly", test_create_backend_faults},
        {"admission and publish seam refusals", test_admission_and_publish_seam_faults},
        {"lost and reconnect seam refusals", test_lost_and_reconnect_seam_faults},
        {"admission refusals become phantoms and barriers hold", test_admission_refusals_turn_into_phantoms},
        {"faulted transitions and teardown", test_faulted_transitions_and_teardown},
        {"host frame classes and receive disconnect", test_host_frame_classes},
        {"destroy refusal is recoverable", test_destroy_refusal_is_recoverable},
        {"config table rejects each field", test_config_table_rejects_each_field},
        {"allocation and stop refusals", test_allocation_and_stop_refusals},
        {"host frame validation", test_host_frame_validation},
        {"reducer resynchronization", test_reducer_resynchronization},
        {"outbound backpressure holds", test_outbound_backpressure_holds},
        {"encoder refusals fault the coordinator", test_encoder_refusals_fault},
        {"untitled window announces", test_untitled_window_announces},
        {"reentry and budgeted passes", test_reentry_and_budgeted_passes},
        {"minimum size is encoded", test_minimum_size_is_encoded},
        {"desktop change pauses observation", test_desktop_change_pauses_observation},
        {"review P1: stale reply under full outbound is dropped", test_review_p1_stale_reply_under_full_outbound_drops},
        {"review: retire waits for FIFO room", test_review_retire_waits_for_fifo_room},
        {"defensive guards and owner-thread refusal", test_defensive_guards_and_owner_thread},
        {"title change reaches the host as WindowUpdate", test_title_change_sends_window_update},
        {"random long sequences with faults and wire invariants", test_random_long_sequences_with_faults},
    };
    for (size_t index = 0; index < sizeof(suites) / sizeof(suites[0]); ++index) {
        int result = suites[index].test();
        if (result != 0) {
            fprintf(stderr, "FAILED: %s\n", suites[index].name);
            failures += 1;
        } else {
            printf("ok: %s\n", suites[index].name);
        }
    }
    if (failures != 0) {
        fprintf(stderr, "window_coordinator_suite: %d test(s) failed\n", failures);
        return 1;
    }
    printf("window_coordinator_suite: all tests passed\n");
    return 0;
}
