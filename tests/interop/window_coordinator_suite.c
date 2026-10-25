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
                fp->wire.backpressure_budget = 8u;
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
        fp->wire.backpressure_budget = 8u;
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
