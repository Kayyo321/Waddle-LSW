/**
 * @file session_catalog_handler.c
 * @brief Session feature handler for the host catalog runtime (see session_catalog_handler.h).
 */
#define _XOPEN_SOURCE 700

#include "session_catalog_handler.h"

#include "../../include/waddle/interop_protocol.h"
#include "session_control_host.h"

#include <string.h>
#include <time.h>

/** @brief Scratch size for one outbound frame; the largest is an IconRequest of about 232 bytes. */
enum { CatalogHandlerFrameBytes = 256 };
/** @brief Length of the RFC 3339 UTC text written to manifests (`YYYY-MM-DDTHH:MM:SSZ`). */
enum { CatalogHandlerUpdatedBytes = 20 };
/** @brief Digest length of a content hash and of TransferEnd. */
enum { CatalogHandlerHashBytes = 32 };
/** @brief Delay before a refused (QueueFull) action is retried; the pump never spins on refusal. */
enum { CatalogHandlerRetryMs = 100 };

/** @brief Result of one field lookup: found, absent, or malformed. */
enum { CatalogFieldFound = 0, CatalogFieldAbsent = 1, CatalogFieldMalformed = -1 };

/** @brief Weak reference: NULL in fixtures that do not link the control runtime. */
extern int interop_host_feature_send(interop_host_feature_sender_t *sender, const uint8_t *frame,
                                     size_t length) __attribute__((weak));

/* --- clocks and sends ------------------------------------------------------------------ */

