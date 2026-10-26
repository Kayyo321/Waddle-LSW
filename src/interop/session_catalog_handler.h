/**
 * @file session_catalog_handler.h
 * @brief Session feature handler that drives the host catalog runtime (catalog_runtime.h).
 *
 * The handler owns the catalog message family (CatalogBegin/Add/Update/Remove/End, the
 * IconRequest exchange) and the Transfer* types. It decodes guest frames with the protocol codec,
 * forwards them to catalog_runtime, sends the runtime's IconRequest and TransferCancel actions
 * through the feature sender, and advances timeouts from on_tick. It requires InteropCapCatalog.
 *
 * Frame bytes: every on_frame call receives the complete protocol frame (16-byte header plus
 * payload) as copied by the peer. The header length must equal the payload size; any other frame
 * is ignored. Validation failures never fail the run: an invalid catalog frame is counted and
 * ignored, and the runtime poisons the open transaction as documented in catalog_runtime.h.
 *
 * Ownership: the handler does not own the runtime. The caller creates the runtime, keeps it alive
 * until the handler is no longer registered, and destroys it. The handler owns no heap memory.
 *
 * Threading: all callbacks run on the session pump thread, as the session_handler_t contract
 * requires. No internal locking.
 */
#ifndef WaddleInteropSessionCatalogHandlerH
#define WaddleInteropSessionCatalogHandlerH

#include <stddef.h>
#include <stdint.h>

#include "catalog_runtime.h"
#include "session_handler.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Outbound frame sink used by the handler; the default is interop_host_feature_send.
 * @param[in] context The handler's send_context, passed unchanged.
 * @param[in] sender Feature sender valid only during the current callback; may be used only by
 *            the send function that receives it.
 * @param[in] frame Complete frame bytes, borrowed for the call.
 * @param[in] length Frame length in bytes.
 * @return Zero on success or an interop_host_feature_result_t value (QueueFull means retry later).
 */
typedef int (*session_catalog_send_fn)(void *context, interop_host_feature_sender_t *sender,
                                       const uint8_t *frame, size_t length);

/**
 * @brief Monotonic millisecond clock; the default reads CLOCK_MONOTONIC.
 * @param[in] context The handler's clock_context, passed unchanged.
 * @return Milliseconds on a monotonic timeline.
 */
typedef uint64_t (*session_catalog_monotonic_fn)(void *context);

/**
 * @brief Wall-clock seconds since the Unix epoch; the default reads time(NULL).
 * @param[in] context The handler's clock_context, passed unchanged.
 * @return Seconds used to format the manifest `updated` text.
 */
typedef uint64_t (*session_catalog_wall_fn)(void *context);

/** @brief Handler state. Initialize with session_catalog_handler_init; do not copy after registration. */
typedef struct session_catalog_handler_t {
    catalog_runtime_t *runtime;             /**< Borrowed runtime; outlives the handler. */
    session_catalog_send_fn send;           /**< Outbound sink; NULL selects interop_host_feature_send. */
    void *send_context;                     /**< Passed to send. */
    session_catalog_monotonic_fn monotonic; /**< Clock for timeouts; NULL selects CLOCK_MONOTONIC. */
    session_catalog_wall_fn wall;           /**< Wall clock for `updated`; NULL selects time(NULL). */
    void *clock_context;                    /**< Passed to monotonic and wall. */
    int has_pending;                        /**< 1 when one action waits for a QueueFull retry. */
    catalog_runtime_action_t pending;       /**< The waiting action; its key points into pending_key. */
    uint8_t pending_key[200];               /**< Copy of the waiting IconRequest app key. */
    uint64_t rejected_frames;               /**< Frames ignored by validation or dispatch. */
    uint8_t frame[256];                     /**< Scratch buffer for outbound request frames. */
} session_catalog_handler_t;

/**
 * @brief Initialize a handler around an existing runtime.
 * @param[out] handler Non-NULL caller-owned state; fully reset.
 * @param[in] runtime Non-NULL runtime handle, borrowed until the handler is no longer registered.
 * @note Leaves send, clocks and contexts NULL, which select the production defaults. Tests may set
 *       them after this call and before registration.
 */
void session_catalog_handler_init(session_catalog_handler_t *handler, catalog_runtime_t *runtime);

/**
 * @brief Build the session_handler_t table for a handler.
 * @param[in] handler Initialized handler; its address is the table's context and must outlive it.
 * @return A table with name "catalog", capability InteropCapCatalog and the callbacks below.
 */
session_handler_t session_catalog_handler_table(session_catalog_handler_t *handler);

#ifdef __cplusplus
}
#endif
#endif
