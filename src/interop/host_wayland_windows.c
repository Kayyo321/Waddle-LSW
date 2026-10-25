/**
 * @file host_wayland_windows.c
 * @brief Host realization of interop windows as xdg-shell objects (see host_wayland_windows.h).
 *
 * Layering: frame decoding uses the codec in interop_protocol.h; the window table and the popup
 * geometry are the Zig modules behind host_window_map.h and host_popup_positioner.h. This file owns the
 * Wayland objects, the listener bindings and the press table. It never allocates.
 */
#include "host_wayland_windows.h"

#include "waddle/interop_protocol.h"
#include "wayland-client-protocol.h"
#include "xdg_decoration_client.h"
#include "xdg_shell_client.h"

#include <stdio.h>
#include <string.h>

/** @brief Number of press-table entries (3.6.2: a bounded 64-entry table). */
#define PRESS_COUNT 64u

/** @brief Ceiling of `value / scale` for the integer host scale, zero stays zero. */
static uint32_t ceil_div_scale(uint32_t value, uint32_t scale) {
    return (uint32_t)(((uint64_t)value + scale - 1u) / scale);
}

static bool context_ready(const host_wayland_t *ctx) {
    return ctx != NULL && ctx->initialized != 0;
}

host_wayland_status_t host_wayland_windows_init(host_wayland_t *ctx, const host_wayland_config_t *config) {
    if (ctx == NULL || config == NULL) return HostWaylandInvalidArgument;
    if (config->compositor == NULL || config->wm_base == NULL || config->device == NULL) return HostWaylandInvalidArgument;
    if (config->wm_base_version == 0) return HostWaylandInvalidArgument;
    size_t device_length = 0;
    while (device_length < 65 && config->device[device_length] != '\0') ++device_length;
    if (device_length == 0 || device_length > 64) return HostWaylandInvalidArgument;

    /* A zero fraction means an unscaled host, 1/1. */
    uint32_t numerator = config->scale_numerator == 0 && config->scale_denominator == 0 ? 1u : config->scale_numerator;
    uint32_t denominator = config->scale_numerator == 0 && config->scale_denominator == 0 ? 1u : config->scale_denominator;
    uint32_t scale = 0;
    if (host_popup_positioner_integer_scale(numerator, denominator, &scale) != HostPopupOk) return HostWaylandInvalidArgument;

    memset(ctx, 0, sizeof *ctx);
    ctx->config = *config;
    ctx->config.scale_numerator = numerator;
    ctx->config.scale_denominator = denominator;
    ctx->scale = scale;
    ctx->initialized = 1;
    return HostWaylandOk;
}

host_wayland_status_t host_wayland_windows_set_scale(host_wayland_t *ctx, uint32_t numerator, uint32_t denominator) {
    if (!context_ready(ctx)) return HostWaylandNotInitialized;
    uint32_t scale = 0;
    if (host_popup_positioner_integer_scale(numerator, denominator, &scale) != HostPopupOk) return HostWaylandInvalidArgument;
    ctx->scale = scale;
    ctx->config.scale_numerator = numerator;
    ctx->config.scale_denominator = denominator;
    return HostWaylandOk;
}

uint32_t host_wayland_windows_window_count(const host_wayland_t *ctx) {
    return context_ready(ctx) ? host_window_map_realized_count(&ctx->map) : 0;
}

bool host_wayland_windows_is_mapped(const host_wayland_t *ctx, uint64_t window, uint64_t generation) {
    if (!context_ready(ctx)) return false;
    host_window_ref_t id;
    id.window = window;
    id.generation = generation;
    host_window_entry_t entry;
    if (!host_window_map_lookup(&ctx->map, id, &entry)) return false;
    return ctx->records[entry.slot].mapped != 0;
}

/* ---- press table (3.6.2) ---------------------------------------------------------------------- */

