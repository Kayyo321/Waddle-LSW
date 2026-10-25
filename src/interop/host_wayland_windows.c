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

/* Declared here because the popup event handlers use it before its definition below. */
static void grab_pop_from(host_wayland_t *ctx, const host_wayland_record_t *rec);

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

/** @brief Advance the caller's monotonic clock. A clock that moved backwards makes stored presses untrustworthy. */
static void observe_clock(host_wayland_t *ctx, uint64_t now_ms) {
    if (now_ms < ctx->last_clock_ms) memset(ctx->presses, 0, sizeof ctx->presses);
    ctx->last_clock_ms = now_ms;
}

/** @brief Remove entries whose validity window has passed. */
static void press_expire(host_wayland_t *ctx, uint64_t now_ms) {
    for (size_t index = 0; index < PRESS_COUNT; ++index) {
        host_wayland_press_t *press = &ctx->presses[index];
        if (press->live && now_ms >= press->recorded_ms && now_ms - press->recorded_ms > HostWaylandPressExpiryMs) {
            press->live = 0;
        }
    }
}

/** @brief Whether `seat` is registered: recorded by a press and not forgotten since. */
static bool seat_registered(const host_wayland_t *ctx, const struct wl_seat *seat) {
    if (seat == NULL) return false;
    for (size_t index = 0; index < HostWaylandMaxSeats; ++index) {
        if (ctx->seats[index] == seat) return true;
    }
    return false;
}

/** @brief Register `seat` if it is not registered yet. Returns HostWaylandSeatTableFull when no slot is free. */
static host_wayland_status_t seat_register(host_wayland_t *ctx, struct wl_seat *seat) {
    if (seat_registered(ctx, seat)) return HostWaylandOk;
    for (size_t index = 0; index < HostWaylandMaxSeats; ++index) {
        if (ctx->seats[index] == NULL) {
            ctx->seats[index] = seat;
            return HostWaylandOk;
        }
    }
    return HostWaylandSeatTableFull;
}

/** @brief Whether a press kind is one the table records. */
static bool press_kind_valid(const host_wayland_press_input_t *input) {
    if (input->kind == HostWaylandPressPointer) return input->button != 0;
    return input->kind == HostWaylandPressTouch || input->kind == HostWaylandPressKey;
}

/** @brief Live entry holding protocol serial T, or NULL. Expiry is not applied here. */
static host_wayland_press_t *press_by_serial(host_wayland_t *ctx, uint32_t protocol_serial) {
    for (size_t index = 0; index < PRESS_COUNT; ++index) {
        host_wayland_press_t *press = &ctx->presses[index];
        if (press->live && press->protocol_serial == protocol_serial) return press;
    }
    return NULL;
}

host_wayland_status_t host_wayland_windows_record_press(host_wayland_t *ctx, const host_wayland_press_input_t *input) {
    if (!context_ready(ctx)) return HostWaylandNotInitialized;
    if (input == NULL || input->protocol_serial == 0 || input->seat == NULL || input->window == 0 || input->generation == 0) return HostWaylandInvalidArgument;
    if (!press_kind_valid(input)) return HostWaylandInvalidArgument;
    observe_clock(ctx, input->now_ms);
    press_expire(ctx, input->now_ms);
    if (press_by_serial(ctx, input->protocol_serial) != NULL) return HostWaylandDuplicateSerial;
    host_wayland_press_t *slot = NULL;
    for (size_t index = 0; index < PRESS_COUNT && slot == NULL; ++index) {
        if (!ctx->presses[index].live) slot = &ctx->presses[index];
    }
    /* Every entry is live. Evicting one would lose a grab a compositor may still need, so fail instead. */
    if (slot == NULL) return HostWaylandPressTableFull;
    host_wayland_status_t status = seat_register(ctx, input->seat);
    if (status != HostWaylandOk) return status;
    slot->protocol_serial = input->protocol_serial;
    slot->wayland_serial = input->wayland_serial;
    slot->seat = input->seat;
    slot->window = input->window;
    slot->generation = input->generation;
    slot->recorded_ms = input->now_ms;
    slot->lease_epoch = input->lease_epoch;
    slot->kind = input->kind;
    slot->button = input->button;
    slot->live = 1;
    return HostWaylandOk;
}

void host_wayland_windows_release_press(host_wayland_t *ctx, uint32_t protocol_serial) {
    if (!context_ready(ctx)) return;
    for (size_t index = 0; index < PRESS_COUNT; ++index) {
        host_wayland_press_t *press = &ctx->presses[index];
        if (press->live && press->protocol_serial == protocol_serial) press->live = 0;
    }
}

