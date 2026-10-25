/**
 * @file host_wayland_windows_defensive.h
 * @brief Branch coverage for the defensive paths of host_wayland_windows.c (TODO #11h).
 *
 * The codec validates every frame before the module decodes it, so most field refusals below are reached by
 * decoding payloads directly. Payloads are written with the codec writer and left unfinished, so each one
 * omits exactly the field under test. Corrupted-state paths are reached by breaking the table/record
 * correspondence on purpose, as a corrupt caller storage would. Included by host_wayland_windows_test.c.
 */
#ifndef HostWaylandWindowsDefensiveH
#define HostWaylandWindowsDefensiveH

/** @brief Announce payload (no header) with one field left out, written with the codec writer. */
static size_t announce_payload(uint8_t *buffer, size_t capacity, const announce_spec_t *spec, uint16_t skip_tag, bool skip_parent_generation) {
    interop_writer_t writer;
    interop_writer_init(&writer, buffer, capacity);
    if (skip_tag != InteropTagWindow) interop_put_uint(&writer, InteropTagWindow, spec->window);
    if (skip_tag != InteropTagGeneration) interop_put_uint(&writer, InteropTagGeneration, spec->generation);
    if (spec->has_parent) {
        interop_put_uint(&writer, InteropTagParent, spec->parent);
        if (!skip_parent_generation) interop_put_uint(&writer, InteropTagParentGeneration, spec->parent_generation);
    }
    if (skip_tag != InteropTagRole) interop_put_uint(&writer, InteropTagRole, spec->role);
    if (skip_tag != InteropTagRect) interop_put_rect(&writer, InteropTagRect, spec->x, spec->y, spec->width, spec->height);
    if (spec->title) interop_put_bytes(&writer, InteropTagTitle, spec->title, (uint16_t)strlen(spec->title));
    if (skip_tag != InteropTagAppKey) interop_put_bytes(&writer, InteropTagAppKey, spec->app_key, (uint16_t)strlen(spec->app_key));
    if (skip_tag != InteropTagState) interop_put_uint(&writer, InteropTagState, spec->state);
    if (skip_tag != InteropTagDpi) interop_put_uint(&writer, InteropTagDpi, spec->dpi);
    if (writer.length < InteropHeaderBytes) return 0;
    return writer.length - InteropHeaderBytes;
}

/** @brief Update payload with one field left out. */
static size_t update_payload(uint8_t *buffer, size_t capacity, uint16_t skip_tag, bool parent_only) {
    interop_writer_t writer;
    interop_writer_init(&writer, buffer, capacity);
    if (skip_tag != InteropTagWindow) interop_put_uint(&writer, InteropTagWindow, 1);
    if (skip_tag != InteropTagGeneration) interop_put_uint(&writer, InteropTagGeneration, 1);
    if (parent_only) {
        interop_put_uint(&writer, InteropTagParent, 2);
    }
    if (skip_tag != InteropTagRect) interop_put_rect(&writer, InteropTagRect, 0, 0, 10, 10);
    if (skip_tag != InteropTagState) interop_put_uint(&writer, InteropTagState, 0);
    if (skip_tag != InteropTagDpi) interop_put_uint(&writer, InteropTagDpi, 96);
    if (writer.length < InteropHeaderBytes) return 0;
    return writer.length - InteropHeaderBytes;
}

/** @brief Retire payload with one field left out. */
static size_t retire_payload(uint8_t *buffer, size_t capacity, bool skip_window, bool skip_generation) {
    interop_writer_t writer;
    interop_writer_init(&writer, buffer, capacity);
    if (!skip_window) interop_put_uint(&writer, InteropTagWindow, 1);
    if (!skip_generation) interop_put_uint(&writer, InteropTagGeneration, 1);
    if (writer.length < InteropHeaderBytes) return 0;
    return writer.length - InteropHeaderBytes;
}

