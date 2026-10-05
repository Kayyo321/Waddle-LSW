/**
 * @file daemon_server.h
 * @brief Non-blocking UNIX socket event loop and IPC request dispatcher for waddled.
 */

#ifndef WADDLE_DAEMON_SERVER_H
#define WADDLE_DAEMON_SERVER_H

#include "daemon_state.h"
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum concurrent client connections supported. */
#define DaemonMaxClients 16U

/**
 * @brief Runs the daemon server event loop until stopped.
 *
 * Acquires single-instance lock, binds the daemon IPC UNIX domain socket,
 * registers signal traps, and dispatches client control commands.
 *
 * @param[in]     custom_runtime_dir Directory for sockets and locks (NULL for standard XDG).
 * @param[in,out] stop_flag          Pointer to termination flag set by signal handlers.
 * @return 0 on clean shutdown, or non-zero on error.
 */
int daemon_server_run(const char *custom_runtime_dir, volatile sig_atomic_t *stop_flag);

#ifdef __cplusplus
}
#endif

#endif /* WADDLE_DAEMON_SERVER_H */