void host_wayland_windows_drop_presses(host_wayland_t *ctx, uint64_t lease_epoch) {
    if (!context_ready(ctx)) return;
    for (size_t index = 0; index < PRESS_COUNT; ++index) {
        host_wayland_press_t *press = &ctx->presses[index];
        if (press->live && press->lease_epoch != lease_epoch) press->live = 0;
    }
}

void host_wayland_windows_forget_seat(host_wayland_t *ctx, const struct wl_seat *seat) {
    if (!context_ready(ctx) || seat == NULL) return;
    for (size_t index = 0; index < HostWaylandMaxSeats; ++index) {
        if (ctx->seats[index] == seat) ctx->seats[index] = NULL;
    }
    for (size_t index = 0; index < PRESS_COUNT; ++index) {
        host_wayland_press_t *press = &ctx->presses[index];
        if (press->live && press->seat == seat) press->live = 0;
    }
    for (size_t index = 0; index < HostWindowMapMaxRealized; ++index) {
        host_wayland_record_t *rec = &ctx->records[index];
        if (rec->generation != 0 && rec->grabbed && rec->grab_seat == seat) rec->grabbed = 0;
    }
}

/** @brief Live entry for protocol serial T at `now_ms`, or NULL. */
static host_wayland_press_t *press_find(host_wayland_t *ctx, uint32_t protocol_serial, uint64_t now_ms) {
    press_expire(ctx, now_ms);
    return press_by_serial(ctx, protocol_serial);
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

/* Decoration modes (zxdg_toplevel_decoration_v1.mode). */
#define DECORATION_CLIENT ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
#define DECORATION_SERVER ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE

/** @brief Destroy the decoration object of a record, if any. The record forgets it first. */
static void destroy_decoration(host_wayland_record_t *rec) {
    struct zxdg_toplevel_decoration_v1 *decoration = rec->decoration;
    rec->decoration = NULL;
    if (decoration) zxdg_toplevel_decoration_v1_destroy(decoration);
}

/**
 * @brief xdg_surface.configure: acknowledge the serial, then apply the staged decoration mode (2.6).
 *
 * The first configuration of a decorated surface must carry a mode, otherwise the window stays unmapped until
 * a later configuration does. Server-side mode is acknowledged first and then the decoration is destroyed; the
 * surface keeps its guest frame. Configurations after that reuse the last mode.
 */
static void on_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
    host_wayland_record_t *rec = resolve(data, xdg_surface);
    if (rec == NULL || rec->dismissed) return;
    if (rec->decoration != NULL && rec->decoration_pending != 0) {
        rec->decoration_applied = rec->decoration_pending;
        rec->decoration_pending = 0;
    }
    if (rec->decoration != NULL && rec->decoration_applied == 0) {
        /* No mode yet: acknowledge so the compositor progresses, but keep buffers ineligible. */
        xdg_surface_ack_configure(xdg_surface, serial);
        return;
    }
    xdg_surface_ack_configure(xdg_surface, serial);
    if (rec->decoration != NULL && rec->decoration_applied == DECORATION_SERVER) destroy_decoration(rec);
    if (!rec->decoration_invalid) rec->mapped = 1;
}

/**
 * @brief zxdg_toplevel_decoration_v1.configure: stage the mode for the next surface configuration.
 *
 * Any mode other than client or server terminates delivery for the window: it is never mapped.
 */