/** @brief Remove entries whose validity window has passed. A clock that moved backwards keeps entries. */
static void press_expire(host_wayland_t *ctx, uint64_t now_ms) {
    for (size_t index = 0; index < PRESS_COUNT; ++index) {
        host_wayland_press_t *press = &ctx->presses[index];
        if (press->live && now_ms >= press->recorded_ms && now_ms - press->recorded_ms > HOST_WAYLAND_PRESS_EXPIRY_MS) {
            press->live = 0;
        }
    }
}

host_wayland_status_t host_wayland_windows_record_press(host_wayland_t *ctx, uint32_t protocol_serial, uint32_t wayland_serial, struct wl_seat *seat, uint64_t window, uint64_t generation, uint64_t now_ms) {
    if (!context_ready(ctx)) return HostWaylandNotInitialized;
    if (protocol_serial == 0 || seat == NULL || window == 0 || generation == 0) return HostWaylandInvalidArgument;
    press_expire(ctx, now_ms);
    for (size_t index = 0; index < PRESS_COUNT; ++index) {
        host_wayland_press_t *press = &ctx->presses[index];
        if (press->live) continue;
        press->protocol_serial = protocol_serial;
        press->wayland_serial = wayland_serial;
        press->seat = seat;
        press->window = window;
        press->generation = generation;
        press->recorded_ms = now_ms;
        press->live = 1;
        return HostWaylandOk;
    }
    /* Every entry is live. Evicting one would lose a grab a compositor may still need, so fail instead. */
    return HostWaylandPressTableFull;
}

void host_wayland_windows_release_press(host_wayland_t *ctx, uint32_t protocol_serial) {
    if (!context_ready(ctx)) return;
    for (size_t index = 0; index < PRESS_COUNT; ++index) {
        host_wayland_press_t *press = &ctx->presses[index];
        if (press->live && press->protocol_serial == protocol_serial) press->live = 0;
    }
}

/** @brief Live entry for protocol serial T at `now_ms`, or NULL. */
static host_wayland_press_t *press_find(host_wayland_t *ctx, uint32_t protocol_serial, uint64_t now_ms) {
    press_expire(ctx, now_ms);
    for (size_t index = 0; index < PRESS_COUNT; ++index) {
        host_wayland_press_t *press = &ctx->presses[index];
        if (press->live && press->protocol_serial == protocol_serial) return press;
    }
    return NULL;
}

/* ---- records, bindings and events -------------------------------------------------------------- */

/* Protocol role values, identical to interop_window_role_t (checked by the header and by the tests). */
#define ROLE_TOPLEVEL 1u
#define ROLE_DIALOG 2u
#define ROLE_MENU 3u
#define ROLE_TOOLTIP 4u
#define ROLE_UAC 5u
#define ROLE_IME 6u

static bool role_is_popup(uint32_t role) {
    return role == ROLE_MENU || role == ROLE_TOOLTIP || role == ROLE_IME;
}

static bool role_is_toplevel(uint32_t role) {
    return role == ROLE_TOPLEVEL || role == ROLE_DIALOG || role == ROLE_UAC;
}

/** @brief Object of `kind` currently owned by a record, as an opaque proxy pointer. */
static const void *object_of(const host_wayland_record_t *rec, uint32_t kind) {
    switch (kind) {
    case HostWaylandKindXdgSurface: return rec->xdg_surface;
    case HostWaylandKindToplevel: return rec->toplevel;
    case HostWaylandKindPopup: return rec->popup;
    case HostWaylandKindDecoration: return rec->decoration;
    default: return NULL;
    }
}

/**
 * @brief Record a listener event belongs to, or NULL when the event is stale.
 *
 * The event is valid only for the record's current generation and only when the proxy the compositor
 * named is still the object the record owns for that kind. A destroyed object therefore never matches.
 */