/** @brief Decoders refuse every payload that lacks a required field or pairs a parent badly. */
static int check_decoder_refusals(void) {
    static uint8_t buffer[InteropMaxFrameBytes];
    announce_spec_t spec = toplevel_spec(1, 1, "a-app");
    spec.has_parent = false;
    announce_t req;
    size_t length = announce_payload(buffer, sizeof buffer, &spec, 0, false);
    Check(decode_announce(buffer + InteropHeaderBytes, length, &req) == 0);
    /* Each required tag, when missing, is refused by the decoder. */
    const uint16_t required[] = {InteropTagWindow, InteropTagGeneration, InteropTagRole, InteropTagRect, InteropTagAppKey, InteropTagState, InteropTagDpi};
    for (unsigned index = 0; index < sizeof required / sizeof required[0]; ++index) {
        length = announce_payload(buffer, sizeof buffer, &spec, required[index], false);
        Check(decode_announce(buffer + InteropHeaderBytes, length, &req) != 0);
    }
    /* A parent pair must be complete: a parent without its generation, or a generation without a parent. */
    announce_spec_t with_parent = spec;
    with_parent.has_parent = true;
    with_parent.parent = 9;
    with_parent.parent_generation = 2;
    length = announce_payload(buffer, sizeof buffer, &with_parent, 0, true);
    Check(decode_announce(buffer + InteropHeaderBytes, length, &req) != 0);
    length = announce_payload(buffer, sizeof buffer, &with_parent, 0, false);
    Check(decode_announce(buffer + InteropHeaderBytes, length, &req) == 0 && req.has_parent);

    /* Update: window, generation and rect are required; a parent without its generation is refused. */
    update_t update;
    length = update_payload(buffer, sizeof buffer, InteropTagWindow, false);
    Check(decode_update(buffer + InteropHeaderBytes, length, &update) != 0);
    length = update_payload(buffer, sizeof buffer, InteropTagGeneration, false);
    Check(decode_update(buffer + InteropHeaderBytes, length, &update) != 0);
    length = update_payload(buffer, sizeof buffer, InteropTagRect, false);
    Check(decode_update(buffer + InteropHeaderBytes, length, &update) != 0);
    length = update_payload(buffer, sizeof buffer, 0, true);
    Check(decode_update(buffer + InteropHeaderBytes, length, &update) != 0);
    length = update_payload(buffer, sizeof buffer, 0, false);
    Check(decode_update(buffer + InteropHeaderBytes, length, &update) == 0 && !update.has_parent);

    /* Retire: window and generation are both required. */
    length = retire_payload(buffer, sizeof buffer, true, false);
    Check(handle_retire(&session_ctx, buffer + InteropHeaderBytes, length) == HostWaylandMalformedFrame);
    length = retire_payload(buffer, sizeof buffer, false, true);
    Check(handle_retire(&session_ctx, buffer + InteropHeaderBytes, length) == HostWaylandMalformedFrame);
    return 0;
}

/** @brief Frame-level refusals that the codec reports before the module decodes anything. */
static int check_frame_refusals(void) {
    Check(begin_plain_session() == 0);
    announce_spec_t spec = toplevel_spec(1, 1, "a-app");
    uint8_t frame[InteropMaxFrameBytes];
    size_t length = build_announce(frame, sizeof frame, &spec);
    /* A header that claims more payload than the buffer holds. */
    Check(host_wayland_windows_handle_frame(&session_ctx, frame, InteropHeaderBytes, 0) == HostWaylandMalformedFrame);
    /* An unknown critical tag in the payload: validation reports UnsupportedField, which is malformed here. */
    uint8_t mutated[InteropMaxFrameBytes];
    memcpy(mutated, frame, length);
    mutated[InteropHeaderBytes] = 0x77;
    mutated[InteropHeaderBytes + 1] = 0xF7;
    Check(host_wayland_windows_handle_frame(&session_ctx, mutated, length, 0) == HostWaylandMalformedFrame);
    /* A guest message this layer does not realize reaches the switch default and is unsupported. */
    interop_writer_t writer;
    uint8_t move[InteropMaxFrameBytes];
    interop_writer_init(&writer, move, sizeof move);
    interop_put_uint(&writer, InteropTagWindow, 1);
    interop_put_uint(&writer, InteropTagGeneration, 1);
    interop_put_uint(&writer, InteropTagSerial, 7);
    size_t move_length = 0;
    Check(interop_writer_finish(&writer, InteropMsgWindowMoveRequest, 0, InteropFromGuest, &move_length) == InteropErrorNone);
    Check(host_wayland_windows_handle_frame(&session_ctx, move, move_length, 0) == HostWaylandUnsupportedType);
    /* Header lengths that the buffer cannot hold. */
    Check(host_wayland_windows_handle_frame(&session_ctx, frame, 8, 0) == HostWaylandMalformedFrame);
    return 0;
}

