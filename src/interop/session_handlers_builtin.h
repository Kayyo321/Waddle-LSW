/**
 * @file session_handlers_builtin.h
 * @brief Compiled-in feature handler registry for the waddle-session binary.
 */
#ifndef WaddleInteropSessionHandlersBuiltinH
#define WaddleInteropSessionHandlersBuiltinH

#include <stddef.h>

#include "session_handler.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Return the built-in handler table; the registry is empty today.
 * @param[out] out_count Non-NULL; receives the number of entries (zero today).
 * @return Pointer to the static table, or NULL when the registry is empty. Never freed.
 * @thread_safety Pure read of static data.
 */
const session_handler_t *session_handlers_builtin(size_t *out_count);

#ifdef __cplusplus
}
#endif
#endif
