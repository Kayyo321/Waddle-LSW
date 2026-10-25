/**
 * @file session_input_wayland.c
 * @brief Wayland glue for the host keyboard handler (see session_input_wayland.h).
 *
 * Listener notes:
 * - wl_keyboard.enter and wl_keyboard.leave carry a serial, and key carries a time. The glue does
 *   not use either: the handler orders its own frames and has no timestamp input. The keys array of
 *   enter is ignored; held state is tracked by the handler from key transitions.
 * - wl_keyboard.keymap delivers an fd that the glue closes on every path, including rejected formats,
 *   zero and oversized sizes, and mmap failures.
 * - wl_keyboard.modifiers ignores the group field. The handler derives lock state from the masks.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

#include "keyboard_shortcuts_inhibit_client.h"
#include "keymap_names.h"
#include "session_input_wayland.h"

static void handle_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface,
    struct wl_array *keys);
static void handle_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface);
static void handle_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd, uint32_t size);
static void handle_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t key,
    uint32_t state);
static void handle_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t mods_depressed,
    uint32_t mods_latched, uint32_t mods_locked, uint32_t group);
static void handle_repeat_info(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay);
static void handle_inhibitor_active(void *data, struct zwp_keyboard_shortcuts_inhibitor_v1 *inhibitor);
static void handle_inhibitor_inactive(void *data, struct zwp_keyboard_shortcuts_inhibitor_v1 *inhibitor);

/** @brief Keyboard listener table; the order follows wl_keyboard_listener in the Wayland protocol. */
static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = handle_keymap,
    .enter = handle_enter,
    .leave = handle_leave,
    .key = handle_key,
    .modifiers = handle_modifiers,
    .repeat_info = handle_repeat_info,
};

/** @brief Inhibitor listener table for the active and inactive events. */
static const struct zwp_keyboard_shortcuts_inhibitor_v1_listener inhibitor_listener = {
    .active = handle_inhibitor_active,
    .inactive = handle_inhibitor_inactive,
};

/**
 * @brief Record a handler status and map it to the glue result.
 * @param[in,out] glue Initialized glue.
 * @param[in] status session_input_post_status_t returned by a handler call.
 * @return SessionInputWaylandOk for SessionInputPostOk, otherwise SessionInputWaylandHandlerRejected.
 */
static int record_status(session_input_wayland_t *glue, int status) {
    glue->last_post_status = status;
    return status == SessionInputPostOk ? SessionInputWaylandOk : SessionInputWaylandHandlerRejected;
}

/**
 * @brief Find the table entry in use for a surface.
 * @param[in] glue Initialized glue.
 * @param[in] surface wl_surface pointer, or NULL.
 * @return The entry, or NULL when the surface is NULL or unmapped.
 */
static session_input_wayland_entry_t *find_by_surface(session_input_wayland_t *glue, const struct wl_surface *surface) {
    if (surface == NULL) return NULL;
    for (size_t index = 0; index < MaxSurfaceEntries; index++) {
        if (glue->entries[index].surface == surface) return &glue->entries[index];
    }
    return NULL;
}

/**
 * @brief Find the table entry in use for a guest window identity.
 * @param[in] glue Initialized glue.
 * @param[in] window Guest window identity.
 * @param[in] generation Guest window incarnation.
 * @return The first matching entry, or NULL.
 */
static session_input_wayland_entry_t *find_by_identity(session_input_wayland_t *glue, uint64_t window, uint64_t generation) {
    for (size_t index = 0; index < MaxSurfaceEntries; index++) {
        session_input_wayland_entry_t *entry = &glue->entries[index];
        if (entry->surface != NULL && entry->window == window && entry->generation == generation) return entry;
    }
    return NULL;
}

/**
 * @brief Report a focus leave and forget the glue's focus.
 * @param[in,out] glue Initialized glue.
 * @return Glue status of the handler call.
 */
static int leave_focus(session_input_wayland_t *glue) {
    glue->focused_surface = NULL;
    return record_status(glue, session_input_post_focus_leave(glue->input));
}

/**
 * @brief Destroy the live inhibitor object and clear its record. Does nothing without an inhibitor.
 * @param[in,out] glue Initialized glue.
 */
static void destroy_inhibitor(session_input_wayland_t *glue) {
    if (glue->inhibitor == NULL) return;
    zwp_keyboard_shortcuts_inhibitor_v1_destroy(glue->inhibitor);
    glue->inhibitor = NULL;
    glue->inhibitor_window = 0;
    glue->inhibitor_generation = 0;
}

/**
 * @brief Create the inhibitor for an identity whose surface is mapped, and record it.
 * @param[in,out] glue Initialized glue.
 * @param[in] action Create action from the handler.
 */