static uint64_t default_monotonic_ms(void *context)
{
    struct timespec now;
    (void)context;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static uint64_t default_wall_seconds(void *context)
{
    (void)context;
    return (uint64_t)time(NULL);
}

static uint64_t handler_now(const session_catalog_handler_t *handler)
{
    return handler->monotonic != NULL ? handler->monotonic(handler->clock_context) : default_monotonic_ms(NULL);
}

static int send_frame(session_catalog_handler_t *handler, interop_host_feature_sender_t *sender,
                      const uint8_t *frame, size_t length)
{
    if (handler->send != NULL) return handler->send(handler->send_context, sender, frame, length);
    if (interop_host_feature_send == NULL) return HostFeatureUnsupportedFrame;
    return interop_host_feature_send(sender, frame, length);
}

/* --- outbound frames ------------------------------------------------------------------- */

/**
 * @brief Encode one runtime action as a host-to-guest frame into the handler scratch buffer.
 * @return Zero with *length set, or nonzero when the frame could not be encoded.
 */
static int build_action(session_catalog_handler_t *handler, const catalog_runtime_action_t *action, size_t *length)
{
    interop_writer_t writer;
    interop_writer_init(&writer, handler->frame, sizeof handler->frame);
    /* Runtime actions are bounded (keys at most 200 bytes, identifiers nonzero), so the field
     * writes cannot fail; the writer records any misuse and finish reports it. */
    if (action->kind == CatalogRuntimeActionIconRequest) {
        (void)interop_put_bytes(&writer, InteropTagAppKey, action->key, (uint16_t)action->key_length);
        (void)interop_put_uint(&writer, InteropTagTransfer, action->transfer);
        (void)interop_put_uint(&writer, InteropTagIconSize, action->icon_size);
        return interop_writer_finish(&writer, InteropMsgIconRequest, 0, InteropFromHost, length);
    }
    (void)interop_put_uint(&writer, InteropTagTransfer, action->transfer);
    (void)interop_put_uint(&writer, InteropTagStatus, action->status);
    return interop_writer_finish(&writer, InteropMsgTransferCancel, 0, InteropFromHost, length);
}

/**
 * @brief Keep a refused action for a later callback, copying its key out of the runtime. The
 *        source may already be the retained copy (a retry that is refused again), so the copy
 *        uses memmove, which is well defined when source and destination overlap.
 */
static void retain_pending(session_catalog_handler_t *handler, const catalog_runtime_action_t *action, uint64_t now)
{
    catalog_runtime_action_t copy = *action;
    if (action->key_length != 0) {
        memmove(handler->pending_key, action->key, action->key_length);
        copy.key = handler->pending_key;
    } else {
        copy.key = NULL;
    }
    handler->pending = copy;
    handler->has_pending = 1;
    handler->retry_at = now + CatalogHandlerRetryMs;
    catalog_runtime_set_send_blocked(handler->runtime, 1);
}

/**
 * @brief Send one action. Returns 1 when it was consumed (sent or dropped), 0 when the sender is
 *        full and the action was retained for retry.
 */
static int emit_action(session_catalog_handler_t *handler, interop_host_feature_sender_t *sender,
                       const catalog_runtime_action_t *action, uint64_t now)
{
    size_t length = 0;
    int result;
    if (build_action(handler, action, &length) != 0) return 1;
    result = send_frame(handler, sender, handler->frame, length);
    if (result == HostFeatureQueueFull) {
        retain_pending(handler, action, now);
        return 0;
    }
    catalog_runtime_set_send_blocked(handler->runtime, 0);
    return 1;
}

/**
 * @brief Send queued work in order: the retained action first, then the runtime's queue. Stops at
 *        the first refusal, which keeps order and leaves the rest queued in the runtime. A retained
 *        action is not retried before its retry time.
 */
static void drain_actions(session_catalog_handler_t *handler, interop_host_feature_sender_t *sender, uint64_t now)
{
    catalog_runtime_action_t action;
    if (handler->has_pending) {
        if (now < handler->retry_at) return;
        handler->has_pending = 0;
        /* A request whose transfer was released meanwhile is dropped, never sent. */
        if (handler->pending.kind == CatalogRuntimeActionIconRequest &&
            !catalog_runtime_request_live(handler->runtime, handler->pending.transfer)) {
            /* fall through to the queued actions */
        } else if (!emit_action(handler, sender, &handler->pending, now)) {
            return;
        }
    }
    while (catalog_runtime_next_action(handler->runtime, &action) == 1) {
        if (!emit_action(handler, sender, &action, now)) return;
    }
}

/* --- inbound fields -------------------------------------------------------------------- */

static int field_bytes(const uint8_t *payload, size_t length, uint16_t tag, const uint8_t **value, uint16_t *value_length)
{
    return interop_field_get(payload, length, tag, 0, value, value_length);
}

static int field_u32(const uint8_t *payload, size_t length, uint16_t tag, uint32_t *value)
{
    uint64_t wide = 0;
    int status = interop_field_uint(payload, length, tag, 4, &wide);
    *value = (uint32_t)wide;
    return status;
}

static int field_u64(const uint8_t *payload, size_t length, uint16_t tag, uint64_t *value)
{
    return interop_field_uint(payload, length, tag, 8, value);
}

/* --- guest catalog and transfer messages ----------------------------------------------- */

/*
 * The payload was validated by the codec before these functions run, so every required field is
 * present. Required values are still read with the status ignored: an absent value stays zeroed
 * (NULL with length zero, or identifier zero), which the runtime refuses as invalid input.
 */

static void apply_app(session_catalog_handler_t *handler, const uint8_t *payload, size_t length, int is_update)
{
    catalog_runtime_app_t app;
    const uint8_t *value = NULL;
    uint16_t value_length = 0;
    uint32_t flags = 0;
    memset(&app, 0, sizeof app);
    (void)field_bytes(payload, length, InteropTagAppKey, &value, &value_length);
    app.key = value;
    app.key_length = value_length;
    (void)field_bytes(payload, length, InteropTagDisplayName, &value, &value_length);
    app.display_name = value;
    app.display_name_length = value_length;
    (void)field_u32(payload, length, InteropTagAppFlags, &flags);
    app.flags = flags;
    if (field_bytes(payload, length, InteropTagIconSizes, &value, &value_length) == CatalogFieldFound) {
        app.icon_sizes = value;
        app.icon_sizes_length = value_length;
    }
    if (field_bytes(payload, length, InteropTagContentHash, &value, &value_length) == CatalogFieldFound) {
        app.content_hash = value;
        app.content_hash_length = value_length;
    }
    (void)catalog_runtime_upsert(handler->runtime, is_update, &app);
}

static void apply_remove(session_catalog_handler_t *handler, const uint8_t *payload, size_t length)
{
    const uint8_t *key = NULL;
    uint16_t key_length = 0;
    (void)field_bytes(payload, length, InteropTagAppKey, &key, &key_length);
    (void)catalog_runtime_remove(handler->runtime, key, key_length);
}

static void apply_transfer_begin(session_catalog_handler_t *handler, const uint8_t *payload, size_t length, uint64_t now)
{
    uint32_t id = 0;
    uint64_t total = 0;
    const uint8_t *mime = NULL;
    uint16_t mime_length = 0;
    (void)field_u32(payload, length, InteropTagTransfer, &id);
    (void)field_u64(payload, length, InteropTagTotalBytes, &total);
    (void)field_bytes(payload, length, InteropTagMime, &mime, &mime_length);
    (void)catalog_runtime_transfer_begin(handler->runtime, id, total, mime, mime_length, now);
}

static void apply_transfer_chunk(session_catalog_handler_t *handler, const uint8_t *payload, size_t length, uint64_t now)
{
    uint32_t id = 0;
    uint64_t offset = 0;
    const uint8_t *data = NULL;
    uint16_t data_length = 0;
    (void)field_u32(payload, length, InteropTagTransfer, &id);
    (void)field_u64(payload, length, InteropTagOffset, &offset);
    (void)field_bytes(payload, length, InteropTagData, &data, &data_length);
    (void)catalog_runtime_transfer_chunk(handler->runtime, id, offset, data, data_length, now);
}

static void apply_transfer_end(session_catalog_handler_t *handler, const uint8_t *payload, size_t length, uint64_t now)
{
    uint32_t id = 0;
    const uint8_t *hash = NULL;
    uint16_t hash_length = 0;
    (void)field_u32(payload, length, InteropTagTransfer, &id);
    /* The codec fixes ContentHash at 32 bytes during validation, so no length check is needed. */
    (void)field_bytes(payload, length, InteropTagContentHash, &hash, &hash_length);
    (void)catalog_runtime_transfer_end(handler->runtime, id, hash, now);
}

static void apply_transfer_cancel(session_catalog_handler_t *handler, const uint8_t *payload, size_t length, uint64_t now)
{
    uint32_t id = 0;
    (void)field_u32(payload, length, InteropTagTransfer, &id);
    (void)catalog_runtime_transfer_cancel(handler->runtime, id, now);
}

/* --- session_handler_t callbacks ------------------------------------------------------- */

static int owns_type(void *context, uint32_t message_type)
{
    (void)context;
    return (message_type >= InteropMsgCatalogBegin && message_type <= InteropMsgCatalogEnd) ||
           (message_type >= InteropMsgTransferBegin && message_type <= InteropMsgTransferCancel);
}

static int on_ready(void *context, uint64_t negotiated_capabilities, interop_host_feature_sender_t *sender)
{
    (void)context;
    (void)negotiated_capabilities;
    (void)sender;
    return 0;
}

static int on_frame(void *context, uint32_t message_type, uint32_t request_id, const uint8_t *frame,
                    size_t length, interop_host_feature_sender_t *sender)
{
    session_catalog_handler_t *handler = context;
    interop_header_t header;
    const uint8_t *payload;
    size_t payload_length;
    uint64_t now;
    (void)request_id;
    if (interop_header_decode(frame, length, &header) != 0 || header.type != message_type ||
        (size_t)header.length + InteropHeaderBytes != length) {
        handler->rejected_frames += 1;
        return 0;
    }
    payload = frame + InteropHeaderBytes;
    payload_length = length - InteropHeaderBytes;
    if (interop_payload_validate(message_type, InteropFromGuest, payload, payload_length) != 0) {
        handler->rejected_frames += 1;
        return 0;
    }
    now = handler_now(handler);
    switch (message_type) {
    case InteropMsgCatalogBegin:
        catalog_runtime_begin(handler->runtime);
        break;
    case InteropMsgCatalogAdd:
        apply_app(handler, payload, payload_length, 0);
        break;
    case InteropMsgCatalogUpdate:
        apply_app(handler, payload, payload_length, 1);
        break;
    case InteropMsgCatalogRemove:
        apply_remove(handler, payload, payload_length);
        break;
    case InteropMsgCatalogEnd:
        (void)catalog_runtime_end(handler->runtime);
        break;
    case InteropMsgTransferBegin:
        apply_transfer_begin(handler, payload, payload_length, now);
        break;
    case InteropMsgTransferChunk:
        apply_transfer_chunk(handler, payload, payload_length, now);
        break;
    case InteropMsgTransferEnd:
        apply_transfer_end(handler, payload, payload_length, now);
        break;
    case InteropMsgTransferCancel:
        apply_transfer_cancel(handler, payload, payload_length, now);
        break;
    default:
        /* Unowned types are never dispatched by the peer; a direct call only drains the queue. */
        break;
    }
    drain_actions(handler, sender, now);
    return 0;
}

static int on_tick(void *context, uint64_t now_ms, interop_host_feature_sender_t *sender)
{
    session_catalog_handler_t *handler = context;
    char updated[CatalogHandlerUpdatedBytes + 1];
    time_t seconds = (time_t)(handler->wall != NULL ? handler->wall(handler->clock_context) : default_wall_seconds(NULL));
    struct tm parts;
    if (gmtime_r(&seconds, &parts) == NULL ||
        strftime(updated, sizeof updated, "%Y-%m-%dT%H:%M:%SZ", &parts) != CatalogHandlerUpdatedBytes) {
        return 1;
    }
    (void)catalog_runtime_tick(handler->runtime, now_ms, (const uint8_t *)updated, CatalogHandlerUpdatedBytes);
    drain_actions(handler, sender, now_ms);
    return 0;
}

static uint64_t next_deadline(void *context, uint64_t now_ms)
{
    session_catalog_handler_t *handler = context;
    uint64_t deadline = catalog_runtime_next_deadline(handler->runtime, now_ms);
    if (handler->has_pending) {
        uint64_t retry = handler->retry_at > now_ms ? handler->retry_at : now_ms;
        if (retry < deadline) deadline = retry;
    }
    return deadline;
}

static void on_closing(void *context, uint32_t reason, interop_host_feature_sender_t *sender)
{
    session_catalog_handler_t *handler = context;
    (void)reason;
    (void)sender;
    catalog_runtime_reset_session(handler->runtime);
    handler->has_pending = 0;
    catalog_runtime_set_send_blocked(handler->runtime, 0);
}

/* --- public interface ------------------------------------------------------------------ */

void session_catalog_handler_init(session_catalog_handler_t *handler, catalog_runtime_t *runtime)
{
    memset(handler, 0, sizeof *handler);
    handler->runtime = runtime;
}

session_handler_t session_catalog_handler_table(session_catalog_handler_t *handler)
{
    session_handler_t table;
    memset(&table, 0, sizeof table);
    table.name = "catalog";
    table.context = handler;
    table.capabilities = InteropCapCatalog;
    table.owns_type = owns_type;
    table.on_ready = on_ready;
    table.on_frame = on_frame;
    table.on_tick = on_tick;
    table.next_deadline_ms = next_deadline;
    table.on_closing = on_closing;
    return table;
}
