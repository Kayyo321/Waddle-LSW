/**
 * @file session_handler.h
 * @brief Feature handler interface composed by the waddle-session application runtime.
 *
 * One handler owns one feature family (windows, launch plus local control, catalog).
 * The application runtime (session_app) owns the single host control pump, offers the
 * union of all registered handlers' capabilities in Hello, and routes every callback of
 * @c interop_host_feature_ops_t to the handlers. Handlers never see each other and never
 * own the pump. Every callback runs on the pump's creating thread, never re-entrantly.
 *
 * Ownership: a handler's @c context is owned by whoever registered it and must outlive
 * the run. The application runtime never frees handler contexts. Descriptors a handler
 * reports through @c poll_fds are borrowed and are never closed by the runtime.
 *
 * Failure model: a nonzero return from @c on_ready, @c on_frame, @c on_fds or @c on_tick
 * makes the whole run fail with HostControlFeatureFailed and drain; a handler that can
 * recover from its own errors must handle them itself and return zero. @c on_closing is
 * called exactly once per registered handler, in reverse registration order, and every
 * send through the closing sender returns Draining.
 */
#ifndef WaddleInteropSessionHandlerH
#define WaddleInteropSessionHandlerH

#include <poll.h>
#include <stddef.h>
#include <stdint.h>

#include "session_control_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum extra descriptors one handler may report from @c poll_fds. */
enum { SessionHandlerMaxDescriptors = 4 };

/** @brief Maximum number of handlers one application runtime accepts. */
enum { SessionAppMaxHandlers = 4 };

/** @brief One feature handler's callback table (all members optional except @c owns_type). */
typedef struct session_handler_t {
    const char *name; /**< Borrowed static NUL-terminated diagnostic name, non-NULL. */
    void *context; /**< Passed unchanged to every callback; may be NULL. */
    uint64_t capabilities; /**< Capability bits this handler needs offered; within InteropCapabilityMask. */
    /**
     * @brief Report whether this handler consumes inbound feature frames of a message type.
     * @param[in] context Handler context.
     * @param[in] message_type Interop message type (host-bound feature frame).
     * @return Nonzero when this handler owns the type; at most one handler may own a type.
     * @note Must be pure and non-NULL; the runtime queries it per frame.
     */
    int (*owns_type)(void *context, uint32_t message_type);
    /** @brief Called once after the peer reaches Ready with the negotiated capability intersection. */
    int (*on_ready)(void *context, uint64_t negotiated_capabilities,
        interop_host_feature_sender_t *sender);
    /** @brief Deliver one admitted inbound frame whose type this handler owns (bytes valid only during the call). */
    int (*on_frame)(void *context, uint32_t message_type, uint32_t request_id,
        const uint8_t *frame, size_t length, interop_host_feature_sender_t *sender);
    /**
     * @brief Report up to @c SessionHandlerMaxDescriptors descriptors to multiplex.
     * @param[in] context Handler context.
     * @param[out] fds Capacity entries; the handler fills fd and events and zeroes revents.
     * @param[in] capacity Always @c SessionHandlerMaxDescriptors.
     * @param[out] count Number of entries written, 0..capacity.
     * @return Zero on success, nonzero on failure.
     */
    int (*poll_fds)(void *context, struct pollfd *fds, size_t capacity, size_t *count);
    /** @brief Deliver readiness for this handler's descriptors (same order and count as reported, revents filled). */
    int (*on_fds)(void *context, const struct pollfd *fds, size_t count, uint64_t now_ms,
        interop_host_feature_sender_t *sender);
    /** @brief Periodic work, at least every 100 ms while Ready. */
    int (*on_tick)(void *context, uint64_t now_ms, interop_host_feature_sender_t *sender);
    /** @brief Earliest absolute CLOCK_MONOTONIC ms this handler needs a tick, or UINT64_MAX. */
    uint64_t (*next_deadline_ms)(void *context, uint64_t now_ms);
    /** @brief Final notification (exactly once); release every resource tied to the connection. */
    void (*on_closing)(void *context, uint32_t reason, interop_host_feature_sender_t *sender);
} session_handler_t;

#ifdef __cplusplus
}
#endif
#endif
