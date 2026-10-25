/**
 * @file host_wayland_windows_fixtures.h
 * @brief Codec-to-mapping fixtures (TODO #11g). Each fixture is a frame built with the codec writer, fed to
 * host_wayland_windows_handle_frame, and checked against the exact sequence of Wayland requests the recording
 * double captured. A request signature renders each request as "interface:opcode", with "!" for a destroy.
 * Included by host_wayland_windows_test.c after the module source and the fake.
 */
#ifndef HostWaylandWindowsFixturesH
#define HostWaylandWindowsFixturesH

/** @brief Render the requests recorded from `from` on as one signature string. */
static void request_signature(char *out, size_t capacity, unsigned from) {
    size_t used = 0;
    out[0] = '\0';
    for (unsigned index = from; index < fake_request_count; ++index) {
        if (!fake_requests[index].iface) continue;
        int written = snprintf(out + used, capacity - used, "%s%s:%u%s", used ? " " : "", fake_requests[index].iface, fake_requests[index].opcode, fake_requests[index].destroy ? "!" : "");
        if (written < 0 || (size_t)written >= capacity - used) break;
        used += (size_t)written;
    }
}

static int expect_signature(unsigned from, const char *expected) {
    char actual[2048];
    request_signature(actual, sizeof actual, from);
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "signature mismatch\n  expected: %s\n  actual:   %s\n", expected, actual);
        return 1;
    }
    return 0;
}

/** @brief Fixture session without decoration manager: one toplevel per frame and nothing else. */
static int begin_plain_session(void) {
    fake_begin();
    capture_reset();
    host_wayland_config_t config = fake_config("dev0", 5);
    fixture_creates = fake_creates;
    return host_wayland_windows_init(&session_ctx, &config) == HostWaylandOk ? 0 : 1;
}

