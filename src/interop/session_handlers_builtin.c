/**
 * @file session_handlers_builtin.c
 * @brief Compiled-in feature handler registry. Empty: the binary advertises capability zero.
 */
#include "session_handlers_builtin.h"

/* Feature lanes add their handler tables here in registration order (at most SessionAppMaxHandlers). */
const session_handler_t *session_handlers_builtin(size_t *out_count)
{
    if (out_count == NULL) return NULL;
    *out_count = 0;
    return NULL;
}