static void create_inhibitor(session_input_wayland_t *glue, const waddle_keyboard_inhibit_action_t *action) {
    if (glue->manager == NULL) return;
    session_input_wayland_entry_t *entry = find_by_identity(glue, action->window, action->generation);
    if (entry == NULL) return;
    destroy_inhibitor(glue);
    struct zwp_keyboard_shortcuts_inhibitor_v1 *inhibitor =
        zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(glue->manager, entry->surface, glue->seat);
    if (inhibitor == NULL) return;
    if (zwp_keyboard_shortcuts_inhibitor_v1_add_listener(inhibitor, &inhibitor_listener, glue) != 0) {
        zwp_keyboard_shortcuts_inhibitor_v1_destroy(inhibitor);
        return;
    }
    glue->inhibitor = inhibitor;
    glue->inhibitor_window = action->window;
    glue->inhibitor_generation = action->generation;
}

/**
 * @brief Destroy the live inhibitor when the action names its identity.
 * @param[in,out] glue Initialized glue.
 * @param[in] action Destroy action from the handler.
 */
static void destroy_matching_inhibitor(session_input_wayland_t *glue, const waddle_keyboard_inhibit_action_t *action) {
    if (glue->inhibitor == NULL) return;
    if (glue->inhibitor_window != action->window || glue->inhibitor_generation != action->generation) return;
    destroy_inhibitor(glue);
}

/**
 * @brief Post an inhibitor active or inactive event for the live inhibitor's identity.
 * @param[in,out] glue Initialized glue.
 * @param[in] kind WaddleKeyboardInhibitEventInhibitorActive or Inactive.
 * @return Glue status of the handler call.
 */
static int post_inhibitor_event(session_input_wayland_t *glue, uint8_t kind) {
    waddle_keyboard_inhibit_event_t event;
    memset(&event, 0, sizeof event);
    event.kind = kind;
    event.window = glue->inhibitor_window;
    event.generation = (uint32_t)glue->inhibitor_generation;
    return record_status(glue, session_input_post_inhibit(glue->input, &event));
}

/* ------------------------------------------------------------------------- */
/* wl_keyboard listener callbacks                                             */
/* ------------------------------------------------------------------------- */

static void handle_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface,
    struct wl_array *keys) {
    (void)keyboard;
    (void)serial;
    (void)keys;
    session_input_wayland_t *glue = data;
    session_input_wayland_entry_t *entry = find_by_surface(glue, surface);
    if (entry == NULL) {
        if (glue->focused_surface != NULL) (void)leave_focus(glue);
        return;
    }
    glue->focused_surface = surface;
    (void)record_status(glue, session_input_post_focus_enter(glue->input, entry->window, entry->generation));
}

static void handle_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface) {
    (void)keyboard;
    (void)serial;
    (void)surface;
    (void)leave_focus(data);
}

static void handle_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd, uint32_t size) {
    (void)keyboard;
    session_input_wayland_t *glue = data;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0 || size > WaddleKeymapMaxBytes) {
        close(fd);
        return;
    }
    void *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return;
    }
    (void)record_status(glue, session_input_post_keymap(glue->input, (const uint8_t *)map, size));
    munmap(map, size);
    close(fd);
}

static void handle_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t key,
    uint32_t state) {
    (void)keyboard;
    (void)serial;
    (void)time;
    session_input_wayland_t *glue = data;
    uint8_t pressed = 0;
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        pressed = 1;
    } else if (state != WL_KEYBOARD_KEY_STATE_RELEASED) {
        return;
    }
    (void)record_status(glue, session_input_post_key(glue->input, key, pressed));
}

static void handle_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t mods_depressed,
    uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
    (void)keyboard;
    (void)serial;
    (void)group;
    session_input_wayland_t *glue = data;
    (void)record_status(glue, session_input_post_modifiers(glue->input, mods_depressed, mods_latched, mods_locked));
}

static void handle_repeat_info(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay) {
    (void)keyboard;
    session_input_wayland_t *glue = data;
    (void)record_status(glue, session_input_post_repeat_info(glue->input, rate, delay));
}

/* ------------------------------------------------------------------------- */
/* Inhibitor listener callbacks                                               */
/* ------------------------------------------------------------------------- */

static void handle_inhibitor_active(void *data, struct zwp_keyboard_shortcuts_inhibitor_v1 *inhibitor) {
    session_input_wayland_t *glue = data;
    if (glue->inhibitor != inhibitor) return;
    (void)post_inhibitor_event(glue, WaddleKeyboardInhibitEventInhibitorActive);
}