static int check_codec_fixtures(void) {
    /* Toplevel with a title: surface, xdg_surface, toplevel, app id, title, then the buffer-less commit. */
    Check(begin_plain_session() == 0);
    announce_spec_t toplevel = toplevel_spec(1, 1, "a-app");
    toplevel.title = "Title";
    Check(announce_spec(&session_ctx, &toplevel) == HostWaylandOk);
    Check(expect_signature(0, "wl_compositor:0 xdg_wm_base:2 xdg_surface:1 xdg_toplevel:3 xdg_toplevel:2 wl_surface:6") == 0);

    /* Dialog under that toplevel: set_parent names the parent's toplevel before the commit. */
    unsigned mark = fake_request_count;
    announce_spec_t dialog = toplevel_spec(2, 1, "a-child");
    dialog.role = 2;
    dialog.has_parent = true;
    dialog.parent = 1;
    dialog.parent_generation = 1;
    Check(announce_spec(&session_ctx, &dialog) == HostWaylandOk);
    Check(expect_signature(mark, "wl_compositor:0 xdg_wm_base:2 xdg_surface:1 xdg_toplevel:3 xdg_toplevel:1 wl_surface:6") == 0);

    /* Consent dialog: its own app id, no title. */
    mark = fake_request_count;
    announce_spec_t consent = toplevel_spec(3, 1, "a-ignored");
    consent.role = 5;
    Check(announce_spec(&session_ctx, &consent) == HostWaylandOk);
    Check(expect_signature(mark, "wl_compositor:0 xdg_wm_base:2 xdg_surface:1 xdg_toplevel:3 wl_surface:6") == 0);

    /* A Menu under the toplevel with a live press on it: positioner first, then get_popup, the positioner destroy,
     * the grab, and only then the commit. */
    Check(begin_plain_session() == 0);
    announce_spec_t root = toplevel_spec(1, 1, "a-root");
    Check(announce_spec(&session_ctx, &root) == HostWaylandOk);
    unsigned seat = fake_new("wl_seat");
    Check(record_press_at(&session_ctx, 50, 600, (struct wl_seat *)fake_object(seat), 1, 1, 1000) == HostWaylandOk);
    mark = fake_request_count;
    announce_spec_t menu = menu_spec(3, 1, 1, 3, 30, 12, 40, 20);
    menu.has_serial = true;
    menu.serial = 50;
    Check(announce_spec(&session_ctx, &menu) == HostWaylandOk);
    Check(expect_signature(mark, "wl_compositor:0 xdg_wm_base:2 xdg_wm_base:1 xdg_positioner:1 xdg_positioner:2 xdg_positioner:3 xdg_positioner:4 xdg_positioner:6 xdg_positioner:5 xdg_positioner:7 xdg_surface:2 xdg_positioner:0! xdg_popup:1 wl_surface:6") == 0);

    /* Title change, and a retire that takes the dialog with it: children first, each window in order. */
    Check(begin_plain_session() == 0);
    announce_spec_t parent = toplevel_spec(1, 1, "a-root");
    Check(announce_spec(&session_ctx, &parent) == HostWaylandOk);
    announce_spec_t child = toplevel_spec(2, 1, "a-child");
    child.role = 2;
    child.has_parent = true;
    child.parent = 1;
    child.parent_generation = 1;
    Check(announce_spec(&session_ctx, &child) == HostWaylandOk);
    mark = fake_request_count;
    update_spec_t retitle = {1, 1, false, 0, 0, "Renamed", false, 0, 0};
    Check(update_frame(&retitle) == HostWaylandOk);
    Check(expect_signature(mark, "xdg_toplevel:2") == 0);
    mark = fake_request_count;
    Check(retire_frame(1, 1) == HostWaylandOk);
    Check(expect_signature(mark, "xdg_toplevel:0! xdg_surface:0! wl_surface:0! xdg_toplevel:0! xdg_surface:0! wl_surface:0!") == 0);
    /* The retire sent the dialog's destroy first (child first), then the parent's objects. */
    Check(fake_count_requests("wl_surface", WL_SURFACE_DESTROY) == 2);

    /* Frames that must not reach a request: bad magic, truncated payload, a host-to-guest message type, and an
     * unknown message type. */
    Check(begin_plain_session() == 0);
    uint8_t frame[InteropMaxFrameBytes];
    size_t length = build_announce(frame, sizeof frame, &toplevel);
    Check(length > InteropHeaderBytes);
    unsigned quiet = fake_request_count;
    uint8_t bad_magic[InteropMaxFrameBytes];
    memcpy(bad_magic, frame, length);
    bad_magic[0] ^= 0xFFu;
    Check(host_wayland_windows_handle_frame(&session_ctx, bad_magic, length, 0) == HostWaylandMalformedFrame);
    Check(host_wayland_windows_handle_frame(&session_ctx, frame, length - 4, 0) == HostWaylandMalformedFrame);
    interop_writer_t writer;
    uint8_t configure[InteropMaxFrameBytes];
    interop_writer_init(&writer, configure, sizeof configure);
    interop_put_uint(&writer, InteropTagWindow, 1);
    interop_put_uint(&writer, InteropTagGeneration, 1);
    interop_put_uint(&writer, InteropTagState, 0);
    size_t configure_length = 0;
    Check(interop_writer_finish(&writer, InteropMsgWindowConfigure, 0, InteropFromHost, &configure_length) == InteropErrorNone);
    Check(host_wayland_windows_handle_frame(&session_ctx, configure, configure_length, 0) == HostWaylandUnsupportedType);
    uint8_t unknown[InteropMaxFrameBytes];
    memcpy(unknown, frame, length);
    unknown[6] = 0x99;
    unknown[7] = 0x01;
    Check(host_wayland_windows_handle_frame(&session_ctx, unknown, length, 0) == HostWaylandUnsupportedType);
    Check(fake_request_count == quiet && host_wayland_windows_window_count(&session_ctx) == 0);
    return 0;
}

#endif
