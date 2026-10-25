/**
 * @file host_wayland_windows.c
 * @brief Host realization of interop windows as xdg-shell objects (see host_wayland_windows.h).
 *
 * Layering: frame decoding uses the codec in interop_protocol.h; the window table and the popup
 * geometry are the Zig modules behind host_window_map.h and host_popup_positioner.h. This file owns the
 * Wayland objects, the listener bindings and the press table. It never allocates.
 */
#include "host_wayland_windows.h"

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