static void on_decoration_configure(void *data, struct zxdg_toplevel_decoration_v1 *decoration, uint32_t mode) {
    host_wayland_record_t *rec = resolve(data, decoration);
    if (rec == NULL) return;
    if (mode == DECORATION_CLIENT || mode == DECORATION_SERVER) {
        rec->decoration_pending = mode;
        return;
    }
    host_wayland_t *ctx = ((const host_wayland_binding_t *)data)->owner;
    rec->decoration_invalid = 1;
    rec->mapped = 0;
    emit_error(ctx, 0x0100u, InteropErrorProtocolViolation, rec->window, rec->generation);
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

static const struct zxdg_toplevel_decoration_v1_listener decoration_listener = {
    .configure = on_decoration_configure,
};

/* xdg_popup.configure: the compositor's position, converted to guest pixels (PopupRepositioned). A dismissed
 * popup accepts no further configure. */
static void on_popup_configure(void *data, struct xdg_popup *popup, int32_t x, int32_t y, int32_t width, int32_t height) {
    (void)width;
    (void)height;
    host_wayland_record_t *rec = resolve(data, popup);
    if (rec == NULL || rec->dismissed) return;
    host_wayland_t *ctx = ((const host_wayland_binding_t *)data)->owner;
    host_popup_rect_t guest;
    guest.x = rec->guest_x;
    guest.y = rec->guest_y;
    guest.width = rec->guest_width;
    guest.height = rec->guest_height;
    host_popup_rect_t repositioned;
    if (host_popup_positioner_reposition(&guest, rec->scale, &rec->request, x, y, &repositioned) != HostPopupOk) {
        emit_error(ctx, 0x0103u, InteropErrorUnexpected, rec->window, rec->generation);
        return;
    }
    if (ctx->config.callbacks.on_popup_repositioned) {
        ctx->config.callbacks.on_popup_repositioned(ctx->config.callbacks.user, rec->window, rec->generation, repositioned.x, repositioned.y, repositioned.width, repositioned.height);
    }
}

/* xdg_popup.popup_done: the compositor dismissed the popup. The window stays realized until retired. */
static void on_popup_done(void *data, struct xdg_popup *popup) {
    host_wayland_record_t *rec = resolve(data, popup);
    if (rec == NULL || rec->dismissed) return;
    host_wayland_t *ctx = ((const host_wayland_binding_t *)data)->owner;
    host_window_ref_t id;
    id.window = rec->window;
    id.generation = rec->generation;
    if (host_window_map_mark_dismissed(&ctx->map, id) != HostWindowMapDismissed) return;
    rec->dismissed = 1;
    grab_pop_from(ctx, rec);
    if (ctx->config.callbacks.on_popup_dismissed) ctx->config.callbacks.on_popup_dismissed(ctx->config.callbacks.user, rec->window, rec->generation);
}

/* xdg_popup.repositioned: the compositor applied the reposition with this token. Only the current token completes
 * it; an older token is a stale acknowledgement and is ignored. */
static void on_popup_repositioned(void *data, struct xdg_popup *popup, uint32_t token) {
    host_wayland_record_t *rec = resolve(data, popup);
    if (rec == NULL || token != rec->reposition_token) return;
    rec->reposition_pending = 0;
}

static const struct xdg_popup_listener popup_listener = {
    .configure = on_popup_configure,
    .popup_done = on_popup_done,
    .repositioned = on_popup_repositioned,
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
    struct xdg_positioner *positioner = rec->positioner;
    struct xdg_surface *xdg_surface = rec->xdg_surface;
    struct wl_surface *surface = rec->surface;
    rec->decoration = NULL;
    rec->toplevel = NULL;
    rec->popup = NULL;
    rec->positioner = NULL;
    rec->xdg_surface = NULL;
    rec->surface = NULL;
    rec->generation = 0;
    rec->mapped = 0;
    rec->grabbed = 0;
    if (decoration) zxdg_toplevel_decoration_v1_destroy(decoration);
    if (toplevel) xdg_toplevel_destroy(toplevel);
    if (popup) xdg_popup_destroy(popup);
    if (positioner) xdg_positioner_destroy(positioner);
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

/* ---- popups (8.44, 3.6.2) -------------------------------------------------------------------- */

/** @brief Window pair of the toplevel a popup chain belongs to: its ancestors followed until a non-popup. */
static host_window_ref_t root_of(const host_wayland_t *ctx, host_window_ref_t id) {
    for (unsigned step = 0; step <= HostWindowMapMaxRealized; ++step) {
        host_window_entry_t entry;
        if (!host_window_map_lookup(&ctx->map, id, &entry)) break;
        if (!role_is_popup(entry.role)) return id;
        id.window = entry.parent.window;
        id.generation = entry.parent.generation;
    }
    return id;
}

/** @brief Topmost grabbing popup of a seat: the live grabbed record with the highest grab sequence, or NULL. */
static host_wayland_record_t *seat_top_grab(host_wayland_t *ctx, const struct wl_seat *seat) {
    host_wayland_record_t *top = NULL;
    for (size_t index = 0; index < HostWindowMapMaxRealized; ++index) {
        host_wayland_record_t *rec = &ctx->records[index];
        if (rec->generation == 0 || !rec->grabbed || rec->grab_seat != seat) continue;
        if (top == NULL || rec->grab_sequence > top->grab_sequence) top = rec;
    }
    return top;
}

/**
 * @brief Drop the grab of `rec` and every grab stacked above it on the same seat (dismiss and retire, 3.6.2).
 * A popup above a dismissed one loses its eligibility to grab, and the parent below it becomes topmost again.
 */
static void grab_pop_from(host_wayland_t *ctx, const host_wayland_record_t *rec) {
    if (!rec->grabbed) return;
    for (size_t index = 0; index < HostWindowMapMaxRealized; ++index) {
        host_wayland_record_t *other = &ctx->records[index];
        if (other->grabbed && other->grab_seat == rec->grab_seat && other->grab_sequence >= rec->grab_sequence) other->grabbed = 0;
    }
}

/**
 * @brief Grab for a Menu whose trigger press is live (3.6.2): same root family, not expired, serial T matches, and the
 * seat is grab-eligible. A popup on a toplevel grabs only while no grab is active on its seat. A popup on a popup
 * grabs only when its parent is the topmost grabbing popup of that seat; any other case is created ungrabbed,
 * because the compositor treats a grab from a non-topmost parent as invalid_grab.
 */
static void maybe_grab(host_wayland_t *ctx, host_wayland_record_t *rec, const announce_t *req, uint64_t now_ms) {
    if (rec->role != ROLE_MENU || !req->has_serial) return;
    host_wayland_press_t *press = press_find(ctx, req->serial, now_ms);
    /* A press whose seat was forgotten is never a grab: the seat object may already be destroyed. */
    if (press == NULL || !seat_registered(ctx, press->seat)) return;
    host_window_ref_t trigger_root = root_of(ctx, (host_window_ref_t){press->window, press->generation});
    host_window_ref_t popup_root = root_of(ctx, (host_window_ref_t){req->parent, req->parent_generation});
    if (trigger_root.window != popup_root.window || trigger_root.generation != popup_root.generation) return;
    host_wayland_record_t *parent = find_record(ctx, req->parent, req->parent_generation);
    if (parent == NULL) return;
    if (role_is_popup(parent->role)) {
        if (!parent->grabbed || parent->dismissed || seat_top_grab(ctx, press->seat) != parent) return;
    } else if (seat_top_grab(ctx, press->seat) != NULL) {
        return;
    }
    xdg_popup_grab(rec->popup, press->seat, press->wayland_serial);
    rec->grabbed = 1;
    rec->grab_seat = press->seat;
    rec->grab_sequence = ++ctx->grab_counter;
}

/** @brief Logical size of a direct parent, as the positioner clamp uses it (8.44): ceil of the guest size, at least 1. */
static void parent_logical_size(const host_wayland_record_t *parent, uint32_t scale, uint32_t *width, uint32_t *height) {
    *width = ceil_div_scale(parent->guest_width, scale);
    *height = ceil_div_scale(parent->guest_height, scale);
    if (*width == 0) *width = 1;
    if (*height == 0) *height = 1;
}

/** @brief Apply the pure request to a positioner. set_reactive needs xdg_wm_base version 3. */
static void set_positioner(const host_wayland_t *ctx, struct xdg_positioner *positioner, const host_popup_request_t *request) {
    xdg_positioner_set_size(positioner, (int32_t)request->width, (int32_t)request->height);
    xdg_positioner_set_anchor_rect(positioner, request->anchor_x, request->anchor_y, (int32_t)request->anchor_width, (int32_t)request->anchor_height);
    xdg_positioner_set_anchor(positioner, request->anchor);
    xdg_positioner_set_gravity(positioner, request->gravity);
    xdg_positioner_set_offset(positioner, request->offset_x, request->offset_y);
    xdg_positioner_set_constraint_adjustment(positioner, request->constraint_adjustment);
    if (ctx->config.wm_base_version >= 3) xdg_positioner_set_reactive(positioner);
}

/**
 * @brief Create the popup objects of one record and commit the initial buffer-less surface.
 *
 * The positioner is built from the pure positioner values, passed to get_popup under the DIRECT parent's
 * xdg_surface, destroyed as soon as the popup exists, and the grab (if any) precedes the first commit.
 */
static int realize_popup(host_wayland_t *ctx, host_wayland_record_t *rec, const announce_t *req, host_wayland_record_t *parent, uint64_t now_ms) {
    rec->surface = wl_compositor_create_surface(ctx->config.compositor);
    if (rec->surface == NULL) return -1;
    rec->xdg_surface = xdg_wm_base_get_xdg_surface(ctx->config.wm_base, rec->surface);
    if (rec->xdg_surface == NULL) return -1;
    if (xdg_surface_add_listener(rec->xdg_surface, &surface_listener, &rec->bindings[HostWaylandKindXdgSurface]) != 0) return -1;

    rec->positioner = xdg_wm_base_create_positioner(ctx->config.wm_base);
    if (rec->positioner == NULL) return -1;
    set_positioner(ctx, rec->positioner, &rec->request);

    rec->popup = xdg_surface_get_popup(rec->xdg_surface, parent->xdg_surface, rec->positioner);
    if (rec->popup == NULL) return -1;
    if (xdg_popup_add_listener(rec->popup, &popup_listener, &rec->bindings[HostWaylandKindPopup]) != 0) return -1;
    struct xdg_positioner *positioner = rec->positioner;
    rec->positioner = NULL;
    xdg_positioner_destroy(positioner);

    maybe_grab(ctx, rec, req, now_ms);
    wl_surface_commit(rec->surface);
    return 0;
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
    /* Windows frames are drawn by the guest, so ask for client-side mode before the first commit (2.6). A
     * compositor without the manager simply gets no decoration object. Creation or listener failure rolls back. */
    if (ctx->config.decoration_manager != NULL) {
        rec->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(ctx->config.decoration_manager, rec->toplevel);
        if (rec->decoration == NULL) return -1;
        if (zxdg_toplevel_decoration_v1_add_listener(rec->decoration, &decoration_listener, &rec->bindings[HostWaylandKindDecoration]) != 0) return -1;
        zxdg_toplevel_decoration_v1_set_mode(rec->decoration, DECORATION_CLIENT);
    }
    wl_surface_commit(rec->surface);
    return 0;
}

/** @brief Give the window table the window and roll back on any failure: no partial window is published. */
static host_wayland_status_t fail_realization(host_wayland_t *ctx, host_wayland_record_t *rec, uint64_t window, uint64_t generation, uint32_t error_code) {
    destroy_record_objects(rec);
    host_window_ref_t id;
    id.window = window;
    id.generation = generation;
    host_window_ref_t removed[HostWindowMapMaxOutput];
    uint32_t count = 0;
    (void)host_window_map_retire(&ctx->map, id, removed, HostWindowMapMaxOutput, &count);
    emit_error(ctx, 0x0100u, error_code, window, generation);
    return HostWaylandRealizeFailed;
}

static host_wayland_status_t handle_announce(host_wayland_t *ctx, const uint8_t *payload, size_t length, uint64_t now_ms) {
    announce_t req;
    if (decode_announce(payload, length, &req) != 0) return HostWaylandMalformedFrame;
    if (!role_is_popup(req.role) && !role_is_toplevel(req.role)) return HostWaylandMalformedFrame;
    char app_id[7 + 64 + 1 + InteropMaxAppKeyBytes + 1];
    if (role_is_toplevel(req.role) && !build_app_id(ctx, &req, app_id, sizeof app_id)) return HostWaylandMalformedFrame;

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
    if (role_is_popup(req.role)) {
        /* The popup geometry is relative to the direct parent's logical size (8.44). */
        host_wayland_record_t *parent = find_record(ctx, req.parent, req.parent_generation);
        /* The table only realizes a popup under a realized parent, so this is a defensive refusal. */
        if (parent == NULL) return fail_realization(ctx, rec, req.window, req.generation, InteropErrorStaleWindow);
        uint32_t parent_width = 0;
        uint32_t parent_height = 0;
        parent_logical_size(parent, ctx->scale, &parent_width, &parent_height);
        host_popup_rect_t guest;
        guest.x = req.x;
        guest.y = req.y;
        guest.width = req.width;
        guest.height = req.height;
        if (host_popup_positioner_request(&guest, ctx->scale, parent_width, parent_height, &rec->request) != HostPopupOk) {
            return fail_realization(ctx, rec, req.window, req.generation, InteropErrorMalformedFrame);
        }
        if (realize_popup(ctx, rec, &req, parent, now_ms) != 0) return fail_realization(ctx, rec, req.window, req.generation, InteropErrorUnexpected);
        return HostWaylandOk;
    }
    if (realize_toplevel(ctx, rec, &req, app_id) != 0) return fail_realization(ctx, rec, req.window, req.generation, InteropErrorUnexpected);
    return HostWaylandOk;
}

/* ---- update and retire (4.3, 8.44, 3.2.8) ---------------------------------------------------------- */

/** @brief Decoded WindowUpdate. Optional members are present only when their field was sent. */
typedef struct update_t {
    uint64_t window;
    uint64_t generation;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    bool has_title;
    char title[InteropMaxTitleBytes + 1];
    bool has_parent;
    uint64_t parent;
    uint64_t parent_generation;
    bool has_min_size;
    uint32_t min_width;
    uint32_t min_height;
} update_t;

static int decode_update(const uint8_t *payload, size_t length, update_t *out) {
    memset(out, 0, sizeof *out);
    if (field_u64(payload, length, InteropTagWindow, &out->window) != 0) return -1;
    if (field_u64(payload, length, InteropTagGeneration, &out->generation) != 0) return -1;
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
    int parent_result = field_u64(payload, length, InteropTagParent, &out->parent);
    int parent_generation_result = field_u64(payload, length, InteropTagParentGeneration, &out->parent_generation);
    if (parent_result < 0 || parent_generation_result < 0) return -1;
    /* A parent is sent as a pair; a parent field without its generation, or the reverse, is malformed. */
    if ((parent_result == 0) != (parent_generation_result == 0)) return -1;
    out->has_parent = parent_result == 0;
    const uint8_t *min_size = NULL;
    uint16_t min_length = 0;
    int min_result = interop_field_get(payload, length, InteropTagMinSize, 0u, &min_size, &min_length);
    if (min_result < 0 || (min_result == 0 && min_length != 8u)) return -1;
    if (min_result == 0) {
        out->has_min_size = true;
        out->min_width = read_u32le(min_size);
        out->min_height = read_u32le(min_size + 4);
    }
    return 0;
}

/** @brief Record of a window that is realized exactly, or NULL. */
static host_wayland_record_t *exact_record(host_wayland_t *ctx, uint64_t window, uint64_t generation) {
    return find_record(ctx, window, generation);
}

/** @brief Mirror the record's parent onto the xdg_toplevel: set_parent for a toplevel or dialog parent, else unset. */
static void mirror_parent(host_wayland_t *ctx, host_wayland_record_t *rec) {
    host_wayland_record_t *target = find_record(ctx, rec->parent, rec->parent_generation);
    if (target != NULL && (target->role == ROLE_TOPLEVEL || target->role == ROLE_DIALOG) && target->toplevel != NULL) {
        xdg_toplevel_set_parent(rec->toplevel, target->toplevel);
    } else {
        xdg_toplevel_set_parent(rec->toplevel, NULL);
    }
}

/**
 * @brief Move a realized popup to a new guest rectangle (8.44). The new request is built in temporaries, the
 * positioner is created, and xdg_popup.reposition is sent with the next token. Only after all of that succeeds are
 * the rectangle, the request and the token stored, so a refusal leaves the record and the compositor unchanged.
 */
static host_wayland_status_t reposition_popup(host_wayland_t *ctx, host_wayland_record_t *rec, const update_t *req) {
    host_wayland_record_t *parent = find_record(ctx, rec->parent, rec->parent_generation);
    if (parent == NULL) {
        emit_error(ctx, 0x0101u, InteropErrorStaleWindow, rec->window, rec->generation);
        return HostWaylandRefused;
    }
    uint32_t parent_width = 0;
    uint32_t parent_height = 0;
    parent_logical_size(parent, rec->scale, &parent_width, &parent_height);
    host_popup_rect_t guest;
    guest.x = req->x;
    guest.y = req->y;
    guest.width = req->width;
    guest.height = req->height;
    host_popup_request_t request;
    if (host_popup_positioner_request(&guest, rec->scale, parent_width, parent_height, &request) != HostPopupOk) {
        emit_error(ctx, 0x0101u, InteropErrorMalformedFrame, rec->window, rec->generation);
        return HostWaylandRefused;
    }
    /* xdg_popup.reposition exists from xdg_wm_base version 3. An older compositor cannot move the popup. */
    if (ctx->config.wm_base_version < 3) {
        emit_error(ctx, 0x0101u, InteropErrorVersionMismatch, rec->window, rec->generation);
        return HostWaylandRefused;
    }
    struct xdg_positioner *positioner = xdg_wm_base_create_positioner(ctx->config.wm_base);
    if (positioner == NULL) {
        emit_error(ctx, 0x0101u, InteropErrorUnexpected, rec->window, rec->generation);
        return HostWaylandRefused;
    }
    set_positioner(ctx, positioner, &request);
    uint32_t token = rec->reposition_token + 1u;
    if (token == 0) token = 1u;
    xdg_popup_reposition(rec->popup, positioner, token);
    xdg_positioner_destroy(positioner);
    rec->guest_x = req->x;
    rec->guest_y = req->y;
    rec->guest_width = req->width;
    rec->guest_height = req->height;
    rec->request = request;
    rec->reposition_token = token;
    rec->reposition_pending = 1;
    return HostWaylandOk;
}

/**
 * @brief Apply one WindowUpdate, all-or-nothing (4.3). Every refusal happens before any field or Wayland request
 * changes: the table reparent is the only step that can refuse, so it runs first, and the rectangle, title, parent
 * mirror and minimum size are applied only after it succeeds.
 */
static host_wayland_status_t handle_update(host_wayland_t *ctx, const uint8_t *payload, size_t length) {
    update_t req;
    if (decode_update(payload, length, &req) != 0) return HostWaylandMalformedFrame;
    host_wayland_record_t *rec = exact_record(ctx, req.window, req.generation);
    if (rec == NULL) {
        emit_error(ctx, 0x0101u, InteropErrorStaleWindow, req.window, req.generation);
        return HostWaylandRefused;
    }
    bool parent_changes = req.has_parent && (req.parent != rec->parent || req.parent_generation != rec->parent_generation);
    if (role_is_popup(rec->role)) {
        /* A popup never changes parent. The table refuses it by role; this module refuses it first, with the same code. */
        if (parent_changes) {
            emit_error(ctx, 0x0101u, host_window_map_reparent_error_code(HostWindowMapReparentRefuseRole), rec->window, rec->generation);
            return HostWaylandRefused;
        }
        bool rect_changes = req.x != rec->guest_x || req.y != rec->guest_y || req.width != rec->guest_width || req.height != rec->guest_height;
        return rect_changes ? reposition_popup(ctx, rec, &req) : HostWaylandOk;
    }
    if (parent_changes) {
        host_window_ref_t child;
        child.window = rec->window;
        child.generation = rec->generation;
        host_window_ref_t new_parent;
        new_parent.window = req.parent;
        new_parent.generation = req.parent_generation;
        host_window_map_reparent_status_t status = host_window_map_reparent(&ctx->map, child, new_parent);
        if (status != HostWindowMapReparented) {
            emit_error(ctx, 0x0101u, host_window_map_reparent_error_code(status), rec->window, rec->generation);
            return HostWaylandRefused;
        }
        rec->parent = req.parent;
        rec->parent_generation = req.parent_generation;
    }
    if (req.has_title && (strlen(req.title) != rec->title_length || memcmp(req.title, rec->title, rec->title_length) != 0)) {
        xdg_toplevel_set_title(rec->toplevel, req.title);
        rec->title_length = (uint16_t)strlen(req.title);
        memcpy(rec->title, req.title, rec->title_length + 1u);
    }
    if (parent_changes) mirror_parent(ctx, rec);
    if (req.has_min_size) {
        xdg_toplevel_set_min_size(rec->toplevel, (int32_t)ceil_div_scale(req.min_width, ctx->scale), (int32_t)ceil_div_scale(req.min_height, ctx->scale));
    }
    rec->guest_x = req.x;
    rec->guest_y = req.y;
    rec->guest_width = req.width;
    rec->guest_height = req.height;
    return HostWaylandOk;
}

/** @brief Drop press entries whose trigger window is retired (3.6.2). */
static void press_drop_window(host_wayland_t *ctx, uint64_t window, uint64_t generation) {
    for (size_t index = 0; index < PRESS_COUNT; ++index) {
        host_wayland_press_t *press = &ctx->presses[index];
        if (press->live && press->window == window && press->generation == generation) press->live = 0;
    }
}

/** @brief Destroy one window's objects and drop its press entries. */
static void destroy_window(host_wayland_t *ctx, host_wayland_record_t *rec) {
    uint64_t window = rec->window;
    uint64_t generation = rec->generation;
    grab_pop_from(ctx, rec);
    destroy_record_objects(rec);
    press_drop_window(ctx, window, generation);
}

/** @brief Retire a window and its subtree: the table gives the children-first order; each window goes in order. */
static host_wayland_status_t retire_window(host_wayland_t *ctx, uint64_t window, uint64_t generation) {
    host_window_ref_t id;
    id.window = window;
    id.generation = generation;
    host_window_ref_t removed[HostWindowMapMaxOutput];
    uint32_t count = 0;
    host_window_map_retire_status_t status = host_window_map_retire(&ctx->map, id, removed, HostWindowMapMaxOutput, &count);
    if (status == HostWindowMapStale) return HostWaylandOk;
    if (status != HostWindowMapRetired) return HostWaylandMalformedFrame;
    for (uint32_t index = 0; index < count; ++index) {
        host_wayland_record_t *rec = find_record(ctx, removed[index].window, removed[index].generation);
        if (rec != NULL) destroy_window(ctx, rec);
    }
    return HostWaylandOk;
}

static host_wayland_status_t handle_retire(host_wayland_t *ctx, const uint8_t *payload, size_t length) {
    uint64_t window = 0;
    uint64_t generation = 0;
    if (field_u64(payload, length, InteropTagWindow, &window) != 0) return HostWaylandMalformedFrame;
    if (field_u64(payload, length, InteropTagGeneration, &generation) != 0) return HostWaylandMalformedFrame;
    return retire_window(ctx, window, generation);
}

/* ---- mapped parents (4.5) ------------------------------------------------------------------------ */

host_wayland_status_t host_wayland_windows_note_mapped(host_wayland_t *ctx, uint64_t window, uint64_t generation) {
    if (!context_ready(ctx)) return HostWaylandNotInitialized;
    if (window == 0 || generation == 0) return HostWaylandInvalidArgument;
    host_wayland_record_t *parent = find_record(ctx, window, generation);
    if (parent == NULL || parent->toplevel == NULL) return HostWaylandOk;
    for (size_t index = 0; index < HostWindowMapMaxRealized; ++index) {
        host_wayland_record_t *child = &ctx->records[index];
        if (child->generation == 0 || child->role != ROLE_DIALOG || child->toplevel == NULL) continue;
        if (child->parent != window || child->parent_generation != generation) continue;
        xdg_toplevel_set_parent(child->toplevel, parent->toplevel);
    }
    return HostWaylandOk;
}

/* ---- reset --------------------------------------------------------------------------------------- */

host_wayland_status_t host_wayland_windows_reset(host_wayland_t *ctx) {
    if (!context_ready(ctx)) return HostWaylandNotInitialized;
    host_window_ref_t removed[HostWindowMapMaxOutput];
    uint32_t count = 0;
    if (host_window_map_reset(&ctx->map, removed, HostWindowMapMaxOutput, &count) == HostWindowMapReset) {
        /* The table's teardown order: children before parents, roots newest first. */
        for (uint32_t index = 0; index < count; ++index) {
            host_wayland_record_t *rec = find_record(ctx, removed[index].window, removed[index].generation);
            if (rec != NULL) destroy_record_objects(rec);
        }
    } else {
        /* A corrupt table cannot give an order. Its records are still the objects to destroy: slot order, newest first. */
        for (size_t index = HostWindowMapMaxRealized; index > 0; --index) {
            host_wayland_record_t *rec = &ctx->records[index - 1];
            if (rec->generation != 0) destroy_record_objects(rec);
        }
    }
    memset(&ctx->map, 0, sizeof ctx->map);
    memset(ctx->presses, 0, sizeof ctx->presses);
    memset(ctx->records, 0, sizeof ctx->records);
    ctx->initialized = 0;
    return HostWaylandOk;
}

/* ---- frames ------------------------------------------------------------------------------------ */

host_wayland_status_t host_wayland_windows_handle_frame(host_wayland_t *ctx, const uint8_t *frame, size_t length, uint64_t now_ms) {
    if (ctx == NULL || frame == NULL) return HostWaylandInvalidArgument;
    if (!ctx->initialized) return HostWaylandNotInitialized;
    observe_clock(ctx, now_ms);
    interop_header_t header;
    if (interop_header_decode(frame, length, &header) != InteropErrorNone) return HostWaylandMalformedFrame;
    if (length < InteropHeaderBytes || (size_t)header.length > length - InteropHeaderBytes) return HostWaylandMalformedFrame;
    const uint8_t *payload = frame + InteropHeaderBytes;
    int validation = interop_payload_validate(header.type, InteropFromGuest, payload, header.length);
    if (validation == InteropErrorUnsupportedType) return HostWaylandUnsupportedType;
    if (validation != InteropErrorNone) return HostWaylandMalformedFrame;
    switch (header.type) {
    case InteropMsgWindowAnnounce:
        return handle_announce(ctx, payload, header.length, now_ms);
    case InteropMsgWindowUpdate:
        return handle_update(ctx, payload, header.length);
    case InteropMsgWindowRetire:
        return handle_retire(ctx, payload, header.length);
    default:
        return HostWaylandUnsupportedType;
    }
}