static host_wayland_record_t *resolve(const host_wayland_binding_t *binding, const void *proxy) {
    host_wayland_t *ctx = binding->owner;
    if (ctx == NULL || !ctx->initialized || binding->slot >= HostWindowMapMaxRealized) return NULL;
    host_wayland_record_t *rec = &ctx->records[binding->slot];
    if (rec->generation == 0 || rec->generation != binding->generation) return NULL;
    return object_of(rec, binding->kind) == proxy ? rec : NULL;
}

/** @brief Record of a realized window, or NULL. */
static host_wayland_record_t *find_record(host_wayland_t *ctx, uint64_t window, uint64_t generation) {
    for (size_t index = 0; index < HostWindowMapMaxRealized; ++index) {
        host_wayland_record_t *rec = &ctx->records[index];
        if (rec->generation != 0 && rec->window == window && rec->generation == generation) return rec;
    }
    return NULL;
}

static void emit_error(host_wayland_t *ctx, uint16_t refused_type, uint32_t error_code, uint64_t window, uint64_t generation) {
    if (ctx->config.callbacks.on_error) {
        ctx->config.callbacks.on_error(ctx->config.callbacks.user, refused_type, error_code, window, generation);
    }
}

/* Listener for xdg_surface: configure is the only event. */
static void on_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
    host_wayland_record_t *rec = resolve(data, xdg_surface);
    if (rec == NULL) return;
    xdg_surface_ack_configure(xdg_surface, serial);
    rec->mapped = 1;
}

/** @brief Map a wl_array of xdg_toplevel states onto InteropState bits. Minimized is never reported. */
static uint32_t state_bits(const struct wl_array *states) {
    uint32_t bits = 0;
    if (states == NULL || states->data == NULL) return 0;
    const uint32_t *values = states->data;
    size_t count = states->size / sizeof *values;
    for (size_t index = 0; index < count; ++index) {
        switch (values[index]) {
        case XDG_TOPLEVEL_STATE_MAXIMIZED: bits |= InteropStateMaximized; break;
        case XDG_TOPLEVEL_STATE_FULLSCREEN: bits |= InteropStateFullscreen; break;
        case XDG_TOPLEVEL_STATE_ACTIVATED: bits |= InteropStateActivated; break;
        default: break;
        }
    }
    return bits;
}

static void on_toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height, struct wl_array *states) {
    host_wayland_record_t *rec = resolve(data, toplevel);
    if (rec == NULL) return;
    host_wayland_t *ctx = ((const host_wayland_binding_t *)data)->owner;
    if (ctx->config.callbacks.on_configure) {
        uint32_t w = width > 0 ? (uint32_t)width : 0u;
        uint32_t h = height > 0 ? (uint32_t)height : 0u;
        ctx->config.callbacks.on_configure(ctx->config.callbacks.user, rec->window, rec->generation, state_bits(states), w, h);
    }
}

static void on_toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    host_wayland_record_t *rec = resolve(data, toplevel);
    if (rec == NULL) return;
    host_wayland_t *ctx = ((const host_wayland_binding_t *)data)->owner;
    if (ctx->config.callbacks.on_close) ctx->config.callbacks.on_close(ctx->config.callbacks.user, rec->window, rec->generation);
}

static const struct xdg_surface_listener surface_listener = {
    .configure = on_surface_configure,
};

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = on_toplevel_configure,
    .close = on_toplevel_close,
};

/* ---- object lifetime --------------------------------------------------------------------------- */

/**
 * @brief Detach every object of a record, then destroy them in the fixed order: decoration, toplevel or
 * popup, positioner, xdg_surface, wl_surface.
 *
 * Pointers and the generation are cleared before any destroy request, so an event that arrives during
 * teardown finds no owner and is a no-op.
 */