/** @brief Context and binding edges: uninitialized or NULL contexts, bindings to nothing, unknown kinds. */
static int check_binding_edges(void) {
    static host_wayland_t uninitialized;
    memset(&uninitialized, 0, sizeof uninitialized);
    Check(host_wayland_windows_window_count(NULL) == 0);
    Check(host_wayland_windows_window_count(&uninitialized) == 0);
    Check(!host_wayland_windows_is_mapped(NULL, 1, 1));
    Check(!host_wayland_windows_is_mapped(&uninitialized, 1, 1));
    Check(host_wayland_windows_set_scale(&uninitialized, 1, 1) == HostWaylandNotInitialized);
    Check(host_wayland_windows_handle_frame(NULL, NULL, 0, 0) == HostWaylandInvalidArgument);
    host_wayland_config_t bad = sentinel_config();
    bad.scale_numerator = 0;
    bad.scale_denominator = 5;
    Check(host_wayland_windows_init(&uninitialized, &bad) == HostWaylandInvalidArgument);
    bad = sentinel_config();
    bad.device = NULL;
    Check(host_wayland_windows_init(&uninitialized, &bad) == HostWaylandInvalidArgument);
    host_wayland_record_t record;
    memset(&record, 0, sizeof record);
    Check(object_of(&record, 9) == NULL);

    /* Events through a binding that names no live context, an uninitialized one, or a slot past the table. */
    host_wayland_binding_t orphan;
    memset(&orphan, 0, sizeof orphan);
    Check(resolve(&orphan, NULL) == NULL);
    orphan.owner = &uninitialized;
    Check(resolve(&orphan, NULL) == NULL);
    Check(begin_plain_session() == 0);
    orphan.owner = &session_ctx;
    orphan.slot = 99;
    Check(resolve(&orphan, NULL) == NULL);
    on_surface_configure(&orphan, (struct xdg_surface *)fake_object(0), 1);
    Check(fake_count_requests("xdg_surface", XDG_SURFACE_ACK_CONFIGURE) == 0);
    return 0;
}

/** @brief Session with every callback sink NULL: each event is still handled and nothing is reported. */
static int check_null_callbacks(void) {
    fake_begin();
    capture_reset();
    host_wayland_config_t config = fake_config("dev0", 5);
    config.callbacks.on_configure = NULL;
    config.callbacks.on_close = NULL;
    config.callbacks.on_popup_repositioned = NULL;
    config.callbacks.on_popup_dismissed = NULL;
    config.callbacks.on_error = NULL;
    config.decoration_manager = (struct zxdg_decoration_manager_v1 *)fake_object(fake_new("zxdg_decoration_manager_v1"));
    fixture_creates = fake_creates;
    Check(host_wayland_windows_init(&session_ctx, &config) == HostWaylandOk);
    announce_spec_t root = toplevel_spec(1, 1, "a-root");
    Check(announce_spec(&session_ctx, &root) == HostWaylandOk);
    /* A refusal with no error sink is still a refusal. */
    Check(announce_spec(&session_ctx, &root) == HostWaylandRefused);
    unsigned toplevel = fake_nth("xdg_toplevel", 0);
    deliver_toplevel_configure(toplevel, 1, 1, NULL, 0);
    deliver_toplevel_close(toplevel);
    /* A NULL states array carries no state bits. */
    const struct xdg_toplevel_listener *listener = fake_proxies[toplevel].listener;
    listener->configure(fake_proxies[toplevel].data, (struct xdg_toplevel *)fake_object(toplevel), 4, 4, NULL);
    announce_spec_t menu = menu_spec(3, 1, 1, 3, 30, 12, 40, 20);
    Check(announce_spec(&session_ctx, &menu) == HostWaylandOk);
    unsigned popup = fake_nth("xdg_popup", 0);
    deliver_popup_configure(popup, 15, 6, 40, 20);
    deliver_popup_repositioned(popup, 9);
    deliver_popup_done(popup);
    Check(captured.repositioned == 0 && captured.dismissed == 0 && captured.configures == 0 && captured.closes == 0);
    return 0;
}

