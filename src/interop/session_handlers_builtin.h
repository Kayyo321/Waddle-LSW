/**
 * @file session_handlers_builtin.h
 * @brief Compiled-in feature handler registry for the waddle-session binary.
 *
 * The registry builds the handlers the binary serves. Today it is the session command handler
 * (LocalLaunch, LocalStop, session.sock). The keyboard handler (session_input_handler.h) is
 * deliberately not registered here: it needs a Wayland display and the windows/core lane, so it
 * is added when that lane lands. The hook is this file: a new handler is one more table written
 * into out_handlers and one more storage member, with the same lazy-initialisation rule.
 *
 * Lock proof. The launch handler's `lock_held_proof` is set to 1 at its on_ready, not earlier.
 * on_ready runs only after the runtime's admission succeeded, and admission acquires the device's
 * session.lock inside the session owner it returns, which the runtime keeps alive until every
 * on_closing has run. Initialisation therefore happens after the lock is held, and the handler's
 * own stale-socket decision sees the proof. Attempts that never reach Ready never initialise the
 * handler, and their on_closing is a no-op.
 */
#ifndef WaddleInteropSessionHandlersBuiltinH
#define WaddleInteropSessionHandlersBuiltinH

#include <stddef.h>

#include "session_handler.h"
#include "session_local_handler.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Status returned by a builtin wrapper when its handler cannot start (not a stop value). */
enum { SessionBuiltinFailure = 2 };

/** @brief Caller-owned state of the builtin registry; lives for the whole supervised run. */
typedef struct session_builtin_storage_t {
    const char *runtime_dir; /**< Borrowed absolute XDG_RUNTIME_DIR from the config. */
    const char *device_name; /**< Borrowed device name from the config. */
    session_local_handler_t launch; /**< Launch handler state; re-initialised on every Ready. */
    session_handler_t launch_inner; /**< Table produced by session_local_handler_table for launch. */
    session_handler_t launch_outer; /**< Table handed to session_app; forwards to launch_inner. */
    int launch_ready; /**< Nonzero from a successful initialisation until on_closing. */
    int launch_initialized; /**< Nonzero while launch holds initialised, undestroyed state. */
} session_builtin_storage_t;

/** @brief Inputs of the registry. All members are borrowed until session_handlers_builtin_destroy. */
typedef struct session_builtin_config_t {
    const char *runtime_dir; /**< In: absolute XDG_RUNTIME_DIR, NUL-terminated. */
    const char *device_name; /**< In: device name accepted by daemon_device_validate_name. */
    session_builtin_storage_t *storage; /**< In/out: caller storage, zero or previously destroyed. */
} session_builtin_config_t;

/**
 * @brief Build the builtin handler table for one supervised run.
 * @param[in] config Non-NULL configuration; storage must be non-NULL.
 * @param[out] out_handlers Writable array of SessionAppMaxHandlers entries; fully written on success.
 * @param[out] out_count Non-NULL; receives the number of entries written (one today).
 * @return 0 on success; 1 for NULL inputs, a relative runtime directory, or an invalid device name.
 * @note Performs no I/O. The launch handler is not initialised here; see the lock proof above.
 * @thread_safety Not thread-safe; one supervisor thread.
 */
int session_handlers_builtin_make(const session_builtin_config_t *config,
    session_handler_t out_handlers[SessionAppMaxHandlers], size_t *out_count);

/**
 * @brief Destroy any handler state still held by the storage. Safe after every run and twice.
 * @param[in,out] storage Storage previously passed to session_handlers_builtin_make, or NULL.
 * @thread_safety Call only after the run that used the table has returned.
 */
void session_handlers_builtin_destroy(session_builtin_storage_t *storage);

#ifdef __cplusplus
}
#endif
#endif