static void destroy_record_objects(host_wayland_record_t *rec) {
    struct zxdg_toplevel_decoration_v1 *decoration = rec->decoration;
    struct xdg_toplevel *toplevel = rec->toplevel;
    struct xdg_popup *popup = rec->popup;
    struct xdg_surface *xdg_surface = rec->xdg_surface;
    struct wl_surface *surface = rec->surface;
    rec->decoration = NULL;
    rec->toplevel = NULL;
    rec->popup = NULL;
    rec->xdg_surface = NULL;
    rec->surface = NULL;
    rec->generation = 0;
    rec->mapped = 0;
    if (decoration) zxdg_toplevel_decoration_v1_destroy(decoration);
    if (toplevel) xdg_toplevel_destroy(toplevel);
    if (popup) xdg_popup_destroy(popup);
    if (xdg_surface) xdg_surface_destroy(xdg_surface);
    if (surface) wl_surface_destroy(surface);
}

/* ---- announce ---------------------------------------------------------------------------------- */

/** @brief Decoded WindowAnnounce. Borrowed strings are copied into bounded storage. */
typedef struct announce_t {
    uint64_t window;
    uint64_t generation;
    uint32_t role;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint64_t parent;
    uint64_t parent_generation;
    bool has_parent;
    bool has_title;
    char title[InteropMaxTitleBytes + 1];
    char app_key[InteropMaxAppKeyBytes + 1];
    bool has_min_size;
    uint32_t min_width;
    uint32_t min_height;
    uint32_t dpi;
    uint32_t state;
    bool has_serial;
    uint32_t serial;
} announce_t;