/** @brief Repositioning overflow is reported as an error; the popup is not repositioned. */
static int check_reposition_overflow(void) {
    /* At scale 2 a guest x of 3 has the logical origin 1, so a compositor x near INT32_MAX overflows the
     * guest rectangle: 3 + (INT32_MAX - 1) * 2. */
    Check(begin_popup_session(5) == 0);
    Check(host_wayland_windows_set_scale(&session_ctx, 2, 1) == HostWaylandOk);
    announce_spec_t root = toplevel_spec(1, 1, "a-root");
    Check(announce_spec(&session_ctx, &root) == HostWaylandOk);
    announce_spec_t menu = menu_spec(3, 1, 1, 3, 3, 12, 40, 20);
    Check(announce_spec(&session_ctx, &menu) == HostWaylandOk);
    unsigned popup = fake_nth("xdg_popup", 0);
    deliver_popup_configure(popup, 0x7FFFFFFF, 0, 40, 20);
    Check(captured.repositioned == 0);
    Check(captured.errors == 1 && captured.last_refused_type == 0x0103 && captured.last_error_code == InteropErrorUnexpected);
    return 0;
}

/** @brief A popup_done the table no longer holds realized changes nothing and reports nothing. */
static int check_dismiss_desync(void) {
    Check(begin_popup_session(5) == 0);
    announce_spec_t root = toplevel_spec(1, 1, "a-root");
    Check(announce_spec(&session_ctx, &root) == HostWaylandOk);
    announce_spec_t menu = menu_spec(3, 1, 1, 3, 30, 12, 40, 20);
    Check(announce_spec(&session_ctx, &menu) == HostWaylandOk);
    unsigned popup = fake_nth("xdg_popup", 0);
    host_window_ref_t id;
    id.window = 3;
    id.generation = 1;
    host_window_ref_t removed[HostWindowMapMaxOutput];
    uint32_t count = 0;
    Check(host_window_map_retire(&session_ctx.map, id, removed, HostWindowMapMaxOutput, &count) == HostWindowMapRetired);
    deliver_popup_done(popup);
    Check(captured.dismissed == 0);
    return 0;
}

/** @brief Zero-width parents are sized as one logical unit, so the positioner still has an anchor range. */
static int check_zero_parent_size(void) {
    Check(begin_popup_session(5) == 0);
    announce_spec_t root = toplevel_spec(1, 1, "a-root");
    root.width = 0;
    root.height = 0;
    Check(announce_spec(&session_ctx, &root) == HostWaylandOk);
    announce_spec_t menu = menu_spec(3, 1, 1, 3, 30, 12, 40, 20);
    Check(announce_spec(&session_ctx, &menu) == HostWaylandOk);
    int anchor = fake_find("xdg_positioner", XDG_POSITIONER_SET_ANCHOR_RECT, 0);
    Check(anchor >= 0 && fake_requests[anchor].a0 == 0 && fake_requests[anchor].a1 == 0);
    return 0;
}

