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

static int fixture_open(fixture_t *f, uint64_t capabilities) {
    window_coordinator_config_t config;
    if (f->coordinator != NULL || f->observer != NULL) fixture_close(f);
    memset(f, 0, sizeof(*f));
    f->observer = api_double_new();
    if (f->observer == NULL) return 1;
    wsa_double_init(&f->adapter, 0u);
    f->adapter.current_thread = 0u;
    f->capture_ops = CaptureOpsTable;
    f->capture_ops.context = &f->capture;
    f->capture.mapping_bytes = UINT64_C(1) << 30;
    f->io = WireIo;
    f->io.context = &f->wire;
    f->epoch = CoordEpoch;
    config = make_config(f, CoordEpoch, capabilities);
    if (window_coordinator_create(&config, &f->coordinator) != WindowCoordinatorOk) return 1;
    return 0;
}

static void fixture_close(fixture_t *f) {
    if (f->coordinator != NULL) {
        /* Retire every window and release every pin the host still holds, so the coordinator can be freed. */
        for (uint32_t round = 0; round < 8u && window_coordinator_entry_count(f->coordinator) != 0u; ++round) {
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

    /* The window disappears: the observer retires it; the coordinator stops capture and sends Retire. */
    remove_window(fp, 0x1000);
    advance_clock(fp, 300u);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(count_type(&fp->wire, InteropMsgWindowRetire, 0x1000, 1) == 1u);
    Check(fp->capture.stopped == 1u);
    /* The frame pin is still held by the host, so the quarantine remains. */
    Check(window_coordinator_entry_count(fp->coordinator) == 1u);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(window_coordinator_entry_count(fp->coordinator) == 1u);

    /* The host releases the pin. The next pass completes the retirement. */
    Check(host_release(fp, 0x1000, 1, 0, 1) == 0);
    Check(tick(fp) == WindowCoordinatorOk);
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
    /* Both retirements were sent; the child's Retire comes first in the wire order. */
    {
        uint32_t child_index = 0, parent_index = 0;
        sent_t sent;
        for (uint32_t index = 0; index < fp->wire.count; ++index) {
            if (!sent_at(&fp->wire, index, &sent) || sent.type != InteropMsgWindowRetire) continue;
            if (sent.window == 0x2000u && child_index == 0u) child_index = index + 1u;
            if (sent.window == 0x1000u && parent_index == 0u) parent_index = index + 1u;
        }
        Check(child_index != 0u && parent_index != 0u && child_index < parent_index);
    }
    /* Parent cannot complete while the child still holds its pin. */
    Check(host_release(fp, 0x2000, 2, 0, 1) == 0);
    Check(tick(fp) == WindowCoordinatorOk);
    Check(window_coordinator_entry_count(fp->coordinator) == 1u);
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