/** @brief Read a little-endian u32 from a validated rect or min-size value. */
static uint32_t read_u32le(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/** @brief Field helper: 0 when found (value set), 1 when absent, -1 when malformed. */
static int field_u64(const uint8_t *payload, size_t length, uint16_t tag, uint64_t *value) {
    return interop_field_uint(payload, length, tag, 8u, value);
}

static int field_u32(const uint8_t *payload, size_t length, uint16_t tag, uint32_t *value) {
    uint64_t wide = 0;
    int result = interop_field_uint(payload, length, tag, 4u, &wide);
    if (result == 0) *value = (uint32_t)wide;
    return result;
}

/** @brief Copy a string field into bounded storage; a longer value is malformed. */
static int field_string(const uint8_t *payload, size_t length, uint16_t tag, char *out, size_t capacity, bool *present) {
    const uint8_t *value = NULL;
    uint16_t value_length = 0;
    int result = interop_field_get(payload, length, tag, 0u, &value, &value_length);
    if (result < 0) return -1;
    *present = result == 0;
    if (result == 1) return 0;
    if (value_length >= capacity) return -1;
    memcpy(out, value, value_length);
    out[value_length] = '\0';
    return 0;
}

/** @brief Decode the WindowAnnounce fields the realization needs. Returns -1 for an unreadable field. */
static int decode_announce(const uint8_t *payload, size_t length, announce_t *out) {
    memset(out, 0, sizeof *out);
    uint64_t wide = 0;
    if (field_u64(payload, length, InteropTagWindow, &out->window) != 0) return -1;
    if (field_u64(payload, length, InteropTagGeneration, &out->generation) != 0) return -1;
    if (interop_field_uint(payload, length, InteropTagRole, 1u, &wide) != 0) return -1;
    out->role = (uint32_t)wide;
    int parent_result = field_u64(payload, length, InteropTagParent, &out->parent);
    int parent_generation_result = field_u64(payload, length, InteropTagParentGeneration, &out->parent_generation);
    if (parent_result < 0 || parent_generation_result < 0) return -1;
    /* A parent is named by a nonzero pair; a zero parent means none and then its generation must be zero too. */
    out->has_parent = parent_result == 0 && out->parent != 0;
    if (out->has_parent != (parent_generation_result == 0 && out->parent_generation != 0)) return -1;
    if (!out->has_parent && out->parent_generation != 0) return -1;
    const uint8_t *rect = NULL;
    uint16_t rect_length = 0;
    if (interop_field_get(payload, length, InteropTagRect, 0u, &rect, &rect_length) != 0 || rect_length != 16u) return -1;
    out->x = (int32_t)read_u32le(rect);
    out->y = (int32_t)read_u32le(rect + 4);
    out->width = read_u32le(rect + 8);
    out->height = read_u32le(rect + 12);
    bool present = false;
    if (field_string(payload, length, InteropTagTitle, out->title, sizeof out->title, &present) != 0) return -1;
    out->has_title = present;
    if (field_string(payload, length, InteropTagAppKey, out->app_key, sizeof out->app_key, &present) != 0 || !present) return -1;
    const uint8_t *min_size = NULL;
    uint16_t min_length = 0;
    int min_result = interop_field_get(payload, length, InteropTagMinSize, 0u, &min_size, &min_length);
    if (min_result < 0 || (min_result == 0 && min_length != 8u)) return -1;
    if (min_result == 0) {
        out->has_min_size = true;
        out->min_width = read_u32le(min_size);
        out->min_height = read_u32le(min_size + 4);
    }
    if (field_u32(payload, length, InteropTagDpi, &out->dpi) != 0) return -1;
    if (field_u32(payload, length, InteropTagState, &out->state) != 0) return -1;
    int serial_result = field_u32(payload, length, InteropTagSerial, &out->serial);
    if (serial_result < 0) return -1;
    out->has_serial = serial_result == 0;
    return 0;
}

/** @brief Protocol app id: "waddle.<device>.<app_key>", or "waddle.<device>.uac" for consent dialogs. */
static bool build_app_id(const host_wayland_t *ctx, const announce_t *req, char *out, size_t capacity) {
    int written = req->role == ROLE_UAC
        ? snprintf(out, capacity, "waddle.%s.uac", ctx->config.device)
        : snprintf(out, capacity, "waddle.%s.%s", ctx->config.device, req->app_key);
    return written > 0 && (size_t)written < capacity;
}

/**
 * @brief Create the toplevel objects of one record and commit the initial buffer-less surface.
 *
 * Creation order: wl_surface, xdg_surface, xdg_toplevel. Every object is recorded before its listener is
 * registered, so a failure at any step leaves exactly the created objects for destroy_record_objects.
 * app_id is set before the first commit and never again.
 */
static int realize_toplevel(host_wayland_t *ctx, host_wayland_record_t *rec, const announce_t *req, const char *app_id) {
    rec->surface = wl_compositor_create_surface(ctx->config.compositor);
    if (rec->surface == NULL) return -1;
    rec->xdg_surface = xdg_wm_base_get_xdg_surface(ctx->config.wm_base, rec->surface);
    if (rec->xdg_surface == NULL) return -1;
    if (xdg_surface_add_listener(rec->xdg_surface, &surface_listener, &rec->bindings[HostWaylandKindXdgSurface]) != 0) return -1;
    rec->toplevel = xdg_surface_get_toplevel(rec->xdg_surface);
    if (rec->toplevel == NULL) return -1;
    if (xdg_toplevel_add_listener(rec->toplevel, &toplevel_listener, &rec->bindings[HostWaylandKindToplevel]) != 0) return -1;

    xdg_toplevel_set_app_id(rec->toplevel, app_id);
    if (req->has_title) xdg_toplevel_set_title(rec->toplevel, req->title);
    if (req->role == ROLE_DIALOG && req->has_parent) {
        host_wayland_record_t *parent = find_record(ctx, req->parent, req->parent_generation);
        if (parent != NULL && (parent->role == ROLE_TOPLEVEL || parent->role == ROLE_DIALOG) && parent->toplevel != NULL) {
            xdg_toplevel_set_parent(rec->toplevel, parent->toplevel);
        }
    }
    if (req->has_min_size) {
        xdg_toplevel_set_min_size(rec->toplevel, (int32_t)ceil_div_scale(req->min_width, ctx->scale), (int32_t)ceil_div_scale(req->min_height, ctx->scale));
    }
    wl_surface_commit(rec->surface);
    return 0;
}

/** @brief Give the window table the window and roll back on any failure: no partial window is published. */
static host_wayland_status_t fail_realization(host_wayland_t *ctx, host_wayland_record_t *rec, uint64_t window, uint64_t generation) {
    destroy_record_objects(rec);
    host_window_ref_t id;
    id.window = window;
    id.generation = generation;
    host_window_ref_t removed[HostWindowMapMaxOutput];
    uint32_t count = 0;
    (void)host_window_map_retire(&ctx->map, id, removed, HostWindowMapMaxOutput, &count);
    emit_error(ctx, 0x0100u, InteropErrorUnexpected, window, generation);
    return HostWaylandRealizeFailed;
}

static host_wayland_status_t handle_announce(host_wayland_t *ctx, const uint8_t *payload, size_t length, uint64_t now_ms) {
    (void)now_ms;
    announce_t req;
    if (decode_announce(payload, length, &req) != 0) return HostWaylandMalformedFrame;
    if (role_is_popup(req.role)) return HostWaylandUnsupportedType;
    if (!role_is_toplevel(req.role)) return HostWaylandMalformedFrame;
    char app_id[7 + 64 + 1 + InteropMaxAppKeyBytes + 1];
    if (!build_app_id(ctx, &req, app_id, sizeof app_id)) return HostWaylandMalformedFrame;

    host_window_ref_t window_id;
    window_id.window = req.window;
    window_id.generation = req.generation;
    host_window_ref_t parent_id;
    parent_id.window = req.has_parent ? req.parent : 0;
    parent_id.generation = req.has_parent ? req.parent_generation : 0;
    host_window_map_announce_result_t result = host_window_map_announce(&ctx->map, window_id, parent_id, (uint8_t)req.role);
    if (result.status != HostWindowMapRealize) {
        emit_error(ctx, 0x0100u, host_window_map_announce_error_code(result.status), req.window, req.generation);
        return HostWaylandRefused;
    }

    host_wayland_record_t *rec = &ctx->records[result.slot];
    memset(rec, 0, sizeof *rec);
    rec->window = req.window;
    rec->generation = req.generation;
    rec->parent = parent_id.window;
    rec->parent_generation = parent_id.generation;
    rec->role = (uint8_t)req.role;
    rec->guest_x = req.x;
    rec->guest_y = req.y;
    rec->guest_width = req.width;
    rec->guest_height = req.height;
    rec->scale = ctx->scale;
    rec->title_length = req.has_title ? (uint16_t)strlen(req.title) : 0;
    if (req.has_title) memcpy(rec->title, req.title, rec->title_length + 1u);
    for (uint32_t kind = 0; kind < HostWaylandKindCount; ++kind) {
        rec->bindings[kind].owner = ctx;
        rec->bindings[kind].generation = req.generation;
        rec->bindings[kind].slot = result.slot;
        rec->bindings[kind].kind = kind;
    }
    if (realize_toplevel(ctx, rec, &req, app_id) != 0) return fail_realization(ctx, rec, req.window, req.generation);
    return HostWaylandOk;
}

/* ---- frames ------------------------------------------------------------------------------------ */

host_wayland_status_t host_wayland_windows_handle_frame(host_wayland_t *ctx, const uint8_t *frame, size_t length, uint64_t now_ms) {
    if (ctx == NULL || frame == NULL) return HostWaylandInvalidArgument;
    if (!ctx->initialized) return HostWaylandNotInitialized;
    interop_header_t header;
    if (interop_header_decode(frame, length, &header) != InteropErrorNone) return HostWaylandMalformedFrame;
    if (length < InteropHeaderBytes || (size_t)header.length > length - InteropHeaderBytes) return HostWaylandMalformedFrame;
    const uint8_t *payload = frame + InteropHeaderBytes;
    if (interop_payload_validate(header.type, InteropFromGuest, payload, header.length) != InteropErrorNone) return HostWaylandMalformedFrame;
    switch (header.type) {
    case InteropMsgWindowAnnounce:
        return handle_announce(ctx, payload, header.length, now_ms);
    default:
        return HostWaylandUnsupportedType;
    }
}