/** @brief A dialog moved onto a popup: the table accepts it and the toplevel's parent is unset. */
static int check_mirror_to_popup_parent(void) {
    Check(begin_popup_session(5) == 0);
    announce_spec_t root = toplevel_spec(1, 1, "a-root");
    Check(announce_spec(&session_ctx, &root) == HostWaylandOk);
    announce_spec_t dialog = toplevel_spec(2, 1, "a-child");
    dialog.role = 2;
    dialog.has_parent = true;
    dialog.parent = 1;
    dialog.parent_generation = 1;
    Check(announce_spec(&session_ctx, &dialog) == HostWaylandOk);
    announce_spec_t menu = menu_spec(3, 1, 1, 3, 30, 12, 40, 20);
    Check(announce_spec(&session_ctx, &menu) == HostWaylandOk);
    unsigned before = fake_request_count;
    update_spec_t move = {2, 1, true, 3, 1, NULL, false, 0, 0};
    Check(update_frame(&move) == HostWaylandOk);
    int parent = fake_find("xdg_toplevel", XDG_TOPLEVEL_SET_PARENT, before);
    Check(parent >= 0 && fake_requests[parent].proxy == fake_nth("xdg_toplevel", 1) && fake_requests[parent].a0 == -1);
    return 0;
}

/** @brief The root walk stops after the table's bound even when a corrupt storage makes a popup its own parent. */
static int check_root_walk_bound(void) {
    Check(begin_popup_session(5) == 0);
    announce_spec_t root = toplevel_spec(1, 1, "a-root");
    Check(announce_spec(&session_ctx, &root) == HostWaylandOk);
    announce_spec_t menu = menu_spec(3, 1, 1, 3, 30, 12, 40, 20);
    Check(announce_spec(&session_ctx, &menu) == HostWaylandOk);
    host_window_entry_t entry;
    Check(host_window_map_lookup(&session_ctx.map, (host_window_ref_t){3, 1}, &entry));
    session_ctx.map.slots[entry.slot].parent = (host_window_ref_t){3, 1};
    host_window_ref_t walked = root_of(&session_ctx, (host_window_ref_t){3, 1});
    Check(walked.window == 3 && walked.generation == 1);
    /* An unknown pair is its own root. */
    Check(root_of(&session_ctx, (host_window_ref_t){9, 1}).window == 9);
    return 0;
}

/** @brief Desynchronized storage: a window the table holds with no record. Realization fails cleanly, retire and
 * reset skip the missing record, and nothing is destroyed twice. */
static int check_missing_record(void) {
    Check(begin_popup_session(5) == 0);
    announce_spec_t root = toplevel_spec(1, 1, "a-root");
    Check(announce_spec(&session_ctx, &root) == HostWaylandOk);
    uint32_t slot = 0;
    host_window_entry_t entry;
    Check(host_window_map_lookup(&session_ctx.map, (host_window_ref_t){1, 1}, &entry));
    slot = entry.slot;
    destroy_record_objects(&session_ctx.records[slot]);
    /* A popup under the parent whose record is gone is refused and rolled back, leaving no object behind. */
    unsigned creates = fake_creates, destroys = fake_destroys;
    announce_spec_t menu = menu_spec(3, 1, 1, 3, 30, 12, 40, 20);
    Check(announce_spec(&session_ctx, &menu) == HostWaylandRealizeFailed);
    Check(fake_creates - creates == fake_destroys - destroys);
    Check(captured.last_error_code == InteropErrorStaleWindow);
    /* Retiring the window the table still holds frees its map entry without touching a missing record. */
    host_window_ref_t removed[HostWindowMapMaxOutput];
    uint32_t count = 0;
    Check(retire_window(&session_ctx, 1, 1) == HostWaylandOk);
    (void)removed;
    (void)count;
    Check(host_wayland_windows_window_count(&session_ctx) == 0);
    Check(host_wayland_windows_reset(&session_ctx) == HostWaylandOk);
    return 0;
}

/** @brief Retire and reset take their error and skip paths for a zero identity and for records already gone. */
static int check_retire_and_reset_paths(void) {
    Check(begin_popup_session(5) == 0);
    Check(retire_window(&session_ctx, 0, 0) == HostWaylandRefused);
    Check(captured.last_refused_type == InteropMsgWindowRetire && captured.last_error_code == InteropErrorMalformedFrame);
    announce_spec_t root = toplevel_spec(1, 1, "a-root");
    Check(announce_spec(&session_ctx, &root) == HostWaylandOk);
    Check(host_wayland_windows_reset(&session_ctx) == HostWaylandOk);
    Check(fake_creates - fixture_creates == fake_destroys);
    return 0;
}

#endif
