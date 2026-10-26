/**
 * @file guest_launch_double.h
 * @brief Linux double of the guest launch native table and catalog resolver.
 *
 * Records every native call with its strings, returns a configured Win32 error per call kind, hands out
 * fake process handles, and counts opens against closes so the suite can prove exactly-once closing.
 * Threading: single-threaded; the suite is the only user.
 */
#ifndef WADDLE_TESTS_GUEST_LAUNCH_DOUBLE_H
#define WADDLE_TESTS_GUEST_LAUNCH_DOUBLE_H

#include <stdint.h>
#include <string.h>

#include "guest_launch.h"

/** @brief Native call kinds, also the index into error_for. */
#define GuestLaunchDoubleShell 0u
#define GuestLaunchDoubleConsole 1u
#define GuestLaunchDoubleActivate 2u
#define GuestLaunchDoubleKinds 3u
#define GuestLaunchDoubleMaxCalls 8u
#define GuestLaunchDoubleMaxHandles 8u

/** @brief One recorded native call. Text is NUL-terminated copies. */
typedef struct guest_launch_double_call_t {
    uint32_t kind; /**< GuestLaunchDouble* value. */
    char file[GuestLaunchMaxText + 64u];
    char parameters[GuestLaunchMaxCommandLine + 1u];
    char directory[GuestLaunchMaxText + 1u];
    int has_parameters;
    int has_directory;
    char command[GuestLaunchMaxCommandLine + 1u];
    char aumid[GuestLaunchMaxText + 1u];
    char arguments[GuestLaunchMaxArguments + 1u];
    char verb[8];
} guest_launch_double_call_t;

/** @brief Double state. Set error_for and the flags before the call under test. */
typedef struct guest_launch_double_t {
    guest_launch_native_t table; /**< Wired to the functions below; context points back at this struct. */
    guest_launch_double_call_t calls[GuestLaunchDoubleMaxCalls];
    uint32_t call_count;
    uint32_t error_for[GuestLaunchDoubleKinds]; /**< Win32 error returned by every call of that kind; 0 = success. */
    uint32_t process_id; /**< Process id reported with a successful call. */
    int return_handle; /**< Nonzero: hand out a process handle on success. */
    int return_handle_on_error; /**< Nonzero: also hand out a handle on failure (the executor must close it). */
    uint32_t handles_opened;
    uint32_t handles_closed;
    uint32_t double_closes; /**< close_process calls for a handle that was not open. */
    uintptr_t open_handles[GuestLaunchDoubleMaxHandles];
    uint32_t open_count;
} guest_launch_double_t;

/** @brief Catalog resolver double: one entry per app key, with a forced failure mode. */
typedef struct guest_launch_catalog_t {
    const char *app_key; /**< Single app key the resolver knows. */
    guest_launch_target_t target; /**< Target returned for app_key. */
    int fail; /**< Nonzero: resolve returns -1. */
    uint32_t calls; /**< Resolve calls received. */
} guest_launch_catalog_t;

static void guest_launch_double_record(guest_launch_double_t *d, uint32_t kind, const guest_launch_shell_t *shell,
                                       const char *command, const char *directory_text, const char *aumid,
                                       const char *arguments) {
    guest_launch_double_call_t *call;
    if (d->call_count >= GuestLaunchDoubleMaxCalls) return;
    call = &d->calls[d->call_count];
    d->call_count += 1u;
    memset(call, 0, sizeof(*call));
    call->kind = kind;
    if (shell != NULL) {
        strncpy(call->verb, shell->verb, sizeof(call->verb) - 1u);
        strncpy(call->file, shell->file, sizeof(call->file) - 1u);
        call->has_parameters = shell->parameters != NULL;
        if (shell->parameters != NULL) strncpy(call->parameters, shell->parameters, sizeof(call->parameters) - 1u);
        call->has_directory = shell->directory != NULL;
        if (shell->directory != NULL) strncpy(call->directory, shell->directory, sizeof(call->directory) - 1u);
    }
    if (command != NULL) strncpy(call->command, command, sizeof(call->command) - 1u);
    if (directory_text != NULL) {
        call->has_directory = 1;
        strncpy(call->directory, directory_text, sizeof(call->directory) - 1u);
    }
    if (aumid != NULL) strncpy(call->aumid, aumid, sizeof(call->aumid) - 1u);
    if (arguments != NULL) strncpy(call->arguments, arguments, sizeof(call->arguments) - 1u);
}

static void guest_launch_double_result(guest_launch_double_t *d, uint32_t kind, guest_launch_native_result_t *result) {
    uint32_t error = d->error_for[kind];
    result->error = error;
    result->process_id = error == 0u ? d->process_id : 0u;
    result->process = NULL;
    if ((error == 0u && d->return_handle) || (error != 0u && d->return_handle_on_error)) {
        if (d->open_count < GuestLaunchDoubleMaxHandles) {
            uintptr_t handle = (uintptr_t)(0x1000u + d->handles_opened);
            d->open_handles[d->open_count] = handle;
            d->open_count += 1u;
            d->handles_opened += 1u;
            result->process = (void *)handle;
        }
    }
}

static void guest_launch_double_shell(void *context, const guest_launch_shell_t *shell, guest_launch_native_result_t *result) {
    guest_launch_double_t *d = (guest_launch_double_t *)context;
    guest_launch_double_record(d, GuestLaunchDoubleShell, shell, NULL, NULL, NULL, NULL);
    guest_launch_double_result(d, GuestLaunchDoubleShell, result);
}

static void guest_launch_double_console(void *context, const char *command_line, const char *directory,
                                        guest_launch_native_result_t *result) {
    guest_launch_double_t *d = (guest_launch_double_t *)context;
    guest_launch_double_record(d, GuestLaunchDoubleConsole, NULL, command_line, directory, NULL, NULL);
    guest_launch_double_result(d, GuestLaunchDoubleConsole, result);
}

static void guest_launch_double_activate(void *context, const char *aumid, const char *arguments,
                                         guest_launch_native_result_t *result) {
    guest_launch_double_t *d = (guest_launch_double_t *)context;
    guest_launch_double_record(d, GuestLaunchDoubleActivate, NULL, NULL, NULL, aumid, arguments);
    guest_launch_double_result(d, GuestLaunchDoubleActivate, result);
}

static void guest_launch_double_close(void *context, void *process) {
    guest_launch_double_t *d = (guest_launch_double_t *)context;
    uint32_t index;
    for (index = 0; index < d->open_count; ++index) {
        if (d->open_handles[index] == (uintptr_t)process) {
            d->open_handles[index] = d->open_handles[d->open_count - 1u];
            d->open_count -= 1u;
            d->handles_closed += 1u;
            return;
        }
    }
    d->double_closes += 1u;
}

static void guest_launch_double_init(guest_launch_double_t *d) {
    memset(d, 0, sizeof(*d));
    d->table.context = d;
    d->table.shell_execute = guest_launch_double_shell;
    d->table.create_console = guest_launch_double_console;
    d->table.activate_aumid = guest_launch_double_activate;
    d->table.close_process = guest_launch_double_close;
    d->process_id = 4321u;
}

static int guest_launch_double_resolve(void *context, const uint8_t *app_key, size_t app_key_length,
                                       guest_launch_target_t *target) {
    guest_launch_catalog_t *catalog = (guest_launch_catalog_t *)context;
    catalog->calls += 1u;
    if (catalog->fail) return -1;
    if (app_key_length != strlen(catalog->app_key) || memcmp(app_key, catalog->app_key, app_key_length) != 0) return 1;
    *target = catalog->target;
    return 0;
}

#endif