static void handle_inhibitor_inactive(void *data, struct zwp_keyboard_shortcuts_inhibitor_v1 *inhibitor) {
    session_input_wayland_t *glue = data;
    if (glue->inhibitor != inhibitor) return;
    (void)post_inhibitor_event(glue, WaddleKeyboardInhibitEventInhibitorInactive);
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                 */
/* ------------------------------------------------------------------------- */

void session_input_wayland_apply_inhibit(void *context, const waddle_keyboard_inhibit_result_t *result) {
    session_input_wayland_t *glue = context;
    for (uint8_t index = 0; index < result->count; index++) {
        const waddle_keyboard_inhibit_action_t *action = &result->actions[index];
        if (action->kind == WaddleKeyboardInhibitActionCreate) {
            create_inhibitor(glue, action);
        } else if (action->kind == WaddleKeyboardInhibitActionDestroy) {
            destroy_matching_inhibitor(glue, action);
        }
    }
}

int session_input_wayland_init(session_input_wayland_t *glue, session_input_t *input, uint8_t inhibit_mode,
    struct zwp_keyboard_shortcuts_inhibit_manager_v1 *manager_or_null, struct wl_seat *seat_or_null) {
    if (glue == NULL) return SessionInputWaylandInvalidArgument;
    if (manager_or_null != NULL && seat_or_null == NULL) return SessionInputWaylandInvalidArgument;
    if (session_input_init(input, inhibit_mode, session_input_wayland_apply_inhibit, glue) != SessionInputPostOk) {
        return SessionInputWaylandInvalidArgument;
    }
    memset(glue, 0, sizeof *glue);
    glue->input = input;
    glue->manager = manager_or_null;
    glue->seat = seat_or_null;
    return SessionInputWaylandOk;
}

void session_input_wayland_destroy(session_input_wayland_t *glue) {
    destroy_inhibitor(glue);
    memset(glue, 0, sizeof *glue);
}

int session_input_attach_keyboard(session_input_wayland_t *glue, struct wl_keyboard *keyboard) {
    if (glue == NULL || keyboard == NULL) return SessionInputWaylandInvalidArgument;
    if (glue->keyboard != NULL) return SessionInputWaylandAlreadyAttached;
    if (wl_keyboard_add_listener(keyboard, &keyboard_listener, glue) != 0) return SessionInputWaylandListenerFailed;
    glue->keyboard = keyboard;
    return SessionInputWaylandOk;
}

int session_input_detach_keyboard(session_input_wayland_t *glue) {
    if (glue == NULL) return SessionInputWaylandInvalidArgument;
    if (glue->keyboard == NULL) return SessionInputWaylandNotAttached;
    glue->keyboard = NULL;
    if (glue->focused_surface == NULL) return SessionInputWaylandOk;
    return leave_focus(glue);
}

int session_input_map_surface(session_input_wayland_t *glue, struct wl_surface *surface, uint64_t window, uint64_t generation) {
    if (glue == NULL || surface == NULL || window == 0 || generation == 0 || generation > UINT32_MAX) {
        return SessionInputWaylandInvalidArgument;
    }
    session_input_wayland_entry_t *entry = find_by_surface(glue, surface);
    if (entry == NULL) {
        for (size_t index = 0; index < MaxSurfaceEntries && entry == NULL; index++) {
            if (glue->entries[index].surface == NULL) entry = &glue->entries[index];
        }
        if (entry == NULL) return SessionInputWaylandTableFull;
        entry->surface = surface;
        entry->window = window;
        entry->generation = generation;
        return SessionInputWaylandOk;
    }
    if (entry->window == window && entry->generation == generation) return SessionInputWaylandOk;
    entry->window = window;
    entry->generation = generation;
    if (glue->focused_surface != surface) return SessionInputWaylandOk;
    return record_status(glue, session_input_post_focus_enter(glue->input, window, generation));
}

int session_input_unmap_surface(session_input_wayland_t *glue, struct wl_surface *surface) {
    if (glue == NULL || surface == NULL) return SessionInputWaylandInvalidArgument;
    session_input_wayland_entry_t *entry = find_by_surface(glue, surface);
    if (entry == NULL) return SessionInputWaylandOk;
    int status = SessionInputWaylandOk;
    if (glue->focused_surface == surface) status = leave_focus(glue);
    entry->surface = NULL;
    entry->window = 0;
    entry->generation = 0;
    return status;
}

int session_input_post_surface_state(session_input_wayland_t *glue, uint64_t window, uint64_t generation,
    bool fullscreen, bool pointer_lock) {
    if (glue == NULL || generation > UINT32_MAX) return SessionInputWaylandInvalidArgument;
    waddle_keyboard_inhibit_event_t event;
    memset(&event, 0, sizeof event);
    event.kind = WaddleKeyboardInhibitEventSurfaceState;
    event.fullscreen = fullscreen ? 1u : 0u;
    event.pointer_lock = pointer_lock ? 1u : 0u;
    event.window = window;
    event.generation = (uint32_t)generation;
    return record_status(glue, session_input_post_inhibit(glue->input, &event));
}

int session_input_last_post_status(const session_input_wayland_t *glue) {
    return glue->last_post_status;
}
