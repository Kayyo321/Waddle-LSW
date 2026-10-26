/**
 * @file session_handlers_builtin.c
 * @brief Compiled-in feature handler registry: the session command handler, with lazy initialisation.
 */
#include "session_handlers_builtin.h"

#include <stdint.h>
#include <string.h>

#include "daemon_device.h"

/* ---- Launch handler: forwarding wrappers around session_local_handler's table ---- */

static int builtin_launch_owns_type(void *context, uint32_t message_type)
{
    session_builtin_storage_t *storage = context;
    if (!storage->launch_ready) return 0;
    return storage->launch_inner.owns_type(storage->launch_inner.context, message_type);
}

static int builtin_launch_on_ready(void *context, uint64_t negotiated_capabilities,
    interop_host_feature_sender_t *sender)
{
    session_builtin_storage_t *storage = context;
    session_local_config_t config;
    memset(&config, 0, sizeof(config));
    config.runtime_dir = storage->runtime_dir;
    config.device_name = storage->device_name;
    /* Ready implies admission succeeded, so session.lock is held by the live session owner. */
    config.lock_held_proof = 1;
    config.reserved_zero = 0;
    if (session_local_handler_init(&storage->launch, &config) != SessionLocalSuccess)
        return SessionBuiltinFailure;
    storage->launch_initialized = 1;
    storage->launch_ready = 1;
    return storage->launch_inner.on_ready(storage->launch_inner.context, negotiated_capabilities, sender);
}

static int builtin_launch_on_frame(void *context, uint32_t message_type, uint32_t request_id,
    const uint8_t *frame, size_t length, interop_host_feature_sender_t *sender)
{
    session_builtin_storage_t *storage = context;
    if (!storage->launch_ready) return SessionBuiltinFailure;
    return storage->launch_inner.on_frame(storage->launch_inner.context, message_type, request_id,
        frame, length, sender);
}

static int builtin_launch_poll_fds(void *context, struct pollfd *fds, size_t capacity, size_t *count)
{
    session_builtin_storage_t *storage = context;
    if (!storage->launch_ready) {
        *count = 0;
        return 0;
    }
    return storage->launch_inner.poll_fds(storage->launch_inner.context, fds, capacity, count);
}

static int builtin_launch_on_fds(void *context, const struct pollfd *fds, size_t count,
    uint64_t now_ms, interop_host_feature_sender_t *sender)
{
    session_builtin_storage_t *storage = context;
    if (!storage->launch_ready) return 0;
    return storage->launch_inner.on_fds(storage->launch_inner.context, fds, count, now_ms, sender);
}

static int builtin_launch_on_tick(void *context, uint64_t now_ms, interop_host_feature_sender_t *sender)
{
    session_builtin_storage_t *storage = context;
    if (!storage->launch_ready) return 0;
    return storage->launch_inner.on_tick(storage->launch_inner.context, now_ms, sender);
}

static uint64_t builtin_launch_next_deadline(void *context, uint64_t now_ms)
{
    session_builtin_storage_t *storage = context;
    if (!storage->launch_ready) return UINT64_MAX;
    return storage->launch_inner.next_deadline_ms(storage->launch_inner.context, now_ms);
}

static void builtin_launch_on_closing(void *context, uint32_t reason, interop_host_feature_sender_t *sender)
{
    session_builtin_storage_t *storage = context;
    if (!storage->launch_ready) return;
    storage->launch_ready = 0;
    storage->launch_inner.on_closing(storage->launch_inner.context, reason, sender);
}

/* ---- Registry ---- */

int session_handlers_builtin_make(const session_builtin_config_t *config,
    session_handler_t out_handlers[SessionAppMaxHandlers], size_t *out_count)
{
    session_builtin_storage_t *storage;
    if (config == NULL || out_handlers == NULL || out_count == NULL || config->storage == NULL ||
            config->runtime_dir == NULL || config->runtime_dir[0] != '/' || config->device_name == NULL ||
            daemon_device_validate_name(config->device_name) != 0)
        return 1;
    storage = config->storage;
    memset(storage, 0, sizeof(*storage));
    storage->runtime_dir = config->runtime_dir;
    storage->device_name = config->device_name;
    session_local_handler_table(&storage->launch, &storage->launch_inner);

    memset(&storage->launch_outer, 0, sizeof(storage->launch_outer));
    storage->launch_outer.name = "launch";
    storage->launch_outer.context = storage;
    storage->launch_outer.capabilities = storage->launch_inner.capabilities;
    storage->launch_outer.owns_type = builtin_launch_owns_type;
    storage->launch_outer.on_ready = builtin_launch_on_ready;
    storage->launch_outer.on_frame = builtin_launch_on_frame;
    storage->launch_outer.poll_fds = builtin_launch_poll_fds;
    storage->launch_outer.on_fds = builtin_launch_on_fds;
    storage->launch_outer.on_tick = builtin_launch_on_tick;
    storage->launch_outer.next_deadline_ms = builtin_launch_next_deadline;
    storage->launch_outer.on_closing = builtin_launch_on_closing;

    out_handlers[0] = storage->launch_outer;
    *out_count = 1;
    return 0;
}

void session_handlers_builtin_destroy(session_builtin_storage_t *storage)
{
    if (storage == NULL || !storage->launch_initialized) return;
    /* IMPL_DESC 3.12.5: the supervised run has returned, so its owners and both locks are released;
     * only now may the LocalStop requester receive its result (a no-op when none is pending). */
    (void)session_local_handler_send_stop_reply(&storage->launch);
    session_local_handler_destroy(&storage->launch);
    storage->launch_initialized = 0;
    storage->launch_ready = 0;
}
